/* include/sibling.h  -  switch the SMT sibling of the benchmark CPU idle/busy
 *
 * A helper thread is pinned to the sibling hyperthread of `cpu`.
 *   idle : the thread blocks in the kernel (futex wait) -> the sibling CPU has
 *          nothing to run and halts, so the benchmark thread gets the whole core
 *   busy : the thread runs the workload chosen by SIBLING_LOAD (bench_config.h;
 *          code in busy_chunk() in sibling.c) -> the sibling
 *          competes for the core's execution units / schedulers
 * Both calls return only once the sibling is really in that state.
 * No root needed (unlike offlining the CPU through sysfs).
 */
#ifndef SIBLING_H
#define SIBLING_H

#define SIBLING_NONE  (-1) /* `cpu` has no SMT sibling (SMT off / no sysfs) */
#define SIBLING_ERROR (-2) /* sibling exists but the helper could not be
                              started or pinned to it (reason on stderr) */

/* Find the sibling of `cpu`, start the helper pinned to it (initially idle).
 * Returns the sibling's logical CPU number (>= 0), SIBLING_NONE or
 * SIBLING_ERROR. The helper is never left running unpinned. */
int  sibling_start(int cpu);
void sibling_idle(void);
/* Returns 0 on success, -1 if the busy load could not be started (only
 * possible in command mode: fork failed, or the command exited right away). */
int  sibling_busy(void);
void sibling_stop(void);

/* COMMAND MODE. Instead of the built-in busy_chunk() workload, "busy" runs
 *   /bin/sh -c "<cmd>"
 * as a separate process whose CPU affinity is the sibling CPU only. The
 * command and everything it spawns therefore run on the sibling and nowhere
 * else (a multi-threaded command is squeezed onto that one CPU).
 *   - stdin/stdout/stderr go to /dev/null (redirect inside <cmd> to keep
 *     output); the command runs as the same user as bench (root under sudo).
 *   - it gets its own process group; sibling_idle()/sibling_stop() send it
 *     SIGTERM, then SIGKILL after 1 s, to the whole group. Ctrl-C/SIGTERM
 *     of bench also kills it. A command that detaches itself into another
 *     process group (setsid, daemonizing) escapes this and must be stopped by hand.
 *   - it should run until stopped. If it exits earlier, a warning is printed
 *     (later samples were not measured under load).
 * Call before sibling_start(); NULL or "" selects the built-in workload. */
void sibling_set_command(const char *cmd);

/* Explicitly terminate the `-c` command, if one is running: SIGTERM to its
 * whole process group, SIGKILL after 1 s, then reap it. Idempotent and safe
 * to call at any time, also without a command. bench calls it before it
 * ends on every path; it is also registered with atexit() once a command has
 * been started, as a safety net for exit() calls. */
void sibling_kill_command(void);
const char *sibling_get_command(void); /* NULL = built-in workload */

#endif
