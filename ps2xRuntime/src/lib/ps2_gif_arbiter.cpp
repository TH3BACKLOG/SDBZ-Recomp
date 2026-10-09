#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_gs_gpu.h"
#include "ThreadNaming.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <intrin.h>
#include <mutex>
#include <thread>
#include <vector>

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

namespace
{
// Packet buffers recycled across GifArbiter::submit calls (game thread only,
// like m_queue): no heap alloc + zero fill per packet.
std::vector<std::vector<uint8_t>> g_arbPool;
// The packet drain() is handing to m_processFn right now. ps2xGsThreadSubmit
// takes this buffer instead of copying the bytes a second time (perf 10-09:
// that copy was ~3% of the game thread in a fight).
std::vector<uint8_t> *g_drainingPkt = nullptr;
}

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
    if (!g_arbPool.empty())
    {
        pkt.data = std::move(g_arbPool.back());
        g_arbPool.pop_back();
    }
    pkt.data.assign(data, data + sizeBytes);
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
            g_drainingPkt = &pkt.data;
            m_processFn(pkt.data.data(), static_cast<uint32_t>(pkt.data.size()));
            g_drainingPkt = nullptr;
            ps2diag_gifpath::g_curPath.store(0u, std::memory_order_relaxed);
        }
    }
    for (auto &pkt : m_queue)
    {
        if (pkt.data.capacity() != 0u)
            g_arbPool.push_back(std::move(pkt.data));
    }
    m_queue.clear();
}

uint8_t GifArbiter::pathPriority(GifPathId id)
{
    return static_cast<uint8_t>(id);
}

// ---- GS thread (like PCSX2 MTGS) -----------------------------------------
// GS::processGIFPacket runs on its own thread. The game thread copies each
// packet into a queue and keeps going. It waits for the queue to empty only
// where it reads GS results back (CSR / SIGLBLID, local->host transfers) or
// calls the GS directly; callers use ps2xGsThreadSync() for that.
// GS keeps its own m_stateMutex, so the host presenter needs nothing new.
//
// Declared with extern at each call site so no header changes.
//   PS2X_GS_THREAD=0        run the GS on the game thread (old behaviour)
//   PS2X_GS_THREAD_STATS=1  print sync counts / wait time every ~2 s
//
// Pacing: at each vblank the game waits until the GS has finished everything
// submitted before the previous vblank (ps2xGsThreadVblank), so the GS is at
// most one frame behind. Without it the game ran ahead, filled the queue and
// then stopped dead for 1-2 s at a time (09-27 run).
namespace
{
// Safety cap only; vblank pacing normally keeps the queue near one frame
// (a fight frame is ~2.2 MB of GIF data).
constexpr size_t kGsThreadMaxQueuedBytes = 16u * 1024u * 1024u;

// Nonzero while the host present loop wants the GS lock (m_stateMutex) to
// latch a frame. The GS thread is busy almost all the time and re-takes that
// lock straight after each packet, and the Windows mutex is not fair, so the
// present loop starved: picture froze, pad pushes stopped with START held,
// and the memory card screen never saw a new press (09-27 run).
std::atomic<int> g_presenterWaiting{0};
std::atomic<uint64_t> g_presenterYields{0};
std::atomic<uint64_t> g_presenterYieldNs{0};
}
extern std::atomic<uint64_t> g_gsmtWaitDoneNs; // ps2_gs_rasterizer.cpp
extern std::atomic<uint64_t> g_gsmtWaitDoneCalls;
extern std::atomic<uint64_t> g_gsmtWaitByReason[16][2];
extern std::atomic<uint64_t> g_gsmtBusyTsc[16];
extern std::atomic<uint64_t> g_gsmtJobs[16];
extern std::atomic<uint64_t> g_gsmtSubmitTsc;
extern std::atomic<uint64_t> g_gsmtSubmitCalls;
// TSC ticks -> ms (calibrated once, 2 ms spin).
static double gsmtTicksToMs(uint64_t ticks)
{
    static const double msPerTick = []
    {
        using clock = std::chrono::steady_clock;
        const auto c0 = clock::now();
        const uint64_t t0 = __rdtsc();
        while (clock::now() - c0 < std::chrono::milliseconds(2))
        {
        }
        const uint64_t t1 = __rdtsc();
        const double ms = std::chrono::duration<double, std::milli>(clock::now() - c0).count();
        return t1 > t0 ? ms / static_cast<double>(t1 - t0) : 0.0;
    }();
    return static_cast<double>(ticks) * msPerTick;
}
namespace
{

void waitForPresenter()
{
    if (g_presenterWaiting.load(std::memory_order_acquire) == 0)
        return;
    g_presenterYields.fetch_add(1u, std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    while (g_presenterWaiting.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
    g_presenterYieldNs.fetch_add(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - t0).count()),
                                 std::memory_order_relaxed);
}

struct GsThread
{
    std::mutex m;
    std::condition_variable cvWork;
    std::condition_variable cvIdle;
    std::vector<std::vector<uint8_t>> pending;
    std::vector<std::vector<uint8_t>> pool;
    size_t queuedBytes = 0;
    bool stop = false;
    GS *gs = nullptr;
    std::thread th;
    std::atomic<uint32_t> inFlight{0};
    std::atomic<uint64_t> submittedSeq{0}; // packets submitted, ever
    std::atomic<uint64_t> completedSeq{0}; // packets processed, ever
    uint64_t prevVblankMark = 0;           // submittedSeq at the previous vblank
    // P5b (VU worker): vblank markers go through the worker's queue, so the pacing
    // target comes from the marker job instead of from the EE thread.
    uint64_t vblTicketNext = 0;                // EE thread only
    std::atomic<uint64_t> markDone{0};         // last ticket whose marker job ran
    std::atomic<uint64_t> markSeq[2]{}; // submittedSeq after marker ticket&1

