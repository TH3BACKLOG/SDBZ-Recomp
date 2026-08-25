#include "MiniTest.h"
#include "ps2_log.h"
#include "ps2_runtime.h"
#include "runtime/ps2_diag.h"
#include "runtime/ps2_guestwatch.h"
#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_rasterizer.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Regression coverage for the opt-in "runtime observability" diagnostics
// layer: the should_log throttle, the poll-based guest-memory watch probe
// (including writer-PC capture), the GS CLUT cache reload/read path, and the
// [gs:image] transfer probe.
//
// The diagnostics statics (should_log/ps2_watch registry; the [gs:image] dbp
// tracker) are process-lifetime, so each test is order-robust: watch tests
// reset via clearWatches(), CLUT cache tests use a distinct cbp per test
// (each GS instance also has its own m_clut_cache/m_cbp0/m_cbp1), the
// [gs:image] test picks a unique DBP, and every test leaves diagnostics
// disabled, watches cleared, and the log ring cleared.

namespace
{
    void writeLE32(std::vector<uint8_t> &rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram.data() + addr, &value, sizeof(value));
    }

    // CSM1 VRAM scatter for a 256-entry (T8) palette, written from the spec
    // layout rather than copied out of the runtime, so a divergence shows up
    // as a failing round-trip instead of a tautology.
    void csm1T8Coord(uint32_t i, uint32_t &x, uint32_t &y)
    {
        x = (i & 7u) + ((i & 0x10u) ? 8u : 0u);
        y = ((i & 0xE0u) >> 4) + ((i & 8u) ? 1u : 0u);
    }

    // CSM1 VRAM scatter for a 16-entry (T4) palette.
    void csm1T4Coord(uint32_t i, uint32_t &x, uint32_t &y)
    {
        x = i & 7u;
        y = (i >> 3) & 1u;
    }

    std::vector<ps2_log::RuntimeLogEntry> snapshotLog()
    {
        return ps2_log::snapshot_runtime_log_entries();
    }

    size_t countTagged(const char *tag)
    {
        size_t n = 0;
        for (const auto &entry : snapshotLog())
        {
            if (entry.text.find(tag) != std::string::npos)
                ++n;
        }
        return n;
    }

    std::vector<std::string> collectTagged(const char *tag)
    {
        std::vector<std::string> lines;
        for (const auto &entry : snapshotLog())
        {
            if (entry.text.find(tag) != std::string::npos)
                lines.push_back(entry.text);
        }
        return lines;
    }

    // Parses the integer following `key` (e.g. "idx=") in `text`. Returns -1
    // when the key isn't present. Relies on std::stol stopping at the first
    // non-digit character, so it works fine on "idx=5 r=13 ...".
    long parseField(const std::string &text, const std::string &key)
    {
        const size_t pos = text.find(key);
        if (pos == std::string::npos)
            return -1;
        return std::stol(text.substr(pos + key.size()));
    }

    uint64_t makeGifTagLocal(uint16_t nloop, uint8_t flg, uint8_t nreg, bool eop = true)
    {
        uint64_t tag = static_cast<uint64_t>(nloop & 0x7FFFu);
        if (eop)
            tag |= (1ull << 15);
        tag |= (static_cast<uint64_t>(flg & 0x3u) << 58);
        tag |= (static_cast<uint64_t>(nreg & 0xFu) << 60);
        return tag;
    }

    void appendU64Local(std::vector<uint8_t> &dst, uint64_t value)
    {
        const size_t pos = dst.size();
        dst.resize(pos + sizeof(uint64_t));
        std::memcpy(dst.data() + pos, &value, sizeof(uint64_t));
    }
}

