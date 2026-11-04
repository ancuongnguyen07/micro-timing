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
            imulq $5, %r8
        .endr
    .endr
    .endm # BODY

/* ---- example 1 (active): LATENCY of 64-bit imul  (Zen 3: expect ~3.00)
    .macro BODY
    .rept UNROLL
    imul    %rsi, %rsi              # rsi = rsi * rsi : depends on previous rsi
    .endr
    .endm
*/

/* ---- example 2: THROUGHPUT of imul (independent dest regs) --------------
 *    .macro BODY
 *    .rept UNROLL / 4
 *    imul    %rsi, %rbx
 *    imul    %rsi, %rdi
 *    imul    %rsi, %r8
 *    imul    %rsi, %r9
 *    .endr
 *    .endm
 *   (then divide by UNROLL, as before)
 *
 * ---- example 3: LATENCY of a load (pointer chase, L1 hit: ~4-5) ---------
 *    BODY_SETUP:  lea  slot(%rip), %rsi ; mov %rsi, (%rsi)  # slot -> slot
 *    BODY:        mov  (%rsi), %rsi
 *   (declare `slot: .quad 0` in .data)
 *
 * ---- example 4: a block of different instructions, one dependency chain -
 *    .macro BODY
 *    .rept UNROLL
 *    add     %rsi, %rdi
 *    xor     %rdi, %rsi
 *    rol     $7, %rsi
 *    .endr
 *    .endm
 *   (reports cycles per BLOCK of 3 instructions, not per instruction)
 */
