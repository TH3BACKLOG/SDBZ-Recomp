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

// Function: sub_00119F38
// Address: 0x119f38 - 0x11a280
void sub_00119F38_0x119f38(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sub_00119F38_0x119f38");
#endif

    switch (ctx->pc) {
        case 0x119fc0u: goto label_119fc0;
        case 0x11a090u: goto label_11a090;
        default: break;
    }

    ctx->pc = 0x119f38u;

    // 0x119f38: 0x517c2  srl         $v0, $a1, 31
    ctx->pc = 0x119f38u;
    SET_GPR_S32(ctx, 2, (int32_t)SRL32(GPR_U32(ctx, 5), 31));
    // 0x119f3c: 0x27bdff90  addiu       $sp, $sp, -0x70
    ctx->pc = 0x119f3cu;
    SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 4294967184));
    // 0x119f40: 0xa21021  addu        $v0, $a1, $v0
    ctx->pc = 0x119f40u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 5), GPR_U32(ctx, 2)));
    // 0x119f44: 0x87a30078  lh          $v1, 0x78($sp)
    ctx->pc = 0x119f44u;
    SET_GPR_S32(ctx, 3, (int16_t)READ16(ADD32(GPR_U32(ctx, 29), 120)));
    // 0x119f48: 0x21043  sra         $v0, $v0, 1
    ctx->pc = 0x119f48u;
    SET_GPR_S32(ctx, 2, SRA32(GPR_S32(ctx, 2), 1));
    // 0x119f4c: 0xffb20030  sd          $s2, 0x30($sp)
    ctx->pc = 0x119f4cu;
    WRITE64(ADD32(GPR_U32(ctx, 29), 48), GPR_U64(ctx, 18));
    // 0x119f50: 0xafa20018  sw          $v0, 0x18($sp)
    ctx->pc = 0x119f50u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 24), GPR_U32(ctx, 2));
    // 0x119f54: 0x80782d  daddu       $t7, $a0, $zero
    ctx->pc = 0x119f54u;
    SET_GPR_U64(ctx, 15, (uint64_t)GPR_U64(ctx, 4) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119f58: 0x87a20080  lh          $v0, 0x80($sp)
    ctx->pc = 0x119f58u;
    SET_GPR_S32(ctx, 2, (int16_t)READ16(ADD32(GPR_U32(ctx, 29), 128)));
    // 0x119f5c: 0xa5400  sll         $t2, $t2, 16
    ctx->pc = 0x119f5cu;
    SET_GPR_S32(ctx, 10, (int32_t)SLL32(GPR_U32(ctx, 10), 16));
    // 0x119f60: 0xffb30038  sd          $s3, 0x38($sp)
    ctx->pc = 0x119f60u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 56), GPR_U64(ctx, 19));
    // 0x119f64: 0xa9c03  sra         $s3, $t2, 16
    ctx->pc = 0x119f64u;
    SET_GPR_S32(ctx, 19, SRA32(GPR_S32(ctx, 10), 16));
    // 0x119f68: 0xffb00020  sd          $s0, 0x20($sp)
    ctx->pc = 0x119f68u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 32), GPR_U64(ctx, 16));
    // 0x119f6c: 0xb5c00  sll         $t3, $t3, 16
    ctx->pc = 0x119f6cu;
    SET_GPR_S32(ctx, 11, (int32_t)SLL32(GPR_U32(ctx, 11), 16));
    // 0x119f70: 0xffb10028  sd          $s1, 0x28($sp)
    ctx->pc = 0x119f70u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 40), GPR_U64(ctx, 17));
    // 0x119f74: 0xc0c82d  daddu       $t9, $a2, $zero
    ctx->pc = 0x119f74u;
    SET_GPR_U64(ctx, 25, (uint64_t)GPR_U64(ctx, 6) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119f78: 0xffb40040  sd          $s4, 0x40($sp)
    ctx->pc = 0x119f78u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 64), GPR_U64(ctx, 20));
    // 0x119f7c: 0xb9403  sra         $s2, $t3, 16
    ctx->pc = 0x119f7cu;
    SET_GPR_S32(ctx, 18, SRA32(GPR_S32(ctx, 11), 16));
    // 0x119f80: 0xffb50048  sd          $s5, 0x48($sp)
    ctx->pc = 0x119f80u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 72), GPR_U64(ctx, 21));
    // 0x119f84: 0xffb60050  sd          $s6, 0x50($sp)
    ctx->pc = 0x119f84u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 80), GPR_U64(ctx, 22));
    // 0x119f88: 0xffb70058  sd          $s7, 0x58($sp)
    ctx->pc = 0x119f88u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 88), GPR_U64(ctx, 23));
    // 0x119f8c: 0xffbe0060  sd          $fp, 0x60($sp)
    ctx->pc = 0x119f8cu;
    WRITE64(ADD32(GPR_U32(ctx, 29), 96), GPR_U64(ctx, 30));
    // 0x119f90: 0xafa50000  sw          $a1, 0x0($sp)
    ctx->pc = 0x119f90u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 0), GPR_U32(ctx, 5));
    // 0x119f94: 0xafa70004  sw          $a3, 0x4($sp)
    ctx->pc = 0x119f94u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 4), GPR_U32(ctx, 7));
    // 0x119f98: 0xafa90008  sw          $t1, 0x8($sp)
    ctx->pc = 0x119f98u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 8), GPR_U32(ctx, 9));
    // 0x119f9c: 0xafa20010  sw          $v0, 0x10($sp)
    ctx->pc = 0x119f9cu;
    WRITE32(ADD32(GPR_U32(ctx, 29), 16), GPR_U32(ctx, 2));
    // 0x119fa0: 0xafa00014  sw          $zero, 0x14($sp)
    ctx->pc = 0x119fa0u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 20), GPR_U32(ctx, 0));
    // 0x119fa4: 0xafa3000c  sw          $v1, 0xC($sp)
    ctx->pc = 0x119fa4u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 12), GPR_U32(ctx, 3));
    // 0x119fa8: 0x8fa40018  lw          $a0, 0x18($sp)
    ctx->pc = 0x119fa8u;
    SET_GPR_S32(ctx, 4, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 24)));
    // 0x119fac: 0x84ed0000  lh          $t5, 0x0($a3)
    ctx->pc = 0x119facu;
    SET_GPR_S32(ctx, 13, (int16_t)READ16(ADD32(GPR_U32(ctx, 7), 0)));
    // 0x119fb0: 0x84ea0002  lh          $t2, 0x2($a3)
    ctx->pc = 0x119fb0u;
    SET_GPR_S32(ctx, 10, (int16_t)READ16(ADD32(GPR_U32(ctx, 7), 2)));
    // 0x119fb4: 0x852e0000  lh          $t6, 0x0($t1)
    ctx->pc = 0x119fb4u;
    SET_GPR_S32(ctx, 14, (int16_t)READ16(ADD32(GPR_U32(ctx, 9), 0)));
    // 0x119fb8: 0x1880009f  blez        $a0, . + 4 + (0x9F << 2)
    ctx->pc = 0x119FB8u;
    {
        const bool branch_taken_0x119fb8 = (GPR_S32(ctx, 4) <= 0);
        ctx->pc = 0x119FBCu;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119FB8u;
        // 0x119fbc: 0x85300002  lh          $s0, 0x2($t1) (Delay Slot)
        SET_GPR_S32(ctx, 16, (int16_t)READ16(ADD32(GPR_U32(ctx, 9), 2)));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119fb8) {
            ctx->pc = 0x11A238u;
            goto label_11a238;
        }
    }
    ctx->pc = 0x119FC0u;
