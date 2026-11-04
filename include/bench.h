/* include/bench.h  -  C view of the assembly measurement loops (src/bench.S)
 *
 * Both functions follow the System V AMD64 ABI:
 *   samples[i] = elapsed RDPRU ticks of timed region i, for i < count.
 *
 *   bench_empty : timer only          -> timer overhead
 *   bench_body  : timer + BODY        -> overhead + UNROLL x code under test
 */
#ifndef BENCH_H
#define BENCH_H

#include <stdint.h>

void bench_empty(uint64_t *samples, uint64_t count);
void bench_body (uint64_t *samples, uint64_t count);

/* The run-time sweep (lists in bench_config.h):
 *   bench_empty_s<S>      timer only, setup with S sqrtsd
 *   bench_body_s<S>_f<N>  S sqrtsd in setup, N fill instructions in BODY */
#include "bench_config.h"
#define DECL_EMPTY(s) void bench_empty_s##s(uint64_t *samples, uint64_t count);
FOR_EACH_SQRT(DECL_EMPTY)
#define DECL_BODY(s, n)                                                       \
  void bench_body_s##s##_f##n(uint64_t *samples, uint64_t count);
#define DECL_ROW(s) FOR_EACH_FILL(DECL_BODY, s)
FOR_EACH_SQRT(DECL_ROW)

#endif
