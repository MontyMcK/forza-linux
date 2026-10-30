/* Copies the user's own game dump from a USB drive to the PS5's internal
 * storage, on the console (no network round trip).
 *
 * Looks for /mnt/usbN/<folder>/Mount/forza_durango_release_final.exe and copies
 * <folder>/Mount to /data/OpenHorizon2/<folder>/Mount. Each file is written
 * as <name>.part and renamed when complete; files already there with the right
 * size are skipped, so sending the payload again resumes. Progress and the
 * result go to the log below (tools/ps5/pull_logs.sh). Sent with
 * tools/ps5/send_payload.sh; only run it when the console's owner asks.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_PATH "/data/homebrew/PPSA99642/logs/copydump.txt"
#define DEST_ROOT "/data/OpenHorizon2"
#define GAME_EXE "forza_durango_release_final.exe"
#define CHUNK (8u << 20)

static int log_fd = -1;
static uint64_t total_bytes, total_files, present_bytes, done_bytes, done_files, skipped_files;
static int counting; /* first pass: only count, and note what an earlier run already copied */
static double t_start, t_last_report;
static char *buf;

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void say(const char *fmt, ...) {
  char line[1200];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof line - 2) n = sizeof line - 2;
  line[n++] = '\n';
  write(log_fd, line, n);
  fsync(log_fd);
}

static void report(int force) {
  double t = now_s();
  if (!force && t - t_last_report < 10) return;
  t_last_report = t;
  double el = t - t_start;
  say("PROGRESS %llu/%llu MiB, %llu/%llu files (%llu skipped), %.1f MiB/s, %.0f s",
      (unsigned long long)(done_bytes >> 20), (unsigned long long)(total_bytes >> 20),
      (unsigned long long)done_files, (unsigned long long)total_files, (unsigned long long)skipped_files,
      el > 0 ? (done_bytes / 1048576.0) / el : 0.0, el);
}

static int mkdirs(const char *path) {
  char p[1024];
  snprintf(p, sizeof p, "%s", path);
  for (char *s = p + 1; *s; s++)
    if (*s == '/') {
      *s = 0;
      if (mkdir(p, 0777) && errno != EEXIST) return -1;
      *s = '/';
    }
  return mkdir(p, 0777) && errno != EEXIST ? -1 : 0;
}

/* Walks src into dst (or only counts, see `counting`). Returns 0 or -1 (already logged). */
static int walk(const char *src, const char *dst) {
  DIR *d = opendir(src);
  if (!d) {
    say("FAIL can't open %s (errno %d)", src, errno);
    return -1;
  }
  if (!counting && mkdirs(dst)) {
    say("FAIL can't create %s (errno %d)", dst, errno);
    closedir(d);
    return -1;
  }
  struct dirent *e;
  int rc = 0;
  while (rc == 0 && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char s[1024], t[1024];
    snprintf(s, sizeof s, "%s/%s", src, e->d_name);
    snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
    struct stat st, dt;
    if (stat(s, &st)) {
      say("FAIL can't stat %s (errno %d)", s, errno);
      rc = -1;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      rc = walk(s, t);
      continue;
    }
    int present = stat(t, &dt) == 0 && dt.st_size == st.st_size;
    if (counting) {
      total_files++;
      total_bytes += st.st_size;
      if (present) present_bytes += st.st_size;
      continue;
    }
    if (present) {
      skipped_files++;
      done_files++;
      done_bytes += st.st_size;
      report(0);
      continue;
    }
    char part[1100];
    snprintf(part, sizeof part, "%s.part", t);
    int in = open(s, O_RDONLY);
    int out = in >= 0 ? open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    if (in < 0 || out < 0) {
      say("FAIL can't open %s (errno %d)", in < 0 ? s : part, errno);
      if (in >= 0) close(in);
      rc = -1;
      break;
    }
    uint64_t copied = 0;
    for (;;) {
      ssize_t r = read(in, buf, CHUNK);
      if (r < 0) {
        say("FAIL read %s at %llu (errno %d)", s, (unsigned long long)copied, errno);
        rc = -1;
        break;
      }
      if (r == 0) break;
      for (ssize_t w = 0; w < r;) {
        ssize_t n = write(out, buf + w, r - w);
        if (n <= 0) {
          say("FAIL write %s at %llu (errno %d)", part, (unsigned long long)(copied + w), errno);
          rc = -1;
          break;
        }
        w += n;
      }
      if (rc) break;
      copied += r;
      done_bytes += r;
      report(0);
    }
    if (rc == 0 && fsync(out)) {
      say("FAIL fsync %s (errno %d)", part, errno);
      rc = -1;
    }
    close(out);
    close(in);
    if (rc) break;
    if (copied != (uint64_t)st.st_size) {
      say("FAIL %s: copied %llu bytes, source has %lld", s, (unsigned long long)copied, (long long)st.st_size);
      rc = -1;
      break;
    }
    if (rename(part, t)) {
      say("FAIL rename %s (errno %d)", part, errno);
      rc = -1;
      break;
    }
    done_files++;
  }
  closedir(d);
  return rc;
}