label_119fc0:
    // 0x119fc0: 0x95e30000  lhu         $v1, 0x0($t7)
    ctx->pc = 0x119fc0u;
    SET_GPR_U32(ctx, 3, (uint16_t)READ16(ADD32(GPR_U32(ctx, 15), 0)));
    // 0x119fc4: 0x2406ff00  addiu       $a2, $zero, -0x100
    ctx->pc = 0x119fc4u;
    SET_GPR_S32(ctx, 6, (int32_t)ADD32(GPR_U32(ctx, 0), 4294967040));
    // 0x119fc8: 0x91e40001  lbu         $a0, 0x1($t7)
    ctx->pc = 0x119fc8u;
    SET_GPR_U32(ctx, 4, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 1)));
    // 0x119fcc: 0x31a00  sll         $v1, $v1, 8
    ctx->pc = 0x119fccu;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 8));
    // 0x119fd0: 0x8fa50014  lw          $a1, 0x14($sp)
    ctx->pc = 0x119fd0u;
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 20)));
    // 0x119fd4: 0x661824  and         $v1, $v1, $a2
    ctx->pc = 0x119fd4u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & GPR_U64(ctx, 6));
    // 0x119fd8: 0x832025  or          $a0, $a0, $v1
    ctx->pc = 0x119fd8u;
    SET_GPR_U64(ctx, 4, GPR_U64(ctx, 4) | GPR_U64(ctx, 3));
    // 0x119fdc: 0x51040  sll         $v0, $a1, 1
    ctx->pc = 0x119fdcu;
    SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 5), 1));
    // 0x119fe0: 0x42400  sll         $a0, $a0, 16
    ctx->pc = 0x119fe0u;
    SET_GPR_S32(ctx, 4, (int32_t)SLL32(GPR_U32(ctx, 4), 16));
    // 0x119fe4: 0x42c03  sra         $a1, $a0, 16
    ctx->pc = 0x119fe4u;
    SET_GPR_S32(ctx, 5, SRA32(GPR_S32(ctx, 4), 16));
    // 0x119fe8: 0x30a38000  andi        $v1, $a1, 0x8000
    ctx->pc = 0x119fe8u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) & (uint64_t)(uint16_t)32768);
    // 0x119fec: 0x14600099  bnez        $v1, . + 4 + (0x99 << 2)
    ctx->pc = 0x119FECu;
    {
        const bool branch_taken_0x119fec = (GPR_U64(ctx, 3) != GPR_U64(ctx, 0));
        ctx->pc = 0x119FF0u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119FECu;
        // 0x119ff0: 0x8fa40014  lw          $a0, 0x14($sp) (Delay Slot)
        SET_GPR_S32(ctx, 4, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 20)));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119fec) {
            ctx->pc = 0x11A254u;
            goto label_11a254;
        }
    }
    ctx->pc = 0x119FF4u;
    // 0x119ff4: 0x8fa20070  lw          $v0, 0x70($sp)
    ctx->pc = 0x119ff4u;
    SET_GPR_S32(ctx, 2, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 112)));
    // 0x119ff8: 0x8fa60010  lw          $a2, 0x10($sp)
    ctx->pc = 0x119ff8u;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 16)));
    // 0x119ffc: 0x94430000  lhu         $v1, 0x0($v0)
    ctx->pc = 0x119ffcu;
    SET_GPR_U32(ctx, 3, (uint16_t)READ16(ADD32(GPR_U32(ctx, 2), 0)));
    // 0x11a000: 0xc00013  mtlo        $a2
    ctx->pc = 0x11a000u;
    ctx->lo = GPR_U64(ctx, 6);
    // 0x11a004: 0x8fa6000c  lw          $a2, 0xC($sp)
    ctx->pc = 0x11a004u;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 12)));
    // 0x11a008: 0x41040  sll         $v0, $a0, 1
    ctx->pc = 0x11a008u;
    SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 4), 1));
    // 0x11a00c: 0x70662000  madd        $a0, $v1, $a2
    ctx->pc = 0x11a00cu;
    { uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); int64_t prod = (int64_t)GPR_S32(ctx, 3) * (int64_t)GPR_S32(ctx, 6); int64_t result = acc + prod; ctx->lo = Ps2SignExt32ToU64((uint32_t)result); ctx->hi = Ps2SignExt32ToU64((uint32_t)(result >> 32)); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x11a010: 0xa31826  xor         $v1, $a1, $v1
    ctx->pc = 0x11a010u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) ^ GPR_U64(ctx, 3));
    // 0x11a014: 0x30631fff  andi        $v1, $v1, 0x1FFF
    ctx->pc = 0x11a014u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & (uint64_t)(uint16_t)8191);
    // 0x11a018: 0x2405ff00  addiu       $a1, $zero, -0x100
    ctx->pc = 0x11a018u;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 0), 4294967040));
    // 0x11a01c: 0x24770001  addiu       $s7, $v1, 0x1
    ctx->pc = 0x11a01cu;
    SET_GPR_S32(ctx, 23, (int32_t)ADD32(GPR_U32(ctx, 3), 1));
    // 0x11a020: 0x8fa30070  lw          $v1, 0x70($sp)
    ctx->pc = 0x11a020u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 112)));
    // 0x11a024: 0x30867fff  andi        $a2, $a0, 0x7FFF
    ctx->pc = 0x11a024u;
    SET_GPR_U64(ctx, 6, GPR_U64(ctx, 4) & (uint64_t)(uint16_t)32767);
    // 0x11a028: 0xa4660000  sh          $a2, 0x0($v1)
    ctx->pc = 0x11a028u;
    WRITE16(ADD32(GPR_U32(ctx, 3), 0), (uint16_t)GPR_U32(ctx, 6));
    // 0x11a02c: 0x95e30012  lhu         $v1, 0x12($t7)
    ctx->pc = 0x11a02cu;
    SET_GPR_U32(ctx, 3, (uint16_t)READ16(ADD32(GPR_U32(ctx, 15), 18)));
    // 0x11a030: 0x91e40013  lbu         $a0, 0x13($t7)
    ctx->pc = 0x11a030u;
    SET_GPR_U32(ctx, 4, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 19)));
    // 0x11a034: 0x31a00  sll         $v1, $v1, 8
    ctx->pc = 0x11a034u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 8));
    // 0x11a038: 0x651824  and         $v1, $v1, $a1
    ctx->pc = 0x11a038u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & GPR_U64(ctx, 5));
    // 0x11a03c: 0x832025  or          $a0, $a0, $v1
    ctx->pc = 0x11a03cu;
    SET_GPR_U64(ctx, 4, GPR_U64(ctx, 4) | GPR_U64(ctx, 3));
    // 0x11a040: 0x42400  sll         $a0, $a0, 16
    ctx->pc = 0x11a040u;
    SET_GPR_S32(ctx, 4, (int32_t)SLL32(GPR_U32(ctx, 4), 16));
    // 0x11a044: 0x42c03  sra         $a1, $a0, 16
    ctx->pc = 0x11a044u;
    SET_GPR_S32(ctx, 5, SRA32(GPR_S32(ctx, 4), 16));
    // 0x11a048: 0x30a38000  andi        $v1, $a1, 0x8000
    ctx->pc = 0x11a048u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) & (uint64_t)(uint16_t)32768);
    // 0x11a04c: 0x14600081  bnez        $v1, . + 4 + (0x81 << 2)
    ctx->pc = 0x11A04Cu;
    {
        const bool branch_taken_0x11a04c = (GPR_U64(ctx, 3) != GPR_U64(ctx, 0));
        ctx->pc = 0x11A050u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A04Cu;
        // 0x11a050: 0x8fa30010  lw          $v1, 0x10($sp) (Delay Slot)
        SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 16)));
        ctx->in_delay_slot = false;
        if (branch_taken_0x11a04c) {
            ctx->pc = 0x11A254u;
            goto label_11a254;
        }
    }
    ctx->pc = 0x11A054u;
    // 0x11a054: 0x25ef0002  addiu       $t7, $t7, 0x2
    ctx->pc = 0x11a054u;
    SET_GPR_S32(ctx, 15, (int32_t)ADD32(GPR_U32(ctx, 15), 2));
    // 0x11a058: 0x8fa40070  lw          $a0, 0x70($sp)
    ctx->pc = 0x11a058u;
    SET_GPR_S32(ctx, 4, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 112)));
    // 0x11a05c: 0x34168000  ori         $s6, $zero, 0x8000
    ctx->pc = 0x11a05cu;
    SET_GPR_U64(ctx, 22, GPR_U64(ctx, 0) | (uint64_t)(uint16_t)32768);
    // 0x11a060: 0x600013  mtlo        $v1
    ctx->pc = 0x11a060u;
    ctx->lo = GPR_U64(ctx, 3);
    // 0x11a064: 0x8fa3000c  lw          $v1, 0xC($sp)
    ctx->pc = 0x11a064u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 12)));
    // 0x11a068: 0x3415ffff  ori         $s5, $zero, 0xFFFF
    ctx->pc = 0x11a068u;
    SET_GPR_U64(ctx, 21, GPR_U64(ctx, 0) | (uint64_t)(uint16_t)65535);
    // 0x11a06c: 0x24187fff  addiu       $t8, $zero, 0x7FFF
    ctx->pc = 0x11a06cu;
    SET_GPR_S32(ctx, 24, (int32_t)ADD32(GPR_U32(ctx, 0), 32767));
    // 0x11a070: 0x70661000  madd        $v0, $v1, $a2
    ctx->pc = 0x11a070u;
    { uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); int64_t prod = (int64_t)GPR_S32(ctx, 3) * (int64_t)GPR_S32(ctx, 6); int64_t result = acc + prod; ctx->lo = Ps2SignExt32ToU64((uint32_t)result); ctx->hi = Ps2SignExt32ToU64((uint32_t)(result >> 32)); SET_GPR_S32(ctx, 2, (int32_t)result); }
    // 0x11a074: 0xa61826  xor         $v1, $a1, $a2
    ctx->pc = 0x11a074u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) ^ GPR_U64(ctx, 6));
    // 0x11a078: 0x30631fff  andi        $v1, $v1, 0x1FFF
    ctx->pc = 0x11a078u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & (uint64_t)(uint16_t)8191);
    // 0x11a07c: 0x241e001e  addiu       $fp, $zero, 0x1E
    ctx->pc = 0x11a07cu;
    SET_GPR_S32(ctx, 30, (int32_t)ADD32(GPR_U32(ctx, 0), 30));
    // 0x11a080: 0x24740001  addiu       $s4, $v1, 0x1
    ctx->pc = 0x11a080u;
    SET_GPR_S32(ctx, 20, (int32_t)ADD32(GPR_U32(ctx, 3), 1));
    // 0x11a084: 0x30427fff  andi        $v0, $v0, 0x7FFF
    ctx->pc = 0x11a084u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) & (uint64_t)(uint16_t)32767);
    // 0x11a088: 0xa4820000  sh          $v0, 0x0($a0)
    ctx->pc = 0x11a088u;
    WRITE16(ADD32(GPR_U32(ctx, 4), 0), (uint16_t)GPR_U32(ctx, 2));
    // 0x11a08c: 0x0  nop
    ctx->pc = 0x11a08cu;
    // NOP
