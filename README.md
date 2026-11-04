# RDPRU micro-benchmark skeleton (C driver + x86-64 AT&T measurement loops)

Measures the latency (or throughput) of an instruction / block with `rdpru`
(AMD Zen 2+). Reading APERF gives real **core cycles**, independent of
frequency scaling.

Only the timed code is assembly. Everything else (pinning, CPUID check,
sorting, statistics, printing) is C.

```
micro_bench/
├── Makefile
├── run_exp.sh           build, then run bench N times (-i N, -c CMD) and check the CSVs
├── include/
│   ├── bench_config.h   UNROLL, WARMUP, SAMPLES, APERF/MPERF, CPU_ID (C + asm)
│   ├── bench.h          C prototypes of bench_empty / bench_body
│   ├── rdpru.inc        TIMER_START / TIMER_STOP macros (lfence; rdpru; lfence)
│   ├── body.inc         *** edit this: BODY_SETUP and BODY ***
│   └── sibling.h        API to switch the SMT sibling idle / busy, or run a -c
│                        command on it (incl. sibling_kill_command)
├── src/
│   ├── bench.S          bench_empty / bench_body, generated from one template
│   ├── main.c           pin CPU, CPUID check, qsort, median/min, printf
│   └── sibling.c        helper thread on the SMT sibling (idle / busy); -c command
│                        runner + watchdog that kills it if bench dies (SIGKILL)
├── pysrc/
│   ├── csv_checker.py   checks header and column count of every CSV in odata/
│   └── analyzer.ipynb   plots the latest CSV (PMC counts, idle vs busy sibling)
├── odata/               results: one bench_<timestamp>_cpu<N>.csv per bench run
└── build/               object files and the bench binary (make; make clean removes it)
```

## Use

```
/usr/bin/make run      # edit include/body.inc first
/usr/bin/make dis      # confirm the timed instructions are what you meant
```

Output: `per UNROLL unit` = (median(body) - median(empty timer)) / UNROLL, in
RDPRU ticks (= core cycles with `RDPRU_SEL = RDPRU_APERF`).

## Method

1. Pin to one core (APERF/MPERF are per-core).
2. `lfence; rdpru; lfence` before and after the body (rdpru is not serializing).
3. `bench_empty` has the same loop with no body, so its median is the pure
   timer overhead (~60 ticks on Zen 3) and is subtracted.
4. The body is repeated `UNROLL` times inside one timed region to amortise
   the overhead; the first `WARMUP` samples are dropped; the **median** is
   used to ignore interrupt outliers.

## Latency vs throughput

Decided only by the dependencies inside `BODY`: a chain where each
instruction consumes the previous result measures latency; independent
instructions measure reciprocal throughput. Examples are in `body.inc`.
For a block of K instructions, the result is cycles per block, not per instruction.

## SMT sibling: idle vs busy

The benchmark thread and its SMT sibling share one physical core (execution
units, schedulers, uop cache), so what the sibling does changes the numbers.
`bench` therefore measures every variant twice: once with the sibling idle and
once with it busy. This is done by a helper thread in `src/sibling.c`
(API in `include/sibling.h`). It needs no root, unlike offlining the CPU
through sysfs.

**Finding the sibling.** `sibling_start(CPU_ID)` reads
`/sys/devices/system/cpu/cpu<CPU_ID>/topology/thread_siblings_list` (formats
like `2-3` or `0,8`) and takes the first CPU in it that is not `CPU_ID`. If
there is none (SMT off, or the file is missing) it returns -1 and `bench`
runs once with `sibling_state = none`.

**The helper thread.** `sibling_start` creates one pthread and pins it to the
sibling with `pthread_setaffinity_np`. The pin is explicit because new threads
inherit the creator's mask, which is only `CPU_ID`. The thread is a small state
machine (`IDLE`, `BUSY`, `QUIT`) guarded by a mutex and two condition variables
(`cv_wake`: main to worker, `cv_ack`: worker to main). It starts parked, i.e.
idle.

