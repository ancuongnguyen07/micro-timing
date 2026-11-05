/* src/sibling.c  -  helper thread on the SMT sibling, switchable idle/busy */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench_config.h"
#include "sibling.h"

enum { ST_IDLE, ST_BUSY, ST_QUIT, ST_ALT };

static pthread_t thr;
static int started;
static int sib_cpu = -1;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv_wake = PTHREAD_COND_INITIALIZER; /* main -> worker */
static pthread_cond_t cv_ack = PTHREAD_COND_INITIALIZER;  /* worker -> main */
static atomic_int state = ST_IDLE; /* requested state (written under mu) */
static int parked;                 /* worker blocked in cond_wait (under mu) */
static atomic_int spinning;        /* worker is inside its busy loop */

/* ---- command mode (see sibling.h) ---- */
static const char *cmd;             /* NULL = built-in busy_chunk() */
static volatile pid_t cmd_pid = -1; /* process group leader, -1 = none */

void sibling_set_command(const char *c) { cmd = (c && *c) ? c : NULL; }
const char *sibling_get_command(void) { return cmd; }

/* bench was interrupted: do not leave the load running on the sibling.
 * Only async-signal-safe calls here. */
static void kill_cmd_on_signal(int sig) {
  pid_t p = cmd_pid;
  if (p > 0)
    kill(-p, SIGKILL);
  signal(sig, SIG_DFL);
  raise(sig);
}

static void install_signal_cleanup(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = kill_cmd_on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGHUP, &sa, NULL);
}

/* Parse /sys/.../cpu<cpu>/topology/thread_siblings_list ("2-3", "0,8", ...)
 * and return the first CPU in it that is not `cpu`, or -1. */
