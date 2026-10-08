// Multi-threaded GS rasterizer (perf 09-28). Included by ps2_gs_rasterizer.cpp
// only, right before GSRasterizer::drawPrimitive, so it can use that file's
// helpers (combineTexture, applyTexa, lerpChannel, ...). Not a header: no other
// file includes it.
//
// How it works
//   The GS thread (the "producer") no longer draws. drawPrimitive copies the
//   primitive and every register it needs into a Job and appends it to a ring.
//   N worker threads each read every job, in order, and draw only the rows
//   they own (row y belongs to worker y % N). A pixel is therefore always
//   written by the same worker, in primitive order, so the result is the same
//   as drawing on one thread.
//
//   Texture reads are the only pixels a worker reads that another worker may
//   write. The producer keeps the VRAM block ranges that queued jobs read
//   (textures) and write (frame, z) and waits for the workers (a barrier)
//   before a job would read what another queued job writes, or write what one
//   reads. A primitive that samples its own render target is drawn on the
//   producer after a barrier, in the same pixel order as the old code.
//
//   Every other GS operation that touches VRAM (image uploads, local->local
//   and local->host transfers, CLUT loads, the display latch) calls
//   ps2xGsRasterSyncRect / ps2xGsRasterFlush from ps2_gs_gpu.cpp first.
//
//   Each worker has its own texture page cache. A cached page is valid while
//   the generation of the VRAM pages it came from is unchanged; the producer
//   bumps a page's generation when an upload or a queued job writes it.
//
//   The worker drawing code below is the old single-thread code (drawSprite,
//   drawTriangle, drawLine, writePixel, sampleTexture) with the probes taken
//   out. Probes need the old path, so PS2X_DIAG=1 keeps it.
//
//   PS2X_GS_RASTER_THREADS=N  workers (default 4, 0 = old single-thread path)
namespace gsmt
{
constexpr uint32_t kVramBlocks = 0x4000u; // 4 MB / 256-byte blocks
constexpr uint32_t kVramPages = kVramBlocks / 32u;
constexpr uint32_t kMaxWorkers = 16u;
constexpr uint32_t kRingSize = 4096u; // power of two
constexpr uint32_t kClutSlots = 64u;
constexpr uint32_t kTexSlots = 16u;

struct Job
{
    GSVertex v[3];
    GSContext ctx;
    GSPrimReg prim;
    GSPabeReg pabe;
    GSColClampReg colclamp;
    GSTexaReg texa;
    uint64_t fogcol;
    uint64_t scanmsk;
    uint64_t dthe;
    uint64_t dimx;
    uint8_t *vram;
    const uint8_t *clut;
};

// Per-page generation: bumped by the producer before VRAM in that page changes
// under a cached texture. g_epoch is bumped when the whole VRAM may have
// changed behind the GS (new GS object, or the bench reseeding VRAM).
std::atomic<uint32_t> g_pageGen[kVramPages];
std::atomic<uint32_t> g_epoch{1};

inline bool isPaletted(uint32_t psm)
{
    return psm == GS_PSM_T8 || psm == GS_PSM_T8H || psm == GS_PSM_T4 ||
           psm == GS_PSM_T4HL || psm == GS_PSM_T4HH;
}

// ---- texture page cache (one per worker) ---------------------------------
// Same page decode and addressing as GS::ReadTexturePageCache.
struct TexCache
{
    struct Slot
    {
        uint32_t block = 0xFFFFFFFFu;
        uint32_t psm = 0;
        uint32_t gen0 = 0;
        uint32_t gen1 = 0;
        uint32_t epoch = 0;
        const uint8_t *vram = nullptr;
        alignas(64) uint8_t buf[16 * 1024];
    };
    Slot slot[kTexSlots];
    int mru = -1;
    uint32_t nextVictim = 0;

    bool valid(const Slot &s, const uint8_t *vram, uint32_t block, uint32_t psm, uint32_t epoch) const
    {
        return s.block == block && s.psm == psm && s.vram == vram && s.epoch == epoch &&
               s.gen0 == g_pageGen[(block >> 5) & (kVramPages - 1u)].load(std::memory_order_relaxed) &&
               s.gen1 == g_pageGen[((block + 31u) >> 5) & (kVramPages - 1u)].load(std::memory_order_relaxed);
    }

    __forceinline u32 read(uint8_t *vram, u32 psm, u32 tbp0, u32 tbw, u32 u, u32 v)
    {
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

        u32 pw2, ph2, bpp, pitch;
        switch (psm)
        {
        case GS_PSM_CT32:
        case GS_PSM_Z32:
            pw2 = 6; ph2 = 5; bpp = 4; pitch = 256;
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        case GS_PSM_Z16:
        case GS_PSM_Z16S:
            pw2 = 6; ph2 = 6; bpp = 2; pitch = 128;
            break;
        case GS_PSM_T8:
            pw2 = 7; ph2 = 6; bpp = 1; pitch = 128;
            break;
        case GS_PSM_T4:
            pw2 = 7; ph2 = 7; bpp = 1; pitch = 128;
            break;
        default:
            return 0;
        }

        const u32 pagesPerRow = std::max(1u, (tbw * 64u) >> pw2);
        const u32 pageId = (v >> ph2) * pagesPerRow + (u >> pw2);
        const u32 block = (tbp0 + pageId * 32u) & 0x3FFF;
        const uint32_t epoch = g_epoch.load(std::memory_order_relaxed);

        if (mru < 0 || !valid(slot[mru], vram, block, psm, epoch))
        {
            int hit = -1;
            for (uint32_t i = 0; i < kTexSlots; ++i)
            {
                if (valid(slot[i], vram, block, psm, epoch))
                {
                    hit = static_cast<int>(i);
                    break;
                }
            }
            if (hit < 0)
            {
                uint32_t victim = nextVictim++ % kTexSlots;
                if (static_cast<int>(victim) == mru)
                    victim = nextVictim++ % kTexSlots;
                Slot &s = slot[victim];
                // Generations first: a page written after this load then
                // reads as changed, never as current.
                s.gen0 = g_pageGen[(block >> 5) & (kVramPages - 1u)].load(std::memory_order_acquire);
                s.gen1 = g_pageGen[((block + 31u) >> 5) & (kVramPages - 1u)].load(std::memory_order_acquire);
                loadTexturePage(s.buf, vram, psm, block);
                s.block = block;
                s.psm = psm;
                s.vram = vram;
                s.epoch = epoch;
                hit = static_cast<int>(victim);
            }
            mru = hit;
        }

        const u32 off = (v & ((1u << ph2) - 1u)) * pitch + (u & ((1u << pw2) - 1u)) * bpp;
        const uint8_t *ptr = &slot[mru].buf[off];
        if (bpp == 4)
        {
            u32 val;
            std::memcpy(&val, ptr, 4);
            return val;
        }
        if (bpp == 2)
        {
            u16 val;
            std::memcpy(&val, ptr, 2);
            return val;
        }
        return static_cast<u32>(*ptr);
    }

