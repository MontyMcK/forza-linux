/* Starts the Open Horizon 2 title (OH2_TITLE_ID) on the console, as a payload
 * sent with tools/ps5/send_payload.sh, so test runs don't need anyone at the
 * console. It logs which app is running; if that is our own title (an earlier
 * run that hasn't ended), it closes it first. Any other app is left alone and
 * the launch is skipped. Then it tries the launch calls homebrew launchers use
 * (sceLncUtilLaunchApp with the foreground user, then the first logged-in
 * user, then sceSystemServiceLaunchApp) and logs each result to the title's
 * log folder. Built with OH2_STATUS_ONLY (oh2status.elf) it only logs the
 * running app.
 * Our own code, written from the public libSceSystemService and
 * libSceUserService interfaces.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef OH2_TITLE_ID
#define OH2_TITLE_ID "PPSA99642"
#endif
#define LOG_PATH "/data/homebrew/" OH2_TITLE_ID "/logs/launch.txt"
#define ALREADY_RUNNING 0x8094000cu
/* The title's arguments, one per line (e.g. --game-dir <dump>), uploaded by
 * tools/ps5/run_title.sh; none if the file is missing. */
#define ARGS_PATH "/data/homebrew/" OH2_TITLE_ID "/launch-args.txt"

typedef struct {
  uint32_t size;
  int user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint64_t check_flag;
} launch_param_t;
typedef struct {
  int user_id[4];
} login_users_t;

int sceUserServiceInitialize(void *param);
int sceUserServiceGetForegroundUser(int *user_id);
int sceUserServiceGetLoginUserIdList(login_users_t *list);
int sceLncUtilLaunchApp(const char *title_id, const char *argv[], launch_param_t *param);
uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
uint32_t sceLncUtilKillApp(uint32_t app_id);
int sceSystemServiceLaunchApp(const char *title_id, const char *argv[], launch_param_t *param);

static int log_fd = -1;
static void say(const char *fmt, ...) {
  char line[256];
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

static char args_buf[4096];
static const char *args[33];

static const char **read_args(void) {
  int fd = open(ARGS_PATH, O_RDONLY);
  if (fd < 0) return 0;
  ssize_t n = read(fd, args_buf, sizeof args_buf - 1);
  close(fd);
  if (n <= 0) return 0;
  args_buf[n] = 0;
  int k = 0;
  for (char *line = strtok(args_buf, "\n"); line && k < 32; line = strtok(0, "\n"))
    if (line[0]) args[k++] = line;
  args[k] = 0;
  say("arguments: %d from " ARGS_PATH, k);
  return args;
}

int main(void) {
  mkdir("/data/homebrew/" OH2_TITLE_ID "/logs", 0777);
  log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0666);
  sceUserServiceInitialize(0);
  uint32_t running = sceLncUtilGetAppIdOfRunningBigApp();
  char title[32] = "";
  if (running != 0 && running != 0xffffffffu) sceLncUtilGetAppTitleId(running, title);
  say("running big app: id 0x%x title '%s'", running, title);
#ifdef OH2_STATUS_ONLY
  return 0; /* oh2status: only report what is running */
#endif
  if (title[0] && strcmp(title, OH2_TITLE_ID) != 0) {
    say("another app is running: not launching over it");
    return 1;
  }
  if (!strcmp(title, OH2_TITLE_ID)) {
    uint32_t k = sceLncUtilKillApp(running);
    say("closed our earlier run (app 0x%x) -> 0x%08x", running, k);
    for (int i = 0; i < 40 && sceLncUtilGetAppIdOfRunningBigApp() == running; i++) usleep(250000);
  }
  int fg = -1, login = -1;
  sceUserServiceGetForegroundUser(&fg);
  login_users_t users;
  memset(&users, 0xff, sizeof users);
  if (sceUserServiceGetLoginUserIdList(&users) == 0)
    for (int i = 0; i < 4 && login < 0; i++)
      if (users.user_id[i] >= 0) login = users.user_id[i];
  struct {
    const char *name;
    int (*call)(const char *, const char *[], launch_param_t *);
    int user;
  } tries[] = {
      {"sceLncUtilLaunchApp/foreground", sceLncUtilLaunchApp, fg},
      {"sceLncUtilLaunchApp/login", sceLncUtilLaunchApp, login},
      {"sceSystemServiceLaunchApp/login", sceSystemServiceLaunchApp, login},
  };
  const char **argv = read_args();
  for (unsigned i = 0; i < sizeof tries / sizeof *tries; i++) {
    if (tries[i].user < 0) continue;
    launch_param_t p;
    memset(&p, 0, sizeof p);
    p.size = sizeof p;
    p.user_id = tries[i].user;
    uint32_t r = (uint32_t)tries[i].call(OH2_TITLE_ID, argv, &p);
    say("launch " OH2_TITLE_ID " via %s user 0x%x -> 0x%08x", tries[i].name, tries[i].user, r);
    if (r == 0 || r == ALREADY_RUNNING) return 0;
  }
  say("launch " OH2_TITLE_ID ": every call failed");
  return 1;
}