| State | What the helper does | Effect on the core |
|-------|----------------------|--------------------|
| idle  | blocks in `pthread_cond_wait` (a futex wait in the kernel) | the sibling CPU has nothing to run and halts, so the benchmark thread gets the whole core |
| busy  | loops over `busy_chunk()`, re-checking the state between chunks | the sibling competes for the core's resources |

**What "busy" runs** is chosen at compile time and is the main knob for the
busy experiments. It is the function `busy_chunk()` in `src/sibling.c`, which
the helper calls over and over (so the load is effectively endless) until the
benchmark leaves the busy state. Which version of it is compiled is decided by
`SIBLING_LOAD` in `include/bench_config.h`:

| `SIBLING_LOAD` | Value | What the sibling executes |
|----------------|-------|---------------------------|
| `SIBLING_LOAD_PAUSE` | 0 | a `pause` spin loop. Busy, but yields core resources to the other thread: mildest interference |
| `SIBLING_LOAD_IMUL` (default) | 1 | a dependent `imul` chain. Occupies the multiplier and its scheduler queue |
| `SIBLING_LOAD_EMPTY` | 2 | an empty loop: only a counter decrement + branch, no real work and no `pause`. The sibling issues at full speed |
| `SIBLING_LOAD_CUSTOM` | 3 | your own code, in the marked `SIBLING_LOAD_CUSTOM` branch of `busy_chunk()` |
| `SIBLING_LOAD_COMMAND` | 4 | an external command given at run time with `bench -c CMD`; not a compile-time choice, so you never set `SIBLING_LOAD` to it. `busy_chunk()` is not used in this case, and the helper thread stays parked. See "Running your own command on the sibling" below. The value 4 is what lands in the CSV `sibling_load` column, together with the command text in `sibling_cmd` |

Values 0-3 are what `SIBLING_LOAD` can be set to. The `sibling_load` CSV column
can hold any of 0-4, so it tells you which kind of busy load produced a row.

To change the workload, either:

1. set `SIBLING_LOAD` in `include/bench_config.h` (or pass it for one build,
   e.g. `make clean && make EXTRA_CFLAGS=-DSIBLING_LOAD=2`; `make` does not
   notice a changed `EXTRA_CFLAGS`, hence the `clean`), or
2. edit the `SIBLING_LOAD_CUSTOM` branch of `busy_chunk()` in `src/sibling.c`
   and select it with `SIBLING_LOAD_CUSTOM`. The comment block above
   `busy_chunk()` lists the rules and ideas (`sqrtsd` chain, independent adds,
   load/store traffic, ...).

`SIBLING_CHUNK_ITERS` (also in `bench_config.h`, default 1000) is the number of
loop iterations per call, i.e. how often the helper checks whether it should
stop. The value used is recorded in the CSV column `sibling_load`, and the
banner printed before the busy sweep shows its name.

Rules for a workload: a call to `busy_chunk()` **must return** after a short
time (the stop request is only checked between calls; an unbounded loop inside
it would make `sibling_idle()` / `sibling_stop()` hang), it must not block
(a blocked thread lets the CPU halt, which is the idle state), and it must use
`__asm__ volatile` or volatile accesses so the compiler does not remove it.

**Switching is synchronous.** The calls return only once the sibling really is
in the requested state:

- `sibling_busy()` sets the state, wakes the worker and waits until it reports
  `spinning`, then sleeps 2 ms so clocks and the uop cache reach steady state.
- `sibling_idle()` sets the state, waits for the worker to leave its loop and
  park, then sleeps 2 ms so the CPU actually enters its idle state.
- `sibling_stop()` sets `QUIT` and joins the thread.

**Order in `main.c`.** After pinning and the CPUID check, `bench` calls
`sibling_start`, then for each requested state calls `sibling_idle()` or
`sibling_busy()` and runs the full sweep. The state is recorded in the
`sibling_state`, `sibling_cpu`, `sibling_load` and `sibling_cmd` CSV columns.

Choose the states (and optionally an external load) from the command line:

```
./build/bench [-c CMD] [idle|busy|both]      # mode defaults to both
```

`run_exp.sh` runs `bench` in the default mode `both`; `run_exp.sh -c CMD` adds
`-c`.

### Running your own command on the sibling (`-c`)

With `-c CMD`, the busy sweep does not use `busy_chunk()`. Instead `bench`
starts `/bin/sh -c "CMD"` as a separate process and confines it to the sibling
CPU, so you can measure the effect of a real program (a compiler, `stress-ng`,
a browser benchmark, ...) on the measured thread:

```
sudo ./build/bench -c 'while :; do :; done'          # endless shell loop
sudo ./build/bench -c 'stress-ng --cpu 1' busy        # only the busy sweep
./run_exp.sh -i 3 -c 'openssl speed -multi 1 sha256'
```

How it works (`start_command()` / `stop_command()` in `src/sibling.c`):

1. `fork()`; the child puts itself in its own process group, sets its CPU
   affinity to the sibling CPU only, points stdin/stdout/stderr at `/dev/null`
   and `exec`s the shell. If the pinning fails the child exits (code 126)
   instead of running unpinned, and `bench` aborts.
2. The helper thread stays parked; `bench` waits `SIBLING_CMD_SETTLE_MS`
   (500 ms, `bench_config.h`) so the command can start and reach steady state.
   If the command has already exited by then, `bench` aborts with a message.
3. The busy sweep runs. The CSV records `sibling_load = 4` and the command in
   the `sibling_cmd` column (quoted).
4. Before the benchmark ends, `main()` explicitly calls
   `sibling_kill_command()` (on the normal path and on the abort path after a
   failed start). It sends SIGTERM to the command's whole process group,
   SIGKILL after 1 s, and reaps it, so the command is gone before `bench`
   returns. `sibling_idle()` and `sibling_stop()` call the same function.
   As a safety net it is also registered with `atexit()`, so any `exit()`
   kills the command too. Ctrl-C / SIGTERM / SIGHUP of `bench` also kill it
   (signal handler).
5. A small **watchdog** process covers the cases where no code of `bench`
   can run any more (SIGKILL, crash, OOM kill); see the next subsection.

#### The watchdog

*Why it is needed.* The explicit kill in `main()`, the `atexit()` hook and the
signal handler are all code inside `bench`. None of them runs if `bench` is
killed with SIGKILL (`kill -9`, the OOM killer) or crashes. The kernel offers
only a "die with my parent" request (`PR_SET_PDEATHSIG`); it is set on the
command's shell, but a test showed it does not stop the processes that the
shell started. Without more, such a command would keep burning the sibling
CPU and silently spoil every later measurement.

*What it is.* A tiny extra process created by `start_watchdog()` in
`src/sibling.c`, right after the command was started. It does nothing but
wait on a pipe:

```
bench ---- write end of pipe ----> watchdog (blocked in read(), no CPU use)
  |                                    |
  +-- forks --> sh -c "CMD" (own       +-- on wake-up: maybe kill(-pgid, SIGKILL)
                process group, pgid)
```

*Protocol.* The pipe is created after the command was forked, so the command
and its children never inherit the write end (otherwise the pipe would never
close). Only `bench` holds it.

| What `bench` does | What the watchdog sees | What the watchdog does |
|-------------------|------------------------|------------------------|
| ends normally: kills the command, writes `D`, closes the pipe, `waitpid`s the watchdog | byte `D` | exits quietly, nothing killed |
| dies by SIGKILL, crash or OOM: the kernel closes the pipe | end-of-file, no `D` | sends SIGKILL to the command's process group, then exits |
| is stopped by SIGINT/SIGTERM/SIGHUP: handler kills the group, then `bench` dies | end-of-file, no `D` | kills the (already dead) group again, harmless, then exits |

The explicit `D` byte exists so that a normal end never makes the watchdog
kill a process group by mistake: once the command is gone, its process-group
id could in theory be reused by an unrelated process.

