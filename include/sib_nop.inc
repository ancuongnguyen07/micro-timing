/* include/sib_nop.inc  -  sibling block #2: `fill` nop
 *
 * SIB_NOP_BLOCK fill   emits `fill` nop: uses front-end / dispatch slots but
 *                      no execution unit and no scheduler-queue entry for a
 *                      result.
 *
 * Used by src/sib_kernels.S (function sib_nop) on the SMT SIBLING thread.
 * Touches no register.
 */
#include "bench_config.h"

    .macro SIB_NOP_BLOCK fill=TS_SIB_FILL
    .rept \fill
        nop
    .endr
    .endm
