/* Open Horizon 2 PS5 platform probe (docs/PS5_PORT.md, Step 3).
 *
 * Checks, one PASS/FAIL line each, what the runtime needs from the console:
 * executable memory, the GS base, umtx, page size, direct memory, range
 * reservation, signal contexts (register and FP offsets), and write faults
 * resolved by a handler. INFO lines carry the numbers behind them.
 *
 * Built as a payload by tools/ps5/build.sh probe (started with
 * tools/ps5/send_payload.sh), or with OH2_PROBE_TITLE as the eboot of a title
 * (tools/ps5/build.sh probe-title), which adds what only a title can answer:
 * its sandbox (which paths it sees and can read), direct-memory budget, and
 * open-file and malloc limits. The log is written one fsync'd line at a time
 * so a crash leaves everything before it.
 * Our own code, written from public FreeBSD and libkernel interfaces.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#ifdef OH2_PROBE_TITLE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#ifdef OH2_PROBE_TITLE
#define OH2_PROBE_CONTEXT "title"
/* The first of these a title can create (its sandbox may not show /data). */
static const char *const log_paths[] = {"/data/homebrew/PPSA99642/logs/probe-title.txt", "/download0/probe-title.txt"};
#else
#define OH2_PROBE_CONTEXT "payload"
static const char *const log_paths[] = {"/data/homebrew/PPSA99642/logs/probe-payload.txt"};
#endif
#define GAME_EXE "forza_durango_release_final.exe"

/* libkernel (no SDK header declares these). */
int sceKernelAllocateDirectMemory(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *phys);
int sceKernelAllocateMainDirectMemory(size_t len, size_t align, int type, int64_t *phys);
int sceKernelAllocateDirectMemoryForApp(int64_t start, int64_t end, size_t len, size_t align, int type, int64_t *phys);
int sceKernelReleaseDirectMemory(int64_t start, size_t len);
int sceKernelMapDirectMemory(void **addr, size_t len, int prot, int flags, int64_t phys, size_t align);
int64_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t *phys, size_t *size);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
int sceKernelConfiguredFlexibleMemorySize(size_t *size);
int sceKernelMprotect(const void *addr, size_t len, int prot);
int sceKernelReserveVirtualRange(void **addr, size_t len, int flags, size_t align);
int sceKernelMunmap(void *addr, size_t len);
int sceKernelQueryMemoryProtection(void *addr, void **start, void **end, uint32_t *prot);
uint64_t sceKernelGetTscFrequency(void);
struct sw_version { size_t size; char text[0x1c]; uint32_t version; };
int sceKernelGetSystemSwVersion(struct sw_version *v);
int sysarch(int number, void *args);
int _umtx_op(void *obj, int op, unsigned long val, void *uaddr, void *uaddr2);

#define K_READ 0x01
#define K_WRITE 0x02
#define K_EXEC 0x04
#define K_MAP_FIXED 0x10
#define DMEM_TYPE_CPU 12
#define AMD64_GET_GSBASE 130
#define AMD64_SET_GSBASE 131
#define UMTX_OP_WAKE 3
#define UMTX_OP_WAIT_UINT 11
#define UMTX_OP_WAIT_UINT_PRIVATE 15
#define UMTX_OP_WAKE_PRIVATE 16

/* ---- log ---- */

static int log_fd = -1;
static int n_pass, n_fail;

#ifdef OH2_PROBE_TITLE
/* A title's /download0 is a disk image FTP can't read into, and its sandbox
 * may not show /data: the log is also kept in memory and uploaded to the
 * console's own FTP server (ftpsrv, another process) over the loopback after
 * each test group, so a crash still leaves what came before. */
static char remote_log[64 << 10];
static size_t remote_len;

static int ftp_reply(int s, char *line, size_t n) {
  size_t k = 0;
  for (;;) { /* read until a final "NNN " line */
    char c;
    if (recv(s, &c, 1, 0) != 1) return -1;
    if (k + 1 < n) line[k++] = c;
    if (c == '\n') {
      line[k] = 0;
      if (k >= 4 && line[3] == ' ') return atoi(line);
      k = 0;
    }
  }
}

static int ftp_cmd(int s, const char *cmd, char *line, size_t n) {
  send(s, cmd, strlen(cmd), 0);
  return ftp_reply(s, line, n);
}

static int tcp_connect(uint32_t ip, int port) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
  if (s >= 0 && connect(s, (struct sockaddr *)&a, sizeof a) == 0) return s;
  if (s >= 0) close(s);
  return -1;
}

/* With OH2_LOG_HOST/PORT (tools/ps5/ps5.env): every line also goes straight
 * to tools/ps5/netlog.py on the PC, connected first thing in main. */
static int net_fd = -1;

static int upload_log(void) {
  char line[512];
  int c = tcp_connect(htonl(INADDR_LOOPBACK), 2121);
  if (c < 0) return -1;
  int ok = ftp_reply(c, line, sizeof line) == 220 && ftp_cmd(c, "USER anonymous\r\n", line, sizeof line) / 100 <= 3 &&
           ftp_cmd(c, "PASS oh2\r\n", line, sizeof line) / 100 == 2 && ftp_cmd(c, "TYPE I\r\n", line, sizeof line) / 100 == 2 &&
           ftp_cmd(c, "PASV\r\n", line, sizeof line) == 227;
  int d = -1;
  if (ok) {
    unsigned h[4], p[2];
    char *q = strchr(line, '(');
    ok = q && sscanf(q + 1, "%u,%u,%u,%u,%u,%u", &h[0], &h[1], &h[2], &h[3], &p[0], &p[1]) == 6;
    if (ok) d = tcp_connect(htonl(INADDR_LOOPBACK), (int)(p[0] * 256 + p[1]));
    ok = d >= 0 && ftp_cmd(c, "STOR /data/homebrew/PPSA99642/logs/probe-title.txt\r\n", line, sizeof line) / 100 == 1;
  }
  if (ok) send(d, remote_log, remote_len, 0);
  if (d >= 0) close(d);
  if (ok) ok = ftp_reply(c, line, sizeof line) == 226;
  ftp_cmd(c, "QUIT\r\n", line, sizeof line);
  close(c);
  return ok ? 0 : -1;
}
#endif

