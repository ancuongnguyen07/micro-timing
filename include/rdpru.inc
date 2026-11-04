/* include/rdpru.inc  -  timer macros built on RDPRU (AMD Zen 2+)
 *
 * RDPRU  (opcode 0F 01 FD)
 *   in : %ecx = register selector (0 = MPERF, 1 = APERF)
 *   out: %edx:%eax = 64-bit counter value      (clobbers %rax %rcx %rdx)
 *
 * RDPRU is NOT serializing: the CPU may execute it before older
 * instructions finish, or let younger ones start before it completes.
 * AMD's recommended fence (same trick as for RDTSC) is LFENCE:
 *   - lfence BEFORE rdpru : older instructions have completed
 *   - lfence AFTER  rdpru : younger instructions do not start earlier
 * (On Zen, Linux sets DE_CFG[1] so lfence is dispatch-serializing.)
 *
 * Register contract used by bench.S:
 *   %r12 = start timestamp (TIMER_START writes it, TIMER_STOP reads it)
 * so the code under test must NOT touch %r12. Also keep it away from
 * %rax %rcx %rdx between the macros, and away from %r13-%r15 (bench.S loop
 * state). Free for the body: %rbx %rsi %rdi %rbp %r8-%r11 and all xmm/ymm.
 *
 * Contains gas directives (.macro), so include it from .S files only.
 */

#include "bench_config.h"

/* %rax = 64-bit counter value (no fencing) */
    .macro RDPRU_READ
    mov     $RDPRU_SEL, %ecx
    rdpru
    shl     $32, %rdx
    or      %rdx, %rax
    .endm

/* Begin the timed region: %r12 = timestamp */
    .macro TIMER_START
    RDPRU_READ
    mov     %rax, %r12
    .endm

/* End the timed region: %rax = elapsed ticks since TIMER_START */
    .macro TIMER_STOP
    RDPRU_READ
    sub     %r12, %rax
    .endm