label_11a090:
    // 0x11a090: 0x91e30000  lbu         $v1, 0x0($t7)
    ctx->pc = 0x11a090u;
    SET_GPR_U32(ctx, 3, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 0)));
    // 0x11a094: 0x24a1018  mult        $v0, $s2, $t2
    ctx->pc = 0x11a094u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 10); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 2, (int32_t)result); }
    // 0x11a098: 0x726d2018  mult1       $a0, $s3, $t5
    ctx->pc = 0x11a098u;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 13); ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x11a09c: 0x91e60012  lbu         $a2, 0x12($t7)
    ctx->pc = 0x11a09cu;
    SET_GPR_U32(ctx, 6, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 18)));
    // 0x11a0a0: 0x31e00  sll         $v1, $v1, 24
    ctx->pc = 0x11a0a0u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 24));
    // 0x11a0a4: 0x1a0502d  daddu       $t2, $t5, $zero
    ctx->pc = 0x11a0a4u;
    SET_GPR_U64(ctx, 10, (uint64_t)GPR_U64(ctx, 13) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a0a8: 0x32f03  sra         $a1, $v1, 28
    ctx->pc = 0x11a0a8u;
    SET_GPR_S32(ctx, 5, SRA32(GPR_S32(ctx, 3), 28));
    // 0x11a0ac: 0x63600  sll         $a2, $a2, 24
    ctx->pc = 0x11a0acu;
    SET_GPR_S32(ctx, 6, (int32_t)SLL32(GPR_U32(ctx, 6), 24));
    // 0x11a0b0: 0xb72818  mult        $a1, $a1, $s7
    ctx->pc = 0x11a0b0u;
    { int64_t result = (int64_t)GPR_S32(ctx, 5) * (int64_t)GPR_S32(ctx, 23); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 5, (int32_t)result); }
    // 0x11a0b4: 0x36603  sra         $t4, $v1, 24
    ctx->pc = 0x11a0b4u;
    SET_GPR_S32(ctx, 12, SRA32(GPR_S32(ctx, 3), 24));
    // 0x11a0b8: 0x822021  addu        $a0, $a0, $v0
    ctx->pc = 0x11a0b8u;
    SET_GPR_S32(ctx, 4, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 2)));
    // 0x11a0bc: 0x3182000f  andi        $v0, $t4, 0xF
    ctx->pc = 0x11a0bcu;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 12) & (uint64_t)(uint16_t)15);
    // 0x11a0c0: 0x61f03  sra         $v1, $a2, 28
    ctx->pc = 0x11a0c0u;
    SET_GPR_S32(ctx, 3, SRA32(GPR_S32(ctx, 6), 28));
    // 0x11a0c4: 0x63603  sra         $a2, $a2, 24
    ctx->pc = 0x11a0c4u;
    SET_GPR_S32(ctx, 6, SRA32(GPR_S32(ctx, 6), 24));
    // 0x11a0c8: 0x42303  sra         $a0, $a0, 12
    ctx->pc = 0x11a0c8u;
    SET_GPR_S32(ctx, 4, SRA32(GPR_S32(ctx, 4), 12));
    // 0x11a0cc: 0x746018  mult        $t4, $v1, $s4
    ctx->pc = 0x11a0ccu;
    { int64_t result = (int64_t)GPR_S32(ctx, 3) * (int64_t)GPR_S32(ctx, 20); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 12, (int32_t)result); }
    // 0x11a0d0: 0x3c03004b  lui         $v1, 0x4B
    ctx->pc = 0x11a0d0u;
    SET_GPR_S32(ctx, 3, (int32_t)((uint32_t)75 << 16));
    // 0x11a0d4: 0x852821  addu        $a1, $a0, $a1
    ctx->pc = 0x11a0d4u;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 5)));
    // 0x11a0d8: 0x21080  sll         $v0, $v0, 2
    ctx->pc = 0x11a0d8u;
    SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 2), 2));
    // 0x11a0dc: 0x24637cd8  addiu       $v1, $v1, 0x7CD8
    ctx->pc = 0x11a0dcu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), 31960));
    // 0x11a0e0: 0x435821  addu        $t3, $v0, $v1
    ctx->pc = 0x11a0e0u;
    SET_GPR_S32(ctx, 11, (int32_t)ADD32(GPR_U32(ctx, 2), GPR_U32(ctx, 3)));
    // 0x11a0e4: 0xb61021  addu        $v0, $a1, $s6
    ctx->pc = 0x11a0e4u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 5), GPR_U32(ctx, 22)));
    // 0x11a0e8: 0x28a38000  slti        $v1, $a1, -0x8000
    ctx->pc = 0x11a0e8u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 5) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x11a0ec: 0x2a2102b  sltu        $v0, $s5, $v0
    ctx->pc = 0x11a0ecu;
    SET_GPR_U64(ctx, 2, ((uint64_t)GPR_U64(ctx, 21) < (uint64_t)GPR_U64(ctx, 2)) ? 1 : 0);
    // 0x11a0f0: 0x24a8818  mult        $s1, $s2, $t2
    ctx->pc = 0x11a0f0u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 10); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 17, (int32_t)result); }
    // 0x11a0f4: 0x25ef0001  addiu       $t7, $t7, 0x1
    ctx->pc = 0x11a0f4u;
    SET_GPR_S32(ctx, 15, (int32_t)ADD32(GPR_U32(ctx, 15), 1));
    // 0x11a0f8: 0x305202a  slt         $a0, $t8, $a1
    ctx->pc = 0x11a0f8u;
    SET_GPR_U64(ctx, 4, ((int64_t)GPR_S64(ctx, 24) < (int64_t)GPR_S64(ctx, 5)) ? 1 : 0);
    // 0x11a0fc: 0x38630000  xori        $v1, $v1, 0x0
    ctx->pc = 0x11a0fcu;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) ^ (uint64_t)(uint16_t)0);
    // 0x11a100: 0x10400004  beqz        $v0, . + 4 + (0x4 << 2)
    ctx->pc = 0x11A100u;
    {
        const bool branch_taken_0x11a100 = (GPR_U64(ctx, 2) == GPR_U64(ctx, 0));
        ctx->pc = 0x11A104u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A100u;
        // 0x11a104: 0x30c9000f  andi        $t1, $a2, 0xF (Delay Slot)
        SET_GPR_U64(ctx, 9, GPR_U64(ctx, 6) & (uint64_t)(uint16_t)15);
        ctx->in_delay_slot = false;
        if (branch_taken_0x11a100) {
            ctx->pc = 0x11A114u;
            goto label_11a114;
        }
    }
    ctx->pc = 0x11A108u;
    // 0x11a108: 0x304280b  movn        $a1, $t8, $a0
    ctx->pc = 0x11a108u;
    if (GPR_U64(ctx, 4) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 24));
    // 0x11a10c: 0x24028000  addiu       $v0, $zero, -0x8000
    ctx->pc = 0x11a10cu;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x11a110: 0x43280b  movn        $a1, $v0, $v1
    ctx->pc = 0x11a110u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 2));
