/* include/bench_config.h
 *
 * Tunables for the micro-benchmark. Only preprocessor lines here.
 */
#ifndef BENCH_CONFIG_H
#define BENCH_CONFIG_H

/* How many times the instruction(s) under test are repeated INSIDE one
 * timed region. Bigger = the fixed cost of the timer (lfence + rdpru, ~tens
 * of cycles) is divided by more, so per-instruction numbers get more exact.
 * Keep it small enough that the unrolled body stays in the L1 I-cache / uop
 * cache (100 x ~4 bytes is fine). */
#define UNROLL      1

/* Timed regions measured and kept / thrown away (cold caches, branch
 * predictors, frequency ramp-up). Total samples = WARMUP + SAMPLES. */
#define SAMPLES     100001            /* odd, so the median is one real sample */
#define WARMUP ((SAMPLES - 1) / 100) /* throw away the first 1% of samples */


/* Which RDPRU register to read (value goes in %ecx):
 *   0 = MPERF : ticks at a FIXED reference rate (like the TSC)
 *   1 = APERF : ticks at the ACTUAL core clock  -> real core cycles
 * For latency in core cycles use APERF. */
#define RDPRU_MPERF 0
#define RDPRU_APERF 1
#define RDPRU_SEL   RDPRU_APERF

/* Logical CPU to pin to. APERF/MPERF are per-core counters: if the thread
 * migrates between two timer reads the difference is garbage.
 * Must be < 31 (used as an immediate shift below). */
#define CPU_ID      0


#define ZEN3_QUEUE_WATERMARK 18
#define ZEN3_QUEUE_ENTRIES 24

#define FILL_ENTRY 18   /* default for the plain bench_body() */

/* Number of dependent sqrtsd in BODY_SETUP (body.inc) that delay the
 * imul chain's input. Default for the plain bench_empty()/bench_body().
 * NB: do not #define a macro called SQRT: body.inc uses it as a macro
 * parameter name and cpp would rewrite it. */
#define SQRT_ENTRY 3

/* The sweep, run at run time. One function is generated per combination
 * (bench.S) and listed in main.c's table:
 *     bench_empty_s<S>            timer only, setup with S sqrtsd
 *     bench_body_s<S>_f<N>        S sqrtsd in setup, N fill instructions
 * Extending or shrinking the sweep is a one-line edit here.
 * X-macros: usable from both C and assembly.
 *   FOR_EACH_SQRT(X)        -> X(S) for every sqrtsd count
 *   FOR_EACH_FILL(X, S)     -> X(S, N) for every fill count N */
#define FOR_EACH_SQRT(X) X(3) X(6)
#define FOR_EACH_FILL(X, S)                                                   \
  X(S,14) X(S, 15) X(S, 16) X(S, 17) X(S, 18) X(S, 19) X(S, 20) X(S, 21) X(S, 22) X(S, 23)     \
  X(S, 24) X(S, 25) X(S, 26)

/* ======================================================================
 * TIME-SERIES MODE  (./build/bench -t)  -  RDPRU only, no PMC
 *
 * The measurement thread times the BODY probe back to back until TS_SAMPLES
 * samples are collected (or TS_WINDOW_TICKS RDPRU ticks passed) while the SMT sibling alternates, segment by
 * segment, between a block of imul (include/sib_imul.inc) and a block of nop
 * (include/sib_nop.inc). Output: odata/ts/bench_<stamp>_cpu<N>_timeseries.csv with one row
 * per probe: (timestamp, latency, seg_id, sibling).
 * ====================================================================== */
#define TS_SAMPLES      437365 /* samples to collect (the run ends when reached) */
#define TS_MAX_SAMPLES  TS_SAMPLES /* capacity of the sample buffer (24 B each) */
#define TS_WINDOW_TICKS (1ULL << 40) /* safety cap on the duration (RDPRU
                                  ticks); normally TS_SAMPLES ends the run first */
#define TS_SEG_TICKS    1200000000 /* target length of one sibling segment (RDPRU
                                  ticks); the sibling calibrates its loop counts
                                  to it. Every sample costs ~18000 ticks (BODY_SETUP
                                  runs per sample), so 1.2e9 ticks = ~65000
                                  samples per phase: long, clearly separated
                                  imul / nop phases (a square wave) */
#define TS_SIB_FILL     15     /* instructions per sibling block (imul / nop) */

