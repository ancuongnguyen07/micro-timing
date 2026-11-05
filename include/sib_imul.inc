/* include/sib_imul.inc  -  sibling block #1: a chain of dependent imul
 *
 * SIB_IMUL_BLOCK fill   emits `fill` dependent `imulq $5, %r8`: occupies the
 *                       multiplier and its integer scheduler queue.
 *
 * Used by src/sib_kernels.S (function sib_imul) on the SMT SIBLING thread,
 * not on the thread that runs the RDPRU measurement. Edit the instruction
 * here to change what the sibling runs in the "imul" segments.
 * Only %r8 is touched (caller-saved, set up by sib_imul).
 */
#include "bench_config.h"

    .macro SIB_IMUL_BLOCK fill=TS_SIB_FILL
    .rept \fill
        imulq   $5, %r8
    .endr
    .endm
