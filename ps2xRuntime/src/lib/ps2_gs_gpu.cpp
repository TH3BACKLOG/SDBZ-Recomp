#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "ps2_log.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_gs_memory.h"
#include "runtime/ps2_diag.h"
#include "runtime/ps2_pipeline_stats.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// Defined in ps2_gs_rasterizer.cpp -- see the [fbdest] block in the present
// probe below. Declared here rather than in a header so a diagnostic counter
// never triggers a full 30,000-TU rebuild.
namespace ps2diag_fbstat
{
    extern std::atomic<uint32_t> g_fbNonBlack[512];
    extern std::atomic<uint32_t> g_fbBlack[512];
    extern std::atomic<uint64_t> g_writeSeq;
    extern std::atomic<uint64_t> g_lastNonBlackSeq;

    // [blackwho] -- see the definition comment in ps2_gs_rasterizer.cpp.
    extern std::atomic<uint64_t> g_blkUntex;
    extern std::atomic<uint64_t> g_blkTexZero;
    extern std::atomic<uint64_t> g_blkTexCol;

    extern std::atomic<uint32_t> g_tailX0;
    extern std::atomic<uint32_t> g_tailY0;
    extern std::atomic<uint32_t> g_tailX1;
    extern std::atomic<uint32_t> g_tailY1;

    extern std::atomic<uint32_t> g_snapPrim;
    extern std::atomic<uint32_t> g_snapTexPsm;
    extern std::atomic<uint32_t> g_snapTbp;
    extern std::atomic<uint32_t> g_snapCbp;
    extern std::atomic<uint32_t> g_snapFbmsk;
    extern std::atomic<uint32_t> g_snapAlpha;
    extern std::atomic<uint32_t> g_snapTexel;
    extern std::atomic<uint32_t> g_snapSrcRgb;
    extern std::atomic<uint32_t> g_snapXy;
    extern std::atomic<uint32_t> g_snapFbp;

    // [texzero] -- see the definition comment in ps2_gs_rasterizer.cpp.
    extern std::atomic<uint64_t> g_tzAbeOn;
    extern std::atomic<uint64_t> g_tzAbeOff;
    extern std::atomic<uint64_t> g_tzAteOn;
    extern std::atomic<uint32_t> g_tzPrim;
    extern std::atomic<uint32_t> g_tzTest;
    extern std::atomic<uint32_t> g_tzAlpha;
    extern std::atomic<uint32_t> g_tzSrcA;
    extern std::atomic<uint32_t> g_tzTexel;
    extern std::atomic<uint32_t> g_tzTbp;
    extern std::atomic<uint32_t> g_tzCbp;
    extern std::atomic<uint32_t> g_tzTexPsm;
    extern std::atomic<uint32_t> g_tzDstRgb;
    extern std::atomic<uint32_t> g_tzFbp;
    // [clutmap] -- see the pre-registered reading table at the declaration site
    // in ps2_gs_rasterizer.cpp.
    extern std::atomic<uint32_t> g_tzIdxOr;
    extern std::atomic<uint32_t> g_tzIdxNz;
    extern std::atomic<uint32_t> g_tzIdxSamp;
    extern std::atomic<uint32_t> g_tzIdxLast;
    extern std::atomic<uint32_t> g_tzCsa;
    extern std::atomic<uint32_t> g_tzCsm;
    extern std::atomic<uint32_t> g_tzCpsm;
    extern std::atomic<uint32_t> g_tzX0;
    extern std::atomic<uint32_t> g_tzY0;
    extern std::atomic<uint32_t> g_tzX1;
    extern std::atomic<uint32_t> g_tzY1;
    // [texred] -- see the pre-registered reading table at the declaration site
    // in ps2_gs_rasterizer.cpp. Read anysamp FIRST: it is the liveness guard.
    extern std::atomic<uint64_t> g_trAnySamp;
    extern std::atomic<uint64_t> g_trRedSamp;
    extern std::atomic<uint64_t> g_trBoxSamp;
    extern std::atomic<uint64_t> g_trKillAte;
    extern std::atomic<uint64_t> g_trKillZ;
    extern std::atomic<uint64_t> g_trStored;
    extern std::atomic<uint64_t> g_trStoredBlack;
    extern std::atomic<uint32_t> g_trTexel;
    extern std::atomic<uint32_t> g_trTest;
    extern std::atomic<uint32_t> g_trAlpha;
    extern std::atomic<uint32_t> g_trPrim;
    extern std::atomic<uint32_t> g_trSrcA;
    extern std::atomic<uint32_t> g_trSrcRgb;
    extern std::atomic<uint32_t> g_trDstRgb;
    extern std::atomic<uint32_t> g_trPixel;
    extern std::atomic<uint32_t> g_trFbpOr;
    extern std::atomic<uint32_t> g_trFbpLast;
    extern std::atomic<uint32_t> g_trTexPsm;
    // [boxblk] -- reading table is at the declaration site in
    // ps2_gs_rasterizer.cpp. Read bbany FIRST (liveness guard), then bb16,
    // then bbred (the rival reading), and only then the index histogram.
    extern std::atomic<uint64_t> g_bbAny;
    extern std::atomic<uint64_t> g_bbBlack;
    extern std::atomic<uint64_t> g_bbRed;
    extern std::atomic<uint64_t> g_bbBlkTex;
    extern std::atomic<uint64_t> g_bbBlkUntex;
    extern std::atomic<uint64_t> g_bb16;
    extern std::atomic<uint64_t> g_bbIdx[9];
    extern std::atomic<uint32_t> g_bbIdxLast;
    extern std::atomic<uint32_t> g_bbTexel;
    extern std::atomic<uint32_t> g_bbTexPsm;
    extern std::atomic<uint32_t> g_bbTbp;
    extern std::atomic<uint32_t> g_bbCbp;
    extern std::atomic<uint32_t> g_bbCsa;
    extern std::atomic<uint32_t> g_bbCpsm;

    // [texfetch] -- Stage 5.11 run 24, re-pointed at the T4 glyph fetch.
    // Reading table lives at the counter definitions in
    // ps2_gs_rasterizer.cpp; do not read these without it.
    extern std::atomic<uint64_t> g_tfSamp;
    extern std::atomic<uint64_t> g_tfAll;
    extern std::atomic<uint64_t> g_tfT4;
    extern std::atomic<uint64_t> g_tfHist[16];
    extern std::atomic<uint64_t> g_tfOor;
    extern std::atomic<uint32_t> g_tfIdxMax;
    extern std::atomic<uint64_t> g_tfChk;
    extern std::atomic<uint64_t> g_tfDis;
    extern std::atomic<uint32_t> g_tfBadU;
    extern std::atomic<uint32_t> g_tfBadV;
    extern std::atomic<uint32_t> g_tfBadCache;
    extern std::atomic<uint32_t> g_tfBadVram;
    extern std::atomic<uint32_t> g_tfUmin;
    extern std::atomic<uint32_t> g_tfUmax;
    extern std::atomic<uint32_t> g_tfVmin;
    extern std::atomic<uint32_t> g_tfVmax;
    extern std::atomic<uint32_t> g_tfTbw;
    extern std::atomic<uint32_t> g_tfTw;
    extern std::atomic<uint32_t> g_tfTh;
    extern std::atomic<uint32_t> g_tfTbp;
    extern std::atomic<uint32_t> g_tfCsa;
    extern std::atomic<uint32_t> g_tfCpsm;
    extern std::atomic<uint64_t> g_tfA0;
    extern std::atomic<uint32_t> g_tfAMax;
    extern std::atomic<uint64_t> g_tfWhite;

    // [glyphfate] -- Stage 5.11 run 25. Reading table lives at the counter
    // definitions in ps2_gs_rasterizer.cpp. Read gfdraws/gfin FIRST: they are
    // the guards, and a zero in either makes every later field stale.
    extern std::atomic<uint64_t> g_gfDraws;
    extern std::atomic<uint64_t> g_gfIn;
    extern std::atomic<uint64_t> g_gfScis;
    extern std::atomic<uint64_t> g_gfAte;
    extern std::atomic<uint64_t> g_gfZ;
    extern std::atomic<uint64_t> g_gfStored;
    extern std::atomic<uint64_t> g_gfWhite;
    extern std::atomic<uint64_t> g_gfDark;
    extern std::atomic<uint32_t> g_gfSrcAMax;
    extern std::atomic<uint32_t> g_gfX0;
    extern std::atomic<uint32_t> g_gfY0;
    extern std::atomic<uint32_t> g_gfX1;
    extern std::atomic<uint32_t> g_gfY1;
    extern std::atomic<uint32_t> g_gfPix;
    extern std::atomic<uint32_t> g_gfDst;
    extern std::atomic<uint32_t> g_gfFbpOr;
    extern std::atomic<uint32_t> g_gfFbpMin;
    extern std::atomic<uint32_t> g_gfFbpMax;
    extern std::atomic<uint32_t> g_gfTest;
    extern std::atomic<uint32_t> g_gfCc;
    extern std::atomic<uint32_t> g_gfAbe;
    extern std::atomic<uint32_t> g_gfScX0;
    extern std::atomic<uint32_t> g_gfScY0;
    extern std::atomic<uint32_t> g_gfScX1;
    extern std::atomic<uint32_t> g_gfScY1;
    extern std::atomic<uint64_t> g_gfLastSeq;
    extern std::atomic<uint64_t> g_gfBoxSeq;
    extern std::atomic<uint64_t> g_writeSeq;

    // [fbsplit] -- Stage 5.11 run 26. Reading table is at the counter
    // definitions in ps2_gs_rasterizer.cpp. Read glyph=/box= FIRST: if both
    // lists are empty the dialog was not up in this interval and every other
    // field on the line describes nothing.
    extern std::atomic<uint32_t> g_fsGlyphByFbp[512];
    extern std::atomic<uint32_t> g_fsBoxByFbp[512];
    extern std::atomic<uint64_t> g_fsGlyphOrd[512];
    extern std::atomic<uint64_t> g_fsBoxOrd[512];
    extern std::atomic<uint64_t> g_fsOrdSeq;
    extern std::atomic<uint32_t> g_fsGlyphDstOr;
    extern std::atomic<uint32_t> g_fsBoxPix;

    // [boxover] -- Stage 5.11 run 27. The reciprocal of run 26's dstor: what
    // the BOX lands on. Check bwhite+bblack+bother == bstores first; if that
    // identity fails the classifier is broken and nothing else here counts.
    extern std::atomic<uint64_t> g_boStores;
    extern std::atomic<uint64_t> g_boWhite;
    extern std::atomic<uint64_t> g_boBlack;
    extern std::atomic<uint64_t> g_boOther;
    extern std::atomic<uint32_t> g_boDstOr;
    extern std::atomic<uint32_t> g_boDstWhite;
    extern std::atomic<uint32_t> g_boAbe;
    extern std::atomic<uint32_t> g_boSrcAMax;
    extern std::atomic<uint32_t> g_boAlphaReg;

    // [fbaddr] -- Stage 5.11 run 28. The three terms the VRAM address is
    // actually built from (fbp/fbw/fpsm), sampled per sprite, plus fbmsk and
    // the store-site XY extents. Read the constancy flags before the verdict:
    // an OR/AND pair that disagrees on fbw or fpsm means the value moved
    // within the interval and no cross-sprite comparison of it is meaningful.
    extern std::atomic<uint64_t> g_faGN;
    extern std::atomic<uint64_t> g_faBN;
    extern std::atomic<uint32_t> g_faGFbpOr, g_faGFbpAnd, g_faBFbpOr, g_faBFbpAnd;
    extern std::atomic<uint32_t> g_faGFbwOr, g_faGFbwAnd, g_faBFbwOr, g_faBFbwAnd;
    extern std::atomic<uint32_t> g_faGPsmOr, g_faGPsmAnd, g_faBPsmOr, g_faBPsmAnd;
    extern std::atomic<uint32_t> g_faGMskOr, g_faGMskAnd, g_faBMskOr, g_faBMskAnd;
    extern std::atomic<uint32_t> g_faGX0, g_faGX1, g_faGY0, g_faGY1;
    extern std::atomic<uint32_t> g_faBX0, g_faBX1, g_faBY0, g_faBY1;

    // [pixlog] -- Stage 5.11 run 29. One latched address, every writer, in
    // order. kPlRing must match the definition in ps2_gs_rasterizer.cpp; the
    // ring is read oldest-first from (count % kPlRing) and a slot whose ord is
    // 0 was never filled. tag: 0 = untagged third party, 1 = box, 2 = glyph.
    // Run 30 adds a second latch point and the texture identity per entry;
    // texel is the fetched value BEFORE blending, which is what separates a
    // degenerate fetch from a destructive blend.
    constexpr uint32_t kPlRing = 8;
    constexpr uint32_t kPlPts = 2;
    extern std::atomic<uint32_t> g_plClaim[kPlPts], g_plArmed[kPlPts];
    extern std::atomic<uint32_t> g_plX[kPlPts], g_plY[kPlPts], g_plFbp[kPlPts];
    extern std::atomic<uint64_t> g_plCount[kPlPts], g_plGlyphHits[kPlPts];
    extern std::atomic<uint64_t> g_plOrd[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plTag[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plVal[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plDst[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plPrim[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plTexel[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plTbp[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plCbp[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plTfmt[kPlPts][kPlRing];
    extern std::atomic<uint32_t> g_plAlpha[kPlPts][kPlRing];

    // ---- [drawpath] -- Stage 5.11 run 32, re-gated run 38 -----------------
    // kDpRing MUST track the definition in ps2_gs_rasterizer.cpp.
    constexpr uint32_t kDpRing = 320;
    constexpr uint32_t kDpRoles = 10;
    extern std::atomic<uint32_t> g_dpReady;
    extern std::atomic<uint32_t> g_dpCount;
    extern std::atomic<uint64_t> g_dpArmClear;
    extern std::atomic<uint64_t> g_dpArmCopy;
    extern std::atomic<uint64_t> g_dpRoleSeen[kDpRoles];
    extern std::atomic<uint32_t> g_dpRole[kDpRing];
    extern std::atomic<uint32_t> g_dpPath[kDpRing];
    extern std::atomic<uint32_t> g_dpSpanX[kDpRing];
    extern std::atomic<uint32_t> g_dpSpanY[kDpRing];
    extern std::atomic<uint32_t> g_dpTbp[kDpRing];
    extern std::atomic<uint32_t> g_dpSite[kDpRing];
    extern std::atomic<uint32_t> g_dpSrc[kDpRing];

    // [uvspan] -- Stage 5.11 run 23, re-pointed at the glyph sprites. The
    // reading table lives at the counter definitions in
    // ps2_gs_rasterizer.cpp; do not read one field here without it. Read
    // allspr/t4any/draws FIRST -- they are the guards, and they are what
    // makes a zero result diagnosable. Scale factors are documented there:
    // u/v and du are /256, s/t/q and ds are /65536, rawu/rawv are unscaled
    // 4.4 fixed point.
    extern std::atomic<uint64_t> g_uvAllSpr;
    extern std::atomic<uint64_t> g_uvT4Any;
    extern std::atomic<uint64_t> g_uvDraws;
    extern std::atomic<uint64_t> g_uvFlat;
    extern std::atomic<uint64_t> g_uvWide;
    extern std::atomic<uint64_t> g_uvFst0;
    extern std::atomic<uint64_t> g_uvFst1;
    extern std::atomic<uint64_t> g_uvQBad;
    extern std::atomic<uint32_t> g_uvDuMin;
    extern std::atomic<uint32_t> g_uvDuMax;
    extern std::atomic<uint32_t> g_uvDsMin;
    extern std::atomic<uint32_t> g_uvDsMax;
    extern std::atomic<uint32_t> g_uvFst;
    extern std::atomic<int32_t> g_uvU0;
    extern std::atomic<int32_t> g_uvV0;
    extern std::atomic<int32_t> g_uvU1;
    extern std::atomic<int32_t> g_uvV1;
    extern std::atomic<int32_t> g_uvS0;
    extern std::atomic<int32_t> g_uvT0;
    extern std::atomic<int32_t> g_uvQ0;
    extern std::atomic<int32_t> g_uvS1;
    extern std::atomic<int32_t> g_uvT1;
    extern std::atomic<int32_t> g_uvQ1;
    extern std::atomic<int32_t> g_uvRawU0;
    extern std::atomic<int32_t> g_uvRawV0;
    extern std::atomic<int32_t> g_uvRawU1;
    extern std::atomic<int32_t> g_uvRawV1;
    extern std::atomic<int32_t> g_uvSpanX;
    extern std::atomic<int32_t> g_uvSpanY;

    // [boxtex] -- Stage 5.11 run 22. The reading table lives at the counter
    // definitions in ps2_gs_rasterizer.cpp and MUST be read alongside these.
    // The 12 below is kBxCen there; it is spelled out because the constant
    // cannot cross the TU boundary without touching a header.
    extern std::atomic<uint64_t> g_bxAll;
    extern std::atomic<uint64_t> g_bxDraws;
    extern std::atomic<uint64_t> g_bxCenKey[12];
    extern std::atomic<uint64_t> g_bxCenCnt[12];
    extern std::atomic<uint64_t> g_bxCenOvf;
    extern std::atomic<uint64_t> g_bxTex0;
    extern std::atomic<int32_t> g_bxX0;
    extern std::atomic<int32_t> g_bxY0;
    extern std::atomic<int32_t> g_bxSpanX;
    extern std::atomic<int32_t> g_bxSpanY;
    extern std::atomic<uint64_t> g_bxIdxSamp;
    extern std::atomic<uint64_t> g_bxIdxHist[16];
    extern std::atomic<uint32_t> g_bxIdxMin;
    extern std::atomic<uint32_t> g_bxIdxMax;
    extern std::atomic<uint32_t> g_bxEnt50;
    extern std::atomic<uint32_t> g_bxEnt8;
    extern std::atomic<uint32_t> g_bxTexel;

    extern std::atomic<uint32_t> g_bbPrim;
    extern std::atomic<uint32_t> g_bbTest;
    extern std::atomic<uint32_t> g_bbSrcA;
    extern std::atomic<uint32_t> g_bbXy;
    extern std::atomic<uint32_t> g_bbFbp;
    extern std::atomic<uint32_t> g_trX0;
    extern std::atomic<uint32_t> g_trY0;
    extern std::atomic<uint32_t> g_trX1;
    extern std::atomic<uint32_t> g_trY1;

    // ---- [frameord] -------------------------------------------------------
    // [texzero] exonerated the dominant black population: alpha=0x44 decodes to
    // (Cs - Cd)*As + Cd, and those stores carry srca=0, so they write the
    // destination back UNCHANGED. They are counted black only because Cd was
    // already black. All three [blackwho] buckets are now accounted for and
    // none of them destroys colour.
    //
    // What survives is the shape of [fbdest]: lastnbpct=99 with
    // tail=0,0..511,447 -- after the final coloured pixel there is a
    // full-screen black run. But [fbdest] is emitted on the ~1/sec THROTTLED
    // interval, so its seq/lastnb span ~60 frames and cannot tell
    //   (a) "a clear runs after the draws EVERY frame" from
    //   (b) "one clear happened at the end of the interval".
    // Only (a) is a bug.
    //
    // These accumulate a per-latch (per-frame, unthrottled) measurement and
    // are drained on the throttled interval. Host-side only -- the rasterizer
    // already publishes everything needed, so nothing in the hot pixel loop
    // changes.
    std::atomic<uint64_t> g_foFrames{0};   // latches sampled
    std::atomic<uint64_t> g_foWiped{0};    // latches whose trailing black run >= half a screen
    std::atomic<uint64_t> g_foMaxTrail{0}; // largest trailing black run seen
    std::atomic<uint64_t> g_foLastTrail{0};
    std::atomic<uint32_t> g_foWipeX0{0xFFFFu}; // tail bbox at the last wiped latch
    std::atomic<uint32_t> g_foWipeY0{0xFFFFu};
    std::atomic<uint32_t> g_foWipeX1{0};
    std::atomic<uint32_t> g_foWipeY1{0};

    // ---- [clutlive] -------------------------------------------------------
    // Stage 5.11. Both producer (the TEX0 write handler) and consumer (the
    // present probe) live in THIS TU, so these are defined here rather than
    // declared extern -- and, like everything else in this namespace, they stay
    // out of any header so a diagnostic never costs a 30,000-TU rebuild.
    //
    // WHY THIS PROBE EXISTS. gsdump_diff.py (rebuilt on format+geometry, not
    // addresses) proved the live EE emits the memory-card dialog with the same
    // formats, the same per-role draw counts, the same 17x17 glyph cell and a
    // pixel-identical 464x120 @ (24,302) box as an authentic PCSX2 capture.
    // The ONLY divergence is where the CLUT lives: dump cbp=0x2B08, live
    // cbp=0x2A48. That is VRAM allocation -- two runs reached the screen by
    // different paths -- so cross-run address equality is NOT a valid test and
    // must never be reported as a finding. The remaining question is intra-run:
    // does the palette at the LIVE cbp correctly serve the atlas at tbp0?
    //
    // GROUND TRUTH, extracted from the dump with no build (gsdump_draws.py
    // --uploads plus a frame-0 event-order trace):
    //
    //   1. Ordering is  upload(atlas 0x2B60) -> upload(palette 0x2B08) ->
    //      TEX0 write consuming it -> draws. The palette is in VRAM BEFORE the
    //      TEX0 write. Our reload-on-every-TEX0-write is correctly ordered for
    //      that pattern, which DEMOTES the stale-cache hypothesis (it can only
    //      fire if the live EE inverts the order -- which cache0_15 vs
    //      vram0_15 below still detects for free).
    //
    //   2. The glyph CLUT is an ALPHA COVERAGE RAMP, not colours. All 16
    //      entries are RGB=(255,255,255) white; only alpha varies:
    //        0x00000000 0x08FFFFFF 0x11FFFFFF 0x19FFFFFF 0x22FFFFFF 0x2AFFFFFF
    //        0x33FFFFFF 0x3BFFFFFF 0x44FFFFFF 0x4CFFFFFF 0x55FFFFFF 0x5DFFFFFF
    //        0x66FFFFFF 0x6EFFFFFF 0x77FFFFFF 0x80FFFFFF
    //      So the T4 index is an antialiasing coverage LEVEL, and index 0 is
    //      fully transparent. Max alpha 0x80 = 128 = 1.0 on PS2 (alpha is
    //      0..128, NOT 0..255). Live ALPHA=0x44 decodes to (Cs-Cd)*As + Cd,
    //      the correct standard blend -- the blend equation is exonerated.
    //
    // That ramp is the whole reason the top suspect changed. If our T4 index
    // fetch is degenerate and yields 0 everywhere, every glyph texel is fully
    // transparent, no text is composited, and the dialog stays a flat maroon
    // box -- which is exactly the reported symptom.
    //
    // SCOPE LIMIT, stated up front so the result is not over-read: everything
    // here is measured HOST-side, from VRAM and the CLUT cache. It cannot see
    // what the rasterizer's sampler computed, because that would need a
    // rasterizer edit. So idxhist16 below answers "does the ATLAS DATA contain
    // a healthy spread of coverage indices?" -- NOT "did the sampler fetch
    // them correctly". Those are different questions and the difference is the
    // entire point of reading row 2 against row 5 in the table at the emit
    // site.
    std::atomic<uint32_t> g_clHits{0};   // atlas-shaped TEX0 writes seen (liveness guard)
    std::atomic<uint32_t> g_clTbp0{0};
    std::atomic<uint32_t> g_clTbw{0};
    std::atomic<uint32_t> g_clCbp{0};
    std::atomic<uint32_t> g_clCpsm{0};
    std::atomic<uint32_t> g_clCsm{0};
    std::atomic<uint32_t> g_clCsa{0};
    std::atomic<uint32_t> g_clCld{0};

    // ---- [clutlive] round 2: the AT-WRITE snapshot ------------------------
    // Round 1 answered two of its three questions and then tripped over its own
    // sampling point:
    //
    //   * idxnz=4898/65536 with idxhist16 spread across 0,3..12,14,15 -- the
    //     atlas holds real coverage data. The degenerate-T4-fetch hypothesis is
    //     REFUTED at the data level.
    //   * vram0_15 came back byte-identical to the authentic PCSX2 dump's alpha
    //     ramp (0x00000000, 0x08FFFFFF ... 0x80FFFFFF). The palette upload is
    //     correct in VRAM.
    //   * cache0_15 ALTERNATED between that white ramp and an all-RGB-black ramp
    //     (0x00000000, 0x0A000000 ... 0x77000000, nonblack=0/16).
    //
    // That last one is NOT yet a finding, and saying so is the whole reason this
    // block exists. The PS2 CLUT buffer is a single shared 1KB window; every
    // TEX0 write with cld!=0 reloads it. The round-1 consumer samples at PRESENT
    // time, long after the glyph draw, so a black ramp there may simply be some
    // later texture's palette sitting in the buffer entirely legitimately. A
    // probe cannot distinguish "stale for the glyph draw" from "correct for a
    // different draw" if it never samples at the draw.
    //
    // So: snapshot the cache INSIDE the producer, immediately after
    // ReloadClutCache returns, which is exactly what this atlas draw will
    // sample. Then classify every snapshot as white (>=15 non-black entries),
    // black (0), or other, so the counts answer it over the whole run instead of
    // at one arbitrary instant.
    //
    // READING, pre-registered:
    //   atw black == 0 && white == hits -> every atlas draw sees the correct
    //       white ramp. The round-1 present-time mismatch was a later texture,
    //       nothing more. Both inputs (atlas data + palette) are then proven
    //       good in memory and the fault is downstream of them: the sampler, or
    //       alpha scaling. Next step becomes a rasterizer-side probe.
    //   atw black > 0 -> some atlas draws sample an all-black palette. Black
    //       glyphs at <=50% alpha over a maroon box are near-invisible, which
    //       matches the symptom exactly. firstblack cbp= then names the palette.
    //   atw other dominant -> neither ramp; read atwrite0_15 verbatim.
    //   hits > 0 but white+black+other == 0 -> convict the probe, not the game.
    //
    // cbpcensus exists because the round-1 latch was last-write-wins on a single
    // cbp. If the gate matches two different palettes (e.g. a white text pass
    // and a black drop-shadow pass sharing one atlas) that collapse hides it.
    std::atomic<uint32_t> g_clSnap[16];    // cache as seen AT the atlas TEX0 write
    std::atomic<uint32_t> g_clSnapBad[16]; // first all-black snapshot, verbatim
    std::atomic<uint32_t> g_clAtwWhite{0};
    std::atomic<uint32_t> g_clAtwBlack{0};
    std::atomic<uint32_t> g_clAtwOther{0};
    std::atomic<uint32_t> g_clBadSeen{0};
    std::atomic<uint32_t> g_clBadCbp{0};
    // Distinct-cbp census. Slot value is cbp+1 so that 0 means "empty" and a
    // genuine cbp of 0 is not mistaken for one.
    std::atomic<uint32_t> g_clCbpVal[4];
    std::atomic<uint32_t> g_clCbpHit[4];
}

// ---- [chainord] -- Stage 5.11 run 33 ------------------------------------
// Defined in ps2_memory.cpp, recorded inside the VIF1 source-chain gather.
// Read the reading table at the definition site -- it carries the PCSX2
// ground-truth addresses these are meant to be compared against.
namespace ps2diag_chainord
{
    constexpr uint32_t kCoRing = 1024;
    extern std::atomic<uint32_t> g_coSeq;
    extern std::atomic<uint32_t> g_coAddr[kCoRing];
    extern std::atomic<uint32_t> g_coQwc[kCoRing];
    extern std::atomic<uint32_t> g_coOff[kCoRing];
}

namespace
{
    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    // ---------------------------------------------------------------------
    // [gsdump] -- Stage 5.9 tooling.
    //
    // Every probe so far has been a counter, and a counter can only answer one
    // yes/no per build+run cycle (~96s of run plus a link). The framebuffer is
    // ALREADY decoded to host RGBA inside the [fbscan] block below, so writing
    // it to disk costs nothing extra and lets us LOOK instead of infer.
    //
    // Entirely opt-in: nothing is written unless PS2X_GSDUMP names an output
    // directory, so a normal run pays one getenv. TGA rather than PNG so there
    // is no new dependency, and everything lives in this .cpp so no header is
    // touched (a header edit = 30,000-TU rebuild).
    //
    // Reading the output:
    //   fb_0x*.tga all black, but tex_*.txt shows non-zero bytes
    //       -> the texture data IS resident; the PSMT4 unswizzle or the CLUT
    //          lookup is producing index/colour 0
    //   tex_*.txt all zero
    //       -> the texture upload never landed at TBP; look upstream at the
    //          image-mode GIF transfer, not at the rasterizer
    //   clut_*.txt all zero
    //       -> palette upload is the fault; indices may be perfectly fine
    // ---------------------------------------------------------------------
    const char *gsDumpDir()
    {
        static const char *dir = std::getenv("PS2X_GSDUMP");
        return dir;
    }

    std::string gsDumpPath(const char *prefix, uint32_t id, const char *ext)
    {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "/%s_0x%04x.%s", prefix, id, ext);
        return std::string(gsDumpDir()) + buf;
    }

    // Uncompressed 32-bit TGA, top-left origin. Input is the host RGBA buffer
    // produced by copyFrameToHostRgbaUnlocked; TGA wants BGRA.
    bool writeTgaRgba(const std::string &path, uint32_t w, uint32_t h,
                      const std::vector<uint8_t> &rgba)
    {
        if (w == 0u || h == 0u)
        {
            return false;
        }
        if (rgba.size() < static_cast<size_t>(w) * static_cast<size_t>(h) * 4u)
        {
            return false;
        }

        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            return false;
        }

        uint8_t hdr[18] = {};
        hdr[2] = 2u; // uncompressed true-colour
        hdr[12] = static_cast<uint8_t>(w & 0xFFu);
        hdr[13] = static_cast<uint8_t>((w >> 8) & 0xFFu);
        hdr[14] = static_cast<uint8_t>(h & 0xFFu);
        hdr[15] = static_cast<uint8_t>((h >> 8) & 0xFFu);
        hdr[16] = 32u;
        hdr[17] = 0x28u; // top-left origin, 8 alpha bits
        std::fwrite(hdr, 1, sizeof(hdr), f);

        std::vector<uint8_t> row(static_cast<size_t>(w) * 4u);
        for (uint32_t y = 0; y < h; ++y)
        {
            const uint8_t *src = rgba.data() + static_cast<size_t>(y) * w * 4u;
            for (uint32_t x = 0; x < w; ++x)
            {
                row[x * 4u + 0u] = src[x * 4u + 2u];
                row[x * 4u + 1u] = src[x * 4u + 1u];
                row[x * 4u + 2u] = src[x * 4u + 0u];
                row[x * 4u + 3u] = src[x * 4u + 3u];
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }

        std::fclose(f);
        return true;
    }

    // Raw 32-bit words straight out of VRAM, 8 per line. Used for the CLUT,
    // where the whole question is "are these 16/256 entries all black".
    bool writeVramWords(const std::string &path, const uint8_t *vram, size_t vramSize,
                        size_t byteOffset, uint32_t wordCount)
    {
        if (!vram || byteOffset + static_cast<size_t>(wordCount) * 4u > vramSize)
        {
            return false;
        }

        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            return false;
        }

        std::fprintf(f, "# vram byte offset 0x%zx, %u words\n", byteOffset, wordCount);
        uint32_t nonZero = 0u;
        for (uint32_t i = 0; i < wordCount; ++i)
        {
            uint32_t v = 0u;
            std::memcpy(&v, vram + byteOffset + static_cast<size_t>(i) * 4u, 4);
            if ((v & 0x00FFFFFFu) != 0u)
            {
                ++nonZero;
            }
            std::fprintf(f, "%08x%s", v, ((i % 8u) == 7u) ? "\n" : " ");
        }
        std::fprintf(f, "\n# nonblack=%u of %u\n", nonZero, wordCount);
        std::fclose(f);
        return true;
    }

    // Per-256-byte-block census of a VRAM region. Deliberately swizzle-blind:
    // whether or not our PSMT4 unswizzle is correct, zero bytes stay zero, so
    // this answers "did any texture data arrive at all" without depending on
    // the code path currently under suspicion.
    bool writeVramCensus(const std::string &path, const uint8_t *vram, size_t vramSize,
                         size_t byteOffset, size_t byteLen)
    {
        if (!vram || byteOffset >= vramSize)
        {
            return false;
        }
        byteLen = std::min(byteLen, vramSize - byteOffset);

        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
        {
            return false;
        }

        std::fprintf(f, "# vram byte offset 0x%zx, %zu bytes, per-256B block\n",
                     byteOffset, byteLen);
        size_t totalNonZero = 0u;
        const size_t blocks = (byteLen + 255u) / 256u;
        for (size_t b = 0; b < blocks; ++b)
        {
            const size_t start = byteOffset + b * 256u;
            const size_t len = std::min<size_t>(256u, byteOffset + byteLen - start);
            uint32_t hits = 0u;
            for (size_t i = 0; i < len; ++i)
            {
                if (vram[start + i] != 0u)
                {
                    ++hits;
                }
            }
            totalNonZero += hits;
            if (hits != 0u)
            {
                std::fprintf(f, "block %zu (tbp+%zu) nonzero=%u/%zu\n", b, b, hits, len);
            }
        }
        std::fprintf(f, "# blocks=%zu totalnonzero=%zu of %zu\n",
                     blocks, totalNonZero, byteLen);
        std::fclose(f);
        return true;
    }

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    struct PackedGifPacketTag
    {
        uint64_t lo = 0u;
        uint64_t hi = 0u;
        uint32_t payloadOffset = 0u;
        uint32_t nloop = 0u;
        uint32_t nreg = 0u;
        uint8_t regs[16]{};
    };

    template <typename Visitor>
    bool visitPackedGifPacket(const uint8_t *data, uint32_t sizeBytes, Visitor &&visitor)
    {
        uint32_t offset = 0u;
        while (offset + 16u <= sizeBytes)
        {
            PackedGifPacketTag tag{};
            tag.lo = loadLE64(data + offset);
            tag.hi = loadLE64(data + offset + 8u);

            const uint8_t flg = static_cast<uint8_t>((tag.lo >> 58u) & 0x3u);
            if (flg != GIF_FMT_PACKED)
                return false;

            tag.nloop = static_cast<uint32_t>(tag.lo & 0x7FFFu);
            tag.nreg = static_cast<uint32_t>((tag.lo >> 60u) & 0xFu);
            if (tag.nreg == 0u)
                tag.nreg = 16u;

            const uint64_t payloadBytes64 =
                static_cast<uint64_t>(tag.nloop) * static_cast<uint64_t>(tag.nreg) * 16ull;
            if (payloadBytes64 > 0xFFFFFFFFull)
                return false;

            offset += 16u;
            const uint32_t payloadBytes = static_cast<uint32_t>(payloadBytes64);
            if (payloadBytes > sizeBytes - offset)
                return false;

            tag.payloadOffset = offset;
            for (uint32_t i = 0u; i < tag.nreg; ++i)
                tag.regs[i] = static_cast<uint8_t>((tag.hi >> (i * 4u)) & 0xFu);

            if (!visitor(tag))
                return false;

            offset += payloadBytes;
        }

        return offset == sizeBytes;
    }

    bool validatePackedGifPacket(const uint8_t *data, uint32_t sizeBytes)
    {
        return visitPackedGifPacket(data, sizeBytes, [](const PackedGifPacketTag &) { return true; });
    }

    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight)
    {
        const uint32_t dx = static_cast<uint32_t>((display64 >> 0) & 0x0FFFu);
        const uint32_t dy = static_cast<uint32_t>((display64 >> 12) & 0x07FFu);
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;

        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }

        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        GSDisplayReadOrigin origin{};
        origin.x = static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu);
        origin.y = static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu);
        return origin;
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        GSPmodeState pmode{};
        pmode.enableCrt1 = (pmode64 & 0x1ull) != 0ull;
        pmode.enableCrt2 = (pmode64 & 0x2ull) != 0ull;
        pmode.mmod = ((pmode64 >> 5) & 0x1ull) != 0ull;
        pmode.amod = ((pmode64 >> 6) & 0x1ull) != 0ull;
        pmode.slbg = ((pmode64 >> 7) & 0x1ull) != 0ull;
        pmode.alp = static_cast<uint8_t>((pmode64 >> 8) & 0xFFu);
        return pmode;
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode264)
    {
        GSSmode2State smode2{};
        smode2.interlaced = (smode264 & 0x1ull) != 0ull;
        smode2.frameMode = ((smode264 >> 1) & 0x1ull) != 0ull;
        return smode2;
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
        {
            return;
        }

        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
            {
                sourceY = height - 1u;
            }

            const uint8_t *srcRow = source.data() + (sourceY * kHostFrameWidth * 4u);
            uint8_t *dstRow = pixels.data() + (y * kHostFrameWidth * 4u);
            std::memcpy(dstRow, srcRow, width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        if (pixels.empty() || width == 0u || height == 0u)
        {
            return;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x * 4u + 3u] = 255u;
            }
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t r = row[x * 4u + 0u];
                const uint8_t g = row[x * 4u + 1u];
                const uint8_t b = row[x * 4u + 2u];
                if (r != 0u || g != 0u || b != 0u)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    // [redbox] -- Stage 5.11 run 17. [texred] proved the red survives the
    // rasterizer: 471k red-texel pixels, killate=0, killz=0, storedblack=0,
    // and the framebuffer UNDER them already read dstrgb=0x200e64 (R=0x64,
    // G=0x0e, B=0x20 -- dark red). So the colour reaches VRAM. The remaining
    // question is purely downstream: does it survive to the presented image?
    //
    // Census a rectangle of an already-linearised RGBA buffer. Box defaults to
    // the [texred] bbox (23,301)..(484,419), clipped to the buffer.
    struct RedBoxStat
    {
        uint32_t px = 0u;      // pixels examined
        uint32_t nonblack = 0u;
        uint32_t red = 0u;     // clearly red: R high, and well above G and B
        uint32_t maxr = 0u;
        uint32_t centre = 0u;  // packed RGB at the box centre
    };

    RedBoxStat censusRedBox(const std::vector<uint8_t> &pixels,
                            uint32_t width, uint32_t height,
                            uint32_t bx0, uint32_t by0, uint32_t bx1, uint32_t by1)
    {
        RedBoxStat s;
        if (pixels.empty() || width == 0u || height == 0u)
        {
            return s;
        }
        if (bx1 >= width)  bx1 = width - 1u;
        if (by1 >= height) by1 = height - 1u;
        if (bx0 > bx1 || by0 > by1)
        {
            return s;
        }

        const uint32_t cx = (bx0 + bx1) / 2u;
        const uint32_t cy = (by0 + by1) / 2u;

        for (uint32_t y = by0; y <= by1; ++y)
        {
            const uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = bx0; x <= bx1; ++x)
            {
                const uint32_t r = row[x * 4u + 0u];
                const uint32_t g = row[x * 4u + 1u];
                const uint32_t b = row[x * 4u + 2u];

                ++s.px;
                if (r != 0u || g != 0u || b != 0u)
                {
                    ++s.nonblack;
                }
                // Deliberately STRICTER than the [texred] trigger, which was
                // loose enough (R>=0x20, G/B<0x20) to admit neutral dark greys
                // like 0x1f1f21. Nothing on a grey ramp can satisfy this.
                if (r >= 0x30u && r >= g + 0x18u && r >= b + 0x18u)
                {
                    ++s.red;
                }
                if (r > s.maxr)
                {
                    s.maxr = r;
                }
                if (x == cx && y == cy)
                {
                    s.centre = r | (g << 8) | (b << 16);
                }
            }
        }
        return s;
    }

    bool clearFramebufferRect(GS* gs, const GSContext &ctx, uint32_t rgba)
    {
        if (ctx.frame.fbw == 0u)
        {
            return false;
        }

        const uint32_t stride = GSInternal::fbStride(ctx.frame.fbw, ctx.frame.psm);
        if (stride == 0u)
        {
            return false;
        }

        const u32 x0 = static_cast<u32>(std::max<int>(0, ctx.scissor.x0));
        const u32 x1 = static_cast<u32>(std::max<int>(x0, ctx.scissor.x1));
        const u32 y0 = static_cast<u32>(std::max<int>(0, ctx.scissor.y0));
        const u32 y1 = static_cast<u32>(std::max<int>(y0, ctx.scissor.y1));

        uint8_t r = static_cast<uint8_t>(rgba & 0xFFu);
        uint8_t g = static_cast<uint8_t>((rgba >> 8) & 0xFFu);
        uint8_t b = static_cast<uint8_t>((rgba >> 16) & 0xFFu);
        uint8_t a = static_cast<uint8_t>((rgba >> 24) & 0xFFu);

        u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
        u32 fpsm = ctx.frame.psm;

        if (ctx.fba.fba && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
        {
            const uint32_t srcPixel =
                static_cast<uint32_t>(r) |
                (static_cast<uint32_t>(g) << 8) |
                (static_cast<uint32_t>(b) << 16) |
                (static_cast<uint32_t>(a) << 24);

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    uint32_t pixel = srcPixel;
                    if (ctx.frame.fbmsk != 0u)
                    {
                        const u32 c = gs->ReadVram(fpsm, fbp, fbw, x, y);
                        pixel = (pixel & ~ctx.frame.fbmsk) | (c & ctx.frame.fbmsk);
                    }
                    gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
                }
            }
            return true;
        }

        if (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S)
        {
            const uint16_t srcPixel = encodeFramePixelPSMCT16(r, g, b, a);
            const uint16_t mask = static_cast<uint16_t>(ctx.frame.fbmsk & 0xFFFFu);
            const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
            const uint32_t basePtr = GSInternal::framePageBaseToBlock(ctx.frame.fbp);

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    uint16_t pixel = srcPixel;
                    if (mask != 0u)
                    {
                        const u16 c = gs->ReadVram(fpsm, fbp, fbw, x, y);
                        pixel = static_cast<uint16_t>((pixel & ~mask) | (c & mask));
                    }
                    gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
                }
            }
            return true;
        }