label_11a114:
    // 0x11a114: 0x2502018  mult        $a0, $s2, $s0
    ctx->pc = 0x11a114u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 16); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x11a118: 0x1c0802d  daddu       $s0, $t6, $zero
    ctx->pc = 0x11a118u;
    SET_GPR_U64(ctx, 16, (uint64_t)GPR_U64(ctx, 14) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a11c: 0x726e1018  mult1       $v0, $s3, $t6
    ctx->pc = 0x11a11cu;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 14); ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 2, (int32_t)result); }
    // 0x11a120: 0xa0682d  daddu       $t5, $a1, $zero
    ctx->pc = 0x11a120u;
    SET_GPR_U64(ctx, 13, (uint64_t)GPR_U64(ctx, 5) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a124: 0x26d1818  mult        $v1, $s3, $t5
    ctx->pc = 0x11a124u;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 13); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 3, (int32_t)result); }
    // 0x11a128: 0x2503818  mult        $a3, $s2, $s0
    ctx->pc = 0x11a128u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 16); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 7, (int32_t)result); }
    // 0x11a12c: 0x1a0502d  daddu       $t2, $t5, $zero
    ctx->pc = 0x11a12cu;
    SET_GPR_U64(ctx, 10, (uint64_t)GPR_U64(ctx, 13) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a130: 0x441021  addu        $v0, $v0, $a0
    ctx->pc = 0x11a130u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 2), GPR_U32(ctx, 4)));
    // 0x11a134: 0x21303  sra         $v0, $v0, 12
    ctx->pc = 0x11a134u;
    SET_GPR_S32(ctx, 2, SRA32(GPR_S32(ctx, 2), 12));
    // 0x11a138: 0x711821  addu        $v1, $v1, $s1
    ctx->pc = 0x11a138u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 17)));
    // 0x11a13c: 0x4c2821  addu        $a1, $v0, $t4
    ctx->pc = 0x11a13cu;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 2), GPR_U32(ctx, 12)));
    // 0x11a140: 0x33303  sra         $a2, $v1, 12
    ctx->pc = 0x11a140u;
    SET_GPR_S32(ctx, 6, SRA32(GPR_S32(ctx, 3), 12));
    // 0x11a144: 0xb61821  addu        $v1, $a1, $s6
    ctx->pc = 0x11a144u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 5), GPR_U32(ctx, 22)));
    // 0x11a148: 0x28a28000  slti        $v0, $a1, -0x8000
    ctx->pc = 0x11a148u;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 5) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x11a14c: 0x2a3182b  sltu        $v1, $s5, $v1
    ctx->pc = 0x11a14cu;
    SET_GPR_U64(ctx, 3, ((uint64_t)GPR_U64(ctx, 21) < (uint64_t)GPR_U64(ctx, 3)) ? 1 : 0);
    // 0x11a150: 0x305202a  slt         $a0, $t8, $a1
    ctx->pc = 0x11a150u;
    SET_GPR_U64(ctx, 4, ((int64_t)GPR_S64(ctx, 24) < (int64_t)GPR_S64(ctx, 5)) ? 1 : 0);
    // 0x11a154: 0x10600004  beqz        $v1, . + 4 + (0x4 << 2)
    ctx->pc = 0x11A154u;
    {
        const bool branch_taken_0x11a154 = (GPR_U64(ctx, 3) == GPR_U64(ctx, 0));
        ctx->pc = 0x11A158u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A154u;
        // 0x11a158: 0x384c0000  xori        $t4, $v0, 0x0 (Delay Slot)
        SET_GPR_U64(ctx, 12, GPR_U64(ctx, 2) ^ (uint64_t)(uint16_t)0);
        ctx->in_delay_slot = false;
        if (branch_taken_0x11a154) {
            ctx->pc = 0x11A168u;
            goto label_11a168;
        }
    }
    ctx->pc = 0x11A15Cu;
    // 0x11a15c: 0x304280b  movn        $a1, $t8, $a0
    ctx->pc = 0x11a15cu;
    if (GPR_U64(ctx, 4) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 24));
    // 0x11a160: 0x24028000  addiu       $v0, $zero, -0x8000
    ctx->pc = 0x11a160u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x11a164: 0x4c280b  movn        $a1, $v0, $t4
    ctx->pc = 0x11a164u;
    if (GPR_U64(ctx, 12) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 2));
