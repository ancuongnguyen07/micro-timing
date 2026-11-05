/* src/sib_kernels.S  -  the two code blocks run on the SMT sibling
 *
 *   void sib_imul(uint64_t iters)   iters x SIB_IMUL_BLOCK  (sib_imul.inc)
 *   void sib_nop (uint64_t iters)   iters x SIB_NOP_BLOCK   (sib_nop.inc)
 *
 * Same loop skeleton in both (dec + jnz, 32-byte aligned) so they differ only
 * in the block. C-callable (SysV): %rdi = iters; only caller-saved registers
 * are touched. sibling.c picks one per segment at run time.
 */
#include "sib_imul.inc"
#include "sib_nop.inc"

    .text

    .globl  sib_imul
    .type   sib_imul, @function
    .p2align 5
sib_imul:
    mov     $3, %r8d                # start of the dependent chain
    test    %rdi, %rdi
    jz      2f
    .p2align 5
1:
    SIB_IMUL_BLOCK
    dec     %rdi
    jnz     1b
2:
    ret
    .size   sib_imul, .-sib_imul

    .globl  sib_nop
    .type   sib_nop, @function
    .p2align 5
sib_nop:
    test    %rdi, %rdi
    jz      2f
    .p2align 5
1:
    SIB_NOP_BLOCK
    dec     %rdi
    jnz     1b
2:
    ret
    .size   sib_nop, .-sib_nop

    .section .note.GNU-stack,"",@progbits