        return false;
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};

    // Mirrors the set of register addresses GS::writeRegister actually
    // handles (i.e. every non-default case label there). Used only to tag
    // [gs:ad] diagnostic lines whose address doesn't match anything known.
    bool isKnownGsRegister(uint8_t regAddr)
    {
        switch (regAddr)
        {
        case GS_REG_PRIM:
        case GS_REG_RGBAQ:
        case GS_REG_ST:
        case GS_REG_UV:
        case GS_REG_XYZF2:
        case GS_REG_XYZF3:
        case GS_REG_XYZ2:
        case GS_REG_XYZ3:
        case GS_REG_TEX0_1:
        case GS_REG_TEX0_2:
        case GS_REG_CLAMP_1:
        case GS_REG_CLAMP_2:
        case GS_REG_FOG:
        case GS_REG_TEX1_1:
        case GS_REG_TEX1_2:
        case GS_REG_TEX2_1:
        case GS_REG_TEX2_2:
        case GS_REG_XYOFFSET_1:
        case GS_REG_XYOFFSET_2:
        case GS_REG_PRMODECONT:
        case GS_REG_PRMODE:
        case GS_REG_TEXCLUT:
        case GS_REG_SCISSOR_1:
        case GS_REG_SCISSOR_2:
        case GS_REG_ALPHA_1:
        case GS_REG_ALPHA_2:
        case GS_REG_TEST_1:
        case GS_REG_TEST_2:
        case GS_REG_FRAME_1:
        case GS_REG_FRAME_2:
        case GS_REG_ZBUF_1:
        case GS_REG_ZBUF_2:
        case GS_REG_FBA_1:
        case GS_REG_FBA_2:
        case GS_REG_BITBLTBUF:
        case GS_REG_TRXPOS:
        case GS_REG_TRXREG:
        case GS_REG_TRXDIR:
        case GS_REG_HWREG:
        case GS_REG_PABE:
        case GS_REG_TEXFLUSH:
        case GS_REG_SCANMSK:
        case GS_REG_FOGCOL:
        case GS_REG_DIMX:
        case GS_REG_DTHE:
        case GS_REG_COLCLAMP:
        case GS_REG_MIPTBP1_1:
        case GS_REG_MIPTBP1_2:
        case GS_REG_MIPTBP2_1:
        case GS_REG_MIPTBP2_2:
        case GS_REG_TEXA:
        case GS_REG_SIGNAL:
        case GS_REG_FINISH:
        case GS_REG_LABEL:
        case 0x59:
        case 0x5a:
        case 0x5b:
        case 0x5c:
        case 0x5f:
            return true;
        default:
            return false;
        }
    }

    // -----------------------------------------------------------------------
    // Live-side counterpart to `gsdump_draws.py --emit-jsonl`. Stage 5.11 needs
    // to diff the live EE path's actual TEX0/CLUT/draw state against an
    // authentic PCSX2 dump; the debug-history ring (GSDebugHistoryEntry) already
    // records everything needed, it's just never been written out in a format
    // the Python side can diff against.
    //
    // Opt-in, same convention as PS2X_GSDUMP above: nothing happens unless both
    // PS2X_GSHISTORY_DUMP (output path) and PS2X_GSHISTORY_FRAMES (how many
    // vsyncs to capture before flushing once and going quiet) are set. Entirely
    // self-contained in this .cpp -- no header touched.
    //
    // Frame counting deliberately does NOT use GS::m_debugFrameIndex: that
    // counter resets to 0 on every clearDebugHistory() call, and this drains
    // the 512-entry ring after every GIF transfer to avoid overflow (same
    // reason ps2_gsdump_replay_tests.cpp drains per-transfer). Instead this
    // tracks vsync-tick transitions itself across drains, so "frame" stays
    // monotonic for the whole capture window regardless of how often the ring
    // is drained.
    void gsHistoryDrainAndMaybeFlush(GS &gs)
    {
        static bool initialized = false;
        static bool active = false;
        static bool done = false;
        static uint32_t targetFrames = 0;
        static std::string outPath;
        static std::vector<GSDebugHistoryEntry> accum;
        static uint64_t lastTick = UINT64_MAX;
        static uint32_t framesSeen = 0;

        if (!initialized)
        {
            initialized = true;
            const char *path = std::getenv("PS2X_GSHISTORY_DUMP");
            const char *framesEnv = std::getenv("PS2X_GSHISTORY_FRAMES");
            if (path && *path && framesEnv && *framesEnv)
            {
                targetFrames = static_cast<uint32_t>(std::strtoul(framesEnv, nullptr, 10));
                if (targetFrames > 0)
                {
                    outPath = path;
                    active = true;
                    gs.setDebugHistoryPaused(false);
                }
            }
        }

        if (!active || done)
        {
            return;
        }

        for (GSDebugHistoryEntry e : gs.getDebugHistory())
        {
            if (e.kind != GSDebugEventKind::Draw)
            {
                continue;
            }
            if (lastTick == UINT64_MAX)
            {
                lastTick = e.vsyncTick;
            }
            else if (e.vsyncTick != lastTick)
            {
                ++framesSeen;
                lastTick = e.vsyncTick;
            }
            e.frameIndex = framesSeen; // stamp OUR relative frame count, not GS's
            accum.push_back(e);
        }
        gs.clearDebugHistory();

        if (framesSeen < targetFrames)
        {
            return;
        }

        std::FILE *f = std::fopen(outPath.c_str(), "wb");
        if (f)
        {
            for (size_t i = 0; i < accum.size(); ++i)
            {
                const GSDebugHistoryEntry &e = accum[i];
                std::fprintf(f,
                    "{\"probe\":\"GSDRAW\",\"seq\":\"0x%zX\",\"frame\":\"0x%X\","
                    "\"prim\":\"0x%X\",\"tme\":\"0x%X\",\"abe\":\"0x%X\","
                    "\"tbp0\":\"0x%X\",\"tbw\":\"0x%X\",\"psm\":\"0x%X\","
                    "\"tw\":\"0x%X\",\"th\":\"0x%X\",\"cbp\":\"0x%X\",\"cpsm\":\"0x%X\","
                    "\"csm\":\"0x%X\",\"csa\":\"0x%X\",\"cld\":\"0x%X\","
                    "\"test\":\"0x%llX\",\"alpha\":\"0x%llX\","
                    "\"x0\":\"%.1f\",\"y0\":\"%.1f\",\"x1\":\"%.1f\",\"y1\":\"%.1f\"}\n",
                    i, static_cast<unsigned>(e.frameIndex),
                    static_cast<unsigned>(e.prim.prim), e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u,
                    static_cast<unsigned>(e.tex0.tbp0), static_cast<unsigned>(e.tex0.tbw),
                    static_cast<unsigned>(e.tex0.psm),
                    1u << static_cast<unsigned>(e.tex0.tw), 1u << static_cast<unsigned>(e.tex0.th),
                    static_cast<unsigned>(e.tex0.cbp), static_cast<unsigned>(e.tex0.cpsm),
                    static_cast<unsigned>(e.tex0.csm), static_cast<unsigned>(e.tex0.csa),
                    static_cast<unsigned>(e.tex0.cld),
                    static_cast<unsigned long long>(e.test), static_cast<unsigned long long>(e.alpha),
                    static_cast<double>(e.xMin), static_cast<double>(e.yMin),
                    static_cast<double>(e.xMax), static_cast<double>(e.yMax));
            }
            std::fclose(f);
        }

        gs.setDebugHistoryPaused(true);
        done = true;
        accum.clear();
    }
}

using namespace GSInternal;

GS::GS()
{
    using namespace GSMem;

    InitLookupTables();

    for (usz i = 0; i < 0x3F; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_pixel_address_funcs[i] = LookupPixelAddressCT32;
            m_read_address_funcs[i]  = ReadPixelAddressCT32;
            m_write_address_funcs[i] = WritePixelAddressCT32;
            m_read_pixel_funcs[i]    = ReadPixelCT32;
            m_write_pixel_funcs[i]   = WritePixelCT32;
            break;
        case GS_PSM_CT24:
            m_pixel_address_funcs[i] = LookupPixelAddressCT32;
            m_read_address_funcs[i]  = ReadPixelAddressCT24;
            m_write_address_funcs[i] = WritePixelAddressCT24;
            m_read_pixel_funcs[i]    = ReadPixelCT24;
            m_write_pixel_funcs[i]   = WritePixelCT24;
            break;
        case GS_PSM_CT16:
            m_pixel_address_funcs[i] = LookupPixelAddressCT16;
            m_read_address_funcs[i]  = ReadPixelAddressCT16;
            m_write_address_funcs[i] = WritePixelAddressCT16;
            m_read_pixel_funcs[i]    = ReadPixelCT16;
            m_write_pixel_funcs[i]   = WritePixelCT16;
            break;
        case GS_PSM_CT16S:
            m_pixel_address_funcs[i] = LookupPixelAddressCT16S;
            m_read_address_funcs[i]  = ReadPixelAddressCT16S;
            m_write_address_funcs[i] = WritePixelAddressCT16S;
            m_read_pixel_funcs[i]    = ReadPixelCT16S;
            m_write_pixel_funcs[i]   = WritePixelCT16S;
            break;
        case GS_PSM_T8:
            m_pixel_address_funcs[i] = LookupPixelAddressP8;
            m_read_address_funcs[i]  = ReadPixelAddressP8;
            m_write_address_funcs[i] = WritePixelAddressP8;
            m_read_pixel_funcs[i]     = ReadPixelP8;
            m_write_pixel_funcs[i]    = WritePixelP8;
            break;
        case GS_PSM_T8H:
            m_pixel_address_funcs[i] = LookupPixelAddressCT32;
            m_read_address_funcs[i]  = ReadPixelAddressP8H;
            m_write_address_funcs[i] = WritePixelAddressP8H;
            m_read_pixel_funcs[i]    = ReadPixelP8H;
            m_write_pixel_funcs[i]   = WritePixelP8H;
            break;
        case GS_PSM_T4:
            m_pixel_address_funcs[i] = LookupPixelAddressP4;
            m_read_address_funcs[i]  = ReadPixelAddressP4;
            m_write_address_funcs[i] = WritePixelAddressP4;
            m_read_pixel_funcs[i]    = ReadPixelP4;
            m_write_pixel_funcs[i]   = WritePixelP4;
            break;
        case GS_PSM_T4HH:
            m_pixel_address_funcs[i] = LookupPixelAddressCT32;
            m_read_address_funcs[i]  = ReadPixelAddressP4HH;
            m_write_address_funcs[i] = WritePixelAddressP4HH;
            m_read_pixel_funcs[i]    = ReadPixelP4HH;
            m_write_pixel_funcs[i]   = WritePixelP4HH;
            break;
        case GS_PSM_T4HL:
            m_pixel_address_funcs[i] = LookupPixelAddressCT32;
            m_read_address_funcs[i]  = ReadPixelAddressP4HL;
            m_write_address_funcs[i] = WritePixelAddressP4HL;
            m_read_pixel_funcs[i]    = ReadPixelP4HL;
            m_write_pixel_funcs[i]   = WritePixelP4HL;
            break;
        case GS_PSM_Z32:
            m_pixel_address_funcs[i] = LookupPixelAddressZ32;
            m_read_address_funcs[i]  = ReadPixelAddressZ32;
            m_write_address_funcs[i] = WritePixelAddressZ32;
            m_read_pixel_funcs[i]    = ReadPixelZ32;
            m_write_pixel_funcs[i]   = WritePixelZ32;
            break;
        case GS_PSM_Z24:
            m_pixel_address_funcs[i] = LookupPixelAddressZ32;
            m_read_address_funcs[i]  = ReadPixelAddressZ24;
            m_write_address_funcs[i] = WritePixelAddressZ24;
            m_read_pixel_funcs[i]    = ReadPixelZ24;
            m_write_pixel_funcs[i]   = WritePixelZ24;
            break;
        case GS_PSM_Z16:
            m_pixel_address_funcs[i] = LookupPixelAddressZ16;
            m_read_address_funcs[i]  = ReadPixelAddressZ16;
            m_write_address_funcs[i] = WritePixelAddressZ16;
            m_read_pixel_funcs[i]    = ReadPixelZ16;
            m_write_pixel_funcs[i]   = WritePixelZ16;
            break;
        case GS_PSM_Z16S:
            m_pixel_address_funcs[i] = LookupPixelAddressZ16S;
            m_read_address_funcs[i]  = ReadPixelAddressZ16S;
            m_write_address_funcs[i] = WritePixelAddressZ16S;
            m_read_pixel_funcs[i]    = ReadPixelZ16S;
            m_write_pixel_funcs[i]   = WritePixelZ16S;
            break;
        default:
            m_pixel_address_funcs[i] = LookupPixelAddressNull;
            m_read_address_funcs[i]  = ReadPixelAddressNull;
            m_write_address_funcs[i] = WritePixelAddressNull;
            m_read_pixel_funcs[i]    = ReadPixelNull;
            m_write_pixel_funcs[i]   = WritePixelNull;
            break;
        }
    }

    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs, PS2Runtime *runtime)
{
    m_vram = vram;
    m_vramSize = vramSize;
    m_privRegs = privRegs;
    m_runtime = runtime;
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_pendingImageBytes = 0;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    m_hostPresentationFrame.clear();
    m_hostPresentationWidth = 0u;
    m_hostPresentationHeight = 0u;
    m_hostPresentationDisplayFbp = 0u;
    m_hostPresentationSourceFbp = 0u;
    m_hostPresentationUsedPreferred = false;
    m_hasHostPresentationFrame = false;
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

GSContext &GS::activeContext()
{
    return m_registers.ctx[m_registers.prim.ctxt];
}

void GS::snapshotVRAM()
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (!m_vram || m_vramSize == 0)
        return;
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.resize(m_vramSize);
    std::memcpy(m_displaySnapshot.data(), m_vram, m_vramSize);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

GSDebugSnapshot GS::getDebugSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    GSDebugSnapshot snapshot{};
    snapshot.ctx[0] = m_registers.ctx[0];
    snapshot.ctx[1] = m_registers.ctx[1];
    snapshot.prim = m_registers.prim;
    snapshot.texa = m_registers.texa;
    snapshot.texclut = m_registers.texclut;
    snapshot.bitbltbuf = m_registers.bitbltbuf;
    snapshot.trxpos = m_registers.trxpos;
    snapshot.trxreg = m_registers.trxreg;
    snapshot.trxdir = m_registers.trxdir.data;
    snapshot.transferX = m_transferState.x;
    snapshot.transferY = m_transferState.y;
    snapshot.transferTotalPixels = m_transferState.total_pixels;
    snapshot.transferCopiedPixels = m_transferState.copied_pixels;
    snapshot.pendingImageBytes = m_pendingImageBytes;
    snapshot.lastDisplayBaseBytes = m_lastDisplayBaseBytes;
    snapshot.preferredDisplaySourceFrame = m_preferredDisplaySourceFrame;
    snapshot.preferredDisplayDestFbp = m_preferredDisplayDestFbp;
    snapshot.hasPreferredDisplaySource = m_hasPreferredDisplaySource;
    snapshot.hostPresentationWidth = m_hostPresentationWidth;
    snapshot.hostPresentationHeight = m_hostPresentationHeight;
    snapshot.hostPresentationDisplayFbp = m_hostPresentationDisplayFbp;
    snapshot.hostPresentationSourceFbp = m_hostPresentationSourceFbp;
    snapshot.hostPresentationUsedPreferred = m_hostPresentationUsedPreferred;
    snapshot.hasHostPresentationFrame = m_hasHostPresentationFrame;
    snapshot.localToHostPendingBytes = (m_localToHostReadPos < m_localToHostBuffer.size())
                                           ? (m_localToHostBuffer.size() - m_localToHostReadPos)
                                           : 0u;
    return snapshot;
}


std::vector<GSDebugHistoryEntry> GS::getDebugHistory() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    std::vector<GSDebugHistoryEntry> out;
    out.reserve(m_debugHistoryCount);
    const size_t first = (m_debugHistoryWrite + kDebugHistoryCapacity - m_debugHistoryCount) % kDebugHistoryCapacity;
    for (size_t i = 0; i < m_debugHistoryCount; ++i)
    {
        out.push_back(m_debugHistory[(first + i) % kDebugHistoryCapacity]);
    }
    return out;
}

void GS::clearDebugHistory()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryWrite = 0;
    m_debugHistoryCount = 0;
    m_debugNextSeq = 1;
    m_debugFrameIndex = 0;
    m_debugLastVsyncTick = UINT64_MAX;
}

bool GS::isDebugHistoryPaused() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_debugHistoryPaused;
}

void GS::setDebugHistoryPaused(bool paused)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_debugHistoryPaused = paused;
}

GSDebugHistoryEntry GS::makeDebugEventUnlocked(GSDebugEventKind kind) const
{
    GSDebugHistoryEntry entry{};
    entry.kind = kind;
    entry.prim = m_registers.prim;
    const uint32_t ci = m_registers.prim.ctxt ? 1u : 0u;
    entry.frame = m_registers.ctx[ci].frame;
    entry.zbuf = m_registers.ctx[ci].zbuf;
    entry.tex0 = m_registers.ctx[ci].tex0;
    entry.scissor = m_registers.ctx[ci].scissor;
    entry.test = m_registers.ctx[ci].test.data;
    entry.alpha = m_registers.ctx[ci].alpha.data;
    entry.bitbltbuf = m_registers.bitbltbuf;
    entry.trxpos = m_registers.trxpos;
    entry.trxreg = m_registers.trxreg;
    entry.trxdir = m_registers.trxdir.data;
    entry.transferPixels = m_transferState.total_pixels;
    return entry;
}

void GS::recordDebugEventUnlocked(GSDebugHistoryEntry entry)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    const uint64_t tick = m_runtime ? ps2_syscalls::GetCurrentVSyncTick(m_runtime) : 0ull;
    if (m_debugLastVsyncTick == UINT64_MAX)
    {
        m_debugLastVsyncTick = tick;
    }
    else if (tick != m_debugLastVsyncTick)
    {
        ++m_debugFrameIndex;
        m_debugLastVsyncTick = tick;
    }

    entry.seq = m_debugNextSeq++;
    entry.vsyncTick = tick;
    entry.frameIndex = m_debugFrameIndex;

    m_debugHistory[m_debugHistoryWrite] = entry;
    m_debugHistoryWrite = (m_debugHistoryWrite + 1u) % kDebugHistoryCapacity;
    if (m_debugHistoryCount < kDebugHistoryCapacity)
    {
        ++m_debugHistoryCount;
    }
}

