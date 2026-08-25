#include "runtime/ps2_gif_arbiter.h"
#include <algorithm>
#include <atomic>
#include <cstring>

// ---- [drawpath] -- Stage 5.11 run 32 ------------------------------------
// Run 31 cleared this file: every drain carried maxbatch=1, so the stable_sort
// below is a pass-through and cannot reorder anything (verdict was
// NO-MIXED-PATH-BATCH on all 7 records, p1=0 throughout). The sort is
// therefore left exactly as it was.
//
// What run 31 did establish is that draws reach the GS one packet at a time,
// in submission order -- so the measured box-before-background inversion is
// decided upstream, by which DMA channel each sprite rides and the fixed
// GIF -> VIF0 -> VIF1 drain order in PS2Memory::processPendingTransfers().
//
// This publishes the path of the packet currently being dispatched so the
// rasterizer can stamp each draw with its origin. Defined here (not in a
// header) so no generated TU is disturbed.
namespace ps2diag_gifpath
{
std::atomic<uint32_t> g_curPath{0}; // 0 = unknown, else GifPathId

// Run 33: which submitGifPacket() call site produced the packet in flight.
// Run 31 proved drain is synchronous with submit (maxbatch=1 always), so a
// plain global is current at draw time.
//   1 = VIF1 image/unpack path   ps2_vif1_interpreter.cpp:439
//   2 = VIF1 DIRECT path         ps2_vif1_interpreter.cpp:649
//   3 = GIF chain (chainData)    ps2_memory.cpp:1857
//   4 = GIF chain (scratchpad)   ps2_memory.cpp:1886
//   5 = GIF chain (rdram)        ps2_memory.cpp:1905
//   6 = GIF write (rdram)        ps2_memory.cpp:2245
//   7 = GIF write (direct)       ps2_memory.cpp:2255
std::atomic<uint32_t> g_curSite{0};

// Run 34: the EE address the packet in flight was read from.
// Run 33 left one inference unproven -- it assumed the Nth role=1 draw came
// from the Nth sprite link in the [chainord] ring. That is exactly the kind of
// positional guess that has cost runs before, so stamp the address onto the
// draw itself and let the ring be corroboration rather than the argument.
// 0xFFFFFFFF = unresolved (not a chain/rdram read, or the lookup missed).
std::atomic<uint32_t> g_curSrc{0xFFFFFFFFu};
}
// -------------------------------------------------------------------------

GifArbiter::GifArbiter(ProcessPacketFn processFn)
    : m_processFn(std::move(processFn))
{
}

bool GifArbiter::isImagePacket(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || sizeBytes < 16u)
        return false;

    uint64_t tagLo = 0;
    std::memcpy(&tagLo, data, sizeof(tagLo));
    const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
    return flg == 2u;
}

void GifArbiter::submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes, bool path2DirectHl)
{
    if (!data || sizeBytes < 16 || !m_processFn)
        return;

    GifArbiterPacket pkt;
    pkt.pathId = pathId;
    pkt.path2DirectHl = (pathId == GifPathId::Path2) && path2DirectHl;
    pkt.path3Image = (pathId == GifPathId::Path3) && isImagePacket(data, sizeBytes);
    pkt.data.resize(sizeBytes);
    std::memcpy(pkt.data.data(), data, sizeBytes);
    m_queue.push_back(std::move(pkt));
}

void GifArbiter::drain()
{
    if (!m_processFn)
        return;

    std::stable_sort(m_queue.begin(), m_queue.end(),
                     [](const GifArbiterPacket &a, const GifArbiterPacket &b)
                     {
                         // DIRECTHL cannot preempt PATH3 IMAGE transfers.
                         if (a.path2DirectHl != b.path2DirectHl || a.path3Image != b.path3Image)
                         {
                             if (a.path3Image && b.path2DirectHl)
                                 return true;
                             if (a.path2DirectHl && b.path3Image)
                                 return false;
                         }
                         return pathPriority(a.pathId) < pathPriority(b.pathId);
                     });

    for (size_t i = 0; i < m_queue.size(); ++i)
    {
        auto &pkt = m_queue[i];
        if (!pkt.data.empty())
        {
            // [drawpath] run 32: stamp the origin of every draw this packet emits.
            ps2diag_gifpath::g_curPath.store(static_cast<uint32_t>(pkt.pathId),
                                             std::memory_order_relaxed);
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            ps2diag_gifpath::g_curPath.store(0u, std::memory_order_relaxed);
        }
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}