    static void loadTexturePage(uint8_t *dst, const uint8_t *vram, u32 psm, u32 block)
    {
        u8 *src = const_cast<u8 *>(vram);
        switch (psm)
        {
        case GS_PSM_CT32:
            GSMem::ReadPageToLinearBufferCT32(dst, 256, src, block);
            break;
        case GS_PSM_Z32:
            GSMem::ReadPageToLinearBufferZ32(dst, 256, src, block);
            break;
        case GS_PSM_CT16:
            GSMem::ReadPageToLinearBufferCT16(dst, 128, src, block);
            break;
        case GS_PSM_CT16S:
            GSMem::ReadPageToLinearBufferCT16S(dst, 128, src, block);
            break;
        case GS_PSM_Z16:
            GSMem::ReadPageToLinearBufferZ16(dst, 128, src, block);
            break;
        case GS_PSM_Z16S:
            GSMem::ReadPageToLinearBufferZ16S(dst, 128, src, block);
            break;
        case GS_PSM_T8:
            GSMem::ReadPageToLinearBufferP8(dst, 128, src, block);
            break;
        case GS_PSM_T4:
            GSMem::ReadPageToLinearBufferP4(dst, 128, src, block);
            break;
        default:
            break;
        }
    }
};

// ---- per-job drawing state ------------------------------------------------
struct Setup
{
    const Job *job;
    uint8_t *vram;
    TexCache *cache;
    int rowN;   // workers
    int rowIdx; // this worker

    RasterReadFn frd, zrd;
    RasterWriteFn fwr, zwr;
    u32 fbp, fbw, fpsm, fmsk, zbp, zpsm;
    bool fb16, zmsk, abe, pabe, clamp, fbaOr, fge, dither;
    uint64_t test, fogcol, scanmsk, dimx;
    uint32_t ztst;
    int sx0, sx1, sy0, sy1;
    uint8_t asel, bsel, csel, dsel, afix;

    // texture
    GSTex0Reg tex;
    GSTexaReg texa;
    bool fst, linear;
    int texW, texH;

