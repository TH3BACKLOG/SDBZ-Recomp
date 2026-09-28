// Offline GS-dump replay harness.
//
// Why this exists
// ---------------
// Diagnosing the GS by instrumenting the runtime costs ~48 min of build plus a
// 90 s game run per hypothesis, and every answer arrives as a printf. A PCSX2
// single-frame GS dump is the same GIF byte stream the game produced, so
// replaying it through our own GS reproduces the frame with no game, no runner
// and no wait -- in a Debug test binary you can set real breakpoints in.
//
//   1. PCSX2: pause on the frame -> Debug -> "Save Single Frame GS Dump"
//   2. python build_scripts/gsdump_parse.py dump.gs --emit-replay gsdump/frame.gsr
//   3. build.ps1 Debug -Test
//   4. ps2x_tests.exe GSDumpReplay
//
// The test looks for the .gsr next to the project root by default; override
// with PS2X_GSDUMP. It writes the replayed framebuffer to a .bmp so the result
// can be eyeballed against PCSX2's own replay of the same dump.
//
// Environment:
//   PS2X_GSDUMP      path to the .gsr        (default: gsdump/frame.gsr)
//   PS2X_GSDUMP_OUT  path to write the .bmp  (default: gsdump/replay.bmp)
//   PS2X_GSDUMP_RECT "x,y,w,h" region to report a census for
//                    (default: 24,302,464,120 -- the Stage 5.11 black box)

#include "MiniTest.h"
#include "ps2_runtime.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_diag.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kVramSize = 4u * 1024u * 1024u;

    // Written by build_scripts/gsdump_parse.py --emit-replay. Keep in lockstep
    // with the layout comment in emit_replay(); a mismatch here silently
    // replays garbage, so every field is range-checked below.
    struct GsrTransfer
    {
        uint32_t offset;
        uint32_t size;
        uint32_t path;
    };

    struct GsrDump
    {
        std::vector<uint8_t> regs;
        std::vector<GsrTransfer> transfers;
        std::vector<uint8_t> payload;
    };

    struct Rect
    {
        uint32_t x, y, w, h;
    };

    std::string envOr(const char *name, const char *fallback)
    {
        const char *v = std::getenv(name);
        return (v != nullptr && v[0] != '\0') ? std::string(v) : std::string(fallback);
    }

    Rect parseRect(const std::string &spec, Rect fallback)
    {
        Rect r{};
        if (std::sscanf(spec.c_str(), "%u,%u,%u,%u", &r.x, &r.y, &r.w, &r.h) != 4)
            return fallback;
        return r;
    }

    // Returns false with *why* populated when the dump cannot be used. An
    // absent file is not an error -- the harness is opt-in.
    bool loadGsr(const std::string &path, GsrDump &out, std::string &why)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
        {
            why = "not found";
            return false;
        }

        uint8_t header[24]{};
        const bool gotHeader = std::fread(header, 1, sizeof(header), f) == sizeof(header);
        if (!gotHeader || std::memcmp(header, "GSR1", 4) != 0)
        {
            std::fclose(f);
            why = "bad magic -- re-run gsdump_parse.py --emit-replay";
            return false;
        }

        uint32_t fields[5]{};
        std::memcpy(fields, header + 4, sizeof(fields));
        const uint32_t version = fields[0];
        const uint32_t count = fields[1];
        const uint32_t regsSize = fields[2];
        const uint32_t payloadSize = fields[3];

        if (version != 1u)
        {
            std::fclose(f);
            why = "unsupported .gsr version " + std::to_string(version);
            return false;
        }

        out.regs.resize(regsSize);
        out.transfers.resize(count);
        out.payload.resize(payloadSize);

        bool ok = true;
        ok = ok && (regsSize == 0u || std::fread(out.regs.data(), 1, regsSize, f) == regsSize);
        if (count != 0u)
        {
            const size_t indexBytes = static_cast<size_t>(count) * sizeof(GsrTransfer);
            ok = ok && std::fread(out.transfers.data(), 1, indexBytes, f) == indexBytes;
        }
        ok = ok && (payloadSize == 0u ||
                    std::fread(out.payload.data(), 1, payloadSize, f) == payloadSize);
        std::fclose(f);

        if (!ok)
        {
            why = "truncated -- header promises more bytes than the file holds";
            return false;
        }

        // The index is the one place a silently-wrong export would still parse,
        // so bounds-check every entry before it reaches processGIFPacket.
        for (const GsrTransfer &t : out.transfers)
        {
            if (static_cast<uint64_t>(t.offset) + t.size > payloadSize)
            {
                why = "transfer index points outside the payload";
                return false;
            }
        }
        return true;
    }

    // Minimal 32-bit bottom-up BMP. Avoids pulling stb_image_write (which only
    // exists inside the raylib dependency tree) into the test binary.
    bool writeBmp(const std::string &path, const std::vector<uint32_t> &abgr,
                  uint32_t width, uint32_t height)
    {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return false;

        const uint32_t pixelBytes = width * height * 4u;
        const uint32_t fileSize = 54u + pixelBytes;

        uint8_t header[54]{};
        header[0] = 'B';
        header[1] = 'M';
        std::memcpy(header + 2, &fileSize, 4);
        const uint32_t dataOffset = 54u;
        std::memcpy(header + 10, &dataOffset, 4);
        const uint32_t dibSize = 40u;
        std::memcpy(header + 14, &dibSize, 4);
        std::memcpy(header + 18, &width, 4);
        std::memcpy(header + 22, &height, 4);
        const uint16_t planes = 1u;
        const uint16_t bpp = 32u;
        std::memcpy(header + 26, &planes, 2);
        std::memcpy(header + 28, &bpp, 2);
        std::memcpy(header + 34, &pixelBytes, 4);
        std::fwrite(header, 1, sizeof(header), f);

        // GS PSMCT32 words are ABGR (red in the low byte); BMP wants BGRA in
        // file order, so red and blue swap. Rows go bottom-up.
        std::vector<uint8_t> row(width * 4u);
        for (uint32_t y = height; y-- > 0;)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t p = abgr[static_cast<size_t>(y) * width + x];
                row[x * 4u + 0] = static_cast<uint8_t>((p >> 16) & 0xFFu); // B
                row[x * 4u + 1] = static_cast<uint8_t>((p >> 8) & 0xFFu);  // G
                row[x * 4u + 2] = static_cast<uint8_t>(p & 0xFFu);         // R
                row[x * 4u + 3] = 0xFFu;
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        return true;
    }
}

