/* src/bench.S  -  the measurement loops
 *
 * Two functions are generated from ONE template so they are identical except
 * for the body:
 *     bench_empty : timer + nothing      -> measures the timer overhead
 *     bench_body  : timer + BODY         -> overhead + the code under test
 *
 * void bench_xxx(uint64_t *samples, uint64_t count)
 *   in : %rdi = output array, %rsi = number of timed regions to run
 *   out: samples[i] = elapsed RDPRU ticks of region i
 *
 * Registers inside the loop:
 *   %r12 start timestamp   %r13 samples ptr   %r14 count   %r15 index
 */

#include "rdpru.inc"
#include "body.inc"

    .text

    /* fill / sqrt are optional; empty = the macro defaults (FILL_ENTRY,
     * SQRT_ENTRY). Call with keywords: BENCH_FN f, BODY, fill=20, sqrt=6 */
    .macro BENCH_FN name, body, fill=, sqrt=
    .globl  \name
    .type   \name, @function
\name:
    push    %rbx                    # callee-saved regs that BODY may use
    push    %rbp
    push    %r12
    push    %r13
    push    %r14
    push    %r15 /* index i for the loop */

    mov     %rdi, %r13 /* Output array */
    mov     %rsi, %r14 /* Number of repetitions */
    xor     %r15d, %r15d

.L\name\()_loop:
    BODY_SETUP \sqrt                # untimed
    TIMER_START
    \body \fill                     # <-- the only difference between the two
    TIMER_STOP
    mov     %rax, (%r13,%r15,8)     # samples[i] = elapsed
    inc     %r15
    cmp     %r14, %r15
    jb      .L\name\()_loop

    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbp
    pop     %rbx
    ret
    .size   \name, .-\name
    .endm

    .macro  BODY_NONE
    .endm

    BENCH_FN bench_empty, BODY_NONE
    BENCH_FN bench_body,  BODY

    /* Run-time sweep, generated from the lists in bench_config.h.
     * cpp expands the lists; ';' separates the generated statements.
     *   bench_empty_s<S>       timer only, setup with S sqrtsd
     *   bench_body_s<S>_f<N>   S sqrtsd in setup, N fill instructions */
#define GEN_EMPTY(s) BENCH_FN bench_empty_s##s, BODY_NONE, sqrt=s ;
    FOR_EACH_SQRT(GEN_EMPTY)
#define GEN_BODY(s, n) BENCH_FN bench_body_s##s##_f##n, BODY, fill=n, sqrt=s ;
#define GEN_ROW(s) FOR_EACH_FILL(GEN_BODY, s)
    FOR_EACH_SQRT(GEN_ROW)

    .section .note.GNU-stack,"",@progbits