*Design details*
- It runs in its own session (`setsid`) and ignores SIGINT/SIGHUP/SIGTERM, so
  Ctrl-C or a closed terminal does not kill it before it has done its job.
- After `fork()` it only calls async-signal-safe functions (`close`, `setsid`,
  `signal`, `read`, `kill`, `_exit`), because `bench` has other threads.
- It is created before the busy sweep starts, never during timed regions,
  and it sleeps in `read()` for the whole sweep, so it does not disturb the
  measurement.
- If it cannot be created, `bench` kills the command again and aborts instead
  of running it unprotected.
- While a `-c` command runs, `ps` / `pgrep -x bench` shows **two** `bench`
  processes: `bench` itself and the watchdog. After a normal end, none is left.

*Coverage (tested)*

| How `bench` ended | Command (and its children) left running? |
|---|---|
| normal end of the sweep | no |
| SIGINT, SIGTERM | no |
| `kill -9`, command with two background children | no |
| `kill -9`, command = one plain program | no |
| `kill -9`, command moved itself out with `setsid` | **yes** |

*Limits*
- It kills the command's **process group**. A command that leaves the group
  (`setsid`, daemonizing, `nohup ... &` that starts a new session) is not
  reached; stop it by hand.
- The kill happens a few milliseconds after `bench` dies, not instantly.
- A gap of a few microseconds remains between starting the command and
  starting the watchdog; if `bench` is SIGKILLed exactly then, only the
  "die with my parent" request on the shell applies.
- The watchdog itself can be killed (e.g. by a broad `pkill bench`). Then it
  is gone, and a later SIGKILL of `bench` is no longer covered.

*Checking it yourself.* Start `bench` with a command, find the older of the two
`bench` processes, kill it hard, and check that the command is gone:

```
sudo ./build/bench -c 'sleep 900 & sleep 900 & wait' busy &
pgrep -x bench          # two pids: the older one is bench, the newer the watchdog
sudo kill -9 <older pid>
sleep 1; pgrep -x sleep # no sleep 900 left
```

Notes and limits of command mode:

- The command must **keep running** for the whole busy sweep (all
  `sqrtsd x fill` variants). If it exits earlier, `bench` prints a warning at
  the end: the later samples were not measured under load. Wrap short tasks in
  a loop (`while :; do mytask; done`).
- Everything the command spawns inherits the single-CPU affinity, so a
  multi-threaded command (`stress-ng --cpu 4`, `make -j8`) is squeezed onto the
  one sibling CPU. That is the intended isolation, but it is not how the
  program would run normally.
- stdout/stderr are discarded so they do not disturb the measurement. Redirect
  inside the command to keep them: `-c 'mytool > /tmp/out.log 2>&1'`.
- The command runs with the privileges of `bench`, i.e. **as root** when
  started with `sudo` or `run_exp.sh`. Do not pass untrusted commands.
- *`bench` killed with SIGKILL / crash / OOM:* handled by the watchdog (see
  above).
- A command that detaches itself (`setsid`, daemonizing, `nohup ... &` that
  leaves the process group) escapes the clean-up, including the watchdog, and
  has to be stopped by hand (tested: a `setsid` child survived a SIGKILL of
  `bench`).
- The idle sweep is unchanged: `-c` only affects `busy`. With mode `idle` it is
  ignored (a note is printed), and without an SMT sibling it is ignored too.
- The CSV column `sibling_cmd` only records the command text, not its
  arguments' effect (input files, ...), so keep notes for your experiment.

**Caveats and limitations of the sibling control**

- *Idle is not isolated.* "Idle" only means the helper is blocked. Nothing
  keeps other tasks, kernel threads, timers or interrupts off the sibling CPU,
  so it may not be fully quiet. For cleaner runs, boot with `isolcpus` /
  `nohz_full` for both hyperthreads or at least stop background jobs.
- *Idle is not offline.* The sibling halts in whatever C-state the kernel idle
  driver picks. That state is not checked: the code only sleeps a fixed 2 ms
  after parking. The settling time may be too short for deep C-states, and an
  offlined CPU can behave differently from a halted one.