void GS::recordGifTagDebugEventUnlocked(uint32_t sizeBytes, uint32_t nloop, uint8_t flg, uint32_t nreg)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::GifTag);
    entry.gifSizeBytes = sizeBytes;
    entry.gifNloop = nloop;
    entry.gifFlg = flg;
    entry.gifNreg = static_cast<uint8_t>(std::min<uint32_t>(nreg, 16u));
    recordDebugEventUnlocked(entry);
}

void GS::recordRegisterDebugEventUnlocked(uint8_t regAddr, uint64_t value)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    switch (regAddr)
    {
    case GS_REG_PRIM:
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    case GS_REG_TEXA:
    case GS_REG_TEXCLUT:
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    case GS_REG_BITBLTBUF:
    case GS_REG_TRXPOS:
    case GS_REG_TRXREG:
    case GS_REG_TRXDIR:
        break;
    default:
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Register);
    entry.reg = regAddr;
    entry.regValue = value;
    recordDebugEventUnlocked(entry);
}

void GS::recordDrawDebugEventUnlocked(int vertexCount)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    if (vertexCount <= 0)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Draw);
    entry.vertexCount = static_cast<uint32_t>(vertexCount);

    const int count = std::min(vertexCount, kMaxVerts);
    entry.xMin = entry.xMax = m_vtxQueue[0].x;
    entry.yMin = entry.yMax = m_vtxQueue[0].y;
    entry.zMin = entry.zMax = m_vtxQueue[0].z;
    entry.aMin = entry.aMax = m_vtxQueue[0].a;

    for (int i = 1; i < count; ++i)
    {
        const GSVertex &v = m_vtxQueue[i];
        entry.xMin = std::min(entry.xMin, v.x);
        entry.xMax = std::max(entry.xMax, v.x);
        entry.yMin = std::min(entry.yMin, v.y);
        entry.yMax = std::max(entry.yMax, v.y);
        entry.zMin = std::min(entry.zMin, v.z);
        entry.zMax = std::max(entry.zMax, v.z);
        entry.aMin = std::min(entry.aMin, v.a);
        entry.aMax = std::max(entry.aMax, v.a);
    }

    recordDebugEventUnlocked(entry);
}

void GS::recordTransferDebugEventUnlocked()
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Transfer);
    entry.transferPixels = m_transferState.total_pixels;
    recordDebugEventUnlocked(entry);
}

void GS::recordPresentDebugEventUnlocked(uint32_t displayFbp, uint32_t sourceFbp, uint32_t width, uint32_t height, bool usedPreferred)
{
    if (m_debugHistoryPaused)
    {
        return;
    }

    GSDebugHistoryEntry entry = makeDebugEventUnlocked(GSDebugEventKind::Present);
    entry.displayFbp = displayFbp;
    entry.sourceFbp = sourceFbp;
    entry.width = width;
    entry.height = height;
    entry.usedPreferred = usedPreferred;
    recordDebugEventUnlocked(entry);
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

bool GS::copyFrameToHostRgbaUnlocked(const GSFrameReg &frame,
                                     uint32_t width,
                                     uint32_t height,
                                     std::vector<uint8_t> &outPixels,
                                     bool preserveAlpha,
                                     bool useLocalMemoryLayout,
                                     bool frameBaseIsPages,
                                     uint32_t sourceOriginX,
                                     uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
    {
        return false;
    }

    outPixels.resize(kHostFrameWidth * kHostFrameHeight * 4u);
    auto failCopy = [&outPixels]() -> bool
    {
        outPixels.clear();
        return false;
    };

    const uint32_t baseBytes = frameBaseIsPages ? (frame.fbp * 8192u) : (frame.fbp * 256u);
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbwBlocks = frame.fbw ? frame.fbw : (kHostFrameWidth / 64u);
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t strideBytes = fbwBlocks * 64u * bytesPerPixel;

    if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
    {
        const uint32_t srcPixelBytes = (frame.psm == GS_PSM_CT24) ? 3u : 4u;
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = outPixels.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;

                    const u32 c = ReadVram(frame.psm, basePtr, fbwBlocks, srcX, srcY);

                    const u32 r = c & 0xFF;
                    const u32 g = (c >> 8) & 0xFF;
                    const u32 b = (c >> 16) & 0xFF;

                    u32 a = 0xFF;
                    if (preserveAlpha && frame.psm != GS_PSM_CT24)
                    {
                        a = (c >> 24) & 0xFF;
                    }

                    dstRow[x * 4u + 0u] = r;
                    dstRow[x * 4u + 1u] = g;
                    dstRow[x * 4u + 2u] = b;
                    dstRow[x * 4u + 3u] = a;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dstRow = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * srcPixelBytes);
                if (srcOff + srcPixelBytes > m_vramSize)
                {
                    return failCopy();
                }

                dstRow[x * 4u + 0u] = m_vram[srcOff + 0u];
                dstRow[x * 4u + 1u] = m_vram[srcOff + 1u];
                dstRow[x * 4u + 2u] = m_vram[srcOff + 2u];
                dstRow[x * 4u + 3u] =
                    (preserveAlpha && frame.psm != GS_PSM_CT24) ? m_vram[srcOff + 3u] : 255u;
            }
        }
        return true;
    }

    if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
    {
        if (useLocalMemoryLayout)
        {
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint32_t dstOff = y * kHostFrameWidth * 4u;
                uint8_t *dst = outPixels.data() + dstOff;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    const uint32_t srcY = sourceOriginY + y;

                    const u16 c = ReadVram(frame.psm, basePtr, fbwBlocks, srcX, srcY);

                    const uint32_t r = c & 31u;
                    const uint32_t g = (c >> 5) & 31u;
                    const uint32_t b = (c >> 10) & 31u;
                    dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                    dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                    dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                    dst[x * 4u + 3u] = preserveAlpha ? ((c & 0x8000u) ? 0x80u : 0x00u) : 255u;
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dst = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * 2u);
                if (srcOff + sizeof(uint16_t) > m_vramSize)
                {
                    return failCopy();
                }

                uint16_t pixel = 0u;
                std::memcpy(&pixel, m_vram + srcOff, sizeof(pixel));
                const uint32_t r = pixel & 31u;
                const uint32_t g = (pixel >> 5) & 31u;
                const uint32_t b = (pixel >> 10) & 31u;
                dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                dst[x * 4u + 3u] = preserveAlpha ? ((pixel & 0x8000u) ? 0x80u : 0x00u) : 255u;
            }
        }
        return true;
    }

    return failCopy();
}

void GS::latchHostPresentationFrame()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    latchHostPresentationFrameUnlocked();
}