    void init(const Job &j, TexCache &c, int n, int idx)
    {
        job = &j;
        vram = j.vram;
        cache = &c;
        rowN = n;
        rowIdx = idx;
        const GSContext &ctx = j.ctx;
        fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        fbw = std::max<u32>(ctx.frame.fbw, 1u);
        fpsm = ctx.frame.psm;
        fmsk = ctx.frame.fbmsk;
        zbp = GSInternal::framePageBaseToBlock(ctx.zbuf.zbp);
        zpsm = ctx.zbuf.psm | 0x30;
        frd = g_rasterVram.read[fpsm & 0x3Fu];
        fwr = g_rasterVram.write[fpsm & 0x3Fu];
        zrd = g_rasterVram.read[zpsm & 0x3Fu];
        zwr = g_rasterVram.write[zpsm & 0x3Fu];
        fb16 = GSInternal::bitsPerPixel(static_cast<uint8_t>(fpsm)) == 16;
        zmsk = ctx.zbuf.zmsk;
        dither = (j.dthe & 1u) != 0u && fb16; // PCSX2: 16-bit frames only
        abe = j.prim.abe;
        pabe = j.pabe.pabe;
        clamp = j.colclamp.clamp;
        fbaOr = (ctx.fba.data & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24;
        test = ctx.test.data;
        fge = j.prim.fge;
        fogcol = j.fogcol;
        scanmsk = j.scanmsk;
        dimx = j.dimx;
        ztst = static_cast<uint32_t>((ctx.test.data >> 17) & 3);
        sx0 = static_cast<int>(ctx.scissor.x0);
        sx1 = static_cast<int>(ctx.scissor.x1);
        sy0 = static_cast<int>(ctx.scissor.y0);
        sy1 = static_cast<int>(ctx.scissor.y1);
        asel = ctx.alpha.a;
        bsel = ctx.alpha.b;
        csel = ctx.alpha.c;
        dsel = ctx.alpha.d;
        afix = ctx.alpha.fix;

        tex = ctx.tex0;
        texa = j.texa;
        fst = j.prim.fst;
        linear = tex1UsesLinearFilter(ctx.tex1.data);
        texW = 1 << tex.tw;
        texH = 1 << tex.th;
    }

    // First row >= y0 this worker owns (y0 >= 0).
    int firstRow(int y0) const
    {
        const int r = y0 % rowN;
        return y0 + ((rowIdx - r + rowN) % rowN);
    }

    bool ownsRow(int y) const
    {
        return y >= 0 && (y % rowN) == rowIdx;
    }
};

// GSRasterizer::writePixel without the probes.
__forceinline void writePixel(const Setup &S, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (x < S.sx0 || x > S.sx1 || y < S.sy0 || y > S.sy1)
        return;
    if (scanMasked(S.scanmsk, y))
        return;

    const AlphaTestResult alphaTest = classifyAlphaTest(S.test, a);
    if (!alphaTest.writeFramebuffer && !alphaTest.writeZ)
        return;

    uint8_t *vram = S.vram;
    const bool date = ((S.test >> 14) & 1u) != 0u;
    const bool frmw = (S.fmsk != 0) || S.abe || alphaTest.preserveDestinationAlpha || date;

    u32 fbrgba = 0;
    if (frmw)
    {
        fbrgba = S.frd(vram, S.fbp, S.fbw, x, y);
        if (S.fb16)
            fbrgba = Rgba5551ToRgba8888(fbrgba);
    }

    bool zpass = false;
    switch (S.ztst)
    {
    case 0:
        zpass = false;
        break;
    case 1:
        zpass = true;
        break;
    case 2:
        zpass = z >= S.zrd(vram, S.zbp, S.fbw, x, y);
        break;
    case 3:
        zpass = z > S.zrd(vram, S.zbp, S.fbw, x, y);
        break;
    }
    if (!zpass)
        return;
    if (date && failsDestAlphaTest(S.test, S.fpsm, fbrgba))
        return;

    int ir = r, ig = g, ib = b; // unclamped until finishColour (blend, then dither)
    if (S.abe)
    {
        uint8_t dr = fbrgba & 0xFF;
        uint8_t dg = (fbrgba >> 8) & 0xFF;
        uint8_t db = (fbrgba >> 16) & 0xFF;
        uint8_t da = (fbrgba >> 24) & 0xFF;

        if (!(S.pabe && (a & 0x80u) == 0u))
        {
            auto pickRGB = [](uint8_t sel, int cs, int cd) -> int
            {
                if (sel == 0)
                    return cs;
                if (sel == 1)
                    return cd;
                return 0;
            };
            // CT24 has no alpha: C=Ad multiplies by 1.0 (PCSX2 skips the modulate).
            int cAlpha = (S.csel == 0) ? a : (S.csel == 1) ? (S.fpsm == GS_PSM_CT24 ? 128 : da)
                                                             : S.afix;

            ir = ((pickRGB(S.asel, r, dr) - pickRGB(S.bsel, r, dr)) * cAlpha >> 7) + pickRGB(S.dsel, r, dr);
            ig = ((pickRGB(S.asel, g, dg) - pickRGB(S.bsel, g, dg)) * cAlpha >> 7) + pickRGB(S.dsel, g, dg);
            ib = ((pickRGB(S.asel, b, db) - pickRGB(S.bsel, b, db)) * cAlpha >> 7) + pickRGB(S.dsel, b, db);
        }
    }
    finishColour(S.dither, S.dimx, S.clamp, x, y, ir, ig, ib, r, g, b);

    if (!alphaTest.preserveDestinationAlpha && S.fbaOr)
        a = static_cast<uint8_t>(a | 0x80u);

    u32 pixel = pack32(r, g, b, a);
    if (S.fmsk != 0)
        pixel = (pixel & ~S.fmsk) | (fbrgba & S.fmsk);
    if (alphaTest.preserveDestinationAlpha)
        pixel = (pixel & 0x00FFFFFFu) | (fbrgba & 0xFF000000u);
    if (S.fb16)
        pixel = Rgba8888ToRgba5551(pixel);

    if (alphaTest.writeFramebuffer)
        S.fwr(vram, S.fbp, S.fbw, x, y, pixel);
    if (!S.zmsk && alphaTest.writeZ)
        S.zwr(vram, S.zbp, S.fbw, x, y, z);
}

// GSRasterizer::sampleTexture without the probes.
__forceinline uint32_t samplePoint(const Setup &S, int sampleU, int sampleV)
{
    const GSTex0Reg &tex = S.tex;
    // CLAMP_1/2 WMS/WMT (REPEAT / CLAMP / REGION_*), same as GSRasterizer::sampleTexture. This used to clamp
    // unconditionally, so every REPEAT texture sampled the edge texel instead of wrapping (10-06: orig3 Demo
    // fountain spray: bilinear neighbour u/v = -1 or 128 must wrap; threads=0 matched PCSX2, threads>=1 did not).
    const auto &wrapReg = S.job->ctx.clamp;
    sampleU = wrapTexCoord(sampleU, S.texW, static_cast<unsigned>(wrapReg.wms),
                           static_cast<int>(wrapReg.minu), static_cast<int>(wrapReg.maxu));
    sampleV = wrapTexCoord(sampleV, S.texH, static_cast<unsigned>(wrapReg.wmt),
                           static_cast<int>(wrapReg.minv), static_cast<int>(wrapReg.maxv));

    u32 out = S.cache->read(S.vram, tex.psm, tex.tbp0, tex.tbw, sampleU, sampleV);

    switch (tex.psm)
    {
    case GS_PSM_CT32:
    case GS_PSM_Z32:
    case GS_PSM_CT24:
    case GS_PSM_Z24:
        return applyTexa(S.texa, tex.psm, out);
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return applyTexa(S.texa, tex.psm, Rgba5551ToRgba8888(out));
    case GS_PSM_T8:
    case GS_PSM_T8H:
    case GS_PSM_T4:
    case GS_PSM_T4HL:
    case GS_PSM_T4HH:
    {
        // GS::ReadClutCache on the job's CLUT snapshot (offsets masked to
        // the 1 KB cache; the old code read past it for csa > 15 on T8).
        const u8 index = paletteIndex(tex.psm, out);
        const u32 csa = tex.csa;
        u32 clutVal = 0;
        switch (static_cast<u32>(tex.cpsm))
        {
        case GS_PSM_CT32:
        case GS_PSM_CT24:
            std::memcpy(&clutVal, &S.job->clut[((csa * 16 * 4) + (index * 4)) & 0x3FCu], 4);
            break;
        case GS_PSM_CT16:
        case GS_PSM_CT16S:
        {
            u16 v16;
            std::memcpy(&v16, &S.job->clut[((csa * 16 * 2) + (index * 2)) & 0x3FEu], 2);
            clutVal = v16;
            break;
        }
        default:
            break;
        }
        return clutEntryToRgba(S.texa, tex.cpsm, clutVal);
    }
    }
    return 0xFFFF00FFu;
}

uint32_t sampleTexel(const Setup &S, float texUf, float texVf);

// A ready 16.16 coordinate (TriTexInterp; -0x8000 already applied when linear).
__forceinline uint32_t sampleTexelFixed(const Setup &S, int fu, int fv)
{
    if (!S.linear)
        return samplePoint(S, fu >> 16, fv >> 16);
    return bilinearFilter(samplePoint(S, fu >> 16, fv >> 16), samplePoint(S, (fu >> 16) + 1, fv >> 16),
                          samplePoint(S, fu >> 16, (fv >> 16) + 1), samplePoint(S, (fu >> 16) + 1, (fv >> 16) + 1),
                          (fu & 0xFFFF) >> 12, (fv & 0xFFFF) >> 12);
}

__forceinline uint32_t sampleTexture(const Setup &S, float s, float t, float q, uint16_t u, uint16_t v)
{
    float texUf, texVf;
    if (S.fst)
    {
        texUf = static_cast<float>(u) / 16.0f;
        texVf = static_cast<float>(v) / 16.0f;
    }
    else
    {
        const float invQ = 1.0f / fabsQ(q);
        texUf = s * invQ * static_cast<float>(S.texW);
        texVf = t * invQ * static_cast<float>(S.texH);
    }

    return sampleTexel(S, texUf, texVf);
}

// Sample at a texel-space coordinate (sprites step U/V in texels directly).
__forceinline uint32_t sampleTexel(const Setup &S, float texUf, float texVf)
{
    if (!S.linear)
        return samplePoint(S, texFixed16(texUf) >> 16, texFixed16(texVf) >> 16); // PCSX2: 16.16 >> 16 (floor)

    // PCSX2 LTF fixed point (see bilinearTap in ps2_gs_rasterizer.cpp).
    const BilinearTap tap = bilinearTap(texUf, texVf);
    return bilinearFilter(samplePoint(S, tap.u0, tap.v0), samplePoint(S, tap.u0 + 1, tap.v0),
                          samplePoint(S, tap.u0, tap.v0 + 1), samplePoint(S, tap.u0 + 1, tap.v0 + 1),
                          tap.fu, tap.fv);
}

// GSRasterizer::drawSprite, drawing only this worker's rows. The display-copy
// detection it also did runs on the producer (submit()).
void drawSprite(const Setup &S)
{
    const Job &j = *S.job;
    const auto &prim = j.prim;
    const GSVertex &v0 = j.v[0];
    const GSVertex &v1 = j.v[1];
    const auto &ctx = j.ctx;

    u32 z1 = static_cast<u32>(v1.z);

    // PCSX2 ceil coverage (spriteCoverage in ps2_gs_rasterizer.cpp).
    int unclippedX0, unclippedY0, unclippedX1, unclippedY1;
    if (!spriteCoverage(v0.x, v0.y, v1.x, v1.y, ctx.xyoffset.ofx, ctx.xyoffset.ofy,
                        unclippedX0, unclippedY0, unclippedX1, unclippedY1))
        return;

    // SCISSOR fields are unsigned 64-bit bitfields: compare as int, or a sprite
    // whose left/top edge is negative converts to a huge value and is culled.
    if (unclippedX1 < static_cast<int>(ctx.scissor.x0) || unclippedX0 > static_cast<int>(ctx.scissor.x1) ||
        unclippedY1 < static_cast<int>(ctx.scissor.y0) || unclippedY0 > static_cast<int>(ctx.scissor.y1))
        return;

    const int drawX0 = clampInt(unclippedX0, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY0 = clampInt(unclippedY0, ctx.scissor.y0, ctx.scissor.y1);
    const int drawX1 = clampInt(unclippedX1, ctx.scissor.x0, ctx.scissor.x1);
    const int drawY1 = clampInt(unclippedY1, ctx.scissor.y0, ctx.scissor.y1);

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
            // Keep the 4 fraction bits: games send u0=0.5 so texel centres
            // land on pixels; truncating to whole texels shifted every sprite.
            u0f = static_cast<float>(v0.u) / 16.0f;
            v0f = static_cast<float>(v0.v) / 16.0f;
            u1f = static_cast<float>(v1.u) / 16.0f;
            v1f = static_cast<float>(v1.v) / 16.0f;
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

        // GS evaluates sprite attributes at INTEGER pixel coordinates, linearly
        // from v0's exact (sub-pixel) position to v1's, independent of vertex
        // order: U(x) = U0 + (x - X0) * (U1 - U0) / (X1 - X0).
        const float ofxF = static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
        const float ofyF = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
        const float vx0 = v0.x - ofxF;
        const float vy0 = v0.y - ofyF;
        const float spanXf = (v1.x - ofxF) - vx0;
        const float spanYf = (v1.y - ofyF) - vy0;
        const float duDx = (spanXf != 0.0f) ? (u1f - u0f) / spanXf : 0.0f;
        const float dvDy = (spanYf != 0.0f) ? (v1f - v0f) / spanYf : 0.0f;

        for (int y = S.firstRow(drawY0); y <= drawY1; y += S.rowN)
        {
            float texVf = v0f + (static_cast<float>(y) - vy0) * dvDy;

            for (int x = drawX0; x <= drawX1; ++x)
            {
                float texUf = u0f + (static_cast<float>(x) - vx0) * duDx;
                // FST and STQ alike: PCSX2 steps sprite U/V in 16.16 texels.
                // Rounding FST to 1/16 texel here broke scaled sprites
                // (gsfeature sprite_*_uv_scaled).
                const uint32_t texel = sampleTexel(S, texUf, texVf);

                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                uint8_t fr = color.r, fg = color.g, fb = color.b;
                if (S.fge) // PCSX2: a sprite's fog is v1's
                    applyFog(S.fogcol, v1.fog << 7, fr, fg, fb);
                writePixel(S, x, y, z1, fr, fg, fb, color.a);
            }
        }
    }
    else
    {
        if (S.fge)
            applyFog(S.fogcol, v1.fog << 7, r, g, b);
        for (int y = S.firstRow(drawY0); y <= drawY1; y += S.rowN)
            for (int x = drawX0; x <= drawX1; ++x)
                writePixel(S, x, y, z1, r, g, b, a);
    }
}

// GSRasterizer::drawTriangle, drawing only this worker's rows.
void drawTriangle(const Setup &S)
{
    const Job &j = *S.job;
    const auto &prim = j.prim;
    const GSVertex &v0 = j.v[0];
    const GSVertex &v1 = j.v[1];
    const GSVertex &v2 = j.v[2];
    const auto &ctx = j.ctx;

    // GS samples pixels at INTEGER coordinates (not x+0.5) with a top-left fill
    // rule: a pixel is in if ceil(left) <= x < ceil(right), same for y. Done in
    // the GS's own 1/16-pixel fixed point so ties (vertex exactly on a pixel)
    // resolve exactly; XYOFFSET keeps its fraction bits.
    const int64_t ofx16 = static_cast<int64_t>(ctx.xyoffset.ofx);
    const int64_t ofy16 = static_cast<int64_t>(ctx.xyoffset.ofy);
    const int64_t X[3] = {static_cast<int64_t>(std::lround(v0.x * 16.0f)) - ofx16,
                          static_cast<int64_t>(std::lround(v1.x * 16.0f)) - ofx16,
                          static_cast<int64_t>(std::lround(v2.x * 16.0f)) - ofx16};
    const int64_t Y[3] = {static_cast<int64_t>(std::lround(v0.y * 16.0f)) - ofy16,
                          static_cast<int64_t>(std::lround(v1.y * 16.0f)) - ofy16,
                          static_cast<int64_t>(std::lround(v2.y * 16.0f)) - ofy16};

    // Twice the signed area; orient so the inside of every edge is positive.
    int64_t area2 = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    if (area2 == 0)
        return;
    const int64_t sgn = (area2 < 0) ? -1 : 1;
    area2 *= sgn;

    // Edge i is opposite vertex i: A = vertex (i+1)%3, B = vertex (i+2)%3.
    // E_i(P) = sgn * ((Bx-Ax)(Py-Ay) - (By-Ay)(Px-Ax)); E_i(vertex i) = area2.
    int64_t edx[3], edy[3], bias[3];
    for (int i = 0; i < 3; ++i)
    {
        const int a = (i + 1) % 3, b = (i + 2) % 3;
        edx[i] = sgn * (X[b] - X[a]);
        edy[i] = sgn * (Y[b] - Y[a]);
        // Left edge (inside to its right: edy < 0) or top edge (horizontal,
        // inside below: edx > 0) includes pixels exactly on it; others don't.
        const bool topLeft = (edy[i] < 0) || (edy[i] == 0 && edx[i] > 0);
        bias[i] = topLeft ? 0 : -1;
    }

    auto ceil16 = [](int64_t v) -> int { return static_cast<int>((v >= 0) ? (v + 15) / 16 : -((-v) / 16)); };
    int minX = ceil16(std::min({X[0], X[1], X[2]}));
    int maxX = ceil16(std::max({X[0], X[1], X[2]})) - 1;
    int minY = ceil16(std::min({Y[0], Y[1], Y[2]}));
    int maxY = ceil16(std::max({Y[0], Y[1], Y[2]})) - 1;

    minX = std::max(minX, static_cast<int>(ctx.scissor.x0));
    maxX = std::min(maxX, static_cast<int>(ctx.scissor.x1));
    minY = std::max(minY, static_cast<int>(ctx.scissor.y0));
    maxY = std::min(maxY, static_cast<int>(ctx.scissor.y1));
    if (minX > maxX || minY > maxY)
        return;

    const float invArea = 1.0f / static_cast<float>(area2);

    // PCSX2-exact texture coordinates (TriTexInterp in ps2_gs_rasterizer.cpp).
    TriTexInterp triTex;
    const bool triTexOn = prim.tme &&
                          triTex.setup(v0, v1, v2, static_cast<float>(ctx.xyoffset.ofx) / 16.0f,
                                       static_cast<float>(ctx.xyoffset.ofy) / 16.0f, prim.fst != 0,
                                       static_cast<int>(ctx.tex0.tw), static_cast<int>(ctx.tex0.th), S.linear);
    const auto &tex = ctx.tex0;

    for (int y = S.firstRow(minY); y <= maxY; y += S.rowN)
    {
        const int64_t py = static_cast<int64_t>(y) * 16;
        const int64_t px0 = static_cast<int64_t>(minX) * 16;
        bool rowStarted = false;
        int64_t e[3];
        for (int i = 0; i < 3; ++i)
        {
            const int a = (i + 1) % 3;
            e[i] = edx[i] * (py - Y[a]) - edy[i] * (px0 - X[a]);
        }
        for (int x = minX; x <= maxX; ++x, e[0] -= edy[0] * 16, e[1] -= edy[1] * 16, e[2] -= edy[2] * 16)
        {
            if (e[0] + bias[0] < 0 || e[1] + bias[1] < 0 || e[2] + bias[2] < 0)
                continue;

            const float w0 = static_cast<float>(e[0]) * invArea;
            const float w1 = static_cast<float>(e[1]) * invArea;
            const float w2 = 1.0f - w0 - w1;

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
                    is = v0.s * w0 + v1.s * w1 + v2.s * w2;
                    it = v0.t * w0 + v1.t * w1 + v2.t * w2;
                    iq = v0.q * w0 + v1.q * w1 + v2.q * w2;
                    iu = 0;
                    iv = 0;
                }

                uint32_t texel;
                if (triTexOn)
                {
                    if (!rowStarted)
                    {
                        triTex.beginRow(y, x);
                        rowStarted = true;
                    }
                    int fu, fv;
                    triTex.at(x, fu, fv);
                    texel = sampleTexelFixed(S, fu, fv);
                }
                else
                    texel = sampleTexture(S, is, it, iq, iu, iv);
                uint8_t tr = static_cast<uint8_t>(texel & 0xFF);
                uint8_t tg = static_cast<uint8_t>((texel >> 8) & 0xFF);
                uint8_t tb = static_cast<uint8_t>((texel >> 16) & 0xFF);
                uint8_t ta = static_cast<uint8_t>((texel >> 24) & 0xFF);

                const TextureCombineResult color = combineTexture(tex, r, g, b, a, tr, tg, tb, ta);
                r = color.r;
                g = color.g;
                b = color.b;
                a = color.a;
            }

            if (S.fge)
                applyFog(S.fogcol, fogF16(v0.fog * w0 + v1.fog * w1 + v2.fog * w2), r, g, b);
            writePixel(S, x, y, static_cast<u32>(z + 0.5), r, g, b, a);
        }
    }
}