    // stats (game thread only, except syncWaitNs)
    bool stats = false;
    uint64_t submits = 0;
    uint64_t syncs[8] = {};
    uint64_t syncWaits = 0;
    uint64_t syncWaitNs = 0;
    uint64_t fullWaits = 0;
    uint64_t vblankWaits = 0;
    uint64_t vblankWaitNs = 0;
    std::chrono::steady_clock::time_point lastPrint = std::chrono::steady_clock::now();

    void run()
    {
        ThreadNaming::SetCurrentThreadName("GsThread");
        std::vector<std::vector<uint8_t>> batch;
        std::unique_lock<std::mutex> lock(m);
        for (;;)
        {
            cvWork.wait(lock, [this] { return stop || !pending.empty(); });
            if (pending.empty() && stop)
                return;
            batch.swap(pending);
            GS *target = gs;
            lock.unlock();
            size_t bytes = 0;
            for (std::vector<uint8_t> &pkt : batch)
            {
                waitForPresenter();
                if (pkt.empty())
                {
                    // vblank marker: this frame is fully drawn, so latch it
                    // here. Latching from the present loop caught half-drawn
                    // frames while this thread ran behind (flashing, 09-27).
                    target->latchHostPresentationFrame();
                    continue;
                }
                target->processGIFPacket(pkt.data(), static_cast<uint32_t>(pkt.size()));
                bytes += pkt.size();
            }
            lock.lock();
            for (std::vector<uint8_t> &pkt : batch)
                pool.push_back(std::move(pkt));
            const uint32_t done = static_cast<uint32_t>(batch.size());
            batch.clear();
            queuedBytes -= bytes;
            completedSeq.fetch_add(done, std::memory_order_acq_rel);
            inFlight.fetch_sub(done, std::memory_order_acq_rel);
            cvIdle.notify_all(); // sync/vblank wait on progress, submit on queuedBytes
        }
    }

