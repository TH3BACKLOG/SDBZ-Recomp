#include "runtime/ps2_gs_rasterizer.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_memory.h"
#include "runtime/ps2_diag.h"
#include "ps2_log.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

using namespace GSInternal;

// [drawpath] run 32: origin path of the GIF packet currently being dispatched.
// Defined in ps2_gif_arbiter.cpp; declared here rather than in a header so no
// generated TU is rebuilt.
namespace ps2diag_gifpath
{
extern std::atomic<uint32_t> g_curPath;
extern std::atomic<uint32_t> g_curSite;
extern std::atomic<uint32_t> g_curSrc;
}

// [drawpath] run 33 causality experiment. PS2X_SKIPBG=1 drops the full-screen
// PSMT8/tbw4 background sprite entirely. It is NOT a fix -- it deliberately
// leaves the rest of the screen without its backdrop. Its only job is to
// settle whether the draw-order inversion is the WHOLE story: with the
// background gone the dialog must come back as a maroon panel with legible
// white text. If it does not, a second fault exists downstream and the order
// work would have been chasing only half the bug.
namespace ps2diag_skipbg
{
inline bool enabled()
{
    static const bool on = []
    {
        const char *v = std::getenv("PS2X_SKIPBG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return on;
}
}

// [fbdest] probe state.
//
// The [zbuf] run proved the framebuffer at page 0x70 is 100% occupied
// (2048/2048 raw words non-zero) yet contains zero RGB. So the writes
// land at the right address and arrive already black -- addressing, Z
// aliasing and FBMSK are all ruled out.
//
// Two possibilities remain, and they need different fixes:
//   (a) the coloured pixels go to a *different* frame.fbp than the one
//       that gets displayed, or
//   (b) they are drawn and then buried by a later black full-screen
//       sprite in the same frame (clear-ordering bug).
//
// Both are answered by bucketing writes by destination fbp and by
// recording where in the frame's write sequence the last coloured
// pixel landed. Note these count the FINAL `pixel` value about to be
// stored, not the pre-mask r/g/b that [gs:frame] samples -- that
// ordering hazard is exactly what we are trying to close out.
//
// Defined here and read via extern in ps2_gs_gpu.cpp's present probe;
// adding members to GS would mean touching a header, which recompiles
// all 30,000+ runner translation units.
namespace ps2diag_fbstat
{
    std::atomic<uint32_t> g_fbNonBlack[512];
    std::atomic<uint32_t> g_fbBlack[512];
    std::atomic<uint64_t> g_writeSeq{0};
    std::atomic<uint64_t> g_lastNonBlackSeq{0};

    // ---- [blackwho] -------------------------------------------------------
    // [fbdest] proved coloured pixels DO reach WriteVram on both fbp 0x0 and
    // 0x70, yet [fbscan] finds both buffers empty at present time, and the
    // last ~2% of each interval's stores (~3 full screens) are black. So the
    // artwork is drawn and then buried. This probe answers *by what*:
    //
    //   untex          - a plain black fill/clear sprite (draw-order bug)
    //   texzero        - textured, but the texel sampled as RGB 0
    //                    (PSMT4/CLUT decode returning index 0 -> black)
    //   texcol         - texel had colour, yet we stored black
    //                    (blend / TEXFUNC / FBMSK ate it)
    //
    // Plus the bbox of the trailing run of black writes (reset on every
    // coloured write), which says whether the burying draw is full-screen,
    // and a snapshot of the GS state at the last black store, which names it.
    std::atomic<uint64_t> g_blkUntex{0};
    std::atomic<uint64_t> g_blkTexZero{0};
    std::atomic<uint64_t> g_blkTexCol{0};

    std::atomic<uint32_t> g_tailX0{0xFFFFu};
    std::atomic<uint32_t> g_tailY0{0xFFFFu};
    std::atomic<uint32_t> g_tailX1{0};
    std::atomic<uint32_t> g_tailY1{0};

    // Snapshot of the last black store. Racy by design -- it is a probe.
    std::atomic<uint32_t> g_snapPrim{0};   // raw PRIM
    std::atomic<uint32_t> g_snapTexPsm{0};
    std::atomic<uint32_t> g_snapTbp{0};
    std::atomic<uint32_t> g_snapCbp{0};
    std::atomic<uint32_t> g_snapFbmsk{0};
    std::atomic<uint32_t> g_snapAlpha{0};  // a | b<<2 | c<<4 | d<<6 | fix<<8
    std::atomic<uint32_t> g_snapTexel{0};
    std::atomic<uint32_t> g_snapSrcRgb{0}; // colour entering writePixel
    std::atomic<uint32_t> g_snapXy{0};     // x | y<<16
    std::atomic<uint32_t> g_snapFbp{0};

    // Last texel sampled on this thread. Only meaningful when PRIM.TME is
    // set -- every textured writePixel call site stores it first.
    thread_local uint32_t t_lastTexel = 0;

    // ---- [texzero] --------------------------------------------------------
    // [blackwho] on the 150 s run: untex=13.76M, texzero=15.76M, texcol=19.8k.
    // So the dominant burier is NOT the clear and NOT the blender -- it is
    // textured sprites whose texel decodes to RGB 0 and which then STORE that
    // black over whatever was underneath. On real GS those pixels are supposed
    // to be discarded (alpha test) or blended to Cd (ABE with As=0).
    //
    // The existing snap* set cannot describe them: it is overwritten by every
    // black store, and the last store of any interval is the untextured
    // full-screen clear, so it always reports prim=0x6 tme=0 abe=0. This set is
    // written ONLY on the texzero branch, so it names the real offender.
    //
    // The discriminating pair is g_tzAbeOn / g_tzAbeOff:
    //   AbeOff dominant -> the game submits these unblended and expects the
    //       alpha TEST to kill them; our classifyAlphaTest is letting them
    //       through (check ATE/ATST/AREF against g_tzTest, and whether our
    //       PSMT4 decode produces alpha=0 for index 0 at all -- g_tzTexA).
    //   AbeOn dominant  -> the blend ran but produced black, which means the
    //       destination was already black or the ALPHA regs decoded wrong
    //       (compare g_tzAlpha against g_tzDstRgb).
    std::atomic<uint64_t> g_tzAbeOn{0};
    std::atomic<uint64_t> g_tzAbeOff{0};
    std::atomic<uint64_t> g_tzAteOn{0};   // texzero stores with alpha test enabled

    std::atomic<uint32_t> g_tzPrim{0};    // raw PRIM at a texzero store
    std::atomic<uint32_t> g_tzTest{0};    // raw TEST -- names the alpha-test config
    std::atomic<uint32_t> g_tzAlpha{0};   // a | b<<2 | c<<4 | d<<6 | fix<<8
    std::atomic<uint32_t> g_tzSrcA{0};    // source alpha reaching writePixel
    std::atomic<uint32_t> g_tzTexel{0};   // full 32-bit texel, alpha included
    std::atomic<uint32_t> g_tzTbp{0};
    std::atomic<uint32_t> g_tzCbp{0};
    std::atomic<uint32_t> g_tzTexPsm{0};
    std::atomic<uint32_t> g_tzDstRgb{0};  // framebuffer colour being destroyed
    std::atomic<uint32_t> g_tzFbp{0};

    // [clutmap] -- Stage 5.11 run 14. Run 13 + [vramcen] established that the
    // palette IS colourful (954-1004 of 1024 cache bytes nonzero) and the
    // texture page IS uploaded (4107-10188 of 16384 bytes nonzero), yet every
    // sampled texel resolves to black. That leaves exactly one layer: the
    // index -> entry lookup at ps2_gs_rasterizer.cpp:841.
    //
    // t_lastTexIndex is the RAW value out of ReadTexturePageCache, captured
    // before ReadClutCache consumes it. The accumulators below are written only
    // inside the already-cold texzero branch, so the hot path costs one
    // thread-local store.
    //
    // PRE-REGISTERED READING:
    //   idxor == 0 and idxnz == 0  -> every index is 0. The texture fetch is
    //       broken despite texnz>0: page-cache addressing (base_block/pitch/
    //       page_width2) or a stale m_texture_page_cache. Fix in
    //       ReadTexturePageCache / ReloadTexturePageCache.
    //   idxor wide (>= 0xF0) and idxnz large -> indices are real and varied.
    //       The palette lookup is what is wrong: csa/csm/cpsm handling in
    //       ReadClutCache vs ReloadClutCacheCSM1. Compare csa/csm/cpsm below
    //       against ent0..ent7.
    //   idxor narrow but nonzero (e.g. 0x0F on a T8 texture) -> only the low
    //       nibble survives; a T4/T8 expansion bug in ReadPageToLinearBufferP8.
    //
    // GUARD (rival reading, per feedback_degenerate_result_convicts_the_probe):
    //   idxsamp is the total count of index samples that reached this branch.
    //   If idxsamp is tiny the other two fields describe almost nothing and
    //   must not be read as a distribution.
    thread_local uint32_t t_lastTexIndex = 0;
    std::atomic<uint32_t> g_tzIdxOr{0};    // OR of every raw index seen
    std::atomic<uint32_t> g_tzIdxNz{0};    // count of raw indices != 0
    std::atomic<uint32_t> g_tzIdxSamp{0};  // total raw indices seen (guard)
    std::atomic<uint32_t> g_tzIdxLast{0};  // most recent raw index
    std::atomic<uint32_t> g_tzCsa{0};
    std::atomic<uint32_t> g_tzCsm{0};
    std::atomic<uint32_t> g_tzCpsm{0};

    // bbox of texzero stores only -- says whether they are glyph-sized sprites
    // or full-screen overlays. NOT reset by coloured writes; drained on report.
    std::atomic<uint32_t> g_tzX0{0xFFFFu};
    std::atomic<uint32_t> g_tzY0{0xFFFFu};
    std::atomic<uint32_t> g_tzX1{0};
    std::atomic<uint32_t> g_tzY1{0};

    // ---- [boxblk] -- Stage 5.11 run 18 -----------------------------------
    // [redbox] settled the previous question with positive evidence: three
    // records caught a back buffer holding the COMPLETE red box (54922/54978
    // px strictly red, maxr=0x64, centre=0x200e64) -- once at fbp=0x0, twice
    // at fbp=0x70. The presented frame never had it, and the SAME buffer later
    // read back nonblack=9487 red=0. So the red is destroyed in VRAM before
    // latch. This is NOT a presentation-path loss -- that reading is retracted.
    //
    // It is an ERASE, not an overdraw. The [texred] grey sprite blends 0x64 ->
    // 0x24, which is still nonblack; if overdraw were the cause nonblack would
    // stay near 54978 with red=0. Instead ~45000 px go to EXACT zero while
    // ~9.5k survive. Something writes black over the box and spares the text.
    //
    // The mechanism is almost certainly the one [blackwho] already counted:
    // texzero=15.76M textured sprites whose texel decodes to RGB 0 and store
    // it. That does not contradict [texred]'s storedblack=0 -- t_redPixel is
    // keyed off a RED texel, so black-texel sprites were never in that sample.
    //
    // So the only open question is which CLUT entry those black stores used.
    // [clutdump] showed T8 entries 0..16 are a black alpha-ramp (drop shadow)
    // and entries 20,24,27,29,31 are the reds (R=0x2a..0x6f). Scope everything
    // below to the box rect and bucket the RAW index.
    //
    // PRE-REGISTERED READING (read bbany FIRST -- see GUARD):
    //   i00_03..i16_19 dominant, i20_31 ~0
    //       -> indices land in the black ramp. The palette lookup is FINE and
    //          the texture indices themselves are wrong: suspect the T8/T4
    //          index decode (ReadPageToLinearBufferP8/P4, gs_memory.cpp:1367).
    //   i20_31 dominant while blktex is large
    //       -> indices are correct and point AT the reds, but the texel came
    //          back black => the CLUT lookup IS broken after all, and
    //          [clutdump]'s "chain is clean" verdict must be reopened.
    //   blkuntex >> blktex
    //       -> the eraser is NOT a textured sprite. It is the untextured clear
    //          or a non-rasterizer writer (clearFramebufferRect / l2l / the
    //          host->local upload at gpu.cpp:3725). Next tool is a write
    //          journal on those three, NOT another CLUT probe.
    //   bb16 large
    //       -> the framebuffer is 16bpp here and the RGB decode below does not
    //          apply; every colour-derived field is void. Read bb16 before red.
    //
    // GUARD (rival reading, per feedback_degenerate_result_convicts_the_probe):
    //   bbany  = every store landing in the box (liveness). If bbany is 0 the
    //            box rect or the fbp filter is wrong and NOTHING else here
    //            means anything -- the probe is convicted, not the renderer.
    //   bbred  = strictly-red stores into the box, same test as censusRedBox.
    //            This is the rival: if bbred is large and bbblack is ~0, the
    //            rasterizer is NOT the eraser and the reading table above is
    //            moot -- go straight to the non-rasterizer writers.
    std::atomic<uint64_t> g_bbAny{0};
    std::atomic<uint64_t> g_bbBlack{0};
    std::atomic<uint64_t> g_bbRed{0};
    std::atomic<uint64_t> g_bbBlkTex{0};
    std::atomic<uint64_t> g_bbBlkUntex{0};
    std::atomic<uint64_t> g_bb16{0};
    // Raw-index histogram for BLACK TEXTURED stores inside the box.
    // Buckets 0..7 are indices 0-3,4-7,...,28-31; bucket 8 is index >= 32.
    std::atomic<uint64_t> g_bbIdx[9] = {};
    // Snapshot of the most recent black textured store inside the box.
    std::atomic<uint32_t> g_bbIdxLast{0};
    std::atomic<uint32_t> g_bbTexel{0};
    std::atomic<uint32_t> g_bbTexPsm{0};
    std::atomic<uint32_t> g_bbTbp{0};
    std::atomic<uint32_t> g_bbCbp{0};
    std::atomic<uint32_t> g_bbCsa{0};
    std::atomic<uint32_t> g_bbCpsm{0};
    std::atomic<uint32_t> g_bbPrim{0};
    std::atomic<uint32_t> g_bbTest{0};
    std::atomic<uint32_t> g_bbSrcA{0};
    std::atomic<uint32_t> g_bbXy{0};
    std::atomic<uint32_t> g_bbFbp{0};

    // ---- [texfetch] -- Stage 5.11 run 24 (RE-POINTED at the T4 glyph fetch) --
    //
    // WHY THIS PROBE WAS REWRITTEN. Runs 20 and 22 gated on
    //     tex.psm == GS_PSM_T8 && tex.tbp0 == kTfTbp
    // i.e. the WRONG FORMAT and then an ADDRESS CONSTANT. The dialog text is
    // PSMT4, so this probe has never once observed the fetch that draws it --
    // exactly the failure [uvspan] had, in the same file, 1300 lines away.
    // Gate on SHAPE (feedback_probe_gate_on_shape_not_address): the glyph
    // atlas is the only PSMT4 256x256 tbw=4 texture in the frame, on both the
    // PCSX2 dump side and the live side.
    //
    // WHAT IS ALREADY PROVEN, SO THIS PROBE DOES NOT RE-ASK IT:
    //   * atlas index data in VRAM is healthy    ([clutlive] idxnz/idxhist16)
    //   * palette in VRAM is byte-exact vs PCSX2 ([shadow], PS2_PROJECT_STATE)
    //   * palette AT THE DRAW is a clean white alpha ramp, 0 black entries
    //                                            ([clutlive] round 2: atw
    //                                             white=2932 black=0 hits=2932)
    //   * alpha test / depth test both ALWAYS-pass (test=0x30003 both sides)
    //   * blend eq 0x44, blender math ((A-B)*C >> 7)+D   -- correct 0..128
    //   * MODULATE is (t*v)>>7, applyTexa is a pass-through for T4
    //   * vertex ST/Q, FST decode, per-corner UV, sprite size -- all correct
    //                                            ([uvspan] run 23: fst0=draws,
    //                                             qbad=0, du256==ds64k==6168,
    //                                             spanx=spany=17)
    //
    // So every link is measured EXCEPT one:
    //     gs->ReadTexturePageCache(GS_PSM_T4, tbp0, tbw=4, sampleU, sampleV)
    // and the T4 CLUT lookup immediately after it. [clutlive] cannot cover
    // this -- its own SCOPE LIMIT note says it reads VRAM and the CLUT cache
    // HOST-side and "cannot see what the rasterizer's sampler computed".
    //
    // GROUND TRUTH to compare the index histogram against. [clutlive] walked
    // the whole 256x256 atlas in VRAM and binned every texel by index value:
    //     60638, 0, 0, 22, 75, 83, 104, 416, 68, 68, 63, 365, 108, 0, 158, 3368
    // Index 0 is 92.5% of the atlas because most of a font page is empty. We
    // sample only glyph CELLS here, so bin0 must be a clear majority but must
    // NOT be everything -- bins 3..15 have to carry real weight or no ink is
    // being fetched. The palette makes index 0 fully transparent and index 15
    // fully opaque white, so "all bin0" and "invisible text" are the same
    // statement.
    //
    // UNITS. u/v are integer texels 0..255, post-clamp, exactly as handed to
    // ReadTexturePageCache. idxmax/hist are the RAW nibble out of the fetch.
    // amax is PS2 alpha, so 128 == 1.0, NOT 255.
    //
    // PRE-REGISTERED READING -- take the branches in this order.
    //
    //   (0) GUARDS, before anything else. tfall -> tft4 -> tfsamp.
    //       tfall  = paletted samples at ANY psm. 0 => no textured sprite ever
    //                sampled; probe is dead, every field below is noise.
    //       tft4   = T4 samples at any shape. 0 while tfall > 0 => the glyph
    //                sprite never reaches the sampler, or psm decode is wrong.
    //                Nothing below describes the text.
    //       tfsamp = T4 samples at 256x256 tbw=4. 0 while tft4 > 0 => the
    //                shape gate is wrong, not the renderer. Read tbw/tw/th
    //                from the echo fields and re-point. Values below are STALE.
    //       tfchk  = rival ReadVram reads actually taken (1 in 64). tfdis is
    //                meaningless unless tfchk > 0. tfdis==0 with tfchk==0 is a
    //                SILENT probe, not a pass.
    //
    //   (1) COORDINATE SPAN -- u=..  v=..
    //       [uvspan] proved the sprite CORNERS are right but only reported the
    //       LAST draw. These are min..max across every sample of every glyph.
    //       ~24 texels wide total (a single cell)
    //            -> every one of the ~98k glyph draws samples the SAME atlas
    //               cell. The corners are per-draw constants and the text is
    //               one character repeated. Fault is upstream, in whatever
    //               computes ST per glyph, NOT in the fetch.
    //       spans most of 0..255
    //            -> different glyphs really are being addressed; continue.
    //       collapses to one or two values
    //            -> the per-pixel interpolation between the (correct) corners
    //               is broken; ~10 lines in drawSprite, not the fetch.
    //
    //   (2) THE T4 FETCH -- hist16, idxmax, oor
    //       hist bin0 == tfsamp  (everything else 0)
    //            -> THE PSMT4 NIBBLE FETCH IS BROKEN. Every texel reads index
    //               0 = fully transparent = invisible text over a flat box.
    //               This is the top suspect and this is its exact signature.
    //       oor > 0 or idxmax > 15
    //            -> ReadTexturePageCache is not masking to a nibble for T4;
    //               the index runs off the end of a 16-entry CLUT.
    //       histogram broadly matches the VRAM ground truth above
    //            -> the fetch is CORRECT. Go to (3).
    //
    //   (3) THE T4 CLUT LOOKUP -- a0, amax, white
    //       a0 == tfsamp (every post-CLUT alpha zero) while hist is healthy
    //            -> the indices are right but ReadClutCache returns a
    //               transparent entry for all of them. Suspect csa/cpsm
    //               pairing on the T4 path (csa is echoed below).
    //       amax == 128 and white == tfsamp
    //            -> the sampler is returning exactly the white coverage ramp
    //               PCSX2 shows. Fetch AND lookup are both correct and the
    //               loss is downstream in compositing -- which, given MODULATE
    //               and the blend equation are already verified, would point
    //               at writePixel / the framebuffer write.
    //       white < tfsamp
    //            -> some samples came back non-white; the palette in use is
    //               not the ramp [clutlive] verified. Read csa/cpsm.
    //
    //   (4) RIVAL READING -- tfdis vs tfchk
    //       tfdis > 0, tfdis < tfchk
    //            -> the page cache disagrees with VRAM on some texels. badu/
    //               badv/badc/badv2 localise it. THIS is what licenses
    //               touching the texture cache.
    //       tfdis == tfchk (100%) with badv2 == 0
    //            -> does NOT convict the cache. Far more likely gs->ReadVram
    //               has no PSMT4 read handler and returns 0 for every texel,
    //               i.e. the RIVAL is broken. Verify m_read_address_funcs
    //               for 0x14 before believing anything.
    constexpr uint32_t kTfTbw = 4u;   // 256-wide atlas => tbw 4
    constexpr uint32_t kTfDim = 256;  // 1<<tw and 1<<th
    std::atomic<uint64_t> g_tfAll{0};   // GUARD: paletted samples, any psm
    std::atomic<uint64_t> g_tfT4{0};    // GUARD: T4 samples, any shape
    std::atomic<uint64_t> g_tfSamp{0};  // GUARD: T4 samples at the glyph shape
    std::atomic<uint64_t> g_tfHist[16] = {};
    std::atomic<uint64_t> g_tfOor{0};
    std::atomic<uint32_t> g_tfIdxMax{0};
    std::atomic<uint64_t> g_tfChk{0};
    std::atomic<uint64_t> g_tfDis{0};
    std::atomic<uint32_t> g_tfBadU{0xFFFFFFFFu};
    std::atomic<uint32_t> g_tfBadV{0xFFFFFFFFu};
    std::atomic<uint32_t> g_tfBadCache{0};
    std::atomic<uint32_t> g_tfBadVram{0};
    std::atomic<uint32_t> g_tfUmin{0xFFFFFFFFu};
    std::atomic<uint32_t> g_tfUmax{0};
    std::atomic<uint32_t> g_tfVmin{0xFFFFFFFFu};
    std::atomic<uint32_t> g_tfVmax{0};
    std::atomic<uint32_t> g_tfTbw{0};
    std::atomic<uint32_t> g_tfTw{0};
    std::atomic<uint32_t> g_tfTh{0};
    std::atomic<uint32_t> g_tfTbp{0};
    std::atomic<uint32_t> g_tfCsa{0};
    std::atomic<uint32_t> g_tfCpsm{0};
    std::atomic<uint64_t> g_tfA0{0};    // post-CLUT alpha == 0
    std::atomic<uint32_t> g_tfAMax{0};  // post-CLUT alpha max (128 == 1.0)
    std::atomic<uint64_t> g_tfWhite{0}; // post-CLUT RGB == 255,255,255

    inline void tfMin(std::atomic<uint32_t> &slot, uint32_t v)
    {
        uint32_t cur = slot.load(std::memory_order_relaxed);
        while (v < cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) { }
    }
    inline void tfMax(std::atomic<uint32_t> &slot, uint32_t v)
    {
        uint32_t cur = slot.load(std::memory_order_relaxed);
        while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) { }
    }

    // ---- [glyphfate] -- Stage 5.11 run 25 -----------------------------------
    //
    // WHY. Run 24's [texfetch] closed the last arithmetic link. Measured on
    // 115,043,964 glyph samples at the T4 256x256 tbw=4 shape gate:
    //   * hist16 matches the VRAM ground-truth atlas rank-for-rank, and bins
    //     1, 2 and 13 are zero on BOTH sides. The nibble fetch is correct.
    //   * idxmax=15, oor=0 -- masking correct. u=0..232 v=0..128 -- different
    //     glyphs really do address different atlas cells.
    //   * tfdis=0 over 1,797,562 rival ReadVram reads -- page cache agrees
    //     with VRAM. Not a silent probe; tfchk was large.
    //   * a0 == bin0 to the digit (93,409,752) and white == tfsamp - bin0 to
    //     the digit (21,634,212). Every ink texel returns white with alpha up
    //     to amax=128 (== 1.0); every index-0 texel returns alpha 0.
    //
    // So the sampler hands the pixel loop CORRECT white glyph pixels with
    // correct coverage alpha. TEST decodes to 0x30003 = ATE on/ATST ALWAYS,
    // ZTE on/ZTST ALWAYS -- both always-pass. Blend 0x44 = (Cs-Cd)*As/128 + Cd
    // with Cs = white gives white text over the maroon box. By every value we
    // have measured, these pixels should be VISIBLE.
    //
    // That leaves only WHERE they land and WHAT buries them. No arithmetic
    // suspect survives, so this probe measures fate and ordering, nothing else.
    //
    // The gate is t_glyphDraw, set in drawSprite from the SAME shape predicate
    // [texfetch] just validated on 115M samples (feedback_probe_gate_on_shape_
    // not_address). It mirrors t_boxDraw exactly, including the clear after
    // the pixel loop.
    //
    // UNITS. gfx0/gfy0/gfx1/gfy1 are SCREEN pixels of STORED glyph pixels only
    // (post-scissor, post-tests, at the store site). srca is PS2 alpha, 128 ==
    // 1.0. Sequence numbers share g_writeSeq with [fbdest], so they are
    // directly comparable to each other and to the interval total.
    //
    // REFERENCE RECT. The dump puts the dialog box at 464x120 @ (24,302), so
    // stored glyph pixels must land inside (24,302)-(488,422).
    //
    // PRE-REGISTERED READING -- take the branches in this order.
    //
    //   (0) GUARDS. gfin -> gfstored.
    //       gfin == 0    -> glyph sprites never reach writePixel. Either the
    //                      drawSprite gate is wrong, or the text is not drawn
    //                      as sprites at all (triangles take a different path
    //                      and would never set t_glyphDraw). EVERYTHING below
    //                      is stale. Read gfdraws: 0 there too means the gate;
    //                      nonzero means the pixel loop is empty (clipped).
    //       gfdraws      -> glyph SPRITES that passed the shape gate. Nonzero
    //                      with gfin == 0 is a fully-clipped rect.
    //
    //   (1) FATE. gfscis + gfate + gfz + gfstored should account for gfin.
    //       gfscis == gfin -> every glyph pixel is scissored away. Compare the
    //                      echoed scissor rect against (24,302)-(488,422); a
    //                      stale or mis-decoded SCISSOR is then the whole bug.
    //       gfate > 0     -> CONTRADICTS test=0x30003 ALWAYS. classifyAlphaTest
    //                      is mis-decoding ATST. Read gftest.
    //       gfz > 0       -> the depth test is killing text despite ZTST
    //                      ALWAYS. Same contradiction, different field.
    //       gfstored == gfin -> nothing is discarded. Go to (2).
    //
    //   (2) PLACEMENT. gfx0..gfy1 vs the reference rect.
    //       bbox inside (24,302)-(488,422)
    //            -> the text IS written to the correct screen pixels. Go to (3).
    //       bbox elsewhere on screen
    //            -> a positioning bug: correct glyphs, wrong destination. The
    //               fault is in the sprite XY, not in any texture stage.
    //       bbox degenerate (x0 > x1)
    //            -> nothing was ever stored; re-read (1), one branch ate it all.
    //
    //   (3) VALUE. gfwhite / gfdark / gfpix / gfdst.
    //       gfwhite == 0 while srcamax == 128
    //            -> white source, covered alpha, yet every stored pixel is
    //               dark. The blend or the pack is destroying it. gfpix is the
    //               exact stored word and gfdst the destination under it.
    //               NOTE the COLCLAMP==0 path in this function discards the
    //               blend result (keeps unblended r/g/b) -- gfcc echoes
    //               COLCLAMP so that bug can be confirmed or excluded here.
    //       gfwhite > 0
    //            -> correct white pixels are being stored at correct screen
    //               coordinates. Nothing in the draw is wrong. Go to (4).
    //
    //   (4) BURIAL / TARGET. gffbp*, gflastseq, gfboxseq, and the interval's
    //       total g_writeSeq (printed by [fbdest]).
    //       gffbpor != the presented fbp
    //            -> the glyphs go to a framebuffer that is never displayed.
    //               gffbpmin != gffbpmax means they are split across buffers.
    //       gfboxseq > gflastseq
    //            -> THE BOX IS DRAWN AFTER THE TEXT and paints over it. A
    //               draw-ORDER bug, entirely outside the texture pipeline.
    //               This is the leading hypothesis given (1)-(3) pass.
    //       gflastseq far below the interval total, box not implicated
    //            -> something else re-covers the region later; reuse the
    //               [blackwho] tail bbox to name it.
    std::atomic<uint64_t> g_gfDraws{0};   // GUARD: glyph sprites past the gate
    std::atomic<uint64_t> g_gfIn{0};      // GUARD: glyph pixels entering writePixel
    std::atomic<uint64_t> g_gfScis{0};
    std::atomic<uint64_t> g_gfAte{0};
    std::atomic<uint64_t> g_gfZ{0};
    std::atomic<uint64_t> g_gfStored{0};
    std::atomic<uint64_t> g_gfWhite{0};   // stored pixel with all of r,g,b >= 0xC0
    std::atomic<uint64_t> g_gfDark{0};    // stored pixel with RGB == 0
    std::atomic<uint32_t> g_gfSrcAMax{0};
    std::atomic<uint32_t> g_gfX0{0xFFFFFFFFu};
    std::atomic<uint32_t> g_gfY0{0xFFFFFFFFu};
    std::atomic<uint32_t> g_gfX1{0};
    std::atomic<uint32_t> g_gfY1{0};
    std::atomic<uint32_t> g_gfPix{0};     // last stored word for a covered texel
    std::atomic<uint32_t> g_gfDst{0};     // destination under that pixel
    std::atomic<uint32_t> g_gfFbpOr{0};
    std::atomic<uint32_t> g_gfFbpMin{0xFFFFFFFFu};
    std::atomic<uint32_t> g_gfFbpMax{0};
    std::atomic<uint32_t> g_gfTest{0};
    std::atomic<uint32_t> g_gfCc{0};      // COLCLAMP at a glyph store
    std::atomic<uint32_t> g_gfAbe{0};     // PRIM.ABE at a glyph store
    std::atomic<uint32_t> g_gfScX0{0};
    std::atomic<uint32_t> g_gfScY0{0};
    std::atomic<uint32_t> g_gfScX1{0};
    std::atomic<uint32_t> g_gfScY1{0};
    std::atomic<uint64_t> g_gfLastSeq{0}; // g_writeSeq at the last glyph store
    std::atomic<uint64_t> g_gfBoxSeq{0};  // g_writeSeq at the last box store
    thread_local bool t_glyphDraw = false;

    // ---- [fbsplit] -- Stage 5.11 run 26 -----------------------------------
    //
    // WHAT RUN 25 SETTLED. Nothing kills the glyph pixels: gfscis, gfate and
    // gfz were all exactly 0 and gfstored == gfin (26,509,392 of 26,509,392).
    // 3.3M of them stored as white (pix=0x80fffffe) inside the dialog rect
    // (bbox 25,306-491,378) under a full-screen scissor. The text is drawn,
    // it is white, it is in the right place, and it is never discarded.
    //
    // TWO FIELDS FROM RUN 25 POINT SOMEWHERE VERY SPECIFIC:
    //
    //   dst=0x0    The destination under a white glyph pixel is BLACK. If the
    //              maroon box had been painted into that pixel first, dst
    //              would be maroon. So at the moment the glyph lands, nothing
    //              else has drawn there.
    //   fbpmin=0 fbpmax=112
    //              Glyph stores go to TWO different destination framebuffers.
    //              ps2_gs_gpu.cpp's [dispfb] note already records that
    //              FRAME.FBP alternates 0x0/0x70 per frame while DISPFB stays
    //              constant at 0x1070 -- and 0x1070 & 0x1FF == 112.
    //
    // Together those say: the box and the text may be landing in DIFFERENT
    // buffers in the same frame, and only the box's buffer is presented. That
    // renders exactly the observed symptom -- a flat maroon box.
    //
    // WHY RUN 25's boxafter CANNOT BE USED TO TEST THIS. It alternated 0/1
    // across records with two perfectly constant deltas (272089 and 294656),
    // which no real draw-order change produces. The cause is that
    // g_writeSeq is `exchange(0)`'d by the [fbdest] emit (ps2_gs_gpu.cpp),
    // so the counter both stamps resets asynchronously between the box store
    // and the glyph store. run 25's `total=0` is the same defect visible
    // directly: the total was read after [fbdest] had already zeroed it. This
    // probe therefore uses its OWN order counter that nothing else resets.
    //
    // WHY min/max IS THE WRONG SHAPE HERE. fbpmin/fbpmax collapse a
    // population into two extremes: they cannot say whether fbp 0 took one
    // stray pixel or half of them, and they cannot be compared against the
    // box at all. These are per-fbp POPULATIONS for both sprites, so the two
    // distributions can be laid side by side.
    //
    // READING ORDER -- stop at the first line that answers:
    //   1 GUARDS   glyph= and box= empty -> the dialog was not up in this
    //              interval; read a later record, do not conclude.
    //   2 TARGET   the two fbp lists. Disjoint -> split-target bug, done.
    //   3 DISPLAY  shared fbp vs disp1/disp2. Not equal -> the whole dialog
    //              is drawn into a buffer that is never shown.
    //   4 ORDER    within the shared fbp, boxord vs glyphord. Box later ->
    //              genuine draw-order bug (now measured on a sound counter).
    //   5 VALUES   dstor is the OR of everything underneath the white glyph
    //              pixels; boxpix is the word the box stores. If dstor is 0
    //              while boxpix is maroon, the two never met.
    std::atomic<uint32_t> g_fsGlyphByFbp[512]; // white glyph stores per dest fbp
    std::atomic<uint32_t> g_fsBoxByFbp[512];   // box stores per dest fbp
    std::atomic<uint64_t> g_fsGlyphOrd[512];   // order stamp, last white glyph store
    std::atomic<uint64_t> g_fsBoxOrd[512];     // order stamp, last box store
    std::atomic<uint64_t> g_fsOrdSeq{0};       // dedicated -- NOTHING resets this
    std::atomic<uint32_t> g_fsGlyphDstOr{0};   // OR of dest under white glyphs
    std::atomic<uint32_t> g_fsBoxPix{0};       // last word the box stored

    // ---- [boxover] -- Stage 5.11 run 27 -----------------------------------
    //
    // WHAT RUN 26 SETTLED, AND WHAT IT DID NOT.
    //
    // The populations were clean and they killed the split-target theory:
    //   glyph white stores  85232 / 154483 / 159810 / 165137 == 5327 x
    //                       16 / 29 / 30 / 31
    //   box stores          1614720 / 1670400 / 1726080     == 55680 x
    //                       29 / 30 / 31        (55680 == 464 x 120)
    // So 5327 white pixels is one complete dialog's worth of text, and per
    // interval the text is drawn the SAME number of times as the box, into
    // BOTH framebuffers, in near-equal proportion. One box plus one full text
    // pass per frame. Nothing is offscreen and nothing is split.
    //
    // Run 26's verdict= field said otherwise and it was WRONG. It picked a
    // "top" fbp, and the two buckets were near-ties in every record
    // (154483 vs 159810), so SPLIT-TARGET and OFFSCREEN were tie-break noise.
    // Same failure class as run 25's boxafter. Do not cite either field.
    // The populations under the verdict are sound; the verdict was not.
    //
    // THE FIELD THAT ACTUALLY DECIDED IT: dstor == 0x80000000, the OR of the
    // destination across every white glyph store (314,293 samples in one
    // interval), exactly black-opaque. Not one white glyph pixel ever landed
    // on maroon, and boxpix == 0x66200e64 (r=100 g=14 b=32) IS the maroon.
    // The two sprites overlap in space -- glyph bbox 25,306-491,378 sits
    // inside the box rect 24,302-488,422 -- and go to the same buffers at the
    // same rate, yet the text never sees the box beneath it.
    //
    // THE RECIPROCAL MEASUREMENT, WHICH IS ALL THIS PROBE IS. Run 26 sampled
    // the destination under the TEXT. This samples the destination under the
    // BOX. The two answers together settle it without any ordering counter at
    // all, which is why no ordering counter appears here -- run 25 and run 26
    // both produced a bad answer from one, and interval-granular order stamps
    // straddle ~30 frames so they cannot see within-frame order anyway.
    //
    //   bwhite > 0, and close to the [fbsplit] glyph white count
    //       -> BOX PAINTS OVER TEXT. The text is drawn first onto a cleared
    //          buffer and the box is then laid on top of it. Draw order is
    //          inverted, or the box should be blending and is not.
    //   bwhite == 0 and bblack accounts for essentially every box store
    //       -> SOMETHING CLEARS BETWEEN THEM. The box is innocent; a wipe
    //          between the text pass and the box pass destroys the glyphs.
    //          That is a completely different bug and a different fix site.
    //
    // Those two outcomes are mutually exclusive and both are actionable, so
    // this probe cannot come back with "no information".
    //
    // bother exists so the two named buckets cannot silently disagree with
    // the total: bwhite + bblack + bother == bstores must hold exactly. If it
    // does not, the classifier is broken and no other field here is
    // trustworthy -- check that identity FIRST.
    //
    // babe/balpha/bsrca describe the box draw itself, and only matter in the
    // BOX-PAINTS-OVER-TEXT branch: they say whether the box was entitled to
    // occlude. ALPHA 0x44 is (Cs-Cd)*As/128 + Cd, so with abe=1 and a source
    // alpha of 0x66 (102/128 = 80%) roughly a fifth of the text should still
    // show through -- which is NOT what the screen shows. abe=0 would mean a
    // straight opaque overwrite and would explain a perfectly flat box.
    std::atomic<uint64_t> g_boStores{0};  // box stores classified (denominator)
    std::atomic<uint64_t> g_boWhite{0};   // ...onto a white-ish destination
    std::atomic<uint64_t> g_boBlack{0};   // ...onto a destination with RGB == 0
    std::atomic<uint64_t> g_boOther{0};   // ...onto anything else
    std::atomic<uint32_t> g_boDstOr{0};   // OR of destination under box stores
    std::atomic<uint32_t> g_boDstWhite{0}; // a destination that classified white
    std::atomic<uint32_t> g_boAbe{0};     // PRIM.ABE at a box store
    std::atomic<uint32_t> g_boSrcAMax{0}; // max source alpha fed to the blender
    std::atomic<uint32_t> g_boAlphaReg{0}; // ctx.alpha low word at a box store

    // ---- [fbaddr] -- Stage 5.11 run 28 -- DO THE TWO SPRITES SHARE MEMORY? --
    //
    // WHAT RUN 27 ACTUALLY SHOWED. bwhite=0 over 3 340 800 box stores per
    // interval: the box never lands on a white pixel. Run 26 showed the
    // reciprocal, dstor=0x80000000: the text never lands on a maroon pixel.
    // Neither sprite ever sees the other. Both draw ~59 times per interval,
    // into both fbp 0 and fbp 112, over overlapping screen rects.
    //
    // THE FIELD THAT MATTERS IS NOT THE VERDICT, IT IS THE RESET VALUE.
    //   glyph destination OR = 0x80000000  (opaque black)
    //   box   destination OR = 0x00000000  (every bit zero, alpha included)
    // A single cleared buffer cannot hand one sprite 0x80 alpha and the other
    // 0x00. Two different reset values means two different destinations --
    // OR a wipe between the passes that clears to a third value. Those are
    // the only two readings left, and this probe separates them.
    //
    // WHY THIS WAS NEVER MEASURED. The framebuffer address is computed from
    // THREE inputs (see the top of writePixel):
    //     fbp  = framePageBaseToBlock(ctx.frame.fbp)
    //     fbw  = max(ctx.frame.fbw, 1)
    //     fpsm = ctx.frame.psm
    // Every probe from run 24 onward recorded only `ctx.frame.fbp & 0x1FF`.
    // fbw and fpsm have never been sampled on either sprite, and the 0x1FF
    // mask discards any high bit that framePageBaseToBlock would have used.
    // So "both sprites hit fbp 0 and fbp 112" was never the same claim as
    // "both sprites write the same bytes" -- it only ever constrained one of
    // the three terms. This closes the other two.
    //
    // OR/AND RATHER THAN MIN/MAX, AND WHY fbp IS ALLOWED TO VARY. OR==AND is
    // an exact constancy test in one pair of words. fbw and fpsm are expected
    // constant; fbp is NOT -- it alternates every frame with double buffering,
    // so requiring it constant would trip a guard in every record and the
    // probe would report nothing forever. Instead the OR/AND PAIR is compared
    // between the two sprites: for the same alternating two-element set both
    // words match, and any difference in the set shows up in one or the other.
    //
    // fbmsk rides along because it sits in the same write path (pixel is
    // merged with the destination through it just below) and a nonzero mask
    // on one sprite only would be its own explanation. One atomic, so it is
    // cheaper to carry than to argue about.
    //
    // THE XY EXTENTS ARE THE PREMISE, NOT A DECORATION. Every reading above
    // assumes the two rects overlap. That came from a bbox note, not from the
    // store site. If the extents come back disjoint the whole line of enquiry
    // was mis-aimed, and the probe says so instead of reporting a false
    // "same target" -- which is why NO-SPATIAL-OVERLAP is a verdict branch.
    //
    // Reset identities per interval: OR -> 0, AND -> 0xFFFFFFFF,
    // min -> 0xFFFFFFFF, max -> 0. Getting these wrong is silent, so the
    // emit site sets each one explicitly rather than exchanging with 0.
    std::atomic<uint64_t> g_faGN{0};   // glyph white stores sampled
    std::atomic<uint64_t> g_faBN{0};   // box stores sampled
    std::atomic<uint32_t> g_faGFbpOr{0}, g_faGFbpAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faBFbpOr{0}, g_faBFbpAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faGFbwOr{0}, g_faGFbwAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faBFbwOr{0}, g_faBFbwAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faGPsmOr{0}, g_faGPsmAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faBPsmOr{0}, g_faBPsmAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faGMskOr{0}, g_faGMskAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faBMskOr{0}, g_faBMskAnd{0xFFFFFFFFu};
    std::atomic<uint32_t> g_faGX0{0xFFFFFFFFu}, g_faGX1{0}, g_faGY0{0xFFFFFFFFu}, g_faGY1{0};
    std::atomic<uint32_t> g_faBX0{0xFFFFFFFFu}, g_faBX1{0}, g_faBY0{0xFFFFFFFFu}, g_faBY1{0};

    // ---- [pixlog] -- Stage 5.11 run 29 -- ONE PIXEL, EVERY WRITER, IN ORDER --
    //
    // WHY A PIXEL HISTORY AND NOT ANOTHER AGGREGATE. Runs 24-28 have measured
    // every property of the two sprites and they all agree: same fbp set, same
    // fbw, same fpsm, same fbmsk, overlapping rects, both sprites in BOTH
    // buffers at matched rates (run 28 + [fbsplit] on the same log), and the
    // glyphs stored AFTER the box within a buffer. Yet the destinations stay
    // disjoint and constant -- glyph reads 0x80000000, box reads 0x00000000,
    // never each other, never even the box's own maroon from the previous
    // frame. No property of the two sprites can explain that, because the
    // explanation is a THIRD writer that no sprite-gated probe can see. So
    // this probe drops the sprite gate entirely and follows a single address.
    //
    // WHY THE COORDINATE IS LATCHED, NOT CONSTANT. Hard-coding a pixel is the
    // exact mistake feedback_probe_gate_on_shape_not_address names: a guessed
    // constant reports confidently about the wrong object, and a pixel picked
    // from a bbox can easily land in the GAP BETWEEN two glyphs, where the
    // text never writes and the log would look like "the glyphs never drew".
    // Instead the first glyph-WHITE store of each interval latches its own
    // coordinate. The address is therefore guaranteed to be one the text
    // actually covers, and it is printed, so the reader never has to trust it.
    // Re-latched every interval because the dialog can move.
    //
    // WHY A RING AND NOT A COUNTER. A counter says how many; the question is
    // WHICH ORDER and BY WHOM, and the last writer decides the colour. The
    // ring keeps the most recent kPlRing stores, so it always describes whole
    // recent frames rather than the start of the interval -- and being a ring
    // it cannot saturate, which is the failure mode
    // feedback_capped_probes_false_negatives describes.
    //
    // prim rides along so an untagged (third-party) writer can be identified:
    // tme=0 on a full-screen value is a clear, tme=1 is another sprite.
    //
    // Reset identities per interval: count -> 0, every ord slot -> 0 (a zero
    // ord means "slot never filled" and is skipped at emit), armed/claim -> 0,
    // x/y/fbp -> 0xFFFFFFFF so a stale coordinate can never be mistaken for a
    // live one.
    // ---- run 30 upgrade: TWO points, and the TEXTURE IDENTITY per entry ----
    //
    // Run 29 answered the ordering question completely. The per-frame cycle at
    // a text pixel is, in both buffers, on every interval:
    //     clear 0x00000000 prim 0x06 -> box 0x66200e64 prim 0x56
    //       -> OTHER 0x80000000 prim 0x56 -> glyph 0x80fffffe prim 0x56
    // so runs 26/27's "disjoint destinations" were never a contradiction (box
    // follows the clear, glyph follows the black sprite), and the glyph really
    // is the last writer. What run 29 could NOT do is say WHICH draw the black
    // sprite is: prim 0x56 (SPRITE|TME|ABE) is shared by all three writers, so
    // prim does not discriminate. TEX0 does.
    //
    // WHY t_lastTexel IS THE FIELD THAT MATTERS. It is the texel as fetched,
    // before blending. Against the stored value it splits the two remaining
    // explanations cleanly, and neither can hide behind the other:
    //     texel black, stored black  -> the FETCH is degenerate (T4 index or
    //                                   CLUT), the blender is innocent
    //     texel coloured, stored black -> the fetch is fine and the BLEND is
    //                                   destroying it
    // tbp0/cbp/psm/tbw additionally name the draw, and those are directly
    // comparable against dump.jsonl, which holds the PCSX2 ground truth for
    // this exact screen.
    //
    // WHY TWO POINTS. Run 29 latched (25,308) -- the min corner of the glyph
    // rect (25,306-480,377), i.e. very likely a border decoration from the
    // same atlas rather than body text. One sample cannot tell "the dialog
    // does this" from "this corner does this". Point 1 latches a glyph store
    // at least 120px to the right, so it is a different glyph by construction.
    // If point 1 never arms it says so loudly instead of reporting point 0's
    // story twice.
    constexpr uint32_t kPlRing = 8;
    constexpr uint32_t kPlPts = 2;
    std::atomic<uint32_t> g_plClaim[kPlPts];  // CAS gate so exactly one store latches
    std::atomic<uint32_t> g_plArmed[kPlPts];  // published last; gates the log site
    std::atomic<uint32_t> g_plX[kPlPts];
    std::atomic<uint32_t> g_plY[kPlPts];
    std::atomic<uint32_t> g_plFbp[kPlPts];
    std::atomic<uint64_t> g_plCount[kPlPts];      // stores that hit the latched pixel
    std::atomic<uint64_t> g_plGlyphHits[kPlPts];  // ... of which were glyph draws
    std::atomic<uint64_t> g_plOrd[kPlPts][kPlRing];
    std::atomic<uint32_t> g_plTag[kPlPts][kPlRing];    // 0 = other, 1 = box, 2 = glyph
    std::atomic<uint32_t> g_plVal[kPlPts][kPlRing];    // word stored
    std::atomic<uint32_t> g_plDst[kPlPts][kPlRing];    // destination it replaced
    std::atomic<uint32_t> g_plPrim[kPlPts][kPlRing];
    std::atomic<uint32_t> g_plTexel[kPlPts][kPlRing];  // t_lastTexel -- BEFORE blending

    // ---- [drawpath] -- Stage 5.11 run 32 state. Capture window is one frame.
    //
    // Run 38 re-gate. Runs 32-34 opened the window on the first role-1 (box)
    // draw. That made the run-34 verdict unfalsifiable: a background drawn
    // BEFORE the box could never enter the ring, so "background after box" was
    // produced by the gate, not measured -- the exact failure
    // feedback_probe_gate_on_shape_not_address describes. The window now opens
    // on a frame boundary (the untextured full-screen clear, or the display
    // copy that ends the previous frame), both of which precede every sprite in
    // the frame, so whatever the guest draws first lands at index 0.
    //
    // The ring is sized for a whole dialog frame: 1 background + 9 box/border
    // strips + ~147 glyph quads = ~157 textured sprites, plus headroom.
    constexpr uint32_t kDpRing = 320;
    constexpr uint32_t kDpRoles = 10;
    std::atomic<uint32_t> g_dpArm{0};        // frame boundary seen, open on next sprite
    std::atomic<uint32_t> g_dpCount{0};      // valid slots in the latched window
    std::atomic<uint64_t> g_dpArmClear{0};   // times the clear opened a window
    std::atomic<uint64_t> g_dpArmCopy{0};    // times the display copy opened one
    std::atomic<uint32_t> g_dpPrevRole{0};
    std::atomic<uint32_t> g_dpCapturing{0};
    std::atomic<uint32_t> g_dpReady{0};
    std::atomic<uint32_t> g_dpIdx{0};
    std::atomic<uint64_t> g_dpRoleSeen[kDpRoles];
    std::atomic<uint32_t> g_dpRole[kDpRing];
    std::atomic<uint32_t> g_dpPath[kDpRing];   // 0 = unknown, else GifPathId
    std::atomic<uint32_t> g_dpSpanX[kDpRing];
    std::atomic<uint32_t> g_dpSpanY[kDpRing];
    std::atomic<uint32_t> g_dpTbp[kDpRing];
    std::atomic<uint32_t> g_dpSite[kDpRing];  // submitGifPacket call site
    std::atomic<uint32_t> g_dpSrc[kDpRing];   // run 34: EE address of this draw
    std::atomic<uint32_t> g_plTbp[kPlPts][kPlRing];
    std::atomic<uint32_t> g_plCbp[kPlPts][kPlRing];
    std::atomic<uint32_t> g_plTfmt[kPlPts][kPlRing];   // psm | tbw<<8 | tfx<<16 | tcc<<20 | cpsm<<24
    std::atomic<uint32_t> g_plAlpha[kPlPts][kPlRing];  // a | b<<2 | c<<4 | d<<6 | fix<<8

    // ---- [uvspan] -- Stage 5.11 run 23 (RE-POINTED at the glyph sprites) --
    //
    // WHY THIS PROBE WAS REWRITTEN. Runs 21 and 22 both reported
    // "draws=0 fst=4294967295" and every value field at zero. That was never
    // a statement about the renderer: the old gate was
    //     tex.psm == GS_PSM_T8 && tex.tbp0 == kTfTbp (0x2A80)
    // and the sprites that draw the memory-card TEXT are
    //     psm = T4 (0x14), tbw = 4, 256x256, tbp0 = 0x2B60.
    // The gate never matched a single glyph. Exactly the failure mode
    // feedback_probe_gate_on_shape_not_address describes: an inferred address
    // constant in a gate does not fail loudly, it reports confidently about a
    // different object. The gate below now contains NO address at all.
    //
    // GROUND TRUTH (PCSX2 GS dump of this exact screen, decoded offline by
    // build_scripts/gsdump_draws.py -- none of it comes from our renderer):
    //
    //     prim=0x056  sprite  tme=1 abe=1 iip=0  ***fst=0***
    //     TEX0: tbp0=0x2B60 tbw=4 psm=T4 256x256 tcc=1 tfx=0
    //           cbp=0x2B08 cpsm=CT32 csm=0 csa=0 cld=2
    //     vtx0: uv=(0,0)  st=(0.101961, 0.000000) q=1.0  rgba=(0,0,0,0)
    //     vtx1: uv=(0,0)  st=(0.196078, 0.094118) q=1.0  rgba=(128,128,128,128)
    //     screen rect 17 x 17 px
    //
    // Read that carefully: fst=0, and UV is literally (0,0) on BOTH vertices.
    // The game addresses the glyph atlas ONLY through ST/Q. The UV register is
    // never written for these draws. So if our vertex assembly loses s/t/q,
    // or if prim.fst decodes as 1, every glyph samples texel (0,0) -- which in
    // this atlas is T4 index 0, which in this CLUT is alpha 0, fully
    // transparent. Invisible text over a bare maroon box. That is the exact
    // reported symptom, which is why this probe exists.
    //
    // Floats are stored as fixed-point integers to avoid any float-printing
    // or memcpy/bit-cast dependency at the emit site. DIVISORS:
    //     u0/v0/u1/v1, dumin/dumax  -> divide by 256    (1/256 texel)
    //     s0/t0/q0/s1/t1/q1, dsmin/dsmax -> divide by 65536  (ST and Q)
    // rawu0/rawv0/rawu1/rawv1 are the UV registers verbatim, 4.4 fixed point,
    // NOT scaled -- divide by 16 for texels.
    //
    // Because u = s * texW and texW = 256 here, the SAME number comes out of
    // both scales: ds(/65536) and du(/256) must agree numerically on a healthy
    // draw. That is a free internal cross-check, not a second measurement.
    //
    // EXPECTED VALUES ON A HEALTHY RUN, derived from the dump above:
    //     fst0 == draws, fst1 == 0
    //     ds/du span for one 17px glyph cell = 0.094118 * 65536 ~= 6169
    //       (cells vary a little; anything in 3000..7000 is a real span)
    //     q64k = 65536 on both corners
    //     wide == draws, flat == 0
    //
    // READING TABLE (write once, before the run). Read STRICTLY in order.
    //
    // (0) GUARDS -- allspr / t4any / draws. Read these three FIRST.
    //   allspr == 0 -> no textured sprite reached drawSprite at all. The probe
    //       describes nothing. Do not read one field below it. Something is
    //       wrong with the probe's placement, not with texturing.
    //   allspr > 0 but t4any == 0 -> textured sprites exist but NONE is psm
    //       T4. Either the screen never came up in this run, or our TEX0 psm
    //       decode is wrong. Check psm decode before believing anything else;
    //       the dump says T4 is 128+ draws on this screen.
    //   t4any > 0 but draws == 0 -> psm is right, the DIMENSIONS are not.
    //       tbw/tw/th are being decoded wrong. That alone would corrupt every
    //       texel address. Compare against the census in [boxtex].
    //   draws > 0 -> the gate is on the glyph sprites. Proceed to (1).
    //
    // (1) THE PRIMARY QUESTION -- fst0 vs fst1.
    //   fst1 > 0 -> ***WE DECODE PRIM WRONG.*** The dump proves fst=0 on every
    //       glyph sprite. Taking the UV branch reads v0.u/v1.u, which the game
    //       never wrote, so both corners land on texel (0,0) = index 0 =
    //       transparent. This is the bug; fix is in PRIM decode / prim.fst
    //       propagation, NOT in any texturing code.
    //   fst0 == draws -> PRIM decode is correct. Go to (2).
    //
    // (2) DID ST ACTUALLY ARRIVE? -- dsmin/dsmax, and flat/wide.
    //   dsmax < 256 (i.e. under 0.004 in ST) -> ***ST WAS NEVER LATCHED.***
    //       Both corners carry the same (or zero) ST, so every glyph samples
    //       one texel. Fault is upstream in GIF vertex assembly: the six
    //       vtx.s/.t/.q fill sites in ps2_gs_gpu.cpp, or the packed-ST decode.
    //       Look there; drawSprite is innocent.
    //   dsmax in 3000..7000 and dsmin likewise -> ST arrived intact with the
    //       span the dump predicts. The coordinates are RIGHT and the fault is
    //       downstream in sampleTexture / the T4 index fetch. Stop looking at
    //       vertex assembly.
    //   dsmin tiny while dsmax healthy -> only SOME glyphs lose their span.
    //       Believe the counters, not the last-observed corner values below,
    //       and find what distinguishes the flat draws.
    //
    // (3) THE PERSPECTIVE DIVIDE -- qbad, q64k.
    //   qbad > 0 -> at least one corner had |q| < 0.015, so u = s/q blows up
    //       into garbage that clamps to a single texel. A q64k of exactly 0
    //       means Q was never written by RGBAQ at all. Note fabsQ() substitutes
    //       1.0 below 1e-8, so an exact-zero Q is SILENTLY survivable -- qbad
    //       is the only thing that will tell you it happened.
    //   qbad == 0 and q64k == 65536..65536 -> Q is correct; the divide is a
    //       no-op exactly as the dump says it should be.
    //
    // (4) RIVAL READING -- rawu/rawv.
    //   These are captured on EVERY gated draw regardless of which branch fst
    //   selected, so they are an independent reading and not a copy. The dump
    //   says they must be 0..0. If rawu is NONZERO while fst0 == draws, then
    //   the game is writing UV we did not expect and the dump and the live run
    //   disagree about the draw itself -- which would invalidate the ground
    //   truth this whole table is built on. Report that, do not fix anything.
    std::atomic<uint64_t> g_uvAllSpr{0};
    std::atomic<uint64_t> g_uvT4Any{0};
    std::atomic<uint64_t> g_uvDraws{0};
    std::atomic<uint64_t> g_uvFlat{0};
    std::atomic<uint64_t> g_uvWide{0};
    std::atomic<uint64_t> g_uvFst0{0};
    std::atomic<uint64_t> g_uvFst1{0};
    std::atomic<uint64_t> g_uvQBad{0};
    std::atomic<uint32_t> g_uvDuMin{0xFFFFFFFFu};
    std::atomic<uint32_t> g_uvDuMax{0};
    std::atomic<uint32_t> g_uvDsMin{0xFFFFFFFFu};
    std::atomic<uint32_t> g_uvDsMax{0};
    std::atomic<uint32_t> g_uvFst{0xFFFFFFFFu};
    std::atomic<int32_t> g_uvU0{0};
    std::atomic<int32_t> g_uvV0{0};
    std::atomic<int32_t> g_uvU1{0};
    std::atomic<int32_t> g_uvV1{0};
    std::atomic<int32_t> g_uvS0{0};
    std::atomic<int32_t> g_uvT0{0};
    std::atomic<int32_t> g_uvQ0{0};
    std::atomic<int32_t> g_uvS1{0};
    std::atomic<int32_t> g_uvT1{0};
    std::atomic<int32_t> g_uvQ1{0};
    std::atomic<int32_t> g_uvRawU0{0};
    std::atomic<int32_t> g_uvRawV0{0};
    std::atomic<int32_t> g_uvRawU1{0};
    std::atomic<int32_t> g_uvRawV1{0};
    std::atomic<int32_t> g_uvSpanX{0};
    std::atomic<int32_t> g_uvSpanY{0};

    // ---- [boxtex] -- Stage 5.11 run 22 -----------------------------------
    // This is the first probe in this stage with a fully independent ground
    // truth. A PCSX2 GS dump of the same screen was decoded offline, so every
    // expected value below is known BEFORE the run and none of it is derived
    // from our own renderer. Ground truth for the red box behind "No Data":
    //
    //     screen rect (24.0, 301.5) - (488.0, 421.5)   => 464 x 120
    //     TEX0: tbp0=0x2B20 tbw=2 psm=T8(0x13) tw=th=128 tcc=1 tfx=0
    //           cbp=0x2B04 cpsm=CT32(0x00) csm=0 csa=0 cld=2
    //     texel sampled at (72, 8) of the 128x128 upload
    //     texel INDEX = 50 (0x32)
    //     CLUT 0x2B04 entry 50 = 0x8017176F  (R=111 G=23 B=23 A=128, RED)
    //
    // What we currently draw there is 0x80000000 -- opaque black -- which is
    // exactly entry 8 of that same palette. So the palette we hold may well be
    // right; the index we look up is 8 when it should be 50.
    //
    // READING TABLE (write once, before the run). Read STRICTLY in order.
    //
    // (1) GUARD -- all / draws.
    //   all == 0    -> no textured sprite reached drawSprite at all. The probe
    //                  describes nothing; do not read a single field below.
    //   draws == 0  -> the rect gate missed. This is NOT a statement about the
    //                  box. Go straight to the census (2), which is gated only
    //                  on `all` and therefore still valid, and re-derive the
    //                  gate from it. This is the whole reason the census
    //                  exists: a zero here must still teach something.
    //
    // (2) CENSUS -- cen[i]=tbp/psm/cbp:count over EVERY textured sprite.
    //     Independent of the rect gate. Ground truth says the frame contains
    //     exactly three combos, and nothing else:
    //         tbp=0x2B60 psm=T4  cbp=0x2B08   852 draws
    //         tbp=0x2B20 psm=T8  cbp=0x2B04    36 draws
    //         tbp=0x2A00 psm=T8  cbp=0x2B00     4 draws
    //   census matches those three
    //       -> TEX0 delivery is correct end to end. Suspect #16 dies here and
    //          the fault is below TEX0: addressing or the CLUT lookup.
    //   census shows tbp values not in that list
    //       -> ***suspect #16 convicted***. The wrong texture base is reaching
    //          the draw. Stop; the bug is in GIF register decode / context
    //          selection, upstream of every texturing routine.
    //   census right tbp but wrong cbp
    //       -> TEX0 arrives intact but CLD/CBP handling picks the wrong
    //          palette. Look at ReloadClutCache's cld==2 path.
    //   cenovf > 0
    //       -> more than kBxCen distinct combos; the table is truncated and
    //          absence of a combo from it proves nothing.
    //
    // (3) TEX0 of the last box draw -- tex0 (raw u64) and the decoded fields.
    //     Compare field by field against the ground truth above. Any single
    //     mismatched field localises the bug on its own.
    //
    // (4) INDEX -- idxmin/idxmax/idxhist/idxsamp, captured for box pixels only.
    //   idxsamp == 0 -> no paletted texel was fetched inside a box draw even
    //          though draws > 0. The gate fired but the loop did not sample;
    //          treat idxmin/idxmax/hist as meaningless.
    //   idxmax <= 15 (all weight in hist bucket 0)
    //       -> reproduces [texfetch]'s finding on the real texture. Indices
    //          are too small. Truth is 50 (bucket 3). The fault is in texel
    //          ADDRESSING -- ReadTexturePageCache's block math at
    //          ps2_gs_gpu.cpp:4599, (tbp0 + page_id * 32) & 0x3FFF.
    //   idxmin..idxmax brackets 50 with weight in bucket 3
    //       -> addressing is right and the index is right. The fault is the
    //          index -> colour step. Read (5).
    //
    // (5) PALETTE -- ent50 / ent8, read via the same ReadClutCache the sampler
    //     uses, with the box draw's own cpsm/csa.
    //   ent50 == 0x8017176F -> our palette is correct AND correctly addressed.
    //          Combined with a wrong idxmax this is conclusive: addressing.
    //   ent50 == 0x80000000 and ent8 == 0x80000000
    //       -> we are holding a palette whose low entries are all black; we
    //          have the wrong CLUT, or it was never reloaded for this draw.
    //   ent50 some other colour
    //       -> right CLUT base, wrong entry ordering. Only now does the CSM1
    //          swizzle question matter -- and note the dump says BOTH candidate
    //          orderings give red, so a black ent50 cannot be a swizzle bug.
    //
    // (6) texel = the final RGBA the box loop actually wrote, after CLUT and
    //     TEXA. If ent50 is red but texel is black, the loss is in applyTexa
    //     or combineTexture, downstream of everything above.
    constexpr int kBxCen = 12;
    std::atomic<uint64_t> g_bxAll{0};
    std::atomic<uint64_t> g_bxDraws{0};
    std::atomic<uint64_t> g_bxCenKey[kBxCen] = {};
    std::atomic<uint64_t> g_bxCenCnt[kBxCen] = {};
    std::atomic<uint64_t> g_bxCenOvf{0};
    std::atomic<uint64_t> g_bxTex0{0};
    std::atomic<int32_t> g_bxX0{0};
    std::atomic<int32_t> g_bxY0{0};
    std::atomic<int32_t> g_bxSpanX{0};
    std::atomic<int32_t> g_bxSpanY{0};
    std::atomic<uint64_t> g_bxIdxSamp{0};
    std::atomic<uint64_t> g_bxIdxHist[16] = {};
    std::atomic<uint32_t> g_bxIdxMin{0xFFFFFFFFu};
    std::atomic<uint32_t> g_bxIdxMax{0};
    std::atomic<uint32_t> g_bxEnt50{0xDEADBEEFu};
    std::atomic<uint32_t> g_bxEnt8{0xDEADBEEFu};
    std::atomic<uint32_t> g_bxTexel{0xDEADBEEFu};

    // Set for the duration of one box-shaped sprite so the per-texel capture
    // in samplePoint can tell box pixels from everything else. Sprites do not
    // nest, so a plain store/restore is sufficient.
    thread_local bool t_boxDraw = false;
    thread_local bool t_bgSkip = false;  // PS2X_SKIPBG experiment, run 33

    // One census row per distinct (tbp, cbp, psm) seen on a textured sprite.
    // Called once per DRAW, never per pixel. A lost race can at worst duplicate
    // a row or drop a count, which cannot manufacture a combo that never
    // happened -- and inventing combos is the only error that would mislead.
    inline void bxCensus(uint32_t tbp, uint32_t cbp, uint32_t psm)
    {
        const uint64_t key = (1ull << 63)
                           | (static_cast<uint64_t>(tbp & 0x3FFFu) << 40)
                           | (static_cast<uint64_t>(cbp & 0x3FFFu) << 16)
                           | static_cast<uint64_t>(psm & 0x3Fu);
        for (int i = 0; i < kBxCen; ++i)
        {
            const uint64_t cur = g_bxCenKey[i].load(std::memory_order_relaxed);
            if (cur == key)
            {
                g_bxCenCnt[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (cur == 0)
            {
                uint64_t expect = 0;
                if (!g_bxCenKey[i].compare_exchange_strong(expect, key,
                                                           std::memory_order_relaxed))
                {
                    --i;   // another thread claimed it; re-examine this slot
                    continue;
                }
                g_bxCenCnt[i].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        g_bxCenOvf.fetch_add(1, std::memory_order_relaxed);
    }

    // Saturating float -> fixed-point. A NaN or an enormous Q must not become
    // implementation-defined garbage that reads like a plausible coordinate.
    inline int32_t uvFix(float v, float scale)
    {
        const float s = v * scale;
        if (!(s > -2.0e9f && s < 2.0e9f))
            return 0x7FFFFFFF;
        return static_cast<int32_t>(s);
    }

    // ---- [texred] -- Stage 5.11 run 16 -----------------------------------
    // [clutdump] closed the whole CLUT chain: vraml == cachel == readl, and
    // ent0_31 showed both palettes are ordinary font antialias ramps (T8 =
    // black ramp 0x00..0x80 in entries 0..16, T4 = white ramp). The 0x80000000
    // that [texzero] kept reporting is entry 16, the opaque end of a real
    // drop-shadow ramp -- not a lookup failure. Suspects #1..#6 are dead.
    //
    // But the same dump showed the RED IS IN THE PALETTE: T8 entries 20, 24,
    // 27, 29 and 31 are 0x6600002a, 0x66000045, 0x66000069, 0x8000006f and
    // 0x8002026f -- ABGR, so R = 0x2a..0x6f with G = B = 0. Dark red. Those
    // are the box.
    //
    // Every probe we own is blind to them. [texzero] fires only when the texel
    // is black, so it has been describing the font SHADOW layer this whole
    // time and structurally cannot see the box. Worse, writePixel has two
    // uninstrumented early returns ahead of every diag block:
    //     line ~567  !alphaTest.writeFramebuffer  -> return
    //     line ~617  !zpass                       -> return
    // A red pixel killed at either one leaves no trace anywhere. That is the
    // blind spot this probe closes.
    //
    // Trigger is the texel, not the stored pixel: R >= 0x20 with G and B both
    // < 0x20, which matches entries 20/24/27/29/31 and nothing in either ramp.
    //
    // PRE-REGISTERED READING (fates are mutually exclusive and sum to redsamp):
    //   redsamp == 0, anysamp large
    //       -> the red entries are NEVER SAMPLED. The palette holds them but
    //          the texture's index data never selects them. Back to the index
    //          side: ReadPageToLinearBufferP8 / P4 unswizzle in
    //          ps2_gs_memory.cpp:1367/1384, which no probe has ever covered.
    //   killate dominant
    //       -> the alpha test is discarding the box. Read `test`: ATE/ATST/
    //          AREF. Bug is classifyAlphaTest, or the `a` we hand it.
    //   killz dominant
    //       -> the depth test is discarding it. Read ztst in `test` bits17-18;
    //          ztst == 0 (NEVER) would mean we mis-decoded ZTEST entirely.
    //   stored > 0 and storedblack ~= stored
    //       -> the pixel survives both tests and the BLENDER zeroes it.
    //          Compare alpha (a|b<<2|c<<4|d<<6|fix<<8) against dstrgb.
    //   stored > 0 and storedblack == 0
    //       -> the red pixel IS written to VRAM. Defect is downstream of the
    //          rasterizer: fbpor/fbplast say which framebuffer received it,
    //          and Stage 5.11 becomes a present/overdraw question.
    //
    // GUARD (rival reading, per feedback_degenerate_result_convicts_the_probe):
    //   anysamp counts EVERY textured writePixel entry, red or not. redsamp==0
    //   is only evidence about the game if anysamp is large. If both are 0 the
    //   probe is dead (not compiled, diag off, wrong call site) and says
    //   nothing at all -- check anysamp FIRST, before reading any other field.
    //
    // GUARD (capped-probe, per feedback_capped_probes_false_negatives):
    //   these are unbounded counters and the snapshot is last-wins. Nothing
    //   saturates, so absence of a fate really is absence.
    std::atomic<uint64_t> g_trAnySamp{0};
    std::atomic<uint64_t> g_trRedSamp{0};
    // Stage 5.11 run 23. Samples admitted by the SHAPE gate rather than the
    // colour predicate. Read this first: if boxsamp == 0 the box never reached
    // writePixel at all and every fate below is silent about it -- a different
    // and much earlier failure than any of #7/#8/#9/#11.
    std::atomic<uint64_t> g_trBoxSamp{0};
    std::atomic<uint64_t> g_trKillAte{0};
    std::atomic<uint64_t> g_trKillZ{0};
    std::atomic<uint64_t> g_trStored{0};
    std::atomic<uint64_t> g_trStoredBlack{0};

    std::atomic<uint32_t> g_trTexel{0};   // the red texel itself
    std::atomic<uint32_t> g_trTest{0};    // raw TEST at that pixel
    std::atomic<uint32_t> g_trAlpha{0};   // a | b<<2 | c<<4 | d<<6 | fix<<8
    std::atomic<uint32_t> g_trPrim{0};
    std::atomic<uint32_t> g_trSrcA{0};    // source alpha entering the pipeline
    std::atomic<uint32_t> g_trSrcRgb{0};  // colour before the blender
    std::atomic<uint32_t> g_trDstRgb{0};  // framebuffer colour underneath
    std::atomic<uint32_t> g_trPixel{0};   // final packed value at the store
    std::atomic<uint32_t> g_trFbpOr{0};   // OR of every fbp that received red
    std::atomic<uint32_t> g_trFbpLast{0};
    std::atomic<uint32_t> g_trTexPsm{0};

    // bbox of red-texel pixels, whatever their fate. A glyph-sized box here
    // and a full-screen bbox in g_tz* would confirm they are different draws.
    std::atomic<uint32_t> g_trX0{0xFFFFu};
    std::atomic<uint32_t> g_trY0{0xFFFFu};
    std::atomic<uint32_t> g_trX1{0};
    std::atomic<uint32_t> g_trY1{0};

    // Set once per writePixel, read by the two early-return sites below.
    thread_local bool t_redPixel = false;
}

namespace
{
    float fabsQ(float q)
    {
        return (std::fabs(q) > 1.0e-8f) ? q : 1.0f;
    }

    u16 Rgba8888ToRgba5551(u32 c)
    {
        uint32_t r = ((c >> 0)  & 0xFF) >> 3;
        uint32_t g = ((c >> 8)  & 0xFF) >> 3;
        uint32_t b = ((c >> 16) & 0xFF) >> 3;
        uint32_t a = ((c >> 24) & 0xFF) >> 7;

        return (r | (g << 5) | (b << 10) | (a << 15));
    }

    u32 Rgba5551ToRgba8888(u16 c)
    {
        u32 r = ((c >> 0)  & 0x1F) << 3;
        u32 g = ((c >> 5)  & 0x1F) << 3;
        u32 b = ((c >> 10) & 0x1F) << 3;
        u32 a = ((c >> 15) & 0x01) << 7;

        return (r | (g << 8) | (b << 16) | (a << 24));
    }

    u32 pack32(u8 r, u8 g, u8 b, u8 a)
    {
        return static_cast<u32>(r) | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t applyTexa(const GSTexaReg &texa, uint8_t psm, uint32_t texel)
    {
        if (psm == GS_PSM_CT32)
            return texel;

        const uint8_t r = static_cast<uint8_t>(texel & 0xFFu);
        const uint8_t g = static_cast<uint8_t>((texel >> 8) & 0xFFu);
        const uint8_t b = static_cast<uint8_t>((texel >> 16) & 0xFFu);
        const bool rgbZero = r == 0u && g == 0u && b == 0u;
        uint8_t a = static_cast<uint8_t>((texel >> 24) & 0xFFu);

        switch (psm)
        {
        case GS_PSM_CT24:
            a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
            if ((a & 0x80u) != 0u)
                a = texa.ta1;
            else
                a = (texa.aem && rgbZero) ? 0u : texa.ta0;
            break;
        default:
            break;
        }

        return (texel & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
    }

    std::atomic<uint32_t> s_debugPrimitiveCount{0};
    std::atomic<uint32_t> s_debugPixelCount{0};
    std::atomic<uint32_t> s_debugContext1PrimitiveCount{0};
    std::atomic<uint32_t> s_debugFbp150PixelCount{0};
    bool passesAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        if ((testReg & 0x1u) == 0u)
            return true;

        const uint8_t atst = static_cast<uint8_t>((testReg >> 1) & 0x7u);
        const uint8_t aref = static_cast<uint8_t>((testReg >> 4) & 0xFFu);

        switch (atst)
        {
        case 0:
            return false;
        case 1:
            return true;
        case 2:
            return alpha < aref;
        case 3:
            return alpha <= aref;
        case 4:
            return alpha == aref;
        case 5:
            return alpha >= aref;
        case 6:
            return alpha > aref;
        case 7:
            return alpha != aref;
        default:
            return true;
        }
    }

    struct AlphaTestResult
    {
        bool writeFramebuffer;
        bool preserveDestinationAlpha;
    };

    AlphaTestResult classifyAlphaTest(uint64_t testReg, uint8_t alpha)
    {
        const bool pass = passesAlphaTest(testReg, alpha);
        if (pass)
            return {true, false};

        // TEST.AFAIL controls what happens when the alpha comparison fails.
        switch (static_cast<uint8_t>((testReg >> 12) & 0x3u))
        {
        case 1: // FB_ONLY
            return {true, false};
        case 3: // RGB_ONLY
            return {true, true};
        case 0: // KEEP
        case 2: // ZB_ONLY
        default:
            return {false, false};
        }
    }

    struct TextureCombineResult
    {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    };

    TextureCombineResult combineTexture(const GSTex0Reg &tex,
                                        uint8_t vr,
                                        uint8_t vg,
                                        uint8_t vb,
                                        uint8_t va,
                                        uint8_t tr,
                                        uint8_t tg,
                                        uint8_t tb,
                                        uint8_t ta)
    {
        const bool textureHasAlpha = tex.tcc != 0u;
        TextureCombineResult out{tr, tg, tb, textureHasAlpha ? ta : va};

        switch (tex.tfx)
        {
        case 0: // MODULATE
            out.r = clampU8((tr * vr) >> 7);
            out.g = clampU8((tg * vg) >> 7);
            out.b = clampU8((tb * vb) >> 7);
            out.a = textureHasAlpha ? clampU8((ta * va) >> 7) : va;
            break;
        case 1: // DECAL
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        case 2: // HIGHLIGHT
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? clampU8(ta + va) : va;
            break;
        case 3: // HIGHLIGHT2
            out.r = clampU8(((tr * vr) >> 7) + va);
            out.g = clampU8(((tg * vg) >> 7) + va);
            out.b = clampU8(((tb * vb) >> 7) + va);
            out.a = textureHasAlpha ? ta : va;
            break;
        default:
            out.r = tr;
            out.g = tg;
            out.b = tb;
            out.a = textureHasAlpha ? ta : va;
            break;
        }

        return out;
    }

    uint32_t swizzleClutIndexCSM1(uint32_t index)
    {
        return (index & 0xE7u) | ((index & 0x08u) << 1u) | ((index & 0x10u) >> 1u);
    }

    // TODO: clut cache
    uint32_t resolveClutIndex(uint8_t index, uint8_t csm, uint8_t csa, uint8_t sourcePsm)
    {
        uint32_t clutIndex = static_cast<uint32_t>(index);

        switch (sourcePsm)
        {
        case GS_PSM_T4:
        case GS_PSM_T4HH:
        case GS_PSM_T4HL:
        {
            clutIndex = (static_cast<uint32_t>(csa) << 4u) | (clutIndex & 0x0Fu);

            if (csm == 0u)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
        }
        break;
        case GS_PSM_T8:
        case GS_PSM_T8H:
            if (csm == 0)
                clutIndex = swizzleClutIndexCSM1(clutIndex);
            break;
        default:
            break;
        }

        return clutIndex;
    }

    bool tex1UsesLinearFilter(uint64_t tex1)
    {
        const uint8_t mmag = static_cast<uint8_t>((tex1 >> 5) & 0x1u);
        const uint8_t mmin = static_cast<uint8_t>((tex1 >> 6) & 0x7u);
        return mmag != 0u || mmin == 1u || (mmin & 0x4u) != 0u;
    }

    uint8_t lerpChannel(uint8_t c00, uint8_t c10, uint8_t c01, uint8_t c11, float fx, float fy)
    {
        const float top = static_cast<float>(c00) + (static_cast<float>(c10) - static_cast<float>(c00)) * fx;
        const float bottom = static_cast<float>(c01) + (static_cast<float>(c11) - static_cast<float>(c01)) * fx;
        return clampU8(static_cast<int>(std::lround(top + (bottom - top) * fy)));
    }
}

void GSRasterizer::drawPrimitive(GS *gs)
{
    const auto &ctx = gs->activeContext();
    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t primitiveIndex = s_debugPrimitiveCount.fetch_add(1u, std::memory_order_relaxed);
        if (primitiveIndex < 64u)
        {
            std::cout << "[gs:prim] idx=" << primitiveIndex
                      << " type=" << static_cast<uint32_t>(gs->m_registers.prim.prim)
                      << " tme=" << static_cast<uint32_t>(gs->m_registers.prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_registers.prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_registers.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_registers.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tw=" << static_cast<uint32_t>(ctx.tex0.tw)
                      << " th=" << static_cast<uint32_t>(ctx.tex0.th)
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_registers.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_registers.texclut.cou)
                      << " cov=" << gs->m_registers.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec
                      << " v0=(" << gs->m_vtxQueue[0].x << "," << gs->m_vtxQueue[0].y << ")"
                      << " uv0=(" << (gs->m_vtxQueue[0].u >> 4) << "," << (gs->m_vtxQueue[0].v >> 4) << ")"
                      << " stq0=(" << gs->m_vtxQueue[0].s << "," << gs->m_vtxQueue[0].t << "," << gs->m_vtxQueue[0].q << ")"
                      << " v1=(" << gs->m_vtxQueue[1].x << "," << gs->m_vtxQueue[1].y << ")"
                      << " uv1=(" << (gs->m_vtxQueue[1].u >> 4) << "," << (gs->m_vtxQueue[1].v >> 4) << ")"
                      << " stq1=(" << gs->m_vtxQueue[1].s << "," << gs->m_vtxQueue[1].t << "," << gs->m_vtxQueue[1].q << ")"
                      << " v2=(" << gs->m_vtxQueue[2].x << "," << gs->m_vtxQueue[2].y << ")"
                      << " uv2=(" << (gs->m_vtxQueue[2].u >> 4) << "," << (gs->m_vtxQueue[2].v >> 4) << ")"
                      << " stq2=(" << gs->m_vtxQueue[2].s << "," << gs->m_vtxQueue[2].t << "," << gs->m_vtxQueue[2].q << ")"
                      << " rgba0=(" << static_cast<uint32_t>(gs->m_vtxQueue[0].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[0].a) << ")"
                      << " rgba1=(" << static_cast<uint32_t>(gs->m_vtxQueue[1].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[1].a) << ")"
                      << " rgba2=(" << static_cast<uint32_t>(gs->m_vtxQueue[2].r) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].g) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].b) << ","
                      << static_cast<uint32_t>(gs->m_vtxQueue[2].a) << ")"
                      << std::endl;
        }
    });