- *Each busy load is one narrow pattern.* The built-in loads (`pause`, one
  dependent `imul` chain, an empty loop) are not a worst case or a typical
  workload. Results with "busy" only describe contention with the load that
  was compiled in, so always check the `sibling_load` column. Switching loads
  needs a rebuild, and only one load is used per program run.
- *Pinning is checked at creation.* The helper is created with its CPU mask
  already set to the sibling (`pthread_attr_setaffinity_np`), so it never runs
  on the benchmark CPU. If that is impossible, `sibling_start` prints the
  reason and returns `SIBLING_ERROR`, and `bench` aborts instead of producing
  invalid "busy" numbers. The pin is not re-verified while running.
- *Only one sibling is used.* The first other CPU in `thread_siblings_list` is
  chosen. This is correct for 2-way SMT (Zen); with more threads per core the
  others stay uncontrolled.
- *"No sibling" vs "error".* `sibling_start` returns `SIBLING_NONE` when the
  CPU has no sibling (`bench` then runs once with state `none`) and
  `SIBLING_ERROR` when the sibling exists but the helper could not be started.
  The latter aborts `bench` with exit code 1.
- *Fixed order.* Idle always runs before busy within one invocation, so any
  drift (temperature, boost state, page cache) is mixed with the idle-vs-busy
  difference. Repeat runs (`run_exp.sh -i N`) to see whether the difference is
  stable.
- *Virtual machines.* The topology files may not reflect real hyperthread
  pairs, so the "sibling" may be on another physical core, or on the same one
  as some other guest.
- *Not covered:* the helper does not control the frequency governor, other
  physical cores, or shared L3 / memory traffic.

## CSV output

Each program run writes one file, `odata/bench_<timestamp>_cpu<CPU_ID>.csv`
(`odata/` is created relative to the current directory). The header is written
once and one row is appended per benchmark, i.e. per combination of sibling
state x `sqrtsd` count x fill count, and flushed immediately, so a crash or
Ctrl-C keeps the rows measured so far. All times are in RDPRU ticks (core
cycles when `rdpru_sel` is 1). "Blank" means an empty field.

For each row, `bench_empty` (timer only) and `bench_body` (timer + code under
test) are each run `warmup + iterations` times. The first `warmup` samples are
dropped; the statistics below use the remaining `iterations` samples.

**Identification and configuration**

| Column | Meaning |
|--------|---------|
| `timestamp` | Start time of the program run (`YYYYMMDD_HHMMSS`). The same for all rows of a file and part of its name. |
| `cpu_id` | Logical CPU the benchmark thread was pinned to (`CPU_ID`). |
| `cpu_model` | CPU model string: the `model name` field of `/proc/cpuinfo` for the pinned CPU (the `processor : <cpu_id>` block), e.g. `AMD Ryzen 7 7730U with Radeon Graphics`. Written CSV-quoted. `unknown` if it cannot be read. The same value is printed at startup. |
| `sibling_cpu` | Logical CPU of the SMT sibling that the helper thread ran on. Blank if the CPU has no sibling. |
| `sibling_state` | `idle`, `busy` or `none` (no sibling found). See the sibling section above. |
| `sibling_load` | Which busy load ran: the value of `SIBLING_LOAD` (0 = `pause` loop, 1 = dependent `imul` chain, 2 = empty loop, 3 = custom code), or 4 = external command given with `-c`. Only set when `sibling_state` is `busy`; blank otherwise. For 3, the CSV only records the number, so note what your custom code does. |
| `sibling_cmd` | The `-c` command text (CSV-quoted, so it may contain commas and quotes). Blank unless `sibling_load` is 4. |
| `fill_entry` | Number of dependent `imulq $5, %r8` instructions per `UNROLL` repetition in `BODY` (the "fill" count of this variant, from `FOR_EACH_FILL`). The timed region contains `unroll x fill_entry` of them. |
| `sqrt_delay` | Number of dependent `sqrtsd` in `BODY_SETUP` (from `FOR_EACH_SQRT`). They run outside the timed region but delay `%r8`, and so the start of the `imulq` chain. |
| `unroll` | `UNROLL`: how many times `BODY` is repeated inside one timed region. |
| `warmup` | `WARMUP`: samples measured and discarded at the start. |
| `iterations` | `SAMPLES`: samples kept for the statistics (odd, so the median is a real sample). Not `warmup + iterations`; that sum is `pmc_regions`. |
| `rdpru_sel` | RDPRU register read: 0 = MPERF (fixed reference rate), 1 = APERF (actual core clock). |