label_11a168:
    // 0x11a168: 0x8d6c0000  lw          $t4, 0x0($t3)
    ctx->pc = 0x11a168u;
    SET_GPR_S32(ctx, 12, (int32_t)READ32(ADD32(GPR_U32(ctx, 11), 0)));
    // 0x11a16c: 0xa0702d  daddu       $t6, $a1, $zero
    ctx->pc = 0x11a16cu;
    SET_GPR_U64(ctx, 14, (uint64_t)GPR_U64(ctx, 5) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a170: 0x26e1018  mult        $v0, $s3, $t6
    ctx->pc = 0x11a170u;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 14); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 2, (int32_t)result); }
    // 0x11a174: 0x3c05004b  lui         $a1, 0x4B
    ctx->pc = 0x11a174u;
    SET_GPR_S32(ctx, 5, (int32_t)((uint32_t)75 << 16));
    // 0x11a178: 0x1972018  mult        $a0, $t4, $s7
    ctx->pc = 0x11a178u;
    { int64_t result = (int64_t)GPR_S32(ctx, 12) * (int64_t)GPR_S32(ctx, 23); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x11a17c: 0x24a57cd8  addiu       $a1, $a1, 0x7CD8
    ctx->pc = 0x11a17cu;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 5), 31960));
    // 0x11a180: 0x91880  sll         $v1, $t1, 2
    ctx->pc = 0x11a180u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 9), 2));
    // 0x11a184: 0xa72d0000  sh          $t5, 0x0($t9)
    ctx->pc = 0x11a184u;
    WRITE16(ADD32(GPR_U32(ctx, 25), 0), (uint16_t)GPR_U32(ctx, 13));
    // 0x11a188: 0x651821  addu        $v1, $v1, $a1
    ctx->pc = 0x11a188u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 5)));
    // 0x11a18c: 0x1c0802d  daddu       $s0, $t6, $zero
    ctx->pc = 0x11a18cu;
    SET_GPR_U64(ctx, 16, (uint64_t)GPR_U64(ctx, 14) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a190: 0x471021  addu        $v0, $v0, $a3
    ctx->pc = 0x11a190u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 2), GPR_U32(ctx, 7)));
    // 0x11a194: 0xa50e0000  sh          $t6, 0x0($t0)
    ctx->pc = 0x11a194u;
    WRITE16(ADD32(GPR_U32(ctx, 8), 0), (uint16_t)GPR_U32(ctx, 14));
    // 0x11a198: 0xc42821  addu        $a1, $a2, $a0
    ctx->pc = 0x11a198u;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 4)));
    // 0x11a19c: 0x8c660000  lw          $a2, 0x0($v1)
    ctx->pc = 0x11a19cu;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 3), 0)));
    // 0x11a1a0: 0x26303  sra         $t4, $v0, 12
    ctx->pc = 0x11a1a0u;
    SET_GPR_S32(ctx, 12, SRA32(GPR_S32(ctx, 2), 12));
    // 0x11a1a4: 0xb61021  addu        $v0, $a1, $s6
    ctx->pc = 0x11a1a4u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 5), GPR_U32(ctx, 22)));
    // 0x11a1a8: 0x28a38000  slti        $v1, $a1, -0x8000
    ctx->pc = 0x11a1a8u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 5) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x11a1ac: 0x2a2102b  sltu        $v0, $s5, $v0
    ctx->pc = 0x11a1acu;
    SET_GPR_U64(ctx, 2, ((uint64_t)GPR_U64(ctx, 21) < (uint64_t)GPR_U64(ctx, 2)) ? 1 : 0);
    // 0x11a1b0: 0x27390002  addiu       $t9, $t9, 0x2
    ctx->pc = 0x11a1b0u;
    SET_GPR_S32(ctx, 25, (int32_t)ADD32(GPR_U32(ctx, 25), 2));
    // 0x11a1b4: 0x25080002  addiu       $t0, $t0, 0x2
    ctx->pc = 0x11a1b4u;
    SET_GPR_S32(ctx, 8, (int32_t)ADD32(GPR_U32(ctx, 8), 2));
    // 0x11a1b8: 0xd43018  mult        $a2, $a2, $s4
    ctx->pc = 0x11a1b8u;
    { int64_t result = (int64_t)GPR_S32(ctx, 6) * (int64_t)GPR_S32(ctx, 20); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 6, (int32_t)result); }
    // 0x11a1bc: 0x305202a  slt         $a0, $t8, $a1
    ctx->pc = 0x11a1bcu;
    SET_GPR_U64(ctx, 4, ((int64_t)GPR_S64(ctx, 24) < (int64_t)GPR_S64(ctx, 5)) ? 1 : 0);
    // 0x11a1c0: 0x10400004  beqz        $v0, . + 4 + (0x4 << 2)
    ctx->pc = 0x11A1C0u;
    {
        const bool branch_taken_0x11a1c0 = (GPR_U64(ctx, 2) == GPR_U64(ctx, 0));
        ctx->pc = 0x11A1C4u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A1C0u;
        // 0x11a1c4: 0x38630000  xori        $v1, $v1, 0x0 (Delay Slot)
        SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) ^ (uint64_t)(uint16_t)0);
        ctx->in_delay_slot = false;
        if (branch_taken_0x11a1c0) {
            ctx->pc = 0x11A1D4u;
            goto label_11a1d4;
        }
    }
    ctx->pc = 0x11A1C8u;
    // 0x11a1c8: 0x304280b  movn        $a1, $t8, $a0
    ctx->pc = 0x11a1c8u;
    if (GPR_U64(ctx, 4) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 24));
    // 0x11a1cc: 0x24028000  addiu       $v0, $zero, -0x8000
    ctx->pc = 0x11a1ccu;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x11a1d0: 0x43280b  movn        $a1, $v0, $v1
    ctx->pc = 0x11a1d0u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 2));