    PS2_IF_AGRESSIVE_LOGS({
        if ((gs->m_registers.prim.ctxt != 0u || ctx.frame.fbp == 150u) &&
            s_debugContext1PrimitiveCount.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::cout << "[gs:copy-prim]"
                      << " type=" << static_cast<uint32_t>(gs->m_registers.prim.prim)
                      << " tme=" << static_cast<uint32_t>(gs->m_registers.prim.tme)
                      << " abe=" << static_cast<uint32_t>(gs->m_registers.prim.abe)
                      << " fst=" << static_cast<uint32_t>(gs->m_registers.prim.fst)
                      << " ctxt=" << static_cast<uint32_t>(gs->m_registers.prim.ctxt)
                      << " fbp=" << ctx.frame.fbp
                      << " fbw=" << ctx.frame.fbw
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.frame.psm) << std::dec
                      << " tex0=("
                      << "tbp0=" << ctx.tex0.tbp0
                      << " tbw=" << static_cast<uint32_t>(ctx.tex0.tbw)
                      << " psm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.psm) << std::dec
                      << " tcc=" << static_cast<uint32_t>(ctx.tex0.tcc)
                      << " tfx=" << static_cast<uint32_t>(ctx.tex0.tfx)
                      << " cbp=" << ctx.tex0.cbp
                      << " cpsm=0x" << std::hex << static_cast<uint32_t>(ctx.tex0.cpsm) << std::dec
                      << " csm=" << static_cast<uint32_t>(ctx.tex0.csm)
                      << " csa=" << static_cast<uint32_t>(ctx.tex0.csa)
                      << ")"
                      << " texclut=("
                      << "cbw=" << static_cast<uint32_t>(gs->m_registers.texclut.cbw)
                      << " cou=" << static_cast<uint32_t>(gs->m_registers.texclut.cou)
                      << " cov=" << gs->m_registers.texclut.cov
                      << ")"
                      << " ofx=" << (ctx.xyoffset.ofx >> 4)
                      << " ofy=" << (ctx.xyoffset.ofy >> 4)
                      << " scissor=(" << ctx.scissor.x0
                      << "," << ctx.scissor.y0
                      << ")-(" << ctx.scissor.x1
                      << "," << ctx.scissor.y1 << ")"
                      << " test=0x" << std::hex << ctx.test
                      << " alpha=0x" << ctx.alpha
                      << std::dec << std::endl;
        }
    });

    if (gs->m_hasPreferredDisplaySource && ctx.frame.fbp == gs->m_preferredDisplayDestFbp)
    {
        gs->m_hasPreferredDisplaySource = false;
    }

    const auto prim = gs->m_registers.prim;

    switch (prim.prim)
    {
    case GS_PRIM_SPRITE:
        drawSprite(gs);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        drawTriangle(gs);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        drawLine(gs);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = gs->m_vtxQueue[0];
        const auto &ctx = gs->activeContext();
        int px = static_cast<int>(v.x) - (ctx.xyoffset.ofx >> 4);
        int py = static_cast<int>(v.y) - (ctx.xyoffset.ofy >> 4);
        writePixel(gs, px, py, static_cast<u32>(v.z), v.r, v.g, v.b, v.a);
        break;
    }
    default:
        break;
    }
}

