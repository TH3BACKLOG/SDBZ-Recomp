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

// Function: sub_00425028
// Address: 0x425028 - 0x425118
void sub_00425028_0x425028(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sub_00425028_0x425028");
#endif

    switch (ctx->pc) {
        case 0x425038u: goto label_425038;
        default: break;
    }

    ctx->pc = 0x425028u;

    // 0x425028: 0x24020018  addiu       $v0, $zero, 0x18
    ctx->pc = 0x425028u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 24));
    // 0x42502c: 0x3c0a0042  lui         $t2, 0x42
    ctx->pc = 0x42502cu;
    SET_GPR_S32(ctx, 10, (int32_t)((uint32_t)66 << 16));
    // 0x425030: 0x254a51c0  addiu       $t2, $t2, 0x51C0
    ctx->pc = 0x425030u;
    SET_GPR_S32(ctx, 10, (int32_t)ADD32(GPR_U32(ctx, 10), 20928));
    // 0x425034: 0x79430000  lq          $v1, 0x0($t2)
    ctx->pc = 0x425034u;
    SET_GPR_VEC(ctx, 3, FAST_READ128(0x4251C0u));
label_425038:
    // 0x425038: 0x78c80000  lq          $t0, 0x0($a2)
    ctx->pc = 0x425038u;
    SET_GPR_VEC(ctx, 8, READ128(ADD32(GPR_U32(ctx, 6), 0)));
    // 0x42503c: 0x2042fffc  addi        $v0, $v0, -0x4
    ctx->pc = 0x42503cu;
    { uint32_t tmp; bool ov; ADD32_OV(GPR_U32(ctx, 2), (int32_t)4294967292, tmp, ov); if (ov) runtime->SignalException(ctx, EXCEPTION_INTEGER_OVERFLOW); else SET_GPR_S32(ctx, 2, (int32_t)tmp); }
    // 0x425040: 0x78c90010  lq          $t1, 0x10($a2)
    ctx->pc = 0x425040u;
    SET_GPR_VEC(ctx, 9, READ128(ADD32(GPR_U32(ctx, 6), 16)));
    // 0x425044: 0x78ca0020  lq          $t2, 0x20($a2)
    ctx->pc = 0x425044u;
    SET_GPR_VEC(ctx, 10, READ128(ADD32(GPR_U32(ctx, 6), 32)));
    // 0x425048: 0x78cb0030  lq          $t3, 0x30($a2)
    ctx->pc = 0x425048u;
    SET_GPR_VEC(ctx, 11, READ128(ADD32(GPR_U32(ctx, 6), 48)));
    // 0x42504c: 0x78cc0040  lq          $t4, 0x40($a2)
    ctx->pc = 0x42504cu;
    SET_GPR_VEC(ctx, 12, READ128(ADD32(GPR_U32(ctx, 6), 64)));
    // 0x425050: 0x78cd0050  lq          $t5, 0x50($a2)
    ctx->pc = 0x425050u;
    SET_GPR_VEC(ctx, 13, READ128(ADD32(GPR_U32(ctx, 6), 80)));
    // 0x425054: 0x78ce0060  lq          $t6, 0x60($a2)
    ctx->pc = 0x425054u;
    SET_GPR_VEC(ctx, 14, READ128(ADD32(GPR_U32(ctx, 6), 96)));
    // 0x425058: 0x78cf0070  lq          $t7, 0x70($a2)
    ctx->pc = 0x425058u;
    SET_GPR_VEC(ctx, 15, READ128(ADD32(GPR_U32(ctx, 6), 112)));
    // 0x42505c: 0x78b90000  lq          $t9, 0x0($a1)
    ctx->pc = 0x42505cu;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 0)));
    // 0x425060: 0x71194108  paddh       $t0, $t0, $t9
    ctx->pc = 0x425060u;
    SET_GPR_VEC(ctx, 8, PS2_PADDH(GPR_VEC(ctx, 8), GPR_VEC(ctx, 25)));
    // 0x425064: 0x78b90010  lq          $t9, 0x10($a1)
    ctx->pc = 0x425064u;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 16)));
    // 0x425068: 0x710341e8  pminh       $t0, $t0, $v1
    ctx->pc = 0x425068u;
    SET_GPR_VEC(ctx, 8, PS2_PMINH(GPR_VEC(ctx, 8), GPR_VEC(ctx, 3)));
    // 0x42506c: 0x710041c8  pmaxh       $t0, $t0, $zero
    ctx->pc = 0x42506cu;
    SET_GPR_VEC(ctx, 8, PS2_PMAXH(GPR_VEC(ctx, 8), GPR_VEC(ctx, 0)));
    // 0x425070: 0x71394908  paddh       $t1, $t1, $t9
    ctx->pc = 0x425070u;
    SET_GPR_VEC(ctx, 9, PS2_PADDH(GPR_VEC(ctx, 9), GPR_VEC(ctx, 25)));
    // 0x425074: 0x78b90020  lq          $t9, 0x20($a1)
    ctx->pc = 0x425074u;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 32)));
    // 0x425078: 0x712349e8  pminh       $t1, $t1, $v1
    ctx->pc = 0x425078u;
    SET_GPR_VEC(ctx, 9, PS2_PMINH(GPR_VEC(ctx, 9), GPR_VEC(ctx, 3)));
    // 0x42507c: 0x712049c8  pmaxh       $t1, $t1, $zero
    ctx->pc = 0x42507cu;
    SET_GPR_VEC(ctx, 9, PS2_PMAXH(GPR_VEC(ctx, 9), GPR_VEC(ctx, 0)));
    // 0x425080: 0x71284ec8  ppacb       $t1, $t1, $t0
    ctx->pc = 0x425080u;
    SET_GPR_VEC(ctx, 9, PS2_PPACB(GPR_VEC(ctx, 9), GPR_VEC(ctx, 8)));
    // 0x425084: 0x71595108  paddh       $t2, $t2, $t9
    ctx->pc = 0x425084u;
    SET_GPR_VEC(ctx, 10, PS2_PADDH(GPR_VEC(ctx, 10), GPR_VEC(ctx, 25)));
    // 0x425088: 0x714351e8  pminh       $t2, $t2, $v1
    ctx->pc = 0x425088u;
    SET_GPR_VEC(ctx, 10, PS2_PMINH(GPR_VEC(ctx, 10), GPR_VEC(ctx, 3)));
    // 0x42508c: 0x78b90030  lq          $t9, 0x30($a1)
    ctx->pc = 0x42508cu;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 48)));
    // 0x425090: 0x714051c8  pmaxh       $t2, $t2, $zero
    ctx->pc = 0x425090u;
    SET_GPR_VEC(ctx, 10, PS2_PMAXH(GPR_VEC(ctx, 10), GPR_VEC(ctx, 0)));
    // 0x425094: 0x71795908  paddh       $t3, $t3, $t9
    ctx->pc = 0x425094u;
    SET_GPR_VEC(ctx, 11, PS2_PADDH(GPR_VEC(ctx, 11), GPR_VEC(ctx, 25)));
    // 0x425098: 0x78b90040  lq          $t9, 0x40($a1)
    ctx->pc = 0x425098u;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 64)));
    // 0x42509c: 0x716359e8  pminh       $t3, $t3, $v1
    ctx->pc = 0x42509cu;
    SET_GPR_VEC(ctx, 11, PS2_PMINH(GPR_VEC(ctx, 11), GPR_VEC(ctx, 3)));
    // 0x4250a0: 0x716059c8  pmaxh       $t3, $t3, $zero
    ctx->pc = 0x4250a0u;
    SET_GPR_VEC(ctx, 11, PS2_PMAXH(GPR_VEC(ctx, 11), GPR_VEC(ctx, 0)));
    // 0x4250a4: 0x716a5ec8  ppacb       $t3, $t3, $t2
    ctx->pc = 0x4250a4u;
    SET_GPR_VEC(ctx, 11, PS2_PPACB(GPR_VEC(ctx, 11), GPR_VEC(ctx, 10)));
    // 0x4250a8: 0x71996108  paddh       $t4, $t4, $t9
    ctx->pc = 0x4250a8u;
    SET_GPR_VEC(ctx, 12, PS2_PADDH(GPR_VEC(ctx, 12), GPR_VEC(ctx, 25)));
    // 0x4250ac: 0x78b90050  lq          $t9, 0x50($a1)
    ctx->pc = 0x4250acu;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 80)));
    // 0x4250b0: 0x718361e8  pminh       $t4, $t4, $v1
    ctx->pc = 0x4250b0u;
    SET_GPR_VEC(ctx, 12, PS2_PMINH(GPR_VEC(ctx, 12), GPR_VEC(ctx, 3)));
    // 0x4250b4: 0x718061c8  pmaxh       $t4, $t4, $zero
    ctx->pc = 0x4250b4u;
    SET_GPR_VEC(ctx, 12, PS2_PMAXH(GPR_VEC(ctx, 12), GPR_VEC(ctx, 0)));
    // 0x4250b8: 0x71b96908  paddh       $t5, $t5, $t9
    ctx->pc = 0x4250b8u;
    SET_GPR_VEC(ctx, 13, PS2_PADDH(GPR_VEC(ctx, 13), GPR_VEC(ctx, 25)));
    // 0x4250bc: 0x78b90060  lq          $t9, 0x60($a1)
    ctx->pc = 0x4250bcu;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 96)));
    // 0x4250c0: 0x71a369e8  pminh       $t5, $t5, $v1
    ctx->pc = 0x4250c0u;
    SET_GPR_VEC(ctx, 13, PS2_PMINH(GPR_VEC(ctx, 13), GPR_VEC(ctx, 3)));
    // 0x4250c4: 0x71a069c8  pmaxh       $t5, $t5, $zero
    ctx->pc = 0x4250c4u;
    SET_GPR_VEC(ctx, 13, PS2_PMAXH(GPR_VEC(ctx, 13), GPR_VEC(ctx, 0)));
    // 0x4250c8: 0x71ac6ec8  ppacb       $t5, $t5, $t4
    ctx->pc = 0x4250c8u;
    SET_GPR_VEC(ctx, 13, PS2_PPACB(GPR_VEC(ctx, 13), GPR_VEC(ctx, 12)));
    // 0x4250cc: 0x71d97108  paddh       $t6, $t6, $t9
    ctx->pc = 0x4250ccu;
    SET_GPR_VEC(ctx, 14, PS2_PADDH(GPR_VEC(ctx, 14), GPR_VEC(ctx, 25)));
    // 0x4250d0: 0x78b90070  lq          $t9, 0x70($a1)
    ctx->pc = 0x4250d0u;
    SET_GPR_VEC(ctx, 25, READ128(ADD32(GPR_U32(ctx, 5), 112)));
    // 0x4250d4: 0x71c371e8  pminh       $t6, $t6, $v1
    ctx->pc = 0x4250d4u;
    SET_GPR_VEC(ctx, 14, PS2_PMINH(GPR_VEC(ctx, 14), GPR_VEC(ctx, 3)));
    // 0x4250d8: 0x71c071c8  pmaxh       $t6, $t6, $zero
    ctx->pc = 0x4250d8u;
    SET_GPR_VEC(ctx, 14, PS2_PMAXH(GPR_VEC(ctx, 14), GPR_VEC(ctx, 0)));
    // 0x4250dc: 0x7c890000  sq          $t1, 0x0($a0)
    ctx->pc = 0x4250dcu;
    WRITE128(ADD32(GPR_U32(ctx, 4), 0), GPR_VEC(ctx, 9));
    // 0x4250e0: 0x71f97908  paddh       $t7, $t7, $t9
    ctx->pc = 0x4250e0u;
    SET_GPR_VEC(ctx, 15, PS2_PADDH(GPR_VEC(ctx, 15), GPR_VEC(ctx, 25)));
    // 0x4250e4: 0x71e379e8  pminh       $t7, $t7, $v1
    ctx->pc = 0x4250e4u;
    SET_GPR_VEC(ctx, 15, PS2_PMINH(GPR_VEC(ctx, 15), GPR_VEC(ctx, 3)));
    // 0x4250e8: 0x71e079c8  pmaxh       $t7, $t7, $zero
    ctx->pc = 0x4250e8u;
    SET_GPR_VEC(ctx, 15, PS2_PMAXH(GPR_VEC(ctx, 15), GPR_VEC(ctx, 0)));
    // 0x4250ec: 0x71ee7ec8  ppacb       $t7, $t7, $t6
    ctx->pc = 0x4250ecu;
    SET_GPR_VEC(ctx, 15, PS2_PPACB(GPR_VEC(ctx, 15), GPR_VEC(ctx, 14)));
    // 0x4250f0: 0x7c8b0010  sq          $t3, 0x10($a0)
    ctx->pc = 0x4250f0u;
    WRITE128(ADD32(GPR_U32(ctx, 4), 16), GPR_VEC(ctx, 11));
    // 0x4250f4: 0x7c8d0020  sq          $t5, 0x20($a0)
    ctx->pc = 0x4250f4u;
    WRITE128(ADD32(GPR_U32(ctx, 4), 32), GPR_VEC(ctx, 13));
    // 0x4250f8: 0x7c8f0030  sq          $t7, 0x30($a0)
    ctx->pc = 0x4250f8u;
    WRITE128(ADD32(GPR_U32(ctx, 4), 48), GPR_VEC(ctx, 15));
    // 0x4250fc: 0x24a50080  addiu       $a1, $a1, 0x80
    ctx->pc = 0x4250fcu;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 5), 128));
    // 0x425100: 0x24840040  addiu       $a0, $a0, 0x40
    ctx->pc = 0x425100u;
    SET_GPR_S32(ctx, 4, (int32_t)ADD32(GPR_U32(ctx, 4), 64));
    // 0x425104: 0x1440ffcc  bnez        $v0, . + 4 + (-0x34 << 2)
    ctx->pc = 0x425104u;
    {
        const bool branch_taken_0x425104 = (GPR_U64(ctx, 2) != GPR_U64(ctx, 0));
        ctx->pc = 0x425108u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x425104u;
        // 0x425108: 0x24c60080  addiu       $a2, $a2, 0x80 (Delay Slot)
        SET_GPR_S32(ctx, 6, (int32_t)ADD32(GPR_U32(ctx, 6), 128));
        ctx->in_delay_slot = false;
        if (branch_taken_0x425104) {
            ctx->pc = 0x425038u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_425038;
        }
    }
    ctx->pc = 0x42510Cu;
    // 0x42510c: 0x3e00008  jr          $ra
    ctx->pc = 0x42510Cu;
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        #if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x42510Cu, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
        return;
        #else
        ctx->pc = jumpTarget;
        return;
        #endif
    }
    ctx->pc = 0x425114u;
    // 0x425114: 0x0  nop
    ctx->pc = 0x425114u;
    // NOP
    ctx->pc = 0x425118u;
}