label_11a1d4:
    // 0x11a1d4: 0xa0682d  daddu       $t5, $a1, $zero
    ctx->pc = 0x11a1d4u;
    SET_GPR_U64(ctx, 13, (uint64_t)GPR_U64(ctx, 5) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a1d8: 0x1862821  addu        $a1, $t4, $a2
    ctx->pc = 0x11a1d8u;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 12), GPR_U32(ctx, 6)));
    // 0x11a1dc: 0xb61021  addu        $v0, $a1, $s6
    ctx->pc = 0x11a1dcu;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 5), GPR_U32(ctx, 22)));
    // 0x11a1e0: 0x28a38000  slti        $v1, $a1, -0x8000
    ctx->pc = 0x11a1e0u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 5) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x11a1e4: 0x2a2102b  sltu        $v0, $s5, $v0
    ctx->pc = 0x11a1e4u;
    SET_GPR_U64(ctx, 2, ((uint64_t)GPR_U64(ctx, 21) < (uint64_t)GPR_U64(ctx, 2)) ? 1 : 0);
    // 0x11a1e8: 0x305202a  slt         $a0, $t8, $a1
    ctx->pc = 0x11a1e8u;
    SET_GPR_U64(ctx, 4, ((int64_t)GPR_S64(ctx, 24) < (int64_t)GPR_S64(ctx, 5)) ? 1 : 0);
    // 0x11a1ec: 0x10400004  beqz        $v0, . + 4 + (0x4 << 2)
    ctx->pc = 0x11A1ECu;
    {
        const bool branch_taken_0x11a1ec = (GPR_U64(ctx, 2) == GPR_U64(ctx, 0));
        ctx->pc = 0x11A1F0u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A1ECu;
        // 0x11a1f0: 0x38630000  xori        $v1, $v1, 0x0 (Delay Slot)
        SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) ^ (uint64_t)(uint16_t)0);
        ctx->in_delay_slot = false;
        if (branch_taken_0x11a1ec) {
            ctx->pc = 0x11A200u;
            goto label_11a200;
        }
    }
    ctx->pc = 0x11A1F4u;
    // 0x11a1f4: 0x304280b  movn        $a1, $t8, $a0
    ctx->pc = 0x11a1f4u;
    if (GPR_U64(ctx, 4) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 24));
    // 0x11a1f8: 0x24028000  addiu       $v0, $zero, -0x8000
    ctx->pc = 0x11a1f8u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x11a1fc: 0x43280b  movn        $a1, $v0, $v1
    ctx->pc = 0x11a1fcu;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 5, GPR_U64(ctx, 2));
