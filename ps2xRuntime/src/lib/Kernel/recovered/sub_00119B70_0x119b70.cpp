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

// Function: sub_00119B70
// Address: 0x119b70 - 0x119f38
void sub_00119B70_0x119b70(uint8_t* rdram, R5900Context* ctx, PS2Runtime *runtime) {
#ifdef PS2_FUNCTION_LOG_TRACKER
    PS_LOG_ENTRY("sub_00119B70_0x119b70");
#endif

    switch (ctx->pc) {
        case 0x119c00u: goto label_119c00;
        case 0x119cd0u: goto label_119cd0;
        default: break;
    }

    ctx->pc = 0x119b70u;

    // 0x119b70: 0x517c2  srl         $v0, $a1, 31
    ctx->pc = 0x119b70u;
    SET_GPR_S32(ctx, 2, (int32_t)SRL32(GPR_U32(ctx, 5), 31));
    // 0x119b74: 0x27bdff90  addiu       $sp, $sp, -0x70
    ctx->pc = 0x119b74u;
    SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 4294967184));
    // 0x119b78: 0xa21021  addu        $v0, $a1, $v0
    ctx->pc = 0x119b78u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 5), GPR_U32(ctx, 2)));
    // 0x119b7c: 0xafa90008  sw          $t1, 0x8($sp)
    ctx->pc = 0x119b7cu;
    WRITE32(ADD32(GPR_U32(ctx, 29), 8), GPR_U32(ctx, 9));
    // 0x119b80: 0x21043  sra         $v0, $v0, 1
    ctx->pc = 0x119b80u;
    SET_GPR_S32(ctx, 2, SRA32(GPR_S32(ctx, 2), 1));
    // 0x119b84: 0x87a30078  lh          $v1, 0x78($sp)
    ctx->pc = 0x119b84u;
    SET_GPR_S32(ctx, 3, (int16_t)READ16(ADD32(GPR_U32(ctx, 29), 120)));
    // 0x119b88: 0xafa20014  sw          $v0, 0x14($sp)
    ctx->pc = 0x119b88u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 20), GPR_U32(ctx, 2));
    // 0x119b8c: 0xc0482d  daddu       $t1, $a2, $zero
    ctx->pc = 0x119b8cu;
    SET_GPR_U64(ctx, 9, (uint64_t)GPR_U64(ctx, 6) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119b90: 0x87a20080  lh          $v0, 0x80($sp)
    ctx->pc = 0x119b90u;
    SET_GPR_S32(ctx, 2, (int16_t)READ16(ADD32(GPR_U32(ctx, 29), 128)));
    // 0x119b94: 0xa5400  sll         $t2, $t2, 16
    ctx->pc = 0x119b94u;
    SET_GPR_S32(ctx, 10, (int32_t)SLL32(GPR_U32(ctx, 10), 16));
    // 0x119b98: 0xffb20030  sd          $s2, 0x30($sp)
    ctx->pc = 0x119b98u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 48), GPR_U64(ctx, 18));
    // 0x119b9c: 0xb5c00  sll         $t3, $t3, 16
    ctx->pc = 0x119b9cu;
    SET_GPR_S32(ctx, 11, (int32_t)SLL32(GPR_U32(ctx, 11), 16));
    // 0x119ba0: 0xffb30038  sd          $s3, 0x38($sp)
    ctx->pc = 0x119ba0u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 56), GPR_U64(ctx, 19));
    // 0x119ba4: 0xa9c03  sra         $s3, $t2, 16
    ctx->pc = 0x119ba4u;
    SET_GPR_S32(ctx, 19, SRA32(GPR_S32(ctx, 10), 16));
    // 0x119ba8: 0xffb70058  sd          $s7, 0x58($sp)
    ctx->pc = 0x119ba8u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 88), GPR_U64(ctx, 23));
    // 0x119bac: 0xb82d  daddu       $s7, $zero, $zero
    ctx->pc = 0x119bacu;
    SET_GPR_U64(ctx, 23, (uint64_t)GPR_U64(ctx, 0) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119bb0: 0xafa50000  sw          $a1, 0x0($sp)
    ctx->pc = 0x119bb0u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 0), GPR_U32(ctx, 5));
    // 0x119bb4: 0xb9403  sra         $s2, $t3, 16
    ctx->pc = 0x119bb4u;
    SET_GPR_S32(ctx, 18, SRA32(GPR_S32(ctx, 11), 16));
    // 0x119bb8: 0xffb00020  sd          $s0, 0x20($sp)
    ctx->pc = 0x119bb8u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 32), GPR_U64(ctx, 16));
    // 0x119bbc: 0x80782d  daddu       $t7, $a0, $zero
    ctx->pc = 0x119bbcu;
    SET_GPR_U64(ctx, 15, (uint64_t)GPR_U64(ctx, 4) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119bc0: 0xffb10028  sd          $s1, 0x28($sp)
    ctx->pc = 0x119bc0u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 40), GPR_U64(ctx, 17));
    // 0x119bc4: 0xffb40040  sd          $s4, 0x40($sp)
    ctx->pc = 0x119bc4u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 64), GPR_U64(ctx, 20));
    // 0x119bc8: 0xffb50048  sd          $s5, 0x48($sp)
    ctx->pc = 0x119bc8u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 72), GPR_U64(ctx, 21));
    // 0x119bcc: 0xffb60050  sd          $s6, 0x50($sp)
    ctx->pc = 0x119bccu;
    WRITE64(ADD32(GPR_U32(ctx, 29), 80), GPR_U64(ctx, 22));
    // 0x119bd0: 0xffbe0060  sd          $fp, 0x60($sp)
    ctx->pc = 0x119bd0u;
    WRITE64(ADD32(GPR_U32(ctx, 29), 96), GPR_U64(ctx, 30));
    // 0x119bd4: 0xafa70004  sw          $a3, 0x4($sp)
    ctx->pc = 0x119bd4u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 4), GPR_U32(ctx, 7));
    // 0x119bd8: 0xafa20010  sw          $v0, 0x10($sp)
    ctx->pc = 0x119bd8u;
    WRITE32(ADD32(GPR_U32(ctx, 29), 16), GPR_U32(ctx, 2));
    // 0x119bdc: 0xafa3000c  sw          $v1, 0xC($sp)
    ctx->pc = 0x119bdcu;
    WRITE32(ADD32(GPR_U32(ctx, 29), 12), GPR_U32(ctx, 3));
    // 0x119be0: 0x8fa50008  lw          $a1, 0x8($sp)
    ctx->pc = 0x119be0u;
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 8)));
    // 0x119be4: 0x8fa60014  lw          $a2, 0x14($sp)
    ctx->pc = 0x119be4u;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 20)));
    // 0x119be8: 0x84f80000  lh          $t8, 0x0($a3)
    ctx->pc = 0x119be8u;
    SET_GPR_S32(ctx, 24, (int16_t)READ16(ADD32(GPR_U32(ctx, 7), 0)));
    // 0x119bec: 0x84ec0002  lh          $t4, 0x2($a3)
    ctx->pc = 0x119becu;
    SET_GPR_S32(ctx, 12, (int16_t)READ16(ADD32(GPR_U32(ctx, 7), 2)));
    // 0x119bf0: 0x84b90000  lh          $t9, 0x0($a1)
    ctx->pc = 0x119bf0u;
    SET_GPR_S32(ctx, 25, (int16_t)READ16(ADD32(GPR_U32(ctx, 5), 0)));
    // 0x119bf4: 0x18c000bd  blez        $a2, . + 4 + (0xBD << 2)
    ctx->pc = 0x119BF4u;
    {
        const bool branch_taken_0x119bf4 = (GPR_S32(ctx, 6) <= 0);
        ctx->pc = 0x119BF8u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119BF4u;
        // 0x119bf8: 0x84b10002  lh          $s1, 0x2($a1) (Delay Slot)
        SET_GPR_S32(ctx, 17, (int16_t)READ16(ADD32(GPR_U32(ctx, 5), 2)));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119bf4) {
            ctx->pc = 0x119EECu;
            goto label_119eec;
        }
    }
    ctx->pc = 0x119BFCu;
    // 0x119bfc: 0x0  nop
    ctx->pc = 0x119bfcu;
    // NOP
