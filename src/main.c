/* src/main.c  -  driver: pin, check RDPRU, calibrate, measure, report, CSV
 * (src/utils.c)
 *
 * Only the timed loops live in assembly (src/bench.S + include/body.inc).
 *
 *   1. pin the thread to CPU_ID           (APERF/MPERF are per-core)
 *   2. CPUID 0x80000008: EBX bit 4 = RDPRU supported (else SIGILL later)
 *   3. for each sibling state (idle / busy), each sqrtsd count in
 *      FOR_EACH_SQRT and each fill count in FOR_EACH_FILL (bench_config.h):
 *        bench_empty_s<S>      -> median  = timer overhead
 *        bench_body_s<S>_f<N>  -> median  = overhead + UNROLL x code under test
 *        net = median(body) - median(empty);  per-unit = net / UNROLL
 *        write one CSV file to OUT_DIR
 *
 * Median (not mean) because interrupts / SMIs add rare huge outliers.
 * The first WARMUP samples are skipped (cold caches, predictors, clock).
 */
#define _GNU_SOURCE
#include <cpuid.h>
#include <errno.h>
#include <linux/perf_event.h>
#include <math.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "bench.h"
#include "bench_config.h"
#include "sibling.h"
#include "utils.h"

#define bit_RDPRU (1u << 4) /* 0x10 */
#define TOTAL (WARMUP + SAMPLES)

#define ERROR_CODE -1
#define PRINT_SEP                                                              \
  printf("------------------------------------------------------------\n")

typedef void (*bench_fn)(uint64_t *, uint64_t);

/* One entry per (sqrtsd count, fill count) pair of FOR_EACH_SQRT x
 * FOR_EACH_FILL: the generated empty and body functions for that setup. */
struct variant {
  int sqrt;
  int fill;
  bench_fn empty, body;
};
#define VARIANT(s, n) {s, n, bench_empty_s##s, bench_body_s##s##_f##n},
#define VARIANT_ROW(s) FOR_EACH_FILL(VARIANT, s)
static const struct variant variants[] = {FOR_EACH_SQRT(VARIANT_ROW)};
#define N_VARIANTS (sizeof variants / sizeof variants[0])

static uint64_t buf_empty[TOTAL];
static uint64_t buf_body[TOTAL];
static char run_stamp[32]; /* sweep start time, used in CSV rows / file names */
static char cpu_model[128] = "unknown"; /* "model name" of CPU_ID, see below */
static bool latency_only = false; /* -l: RDPRU timing only, no PMC counter */
static bool time_series = false;  /* -t: alternating-sibling time series */
static struct ts_sample ts_buf[TS_MAX_SAMPLES];

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

/* Open PMC_CONFIG as a user-space-only counter for this thread.
 * Returns an fd, or -1 (errno set) e.g. EACCES when perf_event_paranoid is
 * too strict. */
static int pmc_open(void) {
  struct perf_event_attr attr;
  memset(&attr, 0, sizeof attr);
  attr.type = PERF_TYPE_RAW;
  attr.size = sizeof attr;
  attr.config = PMC_CONFIG;
  attr.disabled = 1;
  attr.exclude_kernel = 1;
  attr.exclude_hv = 1;
  return (int)syscall(SYS_perf_event_open, &attr, 0 /* this thread */,
                      -1 /* any cpu */, -1 /* no group */, 0);
}

/* Run fn(buf, TOTAL) with the counter enabled around the whole call.
 * *count is the number of events over all TOTAL timed regions (warm-up
 * included). Returns 0 if a count was obtained, -1 if fd < 0 or read failed;
 * fn always runs. */
static int run_counted(int fd, bench_fn fn, uint64_t *buf, uint64_t *count) {
  if (fd < 0) {
    fn(buf, TOTAL);
    return ERROR_CODE;
  }
  ioctl(fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
  fn(buf, TOTAL);
  ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
  return read(fd, count, sizeof *count) == (ssize_t)sizeof *count ? 0
                                                                  : ERROR_CODE;
}

/* Drop warm-up samples, sort the rest; returns the sorted SAMPLES-array. */
static uint64_t *sorted(uint64_t *buf) {
  qsort(buf + WARMUP, SAMPLES, sizeof buf[0], cmp_u64);
  return buf + WARMUP;
}

static struct stats stats_of(uint64_t *buf) {
  uint64_t *s = sorted(buf);
  struct stats st;
  uint64_t sum = 0;
  for (int i = 0; i < SAMPLES; i++)
    sum += s[i];
  st.min = s[0];
  st.median = s[SAMPLES / 2];
  st.p99 = s[(uint64_t)SAMPLES * 99 / 100];
  st.max = s[SAMPLES - 1];
  st.mean = (double)sum / SAMPLES;

  // Calculate standard deviation
  double variance_sum = 0.0;
  for (int i = 0; i < SAMPLES; i++) {
    double diff = s[i] - st.mean;
    variance_sum += diff * diff;
  }
  st.stddev = sqrt(variance_sum / SAMPLES);
  return st;
}

/* Read one line from /sys/devices/system/cpu/cpu<cpu>/topology/<name> into
 * out (trailing newline stripped). Returns 0 on success, -1 on failure. */
static int read_topology(int cpu, const char *name, char *out, size_t n) {
  char path[128];
  snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/%s", cpu,
           name);
  FILE *f = fopen(path, "r");
  if (!f)
    return ERROR_CODE;
  if (!fgets(out, (int)n, f)) {
    fclose(f);
    return ERROR_CODE;
  }
  fclose(f);
  out[strcspn(out, "\n")] = '\0';
  return 0;
}