label_11a200:
    // 0x11a200: 0xa0702d  daddu       $t6, $a1, $zero
    ctx->pc = 0x11a200u;
    SET_GPR_U64(ctx, 14, (uint64_t)GPR_U64(ctx, 5) + (uint64_t)GPR_U64(ctx, 0));
    // 0x11a204: 0x27defffe  addiu       $fp, $fp, -0x2
    ctx->pc = 0x11a204u;
    SET_GPR_S32(ctx, 30, (int32_t)ADD32(GPR_U32(ctx, 30), 4294967294));
    // 0x11a208: 0xa72d0000  sh          $t5, 0x0($t9)
    ctx->pc = 0x11a208u;
    WRITE16(ADD32(GPR_U32(ctx, 25), 0), (uint16_t)GPR_U32(ctx, 13));
    // 0x11a20c: 0x27390002  addiu       $t9, $t9, 0x2
    ctx->pc = 0x11a20cu;
    SET_GPR_S32(ctx, 25, (int32_t)ADD32(GPR_U32(ctx, 25), 2));
    // 0x11a210: 0xa50e0000  sh          $t6, 0x0($t0)
    ctx->pc = 0x11a210u;
    WRITE16(ADD32(GPR_U32(ctx, 8), 0), (uint16_t)GPR_U32(ctx, 14));
    // 0x11a214: 0x7c1ff9e  bgez        $fp, . + 4 + (-0x62 << 2)
    ctx->pc = 0x11A214u;
    {
        const bool branch_taken_0x11a214 = (GPR_S32(ctx, 30) >= 0);
        ctx->pc = 0x11A218u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A214u;
        // 0x11a218: 0x25080002  addiu       $t0, $t0, 0x2 (Delay Slot)
        SET_GPR_S32(ctx, 8, (int32_t)ADD32(GPR_U32(ctx, 8), 2));
        ctx->in_delay_slot = false;
        if (branch_taken_0x11a214) {
            ctx->pc = 0x11A090u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_11a090;
        }
    }
    ctx->pc = 0x11A21Cu;
    // 0x11a21c: 0x8fa60014  lw          $a2, 0x14($sp)
    ctx->pc = 0x11a21cu;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 20)));
    // 0x11a220: 0x25ef0012  addiu       $t7, $t7, 0x12
    ctx->pc = 0x11a220u;
    SET_GPR_S32(ctx, 15, (int32_t)ADD32(GPR_U32(ctx, 15), 18));
    // 0x11a224: 0x8fa30018  lw          $v1, 0x18($sp)
    ctx->pc = 0x11a224u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 24)));
    // 0x11a228: 0x24c60001  addiu       $a2, $a2, 0x1
    ctx->pc = 0x11a228u;
    SET_GPR_S32(ctx, 6, (int32_t)ADD32(GPR_U32(ctx, 6), 1));
    // 0x11a22c: 0xc3102a  slt         $v0, $a2, $v1
    ctx->pc = 0x11a22cu;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 6) < (int64_t)GPR_S64(ctx, 3)) ? 1 : 0);
    // 0x11a230: 0x1440ff63  bnez        $v0, . + 4 + (-0x9D << 2)
    ctx->pc = 0x11A230u;
    {
        const bool branch_taken_0x11a230 = (GPR_U64(ctx, 2) != GPR_U64(ctx, 0));
        ctx->pc = 0x11A234u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A230u;
        // 0x11a234: 0xafa60014  sw          $a2, 0x14($sp) (Delay Slot)
        WRITE32(ADD32(GPR_U32(ctx, 29), 20), GPR_U32(ctx, 6));
        ctx->in_delay_slot = false;
        if (branch_taken_0x11a230) {
            ctx->pc = 0x119FC0u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_119fc0;
        }
    }
    ctx->pc = 0x11A238u;