void GS::latchHostPresentationFrameUnlocked()
{
    // [present] probe (PS2X_DIAG=1). Stage 5.7: the rasterizer is demonstrably
    // busy (GSRasterizer::writePixel dominates the EE thread) yet the screen is
    // black, so the open question is whether DISPFB/DISPLAY point at the pages
    // the draws land in. Emitted from a scope guard so every early-return path
    // is covered by one insertion, and throttled to ~1 line/sec at 60Hz because
    // this runs per latch on the presentation thread.
    struct PresentProbe
    {
        GS *self;
        bool armed;
        ~PresentProbe()
        {
            // [frameord] per-latch sample. Deliberately OUTSIDE the `armed`
            // throttle: the question is how many INDIVIDUAL frames end with a
            // full-screen black fill, which a 1/sec sample cannot answer.
            //
            // trail = stores since the last coloured store. g_writeSeq and
            // g_lastNonBlackSeq are only reset on the throttled interval
            // below, so the difference is epoch-independent -- except on the
            // single latch immediately after a reset, where lastNb can still
            // read 0 and inflate trail. That is 1 latch in ~60; it cannot
            // manufacture a "wiped on every frame" result.
            if (ps2_diag::enabled())
            {
                using namespace ps2diag_fbstat;
                const uint64_t seq = g_writeSeq.load(std::memory_order_relaxed);
                const uint64_t lastNb = g_lastNonBlackSeq.load(std::memory_order_relaxed);
                const uint64_t trail = (seq > lastNb) ? (seq - lastNb) : 0ull;

                g_foFrames.fetch_add(1, std::memory_order_relaxed);
                g_foLastTrail.store(trail, std::memory_order_relaxed);

                uint64_t prevMax = g_foMaxTrail.load(std::memory_order_relaxed);
                if (trail > prevMax)
                    g_foMaxTrail.store(trail, std::memory_order_relaxed);

                // Half of 512x448 = 114688. A trailing black run that big can
                // only be a full-screen fill, not scattered sprite pixels.
                if (trail >= 114688ull)
                {
                    g_foWiped.fetch_add(1, std::memory_order_relaxed);
                    g_foWipeX0.store(g_tailX0.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    g_foWipeY0.store(g_tailY0.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    g_foWipeX1.store(g_tailX1.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    g_foWipeY1.store(g_tailY1.load(std::memory_order_relaxed), std::memory_order_relaxed);
                }
            }

            if (!armed)
            {
                return;
            }

            uint32_t nonBlack = 0u;
            if (self->m_hasHostPresentationFrame && self->m_hostPresentationWidth != 0u)
            {
                nonBlack = countNonBlackPixels(self->m_hostPresentationFrame,
                                               self->m_hostPresentationWidth,
                                               self->m_hostPresentationHeight);
            }

            const uint64_t pmodeRaw = self->m_privRegs ? self->m_privRegs->pmode : 0ull;
            const uint64_t dispfb1Raw = self->m_privRegs ? self->m_privRegs->dispfb1 : 0ull;
            const uint64_t dispfb2Raw = self->m_privRegs ? self->m_privRegs->dispfb2 : 0ull;
            const uint64_t display1Raw = self->m_privRegs ? self->m_privRegs->display1 : 0ull;
            const uint64_t display2Raw = self->m_privRegs ? self->m_privRegs->display2 : 0ull;

            RUNTIME_LOG("[present] has=" << (self->m_hasHostPresentationFrame ? 1 : 0)
                                         << " w=" << std::dec << self->m_hostPresentationWidth
                                         << " h=" << self->m_hostPresentationHeight
                                         << " nonblack=" << nonBlack
                                         << " dispFbp=0x" << std::hex << self->m_hostPresentationDisplayFbp
                                         << " srcFbp=0x" << self->m_hostPresentationSourceFbp
                                         << " pref=" << std::dec << (self->m_hostPresentationUsedPreferred ? 1 : 0)
                                         << " ctx0.fbp=0x" << std::hex << self->m_registers.ctx[0].frame.fbp
                                         << " ctx0.fbw=" << std::dec << self->m_registers.ctx[0].frame.fbw
                                         << " ctx0.psm=0x" << std::hex << static_cast<uint32_t>(self->m_registers.ctx[0].frame.psm)
                                         << " ctx1.fbp=0x" << self->m_registers.ctx[1].frame.fbp
                                         << " ctx1.fbw=" << std::dec << self->m_registers.ctx[1].frame.fbw
                                         << " ctx1.psm=0x" << std::hex << static_cast<uint32_t>(self->m_registers.ctx[1].frame.psm)
                                         << " pmode=0x" << pmodeRaw
                                         << " dispfb1=0x" << dispfb1Raw
                                         << " dispfb2=0x" << dispfb2Raw
                                         << " display1=0x" << display1Raw
                                         << " display2=0x" << display2Raw
                                         << std::dec);

            // [redbox] on the PRESENTED frame -- the image the user actually
            // sees. Paired below with the same census run against each raw
            // candidate fbp, so the two can be compared directly.
            //
            // PRE-REGISTERED READING (compare src=present against src=fbp*):
            //   present.red > 0
            //       -> the red DOES reach the screen. The box is not black and
            //          Stage 5.11's premise is wrong; re-check what is actually
            //          on screen before touching any more code.
            //   present.red == 0 but some fbp.red > 0
            //       -> the colour is in VRAM and lost in presentation. Suspect
            //          copyDisplaySource / copyFrameToHostRgbaUnlocked, or we
            //          present the wrong buffer. `dispFbp` vs the fbp that has
            //          the red names which.
            //   every red == 0 while [texred] stored stays large
            //       -> the red is written and then OVERDRAWN before latch.
            //          Next instrument is an overdraw counter on the bbox, not
            //          another read-back.
            //   present.nonblack > 0 but present.maxr low everywhere
            //       -> the box is being drawn dark, not erased; go back to the
            //          blender (alpha=0x44, srca=0x78 from [texred]).
            //
            // GUARD (rival reading): `px` is the pixel count actually examined.
            // If px == 0 the box fell outside the buffer and every other field
            // on that line is meaningless -- read px FIRST on every line.
            if (self->m_hasHostPresentationFrame && self->m_hostPresentationWidth != 0u)
            {
                const RedBoxStat rb = censusRedBox(self->m_hostPresentationFrame,
                                                   self->m_hostPresentationWidth,
                                                   self->m_hostPresentationHeight,
                                                   23u, 301u, 484u, 419u);
                RUNTIME_LOG("[redbox] src=present"
                            << " w=" << std::dec << self->m_hostPresentationWidth
                            << " h=" << self->m_hostPresentationHeight
                            << " px=" << rb.px
                            << " nonblack=" << rb.nonblack
                            << " red=" << rb.red
                            << " maxr=0x" << std::hex << rb.maxr
                            << " centre=0x" << rb.centre << std::dec);
            }

            // [fbscan]: the 08-03d flashing-bar report says content DOES reach
            // the screen, but only on alternate frames. So measure both halves
            // of the double buffer with byte-identical decode settings, varying
            // only fbp. Reading:
            //   exactly one fbp ever non-black -> the game draws into one buffer
            //       while we present the other (swap/flip bug)
            //   both non-black but [present] nonblack=0 -> the loss is
            //       downstream of this scan, inside copyDisplaySource
            //   all zero while [gs:frame] nonblack is large -> the rasterizer is
            //       writing VRAM at an address this decode never reads
            if (self->m_privRegs && self->m_vram && self->m_vramSize != 0u)
            {
                const GSFrameReg scanDisp = decodeDisplayFrame(self->m_privRegs->dispfb1);
                uint32_t scanW = 0u;
                uint32_t scanH = 0u;
                decodeDisplaySize(self->m_privRegs->display1, scanW, scanH);

                if (scanW != 0u && scanH != 0u)
                {
                    const uint32_t candidates[4] = {scanDisp.fbp,
                                                    self->m_registers.ctx[0].frame.fbp,
                                                    self->m_registers.ctx[1].frame.fbp,
                                                    0u};
                    for (int i = 0; i < 4; ++i)
                    {
                        bool dup = false;
                        for (int j = 0; j < i; ++j)
                        {
                            dup = dup || (candidates[j] == candidates[i]);
                        }
                        if (dup)
                        {
                            continue;
                        }

                        GSFrameReg probeFrame = scanDisp;
                        probeFrame.fbp = candidates[i];

                        std::vector<uint8_t> probePixels;
                        uint32_t nb = 0u;
                        const bool copied = self->copyFrameToHostRgbaUnlocked(
                            probeFrame, scanW, scanH, probePixels, true, true, true, 0u, 0u);
                        if (copied)
                        {
                            nb = countNonBlackPixels(probePixels, scanW, scanH);
                        }

                        RUNTIME_LOG("[fbscan] fbp=0x" << std::hex << candidates[i]
                                                      << " psm=0x" << static_cast<uint32_t>(probeFrame.psm)
                                                      << std::dec
                                                      << " fbw=" << probeFrame.fbw
                                                      << " w=" << scanW
                                                      << " h=" << scanH
                                                      << " copied=" << (copied ? 1 : 0)
                                                      << " nonblack=" << nb);

                        // [redbox] on this raw fbp, same box, same decode as
                        // the presented-frame line above. See the reading table
                        // at that emit. Read px first.
                        if (copied)
                        {
                            const RedBoxStat rb = censusRedBox(probePixels, scanW, scanH,
                                                               23u, 301u, 484u, 419u);
                            RUNTIME_LOG("[redbox] src=fbp0x" << std::hex << candidates[i]
                                        << std::dec
                                        << " w=" << scanW
                                        << " h=" << scanH
                                        << " px=" << rb.px
                                        << " nonblack=" << rb.nonblack
                                        << " red=" << rb.red
                                        << " maxr=0x" << std::hex << rb.maxr
                                        << " centre=0x" << rb.centre << std::dec);
                        }

                        // [gsdump] -- same buffer [fbscan] just counted, written
                        // out so it can be looked at. Overwrites each interval,
                        // so the surviving file is the last state before exit.
                        if (copied && gsDumpDir())
                        {
                            const std::string p = gsDumpPath("fb", candidates[i], "tga");
                            const bool ok = writeTgaRgba(p, scanW, scanH, probePixels);
                            RUNTIME_LOG("[gsdump] " << p << " ok=" << (ok ? 1 : 0));
                        }
                    }
                }
            }

            // [gsdump] -- the texture and palette behind the 16.3M "textured but
            // decodes to black" pixels that [blackwho] counted. TBP/CBP come
            // from the rasterizer's last-black-store snapshot, so these are the
            // exact addresses the failing samples were reading.
            if (gsDumpDir() && self->m_vram && self->m_vramSize != 0u)
            {
                const uint32_t tbp = ps2diag_fbstat::g_snapTbp.load(std::memory_order_relaxed);
                const uint32_t cbp = ps2diag_fbstat::g_snapCbp.load(std::memory_order_relaxed);
                const uint32_t tpsm = ps2diag_fbstat::g_snapTexPsm.load(std::memory_order_relaxed);

                // 256 entries covers PSMT8 as well as the 16 a PSMT4 CLUT needs.
                const bool clutOk = writeVramWords(gsDumpPath("clut", cbp, "txt"),
                                                   self->m_vram, self->m_vramSize,
                                                   static_cast<size_t>(cbp) * 256u, 256u);
                // 64 KB from TBP is 128k PSMT4 texels -- more than any single
                // menu sprite, and cheap enough to dump unconditionally.
                const bool texOk = writeVramCensus(gsDumpPath("tex", tbp, "txt"),
                                                   self->m_vram, self->m_vramSize,
                                                   static_cast<size_t>(tbp) * 256u, 65536u);

                RUNTIME_LOG("[gsdump] tbp=0x" << std::hex << tbp
                                              << " cbp=0x" << cbp
                                              << " texpsm=0x" << tpsm << std::dec
                                              << " clut=" << (clutOk ? 1 : 0)
                                              << " tex=" << (texOk ? 1 : 0));
            }

            // [vramcensus]: [fbscan] came back all-zero at every candidate fbp
            // while [gs:frame] reported 3.8M non-black pixels rasterized, so
            // stop guessing addresses and ask VRAM directly where the colour
            // is. Walks raw m_vram as 32-bit words, bucketed by FBP page unit
            // (8192 bytes), and reports the busiest pages. Reading:
            //   busiest page == 0x0 / 0x70 -> the bytes ARE in the right pages
            //       and the swizzled read-back in copyFrameToHostRgbaUnlocked
            //       disagrees with the swizzled write in GS::WriteVram
            //   busiest page is some other value -> FRAME.FBP as the rasterizer
            //       sees it is not what DISPFB points at
            //   nothing non-black anywhere -> the writes never land in m_vram
            //       at all (masked out, or a different buffer)
            // fbmsk is printed alongside because the [gs:frame] counters sample
            // r/g/b BEFORE the FBMSK merge, so a mask of 0x00FFFFFF would make
            // "3.8M non-black pixels" and "black VRAM" both true at once.
            if (self->m_vram && self->m_vramSize >= 8192u)
            {
                constexpr uint32_t kPageBytes = 8192u;
                const uint32_t pageCount = static_cast<uint32_t>(self->m_vramSize / kPageBytes);
                std::vector<uint32_t> pageNonBlack(pageCount, 0u);
                uint64_t totalNonBlack = 0u;

                const uint32_t *words = reinterpret_cast<const uint32_t *>(self->m_vram);
                for (uint32_t page = 0; page < pageCount; ++page)
                {
                    const uint32_t *pageWords = words + (static_cast<size_t>(page) * (kPageBytes / 4u));
                    uint32_t hits = 0u;
                    for (uint32_t w = 0; w < kPageBytes / 4u; ++w)
                    {
                        if ((pageWords[w] & 0x00FFFFFFu) != 0u)
                        {
                            ++hits;
                        }
                    }
                    pageNonBlack[page] = hits;
                    totalNonBlack += hits;
                }

                RUNTIME_LOG("[vramcensus] pages=" << std::dec << pageCount
                                                  << " totalnonblack=" << totalNonBlack
                                                  << " ctx0.fbmsk=0x" << std::hex
                                                  << static_cast<uint32_t>(self->m_registers.ctx[0].frame.fbmsk)
                                                  << " ctx1.fbmsk=0x"
                                                  << static_cast<uint32_t>(self->m_registers.ctx[1].frame.fbmsk)
                                                  << std::dec);

                // [fbdest]: [zbuf] answered "is the colour misplaced or
                // destroyed" -- neither. Page 0x70 came back 2048/2048 raw
                // words non-zero with zero RGB, so writes land at the right
                // address carrying only alpha. These counters (defined in
                // ps2_gs_rasterizer.cpp) bucket the FINAL stored pixel value
                // by destination frame.fbp, which separates "colour went to
                // an fbp we never display" from "colour was drawn and then
                // buried by a later black sprite". lastnb/seq is the draw-
                // order position of the last coloured pixel: well below 1.0
                // means a black full-screen sprite ran after the artwork.
                {
                    using namespace ps2diag_fbstat;
                    const uint64_t seq = g_writeSeq.exchange(0, std::memory_order_relaxed);
                    const uint64_t lastNb = g_lastNonBlackSeq.exchange(0, std::memory_order_relaxed);

                    std::ostringstream fbdest;
                    for (uint32_t p = 0; p < 512u; ++p)
                    {
                        const uint32_t nb = g_fbNonBlack[p].exchange(0, std::memory_order_relaxed);
                        const uint32_t bl = g_fbBlack[p].exchange(0, std::memory_order_relaxed);
                        if (nb == 0u && bl == 0u)
                            continue;
                        fbdest << " fbp0x" << std::hex << p << std::dec
                               << "=" << nb << "/" << bl;
                    }

                    RUNTIME_LOG("[fbdest] seq=" << seq
                                                << " lastnb=" << lastNb
                                                << " lastnbpct="
                                                << (seq ? static_cast<int>((lastNb * 100ull) / seq) : -1)
                                                << " (nonblack/black per fbp)" << fbdest.str());

                    // [blackwho]: [fbdest] settled that colour IS stored to the
                    // right fbp and then buried (lastnbpct=98, both buffers
                    // scanning black at present). This says what buries it.
                    //   untex  -> a plain black fill ran after the artwork:
                    //             draw/clear ordering.
                    //   texzero-> textured sprites sampling RGB 0. tex0.psm=0x14
                    //             is PSMT4, so this points at the 4-bit/CLUT
                    //             decode path returning index 0.
                    //   texcol -> the texel had colour but we stored black:
                    //             blending, TEXFUNC or FBMSK ate it.
                    // tail* is the bbox of the trailing run of black writes
                    // (cleared by every coloured write) -- 0,0..511,447 means
                    // the burying draw is full-screen. snap* is the GS state at
                    // the last black store, which names the offending draw.
                    const uint64_t blkUntex = g_blkUntex.exchange(0, std::memory_order_relaxed);
                    const uint64_t blkTexZero = g_blkTexZero.exchange(0, std::memory_order_relaxed);
                    const uint64_t blkTexCol = g_blkTexCol.exchange(0, std::memory_order_relaxed);

                    RUNTIME_LOG("[blackwho] untex=" << std::dec << blkUntex
                                << " texzero=" << blkTexZero
                                << " texcol=" << blkTexCol
                                << " tail=" << g_tailX0.load(std::memory_order_relaxed)
                                << "," << g_tailY0.load(std::memory_order_relaxed)
                                << ".." << g_tailX1.load(std::memory_order_relaxed)
                                << "," << g_tailY1.load(std::memory_order_relaxed)
                                << " | last black store:"
                                << " prim=0x" << std::hex << g_snapPrim.load(std::memory_order_relaxed)
                                << " texpsm=0x" << g_snapTexPsm.load(std::memory_order_relaxed)
                                << " tbp=0x" << g_snapTbp.load(std::memory_order_relaxed)
                                << " cbp=0x" << g_snapCbp.load(std::memory_order_relaxed)
                                << " fbmsk=0x" << g_snapFbmsk.load(std::memory_order_relaxed)
                                << " alpha=0x" << g_snapAlpha.load(std::memory_order_relaxed)
                                << " texel=0x" << g_snapTexel.load(std::memory_order_relaxed)
                                << " srcrgb=0x" << g_snapSrcRgb.load(std::memory_order_relaxed)
                                << " xy=0x" << g_snapXy.load(std::memory_order_relaxed)
                                << " fbp=0x" << g_snapFbp.load(std::memory_order_relaxed)
                                << std::dec);

                    // [texzero]: texzero is the LARGEST black bucket (15.8M vs
                    // 13.8M for the clear), so it -- not the clear -- is what
                    // buries the artwork. snap* above cannot describe it: the
                    // untextured full-screen clear always lands last and
                    // overwrites the snapshot, which is why it forever reports
                    // prim=0x6 (tme=0, abe=0). These fields are captured on the
                    // texzero branch only.
                    //
                    // Read abeoff vs abeon first -- everything else follows:
                    //   abeoff >> abeon -> the game relies on the alpha TEST to
                    //       discard index-0 texels and we are not discarding
                    //       them. Check test=0x... (bit0 ATE, bits1-3 ATST,
                    //       bits4-11 AREF, bits12-13 AFAIL) and srca -- if srca
                    //       is nonzero for a texel whose alpha is zero, the
                    //       PSMT4/CLUT decode is dropping the alpha channel.
                    //   abeon  >> abeoff -> the blend ran and still produced
                    //       black; compare alpha=0x... against dstrgb.
                    // bbox says whether these are glyph-sized or full-screen.
                    const uint64_t tzAbeOn = g_tzAbeOn.exchange(0, std::memory_order_relaxed);
                    const uint64_t tzAbeOff = g_tzAbeOff.exchange(0, std::memory_order_relaxed);
                    const uint64_t tzAteOn = g_tzAteOn.exchange(0, std::memory_order_relaxed);

                    RUNTIME_LOG("[texzero] abeon=" << std::dec << tzAbeOn
                                << " abeoff=" << tzAbeOff
                                << " ateon=" << tzAteOn
                                << " bbox=" << g_tzX0.exchange(0xFFFFu, std::memory_order_relaxed)
                                << "," << g_tzY0.exchange(0xFFFFu, std::memory_order_relaxed)
                                << ".." << g_tzX1.exchange(0, std::memory_order_relaxed)
                                << "," << g_tzY1.exchange(0, std::memory_order_relaxed)
                                << " | prim=0x" << std::hex << g_tzPrim.load(std::memory_order_relaxed)
                                << " test=0x" << g_tzTest.load(std::memory_order_relaxed)
                                << " alpha=0x" << g_tzAlpha.load(std::memory_order_relaxed)
                                << " srca=0x" << g_tzSrcA.load(std::memory_order_relaxed)
                                << " texel=0x" << g_tzTexel.load(std::memory_order_relaxed)
                                << " texpsm=0x" << g_tzTexPsm.load(std::memory_order_relaxed)
                                << " tbp=0x" << g_tzTbp.load(std::memory_order_relaxed)
                                << " cbp=0x" << g_tzCbp.load(std::memory_order_relaxed)
                                << " dstrgb=0x" << g_tzDstRgb.load(std::memory_order_relaxed)
                                << " fbp=0x" << g_tzFbp.load(std::memory_order_relaxed)
                                << std::dec);

                    // [frameord]: drains the per-latch samples taken above.
                    // Pre-registered reading -- wiped/frames FIRST:
                    //   wiped ~= frames  -> every frame ends with a full-screen
                    //       black fill AFTER the last coloured pixel. That is a
                    //       clear-ordering bug and it fully explains why only
                    //       the last-drawn layer (the 9487-pixel text) survives
                    //       to present. Fix site is wherever the clear sprite
                    //       is issued relative to the draw list, not the
                    //       rasterizer.
                    //   wiped == 0      -> no per-frame erasure. [fbdest]'s
                    //       lastnbpct=99 tail was a single end-of-interval
                    //       clear and ordering is EXONERATED. Then the live
                    //       suspect is the [texzero] PSMT8 family
                    //       (texpsm=0x13, texel=0x80000000, srca=0x80), whose
                    //       blend drives the destination toward black by half
                    //       on every pass -- a CLUT decode returning RGB 0 with
                    //       a nonzero alpha.
                    //   0 < wiped < frames -> intermittent; cross-check the
                    //       wiped count against how many [present] records
                    //       report nonblack=0.
                    //   maxtrail huge but wiped tiny -> one giant wipe, i.e. a
                    //       mode change or shutdown clear, not a per-frame bug.
                    // wipebbox says whether the wiping run really is full
                    // screen (0,0..511,447) or just a large sprite.
                    const uint64_t foFrames = g_foFrames.exchange(0, std::memory_order_relaxed);
                    const uint64_t foWiped = g_foWiped.exchange(0, std::memory_order_relaxed);

                    RUNTIME_LOG("[frameord] frames=" << std::dec << foFrames
                                << " wiped=" << foWiped
                                << " wipedpct="
                                << (foFrames ? static_cast<int>((foWiped * 100ull) / foFrames) : -1)
                                << " maxtrail=" << g_foMaxTrail.exchange(0, std::memory_order_relaxed)
                                << " lasttrail=" << g_foLastTrail.load(std::memory_order_relaxed)
                                << " wipebbox=" << g_foWipeX0.exchange(0xFFFFu, std::memory_order_relaxed)
                                << "," << g_foWipeY0.exchange(0xFFFFu, std::memory_order_relaxed)
                                << ".." << g_foWipeX1.exchange(0, std::memory_order_relaxed)
                                << "," << g_foWipeY1.exchange(0, std::memory_order_relaxed));

                    // [vramcen] -- discriminates the two remaining readings of
                    // the texzero families. Costs nothing in the pixel loop:
                    // it is a once-per-second scan of buffers already in scope.
                    //
                    // Established facts it builds on:
                    //   texpsm=0x13 (PSMT8) tbp=0x2a00 cbp=0x2a40 texel=0x80000000
                    //   texpsm=0x14 (PSMT4) tbp=0x2b80 cbp=0x2a4c texel=0x00000000
                    // applyTexa() takes its `default:` branch for T4/T8, so it
                    // does NOT synthesise that 0x80 alpha -- the CLUT entry
                    // really is opaque black. Both families are therefore just
                    // "every sampled texel resolved to a black palette entry".
                    // Two ways that happens, and they need different fixes:
                    //   (a) the CLUT itself is empty/black, or
                    //   (b) the CLUT is fine but every INDEX is 0 because the
                    //       texture never landed in VRAM at tbp.
                    //
                    // TBP0/CBP are block units of 256 bytes: 0x2a00*256 =
                    // 0x2A0000, inside a 4 MB VRAM. The rival reading (page
                    // units of 8192) gives 88 MB and is impossible, so the
                    // address math has only one admissible interpretation.
                    //
                    // Pre-registered reading, in order:
                    //   clutnz == 0                  -> (a). The CLUT cache was
                    //       never populated. Fix is in ReloadClutCache/CSM1 or
                    //       its CLD gating, NOT in the sampler.
                    //   clutnz > 0 but cvnz == 0     -> (a). The cache holds
                    //       stale junk copied from empty VRAM at cbp.
                    //   clutnz > 0, cvnz > 0, texnz == 0 -> (b). Palette is
                    //       real, texture page is empty, so every index is 0.
                    //       Fix is in the GIF image-mode / TRXDIR upload path.
                    //   clutnz > 0 and texnz > 0     -> both present; the fault
                    //       is the index->entry mapping (csa offset, cpsm, or
                    //       the missing Rgba5551ToRgba8888 on CT16 CLUTs at
                    //       ps2_gs_rasterizer.cpp:841).
                    //   vramnz tiny                  -> nothing is uploaded at
                    //       all; this probe's address math is NOT the suspect,
                    //       the whole transfer path is. (Rival-reading guard:
                    //       texnz==0 alone must never be trusted without it.)
                    {
                        uint32_t clutNz = 0;
                        for (u8 b : self->m_clut_cache)
                        {
                            if (b != 0u)
                                ++clutNz;
                        }
                        u32 clut0 = 0, clut1 = 0;
                        std::memcpy(&clut0, &self->m_clut_cache[0], 4);
                        std::memcpy(&clut1, &self->m_clut_cache[4], 4);

                        const uint32_t tzTbp = g_tzTbp.load(std::memory_order_relaxed);
                        const uint32_t tzCbp = g_tzCbp.load(std::memory_order_relaxed);
                        const uint64_t texBase = static_cast<uint64_t>(tzTbp) * 256ull;
                        const uint64_t clutBase = static_cast<uint64_t>(tzCbp) * 256ull;

                        // 16 KB at tbp covers a 128x128 PSMT8 or 256x128 PSMT4.
                        uint32_t texNz = 0;
                        u32 texW0 = 0;
                        if (self->m_vram && texBase + 16384ull <= self->m_vramSize)
                        {
                            const u8 *p = self->m_vram + texBase;
                            for (uint32_t i = 0; i < 16384u; ++i)
                            {
                                if (p[i] != 0u)
                                    ++texNz;
                            }
                            std::memcpy(&texW0, p, 4);
                        }

                        // 1 KB at cbp is a full 256-entry RGBA32 palette.
                        uint32_t cvNz = 0;
                        u32 cvW0 = 0;
                        if (self->m_vram && clutBase + 1024ull <= self->m_vramSize)
                        {
                            const u8 *p = self->m_vram + clutBase;
                            for (uint32_t i = 0; i < 1024u; ++i)
                            {
                                if (p[i] != 0u)
                                    ++cvNz;
                            }
                            std::memcpy(&cvW0, p, 4);
                        }

                        uint64_t vramNz = 0;
                        if (self->m_vram && self->m_vramSize >= 4u)
                        {
                            const u32 *w = reinterpret_cast<const u32 *>(self->m_vram);
                            const size_t n = self->m_vramSize / 4u;
                            for (size_t i = 0; i < n; ++i)
                            {
                                if (w[i] != 0u)
                                    ++vramNz;
                            }
                        }

                        RUNTIME_LOG("[vramcen] clutnz=" << std::dec << clutNz
                                    << "/1024 clut0=0x" << std::hex << clut0
                                    << " clut1=0x" << clut1
                                    << " | tbp=0x" << tzTbp
                                    << " texbase=0x" << texBase
                                    << " texnz=" << std::dec << texNz
                                    << "/16384 texw0=0x" << std::hex << texW0
                                    << " | cbp=0x" << tzCbp
                                    << " clutbase=0x" << clutBase
                                    << " cvnz=" << std::dec << cvNz
                                    << "/1024 cvw0=0x" << std::hex << cvW0
                                    << " | vramnz=" << std::dec << vramNz
                                    << " vramsz=" << self->m_vramSize);

                        // [clutmap] -- the index->entry step, the only layer
                        // [vramcen] left standing. ent0..ent7 are the first 8
                        // CT32 cache entries; rgbnz counts how many of the 256
                        // entries have a nonzero RGB, which is what decides
                        // whether a black result can be blamed on the palette
                        // at all.
                        uint32_t rgbNz = 0;
                        for (uint32_t e = 0; e < 256u; ++e)
                        {
                            u32 ev = 0;
                            std::memcpy(&ev, &self->m_clut_cache[e * 4u], 4);
                            if ((ev & 0x00FFFFFFu) != 0u)
                                ++rgbNz;
                        }
                        u32 ent[8] = {};
                        for (uint32_t e = 0; e < 8u; ++e)
                            std::memcpy(&ent[e], &self->m_clut_cache[e * 4u], 4);

                        RUNTIME_LOG("[clutmap] idxsamp=" << std::dec << g_tzIdxSamp.load(std::memory_order_relaxed)
                                    << " idxnz=" << g_tzIdxNz.load(std::memory_order_relaxed)
                                    << " idxor=0x" << std::hex << g_tzIdxOr.load(std::memory_order_relaxed)
                                    << " idxlast=0x" << g_tzIdxLast.load(std::memory_order_relaxed)
                                    << " | csa=" << std::dec << g_tzCsa.load(std::memory_order_relaxed)
                                    << " csm=" << g_tzCsm.load(std::memory_order_relaxed)
                                    << " cpsm=0x" << std::hex << g_tzCpsm.load(std::memory_order_relaxed)
                                    << " texpsm=0x" << g_tzTexPsm.load(std::memory_order_relaxed)
                                    << " | rgbnz=" << std::dec << rgbNz
                                    << "/256 ent=0x" << std::hex << ent[0]
                                    << ",0x" << ent[1] << ",0x" << ent[2] << ",0x" << ent[3]
                                    << ",0x" << ent[4] << ",0x" << ent[5] << ",0x" << ent[6]
                                    << ",0x" << ent[7]);

                        // [clutdump] -- Stage 5.11 run 15. [texzero] showed the
                        // T8 draw sampling texel=0x80000000 with srca=0x80:
                        // OPAQUE black, passing ATST=NOTEQUAL/AREF=0, blended by
                        // ALPHA=0x44 as (Cs-Cd)*As+Cd at As=1.0 -> pure black
                        // overwrite. But [clutmap]'s ent0..ent7 alphas are a ramp
                        // (0x00,0x0a,0x13,0x1a,0x21,0x28,0x30,0x38) that never
                        // reaches 0x80, and idxor=0x1f bounds every offending
                        // index to 0..31. So the 0x80 comes from somewhere in
                        // entries 8..31 -- or from the read path, not the cache.
                        //
                        // applyTexa is already exonerated (psm=0x13 hits the
                        // default: case, alpha passes through unchanged) and the
                        // CSM1 T8 scatter matches spec, so this probe separates
                        // the three remaining candidates by measuring the SAME
                        // entry at three stages:
                        //
                        //   vramL  = ReadVram at idxlast's scattered coordinate
                        //            -> what the palette actually holds in VRAM
                        //   cacheL = m_clut_cache[idxlast]  (direct byte read)
                        //            -> what ReloadClutCacheCSM1 deposited
                        //   readL  = ReadClutCache(cpsm, idxlast, csa)
                        //            -> what the sampler at rasterizer:889 gets
                        //
                        // PRE-REGISTERED READING:
                        //   vramL == cacheL == readL == 0x80000000
                        //       -> the palette genuinely contains opaque black at
                        //          that index. Our CLUT path is CORRECT and the
                        //          T8 draw is a legitimate shadow/outline layer.
                        //          Stage 5.11 then moves OFF the texture pipeline
                        //          entirely: the red box is a different draw that
                        //          [texzero] cannot see (it fires only on black
                        //          texels). Next look: the fbp=0x70 vs fbp=0x0
                        //          split, i.e. which framebuffer is presented.
                        //   vramL colourful, cacheL == 0x80000000
                        //       -> ReloadClutCacheCSM1 wrote the wrong slot.
                        //          Suspect the scatter or the CLD/CBP trigger.
                        //   cacheL colourful, readL == 0x80000000
                        //       -> ReadClutCache addressing is wrong. That is the
                        //          unmasked csa*64+index*4 at gpu.cpp:4134.
                        //   a80 large (most of 0..31 are alpha 0x80)
                        //       -> the region was never overwritten by a real
                        //          upload; 0x80000000 is a fill/default value and
                        //          the palette load never covered these entries.
                        //
                        // GUARD (rival reading): a80 and rgbnz are computed over
                        // DIFFERENT windows (0..31 vs 0..255) on purpose. If a80
                        // is 0 while readL is 0x80000000, idxlast is not
                        // representative and the single-entry fields describe one
                        // sample, not the population -- do not generalise.
                        const uint32_t idxL = g_tzIdxLast.load(std::memory_order_relaxed) & 0xFFu;
                        const uint32_t csaL = g_tzCsa.load(std::memory_order_relaxed);
                        const uint32_t cpsmL = g_tzCpsm.load(std::memory_order_relaxed);

                        u32 dmp[32] = {};
                        uint32_t a80 = 0;
                        for (uint32_t e = 0; e < 32u; ++e)
                        {
                            std::memcpy(&dmp[e], &self->m_clut_cache[e * 4u], 4);
                            if (((dmp[e] >> 24) & 0xFFu) == 0x80u)
                                ++a80;
                        }

                        u32 cacheL = 0;
                        std::memcpy(&cacheL, &self->m_clut_cache[(idxL * 4u) & 0x3FFu], 4);
                        const u32 readL = self->ReadClutCache(cpsmL, static_cast<u8>(idxL), csaL);

                        // CSM1 T8 scatter, same formula as ReloadClutCacheCSM1.
                        u32 sx = idxL & 7u;
                        if (idxL & 0x10u) { sx += 8u; }
                        u32 sy = (idxL & 0xE0u) / 16u;
                        if (idxL & 0x8u) { sy++; }
                        const u32 vramL = self->ReadVram(cpsmL, tzCbp, 1, sx, sy);

                        RUNTIME_LOG("[clutdump] idxlast=0x" << std::hex << idxL
                                    << " sx=" << std::dec << sx << " sy=" << sy
                                    << " | vraml=0x" << std::hex << vramL
                                    << " cachel=0x" << cacheL
                                    << " readl=0x" << readL
                                    << " | a80=" << std::dec << a80 << "/32"
                                    << " rgbnz=" << rgbNz << "/256");

                        std::ostringstream dumpLine;
                        dumpLine << std::hex;
                        for (uint32_t e = 0; e < 32u; ++e)
                            dumpLine << (e ? ",0x" : "0x") << dmp[e];
                        RUNTIME_LOG("[clutdump] ent0_31=" << dumpLine.str());

                        // ---- [clutlive] ------------------------------------
                        // Consumer. See the counter definitions above for why
                        // this exists, the dump-derived ground truth, and the
                        // stated scope limit (host-side only; this cannot see
                        // what the sampler computed).
                        //
                        // Three measurements, all on the LIVE atlas:
                        //   cache0_15 = ReadClutCache(cpsm, i, csa), i=0..15
                        //       -> the 16 words the sampler will actually use.
                        //   vram0_15  = ReadVram(cpsm, cbp, 1, i&7, (i/8)&1)
                        //       -> the same 16 words re-read from VRAM NOW,
                        //          with the identical CSM1 T4 scatter used by
                        //          ReloadClutCacheCSM1. Same data, two read
                        //          times: they agree iff the cache is fresh.
                        //          This is also the RIVAL READING -- if all 16
                        //          come back identical, the degenerate result
                        //          convicts the probe before the palette.
                        //   idxhist16 = histogram of the 4-bit indices in the
                        //       256x256 atlas at tbp0. Whole atlas, not one
                        //       glyph: UVs are not observable in this TU, and
                        //       the question ("is there ANY coverage in the
                        //       atlas?") does not need them.
                        //
                        // EXPECTED cache0_15 if the live palette is correct
                        // (from the authentic dump, verbatim):
                        //   0x00000000 0x08FFFFFF 0x11FFFFFF 0x19FFFFFF
                        //   0x22FFFFFF 0x2AFFFFFF 0x33FFFFFF 0x3BFFFFFF
                        //   0x44FFFFFF 0x4CFFFFFF 0x55FFFFFF 0x5DFFFFFF
                        //   0x66FFFFFF 0x6EFFFFFF 0x77FFFFFF 0x80FFFFFF
                        //
                        // PRE-REGISTERED READING. Read hits FIRST; it is the
                        // liveness guard and every other field is meaningless
                        // without it.
                        //   0. hits == 0
                        //       -> the gate never matched. ABSENCE PROVES
                        //          NOTHING. Read the gate at the TEX0 handler
                        //          before concluding anything at all; do not
                        //          read any field below.
                        //   1. cache0_15 matches the ramp above AND idxnz is a
                        //      large fraction of 65536
                        //       -> live CLUT and atlas data are both correct.
                        //          The palette is exonerated and the fault is
                        //          downstream, in the sampler or in suspects
                        //          #7/#8/#9/#11. Next probe goes in the
                        //          rasterizer, which this one deliberately
                        //          does not touch.
                        //   2. idxnz == 0 (or nearly), cache0_15 correct
                        //       -> TOP HYPOTHESIS CONFIRMED at the DATA layer:
                        //          the atlas itself holds index 0 everywhere,
                        //          which is fully transparent, so no text can
                        //          ever composite. The bug is the T4 upload or
                        //          its swizzle, NOT the palette and NOT the
                        //          sampler.
                        //   3. idxnz healthy but cache0_15 alpha column is
                        //      scaled (max 0xFF instead of 0x80, or halved)
                        //       -> alpha-scale bug. PS2 alpha is 0..128, not
                        //          0..255. Find the conversion site.
                        //   4. cache0_15 != vram0_15
                        //       -> stale cache: the palette upload landed
                        //          after the TEX0 write that loaded it. The
                        //          dump proves upload precedes TEX0, so this
                        //          means the LIVE EE inverted the order. Fix
                        //          would be CLUT invalidation on a transfer
                        //          overlapping [cbp, cbp+1).
                        //   5. cache0_15 == vram0_15 but all 16 are zero
                        //       -> the palette was never written to this cbp.
                        //          Bug is upstream of the GS, in the EE's
                        //          transfer. Next probe goes on BITBLTBUF
                        //          dest, not here.
                        //   6. cache0_15 all ONE value (any value)
                        //       -> degenerate column. CONVICT THE PROBE, not
                        //          the palette: check csa/cpsm are being
                        //          passed through rather than defaulted.
                        //
                        // NOTE the rows are not mutually exclusive by
                        // accident: 2 and 5 are the two ways to get a blank
                        // dialog, and they point at different subsystems. That
                        // separation is the reason both idxnz and vram0_15 are
                        // emitted rather than just the cache.
                        {
                            using namespace ps2diag_fbstat;
                            const uint32_t clHits = g_clHits.load(std::memory_order_relaxed);
                            const uint32_t clCbp = g_clCbp.load(std::memory_order_relaxed);
                            const uint32_t clCpsm = g_clCpsm.load(std::memory_order_relaxed);
                            const uint32_t clCsa = g_clCsa.load(std::memory_order_relaxed);
                            const uint32_t clTbp0 = g_clTbp0.load(std::memory_order_relaxed);
                            const uint32_t clTbw = g_clTbw.load(std::memory_order_relaxed);

                            RUNTIME_LOG("[clutlive] hits=" << std::dec << clHits
                                        << " tbp0=0x" << std::hex << clTbp0
                                        << " tbw=" << std::dec << clTbw
                                        << " cbp=0x" << std::hex << clCbp
                                        << " cpsm=0x" << clCpsm
                                        << " csm=" << std::dec << g_clCsm.load(std::memory_order_relaxed)
                                        << " csa=" << clCsa
                                        << " cld=" << g_clCld.load(std::memory_order_relaxed));

                            if (clHits == 0u)
                            {
                                // Never silently skip: a missing block is
                                // indistinguishable from "it never happened".
                                //
                                // "tag=clutlive" is not decoration -- analyze_run.py
                                // detects caps with  \[cap\] tag=(\S+)  and, in
                                // --tag mode, by requiring the tag NAME inside the
                                // [cap] chunk. Round 1 wrote the cap without it, so
                                // the analyzer printed "no [cap] for this tag:
                                // absence IS evidence" over a run that HAD capped.
                                // A cap line the cap detector cannot see is worse
                                // than no cap line at all.
                                RUNTIME_LOG("[clutlive] [cap] tag=clutlive gate never"
                                            " matched (psm=0x14 tbw=4 256x256 TEX0"
                                            " write); fields below intentionally"
                                            " omitted");
                            }
                            else
                            {
                                std::ostringstream cacheLine, vramLine;
                                cacheLine << std::hex;
                                vramLine << std::hex;
                                uint32_t cacheNonBlack = 0;
                                uint32_t mismatch = 0;
                                uint32_t distinct = 0;
                                u32 seen[16] = {};
                                for (uint32_t ce = 0; ce < 16u; ++ce)
                                {
                                    const u32 cv = self->ReadClutCache(clCpsm,
                                                                       static_cast<u8>(ce),
                                                                       clCsa);
                                    // Identical CSM1 T4 scatter to
                                    // ReloadClutCacheCSM1 -- deliberately
                                    // duplicated rather than shared so a bug in
                                    // one does not hide itself in the other.
                                    const u32 vv = self->ReadVram(clCpsm, clCbp, 1,
                                                                  ce & 7u, (ce / 8u) & 1u);
                                    cacheLine << (ce ? ",0x" : "0x") << cv;
                                    vramLine << (ce ? ",0x" : "0x") << vv;
                                    if ((cv & 0x00FFFFFFu) != 0u)
                                        ++cacheNonBlack;
                                    if (cv != vv)
                                        ++mismatch;
                                    bool dup = false;
                                    for (uint32_t k = 0; k < distinct; ++k)
                                        if (seen[k] == cv) { dup = true; break; }
                                    if (!dup)
                                        seen[distinct++] = cv;
                                }

                                RUNTIME_LOG("[clutlive] cache0_15=" << cacheLine.str());
                                RUNTIME_LOG("[clutlive] vram0_15=" << vramLine.str());
                                RUNTIME_LOG("[clutlive] nonblack=" << std::dec << cacheNonBlack
                                            << "/16 mismatch=" << mismatch
                                            << "/16 distinct=" << distinct << "/16");

                                // idxhist16 over the whole 256x256 PSMT4 atlas.
                                // 65536 reads, but this sits on the ~1/sec
                                // throttled interval, so it is ~0.1% of a
                                // frame's pixel work -- and it is the field
                                // that separates reading 2 from reading 1.
                                uint64_t idxHist[16] = {};
                                uint64_t idxNz = 0;
                                for (uint32_t ay = 0; ay < 256u; ++ay)
                                {
                                    for (uint32_t ax = 0; ax < 256u; ++ax)
                                    {
                                        const u32 aidx = self->ReadVram(0x14u, clTbp0,
                                                                        clTbw, ax, ay) & 0xFu;
                                        ++idxHist[aidx];
                                        if (aidx != 0u)
                                            ++idxNz;
                                    }
                                }

                                std::ostringstream histLine;
                                for (uint32_t hb = 0; hb < 16u; ++hb)
                                    histLine << (hb ? "," : "") << idxHist[hb];
                                RUNTIME_LOG("[clutlive] idxnz=" << std::dec << idxNz
                                            << "/65536 idxhist16=" << histLine.str());

                                // Round 2: what the atlas draw ITSELF sampled.
                                // Read the classification counts BEFORE the
                                // verbatim dump -- atwrite0_15 is one instant
                                // and the counts are the whole run, so the
                                // counts are the evidence and the dump is only
                                // there to be read when they disagree.
                                const uint32_t atwW = g_clAtwWhite.load(std::memory_order_relaxed);
                                const uint32_t atwB = g_clAtwBlack.load(std::memory_order_relaxed);
                                const uint32_t atwO = g_clAtwOther.load(std::memory_order_relaxed);
                                RUNTIME_LOG("[clutlive] atw white=" << std::dec << atwW
                                            << " black=" << atwB
                                            << " other=" << atwO
                                            << " sum=" << (atwW + atwB + atwO)
                                            << " hits=" << clHits);

                                std::ostringstream atwLine;
                                atwLine << std::hex;
                                for (uint32_t ce = 0; ce < 16u; ++ce)
                                    atwLine << (ce ? ",0x" : "0x")
                                            << g_clSnap[ce].load(std::memory_order_relaxed);
                                RUNTIME_LOG("[clutlive] atwrite0_15=" << atwLine.str());

                                if (g_clBadSeen.load(std::memory_order_relaxed) != 0u)
                                {
                                    std::ostringstream badLine;
                                    badLine << std::hex;
                                    for (uint32_t ce = 0; ce < 16u; ++ce)
                                        badLine << (ce ? ",0x" : "0x")
                                                << g_clSnapBad[ce].load(std::memory_order_relaxed);
                                    RUNTIME_LOG("[clutlive] firstblack cbp=0x" << std::hex
                                                << g_clBadCbp.load(std::memory_order_relaxed)
                                                << " pal=" << badLine.str());
                                }
                                else
                                {
                                    RUNTIME_LOG("[clutlive] firstblack none"
                                                " (no all-black palette was ever"
                                                " sampled at an atlas TEX0 write)");
                                }

                                // How many distinct palettes the gate matched.
                                // More than one means the single latched cbp
                                // above is last-write-wins and must not be read
                                // as "the" glyph palette.
                                std::ostringstream cbpLine;
                                cbpLine << std::hex;
                                uint32_t cbpSlots = 0;
                                for (uint32_t s = 0; s < 4u; ++s)
                                {
                                    const uint32_t slot =
                                        g_clCbpVal[s].load(std::memory_order_relaxed);
                                    if (slot == 0u)
                                        continue;
                                    cbpLine << (cbpSlots ? " 0x" : "0x") << (slot - 1u)
                                            << "x" << std::dec
                                            << g_clCbpHit[s].load(std::memory_order_relaxed)
                                            << std::hex;
                                    ++cbpSlots;
                                }
                                RUNTIME_LOG("[clutlive] cbpcensus slots=" << std::dec << cbpSlots
                                            << "/4 " << cbpLine.str()
                                            << (cbpSlots >= 4u
                                                    ? " [cap] tag=clutlive census full,"
                                                      " more palettes may exist"
                                                    : ""));
                            }
                        }

                        // [texred] -- the fate of the pixels that actually
                        // carry the box's colour. Read anysamp first (liveness
                        // guard), then boxsamp, then the fate breakdown.
                        //
                        // Stage 5.11 run 23: the sample is now redsamp UNION
                        // boxsamp, not redsamp alone. [boxtex] measured the
                        // box texel as 0x6629127e, whose tb=0x29 FAILS the
                        // colour predicate's tb<0x20 -- so every earlier
                        // killate/killz/storedblack==0 described other
                        // sprites and said nothing about the box.
                        //
                        //   boxsamp == 0  -> the box never reached writePixel.
                        //       Every fate below is silent about it; the loss
                        //       is upstream, in drawSprite's clip or in the
                        //       caller. Read no other field.
                        //   killate dominant -> suspect #7 convicted; read test.
                        //   killz   dominant -> suspect #8 convicted; read ztst.
                        //   stored ~= storedblack -> suspect #9, the blender;
                        //       compare alpha against dstrgb.
                        //   stored large, storedblack == 0 -> the red IS in
                        //       VRAM and suspect #11 (overdraw) is the last
                        //       one standing; next instrument is an overdraw
                        //       counter on the bbox, not another read-back.
                        //
                        // The four fates are exclusive and must sum to
                        // redsamp+boxsamp minus their overlap; if they exceed
                        // it, a pixel is escaping down a path this probe does
                        // not cover and the split is unsafe to read.
                        RUNTIME_LOG("[texred] anysamp=" << std::dec << g_trAnySamp.load(std::memory_order_relaxed)
                                    << " redsamp=" << g_trRedSamp.load(std::memory_order_relaxed)
                                    << " boxsamp=" << g_trBoxSamp.load(std::memory_order_relaxed)
                                    << " | killate=" << g_trKillAte.load(std::memory_order_relaxed)
                                    << " killz=" << g_trKillZ.load(std::memory_order_relaxed)
                                    << " stored=" << g_trStored.load(std::memory_order_relaxed)
                                    << " storedblack=" << g_trStoredBlack.load(std::memory_order_relaxed)
                                    << " | texel=0x" << std::hex << g_trTexel.load(std::memory_order_relaxed)
                                    << " texpsm=0x" << g_trTexPsm.load(std::memory_order_relaxed)
                                    << " prim=0x" << g_trPrim.load(std::memory_order_relaxed)
                                    << " test=0x" << g_trTest.load(std::memory_order_relaxed)
                                    << " alpha=0x" << g_trAlpha.load(std::memory_order_relaxed)
                                    << " srca=0x" << g_trSrcA.load(std::memory_order_relaxed)
                                    << " | srcrgb=0x" << g_trSrcRgb.load(std::memory_order_relaxed)
                                    << " dstrgb=0x" << g_trDstRgb.load(std::memory_order_relaxed)
                                    << " pixel=0x" << g_trPixel.load(std::memory_order_relaxed)
                                    << " | fbpor=0x" << g_trFbpOr.load(std::memory_order_relaxed)
                                    << " fbplast=0x" << g_trFbpLast.load(std::memory_order_relaxed)
                                    << " bbox=" << std::dec << g_trX0.load(std::memory_order_relaxed)
                                    << "," << g_trY0.load(std::memory_order_relaxed)
                                    << ".." << g_trX1.load(std::memory_order_relaxed)
                                    << "," << g_trY1.load(std::memory_order_relaxed));

                        // [boxblk] -- who blacks out the red box. Scoped to the
                        // same rect censusRedBox uses, so the two probes are
                        // describing the same pixels and can be compared.
                        std::ostringstream idxLine;
                        for (uint32_t e = 0; e < 9u; ++e)
                            idxLine << (e ? "," : "") << g_bbIdx[e].load(std::memory_order_relaxed);

                        RUNTIME_LOG("[boxblk] bbany=" << std::dec << g_bbAny.load(std::memory_order_relaxed)
                                    << " bb16=" << g_bb16.load(std::memory_order_relaxed)
                                    << " bbred=" << g_bbRed.load(std::memory_order_relaxed)
                                    << " bbblack=" << g_bbBlack.load(std::memory_order_relaxed)
                                    << " | blktex=" << g_bbBlkTex.load(std::memory_order_relaxed)
                                    << " blkuntex=" << g_bbBlkUntex.load(std::memory_order_relaxed)
                                    // NOTE: no '[' in this field name. analyze_run.py matches
                                    // records as \[tag\][^\[]* , so a literal '[' truncates the
                                    // record at the analyzer (run 18 lost every field after it).
                                    << " | idxb_0_4_8_12_16_20_24_28_32="
                                    << idxLine.str()
                                    << " | idxlast=" << g_bbIdxLast.load(std::memory_order_relaxed)
                                    << " texel=0x" << std::hex << g_bbTexel.load(std::memory_order_relaxed)
                                    << " texpsm=0x" << g_bbTexPsm.load(std::memory_order_relaxed)
                                    << " tbp=0x" << g_bbTbp.load(std::memory_order_relaxed)
                                    << " cbp=0x" << g_bbCbp.load(std::memory_order_relaxed)
                                    << " csa=0x" << g_bbCsa.load(std::memory_order_relaxed)
                                    << " prim=0x" << g_bbPrim.load(std::memory_order_relaxed)
                                    << " test=0x" << g_bbTest.load(std::memory_order_relaxed)
                                    << " srca=0x" << g_bbSrcA.load(std::memory_order_relaxed)
                                    << " fbp=0x" << g_bbFbp.load(std::memory_order_relaxed)
                                    << " at=" << std::dec << (g_bbXy.load(std::memory_order_relaxed) & 0xFFFFu)
                                    << "," << (g_bbXy.load(std::memory_order_relaxed) >> 16));

                        // ---- [shadow] -- Stage 5.11 run 19 -------------------
                        // [boxblk] settled WHO erases the box: black TEXTURED
                        // stores, ~87.9k/frame into a 54,978 px rect, while the
                        // untextured clear contributed exactly one full rect per
                        // frame (innocent) and the red box was drawn in full every
                        // frame (~55.8k/frame, so the draw path is fine).
                        //
                        // RETRACTED: [boxblk]'s own index histogram cannot convict
                        // the index decode. g_bbIdx only counts stores that came
                        // out BLACK, so a red index can never appear in it -- the
                        // "i20_31 == 0" result is near-tautological. Row 1 of the
                        // [boxblk] reading table was mis-designed. What survives is
                        // row 2's refutation: no black store came from a red index.
                        //
                        // The surviving arithmetic: ~45.5k px of the box go black
                        // and ~9.5k survive, and the survivors are the glyphs. That
                        // is an INVERTED MASK -- the shadow layer is opaque
                        // everywhere except the text, when a drop shadow should be
                        // the exact reverse.
                        //
                        // The eraser's snapshot is idx=8 -> texel=0x80000000 (opaque
                        // black) from cbp=0x2a48. [clutdump] dumped m_clut_cache,
                        // i.e. whatever palette was resident at dump time -- and
                        // [gs:image] shows FOUR distinct 16x16 CT32 CLUTs uploaded
                        // at 0x2a40/0x2a44/0x2a48/0x2a4c. So the known "entries 0..16
                        // are a black alpha ramp" result describes a DIFFERENT
                        // palette and must not be assumed here.
                        //
                        // This probe reads the eraser's OWN palette (cbp = g_bbCbp)
                        // straight out of VRAM with the CSM1 scatter, alongside the
                        // cache, entries 0..31.
                        //
                        // PRE-REGISTERED READING (read shcbp + shnz FIRST -- GUARD):
                        //   vram entry 8 is alpha 0x80 with RGB 0, and cache agrees
                        //       -> the palette GENUINELY holds opaque black at the
                        //          index the sprite sampled. The CLUT chain is
                        //          correct and the fault is UPSTREAM: either the
                        //          texture indices or the sprite's UVs. Next tool is
                        //          a UV/index probe on tbp=0x2a80, NOT a CLUT fix.
                        //   vram entry 8 has LOW alpha but cache/read entry 8 reads
                        //   back 0x80000000
                        //       -> the cache load corrupts it; the bug is in
                        //          ReloadClutCacheCSM1. This is the only reading
                        //          that licenses touching CLUT code.
                        //   any e where vram[e] != cache[e]
                        //       -> loader mismatch, localised to that entry; report
                        //          the first such e before doing anything else.
                        //   opaque count (a80) is ~32/32
                        //       -> the whole palette is opaque, so NO index could
                        //          have produced a transparent texel and the
                        //          inverted-mask reading is explained by the palette
                        //          alone.
                        //
                        // GUARD (rival reading):
                        //   shcbp = the cbp actually captured by [boxblk]. If it is
                        //          0 the eraser snapshot never fired and every field
                        //          below is void -- the probe is convicted, not the
                        //          renderer. Expect 0x2a48.
                        //   shnz  = how many of the 32 VRAM entries are nonzero. If
                        //          this is 0 the scatter formula or cbp is wrong and
                        //          we are reading empty VRAM, NOT a black palette.
                        //          A genuinely-opaque-black palette still reads
                        //          nonzero (alpha 0x80 in the top byte).
                        {
                            const uint32_t shCbp  = g_bbCbp.load(std::memory_order_relaxed);
                            const uint32_t shCpsm = g_bbCpsm.load(std::memory_order_relaxed);
                            const uint32_t shCsa  = g_bbCsa.load(std::memory_order_relaxed);

                            std::ostringstream vramLine;
                            std::ostringstream cacheLine;
                            vramLine << std::hex;
                            cacheLine << std::hex;

                            uint32_t shNz = 0, shA80 = 0, shOpaqueBlack = 0;
                            int firstMismatch = -1;

                            for (uint32_t e = 0; e < 32u; ++e)
                            {
                                // Same CSM1 T8 scatter ReloadClutCacheCSM1 uses.
                                uint32_t sx = e & 7u;
                                if (e & 0x10u) { sx += 8u; }
                                uint32_t sy = (e & 0xE0u) / 16u;
                                if (e & 0x8u) { sy++; }

                                const u32 v = self->ReadVram(shCpsm, shCbp, 1, sx, sy);
                                u32 c = 0;
                                std::memcpy(&c, &self->m_clut_cache[(e * 4u) & 0x3FFu], 4);

                                if (v != 0u) ++shNz;
                                if (((v >> 24) & 0xFFu) == 0x80u) ++shA80;
                                if (((v >> 24) & 0xFFu) == 0x80u && (v & 0x00FFFFFFu) == 0u)
                                    ++shOpaqueBlack;
                                if (v != c && firstMismatch < 0) firstMismatch = static_cast<int>(e);

                                vramLine  << (e ? ",0x" : "0x") << v;
                                cacheLine << (e ? ",0x" : "0x") << c;
                            }

                            RUNTIME_LOG("[shadow] shcbp=0x" << std::hex << shCbp
                                        << " shcpsm=0x" << shCpsm
                                        << " shcsa=0x" << shCsa
                                        << " | shnz=" << std::dec << shNz << "/32"
                                        << " a80=" << shA80 << "/32"
                                        << " opaqueblack=" << shOpaqueBlack << "/32"
                                        << " firstmismatch=" << firstMismatch);
                            RUNTIME_LOG("[shadow] vram0_31=" << vramLine.str());
                            RUNTIME_LOG("[shadow] cache0_31=" << cacheLine.str());
                        }

                        // ---- [texfetch] -- Stage 5.11 run 24 -------------
                        // Re-pointed at the T4 glyph fetch (psm T4, tbw 4,
                        // 256x256). Reading table is at the counter
                        // definitions in ps2_gs_rasterizer.cpp. Read
                        // tfall/tft4/tfsamp/tfchk FIRST; they are the guards
                        // and every other field is meaningless without them.
                        {
                            const uint64_t tfAll  = g_tfAll.load(std::memory_order_relaxed);
                            const uint64_t tfT4   = g_tfT4.load(std::memory_order_relaxed);
                            const uint64_t tfSamp = g_tfSamp.load(std::memory_order_relaxed);
                            const uint64_t tfChk  = g_tfChk.load(std::memory_order_relaxed);

                            std::ostringstream histLine;
                            uint64_t histSum = 0;
                            for (uint32_t b = 0; b < 16u; ++b)
                            {
                                const uint64_t hv = g_tfHist[b].load(std::memory_order_relaxed);
                                histSum += hv;
                                histLine << (b ? "," : "") << hv;
                            }
                            const uint64_t bin0 = g_tfHist[0].load(std::memory_order_relaxed);
                            const uint64_t a0   = g_tfA0.load(std::memory_order_relaxed);

                            const char *verdict =
                                (tfAll  == 0) ? "PROBE-DEAD-no-paletted-sample-reached-sampler"
                              : (tfT4   == 0) ? "NO-T4-FETCH-glyph-sprite-never-sampled-or-psm-decode-wrong"
                              : (tfSamp == 0) ? "SHAPE-WRONG-t4-seen-but-not-256x256-tbw4-values-below-are-STALE"
                              : (bin0 == tfSamp) ? "FETCH-DEGENERATE-every-texel-index-0-fully-transparent"
                              : (a0 == tfSamp) ? "CLUT-DEGENERATE-indices-vary-but-every-alpha-is-0"
                                               : "gate-matched-values-below-are-valid";

                            RUNTIME_LOG("[texfetch] tfall=" << std::dec << tfAll
                                        << " tft4=" << tfT4
                                        << " tfsamp=" << tfSamp
                                        << " tfchk=" << tfChk
                                        << " tfdis=" << g_tfDis.load(std::memory_order_relaxed)
                                        << " | u=" << g_tfUmin.load(std::memory_order_relaxed)
                                        << ".." << g_tfUmax.load(std::memory_order_relaxed)
                                        << " v=" << g_tfVmin.load(std::memory_order_relaxed)
                                        << ".." << g_tfVmax.load(std::memory_order_relaxed)
                                        << " | idxmax=" << g_tfIdxMax.load(std::memory_order_relaxed)
                                        << " oor=" << g_tfOor.load(std::memory_order_relaxed)
                                        << " bin0=" << bin0
                                        << " histsum=" << histSum
                                        << " | a0=" << a0
                                        << " amax=" << g_tfAMax.load(std::memory_order_relaxed)
                                        << " white=" << g_tfWhite.load(std::memory_order_relaxed)
                                        << " | verdict=" << verdict);

                            RUNTIME_LOG("[texfetch] cfg tbw=" << std::dec
                                        << g_tfTbw.load(std::memory_order_relaxed)
                                        << " tw=" << g_tfTw.load(std::memory_order_relaxed)
                                        << " th=" << g_tfTh.load(std::memory_order_relaxed)
                                        << " tbp0=0x" << std::hex
                                        << g_tfTbp.load(std::memory_order_relaxed)
                                        << " cpsm=0x" << g_tfCpsm.load(std::memory_order_relaxed)
                                        << " csa=" << std::dec
                                        << g_tfCsa.load(std::memory_order_relaxed)
                                        << " | badu=" << g_tfBadU.load(std::memory_order_relaxed)
                                        << " badv=" << g_tfBadV.load(std::memory_order_relaxed)
                                        << " badc=0x" << std::hex
                                        << g_tfBadCache.load(std::memory_order_relaxed)
                                        << " badv2=0x"
                                        << g_tfBadVram.load(std::memory_order_relaxed)
                                        << std::dec);

                            RUNTIME_LOG("[texfetch] hist16=" << histLine.str());
                            RUNTIME_LOG("[texfetch] vramtruth=60638,0,0,22,75,83,104,416,68,68,63,365,108,0,158,3368");
                        }

                        // ---- [glyphfate] -- Stage 5.11 run 25 ------------
                        // Run 24 closed every arithmetic link: the T4 fetch
                        // and the CLUT lookup both verified correct on 115M
                        // samples. What remains is fate and ordering, which
                        // is all this emits. Table at the counters in
                        // ps2_gs_rasterizer.cpp.
                        {
                            const uint64_t gfDraws  = g_gfDraws.load(std::memory_order_relaxed);
                            const uint64_t gfIn     = g_gfIn.load(std::memory_order_relaxed);
                            const uint64_t gfScis   = g_gfScis.load(std::memory_order_relaxed);
                            const uint64_t gfStored = g_gfStored.load(std::memory_order_relaxed);
                            const uint64_t gfWhite  = g_gfWhite.load(std::memory_order_relaxed);
                            const uint64_t gfLast   = g_gfLastSeq.load(std::memory_order_relaxed);
                            const uint64_t gfBox    = g_gfBoxSeq.load(std::memory_order_relaxed);

                            const uint32_t gx0 = g_gfX0.load(std::memory_order_relaxed);
                            const uint32_t gy0 = g_gfY0.load(std::memory_order_relaxed);
                            const uint32_t gx1 = g_gfX1.load(std::memory_order_relaxed);
                            const uint32_t gy1 = g_gfY1.load(std::memory_order_relaxed);

                            // Dump rect for the dialog: 464x120 at 24,302.
                            const bool inRect = (gx0 >= 20u && gy0 >= 298u &&
                                                 gx1 <= 492u && gy1 <= 426u);

                            const char *verdict =
                                (gfDraws == 0)   ? "GATE-DEAD-no-t4-256x256-tbw4-sprite-reached-drawSprite"
                              : (gfIn == 0)      ? "CLIPPED-OUT-sprite-gated-but-pixel-loop-emitted-nothing"
                              : (gfScis == gfIn) ? "ALL-SCISSORED-compare-sc-rect-against-24,302-488,422"
                              : (gfStored == 0)  ? "ALL-KILLED-see-gfate-and-gfz-both-should-be-0"
                              : (gfWhite == 0)   ? "STORED-BUT-DARK-blend-or-pack-destroys-white-check-gfcc"
                              : (!inRect)        ? "WRONG-PLACE-white-text-stored-outside-the-dialog-rect"
                                                 : "TEXT-STORED-WHITE-IN-RECT-see-fbsplit-for-target-and-order";
                            // The old ordering branch was removed after run 25:
                            // gfBox/gfLast are stamped from g_writeSeq, which
                            // [fbdest] exchange(0)'s below, so the comparison
                            // could flip on a reset landing between the two
                            // stores. [fbsplit] re-measures it on its own
                            // counter. Do not restore this branch.

                            RUNTIME_LOG("[glyphfate] gfdraws=" << std::dec << gfDraws
                                        << " gfin=" << gfIn
                                        << " gfscis=" << gfScis
                                        << " gfate=" << g_gfAte.load(std::memory_order_relaxed)
                                        << " gfz=" << g_gfZ.load(std::memory_order_relaxed)
                                        << " gfstored=" << gfStored
                                        << " | white=" << gfWhite
                                        << " dark=" << g_gfDark.load(std::memory_order_relaxed)
                                        << " srcamax=" << g_gfSrcAMax.load(std::memory_order_relaxed)
                                        << " | bbox=" << gx0 << "," << gy0
                                        << "-" << gx1 << "," << gy1
                                        << " inrect=" << (inRect ? 1 : 0)
                                        << " | verdict=" << verdict);

                            RUNTIME_LOG("[glyphfate] pix=0x" << std::hex
                                        << g_gfPix.load(std::memory_order_relaxed)
                                        << " dst=0x" << g_gfDst.load(std::memory_order_relaxed)
                                        << " test=0x" << g_gfTest.load(std::memory_order_relaxed)
                                        << std::dec
                                        << " cc=" << g_gfCc.load(std::memory_order_relaxed)
                                        << " abe=" << g_gfAbe.load(std::memory_order_relaxed)
                                        << " | fbpor=" << g_gfFbpOr.load(std::memory_order_relaxed)
                                        << " fbpmin=" << g_gfFbpMin.load(std::memory_order_relaxed)
                                        << " fbpmax=" << g_gfFbpMax.load(std::memory_order_relaxed)
                                        << " | sc=" << g_gfScX0.load(std::memory_order_relaxed)
                                        << "," << g_gfScY0.load(std::memory_order_relaxed)
                                        << "-" << g_gfScX1.load(std::memory_order_relaxed)
                                        << "," << g_gfScY1.load(std::memory_order_relaxed));

                            // UNSOUND ORDER FIELDS -- kept only so the run 25
                            // log stays comparable. Both stamps come from
                            // g_writeSeq, which [fbdest] zeroes, so a reset
                            // between the two stores inverts the comparison
                            // and `total` reads 0 because [fbdest] already
                            // took it. Use [fbsplit] boxord/glyphord instead.
                            RUNTIME_LOG("[glyphfate] unsound-seq glyphlast=" << std::dec << gfLast
                                        << " boxlast=" << gfBox
                                        << " total=" << g_writeSeq.load(std::memory_order_relaxed));
                        }

                        // ---- [fbsplit] -- Stage 5.11 run 26 --------------
                        // Run 25 proved the white text is stored, in the
                        // right rect, and never killed -- yet dst under it
                        // was 0x0 (no maroon box beneath) and its stores
                        // spanned two destination fbps. This asks the only
                        // two questions left: WHICH BUFFER each sprite
                        // lands in, and WHICH ORDER. Full reading table at
                        // the counter definitions in ps2_gs_rasterizer.cpp.
                        {
                            std::ostringstream gList, bList;
                            uint32_t gTop = 0, bTop = 0;
                            uint32_t gTopFbp = 0xFFFFFFFFu, bTopFbp = 0xFFFFFFFFu;
                            uint32_t gFbpCount = 0, bFbpCount = 0;

                            const uint32_t disp1 = self->m_privRegs
                                                       ? static_cast<uint32_t>(self->m_privRegs->dispfb1 & 0x1FFull)
                                                       : 0xFFFFFFFFu;
                            const uint32_t disp2 = self->m_privRegs
                                                       ? static_cast<uint32_t>(self->m_privRegs->dispfb2 & 0x1FFull)
                                                       : 0xFFFFFFFFu;

                            // run 26 fix. The old verdict compared a single
                            // "top" fbp per sprite, and the two buckets are
                            // near-ties every record (154483 vs 159810 is a
                            // coin flip), so it reported SPLIT-TARGET and
                            // OFFSCREEN as tie-break noise. Split is a
                            // property of the SETS, not of an argmax: it is
                            // real only if no single fbp received both.
                            // Likewise "shown" is real only if NO glyph fbp
                            // is displayed. These two flags are argmax-free.
                            bool overlapFbp = false;
                            bool anyGlyphShown = false;

                            // Interval totals, kept so [boxover] below can be
                            // compared against them in the SAME record. The
                            // prediction it tests is quantitative, not just
                            // "nonzero": if the box lands on the text, bwhite
                            // should come out near gTotal.
                            uint64_t gTotal = 0, bTotal = 0;

                            for (uint32_t p = 0; p < 512u; ++p)
                            {
                                const uint32_t gc = g_fsGlyphByFbp[p].exchange(0, std::memory_order_relaxed);
                                const uint32_t bc = g_fsBoxByFbp[p].exchange(0, std::memory_order_relaxed);
                                if (gc != 0u)
                                {
                                    gList << " fbp" << std::dec << p << "=" << gc;
                                    ++gFbpCount;
                                    if (gc > gTop) { gTop = gc; gTopFbp = p; }
                                    if (p == disp1 || p == disp2)
                                        anyGlyphShown = true;
                                }
                                if (bc != 0u)
                                {
                                    bList << " fbp" << std::dec << p << "=" << bc;
                                    ++bFbpCount;
                                    if (bc > bTop) { bTop = bc; bTopFbp = p; }
                                }
                                if (gc != 0u && bc != 0u)
                                    overlapFbp = true;
                                gTotal += gc;
                                bTotal += bc;
                            }

                            const uint64_t gOrd = (gTopFbp != 0xFFFFFFFFu)
                                                      ? g_fsGlyphOrd[gTopFbp].load(std::memory_order_relaxed)
                                                      : 0ull;
                            const uint64_t bOrd = (bTopFbp != 0xFFFFFFFFu)
                                                      ? g_fsBoxOrd[bTopFbp].load(std::memory_order_relaxed)
                                                      : 0ull;

                            const bool haveG = (gTopFbp != 0xFFFFFFFFu);
                            const bool haveB = (bTopFbp != 0xFFFFFFFFu);

                            // The ordering branch is gone for good. gOrd/bOrd
                            // are the LAST stamp of each sprite across an
                            // interval that spans ~60 frames, so they cannot
                            // resolve within-frame order no matter how sound
                            // the counter is. They stay in the value line as
                            // raw numbers only. [boxover] answers order by
                            // sampling the destination under the box instead.
                            const char *fsVerdict =
                                (!haveG)          ? "GUARD-no-white-glyph-stores-dialog-not-up-read-a-later-record"
                              : (!haveB)          ? "GUARD-no-box-stores-box-gate-silent-this-interval"
                              : (!overlapFbp)     ? "SPLIT-TARGET-no-single-fbp-received-both-sprites"
                              : (!anyGlyphShown)  ? "OFFSCREEN-no-fbp-holding-text-is-pointed-at-by-dispfb"
                                                  : "SHARED-BUFFER-AND-DISPLAYED-order-is-decided-by-boxover";

                            RUNTIME_LOG("[fbsplit] glyph:" << (gFbpCount ? gList.str() : std::string(" none"))
                                        << " | box:" << (bFbpCount ? bList.str() : std::string(" none"))
                                        << " | verdict=" << fsVerdict);

                            RUNTIME_LOG("[fbsplit] gtopfbp=" << std::dec << static_cast<int64_t>(haveG ? static_cast<int64_t>(gTopFbp) : -1)
                                        << " btopfbp=" << static_cast<int64_t>(haveB ? static_cast<int64_t>(bTopFbp) : -1)
                                        << " disp1=" << static_cast<int64_t>(self->m_privRegs ? static_cast<int64_t>(disp1) : -1)
                                        << " disp2=" << static_cast<int64_t>(self->m_privRegs ? static_cast<int64_t>(disp2) : -1)
                                        << " | glyphord=" << gOrd
                                        << " boxord=" << bOrd
                                        << " ordtotal=" << g_fsOrdSeq.load(std::memory_order_relaxed)
                                        << " | dstor=0x" << std::hex
                                        << g_fsGlyphDstOr.exchange(0, std::memory_order_relaxed)
                                        << " boxpix=0x" << g_fsBoxPix.load(std::memory_order_relaxed)
                                        << std::dec);

                            // ---- [boxover] -- Stage 5.11 run 27 ----------
                            // Emitted inside the [fbsplit] scope on purpose:
                            // gtotal (this interval's white glyph stores) is
                            // the number bwhite has to be compared against,
                            // and reading it from a different record would
                            // compare two different intervals.
                            const uint64_t boSt = g_boStores.exchange(0, std::memory_order_relaxed);
                            const uint64_t boWh = g_boWhite.exchange(0, std::memory_order_relaxed);
                            const uint64_t boBl = g_boBlack.exchange(0, std::memory_order_relaxed);
                            const uint64_t boOt = g_boOther.exchange(0, std::memory_order_relaxed);

                            const bool boSane = (boWh + boBl + boOt) == boSt;

                            const char *boVerdict =
                                (boSt == 0ull) ? "GUARD-no-box-stores-this-interval-read-a-later-record"
                              : (!boSane)      ? "BROKEN-buckets-do-not-sum-to-bstores-ignore-every-other-field"
                              : (boWh != 0ull) ? "BOX-PAINTS-OVER-TEXT-text-drawn-first-then-covered"
                              : (boBl == boSt) ? "CLEARED-BETWEEN-box-lands-on-pure-black-everywhere-text-was-wiped"
                                               : "BOX-ON-NON-BLACK-BUT-NOT-TEXT-see-bdstor-for-what-is-underneath";

                            RUNTIME_LOG("[boxover] bstores=" << std::dec << boSt
                                        << " bwhite=" << boWh
                                        << " bblack=" << boBl
                                        << " bother=" << boOt
                                        << " gtotal=" << gTotal
                                        << " | verdict=" << boVerdict);

                            RUNTIME_LOG("[boxover] bdstor=0x" << std::hex
                                        << g_boDstOr.exchange(0, std::memory_order_relaxed)
                                        << " bdstwhite=0x" << g_boDstWhite.load(std::memory_order_relaxed)
                                        << " balpha=0x" << g_boAlphaReg.load(std::memory_order_relaxed)
                                        << std::dec
                                        << " babe=" << g_boAbe.load(std::memory_order_relaxed)
                                        << " bsrca=" << g_boSrcAMax.exchange(0, std::memory_order_relaxed)
                                        << " btotal=" << bTotal);

                            // ---- [fbaddr] -- Stage 5.11 run 28 -------------
                            // Emitted inside the [boxover] scope on purpose:
                            // gtotal/btotal above are the same populations
                            // these extents describe, so a reader can check
                            // gn/bn against them in one record instead of
                            // correlating two tags across an interval.
                            //
                            // Every read below uses the correct reset identity
                            // (OR->0, AND->all-ones, min->all-ones, max->0).
                            // Exchanging an AND or a min with 0 would pin it
                            // at 0 for the rest of the run and the probe would
                            // report "everything matches" forever.
                            const uint64_t faGN = g_faGN.exchange(0, std::memory_order_relaxed);
                            const uint64_t faBN = g_faBN.exchange(0, std::memory_order_relaxed);

                            const uint32_t gFbpO = g_faGFbpOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t gFbpA = g_faGFbpAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t bFbpO = g_faBFbpOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t bFbpA = g_faBFbpAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);

                            const uint32_t gFbwO = g_faGFbwOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t gFbwA = g_faGFbwAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t bFbwO = g_faBFbwOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t bFbwA = g_faBFbwAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);

                            const uint32_t gPsmO = g_faGPsmOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t gPsmA = g_faGPsmAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t bPsmO = g_faBPsmOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t bPsmA = g_faBPsmAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);

                            const uint32_t gMskO = g_faGMskOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t gMskA = g_faGMskAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t bMskO = g_faBMskOr.exchange(0, std::memory_order_relaxed);
                            const uint32_t bMskA = g_faBMskAnd.exchange(0xFFFFFFFFu, std::memory_order_relaxed);

                            const uint32_t gx0 = g_faGX0.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t gx1 = g_faGX1.exchange(0, std::memory_order_relaxed);
                            const uint32_t gy0 = g_faGY0.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t gy1 = g_faGY1.exchange(0, std::memory_order_relaxed);
                            const uint32_t bx0 = g_faBX0.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t bx1 = g_faBX1.exchange(0, std::memory_order_relaxed);
                            const uint32_t by0 = g_faBY0.exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                            const uint32_t by1 = g_faBY1.exchange(0, std::memory_order_relaxed);

                            // fbw and fpsm must not move within an interval;
                            // fbp is EXPECTED to alternate with the double
                            // buffer, so it is compared as an OR/AND pair
                            // between the sprites rather than required stable.
                            const bool fmtStable = (gFbwO == gFbwA) && (bFbwO == bFbwA) &&
                                                   (gPsmO == gPsmA) && (bPsmO == bPsmA);
                            const bool psmMatch = (gPsmO == bPsmO) && (gPsmA == bPsmA);
                            const bool fbwMatch = (gFbwO == bFbwO) && (gFbwA == bFbwA);
                            const bool fbpMatch = (gFbpO == bFbpO) && (gFbpA == bFbpA);
                            const bool mskMatch = (gMskO == bMskO) && (gMskA == bMskA);
                            const bool xyOverlap = (gx0 <= bx1) && (bx0 <= gx1) &&
                                                   (gy0 <= by1) && (by0 <= gy1);

                            const char *faVerdict =
                                (faGN == 0ull)  ? "GUARD-no-glyph-white-stores-dialog-not-up-read-a-later-record"
                              : (faBN == 0ull)  ? "GUARD-no-box-stores-box-gate-silent-this-interval"
                              : (!fmtStable)    ? "VARIES-fbw-or-psm-moved-mid-interval-no-comparison-is-valid"
                              : (!psmMatch)     ? "DIFFERENT-PSM-the-two-sprites-write-different-pixel-formats"
                              : (!fbwMatch)     ? "DIFFERENT-FBW-different-stride-so-the-addresses-diverge"
                              : (!fbpMatch)     ? "DIFFERENT-FBP-block-base-differs-the-0x1FF-mask-hid-it"
                              : (!xyOverlap)    ? "NO-SPATIAL-OVERLAP-the-rects-never-shared-a-pixel-premise-was-wrong"
                              : (!mskMatch)     ? "SAME-TARGET-BUT-FBMSK-DIFFERS-see-gmsk-and-bmsk"
                                                : "SAME-TARGET-AND-OVERLAPPING-so-memory-is-reset-between-the-passes";

                            RUNTIME_LOG("[fbaddr] gn=" << std::dec << faGN
                                        << " bn=" << faBN
                                        << " gpsm=0x" << std::hex << gPsmO << "/0x" << gPsmA
                                        << " bpsm=0x" << bPsmO << "/0x" << bPsmA
                                        << " gfbw=0x" << gFbwO << "/0x" << gFbwA
                                        << " bfbw=0x" << bFbwO << "/0x" << bFbwA
                                        << std::dec
                                        << " | verdict=" << faVerdict);

                            RUNTIME_LOG("[fbaddr] gfbp=0x" << std::hex << gFbpO << "/0x" << gFbpA
                                        << " bfbp=0x" << bFbpO << "/0x" << bFbpA
                                        << " gmsk=0x" << gMskO << "/0x" << gMskA
                                        << " bmsk=0x" << bMskO << "/0x" << bMskA
                                        << std::dec
                                        << " grect=" << gx0 << "," << gy0 << "-" << gx1 << "," << gy1
                                        << " brect=" << bx0 << "," << by0 << "-" << bx1 << "," << by1
                                        << " overlap=" << (xyOverlap ? 1 : 0));
                        }

                        // ---- [pixlog] -- Stage 5.11 run 29 ---------------
                        // ONE ADDRESS, EVERY WRITER, IN ORDER. Runs 24-28
                        // proved the two sprites share the buffer, overlap
                        // spatially, and that the glyphs store AFTER the box
                        // -- yet each sprite reads a destination the other
                        // never wrote. Only a writer outside both gates can
                        // do that, so this probe has no sprite gate at all.
                        //
                        // READ IN THIS ORDER:
                        //   1. the header line. n=0 or a GUARD verdict means
                        //      the dialog was not up -- read a later record.
                        //      pixel=X,Y is LATCHED from a real glyph store,
                        //      so it is a pixel the text genuinely covers.
                        //   2. the ring, printed OLDEST FIRST. who= is the
                        //      writer, val= the word stored, dst= what it
                        //      replaced. The LAST line decides the colour.
                        //   3. the verdict, which is derived from the ring --
                        //      shown= tells you how much it actually saw.
                        {
                            using namespace ps2diag_fbstat;

                            for (uint32_t p = 0; p < kPlPts; ++p)
                            {
                                const uint32_t plArmed = g_plArmed[p].exchange(0, std::memory_order_acquire);
                                const uint32_t plX = g_plX[p].exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                                const uint32_t plY = g_plY[p].exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                                const uint32_t plFbp = g_plFbp[p].exchange(0xFFFFFFFFu, std::memory_order_relaxed);
                                const uint64_t plN = g_plCount[p].exchange(0, std::memory_order_relaxed);
                                const uint64_t plG = g_plGlyphHits[p].exchange(0, std::memory_order_relaxed);
                                g_plClaim[p].store(0, std::memory_order_relaxed);

                                // Snapshot and clear in one pass so the next
                                // interval starts empty and ord==0 unambiguously
                                // means "slot never filled".
                                uint64_t rOrd[kPlRing];
                                uint32_t rTag[kPlRing], rVal[kPlRing], rDst[kPlRing], rPrim[kPlRing];
                                uint32_t rTexel[kPlRing], rTbp[kPlRing], rCbp[kPlRing];
                                uint32_t rTfmt[kPlRing], rAlpha[kPlRing];
                                for (uint32_t i = 0; i < kPlRing; ++i)
                                {
                                    rOrd[i] = g_plOrd[p][i].exchange(0, std::memory_order_acquire);
                                    rTag[i] = g_plTag[p][i].load(std::memory_order_relaxed);
                                    rVal[i] = g_plVal[p][i].load(std::memory_order_relaxed);
                                    rDst[i] = g_plDst[p][i].load(std::memory_order_relaxed);
                                    rPrim[i] = g_plPrim[p][i].load(std::memory_order_relaxed);
                                    rTexel[i] = g_plTexel[p][i].load(std::memory_order_relaxed);
                                    rTbp[i] = g_plTbp[p][i].load(std::memory_order_relaxed);
                                    rCbp[i] = g_plCbp[p][i].load(std::memory_order_relaxed);
                                    rTfmt[i] = g_plTfmt[p][i].load(std::memory_order_relaxed);
                                    rAlpha[i] = g_plAlpha[p][i].load(std::memory_order_relaxed);
                                }

                                if (plArmed == 0u || plN == 0ull)
                                {
                                    RUNTIME_LOG("[pixlog] pt=" << std::dec << p << " n=0 | verdict="
                                                "GUARD-not-latched-dialog-not-up-or-no-second-glyph-120px-to-the-right");
                                    continue;
                                }

                                const uint32_t start = static_cast<uint32_t>(plN % kPlRing);
                                uint32_t lastGlyph = 0xFFFFFFFFu;
                                uint32_t afterBox = 0, afterOther = 0, shown = 0;
                                uint32_t blkSeen = 0, blkTexel = 0, blkTbp = 0, blkCbp = 0;
                                uint32_t blkTfmt = 0, blkAlpha = 0, blkVal = 0;

                                RUNTIME_LOG("[pixlog] pt=" << std::dec << p
                                            << " pixel=" << plX << "," << plY
                                            << " fbp=" << plFbp
                                            << " n=" << plN
                                            << " glyphhits=" << plG
                                            << " ring=" << kPlRing);

                                for (uint32_t i = 0; i < kPlRing; ++i)
                                {
                                    const uint32_t s = (start + i) % kPlRing;
                                    if (rOrd[s] == 0ull)
                                        continue;
                                    ++shown;

                                    const char *who = (rTag[s] == 1u) ? "box"
                                                    : (rTag[s] == 2u) ? "glyph"
                                                                      : "other";

                                    RUNTIME_LOG("[pixlog] pt=" << std::dec << p
                                                << " ord=" << rOrd[s]
                                                << " who=" << who
                                                << " val=0x" << std::hex << rVal[s]
                                                << " dst=0x" << rDst[s]
                                                << " texel=0x" << rTexel[s]
                                                << " prim=0x" << rPrim[s]
                                                << " tbp0=0x" << rTbp[s]
                                                << " cbp=0x" << rCbp[s]
                                                << " tfmt=0x" << rTfmt[s]
                                                << " alpha=0x" << rAlpha[s]
                                                << std::dec);

                                    // The run-29 step-3 writer: untagged by both
                                    // sprite gates, but TEXTURED (prim bit 4),
                                    // which excludes the untextured frame clear.
                                    if (rTag[s] == 0u && (rPrim[s] & 0x10u) != 0u)
                                    {
                                        ++blkSeen;
                                        blkTexel = rTexel[s];
                                        blkTbp = rTbp[s];
                                        blkCbp = rCbp[s];
                                        blkTfmt = rTfmt[s];
                                        blkAlpha = rAlpha[s];
                                        blkVal = rVal[s];
                                    }

                                    if (rTag[s] == 2u)
                                    {
                                        lastGlyph = i;
                                        afterBox = 0;
                                        afterOther = 0;
                                    }
                                    else if (lastGlyph != 0xFFFFFFFFu)
                                    {
                                        if (rTag[s] == 1u)
                                            ++afterBox;
                                        else
                                            ++afterOther;
                                    }
                                }

                                const bool blkTexelBlack = (blkTexel & 0x00FFFFFFu) == 0u;
                                const bool blkStoredBlack = (blkVal & 0x00FFFFFFu) == 0u;

                                const char *plVerdict =
                                    (shown == 0u)              ? "GUARD-ring-empty-read-a-later-record"
                                  : (lastGlyph == 0xFFFFFFFFu) ? "NO-GLYPH-IN-RING-latched-pixel-rarely-covered-read-a-later-record"
                                  : (blkSeen == 0u)            ? "NO-TEXTURED-THIRD-PARTY-the-run-29-black-sprite-did-not-recur-at-this-pixel"
                                  : (!blkStoredBlack)          ? "THIRD-PARTY-IS-NOT-BLACK-HERE-see-blkval-it-differs-from-run-29"
                                  : (blkTexelBlack)            ? "FETCH-IS-DEGENERATE-third-party-texel-is-black-BEFORE-blending-see-blktbp0-and-blkcbp"
                                                               : "BLEND-DESTROYS-IT-third-party-texel-has-colour-but-stores-black-see-blkalpha";

                                RUNTIME_LOG("[pixlog] pt=" << std::dec << p
                                            << " shown=" << shown
                                            << " afterbox=" << afterBox
                                            << " afterother=" << afterOther
                                            << " blkseen=" << blkSeen
                                            << " blkval=0x" << std::hex << blkVal
                                            << " blktexel=0x" << blkTexel
                                            << " blktbp0=0x" << blkTbp
                                            << " blkcbp=0x" << blkCbp
                                            << " blktfmt=0x" << blkTfmt
                                            << " blkalpha=0x" << blkAlpha
                                            << std::dec
                                            << " | verdict=" << plVerdict);
                            }
                        }

                        // ---- [drawpath] -- Stage 5.11 run 32 -------------
                        // One frame's draw order, each entry stamped with the
                        // DMA path it arrived on. Answers the only inferred
                        // link left in the order-inversion case: which channel
                        // carries the background vs the box.
                        {
                            using namespace ps2diag_fbstat;
                            if (g_dpReady.exchange(0, std::memory_order_acq_rel) != 0u)
                            {
                                const uint32_t dpRaw = g_dpCount.load(std::memory_order_relaxed);
                                const uint32_t dpN = (dpRaw < kDpRing) ? dpRaw : kDpRing;
                                uint32_t dRole[kDpRing], dPath[kDpRing], dSite[kDpRing];
                                uint32_t dSX[kDpRing], dSY[kDpRing], dTbp[kDpRing];
                                uint32_t dSrc[kDpRing];
                                for (uint32_t i = 0; i < dpN; ++i)
                                {
                                    dRole[i] = g_dpRole[i].load(std::memory_order_relaxed);
                                    dPath[i] = g_dpPath[i].load(std::memory_order_relaxed);
                                    dSite[i] = g_dpSite[i].load(std::memory_order_relaxed);
                                    dSrc[i] = g_dpSrc[i].load(std::memory_order_relaxed);
                                    dSX[i] = g_dpSpanX[i].load(std::memory_order_relaxed);
                                    dSY[i] = g_dpSpanY[i].load(std::memory_order_relaxed);
                                    dTbp[i] = g_dpTbp[i].load(std::memory_order_relaxed);
                                }

                                // Where each role first lands in the frame, and
                                // which path carried it.
                                uint32_t boxAt = 0xFFFFFFFFu, bgAt = 0xFFFFFFFFu, glyAt = 0xFFFFFFFFu;
                                uint32_t boxPath = 0, bgPath = 0, glyPath = 0;
                                uint32_t boxSite = 0, bgSite = 0, glySite = 0;

                                // Run 34. The body is picked out by its shape,
                                // not by its position in the role-1 run: run 33
                                // showed the 464x120 body is the LAST of the
                                // nine role-1 draws, not the first, so boxAt
                                // names a border and is the wrong thing to
                                // compare an address against.
                                //
                                // Run 38 adds the same treatment to the
                                // background. bgSrc used to take the FIRST
                                // role-2 draw of the window, unshaped -- but
                                // dump.jsonl shows a second psm-0x13 tbw-4
                                // sprite at 256x256, so "role 2" alone does not
                                // name the 512x448 backdrop. bgFull* is the
                                // shape-gated reading; bgAny* is kept as its
                                // rival so a divergence between them convicts
                                // the gate instead of the runtime.
                                uint32_t bodyAt = 0xFFFFFFFFu;
                                uint32_t bgFullAt = 0xFFFFFFFFu;
                                uint32_t bodySrc = 0xFFFFFFFFu;
                                uint32_t bgFullSrc = 0xFFFFFFFFu, bgAnySrc = 0xFFFFFFFFu;
                                for (uint32_t i = 0; i < dpN; ++i)
                                {
                                    if (dRole[i] == 1u && dSX[i] == 464u && dSY[i] == 120u &&
                                        bodyAt == 0xFFFFFFFFu)
                                    {
                                        bodyAt = i;
                                        bodySrc = dSrc[i];
                                    }
                                    if (dRole[i] == 2u && dSX[i] >= 480u && dSY[i] >= 400u &&
                                        bgFullAt == 0xFFFFFFFFu)
                                    {
                                        bgFullAt = i;
                                        bgFullSrc = dSrc[i];
                                    }
                                    if (dRole[i] == 2u && bgAnySrc == 0xFFFFFFFFu)
                                        bgAnySrc = dSrc[i];
                                    if (dRole[i] == 1u && boxAt == 0xFFFFFFFFu)
                                    {
                                        boxAt = i;
                                        boxPath = dPath[i];
                                        boxSite = dSite[i];
                                    }
                                    if (dRole[i] == 2u && bgAt == 0xFFFFFFFFu)
                                    {
                                        bgAt = i;
                                        bgPath = dPath[i];
                                        bgSite = dSite[i];
                                    }
                                    if (dRole[i] == 3u && glyAt == 0xFFFFFFFFu)
                                    {
                                        glyAt = i;
                                        glyPath = dPath[i];
                                        glySite = dSite[i];
                                    }
                                }

                                // Run 38. The window now starts at a frame
                                // boundary, so index order IS submission order
                                // from the first sprite of the frame -- the
                                // comparison the run-32..34 gate could not make.
                                // Shape-gated indices decide it; the unshaped
                                // role-first indices stay in the log as the
                                // rival reading.
                                const char *dpVerdict =
                                    (dpN == 0u)
                                        ? "EMPTY-WINDOW-no-sprite-between-two-frame-boundaries-check-armclear-armcopy"
                                    : (bgFullAt == 0xFFFFFFFFu && bodyAt == 0xFFFFFFFFu)
                                        ? "NEITHER-IN-WINDOW-this-frame-has-no-dialog-or-kDpRing-is-too-small-see-n-and-cap"
                                    : (bgFullAt == 0xFFFFFFFFu)
                                        ? "NO-512x448-BACKGROUND-IN-WINDOW-see-bgany-the-role-2-draw-present-is-a-different-shape"
                                    : (bodyAt == 0xFFFFFFFFu)
                                        ? "NO-464x120-BODY-IN-WINDOW-the-dialog-is-not-in-this-frame-or-it-fell-past-the-ring"
                                    : (bgFullAt < bodyAt)
                                        ? "BACKGROUND-FIRST-matches-the-dump-so-submission-order-is-CORRECT-and-run34-was-a-gate-artifact"
                                    : (bgPath != boxPath)
                                        ? "BODY-FIRST-AND-PATHS-DIFFER-channel-drain-order-is-the-cause-see-boxpath-bgpath"
                                    : (bgSite != boxSite)
                                        ? "BODY-FIRST-AND-SUBMIT-SITES-DIFFER-our-vif1-layer-reorders-see-boxsite-bgsite"
                                        : "BODY-FIRST-SAME-PATH-SAME-SITE-so-the-guest-itself-submits-in-this-order";

                                RUNTIME_LOG("[drawpath] n=" << std::dec << dpN
                                            << " ring=" << kDpRing
                                            << " armclear=" << g_dpArmClear.load(std::memory_order_relaxed)
                                            << " armcopy=" << g_dpArmCopy.load(std::memory_order_relaxed)
                                            << " roles1=" << g_dpRoleSeen[1].load(std::memory_order_relaxed)
                                            << " roles2=" << g_dpRoleSeen[2].load(std::memory_order_relaxed)
                                            << " roles3=" << g_dpRoleSeen[3].load(std::memory_order_relaxed)
                                            << " roles9=" << g_dpRoleSeen[9].load(std::memory_order_relaxed));

                                // The per-entry listing is the expensive part
                                // (up to kDpRing lines a frame). Print it only
                                // for windows that actually contain the dialog
                                // body, and only for the first few of those --
                                // the summary line above still prints every
                                // time, so a capped listing can never look like
                                // "it never happened".
                                {
                                    static std::atomic<uint32_t> s_dpDumps{0};
                                    const bool wantDump = (bodyAt != 0xFFFFFFFFu) &&
                                                          (s_dpDumps.load(std::memory_order_relaxed) < 3u);
                                    if (wantDump)
                                    {
                                        const uint32_t d = s_dpDumps.fetch_add(1, std::memory_order_relaxed);
                                        for (uint32_t i = 0; i < dpN; ++i)
                                        {
                                            RUNTIME_LOG("[drawpath] dump=" << std::dec << d
                                                        << " i=" << i
                                                        << " role=" << dRole[i]
                                                        << " path=" << dPath[i]
                                                        << " site=" << dSite[i]
                                                        << " span=" << dSX[i] << "x" << dSY[i]
                                                        << " tbp0=0x" << std::hex << dTbp[i]
                                                        << " src=0x" << dSrc[i] << std::dec);
                                        }
                                    }
                                    else if (bodyAt != 0xFFFFFFFFu)
                                    {
                                        RUNTIME_LOG("[cap] drawpath per-entry listing capped at 3 dumps"
                                                    " -- absence of dump= lines below this point is the cap,"
                                                    " not an absence of dialog frames");
                                    }
                                }

                                // ---- [drawsrc] -- run 34, re-read run 38 ---
                                // CAVEAT, and it is load bearing: g_curSrc is
                                // dsBaseEE + offset, an address INSIDE the DMA
                                // transfer buffer being walked. It is monotonic
                                // only within one transfer. If two draws arrive
                                // on different transfers their addresses come
                                // from unrelated buffers and comparing them
                                // means nothing -- which is why run 34's
                                // "GUEST-CHAIN-IS-INVERTED" is withdrawn and
                                // the addresses below are corroboration only.
                                // Index order (dpVerdict above) is the finding.
                                // PCSX2 ground truth, dialog frame, buffer B
                                // (+0x64000): background link 0x7d57d0 precedes
                                // box body 0x7d5a00.
                                {
                                    const bool sameTransfer =
                                        (bodyAt != 0xFFFFFFFFu) && (bgFullAt != 0xFFFFFFFFu) &&
                                        (dSite[bodyAt] == dSite[bgFullAt]) &&
                                        (dPath[bodyAt] == dPath[bgFullAt]);
                                    const bool srcOk = (bodySrc != 0xFFFFFFFFu) &&
                                                       (bgFullSrc != 0xFFFFFFFFu);
                                    const char *srcVerdict =
                                        !srcOk
                                            ? "UNRESOLVED-one-or-both-draws-carry-no-source-address-so-the-lookup-is-broken-not-the-runtime"
                                        : !sameTransfer
                                            ? "NOT-COMPARABLE-the-two-draws-came-from-different-submit-sites-or-paths-so-their-buffer-offsets-are-unrelated-read-the-index-order-instead"
                                        : (bgFullSrc < bodySrc)
                                            ? "SAME-TRANSFER-background-offset-precedes-box-body-offset"
                                            : "SAME-TRANSFER-box-body-offset-precedes-background-offset";

                                    RUNTIME_LOG("[drawsrc] bodyat=" << std::dec
                                                << static_cast<int32_t>(bodyAt)
                                                << " bodysrc=0x" << std::hex << bodySrc
                                                << std::dec << " bgfullat=" << static_cast<int32_t>(bgFullAt)
                                                << " bgfullsrc=0x" << std::hex << bgFullSrc
                                                << " bganysrc=0x" << bgAnySrc << std::dec
                                                << " bgat=" << static_cast<int32_t>(bgAt)
                                                << " pcsx2bg=0x7d57d0 pcsx2body=0x7d5a00");
                                    RUNTIME_LOG("[drawsrc] verdict=" << srcVerdict);
                                }
                                RUNTIME_LOG("[drawpath] boxat=" << std::dec << static_cast<int32_t>(boxAt)
                                            << " boxpath=" << boxPath
                                            << " bgat=" << static_cast<int32_t>(bgAt)
                                            << " bgpath=" << bgPath
                                            << " glyphat=" << static_cast<int32_t>(glyAt)
                                            << " glyphpath=" << glyPath
                                            << " boxsite=" << boxSite
                                            << " bgsite=" << bgSite
                                            << " glyphsite=" << glySite
                                            << " | verdict=" << dpVerdict);

                                // ---- [chainord] -- run 33 ----------------
                                // The EE source addresses our VIF1 chain
                                // walk gathered for this same frame. PCSX2
                                // ground truth for the dialog frame is
                                // ascending, background (0x7717d0) before
                                // box body (0x771a00). Compare directly.
                                {
                                    using namespace ps2diag_chainord;
                                    const uint32_t coSeq =
                                        g_coSeq.load(std::memory_order_relaxed);
                                    // The ring holds 1024 for resolveSrc()'s
                                    // benefit; only the newest kCoShow are
                                    // worth printing, since the per-draw src=
                                    // fields are the primary reading now and
                                    // this list is only corroboration.
                                    constexpr uint32_t kCoShow = 64;
                                    const uint32_t coAvail = (coSeq < kCoRing) ? coSeq : kCoRing;
                                    const uint32_t coN = (coAvail < kCoShow) ? coAvail : kCoShow;
                                    const uint32_t coStart = (coSeq - coN) % kCoRing;

                                    uint32_t coA[kCoShow], coQ[kCoShow], coO[kCoShow];
                                    for (uint32_t j = 0; j < coN; ++j)
                                    {
                                        const uint32_t k = (coStart + j) % kCoRing;
                                        coA[j] = g_coAddr[k].load(std::memory_order_relaxed);
                                        coQ[j] = g_coQwc[k].load(std::memory_order_relaxed);
                                        coO[j] = g_coOff[k].load(std::memory_order_relaxed);
                                    }

                                    // Rival readings, so a broken probe convicts
                                    // itself instead of the runtime: a walk that
                                    // never moves shows distinct=1, and a window
                                    // that caught no gather at all shows n=0.
                                    uint32_t coAsc = 0, coDesc = 0, coDistinct = 0;
                                    for (uint32_t j = 0; j < coN; ++j)
                                    {
                                        bool seen = false;
                                        for (uint32_t p = 0; p < j; ++p)
                                            if (coA[p] == coA[j])
                                            {
                                                seen = true;
                                                break;
                                            }
                                        if (!seen)
                                            ++coDistinct;
                                        if (j > 0)
                                        {
                                            if (coA[j] > coA[j - 1])
                                                ++coAsc;
                                            else if (coA[j] < coA[j - 1])
                                                ++coDesc;
                                        }
                                    }

                                    // Run 33's ordering verdict is retired: it
                                    // counted a REF tag's jump to its texture
                                    // payload as a backwards step and cried
                                    // "not monotonic" over a chain that walks
                                    // strictly forward. Ordering is [drawsrc]'s
                                    // job now; this one only reports ring
                                    // health so a broken probe still convicts
                                    // itself.
                                    const char *coVerdict =
                                        (coN == 0)          ? "NO-CHAIN-CHUNKS-IN-WINDOW-this-frame-gathered-no-source-chain-so-vif1-was-fed-some-other-way"
                                      : (coDistinct <= 1u)  ? "DEGENERATE-EVERY-CHUNK-SAME-ADDRESS-the-probe-is-wrong-not-the-runtime"
                                                            : "RING-HEALTHY-see-drawsrc-for-the-ordering-verdict-asc-and-desc-are-raw-counts-and-ref-tags-jump-legitimately";

                                    RUNTIME_LOG("[chainord] n=" << std::dec << coN
                                                << " seq=" << coSeq
                                                << " distinct=" << coDistinct
                                                << " asc=" << coAsc
                                                << " desc=" << coDesc
                                                << " ring=" << kCoRing);
                                    for (uint32_t j = 0; j < coN; ++j)
                                    {
                                        RUNTIME_LOG("[chainord] j=" << std::dec << j
                                                    << " addr=0x" << std::hex << coA[j]
                                                    << std::dec << " qwc=" << coQ[j]
                                                    << " off=0x" << std::hex << coO[j]
                                                    << std::dec);
                                    }
                                    RUNTIME_LOG("[chainord] verdict=" << coVerdict);
                                }
                            }
                        }

                        // ---- [uvspan] -- Stage 5.11 run 23 ---------------
                        // Re-pointed at the glyph sprites (psm T4, tbw 4,
                        // 256x256) -- runs 21/22 gated on psm T8 AND tbp
                        // 0x2A80 and so measured nothing at all, reporting
                        // draws=0 on every single record.
                        //
                        // Reading table is at the counter definitions in
                        // ps2_gs_rasterizer.cpp; it is long and it is the
                        // point. Read the guard line below FIRST, in the
                        // order allspr -> t4any -> draws, and stop at the
                        // first zero. Only then read the values line.
                        // Scales: u/v and du /256, s/t/q and ds /65536,
                        // rawu/rawv unscaled 4.4.
                        {
                            const uint64_t uvAll  = g_uvAllSpr.load(std::memory_order_relaxed);
                            const uint64_t uvT4   = g_uvT4Any.load(std::memory_order_relaxed);
                            const uint64_t uvDrw  = g_uvDraws.load(std::memory_order_relaxed);

                            // Spell the verdict out so a zero cannot be read
                            // as "the renderer is fine". Absence of a value
                            // past a failed guard is NOT evidence.
                            const char *verdict =
                                (uvAll == 0)  ? "PROBE-DEAD-no-textured-sprite-reached-drawSprite"
                              : (uvT4  == 0)  ? "NO-T4-SPRITE-screen-absent-or-psm-decode-wrong"
                              : (uvDrw == 0)  ? "DIMS-WRONG-psm-ok-but-tbw-tw-th-missed-values-below-are-STALE"
                                              : "gate-matched-values-below-are-valid";

                            RUNTIME_LOG("[uvspan] allspr=" << std::dec << uvAll
                                        << " t4any=" << uvT4
                                        << " draws=" << uvDrw
                                        << " | fst0=" << g_uvFst0.load(std::memory_order_relaxed)
                                        << " fst1=" << g_uvFst1.load(std::memory_order_relaxed)
                                        << " qbad=" << g_uvQBad.load(std::memory_order_relaxed)
                                        << " flat=" << g_uvFlat.load(std::memory_order_relaxed)
                                        << " wide=" << g_uvWide.load(std::memory_order_relaxed)
                                        << " | du256=" << g_uvDuMin.load(std::memory_order_relaxed)
                                        << ".." << g_uvDuMax.load(std::memory_order_relaxed)
                                        << " ds64k=" << g_uvDsMin.load(std::memory_order_relaxed)
                                        << ".." << g_uvDsMax.load(std::memory_order_relaxed)
                                        << " | verdict=" << verdict);
                        }
                        RUNTIME_LOG("[uvspan] last"
                                    << " fst=" << std::dec
                                    << g_uvFst.load(std::memory_order_relaxed)
                                    << " | u256=" << g_uvU0.load(std::memory_order_relaxed)
                                    << ".." << g_uvU1.load(std::memory_order_relaxed)
                                    << " v256=" << g_uvV0.load(std::memory_order_relaxed)
                                    << ".." << g_uvV1.load(std::memory_order_relaxed)
                                    << " | s64k=" << g_uvS0.load(std::memory_order_relaxed)
                                    << ".." << g_uvS1.load(std::memory_order_relaxed)
                                    << " t64k=" << g_uvT0.load(std::memory_order_relaxed)
                                    << ".." << g_uvT1.load(std::memory_order_relaxed)
                                    << " q64k=" << g_uvQ0.load(std::memory_order_relaxed)
                                    << ".." << g_uvQ1.load(std::memory_order_relaxed)
                                    << " | rawu=" << g_uvRawU0.load(std::memory_order_relaxed)
                                    << ".." << g_uvRawU1.load(std::memory_order_relaxed)
                                    << " rawv=" << g_uvRawV0.load(std::memory_order_relaxed)
                                    << ".." << g_uvRawV1.load(std::memory_order_relaxed)
                                    << " | spanx=" << g_uvSpanX.load(std::memory_order_relaxed)
                                    << " spany=" << g_uvSpanY.load(std::memory_order_relaxed));
                    }
                }

                // ---- [boxtex] -- Stage 5.11 run 22 -----------------------
                // Reading table is at the counter definitions in
                // ps2_gs_rasterizer.cpp. Read in the numbered order given
                // there: all/draws, then census, then tex0, then index,
                // then palette, then texel. Every expected value came from
                // a PCSX2 GS dump of this same screen, decoded offline, so
                // a mismatch localises the bug without a second run.
                // No square brackets in any field: analyze_run.py truncates
                // a record at the first one.
                {
                    using namespace ps2diag_fbstat;

                    std::ostringstream cenLine;
                    for (int i = 0; i < 12; ++i)
                    {
                        const uint64_t key = g_bxCenKey[i].load(std::memory_order_relaxed);
                        if (key == 0)
                            continue;
                        cenLine << " tbp=0x" << std::hex
                                << ((key >> 40) & 0x3FFFull)
                                << ",cbp=0x" << ((key >> 16) & 0x3FFFull)
                                << ",psm=0x" << (key & 0x3Full)
                                << std::dec << ":"
                                << g_bxCenCnt[i].load(std::memory_order_relaxed);
                    }
                    if (cenLine.str().empty())
                        cenLine << " none";

                    std::ostringstream idxLine;
                    for (int i = 0; i < 16; ++i)
                    {
                        idxLine << (i ? "," : "")
                                << g_bxIdxHist[i].load(std::memory_order_relaxed);
                    }

                    const uint64_t t0 = g_bxTex0.load(std::memory_order_relaxed);
                    auto tfield = [&](int lo, int len) -> uint64_t {
                        return (t0 >> lo) & ((1ull << len) - 1ull);
                    };

                    RUNTIME_LOG("[boxtex] all=" << std::dec
                                << g_bxAll.load(std::memory_order_relaxed)
                                << " draws=" << g_bxDraws.load(std::memory_order_relaxed)
                                << " cenovf=" << g_bxCenOvf.load(std::memory_order_relaxed));
                    RUNTIME_LOG("[boxtex] census" << cenLine.str());
                    RUNTIME_LOG("[boxtex] tex0=0x" << std::hex << t0 << std::dec
                                << " tbp0=0x" << std::hex << tfield(0, 14)
                                << std::dec
                                << " tbw=" << tfield(14, 6)
                                << " psm=0x" << std::hex << tfield(20, 6) << std::dec
                                << " tw=" << (1u << tfield(26, 4))
                                << " th=" << (1u << tfield(30, 4))
                                << " tcc=" << tfield(34, 1)
                                << " tfx=" << tfield(35, 2)
                                << " cbp=0x" << std::hex << tfield(37, 14) << std::dec
                                << " cpsm=0x" << std::hex << tfield(51, 4) << std::dec
                                << " csm=" << tfield(55, 1)
                                << " csa=" << tfield(56, 5)
                                << " cld=" << tfield(61, 3));
                    RUNTIME_LOG("[boxtex] rect x0=" << std::dec
                                << g_bxX0.load(std::memory_order_relaxed)
                                << " y0=" << g_bxY0.load(std::memory_order_relaxed)
                                << " spanx=" << g_bxSpanX.load(std::memory_order_relaxed)
                                << " spany=" << g_bxSpanY.load(std::memory_order_relaxed));
                    RUNTIME_LOG("[boxtex] idxsamp=" << std::dec
                                << g_bxIdxSamp.load(std::memory_order_relaxed)
                                << " idxmin=" << g_bxIdxMin.load(std::memory_order_relaxed)
                                << " idxmax=" << g_bxIdxMax.load(std::memory_order_relaxed)
                                << " idxhist=" << idxLine.str());
                    RUNTIME_LOG("[boxtex] ent50=0x" << std::hex
                                << g_bxEnt50.load(std::memory_order_relaxed)
                                << " ent8=0x" << g_bxEnt8.load(std::memory_order_relaxed)
                                << " texel=0x" << g_bxTexel.load(std::memory_order_relaxed)
                                << std::dec);
                }

                // [zbuf]: the census showed every non-black word sitting in the
                // texture-upload pages (0x150+) and *nothing* in the two frame
                // pages, even though 3.9M coloured pixels were rasterized. The
                // remaining way for a colour write to vanish without moving is
                // the Z write two lines below it in writePixel:
                //     gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
                //     if (!zmask) gs->WriteVram(zpsm, zbp, fbw, x, y, z);
                // If ZBP aliases FBP, the depth store lands on the pixel we
                // just wrote. For 2D sprite work z is typically 0, which erases
                // it to black -- exactly the observed state. So print the
                // depth-buffer setup next to the frame setup and compare.
                // "raw" counts use the full 32 bits (alpha included) so a
                // non-zero Z value written into the frame page still shows up
                // even though its RGB bits read as black.
                {
                    auto rawPageHits = [&](uint32_t page) -> uint32_t {
                        if (page >= pageCount) return 0u;
                        const uint32_t *pw = words + (static_cast<size_t>(page) * (kPageBytes / 4u));
                        uint32_t hits = 0u;
                        for (uint32_t w = 0; w < kPageBytes / 4u; ++w)
                        {
                            if (pw[w] != 0u) { ++hits; }
                        }
                        return hits;
                    };

                    for (int c = 0; c < 2; ++c)
                    {
                        const auto &cc = self->m_registers.ctx[c];
                        RUNTIME_LOG("[zbuf] ctx=" << std::dec << c
                                                  << " frame.fbp=0x" << std::hex << static_cast<uint32_t>(cc.frame.fbp)
                                                  << " zbuf.zbp=0x" << static_cast<uint32_t>(cc.zbuf.zbp)
                                                  << " zbuf.psm=0x" << static_cast<uint32_t>(cc.zbuf.psm)
                                                  << std::dec
                                                  << " zmsk=" << (cc.zbuf.zmsk ? 1 : 0)
                                                  << " ztst=" << ((static_cast<uint64_t>(cc.test.data) >> 17) & 3ull)
                                                  << " alias=" << ((static_cast<uint32_t>(cc.zbuf.zbp) ==
                                                                    static_cast<uint32_t>(cc.frame.fbp)) ? 1 : 0));
                    }

                    // Raw (alpha-inclusive) occupancy of the pages that actually
                    // matter, so "0 non-black" can be told apart from "0 bytes".
                    RUNTIME_LOG("[zbuf] rawwords page0x0=" << std::dec << rawPageHits(0u)
                                                           << " page0x70=" << rawPageHits(0x70u)
                                                           << " page0x150=" << rawPageHits(0x150u));
                }

                // Top 8 pages, selection-sort style so no extra headers are
                // needed and the scan array is left untouched for clarity.
                for (int rank = 0; rank < 8; ++rank)
                {
                    uint32_t bestPage = 0u;
                    uint32_t bestHits = 0u;
                    for (uint32_t page = 0; page < pageCount; ++page)
                    {
                        if (pageNonBlack[page] > bestHits)
                        {
                            bestHits = pageNonBlack[page];
                            bestPage = page;
                        }
                    }
                    if (bestHits == 0u)
                    {
                        break;
                    }
                    RUNTIME_LOG("[vramcensus] rank=" << std::dec << rank
                                                     << " page=0x" << std::hex << bestPage
                                                     << std::dec << " nonblackwords=" << bestHits);
                    pageNonBlack[bestPage] = 0u;
                }
            }

            // [gs:frame]: aggregate replacement for the old sampled
            // [gs:pixels] probe. Counts cover the interval since the previous
            // report, so `nonblack=0` here really does mean nothing coloured
            // was rasterized -- unlike the sampled probe it cannot miss draws.
            const uint64_t px = self->m_statPixelsWritten.exchange(0, std::memory_order_relaxed);
            const uint64_t pxNonBlack = self->m_statPixelsNonBlack.exchange(0, std::memory_order_relaxed);
            const uint64_t pxTextured = self->m_statPixelsTextured.exchange(0, std::memory_order_relaxed);
            const uint32_t pxMaxRgb = self->m_statPixelMaxRgb.exchange(0, std::memory_order_relaxed);
            const uint32_t primMask = self->m_statPrimMask.exchange(0, std::memory_order_relaxed);

            RUNTIME_LOG("[gs:frame] px=" << std::dec << px
                                         << " nonblack=" << pxNonBlack
                                         << " textured=" << pxTextured
                                         << " maxrgb=" << pxMaxRgb
                                         << " primmask=0x" << std::hex << primMask
                                         << " imagebytes=" << std::dec
                                         << self->m_statImageBytes.load(std::memory_order_relaxed)
                                         << " prims=" << self->m_statPrims.load(std::memory_order_relaxed));

            // [vu:frame]: the VIF1 -> VU1 -> XGKICK geometry path, same
            // interval as [gs:frame] above. Unconditional counters -- unlike a
            // throttled probe, a zero here really is a zero. Read it as a
            // ladder: the first field that is 0 is where 3D geometry dies.
            const auto vu = ps2_pipeline_stats::drain();
            RUNTIME_LOG("[vu:frame] vif1calls=" << std::dec << vu.vif1Calls
                                                << " vif1bytes=" << vu.vif1Bytes
                                                << " mscal=" << vu.mscal
                                                << " mscnt=" << vu.mscnt
                                                << " vu1runs=" << vu.vu1Runs
                                                << " xgkicks=" << vu.xgkicks
                                                << " xgkickbytes=" << vu.xgkickBytes
                                                << " mpg=" << vu.mpgUploads
                                                << " mpgbytes=" << vu.mpgBytes
                                                << " vu1instrs=" << vu.vu1Instrs
                                                << " endebit=" << vu.endEbit
                                                << " endlimit=" << vu.endCycleLimit
                                                << " endrange=" << vu.endRange
                                                << " codenz=" << vu.codeNonzero
                                                << " datanz=" << vu.dataNonzero
                                                << " mscalpc=0x" << std::hex << vu.lastMscalPC
                                                << " vifops=" << vu.opHi << ":" << vu.opLo
                                                << std::dec);

            // [vu:vifbuf]: one real VIF1 buffer per interval, dumped verbatim.
            // badpos is the first byte offset carrying an opcode outside the
            // VIF ISA -- i.e. the point where the parse is provably desynced;
            // -1 means this buffer parsed cleanly. endpos < size means the loop
            // bailed early. Walk head= by hand from offset 0 and compare.
            ps2_pipeline_stats::Vif1Snap snap;
            if (ps2_pipeline_stats::takeVif1Snap(snap))
            {
                static const char kHexDigits[] = "0123456789abcdef";
                std::string dump;
                dump.reserve(static_cast<size_t>(snap.headBytes) * 3u);
                for (uint32_t i = 0; i < snap.headBytes; ++i)
                {
                    if (i != 0u && (i % 4u) == 0u)
                        dump.push_back(' ');
                    dump.push_back(kHexDigits[snap.head[i] >> 4]);
                    dump.push_back(kHexDigits[snap.head[i] & 0x0Fu]);
                }
                RUNTIME_LOG("[vu:vifbuf] size=" << std::dec << snap.sizeBytes
                                                << " endpos=" << snap.endPos
                                                << " cmds=" << snap.cmdCount
                                                << " bad=" << snap.badCount
                                                << " badpos="
                                                << static_cast<int64_t>(static_cast<int32_t>(snap.firstBadPos))
                                                << " badcmd=0x" << std::hex << snap.firstBadCmd
                                                << std::dec << " head=" << dump);
            }
        }
    };

    static std::atomic<uint64_t> s_latchCount{0};
    const uint64_t latchIndex = s_latchCount.fetch_add(1, std::memory_order_relaxed);
    PresentProbe presentProbe{this, ps2_diag::enabled() && ps2_diag::should_log(latchIndex, 4, 60)};

    if (!m_privRegs || !m_vram || m_vramSize == 0u)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    const GSPmodeState pmode = decodePmode(m_privRegs->pmode);
    const GSSmode2State smode2 = decodeSMode2(m_privRegs->smode2);
    const bool applyFieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = m_runtime && ((ps2_syscalls::GetCurrentVSyncTick(m_runtime) & 1ull) != 0ull);
    const GSFrameReg displayFrame1 = decodeDisplayFrame(m_privRegs->dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(m_privRegs->dispfb2);
    const GSDisplayReadOrigin displayOrigin1 = decodeDisplayReadOrigin(m_privRegs->dispfb1);
    const GSDisplayReadOrigin displayOrigin2 = decodeDisplayReadOrigin(m_privRegs->dispfb2);

    uint32_t width1 = 0u;
    uint32_t height1 = 0u;
    uint32_t width2 = 0u;
    uint32_t height2 = 0u;
    decodeDisplaySize(m_privRegs->display1, width1, height1);
    decodeDisplaySize(m_privRegs->display2, width2, height2);

    const bool validCrt1 = pmode.enableCrt1 && hasDisplaySetup(m_privRegs->display1, displayFrame1);
    const bool validCrt2 = pmode.enableCrt2 && hasDisplaySetup(m_privRegs->display2, displayFrame2);

    auto copyDisplaySource = [&](const GSFrameReg &displayFrame,
                                 const GSDisplayReadOrigin &displayOrigin,
                                 uint32_t width,
                                 uint32_t height,
                                 bool allowPreferred,
                                 bool preserveAlpha,
                                 GSFrameReg &selectedFrame,
                                 std::vector<uint8_t> &scratch,
                                 bool &usedPreferred) -> bool
    {
        selectedFrame = displayFrame;
        scratch.clear();
        usedPreferred = false;

        if (allowPreferred &&
            m_hasPreferredDisplaySource &&
            m_preferredDisplayDestFbp == displayFrame.fbp &&
            (m_preferredDisplaySourceFrame.fbw != 0u || m_preferredDisplaySourceFrame.fbp != displayFrame.fbp))
        {
            if (copyFrameToHostRgbaUnlocked(m_preferredDisplaySourceFrame,
                                            width,
                                            height,
                                            scratch,
                                            preserveAlpha,
                                            true,
                                            false,
                                            0u,
                                            0u))
            {
                selectedFrame = m_preferredDisplaySourceFrame;
                usedPreferred = true;
            }
        }

        if (scratch.empty() &&
            !copyFrameToHostRgbaUnlocked(displayFrame,
                                         width,
                                         height,
                                         scratch,
                                         preserveAlpha,
                                         true,
                                         true,
                                         displayOrigin.x,
                                         displayOrigin.y))
        {
            return false;
        }

        if (!usedPreferred && displayFrame.fbp == 0u && countNonBlackPixels(scratch, width, height) == 0u)
        {
            for (int contextIndex = 0; contextIndex < 2; ++contextIndex)
            {
                const GSFrameReg &candidate = m_registers.ctx[contextIndex].frame;
                if (candidate.fbp == selectedFrame.fbp &&
                    candidate.fbw == selectedFrame.fbw &&
                    candidate.psm == selectedFrame.psm)
                {
                    continue;
                }

                std::vector<uint8_t> candidatePixels;
                if (!copyFrameToHostRgbaUnlocked(candidate,
                                                 width,
                                                 height,
                                                 candidatePixels,
                                                 preserveAlpha,
                                                 true,
                                                 true,
                                                 0u,
                                                 0u))
                {
                    continue;
                }

                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                {
                    continue;
                }

                selectedFrame = candidate;
                scratch.swap(candidatePixels);
                break;
            }
        }

        return true;
    };

    if (!validCrt1 && !validCrt2)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (validCrt1 && validCrt2)
    {
        GSFrameReg selectedFrame1{};
        GSFrameReg selectedFrame2{};
        std::vector<uint8_t> rc1;
        std::vector<uint8_t> rc2;
        bool usedPreferred1 = false;
        bool usedPreferred2 = false;

        const bool copiedCrt1 = copyDisplaySource(displayFrame1, displayOrigin1, width1, height1, false, true, selectedFrame1, rc1, usedPreferred1);
        const bool copiedCrt2 = copyDisplaySource(displayFrame2, displayOrigin2, width2, height2, false, true, selectedFrame2, rc2, usedPreferred2);

        if (copiedCrt1 && copiedCrt2)
        {
            const uint32_t width = std::max(width1, width2);
            const uint32_t height = std::max(height1, height2);
            const uint8_t bgR = static_cast<uint8_t>(m_privRegs->bgcolor & 0xFFu);
            const uint8_t bgG = static_cast<uint8_t>((m_privRegs->bgcolor >> 8) & 0xFFu);
            const uint8_t bgB = static_cast<uint8_t>((m_privRegs->bgcolor >> 16) & 0xFFu);
            const uint8_t bgA = pmode.alp;

            std::vector<uint8_t> merged(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    dstRow[x * 4u + 0u] = bgR;
                    dstRow[x * 4u + 1u] = bgG;
                    dstRow[x * 4u + 2u] = bgB;
                    dstRow[x * 4u + 3u] = bgA;
                }
            }

            if (!pmode.slbg)
            {
                for (uint32_t y = 0; y < height2; ++y)
                {
                    const uint8_t *srcRow = rc2.data() + (y * kHostFrameWidth * 4u);
                    uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0; x < width2; ++x)
                    {
                        dstRow[x * 4u + 0u] = srcRow[x * 4u + 0u];
                        dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                        dstRow[x * 4u + 2u] = srcRow[x * 4u + 2u];
                        dstRow[x * 4u + 3u] = srcRow[x * 4u + 3u];
                    }
                }
            }

            for (uint32_t y = 0; y < height1; ++y)
            {
                const uint8_t *srcRow = rc1.data() + (y * kHostFrameWidth * 4u);
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t srcR = srcRow[x * 4u + 0u];
                    const uint8_t srcG = srcRow[x * 4u + 1u];
                    const uint8_t srcB = srcRow[x * 4u + 2u];
                    const uint8_t srcA = srcRow[x * 4u + 3u];
                    const uint8_t dstR = dstRow[x * 4u + 0u];
                    const uint8_t dstG = dstRow[x * 4u + 1u];
                    const uint8_t dstB = dstRow[x * 4u + 2u];
                    const uint8_t dstA = dstRow[x * 4u + 3u];
                    const uint32_t factor = pmode.mmod
                                                ? static_cast<uint32_t>(pmode.alp)
                                                : std::min<uint32_t>(255u, static_cast<uint32_t>(srcA) * 2u);

                    dstRow[x * 4u + 0u] = blendPresentationChannel(srcR, dstR, factor);
                    dstRow[x * 4u + 1u] = blendPresentationChannel(srcG, dstG, factor);
                    dstRow[x * 4u + 2u] = blendPresentationChannel(srcB, dstB, factor);
                    dstRow[x * 4u + 3u] = pmode.amod ? dstA : srcA;
                }
            }

            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *row = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    row[x * 4u + 3u] = 255u;
                }
            }

            if (applyFieldMode)
            {
                applyFieldPresentation(merged, width, height, oddField);
            }

            m_hostPresentationFrame.swap(merged);
            m_hostPresentationWidth = width;
            m_hostPresentationHeight = height;
            m_hostPresentationDisplayFbp = displayFrame1.fbp;
            m_hostPresentationSourceFbp = selectedFrame1.fbp;
            m_hostPresentationUsedPreferred = false;
            m_hasHostPresentationFrame = true;
            recordPresentDebugEventUnlocked(m_hostPresentationDisplayFbp,
                                            m_hostPresentationSourceFbp,
                                            m_hostPresentationWidth,
                                            m_hostPresentationHeight,
                                            m_hostPresentationUsedPreferred);
            return;
        }
    }

    const GSFrameReg &displayFrame = validCrt1 ? displayFrame1 : displayFrame2;
    const uint32_t width = validCrt1 ? width1 : width2;
    const uint32_t height = validCrt1 ? height1 : height2;

    GSFrameReg selectedFrame = displayFrame;
    std::vector<uint8_t> scratch;
    bool usedPreferred = false;
    const GSDisplayReadOrigin &displayOrigin = validCrt1 ? displayOrigin1 : displayOrigin2;
    if (!copyDisplaySource(displayFrame, displayOrigin, width, height, true, false, selectedFrame, scratch, usedPreferred))
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = displayFrame.fbp;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    if (applyFieldMode)
    {
        applyFieldPresentation(scratch, width, height, oddField);
    }

    normalizePresentationAlpha(scratch, width, height);

    m_hostPresentationFrame.swap(scratch);
    m_hostPresentationWidth = width;
    m_hostPresentationHeight = height;
    m_hostPresentationDisplayFbp = displayFrame.fbp;
    m_hostPresentationSourceFbp = selectedFrame.fbp;
    m_hostPresentationUsedPreferred = usedPreferred;
    m_hasHostPresentationFrame = true;
    recordPresentDebugEventUnlocked(m_hostPresentationDisplayFbp,
                                    m_hostPresentationSourceFbp,
                                    m_hostPresentationWidth,
                                    m_hostPresentationHeight,
                                    m_hostPresentationUsedPreferred);
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.resize(packedRowBytes * static_cast<size_t>(outHeight));
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    // Runs on every return path (RAII, declared after the lock so it still
    // holds it -- m_stateMutex is recursive). No-op unless PS2X_GSHISTORY_DUMP
    // / PS2X_GSHISTORY_FRAMES are set; see gsHistoryDrainAndMaybeFlush above.
    struct GsHistoryDrainGuard
    {
        GS &gs;
        ~GsHistoryDrainGuard() { gsHistoryDrainAndMaybeFlush(gs); }
    } historyDrainGuard{*this};

    if (!data || sizeBytes == 0 || !m_vram)
        return;

    // Drain any IMAGE payload still owed from a previous packet before this
    // buffer is interpreted as GIFtags -- the leading qwords are raw pixel
    // data, not tags.
    uint32_t offset = 0;
    if (m_pendingImageBytes != 0)
    {
        const uint32_t take = static_cast<uint32_t>(
            std::min<uint64_t>(m_pendingImageBytes, sizeBytes));
        // Decrement first: processImageData may complete the transfer and call
        // EndTransfer(), and the remaining declared qwords must still be
        // consumed as payload rather than re-parsed as GIFtags.
        m_pendingImageBytes -= take;
        processImageData(data, take);
        offset = take;
        if (offset >= sizeBytes)
            return;
    }

    if (offset == 0 && sizeBytes >= 16 && tryProcessNativeImageUploadPacket(data, sizeBytes))
        return;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u && offset + 16u <= sizeBytes)
        {
            const uint64_t tagLo = loadLE64(data + offset);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });


    while (offset + 16 <= sizeBytes)
    {
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_registers.rgbaq.q = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        if (ps2_diag::enabled())
        {
            static std::atomic<uint64_t> s_giftagCount{0};
            const uint64_t n = s_giftagCount.fetch_add(1, std::memory_order_relaxed);
            if (ps2_diag::should_log(n, 32, 5000))
            {
                const uint32_t eop = static_cast<uint32_t>((tagLo >> 15) & 1u);
                RUNTIME_LOG("[gs:giftag] n=" << n
                                             << " lo=0x" << std::hex << tagLo
                                             << " hi=0x" << tagHi
                                             << std::dec
                                             << " nloop=" << nloop
                                             << " eop=" << eop
                                             << " flg=" << static_cast<uint32_t>(flg)
                                             << " nreg=" << nreg
                                             << std::endl);
            }
        }

        recordGifTagDebugEventUnlocked(sizeBytes, nloop, flg, nreg);

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            writeRegister(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                        return;
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                        return;
                    writeRegister(regs[r], loadLE64(data + offset));
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            const uint64_t requested = static_cast<uint64_t>(nloop) * 16ull;
            const uint32_t available = sizeBytes - offset;
            const uint32_t imageBytes = static_cast<uint32_t>(
                std::min<uint64_t>(requested, available));

            // The rest of the payload arrives in following GIF packets. Record
            // the debt before dispatching so those packets are consumed as
            // pixel data instead of being misparsed as GIFtags.
            m_pendingImageBytes = requested - imageBytes;

            if (imageBytes != 0)
                processImageData(data + offset, imageBytes);
            offset += imageBytes;

            if (m_pendingImageBytes != 0)
                return;
        }
    }
}

