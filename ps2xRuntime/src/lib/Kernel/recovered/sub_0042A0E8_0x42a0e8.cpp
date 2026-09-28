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

// Function: sub_0042A0E8
// Address: 0x42a0e8 - 0x42a150
void sub_0042A0E8_0x42a0e8(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sub_0042A0E8_0x42a0e8");
#endif

    switch (ctx->pc) {
        case 0x42a110u: goto label_42a110;
        default: break;
    }

    ctx->pc = 0x42a0e8u;

    // 0x42a0e8: 0x10800006  beqz        $a0, . + 4 + (0x6 << 2)
    ctx->pc = 0x42A0E8u;
    {
        const bool branch_taken_0x42a0e8 = (GPR_U64(ctx, 4) == GPR_U64(ctx, 0));
        ctx->pc = 0x42A0ECu;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x42A0E8u;
        // 0x42a0ec: 0x182d  daddu       $v1, $zero, $zero (Delay Slot)
        SET_GPR_U64(ctx, 3, (uint64_t)GPR_U64(ctx, 0) + (uint64_t)GPR_U64(ctx, 0));
        ctx->in_delay_slot = false;
        if (branch_taken_0x42a0e8) {
            ctx->pc = 0x42A104u;
            goto label_42a104;
        }
    }
    ctx->pc = 0x42A0F0u;
    // 0x42a0f0: 0x24020001  addiu       $v0, $zero, 0x1
    ctx->pc = 0x42a0f0u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 1));
    // 0x42a0f4: 0x1082000f  beq         $a0, $v0, . + 4 + (0xF << 2)
    ctx->pc = 0x42A0F4u;
    {
        const bool branch_taken_0x42a0f4 = (GPR_U64(ctx, 4) == GPR_U64(ctx, 2));
        ctx->pc = 0x42A0F8u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x42A0F4u;
        // 0x42a0f8: 0x60102d  daddu       $v0, $v1, $zero (Delay Slot)
        SET_GPR_U64(ctx, 2, (uint64_t)GPR_U64(ctx, 3) + (uint64_t)GPR_U64(ctx, 0));
        ctx->in_delay_slot = false;
        if (branch_taken_0x42a0f4) {
            ctx->pc = 0x42A134u;
            goto label_42a134;
        }
    }
    ctx->pc = 0x42A0FCu;
    // 0x42a0fc: 0x10000012  b           . + 4 + (0x12 << 2)
    ctx->pc = 0x42A0FCu;
    {
        const bool branch_taken_0x42a0fc = (GPR_U64(ctx, 0) == GPR_U64(ctx, 0));
        if (branch_taken_0x42a0fc) {
            ctx->pc = 0x42A148u;
            goto label_42a148;
        }
    }
    ctx->pc = 0x42A104u;
label_42a104:
    // 0x42a104: 0x3c031000  lui         $v1, 0x1000
    ctx->pc = 0x42a104u;
    SET_GPR_S32(ctx, 3, (int32_t)((uint32_t)4096 << 16));
    // 0x42a108: 0x34632010  ori         $v1, $v1, 0x2010
    ctx->pc = 0x42a108u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) | (uint64_t)(uint16_t)8208);
    // 0x42a10c: 0x0  nop
    ctx->pc = 0x42a10cu;
    // NOP
label_42a110:
    // 0x42a110: 0x8c620000  lw          $v0, 0x0($v1)
    ctx->pc = 0x42a110u;
    SET_GPR_S32(ctx, 2, (int32_t)READ32(ADD32(GPR_U32(ctx, 3), 0)));
    // 0x42a114: 0x0  nop
    ctx->pc = 0x42a114u;
    // NOP
    // 0x42a118: 0x0  nop
    ctx->pc = 0x42a118u;
    // NOP
    // 0x42a11c: 0x0  nop
    ctx->pc = 0x42a11cu;
    // NOP
    // 0x42a120: 0x0  nop
    ctx->pc = 0x42a120u;
    // NOP
    // 0x42a124: 0x440fffa  bltz        $v0, . + 4 + (-0x6 << 2)
    ctx->pc = 0x42A124u;
    {
        const bool branch_taken_0x42a124 = (GPR_S32(ctx, 2) < 0);
        if (branch_taken_0x42a124) {
            ctx->pc = 0x42A110u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_42a110;
        }
    }
    ctx->pc = 0x42A12Cu;
    // 0x42a12c: 0x10000005  b           . + 4 + (0x5 << 2)
    ctx->pc = 0x42A12Cu;
    {
        const bool branch_taken_0x42a12c = (GPR_U64(ctx, 0) == GPR_U64(ctx, 0));
        ctx->pc = 0x42A130u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x42A12Cu;
        // 0x42a130: 0x182d  daddu       $v1, $zero, $zero (Delay Slot)
        SET_GPR_U64(ctx, 3, (uint64_t)GPR_U64(ctx, 0) + (uint64_t)GPR_U64(ctx, 0));
        ctx->in_delay_slot = false;
        if (branch_taken_0x42a12c) {
            ctx->pc = 0x42A144u;
            goto label_42a144;
        }
    }
    ctx->pc = 0x42A134u;
label_42a134:
    // 0x42a134: 0x3c021000  lui         $v0, 0x1000
    ctx->pc = 0x42a134u;
    SET_GPR_S32(ctx, 2, (int32_t)((uint32_t)4096 << 16));
    // 0x42a138: 0x34422010  ori         $v0, $v0, 0x2010
    ctx->pc = 0x42a138u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) | (uint64_t)(uint16_t)8208);
    // 0x42a13c: 0x8c430000  lw          $v1, 0x0($v0)
    ctx->pc = 0x42a13cu;
    SET_GPR_S32(ctx, 3, (int32_t)runtime->Load32(rdram, ctx, 0x10002010u));
    // 0x42a140: 0x31fc2  srl         $v1, $v1, 31
    ctx->pc = 0x42a140u;
    SET_GPR_S32(ctx, 3, (int32_t)SRL32(GPR_U32(ctx, 3), 31));
label_42a144:
    // 0x42a144: 0x60102d  daddu       $v0, $v1, $zero
    ctx->pc = 0x42a144u;
    SET_GPR_U64(ctx, 2, (uint64_t)GPR_U64(ctx, 3) + (uint64_t)GPR_U64(ctx, 0));
label_42a148:
    // 0x42a148: 0x3e00008  jr          $ra
    ctx->pc = 0x42A148u;
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = jumpTarget;
        #if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x42A148u, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
        return;
        #else
        ctx->pc = jumpTarget;
        return;
        #endif
    }
    ctx->pc = 0x42A150u;
}
