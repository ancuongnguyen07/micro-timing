/* include/body.inc  -  *** THE FILE YOU EDIT *** : the code under test
 *
 * Two macros:
 *   BODY_SETUP  runs once per sample, OUTSIDE the timed region. Put register
 *               / memory initialisation here.
 *   BODY        the instruction(s) being timed. It is expanded ONCE per
 *               sample; repeat it UNROLL times with .rept so the timer cost
 *               is amortised. Result = (median - overhead) / UNROLL.
 *
 * Constraints (see rdpru.inc): do not touch %r12-%r15 %rax %rcx %rdx.
 * Free: %rbx %rsi %rdi %rbp %r8-%r11, xmm/ymm.
 *
 * LATENCY  vs  THROUGHPUT is decided by data dependencies in BODY:
 *   - every instruction reads the result of the previous one  => LATENCY
 *   - instructions are independent of each other               => THROUGHPUT
 *     (reciprocal; limited by ports / decode / retire width)
 * To time a single instruction with .rept 1 you would measure mostly timer
 * noise; prefer a long chain and divide.
 */

#include "bench_config.h"

    /* SQRT = number of dependent sqrtsd that delay r8; bench.S instantiates
     * the loops once per value in FOR_EACH_SQRT (bench_config.h). */
    .macro BODY_SETUP SQRT=SQRT_ENTRY
    mov     $3, %esi
    mov     $5, %edi

    /* Drain the scheduler queue by dummy loop */
    movl $10000, %ebx
1:
    dec %ebx
    jnz 1b /* Jump to the nearest label 1 backwards */

    /* Delay subsequent multiplications in BODY */
    movq $123456789, %r8
    cvtsi2sd %r8, %xmm0
    .rept \SQRT
    sqrtsd %xmm0, %xmm0
    .endr
    cvtsd2si %xmm0, %r8

    .endm # BODY_SETUP

    /* fill = number of dependent imulq per UNROLL; bench.S instantiates this
     * once per value in FOR_EACH_FILL (bench_config.h). */
    .macro BODY fill=FILL_ENTRY
    .rept UNROLL
        .rept \fill
            imulq $5, %r8 /* change the instruction later for different target ALU */
        .endr
    .endr
    .endm # BODY