label_119c00:
    // 0x119c00: 0x95e30000  lhu         $v1, 0x0($t7)
    ctx->pc = 0x119c00u;
    SET_GPR_U32(ctx, 3, (uint16_t)READ16(ADD32(GPR_U32(ctx, 15), 0)));
    // 0x119c04: 0x2405ff00  addiu       $a1, $zero, -0x100
    ctx->pc = 0x119c04u;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 0), 4294967040));
    // 0x119c08: 0x91e40001  lbu         $a0, 0x1($t7)
    ctx->pc = 0x119c08u;
    SET_GPR_U32(ctx, 4, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 1)));
    // 0x119c0c: 0x31a00  sll         $v1, $v1, 8
    ctx->pc = 0x119c0cu;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 8));
    // 0x119c10: 0x651824  and         $v1, $v1, $a1
    ctx->pc = 0x119c10u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & GPR_U64(ctx, 5));
    // 0x119c14: 0x832025  or          $a0, $a0, $v1
    ctx->pc = 0x119c14u;
    SET_GPR_U64(ctx, 4, GPR_U64(ctx, 4) | GPR_U64(ctx, 3));
    // 0x119c18: 0x42400  sll         $a0, $a0, 16
    ctx->pc = 0x119c18u;
    SET_GPR_S32(ctx, 4, (int32_t)SLL32(GPR_U32(ctx, 4), 16));
    // 0x119c1c: 0x42c03  sra         $a1, $a0, 16
    ctx->pc = 0x119c1cu;
    SET_GPR_S32(ctx, 5, SRA32(GPR_S32(ctx, 4), 16));
    // 0x119c20: 0x30a38000  andi        $v1, $a1, 0x8000
    ctx->pc = 0x119c20u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) & (uint64_t)(uint16_t)32768);
    // 0x119c24: 0x146000b8  bnez        $v1, . + 4 + (0xB8 << 2)
    ctx->pc = 0x119C24u;
    {
        const bool branch_taken_0x119c24 = (GPR_U64(ctx, 3) != GPR_U64(ctx, 0));
        ctx->pc = 0x119C28u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119C24u;
        // 0x119c28: 0x171040  sll         $v0, $s7, 1 (Delay Slot)
        SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 23), 1));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119c24) {
            ctx->pc = 0x119F08u;
            goto label_119f08;
        }
    }
    ctx->pc = 0x119C2Cu;
    // 0x119c2c: 0x8fa60070  lw          $a2, 0x70($sp)
    ctx->pc = 0x119c2cu;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 112)));
    // 0x119c30: 0x94c30000  lhu         $v1, 0x0($a2)
    ctx->pc = 0x119c30u;
    SET_GPR_U32(ctx, 3, (uint16_t)READ16(ADD32(GPR_U32(ctx, 6), 0)));
    // 0x119c34: 0x8fa60010  lw          $a2, 0x10($sp)
    ctx->pc = 0x119c34u;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 16)));
    // 0x119c38: 0xc00013  mtlo        $a2
    ctx->pc = 0x119c38u;
    ctx->lo = GPR_U64(ctx, 6);
    // 0x119c3c: 0x8fa6000c  lw          $a2, 0xC($sp)
    ctx->pc = 0x119c3cu;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 12)));
    // 0x119c40: 0x70662000  madd        $a0, $v1, $a2
    ctx->pc = 0x119c40u;
    { uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); int64_t prod = (int64_t)GPR_S32(ctx, 3) * (int64_t)GPR_S32(ctx, 6); int64_t result = acc + prod; ctx->lo = Ps2SignExt32ToU64((uint32_t)result); ctx->hi = Ps2SignExt32ToU64((uint32_t)(result >> 32)); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x119c44: 0xa31826  xor         $v1, $a1, $v1
    ctx->pc = 0x119c44u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) ^ GPR_U64(ctx, 3));
    // 0x119c48: 0x30631fff  andi        $v1, $v1, 0x1FFF
    ctx->pc = 0x119c48u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & (uint64_t)(uint16_t)8191);
    // 0x119c4c: 0x2405ff00  addiu       $a1, $zero, -0x100
    ctx->pc = 0x119c4cu;
    SET_GPR_S32(ctx, 5, (int32_t)ADD32(GPR_U32(ctx, 0), 4294967040));
    // 0x119c50: 0x24750001  addiu       $s5, $v1, 0x1
    ctx->pc = 0x119c50u;
    SET_GPR_S32(ctx, 21, (int32_t)ADD32(GPR_U32(ctx, 3), 1));
    // 0x119c54: 0x8fa30070  lw          $v1, 0x70($sp)
    ctx->pc = 0x119c54u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 112)));
    // 0x119c58: 0x30867fff  andi        $a2, $a0, 0x7FFF
    ctx->pc = 0x119c58u;
    SET_GPR_U64(ctx, 6, GPR_U64(ctx, 4) & (uint64_t)(uint16_t)32767);
    // 0x119c5c: 0xa4660000  sh          $a2, 0x0($v1)
    ctx->pc = 0x119c5cu;
    WRITE16(ADD32(GPR_U32(ctx, 3), 0), (uint16_t)GPR_U32(ctx, 6));
    // 0x119c60: 0x95e30012  lhu         $v1, 0x12($t7)
    ctx->pc = 0x119c60u;
    SET_GPR_U32(ctx, 3, (uint16_t)READ16(ADD32(GPR_U32(ctx, 15), 18)));
    // 0x119c64: 0x91e40013  lbu         $a0, 0x13($t7)
    ctx->pc = 0x119c64u;
    SET_GPR_U32(ctx, 4, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 19)));
    // 0x119c68: 0x31a00  sll         $v1, $v1, 8
    ctx->pc = 0x119c68u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 8));
    // 0x119c6c: 0x651824  and         $v1, $v1, $a1
    ctx->pc = 0x119c6cu;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & GPR_U64(ctx, 5));
    // 0x119c70: 0x832025  or          $a0, $a0, $v1
    ctx->pc = 0x119c70u;
    SET_GPR_U64(ctx, 4, GPR_U64(ctx, 4) | GPR_U64(ctx, 3));
    // 0x119c74: 0x42400  sll         $a0, $a0, 16
    ctx->pc = 0x119c74u;
    SET_GPR_S32(ctx, 4, (int32_t)SLL32(GPR_U32(ctx, 4), 16));
    // 0x119c78: 0x42c03  sra         $a1, $a0, 16
    ctx->pc = 0x119c78u;
    SET_GPR_S32(ctx, 5, SRA32(GPR_S32(ctx, 4), 16));
    // 0x119c7c: 0x30a38000  andi        $v1, $a1, 0x8000
    ctx->pc = 0x119c7cu;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) & (uint64_t)(uint16_t)32768);
    // 0x119c80: 0x146000a1  bnez        $v1, . + 4 + (0xA1 << 2)
    ctx->pc = 0x119C80u;
    {
        const bool branch_taken_0x119c80 = (GPR_U64(ctx, 3) != GPR_U64(ctx, 0));
        ctx->pc = 0x119C84u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119C80u;
        // 0x119c84: 0x8fa30010  lw          $v1, 0x10($sp) (Delay Slot)
        SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 16)));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119c80) {
            ctx->pc = 0x119F08u;
            goto label_119f08;
        }
    }
    ctx->pc = 0x119C88u;
    // 0x119c88: 0x3c04004b  lui         $a0, 0x4B
    ctx->pc = 0x119c88u;
    SET_GPR_S32(ctx, 4, (int32_t)((uint32_t)75 << 16));
    // 0x119c8c: 0x249e7cd8  addiu       $fp, $a0, 0x7CD8
    ctx->pc = 0x119c8cu;
    SET_GPR_S32(ctx, 30, (int32_t)ADD32(GPR_U32(ctx, 4), 31960));
    // 0x119c90: 0x25ef0002  addiu       $t7, $t7, 0x2
    ctx->pc = 0x119c90u;
    SET_GPR_S32(ctx, 15, (int32_t)ADD32(GPR_U32(ctx, 15), 2));
    // 0x119c94: 0x600013  mtlo        $v1
    ctx->pc = 0x119c94u;
    ctx->lo = GPR_U64(ctx, 3);
    // 0x119c98: 0x8fa3000c  lw          $v1, 0xC($sp)
    ctx->pc = 0x119c98u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 12)));
    // 0x119c9c: 0x34108000  ori         $s0, $zero, 0x8000
    ctx->pc = 0x119c9cu;
    SET_GPR_U64(ctx, 16, GPR_U64(ctx, 0) | (uint64_t)(uint16_t)32768);
    // 0x119ca0: 0x340bffff  ori         $t3, $zero, 0xFFFF
    ctx->pc = 0x119ca0u;
    SET_GPR_U64(ctx, 11, GPR_U64(ctx, 0) | (uint64_t)(uint16_t)65535);
    // 0x119ca4: 0x70661000  madd        $v0, $v1, $a2
    ctx->pc = 0x119ca4u;
    { uint64_t acc = Ps2HiLoToU64(ctx->hi, ctx->lo); int64_t prod = (int64_t)GPR_S32(ctx, 3) * (int64_t)GPR_S32(ctx, 6); int64_t result = acc + prod; ctx->lo = Ps2SignExt32ToU64((uint32_t)result); ctx->hi = Ps2SignExt32ToU64((uint32_t)(result >> 32)); SET_GPR_S32(ctx, 2, (int32_t)result); }
    // 0x119ca8: 0xa61826  xor         $v1, $a1, $a2
    ctx->pc = 0x119ca8u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 5) ^ GPR_U64(ctx, 6));
    // 0x119cac: 0x8fa50070  lw          $a1, 0x70($sp)
    ctx->pc = 0x119cacu;
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 112)));
    // 0x119cb0: 0x30631fff  andi        $v1, $v1, 0x1FFF
    ctx->pc = 0x119cb0u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 3) & (uint64_t)(uint16_t)8191);
    // 0x119cb4: 0x24740001  addiu       $s4, $v1, 0x1
    ctx->pc = 0x119cb4u;
    SET_GPR_S32(ctx, 20, (int32_t)ADD32(GPR_U32(ctx, 3), 1));
    // 0x119cb8: 0x240d7fff  addiu       $t5, $zero, 0x7FFF
    ctx->pc = 0x119cb8u;
    SET_GPR_S32(ctx, 13, (int32_t)ADD32(GPR_U32(ctx, 0), 32767));
    // 0x119cbc: 0x240a000a  addiu       $t2, $zero, 0xA
    ctx->pc = 0x119cbcu;
    SET_GPR_S32(ctx, 10, (int32_t)ADD32(GPR_U32(ctx, 0), 10));
    // 0x119cc0: 0x2416001e  addiu       $s6, $zero, 0x1E
    ctx->pc = 0x119cc0u;
    SET_GPR_S32(ctx, 22, (int32_t)ADD32(GPR_U32(ctx, 0), 30));
    // 0x119cc4: 0x30427fff  andi        $v0, $v0, 0x7FFF
    ctx->pc = 0x119cc4u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) & (uint64_t)(uint16_t)32767);
    // 0x119cc8: 0xa4a20000  sh          $v0, 0x0($a1)
    ctx->pc = 0x119cc8u;
    WRITE16(ADD32(GPR_U32(ctx, 5), 0), (uint16_t)GPR_U32(ctx, 2));
    // 0x119ccc: 0x0  nop
    ctx->pc = 0x119cccu;
    // NOP