/* Print which physical core a logical CPU belongs to, and which logical CPUs
 * share that core (SMT siblings: they share the execution units, so a busy
 * sibling would distort the measurement). */
static void print_topology(int cpu) {
  char pkg[32], core[32], sib[64];
  if (read_topology(cpu, "physical_package_id", pkg, sizeof pkg) ||
      read_topology(cpu, "core_id", core, sizeof core) ||
      read_topology(cpu, "thread_siblings_list", sib, sizeof sib)) {
    fprintf(stderr, "could not read topology for CPU %d\n", cpu);
    return;
  }
  printf("logical CPU %d -> package %s, physical core %s, SMT siblings {%s}\n",
         cpu, pkg, core, sib);
}

/* Fill cpu_model with the "model name" line of /proc/cpuinfo that belongs to
 * logical CPU `cpu` (the "processor : N" block with N == cpu). Hybrid CPUs can
 * have different models per core, so match the pinned CPU, not just the
 * first block. Leaves "unknown" if the file or the field is missing. */
static void read_cpu_model(int cpu) {
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (!f)
    return;
  char line[512];
  int cur = -1;
  while (fgets(line, sizeof line, f)) {
    if (!strncmp(line, "processor", 9)) {
      char *c = strchr(line, ':');
      cur = c ? atoi(c + 1) : -1;
    } else if (cur == cpu && !strncmp(line, "model name", 10)) {
      char *c = strchr(line, ':');
      if (!c)
        break;
      c++;
      while (*c == ' ' || *c == '\t')
        c++;
      c[strcspn(c, "\r\n")] = '\0';
      if (*c)
        snprintf(cpu_model, sizeof cpu_model, "%s", c);
      break;
    }
  }
  fclose(f);
}

/* One benchmark: empty region, body variant, report + CSV. */
static void run_once(int pmc_fd, int sib_cpu, const char *state,
                     const struct variant *v) {
  struct result r;
  memset(&r, 0, sizeof r);
  r.sibling_cpu = sib_cpu;
  r.sibling_state = state;
  r.fill = v->fill;
  r.sqrt_delay = v->sqrt;

  uint64_t pmc_empty = 0, pmc_body = 0;

  /* 3a. calibrate: empty timed region */
  int ok_empty = run_counted(pmc_fd, v->empty, buf_empty, &pmc_empty);
  if (!latency_only && ok_empty == ERROR_CODE) {
    fprintf(stderr, "Warning: PMC counter not available or read failed. "
                    "Continuing with timing only.\n");
  }
  r.empty = stats_of(buf_empty);

  /* 3b. the code under test */
  int ok_body = run_counted(pmc_fd, v->body, buf_body, &pmc_body);
  if (!latency_only && ok_body == ERROR_CODE) {
    fprintf(stderr, "Warning: PMC counter not available or read failed. "
                    "Continuing with timing only.\n");
  }
  r.body = stats_of(buf_body);

  /* 4. net, clamped at 0 */
  r.net = r.body.median > r.empty.median ? r.body.median - r.empty.median : 0;
  r.per_unit = (double)r.net / UNROLL;

  printf("sqrtsd=%d, fill=%d dummy imulq, sibling %s\n", r.sqrt_delay, r.fill,
         state);
  printf("timer overhead  (median ticks) : %lu\n", r.empty.median);
  printf("body total      (min ticks)    : %lu\n", r.body.min);
  printf("body total      (median ticks) : %lu\n", r.body.median);
  printf("body total      (mean ticks)   : %.4f\n", r.body.mean);
  printf("body total      (stddev ticks) : %.4f\n", r.body.stddev);
  printf("net, median - overhead (ticks): %lu\n", r.net);
  printf("per UNROLL (%d) unit (ticks)        : %.2f\n", UNROLL, r.per_unit);

  /* 5. PMC: same subtraction trick, but on totals over all TOTAL regions */
  if (pmc_fd >= 0 && ok_empty == 0 && ok_body == 0) {
    r.pmc_ok = 1;
    r.pmc_empty = pmc_empty;
    r.pmc_body = pmc_body;
    r.pmc_net = pmc_body > pmc_empty ? pmc_body - pmc_empty : 0;
    r.pmc_per_region = (double)r.pmc_net / TOTAL;
    r.pmc_per_unit = (double)r.pmc_net / ((double)TOTAL * UNROLL);
    printf("%s empty run (events)   : %lu\n", PMC_NAME, r.pmc_empty);
    printf("%s body run  (events)   : %lu\n", PMC_NAME, r.pmc_body);
    printf("%s net       (events)   : %lu\n", PMC_NAME, r.pmc_net);
    printf("%s per timed region     : %.2f\n", PMC_NAME, r.pmc_per_region);
    printf("%s per UNROLL (%d) unit : %.4f\n", PMC_NAME, UNROLL,
           r.pmc_per_unit);
  }
  csv_write(&r);
  PRINT_SEP;
}