static void say(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
  buf[n++] = '\n';
  if (log_fd >= 0) {
    write(log_fd, buf, n);
    fsync(log_fd);
  }
#ifdef OH2_PROBE_TITLE
  if (net_fd >= 0) send(net_fd, buf, n, 0);
  if (remote_len + n <= sizeof remote_log) {
    memcpy(remote_log + remote_len, buf, n);
    remote_len += n;
  }
#endif
}

#ifdef OH2_PROBE_TITLE
#define CHECKPOINT() upload_log()
#else
#define CHECKPOINT() ((void)0)
#endif
#define INFO(name, ...) say("INFO " name ": " __VA_ARGS__)
#define PASS(name, ...) (n_pass++, say("PASS " name ": " __VA_ARGS__))
#define FAIL(name, ...) (n_fail++, say("FAIL " name ": " __VA_ARGS__))
#define CHECK(ok, name, ...) ((ok) ? PASS(name, __VA_ARGS__) : FAIL(name, __VA_ARGS__))

static double now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---- signals ----
 * One handler for every fault signal. A test sets `on_fault`; if it returns 1
 * the fault is handled and execution resumes. Otherwise, inside a guarded
 * test, we jump back to the guard and the test is reported as crashed. */

typedef int (*fault_fn)(int sig, siginfo_t *si, void *uc);
static volatile fault_fn on_fault;
static sigjmp_buf guard_jmp;
static volatile int guard_on;
static volatile int last_sig;
static void *volatile last_addr;

static void fault_handler(int sig, siginfo_t *si, void *uc) {
  fault_fn fn = on_fault;
  if (fn && fn(sig, si, uc)) return;
  last_sig = sig;
  last_addr = si ? si->si_addr : 0;
  if (guard_on) siglongjmp(guard_jmp, 1);
  say("CRASH signal %d at %p outside any test", sig, si ? si->si_addr : 0);
  _exit(70);
}

static void install_handlers(int flags) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = fault_handler;
  sa.sa_flags = SA_SIGINFO | flags;
  sigemptyset(&sa.sa_mask);
  int sigs[] = {SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE};
  for (unsigned i = 0; i < sizeof sigs / sizeof *sigs; i++) sigaction(sigs[i], &sa, 0);
}

/* Runs one test; an unexpected fault inside it is reported, not fatal.
 * `break` inside the body ends the test early. */
#define GUARDED(name, ...)                                                       \
  do {                                                                           \
    on_fault = 0;                                                                \
    if (sigsetjmp(guard_jmp, 1) == 0) {                                          \
      guard_on = 1;                                                              \
      do __VA_ARGS__ while (0);                                                  \
    } else {                                                                     \
      FAIL(name, "crashed: signal %d at %p", last_sig, last_addr);              \
    }                                                                            \
    guard_on = 0;                                                                \
    on_fault = 0;                                                                \
  } while (0)

/* ---- 1. system info and page size ---- */

static void test_info(void) {
  struct sw_version v = {sizeof v};
  if (sceKernelGetSystemSwVersion(&v) == 0) INFO("firmware", "%.28s (0x%08x)", v.text, v.version);
  INFO("context", "%s, pid %d", OH2_PROBE_CONTEXT, getpid());
  INFO("cpus", "sysconf online %ld configured %ld", sysconf(_SC_NPROCESSORS_ONLN), sysconf(_SC_NPROCESSORS_CONF));
  INFO("tsc", "%llu Hz", (unsigned long long)sceKernelGetTscFrequency());
  unsigned mxcsr;
  __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
  INFO("mxcsr", "0x%x at start", mxcsr);
  size_t flex_conf = 0, flex_avail = 0;
  int r1 = sceKernelConfiguredFlexibleMemorySize(&flex_conf);
  int r2 = sceKernelAvailableFlexibleMemorySize(&flex_avail);
  INFO("flexible memory", "configured %zu MiB (ret 0x%x), available %zu MiB (ret 0x%x)",
       flex_conf >> 20, r1, flex_avail >> 20, r2);
  int64_t dsize = sceKernelGetDirectMemorySize(), dphys = 0;
  size_t davail = 0;
  int r3 = sceKernelAvailableDirectMemorySize(0, dsize, 0x10000, &dphys, &davail);
  INFO("direct memory", "size %lld MiB, largest free block %zu MiB at 0x%llx (ret 0x%x)",
       (long long)(dsize >> 20), davail >> 20, (unsigned long long)dphys, r3);
  pthread_attr_t a;
  size_t stack = 0;
  pthread_attr_init(&a);
  pthread_attr_getstacksize(&a, &stack);
  pthread_attr_destroy(&a);
  INFO("threads", "default stack %zu KiB", stack >> 10);

  long ps = sysconf(_SC_PAGESIZE);
  CHECK(ps == 16384 && getpagesize() == 16384, "page size", "sysconf %ld, getpagesize %d (expected 16384)", ps, getpagesize());
}

/* ---- 2. executable memory ---- */

/* mov eax, imm32; ret */
static void put_code(void *p, uint32_t v) {
  uint8_t *c = p;
  c[0] = 0xB8;
  memcpy(c + 1, &v, 4);
  c[5] = 0xC3;
}
static uint32_t run_code(void *p) {
  return ((uint32_t(*)(void))p)();
}