label_119cd0:
    // 0x119cd0: 0x91e20000  lbu         $v0, 0x0($t7)
    ctx->pc = 0x119cd0u;
    SET_GPR_U32(ctx, 2, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 0)));
    // 0x119cd4: 0x24c3018  mult        $a2, $s2, $t4
    ctx->pc = 0x119cd4u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 12); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 6, (int32_t)result); }
    // 0x119cd8: 0x72781818  mult1       $v1, $s3, $t8
    ctx->pc = 0x119cd8u;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 24); ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 3, (int32_t)result); }
    // 0x119cdc: 0x91e40012  lbu         $a0, 0x12($t7)
    ctx->pc = 0x119cdcu;
    SET_GPR_U32(ctx, 4, (uint8_t)READ8(ADD32(GPR_U32(ctx, 15), 18)));
    // 0x119ce0: 0x21600  sll         $v0, $v0, 24
    ctx->pc = 0x119ce0u;
    SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 2), 24));
    // 0x119ce4: 0x25ef0001  addiu       $t7, $t7, 0x1
    ctx->pc = 0x119ce4u;
    SET_GPR_S32(ctx, 15, (int32_t)ADD32(GPR_U32(ctx, 15), 1));
    // 0x119ce8: 0x22f03  sra         $a1, $v0, 28
    ctx->pc = 0x119ce8u;
    SET_GPR_S32(ctx, 5, SRA32(GPR_S32(ctx, 2), 28));
    // 0x119cec: 0x27603  sra         $t6, $v0, 24
    ctx->pc = 0x119cecu;
    SET_GPR_S32(ctx, 14, SRA32(GPR_S32(ctx, 2), 24));
    // 0x119cf0: 0xb52818  mult        $a1, $a1, $s5
    ctx->pc = 0x119cf0u;
    { int64_t result = (int64_t)GPR_S32(ctx, 5) * (int64_t)GPR_S32(ctx, 21); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 5, (int32_t)result); }
    // 0x119cf4: 0x42600  sll         $a0, $a0, 24
    ctx->pc = 0x119cf4u;
    SET_GPR_S32(ctx, 4, (int32_t)SLL32(GPR_U32(ctx, 4), 24));
    // 0x119cf8: 0x661821  addu        $v1, $v1, $a2
    ctx->pc = 0x119cf8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 6)));
    // 0x119cfc: 0x46703  sra         $t4, $a0, 28
    ctx->pc = 0x119cfcu;
    SET_GPR_S32(ctx, 12, SRA32(GPR_S32(ctx, 4), 28));
    // 0x119d00: 0x31b03  sra         $v1, $v1, 12
    ctx->pc = 0x119d00u;
    SET_GPR_S32(ctx, 3, SRA32(GPR_S32(ctx, 3), 12));
    // 0x119d04: 0x653021  addu        $a2, $v1, $a1
    ctx->pc = 0x119d04u;
    SET_GPR_S32(ctx, 6, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 5)));
    // 0x119d08: 0xd01021  addu        $v0, $a2, $s0
    ctx->pc = 0x119d08u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 16)));
    // 0x119d0c: 0x162102b  sltu        $v0, $t3, $v0
    ctx->pc = 0x119d0cu;
    SET_GPR_U64(ctx, 2, ((uint64_t)GPR_U64(ctx, 11) < (uint64_t)GPR_U64(ctx, 2)) ? 1 : 0);
    // 0x119d10: 0x10400007  beqz        $v0, . + 4 + (0x7 << 2)
    ctx->pc = 0x119D10u;
    {
        const bool branch_taken_0x119d10 = (GPR_U64(ctx, 2) == GPR_U64(ctx, 0));
        ctx->pc = 0x119D14u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119D10u;
        // 0x119d14: 0x43e03  sra         $a3, $a0, 24 (Delay Slot)
        SET_GPR_S32(ctx, 7, SRA32(GPR_S32(ctx, 4), 24));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119d10) {
            ctx->pc = 0x119D30u;
            goto label_119d30;
        }
    }
    ctx->pc = 0x119D18u;
    // 0x119d18: 0x28c28000  slti        $v0, $a2, -0x8000
    ctx->pc = 0x119d18u;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 6) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x119d1c: 0x1a6182a  slt         $v1, $t5, $a2
    ctx->pc = 0x119d1cu;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 13) < (int64_t)GPR_S64(ctx, 6)) ? 1 : 0);
    // 0x119d20: 0x1a3300b  movn        $a2, $t5, $v1
    ctx->pc = 0x119d20u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 13));
    // 0x119d24: 0x24038000  addiu       $v1, $zero, -0x8000
    ctx->pc = 0x119d24u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x119d28: 0x38420000  xori        $v0, $v0, 0x0
    ctx->pc = 0x119d28u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) ^ (uint64_t)(uint16_t)0);
    // 0x119d2c: 0x62300b  movn        $a2, $v1, $v0
    ctx->pc = 0x119d2cu;
    if (GPR_U64(ctx, 2) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 3));