/* All fill counts with the sibling in its current state. */
static void sweep(int pmc_fd, int sib_cpu, const char *state) {
  for (size_t i = 0; i < N_VARIANTS; i++)
    run_once(pmc_fd, sib_cpu, state, &variants[i]);
}

/* -t: measure the probe back to back until TS_SAMPLES samples while the sibling
 * alternates imul / nop segments; write (timestamp, latency, ...) to a CSV. */
static int run_timeseries(void) {
  static volatile uint32_t seg_id; /* written by the sibling, read by the asm */
  int sib = sibling_start(CPU_ID);
  if (sib < 0) {
    fprintf(stderr, "time-series mode needs a controllable SMT sibling of "
                    "CPU %d\n", CPU_ID);
    return 1;
  }
  /* touch the buffer now: page faults must not land inside the window */
  memset(ts_buf, 0, sizeof ts_buf);
  printf("=== time series: %d samples, sibling CPU %d alternating imul/nop "
         "(fill %d, segment ~%d ticks) ===\n",
         TS_SAMPLES, sib, TS_SIB_FILL, TS_SEG_TICKS);
  if (sibling_alt_start(&seg_id) != 0) {
    sibling_stop();
    return 1;
  }
  sibling_alt_go();
  uint64_t n = bench_timeseries(ts_buf, TS_MAX_SAMPLES, TS_WINDOW_TICKS, &seg_id);
  sibling_idle();
  sibling_stop();
  if (n < TS_SAMPLES)
    fprintf(stderr, "warning: only %lu of %d samples before the TS_WINDOW_TICKS "
                    "cap\n", n, TS_SAMPLES);

  /* quick summary: median latency per sibling activity (the mean is spoiled
   * by rare outliers: interrupts, and RDPRU reads that go backwards) */
  static uint64_t tmp[TS_MAX_SAMPLES];
  const char *name[3] = {"none", "imul", "nop"};
  for (int k = 0; k < 3; k++) {
    uint64_t m = 0;
    for (uint64_t i = 0; i < n; i++) {
      int kind = !ts_buf[i].seg_id ? 0 : (ts_buf[i].seg_id & 1) ? 1 : 2;
      if (kind == k)
        tmp[m++] = ts_buf[i].latency;
    }
    if (m) {
      qsort(tmp, m, sizeof tmp[0], cmp_u64);
      printf("sibling %-4s: %6lu samples, median latency %lu ticks\n", name[k],
             m, tmp[m / 2]);
    }
  }
  return csv_ts_write(ts_buf, n, run_stamp) == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  /* 1. pin */
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(CPU_ID, &set);
  if (sched_setaffinity(0, sizeof set, &set) != 0) {
    perror("sched_setaffinity");
    return 1;
  }
  printf("pinned to CPU %d (running on logical CPU %d)\n", CPU_ID,
         sched_getcpu());
  print_topology(CPU_ID);
  read_cpu_model(CPU_ID);
  printf("CPU model: %s\n", cpu_model);
  printf("Running %d warmup + %d timed samples, UNROLL=%d, %zu (sqrtsd, fill) "
         "combinations per sibling state\n",
         WARMUP, SAMPLES, UNROLL, N_VARIANTS);
  PRINT_SEP;

  /* 2. feature check */
  unsigned eax, ebx, ecx, edx;
  if (!__get_cpuid(0x80000008, &eax, &ebx, &ecx, &edx) || !(ebx & bit_RDPRU)) {
    fputs("RDPRU not supported by this CPU (CPUID 0x80000008 EBX[4])\n",
          stderr);
    return 1;
  }

  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(run_stamp, sizeof run_stamp, "%Y%m%d_%H%M%S", &tm);

  /* 3. SMT sibling control: ./bench [-l] [-c CMD] [idle|busy|both]
   *    (default mode: both). -c CMD: the "busy" sibling runs CMD instead of
   *    the built-in workload (see sibling.h). */
  const char *mode = "both";
  const char *sib_cmd = NULL;
  int opt, bad = 0;
  optind = 1;
  while ((opt = getopt(argc, argv, "ltc:h")) != -1) {
    if (opt == 'c')
      sib_cmd = optarg;
    else if (opt == 'l')
      latency_only = true;
    else if (opt == 't')
      time_series = latency_only = true; /* -t implies RDPRU only */
    else
      bad = 1; /* -h or unknown option */
  }
  int mode_given = optind < argc;
  if (mode_given)
    mode = argv[optind++];
  int want_idle = !strcmp(mode, "idle") || !strcmp(mode, "both");
  int want_busy = !strcmp(mode, "busy") || !strcmp(mode, "both");
  if (time_series && (sib_cmd || mode_given))
    bad = 1; /* -t takes no -c and no mode */
  if (bad || optind < argc || (!want_idle && !want_busy)) {
    fprintf(stderr,
            "usage: %s [-l] [-c CMD] [idle|busy|both]\n"
            "       %s -t\n"
            "  -l      latency mode: RDPRU timing only, do not open the PMC\n"
            "  -t      time series: probe latency over a window while the sibling\n"
            "          alternates imul / nop segments -> odata/ts/*.csv (implies -l,\n"
            "          no -c, no mode)\n"
            "  -c CMD  run CMD (via sh -c) on the SMT sibling during the busy\n"
            "          sweep instead of the built-in workload; it must keep\n"
            "          running; output is discarded\n",
            argv[0], argv[0]);
    return 1;
  }

  /* Open the PMC counter, for reading `IntSch1TokenStall` events. Skipped in
   * latency mode (-l): only the RDPRU timer is used, no perf_event_open. */
  int pmc_fd = -1;
  if (latency_only) {
    printf("latency mode: RDPRU timing only, PMC %s not opened\n", PMC_NAME);
  } else {
    pmc_fd = pmc_open();
    if (pmc_fd < 0)
      fprintf(stderr,
              "note: cannot open %s counter (%s); timing only. Try: sudo, or "
              "sudo sysctl kernel.perf_event_paranoid=2\n",
              PMC_NAME, strerror(errno));
  }

  if (time_series)
    return run_timeseries();
  if (csv_open(run_stamp, cpu_model) != 0)
    return 1;

  if (sib_cmd && !want_busy)
    fprintf(stderr, "note: -c is ignored, mode '%s' has no busy sweep\n", mode);
  sibling_set_command(sib_cmd);
  int sib = sibling_start(CPU_ID);
  if (sib == SIBLING_ERROR) {
    fprintf(stderr,
            "cannot control the SMT sibling of CPU %d: aborting "
            "(results would not be valid)\n",
            CPU_ID);
    return 1;
  }

  if (sib < 0) {
    printf("no SMT sibling found for CPU %d: sibling mode ignored%s\n", CPU_ID,
           sib_cmd ? " (and so is -c)" : "");
    sweep(pmc_fd, -1, "none");
  } else {
    if (want_idle) {
      sibling_idle();
      printf("=== sibling CPU %d: IDLE ===\n", sib);
      sweep(pmc_fd, sib, "idle");
    }
    if (want_busy) {
      if (sibling_busy() != 0) {
        fprintf(stderr, "cannot start the busy sibling load: aborting\n");
        sibling_kill_command();
        sibling_stop();
        csv_close();
        return 1;
      }
      if (sib_cmd)
        printf("=== sibling CPU %d: BUSY (command: %s) ===\n", sib, sib_cmd);
      else
        printf("=== sibling CPU %d: BUSY (SIBLING_LOAD=%d, %s) ===\n", sib,
               SIBLING_LOAD, SIBLING_LOAD_NAME);
      sweep(pmc_fd, sib, "busy");
    }
    /* Explicitly end the -c command before the benchmark ends (no-op when
     * none is running), then stop the helper thread. */
    sibling_kill_command();
    sibling_stop();
  }

  if (pmc_fd >= 0)
    close(pmc_fd);
  csv_close();
  return 0;
}