bool GS::processNativePackedGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16u || !m_vram)
        return false;

    if (!validatePackedGifPacket(data, sizeBytes))
        return false;

    const bool processed = visitPackedGifPacket(data, sizeBytes, [&](const PackedGifPacketTag &tag)
    {
        m_registers.rgbaq.q = 1.0f;

        recordGifTagDebugEventUnlocked(sizeBytes, tag.nloop, GIF_FMT_PACKED, tag.nreg);

        const bool pre = ((tag.lo >> 46u) & 1u) != 0u;
        if (pre)
            writeRegister(GS_REG_PRIM, (tag.lo >> 47u) & 0x7FFu);

        uint32_t offset = tag.payloadOffset;
        for (uint32_t loop = 0u; loop < tag.nloop; ++loop)
        {
            for (uint32_t r = 0u; r < tag.nreg; ++r)
            {
                const uint64_t lo = loadLE64(data + offset);
                const uint64_t hi = loadLE64(data + offset + 8u);
                offset += 16u;
                writeRegisterPacked(tag.regs[r], lo, hi);
            }
        }

        return true;
    });

    if (!processed)
        return false;

    ++m_nativePackedGIFPacketCount;
    return true;
}

void GS::uploadImageNative(uint64_t bitbltbuf,
                           uint64_t trxpos,
                           uint64_t trxreg,
                           uint64_t trxdir,
                           const uint8_t *data,
                           uint32_t sizeBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes == 0 || !m_vram)
        return;

    writeRegister(GS_REG_BITBLTBUF, bitbltbuf);
    writeRegister(GS_REG_TRXPOS, trxpos);
    writeRegister(GS_REG_TRXREG, trxreg);
    writeRegister(GS_REG_TRXDIR, trxdir);
    processImageData(data, sizeBytes);
    ++m_nativeImageUploadCount;
}

