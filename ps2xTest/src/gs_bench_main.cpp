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
void ps2xGsThreadSync(uint32_t reason);
void ps2xGsThreadStop();

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

        const auto t0 = std::chrono::steady_clock::now();
        for (const GsrTransfer &t : transfers)
            if (t.size != 0u)
            {
                if (threaded)
                    ps2xGsThreadSubmit(&gs, payload + t.offset, t.size);
                else
                    gs.processGIFPacket(payload + t.offset, t.size);
            }
        if (threaded)
            ps2xGsThreadSync(0u);
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());

        if (r == repeat - 1)
        {
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

    std::vector<double> sorted = times;
    std::sort(sorted.begin(), sorted.end());
    std::printf("bench: gs median %.1f ms (min %.1f, max %.1f) over %d replays\n",
                sorted[sorted.size() / 2], sorted.front(), sorted.back(), repeat);
    std::fflush(stdout);
    std::_Exit(0);
}