// GSRasterizer::drawLine; every worker walks the line, each writes its rows.
void drawLine(const Setup &S)
{
    const Job &j = *S.job;
    const auto &prim = j.prim;
    const GSVertex &v0 = j.v[0];
    const GSVertex &v1 = j.v[1];
    const auto &ctx = j.ctx;

    // PCSX2 line walk (walkLinePcsx2 in ps2_gs_rasterizer.cpp).
    const float ofxF = static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
    const float ofyF = static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
    walkLinePcsx2(v0.x - ofxF, v0.y - ofyF, v1.x - ofxF, v1.y - ofyF, [&](int x0, int y0, float t)
    {
        if (!S.ownsRow(y0))
            return;
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
        if (S.fge)
            applyFog(S.fogcol, fogF16(v0.fog + (v1.fog - v0.fog) * t), r, g, b);
        writePixel(S, x0, y0, static_cast<u32>(z), r, g, b, a);
    });
}

void runJob(const Job &j, TexCache &cache, int n, int idx)
{
    Setup S;
    S.init(j, cache, n, idx);
    switch (j.prim.prim)
    {
    case GS_PRIM_SPRITE:
        drawSprite(S);
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRISTRIP:
    case GS_PRIM_TRIFAN:
        drawTriangle(S);
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        drawLine(S);
        break;
    case GS_PRIM_POINT:
    {
        const GSVertex &v = j.v[0];
        // PCSX2 DrawPoint: p = int(v - offset + 0.5) (truncating).
        int px = static_cast<int>(v.x - static_cast<float>(j.ctx.xyoffset.ofx) / 16.0f + 0.5f);
        int py = static_cast<int>(v.y - static_cast<float>(j.ctx.xyoffset.ofy) / 16.0f + 0.5f);
        uint8_t pr = v.r, pg = v.g, pb = v.b;
        if (S.fge)
            applyFog(S.fogcol, v.fog << 7, pr, pg, pb);
        if (S.ownsRow(py))
            writePixel(S, px, py, static_cast<u32>(v.z), pr, pg, pb, v.a);
        break;
    }
    default:
        break;
    }
}