static int find_sibling(int cpu) {
  char path[128], buf[256];
  snprintf(path, sizeof path,
           "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);

  FILE *f = fopen(path, "r");
  if (!f)
    return -1;
  if (!fgets(buf, sizeof buf, f)) {
    fclose(f);
    return -1;
  }
  fclose(f);

  for (char *p = buf; *p;) {
    char *end;
    long lo = strtol(p, &end, 10);
    if (end == p)
      break;
    long hi = lo;
    if (*end == '-')
      hi = strtol(end + 1, &end, 10);
    for (long c = lo; c <= hi; c++)
      if (c != cpu)
        return (int)c;
    p = end;
    if (*p == ',')
      p++;
  }
  return -1;
}

/* ======================================================================
 * SIBLING BUSY WORKLOAD  --  *** THIS IS THE PART YOU EDIT ***
 *
 * busy_chunk() is what the helper thread runs on the SMT sibling while the
 * benchmark is in "busy" mode. worker() calls it in a loop for as long as
 * the busy state is requested, and re-checks that request between calls.
 * So the workload is effectively endless; each call only has to be one
 * short slice of it.
 *
 * Which branch is compiled is chosen by SIBLING_LOAD in include/bench_config.h
 * (the choice is also recorded in the CSV column `sibling_load`).
 *
 * Rules for a chunk:
 *   - it MUST return after a short time (microseconds to a few ms). Do not
 *     put an unbounded loop inside it: sibling_idle()/sibling_stop() wait
 *     for the chunk to return and would hang.
 *   - do not block (no sleep/lock/IO) if you want the sibling to stay busy;
 *     a blocked thread makes the CPU halt, which is the "idle" state.
 *   - use `__asm__ volatile` (or volatile accesses) so the compiler does
 *     not delete the work as dead code.
 *   - the thread is already pinned to the sibling CPU; you need no setup
 *     for that. State you want to keep between chunks can live in `static`
 *     variables.
 *   - SIBLING_CHUNK_ITERS (bench_config.h) is the usual loop count.
 * ====================================================================== */
static void busy_chunk(void) {
#if SIBLING_LOAD == SIBLING_LOAD_PAUSE
  /* PAUSE: spinning, but tells the core to favour the other thread */
  for (int i = 0; i < SIBLING_CHUNK_ITERS; i++)
    __asm__ volatile("pause");

#elif SIBLING_LOAD == SIBLING_LOAD_IMUL
  /* dependent imul chain: occupies the multiplier / its scheduler queue */
  unsigned long x = 3;
  for (int i = 0; i < SIBLING_CHUNK_ITERS; i++)
    __asm__ volatile("imul %0, %0" : "+r"(x));

#elif SIBLING_LOAD == SIBLING_LOAD_EMPTY
  /* Empty loop: only the loop counter (decrement + branch) executes. The
   * volatile empty asm statement stops the compiler from removing the loop
   * but emits no instruction of its own. No PAUSE, so the sibling issues at
   * full speed; it is "infinite" because worker() keeps calling this until
   * the benchmark leaves the busy state. */
  for (int i = 0; i < SIBLING_CHUNK_ITERS; i++)
    __asm__ volatile("");

#elif SIBLING_LOAD == SIBLING_LOAD_CUSTOM
  /* ---- YOUR WORKLOAD GOES HERE (select it with SIBLING_LOAD_CUSTOM) ----
   * Replace the example below. Examples of things you could run:
   *   dependent sqrtsd chain : __asm__ volatile("sqrtsd %0, %0" : "+x"(d));
   *   independent adds       : four separate registers per iteration
   *   load/store traffic     : walk a static buffer with a volatile pointer
   *   a block of your own instructions (the same code as in body.inc)
   * Remember the rules above: return after SIBLING_CHUNK_ITERS iterations. */
  unsigned long x = 1;
  for (int i = 0; i < SIBLING_CHUNK_ITERS; i++)
    __asm__ volatile("add $1, %0" : "+r"(x)); /* example: dependent add chain */
#endif
}

/* ---- alternating mode: imul segment, nop segment, imul segment, ... ---- */
static volatile uint32_t *alt_seg_id; /* where the current segment id goes */
static atomic_int alt_armed;          /* calibrated, waiting for alt_go */
static atomic_int alt_go;

static uint64_t rdpru_now(void) {
  uint32_t lo, hi;
  __asm__ volatile("lfence\n\tmov %2, %%ecx\n\trdpru\n\tlfence"
                   : "=a"(lo), "=d"(hi)
                   : "i"(RDPRU_SEL)
                   : "ecx", "memory");
  return ((uint64_t)hi << 32) | lo;
}

/* Loop count that makes fn run for about TS_SEG_TICKS (RDPRU ticks). */
static uint64_t calibrate(void (*fn)(uint64_t)) {
  const uint64_t n = 2000;
  uint64_t best = UINT64_MAX;
  fn(n); /* warm up */
  for (int i = 0; i < 5; i++) {
    uint64_t t0 = rdpru_now();
    fn(n);
    uint64_t dt = rdpru_now() - t0;
    if (dt < best)
      best = dt;
  }
  uint64_t iters = best ? (uint64_t)TS_SEG_TICKS * n / best : 1;
  return iters ? iters : 1;
}

static void alt_run(void) {
  void (*const kernel[2])(uint64_t) = {sib_imul, sib_nop};
  uint64_t iters[2] = {calibrate(sib_imul), calibrate(sib_nop)};
  atomic_store(&alt_armed, 1);
  while (!atomic_load(&alt_go) &&
         atomic_load_explicit(&state, memory_order_relaxed) == ST_ALT)
    __builtin_ia32_pause();
  for (uint32_t id = 1;
       atomic_load_explicit(&state, memory_order_relaxed) == ST_ALT; id++) {
    int k = (id - 1) & 1; /* odd ids -> imul, even -> nop */
    __atomic_store_n(alt_seg_id, id, __ATOMIC_RELEASE);
    kernel[k](iters[k]);
  }
  atomic_store(&alt_armed, 0);
  atomic_store(&alt_go, 0);
}

static void *worker(void *arg) {
  (void)arg; /* already pinned to sib_cpu by the attribute in sibling_start */
  pthread_mutex_lock(&mu);
  for (;;) {
    while (atomic_load(&state) == ST_IDLE) {
      parked = 1;
      pthread_cond_signal(&cv_ack);
      pthread_cond_wait(&cv_wake, &mu); /* kernel puts this CPU to sleep */
    }
    parked = 0;
    if (atomic_load(&state) == ST_QUIT)
      break;
    pthread_mutex_unlock(&mu);
    atomic_store(&spinning, 1);
    if (atomic_load(&state) == ST_ALT)
      alt_run();
    else
      while (atomic_load_explicit(&state, memory_order_relaxed) == ST_BUSY)
        busy_chunk();
    atomic_store(&spinning, 0);
    pthread_mutex_lock(&mu);
  }
  pthread_mutex_unlock(&mu);
  return NULL;
}

static void wait_parked(void) {
  pthread_mutex_lock(&mu);
  while (!parked)
    pthread_cond_wait(&cv_ack, &mu);
  pthread_mutex_unlock(&mu);
}

int sibling_start(int cpu) {
  sib_cpu = find_sibling(cpu);
  if (sib_cpu < 0)
    return SIBLING_NONE;

  /* New threads inherit the creator's mask (= only `cpu`), so give the thread
   * its own mask at creation. It is then born on the sibling and never runs on
   * `cpu`; if the pin is impossible (CPU not allowed by cpuset/cgroup, ...)
   * pthread_create fails and we report it instead of running unpinned. */
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(sib_cpu, &set);
  pthread_attr_t attr;

  int err = pthread_attr_init(&attr);
  if (!err)
    err = pthread_attr_setaffinity_np(&attr, sizeof set, &set);
  if (!err)
    err = pthread_create(&thr, &attr, worker, NULL);

  pthread_attr_destroy(&attr);
  if (err) {
    fprintf(stderr, "sibling: cannot start helper pinned to CPU %d: %s\n",
            sib_cpu, strerror(err));
    sib_cpu = -1;
    return SIBLING_ERROR;
  }
  started = 1;
  wait_parked();
  return sib_cpu;
}

static void ms_sleep(int ms) {
  struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* ---- watchdog: also kill the command when bench dies without cleaning up ----
 * Signal handlers and atexit() do not run on SIGKILL / crash / OOM kill. So a
 * tiny separate process (the watchdog) blocks on a pipe whose write end only
 * bench holds. When bench ends normally it writes 'D' ("done") before closing
 * the pipe and the watchdog exits quietly. If the pipe closes WITHOUT 'D',
 * bench vanished: the watchdog SIGKILLs the command's process group.
 * The pipe is created after the command was forked, so the command never
 * inherits the write end (that would keep the pipe open forever). */
static pid_t wd_pid = -1; /* watchdog process, -1 = none */
static int wd_fd = -1;    /* bench's write end of the pipe */

static int start_watchdog(pid_t pgid) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) != 0)
    return -1;
  pid_t w = fork();
  if (w < 0) {
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (w == 0) {
    /* watchdog: only async-signal-safe calls (bench has other threads) */
    close(fds[1]);
    setsid(); /* survive terminal hangup / Ctrl-C aimed at bench */
    signal(SIGINT, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    char c = 0;
    ssize_t n;
    do {
      n = read(fds[0], &c, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1 || c != 'D')
      kill(-pgid, SIGKILL); /* EOF without 'D': bench is gone */
    _exit(0);
  }
  close(fds[0]);
  wd_pid = w;
  wd_fd = fds[1];
  return 0;
}

/* Normal end: tell the watchdog it is not needed, and reap it. */
static void dismiss_watchdog(void) {
  if (wd_pid <= 0)
    return;
  ssize_t r = write(wd_fd, "D", 1);
  (void)r;
  close(wd_fd);
  wd_fd = -1;
  waitpid(wd_pid, NULL, 0);
  wd_pid = -1;
}

/* Start `cmd` as a process confined to the sibling CPU. 0 = running. */
static int start_command(void) {
  fflush(NULL); /* do not duplicate buffered output in the child */
  pid_t p = fork();
  if (p < 0) {
    fprintf(stderr, "sibling: fork failed: %s\n", strerror(errno));
    return -1;
  }
  if (p == 0) {
    /* child: own group, die with bench, pin to the sibling, silence, exec */
    setpgid(0, 0);
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(sib_cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
      _exit(126); /* could not pin: never run unpinned */
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
      dup2(fd, 0);
      dup2(fd, 1);
      dup2(fd, 2);
      if (fd > 2)
        close(fd);
    }
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
  }
  setpgid(p, p); /* also from the parent: no race with the child */
  cmd_pid = p;
  install_signal_cleanup();
  atexit(sibling_kill_command); /* safety net: any exit() also kills it */
  if (start_watchdog(p) != 0) {
    /* without the watchdog a SIGKILL of bench would leak the command */
    fprintf(stderr, "sibling: cannot start the watchdog: %s\n",
            strerror(errno));
    kill(-p, SIGKILL);
    waitpid(p, NULL, 0);
    cmd_pid = -1;
    return -1;
  }

  ms_sleep(SIBLING_CMD_SETTLE_MS);
  int st;
  if (waitpid(p, &st, WNOHANG) == p) {
    cmd_pid = -1;
    kill(-p, SIGKILL); /* anything it left behind */
    dismiss_watchdog();
    if (WIFEXITED(st) && WEXITSTATUS(st) == 126)
      fprintf(stderr, "sibling: could not pin the command to CPU %d\n",
              sib_cpu);
    else if (WIFEXITED(st) && WEXITSTATUS(st) == 127)
      fprintf(stderr, "sibling: could not run /bin/sh\n");
    else
      fprintf(stderr,
              "sibling: command exited right after start (status %d) - it "
              "must keep running while the busy sweep runs: %s\n",
              WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st), cmd);
    return -1;
  }
  return 0;
}

/* Stop the command (if any): SIGTERM to its group, SIGKILL after 1 s. */
static void stop_command(void) {
  pid_t p = cmd_pid;
  if (p <= 0)
    return;
  int st;
  if (waitpid(p, &st, WNOHANG) == p) {
    fprintf(stderr,
            "sibling: WARNING: the command exited before the busy sweep "
            "ended; later samples were not measured under its load: %s\n",
            cmd);
  } else {
    kill(-p, SIGTERM);
    for (int i = 0; i < 100; i++) {
      if (waitpid(p, &st, WNOHANG) == p)
        break;
      ms_sleep(10);
      if (i == 99) {
        kill(-p, SIGKILL);
        waitpid(p, &st, 0);
      }
    }
  }
  kill(-p, SIGKILL); /* stragglers in the group; ESRCH if none */
  cmd_pid = -1;
  dismiss_watchdog(); /* the command is gone: watchdog no longer needed */
}

void sibling_kill_command(void) { stop_command(); }

void sibling_idle(void) {
  if (!started)
    return;
  stop_command();
  pthread_mutex_lock(&mu);
  atomic_store(&state, ST_IDLE);
  pthread_mutex_unlock(&mu);
  /* worker leaves its loop, re-locks, parks */
  while (atomic_load(&spinning))
    sched_yield();
  wait_parked();
  usleep(2000); /* let the CPU actually enter its idle state */
}

int sibling_busy(void) {
  if (!started)
    return 0;
  if (cmd) {
    /* command mode: the helper thread stays parked; the child process does
     * the work on the sibling CPU */
    return start_command();
  }
  pthread_mutex_lock(&mu);
  atomic_store(&state, ST_BUSY);
  pthread_cond_broadcast(&cv_wake);
  pthread_mutex_unlock(&mu);
  while (!atomic_load(&spinning))
    sched_yield();
  usleep(2000); /* let it reach steady state (clocks, uop cache) */
  return 0;
}

int sibling_alt_start(volatile uint32_t *seg_id) {
  if (!started)
    return -1;
  alt_seg_id = seg_id;
  atomic_store(&alt_armed, 0);
  atomic_store(&alt_go, 0);
  pthread_mutex_lock(&mu);
  atomic_store(&state, ST_ALT);
  pthread_cond_broadcast(&cv_wake);
  pthread_mutex_unlock(&mu);
  while (!atomic_load(&alt_armed))
    sched_yield();
  return 0;
}

void sibling_alt_go(void) { atomic_store(&alt_go, 1); }

void sibling_stop(void) {
  if (!started)
    return;
  stop_command();
  pthread_mutex_lock(&mu);
  atomic_store(&state, ST_QUIT);
  pthread_cond_broadcast(&cv_wake);
  pthread_mutex_unlock(&mu);
  pthread_join(thr, NULL);
  started = 0;
}