bool GS::tryProcessNativeImageUploadPacket(const uint8_t *data, uint32_t sizeBytes)
{
    constexpr uint32_t kSetupRegisters = 4u;
    constexpr uint32_t kPackedAdPayloadBytes = kSetupRegisters * 16u;
    constexpr uint64_t kPackedAdDescriptor = 0x0Eull;

    if (!data || sizeBytes < 16u + kPackedAdPayloadBytes + 16u)
        return false;

    const uint64_t setupTagLo = loadLE64(data);
    const uint64_t setupTagHi = loadLE64(data + 8u);
    const uint32_t setupNloop = static_cast<uint32_t>(setupTagLo & 0x7FFFu);
    const uint8_t setupFlg = static_cast<uint8_t>((setupTagLo >> 58u) & 0x3u);
    uint32_t setupNreg = static_cast<uint32_t>((setupTagLo >> 60u) & 0xFu);
    if (setupNreg == 0u)
        setupNreg = 16u;

    if (setupNloop != kSetupRegisters ||
        setupFlg != GIF_FMT_PACKED ||
        setupNreg != 1u ||
        (setupTagHi & 0xFull) != kPackedAdDescriptor)
    {
        return false;
    }

    uint64_t regs[kSetupRegisters] = {};
    uint32_t offset = 16u;
    constexpr uint8_t expectedRegs[kSetupRegisters] = {
        GS_REG_BITBLTBUF,
        GS_REG_TRXPOS,
        GS_REG_TRXREG,
        GS_REG_TRXDIR,
    };

    for (uint32_t i = 0; i < kSetupRegisters; ++i)
    {
        regs[i] = loadLE64(data + offset);
        const uint64_t reg = loadLE64(data + offset + 8u);
        if ((reg & 0xFFu) != expectedRegs[i])
            return false;
        offset += 16u;
    }

    const uint32_t trxdirMode = static_cast<uint32_t>(regs[3] & 0x3ull);
    const uint32_t rrw = static_cast<uint32_t>(regs[2] & 0xFFFull);
    const uint32_t rrh = static_cast<uint32_t>((regs[2] >> 32u) & 0xFFFull);
    if (trxdirMode != 0u || rrw == 0u || rrh == 0u)
        return false;

    if (offset + 16u > sizeBytes)
        return false;

    const uint64_t imageTagLo = loadLE64(data + offset);
    const uint8_t imageFlg = static_cast<uint8_t>((imageTagLo >> 58u) & 0x3u);
    const uint32_t imageNloop = static_cast<uint32_t>(imageTagLo & 0x7FFFu);
    if (imageFlg != GIF_FMT_IMAGE || imageNloop == 0u)
        return false;

    offset += 16u;
    const uint64_t imageBytes64 = static_cast<uint64_t>(imageNloop) * 16ull;
    if (imageBytes64 > 0xFFFFFFFFull)
        return false;
    const uint32_t imageBytes = static_cast<uint32_t>(imageBytes64);
    if (offset + imageBytes != sizeBytes)
        return false;

    uploadImageNative(regs[0], regs[1], regs[2], regs[3], data + offset, imageBytes);
    return true;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    if (ps2_diag::enabled())
    {
        // [gs:packed] histogram: one line per descriptor every 200000 hits.
        static std::atomic<uint64_t> s_packedHist[256]{};
        const uint64_t n = s_packedHist[regDesc].fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n % 200000u) == 0u)
        {
            RUNTIME_LOG("[gs:packed] reg=0x" << std::hex << static_cast<uint32_t>(regDesc)
                                              << std::dec
                                              << " count=" << n);
        }
    }

    switch (regDesc)
    {
    case 0x00:
        writeRegister(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
    {
        GSRgbaqReg& rgbaq = m_registers.rgbaq;
        rgbaq.r = static_cast<uint8_t>(lo & 0xFF);
        rgbaq.g = static_cast<uint8_t>((lo >> 32) & 0xFF);
        rgbaq.b = static_cast<uint8_t>(hi & 0xFF);
        rgbaq.a = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    }
    case 0x02:
    {
        GSStReg& st = m_registers.st;
        GSRgbaqReg& rgbaq = m_registers.rgbaq;

        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);

        st.s = std::bit_cast<f32>(sBits);
        st.t = std::bit_cast<f32>(tBits);
        rgbaq.q = std::bit_cast<f32>(qBits);

        if (rgbaq.q == 0.0f)
            rgbaq.q = 1.0f;
        break;
    }
    case 0x03:
    {
        GSUvReg& uv = m_registers.uv;
        uv.u = static_cast<uint16_t>(lo & 0xFFFFu);
        uv.v = static_cast<uint16_t>((lo >> 32) & 0xFFFFu);
        break;
    }
    case 0x04:
    {
        const auto rgbaq = m_registers.rgbaq;
        const auto st = m_registers.st;
        const auto uv = m_registers.uv;

        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = rgbaq.r;
        vtx.g = rgbaq.g;
        vtx.b = rgbaq.b;
        vtx.a = rgbaq.a;
        vtx.q = rgbaq.q;
        vtx.s = st.s;
        vtx.t = st.t;
        vtx.u = uv.u;
        vtx.v = uv.v;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        const auto rgbaq = m_registers.rgbaq;
        const auto st = m_registers.st;
        const auto uv = m_registers.uv;
        const auto fog = m_registers.fog;

        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = rgbaq.r;
        vtx.g = rgbaq.g;
        vtx.b = rgbaq.b;
        vtx.a = rgbaq.a;
        vtx.q = rgbaq.q;
        vtx.s = st.s;
        vtx.t = st.t;
        vtx.u = uv.u;
        vtx.v = uv.v;
        vtx.fog = fog.f;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_registers.fog.f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        const auto rgbaq = m_registers.rgbaq;
        const auto st = m_registers.st;
        const auto uv = m_registers.uv;

        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = rgbaq.r;
        vtx.g = rgbaq.g;
        vtx.b = rgbaq.b;
        vtx.a = rgbaq.a;
        vtx.q = rgbaq.q;
        vtx.s = st.s;
        vtx.t = st.t;
        vtx.u = uv.u;
        vtx.v = uv.v;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        const auto rgbaq = m_registers.rgbaq;
        const auto st = m_registers.st;
        const auto uv = m_registers.uv;
        const auto fog = m_registers.fog;

        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = rgbaq.r;
        vtx.g = rgbaq.g;
        vtx.b = rgbaq.b;
        vtx.a = rgbaq.a;
        vtx.q = rgbaq.q;
        vtx.s = st.s;
        vtx.t = st.t;
        vtx.u = uv.u;
        vtx.v = uv.v;
        vtx.fog = fog.f;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);

        if (ps2_diag::enabled())
        {
            static std::atomic<uint64_t> s_adCount{0};
            const uint64_t n = s_adCount.fetch_add(1, std::memory_order_relaxed);
            if (ps2_diag::should_log(n, 32, 1000))
            {
                RUNTIME_LOG("[gs:ad] n=" << n
                                         << " addr=0x" << std::hex << static_cast<uint32_t>(addr)
                                         << " data=0x" << lo
                                         << std::dec
                                         << (isKnownGsRegister(addr) ? "" : " UNKNOWN"));
            }
        }

        writeRegister(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegister(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_registers.prim.data = value;
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_registers.rgbaq.data = value;

        if (m_registers.rgbaq.q == 0.0f)
        {
            m_registers.rgbaq.q = 1.0f;
        }
        break;
    }
    case GS_REG_ST:
    {
        m_registers.st.data = value;
        break;
    }
    case GS_REG_UV:
    {
        m_registers.uv.data = value;
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];

        const GSRgbaqReg rgbaq = m_registers.rgbaq;
        const GSStReg st = m_registers.st;
        const GSUvReg uv = m_registers.uv;

        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = rgbaq.r;
        vtx.g = rgbaq.g;
        vtx.b = rgbaq.b;
        vtx.a = rgbaq.a;
        vtx.q = rgbaq.q;
        vtx.s = st.s;
        vtx.t = st.t;
        vtx.u = uv.u;
        vtx.v = uv.v;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        const GSRgbaqReg rgbaq = m_registers.rgbaq;
        const GSStReg st = m_registers.st;
        const GSUvReg uv = m_registers.uv;
        const GSFogReg fog = m_registers.fog;

        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = rgbaq.r;
        vtx.g = rgbaq.g;
        vtx.b = rgbaq.b;
        vtx.a = rgbaq.a;
        vtx.q = rgbaq.q;
        vtx.s = st.s;
        vtx.t = st.t;
        vtx.u = uv.u;
        vtx.v = uv.v;
        vtx.fog = fog.f;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;

        m_registers.ctx[ci].tex0.data = value;

        GSTex0Reg tex0{ };
        tex0.data = value;

        ReloadClutCache(tex0.psm, tex0.cpsm, tex0.cbp, tex0.csm, tex0.csa, tex0.cld);

        // [clutlive] producer. Gated on the atlas SHAPE, never on an address:
        // a probe gated on an inferred constant does not fail loudly, it
        // reports confidently about the wrong sprite. psm=0x14 is PSMT4,
        // tbw=4, and tw/th are log2 so 8 == 256 pixels -- the exact signature
        // gsdump_diff.py matched on BOTH sides (dump frame 0 and live frame
        // 249), which is what makes it allocation-independent.
        //
        // PRIM.TME is deliberately absent from the gate: TME lives in PRIM,
        // not TEX0, so it is not observable at this site. It costs nothing --
        // a TEX0 write carrying a 256x256 PSMT4 tbw=4 texture IS a texture
        // setup by construction.
        //
        // Latched (store, not store-once) so the emit always describes the
        // most recent atlas setup rather than a stale first one; g_clHits is
        // the liveness guard that separates "gate never matched" from "gate
        // matched and the values are these".
        if (ps2_diag::enabled()
            && static_cast<uint32_t>(tex0.psm) == 0x14u
            && static_cast<uint32_t>(tex0.tbw) == 4u
            && static_cast<uint32_t>(tex0.tw) == 8u
            && static_cast<uint32_t>(tex0.th) == 8u)
        {
            using namespace ps2diag_fbstat;
            g_clHits.fetch_add(1, std::memory_order_relaxed);
            g_clTbp0.store(static_cast<uint32_t>(tex0.tbp0), std::memory_order_relaxed);
            g_clTbw.store(static_cast<uint32_t>(tex0.tbw), std::memory_order_relaxed);
            g_clCbp.store(static_cast<uint32_t>(tex0.cbp), std::memory_order_relaxed);
            g_clCpsm.store(static_cast<uint32_t>(tex0.cpsm), std::memory_order_relaxed);
            g_clCsm.store(static_cast<uint32_t>(tex0.csm), std::memory_order_relaxed);
            g_clCsa.store(static_cast<uint32_t>(tex0.csa), std::memory_order_relaxed);
            g_clCld.store(static_cast<uint32_t>(tex0.cld), std::memory_order_relaxed);

            // Round 2. ReloadClutCache has just run for THIS atlas setup, so
            // the buffer now holds precisely what this draw will sample. 16
            // reads at ~240 gate matches/sec is ~3.8k reads/sec -- noise.
            uint32_t nb = 0;
            for (uint32_t ce = 0; ce < 16u; ++ce)
            {
                const u32 cv = ReadClutCache(static_cast<u32>(tex0.cpsm),
                                             static_cast<u8>(ce),
                                             static_cast<u32>(tex0.csa));
                g_clSnap[ce].store(static_cast<uint32_t>(cv),
                                   std::memory_order_relaxed);
                if ((cv & 0x00FFFFFFu) != 0u)
                    ++nb;
            }

            if (nb >= 15u)
            {
                g_clAtwWhite.fetch_add(1, std::memory_order_relaxed);
            }
            else if (nb == 0u)
            {
                g_clAtwBlack.fetch_add(1, std::memory_order_relaxed);
                if (g_clBadSeen.exchange(1u, std::memory_order_relaxed) == 0u)
                {
                    g_clBadCbp.store(static_cast<uint32_t>(tex0.cbp),
                                     std::memory_order_relaxed);
                    for (uint32_t ce = 0; ce < 16u; ++ce)
                        g_clSnapBad[ce].store(
                            g_clSnap[ce].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
                }
            }
            else
            {
                g_clAtwOther.fetch_add(1, std::memory_order_relaxed);
            }

            const uint32_t cbKey = static_cast<uint32_t>(tex0.cbp) + 1u;
            for (uint32_t s = 0; s < 4u; ++s)
            {
                const uint32_t slot = g_clCbpVal[s].load(std::memory_order_relaxed);
                if (slot == 0u)
                    g_clCbpVal[s].store(cbKey, std::memory_order_relaxed);
                else if (slot != cbKey)
                    continue;
                g_clCbpHit[s].fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;

        m_registers.ctx[ci].clamp.data = value;
        break;
    }
    case GS_REG_FOG:
        m_registers.fog.data = value;
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_registers.ctx[ci].tex1.data = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;

        constexpr u64 mask = 0x0000001FFC0FFFFF;

        m_registers.ctx[ci].tex0.data = (m_registers.ctx[ci].tex0.data & mask) | (value & ~mask);
        m_registers.ctx[ci].tex2.data = value;

        const auto tex0 = m_registers.ctx[ci].tex0;

        ReloadClutCache(tex0.psm, tex0.cpsm, tex0.cbp, tex0.csm, tex0.csa, tex0.cld);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;

        m_registers.ctx[ci].xyoffset.data = value;
        break;
    }
    case GS_REG_PRMODECONT:
        m_registers.prmodecont.data = value;
        break;
    case GS_REG_PRMODE:
        m_registers.prmode.data = value;
        break;
    case GS_REG_TEXCLUT:
        m_registers.texclut.data = value;
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;

        m_registers.ctx[ci].scissor.data = value;
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;

        m_registers.ctx[ci].alpha.data = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;

        m_registers.ctx[ci].test.data = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;

        m_registers.ctx[ci].frame.data = value;
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;

        m_registers.ctx[ci].zbuf.data = value;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_registers.ctx[ci].fba.data = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_registers.bitbltbuf.data = value;
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_registers.trxpos.data = value;
        break;
    }
    case GS_REG_TRXREG:
    {
        m_registers.trxreg.data = value;
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_registers.trxdir.data = value;

        // We need the transfer state to survive the call to performLocalTo*Transfer
        // This is because transfers can be broken into multiple IMAGE tags and we
        // don't want to start all over again from the initial state
        // The transfer starts officially when TRXDIR is accessed
        m_transferState.x = m_registers.trxpos.dsax;
        m_transferState.y = m_registers.trxpos.dsay;
        m_transferState.total_pixels = m_registers.trxreg.rrw * m_registers.trxreg.rrh;
        m_transferState.copied_pixels = 0;
        m_pendingImageBytes = 0;

        const auto xdir = m_registers.trxdir.xdir;

        if (xdir == 2 && m_vram)
        {
            performLocalToLocalTransfer();
        }
        else if (xdir == 1 && m_vram)
        {
            performLocalToHostToBuffer();
        }
        recordTransferDebugEventUnlocked();
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_registers.pabe.data = value;
        break;
    case GS_REG_TEXFLUSH:
        m_registers.texflush.data = value;
        InvalidateTexturePageCache();
        break;
    case GS_REG_SCANMSK:
        m_registers.scanmsk.data = value;
        break;
    case GS_REG_FOGCOL:
        m_registers.fogcol.data = value;
        break;
    case GS_REG_DIMX:
    case GS_REG_DTHE:
        break;
    case GS_REG_COLCLAMP:
        m_registers.colclamp.data = value;
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
        break;
    case GS_REG_TEXA:
    {
        m_registers.texa.data = value;
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr.fetch_or(0x1);
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_privRegs)
            m_privRegs->csr.fetch_or(0x2);
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
        {
            // [dispfb] Stage 5.9 flashing probe. FRAME.FBP alternates 0x0/0x70 every
            // frame but [present] reports dispfb1==dispfb2==0x1070 constant, so half
            // the drawn frames are never scanned out. Two possible causes: the game
            // never writes DISPFB (so zero lines here is the answer), or it writes the
            // same value every time. Change-gated so a static register stays silent
            // after the first write.
            static uint64_t s_lastDispfb1 = ~0ull;
            static uint32_t s_dispfb1Emit = 0;
            if (value != s_lastDispfb1 && ps2_diag::enabled())
            {
                s_lastDispfb1 = value;
                if (s_dispfb1Emit < 64)
                {
                    ++s_dispfb1Emit;
                    RUNTIME_LOG("[dispfb] reg=1 raw=0x" << std::hex << value
                                                        << " fbp=0x" << (value & 0x1FFull)
                                                        << " fbw=" << std::dec << ((value >> 9) & 0x3Full)
                                                        << " psm=0x" << std::hex << ((value >> 15) & 0x1Full)
                                                        << std::dec
                                                        << " n=" << s_dispfb1Emit
                                                        << (s_dispfb1Emit == 64 ? " [cap]" : ""));
                }
            }
            m_privRegs->dispfb1 = value;
        }
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
        {
            // [dispfb] see reg=1 above. DISPFB2 is the one SDBZ most likely uses.
            static uint64_t s_lastDispfb2 = ~0ull;
            static uint32_t s_dispfb2Emit = 0;
            if (value != s_lastDispfb2 && ps2_diag::enabled())
            {
                s_lastDispfb2 = value;
                if (s_dispfb2Emit < 64)
                {
                    ++s_dispfb2Emit;
                    RUNTIME_LOG("[dispfb] reg=2 raw=0x" << std::hex << value
                                                        << " fbp=0x" << (value & 0x1FFull)
                                                        << " fbw=" << std::dec << ((value >> 9) & 0x3Full)
                                                        << " psm=0x" << std::hex << ((value >> 15) & 0x1Full)
                                                        << std::dec
                                                        << " n=" << s_dispfb2Emit
                                                        << (s_dispfb2Emit == 64 ? " [cap]" : ""));
                }
            }
            m_privRegs->dispfb2 = value;
        }
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }

    recordRegisterDebugEventUnlocked(regAddr, value);
}

void GS::performLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const GSBitBltBufReg bitbltbuf = m_registers.bitbltbuf;
    const GSTrxReg trxreg = m_registers.trxreg;
    const GSTrxPosReg trxpos = m_registers.trxpos;

    const u32 sbp = bitbltbuf.sbp;
    const u8 sbw = bitbltbuf.sbw;
    const u8 spsm = bitbltbuf.spsm;
    const u32 dbp = bitbltbuf.dbp;
    const u8 dbw = bitbltbuf.dbw;
    const u8 dpsm = bitbltbuf.dpsm;
    const u32 rrw = trxreg.rrw;
    const u32 rrh = trxreg.rrh;
    const u32 ssax = trxpos.ssax;
    const u32 ssay = trxpos.ssay;
    const u32 dsax = trxpos.dsax;
    const u32 dsay = trxpos.dsay;
    const u32 dir = trxpos.dir;

    const u32 total_pixels = rrw * rrh;

    if (total_pixels == 0)
    {
        EndTransfer();
        return;
    }

    if (ps2_diag::enabled())
    {
        static std::atomic<uint64_t> s_l2lCount{0};
        const uint64_t n = s_l2lCount.fetch_add(1, std::memory_order_relaxed);
        if (ps2_diag::should_log(n, 16, 600))
        {
            RUNTIME_LOG("[gs:l2l] n=" << n
                                      << " src.bp=0x" << std::hex << sbp
                                      << " src.psm=0x" << static_cast<uint32_t>(spsm)
                                      << " src.bw=" << std::dec << static_cast<uint32_t>(sbw)
                                      << " dst.bp=0x" << std::hex << dbp
                                      << " dst.psm=0x" << static_cast<uint32_t>(dpsm)
                                      << " dst.bw=" << std::dec << static_cast<uint32_t>(dbw)
                                      << " w=" << rrw << " h=" << rrh);
        }
    }

    // TODO: clean this up / optimize
    switch (dir)
    {
    case 0: // left -> right top -> bottom
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = pixel_count % rrw;
            const u32 y = pixel_count / rrw;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;


    // left -> right
    // bottom -> top (invert y)
    case 1:
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = pixel_count % rrw;
            const u32 y = rrh - (pixel_count / rrw) - 1;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;

    // right -> left (invert x)
    // top -> bottom
    case 2:
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = rrw - (pixel_count % rrw) - 1;
            const u32 y = pixel_count / rrw;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;

    // right to left (invert x)
    // bottom to top (invert y)
    case 3:
    {
        u32 pixel_count = 0;
        while (pixel_count < total_pixels)
        {
            const u32 x = rrw - (pixel_count % rrw) - 1;
            const u32 y = rrh - (pixel_count / rrw) - 1;

            const u32 sx = x + ssax;
            const u32 sy = y + ssay;
            const u32 dx = x + dsax;
            const u32 dy = y + dsay;

            WriteVram(dpsm, dbp, dbw, dx, dy, ReadVram(spsm, sbp, sbw, sx, sy));

            pixel_count++;
        }
    }
    break;

    default:
        break;
    }

    EndTransfer();
}

