#include <stdexcept>
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "ps2_recompiled_stubs.h"

#include "ps2_syscalls.h"
#include "ps2_stubs.h"

#ifdef PS2_FUNCTION_LOG_TRACKER
#include "ps2_log.h"
#endif

// Function: sub_00425118
// Address: 0x425118 - 0x4251d8
void sub_00425118_0x425118(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sub_00425118_0x425118");
#endif

    switch (ctx->pc) {
        case 0x425128u: goto label_425128;
        default: break;
    }

    ctx->pc = 0x425118u;

    // 0x425118: 0x24020018  addiu       $v0, $zero, 0x18
    ctx->pc = 0x425118u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 24));
    // 0x42511c: 0x3c0a0042  lui         $t2, 0x42
    ctx->pc = 0x42511cu;
    SET_GPR_S32(ctx, 10, (int32_t)((uint32_t)66 << 16));
    // 0x425120: 0x254a51c0  addiu       $t2, $t2, 0x51C0
    ctx->pc = 0x425120u;
    SET_GPR_S32(ctx, 10, (int32_t)ADD32(GPR_U32(ctx, 10), 20928));
    // 0x425124: 0x79430000  lq          $v1, 0x0($t2)
    ctx->pc = 0x425124u;
    SET_GPR_VEC(ctx, 3, FAST_READ128(0x4251C0u));
