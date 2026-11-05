/* src/timeseries.S  -  RDPRU time series of the BODY probe
 *
 * uint64_t bench_timeseries(struct ts_sample *out, uint64_t cap,
 *                           uint64_t window_ticks, volatile uint32_t *seg_id)
 *   in : %rdi = out array, %rsi = capacity, %rdx = window (RDPRU ticks),
 *        %rcx = word the sibling thread updates with the current segment id
 *   out: %rax = number of samples written
 *
 * Probes run back to back until (now - t0) >= window or the array is full.
 * Per sample (24 bytes, see bench.h):
 *   +0  timestamp = probe start - t0   (RDPRU ticks)
 *   +8  latency   = elapsed ticks of the timed BODY
 *   +16 seg_id    = *seg_id read right after the probe (untimed)
 * BODY_SETUP (drain loop + sqrtsd delay) runs before EVERY sample, untimed,
 * exactly as in bench.S: the delayed imul chain backs up in the scheduler
 * queue, which is what makes the probe sensitive to the sibling thread.
 * (Running it only once removes that effect: imul and nop then differ by
 * only a few cycles.) Registers: %r12 start ts, %r13 out cursor, %r14 slots left,
 * %r15 deadline; stack: [0] out base, [8] t0, [16] seg_id pointer.
 */
#include "rdpru.inc"
#include "body.inc"

    .text
    .globl  bench_timeseries
    .type   bench_timeseries, @function
bench_timeseries:
    push    %rbx
    push    %rbp
    push    %r12
    push    %r13
    push    %r14
    push    %r15
    sub     $24, %rsp

    mov     %rdi, 0(%rsp)
    mov     %rcx, 16(%rsp)
    mov     %rdi, %r13
    mov     %rsi, %r14
    mov     %rdx, %r15

    RDPRU_READ                      # %rax = t0
    mov     %rax, 8(%rsp)
    add     %rax, %r15              # deadline = t0 + window

    test    %r14, %r14
    jz      2f
.Lts_loop:
    BODY_SETUP                      # per sample, untimed
    TIMER_START
    BODY
    TIMER_STOP                      # %rax = elapsed
    mov     %r12, %r11
    sub     8(%rsp), %r11           # timestamp relative to t0
    mov     %r11, 0(%r13)
    mov     %rax, 8(%r13)
    mov     16(%rsp), %r11
    mov     (%r11), %r11d           # current sibling segment id
    mov     %r11, 16(%r13)          # (upper 32 bits = 0)
    add     $24, %r13
    add     %r12, %rax              # end of this probe
    cmp     %r15, %rax
    jae     2f
    dec     %r14
    jnz     .Lts_loop
2:
    mov     %r13, %rax
    sub     0(%rsp), %rax
    xor     %edx, %edx
    mov     $24, %ecx
    div     %rcx                    # bytes / 24 = samples

    add     $24, %rsp
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %rbp
    pop     %rbx
    ret
    .size   bench_timeseries, .-bench_timeseries

    .section .note.GNU-stack,"",@progbits