void GSRasterizer::writePixel(GS *gs, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    // [drawpath] run 33 causality experiment -- PS2X_SKIPBG only. Off by
    // default, so this costs one thread_local test per pixel and nothing else.
    if (ps2diag_fbstat::t_bgSkip)
        return;

    const auto &ctx = gs->activeContext();

    const auto prim = gs->m_registers.prim;
    const auto pabe = gs->m_registers.pabe;
    const auto colclamp = gs->m_registers.colclamp;

    const auto scissor = ctx.scissor;
    const auto frame = ctx.frame;
    const auto zbuf = ctx.zbuf;
    const auto test = ctx.test;
    const auto alpha = ctx.alpha;

    // [glyphfate] (0)/(1). Must sit BEFORE the scissor return -- a scissor kill
    // is one of the fates being measured, and counting after the return would
    // make it indistinguishable from "never happened".
    if (ps2_diag::enabled() && ps2diag_fbstat::t_glyphDraw)
    {
        using namespace ps2diag_fbstat;
        g_gfIn.fetch_add(1, std::memory_order_relaxed);
        g_gfScX0.store(static_cast<uint32_t>(scissor.x0), std::memory_order_relaxed);
        g_gfScY0.store(static_cast<uint32_t>(scissor.y0), std::memory_order_relaxed);
        g_gfScX1.store(static_cast<uint32_t>(scissor.x1), std::memory_order_relaxed);
        g_gfScY1.store(static_cast<uint32_t>(scissor.y1), std::memory_order_relaxed);
        g_gfTest.store(static_cast<uint32_t>(test.data & 0xFFFFFFFFull), std::memory_order_relaxed);
        g_gfCc.store(static_cast<uint32_t>(colclamp.clamp), std::memory_order_relaxed);
        g_gfAbe.store(static_cast<uint32_t>(prim.abe), std::memory_order_relaxed);
        tfMax(g_gfSrcAMax, static_cast<uint32_t>(a));
        if (x < scissor.x0 || x > scissor.x1 || y < scissor.y0 || y > scissor.y1)
            g_gfScis.fetch_add(1, std::memory_order_relaxed);
    }

    // [boxover] run 27, draw-state half. Sits here rather than at the store
    // site purely because prim/alpha/a are unambiguously in scope here; these
    // three describe the box DRAW, not the outcome of any one store, so
    // sampling them before the scissor return costs nothing and loses nothing.
    if (ps2_diag::enabled() && ps2diag_fbstat::t_boxDraw)
    {
        using namespace ps2diag_fbstat;
        g_boAbe.store(static_cast<uint32_t>(prim.abe), std::memory_order_relaxed);
        g_boAlphaReg.store(static_cast<uint32_t>(alpha.data & 0xFFFFFFFFull),
                           std::memory_order_relaxed);
        tfMax(g_boSrcAMax, static_cast<uint32_t>(a));
    }

    if (x < scissor.x0 || x > scissor.x1 || y < scissor.y0 || y > scissor.y1)
        return;

    // Colour entering the pixel pipeline, before blending mutates r/g/b --
    // the [blackwho] probe needs it to tell "arrived black" from "blended
    // to black".
    const uint32_t diagSrcRgb = static_cast<uint32_t>(r) |
                                (static_cast<uint32_t>(g) << 8) |
                                (static_cast<uint32_t>(b) << 16);

    // [texred] classification. Must happen BEFORE the alpha-test and z-test
    // returns below, which are the two places a red pixel can vanish without
    // touching any existing probe.
    ps2diag_fbstat::t_redPixel = false;
    if (ps2_diag::enabled() && prim.tme)
    {
        using namespace ps2diag_fbstat;

        g_trAnySamp.fetch_add(1, std::memory_order_relaxed);

        const uint32_t tx0 = t_lastTexel;
        const uint32_t tr = tx0 & 0xFFu;
        const uint32_t tg = (tx0 >> 8) & 0xFFu;
        const uint32_t tb = (tx0 >> 16) & 0xFFu;

        // Stage 5.11 run 23. The colour predicate below is `tb < 0x20`, and
        // [boxtex] measured the box's own texel as 0x6629127e -- tb = 0x29.
        // The box FAILED this test by nine, so every [texred] field has been
        // describing some other sprite and killate/killz/storedblack == 0
        // never applied to the box at all. Suspects #7/#8/#9/#11 were
        // acquitted by a sample that structurally excluded the defendant.
        //
        // t_boxDraw is the shape gate [boxtex] already proved lands on the box
        // (rect 24,302 464x120, exactly the PCSX2 dump's rect), and it is live
        // for the whole pixel loop. OR-ing it in scopes the existing field set
        // onto the box without disturbing the original colour sample.
        const bool colourRed = (tr >= 0x20u && tg < 0x20u && tb < 0x20u);
        if (t_boxDraw)
            g_trBoxSamp.fetch_add(1, std::memory_order_relaxed);

        if (colourRed || t_boxDraw)
        {
            t_redPixel = true;
            if (colourRed)
                g_trRedSamp.fetch_add(1, std::memory_order_relaxed);

            g_trTexel.store(tx0, std::memory_order_relaxed);
            g_trTest.store(static_cast<uint32_t>(test.data & 0xFFFFFFFFull), std::memory_order_relaxed);
            g_trAlpha.store(static_cast<uint32_t>(alpha.a) |
                                (static_cast<uint32_t>(alpha.b) << 2) |
                                (static_cast<uint32_t>(alpha.c) << 4) |
                                (static_cast<uint32_t>(alpha.d) << 6) |
                                (static_cast<uint32_t>(alpha.fix) << 8),
                            std::memory_order_relaxed);
            g_trPrim.store(static_cast<uint32_t>(prim.data), std::memory_order_relaxed);
            g_trSrcA.store(static_cast<uint32_t>(a), std::memory_order_relaxed);
            g_trSrcRgb.store(diagSrcRgb, std::memory_order_relaxed);
            g_trTexPsm.store(static_cast<uint32_t>(ctx.tex0.psm), std::memory_order_relaxed);

            const uint32_t rx = static_cast<uint32_t>(x < 0 ? 0 : x) & 0xFFFFu;
            const uint32_t ry = static_cast<uint32_t>(y < 0 ? 0 : y) & 0xFFFFu;
            if (rx < g_trX0.load(std::memory_order_relaxed)) g_trX0.store(rx, std::memory_order_relaxed);
            if (ry < g_trY0.load(std::memory_order_relaxed)) g_trY0.store(ry, std::memory_order_relaxed);
            if (rx > g_trX1.load(std::memory_order_relaxed)) g_trX1.store(rx, std::memory_order_relaxed);
            if (ry > g_trY1.load(std::memory_order_relaxed)) g_trY1.store(ry, std::memory_order_relaxed);
        }
    }

    const AlphaTestResult alphaTest = classifyAlphaTest(ctx.test.data, a);

    if (!alphaTest.writeFramebuffer)
    {
        if (ps2diag_fbstat::t_redPixel)
            ps2diag_fbstat::g_trKillAte.fetch_add(1, std::memory_order_relaxed);
        if (ps2_diag::enabled() && ps2diag_fbstat::t_glyphDraw)
            ps2diag_fbstat::g_gfAte.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    u8* vram = gs->m_vram;

    const u32 fbp  = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
    const u32 fbw  = std::max<u32>(ctx.frame.fbw, 1u);
    const u32 fpsm = ctx.frame.psm;
    const u32 fmsk = ctx.frame.fbmsk;
    const u32 zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
    const u32 zpsm = ctx.zbuf.psm | 0x30;

    const bool alphaBlendEnabled = prim.abe;
    const bool destinationAlpha  = alphaTest.preserveDestinationAlpha;

    // small optimization, avoid reading the framebuffer for simple draws
    // TODO: only one address lookup for rmw
    const bool frmw = (ctx.frame.fbmsk != 0) || alphaBlendEnabled || destinationAlpha;

    u32 fbrgba = 0;
    if (frmw)
    {
        fbrgba = gs->ReadVram(fpsm, fbp, fbw, x, y);

        if (bitsPerPixel(fpsm) == 16)
        {
            fbrgba = Rgba5551ToRgba8888(fbrgba);
        }
    }

    uint ztest_method = (ctx.test.data >> 17) & 3;


    bool zpass = false;
    switch (ztest_method)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        zpass = z >= gs->ReadVram(zpsm, zbp, fbw, x, y);
        break;
    case 3:
        zpass = z > gs->ReadVram(zpsm, zbp, fbw, x, y);
        break;
    }

    if (!zpass)
    {
        if (ps2diag_fbstat::t_redPixel)
            ps2diag_fbstat::g_trKillZ.fetch_add(1, std::memory_order_relaxed);
        if (ps2_diag::enabled() && ps2diag_fbstat::t_glyphDraw)
            ps2diag_fbstat::g_gfZ.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (ps2diag_fbstat::t_redPixel)
        ps2diag_fbstat::g_trDstRgb.store(fbrgba & 0x00FFFFFFu, std::memory_order_relaxed);

    if (prim.abe)
    {
        uint8_t dr = fbrgba & 0xFF;
        uint8_t dg = (fbrgba >> 8) & 0xFF;
        uint8_t db = (fbrgba >> 16) & 0xFF;
        uint8_t da = (fbrgba >> 24) & 0xFF;

        // PABE disables alpha blending when the source alpha MSB is clear.
        if (!(pabe.pabe && (a & 0x80u) == 0u))
        {
            uint8_t asel = alpha.a;
            uint8_t bsel = alpha.b;
            uint8_t csel = alpha.c;
            uint8_t dsel = alpha.d;
            uint8_t fix = alpha.fix;

            auto pickRGB = [&](uint8_t sel, int cs, int cd) -> int
            {
                if (sel == 0)
                    return cs;
                if (sel == 1)
                    return cd;
                return 0;
            };
            int cAlpha = (csel == 0) ? a : (csel == 1) ? da
                                                       : fix;

            int br = ((pickRGB(asel, r, dr) - pickRGB(bsel, r, dr)) * cAlpha >> 7) + pickRGB(dsel, r, dr);
            int bg = ((pickRGB(asel, g, dg) - pickRGB(bsel, g, dg)) * cAlpha >> 7) + pickRGB(dsel, g, dg);
            int bb = ((pickRGB(asel, b, db) - pickRGB(bsel, b, db)) * cAlpha >> 7) + pickRGB(dsel, b, db);

            if (colclamp.clamp)
            {
                r = clampU8(br);
                g = clampU8(bg);
                b = clampU8(bb);
            }
            else
            {
                r &= 0xFF;
                g &= 0xFF;
                b &= 0xFF;
            }
        }
    }

    u32 fbmask = frame.fbmsk;
    bool zmask = zbuf.zmsk;

    if (!alphaTest.preserveDestinationAlpha &&
        (ctx.fba.data & 0x1ull) != 0ull &&
        ctx.frame.psm != GS_PSM_CT24)
    {
        a = static_cast<uint8_t>(a | 0x80u);
    }

    u32 pixel = pack32(r, g, b, a);

    if (fbmask != 0)
    {
        pixel = (pixel & ~fbmask) | (fbrgba & fbmask);
    }

    if (alphaTest.preserveDestinationAlpha)
    {
        pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
    }
    
    // format conversion
    if (bitsPerPixel(fpsm) == 16)
    {
        pixel = Rgba8888ToRgba5551(pixel);
    }

    if (ps2_diag::enabled())
    {
        // Per-frame pixel aggregates consumed by the [gs:frame] probe. The old
        // 1-in-2,000,000 sampled probe was useless here: a full-screen clear is
        // ~229k pixels at ~50Hz, so essentially every sample landed on a black
        // screen-clear sprite and made all geometry look black. Aggregate
        // instead, and break the counts down by textured/untextured so a real
        // draw can never be hidden behind the clear.
        gs->m_statPixelsWritten.fetch_add(1, std::memory_order_relaxed);

        const uint32_t maxChannel = std::max({static_cast<uint32_t>(r),
                                              static_cast<uint32_t>(g),
                                              static_cast<uint32_t>(b)});
        if (maxChannel != 0u)
        {
            gs->m_statPixelsNonBlack.fetch_add(1, std::memory_order_relaxed);
            // Approximate max -- a racy read/store is acceptable for a probe and
            // avoids a CAS loop on the hottest path in the rasterizer.
            if (maxChannel > gs->m_statPixelMaxRgb.load(std::memory_order_relaxed))
                gs->m_statPixelMaxRgb.store(maxChannel, std::memory_order_relaxed);
        }

        if (gs->m_registers.prim.tme)
            gs->m_statPixelsTextured.fetch_add(1, std::memory_order_relaxed);

        const uint32_t primKind = static_cast<uint32_t>(gs->m_registers.prim.prim) & 0x7u;
        gs->m_statPrimMask.fetch_or(static_cast<uint32_t>(1u) << primKind,
                                    std::memory_order_relaxed);
    }

    if (ps2_diag::enabled())
    {
        // Classify the value we are actually about to store. RGB is the
        // low 24 bits for CT32 (see pack32 above); for 16bpp the value
        // has already been packed to 5551, so test its colour bits too.
        const bool colouredPixel = (bitsPerPixel(fpsm) == 16)
                                       ? ((pixel & 0x7FFFu) != 0u)
                                       : ((pixel & 0x00FFFFFFu) != 0u);
        const uint32_t destFbp = static_cast<uint32_t>(ctx.frame.fbp) & 0x1FFu;
        const uint64_t seq = ps2diag_fbstat::g_writeSeq.fetch_add(1, std::memory_order_relaxed) + 1;

        // [glyphfate] (2)/(3)/(4). This is the store site, so `pixel` here is
        // the exact word about to land in VRAM and `seq` is directly comparable
        // to the box's own last store and to the interval total.
        if (ps2diag_fbstat::t_boxDraw)
            ps2diag_fbstat::g_gfBoxSeq.store(seq, std::memory_order_relaxed);

        if (ps2diag_fbstat::t_glyphDraw)
        {
            using namespace ps2diag_fbstat;
            g_gfStored.fetch_add(1, std::memory_order_relaxed);
            g_gfLastSeq.store(seq, std::memory_order_relaxed);

            const uint32_t ux = static_cast<uint32_t>(x < 0 ? 0 : x);
            const uint32_t uy = static_cast<uint32_t>(y < 0 ? 0 : y);
            tfMin(g_gfX0, ux);
            tfMin(g_gfY0, uy);
            tfMax(g_gfX1, ux);
            tfMax(g_gfY1, uy);

            g_gfFbpOr.fetch_or(destFbp, std::memory_order_relaxed);
            tfMin(g_gfFbpMin, destFbp);
            tfMax(g_gfFbpMax, destFbp);

            const uint32_t pr = pixel & 0xFFu;
            const uint32_t pg = (pixel >> 8) & 0xFFu;
            const uint32_t pb = (pixel >> 16) & 0xFFu;
            if (pr >= 0xC0u && pg >= 0xC0u && pb >= 0xC0u)
            {
                g_gfWhite.fetch_add(1, std::memory_order_relaxed);
                // Sample the value/destination pair only on a covered texel;
                // index-0 texels are transparent by design and would otherwise
                // be the value reported, which describes nothing.
                g_gfPix.store(pixel, std::memory_order_relaxed);
                g_gfDst.store(fbrgba, std::memory_order_relaxed);
            }
            if (!colouredPixel)
                g_gfDark.fetch_add(1, std::memory_order_relaxed);
        }

        // [fbsplit] run 26. Both sprites bucketed by destination fbp on one
        // shared order counter, so "which buffer" and "which first" are
        // answered by the same sample. The counter is incremented only for
        // the two gated sprites -- it is not a per-store cost -- and nothing
        // else resets it, which is the defect that made run 25's boxafter
        // flip. Sprites do not nest, so the two gates are mutually exclusive.
        if (ps2diag_fbstat::t_boxDraw || ps2diag_fbstat::t_glyphDraw)
        {
            using namespace ps2diag_fbstat;
            const uint64_t ord = g_fsOrdSeq.fetch_add(1, std::memory_order_relaxed) + 1;

            if (t_boxDraw)
            {
                g_fsBoxByFbp[destFbp].fetch_add(1, std::memory_order_relaxed);
                g_fsBoxOrd[destFbp].store(ord, std::memory_order_relaxed);
                g_fsBoxPix.store(pixel, std::memory_order_relaxed);

                // [boxover] run 27, outcome half. fbrgba is the destination
                // as it stands the instant before the box overwrites it --
                // the exact reciprocal of what run 26 sampled under the text.
                // Classification is total by construction: every box store
                // increments g_boStores and exactly one of the three buckets,
                // so bwhite + bblack + bother == bstores is a self-check.
                const uint32_t dr = fbrgba & 0xFFu;
                const uint32_t dg = (fbrgba >> 8) & 0xFFu;
                const uint32_t db = (fbrgba >> 16) & 0xFFu;
                g_boStores.fetch_add(1, std::memory_order_relaxed);
                g_boDstOr.fetch_or(fbrgba, std::memory_order_relaxed);
                if (dr >= 0xC0u && dg >= 0xC0u && db >= 0xC0u)
                {
                    g_boWhite.fetch_add(1, std::memory_order_relaxed);
                    g_boDstWhite.store(fbrgba, std::memory_order_relaxed);
                }
                else if ((dr | dg | db) == 0u)
                    g_boBlack.fetch_add(1, std::memory_order_relaxed);
                else
                    g_boOther.fetch_add(1, std::memory_order_relaxed);

                // [fbaddr] run 28, box side. fbp/fbw/fpsm are the three terms
                // the VRAM address is built from, sampled here rather than
                // inferred; fmsk shares the write path. All four are the
                // function-scope locals from the top of writePixel, so this
                // is the same arithmetic the store below will use, not a
                // reconstruction of it.
                g_faBN.fetch_add(1, std::memory_order_relaxed);
                g_faBFbpOr.fetch_or(fbp, std::memory_order_relaxed);
                g_faBFbpAnd.fetch_and(fbp, std::memory_order_relaxed);
                g_faBFbwOr.fetch_or(fbw, std::memory_order_relaxed);
                g_faBFbwAnd.fetch_and(fbw, std::memory_order_relaxed);
                g_faBPsmOr.fetch_or(fpsm, std::memory_order_relaxed);
                g_faBPsmAnd.fetch_and(fpsm, std::memory_order_relaxed);
                g_faBMskOr.fetch_or(fmsk, std::memory_order_relaxed);
                g_faBMskAnd.fetch_and(fmsk, std::memory_order_relaxed);
                {
                    const uint32_t bx = static_cast<uint32_t>(x < 0 ? 0 : x);
                    const uint32_t by = static_cast<uint32_t>(y < 0 ? 0 : y);
                    tfMin(g_faBX0, bx);
                    tfMax(g_faBX1, bx);
                    tfMin(g_faBY0, by);
                    tfMax(g_faBY1, by);
                }
            }
            else if ((pixel & 0xFFu) >= 0xC0u &&
                     ((pixel >> 8) & 0xFFu) >= 0xC0u &&
                     ((pixel >> 16) & 0xFFu) >= 0xC0u)
            {
                // White glyph pixels only. Index-0 texels are transparent by
                // design; counting them would bury the signal under coverage.
                g_fsGlyphByFbp[destFbp].fetch_add(1, std::memory_order_relaxed);
                g_fsGlyphOrd[destFbp].store(ord, std::memory_order_relaxed);
                g_fsGlyphDstOr.fetch_or(fbrgba, std::memory_order_relaxed);

                // [fbaddr] run 28, glyph side. Deliberately gated on the SAME
                // white-pixel test as the counter above, so the extents and
                // the address terms describe exactly the population whose
                // destination run 26 measured as 0x80000000 -- comparing a
                // different sample set against the box would prove nothing.
                g_faGN.fetch_add(1, std::memory_order_relaxed);
                g_faGFbpOr.fetch_or(fbp, std::memory_order_relaxed);
                g_faGFbpAnd.fetch_and(fbp, std::memory_order_relaxed);
                g_faGFbwOr.fetch_or(fbw, std::memory_order_relaxed);
                g_faGFbwAnd.fetch_and(fbw, std::memory_order_relaxed);
                g_faGPsmOr.fetch_or(fpsm, std::memory_order_relaxed);
                g_faGPsmAnd.fetch_and(fpsm, std::memory_order_relaxed);
                g_faGMskOr.fetch_or(fmsk, std::memory_order_relaxed);
                g_faGMskAnd.fetch_and(fmsk, std::memory_order_relaxed);
                {
                    const uint32_t gx = static_cast<uint32_t>(x < 0 ? 0 : x);
                    const uint32_t gy = static_cast<uint32_t>(y < 0 ? 0 : y);
                    tfMin(g_faGX0, gx);
                    tfMax(g_faGX1, gx);
                    tfMin(g_faGY0, gy);
                    tfMax(g_faGY1, gy);

                    // [pixlog] run 29. Latch this coordinate as the address to
                    // follow. A white glyph store is by definition a pixel the
                    // text covers, which is the one thing a hard-coded pixel
                    // cannot guarantee. CAS so exactly one store wins; armed is
                    // published LAST so the log site below can never read a
                    // half-written coordinate.
                    // Point 1 additionally requires 120px of horizontal
                    // separation from point 0, so it is a DIFFERENT glyph by
                    // construction rather than a neighbouring pixel of the
                    // same one -- otherwise the second sample would just
                    // confirm the first by accident.
                    for (uint32_t p = 0; p < kPlPts; ++p)
                    {
                        if (g_plArmed[p].load(std::memory_order_acquire) != 0u)
                            continue;
                        if (p == 1u)
                        {
                            if (g_plArmed[0].load(std::memory_order_acquire) == 0u)
                                break;   // point 0 must arm first
                            if (gx < g_plX[0].load(std::memory_order_relaxed) + 120u)
                                break;
                        }
                        uint32_t claimed = 0u;
                        if (g_plClaim[p].compare_exchange_strong(claimed, 1u,
                                                                 std::memory_order_acq_rel,
                                                                 std::memory_order_relaxed))
                        {
                            g_plX[p].store(gx, std::memory_order_relaxed);
                            g_plY[p].store(gy, std::memory_order_relaxed);
                            g_plFbp[p].store(destFbp, std::memory_order_relaxed);
                            g_plArmed[p].store(1u, std::memory_order_release);
                        }
                        break;
                    }
                }
            }
        }

        // [pixlog] run 29 -- the log site. NO SPRITE GATE. Every store to the
        // latched address is recorded whoever makes it, which is the whole
        // point: the writer that explains runs 26-28 is one no sprite gate can
        // see. The latching store itself is caught here because the latch runs
        // just above it in the same call.
        {
            using namespace ps2diag_fbstat;
            const uint32_t px = static_cast<uint32_t>(x < 0 ? 0 : x);
            const uint32_t py = static_cast<uint32_t>(y < 0 ? 0 : y);
            for (uint32_t p = 0; p < kPlPts; ++p)
            {
                if (g_plArmed[p].load(std::memory_order_acquire) == 0u)
                    continue;
                if (px != g_plX[p].load(std::memory_order_relaxed) ||
                    py != g_plY[p].load(std::memory_order_relaxed) ||
                    destFbp != g_plFbp[p].load(std::memory_order_relaxed))
                    continue;

                const uint64_t n = g_plCount[p].fetch_add(1, std::memory_order_relaxed);
                const uint32_t slot = static_cast<uint32_t>(n % kPlRing);
                g_plTag[p][slot].store(t_boxDraw ? 1u : (t_glyphDraw ? 2u : 0u),
                                       std::memory_order_relaxed);
                g_plVal[p][slot].store(pixel, std::memory_order_relaxed);
                g_plDst[p][slot].store(fbrgba, std::memory_order_relaxed);
                g_plPrim[p][slot].store(static_cast<uint32_t>(prim.data & 0xFFFFFFFFull),
                                        std::memory_order_relaxed);

                // The texel as FETCHED, before the blender touched it. This is
                // the field that separates a degenerate texture fetch from a
                // destructive blend -- see the note at the counter definitions.
                g_plTexel[p][slot].store(t_lastTexel, std::memory_order_relaxed);
                g_plTbp[p][slot].store(static_cast<uint32_t>(ctx.tex0.tbp0),
                                       std::memory_order_relaxed);
                g_plCbp[p][slot].store(static_cast<uint32_t>(ctx.tex0.cbp),
                                       std::memory_order_relaxed);
                g_plTfmt[p][slot].store(
                    (static_cast<uint32_t>(ctx.tex0.psm) & 0xFFu) |
                    ((static_cast<uint32_t>(ctx.tex0.tbw) & 0xFFu) << 8) |
                    ((static_cast<uint32_t>(ctx.tex0.tfx) & 0xFu) << 16) |
                    ((static_cast<uint32_t>(ctx.tex0.tcc) & 0xFu) << 20) |
                    ((static_cast<uint32_t>(ctx.tex0.cpsm) & 0xFFu) << 24),
                    std::memory_order_relaxed);
                g_plAlpha[p][slot].store(
                    (static_cast<uint32_t>(alpha.a) & 0x3u) |
                    ((static_cast<uint32_t>(alpha.b) & 0x3u) << 2) |
                    ((static_cast<uint32_t>(alpha.c) & 0x3u) << 4) |
                    ((static_cast<uint32_t>(alpha.d) & 0x3u) << 6) |
                    ((static_cast<uint32_t>(alpha.fix) & 0xFFu) << 8),
                    std::memory_order_relaxed);

                // ord is published last and is the "slot is filled" flag, so a
                // partially written slot is skipped rather than misread.
                g_plOrd[p][slot].store(n + 1u, std::memory_order_release);
                if (t_glyphDraw)
                    g_plGlyphHits[p].fetch_add(1, std::memory_order_relaxed);
            }
        }

        if (colouredPixel)
        {
            ps2diag_fbstat::g_fbNonBlack[destFbp].fetch_add(1, std::memory_order_relaxed);
            ps2diag_fbstat::g_lastNonBlackSeq.store(seq, std::memory_order_relaxed);

            // A coloured store ends the current trailing black run.
            ps2diag_fbstat::g_tailX0.store(0xFFFFu, std::memory_order_relaxed);
            ps2diag_fbstat::g_tailY0.store(0xFFFFu, std::memory_order_relaxed);
            ps2diag_fbstat::g_tailX1.store(0, std::memory_order_relaxed);
            ps2diag_fbstat::g_tailY1.store(0, std::memory_order_relaxed);
        }
        else
        {
            using namespace ps2diag_fbstat;

            g_fbBlack[destFbp].fetch_add(1, std::memory_order_relaxed);

            const uint32_t texel = t_lastTexel;
            if (!prim.tme)
                g_blkUntex.fetch_add(1, std::memory_order_relaxed);
            else if ((texel & 0x00FFFFFFu) == 0u)
            {
                g_blkTexZero.fetch_add(1, std::memory_order_relaxed);

                // [texzero] -- written only here, so it survives the untextured
                // clear that always lands last and owns the snap* set.
                if (prim.abe)
                    g_tzAbeOn.fetch_add(1, std::memory_order_relaxed);
                else
                    g_tzAbeOff.fetch_add(1, std::memory_order_relaxed);

                if ((test.data & 0x1ull) != 0ull)
                    g_tzAteOn.fetch_add(1, std::memory_order_relaxed);

                g_tzPrim.store(static_cast<uint32_t>(prim.data), std::memory_order_relaxed);
                g_tzTest.store(static_cast<uint32_t>(test.data & 0xFFFFFFFFull), std::memory_order_relaxed);
                g_tzAlpha.store(static_cast<uint32_t>(alpha.a) |
                                    (static_cast<uint32_t>(alpha.b) << 2) |
                                    (static_cast<uint32_t>(alpha.c) << 4) |
                                    (static_cast<uint32_t>(alpha.d) << 6) |
                                    (static_cast<uint32_t>(alpha.fix) << 8),
                                std::memory_order_relaxed);
                g_tzSrcA.store(static_cast<uint32_t>(a), std::memory_order_relaxed);
                g_tzTexel.store(texel, std::memory_order_relaxed);
                g_tzTbp.store(static_cast<uint32_t>(ctx.tex0.tbp0), std::memory_order_relaxed);
                g_tzCbp.store(static_cast<uint32_t>(ctx.tex0.cbp), std::memory_order_relaxed);
                g_tzTexPsm.store(static_cast<uint32_t>(ctx.tex0.psm), std::memory_order_relaxed);
                g_tzDstRgb.store(fbrgba & 0x00FFFFFFu, std::memory_order_relaxed);
                g_tzFbp.store(destFbp, std::memory_order_relaxed);

                // [clutmap] -- cold branch, so these are free.
                const uint32_t rawIdx = t_lastTexIndex;
                g_tzIdxSamp.fetch_add(1, std::memory_order_relaxed);
                g_tzIdxOr.fetch_or(rawIdx, std::memory_order_relaxed);
                if (rawIdx != 0u)
                    g_tzIdxNz.fetch_add(1, std::memory_order_relaxed);
                g_tzIdxLast.store(rawIdx, std::memory_order_relaxed);
                g_tzCsa.store(static_cast<uint32_t>(ctx.tex0.csa), std::memory_order_relaxed);
                g_tzCsm.store(static_cast<uint32_t>(ctx.tex0.csm), std::memory_order_relaxed);
                g_tzCpsm.store(static_cast<uint32_t>(ctx.tex0.cpsm), std::memory_order_relaxed);

                const uint32_t tx = static_cast<uint32_t>(x < 0 ? 0 : x) & 0xFFFFu;
                const uint32_t ty = static_cast<uint32_t>(y < 0 ? 0 : y) & 0xFFFFu;
                if (tx < g_tzX0.load(std::memory_order_relaxed)) g_tzX0.store(tx, std::memory_order_relaxed);
                if (ty < g_tzY0.load(std::memory_order_relaxed)) g_tzY0.store(ty, std::memory_order_relaxed);
                if (tx > g_tzX1.load(std::memory_order_relaxed)) g_tzX1.store(tx, std::memory_order_relaxed);
                if (ty > g_tzY1.load(std::memory_order_relaxed)) g_tzY1.store(ty, std::memory_order_relaxed);
            }
            else
                g_blkTexCol.fetch_add(1, std::memory_order_relaxed);

            const uint32_t ux = static_cast<uint32_t>(x < 0 ? 0 : x) & 0xFFFFu;
            const uint32_t uy = static_cast<uint32_t>(y < 0 ? 0 : y) & 0xFFFFu;
            if (ux < g_tailX0.load(std::memory_order_relaxed)) g_tailX0.store(ux, std::memory_order_relaxed);
            if (uy < g_tailY0.load(std::memory_order_relaxed)) g_tailY0.store(uy, std::memory_order_relaxed);
            if (ux > g_tailX1.load(std::memory_order_relaxed)) g_tailX1.store(ux, std::memory_order_relaxed);
            if (uy > g_tailY1.load(std::memory_order_relaxed)) g_tailY1.store(uy, std::memory_order_relaxed);

            g_snapPrim.store(static_cast<uint32_t>(prim.data), std::memory_order_relaxed);
            g_snapTexPsm.store(static_cast<uint32_t>(ctx.tex0.psm), std::memory_order_relaxed);
            g_snapTbp.store(static_cast<uint32_t>(ctx.tex0.tbp0), std::memory_order_relaxed);
            g_snapCbp.store(static_cast<uint32_t>(ctx.tex0.cbp), std::memory_order_relaxed);
            g_snapFbmsk.store(fmsk, std::memory_order_relaxed);
            g_snapAlpha.store(static_cast<uint32_t>(alpha.a) |
                                  (static_cast<uint32_t>(alpha.b) << 2) |
                                  (static_cast<uint32_t>(alpha.c) << 4) |
                                  (static_cast<uint32_t>(alpha.d) << 6) |
                                  (static_cast<uint32_t>(alpha.fix) << 8),
                              std::memory_order_relaxed);
            g_snapTexel.store(prim.tme ? texel : 0u, std::memory_order_relaxed);
            g_snapSrcRgb.store(diagSrcRgb, std::memory_order_relaxed);
            g_snapXy.store(ux | (uy << 16), std::memory_order_relaxed);
            g_snapFbp.store(destFbp, std::memory_order_relaxed);
        }
    }

    if (ps2diag_fbstat::t_redPixel)
    {
        using namespace ps2diag_fbstat;

        const uint32_t destFbpR = static_cast<uint32_t>(ctx.frame.fbp) & 0x1FFu;
        const bool blackNow = (bitsPerPixel(fpsm) == 16)
                                  ? ((pixel & 0x7FFFu) == 0u)
                                  : ((pixel & 0x00FFFFFFu) == 0u);

        g_trStored.fetch_add(1, std::memory_order_relaxed);
        if (blackNow)
            g_trStoredBlack.fetch_add(1, std::memory_order_relaxed);

        g_trPixel.store(pixel, std::memory_order_relaxed);
        g_trFbpOr.fetch_or(destFbpR, std::memory_order_relaxed);
        g_trFbpLast.store(destFbpR, std::memory_order_relaxed);
    }

    // ---- [boxblk] -- classify every store landing in the red-box rect. -----
    // Rect is the [texred] bbox, identical to censusRedBox in ps2_gs_gpu.cpp so
    // the two probes describe the same pixels. Reading table is at the counter
    // declarations near the top of this file.
    if (x >= 23 && x <= 484 && y >= 301 && y <= 419)
    {
        using namespace ps2diag_fbstat;

        g_bbAny.fetch_add(1, std::memory_order_relaxed);

        if (bitsPerPixel(fpsm) != 32)
        {
            // RGB decode below is only valid for a 32bpp framebuffer. Count
            // these rather than silently folding them into "not red".
            g_bb16.fetch_add(1, std::memory_order_relaxed);
        }
        else
        {
            const uint32_t pr = pixel & 0xFFu;
            const uint32_t pg = (pixel >> 8) & 0xFFu;
            const uint32_t pb = (pixel >> 16) & 0xFFu;

            // Same strict test as censusRedBox: no grey-ramp value can pass.
            if (pr >= 0x30u && pr >= pg + 0x18u && pr >= pb + 0x18u)
            {
                g_bbRed.fetch_add(1, std::memory_order_relaxed);
            }

            if ((pixel & 0x00FFFFFFu) == 0u)
            {
                g_bbBlack.fetch_add(1, std::memory_order_relaxed);

                if (!prim.tme)
                {
                    g_bbBlkUntex.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    g_bbBlkTex.fetch_add(1, std::memory_order_relaxed);

                    const uint32_t rawIdx = t_lastTexIndex;
                    const uint32_t bucket = (rawIdx >= 32u) ? 8u : (rawIdx >> 2);
                    g_bbIdx[bucket].fetch_add(1, std::memory_order_relaxed);

                    g_bbIdxLast.store(rawIdx, std::memory_order_relaxed);
                    g_bbTexel.store(t_lastTexel, std::memory_order_relaxed);
                    g_bbTexPsm.store(static_cast<uint32_t>(ctx.tex0.psm), std::memory_order_relaxed);
                    g_bbTbp.store(static_cast<uint32_t>(ctx.tex0.tbp0), std::memory_order_relaxed);
                    g_bbCbp.store(static_cast<uint32_t>(ctx.tex0.cbp), std::memory_order_relaxed);
                    g_bbCsa.store(static_cast<uint32_t>(ctx.tex0.csa), std::memory_order_relaxed);
                    g_bbCpsm.store(static_cast<uint32_t>(ctx.tex0.cpsm), std::memory_order_relaxed);
                    g_bbPrim.store(static_cast<uint32_t>(prim.data), std::memory_order_relaxed);
                    g_bbTest.store(static_cast<uint32_t>(test.data & 0xFFFFFFFFull), std::memory_order_relaxed);
                    g_bbSrcA.store(static_cast<uint32_t>(a), std::memory_order_relaxed);
                    g_bbXy.store(static_cast<uint32_t>(x) | (static_cast<uint32_t>(y) << 16),
                                 std::memory_order_relaxed);
                    g_bbFbp.store(static_cast<uint32_t>(ctx.frame.fbp) & 0x1FFu,
                                  std::memory_order_relaxed);
                }
            }
        }
    }

    gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);

    if (!zmask)
    {
        gs->WriteVram(zpsm, zbp, fbw, x, y, z);
    }
}