// ---- block ranges ---------------------------------------------------------
struct Range
{
    uint32_t lo, hi; // blocks, hi exclusive, hi <= kVramBlocks
    uint64_t seq;    // last job index that uses it
};

struct PageDims
{
    uint32_t w, h;
};

inline PageDims pageDims(uint32_t psm)
{
    switch (psm)
    {
    case GS_PSM_CT16:
    case GS_PSM_CT16S:
    case GS_PSM_Z16:
    case GS_PSM_Z16S:
        return {64u, 64u};
    case GS_PSM_T8:
        return {128u, 64u};
    case GS_PSM_T4:
        return {128u, 128u};
    default: // 32-bit formats and the ones that alias them (T8H, T4HL, T4HH)
        return {64u, 32u};
    }
}

// Blocks a rect in a buffer may touch: every page from the rect's first page
// to its last one (row-major page order), so it is a single range.
// base is in blocks, bw in 64-pixel units. Calls add(lo, hi) once or twice
// (twice when the range wraps past the end of VRAM).
template <typename F>
void rectBlocks(uint32_t base, uint32_t bw, uint32_t psm, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, F &&add)
{
    const PageDims d = pageDims(psm);
    const uint32_t ppr = std::max(1u, (bw * 64u) / d.w);
    const uint64_t first = static_cast<uint64_t>(y0 / d.h) * ppr + x0 / d.w;
    const uint64_t last = static_cast<uint64_t>(y1 / d.h) * ppr + x1 / d.w;
    const uint64_t lo = base + first * 32u;
    const uint64_t hi = base + (last + 1u) * 32u;
    if (hi - lo >= kVramBlocks)
    {
        add(0u, kVramBlocks);
        return;
    }
    const uint32_t l = static_cast<uint32_t>(lo % kVramBlocks);
    const uint32_t h = static_cast<uint32_t>(l + (hi - lo));
    if (h <= kVramBlocks)
        add(l, h);
    else
    {
        add(l, kVramBlocks);
        add(0u, h - kVramBlocks);
    }
}