/* Hardware performance counter to read around each bench_* call (raw perf
 * config = event | umask << 8, AMD Zen 3 "dispatch token stalls 2", PMCx0AF):
 *   0x01af int_sch0_token_stall    0x02af int_sch1_token_stall (IntSch1)
 *   0x04af int_sch2_token_stall    0x08af int_sch3_token_stall
 *   0x10af agsq_token_stall        0x20af retire_token_stall
 * "Dispatch group valid but not dispatched: no tokens for Integer Scheduler
 * Queue N" = that scheduler queue is full (scheduler contention).
 * Needs perf_event_paranoid <= 2 (or root / CAP_PERFMON). */
#define PMC_NAME    "IntSch1TokenStall"
#define PMC_CONFIG  0x02af

/* ======================================================================
 * SIBLING "BUSY" WORKLOAD  (what the helper thread on the SMT sibling runs
 * while sibling_state = busy; see sibling.h / sibling.c)
 *
 * >>> TO CHANGE THE WORKLOAD:
 *       1. pick a value for SIBLING_LOAD below, or
 *       2. write your own code in the SIBLING_LOAD_CUSTOM branch of
 *          busy_chunk() in src/sibling.c (clearly marked there), and set
 *          SIBLING_LOAD to SIBLING_LOAD_CUSTOM.
 *     Then rebuild (make). The value is written to the CSV column
 *     `sibling_load`, so every row records which workload it ran under.
 *
 *   SIBLING_LOAD_PAUSE   0  PAUSE spin loop: the sibling is busy (never
 *                           halts) but asks the core to favour the other
 *                           thread -> mildest interference
 *   SIBLING_LOAD_IMUL    1  dependent imul chain: occupies the multiplier
 *                           and its scheduler queue
 *   SIBLING_LOAD_EMPTY   2  empty loop (just a counter decrement + branch, no real
 *                           work): the sibling runs flat out but uses almost
 *                           no execution resources besides ALU + branch
 *   SIBLING_LOAD_CUSTOM  3  your own code (busy_chunk() in src/sibling.c)
 *
 * All loads run "forever" in the sense that the helper keeps calling
 * busy_chunk() until the benchmark asks it to stop. It checks the stop
 * request once per chunk, so a chunk must return after a short time (see
 * SIBLING_CHUNK_ITERS). Never put a loop that does not return into a chunk:
 * sibling_idle() / sibling_stop() would wait for it forever.
 * ====================================================================== */
#define SIBLING_LOAD_PAUSE  0
#define SIBLING_LOAD_IMUL   1
#define SIBLING_LOAD_EMPTY  2
#define SIBLING_LOAD_CUSTOM 3
/* Not selectable here: written to the CSV when the busy sibling ran an
 * external command given at run time with `bench -c CMD` (see sibling.h). */
#define SIBLING_LOAD_COMMAND 4

/* How long to wait after starting the `-c` command before the busy sweep
 * begins (ms): time for it to start up and reach steady state. If it has
 * already exited by then, bench aborts. */
#define SIBLING_CMD_SETTLE_MS 500

#ifndef SIBLING_LOAD /* can also be overridden: make EXTRA_CFLAGS=-DSIBLING_LOAD=2 */
#define SIBLING_LOAD SIBLING_LOAD_IMUL      /* <<< select the workload here */
#endif

/* Iterations of the workload per chunk, i.e. between two checks of the
 * stop request. Larger = the check disturbs the load less, but the switch to
 * idle takes longer (it must wait for the current chunk to finish).
 * 1000 PAUSE/imul/empty iterations take roughly 1-100 us. */
#define SIBLING_CHUNK_ITERS 1000

#if SIBLING_LOAD == SIBLING_LOAD_PAUSE
#define SIBLING_LOAD_NAME "pause"
#elif SIBLING_LOAD == SIBLING_LOAD_IMUL
#define SIBLING_LOAD_NAME "imul"
#elif SIBLING_LOAD == SIBLING_LOAD_EMPTY
#define SIBLING_LOAD_NAME "empty"
#elif SIBLING_LOAD == SIBLING_LOAD_CUSTOM
#define SIBLING_LOAD_NAME "custom"
#else
#error "SIBLING_LOAD must be one of SIBLING_LOAD_PAUSE/IMUL/EMPTY/CUSTOM"
#endif

#endif