static void test_exec_flexible(void) {
  void *x = mmap(0, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("exec at map time (flexible)", "mmap RWX %s (errno %d)", x == MAP_FAILED ? "refused" : "granted", x == MAP_FAILED ? errno : 0);
  if (x != MAP_FAILED) {
    uint32_t prot = 0;
    void *s, *e;
    sceKernelQueryMemoryProtection(x, &s, &e, &prot);
    INFO("exec at map time (flexible)", "granted mapping reports prot 0x%x", prot);
    munmap(x, 16384);
  }

  GUARDED("exec via sceKernelMprotect (flexible)", {
    uint8_t *p = mmap(0, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
      FAIL("exec via sceKernelMprotect (flexible)", "mmap RW failed, errno %d", errno);
      break;
    }
    put_code(p, 0x1234);
    int r = sceKernelMprotect(p, 16384, K_READ | K_EXEC);
    uint32_t got = r == 0 ? run_code(p) : 0;
    CHECK(r == 0 && got == 0x1234, "exec via sceKernelMprotect (flexible)", "RW map, write, mprotect RX ret 0x%x, call returned 0x%x", r, got);

    r = sceKernelMprotect(p, 16384, K_READ | K_WRITE | K_EXEC);
    got = 0;
    if (r == 0) {
      put_code(p, 0x5678); /* rewrite while RWX, no protection change */
      got = run_code(p);
    }
    CHECK(r == 0 && got == 0x5678, "RWX via sceKernelMprotect (flexible)", "mprotect RWX ret 0x%x, rewritten code returned 0x%x", r, got);

    r = mprotect(p, 16384, PROT_READ | PROT_EXEC);
    INFO("libc mprotect RX (flexible)", "ret %d errno %d", r, r ? errno : 0);
    munmap(p, 16384);
  });
}

/* ---- 3. GS base ---- */

static uint64_t gs_read0(void) {
  uint64_t v;
  __asm__ volatile("movq %%gs:0, %0" : "=r"(v));
  return v;
}
static uint64_t get_gsbase(void) {
  uint64_t b = 0;
  sysarch(AMD64_GET_GSBASE, &b);
  return b;
}

static uint64_t gs_block_main[8] __attribute__((aligned(64)));
static uint64_t gs_block_thread[8] __attribute__((aligned(64)));
static volatile uint64_t thread_saw_base, thread_saw_word;

static void *gs_thread(void *arg) {
  uint64_t before = get_gsbase();
  gs_block_thread[0] = 0x7777;
  uint64_t b = (uint64_t)gs_block_thread;
  sysarch(AMD64_SET_GSBASE, &b);
  usleep(5000);
  thread_saw_base = get_gsbase();
  thread_saw_word = gs_read0();
  sysarch(AMD64_SET_GSBASE, &before);
  return 0;
}

static void test_gsbase(void) {
  GUARDED("GS base", {
    uint64_t orig = get_gsbase();
    INFO("GS base", "original 0x%llx", (unsigned long long)orig);
    gs_block_main[0] = 0xC0FFEE;
    uint64_t want = (uint64_t)gs_block_main;
    int r = sysarch(AMD64_SET_GSBASE, &want);
    uint64_t back = get_gsbase();
    uint64_t word = gs_read0();
    CHECK(r == 0 && back == want && word == 0xC0FFEE, "GS base set/read",
          "sysarch SET ret %d, GET 0x%llx (want 0x%llx), gs:[0] 0x%llx", r, (unsigned long long)back,
          (unsigned long long)want, (unsigned long long)word);

    /* Kept across sleeps, yields and syscalls (context switches). */
    int ok = 1;
    for (int i = 0; i < 50 && ok; i++) {
      usleep(1000);
      sched_yield();
      getpid();
      ok = get_gsbase() == want && gs_read0() == 0xC0FFEE;
    }
    CHECK(ok, "GS base survives context switches", "50 sleep/yield/syscall rounds");

    /* Per thread: another thread's base doesn't disturb ours. */
    pthread_t t;
    pthread_create(&t, 0, gs_thread, 0);
    pthread_join(t, 0);
    CHECK(thread_saw_base == (uint64_t)gs_block_thread && thread_saw_word == 0x7777 && get_gsbase() == want && gs_read0() == 0xC0FFEE,
          "GS base per thread", "thread saw 0x%llx/0x%llx, main still 0x%llx", (unsigned long long)thread_saw_base,
          (unsigned long long)thread_saw_word, (unsigned long long)get_gsbase());
    sysarch(AMD64_SET_GSBASE, &orig);
  });
}

/* ---- 4. umtx ---- */

static volatile uint32_t umtx_word;
static volatile double umtx_woke_ms;
static volatile int umtx_ret, umtx_errno;

static void *umtx_waiter(void *arg) {
  int op = (int)(intptr_t)arg;
  double t0 = now_ms();
  struct timespec to = {2, 0};
  umtx_ret = _umtx_op((void *)&umtx_word, op, 0, 0, &to);
  umtx_errno = umtx_ret ? errno : 0;
  umtx_woke_ms = now_ms() - t0;
  return 0;
}

static void umtx_pair(const char *name, int wait_op, int wake_op) {
  umtx_word = 0;
  pthread_t t;
  pthread_create(&t, 0, umtx_waiter, (void *)(intptr_t)wait_op);
  usleep(50000);
  umtx_word = 1;
  int woken = _umtx_op((void *)&umtx_word, wake_op, 0x7fffffff, 0, 0);
  pthread_join(t, 0);
  CHECK(umtx_ret == 0 && umtx_woke_ms > 30 && umtx_woke_ms < 1500, "umtx wait/wake",
        "%s: waiter ret %d errno %d after %.1f ms, wake ret %d", name, umtx_ret, umtx_errno, umtx_woke_ms, woken);
}

static void test_umtx(void) {
  GUARDED("umtx wait/wake", {
    umtx_pair("private", UMTX_OP_WAIT_UINT_PRIVATE, UMTX_OP_WAKE_PRIVATE);
    umtx_pair("shared", UMTX_OP_WAIT_UINT, UMTX_OP_WAKE);
    /* Value already changed: must not sleep. */
    umtx_word = 5;
    double t0 = now_ms();
    struct timespec to = {1, 0};
    int r = _umtx_op((void *)&umtx_word, UMTX_OP_WAIT_UINT_PRIVATE, 4, 0, &to);
    double dt = now_ms() - t0;
    CHECK(dt < 100, "umtx value mismatch returns at once", "ret %d errno %d after %.2f ms", r, r ? errno : 0, dt);
    /* Timeout. */
    umtx_word = 0;
    t0 = now_ms();
    struct timespec to2 = {0, 30 * 1000 * 1000};
    r = _umtx_op((void *)&umtx_word, UMTX_OP_WAIT_UINT_PRIVATE, 0, 0, &to2);
    int e = r ? errno : 0;
    dt = now_ms() - t0;
    CHECK(r != 0 && e == ETIMEDOUT && dt > 20 && dt < 500, "umtx timeout", "30 ms wait: ret %d errno %d after %.1f ms", r, e, dt);
  });
}

/* ---- 5. range reservation, fixed mappings ---- */

static void test_reserve(void) {
  /* The game's image base and size class (forza-linux maps the PE at 0x140000000). */
  GUARDED("reserve at the game's image base", {
    void *want = (void *)0x140000000ull, *a = want;
    size_t len = 512ull << 20;
    int r = sceKernelReserveVirtualRange(&a, len, K_MAP_FIXED, 0x10000);
    CHECK(r == 0 && a == want, "reserve at the game's image base", "512 MiB at %p: ret 0x%x, got %p", want, r, a);
    if (r == 0) {
      /* Anonymous memory mapped fixed over part of the reservation. */
      uint8_t *p = mmap(a, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
      if (p != MAP_FAILED) {
        p[0] = 1;
        p[(1 << 20) - 1] = 2;
      }
      CHECK(p == a && p[0] == 1 && p[(1 << 20) - 1] == 2, "fixed anonymous map inside a reservation",
            "mmap MAP_FIXED 1 MiB at %p: got %p errno %d", a, p, p == MAP_FAILED ? errno : 0);
      /* A file mapped fixed inside the reservation (reported broken by prospero-win). */
      int fd = open("/data/homebrew/PPSA99642/ftp_test.txt", O_RDONLY);
      if (fd >= 0) {
        char *f = mmap((char *)a + (16 << 20), 16384, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
        int okf = f != MAP_FAILED && memcmp(f, "Open Horizon 2", 14) == 0;
        INFO("fixed file map inside a reservation", "%s (got %p errno %d)", okf ? "reads the file" : "does not work",
             f, f == MAP_FAILED ? errno : 0);
        close(fd);
      }
      sceKernelMunmap(a, len);
    }
  });

  GUARDED("reserve 16 GiB at 0x1000000000", {
    void *want = (void *)0x1000000000ull, *a = want;
    int r = sceKernelReserveVirtualRange(&a, 16ull << 30, K_MAP_FIXED, 0x10000);
    CHECK(r == 0 && a == want, "reserve 16 GiB at 0x1000000000", "ret 0x%x, got %p", r, a);
    if (r == 0) sceKernelMunmap(a, 16ull << 30);
    a = 0;
    r = sceKernelReserveVirtualRange(&a, 1ull << 30, 0, 0x10000);
    INFO("reserve 1 GiB with no address", "ret 0x%x, placed at %p", r, a);
    if (r == 0) sceKernelMunmap(a, 1ull << 30);
  });

  GUARDED("MAP_FIXED|MAP_EXCL keeps an existing mapping", {
    volatile uint8_t *p = mmap(0, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    p[0] = 0x55;
    void *q = mmap((void *)p, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_EXCL, -1, 0);
    int e = q == MAP_FAILED ? errno : 0;
    CHECK(q == MAP_FAILED && p[0] == 0x55, "MAP_FIXED|MAP_EXCL keeps an existing mapping",
          "second map %s (errno %d), first page byte 0x%x (0x55 = kept)", q == MAP_FAILED ? "refused" : "succeeded", e, p[0]);
    munmap((void *)p, 16384);
  });

  void *big = mmap(0, 4ull << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("mmap(NULL, 4 GiB, PROT_NONE)", "%s (errno %d)", big == MAP_FAILED ? "refused" : "granted", big == MAP_FAILED ? errno : 0);
  if (big != MAP_FAILED) munmap(big, 4ull << 30);
  void *flex = mmap(0, 512ull << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  INFO("mmap(NULL, 512 MiB, RW)", "%s (errno %d)", flex == MAP_FAILED ? "refused" : "granted", flex == MAP_FAILED ? errno : 0);
  if (flex != MAP_FAILED) munmap(flex, 512ull << 20);
}

/* ---- 6. direct memory ---- */

static int dmem_alloc(const char *how, size_t len, int64_t *phys) {
  int64_t size = sceKernelGetDirectMemorySize();
  int r;
  if (!strcmp(how, "AllocateDirectMemory"))
    r = sceKernelAllocateDirectMemory(0, size, len, 0x10000, DMEM_TYPE_CPU, phys);
  else if (!strcmp(how, "AllocateMainDirectMemory"))
    r = sceKernelAllocateMainDirectMemory(len, 0x10000, DMEM_TYPE_CPU, phys);
  else
    r = sceKernelAllocateDirectMemoryForApp(0, size, len, 0x10000, DMEM_TYPE_CPU, phys);
  return r;
}

static void test_dmem(void) {
  const char *hows[] = {"AllocateDirectMemory", "AllocateMainDirectMemory", "AllocateDirectMemoryForApp"};
  const char *used = 0;
  int64_t phys = 0;
  for (int i = 0; i < 3 && !used; i++) {
    int r = dmem_alloc(hows[i], 64 << 10, &phys);
    INFO("direct memory", "sceKernel%s(64 KiB, type %d): ret 0x%x phys 0x%llx", hows[i], DMEM_TYPE_CPU, r, (unsigned long long)phys);
    if (r == 0) used = hows[i];
  }
  if (!used) {
    FAIL("direct memory allocation", "every allocator refused (a payload has no direct memory budget; needs a title run)");
    return;
  }
  PASS("direct memory allocation", "64 KiB with sceKernel%s", used);

  GUARDED("direct memory fixed map", {
    void *resv = (void *)0x1000000000ull;
    int r = sceKernelReserveVirtualRange(&resv, 1ull << 30, K_MAP_FIXED, 0x10000);
    if (r) {
      FAIL("direct memory fixed map", "reserve 1 GiB at 0x1000000000 ret 0x%x", r);
      break;
    }
    void *a = (uint8_t *)resv + (64 << 20);
    void *want = a;
    r = sceKernelMapDirectMemory(&a, 64 << 10, K_READ | K_WRITE | K_EXEC, K_MAP_FIXED, phys, 0x10000);
    INFO("exec at map time (direct)", "MapDirectMemory RWX ret 0x%x (refusal expected)", r);
    if (r == 0) sceKernelMunmap(a, 64 << 10);
    a = want;
    r = sceKernelMapDirectMemory(&a, 64 << 10, K_READ | K_WRITE, K_MAP_FIXED, phys, 0x10000);
    if (r != 0 || a != want) {
      FAIL("direct memory fixed map", "MapDirectMemory RW fixed at %p ret 0x%x got %p", want, r, a);
      sceKernelMunmap(resv, 1ull << 30);
      break;
    }
    volatile uint64_t *w = a;
    for (int i = 0; i < 8192; i++) w[i] = 0x1111111111111111ull * (i & 15);
    int ok = 1;
    for (int i = 0; i < 8192; i++) ok &= w[i] == 0x1111111111111111ull * (i & 15);
    CHECK(ok, "direct memory fixed map", "64 KiB mapped RW at %p inside a reservation, written and read back", a);

    put_code(a, 0xD1EC);
    r = sceKernelMprotect(a, 16384, K_READ | K_WRITE | K_EXEC);
    uint32_t got = r == 0 ? run_code(a) : 0;
    CHECK(r == 0 && got == 0xD1EC, "exec via sceKernelMprotect (direct)", "mprotect RWX ret 0x%x, call returned 0x%x", r, got);
    sceKernelMunmap(resv, 1ull << 30);
  });
  sceKernelReleaseDirectMemory(phys, 64 << 10);

  /* A large allocation, as the game's title memory will need. Not from the
   * foreground app's pool: that could starve whatever is running. */
  if (!strcmp(used, "AllocateDirectMemoryForApp")) {
    INFO("direct memory 1 GiB", "skipped: only the app-pool allocator worked here");
    return;
  }
  int64_t big = 0;
  int r = dmem_alloc(used, 1ull << 30, &big);
  CHECK(r == 0, "direct memory 1 GiB", "sceKernel%s(1 GiB) ret 0x%x", used, r);
  if (r == 0) sceKernelReleaseDirectMemory(big, 1ull << 30);
#ifdef OH2_PROBE_TITLE
  /* The game's own memory (the Xbox One gives a title ~5 GiB) plus the GPU's:
   * allocate, map fixed in a reservation, touch both ends, release. */
  for (int gib = 4; gib <= 8; gib += 4) {
    size_t len = (size_t)gib << 30;
    big = 0;
    r = dmem_alloc(used, len, &big);
    int mapped = 0;
    if (r == 0) {
      void *resv = (void *)0x1000000000ull;
      if (sceKernelReserveVirtualRange(&resv, 16ull << 30, K_MAP_FIXED, 0x10000) == 0) {
        void *a = resv;
        if (sceKernelMapDirectMemory(&a, len, K_READ | K_WRITE, K_MAP_FIXED, big, 0x10000) == 0) {
          volatile uint64_t *w = a;
          w[0] = 1;
          w[len / 8 - 1] = 2;
          mapped = w[0] == 1 && w[len / 8 - 1] == 2;
        }
        sceKernelMunmap(resv, 16ull << 30);
      }
      sceKernelReleaseDirectMemory(big, len);
    }
    CHECK(r == 0 && mapped, "direct memory large", "%d GiB: allocate ret 0x%x, mapped and touched at both ends: %s",
          gib, r, mapped ? "yes" : "no");
  }
#endif
}

/* ---- 7. signal context layout ---- */

/* Loads markers into every general register and xmm0-15, then reads from
 * rdi (a PROT_NONE page). The handler finds each marker in the ucontext,
 * moves rip to probe_ctx_resume and returns. */
__asm__(".text\n"
        ".globl probe_ctx_fault\n"
        "probe_ctx_fault:\n"
        "  push %rbx\n  push %rbp\n  push %r12\n  push %r13\n  push %r14\n  push %r15\n"
        "  movdqu 0(%rsi),%xmm0\n  movdqu 16(%rsi),%xmm1\n  movdqu 32(%rsi),%xmm2\n  movdqu 48(%rsi),%xmm3\n"
        "  movdqu 64(%rsi),%xmm4\n  movdqu 80(%rsi),%xmm5\n  movdqu 96(%rsi),%xmm6\n  movdqu 112(%rsi),%xmm7\n"
        "  movdqu 128(%rsi),%xmm8\n  movdqu 144(%rsi),%xmm9\n  movdqu 160(%rsi),%xmm10\n  movdqu 176(%rsi),%xmm11\n"
        "  movdqu 192(%rsi),%xmm12\n  movdqu 208(%rsi),%xmm13\n  movdqu 224(%rsi),%xmm14\n  movdqu 240(%rsi),%xmm15\n"
        "  mov %rdi,%r11\n"
        "  movabs $0x0f1e2d3c4b5a0001,%rax\n"
        "  movabs $0x0f1e2d3c4b5a0002,%rbx\n"
        "  movabs $0x0f1e2d3c4b5a0003,%rcx\n"
        "  movabs $0x0f1e2d3c4b5a0004,%rdx\n"
        "  movabs $0x0f1e2d3c4b5a0005,%rsi\n"
        "  movabs $0x0f1e2d3c4b5a0006,%rdi\n"
        "  movabs $0x0f1e2d3c4b5a0007,%rbp\n"
        "  movabs $0x0f1e2d3c4b5a0008,%r8\n"
        "  movabs $0x0f1e2d3c4b5a0009,%r9\n"
        "  movabs $0x0f1e2d3c4b5a000a,%r10\n"
        "  movabs $0x0f1e2d3c4b5a000c,%r12\n"
        "  movabs $0x0f1e2d3c4b5a000d,%r13\n"
        "  movabs $0x0f1e2d3c4b5a000e,%r14\n"
        "  movabs $0x0f1e2d3c4b5a000f,%r15\n"
        ".globl probe_ctx_insn\n"
        "probe_ctx_insn:\n"
        "  mov (%r11),%r11\n"
        ".globl probe_ctx_resume\n"
        "probe_ctx_resume:\n"
        "  pop %r15\n  pop %r14\n  pop %r13\n  pop %r12\n  pop %rbp\n  pop %rbx\n"
        "  ret\n");
void probe_ctx_fault(void *fault_at, const void *xmm_markers);
extern char probe_ctx_insn[], probe_ctx_resume[];

#define CTX_SCAN 1536
static uint8_t ctx_copy[CTX_SCAN];
static volatile int ctx_sig, ctx_rip_off = -1;
static void *volatile ctx_addr;
static volatile uint64_t ctx_gsbase_in_handler;

static int ctx_on_fault(int sig, siginfo_t *si, void *uc) {
  ctx_sig = sig;
  ctx_addr = si->si_addr;
  ctx_gsbase_in_handler = get_gsbase();
  memcpy(ctx_copy, uc, CTX_SCAN);
  uint64_t *w = uc;
  for (int i = 0; i < CTX_SCAN / 8; i++)
    if (w[i] == (uint64_t)probe_ctx_insn) {
      ctx_rip_off = i * 8;
      w[i] = (uint64_t)probe_ctx_resume;
      return 1;
    }
  return 0;
}

static int find_u64(uint64_t v) {
  for (int i = 0; i + 8 <= CTX_SCAN; i += 8)
    if (!memcmp(ctx_copy + i, &v, 8)) return i;
  return -1;
}

static void test_signal_context(void) {
  GUARDED("SIGSEGV caught with context", {
    uint8_t *page = mmap(0, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t xmm[32];
    for (int i = 0; i < 16; i++) {
      xmm[2 * i] = 0xa5a5000000000000ull | i;
      xmm[2 * i + 1] = 0xc3c3000000000000ull | i;
    }
    uint64_t mygs[4] = {0x65};
    uint64_t orig_gs = get_gsbase(), want_gs = (uint64_t)mygs;
    sysarch(AMD64_SET_GSBASE, &want_gs);
    on_fault = ctx_on_fault;
    probe_ctx_fault(page + 128, xmm);
    on_fault = 0;
    sysarch(AMD64_SET_GSBASE, &orig_gs);

    CHECK(ctx_rip_off >= 0 && ctx_addr == page + 128, "SIGSEGV caught with context",
          "signal %d, si_addr %p (want %p), rip found at ucontext+%d and resumed", ctx_sig, ctx_addr, page + 128, ctx_rip_off);
    const char *names[] = {"", "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10", "", "r12", "r13", "r14", "r15"};
    char line[512];
    int n = 0;
    for (int i = 1; i < 16; i++)
      if (names[i][0]) n += snprintf(line + n, sizeof line - n, " %s+%d", names[i], find_u64(0x0f1e2d3c4b5a0000ull | i));
    INFO("ucontext offsets", "%s rip+%d r11(fault addr)+%d", line, ctx_rip_off, find_u64((uint64_t)(page + 128)));
    /* FreeBSD amd64 layout: mcontext = { onstack, rdi, rsi, rdx, rcx, r8, r9, rax, rbx, rbp, r10..r15, ... rip at +160 }. */
    int rdi = find_u64(0x0f1e2d3c4b5a0006ull);
    int mc = rdi - 8;
    CHECK(mc == 64 && ctx_rip_off == mc + 160, "mcontext at ucontext+64",
          "rdi at +%d -> mcontext at +%d, rip at mcontext+%d (FreeBSD: +160)", rdi, mc, ctx_rip_off - mc);
    /* FXSAVE image: xmm0 at +160 within it, MXCSR at +24. */
    int x0 = -1;
    for (int i = 0; i + 16 <= CTX_SCAN; i += 16)
      if (!memcmp(ctx_copy + i, &xmm[0], 16)) { x0 = i; break; }
    int fx = x0 >= 0 ? x0 - 160 : -1;
    uint32_t mx = 0;
    if (fx >= 0) memcpy(&mx, ctx_copy + fx + 24, 4);
    int x15ok = fx >= 0 && fx + 160 + 16 * 16 <= CTX_SCAN && !memcmp(ctx_copy + fx + 160 + 15 * 16, &xmm[30], 16);
    CHECK(fx == 320 && x15ok, "FXSAVE image at ucontext+320", "xmm0 at +%d -> FXSAVE at +%d, xmm15 %s, MXCSR there 0x%x",
          x0, fx, x15ok ? "in place" : "NOT found", mx);
    CHECK(ctx_gsbase_in_handler == want_gs, "GS base inside the signal handler",
          "handler saw 0x%llx (set 0x%llx)", (unsigned long long)ctx_gsbase_in_handler, (unsigned long long)want_gs);
    INFO("GS base in the ucontext", "found at +%d", find_u64(want_gs));
    munmap(page, 16384);
  });
}

/* ---- 8. write faults resolved by the handler (GPU write tracking) ---- */

static uint8_t *volatile wf_base;
static volatile size_t wf_len;
static volatile int wf_faults, wf_sig;
static int wf_on_fault(int sig, siginfo_t *si, void *uc) {
  uint8_t *a = si->si_addr;
  if (a < wf_base || a >= wf_base + wf_len) return 0;
  uintptr_t page = (uintptr_t)a & ~(uintptr_t)16383;
  wf_sig = sig;
  wf_faults++;
  return sceKernelMprotect((void *)page, 16384, K_READ | K_WRITE) == 0;
}

static void test_write_faults(void) {
  GUARDED("write fault resolved by handler", {
    size_t len = 64 * 16384;
    uint8_t *p = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    memset(p, 0, len);
    wf_base = p;
    wf_len = len;
    wf_faults = 0;
    on_fault = wf_on_fault;
    double t0 = now_ms();
    int ok = 1;
    for (int round = 0; round < 10; round++) {
      sceKernelMprotect(p, len, K_READ);
      for (int i = 0; i < 64; i++) {
        volatile uint64_t *w = (volatile uint64_t *)(p + i * 16384 + 8 * (round + 1));
        *w = 0xabc0000 + i;
        ok &= *w == 0xabc0000u + i;
      }
    }
    double dt = now_ms() - t0;
    on_fault = 0;
    CHECK(ok && wf_faults == 640, "write fault resolved by handler", "640 writes to read-only pages: %d faults (signal %d), all values landed, %.1f us per fault+mprotect",
          wf_faults, wf_sig, dt * 1000 / (wf_faults ? wf_faults : 1));

    /* Protection granularity: does a 4 KiB mprotect protect the whole 16 KiB page? */
    sceKernelMprotect(p, len, K_READ | K_WRITE);
    int r = sceKernelMprotect(p + 4096, 4096, K_READ);
    wf_faults = 0;
    on_fault = wf_on_fault;
    p[0] = 1;            /* same 16 KiB page, outside the 4 KiB range */
    int faults_low = wf_faults;
    on_fault = 0;
    INFO("4 KiB mprotect granularity", "sceKernelMprotect(base+4K, 4K, R) ret 0x%x; a write to base+0 %s",
         r, faults_low ? "faulted (whole 16 KiB page protected)" : "did not fault");
    munmap(p, len);
  });
}

/* ---- 9. other signals the runtime uses ---- */

static volatile int trap_seen;
static int trap_on_fault(int sig, siginfo_t *si, void *uc) {
  if (sig != SIGTRAP && sig != SIGILL) return 0;
  trap_seen = sig;
  if (sig == SIGILL && ctx_rip_off >= 0) *(uint64_t *)((uint8_t *)uc + ctx_rip_off) += 2; /* skip ud2 */
  return 1;
}

static volatile uintptr_t alt_lo, alt_hi, alt_seen;
static int alt_on_fault(int sig, siginfo_t *si, void *uc) {
  int local;
  alt_seen = (uintptr_t)&local;
  return trap_on_fault(sig, si, uc);
}

static void test_other_signals(void) {
  GUARDED("SIGTRAP from int3", {
    trap_seen = 0;
    on_fault = trap_on_fault;
    __asm__ volatile("int3");
    on_fault = 0;
    CHECK(trap_seen == SIGTRAP, "SIGTRAP from int3", "signal %d, resumed after int3", trap_seen);
  });
  GUARDED("SIGILL from ud2", {
    trap_seen = 0;
    if (ctx_rip_off < 0) {
      FAIL("SIGILL from ud2", "skipped: rip offset unknown");
      break;
    }
    on_fault = trap_on_fault;
    __asm__ volatile("ud2");
    on_fault = 0;
    CHECK(trap_seen == SIGILL, "SIGILL from ud2", "signal %d, rip moved past ud2 and resumed", trap_seen);
  });
  GUARDED("sigaltstack delivery", {
    size_t sz = 256 << 10;
    void *st = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    stack_t ss = {.ss_sp = st, .ss_size = sz, .ss_flags = 0};
    int r = sigaltstack(&ss, 0);
    install_handlers(SA_ONSTACK);
    alt_lo = (uintptr_t)st;
    alt_hi = alt_lo + sz;
    alt_seen = 0;
    trap_seen = 0;
    on_fault = alt_on_fault;
    __asm__ volatile("int3");
    on_fault = 0;
    install_handlers(0);
    CHECK(r == 0 && alt_seen >= alt_lo && alt_seen < alt_hi, "sigaltstack delivery",
          "sigaltstack ret %d, handler stack %p in [%p, %p)", r, (void *)alt_seen, (void *)alt_lo, (void *)alt_hi);
    ss.ss_flags = SS_DISABLE;
    sigaltstack(&ss, 0);
    munmap(st, sz);
  });
}

/* ---- 10. files and storage ---- */

static void test_files(void) {
  const char *f = "/data/homebrew/PPSA99642/ftp_test.txt";
  struct stat st;
  int a = access(f, R_OK), ae = a ? errno : 0;
  int s = stat(f, &st), se = s ? errno : 0;
  INFO("access vs stat", "%s: access ret %d errno %d, stat ret %d errno %d", f, a, ae, s, se);
  for (int i = 0; i < 8; i++) {
    char path[32];
    snprintf(path, sizeof path, "/mnt/usb%d", i);
    DIR *d = opendir(path);
    if (!d) continue;
    int n = 0;
    char first[3][64] = {{0}};
    struct dirent *e;
    while ((e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      if (n < 3) snprintf(first[n], sizeof first[n], "%s", e->d_name);
      n++;
    }
    closedir(d);
    if (n) INFO("usb", "%s: %d entries (%s %s %s)", path, n, first[0], first[1], first[2]);
  }
  for (int i = 0; i < 8; i++) {
    char p[160];
    snprintf(p, sizeof p, "/mnt/usb%d/Anthem_1.0.0.64_x64__8wekyb3d8bbwe/Mount/forza_durango_release_final.exe", i);
    if (stat(p, &st) == 0) INFO("dump", "found the game exe on /mnt/usb%d (%lld bytes)", i, (long long)st.st_size);
  }
}

#ifdef OH2_PROBE_TITLE
/* stat, lstat, access and a read of one path, as a title sees it. */
static int probe_path(const char *p, int want_mz) {
  struct stat st;
  int s = stat(p, &st), se = s ? errno : 0;
  int l = lstat(p, &st), le = l ? errno : 0;
  int a = access(p, R_OK), ae = a ? errno : 0;
  char extra[96] = "";
  int readable = 0;
  if (s == 0 && S_ISDIR(st.st_mode)) {
    DIR *d = opendir(p);
    int n = 0;
    if (d) {
      while (readdir(d)) n++;
      closedir(d);
    }
    readable = d != 0;
    snprintf(extra, sizeof extra, ", opendir %s (%d entries)", d ? "ok" : "fails", n);
  } else if (s == 0) {
    int fd = open(p, O_RDONLY);
    char b[2] = {0};
    int r = fd >= 0 ? (int)read(fd, b, 2) : -1;
    if (fd >= 0) close(fd);
    readable = r == 2 && (!want_mz || (b[0] == 'M' && b[1] == 'Z'));
    snprintf(extra, sizeof extra, ", %lld bytes, read %s", (long long)st.st_size, readable ? "ok" : "fails");
  }
  INFO("sandbox", "%s: stat %d/%d lstat %d/%d access %d/%d%s", p, s, se, l, le, a, ae, extra);
  return readable;
}

/* Finds <root>/<folder>/Mount/<exe> one level down, as the launcher will. */
static int find_dump_under(const char *root, char *out, size_t n) {
  DIR *d = opendir(root);
  if (!d) return 0;
  struct dirent *e;
  struct stat st;
  int found = 0;
  while (!found && (e = readdir(d))) {
    if (e->d_name[0] == '.') continue;
    snprintf(out, n, "%s/%s/Mount/" GAME_EXE, root, e->d_name);
    found = stat(out, &st) == 0;
  }
  closedir(d);
  return found;
}

static void test_title_sandbox(void) {
  const char *paths[] = {"/app0", "/app0/eboot.bin", "/download0", "/data", "/data/homebrew", "/data/homebrew/PPSA99642",
                         "/data/OpenHorizon2", "/mnt/usb0", "/mnt/usb1", "/user", "/system"};
  for (unsigned i = 0; i < sizeof paths / sizeof *paths; i++) probe_path(paths[i], 0);
  char exe[512];
  int internal = find_dump_under("/data/OpenHorizon2", exe, sizeof exe) && probe_path(exe, 1);
  CHECK(internal, "title reads the dump on internal storage", "/data/OpenHorizon2/<folder>/Mount/" GAME_EXE "%s", internal ? "" : " not readable");
  int usb = 0;
  for (int i = 0; i < 8 && !usb; i++) {
    char root[16];
    snprintf(root, sizeof root, "/mnt/usb%d", i);
    usb = find_dump_under(root, exe, sizeof exe) && probe_path(exe, 1);
  }
  INFO("title reads a dump on USB", "%s", usb ? exe : "none readable (stick may be unplugged)");

  int fd = open("/download0/oh2_probe_write.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  int w = fd >= 0 ? (int)write(fd, "ok", 2) : -1;
  if (fd >= 0) close(fd);
  int u = unlink("/download0/oh2_probe_write.tmp");
  CHECK(w == 2 && u == 0, "/download0 writable", "write %d, unlink %d (errno %d)", w, u, u ? errno : 0);
}

static void test_title_limits(void) {
  static int fds[4096];
  int n = 0, e = 0;
  while (n < 4096) {
    int fd = open("/app0/sce_sys/param.json", O_RDONLY);
    if (fd < 0) {
      e = errno;
      break;
    }
    fds[n++] = fd;
  }
  for (int i = 0; i < n; i++) close(fds[i]);
  INFO("open files", "%d opens of one path before failing (errno %d)", n, e);

  static void *blocks[2048];
  int m = 0;
  while (m < 2048) {
    char *b = malloc(1 << 20);
    if (!b) break;
    b[0] = 1;
    blocks[m++] = b;
  }
  for (int i = 0; i < m; i++) free(blocks[i]);
  INFO("libc malloc", "%d MiB in 1 MiB blocks before NULL (cap 2048)", m);
}
#endif

int main(void) {
#if defined(OH2_PROBE_TITLE) && defined(OH2_LOG_HOST)
  net_fd = tcp_connect(inet_addr(OH2_LOG_HOST), OH2_LOG_PORT);
  if (net_fd >= 0) send(net_fd, "hello from the title's main\n", 28, 0);
#endif
  mkdir("/data/homebrew/PPSA99642/logs", 0777);
  const char *log_path = 0;
  for (unsigned i = 0; i < sizeof log_paths / sizeof *log_paths && log_fd < 0; i++) {
    log_fd = open(log_paths[i], O_WRONLY | O_CREAT | O_TRUNC, 0666);
    log_path = log_paths[i];
  }
  say("Open Horizon 2 platform probe, %s build, %s %s, log %s", OH2_PROBE_CONTEXT, __DATE__, __TIME__, log_path);
  install_handlers(0);
#ifdef OH2_PROBE_TITLE
  say("INFO log: local file %s", log_fd >= 0 ? "open" : "not writable, loopback FTP only");
  if (upload_log()) say("INFO log: loopback FTP upload failed");
  test_title_sandbox(); /* first: what the port needs most */
  CHECKPOINT();
#endif
  test_info();
  CHECKPOINT();
  test_exec_flexible();
  test_gsbase();
  test_umtx();
  CHECKPOINT();
  test_reserve();
  test_dmem();
  CHECKPOINT();
  test_signal_context();
  test_write_faults();
  test_other_signals();
  CHECKPOINT();
  test_files();
#ifdef OH2_PROBE_TITLE
  test_title_limits();
#endif
  say("DONE %d pass, %d fail", n_pass, n_fail);
  CHECKPOINT();
  close(log_fd);
  return 0;
}