void GS::vertexKick(bool drawing)
{
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    if (!drawing)
        return;

    const GSPrimReg prim = m_registers.prim;

    int needed = 0;
    switch (prim.prim)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    if (ps2_diag::enabled())
    {
        // Draw-activity stats consumed by the [gs-activity] probe. Gated so
        // the draw-dispatch hot path pays nothing (no locked RMW) when
        // diagnostics are off; the reader on the run-loop thread accepts the
        // relaxed-atomic staleness.
        const GSContext &drawCtx = activeContext();
        m_statPrims.fetch_add(1, std::memory_order_relaxed);
        m_statLastDrawFbp.store(drawCtx.frame.fbp, std::memory_order_relaxed);

        // [gs:frame-change]: unthrottled — FBP changes are rare and are the
        // single most useful signal for spotting render-target thrash.
        static uint32_t s_prevDrawFbp = 0xFFFFFFFFu;
        if (drawCtx.frame.fbp != s_prevDrawFbp)
        {
            const int ctxIndex = m_registers.prim.ctxt ? 1 : 0;
            RUNTIME_LOG("[gs:frame-change] prim=" << static_cast<uint32_t>(prim.prim)
                                                   << " ctx=" << ctxIndex
                                                   << " frame.fbp=0x" << std::hex << drawCtx.frame.fbp
                                                   << " frame.fbw=" << std::dec << drawCtx.frame.fbw
                                                   << " frame.psm=0x" << std::hex << static_cast<uint32_t>(drawCtx.frame.psm)
                                                   << " tex0.tbp=0x" << drawCtx.tex0.tbp0
                                                   << " tex0.tbw=" << std::dec << static_cast<uint32_t>(drawCtx.tex0.tbw)
                                                   << " tex0.psm=0x" << std::hex << static_cast<uint32_t>(drawCtx.tex0.psm)
                                                   << std::dec
                                                   << " tme=" << static_cast<uint32_t>(m_registers.prim.tme ? 1u : 0u)
                                                   << " prmodecont=" << static_cast<uint32_t>(m_registers.prmodecont.ac ? 1u : 0u));
            s_prevDrawFbp = drawCtx.frame.fbp;
        }
    }

    m_rasterizer.drawPrimitive(this);
    recordDrawDebugEventUnlocked(needed);

    switch (prim.prim)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    if (ps2_diag::enabled())
    {
        // Image-transfer byte counter consumed by the [gs-activity] probe;
        // gated so the transfer path pays nothing when diagnostics are off.
        m_statImageBytes.fetch_add(sizeBytes, std::memory_order_relaxed);

        // [gs:image]: first-N verbose + every-600th, PLUS always when the
        // destination BITBLTBUF.DBP changes (this is the probe that exposed
        // an 8x over-transfer bug, so `sizeBytes` matters here).
        static std::atomic<uint64_t> s_imageCount{0};
        static uint32_t s_prevImageDbp = 0xFFFFFFFFu;
        const uint64_t n = s_imageCount.fetch_add(1, std::memory_order_relaxed);
        const bool dbpChanged = (m_registers.bitbltbuf.dbp != s_prevImageDbp);
        if (ps2_diag::should_log(n, 16, 600) || dbpChanged)
        {
            uint64_t nonZero = 0;
            if (data && sizeBytes > 0)
            {
                for (uint32_t i = 0; i < sizeBytes; ++i)
                    if (data[i] != 0)
                        ++nonZero;
            }
            RUNTIME_LOG("[gs:image] n=" << n
                                        << " dbp=0x" << std::hex << m_registers.bitbltbuf.dbp
                                        << " dpsm=0x" << static_cast<uint32_t>(m_registers.bitbltbuf.dpsm)
                                        << " dbw=" << std::dec << static_cast<uint32_t>(m_registers.bitbltbuf.dbw)
                                        << " trxreg=" << m_registers.trxreg.rrw << "x" << m_registers.trxreg.rrh
                                        << " sizeBytes=" << sizeBytes
                                        << " nonZero=" << nonZero
                                        << (dbpChanged ? " (DBP CHANGED)" : ""));
        }
        s_prevImageDbp = m_registers.bitbltbuf.dbp;
    }

    if (!m_vram)
    {
        return;
    }

    const auto bitbltbuf = m_registers.bitbltbuf;
    const auto trxreg = m_registers.trxreg;
    const auto trxpos = m_registers.trxpos;
    const auto trxdir = m_registers.trxdir;

    const u32 dbp = bitbltbuf.dbp;
    const u8 dbw = std::max<u8>(bitbltbuf.dbw, 1u);
    const u8 dpsm = bitbltbuf.dpsm;

    const u32 rrw = trxreg.rrw;
    const u32 rrh = trxreg.rrh;
    const u32 dsax = trxpos.dsax;
    const u32 dsay = trxpos.dsay;

    const auto xdir = trxdir.xdir;

    if (xdir != 0)
    {
        return;
    }

    if (rrw == 0 || rrh == 0)
    {
        return;
    }

    u32 data_offset = 0;

    // remove the format branching from the loops
    // TODO: fixup copypasta
    switch (dpsm)
    {
    case GS_PSM_CT32:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WritePixelCT32(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 4;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_Z32:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WritePixelZ32(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 4;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_CT24:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WritePixelCT24(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 3;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_Z24:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WritePixelZ24(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 3;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_CT16:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WritePixelCT16(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_Z16:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WritePixelZ16(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_CT16S:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WritePixelCT16S(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_Z16S:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WritePixelZ16S(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_T8:
        while (data_offset < sizeBytes)
        {
            u8 c = data[data_offset];

            GSMem::WritePixelP8(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;

    case GS_PSM_T8H:
        while (data_offset < sizeBytes)
        {
            u8 c = data[data_offset];

            GSMem::WritePixelP8H(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;
    case GS_PSM_T4:
    {
        // [psmt4] Stage 5.9 probe. PSMT4 uploads to dbp=0x2b60/0x2b80 are demonstrably
        // issued (434/440 of them, 32768 bytes each) yet the destination census reads
        // back all-zero. The census is swizzle-blind but NOT zero-blind, so it cannot
        // tell "never written" from "written all-zero nibbles". This logs the source
        // bytes before the loop and reads the same texels back after it, which
        // separates those two cases in a single run.
        static std::atomic<uint64_t> s_t4Count{0};
        const uint64_t t4n = s_t4Count.fetch_add(1, std::memory_order_relaxed);
        const bool t4log = ps2_diag::enabled() && ps2_diag::should_log(t4n, 8, 400);

        const u32 t4x0 = m_transferState.x;
        const u32 t4y0 = m_transferState.y;

        // Published by ps2_memory.cpp so the payload pointer can be resolved back to a
        // guest physical address. Block-scope extern: binds to the namespace-scope
        // object without touching any header.
        extern const uint8_t *g_ps2DiagRdramBase;
        extern uint32_t g_ps2DiagRdramSize;

        if (t4log)
        {
            static const char kHex[] = "0123456789abcdef";
            u32 srcNonZero = 0;
            for (u32 i = 0; i < sizeBytes; ++i)
            {
                if (data[i] != 0)
                {
                    ++srcNonZero;
                }
            }
            std::string head;
            for (u32 i = 0; i < 16 && i < sizeBytes; ++i)
            {
                head += kHex[(data[i] >> 4) & 0xF];
                head += kHex[data[i] & 0xF];
                head += ' ';
            }
            // Resolve the payload back to EE physical RAM, then widen the zero test to
            // the surrounding region. If only the payload window is zero the producer
            // wrote a blank glyph sheet; if the whole neighbourhood is zero the buffer
            // was never populated at all (asset load never happened).
            u32 t4ee = 0xFFFFFFFFu;
            u32 nearNonZero = 0;
            u32 nearFrom = 0;
            u32 nearBytes = 0;
            if (g_ps2DiagRdramBase && g_ps2DiagRdramSize != 0 &&
                data >= g_ps2DiagRdramBase &&
                data < g_ps2DiagRdramBase + g_ps2DiagRdramSize)
            {
                t4ee = static_cast<u32>(data - g_ps2DiagRdramBase);
                nearFrom = (t4ee > 0x10000u) ? (t4ee - 0x10000u) : 0u;
                u32 nearTo = t4ee + sizeBytes + 0x10000u;
                if (nearTo > g_ps2DiagRdramSize)
                    nearTo = g_ps2DiagRdramSize;
                nearBytes = nearTo - nearFrom;
                for (u32 i = nearFrom; i < nearTo; ++i)
                    if (g_ps2DiagRdramBase[i] != 0)
                        ++nearNonZero;
            }

            RUNTIME_LOG("[psmt4] pre n=" << std::dec << t4n
                                         << " eeAddr=0x" << std::hex << t4ee
                                         << " nearFrom=0x" << nearFrom
                                         << std::dec
                                         << " nearBytes=" << nearBytes
                                         << " nearNonZero=" << nearNonZero
                                         << std::hex
                                         << " dbp=0x" << dbp
                                         << " dbw=" << std::dec << static_cast<u32>(dbw)
                                         << " rrw=" << rrw << " rrh=" << rrh
                                         << " dsax=" << dsax << " dsay=" << dsay
                                         << " x=" << t4x0 << " y=" << t4y0
                                         << " copied=" << m_transferState.copied_pixels
                                         << " total=" << m_transferState.total_pixels
                                         << " sizeBytes=" << sizeBytes
                                         << " srcNonZero=" << srcNonZero
                                         << " head=" << head);
        }

        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WritePixelP4(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WritePixelP4(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }

        if (t4log)
        {
            static const char kHex[] = "0123456789abcdef";
            // Read back the first 16 texels of this transfer through the same
            // addressing helper the writes used. If these are zero while the
            // source bytes above were not, the fault is in WritePixelP4 /
            // LookupPixelAddressP4 rather than in the incoming data.
            std::string back;
            for (u32 i = 0; i < 16; ++i)
            {
                back += kHex[GSMem::ReadPixelP4(m_vram, dbp, dbw, t4x0 + i, t4y0) & 0xF];
            }
            const u32 addr0 = GSMem::LookupPixelAddressP4(dbp, dbw, t4x0, t4y0);
            RUNTIME_LOG("[psmt4] post n=" << std::dec << t4n
                                          << " addr0=0x" << std::hex << addr0
                                          << " vramBytes=" << std::dec << (m_vramSize)
                                          << " consumed=" << data_offset
                                          << " x=" << m_transferState.x
                                          << " y=" << m_transferState.y
                                          << " copied=" << m_transferState.copied_pixels
                                          << " total=" << m_transferState.total_pixels
                                          << " readback=" << back);
        }
        break;
    }
    case GS_PSM_T4HL:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WritePixelP4HL(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WritePixelP4HL(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;
    case GS_PSM_T4HH:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WritePixelP4HH(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WritePixelP4HH(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                EndTransfer();
                break;
            }
        }
        break;
    }
}

void GS::performLocalToHostToBuffer()
{
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;

    if (!m_vram)
        return;

    const auto bitbltbuf = m_registers.bitbltbuf;
    const auto trxreg = m_registers.trxreg;
    const auto trxpos = m_registers.trxpos;

    uint32_t sbp = bitbltbuf.sbp;
    uint8_t sbw = std::max<u8>(bitbltbuf.sbw, 1u);
    uint8_t spsm = bitbltbuf.spsm;
    uint32_t rrw = trxreg.rrw;
    uint32_t rrh = trxreg.rrh;
    uint32_t ssax = trxpos.ssax;
    uint32_t ssay = trxpos.ssay;

    u32 bpp = GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm));

    u32 pixel_total = rrw * rrh;
    u32 bytes_total = (pixel_total * bpp) / 8;

    m_localToHostBuffer.reserve(bytes_total);

    u32 pixel_count = 0;
    while (pixel_count < pixel_total)
    {
        const u32 x = pixel_count % rrw;
        const u32 y = pixel_count / rrw;

        const u32 v = ReadVram(spsm, sbp, sbw, x + ssax, y + ssay);

        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            m_localToHostBuffer.push_back((v >> 16) & 0xFF);
            m_localToHostBuffer.push_back((v >> 24) & 0xFF);
            break;
        case 24:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            m_localToHostBuffer.push_back((v >> 16) & 0xFF);
            break;
        case 16:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            break;
        case 8:
            m_localToHostBuffer.push_back(v);
            break;
        case 4:
        {
            const u32 v2 = ReadVram(spsm, sbp, sbw, x + ssax + 1, y + ssay);

            m_localToHostBuffer.push_back(v | ((v2 & 0xF) << 4));
            pixel_count++;
            break;
        }
        default:
            break;
        }

        pixel_count++;
    }
}

bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(this, m_registers.ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(this, activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!dst || maxBytes == 0)
        return 0;
    size_t avail = m_localToHostBuffer.size() - m_localToHostReadPos;
    if (avail == 0)
        return 0;
    size_t toCopy = (avail < maxBytes) ? avail : static_cast<size_t>(maxBytes);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, toCopy);
    m_localToHostReadPos += toCopy;
    return static_cast<uint32_t>(toCopy);
}

u32 GS::ReadTexturePageCache(u32 psm, u32 tbp0, u32 tbw, u32 u, u32 v)
{
    // we fill these as 32bit since they are aliases
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_T8H:
    case GS_PSM_T4HH:
    case GS_PSM_T4HL:
        psm = GS_PSM_CT32;
        break;
    case GS_PSM_Z32:
    case GS_PSM_Z24:
        psm = GS_PSM_Z32;
        break;
    default:
        break;
    }

    u32 page_width2;
    u32 page_height2;
    u32 bytes_per_pixel;
    u32 pitch;

    // TODO: add to templates
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_T8H:
    case GS_PSM_T4HH:
    case GS_PSM_T4HL:
    case GS_PSM_Z32:
    case GS_PSM_Z24:
        page_width2 = 6;
        page_height2 = 5;
        bytes_per_pixel = 4;
        pitch = 256;
        break;
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        page_width2 = 6;
        page_height2 = 6;
        bytes_per_pixel = 2;
        pitch = 128;
        break;
    case GS_PSM_T8:
        page_width2 = 7;
        page_height2 = 6;
        bytes_per_pixel = 1;
        pitch = 128;
        break;
    case GS_PSM_T4:
        page_width2 = 7;
        page_height2 = 7;
        bytes_per_pixel = 1; // 4 bit is a special case that is expanded
        pitch = 128;
        break;
    default:
        return 0;
    }

    const u32 page_width = 1u << page_width2;
    const u32 page_height = 1u << page_height2;

    // TODO: I can replace the log2 math with divides
    const u32 pages_per_row = std::max(1u, (tbw * 64u) >> page_width2);
    const u32 page_id = (v >> page_height2) * pages_per_row + (u >> page_width2);
    const u32 block_id = (tbp0 + page_id * 32u) & 0x3FFF;

    const bool needs_reload =
        !m_texture_page_cache.valid ||
        m_texture_page_cache.base_block != block_id ||
        m_texture_page_cache.psm != psm;

    if (needs_reload)
    {
        ReloadTexturePageCache(psm, block_id);
    }

    const u32 off = (v & (page_height - 1)) * pitch + (u & (page_width - 1)) * bytes_per_pixel;
    const u8* ptr = &m_texture_page_cache.buffer[off];

    switch (bytes_per_pixel)
    {
    case 4:
    {
        u32 v;
        memcpy(&v, ptr, 4);
        return v;
    }
    case 2:
    {
        u16 v;
        memcpy(&v, ptr, 2);
        return v;
    }
    break;
    default:
        break;
    }

    return static_cast<u32>(*ptr);
}

void GS::ReloadTexturePageCache(u32 psm, u32 base_block)
{
    u8* dst = m_texture_page_cache.buffer.data();

    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_T8H:
    case GS_PSM_T4HH:
    case GS_PSM_T4HL:
        GSMem::ReadPageToLinearBufferCT32(dst, 256, m_vram, base_block);
        break;
    case GS_PSM_Z32:
    case GS_PSM_Z24:
        GSMem::ReadPageToLinearBufferZ32(dst, 256, m_vram, base_block);
        break;
    case GS_PSM_CT16:
        GSMem::ReadPageToLinearBufferCT16(dst, 128, m_vram, base_block);
        break;
    case GS_PSM_CT16S:
        GSMem::ReadPageToLinearBufferCT16S(dst, 128, m_vram, base_block);
        break;
    case GS_PSM_Z16:
        GSMem::ReadPageToLinearBufferZ16(dst, 128, m_vram, base_block);
        break;
    case GS_PSM_Z16S:
        GSMem::ReadPageToLinearBufferZ16S(dst, 128, m_vram, base_block);
        break;
    case GS_PSM_T8:
        GSMem::ReadPageToLinearBufferP8(dst, 128, m_vram, base_block);
        break;
    case GS_PSM_T4:
        GSMem::ReadPageToLinearBufferP4(dst, 128, m_vram, base_block);
        break;
    default:
        return;
    }

    // we fill these as 32bit since they are aliases
    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    case GS_PSM_T8H:
    case GS_PSM_T4HH:
    case GS_PSM_T4HL:
        psm = GS_PSM_CT32;
        break;
    case GS_PSM_Z32:
    case GS_PSM_Z24:
        psm = GS_PSM_Z32;
        break;
    default:
        break;
    }

    m_texture_page_cache.base_block = base_block;
    m_texture_page_cache.psm = psm;
    m_texture_page_cache.valid = true;
}

void GS::InvalidateTexturePageCache()
{
    m_texture_page_cache.valid = false;
}

u32 GS::ReadClutCache(u32 psm, u8 index, u32 csa)
{

    switch (psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_CT24:
    {
        u32 v;
        memcpy(&v, &m_clut_cache[(csa *  16 * 4) + (index * 4)], 4);

        return v;
    }
    break;

    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    {
        u16 v;
        memcpy(&v, &m_clut_cache[(csa * 16 * 2) + (index * 2)], 2);

        return static_cast<u32>(v);
    }
    break;

    default:
        break;
    }

    return 0;
}

void GS::ReloadClutCacheCSM1(u32 psm, u32 cpsm, u32 cbp, u32 csa)
{
    const u32 cbpp = GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(cpsm));
    const u32 tbpp = GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(psm));

    const u32 clut_bytes_per_pixel = cbpp / 8;

    // offset into the clut buffer
    const u32 offset = csa * 16;

    // number of entries for this load
    const u32 entries = 1 << tbpp;

    // total entries in the cache
    const u32 total_cache_entries = 1024 / clut_bytes_per_pixel;

    // CSA moves the DESTINATION inside the CLUT buffer only. The source always
    // walks logical entries 0..entries-1 of the palette at CBP.
    //
    // The old code started the loop at `i = offset` and fed that same `i` to
    // both the VRAM scatter coordinate and the cache address, so at csa=c a
    // logical index resolved to palette entry index+16c and the first 16c
    // entries were unreachable. That is invisible at T4 (its scatter has
    // period 16, so a 16c shift is the identity) and visible at T8 (period
    // 256) -- see the ps2xTest case "CSM1 T8 at csa!=0 must not shift the
    // VRAM source coordinate".
    const u32 dst_capacity = (offset < total_cache_entries) ? (total_cache_entries - offset) : 0u;
    const u32 load_count = std::min(entries, dst_capacity);

    u32 cache_addr = offset * clut_bytes_per_pixel;
    switch (tbpp)
    {
    case 4:
        for (u32 i = 0; i < load_count; ++i)
        {
            u32 x = i & 7;
            u32 y = (i / 8) & 1;

            const u32 value = ReadVram(cpsm, cbp, 1, x, y);
            memcpy(&m_clut_cache[cache_addr & 0x3FF], &value, clut_bytes_per_pixel);

            cache_addr += clut_bytes_per_pixel;
        }
        break;

    case 8:
        for (u32 i = 0; i < load_count; ++i)
        {
            u32 x = i & 7;
            if (i & 0x10)
            {
                x += 8;
            }

            u32 y = (i & 0xE0) / 16;
            if (i & 0x8)
            {
                y++;
            }

            const u32 value = ReadVram(cpsm, cbp, 1, x, y);
            memcpy(&m_clut_cache[cache_addr & 0x3FF], &value, clut_bytes_per_pixel);

            cache_addr += clut_bytes_per_pixel;
        }
        break;

    default:
        break;
    }
}

void GS::ReloadClutCacheCSM2(u32 psm, u32 cbp)
{
    const auto texclut = m_registers.texclut;

    const u32 tbpp = GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(psm));
    const u32 entries = 1 << tbpp;

    const u32 uoffset = texclut.cou * 16;
    const u32 voffset = texclut.cov;

    const u32 cbw = texclut.cbw;

    for (usz i = 0; i < entries; ++i)
    {
        u32 x = (static_cast<u32>(uoffset) * 16) + i;

        u16 value = ReadVram(GS_PSM_CT16, cbp, cbw, x, texclut.cov);

        memcpy(&m_clut_cache[(i * 2) & 0x3FF], &value, 2);
    }
}

void GS::ReloadClutCache(u32 psm, u32 cpsm, u32 cbp, u8 csm, u8 csa, u8 cld)
{
    bool load = false;

    switch (cld)
    {
    case 0x1:
        load = true;
        break;
    case 0x2:
        load = true;
        m_cbp0 = cbp;
        break;
    case 0x3:
        load = true;
        m_cbp1 = cbp;
        break;
    case 0x4:
        if (cbp != m_cbp0)
        {
            load = true;
            m_cbp0 = cbp;
        }
        break;
    case 0x5:
        if (cbp != m_cbp1)
        {
            load = true;
            m_cbp1 = cbp;
        }
        break;
    default:
        break;
    }

    if (!load)
    {
        return;
    }

    switch (csm)
    {
    case 0:
        ReloadClutCacheCSM1(psm, cpsm, cbp, csa);
        break;
    case 1:
        ReloadClutCacheCSM2(psm, cbp);
        break;
    default:
        break;
    }
}