label_119d30:
    // 0x119d30: 0x2511018  mult        $v0, $s2, $s1
    ctx->pc = 0x119d30u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 17); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 2, (int32_t)result); }
    // 0x119d34: 0x72791818  mult1       $v1, $s3, $t9
    ctx->pc = 0x119d34u;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 25); ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 3, (int32_t)result); }
    // 0x119d38: 0x1942018  mult        $a0, $t4, $s4
    ctx->pc = 0x119d38u;
    { int64_t result = (int64_t)GPR_S32(ctx, 12) * (int64_t)GPR_S32(ctx, 20); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x119d3c: 0x300602d  daddu       $t4, $t8, $zero
    ctx->pc = 0x119d3cu;
    SET_GPR_U64(ctx, 12, (uint64_t)GPR_U64(ctx, 24) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119d40: 0xc0c02d  daddu       $t8, $a2, $zero
    ctx->pc = 0x119d40u;
    SET_GPR_U64(ctx, 24, (uint64_t)GPR_U64(ctx, 6) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119d44: 0x621821  addu        $v1, $v1, $v0
    ctx->pc = 0x119d44u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 2)));
    // 0x119d48: 0x31b03  sra         $v1, $v1, 12
    ctx->pc = 0x119d48u;
    SET_GPR_S32(ctx, 3, SRA32(GPR_S32(ctx, 3), 12));
    // 0x119d4c: 0x643021  addu        $a2, $v1, $a0
    ctx->pc = 0x119d4cu;
    SET_GPR_S32(ctx, 6, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 4)));
    // 0x119d50: 0xd01021  addu        $v0, $a2, $s0
    ctx->pc = 0x119d50u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 16)));
    // 0x119d54: 0x162102b  sltu        $v0, $t3, $v0
    ctx->pc = 0x119d54u;
    SET_GPR_U64(ctx, 2, ((uint64_t)GPR_U64(ctx, 11) < (uint64_t)GPR_U64(ctx, 2)) ? 1 : 0);
    // 0x119d58: 0x10400007  beqz        $v0, . + 4 + (0x7 << 2)
    ctx->pc = 0x119D58u;
    {
        const bool branch_taken_0x119d58 = (GPR_U64(ctx, 2) == GPR_U64(ctx, 0));
        ctx->pc = 0x119D5Cu;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119D58u;
        // 0x119d5c: 0x320882d  daddu       $s1, $t9, $zero (Delay Slot)
        SET_GPR_U64(ctx, 17, (uint64_t)GPR_U64(ctx, 25) + (uint64_t)GPR_U64(ctx, 0));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119d58) {
            ctx->pc = 0x119D78u;
            goto label_119d78;
        }
    }
    ctx->pc = 0x119D60u;
    // 0x119d60: 0x28c28000  slti        $v0, $a2, -0x8000
    ctx->pc = 0x119d60u;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 6) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x119d64: 0x1a6182a  slt         $v1, $t5, $a2
    ctx->pc = 0x119d64u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 13) < (int64_t)GPR_S64(ctx, 6)) ? 1 : 0);
    // 0x119d68: 0x1a3300b  movn        $a2, $t5, $v1
    ctx->pc = 0x119d68u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 13));
    // 0x119d6c: 0x24038000  addiu       $v1, $zero, -0x8000
    ctx->pc = 0x119d6cu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x119d70: 0x38420000  xori        $v0, $v0, 0x0
    ctx->pc = 0x119d70u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) ^ (uint64_t)(uint16_t)0);
    // 0x119d74: 0x62300b  movn        $a2, $v1, $v0
    ctx->pc = 0x119d74u;
    if (GPR_U64(ctx, 2) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 3));