/* Every source file exists at the destination with the same size. */
static int verify(const char *src, const char *dst, uint64_t *checked) {
  DIR *d = opendir(src);
  if (!d) return -1;
  struct dirent *e;
  int rc = 0;
  while (rc == 0 && (e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char s[1024], t[1024];
    snprintf(s, sizeof s, "%s/%s", src, e->d_name);
    snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
    struct stat st, dt;
    if (stat(s, &st)) { rc = -1; break; }
    if (S_ISDIR(st.st_mode)) { rc = verify(s, t, checked); continue; }
    if (stat(t, &dt) || dt.st_size != st.st_size) {
      say("FAIL verify: %s missing or wrong size", t);
      rc = -1;
      break;
    }
    (*checked)++;
  }
  closedir(d);
  return rc;
}

/* Finds /mnt/usbN/<folder>/Mount holding the game's exe. */
static int find_dump(char *mount, size_t n, char *folder, size_t fn) {
  for (int i = 0; i < 8; i++) {
    char root[32];
    snprintf(root, sizeof root, "/mnt/usb%d", i);
    DIR *d = opendir(root);
    if (!d) continue;
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      char exe[1024];
      struct stat st;
      snprintf(exe, sizeof exe, "%s/%s/Mount/" GAME_EXE, root, e->d_name);
      if (stat(exe, &st) == 0) {
        snprintf(mount, n, "%s/%s/Mount", root, e->d_name);
        snprintf(folder, fn, "%s", e->d_name);
        closedir(d);
        return 0;
      }
    }
    closedir(d);
  }
  return -1;
}

int main(void) {
  mkdir("/data/homebrew/PPSA99642/logs", 0777);
  log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (log_fd < 0) return 1;
  say("Open Horizon 2: copy the game dump from USB to internal storage");
  char src[1024], folder[256], dst[1024];
  if (find_dump(src, sizeof src, folder, sizeof folder)) {
    say("FAIL no /mnt/usbN/<folder>/Mount/" GAME_EXE " found");
    return 1;
  }
  snprintf(dst, sizeof dst, DEST_ROOT "/%s/Mount", folder);
  say("FROM %s", src);
  say("TO %s", dst);
  counting = 1;
  if (walk(src, dst)) return 1;
  counting = 0;
  struct statfs fs;
  uint64_t free_bytes = statfs("/data", &fs) == 0 ? (uint64_t)fs.f_bavail * fs.f_bsize : 0;
  uint64_t need = total_bytes - present_bytes;
  say("SIZE %llu MiB in %llu files, %llu MiB still to copy; /data has %llu MiB free",
      (unsigned long long)(total_bytes >> 20), (unsigned long long)total_files,
      (unsigned long long)(need >> 20), (unsigned long long)(free_bytes >> 20));
  if (free_bytes < need + (2ull << 30)) {
    say("FAIL not enough free space on /data (need %llu MiB plus 2 GiB to spare)", (unsigned long long)(need >> 20));
    return 1;
  }
  buf = malloc(CHUNK);
  if (!buf) {
    say("FAIL out of memory");
    return 1;
  }
  t_start = t_last_report = now_s();
  if (walk(src, dst)) {
    report(1);
    return 1;
  }
  report(1);
  uint64_t checked = 0;
  if (verify(src, dst, &checked)) return 1;
  say("DONE %llu files, %llu MiB, all sizes match (%llu copied, %llu already there), %.0f s",
      (unsigned long long)checked, (unsigned long long)(total_bytes >> 20),
      (unsigned long long)(done_files - skipped_files), (unsigned long long)skipped_files, now_s() - t_start);
  free(buf);
  close(log_fd);
  return 0;
}
