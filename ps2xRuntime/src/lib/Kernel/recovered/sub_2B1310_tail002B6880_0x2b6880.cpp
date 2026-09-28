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

// Function: sub_2B1310_tail002B6880
// Address: 0x2b6880 - 0x2b6900
void sub_2B1310_tail002B6880_0x2b6880(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sub_2B1310_tail002B6880_0x2b6880");
#endif

    switch (ctx->pc) {
        case 0x2b6894u: goto label_2b6894;
        default: break;
    }

    ctx->pc = 0x2b6880u;

    // 0x2b6880: 0xac800000  sw          $zero, 0x0($a0)
    ctx->pc = 0x2b6880u;
    WRITE32(ADD32(GPR_U32(ctx, 4), 0), GPR_U32(ctx, 0));
    // 0x2b6884: 0x8c850000  lw          $a1, 0x0($a0)
    ctx->pc = 0x2b6884u;
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 0)));
    // 0x2b6888: 0x28a10005  slti        $at, $a1, 0x5
    ctx->pc = 0x2b6888u;
    SET_GPR_U64(ctx, 1, ((int64_t)GPR_S64(ctx, 5) < (int64_t)(int32_t)5) ? 1 : 0);
    // 0x2b688c: 0x1020001a  beqz        $at, . + 4 + (0x1A << 2)
    ctx->pc = 0x2B688Cu;
    {
        const bool branch_taken_0x2b688c = (GPR_U64(ctx, 1) == GPR_U64(ctx, 0));
        if (branch_taken_0x2b688c) {
            ctx->pc = 0x2B68F8u;
            goto label_2b68f8;
        }
    }
    ctx->pc = 0x2B6894u;
label_2b6894:
    // 0x2b6894: 0x51900  sll         $v1, $a1, 4
    ctx->pc = 0x2b6894u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 5), 4));
    // 0x2b6898: 0x831821  addu        $v1, $a0, $v1
    ctx->pc = 0x2b6898u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 3)));
    // 0x2b689c: 0xac600004  sw          $zero, 0x4($v1)
    ctx->pc = 0x2b689cu;
    WRITE32(ADD32(GPR_U32(ctx, 3), 4), GPR_U32(ctx, 0));
    // 0x2b68a0: 0x8c830000  lw          $v1, 0x0($a0)
    ctx->pc = 0x2b68a0u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 0)));
    // 0x2b68a4: 0x31900  sll         $v1, $v1, 4
    ctx->pc = 0x2b68a4u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 4));
    // 0x2b68a8: 0x831821  addu        $v1, $a0, $v1
    ctx->pc = 0x2b68a8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 3)));
    // 0x2b68ac: 0xac600008  sw          $zero, 0x8($v1)
    ctx->pc = 0x2b68acu;
    WRITE32(ADD32(GPR_U32(ctx, 3), 8), GPR_U32(ctx, 0));
    // 0x2b68b0: 0x8c830000  lw          $v1, 0x0($a0)
    ctx->pc = 0x2b68b0u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 0)));
    // 0x2b68b4: 0x31900  sll         $v1, $v1, 4
    ctx->pc = 0x2b68b4u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 4));
    // 0x2b68b8: 0x831821  addu        $v1, $a0, $v1
    ctx->pc = 0x2b68b8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 3)));
    // 0x2b68bc: 0xac60000c  sw          $zero, 0xC($v1)
    ctx->pc = 0x2b68bcu;
    WRITE32(ADD32(GPR_U32(ctx, 3), 12), GPR_U32(ctx, 0));
    // 0x2b68c0: 0x8c830000  lw          $v1, 0x0($a0)
    ctx->pc = 0x2b68c0u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 0)));
    // 0x2b68c4: 0x31900  sll         $v1, $v1, 4
    ctx->pc = 0x2b68c4u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 4));
    // 0x2b68c8: 0x831821  addu        $v1, $a0, $v1
    ctx->pc = 0x2b68c8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 3)));
    // 0x2b68cc: 0xac600010  sw          $zero, 0x10($v1)
    ctx->pc = 0x2b68ccu;
    WRITE32(ADD32(GPR_U32(ctx, 3), 16), GPR_U32(ctx, 0));
    // 0x2b68d0: 0x8c850000  lw          $a1, 0x0($a0)
    ctx->pc = 0x2b68d0u;
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 0)));
    // 0x2b68d4: 0x51880  sll         $v1, $a1, 2
    ctx->pc = 0x2b68d4u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 5), 2));
    // 0x2b68d8: 0x831821  addu        $v1, $a0, $v1
    ctx->pc = 0x2b68d8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 3)));
    // 0x2b68dc: 0xac650054  sw          $a1, 0x54($v1)
    ctx->pc = 0x2b68dcu;
    WRITE32(ADD32(GPR_U32(ctx, 3), 84), GPR_U32(ctx, 5));
    // 0x2b68e0: 0x8c830000  lw          $v1, 0x0($a0)
    ctx->pc = 0x2b68e0u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 4), 0)));
    // 0x2b68e4: 0x24650001  addiu       $a1, $v1, 0x1
    ctx->pc = 0x2b68e4u;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 3), 1));
    // 0x2b68e8: 0x28a30005  slti        $v1, $a1, 0x5
    ctx->pc = 0x2b68e8u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 5) < (int64_t)(int32_t)5) ? 1 : 0);
    // 0x2b68ec: 0x1460ffe9  bnez        $v1, . + 4 + (-0x17 << 2)
    ctx->pc = 0x2B68ECu;
    {
        const bool branch_taken_0x2b68ec = (GPR_U64(ctx, 3) != GPR_U64(ctx, 0));
        ctx->pc = 0x2B68F0u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x2B68ECu;
        // 0x2b68f0: 0xac850000  sw          $a1, 0x0($a0) (Delay Slot)
        WRITE32(ADD32(GPR_U32(ctx, 4), 0), GPR_U32(ctx, 5));
        ctx->in_delay_slot = false;
        if (branch_taken_0x2b68ec) {
            ctx->pc = 0x2B6894u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_2b6894;
        }
    }
    ctx->pc = 0x2B68F4u;
    // 0x2b68f4: 0x0  nop
    ctx->pc = 0x2b68f4u;
    // NOP
label_2b68f8:
    // 0x2b68f8: 0x3e00008  jr          $ra
    ctx->pc = 0x2B68F8u;
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        #if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x2B68F8u, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
        return;
        #else
        ctx->pc = jumpTarget;
        return;
        #endif
    }
    ctx->pc = 0x2B6900u;
}