// ---- engine -----------------------------------------------------------------
struct alignas(64) Counter
{
    std::atomic<uint64_t> v{0};
};

struct Engine
{
    int n = 0;
    Job *ring = nullptr;
    alignas(64) std::atomic<uint64_t> w{0};
    alignas(64) std::atomic<int> sleepers{0};
    Counter done[kMaxWorkers];

    // producer only (callers hold the GS state mutex)
    std::vector<Range> writes;
    std::vector<Range> reads;
    TexCache *producerCache = nullptr;
    struct ClutSlot
    {
        uint64_t lastUse = 0;
        bool used = false;
        alignas(64) uint8_t data[1024];
    };
    ClutSlot *clut = nullptr;
    uint32_t clutCur = 0;
    uint64_t clutVersion = 1;
    uint64_t clutSnapVersion = 0;
    const void *clutOwner = nullptr;
    uint8_t *lastVram = nullptr;
    uint64_t stBarriers = 0, stSolo = 0, stJobs = 0;

    uint64_t minDone() const
    {
        uint64_t m = ~0ull;
        for (int i = 0; i < n; ++i)
            m = std::min(m, done[i].v.load(std::memory_order_acquire));
        return m;
    }

    void waitDone(uint64_t target)
    {
        if (minDone() >= target)
            return;
        const auto t0 = std::chrono::steady_clock::now();
        uint32_t spins = 0;
        while (minDone() < target)
        {
            if (++spins < 2000u)
                _mm_pause();
            else
                std::this_thread::yield();
        }
        const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
        const int why = g_gsmtWaitReason < 16 ? g_gsmtWaitReason : 0;
        g_gsmtWaitDoneCalls.fetch_add(1u, std::memory_order_relaxed);
        g_gsmtWaitDoneNs.fetch_add(ns, std::memory_order_relaxed);
        g_gsmtWaitByReason[why][0].fetch_add(1u, std::memory_order_relaxed);
        g_gsmtWaitByReason[why][1].fetch_add(ns, std::memory_order_relaxed);
        g_gsmtWaitReason = 0;
    }

    void flush()
    {
        waitDone(w.load(std::memory_order_relaxed));
        writes.clear();
        reads.clear();
    }

    void prune()
    {
        if (writes.empty() && reads.empty())
            return;
        const uint64_t md = minDone();
        auto old = [md](const Range &r) { return r.seq < md; };
        writes.erase(std::remove_if(writes.begin(), writes.end(), old), writes.end());
        reads.erase(std::remove_if(reads.begin(), reads.end(), old), reads.end());
    }

    static bool overlaps(const std::vector<Range> &set, uint32_t lo, uint32_t hi)
    {
        for (const Range &r : set)
            if (lo < r.hi && r.lo < hi)
                return true;
        return false;
    }