label_119d78:
    // 0x119d78: 0xc0c82d  daddu       $t9, $a2, $zero
    ctx->pc = 0x119d78u;
    SET_GPR_U64(ctx, 25, (uint64_t)GPR_U64(ctx, 6) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119d7c: 0x3191821  addu        $v1, $t8, $t9
    ctx->pc = 0x119d7cu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 24), GPR_U32(ctx, 25)));
    // 0x119d80: 0x51400001  beql        $t2, $zero, . + 4 + (0x1 << 2)
    ctx->pc = 0x119D80u;
    {
        const bool branch_taken_0x119d80 = (GPR_U64(ctx, 10) == GPR_U64(ctx, 0));
        if (branch_taken_0x119d80) {
            ctx->pc = 0x119D84u;
            ctx->in_delay_slot = true;
            ctx->branch_pc = 0x119D80u;
            // 0x119d84: 0x1cd  break       0, 7 (Delay Slot)
            runtime->handleBreak(rdram, ctx);
            ctx->in_delay_slot = false;
            ctx->pc = 0x119D88u;
            goto label_119d88;
        }
    }
    ctx->pc = 0x119D88u;
label_119d88:
    // 0x119d88: 0x310c0  sll         $v0, $v1, 3
    ctx->pc = 0x119d88u;
    SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 3), 3));
    // 0x119d8c: 0x431023  subu        $v0, $v0, $v1
    ctx->pc = 0x119d8cu;
    SET_GPR_S32(ctx, 2, (int32_t)SUB32(GPR_U32(ctx, 2), GPR_U32(ctx, 3)));
    // 0x119d90: 0x4a001a  div         $zero, $v0, $t2
    ctx->pc = 0x119d90u;
    { int32_t divisor = GPR_S32(ctx, 10);    int32_t dividend = GPR_S32(ctx, 2);    if (divisor != 0) {        if (divisor == -1 && dividend == INT32_MIN) {            ctx->lo = (uint64_t)(int64_t)INT32_MIN; ctx->hi = 0;        } else {            ctx->lo = (uint64_t)(int64_t)(dividend / divisor);            ctx->hi = (uint64_t)(int64_t)(dividend % divisor);        }    } else {        ctx->lo = (dividend < 0) ? 1ull : 0xFFFFFFFFFFFFFFFFull; ctx->hi = (uint64_t)(int64_t)dividend;    } }
    // 0x119d94: 0x1012  mflo        $v0
    ctx->pc = 0x119d94u;
    SET_GPR_U64(ctx, 2, ctx->lo);
    // 0x119d98: 0x40302d  daddu       $a2, $v0, $zero
    ctx->pc = 0x119d98u;
    SET_GPR_U64(ctx, 6, (uint64_t)GPR_U64(ctx, 2) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119d9c: 0xd01821  addu        $v1, $a2, $s0
    ctx->pc = 0x119d9cu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 16)));
    // 0x119da0: 0x163182b  sltu        $v1, $t3, $v1
    ctx->pc = 0x119da0u;
    SET_GPR_U64(ctx, 3, ((uint64_t)GPR_U64(ctx, 11) < (uint64_t)GPR_U64(ctx, 3)) ? 1 : 0);
    // 0x119da4: 0x10600008  beqz        $v1, . + 4 + (0x8 << 2)
    ctx->pc = 0x119DA4u;
    {
        const bool branch_taken_0x119da4 = (GPR_U64(ctx, 3) == GPR_U64(ctx, 0));
        ctx->pc = 0x119DA8u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119DA4u;
        // 0x119da8: 0x31c2000f  andi        $v0, $t6, 0xF (Delay Slot)
        SET_GPR_U64(ctx, 2, GPR_U64(ctx, 14) & (uint64_t)(uint16_t)15);
        ctx->in_delay_slot = false;
        if (branch_taken_0x119da4) {
            ctx->pc = 0x119DC8u;
            goto label_119dc8;
        }
    }
    ctx->pc = 0x119DACu;
    // 0x119dac: 0x28c28000  slti        $v0, $a2, -0x8000
    ctx->pc = 0x119dacu;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 6) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x119db0: 0x1a6182a  slt         $v1, $t5, $a2
    ctx->pc = 0x119db0u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 13) < (int64_t)GPR_S64(ctx, 6)) ? 1 : 0);
    // 0x119db4: 0x1a3300b  movn        $a2, $t5, $v1
    ctx->pc = 0x119db4u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 13));
    // 0x119db8: 0x24038000  addiu       $v1, $zero, -0x8000
    ctx->pc = 0x119db8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x119dbc: 0x38420000  xori        $v0, $v0, 0x0
    ctx->pc = 0x119dbcu;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) ^ (uint64_t)(uint16_t)0);
    // 0x119dc0: 0x62300b  movn        $a2, $v1, $v0
    ctx->pc = 0x119dc0u;
    if (GPR_U64(ctx, 2) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 3));
    // 0x119dc4: 0x31c2000f  andi        $v0, $t6, 0xF
    ctx->pc = 0x119dc4u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 14) & (uint64_t)(uint16_t)15);