uint32_t GSRasterizer::sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v)
{
    const auto &ctx = gs->activeContext();
    const auto tex = ctx.tex0;
    const auto prim = gs->m_registers.prim;
    const auto texa = gs->m_registers.texa;

    int texW = 1 << tex.tw;
    int texH = 1 << tex.th;

    float texUf, texVf;
    if (prim.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(texW);
        texVf = t * invQ * static_cast<float>(texH);
    }

    auto samplePoint = [&](int sampleU, int sampleV) -> uint32_t
    {
        sampleU = clampInt(sampleU, 0, texW - 1);
        sampleV = clampInt(sampleV, 0, texH - 1);

        u32 out = gs->ReadTexturePageCache(tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

        switch (tex.psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
        case GS_PSM_CT24:
        case GS_PSM_Z24:
            return applyTexa(texa, tex.psm, out);
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            return applyTexa(texa, tex.psm, Rgba5551ToRgba8888(out));
        case GS_PSM_T8:
        case GS_PSM_T8H:
        case GS_PSM_T4:
        case GS_PSM_T4HL:
        case GS_PSM_T4HH:
            ps2diag_fbstat::t_lastTexIndex = out;

            // [boxtex] -- Stage 5.11 run 22. Every paletted format, not just
            // T8: if suspect #16 is live the box may be arriving as T4, and
            // gating on T8 here would hide exactly that.
            if (ps2diag_fbstat::t_boxDraw)
            {
                using namespace ps2diag_fbstat;
                const uint32_t idx = out & 0xFFu;
                g_bxIdxSamp.fetch_add(1, std::memory_order_relaxed);
                g_bxIdxHist[(idx >> 4) & 0xFu].fetch_add(1, std::memory_order_relaxed);
                tfMin(g_bxIdxMin, idx);
                tfMax(g_bxIdxMax, idx);
            }

        {
            // ---- [texfetch] -- Stage 5.11 run 24 --------------------------
            // Reading table lives at the counter definitions near the top of
            // this file. Gated on SHAPE, never on tbp0.
            using namespace ps2diag_fbstat;
            g_tfAll.fetch_add(1, std::memory_order_relaxed);

            const bool isT4 = (tex.psm == GS_PSM_T4);
            if (isT4)
                g_tfT4.fetch_add(1, std::memory_order_relaxed);

            const bool glyphShape =
                isT4 &&
                static_cast<uint32_t>(tex.tbw) == kTfTbw &&
                texW == static_cast<int>(kTfDim) &&
                texH == static_cast<int>(kTfDim);

            uint64_t n = 0;
            if (glyphShape)
            {
                n = g_tfSamp.fetch_add(1, std::memory_order_relaxed);

                // (2) THE FETCH. Bin by index VALUE, not by (out>>4): a T4
                // index is 0..15, so the whole range is one bin per value and
                // the histogram is directly comparable to [clutlive]'s
                // VRAM-side census of the same atlas.
                const uint32_t idx = out;
                g_tfHist[idx & 0xFu].fetch_add(1, std::memory_order_relaxed);
                if (idx > 15u)
                    g_tfOor.fetch_add(1, std::memory_order_relaxed);
                tfMax(g_tfIdxMax, idx);

                // (1) COORDINATE SPAN across every sample of every glyph --
                // [uvspan] only ever reported the last draw's corners.
                tfMin(g_tfUmin, static_cast<uint32_t>(sampleU));
                tfMax(g_tfUmax, static_cast<uint32_t>(sampleU));
                tfMin(g_tfVmin, static_cast<uint32_t>(sampleV));
                tfMax(g_tfVmax, static_cast<uint32_t>(sampleV));

                g_tfTbw.store(static_cast<uint32_t>(tex.tbw), std::memory_order_relaxed);
                g_tfTw.store(static_cast<uint32_t>(texW), std::memory_order_relaxed);
                g_tfTh.store(static_cast<uint32_t>(texH), std::memory_order_relaxed);
                g_tfTbp.store(static_cast<uint32_t>(tex.tbp0), std::memory_order_relaxed);
                g_tfCsa.store(static_cast<uint32_t>(tex.csa), std::memory_order_relaxed);
                g_tfCpsm.store(static_cast<uint32_t>(tex.cpsm), std::memory_order_relaxed);

                // (4) RIVAL READING: the same texel walked straight out of
                // VRAM, sharing no state with the page cache. 1 in 64 to bound
                // cost. A 100% mismatch convicts ReadVram, not the cache.
                if ((n & 63u) == 0u)
                {
                    g_tfChk.fetch_add(1, std::memory_order_relaxed);
                    const u32 direct = gs->ReadVram(tex.psm, tex.tbp0, tex.tbw,
                                                    static_cast<u32>(sampleU),
                                                    static_cast<u32>(sampleV)) & 0xFu;
                    if (direct != (out & 0xFu))
                    {
                        g_tfDis.fetch_add(1, std::memory_order_relaxed);
                        g_tfBadU.store(static_cast<uint32_t>(sampleU), std::memory_order_relaxed);
                        g_tfBadV.store(static_cast<uint32_t>(sampleV), std::memory_order_relaxed);
                        g_tfBadCache.store(out & 0xFu, std::memory_order_relaxed);
                        g_tfBadVram.store(direct, std::memory_order_relaxed);
                    }
                }
            }

            const u32 texelOut =
                applyTexa(texa, tex.psm,
                          gs->ReadClutCache(tex.cpsm, static_cast<u8>(out), tex.csa));

            // (3) THE CLUT LOOKUP, measured on the sampler's own result --
            // this is the value the pixel loop receives, not a host-side
            // re-read of the palette.
            if (glyphShape)
            {
                const uint32_t alpha = (texelOut >> 24) & 0xFFu;
                if (alpha == 0u)
                    g_tfA0.fetch_add(1, std::memory_order_relaxed);
                tfMax(g_tfAMax, alpha);
                if ((texelOut & 0x00FFFFFFu) == 0x00FFFFFFu)
                    g_tfWhite.fetch_add(1, std::memory_order_relaxed);
            }

            return texelOut;
        }
        }

        return 0xFFFF00FFu;
    };

    if (!tex1UsesLinearFilter(ctx.tex1.data))
    {
        return samplePoint(static_cast<int>(texUf), static_cast<int>(texVf));
    }

    const float sampleU = texUf - 0.5f;
    const float sampleV = texVf - 0.5f;
    const int u0 = static_cast<int>(std::floor(sampleU));
    const int v0 = static_cast<int>(std::floor(sampleV));
    const int u1 = u0 + 1;
    const int v1 = v0 + 1;
    const float fx = sampleU - static_cast<float>(u0);
    const float fy = sampleV - static_cast<float>(v0);

    const uint32_t c00 = samplePoint(u0, v0);
    const uint32_t c10 = samplePoint(u1, v0);
    const uint32_t c01 = samplePoint(u0, v1);
    const uint32_t c11 = samplePoint(u1, v1);

    const uint8_t r = lerpChannel(static_cast<uint8_t>(c00 & 0xFFu),
                                  static_cast<uint8_t>(c10 & 0xFFu),
                                  static_cast<uint8_t>(c01 & 0xFFu),
                                  static_cast<uint8_t>(c11 & 0xFFu),
                                  fx, fy);
    const uint8_t g = lerpChannel(static_cast<uint8_t>((c00 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 8) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 8) & 0xFFu),
                                  fx, fy);
    const uint8_t b = lerpChannel(static_cast<uint8_t>((c00 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 16) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 16) & 0xFFu),
                                  fx, fy);
    const uint8_t a = lerpChannel(static_cast<uint8_t>((c00 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c10 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c01 >> 24) & 0xFFu),
                                  static_cast<uint8_t>((c11 >> 24) & 0xFFu),
                                  fx, fy);

    return static_cast<uint32_t>(r) |
           (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(a) << 24);
}