**Timer statistics.** These are computed from the sorted kept samples, in
ticks. The `empty_*` columns come from the empty timed region (pure timer
overhead); the `body_*` columns from the region with the code under test, which
includes that overhead.

| Columns | Meaning |
|---------|---------|
| `empty_min`, `body_min` | Smallest sample. |
| `empty_median`, `body_median` | Middle sample (index `iterations / 2`). The headline statistic, robust to interrupts and SMIs. |
| `empty_mean`, `body_mean` | Arithmetic mean. Pulled up by outliers, so compare with the median. |
| `empty_stddev`, `body_stddev` | Population standard deviation (divides by `iterations`, not `iterations - 1`). Large values signal outliers. |
| `empty_p99`, `body_p99` | 99th percentile: the sample at index `iterations x 99 / 100` of the sorted array. |
| `empty_max`, `body_max` | Largest sample (typically an interrupt). |

**Derived result**

| Column | Meaning |
|--------|---------|
| `net_median_ticks` | `body_median - empty_median`, clamped at 0: the cost of the code under test with the timer overhead removed. |
| `net_ticks_per_unroll` | `net_median_ticks / unroll`: ticks per `UNROLL` unit. Note that a unit contains `fill_entry` `imulq`, so this is not the cost of a single instruction. |

**Performance counter (PMC).** The counter named in `pmc_event` (`PMC_NAME` /
`PMC_CONFIG` in `bench_config.h`, user space only) is enabled around the whole
`bench_empty` call and the whole `bench_body` call. Unlike the timer columns,
these are **totals over all `pmc_regions` timed regions, warm-up included**,
not medians. If the counter could not be opened (for example
`perf_event_paranoid` too strict) or read, all columns from `pmc_regions` on
are blank, but `pmc_event` is still filled.

| Column | Meaning |
|--------|---------|
| `pmc_event` | Name of the counter, e.g. `IntSch1TokenStall` (AMD Zen 3: dispatch stalled because integer scheduler queue 1 had no free tokens, i.e. the queue was full). |
| `pmc_regions` | Number of timed regions the totals cover: `warmup + iterations`. |
| `pmc_empty_total` | Events counted during the whole empty run (the baseline noise of the timer code). |
| `pmc_body_total` | Events counted during the whole body run. |
| `pmc_net_total` | `pmc_body_total - pmc_empty_total`, clamped at 0. |
| `pmc_net_per_region` | `pmc_net_total / pmc_regions`: average events per timed region. |
| `pmc_net_per_unroll` | `pmc_net_total / (pmc_regions x unroll)`: average events per `UNROLL` unit. |

**Reading the file.** Rows of one scenario differ only in `sibling_state`,
`sqrt_delay` and `fill_entry`, so group by those. Prefer the medians over the
means for timing. The PMC values are averages including warm-up, so they are
not comparable to the timer medians sample by sample. `pysrc/csv_checker.py`
checks that every file in `odata/` has exactly this header.

## Caveats

- Register contract: `BODY` must not touch `%r12-%r15 %rax %rcx %rdx`.
- Keep `UNROLL x size` inside the L1 I-cache / uop cache.
- Turbo/SMT: the sibling hyperthread changes the result; compare the idle and
  busy runs (see above). Other load on the sibling CPU also disturbs both.
- Inside a VM, RDPRU may be unavailable or trap (the CPUID check handles the
  first case only).
- Change `CPU_ID` in `bench_config.h` if that CPU does not exist or is busy.
