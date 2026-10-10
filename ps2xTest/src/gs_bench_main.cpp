// GS speed benchmark: replays a PCSX2 GS dump (.gsr from
// build_scripts/gsdump_parse.py --emit-replay) through our GS N times and
// reports the host time spent in GS::processGIFPacket. No game run needed.
//
//   python build_scripts/gsdump_parse.py dump.gs.zst --emit-replay gsdump/x.gsr --emit-vram gsdump/x.vram
//   ps2x_gs_bench.exe gsdump/x.gsr [repeat]
//
// The .vram file (same name, optional) seeds VRAM with the dump's starting
// state so textures sample real data; without it VRAM starts zeroed.
//
// Environment:
//   PS2X_GSBENCH_BMP  write the FRAME context-0 buffer after the last replay
//                     ("fbp,fbw,w,h,path", e.g. "0x70,8,512,448,gsdump/b.bmp")
//   PS2X_PROFILE=1    sample with the host sampler (PS2X_PROFILE_MS=1 for 1 ms)
//   PS2X_GSBENCH_THREAD=1  submit through the GS thread queue (as in game) and
//                     wait for it at the end of each replay
#include "runtime/ps2_gs_gpu.h"

#include <algorithm>
#include <atomic>
#include <intrin.h>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" void ps2x_host_sampler_start(void);
extern "C" void ps2x_host_sampler_stop(void);
void ps2xGsThreadSubmit(GS *gs, const uint8_t *data, uint32_t sizeBytes);
void ps2xGsRasterFlush(); // ps2_gs_raster_mt.inl
void ps2xGsThreadSync(uint32_t reason);
void ps2xGsThreadStop();
extern std::atomic<uint64_t> g_gsmtWaitByReason[16][2]; // ps2_gs_rasterizer.cpp: wait count / ns per reason
extern std::atomic<uint64_t> g_gsmtBusyTsc[16], g_gsmtBarrierTsc[16], g_gsmtJobs[16], g_gsmtSubmitTsc, g_gsmtUpDirect, g_gsmtUpDeferred, g_gsmtApplyTsc;

namespace
{
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;

    struct GsrTransfer
    {
        uint32_t offset;
        uint32_t size;
        uint32_t path;
    };