    void maybePrint()
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPrint < std::chrono::seconds(2))
            return;
        lastPrint = now;
        std::printf("[gsthread] submits=%llu syncWaits=%llu waitMs=%.1f fullWaits=%llu vblWaits=%llu vblWaitMs=%.1f"
                    " presYields=%llu presMs=%.1f rasterWaits=%llu rasterWaitMs=%.1f sync{csr=%llu siglbl=%llu local2host=%llu gsdirect=%llu nativechain=%llu other=%llu}\n",
                    (unsigned long long)submits, (unsigned long long)syncWaits, syncWaitNs / 1e6,
                    (unsigned long long)fullWaits, (unsigned long long)vblankWaits, vblankWaitNs / 1e6,
                    (unsigned long long)g_presenterYields.load(std::memory_order_relaxed),
                    g_presenterYieldNs.load(std::memory_order_relaxed) / 1e6,
                    (unsigned long long)g_gsmtWaitDoneCalls.load(std::memory_order_relaxed),
                    g_gsmtWaitDoneNs.load(std::memory_order_relaxed) / 1e6,
                    (unsigned long long)syncs[1], (unsigned long long)syncs[2],
                    (unsigned long long)syncs[3], (unsigned long long)syncs[4], (unsigned long long)syncs[5],
                    (unsigned long long)syncs[0]);
        std::printf("[gsmt] submitCalls=%llu submitMs=%.1f", (unsigned long long)g_gsmtSubmitCalls.load(std::memory_order_relaxed),
                    gsmtTicksToMs(g_gsmtSubmitTsc.load(std::memory_order_relaxed)));
        for (int i = 0; i < 16; ++i)
        {
            const uint64_t j = g_gsmtJobs[i].load(std::memory_order_relaxed);
            if (j)
                std::printf(" w%d=%llu/%.1fms", i, (unsigned long long)j, gsmtTicksToMs(g_gsmtBusyTsc[i].load(std::memory_order_relaxed)));
        }
        std::printf("\n");
        std::printf("[gsraster-wait]");
        for (int r = 0; r < 16; ++r)
        {
            const uint64_t c = g_gsmtWaitByReason[r][0].load(std::memory_order_relaxed);
            if (c)
                std::printf(" r%d=%llu/%.1fms", r, (unsigned long long)c, g_gsmtWaitByReason[r][1].load(std::memory_order_relaxed) / 1e6);
        }
        std::printf("\n");
        std::fflush(stdout);
    }
};

GsThread *g_gsThread = nullptr;

// Nonzero while the EE thread waits on the GS thread. The deterministic vblank
// pacer (Interrupt.cpp) reads it: a blocked EE retires no guest instructions,
// which looks like a frozen guest, and the stall tick it would then deliver
// lands mid guest code. That breaks determinism and hangs the memory card
// screen.
std::atomic<int> g_eeWaitingOnGs{0};

struct EeWaitScope
{
    EeWaitScope() { g_eeWaitingOnGs.fetch_add(1, std::memory_order_acq_rel); }
    ~EeWaitScope() { g_eeWaitingOnGs.fetch_sub(1, std::memory_order_acq_rel); }
};

bool envOff(const char *name)
{
    const char *v = std::getenv(name);
    return v && v[0] == '0';
}

bool envOn(const char *name)
{
    const char *v = std::getenv(name);
    return v && v[0] != '\0' && v[0] != '0';
}
}

extern "C" int ps2x_ee_waiting_on_gs()
{
    return g_eeWaitingOnGs.load(std::memory_order_acquire);
}

