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

// Function: sub_2F8180_tail002E5010
// Address: 0x2e5010 - 0x2e5040
void sub_2F8180_tail002E5010_0x2e5010(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sub_2F8180_tail002E5010_0x2e5010");
#endif

    switch (ctx->pc) {
        case 0x2e5028u: goto label_2e5028;
        default: break;
    }

    ctx->pc = 0x2e5010u;

    // 0x2e5010: 0x90860084  lbu         $a2, 0x84($a0)
    ctx->pc = 0x2e5010u;
    SET_GPR_U32(ctx, 6, (uint8_t)READ8(ADD32(GPR_U32(ctx, 4), 132)));
    // 0x2e5014: 0x24030002  addiu       $v1, $zero, 0x2
    ctx->pc = 0x2e5014u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 2));
    // 0x2e5018: 0x14c30005  bne         $a2, $v1, . + 4 + (0x5 << 2)
    ctx->pc = 0x2E5018u;
    {
        const bool branch_taken_0x2e5018 = (GPR_U64(ctx, 6) != GPR_U64(ctx, 3));
        ctx->pc = 0x2E501Cu;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x2E5018u;
        // 0x2e501c: 0x24030003  addiu       $v1, $zero, 0x3 (Delay Slot)
        SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 3));
        ctx->in_delay_slot = false;
        if (branch_taken_0x2e5018) {
            ctx->pc = 0x2E5030u;
            goto label_2e5030;
        }
    }
    ctx->pc = 0x2E5020u;
    // 0x2e5020: 0x14c30003  bne         $a2, $v1, . + 4 + (0x3 << 2)
    ctx->pc = 0x2E5020u;
    {
        const bool branch_taken_0x2e5020 = (GPR_U64(ctx, 6) != GPR_U64(ctx, 3));
        if (branch_taken_0x2e5020) {
            ctx->pc = 0x2E5030u;
            goto label_2e5030;
        }
    }
    ctx->pc = 0x2E5028u;
label_2e5028:
    // 0x2e5028: 0x10000003  b           . + 4 + (0x3 << 2)
    ctx->pc = 0x2E5028u;
    {
        const bool branch_taken_0x2e5028 = (GPR_U64(ctx, 0) == GPR_U64(ctx, 0));
        if (branch_taken_0x2e5028) {
            ctx->pc = 0x2E5038u;
            goto label_2e5038;
        }
    }
    ctx->pc = 0x2E5030u;
label_2e5030:
    // 0x2e5030: 0x1000fffd  b           . + 4 + (-0x3 << 2)
    ctx->pc = 0x2E5030u;
    {
        const bool branch_taken_0x2e5030 = (GPR_U64(ctx, 0) == GPR_U64(ctx, 0));
        ctx->pc = 0x2E5034u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x2E5030u;
        // 0x2e5034: 0xa0850084  sb          $a1, 0x84($a0) (Delay Slot)
        WRITE8(ADD32(GPR_U32(ctx, 4), 132), (uint8_t)GPR_U32(ctx, 5));
        ctx->in_delay_slot = false;
        if (branch_taken_0x2e5030) {
            ctx->pc = 0x2E5028u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_2e5028;
        }
    }
    ctx->pc = 0x2E5038u;
label_2e5038:
    // 0x2e5038: 0x3e00008  jr          $ra
    ctx->pc = 0x2E5038u;
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        #if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x2E5038u, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
        return;
        #else
        ctx->pc = jumpTarget;
        return;
        #endif
    }
    ctx->pc = 0x2E5040u;
}