    static void addRange(std::vector<Range> &set, uint32_t lo, uint32_t hi, uint64_t seq)
    {
        for (Range &r : set)
        {
            if (lo <= r.hi && r.lo <= hi) // overlapping or touching: merge
            {
                r.lo = std::min(r.lo, lo);
                r.hi = std::max(r.hi, hi);
                r.seq = std::max(r.seq, seq);
                return;
            }
        }
        set.push_back({lo, hi, seq});
    }

    static void bumpPages(uint32_t lo, uint32_t hi)
    {
        for (uint32_t p = lo >> 5; p <= ((hi - 1u) >> 5) && p < kVramPages; ++p)
            g_pageGen[p].fetch_add(1u, std::memory_order_release);
    }

    void publish(const Job &job)
    {
        const uint64_t idx = w.load(std::memory_order_relaxed);
        if (idx >= kRingSize)
        {
            g_gsmtWaitReason = 7;
            waitDone(idx - kRingSize + 1u);
        }
        ring[idx & (kRingSize - 1u)] = job;
        w.store(idx + 1u, std::memory_order_seq_cst);
        if (sleepers.load(std::memory_order_seq_cst) != 0)
            w.notify_all();
    }

    void workerMain(int idx)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "GsRaster%d", idx);
        ThreadNaming::SetCurrentThreadName(name);
        TexCache *cache = new TexCache();
        uint64_t r = 0;
        for (;;)
        {
            uint64_t avail = w.load(std::memory_order_acquire);
            if (avail == r)
            {
                uint32_t spins = 0;
                while ((avail = w.load(std::memory_order_acquire)) == r)
                {
                    if (++spins < 4000u)
                    {
                        _mm_pause();
                        continue;
                    }
                    sleepers.fetch_add(1, std::memory_order_seq_cst);
                    w.wait(r, std::memory_order_seq_cst);
                    sleepers.fetch_sub(1, std::memory_order_seq_cst);
                    spins = 0;
                }
            }
            while (r < avail)
            {
                runJob(ring[r & (kRingSize - 1u)], *cache, n, idx);
                ++r;
                done[idx].v.store(r, std::memory_order_release);
            }
        }
    }
};

Engine *g_engine = nullptr;

int threadCount()
{
    static const int count = []
    {
        const char *e = std::getenv("PS2X_GS_RASTER_THREADS");
        int c = (e && *e) ? std::atoi(e) : 4;
        if (c < 0)
            c = 0;
        if (c > static_cast<int>(kMaxWorkers))
            c = static_cast<int>(kMaxWorkers);
        if (ps2_diag::enabled() || ps2diag_skipbg::enabled())
            c = 0; // probes live in the single-thread path
        const char *mesh = std::getenv("PS2X_MESHDUMP");
        if (mesh && *mesh)
            c = 0;
        if (c > 0)
            std::printf("[gsraster] %d raster threads (PS2X_GS_RASTER_THREADS=0 for the old path)\n", c);
        return c;
    }();
    return count;
}

Engine &engine()
{
    if (!g_engine)
    {
        Engine *e = new Engine();
        e->n = threadCount();
        e->ring = new Job[kRingSize];
        e->producerCache = new TexCache();
        e->clut = new Engine::ClutSlot[kClutSlots];
        g_engine = e;
        for (int i = 0; i < e->n; ++i)
            std::thread([e, i] { e->workerMain(i); }).detach();
    }
    return *g_engine;
}

inline bool active()
{
    return g_engine != nullptr;
}