// The VU worker wait (ps2_memory.cpp, [vuw]) counts as "EE waiting" too.
extern "C" void ps2x_ee_wait_enter()
{
    g_eeWaitingOnGs.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" void ps2x_ee_wait_leave()
{
    g_eeWaitingOnGs.fetch_sub(1, std::memory_order_acq_rel);
}

// Host present loop brackets its GS frame latch/copy with these.
extern "C" void ps2x_gs_present_begin()
{
    g_presenterWaiting.fetch_add(1, std::memory_order_acq_rel);
}

extern "C" void ps2x_gs_present_end()
{
    g_presenterWaiting.fetch_sub(1, std::memory_order_acq_rel);
}

// Nonzero once the GS thread runs: it latches host frames at each vblank
// marker, so the present loop must only copy the latched frame.
extern "C" int ps2x_gs_thread_latches()
{
    GsThread *t = g_gsThread;
    return (t && !t->stop) ? 1 : 0;
}

bool ps2xGsThreadEnabled()
{
    static const bool enabled = !envOff("PS2X_GS_THREAD");
    return enabled;
}

void ps2xGsThreadSubmit(GS *gs, const uint8_t *data, uint32_t sizeBytes)
{
    if (!g_gsThread)
    {
        g_gsThread = new GsThread();
        g_gsThread->gs = gs;
        g_gsThread->stats = envOn("PS2X_GS_THREAD_STATS");
        g_gsThread->th = std::thread([] { g_gsThread->run(); });
        std::printf("[gsthread] GS runs on its own thread (PS2X_GS_THREAD=0 to disable)\n");
    }
    GsThread &t = *g_gsThread;
    {
        std::unique_lock<std::mutex> lock(t.m);
        if (t.stop)
        {
            // Shutting down: nothing drains the queue any more.
            lock.unlock();
            gs->processGIFPacket(data, sizeBytes);
            return;
        }
        if (t.queuedBytes > kGsThreadMaxQueuedBytes)
        {
            ++t.fullWaits;
            EeWaitScope waiting;
            t.cvIdle.wait(lock, [&t] { return t.queuedBytes <= kGsThreadMaxQueuedBytes / 2u; });
        }
        if (gs != t.gs)
        {
            // New GS object (tools create one per replay): finish the old one first.
            EeWaitScope waiting;
            t.cvIdle.wait(lock, [&t] { return t.inFlight.load(std::memory_order_acquire) == 0u; });
            t.gs = gs;
        }
        std::vector<uint8_t> buf;
        if (!t.pool.empty())
        {
            buf = std::move(t.pool.back());
            t.pool.pop_back();
        }
        if (g_drainingPkt && g_drainingPkt->data() == data && g_drainingPkt->size() == sizeBytes)
            buf.swap(*g_drainingPkt); // take the arbiter's copy; it gets the pool buffer back
        else
            buf.assign(data, data + sizeBytes);
        t.pending.push_back(std::move(buf));
        t.queuedBytes += sizeBytes;
        t.submittedSeq.fetch_add(1u, std::memory_order_acq_rel);
        t.inFlight.fetch_add(1u, std::memory_order_acq_rel);
    }
    t.cvWork.notify_one();
    if (t.stats)
    {
        ++t.submits;
        t.maybePrint();
    }
}

extern "C" void ps2x_p5a_bump(int slot) noexcept; // ps2_memory.cpp, P5a counters
extern "C" void ps2x_p5a_vblank() noexcept;
extern "C" void ps2x_vuw_sync_ee(int reason) noexcept; // ps2_memory.cpp, VU worker
bool ps2xVuwActive();
void ps2xVuwEnqueue(std::function<void()> fn);
void ps2xVuwStop();

// reason: 1 CSR, 2 SIGLBLID, 3 local->host, 4 direct GS call, 5 native GIF chain.
void ps2xGsThreadSync(uint32_t reason)
{
    ps2x_p5a_bump(7);
    ps2x_vuw_sync_ee(4); // packets still queued on the VU worker have not reached the GS yet
    GsThread *t = g_gsThread;
    if (!t)
        return;
    if (t->stats)
        ++t->syncs[reason < 8u ? reason : 0u];
    if (t->inFlight.load(std::memory_order_acquire) == 0u)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        EeWaitScope waiting;
        std::unique_lock<std::mutex> lock(t->m);
        t->cvIdle.wait(lock, [t] { return t->inFlight.load(std::memory_order_acquire) == 0u; });
    }
    if (t->stats)
    {
        ++t->syncWaits;
        t->syncWaitNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        t->maybePrint();
    }
}

