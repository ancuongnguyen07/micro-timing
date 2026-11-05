/* include/utils.h  -  result types and CSV output (src/utils.c) */
#ifndef UTILS_H
#define UTILS_H

#include <stdint.h>

#include "bench.h"

/* Summary of the SAMPLES kept timer values (ticks). */
struct stats {
  uint64_t min, median, p99, max;
  double mean;
  double stddev;
};

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

/* Create OUT_DIR/bench_<run_stamp>_cpu<CPU_ID>.csv and write the header.
 * run_stamp and cpu_model are copied and written into every row.
 * Returns 0 on success, -1 on failure (reason on stderr). */
int csv_open(const char *run_stamp, const char *cpu_model);

/* Append one row (one benchmark) and flush. Returns 0 on success. */
int csv_write(const struct result *r);

/* Close the file (no-op if not open). */
void csv_close(void);

/* Time-series mode: write odata/ts/bench_<run_stamp>_cpu<CPU_ID>_timeseries.csv with one row
 * per sample: timestamp,latency,seg_id,sibling   (sibling = none|imul|nop).
 * Returns 0 on success, -1 on failure (reason on stderr). */
int csv_ts_write(const struct ts_sample *s, uint64_t n, const char *run_stamp);

#endif
