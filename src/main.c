/* src/main.c  -  driver: pin, check RDPRU, calibrate, measure, report, CSV
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

#define bit_RDPRU (1u << 4) /* 0x10 */
#define TOTAL (WARMUP + SAMPLES)
#define OUT_DIR "odata" /* CSV files go here (relative to the cwd) */

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
    return -1;
  }
  ioctl(fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
  fn(buf, TOTAL);
  ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
  return read(fd, count, sizeof *count) == (ssize_t)sizeof *count ? 0 : -1;
}

/* Drop warm-up samples, sort the rest; returns the sorted SAMPLES-array. */
static uint64_t *sorted(uint64_t *buf) {
  qsort(buf + WARMUP, SAMPLES, sizeof buf[0], cmp_u64);
  return buf + WARMUP;
}

/* Summary of the SAMPLES kept timer values (ticks). */
struct stats {
  uint64_t min, median, p99, max;
  double mean;
  double stddev;
};

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
    return -1;
  if (!fgets(out, (int)n, f)) {
    fclose(f);
    return -1;
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

/* Everything one benchmark run measured (one CSV row). */
struct result {
  int sibling_cpu;           /* -1 = none */
  const char *sibling_state; /* "idle" | "busy" | "none" */
  int sqrt_delay, fill;
  struct stats empty, body;
  uint64_t net;    /* median(body) - median(empty), clamped at 0 */
  double per_unit; /* net / UNROLL */
  int pmc_ok;      /* PMC columns valid? */
  uint64_t pmc_empty, pmc_body, pmc_net;
  double pmc_per_region, pmc_per_unit;
};

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

/* The one CSV file of this program run: OUT_DIR/bench_<stamp>_cpu<N>.csv.
 * Header written once; every benchmark (sibling state x fill count) appends
 * one row, so a crash or Ctrl-C keeps the rows measured so far. */
static FILE *csv_file;

static int csv_open(void) {
  if (mkdir(OUT_DIR, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "cannot create %s: %s\n", OUT_DIR, strerror(errno));
    return -1;
  }
  char path[256];
  snprintf(path, sizeof path, OUT_DIR "/bench_%s_cpu%d.csv", run_stamp, CPU_ID);
  csv_file = fopen(path, "w");
  if (!csv_file) {
    fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
    return -1;
  }
  fputs("timestamp,cpu_id,cpu_model,sibling_cpu,sibling_state,sibling_load,sibling_cmd,"
        "fill_entry,"
        "sqrt_delay,unroll,warmup,iterations,rdpru_sel,"
        "empty_min,empty_median,empty_mean,empty_stddev,empty_p99,empty_max,"
        "body_min,body_median,body_mean,body_stddev,body_p99,body_max,"
        "net_median_ticks,net_ticks_per_unroll,"
        "pmc_event,pmc_regions,pmc_empty_total,pmc_body_total,pmc_net_total,"
        "pmc_net_per_region,pmc_net_per_unroll\n",
        csv_file);
  printf("writing results to %s\n", path);
  return 0;
}

/* Write s as one CSV field, quoted because commands contain spaces/commas. */
static void csv_quoted(FILE *f, const char *s) {
  fputc('"', f);
  for (; *s; s++) {
    if (*s == '"')
      fputc('"', f);
    fputc(*s, f);
  }
  fputc('"', f);
}

/* Append one row (one benchmark). Returns 0 on success. */
static int write_csv(const struct result *r) {
  FILE *f = csv_file;
  if (!f)
    return -1;
  /* identification columns */
  fprintf(f, "%s,%d,", run_stamp, CPU_ID);
  csv_quoted(f, cpu_model);
  fputc(',', f);
  if (r->sibling_cpu >= 0)
    fprintf(f, "%d,", r->sibling_cpu);
  else
    fputs(",", f);
  fprintf(f, "%s,", r->sibling_state);
  const char *cmd = sibling_get_command();
  if (!strcmp(r->sibling_state, "busy") && cmd) {
    fprintf(f, "%d,", SIBLING_LOAD_COMMAND);
    csv_quoted(f, cmd);
    fputc(',', f);
  } else if (!strcmp(r->sibling_state, "busy")) {
    fprintf(f, "%d,,", SIBLING_LOAD);
  } else {
    fputs(",,", f);
  }
  fprintf(f, "%d,%d,%d,%d,%d,%d,", r->fill, r->sqrt_delay, UNROLL, WARMUP,
          SAMPLES, RDPRU_SEL);
  /* timer statistics (ticks) */
  fprintf(f, "%lu,%lu,%.4f,%.4f,%lu,%lu,", r->empty.min, r->empty.median,
          r->empty.mean, r->empty.stddev, r->empty.p99, r->empty.max);
  fprintf(f, "%lu,%lu,%.4f,%.4f,%lu,%lu,", r->body.min, r->body.median,
          r->body.mean, r->body.stddev, r->body.p99, r->body.max);
  fprintf(f, "%lu,%.4f,", r->net, r->per_unit);
  /* PMC (blank when the counter was unavailable) */
  fprintf(f, "%s,", PMC_NAME);
  if (r->pmc_ok)
    fprintf(f, "%d,%lu,%lu,%lu,%.4f,%.4f\n", TOTAL, r->pmc_empty, r->pmc_body,
            r->pmc_net, r->pmc_per_region, r->pmc_per_unit);
  else
    fputs(",,,,,\n", f);
  fflush(f);
  return 0;
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
  r.empty = stats_of(buf_empty);

  /* 3b. the code under test */
  int ok_body = run_counted(pmc_fd, v->body, buf_body, &pmc_body);
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
  write_csv(&r);
  PRINT_SEP;
}

/* All fill counts with the sibling in its current state. */
static void sweep(int pmc_fd, int sib_cpu, const char *state) {
  for (size_t i = 0; i < N_VARIANTS; i++)
    run_once(pmc_fd, sib_cpu, state, &variants[i]);
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

  int pmc_fd = pmc_open();
  if (pmc_fd < 0)
    fprintf(stderr,
            "note: cannot open %s counter (%s); timing only. Try: sudo, or "
            "sudo sysctl kernel.perf_event_paranoid=2\n",
            PMC_NAME, strerror(errno));

  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime(run_stamp, sizeof run_stamp, "%Y%m%d_%H%M%S", &tm);
  if (csv_open() != 0)
    return 1;

  /* 3. SMT sibling control: ./bench [-c CMD] [idle|busy|both]
   *    (default mode: both). -c CMD: the "busy" sibling runs CMD instead of
   *    the built-in workload (see sibling.h). */
  const char *mode = "both";
  const char *sib_cmd = NULL;
  int opt, bad = 0;
  optind = 1;
  while ((opt = getopt(argc, argv, "c:h")) != -1) {
    if (opt == 'c')
      sib_cmd = optarg;
    else
      bad = 1; /* -h or unknown option */
  }
  if (optind < argc)
    mode = argv[optind++];
  int want_idle = !strcmp(mode, "idle") || !strcmp(mode, "both");
  int want_busy = !strcmp(mode, "busy") || !strcmp(mode, "both");
  if (bad || optind < argc || (!want_idle && !want_busy)) {
    fprintf(stderr,
            "usage: %s [-c CMD] [idle|busy|both]\n"
            "  -c CMD  run CMD (via sh -c) on the SMT sibling during the busy\n"
            "          sweep instead of the built-in workload; it must keep\n"
            "          running; output is discarded\n",
            argv[0]);
    return 1;
  }
  if (sib_cmd && !want_busy)
    fprintf(stderr, "note: -c is ignored, mode '%s' has no busy sweep\n", mode);
  sibling_set_command(sib_cmd);
  int sib = sibling_start(CPU_ID);
  if (sib == SIBLING_ERROR) {
    fprintf(stderr, "cannot control the SMT sibling of CPU %d: aborting "
                    "(results would not be valid)\n", CPU_ID);
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
        fclose(csv_file);
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
  fclose(csv_file);
  return 0;
}