void GSRasterizer::drawSprite(GS *gs)
{
    const auto prim = gs->m_registers.prim;

    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;
    u32 z1 = static_cast<u32>(v1.z);

    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);

    const int unclippedX0 = x0;
    const int unclippedY0 = y0;
    const int spanX = std::max(1, x1 - x0);
    const int spanY = std::max(1, y1 - y0);
    const int unclippedX1 = unclippedX0 + spanX - 1;
    const int unclippedY1 = unclippedY0 + spanY - 1;

    // If the sprite rectangle is fully outside scissor, nothing should render.
    if (unclippedX1 < ctx.scissor.x0 || unclippedX0 > ctx.scissor.x1 ||
        unclippedY1 < ctx.scissor.y0 || unclippedY0 > ctx.scissor.y1)
    {
        // maybe a log here idk ?
        return;
    }

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

    const uint64_t alphaReg = ctx.alpha.data;
    const uint8_t alphaMode = static_cast<uint8_t>(alphaReg & 0xFFu);
    const uint8_t alphaFix = static_cast<uint8_t>((alphaReg >> 32) & 0xFFu);
    const bool looksLikeDisplayCopy =
        prim.tme &&
        prim.abe &&
        prim.fst &&
        prim.ctxt &&
        ctx.frame.fbp != ctx.tex0.tbp0 &&
        alphaMode == 0x64u &&
        (alphaFix == 0x60u || alphaFix == 0x80u) &&
        unclippedX0 <= 0 &&
        unclippedY0 <= 0 &&
        unclippedX1 >= 639 &&
        unclippedY1 >= 447;

    if (looksLikeDisplayCopy)
    {
        GSFrameReg copy{ };
        copy.fbp = ctx.tex0.tbp0;
        copy.fbw = ctx.tex0.tbw;
        copy.psm = ctx.tex0.psm;

        gs->m_preferredDisplaySourceFrame = std::move(copy);
        gs->m_preferredDisplayDestFbp = ctx.frame.fbp;
        gs->m_hasPreferredDisplaySource = true;
    }

    // ---- [drawpath] window start -- run 38 ------------------------------
    // Two independent frame boundaries, either of which arms the ring. Both
    // sit outside the sprite population under test, so neither can bias the
    // order the ring records:
    //   * an untextured near-full-screen sprite  -- the frame clear
    //   * looksLikeDisplayCopy                   -- the blit that ends a frame
    // Each is counted separately, so a report with armclear=0 armcopy=0 says
    // "the boundary gate missed" instead of quietly reporting a half frame.
    {
        const int dpW = unclippedX1 - unclippedX0;
        const int dpH = unclippedY1 - unclippedY0;
        const bool dpClear = !prim.tme && dpW >= 480 && dpH >= 400;
        if (dpClear || looksLikeDisplayCopy)
        {
            if (dpClear)
                ps2diag_fbstat::g_dpArmClear.fetch_add(1, std::memory_order_relaxed);
            else
                ps2diag_fbstat::g_dpArmCopy.fetch_add(1, std::memory_order_relaxed);

            // A boundary also ENDS the window in progress. Without this the
            // ring only ever latches by filling, so a frame with fewer than
            // kDpRing sprites would spill into the next one and never report.
            // An empty window (no sprite since the last boundary) is dropped,
            // which is what the copy->clear pair back to back produces.
            const uint32_t dpSeen = ps2diag_fbstat::g_dpIdx.load(std::memory_order_relaxed);
            if (ps2diag_fbstat::g_dpCapturing.exchange(0u, std::memory_order_acq_rel) != 0u &&
                dpSeen != 0u)
            {
                ps2diag_fbstat::g_dpCount.store(
                    dpSeen < ps2diag_fbstat::kDpRing ? dpSeen : ps2diag_fbstat::kDpRing,
                    std::memory_order_relaxed);
                ps2diag_fbstat::g_dpReady.store(1u, std::memory_order_release);
            }
            ps2diag_fbstat::g_dpArm.store(1u, std::memory_order_release);
        }
    }

    uint8_t r = v1.r, g = v1.g, b = v1.b, a = v1.a;

    if (prim.tme)
    {
        const auto &tex = ctx.tex0;
        int texW = 1 << tex.tw;
        int texH = 1 << tex.th;
        if (texW == 0)
            texW = 1;
        if (texH == 0)
            texH = 1;

        float u0f, v0f, u1f, v1f;
        if (prim.fst)
        {
            u0f = static_cast<float>(v0.u >> 4);
            v0f = static_cast<float>(v0.v >> 4);
            u1f = static_cast<float>(v1.u >> 4);
            v1f = static_cast<float>(v1.v >> 4);
        }
        else
        {
            const float q0 = fabsQ(v0.q);
            const float q1 = fabsQ(v1.q);
            u0f = (v0.s / q0) * static_cast<float>(texW);
            v0f = (v0.t / q0) * static_cast<float>(texH);
            u1f = (v1.s / q1) * static_cast<float>(texW);
            v1f = (v1.t / q1) * static_cast<float>(texH);
        }

        float spriteW = static_cast<float>(spanX);
        float spriteH = static_cast<float>(spanY);
        if (spriteW < 1.0f)
            spriteW = 1.0f;
        if (spriteH < 1.0f)
            spriteH = 1.0f;

        // [uvspan] -- Stage 5.11 run 23. Reading table at the counters above.
        // Gate is SHAPE ONLY -- psm/tbw/tw/th, no address. The dump says the
        // glyph sprites are psm=T4 tbw=4 256x256; tw/th are log2 here, so
        // 256x256 is tw==8 && th==8. The two counters above the gate are its
        // rival readings: they make a draws==0 result diagnosable instead of
        // silent, which is precisely what runs 21 and 22 lacked.
        ps2diag_fbstat::g_uvAllSpr.fetch_add(1, std::memory_order_relaxed);
        if (tex.psm == GS_PSM_T4)
            ps2diag_fbstat::g_uvT4Any.fetch_add(1, std::memory_order_relaxed);

        if (tex.psm == GS_PSM_T4 &&
            static_cast<uint32_t>(tex.tbw) == 4u &&
            static_cast<uint32_t>(tex.tw) == 8u &&
            static_cast<uint32_t>(tex.th) == 8u)
        {
            using namespace ps2diag_fbstat;
            g_uvDraws.fetch_add(1, std::memory_order_relaxed);

            if (prim.fst)
                g_uvFst1.fetch_add(1, std::memory_order_relaxed);
            else
                g_uvFst0.fetch_add(1, std::memory_order_relaxed);

            // fabsQ() silently substitutes 1.0 below 1e-8, so an exact-zero Q
            // survives the divide and looks healthy. Test the RAW q here --
            // this is the only place that failure becomes visible.
            const float aq0 = v0.q < 0.0f ? -v0.q : v0.q;
            const float aq1 = v1.q < 0.0f ? -v1.q : v1.q;
            if (aq0 < 0.015f || aq1 < 0.015f)
                g_uvQBad.fetch_add(1, std::memory_order_relaxed);

            const float du = u1f - u0f;
            const float dv = v1f - v0f;
            const float adu = du < 0.0f ? -du : du;
            const float adv = dv < 0.0f ? -dv : dv;
            if (adu < 1.0f && adv < 1.0f)
                g_uvFlat.fetch_add(1, std::memory_order_relaxed);
            if (adu >= 8.0f && adv >= 8.0f)
                g_uvWide.fetch_add(1, std::memory_order_relaxed);

            // Distribution, not just the last sample: a single last-observed
            // corner pair cannot tell "all glyphs are flat" from "one glyph
            // happened to be flat". du is 1/256 texel, ds is /65536 -- and
            // since texW==256 the two must come out numerically equal.
            const float ds = v1.s - v0.s;
            const float ads = ds < 0.0f ? -ds : ds;
            const uint32_t duFix =
                static_cast<uint32_t>(adu * 256.0f > 2.0e9f ? 2.0e9f : adu * 256.0f);
            const uint32_t dsFix =
                static_cast<uint32_t>(ads * 65536.0f > 2.0e9f ? 2.0e9f : ads * 65536.0f);
            tfMin(g_uvDuMin, duFix);
            tfMax(g_uvDuMax, duFix);
            tfMin(g_uvDsMin, dsFix);
            tfMax(g_uvDsMax, dsFix);

            g_uvFst.store(prim.fst ? 1u : 0u, std::memory_order_relaxed);
            g_uvU0.store(uvFix(u0f, 256.0f), std::memory_order_relaxed);
            g_uvV0.store(uvFix(v0f, 256.0f), std::memory_order_relaxed);
            g_uvU1.store(uvFix(u1f, 256.0f), std::memory_order_relaxed);
            g_uvV1.store(uvFix(v1f, 256.0f), std::memory_order_relaxed);

            // Both coordinate sources, every draw, whichever branch ran.
            g_uvS0.store(uvFix(v0.s, 65536.0f), std::memory_order_relaxed);
            g_uvT0.store(uvFix(v0.t, 65536.0f), std::memory_order_relaxed);
            g_uvQ0.store(uvFix(v0.q, 65536.0f), std::memory_order_relaxed);
            g_uvS1.store(uvFix(v1.s, 65536.0f), std::memory_order_relaxed);
            g_uvT1.store(uvFix(v1.t, 65536.0f), std::memory_order_relaxed);
            g_uvQ1.store(uvFix(v1.q, 65536.0f), std::memory_order_relaxed);

            g_uvRawU0.store(static_cast<int32_t>(v0.u), std::memory_order_relaxed);
            g_uvRawV0.store(static_cast<int32_t>(v0.v), std::memory_order_relaxed);
            g_uvRawU1.store(static_cast<int32_t>(v1.u), std::memory_order_relaxed);
            g_uvRawV1.store(static_cast<int32_t>(v1.v), std::memory_order_relaxed);

            g_uvSpanX.store(static_cast<int32_t>(spanX), std::memory_order_relaxed);
            g_uvSpanY.store(static_cast<int32_t>(spanY), std::memory_order_relaxed);
        }

        // ---- [boxtex] -- Stage 5.11 run 22. Table at the counters above. ---
        // The census is deliberately gated on nothing but "this is a textured
        // sprite", so it stays readable even if the rect gate below misses.
        ps2diag_fbstat::g_bxAll.fetch_add(1, std::memory_order_relaxed);
        ps2diag_fbstat::bxCensus(static_cast<uint32_t>(tex.tbp0),
                                 static_cast<uint32_t>(tex.cbp),
                                 static_cast<uint32_t>(tex.psm));

        // Gate on SHAPE, not on tbp. The whole reason the last two runs
        // measured the wrong sprite is that they trusted a tbp constant that
        // turned out never to be sent. The dump gives the box as 464 x 120;
        // the only other sprites in the frame are the 512 x 448 backdrop and
        // the 8px 9-slice border strips, so this window cannot collide.
        ps2diag_fbstat::t_boxDraw =
            (spanX >= 400 && spanX <= 500 && spanY >= 90 && spanY <= 150);

        // [glyphfate] gate. Identical predicate to the one [texfetch] run 24
        // validated on 115,043,964 samples, so it is known to select the glyph
        // atlas and nothing else. Shape only -- no tbp constant.
        ps2diag_fbstat::t_glyphDraw =
            (tex.psm == GS_PSM_T4 && static_cast<uint32_t>(tex.tbw) == 4u &&
             texW == 256 && texH == 256);
        if (ps2diag_fbstat::t_glyphDraw)
            ps2diag_fbstat::g_gfDraws.fetch_add(1, std::memory_order_relaxed);

        // ---- [drawpath] -- Stage 5.11 run 32 --------------------------------
        // Question: which DMA path does each dialog sprite ride, and in what
        // order do they reach the GS? dump.jsonl says background then box;
        // live.jsonl and [pixlog] both say box then background, in 100/100
        // frames. Run 31 cleared the GIF arbiter (maxbatch=1), so the order
        // here IS the submission order.
        //
        // Roles are texture-identity only -- no address constant, no rect
        // constant, so a sprite that moves or resizes is still classified:
        //   1 = box + its 8 border strips (PSMT8, tbw 2)
        //   2 = full-screen background      (PSMT8, tbw 4)
        //   3 = glyph atlas                 (PSMT4, tbw 4)
        //   9 = any other textured sprite
        // Span is recorded too, so a mis-shaped role convicts the gate rather
        // than quietly reporting about the wrong sprite.
        {
            using namespace ps2diag_fbstat;
            const uint32_t tbwNow = static_cast<uint32_t>(tex.tbw);
            const uint32_t role =
                (tex.psm == GS_PSM_T8 && tbwNow == 2u)  ? 1u
                : (tex.psm == GS_PSM_T8 && tbwNow == 4u) ? 2u
                : (tex.psm == GS_PSM_T4 && tbwNow == 4u) ? 3u
                                                         : 9u;

            g_dpRoleSeen[role < kDpRoles ? role : 9u].fetch_add(1, std::memory_order_relaxed);
            t_bgSkip = (role == 2u) && ps2diag_skipbg::enabled();

            // Window start comes from the frame boundary above, never from a
            // sprite in the population being ordered. Arm is consumed either
            // way; if a report is still pending the next boundary re-arms.
            g_dpPrevRole.store(role, std::memory_order_relaxed);
            if (g_dpArm.exchange(0u, std::memory_order_acq_rel) != 0u &&
                g_dpReady.load(std::memory_order_acquire) == 0u)
            {
                g_dpIdx.store(0u, std::memory_order_relaxed);
                g_dpCapturing.store(1u, std::memory_order_release);
            }

            if (g_dpCapturing.load(std::memory_order_acquire) != 0u)
            {
                const uint32_t i = g_dpIdx.fetch_add(1, std::memory_order_relaxed);
                if (i < kDpRing)
                {
                    g_dpRole[i].store(role, std::memory_order_relaxed);
                    g_dpPath[i].store(
                        ps2diag_gifpath::g_curPath.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                    g_dpSpanX[i].store(static_cast<uint32_t>(spanX), std::memory_order_relaxed);
                    g_dpSpanY[i].store(static_cast<uint32_t>(spanY), std::memory_order_relaxed);
                    g_dpTbp[i].store(static_cast<uint32_t>(tex.tbp0), std::memory_order_relaxed);
                    g_dpSite[i].store(
                        ps2diag_gifpath::g_curSite.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                    g_dpSrc[i].store(
                        ps2diag_gifpath::g_curSrc.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
                }
                if (i + 1u >= kDpRing)
                {
                    g_dpCount.store(kDpRing, std::memory_order_relaxed);
                    g_dpCapturing.store(0u, std::memory_order_release);
                    g_dpReady.store(1u, std::memory_order_release);
                }
            }
        }
        // ---------------------------------------------------------------------

        if (ps2diag_fbstat::t_boxDraw)
        {
            using namespace ps2diag_fbstat;
            g_bxDraws.fetch_add(1, std::memory_order_relaxed);
            g_bxTex0.store(static_cast<uint64_t>(tex.data), std::memory_order_relaxed);
            g_bxX0.store(static_cast<int32_t>(unclippedX0), std::memory_order_relaxed);
            g_bxY0.store(static_cast<int32_t>(unclippedY0), std::memory_order_relaxed);
            g_bxSpanX.store(static_cast<int32_t>(spanX), std::memory_order_relaxed);
            g_bxSpanY.store(static_cast<int32_t>(spanY), std::memory_order_relaxed);

            // Two palette entries the dump names exactly, read through the
            // same call the sampler uses so this is not a private code path:
            //   entry 50 is what the box SHOULD resolve to (0x8017176F, red)
            //   entry 8  is what we currently paint  (0x80000000, black)
            g_bxEnt50.store(gs->ReadClutCache(static_cast<u32>(tex.cpsm), 50,
                                              static_cast<u32>(tex.csa)),
                            std::memory_order_relaxed);
            g_bxEnt8.store(gs->ReadClutCache(static_cast<u32>(tex.cpsm), 8,
                                             static_cast<u32>(tex.csa)),
                           std::memory_order_relaxed);
        }

        for (int y = drawY0; y <= drawY1; ++y)
        {
            float ty = (static_cast<float>(y - unclippedY0) + 0.5f) / spriteH;
            float texVf = v0f + (v1f - v0f) * ty;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float tx = (static_cast<float>(x - unclippedX0) + 0.5f) / spriteW;
                float texUf = u0f + (u1f - u0f) * tx;
                uint32_t texel = 0xFFFF00FFu;
                if (prim.fst)
                {
                    const int fixedU = static_cast<int>((texUf * 16.0f) + 0.5f);
                    const int fixedV = static_cast<int>((texVf * 16.0f) + 0.5f);
                    const uint16_t sampleU = static_cast<uint16_t>(clampInt(fixedU, 0, 0xFFFF));
                    const uint16_t sampleV = static_cast<uint16_t>(clampInt(fixedV, 0, 0xFFFF));
                    texel = sampleTexture(gs, 0.0f, 0.0f, 1.0f, sampleU, sampleV);
                }
                else
                {
                    texel = sampleTexture(gs,
                                          texUf / static_cast<float>(texW),
                                          texVf / static_cast<float>(texH),
                                          1.0f, 0u, 0u);
                }

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                ps2diag_fbstat::t_lastTexel = texel;

                // [boxtex] (6): the post-CLUT, post-TEXA colour the box loop
                // actually feeds to the blender.
                if (ps2diag_fbstat::t_boxDraw)
                    ps2diag_fbstat::g_bxTexel.store(texel, std::memory_order_relaxed);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                writePixel(gs, x, y, z1, color.r, color.g, color.b, color.a);
            }
        }

        ps2diag_fbstat::t_boxDraw = false;
        ps2diag_fbstat::t_glyphDraw = false;
        ps2diag_fbstat::t_bgSkip = false;
    }
    else
    {
        for (int y = drawY0; y <= drawY1; ++y)
            for (int x = drawX0; x <= drawX1; ++x)
                writePixel(gs, x, y, z1, r, g, b, a);
    }
}

void GSRasterizer::drawTriangle(GS *gs)
{
    const auto prim = gs->m_registers.prim;

    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const GSVertex &v2 = gs->m_vtxQueue[2];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    float fx0 = v0.x - static_cast<float>(ofx);
    float fy0 = v0.y - static_cast<float>(ofy);
    float fx1 = v1.x - static_cast<float>(ofx);
    float fy1 = v1.y - static_cast<float>(ofy);
    float fx2 = v2.x - static_cast<float>(ofx);
    float fy2 = v2.y - static_cast<float>(ofy);

    int minX = static_cast<int>(std::floor(std::min({fx0, fx1, fx2})));
    int maxX = static_cast<int>(std::ceil(std::max({fx0, fx1, fx2})));
    int minY = static_cast<int>(std::floor(std::min({fy0, fy1, fy2})));
    int maxY = static_cast<int>(std::ceil(std::max({fy0, fy1, fy2})));

    minX = clampInt(minX, ctx.scissor.x0, ctx.scissor.x1);
    maxX = clampInt(maxX, ctx.scissor.x0, ctx.scissor.x1);
    minY = clampInt(minY, ctx.scissor.y0, ctx.scissor.y1);
    maxY = clampInt(maxY, ctx.scissor.y0, ctx.scissor.y1);

    float denom = (fy1 - fy2) * (fx0 - fx2) + (fx2 - fx1) * (fy0 - fy2);
    if (std::fabs(denom) < 0.001f)
        return;

    const float winding = (denom < 0.0f) ? -1.0f : 1.0f;
    const float invAbsDenom = 1.0f / std::fabs(denom);
    constexpr float kEdgeEpsilon = 1.0e-4f;

    for (int y = minY; y <= maxY; ++y)
    {
        float py = static_cast<float>(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x)
        {
            float px = static_cast<float>(x) + 0.5f;

            float w0 = (((fy1 - fy2) * (px - fx2) + (fx2 - fx1) * (py - fy2)) * winding) * invAbsDenom;
            float w1 = (((fy2 - fy0) * (px - fx2) + (fx0 - fx2) * (py - fy2)) * winding) * invAbsDenom;
            float w2 = 1.0f - w0 - w1;

            if (w0 < -kEdgeEpsilon || w1 < -kEdgeEpsilon || w2 < -kEdgeEpsilon)
                continue;

            double z = v0.z * w0 + v1.z * w1 + v2.z * w2;

            uint8_t r, g, b, a;
            if (prim.iip)
            {
                r = clampU8(static_cast<int>(v0.r * w0 + v1.r * w1 + v2.r * w2));
                g = clampU8(static_cast<int>(v0.g * w0 + v1.g * w1 + v2.g * w2));
                b = clampU8(static_cast<int>(v0.b * w0 + v1.b * w1 + v2.b * w2));
                a = clampU8(static_cast<int>(v0.a * w0 + v1.a * w1 + v2.a * w2));
            }
            else
            {
                r = v2.r;
                g = v2.g;
                b = v2.b;
                a = v2.a;
            }

            if (prim.tme)
            {
                float is, it, iq;
                uint16_t iu, iv;
                if (prim.fst)
                {
                    iu = static_cast<uint16_t>(v0.u * w0 + v1.u * w1 + v2.u * w2);
                    iv = static_cast<uint16_t>(v0.v * w0 + v1.v * w1 + v2.v * w2);
                    is = 0.0f;
                    it = 0.0f;
                    iq = 1.0f;
                }
                else
                {
                    const float invQ0 = 1.0f / fabsQ(v0.q);
                    const float invQ1 = 1.0f / fabsQ(v1.q);
                    const float invQ2 = 1.0f / fabsQ(v2.q);
                    const float sOverQ = (v0.s * invQ0) * w0 + (v1.s * invQ1) * w1 + (v2.s * invQ2) * w2;
                    const float tOverQ = (v0.t * invQ0) * w0 + (v1.t * invQ1) * w1 + (v2.t * invQ2) * w2;
                    const float invQ = invQ0 * w0 + invQ1 * w1 + invQ2 * w2;
                    iq = (std::fabs(invQ) > 1.0e-8f) ? (1.0f / invQ) : 1.0f;
                    is = sOverQ * iq;
                    it = tOverQ * iq;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel = sampleTexture(gs, is, it, iq, iu, iv);

                ps2diag_fbstat::t_lastTexel = texel;

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const auto &tex = ctx.tex0;
                const uint8_t shadeR = r;
                const uint8_t shadeG = g;
                const uint8_t shadeB = b;
                const uint8_t shadeA = a;
                const TextureCombineResult color = combineTexture(tex, shadeR, shadeG, shadeB, shadeA, tr, tg, tb, ta);

                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            writePixel(gs, x, y, static_cast<u32>(z + 0.5), r, g, b, a);
        }
    }
}

void GSRasterizer::drawLine(GS *gs)
{
    const auto prim = gs->m_registers.prim;

    const GSVertex &v0 = gs->m_vtxQueue[0];
    const GSVertex &v1 = gs->m_vtxQueue[1];
    const auto &ctx = gs->activeContext();

    int ofx = ctx.xyoffset.ofx >> 4;
    int ofy = ctx.xyoffset.ofy >> 4;

    int x0 = static_cast<int>(v0.x) - ofx;
    int y0 = static_cast<int>(v0.y) - ofy;
    int x1 = static_cast<int>(v1.x) - ofx;
    int y1 = static_cast<int>(v1.y) - ofy;

    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;

    int totalSteps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (totalSteps == 0)
        totalSteps = 1;
    int step = 0;

    for (;;)
    {
        float t = static_cast<float>(step) / static_cast<float>(totalSteps);
        uint8_t r, g, b, a;
        if (prim.iip)
        {
            r = clampU8(static_cast<int>(v0.r + (v1.r - v0.r) * t));
            g = clampU8(static_cast<int>(v0.g + (v1.g - v0.g) * t));
            b = clampU8(static_cast<int>(v0.b + (v1.b - v0.b) * t));
            a = clampU8(static_cast<int>(v0.a + (v1.a - v0.a) * t));
        }
        else
        {
            r = v1.r;
            g = v1.g;
            b = v1.b;
            a = v1.a;
        }

        double z = (v0.z + (v1.z - v0.z) * t);

        writePixel(gs, x0, y0, static_cast<u32>(z), r, g, b, a);

        if (x0 == x1 && y0 == y1)
            break;

        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
        ++step;
    }
}