label_11a238:
    // 0x11a238: 0x8fa40004  lw          $a0, 0x4($sp)
    ctx->pc = 0x11a238u;
    SET_GPR_S32(ctx, 4, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 4)));
    // 0x11a23c: 0xa48a0002  sh          $t2, 0x2($a0)
    ctx->pc = 0x11a23cu;
    WRITE16(ADD32(GPR_U32(ctx, 4), 2), (uint16_t)GPR_U32(ctx, 10));
    // 0x11a240: 0x8fa20000  lw          $v0, 0x0($sp)
    ctx->pc = 0x11a240u;
    SET_GPR_S32(ctx, 2, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 0)));
    // 0x11a244: 0xa48d0000  sh          $t5, 0x0($a0)
    ctx->pc = 0x11a244u;
    WRITE16(ADD32(GPR_U32(ctx, 4), 0), (uint16_t)GPR_U32(ctx, 13));
    // 0x11a248: 0x8fa50008  lw          $a1, 0x8($sp)
    ctx->pc = 0x11a248u;
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 8)));
    // 0x11a24c: 0xa4b00002  sh          $s0, 0x2($a1)
    ctx->pc = 0x11a24cu;
    WRITE16(ADD32(GPR_U32(ctx, 5), 2), (uint16_t)GPR_U32(ctx, 16));
    // 0x11a250: 0xa4ae0000  sh          $t6, 0x0($a1)
    ctx->pc = 0x11a250u;
    WRITE16(ADD32(GPR_U32(ctx, 5), 0), (uint16_t)GPR_U32(ctx, 14));
label_11a254:
    // 0x11a254: 0xdfb00020  ld          $s0, 0x20($sp)
    ctx->pc = 0x11a254u;
    SET_GPR_U64(ctx, 16, READ64(ADD32(GPR_U32(ctx, 29), 32)));
    // 0x11a258: 0xdfb10028  ld          $s1, 0x28($sp)
    ctx->pc = 0x11a258u;
    SET_GPR_U64(ctx, 17, READ64(ADD32(GPR_U32(ctx, 29), 40)));
    // 0x11a25c: 0xdfb20030  ld          $s2, 0x30($sp)
    ctx->pc = 0x11a25cu;
    SET_GPR_U64(ctx, 18, READ64(ADD32(GPR_U32(ctx, 29), 48)));
    // 0x11a260: 0xdfb30038  ld          $s3, 0x38($sp)
    ctx->pc = 0x11a260u;
    SET_GPR_U64(ctx, 19, READ64(ADD32(GPR_U32(ctx, 29), 56)));
    // 0x11a264: 0xdfb40040  ld          $s4, 0x40($sp)
    ctx->pc = 0x11a264u;
    SET_GPR_U64(ctx, 20, READ64(ADD32(GPR_U32(ctx, 29), 64)));
    // 0x11a268: 0xdfb50048  ld          $s5, 0x48($sp)
    ctx->pc = 0x11a268u;
    SET_GPR_U64(ctx, 21, READ64(ADD32(GPR_U32(ctx, 29), 72)));
    // 0x11a26c: 0xdfb60050  ld          $s6, 0x50($sp)
    ctx->pc = 0x11a26cu;
    SET_GPR_U64(ctx, 22, READ64(ADD32(GPR_U32(ctx, 29), 80)));
    // 0x11a270: 0xdfb70058  ld          $s7, 0x58($sp)
    ctx->pc = 0x11a270u;
    SET_GPR_U64(ctx, 23, READ64(ADD32(GPR_U32(ctx, 29), 88)));
    // 0x11a274: 0xdfbe0060  ld          $fp, 0x60($sp)
    ctx->pc = 0x11a274u;
    SET_GPR_U64(ctx, 30, READ64(ADD32(GPR_U32(ctx, 29), 96)));
    // 0x11a278: 0x3e00008  jr          $ra
    ctx->pc = 0x11A278u;
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = 0x11A27Cu;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x11A278u;
        // 0x11a27c: 0x27bd0070  addiu       $sp, $sp, 0x70 (Delay Slot)
        SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 112));
        ctx->in_delay_slot = false;
        ctx->pc = jumpTarget;
        #if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x11A278u, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
        return;
        #else
        ctx->pc = jumpTarget;
        return;
        #endif
    }
    ctx->pc = 0x11A280u;
}