// Queue one primitive (called by GSRasterizer::drawPrimitive on the GS
// thread). clutSrc is GS::m_clut_cache, owner the GS object.
void submit(Job &job, const uint8_t *clutSrc, const void *owner)
{
    Engine &e = engine();
    ++e.stJobs;
    if (job.vram != e.lastVram)
    {
        g_gsmtWaitReason = 1;
        e.flush();
        g_epoch.fetch_add(1u, std::memory_order_release);
        e.lastVram = job.vram;
    }

    const GSContext &ctx = job.ctx;
    const auto &prim = job.prim;

    // Pixel bounds (loose), clamped to the scissor: nothing outside it is written.
    int nv = 3;
    switch (prim.prim)
    {
    case GS_PRIM_SPRITE:
    case GS_PRIM_LINE:
    case GS_PRIM_LINESTRIP:
        nv = 2;
        break;
    case GS_PRIM_POINT:
        nv = 1;
        break;
    default:
        break;
    }
    const float ofx = static_cast<float>(static_cast<int>(ctx.xyoffset.ofx >> 4));
    const float ofy = static_cast<float>(static_cast<int>(ctx.xyoffset.ofy >> 4));
    float minX = job.v[0].x, maxX = job.v[0].x, minY = job.v[0].y, maxY = job.v[0].y;
    for (int i = 1; i < nv; ++i)
    {
        minX = std::min(minX, job.v[i].x);
        maxX = std::max(maxX, job.v[i].x);
        minY = std::min(minY, job.v[i].y);
        maxY = std::max(maxY, job.v[i].y);
    }
    const int sx0 = static_cast<int>(ctx.scissor.x0), sx1 = static_cast<int>(ctx.scissor.x1);
    const int sy0 = static_cast<int>(ctx.scissor.y0), sy1 = static_cast<int>(ctx.scissor.y1);
    auto clampF = [](float v, int lo, int hi) -> uint32_t
    {
        if (!(v >= static_cast<float>(lo))) // also NaN
            return static_cast<uint32_t>(lo);
        if (v > static_cast<float>(hi))
            return static_cast<uint32_t>(hi);
        return static_cast<uint32_t>(static_cast<int>(v));
    };
    const uint32_t bx0 = clampF(std::floor(minX - ofx) - 1.0f, sx0, sx1);
    const uint32_t bx1 = clampF(std::ceil(maxX - ofx) + 1.0f, sx0, sx1);
    const uint32_t by0 = clampF(std::floor(minY - ofy) - 1.0f, sy0, sy1);
    const uint32_t by1 = clampF(std::ceil(maxY - ofy) + 1.0f, sy0, sy1);

    Range wr[4];
    int nwr = 0;
    auto addW = [&](uint32_t lo, uint32_t hi) { if (nwr < 4) wr[nwr++] = {lo, hi, 0}; };
    const uint32_t fbw = std::max<u32>(ctx.frame.fbw, 1u);
    rectBlocks(GSInternal::framePageBaseToBlock(ctx.frame.fbp), fbw, ctx.frame.psm, bx0, by0, bx1, by1, addW);
    const uint32_t ztst = static_cast<uint32_t>((ctx.test.data >> 17) & 3);
    if (!ctx.zbuf.zmsk && ztst != 0u)
        rectBlocks(GSInternal::framePageBaseToBlock(ctx.zbuf.zbp), fbw, ctx.zbuf.psm | 0x30u, bx0, by0, bx1, by1, addW);

    Range rd[2];
    int nrd = 0;
    if (prim.tme)
    {
        const auto &tex = ctx.tex0;
        const uint32_t tw = 1u << tex.tw, th = 1u << tex.th;
        rectBlocks(tex.tbp0, tex.tbw, tex.psm, 0u, 0u, tw - 1u, th - 1u,
                   [&](uint32_t lo, uint32_t hi) { if (nrd < 2) rd[nrd++] = {lo, hi, 0}; });
    }

    e.prune();
    bool self = false, hazard = false;
    for (int i = 0; i < nrd; ++i)
    {
        if (Engine::overlaps(e.writes, rd[i].lo, rd[i].hi))
            hazard = true;
        for (int k = 0; k < nwr; ++k)
            if (rd[i].lo < wr[k].hi && wr[k].lo < rd[i].hi)
                self = true;
    }
    for (int k = 0; k < nwr; ++k)
        if (Engine::overlaps(e.reads, wr[k].lo, wr[k].hi))
            hazard = true;

    if (prim.tme && isPaletted(ctx.tex0.psm))
    {
        Engine::ClutSlot *cur = &e.clut[e.clutCur];
        if (!cur->used || e.clutOwner != owner || e.clutSnapVersion != e.clutVersion)
        {
            e.clutCur = (e.clutCur + 1u) % kClutSlots;
            cur = &e.clut[e.clutCur];
            if (cur->used)
            {
                g_gsmtWaitReason = 2;
                e.waitDone(cur->lastUse + 1u);
            }
            std::memcpy(cur->data, clutSrc, sizeof(cur->data));
            cur->used = true;
            e.clutOwner = owner;
            e.clutSnapVersion = e.clutVersion;
        }
        cur->lastUse = e.w.load(std::memory_order_relaxed);
        job.clut = cur->data;
    }
    else
        job.clut = nullptr;

    if (self)
    {
        // Samples its own render target: the result depends on pixel order,
        // so draw it here, alone, in the old order.
        ++e.stSolo;
        g_gsmtWaitReason = 3;
        e.flush();
        runJob(job, *e.producerCache, 1, 0);
        for (int k = 0; k < nwr; ++k)
            Engine::bumpPages(wr[k].lo, wr[k].hi);
        return;
    }
    if (hazard)
    {
        ++e.stBarriers;
        g_gsmtWaitReason = 4;
        e.flush();
    }

    const uint64_t index = e.w.load(std::memory_order_relaxed);
    // Before publishing: a cached copy of a page this job writes goes stale.
    for (int k = 0; k < nwr; ++k)
        Engine::bumpPages(wr[k].lo, wr[k].hi);
    e.publish(job);
    for (int k = 0; k < nwr; ++k)
        Engine::addRange(e.writes, wr[k].lo, wr[k].hi, index);
    for (int i = 0; i < nrd; ++i)
        Engine::addRange(e.reads, rd[i].lo, rd[i].hi, index);
}
} // namespace gsmt

// ---- hooks for ps2_gs_gpu.cpp (declared there with extern) -----------------

// Wait until every queued primitive is drawn. Call before reading or writing
// VRAM outside the rasterizer, holding the GS state mutex.
void ps2xGsRasterFlush()
{
    if (gsmt::active())
        gsmt::g_engine->flush();
}

// VRAM content changed behind the rasterizer (new GS, reseeded VRAM).
void ps2xGsRasterReset()
{
    ps2xGsRasterFlush();
    gsmt::g_epoch.fetch_add(1u, std::memory_order_release);
    if (gsmt::active())
        ++gsmt::g_engine->clutVersion;
}

// The GS is about to read (write=false) or write (write=true) a rect of VRAM.
// Waits only if a queued primitive touches the same blocks.
void ps2xGsRasterSyncRect(uint32_t baseBlock, uint32_t bw, uint32_t psm,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h, bool write)
{
    if (!gsmt::active())
        return;
    gsmt::Engine &e = *gsmt::g_engine;
    if (w == 0u || h == 0u)
        return;
    e.prune();
    // Wait only for the last queued job that touches these blocks (a range's
    // seq is the newest job using it; workers finish jobs in order), not for
    // the whole queue. Host->local texture uploads land here ~110x/s in a
    // fight, and a full flush each time kept the GS thread waiting on the
    // raster workers.
    bool hazard = false;
    uint64_t lastSeq = 0;
    auto note = [&](const std::vector<gsmt::Range> &set, uint32_t lo, uint32_t hi)
    {
        for (const gsmt::Range &r : set)
            if (lo < r.hi && r.lo < hi)
            {
                hazard = true;
                lastSeq = std::max(lastSeq, r.seq);
            }
    };
    auto check = [&](uint32_t lo, uint32_t hi)
    {
        note(e.writes, lo, hi);
        if (write)
            note(e.reads, lo, hi);
    };
    gsmt::rectBlocks(baseBlock, bw, psm, x, y, x + w - 1u, y + h - 1u, check);
    if (hazard)
    {
        ++e.stBarriers;
        g_gsmtWaitReason = write ? 6 : 5;
        e.waitDone(lastSeq + 1u);
    }
    if (write)
        gsmt::rectBlocks(baseBlock, bw, psm, x, y, x + w - 1u, y + h - 1u,
                         [](uint32_t lo, uint32_t hi) { gsmt::Engine::bumpPages(lo, hi); });
}

// The CLUT cache (GS::m_clut_cache) was reloaded.
void ps2xGsRasterClutChanged()
{
    if (gsmt::active())
        ++gsmt::g_engine->clutVersion;
}
