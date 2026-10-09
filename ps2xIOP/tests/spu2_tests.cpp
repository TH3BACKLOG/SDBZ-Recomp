// SPU2 device model tests (E1). No IOP, no guest: drive spu2.h directly.
#include "emulator/core/spu2.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    using namespace ps2x::iop;

    int g_fail = 0;

#define CHECK(cond)                                                              \
    do                                                                           \
    {                                                                            \
        if (!(cond))                                                             \
        {                                                                        \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);          \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

    // core 0 register helpers (offsets per PCSX2 regs.h)
    void setVoice(int v, uint16_t volL, uint16_t volR, uint16_t pitch, uint16_t adsr1, uint16_t adsr2, uint32_t ssaWords)
    {
        const uint32_t b = static_cast<uint32_t>(v) * 16u;
        spu2::writeReg(b + 0, volL);
        spu2::writeReg(b + 2, volR);
        spu2::writeReg(b + 4, pitch);
        spu2::writeReg(b + 6, adsr1);
        spu2::writeReg(b + 8, adsr2);
        const uint32_t a = 0x1C0u + static_cast<uint32_t>(v) * 12u;
        spu2::writeReg(a + 0, static_cast<uint16_t>(ssaWords >> 16));
        spu2::writeReg(a + 2, static_cast<uint16_t>(ssaWords & 0xFFFFu));
    }

    void setTsa(uint32_t words)
    {
        spu2::writeReg(0x1A8, static_cast<uint16_t>(words >> 16));
        spu2::writeReg(0x1AA, static_cast<uint16_t>(words & 0xFFFFu));
    }

    void routeAll()
    {
        // voices 0-23 -> dry L and R, core 0 mix dry, core 1 takes core 0 as external input, masters full
        spu2::writeReg(0x188, 0xFFFF);
        spu2::writeReg(0x18A, 0x00FF);
        spu2::writeReg(0x190, 0xFFFF);
        spu2::writeReg(0x192, 0x00FF);
        spu2::writeReg(0x198, 0x0FF0);
        spu2::writeReg(0x760, 0x3FFF);
        spu2::writeReg(0x762, 0x3FFF);
        spu2::writeReg(0x400 + 0x198, 0x0FFC);
        spu2::writeReg(0x788, 0x3FFF);
        spu2::writeReg(0x78A, 0x3FFF);
        spu2::writeReg(0x790, 0x7FFF);
        spu2::writeReg(0x792, 0x7FFF);
    }

    // One ADPCM block: filter 0, shift 0 => sample = nibble << 12. flags in byte 1.
    std::vector<uint8_t> block(uint8_t flags, uint8_t nibblePattern)
    {
        std::vector<uint8_t> b(16, 0);
        b[0] = 0x00;
        b[1] = flags;
        for (int i = 2; i < 16; ++i)
            b[i] = nibblePattern;
        return b;
    }

    void run(uint64_t &cycles, int frames)
    {
        cycles += static_cast<uint64_t>(frames) * spu2::kCyclesPerFrame;
        spu2::advance(cycles);
    }

    uint16_t readEndx()
    {
        uint16_t v = 0;
        if (!spu2::readLive(0x340, &v))
            return 0xDEAD;
        return v;
    }

    void testEndxLatchAndKon()
    {
        spu2::reset();
        routeAll();
        CHECK(readEndx() == 0xFFFF); // PS2 reset value: all ended

        // block 0 (start flag), block 1 (loop end, no loop) => voice stops after block 1
        std::vector<uint8_t> data = block(0x04, 0x11);
        const auto b1 = block(0x01, 0x11);
        data.insert(data.end(), b1.begin(), b1.end());
        setTsa(0x3000);
        spu2::dmaToSpu(0, data.data(), data.size());

        setVoice(0, 0x3FFF, 0x3FFF, 0x1000, 0x80FF, 0x1FC0, 0x3000);
        spu2::writeReg(0x1A0, 0x0001); // KON voice 0
        CHECK((readEndx() & 1u) == 0u); // KON clears ENDX

        uint64_t cycles = 0;
        run(cycles, 20);
        CHECK((readEndx() & 1u) == 0u); // still inside block 0
        run(cycles, 80);               // 2 blocks = 56 samples at pitch 0x1000
        CHECK((readEndx() & 1u) == 1u); // end flag passed
        uint16_t nax = 0;
        CHECK(spu2::readLive(0x1C0 + 8 + 2, &nax)); // voice 0 NAX lo readable
    }

    void testOutputAndPitch()
    {
        spu2::reset();
        routeAll();
        // looping block of constant +0x1000 samples (nibble 1 => 0x1000 per sample)
        std::vector<uint8_t> data = block(0x07, 0x11); // start+loop+end => loops forever
        setTsa(0x3000);
        spu2::dmaToSpu(0, data.data(), data.size());
        setVoice(0, 0x3FFF, 0x3FFF, 0x1000, 0x80FF, 0x1FC0, 0x3000);
        spu2::writeReg(0x1A0, 0x0001);
        uint64_t cycles = 0;
        run(cycles, 2000);
        std::vector<int16_t> pcm(2 * 8192);
        const size_t n = spu2::popFrames(pcm.data(), 2000);
        CHECK(n > 1000);
        int nonzero = 0;
        for (size_t i = n / 2; i < n; ++i)
            if (pcm[i * 2] != 0 && pcm[i * 2 + 1] != 0)
                ++nonzero;
        CHECK(nonzero > static_cast<int>(n / 4)); // audible, both channels
        CHECK((readEndx() & 1u) == 1u);            // looped through the end flag

        // key off => release => silence
        spu2::writeReg(0x1A4, 0x0001);
        run(cycles, 4000);
        spu2::popFrames(pcm.data(), 8192);
        run(cycles, 100);
        const size_t m = spu2::popFrames(pcm.data(), 100);
        bool silent = true;
        for (size_t i = 0; i < m * 2; ++i)
            silent = silent && pcm[i] == 0;
        CHECK(silent);
    }

    void testDeterministicClock()
    {
        spu2::reset();
        routeAll();
        uint64_t cycles = 0;
        run(cycles, 480);
        CHECK(spu2::framesGenerated() == 480u);
        spu2::advance(cycles + spu2::kCyclesPerFrame - 1u); // one cycle short: no new frame
        CHECK(spu2::framesGenerated() == 480u);
    }
}

int main()
{
    testEndxLatchAndKon();
    testOutputAndPitch();
    testDeterministicClock();
    if (g_fail == 0)
        std::printf("spu2_tests: PASS\n");
    return g_fail == 0 ? 0 : 1;
}