label_119dc8:
    // 0x119dc8: 0x24c2818  mult        $a1, $s2, $t4
    ctx->pc = 0x119dc8u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 12); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 5, (int32_t)result); }
    // 0x119dcc: 0x72782018  mult1       $a0, $s3, $t8
    ctx->pc = 0x119dccu;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 24); ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x119dd0: 0x21080  sll         $v0, $v0, 2
    ctx->pc = 0x119dd0u;
    SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 2), 2));
    // 0x119dd4: 0x5e1021  addu        $v0, $v0, $fp
    ctx->pc = 0x119dd4u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 2), GPR_U32(ctx, 30)));
    // 0x119dd8: 0xa5060000  sh          $a2, 0x0($t0)
    ctx->pc = 0x119dd8u;
    WRITE16(ADD32(GPR_U32(ctx, 8), 0), (uint16_t)GPR_U32(ctx, 6));
    // 0x119ddc: 0x8c4e0000  lw          $t6, 0x0($v0)
    ctx->pc = 0x119ddcu;
    SET_GPR_S32(ctx, 14, (int32_t)READ32(ADD32(GPR_U32(ctx, 2), 0)));
    // 0x119de0: 0x30e3000f  andi        $v1, $a3, 0xF
    ctx->pc = 0x119de0u;
    SET_GPR_U64(ctx, 3, GPR_U64(ctx, 7) & (uint64_t)(uint16_t)15);
    // 0x119de4: 0xa5260000  sh          $a2, 0x0($t1)
    ctx->pc = 0x119de4u;
    WRITE16(ADD32(GPR_U32(ctx, 9), 0), (uint16_t)GPR_U32(ctx, 6));
    // 0x119de8: 0x25290002  addiu       $t1, $t1, 0x2
    ctx->pc = 0x119de8u;
    SET_GPR_S32(ctx, 9, (int32_t)ADD32(GPR_U32(ctx, 9), 2));
    // 0x119dec: 0x852021  addu        $a0, $a0, $a1
    ctx->pc = 0x119decu;
    SET_GPR_S32(ctx, 4, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 5)));
    // 0x119df0: 0x1d52818  mult        $a1, $t6, $s5
    ctx->pc = 0x119df0u;
    { int64_t result = (int64_t)GPR_S32(ctx, 14) * (int64_t)GPR_S32(ctx, 21); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 5, (int32_t)result); }
    // 0x119df4: 0x42303  sra         $a0, $a0, 12
    ctx->pc = 0x119df4u;
    SET_GPR_S32(ctx, 4, SRA32(GPR_S32(ctx, 4), 12));
    // 0x119df8: 0x31880  sll         $v1, $v1, 2
    ctx->pc = 0x119df8u;
    SET_GPR_S32(ctx, 3, (int32_t)SLL32(GPR_U32(ctx, 3), 2));
    // 0x119dfc: 0x7e1821  addu        $v1, $v1, $fp
    ctx->pc = 0x119dfcu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 30)));
    // 0x119e00: 0x25080002  addiu       $t0, $t0, 0x2
    ctx->pc = 0x119e00u;
    SET_GPR_S32(ctx, 8, (int32_t)ADD32(GPR_U32(ctx, 8), 2));
    // 0x119e04: 0x853021  addu        $a2, $a0, $a1
    ctx->pc = 0x119e04u;
    SET_GPR_S32(ctx, 6, (int32_t)ADD32(GPR_U32(ctx, 4), GPR_U32(ctx, 5)));
    // 0x119e08: 0xd01021  addu        $v0, $a2, $s0
    ctx->pc = 0x119e08u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 16)));
    // 0x119e0c: 0x162102b  sltu        $v0, $t3, $v0
    ctx->pc = 0x119e0cu;
    SET_GPR_U64(ctx, 2, ((uint64_t)GPR_U64(ctx, 11) < (uint64_t)GPR_U64(ctx, 2)) ? 1 : 0);
    // 0x119e10: 0x10400007  beqz        $v0, . + 4 + (0x7 << 2)
    ctx->pc = 0x119E10u;
    {
        const bool branch_taken_0x119e10 = (GPR_U64(ctx, 2) == GPR_U64(ctx, 0));
        ctx->pc = 0x119E14u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119E10u;
        // 0x119e14: 0x8c670000  lw          $a3, 0x0($v1) (Delay Slot)
        SET_GPR_S32(ctx, 7, (int32_t)READ32(ADD32(GPR_U32(ctx, 3), 0)));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119e10) {
            ctx->pc = 0x119E30u;
            goto label_119e30;
        }
    }
    ctx->pc = 0x119E18u;
    // 0x119e18: 0x28c28000  slti        $v0, $a2, -0x8000
    ctx->pc = 0x119e18u;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 6) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x119e1c: 0x1a6182a  slt         $v1, $t5, $a2
    ctx->pc = 0x119e1cu;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 13) < (int64_t)GPR_S64(ctx, 6)) ? 1 : 0);
    // 0x119e20: 0x1a3300b  movn        $a2, $t5, $v1
    ctx->pc = 0x119e20u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 13));
    // 0x119e24: 0x24038000  addiu       $v1, $zero, -0x8000
    ctx->pc = 0x119e24u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x119e28: 0x38420000  xori        $v0, $v0, 0x0
    ctx->pc = 0x119e28u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) ^ (uint64_t)(uint16_t)0);
    // 0x119e2c: 0x62300b  movn        $a2, $v1, $v0
    ctx->pc = 0x119e2cu;
    if (GPR_U64(ctx, 2) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 3));
label_119e30:
    // 0x119e30: 0x2511018  mult        $v0, $s2, $s1
    ctx->pc = 0x119e30u;
    { int64_t result = (int64_t)GPR_S32(ctx, 18) * (int64_t)GPR_S32(ctx, 17); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 2, (int32_t)result); }
    // 0x119e34: 0x72791818  mult1       $v1, $s3, $t9
    ctx->pc = 0x119e34u;
    { int64_t result = (int64_t)GPR_S32(ctx, 19) * (int64_t)GPR_S32(ctx, 25); ctx->lo1 = (uint64_t)(int64_t)(int32_t)result; ctx->hi1 = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 3, (int32_t)result); }
    // 0x119e38: 0xf42018  mult        $a0, $a3, $s4
    ctx->pc = 0x119e38u;
    { int64_t result = (int64_t)GPR_S32(ctx, 7) * (int64_t)GPR_S32(ctx, 20); ctx->lo = (uint64_t)(int64_t)(int32_t)result; ctx->hi = (uint64_t)(int64_t)(int32_t)(result >> 32); SET_GPR_S32(ctx, 4, (int32_t)result); }
    // 0x119e3c: 0x300602d  daddu       $t4, $t8, $zero
    ctx->pc = 0x119e3cu;
    SET_GPR_U64(ctx, 12, (uint64_t)GPR_U64(ctx, 24) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119e40: 0xc0c02d  daddu       $t8, $a2, $zero
    ctx->pc = 0x119e40u;
    SET_GPR_U64(ctx, 24, (uint64_t)GPR_U64(ctx, 6) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119e44: 0x621821  addu        $v1, $v1, $v0
    ctx->pc = 0x119e44u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 2)));
    // 0x119e48: 0x31b03  sra         $v1, $v1, 12
    ctx->pc = 0x119e48u;
    SET_GPR_S32(ctx, 3, SRA32(GPR_S32(ctx, 3), 12));
    // 0x119e4c: 0x643021  addu        $a2, $v1, $a0
    ctx->pc = 0x119e4cu;
    SET_GPR_S32(ctx, 6, (int32_t)ADD32(GPR_U32(ctx, 3), GPR_U32(ctx, 4)));
    // 0x119e50: 0xd01021  addu        $v0, $a2, $s0
    ctx->pc = 0x119e50u;
    SET_GPR_S32(ctx, 2, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 16)));
    // 0x119e54: 0x162102b  sltu        $v0, $t3, $v0
    ctx->pc = 0x119e54u;
    SET_GPR_U64(ctx, 2, ((uint64_t)GPR_U64(ctx, 11) < (uint64_t)GPR_U64(ctx, 2)) ? 1 : 0);
    // 0x119e58: 0x10400007  beqz        $v0, . + 4 + (0x7 << 2)
    ctx->pc = 0x119E58u;
    {
        const bool branch_taken_0x119e58 = (GPR_U64(ctx, 2) == GPR_U64(ctx, 0));
        ctx->pc = 0x119E5Cu;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119E58u;
        // 0x119e5c: 0x320882d  daddu       $s1, $t9, $zero (Delay Slot)
        SET_GPR_U64(ctx, 17, (uint64_t)GPR_U64(ctx, 25) + (uint64_t)GPR_U64(ctx, 0));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119e58) {
            ctx->pc = 0x119E78u;
            goto label_119e78;
        }
    }
    ctx->pc = 0x119E60u;
    // 0x119e60: 0x28c28000  slti        $v0, $a2, -0x8000
    ctx->pc = 0x119e60u;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 6) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x119e64: 0x1a6182a  slt         $v1, $t5, $a2
    ctx->pc = 0x119e64u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 13) < (int64_t)GPR_S64(ctx, 6)) ? 1 : 0);
    // 0x119e68: 0x1a3300b  movn        $a2, $t5, $v1
    ctx->pc = 0x119e68u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 13));
    // 0x119e6c: 0x24038000  addiu       $v1, $zero, -0x8000
    ctx->pc = 0x119e6cu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x119e70: 0x38420000  xori        $v0, $v0, 0x0
    ctx->pc = 0x119e70u;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) ^ (uint64_t)(uint16_t)0);
    // 0x119e74: 0x62300b  movn        $a2, $v1, $v0
    ctx->pc = 0x119e74u;
    if (GPR_U64(ctx, 2) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 3));