    bool readFile(const std::string &path, std::vector<uint8_t> &out)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        out.resize(size > 0 ? static_cast<size_t>(size) : 0u);
        const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
        std::fclose(f);
        return ok;
    }

    // .gsr layout: "GSR1", version, count, regsSize, payloadSize, reserved,
    // regs[regsSize], index[count]{offset,size,path}, payload[payloadSize].
    bool parseGsr(const std::vector<uint8_t> &file, std::vector<GsrTransfer> &transfers,
                  const uint8_t *&payload, uint32_t &payloadSize)
    {
        if (file.size() < 24u || std::memcmp(file.data(), "GSR1", 4) != 0)
            return false;
        uint32_t fields[5];
        std::memcpy(fields, file.data() + 4, sizeof(fields));
        if (fields[0] != 1u)
            return false;
        const uint32_t count = fields[1], regsSize = fields[2];
        payloadSize = fields[3];
        const size_t indexAt = 24u + regsSize;
        const size_t payloadAt = indexAt + static_cast<size_t>(count) * sizeof(GsrTransfer);
        if (payloadAt + payloadSize > file.size())
            return false;
        transfers.resize(count);
        std::memcpy(transfers.data(), file.data() + indexAt, count * sizeof(GsrTransfer));
        payload = file.data() + payloadAt;
        for (const GsrTransfer &t : transfers)
            if (static_cast<uint64_t>(t.offset) + t.size > payloadSize)
                return false;
        return true;
    }

    bool writeBmp(const std::string &path, const std::vector<uint32_t> &abgr, uint32_t w, uint32_t h)
    {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        const uint32_t pixelBytes = w * h * 4u, fileSize = 54u + pixelBytes, dataOffset = 54u, dib = 40u;
        const uint16_t planes = 1u, bpp = 32u;
        uint8_t header[54]{};
        header[0] = 'B';
        header[1] = 'M';
        std::memcpy(header + 2, &fileSize, 4);
        std::memcpy(header + 10, &dataOffset, 4);
        std::memcpy(header + 14, &dib, 4);
        std::memcpy(header + 18, &w, 4);
        std::memcpy(header + 22, &h, 4);
        std::memcpy(header + 26, &planes, 2);
        std::memcpy(header + 28, &bpp, 2);
        std::memcpy(header + 34, &pixelBytes, 4);
        std::fwrite(header, 1, sizeof(header), f);
        std::vector<uint8_t> row(w * 4u);
        for (uint32_t y = h; y-- > 0;)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t p = abgr[static_cast<size_t>(y) * w + x];
                row[x * 4u + 0] = static_cast<uint8_t>(p >> 16);
                row[x * 4u + 1] = static_cast<uint8_t>(p >> 8);
                row[x * 4u + 2] = static_cast<uint8_t>(p);
                row[x * 4u + 3] = 0xFFu;
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        return true;
    }

    size_t envIndex(const char *name, size_t fallback)
    {
        const char *v = std::getenv(name);
        return (v && *v) ? static_cast<size_t>(std::strtoull(v, nullptr, 10)) : fallback;
    }

    // PS2X_GSBENCH_WATCH="fbp,fbw,x,y,w,h": after each transfer, hash that screen rect of
    // the frame buffer and list the draws whose bbox overlaps it, so a missing region can
    // be blamed on a draw (or on no draw at all).
    struct Watch
    {
        bool on = false;
        int fbp = 0;
        unsigned fbw = 0, x = 0, y = 0, w = 0, h = 0;
    };

    // Raw GS coordinates in the debug history still carry XYOFFSET; menu/fight use 1792/1824.
    constexpr float kOfx = 1792.0f;
    constexpr float kOfy = 1824.0f;

    void reportWatch(GS &gs, const Watch &wt, size_t transferIdx, uint64_t &lastHash)
    {
        ps2xGsRasterFlush();
        uint64_t h = 1469598103934665603ull;
        unsigned nonBlack = 0;
        for (unsigned y = wt.y; y < wt.y + wt.h; ++y)
            for (unsigned x = wt.x; x < wt.x + wt.w; ++x)
            {
                const uint32_t p = gs.ReadVram(0u, static_cast<uint32_t>(wt.fbp), wt.fbw, x, y);
                h = (h ^ p) * 1099511628211ull;
                nonBlack += (p & 0x00FFFFFFu) != 0u;
            }
        const bool changed = h != lastHash;
        lastHash = h;
        bool anyDraw = false;
        for (const GSDebugHistoryEntry &e : gs.getDebugHistory())
        {
            if (e.kind != GSDebugEventKind::Draw)
                continue;
            const float x0 = e.xMin - kOfx, y0 = e.yMin - kOfy, x1 = e.xMax - kOfx, y1 = e.yMax - kOfy;
            if (x1 < wt.x || x0 > wt.x + wt.w || y1 < wt.y || y0 > wt.y + wt.h)
                continue;
            anyDraw = true;
            std::printf("[watch] t=%zu changed=%d nonblack=%u hash=%016llx draw bbox=(%.2f,%.2f)-(%.2f,%.2f) "
                        "prim=%u tme=%u abe=%u fbp=0x%x tbp0=%u cbp=%u tpsm=%u ate=%u atst=%u zte=%u ztst=%u verts=%u\n",
                        transferIdx, changed ? 1 : 0, nonBlack, static_cast<unsigned long long>(h), x0, y0, x1, y1,
                        static_cast<unsigned>(e.prim.prim), e.prim.tme ? 1u : 0u, e.prim.abe ? 1u : 0u,
                        static_cast<unsigned>(e.frame.fbp), static_cast<unsigned>(e.tex0.tbp0),
                        static_cast<unsigned>(e.tex0.cbp), static_cast<unsigned>(e.tex0.psm),
                        static_cast<unsigned>(e.test & 1u), static_cast<unsigned>((e.test >> 1) & 7u),
                        static_cast<unsigned>((e.test >> 16) & 1u), static_cast<unsigned>((e.test >> 17) & 3u),
                        e.vertexCount);
        }
        if (changed && !anyDraw)
            std::printf("[watch] t=%zu changed=1 nonblack=%u hash=%016llx nodraw\n", transferIdx, nonBlack,
                        static_cast<unsigned long long>(h));
        gs.clearDebugHistory();
    }
}