label_425128:
    // 0x425128: 0x78a80000  lq          $t0, 0x0($a1)
    ctx->pc = 0x425128u;
    SET_GPR_VEC(ctx, 8, READ128(ADD32(GPR_U32(ctx, 5), 0)));
    // 0x42512c: 0x2042fffc  addi        $v0, $v0, -0x4
    ctx->pc = 0x42512cu;
    { uint32_t tmp; bool ov; ADD32_OV(GPR_U32(ctx, 2), (int32_t)4294967292, tmp, ov); if (ov) runtime->SignalException(ctx, EXCEPTION_INTEGER_OVERFLOW); else SET_GPR_S32(ctx, 2, (int32_t)tmp); }
    // 0x425130: 0xa0c82d  daddu       $t9, $a1, $zero
    ctx->pc = 0x425130u;
    SET_GPR_U64(ctx, 25, (uint64_t)GPR_U64(ctx, 5) + (uint64_t)GPR_U64(ctx, 0));
    // 0x425134: 0x24a50080  addiu       $a1, $a1, 0x80
    ctx->pc = 0x425134u;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 5), 128));
    // 0x425138: 0x7b290010  lq          $t1, 0x10($t9)
    ctx->pc = 0x425138u;
    SET_GPR_VEC(ctx, 9, READ128(ADD32(GPR_U32(ctx, 25), 16)));
    // 0x42513c: 0x7b2a0020  lq          $t2, 0x20($t9)
    ctx->pc = 0x42513cu;
    SET_GPR_VEC(ctx, 10, READ128(ADD32(GPR_U32(ctx, 25), 32)));
    // 0x425140: 0x7b2b0030  lq          $t3, 0x30($t9)
    ctx->pc = 0x425140u;
    SET_GPR_VEC(ctx, 11, READ128(ADD32(GPR_U32(ctx, 25), 48)));
    // 0x425144: 0x7b2c0040  lq          $t4, 0x40($t9)
    ctx->pc = 0x425144u;
    SET_GPR_VEC(ctx, 12, READ128(ADD32(GPR_U32(ctx, 25), 64)));
    // 0x425148: 0x7b2d0050  lq          $t5, 0x50($t9)
    ctx->pc = 0x425148u;
    SET_GPR_VEC(ctx, 13, READ128(ADD32(GPR_U32(ctx, 25), 80)));
    // 0x42514c: 0x7b2e0060  lq          $t6, 0x60($t9)
    ctx->pc = 0x42514cu;
    SET_GPR_VEC(ctx, 14, READ128(ADD32(GPR_U32(ctx, 25), 96)));
    // 0x425150: 0x7b2f0070  lq          $t7, 0x70($t9)
    ctx->pc = 0x425150u;
    SET_GPR_VEC(ctx, 15, READ128(ADD32(GPR_U32(ctx, 25), 112)));
    // 0x425154: 0x710341e8  pminh       $t0, $t0, $v1
    ctx->pc = 0x425154u;
    SET_GPR_VEC(ctx, 8, PS2_PMINH(GPR_VEC(ctx, 8), GPR_VEC(ctx, 3)));
    // 0x425158: 0x710041c8  pmaxh       $t0, $t0, $zero
    ctx->pc = 0x425158u;
    SET_GPR_VEC(ctx, 8, PS2_PMAXH(GPR_VEC(ctx, 8), GPR_VEC(ctx, 0)));
    // 0x42515c: 0x712349e8  pminh       $t1, $t1, $v1
    ctx->pc = 0x42515cu;
    SET_GPR_VEC(ctx, 9, PS2_PMINH(GPR_VEC(ctx, 9), GPR_VEC(ctx, 3)));
    // 0x425160: 0x712049c8  pmaxh       $t1, $t1, $zero
    ctx->pc = 0x425160u;
    SET_GPR_VEC(ctx, 9, PS2_PMAXH(GPR_VEC(ctx, 9), GPR_VEC(ctx, 0)));
    // 0x425164: 0x71284ec8  ppacb       $t1, $t1, $t0
    ctx->pc = 0x425164u;
    SET_GPR_VEC(ctx, 9, PS2_PPACB(GPR_VEC(ctx, 9), GPR_VEC(ctx, 8)));
    // 0x425168: 0x714351e8  pminh       $t2, $t2, $v1
    ctx->pc = 0x425168u;
    SET_GPR_VEC(ctx, 10, PS2_PMINH(GPR_VEC(ctx, 10), GPR_VEC(ctx, 3)));
    // 0x42516c: 0x714051c8  pmaxh       $t2, $t2, $zero
    ctx->pc = 0x42516cu;
    SET_GPR_VEC(ctx, 10, PS2_PMAXH(GPR_VEC(ctx, 10), GPR_VEC(ctx, 0)));
    // 0x425170: 0x716359e8  pminh       $t3, $t3, $v1
    ctx->pc = 0x425170u;
    SET_GPR_VEC(ctx, 11, PS2_PMINH(GPR_VEC(ctx, 11), GPR_VEC(ctx, 3)));
    // 0x425174: 0x716059c8  pmaxh       $t3, $t3, $zero
    ctx->pc = 0x425174u;
    SET_GPR_VEC(ctx, 11, PS2_PMAXH(GPR_VEC(ctx, 11), GPR_VEC(ctx, 0)));
    // 0x425178: 0x716a5ec8  ppacb       $t3, $t3, $t2
    ctx->pc = 0x425178u;
    SET_GPR_VEC(ctx, 11, PS2_PPACB(GPR_VEC(ctx, 11), GPR_VEC(ctx, 10)));
    // 0x42517c: 0x718361e8  pminh       $t4, $t4, $v1
    ctx->pc = 0x42517cu;
    SET_GPR_VEC(ctx, 12, PS2_PMINH(GPR_VEC(ctx, 12), GPR_VEC(ctx, 3)));
    // 0x425180: 0x718061c8  pmaxh       $t4, $t4, $zero
    ctx->pc = 0x425180u;
    SET_GPR_VEC(ctx, 12, PS2_PMAXH(GPR_VEC(ctx, 12), GPR_VEC(ctx, 0)));
    // 0x425184: 0x71a369e8  pminh       $t5, $t5, $v1
    ctx->pc = 0x425184u;
    SET_GPR_VEC(ctx, 13, PS2_PMINH(GPR_VEC(ctx, 13), GPR_VEC(ctx, 3)));
    // 0x425188: 0x71a069c8  pmaxh       $t5, $t5, $zero
    ctx->pc = 0x425188u;
    SET_GPR_VEC(ctx, 13, PS2_PMAXH(GPR_VEC(ctx, 13), GPR_VEC(ctx, 0)));
    // 0x42518c: 0x71ac6ec8  ppacb       $t5, $t5, $t4
    ctx->pc = 0x42518cu;
    SET_GPR_VEC(ctx, 13, PS2_PPACB(GPR_VEC(ctx, 13), GPR_VEC(ctx, 12)));
    // 0x425190: 0x71c371e8  pminh       $t6, $t6, $v1
    ctx->pc = 0x425190u;
    SET_GPR_VEC(ctx, 14, PS2_PMINH(GPR_VEC(ctx, 14), GPR_VEC(ctx, 3)));
    // 0x425194: 0x71c071c8  pmaxh       $t6, $t6, $zero
    ctx->pc = 0x425194u;
    SET_GPR_VEC(ctx, 14, PS2_PMAXH(GPR_VEC(ctx, 14), GPR_VEC(ctx, 0)));
    // 0x425198: 0x71e379e8  pminh       $t7, $t7, $v1
    ctx->pc = 0x425198u;
    SET_GPR_VEC(ctx, 15, PS2_PMINH(GPR_VEC(ctx, 15), GPR_VEC(ctx, 3)));
    // 0x42519c: 0x71e079c8  pmaxh       $t7, $t7, $zero
    ctx->pc = 0x42519cu;
    SET_GPR_VEC(ctx, 15, PS2_PMAXH(GPR_VEC(ctx, 15), GPR_VEC(ctx, 0)));
    // 0x4251a0: 0x71ee7ec8  ppacb       $t7, $t7, $t6
    ctx->pc = 0x4251a0u;
    SET_GPR_VEC(ctx, 15, PS2_PPACB(GPR_VEC(ctx, 15), GPR_VEC(ctx, 14)));
    // 0x4251a4: 0x7c890000  sq          $t1, 0x0($a0)
    ctx->pc = 0x4251a4u;
    WRITE128(ADD32(GPR_U32(ctx, 4), 0), GPR_VEC(ctx, 9));
    // 0x4251a8: 0x7c8b0010  sq          $t3, 0x10($a0)
    ctx->pc = 0x4251a8u;
    WRITE128(ADD32(GPR_U32(ctx, 4), 16), GPR_VEC(ctx, 11));
    // 0x4251ac: 0x7c8d0020  sq          $t5, 0x20($a0)
    ctx->pc = 0x4251acu;
    WRITE128(ADD32(GPR_U32(ctx, 4), 32), GPR_VEC(ctx, 13));
    // 0x4251b0: 0x7c8f0030  sq          $t7, 0x30($a0)
    ctx->pc = 0x4251b0u;
    WRITE128(ADD32(GPR_U32(ctx, 4), 48), GPR_VEC(ctx, 15));
    // 0x4251b4: 0x1440ffdc  bnez        $v0, . + 4 + (-0x24 << 2)
    ctx->pc = 0x4251B4u;
    {
        const bool branch_taken_0x4251b4 = (GPR_U64(ctx, 2) != GPR_U64(ctx, 0));
        ctx->pc = 0x4251B8u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x4251B4u;
        // 0x4251b8: 0x24840040  addiu       $a0, $a0, 0x40 (Delay Slot)
        SET_GPR_S32(ctx, 4, (int32_t)ADD32(GPR_U32(ctx, 4), 64));
        ctx->in_delay_slot = false;
        if (branch_taken_0x4251b4) {
            ctx->pc = 0x425128u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_425128;
        }
    }
    ctx->pc = 0x4251BCu;
    // 0x4251bc: 0x0  nop
    ctx->pc = 0x4251bcu;
    // NOP
    // 0x4251c0: 0xff00ff  .word       0x00FF00FF                   # dsra32      $zero, $ra, 3 # 00E00000 <InstrIdType: CPU_SPECIAL>
    ctx->pc = 0x4251c0u;
    SET_GPR_S64(ctx, 0, GPR_S64(ctx, 31) >> (32 + 3));
    // 0x4251c4: 0xff00ff  .word       0x00FF00FF                   # dsra32      $zero, $ra, 3 # 00E00000 <InstrIdType: CPU_SPECIAL>
    ctx->pc = 0x4251c4u;
    SET_GPR_S64(ctx, 0, GPR_S64(ctx, 31) >> (32 + 3));
    // 0x4251c8: 0xff00ff  .word       0x00FF00FF                   # dsra32      $zero, $ra, 3 # 00E00000 <InstrIdType: CPU_SPECIAL>
    ctx->pc = 0x4251c8u;
    SET_GPR_S64(ctx, 0, GPR_S64(ctx, 31) >> (32 + 3));
    // 0x4251cc: 0xff00ff  .word       0x00FF00FF                   # dsra32      $zero, $ra, 3 # 00E00000 <InstrIdType: CPU_SPECIAL>
    ctx->pc = 0x4251ccu;
    SET_GPR_S64(ctx, 0, GPR_S64(ctx, 31) >> (32 + 3));
    // 0x4251d0: 0x3e00008  jr          $ra
    ctx->pc = 0x4251D0u;
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        #if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x4251D0u, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
        return;
        #else
        ctx->pc = jumpTarget;
        return;
        #endif
    }
    ctx->pc = 0x4251D8u;
}