label_119e78:
    // 0x119e78: 0xc0c82d  daddu       $t9, $a2, $zero
    ctx->pc = 0x119e78u;
    SET_GPR_U64(ctx, 25, (uint64_t)GPR_U64(ctx, 6) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119e7c: 0x3191821  addu        $v1, $t8, $t9
    ctx->pc = 0x119e7cu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 24), GPR_U32(ctx, 25)));
    // 0x119e80: 0x51400001  beql        $t2, $zero, . + 4 + (0x1 << 2)
    ctx->pc = 0x119E80u;
    {
        const bool branch_taken_0x119e80 = (GPR_U64(ctx, 10) == GPR_U64(ctx, 0));
        if (branch_taken_0x119e80) {
            ctx->pc = 0x119E84u;
            ctx->in_delay_slot = true;
            ctx->branch_pc = 0x119E80u;
            // 0x119e84: 0x1cd  break       0, 7 (Delay Slot)
            runtime->handleBreak(rdram, ctx);
            ctx->in_delay_slot = false;
            ctx->pc = 0x119E88u;
            goto label_119e88;
        }
    }
    ctx->pc = 0x119E88u;
label_119e88:
    // 0x119e88: 0x310c0  sll         $v0, $v1, 3
    ctx->pc = 0x119e88u;
    SET_GPR_S32(ctx, 2, (int32_t)SLL32(GPR_U32(ctx, 3), 3));
    // 0x119e8c: 0x431023  subu        $v0, $v0, $v1
    ctx->pc = 0x119e8cu;
    SET_GPR_S32(ctx, 2, (int32_t)SUB32(GPR_U32(ctx, 2), GPR_U32(ctx, 3)));
    // 0x119e90: 0x4a001a  div         $zero, $v0, $t2
    ctx->pc = 0x119e90u;
    { int32_t divisor = GPR_S32(ctx, 10);    int32_t dividend = GPR_S32(ctx, 2);    if (divisor != 0) {        if (divisor == -1 && dividend == INT32_MIN) {            ctx->lo = (uint64_t)(int64_t)INT32_MIN; ctx->hi = 0;        } else {            ctx->lo = (uint64_t)(int64_t)(dividend / divisor);            ctx->hi = (uint64_t)(int64_t)(dividend % divisor);        }    } else {        ctx->lo = (dividend < 0) ? 1ull : 0xFFFFFFFFFFFFFFFFull; ctx->hi = (uint64_t)(int64_t)dividend;    } }
    // 0x119e94: 0x1012  mflo        $v0
    ctx->pc = 0x119e94u;
    SET_GPR_U64(ctx, 2, ctx->lo);
    // 0x119e98: 0x40302d  daddu       $a2, $v0, $zero
    ctx->pc = 0x119e98u;
    SET_GPR_U64(ctx, 6, (uint64_t)GPR_U64(ctx, 2) + (uint64_t)GPR_U64(ctx, 0));
    // 0x119e9c: 0xd01821  addu        $v1, $a2, $s0
    ctx->pc = 0x119e9cu;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 6), GPR_U32(ctx, 16)));
    // 0x119ea0: 0x163182b  sltu        $v1, $t3, $v1
    ctx->pc = 0x119ea0u;
    SET_GPR_U64(ctx, 3, ((uint64_t)GPR_U64(ctx, 11) < (uint64_t)GPR_U64(ctx, 3)) ? 1 : 0);
    // 0x119ea4: 0x10600007  beqz        $v1, . + 4 + (0x7 << 2)
    ctx->pc = 0x119EA4u;
    {
        const bool branch_taken_0x119ea4 = (GPR_U64(ctx, 3) == GPR_U64(ctx, 0));
        ctx->pc = 0x119EA8u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119EA4u;
        // 0x119ea8: 0x26d6fffe  addiu       $s6, $s6, -0x2 (Delay Slot)
        SET_GPR_S32(ctx, 22, (int32_t)ADD32(GPR_U32(ctx, 22), 4294967294));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119ea4) {
            ctx->pc = 0x119EC4u;
            goto label_119ec4;
        }
    }
    ctx->pc = 0x119EACu;
    // 0x119eac: 0x28c28000  slti        $v0, $a2, -0x8000
    ctx->pc = 0x119eacu;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 6) < (int64_t)(int32_t)4294934528) ? 1 : 0);
    // 0x119eb0: 0x1a6182a  slt         $v1, $t5, $a2
    ctx->pc = 0x119eb0u;
    SET_GPR_U64(ctx, 3, ((int64_t)GPR_S64(ctx, 13) < (int64_t)GPR_S64(ctx, 6)) ? 1 : 0);
    // 0x119eb4: 0x1a3300b  movn        $a2, $t5, $v1
    ctx->pc = 0x119eb4u;
    if (GPR_U64(ctx, 3) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 13));
    // 0x119eb8: 0x24038000  addiu       $v1, $zero, -0x8000
    ctx->pc = 0x119eb8u;
    SET_GPR_S32(ctx, 3, (int32_t)ADD32(GPR_U32(ctx, 0), 4294934528));
    // 0x119ebc: 0x38420000  xori        $v0, $v0, 0x0
    ctx->pc = 0x119ebcu;
    SET_GPR_U64(ctx, 2, GPR_U64(ctx, 2) ^ (uint64_t)(uint16_t)0);
    // 0x119ec0: 0x62300b  movn        $a2, $v1, $v0
    ctx->pc = 0x119ec0u;
    if (GPR_U64(ctx, 2) != 0) SET_GPR_U64(ctx, 6, GPR_U64(ctx, 3));