int main(int argc, char **argv)
{
    const std::string gsrPath = argc > 1 ? argv[1] : "gsdump/fight_a16.gsr";
    const int repeat = argc > 2 ? (std::max)(1, std::atoi(argv[2])) : 5;

    std::vector<uint8_t> file;
    std::vector<GsrTransfer> transfers;
    const uint8_t *payload = nullptr;
    uint32_t payloadSize = 0u;
    if (!readFile(gsrPath, file) || !parseGsr(file, transfers, payload, payloadSize))
    {
        std::printf("[gsbench] cannot load %s\n", gsrPath.c_str());
        return 2;
    }

    std::vector<uint8_t> seed;
    std::string vramPath = gsrPath;
    const size_t dot = vramPath.rfind('.');
    vramPath = (dot == std::string::npos ? vramPath : vramPath.substr(0, dot)) + ".vram";
    const bool haveSeed = readFile(vramPath, seed) && seed.size() == kVramSize;
    std::printf("[gsbench] %s: %zu transfers, %u bytes, vram seed %s\n", gsrPath.c_str(),
                transfers.size(), payloadSize, haveSeed ? "yes" : "no (zeroed)");

    const char *threadEnv = std::getenv("PS2X_GSBENCH_THREAD");
    const bool threaded = threadEnv && threadEnv[0] == '1';
    if (threaded)
        std::printf("[gsbench] threaded: packets go through the GS thread queue\n");

    // Debug options for tools/gfx_scene_diff.py. All opt-in, none affect timing runs.
    const size_t stopAt = envIndex("PS2X_GSBENCH_STOP", SIZE_MAX);
    const size_t skipAt = envIndex("PS2X_GSBENCH_SKIP", SIZE_MAX);
    Watch watch;
    if (const char *spec = std::getenv("PS2X_GSBENCH_WATCH"))
        watch.on = std::sscanf(spec, "%i,%u,%u,%u,%u,%u", &watch.fbp, &watch.fbw, &watch.x, &watch.y,
                               &watch.w, &watch.h) == 6;
    if (watch.on)
        std::printf("[gsbench] watch fbp=0x%x rect=%u,%u %ux%u\n", watch.fbp, watch.x, watch.y, watch.w, watch.h);

    std::vector<uint8_t> vram(kVramSize);
    std::vector<double> times;
    ps2x_host_sampler_start();
    for (int r = 0; r < repeat; ++r)
    {
        if (haveSeed)
            std::memcpy(vram.data(), seed.data(), kVramSize);
        else
            std::fill(vram.begin(), vram.end(), uint8_t{0});
        GS gs;
        gs.init(vram.data(), kVramSize, nullptr);

        const bool watching = watch.on && r == repeat - 1;
        if (watching)
            gs.setDebugHistoryPaused(false);
        uint64_t lastHash = 0u;
        size_t transferIdx = 0u;
        const auto t0 = std::chrono::steady_clock::now();
        for (const GsrTransfer &t : transfers)
        {
            const size_t cur = transferIdx++;
            if (t.size == 0u)
                continue;
            if (cur >= stopAt)
                break;
            if (cur == skipAt)
                continue;
            if (threaded)
                ps2xGsThreadSubmit(&gs, payload + t.offset, t.size);
            else
                gs.processGIFPacket(payload + t.offset, t.size);
            if (watching)
                reportWatch(gs, watch, cur, lastHash);
        }
        if (threaded)
            ps2xGsThreadSync(0u);
        ps2xGsRasterFlush(); // raster threads finish before the clock stops
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

        if (r == repeat - 1)
        {
            // Correctness gate: gs_bench.ps1 compares this against a saved baseline.
            uint64_t h = 1469598103934665603ull;
            for (const uint8_t b : vram)
                h = (h ^ b) * 1099511628211ull;
            std::printf("bench: vram hash %016llx\n", static_cast<unsigned long long>(h));
            if (const char *spec = std::getenv("PS2X_GSBENCH_BMP"))
            {
                unsigned fbp = 0, fbw = 0, w = 0, h = 0;
                char out[512] = {};
                if (std::sscanf(spec, "%i,%u,%u,%u,%511[^\n]", &fbp, &fbw, &w, &h, out) == 5)
                {
                    std::vector<uint32_t> pix(static_cast<size_t>(w) * h);
                    for (uint32_t y = 0; y < h; ++y)
                        for (uint32_t x = 0; x < w; ++x)
                            pix[static_cast<size_t>(y) * w + x] = gs.ReadVram(0u, fbp, fbw, x, y);
                    std::printf("[gsbench] bmp %s: %s\n", out, writeBmp(out, pix, w, h) ? "written" : "FAILED");
                }
            }
        }
    }
    ps2x_host_sampler_stop();
    ps2xGsThreadStop();

    if (const char *st = std::getenv("PS2X_GS_THREAD_STATS"); st && *st && *st != '0')
    {
        // Producer waits on the raster workers, per reason (see ps2_gs_raster_mt.inl),
        // totals over all replays. r1 vram change, r2 CLUT slot, r3 self-sample, r4 hazard,
        // r5/r6 syncRect read/write, r7 ring full, r9/r10 flush at latch/vblank.
        std::printf("[gsraster-wait]");
        for (int r = 0; r < 16; ++r)
            if (const uint64_t c = g_gsmtWaitByReason[r][0].load(std::memory_order_relaxed))
                std::printf(" r%d=%llu/%.1fms", r, static_cast<unsigned long long>(c),
                            g_gsmtWaitByReason[r][1].load(std::memory_order_relaxed) / 1e6);
        std::printf("\n");
        // TSC -> ms (calibrated against steady_clock): per worker busy time and the
        // producer's time inside gsmt::submit, totals over all replays.
        const auto c0 = std::chrono::steady_clock::now();
        const uint64_t t0 = __rdtsc();
        while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(20)) {}
        const double perMs = static_cast<double>(__rdtsc() - t0) /
                             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
        double total = 0.0;
        for (const double t : times)
            total += t;
        std::printf("[gsmt] applyMs=%.1f ", g_gsmtApplyTsc.load(std::memory_order_relaxed) / perMs);
        std::printf("replays=%.1fms submit=%.1fms upDirect=%llu upDeferred=%llu", total, g_gsmtSubmitTsc.load(std::memory_order_relaxed) / perMs,
                    static_cast<unsigned long long>(g_gsmtUpDirect.load(std::memory_order_relaxed)),
                    static_cast<unsigned long long>(g_gsmtUpDeferred.load(std::memory_order_relaxed)));
        for (int i = 0; i < 16; ++i)
            if (const uint64_t j = g_gsmtJobs[i].load(std::memory_order_relaxed))
                std::printf(" w%d=%llujobs/%.1fms(barrier %.1f)", i, static_cast<unsigned long long>(j),
                            g_gsmtBusyTsc[i].load(std::memory_order_relaxed) / perMs,
                            g_gsmtBarrierTsc[i].load(std::memory_order_relaxed) / perMs);
        std::printf("\n");
    }

    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    std::printf("bench: gs median %.1f ms (min %.1f, max %.1f) over %d replays\n",
                sorted[sorted.size() / 2], sorted.front(), sorted.back(), repeat);
    std::fflush(stdout);
    std::_Exit(0);
}
