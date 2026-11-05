/* src/utils.c  -  CSV output of the benchmark results */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bench_config.h"
#include "sibling.h"
#include "utils.h"

#define OUT_DIR "odata" /* CSV files go here (relative to the cwd) */

/* The one CSV file of this program run: OUT_DIR/bench_<stamp>_cpu<N>.csv.
 * Header written once; every benchmark (sibling state x fill count) appends
 * one row, so a crash or Ctrl-C keeps the rows measured so far. */
static FILE *csv_file;
static char csv_stamp[32];
static char csv_cpu_model[128];

int csv_open(const char *run_stamp, const char *cpu_model) {
  snprintf(csv_stamp, sizeof csv_stamp, "%s", run_stamp);
  snprintf(csv_cpu_model, sizeof csv_cpu_model, "%s", cpu_model);
  if (mkdir(OUT_DIR, 0777) != 0 && errno != EEXIST) {
    fprintf(stderr, "cannot create %s: %s\n", OUT_DIR, strerror(errno));
    return -1;
  }
  char path[256];
  snprintf(path, sizeof path, OUT_DIR "/bench_%s_cpu%d.csv", csv_stamp, CPU_ID);
  csv_file = fopen(path, "w");
  if (!csv_file) {
    fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
    return -1;
  }
  fputs("timestamp,cpu_id,cpu_model,sibling_cpu,sibling_state,sibling_load,"
        "sibling_cmd,"
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

int csv_write(const struct result *r) {
  FILE *f = csv_file;
  if (!f)
    return -1;
  /* identification columns */
  fprintf(f, "%s,%d,", csv_stamp, CPU_ID);
  csv_quoted(f, csv_cpu_model);
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
    fprintf(f, "%d,%lu,%lu,%lu,%.4f,%.4f\n", WARMUP + SAMPLES, r->pmc_empty,
            r->pmc_body, r->pmc_net, r->pmc_per_region, r->pmc_per_unit);
  else
    fputs(",,,,,\n", f);
  fflush(f);
  return 0;
}

void csv_close(void) {
  if (csv_file) {
    fclose(csv_file);
    csv_file = NULL;
  }
}

int csv_ts_write(const struct ts_sample *s, uint64_t n, const char *run_stamp) {
  const char *dir = OUT_DIR "/ts";
  if ((mkdir(OUT_DIR, 0777) != 0 && errno != EEXIST) ||
      (mkdir(dir, 0777) != 0 && errno != EEXIST)) {
    fprintf(stderr, "cannot create %s: %s\n", dir, strerror(errno));
    return -1;
  }
  char path[256];
  snprintf(path, sizeof path, "%s/bench_%s_cpu%d_timeseries_block%d.csv", dir,
           run_stamp, CPU_ID, TS_SIB_FILL);
  FILE *f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
    return -1;
  }
  fputs("timestamp,latency,seg_id,sibling\n", f);
  for (uint64_t i = 0; i < n; i++) {
    const char *who = !s[i].seg_id        ? "none"
                      : (s[i].seg_id & 1) ? "imul"
                                          : "nop";
    fprintf(f, "%lu,%lu,%u,%s\n", s[i].timestamp, s[i].latency, s[i].seg_id,
            who);
  }
  fclose(f);
  printf("wrote %lu samples to %s\n", n, path);
  return 0;
}