label_119ec4:
    // 0x119ec4: 0xa5060000  sh          $a2, 0x0($t0)
    ctx->pc = 0x119ec4u;
    WRITE16(ADD32(GPR_U32(ctx, 8), 0), (uint16_t)GPR_U32(ctx, 6));
    // 0x119ec8: 0xa5260000  sh          $a2, 0x0($t1)
    ctx->pc = 0x119ec8u;
    WRITE16(ADD32(GPR_U32(ctx, 9), 0), (uint16_t)GPR_U32(ctx, 6));
    // 0x119ecc: 0x25290002  addiu       $t1, $t1, 0x2
    ctx->pc = 0x119eccu;
    SET_GPR_S32(ctx, 9, (int32_t)ADD32(GPR_U32(ctx, 9), 2));
    // 0x119ed0: 0x6c1ff7f  bgez        $s6, . + 4 + (-0x81 << 2)
    ctx->pc = 0x119ED0u;
    {
        const bool branch_taken_0x119ed0 = (GPR_S32(ctx, 22) >= 0);
        ctx->pc = 0x119ED4u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119ED0u;
        // 0x119ed4: 0x25080002  addiu       $t0, $t0, 0x2 (Delay Slot)
        SET_GPR_S32(ctx, 8, (int32_t)ADD32(GPR_U32(ctx, 8), 2));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119ed0) {
            ctx->pc = 0x119CD0u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_119cd0;
        }
    }
    ctx->pc = 0x119ED8u;
    // 0x119ed8: 0x8fa30014  lw          $v1, 0x14($sp)
    ctx->pc = 0x119ed8u;
    SET_GPR_S32(ctx, 3, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 20)));
    // 0x119edc: 0x26f70001  addiu       $s7, $s7, 0x1
    ctx->pc = 0x119edcu;
    SET_GPR_S32(ctx, 23, (int32_t)ADD32(GPR_U32(ctx, 23), 1));
    // 0x119ee0: 0x2e3102a  slt         $v0, $s7, $v1
    ctx->pc = 0x119ee0u;
    SET_GPR_U64(ctx, 2, ((int64_t)GPR_S64(ctx, 23) < (int64_t)GPR_S64(ctx, 3)) ? 1 : 0);
    // 0x119ee4: 0x1440ff46  bnez        $v0, . + 4 + (-0xBA << 2)
    ctx->pc = 0x119EE4u;
    {
        const bool branch_taken_0x119ee4 = (GPR_U64(ctx, 2) != GPR_U64(ctx, 0));
        ctx->pc = 0x119EE8u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119EE4u;
        // 0x119ee8: 0x25ef0012  addiu       $t7, $t7, 0x12 (Delay Slot)
        SET_GPR_S32(ctx, 15, (int32_t)ADD32(GPR_U32(ctx, 15), 18));
        ctx->in_delay_slot = false;
        if (branch_taken_0x119ee4) {
            ctx->pc = 0x119C00u;
            if (runtime->shouldPreemptGuestExecution()) {
                return;
            }
            goto label_119c00;
        }
    }
    ctx->pc = 0x119EECu;
label_119eec:
    // 0x119eec: 0x8fa50004  lw          $a1, 0x4($sp)
    ctx->pc = 0x119eecu;
    SET_GPR_S32(ctx, 5, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 4)));
    // 0x119ef0: 0xa4ac0002  sh          $t4, 0x2($a1)
    ctx->pc = 0x119ef0u;
    WRITE16(ADD32(GPR_U32(ctx, 5), 2), (uint16_t)GPR_U32(ctx, 12));
    // 0x119ef4: 0x8fa20000  lw          $v0, 0x0($sp)
    ctx->pc = 0x119ef4u;
    SET_GPR_S32(ctx, 2, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 0)));
    // 0x119ef8: 0xa4b80000  sh          $t8, 0x0($a1)
    ctx->pc = 0x119ef8u;
    WRITE16(ADD32(GPR_U32(ctx, 5), 0), (uint16_t)GPR_U32(ctx, 24));
    // 0x119efc: 0x8fa60008  lw          $a2, 0x8($sp)
    ctx->pc = 0x119efcu;
    SET_GPR_S32(ctx, 6, (int32_t)READ32(ADD32(GPR_U32(ctx, 29), 8)));
    // 0x119f00: 0xa4d10002  sh          $s1, 0x2($a2)
    ctx->pc = 0x119f00u;
    WRITE16(ADD32(GPR_U32(ctx, 6), 2), (uint16_t)GPR_U32(ctx, 17));
    // 0x119f04: 0xa4d90000  sh          $t9, 0x0($a2)
    ctx->pc = 0x119f04u;
    WRITE16(ADD32(GPR_U32(ctx, 6), 0), (uint16_t)GPR_U32(ctx, 25));
label_119f08:
    // 0x119f08: 0xdfb00020  ld          $s0, 0x20($sp)
    ctx->pc = 0x119f08u;
    SET_GPR_U64(ctx, 16, READ64(ADD32(GPR_U32(ctx, 29), 32)));
    // 0x119f0c: 0xdfb10028  ld          $s1, 0x28($sp)
    ctx->pc = 0x119f0cu;
    SET_GPR_U64(ctx, 17, READ64(ADD32(GPR_U32(ctx, 29), 40)));
    // 0x119f10: 0xdfb20030  ld          $s2, 0x30($sp)
    ctx->pc = 0x119f10u;
    SET_GPR_U64(ctx, 18, READ64(ADD32(GPR_U32(ctx, 29), 48)));
    // 0x119f14: 0xdfb30038  ld          $s3, 0x38($sp)
    ctx->pc = 0x119f14u;
    SET_GPR_U64(ctx, 19, READ64(ADD32(GPR_U32(ctx, 29), 56)));
    // 0x119f18: 0xdfb40040  ld          $s4, 0x40($sp)
    ctx->pc = 0x119f18u;
    SET_GPR_U64(ctx, 20, READ64(ADD32(GPR_U32(ctx, 29), 64)));
    // 0x119f1c: 0xdfb50048  ld          $s5, 0x48($sp)
    ctx->pc = 0x119f1cu;
    SET_GPR_U64(ctx, 21, READ64(ADD32(GPR_U32(ctx, 29), 72)));
    // 0x119f20: 0xdfb60050  ld          $s6, 0x50($sp)
    ctx->pc = 0x119f20u;
    SET_GPR_U64(ctx, 22, READ64(ADD32(GPR_U32(ctx, 29), 80)));
    // 0x119f24: 0xdfb70058  ld          $s7, 0x58($sp)
    ctx->pc = 0x119f24u;
    SET_GPR_U64(ctx, 23, READ64(ADD32(GPR_U32(ctx, 29), 88)));
    // 0x119f28: 0xdfbe0060  ld          $fp, 0x60($sp)
    ctx->pc = 0x119f28u;
    SET_GPR_U64(ctx, 30, READ64(ADD32(GPR_U32(ctx, 29), 96)));
    // 0x119f2c: 0x3e00008  jr          $ra
    ctx->pc = 0x119F2Cu;
    {
        const uint32_t jumpTarget = GPR_U32(ctx, 31);
        ctx->pc = 0x119F30u;
        ctx->in_delay_slot = true;
        ctx->branch_pc = 0x119F2Cu;
        // 0x119f30: 0x27bd0070  addiu       $sp, $sp, 0x70 (Delay Slot)
        SET_GPR_S32(ctx, 29, (int32_t)ADD32(GPR_U32(ctx, 29), 112));
        ctx->in_delay_slot = false;
        ctx->pc = jumpTarget;
        #if defined(PS2X_STRICT_RETURN_DIAGNOSTICS) && PS2X_STRICT_RETURN_DIAGNOSTICS
        (void)runtime->dispatchGuestBranch(rdram, ctx, jumpTarget, 0x119F2Cu, 0u, PS2Runtime::GuestBranchKind::Return, "JR $ra");
        return;
        #else
        ctx->pc = jumpTarget;
        return;
        #endif
    }
    ctx->pc = 0x119F34u;
    // 0x119f34: 0x0  nop
    ctx->pc = 0x119f34u;
    // NOP
    ctx->pc = 0x119f38u;
}