// Called at every vblank (EeScheduler VBlankStart): wait until the GS has
// processed everything submitted before the previous vblank.
// Empty packet = vblank marker; the GS thread latches the host frame there.
// Returns submittedSeq after the marker.
static uint64_t gsThreadPushMarker(GsThread *t)
{
    {
        std::lock_guard<std::mutex> lock(t->m);
        if (!t->stop)
        {
            std::vector<uint8_t> marker;
            if (!t->pool.empty())
            {
                marker = std::move(t->pool.back());
                t->pool.pop_back();
                marker.clear();
            }
            t->pending.push_back(std::move(marker));
            t->submittedSeq.fetch_add(1u, std::memory_order_acq_rel);
            t->inFlight.fetch_add(1u, std::memory_order_acq_rel);
        }
    }
    t->cvWork.notify_one();
    return t->submittedSeq.load(std::memory_order_acquire);
}

void ps2xGsThreadVblank()
{
    ps2x_p5a_vblank();
    GsThread *t = g_gsThread;
    if (!t)
        return;
    uint64_t target = 0;
    if (ps2xVuwActive())
    {
        // The marker has to follow every packet kicked before this vblank, and
        // those are still on the VU worker: queue the marker behind them.
        const uint64_t ticket = ++t->vblTicketNext;
        ps2xVuwEnqueue([t, ticket]
                       {
            const uint64_t seq = gsThreadPushMarker(t);
            t->markSeq[ticket & 1u].store(seq, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(t->m);
                t->markDone.store(ticket, std::memory_order_release);
            }
            t->cvIdle.notify_all(); });
        if (ticket == 1u)
            return;
        // Same pacing as below: wait for the GS to finish what was submitted
        // before the previous vblank, i.e. up to the previous marker.
        const uint64_t prev = ticket - 1u;
        if (t->markDone.load(std::memory_order_acquire) < prev)
        {
            EeWaitScope waiting;
            std::unique_lock<std::mutex> lock(t->m);
            t->cvIdle.wait(lock, [t, prev]
                           { return t->stop || t->markDone.load(std::memory_order_acquire) >= prev; });
        }
        if (t->markDone.load(std::memory_order_acquire) >= prev)
            target = t->markSeq[prev & 1u].load(std::memory_order_acquire);
    }
    else
    {
        gsThreadPushMarker(t);
        target = t->prevVblankMark;
        t->prevVblankMark = t->submittedSeq.load(std::memory_order_acquire);
    }
    if (t->completedSeq.load(std::memory_order_acquire) >= target)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    {
        EeWaitScope waiting;
        std::unique_lock<std::mutex> lock(t->m);
        t->cvIdle.wait(lock, [t, target]
                       { return t->stop || t->completedSeq.load(std::memory_order_acquire) >= target; });
    }
    if (t->stats)
    {
        ++t->vblankWaits;
        t->vblankWaitNs += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
}

void ps2xGsThreadStop()
{
    ps2xVuwStop(); // the VU worker feeds this thread; finish and join it first
    // The object is kept (never deleted): a late submit from the game thread
    // then runs the packet itself instead of touching freed memory.
    GsThread *t = g_gsThread;
    if (!t)
        return;
    {
        std::lock_guard<std::mutex> lock(t->m);
        if (t->stop)
            return;
        t->stop = true;
    }
    t->cvWork.notify_one();
    if (t->th.joinable() && t->th.get_id() != std::this_thread::get_id())
        t->th.join();
}