void register_ps2_gsdump_replay_tests()
{
    MiniTest::Case("GSDumpReplay", [](TestCase &tc)
    {
        tc.Run("replay a PCSX2 GS dump through our GS and census the result", [](TestCase &t)
        {
            const std::string dumpPath = envOr("PS2X_GSDUMP", "gsdump/frame.gsr");

            GsrDump dump;
            std::string why;
            if (!loadGsr(dumpPath, dump, why))
            {
                // Opt-in harness: no dump captured yet is not a failure. A
                // malformed one is worth shouting about, because a wrong
                // container branch parses into plausible garbage.
                std::printf("[gsdump] skipped: %s (%s)\n", dumpPath.c_str(), why.c_str());
                std::printf("[gsdump] capture one with:\n"
                            "  python build_scripts/gsdump_parse.py dump.gs --summary\n"
                            "  python build_scripts/gsdump_parse.py dump.gs --emit-replay %s\n",
                            dumpPath.c_str());
                t.IsTrue(why == "not found",
                         "a .gsr that exists must be well-formed: " + why);
                return;
            }

            std::printf("[gsdump] %s: %zu transfers, %zu payload bytes\n",
                        dumpPath.c_str(), dump.transfers.size(), dump.payload.size());

            // The draw-stat counters (m_statPrims / m_statLastDrawFbp) only
            // increment inside `if (ps2_diag::enabled())`. Without this the
            // counters read 0 no matter how much was drawn -- a false negative
            // that looks exactly like "the dump rendered nothing".
            ps2_diag::set_enabled_for_test(true);

            std::vector<uint8_t> vram(kVramSize, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            // ---- per-draw GS state capture --------------------------------
            // GS keeps a 512-entry debug ring recording FRAME/TEST/ALPHA/TEX0
            // plus the vertex bbox for every draw, but m_debugHistoryPaused
            // defaults to TRUE -- which is why every gs_history_frame_*.txt
            // dump written so far has an empty [GS history] table.
            //
            // 512 entries cannot hold ~35k draws, so harvest and clear after
            // every transfer rather than reading the ring once at the end.
            // Nothing is dropped and each getDebugHistory() copy stays tiny.
            //
            // Ranking by bbox area was the wrong lens: the 12 biggest draws are
            // all prim=4 character triangles, and the 464x120 box sprite never
            // appeared. Census by *material* instead -- one bucket per
            // (prim, tme, tbp0, cbp, tpsm) -- so every distinct kind of draw in
            // the frame gets a row whether or not I guessed its shape right.
            // The box's combo (tbp0=0x2a00 cbp=0x2a40 psm=0x13) is then just a
            // row to look up rather than something to gate on in advance.
            //
            // Each bucket keeps the union of its bboxes plus its largest-area
            // exemplar, whose full GS state gets printed below.
            struct DrawBucket
            {
                uint64_t count = 0;
                float xMin = 0.0f, xMax = 0.0f, yMin = 0.0f, yMax = 0.0f;
                double bestArea = -1.0;
                GSDebugHistoryEntry best{};
            };
            std::map<std::array<uint64_t, 5>, DrawBucket> buckets;
            uint64_t totalDraws = 0;

            gs.setDebugHistoryPaused(false);

            for (const GsrTransfer &tr : dump.transfers)
            {
                if (tr.size == 0u)
                    continue;
                gs.processGIFPacket(dump.payload.data() + tr.offset, tr.size);

                for (const GSDebugHistoryEntry &e : gs.getDebugHistory())
                {
                    if (e.kind != GSDebugEventKind::Draw)
                        continue;
                    ++totalDraws;

                    const std::array<uint64_t, 5> key{
                        static_cast<uint64_t>(e.prim.prim),
                        e.prim.tme ? 1ull : 0ull,
                        static_cast<uint64_t>(e.tex0.tbp0),
                        static_cast<uint64_t>(e.tex0.cbp),
                        static_cast<uint64_t>(e.tex0.psm)};

                    DrawBucket &b = buckets[key];
                    if (b.count == 0)
                    {
                        b.xMin = e.xMin; b.xMax = e.xMax;
                        b.yMin = e.yMin; b.yMax = e.yMax;
                    }
                    else
                    {
                        b.xMin = std::min(b.xMin, e.xMin);
                        b.xMax = std::max(b.xMax, e.xMax);
                        b.yMin = std::min(b.yMin, e.yMin);
                        b.yMax = std::max(b.yMax, e.yMax);
                    }
                    ++b.count;

                    const double area = static_cast<double>(e.xMax - e.xMin) *
                                        static_cast<double>(e.yMax - e.yMin);
                    if (area > b.bestArea)
                    {
                        b.bestArea = area;
                        b.best = e;
                    }
                }
                gs.clearDebugHistory();
            }

            gs.setDebugHistoryPaused(true);

            // Corrected reading: the fullscreen clear records exactly
            // (1792,1824)-(2304,2272), i.e. 512x448 -- so these are PIXELS
            // with XYOFFSET still added, NOT 1/16 fixed point. Screen space is
            // raw minus that offset. The dumps agree: XYOFFSET ofx=28672
            // ofy=29192 is 1792 / 1824.5 in pixels. Print both so the raw
            // values stay independently checkable.
            constexpr float kOfx = 1792.0f;
            constexpr float kOfy = 1824.0f;
            std::printf("[gsdraw] totalDraws=%llu buckets=%zu\n",
                        static_cast<unsigned long long>(totalDraws), buckets.size());
            size_t bucketIdx = 0;
            for (const auto &kv : buckets)
            {
                const DrawBucket &b = kv.second;
                const float sx0 = b.xMin - kOfx, sy0 = b.yMin - kOfy;
                const float sx1 = b.xMax - kOfx, sy1 = b.yMax - kOfy;
                // Flag the buckets whose screen span overlaps the Stage 5.11
                // box rect (24,302 464x120). Overlap is necessary, not
                // sufficient -- a union bbox is a union -- but a bucket that
                // does NOT overlap is conclusively not the box.
                const bool hitsBox = (sx1 >= 24.0f && sx0 <= 488.0f &&
                                      sy1 >= 302.0f && sy0 <= 422.0f);
                std::printf("[gsdraw] bucket#%zu n=%llu prim=%u tme=%u tbp0=0x%x cbp=0x%x "
                            "tpsm=0x%x | raw=(%.0f,%.0f)-(%.0f,%.0f) "
                            "screen=(%.0f,%.0f)-(%.0f,%.0f) %.0fx%.0f%s\n",
                            bucketIdx++,
                            static_cast<unsigned long long>(b.count),
                            static_cast<unsigned>(kv.first[0]),
                            static_cast<unsigned>(kv.first[1]),
                            static_cast<unsigned>(kv.first[2]),
                            static_cast<unsigned>(kv.first[3]),
                            static_cast<unsigned>(kv.first[4]),
                            b.xMin, b.yMin, b.xMax, b.yMax,
                            sx0, sy0, sx1, sy1,
                            sx1 - sx0, sy1 - sy0,
                            hitsBox ? "  <== overlaps box rect" : "");
            }

            bucketIdx = 0;
            for (const auto &kv : buckets)
            {
                const GSDebugHistoryEntry &e = kv.second.best;
                const size_t i = bucketIdx++;
                const uint64_t test = e.test;
                const uint64_t alpha = e.alpha;
                // TEST: ATE b0, ATST b1-3, AREF b4-11, AFAIL b12-13,
                //       DATE b14, DATM b15, ZTE b16, ZTST b17-18.
                // ALPHA: A b0-1, B b2-3, C b4-5, D b6-7, FIX b32-39.
                std::printf("[gsdraw] #%zu bbox=(%.1f,%.1f)-(%.1f,%.1f) %.0fx%.0f verts=%u "
                            "prim=%u tme=%u abe=%u | fbp=0x%x fbw=%u psm=0x%x fbmsk=0x%x | "
                            "zbp=%u zpsm=0x%x zmsk=%u | tbp0=0x%x tbw=%u tpsm=0x%x cbp=0x%x | "
                            "ATE=%u ATST=%u AREF=0x%x ZTE=%u ZTST=%u | "
                            "A=%u B=%u C=%u D=%u FIX=0x%x | a=%u..%u\n",
                            i, e.xMin, e.yMin, e.xMax, e.yMax,
                            static_cast<double>(e.xMax - e.xMin),
                            static_cast<double>(e.yMax - e.yMin),
                            e.vertexCount,
                            static_cast<unsigned>(e.prim.prim),
                            e.prim.tme ? 1u : 0u,
                            e.prim.abe ? 1u : 0u,
                            static_cast<unsigned>(e.frame.fbp),
                            static_cast<unsigned>(e.frame.fbw),
                            static_cast<unsigned>(e.frame.psm),
                            static_cast<unsigned>(e.frame.fbmsk),
                            static_cast<unsigned>(e.zbuf.zbp),
                            static_cast<unsigned>(e.zbuf.psm),
                            e.zbuf.zmsk ? 1u : 0u,
                            static_cast<unsigned>(e.tex0.tbp0),
                            static_cast<unsigned>(e.tex0.tbw),
                            static_cast<unsigned>(e.tex0.psm),
                            static_cast<unsigned>(e.tex0.cbp),
                            static_cast<unsigned>(test & 1u),
                            static_cast<unsigned>((test >> 1) & 7u),
                            static_cast<unsigned>((test >> 4) & 0xFFu),
                            static_cast<unsigned>((test >> 16) & 1u),
                            static_cast<unsigned>((test >> 17) & 3u),
                            static_cast<unsigned>(alpha & 3u),
                            static_cast<unsigned>((alpha >> 2) & 3u),
                            static_cast<unsigned>((alpha >> 4) & 3u),
                            static_cast<unsigned>((alpha >> 6) & 3u),
                            static_cast<unsigned>((alpha >> 32) & 0xFFu),
                            static_cast<unsigned>(e.aMin),
                            static_cast<unsigned>(e.aMax));
            }

            // Every Stage 5.11 probe ([texred], [boxtex], [clutdump], [frameord])
            // is emitted from the PresentProbe scope guard inside
            // latchHostPresentationFrameUnlocked -- the vsync/present path, which
            // replaying GIF packets alone never reaches. Its `armed` flag is
            // should_log(latchIndex, 4, 60), so the first latch arms. Without this
            // call the probes are silently absent, which is indistinguishable from
            // "the counters were never incremented".
            gs.latchHostPresentationFrame();

            const uint64_t prims = gs.drawStatPrims();
            const uint64_t imageBytes = gs.drawStatImageBytes();
            const uint32_t lastFbp = gs.drawStatLastFbp();
            std::printf("[gsdump] prims=%llu imageBytes=%llu lastDrawFbp=0x%x\n",
                        static_cast<unsigned long long>(prims),
                        static_cast<unsigned long long>(imageBytes),
                        lastFbp);

            t.IsTrue(!dump.transfers.empty(), "the dump should contain GIF transfers");
            t.IsTrue(prims > 0u, "replaying the dump should have drawn primitives");

            // Pick whichever context the final draw actually used, rather than
            // assuming context 0 -- guessing here would silently read an
            // unwritten region and report a black frame.
            const GSFrameReg &ctx0 = gs.getContextFrame(0);
            const GSFrameReg &ctx1 = gs.getContextFrame(1);
            std::printf("[gsdump] ctx0.fbp=0x%x fbw=%u  ctx1.fbp=0x%x fbw=%u\n",
                        static_cast<uint32_t>(ctx0.fbp), static_cast<uint32_t>(ctx0.fbw),
                        static_cast<uint32_t>(ctx1.fbp), static_cast<uint32_t>(ctx1.fbw));

            // Match on the context the last draw actually used. If lastFbp
            // matches neither (or the stat never moved) fall back to ctx0 and
            // say so, rather than silently censusing an unwritten region.
            const GSFrameReg *framePtr = &ctx0;
            if (static_cast<uint32_t>(ctx1.fbp) == lastFbp && static_cast<uint32_t>(ctx0.fbp) != lastFbp)
                framePtr = &ctx1;
            else if (static_cast<uint32_t>(ctx0.fbp) != lastFbp)
                std::printf("[gsdump] WARNING: lastDrawFbp=0x%x matches neither context; using ctx0\n",
                            lastFbp);
            const GSFrameReg &frame = *framePtr;

            const uint32_t fbp = static_cast<uint32_t>(frame.fbp);
            const uint32_t fbw = static_cast<uint32_t>(frame.fbw);
            const uint32_t psm = static_cast<uint32_t>(frame.psm);
            const uint32_t width = (fbw != 0u) ? (fbw * 64u) : 640u;
            const uint32_t height = 448u;
            std::printf("[gsdump] framebuffer fbp=0x%x fbw=%u psm=0x%x -> %ux%u\n",
                        fbp, fbw, psm, width, height);

            const Rect rect = parseRect(envOr("PS2X_GSDUMP_RECT", "24,302,464,120"),
                                        Rect{24u, 302u, 464u, 120u});

            // The draw census says every UI sprite in this frame targets
            // fbp=0x0 while the presented buffer is fbp=0x70, and [fbdest]
            // reports ~1.4M writes to EACH. Censusing only the presented
            // buffer therefore cannot distinguish "the box was never drawn"
            // from "the box was drawn into the other buffer" -- so read every
            // fbp the frame plausibly wrote and let the numbers say which.
            std::vector<uint32_t> candidates;
            for (uint32_t cand : {static_cast<uint32_t>(ctx0.fbp),
                                  static_cast<uint32_t>(ctx1.fbp),
                                  lastFbp,
                                  0x0u})
            {
                if (std::find(candidates.begin(), candidates.end(), cand) == candidates.end())
                    candidates.push_back(cand);
            }

            // Assertions still describe the presented buffer; the extra
            // candidates are diagnostic only.
            std::vector<uint32_t> pixels;
            uint64_t nonBlack = 0;
            // The per-candidate census below is loop-scoped; the assertions at
            // the end still describe the presented buffer only, so keep its
            // numbers here rather than letting the last candidate win.
            uint64_t presentedRectTotal = 0;
            uint64_t presentedRectNonBlack = 0;

            for (uint32_t candFbp : candidates)
            {
                std::vector<uint32_t> buf(static_cast<size_t>(width) * height, 0u);
                uint64_t nb = 0;
                for (uint32_t y = 0; y < height; ++y)
                {
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        const uint32_t p = gs.ReadVram(psm, candFbp, fbw, x, y);
                        buf[static_cast<size_t>(y) * width + x] = p;
                        if ((p & 0x00FFFFFFu) != 0u)
                            ++nb;
                    }
                }

                uint64_t rectTotal = 0;
                uint64_t rectNonBlack = 0;
                uint64_t rectRedish = 0;
                for (uint32_t y = rect.y; y < rect.y + rect.h && y < height; ++y)
                {
                    for (uint32_t x = rect.x; x < rect.x + rect.w && x < width; ++x)
                    {
                        const uint32_t p = buf[static_cast<size_t>(y) * width + x];
                        ++rectTotal;
                        if ((p & 0x00FFFFFFu) != 0u)
                            ++rectNonBlack;
                        // PSMCT32 words are ABGR, so red is the low byte.
                        // Require red DOMINANCE, not just a lit red channel --
                        // otherwise every white or grey pixel counts as "red"
                        // and the census cannot tell the box from the backdrop.
                        const uint32_t r = p & 0xFFu;
                        const uint32_t g = (p >> 8) & 0xFFu;
                        const uint32_t b = (p >> 16) & 0xFFu;
                        if (r >= 0x40u && r > g + 0x20u && r > b + 0x20u)
                            ++rectRedish;
                    }
                }

                std::printf("[gsdump] fbp=0x%x%s nonblack=%llu / %llu | rect %u,%u %ux%u: "
                            "total=%llu nonblack=%llu redish=%llu\n",
                            candFbp, (candFbp == fbp) ? " (presented)" : "",
                            static_cast<unsigned long long>(nb),
                            static_cast<unsigned long long>(static_cast<uint64_t>(width) * height),
                            rect.x, rect.y, rect.w, rect.h,
                            static_cast<unsigned long long>(rectTotal),
                            static_cast<unsigned long long>(rectNonBlack),
                            static_cast<unsigned long long>(rectRedish));

                // One BMP per candidate so the box can be eyeballed in
                // whichever buffer actually holds it. Next to the dump, never
                // in the project root.
                char outPath[256];
                std::snprintf(outPath, sizeof(outPath), "gsdump/replay_fbp%02x.bmp", candFbp);
                if (writeBmp(outPath, buf, width, height))
                    std::printf("[gsdump] wrote %s (%ux%u)\n", outPath, width, height);
                else
                    std::printf("[gsdump] could not write %s\n", outPath);

                if (candFbp == fbp)
                {
                    pixels = buf;
                    nonBlack = nb;
                    presentedRectTotal = rectTotal;
                    presentedRectNonBlack = rectNonBlack;
                }
            }

            // Keep the historical default path pointing at the presented
            // buffer so PS2X_GSDUMP_OUT still means what it used to.
            const std::string outPath = envOr("PS2X_GSDUMP_OUT", "gsdump/replay.bmp");
            if (writeBmp(outPath, pixels, width, height))
                std::printf("[gsdump] wrote %s (%ux%u)\n", outPath.c_str(), width, height);
            else
                std::printf("[gsdump] could not write %s\n", outPath.c_str());

            // A frame that draws primitives but leaves the whole framebuffer
            // black means the loss is in our pixel path, not in the dump.
            t.IsTrue(nonBlack > 0u, "the replayed framebuffer should not be entirely black");

            // Stage 5.11's open question, reduced to one assertion: the box
            // region is red on hardware, so a black rect here IS the bug and
            // this test is now its reproduction.
            if (presentedRectTotal > 0u)
            {
                t.IsTrue(presentedRectNonBlack > 0u,
                         "the censused rect should not be entirely black");
            }

            // Process-wide gate: leave it as we found it so later suites do
            // not inherit diagnostic logging from this one.
            ps2_diag::set_enabled_for_test(false);
        });
    });
}