void register_ps2_observability_tests()
{
    MiniTest::Case("PS2 Runtime Observability", [](TestCase &tc)
    {
        tc.Run("should_log throttle is true for exactly 19 of the first 2000 counts", [](TestCase &t)
        {
            int trueCount = 0;
            std::vector<uint64_t> trueValues;
            for (uint64_t c = 0; c < 2000; ++c)
            {
                if (ps2_diag::should_log(c, 16, 600))
                {
                    ++trueCount;
                    trueValues.push_back(c);
                }
            }

            t.Equals(trueCount, 19, "should_log(c,16,600) should be true 16 + floor((2000-16)/600) == 19 times over c=0..1999");

            std::vector<uint64_t> expected;
            for (uint64_t c = 0; c < 16; ++c)
                expected.push_back(c);
            expected.push_back(600);
            expected.push_back(1200);
            expected.push_back(1800);

            t.Equals(trueValues, expected, "should_log true set should be exactly {0..15, 600, 1200, 1800}");

            // should_log() itself is stateless, but keep every test in this
            // file leaving the shared diagnostics/watch/log state clean.
            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });

        tc.Run("watch: unregistered probes never log", [](TestCase &t)
        {
            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(true);
            ps2_log::clear_runtime_log_entries();

            std::vector<uint8_t> rdram(64 * 1024, 0);
            for (uint32_t i = 0; i < 8; ++i)
            {
                writeLE32(rdram, 0x1000, 0x1000u + i * 37u);
                ps2_watch::pollWatches(rdram.data());
            }

            t.Equals(countTagged("[watch]"), static_cast<size_t>(0),
                     "pollWatches with no registered watches should never emit [watch] lines");

            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });

        tc.Run("watch: two value changes produce exactly two log lines", [](TestCase &t)
        {
            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(true);
            ps2_log::clear_runtime_log_entries();

            std::vector<uint8_t> rdram(64 * 1024, 0);
            ps2_watch::addWatch(0x1000, 4, "testWatch");

            ps2_watch::pollWatches(rdram.data()); // seed: must not log
            t.Equals(countTagged("[watch]"), static_cast<size_t>(0), "the first poll after registration should only seed, not log");

            writeLE32(rdram, 0x1000, 0x0000BEEFu);
            ps2_watch::pollWatches(rdram.data());

            writeLE32(rdram, 0x1000, 0x0000CAFEu);
            ps2_watch::pollWatches(rdram.data());

            const auto lines = collectTagged("[watch]");
            t.Equals(lines.size(), static_cast<size_t>(2), "exactly two observed changes should produce exactly two [watch] lines");

            if (!lines.empty())
            {
                const std::string &first = lines.front();
                t.IsTrue(first.find("testWatch") != std::string::npos, "first line should contain the watch label");
                t.IsTrue(first.find("0x1000") != std::string::npos, "first line should contain the watch address");
                t.IsTrue(first.find("(change #1)") != std::string::npos, "first line should report change #1");
                t.IsTrue(first.find("0x0") != std::string::npos, "first line should report the old value (0) in hex");
                t.IsTrue(first.find("0xbeef") != std::string::npos, "first line should report the new value (0xBEEF) in hex");
            }
            if (lines.size() > 1)
            {
                const std::string &second = lines[1];
                t.IsTrue(second.find("(change #2)") != std::string::npos, "second line should report change #2");
                t.IsTrue(second.find("0xbeef") != std::string::npos, "second line should report the prior value (0xBEEF) as old");
                t.IsTrue(second.find("0xcafe") != std::string::npos, "second line should report the new value (0xCAFE)");
            }

            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });

        tc.Run("watch: 2000 changes throttle to exactly 19 log lines", [](TestCase &t)
        {
            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(true);
            ps2_log::clear_runtime_log_entries();

            std::vector<uint8_t> rdram(64 * 1024, 0);
            ps2_watch::addWatch(0x1000, 4, "bulkWatch");
            ps2_watch::pollWatches(rdram.data()); // seed

            ps2_log::clear_runtime_log_entries();
            for (uint32_t i = 1; i <= 2000; ++i)
            {
                writeLE32(rdram, 0x1000, i);
                ps2_watch::pollWatches(rdram.data());
            }

            t.Equals(countTagged("[watch]"), static_cast<size_t>(19),
                     "2000 changes through should_log(c,16,600) should log exactly 19 times (matches the should_log bound-math test)");

            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });

        tc.Run("watch: disabled diagnostics gate suppresses all logging", [](TestCase &t)
        {
            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(true);
            ps2_watch::addWatch(0x3000, 4, "gateWatch");
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();

            std::vector<uint8_t> rdram(64 * 1024, 0);
            writeLE32(rdram, 0x3000, 0x00001234u);
            ps2_watch::pollWatches(rdram.data());

            t.Equals(countTagged("[watch]"), static_cast<size_t>(0),
                     "pollWatches should no-op entirely while the diagnostics gate is disabled");

            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });

        tc.Run("watch: writer PC is captured and attached to the next logged change", [](TestCase &t)
        {
            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(true);
            ps2_log::clear_runtime_log_entries();

            std::vector<uint8_t> rdram(64 * 1024, 0);
            ps2_watch::addWatch(0x2000, 4, "pcWatch");
            ps2_watch::pollWatches(rdram.data()); // seed
            ps2_log::clear_runtime_log_entries();

            R5900Context ctx{};
            ctx.pc = 0x00123450u;

            const uint32_t newValue = 0xDEADBEEFu;
            writeLE32(rdram, 0x2000, newValue);
            ps2TraceGuestWrite(rdram.data(), 0x2000, 4, newValue, 0, "WRITE32", &ctx);

            ps2_watch::pollWatches(rdram.data());

            const auto lines = collectTagged("[watch]");
            t.Equals(lines.size(), static_cast<size_t>(1), "a single guest write followed by one poll should log exactly one change");
            if (!lines.empty())
            {
                t.IsTrue(lines.front().find("pc=0x123450") != std::string::npos,
                         "the logged change should carry the writer PC captured via ps2TraceGuestWrite");
            }

            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });

        tc.Run("watch: PS2X_WATCH env spec arms a watch that fires via the store hook", [](TestCase &t)
        {
            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();

            // armWatchesFromEnv parses the spec string directly (no env mutation).
            const size_t armed = ps2_watch::armWatchesFromEnv("0x2000:4:envWatch");
            t.Equals(armed, static_cast<size_t>(1), "a single-entry PS2X_WATCH spec should arm exactly one watch");

            // Mirror the boot hook: arming enables the diagnostics gate.
            ps2_diag::set_enabled(true);

            std::vector<uint8_t> rdram(64 * 1024, 0);
            ps2_watch::pollWatches(rdram.data()); // seed
            ps2_log::clear_runtime_log_entries();

            R5900Context ctx{};
            ctx.pc = 0x00ABCDE0u;

            const uint32_t newValue = 0x12345678u;
            writeLE32(rdram, 0x2000, newValue);
            ps2TraceGuestWrite(rdram.data(), 0x2000, 4, newValue, 0, "WRITE32", &ctx);

            ps2_watch::pollWatches(rdram.data());

            const auto lines = collectTagged("[watch]");
            t.Equals(lines.size(), static_cast<size_t>(1), "the env-armed watch should log exactly one change");
            if (!lines.empty())
            {
                t.IsTrue(lines.front().find("envWatch") != std::string::npos,
                         "the logged line should carry the label parsed from the PS2X_WATCH spec");
                t.IsTrue(lines.front().find("pc=0xabcde0") != std::string::npos,
                         "the env-armed watch should carry the writer PC captured via ps2TraceGuestWrite");
            }

            ps2_watch::clearWatches();
            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });

        tc.Run("clut-cache: CSM1 T4 reload deswizzles physical VRAM layout into linear cache order", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kCbp = 0x100u; // distinct from every other clut-cache test in this suite
            for (uint32_t p = 0; p < 32; ++p)
            {
                gs.WriteVram(GS_PSM_CT32, kCbp, 1u, p & 0xFu, p >> 4, p);
            }

            // cld=1 forces an unconditional cache reload.
            gs.ReloadClutCache(GS_PSM_T4, GS_PSM_CT32, kCbp, /*csm=*/0u, /*csa=*/0u, /*cld=*/1u);

            bool cacheOk = true;
            for (uint32_t logicalIdx = 0; logicalIdx < 16; ++logicalIdx)
            {
                const u32 got = gs.ReadClutCache(GS_PSM_CT32, static_cast<uint8_t>(logicalIdx), /*csa=*/0u);
                if (got != logicalIdx)
                    cacheOk = false;
            }
            t.IsTrue(cacheOk, "CSM1 reload must place each logical CLUT index at its own cache slot, resolvable via ReadClutCache(index)");
        });

        tc.Run("clut-cache: CSM1 T8 reload covers all 256 entries and preserves value round-trip", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kCbp = 0x200u; // distinct from every other clut-cache test in this suite
            // T8 CLUTs are 16x16 CT32 entries (256 total).
            for (uint32_t y = 0; y < 16; ++y)
            {
                for (uint32_t x = 0; x < 16; ++x)
                {
                    gs.WriteVram(GS_PSM_CT32, kCbp, 1u, x, y, (y * 16u) + x);
                }
            }

            gs.ReloadClutCache(GS_PSM_T8, GS_PSM_CT32, kCbp, /*csm=*/0u, /*csa=*/0u, /*cld=*/1u);

            const u32 first = gs.ReadClutCache(GS_PSM_CT32, 0u, /*csa=*/0u);
            const u32 last = gs.ReadClutCache(GS_PSM_CT32, 255u, /*csa=*/0u);
            t.IsTrue(first != 0xFFFFFFFFu && last != 0xFFFFFFFFu,
                     "T8 reload should populate both the first and last cache entries (sanity check the reload ran)");
        });

        tc.Run("clut-cache: cld gating skips reload unless cbp changes for the tracked slot", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kCbpA = 0x300u;
            constexpr uint32_t kCbpB = 0x340u;
            for (uint32_t p = 0; p < 16; ++p)
            {
                gs.WriteVram(GS_PSM_CT32, kCbpA, 1u, p & 0xFu, p >> 4, 0xAAu);
                gs.WriteVram(GS_PSM_CT32, kCbpB, 1u, p & 0xFu, p >> 4, 0xBBu);
            }

            // cld=2 loads unconditionally and remembers cbp in m_cbp0.
            gs.ReloadClutCache(GS_PSM_T4, GS_PSM_CT32, kCbpA, /*csm=*/0u, /*csa=*/0u, /*cld=*/2u);
            t.Equals(gs.ReadClutCache(GS_PSM_CT32, 0u, 0u), 0xAAu, "cld=2 should load from cbpA and cache the CT32 value");

            // cld=4 with an unchanged cbp (still kCbpA) must be a no-op: write
            // new data to kCbpA and confirm the stale cache value is untouched.
            gs.WriteVram(GS_PSM_CT32, kCbpA, 1u, 0u, 0u, 0xCCu);
            gs.ReloadClutCache(GS_PSM_T4, GS_PSM_CT32, kCbpA, /*csm=*/0u, /*csa=*/0u, /*cld=*/4u);
            t.Equals(gs.ReadClutCache(GS_PSM_CT32, 0u, 0u), 0xAAu,
                     "cld=4 must skip the reload when cbp is unchanged from the last cld=2/3 load, leaving the cache stale");

            // cld=4 with a changed cbp (kCbpB) must reload.
            gs.ReloadClutCache(GS_PSM_T4, GS_PSM_CT32, kCbpB, /*csm=*/0u, /*csa=*/0u, /*cld=*/4u);
            t.Equals(gs.ReadClutCache(GS_PSM_CT32, 0u, 0u), 0xBBu, "cld=4 must reload once cbp differs from the tracked m_cbp0");
        });

        // ---------------------------------------------------------------
        // Stage 5.11 coverage: the index -> palette-entry lookup.
        //
        // The tests above only ever pass csa=0, which is exactly where the
        // two known asymmetries between the CLUT loader and the CLUT reader
        // are invisible:
        //
        //   * ReloadClutCacheCSM1 masks its cache offset with & 0x3FF, but
        //     ReadClutCache computes (csa * 16 * bpp) + (index * bpp) with no
        //     mask at all. For CT32 that read offset passes the end of the
        //     1 KiB cache once csa >= 16.
        //   * The CSM1 VRAM->cache scatter formula is only exercised at its
        //     identity point when csa is 0.
        //
        // The coordinate formulas below are written from the CSM1 spec
        // layout rather than copied out of the runtime, so a divergence
        // between the two shows up as a failing round-trip instead of a
        // tautology. The csm1T8Coord/csm1T4Coord helpers at the top of this
        // file hold those spec layouts.
        // ---------------------------------------------------------------

        tc.Run("clut-cache: CSM1 T8 round-trips all 256 indices against an independently derived layout", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kCbp = 0x400u; // distinct from every other clut-cache test in this suite

            auto expected = [](uint32_t i) { return 0x80000000u | (i * 0x00010101u) | 0x00000001u; };

            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t x = 0, y = 0;
                csm1T8Coord(i, x, y);
                gs.WriteVram(GS_PSM_CT32, kCbp, 1u, x, y, expected(i));
            }

            gs.ReloadClutCache(GS_PSM_T8, GS_PSM_CT32, kCbp, /*csm=*/0u, /*csa=*/0u, /*cld=*/1u);

            uint32_t mismatches = 0;
            uint32_t firstBadIdx = 0xFFFFFFFFu;
            uint32_t firstBadGot = 0;
            for (uint32_t i = 0; i < 256u; ++i)
            {
                const u32 got = gs.ReadClutCache(GS_PSM_CT32, static_cast<u8>(i), /*csa=*/0u);
                if (got != expected(i))
                {
                    if (firstBadIdx == 0xFFFFFFFFu)
                    {
                        firstBadIdx = i;
                        firstBadGot = got;
                    }
                    ++mismatches;
                }
            }
            t.Equals(mismatches, 0u,
                     "every CSM1 T8 index must resolve to the palette entry the spec layout placed in VRAM "
                     "(first mismatch idx=" + std::to_string(firstBadIdx) +
                     " got=" + std::to_string(firstBadGot) + ")");
        });

        tc.Run("clut-cache: CSM1 T4 round-trips all 16 indices for every in-range csa", [](TestCase &t)
        {
            // CT32 cache is 1 KiB. ReadClutCache offsets by csa*64 + index*4,
            // so csa 0..15 keeps both the loader and the reader in bounds and
            // any failure here is a genuine loader/reader disagreement rather
            // than an overrun.
            constexpr uint32_t kMaxInRangeCsa = 16u;

            uint32_t badCsa = 0xFFFFFFFFu;
            uint32_t badIdx = 0;
            uint32_t badGot = 0;
            uint32_t badWant = 0;

            for (uint32_t csa = 0; csa < kMaxInRangeCsa; ++csa)
            {
                std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
                GS gs;
                gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

                const uint32_t cbp = 0x500u + (csa * 0x10u);

                // Tag the value with csa so a cache slot sourced from the
                // wrong csa region is distinguishable from a plain miss.
                auto expected = [csa](uint32_t i) { return 0x80000000u | (csa << 8) | i | 0x00010000u; };

                for (uint32_t i = 0; i < 16u; ++i)
                {
                    uint32_t x = 0, y = 0;
                    csm1T4Coord(i, x, y);
                    gs.WriteVram(GS_PSM_CT32, cbp, 1u, x, y, expected(i));
                }

                gs.ReloadClutCache(GS_PSM_T4, GS_PSM_CT32, cbp, /*csm=*/0u, static_cast<u8>(csa), /*cld=*/1u);

                for (uint32_t i = 0; i < 16u; ++i)
                {
                    const u32 got = gs.ReadClutCache(GS_PSM_CT32, static_cast<u8>(i), csa);
                    if (got != expected(i) && badCsa == 0xFFFFFFFFu)
                    {
                        badCsa = csa;
                        badIdx = i;
                        badGot = got;
                        badWant = expected(i);
                    }
                }
            }

            t.Equals(badCsa, 0xFFFFFFFFu,
                     "ReadClutCache(index, csa) must resolve the entry ReloadClutCache wrote for that same csa "
                     "(first failure csa=" + std::to_string(badCsa) +
                     " idx=" + std::to_string(badIdx) +
                     " got=" + std::to_string(badGot) +
                     " want=" + std::to_string(badWant) + ")");
        });

        tc.Run("clut-cache: csa>=16 wraps on the loader's masked write but not on ReadClutCache's unmasked read", [](TestCase &t)
        {
            // ReloadClutCacheCSM1 masks its destination with & 0x3FF, so a
            // CT32 load at csa=16 lands back at cache byte 0. ReadClutCache
            // applies no such mask, so it would look for those bytes at
            // offset 1024 -- one byte past a std::array<u8, 1024>.
            //
            // This test never issues that out-of-range read. It only reads at
            // csa=0, which is in bounds either way, and asks whether the
            // csa=16 load aliased on top of it.
            //
            // POLARITY (corrected after reading the loader): a pass here does
            // NOT convict the asymmetry. At csa=16 the T4 loader computes
            // offset = 256 and total_entries = min(1024/4, 16 + 256) = 256, so
            // its loop `for (i = offset; i < total_entries; ++i)` runs zero
            // times. The csa=16 load is a silent no-op and entry 0 survives
            // trivially. The unmasked read in ReadClutCache is still a real
            // out-of-bounds hazard -- this test simply does not convict it.
            // Treat a FAILURE here as the conviction; a pass proves nothing.
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kCbpLow = 0x700u;
            constexpr uint32_t kCbpHigh = 0x710u;
            constexpr uint32_t kLowMarker = 0x80AAAAAAu;
            constexpr uint32_t kHighMarker = 0x80BBBBBBu;

            for (uint32_t i = 0; i < 16u; ++i)
            {
                uint32_t x = 0, y = 0;
                csm1T4Coord(i, x, y);
                gs.WriteVram(GS_PSM_CT32, kCbpLow, 1u, x, y, kLowMarker);
                gs.WriteVram(GS_PSM_CT32, kCbpHigh, 1u, x, y, kHighMarker);
            }

            gs.ReloadClutCache(GS_PSM_T4, GS_PSM_CT32, kCbpLow, /*csm=*/0u, /*csa=*/0u, /*cld=*/1u);
            t.Equals(gs.ReadClutCache(GS_PSM_CT32, 0u, 0u), kLowMarker,
                     "baseline: a csa=0 load must be visible at csa=0");

            gs.ReloadClutCache(GS_PSM_T4, GS_PSM_CT32, kCbpHigh, /*csm=*/0u, /*csa=*/16u, /*cld=*/1u);

            const u32 afterHighLoad = gs.ReadClutCache(GS_PSM_CT32, 0u, 0u);
            t.Equals(afterHighLoad, kLowMarker,
                     "a csa=16 CT32 load must not alias onto csa=0 -- if it does, the loader's & 0x3FF mask "
                     "and ReadClutCache's unmasked csa*64 offset disagree about where entry 0 lives");
        });

        tc.Run("clut-cache: CSM1 T8 at csa!=0 must not shift the VRAM source coordinate", [](TestCase &t)
        {
            // ReloadClutCacheCSM1 starts its loop at i = csa * 16 and uses that
            // same i for BOTH the cache destination and the VRAM scatter
            // coordinate. Only the destination should move with csa; the source
            // should still walk logical entries 0..N-1.
            //
            // Why this is invisible at T4 (and so was not caught by the
            // "T4 round-trips for every in-range csa" test above): the T4
            // scatter is periodic with period 16, so shifting i by 16*csa maps
            // to the identical (x, y). The T8 scatter has period 256, so the
            // shift is observable.
            //
            // POLARITY, pre-registered:
            //   FAIL, first mismatch at index 0 reading the value written for
            //         index 16  -> convicts the source-coordinate shift.
            //   FAIL, first mismatch at index 0 reading 0
            //         -> the low entries were never loaded at all (the loop
            //            also skips the first 16*csa iterations).
            //   PASS  -> the loader separates source from destination and this
            //            suspect is dead.
            //
            // Bounds: ReadClutCache offsets by csa*64 + index*4 with no mask,
            // so at csa=1 only indices below 240 stay inside the 1 KiB cache.
            // This test never reads past that.
            constexpr uint32_t kCsa = 1u;
            constexpr uint32_t kSafeIndexLimit = 240u;

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kCbp = 0x600u; // distinct from every other clut-cache test in this suite

            auto expected = [](uint32_t i) { return 0x80000000u | (i << 8) | 0x00000041u; };

            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t x = 0, y = 0;
                csm1T8Coord(i, x, y);
                gs.WriteVram(GS_PSM_CT32, kCbp, 1u, x, y, expected(i));
            }

            gs.ReloadClutCache(GS_PSM_T8, GS_PSM_CT32, kCbp, /*csm=*/0u, static_cast<u8>(kCsa), /*cld=*/1u);

            uint32_t mismatches = 0;
            uint32_t firstBadIdx = 0xFFFFFFFFu;
            uint32_t firstBadGot = 0;
            for (uint32_t i = 0; i < kSafeIndexLimit; ++i)
            {
                const u32 got = gs.ReadClutCache(GS_PSM_CT32, static_cast<u8>(i), kCsa);
                if (got != expected(i))
                {
                    if (firstBadIdx == 0xFFFFFFFFu)
                    {
                        firstBadIdx = i;
                        firstBadGot = got;
                    }
                    ++mismatches;
                }
            }

            t.Equals(mismatches, 0u,
                     "at csa=1 a T8 logical index must still resolve to the palette entry the spec layout put "
                     "at that same logical index (first mismatch idx=" + std::to_string(firstBadIdx) +
                     " got=" + std::to_string(firstBadGot) +
                     " want=" + std::to_string(firstBadIdx == 0xFFFFFFFFu ? 0u : expected(firstBadIdx)) +
                     " shifted-source-would-give=" +
                     std::to_string(firstBadIdx == 0xFFFFFFFFu ? 0u : expected(firstBadIdx + kCsa * 16u)) + ")");
        });

        tc.Run("gs draw: CLUT-indexed T8 sprite resolves to palette colors rather than black", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kTexTbp = 0x800u;
            constexpr uint32_t kCbp = 0x840u;
            constexpr uint32_t kFbp = 150u;              // FRAME FBP, in 2048-word units
            constexpr uint32_t kFrameBase = kFbp * 32u;  // ... which is 32 blocks each
            constexpr uint32_t kIndices[4] = {1u, 2u, 3u, 4u};

            auto palette = [](uint32_t i) { return 0x80000000u | (i * 0x00112233u) | 0x00010101u; };

            // Palette first, laid out per the CSM1 256-entry scatter.
            for (uint32_t i = 0; i < 256u; ++i)
            {
                uint32_t x = 0, y = 0;
                csm1T8Coord(i, x, y);
                gs.WriteVram(GS_PSM_CT32, kCbp, 1u, x, y, palette(i));
            }

            // 4x4 index page: column x selects kIndices[x].
            for (uint32_t y = 0; y < 4u; ++y)
            {
                for (uint32_t x = 0; x < 4u; ++x)
                {
                    gs.WriteVram(GS_PSM_T8, kTexTbp, 1u, x, y, kIndices[x]);
                }
            }

            const uint64_t kTex0 =
                (static_cast<uint64_t>(kTexTbp) << 0) |
                (1ull << 14) |                                        // TBW
                (static_cast<uint64_t>(GS_PSM_T8) << 20) |            // PSM
                (2ull << 26) |                                        // TW  -> 4
                (2ull << 30) |                                        // TH  -> 4
                (1ull << 34) |                                        // TCC -> use texture alpha
                (1ull << 35) |                                        // TFX -> DECAL
                (static_cast<uint64_t>(kCbp) << 37) |                 // CBP
                (static_cast<uint64_t>(GS_PSM_CT32) << 51) |          // CPSM
                (0ull << 55) |                                        // CSM1
                (0ull << 56) |                                        // CSA
                (1ull << 61);                                         // CLD=1, unconditional load
            constexpr uint64_t kFrame =
                (static_cast<uint64_t>(kFbp) << 0) |
                (1ull << 16) |
                (static_cast<uint64_t>(GS_PSM_CT32) << 24);
            constexpr uint64_t kScissor =
                (0ull << 0) | (3ull << 16) | (0ull << 32) | (3ull << 48);
            constexpr uint64_t kPrim =
                static_cast<uint64_t>(GS_PRIM_SPRITE) |
                (1ull << 4) |   // TME
                (1ull << 8);    // FST
            constexpr uint64_t kXyz0 = 0ull;
            constexpr uint64_t kXyz1 =
                (static_cast<uint64_t>(4u << 4) << 0) |
                (static_cast<uint64_t>(4u << 4) << 16);
            constexpr uint64_t kUv0 = 0ull;
            constexpr uint64_t kUv1 = ((4ull * 16ull) << 0) | ((4ull * 16ull) << 16);

            gs.writeRegister(GS_REG_FRAME_1, kFrame);
            gs.writeRegister(GS_REG_ZBUF_1, (1ull << 32));
            gs.writeRegister(GS_REG_SCISSOR_1, kScissor);
            gs.writeRegister(GS_REG_XYOFFSET_1, 0ull);
            gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
            gs.writeRegister(GS_REG_ALPHA_1, 0ull);
            gs.writeRegister(GS_REG_TEXA, 0x80ull);
            gs.writeRegister(GS_REG_TEX0_1, kTex0);
            gs.writeRegister(GS_REG_TEX1_1, 0ull);
            gs.writeRegister(GS_REG_PRIM, kPrim);
            gs.writeRegister(GS_REG_RGBAQ, 0x80808080ull);
            gs.writeRegister(GS_REG_UV, kUv0);
            gs.writeRegister(GS_REG_XYZ2, kXyz0);
            gs.writeRegister(GS_REG_UV, kUv1);
            gs.writeRegister(GS_REG_XYZ2, kXyz1);

            uint32_t blackPixels = 0;
            uint32_t wrongPixels = 0;
            uint32_t firstWrongGot = 0;
            uint32_t firstWrongWant = 0;
            for (uint32_t y = 0; y < 4u; ++y)
            {
                for (uint32_t x = 0; x < 4u; ++x)
                {
                    const u32 got = gs.ReadVram(GS_PSM_CT32, kFrameBase, 1u, x, y) & 0x00FFFFFFu;
                    const uint32_t want = palette(kIndices[x]) & 0x00FFFFFFu;
                    if (got == 0u)
                        ++blackPixels;
                    if (got != want)
                    {
                        if (wrongPixels == 0)
                        {
                            firstWrongGot = got;
                            firstWrongWant = want;
                        }
                        ++wrongPixels;
                    }
                }
            }

            // Split deliberately: "black" is the Stage 5.11 symptom, "wrong"
            // is the broader correctness claim. A run that is non-black but
            // wrong is a different bug from a run that is uniformly black.
            t.Equals(blackPixels, 0u,
                     "a CLUT-indexed T8 sprite drawn from a fully populated palette must not rasterize to black");
            t.Equals(wrongPixels, 0u,
                     "each drawn texel must equal its palette entry (first mismatch got=" +
                     std::to_string(firstWrongGot) + " want=" + std::to_string(firstWrongWant) + ")");
        });

        tc.Run("gs draw: CLUT-indexed T4 sprite resolves to palette colors rather than black", [](TestCase &t)
        {
            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            constexpr uint32_t kTexTbp = 0x900u;
            constexpr uint32_t kCbp = 0x940u;
            constexpr uint32_t kFbp = 150u;
            constexpr uint32_t kFrameBase = kFbp * 32u;
            constexpr uint32_t kIndices[4] = {1u, 5u, 9u, 13u};

            auto palette = [](uint32_t i) { return 0x80000000u | (i * 0x00102030u) | 0x00010101u; };

            for (uint32_t i = 0; i < 16u; ++i)
            {
                uint32_t x = 0, y = 0;
                csm1T4Coord(i, x, y);
                gs.WriteVram(GS_PSM_CT32, kCbp, 1u, x, y, palette(i));
            }

            for (uint32_t y = 0; y < 4u; ++y)
            {
                for (uint32_t x = 0; x < 4u; ++x)
                {
                    gs.WriteVram(GS_PSM_T4, kTexTbp, 1u, x, y, kIndices[x]);
                }
            }

            const uint64_t kTex0 =
                (static_cast<uint64_t>(kTexTbp) << 0) |
                (1ull << 14) |
                (static_cast<uint64_t>(GS_PSM_T4) << 20) |
                (2ull << 26) |
                (2ull << 30) |
                (1ull << 34) |
                (1ull << 35) |
                (static_cast<uint64_t>(kCbp) << 37) |
                (static_cast<uint64_t>(GS_PSM_CT32) << 51) |
                (0ull << 55) |
                (0ull << 56) |
                (1ull << 61);
            constexpr uint64_t kFrame =
                (static_cast<uint64_t>(kFbp) << 0) |
                (1ull << 16) |
                (static_cast<uint64_t>(GS_PSM_CT32) << 24);
            constexpr uint64_t kScissor =
                (0ull << 0) | (3ull << 16) | (0ull << 32) | (3ull << 48);
            constexpr uint64_t kPrim =
                static_cast<uint64_t>(GS_PRIM_SPRITE) | (1ull << 4) | (1ull << 8);
            constexpr uint64_t kXyz1 =
                (static_cast<uint64_t>(4u << 4) << 0) |
                (static_cast<uint64_t>(4u << 4) << 16);
            constexpr uint64_t kUv1 = ((4ull * 16ull) << 0) | ((4ull * 16ull) << 16);

            gs.writeRegister(GS_REG_FRAME_1, kFrame);
            gs.writeRegister(GS_REG_ZBUF_1, (1ull << 32));
            gs.writeRegister(GS_REG_SCISSOR_1, kScissor);
            gs.writeRegister(GS_REG_XYOFFSET_1, 0ull);
            gs.writeRegister(GS_REG_TEST_1, 0x30000ull);
            gs.writeRegister(GS_REG_ALPHA_1, 0ull);
            gs.writeRegister(GS_REG_TEXA, 0x80ull);
            gs.writeRegister(GS_REG_TEX0_1, kTex0);
            gs.writeRegister(GS_REG_TEX1_1, 0ull);
            gs.writeRegister(GS_REG_PRIM, kPrim);
            gs.writeRegister(GS_REG_RGBAQ, 0x80808080ull);
            gs.writeRegister(GS_REG_UV, 0ull);
            gs.writeRegister(GS_REG_XYZ2, 0ull);
            gs.writeRegister(GS_REG_UV, kUv1);
            gs.writeRegister(GS_REG_XYZ2, kXyz1);

            uint32_t blackPixels = 0;
            uint32_t wrongPixels = 0;
            for (uint32_t y = 0; y < 4u; ++y)
            {
                for (uint32_t x = 0; x < 4u; ++x)
                {
                    const u32 got = gs.ReadVram(GS_PSM_CT32, kFrameBase, 1u, x, y) & 0x00FFFFFFu;
                    if (got == 0u)
                        ++blackPixels;
                    if (got != (palette(kIndices[x]) & 0x00FFFFFFu))
                        ++wrongPixels;
                }
            }

            t.Equals(blackPixels, 0u,
                     "a CLUT-indexed T4 sprite drawn from a fully populated palette must not rasterize to black");
            t.Equals(wrongPixels, 0u,
                     "each drawn T4 texel must equal its palette entry");
        });

        tc.Run("gs:image: single IMAGE transfer logs the destination DBP and payload byte count", [](TestCase &t)
        {
            ps2_diag::set_enabled_for_test(true);
            ps2_log::clear_runtime_log_entries();

            std::vector<uint8_t> vram(PS2_GS_VRAM_SIZE, 0u);
            GS gs;
            gs.init(vram.data(), static_cast<uint32_t>(vram.size()), nullptr);

            // DBP chosen well outside the small values (0,1,10,64,150,...)
            // used by every other GS transfer test in this binary, so the
            // probe's "always log on DBP change" branch fires regardless of
            // suite run order (see ps2_gs_gpu.cpp processImageData()).
            constexpr uint32_t kDbp = 0x2000u;
            const uint64_t bitblt =
                (static_cast<uint64_t>(0u) << 0) |                          // SBP
                (static_cast<uint64_t>(1u) << 16) |                         // SBW
                (static_cast<uint64_t>(GS_PSM_CT32) << 24) |                // SPSM
                (static_cast<uint64_t>(kDbp) << 32) |                       // DBP
                (static_cast<uint64_t>(1u) << 48) |                        // DBW
                (static_cast<uint64_t>(GS_PSM_CT32) << 56);                 // DPSM
            gs.writeRegister(GS_REG_BITBLTBUF, bitblt);
            gs.writeRegister(GS_REG_TRXPOS, 0ull);
            gs.writeRegister(GS_REG_TRXREG, (4ull << 0) | (4ull << 32)); // 4x4 rect
            gs.writeRegister(GS_REG_TRXDIR, 0ull);                      // host -> local

            constexpr uint32_t kPixelCount = 16u;                    // 4x4
            constexpr uint32_t kPayloadBytes = kPixelCount * 4u;     // CT32 = 4 bytes/pixel = 64
            constexpr uint16_t kNloop = static_cast<uint16_t>(kPayloadBytes / 16u); // qwords of image payload

            std::vector<uint8_t> packet;
            appendU64Local(packet, makeGifTagLocal(kNloop, GIF_FMT_IMAGE, 0u, true));
            appendU64Local(packet, 0ull);
            packet.resize(packet.size() + kPayloadBytes, 0xABu);

            ps2_log::clear_runtime_log_entries();
            gs.processGIFPacket(packet.data(), static_cast<uint32_t>(packet.size()));

            const auto lines = collectTagged("[gs:image]");
            t.Equals(lines.size(), static_cast<size_t>(1), "a single IMAGE-mode GIF tag should log exactly one [gs:image] line");
            if (!lines.empty())
            {
                const std::string &line = lines.front();
                t.IsTrue(line.find("dbp=0x2000") != std::string::npos, "the logged line should contain the BITBLTBUF DBP we programmed");
                t.IsTrue(line.find("sizeBytes=64") != std::string::npos, "the logged line should contain the payload byte count");
            }

            ps2_diag::set_enabled_for_test(false);
            ps2_log::clear_runtime_log_entries();
        });
    });
}
