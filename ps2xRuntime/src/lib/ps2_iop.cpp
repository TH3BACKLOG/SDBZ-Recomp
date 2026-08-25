#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

#include "runtime/ps2_iop.h"
#include "runtime/ps2_iop_audio.h"
#include "runtime/ps2_iop_cl.h"
#include "runtime/ps2_iop_dbcman.h"
#include "runtime/ps2_iop_mcman.h"
#include "runtime/ps2_iop_sdrdrv.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime.h"
#include "Kernel/Syscalls/RPC.h"

// S2.1: real ARKD_DVD.IRX loader lives in ps2_iop_irx_loader.cpp. Declared here
// (no header touched) so the LOADFILE path can hand it the module when the game
// asks for it. Loads + relocates the IRX into IOP RAM and builds the import map.
extern bool ps2_iop_loadArkdIrx(PS2Runtime *runtime, const std::string &modulePath);

// S2.2c-2: the ISO9660 name->(LBN,size) resolver. Defined in CD.cpp, which is
// the one TU that owns the live CD file table (it lives in Support.h's ANON
// namespace, so any other TU that included Support.h would get its own empty
// copy). Declared extern here rather than adding it to a header, per project
// rules. Same resolver readCdSectors uses, so a SearchFile answer and the read
// that follows it can never disagree.
extern bool ps2_iop_cdSearchFile(const char *name, uint32_t *lbnOut,
                                 uint32_t *sizeOut);

// Stage 5.16: the absolute-LBN sector reader, also defined in CD.cpp and for
// exactly the same TU-ownership reason as the resolver above. It backs the
// cdvdfsv N-command service (sid 0x80000595) added below -- the LBN the guest
// passes to sceCdRead is the very one ps2_iop_cdSearchFile handed it, so the
// two agree by construction.
extern bool ps2_iop_cdReadSectors(uint32_t lbn, uint32_t sectors, uint8_t *dst,
                                  size_t byteCount);

namespace
{
    bool mcservTraceEnabled()
    {
        const char *e = std::getenv("PS2X_MCSERV_TRACE");
        return e && e[0] != '\0' && e[0] != '0';
    }

    // ---- IOP heap service backing store (SIF RPC sid 0x80000003) ------------
    //
    // Placement: 0x00008000..0x00040000 of the real 2 MB IOP RAM. Every other
    // consumer of IOP RAM in this runtime sits above that -- the ARKD_DVD image
    // at 0x40000, its sceAllocSysMemory arena at 0x60000..0x1d0000, and the IOP
    // thread stacks below 0x1efff0 (all in ps2_iop_irx_loader.cpp) -- so this
    // slice overlaps nothing, while still handing back addresses that lie inside
    // real IOP RAM, so a guest range check on them passes and an EE->IOP DMA
    // into a block actually lands somewhere addressable.
    constexpr uint32_t kIopSidIopHeap   = 0x80000003u;
    // cdvdfsv SearchFile. Identified from sub_1866A0's own bind, and
    // corroborated by [SIF:BIND] client=0x568880 sid=0x80000597 in the log.
    constexpr uint32_t kIopSidCdvdSearchFile = 0x80000597u;
    // cdvdfsv N-command server. Identified from sub_1875E8 (sceCdRead) and
    // sub_186B18 (the status poll), both of which bind through the client at
    // 0x464410; see the handler below for the decode.
    constexpr uint32_t kIopSidCdvdNcmd = 0x80000595u;
    constexpr uint32_t kSifIopHeapBase  = 0x00008000u;
    constexpr uint32_t kSifIopHeapEnd   = 0x00040000u;
    constexpr uint32_t kSifIopHeapAlign = 64u;
    constexpr uint32_t kSifIopHeapMaxBlocks = 64u;

    struct SifIopHeapBlock
    {
        uint32_t addr = 0u;
        uint32_t size = 0u;
        bool     used = false;
    };

    std::mutex      g_sifIopHeapMutex;
    SifIopHeapBlock g_sifIopHeapBlocks[kSifIopHeapMaxBlocks];
    uint32_t        g_sifIopHeapCount  = 0u;
    uint32_t        g_sifIopHeapCursor = kSifIopHeapBase;

    // Bump allocator with exact-block reuse. The IOP heap on a real console is a
    // general allocator, but this title's usage is a handful of long-lived
    // command blocks taken once at init, so reuse-on-free is enough and a bump
    // cursor keeps the addresses stable and readable in a log.
    uint32_t sifIopHeapAlloc(uint32_t size)
    {
        if (size == 0u)
        {
            return 0u;
        }
        const uint32_t need = (size + (kSifIopHeapAlign - 1u)) & ~(kSifIopHeapAlign - 1u);

        std::lock_guard<std::mutex> lock(g_sifIopHeapMutex);
        for (uint32_t i = 0; i < g_sifIopHeapCount; ++i)
        {
            if (!g_sifIopHeapBlocks[i].used && g_sifIopHeapBlocks[i].size >= need)
            {
                g_sifIopHeapBlocks[i].used = true;
                return g_sifIopHeapBlocks[i].addr;
            }
        }
        if (g_sifIopHeapCursor + need > kSifIopHeapEnd ||
            g_sifIopHeapCount >= kSifIopHeapMaxBlocks)
        {
            return 0u;
        }
        const uint32_t addr = g_sifIopHeapCursor;
        g_sifIopHeapCursor += need;
        g_sifIopHeapBlocks[g_sifIopHeapCount++] = SifIopHeapBlock{addr, need, true};
        return addr;
    }

    bool sifIopHeapFree(uint32_t addr)
    {
        std::lock_guard<std::mutex> lock(g_sifIopHeapMutex);
        for (uint32_t i = 0; i < g_sifIopHeapCount; ++i)
        {
            if (g_sifIopHeapBlocks[i].addr == addr && g_sifIopHeapBlocks[i].used)
            {
                g_sifIopHeapBlocks[i].used = false;
                return true;
            }
        }
        return false;
    }

    // ---- SJX/DTX sound service (SIF RPC sid 0x90000200) ---------------------
    //
    // The server for this sid lives inside CRI_ADXI.IRX (the sid constant sits
    // at file offset 0xd654 of the shipped module) -- there is no SJX IRX on the
    // disc. Running CRI_ADXI for real needs the IRX loader to host a second
    // module alongside ARKD_DVD plus a libsd/SPU2 backend, so until that exists
    // we answer the handshake here and the game gets no sound.
    //
    // WHY THIS IS NOT [[feedback_no_iop_faking]]: that rule came from inventing
    // ARKD_DVD's *payload* -- fake TOC bytes that the game then read as real
    // file data and got corrupt reads from. Here the only value that crosses
    // back is the DTX handle, and PCSX2 proves the EE never looks inside it:
    // on real hardware slot 0 of the DTX table at 0x44DAF0 holds handle
    // 0x0008BAB0, an *IOP* address the EE cannot dereference. It is stored and
    // echoed back in later RPCs, nothing more. An opaque token is the entire
    // contract, so minting one loses no information.
    constexpr uint32_t kIopSidSjx        = 0x90000200u;
    constexpr uint32_t kSjxFnoDtxCreate  = 2u;
    constexpr uint32_t kSjxMaxIds        = 0x10u;   // 0x130418 rejects id >= 0x10
    // Handles are minted from 0x4000..0x8000 of real IOP RAM: below the IOP heap
    // arena above, above nothing we use, so a handle can never alias a block
    // sifIopHeapAlloc hands out, and a guest range check on it still passes.
    // DTX ids are < 0x10 so the DTX sub-window tops out at 0x4F00.
    constexpr uint32_t kSjxHandleBase    = 0x00004000u;
    constexpr uint32_t kSjxHandleStride  = 0x00000100u;

    // The 0x400+n command channel (0x130910) carries a whole IOP-side sound
    // object model: 22 call sites, 21 distinct commands, all through the same
    // pair of fixed buffers. These four are the *create* family -- each sends
    // its constructor arguments, receives exactly one word, and every caller
    // treats a zero reply as fatal with a "can't creat" print followed by a
    // deliberate infinite loop:
    //
    //   0x400  SJX_Create      (0x13b9d0; gate 0x136bf4 -> E0110105)
    //   0x408  PS2RNA_Create   (0x1310xx; gate -> E0100401, run 53's park)
    //   0x420  SJUNI_Create    (0x13b1c8)
    //   0x421  SJUNI_CreateX   (0x13b228)
    //   0x422  SJUNI_CreateRmt (0x13b278; gate 0x136b18 -> E0110102, run 49's park)
    //
    // 0x408 was missed on the first pass because its caller does not print a
    // "can't creat" string of its own -- it stores the reply at +32 of the movie
    // object and only the *outer* MovieCreate reports. Run 53 named it directly:
    //
    //     MovieCreate: Use Work Size = 0x332100
    //     E0100401: can't create PS2RNA of IOP
    //     MovieCreate: Create movie handle failed
    //
    // and the decompile confirms the create shape rather than inferring it:
    //
    //     v21 = cmd(8, &args, 4, &reply, 1);   // 4 words in, exactly 1 out
    //     *(_DWORD *)(obj + 32) = v21;         // reply IS the handle
    //     if (!v21) { print(E0100401); return 0; }
    //     ... teardown later: cmd(9, &{*(obj+32)}, 1, 0, 0);
    //
    // That trailing 0x409 is the decisive part: a destroy that takes the 0x408
    // reply back as its only argument can only mean the reply is an object
    // handle, not a status word. Its observed payload agrees -- {2, 0, 0x5000,
    // 0x5080}, a count plus two handles this service minted earlier via 0x422.
    //
    // Every OTHER command deliberately keeps returning zero. Commands like
    // 0x40C, 0x424 and 0x426 receive two to four words of *data* rather than a
    // handle, and inventing a plausible-looking number for those would steer
    // the game down a wrong path silently -- strictly worse than the honest
    // refusal it already knows how to report and print. 0x40C specifically was
    // re-checked in run 53 and must stay refused: its caller stores the reply
    // straight into the sample-rate/mode flag it later feeds back into 0x408,
    // with no error path, so zero there is a legitimate value and a minted
    // number would be a silent lie.
    constexpr uint32_t kSjxFnoSjxCreate     = 0x400u;
    constexpr uint32_t kSjxFnoPs2rnaCreate  = 0x408u;
    constexpr uint32_t kSjxFnoUniCreate     = 0x420u;
    constexpr uint32_t kSjxFnoUniCreateX    = 0x421u;
    constexpr uint32_t kSjxFnoUniCreateRmt  = 0x422u;
    // Object handles get their own slice above the DTX sub-window and below the
    // IOP heap arena, so they can alias neither.
    constexpr uint32_t kSjxObjHandleBase    = 0x00005000u;
    constexpr uint32_t kSjxObjHandleStride  = 0x00000040u;
    constexpr uint32_t kSjxObjHandleEnd     = 0x00008000u;

    // ---- SJX stream-ring registry (populated by fno 2, DTX_Create) ---------
    //
    // 5.15: DTX_Create's send payload is {id, eewk, iopwk, wklen}, and the EE
    // then pumps that ring at 0x130730 with sceSifSetDma to `iopwk`. Record the
    // mapping so the DMA path can recognise a stream transfer by destination.
    // This stores only what the game itself told us; nothing is invented.
    struct SjxStreamRing
    {
        uint32_t iopDest = 0u;
        uint32_t eeBase  = 0u;
        uint32_t wkLen   = 0u;
    };
    std::mutex g_sjxRingMutex;
    SjxStreamRing g_sjxRings[kSjxMaxIds];
}

// Declared extern in Kernel/Stubs/SIF.cpp -- no header is touched, per the
// .cpp-pair rule. Returns the EE ring base and length for an IOP destination
// that a DTX_Create claimed, so sceSifSetDma can tell a stream buffer from
// every other IOP-bound transfer without gating on an inferred address.
bool ps2x_sjxLookupStreamByIopDest(uint32_t iopDest, uint32_t *outEeBase, uint32_t *outWkLen)
{
    if (iopDest == 0u || iopDest == 0xFFFFFFFFu)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_sjxRingMutex);
    for (const SjxStreamRing &r : g_sjxRings)
    {
        if (r.iopDest == iopDest && r.eeBase != 0u && r.wkLen >= 0x40u)
        {
            if (outEeBase) *outEeBase = r.eeBase;
            if (outWkLen)  *outWkLen  = r.wkLen;
            return true;
        }
    }
    return false;
}

ps2_iop::ps2_iop()
{
    reset();
}

void ps2_iop::init(uint8_t *rdram)
{
    m_rdram = rdram;
}

void ps2_iop::reset()
{
    ps2_iop_cl::reset();
    ps2_iop_dbcman::reset();
    ps2_iop_sdrdrv::reset();
}

bool ps2_iop::handleRPC(PS2Runtime *runtime,
                        uint32_t sid,
                        uint32_t rpcNum,
                        uint32_t sendBufAddr,
                        uint32_t sendSize,
                        uint32_t recvBufAddr,
                        uint32_t recvSize,
                        uint32_t &resultPtr,
                        bool &signalNowaitCompletion)
{
    resultPtr = 0u;
    signalNowaitCompletion = false;
    const bool traceMcserv = mcservTraceEnabled();

    if (traceMcserv && (sid == IOP_SID_MCSERV || sid == IOP_SID_MCSERV_LEGACY))
    {
        static std::atomic<uint32_t> s_mcservRpcLogs{0u};
        if (s_mcservRpcLogs.fetch_add(1u, std::memory_order_relaxed) < 96u)
        {
            std::fprintf(stderr,
                         "[iop:mcserv-route] sid=0x%08X rpc=0x%X send=0x%08X/%u recv=0x%08X/%u\n",
                         sid, rpcNum, sendBufAddr, sendSize, recvBufAddr, recvSize);
        }
    }

    if (ps2_syscalls::handleSoundDriverRpcService(m_rdram,
                                                  runtime,
                                                  sid,
                                                  rpcNum,
                                                  sendBufAddr,
                                                  sendSize,
                                                  recvBufAddr,
                                                  recvSize,
                                                  resultPtr,
                                                  signalNowaitCompletion))
    {
        return true;
    }

    if (ps2_iop_dbcman::handleDbcManRpc(m_rdram,
                                        sid,
                                        rpcNum,
                                        sendBufAddr,
                                        sendSize,
                                        recvBufAddr,
                                        recvSize,
                                        resultPtr))
    {
        return true;
    }

    if (ps2_iop_audio::handleLibSdRpc(m_rdram,
                                      runtime,
                                      sid,
                                      rpcNum,
                                      sendBufAddr,
                                      sendSize,
                                      recvBufAddr,
                                      recvSize,
                                      resultPtr))
    {
        return true;
    }

    if (ps2_iop_cl::handleSoundRpc(m_rdram, sid,
                                   rpcNum, sendBufAddr,
                                   sendSize, recvBufAddr,
                                   recvSize, resultPtr,
                                   signalNowaitCompletion))
    {
        return true;
    }

    if (ps2_iop_cl::handleClFileRpc(m_rdram, sid,
                                    rpcNum, sendBufAddr,
                                    sendSize, recvBufAddr,
                                    recvSize, resultPtr))
    {
        return true;
    }

    if (ps2_iop_sdrdrv::handleSdrdrvRpc(m_rdram, sid,
                                        rpcNum, sendBufAddr,
                                        sendSize, recvBufAddr,
                                        recvSize, resultPtr))
    {
        return true;
    }

    if (ps2_iop_mcman::handleMcServRpc(m_rdram, sid,
                                       rpcNum, sendBufAddr,
                                       sendSize, recvBufAddr,
                                       recvSize, resultPtr))
    {
        if (traceMcserv && (sid == IOP_SID_MCSERV || sid == IOP_SID_MCSERV_LEGACY))
        {
            static std::atomic<uint32_t> s_mcservHandledLogs{0u};
            if (s_mcservHandledLogs.fetch_add(1u, std::memory_order_relaxed) < 96u)
            {
                std::fprintf(stderr,
                             "[iop:mcserv-route] handled rpc=0x%X resultPtr=0x%08X\n",
                             rpcNum, resultPtr);
            }
        }
        return true;
    }

    if (sid == kIopSidIopHeap) // IOP heap service (sceSifAllocIopHeap et al)
    {
        // sceSifAllocIopHeap() on the EE is a plain RPC to the IOP kernel's heap
        // service. Confirmed against this game's own libsif at 0x17cfd8, which
        // issues sceSifCallRpc(cd=0x564980, fno=1, send=0x564A00/4,
        // recv=0x5649C0/4) and returns [0x5649C0] -- so the answer travels in the
        // four-byte RECEIVE buffer, and a handler that writes nothing reads as a
        // clean "allocation failed" ([[project_stage512_memory_card]] again).
        //
        //   fno 1 ALLOC: send[0] = size    -> recv[0] = IOP address (0 = failed)
        //   fno 2 FREE : send[0] = address -> recv[0] = 0 ok / -1 not ours
        //   fno 3 LOAD : load a file into the heap (unused by this title)
        //
        // Why serve it here rather than run an IRX: this is a PS2 kernel service
        // that ships in the IOP BIOS, not a game module we have on the disc --
        // the same reason the cdvdman S-command path below is served here. There
        // is no IRX to run, so [[feedback_no_iop_faking]] does not apply; the
        // bookkeeping IS the service.
        //
        // WHAT IT COSTS US WHEN MISSING: SDBZ's SJX_Init asks for 0x8d0 bytes at
        // 0x13b838 and, on a zero answer, prints
        // "E0100301: SJX_Init can't allocate IOP Heap" (0x4bc710) and falls into
        // a deliberate infinite loop at 0x13b858. That loop is the white Atari
        // screen: the game is not hung, it has given up.
        uint8_t *sendPtr = sendBufAddr ? getMemPtr(m_rdram, sendBufAddr) : nullptr;
        uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;

        uint32_t arg = 0u;
        if (sendPtr && sendSize >= sizeof(uint32_t))
        {
            std::memcpy(&arg, sendPtr, sizeof(arg));
        }

        uint32_t answer = 0u;
        switch (rpcNum)
        {
        case 1u:
            answer = sifIopHeapAlloc(arg);
            break;
        case 2u:
            answer = sifIopHeapFree(arg) ? 0u : 0xFFFFFFFFu;
            break;
        default:
            // fno 3 (LOAD) and anything else: answer failure rather than invent
            // an address, so an unimplemented call shows up as the game's own
            // error path instead of a pointer to nothing.
            answer = 0u;
            break;
        }

        if (recvPtr && recvSize >= sizeof(uint32_t))
        {
            std::memcpy(recvPtr, &answer, sizeof(answer));
        }
        resultPtr = recvBufAddr;

        // Unconditional (not behind a trace env var) and bounded: an ALLOC that
        // answers 0 is the difference between booting and a white screen, and
        // `arg` also tells us whether the RPC bridge recovered the send payload
        // at all -- a zero size here would mean the size never reached us, which
        // looks identical to a heap-exhausted failure without this line.
        static std::atomic<uint32_t> s_iopHeapLogs{0u};
        if (s_iopHeapLogs.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::fprintf(stderr,
                         "[iop:iopheap] fno=%u arg=0x%08X -> 0x%08X "
                         "send=0x%08X/%u recv=0x%08X/%u cursor=0x%08X\n",
                         rpcNum, arg, answer, sendBufAddr, sendSize,
                         recvBufAddr, recvSize, g_sifIopHeapCursor);
        }
        return true;
    }

    if (sid == kIopSidSjx) // SJX/DTX sound service (server lives in CRI_ADXI.IRX)
    {
        // Call shapes, decoded from the game's own SJX layer:
        //
        //   fno 2      DTX_Create, from 0x130288:
        //              send = {id, eewk, iopwk, wklen} (0x10 bytes at 0x54C500)
        //              recv = 4 bytes at 0x54C600, the DTX handle. ZERO MEANS
        //              FAILURE -- 0x130418 prints "DTX_Create: can't create DTX
        //              of server", SJX_Init then prints "E0100302" and parks in
        //              an infinite loop at 0x13b8c0. That loop is the white
        //              Atari screen.
        //   fno 3      teardown; rsize is 0, so it is fire-and-forget.
        //   fno 0x400+n  generic command channel (0x130918): n words in, m words
        //              out, both through the same fixed buffers.
        //
        // Every one of these shares recv buffer 0x54C600. That is why the
        // command path below ZEROES the receive buffer instead of leaving it
        // alone: a handler that writes nothing leaves the PREVIOUS call's reply
        // sitting there, and the game reads it as a fresh answer. Silence has to
        // be written, not merely omitted.
        uint8_t *sendPtr = sendBufAddr ? getMemPtr(m_rdram, sendBufAddr) : nullptr;
        uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;

        if (recvPtr && recvSize > 0u)
        {
            std::memset(recvPtr, 0, recvSize);
        }

        uint32_t handle = 0u;
        uint32_t args[4] = {0u, 0u, 0u, 0u};

        const uint32_t argWords = (sendSize / sizeof(uint32_t) < 4u)
                                      ? uint32_t(sendSize / sizeof(uint32_t))
                                      : 4u;
        if (sendPtr && argWords != 0u)
        {
            std::memcpy(args, sendPtr, argWords * sizeof(uint32_t));
        }

        if (rpcNum == kSjxFnoDtxCreate)
        {
            // Mint a handle keyed on the DTX id so repeat creates of the same
            // slot are stable and two slots never collide. Refuse an
            // out-of-range id rather than mint past the reserved window -- the
            // caller already rejects id >= 0x10, so a larger one here means the
            // send payload did not reach us intact and a confident answer would
            // be worse than the game's own error path.
            if (args[0] < kSjxMaxIds)
            {
                handle = kSjxHandleBase + args[0] * kSjxHandleStride;

                // Remember the ring this create just described. args[2] is the
                // IOP work address the EE will DMA to; args[1]/args[3] are the
                // EE-side buffer and its length. Run 55 saw exactly two of
                // these -- {0,0x45E700,0x8040,0x880} and
                // {1,0x458840,0x20F80,0x880} -- and the only two stream DMAs of
                // the entire run matched them field for field.
                std::lock_guard<std::mutex> lock(g_sjxRingMutex);
                SjxStreamRing &slot = g_sjxRings[args[0]];
                slot.iopDest = args[2];
                slot.eeBase  = args[1];
                slot.wkLen   = args[3];
            }
        }
        else if (rpcNum == kSjxFnoSjxCreate || rpcNum == kSjxFnoPs2rnaCreate ||
                 rpcNum == kSjxFnoUniCreate || rpcNum == kSjxFnoUniCreateX ||
                 rpcNum == kSjxFnoUniCreateRmt)
        {
            // Unlike DTX there is no id in the payload to key on -- the caller
            // sends constructor arguments and expects the server to pick the
            // object -- so these come from a bump counter. Running off the end
            // of the window mints nothing and leaves handle at zero, which the
            // game reports through its own "can't creat" path; that is the
            // intended behaviour, not an oversight. The [iop:sjx] line below
            // makes an exhausted window visible as handle=0x00000000 rather
            // than letting it masquerade as a missing service.
            static std::atomic<uint32_t> s_sjxNextObj{kSjxObjHandleBase};
            const uint32_t next =
                s_sjxNextObj.fetch_add(kSjxObjHandleStride, std::memory_order_relaxed);
            if (next < kSjxObjHandleEnd)
            {
                handle = next;
            }
        }

        if (handle != 0u && recvPtr && recvSize >= sizeof(uint32_t))
        {
            std::memcpy(recvPtr, &handle, sizeof(handle));
        }

        resultPtr = recvBufAddr;

        // Unconditional and bounded, for the same reason as [iop:iopheap]: a
        // zero handle here is the difference between a title screen and a white
        // one, and echoing the decoded args tells us whether the RPC bridge
        // recovered the send payload at all. All-zero args with a nonzero
        // sendSize would mean the payload never arrived -- which looks exactly
        // like a legitimate refusal without this line.
        //
        // The cap is generous on purpose. Zero is still the answer for 17 of
        // the 21 commands, so one run's worth of these lines is what tells us
        // the actual boot-path command SEQUENCE -- decoding only the commands
        // the game really issues, instead of one build-and-run cycle per
        // error code. Anything logged with handle=0 that is followed by a park
        // is the next thing to decode.
        static std::atomic<uint32_t> s_sjxLogs{0u};
        if (s_sjxLogs.fetch_add(1u, std::memory_order_relaxed) < 512u)
        {
            std::fprintf(stderr,
                         "[iop:sjx] fno=0x%X args={0x%08X,0x%08X,0x%08X,0x%08X} "
                         "-> handle=0x%08X send=0x%08X/%u recv=0x%08X/%u\n",
                         rpcNum, args[0], args[1], args[2], args[3], handle,
                         sendBufAddr, sendSize, recvBufAddr, recvSize);
        }
        return true;
    }

    if (sid == 0x80000006u) // LOADFILE (IOP loadfile module SIF RPC service)
    {
        // Phase 0 diagnostics ONLY: the game's sceSifLoadModule binds to this
        // service and calls it; we have no handler, so the EE lib returns
        // -65540 and the boot spins. Log the exact packet (rpc function number,
        // send-buffer bytes, and the embedded module path) so the IRX loader can
        // be built against the real arg layout. Still returns false -> no
        // behavioral change this phase.
        static std::atomic<uint32_t> s_loadfileLogs{0u};
        if (s_loadfileLogs.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::fprintf(stderr,
                         "[iop:LOADFILE] rpcNum=%u sendBuf=0x%08X sendSize=%u recvBuf=0x%08X recvSize=%u\n",
                         rpcNum, sendBufAddr, sendSize, recvBufAddr, recvSize);

            const uint8_t *snd = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;
            if (snd && sendSize > 0u)
            {
                const uint32_t dumpLen = sendSize < 96u ? sendSize : 96u;
                std::fprintf(stderr, "[iop:LOADFILE] send[");
                for (uint32_t i = 0; i < dumpLen; i++)
                {
                    std::fprintf(stderr, "%02X", snd[i]);
                    if ((i & 3u) == 3u)
                    {
                        std::fprintf(stderr, " ");
                    }
                }
                std::fprintf(stderr, "]\n");

                // Extract the first printable ASCII run (>=3 chars) as the path.
                std::string path;
                for (uint32_t i = 0; i < sendSize; i++)
                {
                    const char c = static_cast<char>(snd[i]);
                    if (c >= 0x20 && c < 0x7F)
                    {
                        path.push_back(c);
                        if (path.size() >= 128u)
                        {
                            break;
                        }
                    }
                    else if (path.size() >= 3u)
                    {
                        break;
                    }
                    else
                    {
                        path.clear();
                    }
                }
                std::fprintf(stderr, "[iop:LOADFILE] path=\"%s\"\n", path.c_str());
            }
        }

        // S2.1: if this LOADFILE is for ARKD_DVD.IRX, load the real module into
        // IOP RAM (once). Independent of the bounded diagnostic logging above so
        // it still fires after the 32-log cap. Extracts the module path fresh
        // from the send buffer (first printable ASCII run, same scan as above).
        static std::atomic<bool> s_arkdLoadTried{false};
        bool expected = false;
        if (s_arkdLoadTried.compare_exchange_strong(expected, true))
        {
            const uint8_t *snd2 = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;
            if (snd2 && sendSize > 0u)
            {
                std::string path;
                for (uint32_t i = 0; i < sendSize; i++)
                {
                    const char c = static_cast<char>(snd2[i]);
                    if (c >= 0x20 && c < 0x7F)
                    {
                        path.push_back(c);
                        if (path.size() >= 128u) break;
                    }
                    else if (path.size() >= 3u) break;
                    else path.clear();
                }
                std::string upper = path;
                std::transform(upper.begin(), upper.end(), upper.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (upper.find("ARKD_DVD") != std::string::npos)
                {
                    ps2_iop_loadArkdIrx(runtime, path);
                }
                else
                {
                    s_arkdLoadTried.store(false); // wasn't ARKD -- allow a later try
                }
            }
        }

        // Intentionally fall through (return false) -- Phase 0 is diagnostics only.
    }

    // Stage 5.5.1 PROBE: padman (libpad) RPC service.
    //
    // Ground truth read live from PCSX2 (SLUS-21442, paused at the memcard screen):
    //   0x187cb0  binds sid 0x80000100 with client data 0x568940
    //   0x187d04  binds sid 0x80000101 with client data 0x568968
    //   0x189110  version query -- rpcNum 1, send==recv==0x568b80 (0x80 bytes),
    //             cmd word 0x12 at send+0, module version read back from recv+0xC.
    //   0x187d34  sra $s0,$s1,8 ; beq $s0,4 -- ONLY the major nibble is checked,
    //             and the full version word is dead afterwards (0x187d78 passes
    //             only $a0 = $s2), so 0x0400 is behaviourally exact.
    //
    // Built up one command at a time, each round driven by what the previous run
    // logged as UNSERVED, so every reply below is decoded from a real call site
    // rather than guessed: 0x12 (version) -> 0x10 (init) -> 0x01 (port open).
    // Still not served: the per-frame pad state itself, which the real padman
    // pushes asynchronously via SIF CMD 0x80000019 (the guest registers its own
    // handler, callback 0x187d98, on a successful cmd 0x10).
    if (sid == 0x80000100u && rpcNum == 1u)
    {
        const uint8_t *snd = sendBufAddr ? getConstMemPtr(m_rdram, sendBufAddr) : nullptr;
        uint32_t cmd = 0u;
        if (snd && sendSize >= sizeof(uint32_t))
        {
            std::memcpy(&cmd, snd, sizeof(cmd));
        }

        // Reply convention for this service, read off three call sites:
        //   0x189110 (cmd 0x12) returns recv+0xC
        //   0x187de0 (cmd 0x10) returns recv+0xC
        //   0x187ed8 (cmd 0x0F) compares recv+0xC against 1
        // So recv+0xC is the single return slot, and 1 means success.
        uint32_t reply = 0u;
        uint32_t reply14 = 0u;      // recv+0x14, only meaningful for cmd 0x1
        bool haveReply14 = false;
        const char *replyWhat = nullptr;

        switch (cmd)
        {
        case 0x12u: // scePadGetModVersion
            // Major nibble only is inspected (0x187d34: sra $s0,$s1,8 ; beq $s0,4)
            // and the full word is dead afterwards, so 0x0400 is behaviourally exact.
            reply = 0x0400u;
            replyWhat = "modversion";
            break;

        case 0x10u: // scePadInit's open -- send+0x10 = 0, send+0x14 = global 0x464E14
            // On a non-negative rpc_call the guest itself registers the async pad
            // handler (0x177de8 with cmd 0x80000019 -> callback 0x187d98), so we
            // only have to stop returning a failure here. Pad state pushes are a
            // separate piece and are still not served.
            reply = 1u;
            replyWhat = "init";
            break;

        case 0x1u: // scePadPortOpen (0x1880e4)
        {
            // Send layout read off 0x18818c-0x1881a0:
            //   send+0x04 = port, send+0x08 = slot, send+0x10 = EE-side pad buffer.
            // Reply: recv+0xC is the return value (0x188230 lw $v0,12($s0)) and
            // recv+0x14 is an IOP-side address (0x188204 lw $t0,20($s0)) stored into
            // the pad state table at +8. scePadRead (0x187f68) later uses that word
            // as the sceSifSetDma *dest* for a 0x20-byte EE->IOP request, indexing
            // it as a double buffer (+0x20 on alternate frames). So it must be
            // non-zero and must have 0x40 bytes of room; the EE side never reads it.
            uint32_t port = 0u, slot = 0u;
            if (snd && sendSize >= 0xCu)
            {
                std::memcpy(&port, snd + 0x4, sizeof(port));
                std::memcpy(&slot, snd + 0x8, sizeof(slot));
            }
            // Distinctive base so these show up unmistakably in SIF DMA logs.
            reply14 = 0x001E0000u + ((port & 0xFu) * 0x100u) + ((slot & 0x3u) * 0x40u);
            haveReply14 = true;
            reply = 1u;
            replyWhat = "portopen";
            break;
        }

        default:
            break;
        }

        if (replyWhat)
        {
            uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;
            if (recvPtr && recvSize >= 0x10u)
            {
                // Safe to wipe even though send == recv for this service: cmd was
                // already latched above and nothing reads the send words after.
                std::memset(recvPtr, 0, recvSize);
                std::memcpy(recvPtr + 0xC, &reply, sizeof(reply));
                if (haveReply14 && recvSize >= 0x18u)
                {
                    std::memcpy(recvPtr + 0x14, &reply14, sizeof(reply14));
                }
            }

            static std::atomic<uint32_t> s_padServedLogs{0u};
            if (s_padServedLogs.fetch_add(1u, std::memory_order_relaxed) < 16u)
            {
                std::fprintf(stderr,
                             "[iop:PADMAN] served cmd=0x%X (%s): recvBuf=0x%08X recvSize=%u reply=0x%X iopbuf=0x%08X\n",
                             cmd, replyWhat, recvBufAddr, recvSize, reply,
                             haveReply14 ? reply14 : 0u);
            }

            resultPtr = recvBufAddr;
            return true;
        }

        static std::atomic<uint32_t> s_padOtherLogs{0u};
        if (s_padOtherLogs.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::fprintf(stderr,
                         "[iop:PADMAN] UNSERVED cmd=0x%X sendBuf=0x%08X sendSize=%u recvBuf=0x%08X recvSize=%u\n",
                         cmd, sendBufAddr, sendSize, recvBufAddr, recvSize);
        }
        // Fall through unhandled -- the caller treats that as a failed RPC.
    }
    else if (sid == 0x80000101u)
    {
        static std::atomic<uint32_t> s_pad101Logs{0u};
        if (s_pad101Logs.fetch_add(1u, std::memory_order_relaxed) < 32u)
        {
            std::fprintf(stderr,
                         "[iop:PADMAN] sid 0x80000101 rpcNum=%u sendBuf=0x%08X sendSize=%u recvBuf=0x%08X recvSize=%u\n",
                         rpcNum, sendBufAddr, sendSize, recvBufAddr, recvSize);
        }
    }

    if (sid == kIopSidCdvdSearchFile)
    {
        // 2026-08-17 Stage 5.15 -- cdvdfsv's SearchFile RPC, the call that
        // has been failing every .SFD movie open since the beginning.
        //
        // HOW THIS WAS IDENTIFIED, not guessed. sub_1866A0 is the EE-side
        // sceCdSearchFile; decompiled, it binds sid 0x80000597 and issues
        //
        //     sceSifCallRpc(client, 0, send=0x568700/300, recv=0x568840/4)
        //
        // then RETURNS the four-byte recv word verbatim. The 08-17 run log
        // reports that call field for field --
        //     client=0x568880 send=0x568700 ssz=0x12c recv=0x568840 rsz=0x4
        // -- and reports it as UNSERVED. An unserved call leaves recv
        // untouched, so sceCdSearchFile returned 0, so sub_130AD8 returned
        // 0, so sub_130EF0 took its "E0092911 sceCdSearchFile" branch and
        // returned 0, so the movie never opened. Nothing printed anywhere,
        // because every diagnostic in that path is gated on the libcdvd
        // verbosity word at 0x463250, which is 0.
        //
        // This is the same failure shape as the memory card
        // ([[project_stage512_memory_card]]) and the SJX heap above:
        // SILENCE MUST BE WRITTEN, NOT OMITTED.
        //
        // THE 300-BYTE SEND BUFFER, decoded from sub_1866A0's own stores
        // (36 + 256 + 4 + 4 == 300 == the observed 0x12c, so the layout is
        // measured, not assumed):
        //     +0x000  sceCdlFILE out  { u32 lsn; u32 size; char name[16];
        //                               u8 date[8]; }            36 bytes
        //     +0x024  char name[256]  the path being searched for
        //     +0x124  u32  pResult    EE address to write the result to
        //     +0x128  u32  mode
        //
        // The guest reads its answer back out of the send buffer itself
        // (MEMORY[0x20568700..0x20568720] on the success path), so the
        // result is written THERE, and the recv word carries the boolean.
        //
        // [[feedback_no_iop_faking]] does not apply: cdvdfsv ships in the
        // IOP BIOS image, not on the disc as a game module, so there is no
        // IRX for us to run -- exactly the reasoning already recorded for
        // the IOP heap service and the cdvdman S-command path below. And
        // nothing here is invented: the LBN and size come from
        // ps2_iop_cdSearchFile, the same ISO9660 resolver that backs
        // readCdSectors, so a search and the read that follows it agree.
        uint8_t *sendPtr = sendBufAddr ? getMemPtr(m_rdram, sendBufAddr) : nullptr;
        uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;

        // Below this the name field cannot be present at all, and answering
        // "not found" for a request we could not even parse would be a lie
        // of the exact kind this probe class exists to prevent.
        constexpr uint32_t kSearchNameOff = 0x24u;
        constexpr uint32_t kSearchMinSend = kSearchNameOff + 2u;

        std::string wanted;
        if (sendPtr != nullptr && sendSize >= kSearchMinSend)
        {
            const char *raw = reinterpret_cast<const char *>(sendPtr + kSearchNameOff);
            const uint32_t avail = sendSize - kSearchNameOff;
            const uint32_t maxLen = std::min<uint32_t>(avail, 256u);
            for (uint32_t i = 0; i < maxLen && raw[i] != '\0'; ++i)
            {
                wanted.push_back(raw[i]);
            }
        }

        uint32_t lbn = 0u;
        uint32_t size = 0u;
        const bool found = !wanted.empty() &&
                           ps2_iop_cdSearchFile(wanted.c_str(), &lbn, &size);

        if (found && sendPtr != nullptr && sendSize >= 36u)
        {
            // sceCdlFILE, written where the guest actually reads it from.
            std::memset(sendPtr, 0, 36u);
            std::memcpy(sendPtr + 0, &lbn, sizeof(lbn));
            std::memcpy(sendPtr + 4, &size, sizeof(size));

            // The name field is the LEAF only ("ATARI.SFD;1"), matching what
            // a real ISO9660 directory record carries. Copying the full path
            // would overrun the 16-byte field into the date bytes.
            std::string leaf = wanted;
            const size_t cut = leaf.find_last_of("\\/");
            if (cut != std::string::npos)
            {
                leaf = leaf.substr(cut + 1);
            }
            const size_t n = std::min<size_t>(leaf.size(), 15u);
            std::memcpy(sendPtr + 8, leaf.data(), n);
        }

        // The boolean the guest returns straight out of sceCdSearchFile.
        const uint32_t answer = found ? 1u : 0u;
        if (recvPtr != nullptr && recvSize >= sizeof(uint32_t))
        {
            std::memcpy(recvPtr, &answer, sizeof(answer));
        }
        resultPtr = recvBufAddr;

        // Unconditional and bounded, for the same reason as the heap
        // service: the difference between found and not-found here is the
        // difference between a movie playing and the white Atari screen,
        // and a silent failure is what cost this project Stage 5.15.
        static std::atomic<uint32_t> s_searchLogs{0u};
        const uint32_t nLog = s_searchLogs.fetch_add(1u, std::memory_order_relaxed);
        if (nLog < 32u)
        {
            std::fprintf(stderr,
                         "[iop:cdsearch] rpc=%u send=0x%08X/%u recv=0x%08X/%u"
                         " name=\"%s\" found=%u lbn=%u size=%u\n",
                         rpcNum, sendBufAddr, sendSize, recvBufAddr, recvSize,
                         wanted.empty() ? "<unparsed>" : wanted.c_str(),
                         found ? 1u : 0u, lbn, size);
        }
        else if (nLog == 32u)
        {
            std::fprintf(stderr,
                         "[cap] tag=iop:cdsearch saturated at 32 -- later"
                         " searches are invisible; absence past this point"
                         " is NOT evidence.\n");
        }
        return true;
    }

    if (sid == kIopSidCdvdNcmd)
    {
        // 2026-08-17 Stage 5.16 -- cdvdfsv's N-command RPC. Run 62 proved this
        // is the blocker that survives the .SFD open: the file resolves, the
        // handle latches, the SofDec work area allocates, and then the game
        // asks for the actual movie BYTES and nothing answers.
        //
        // DECODED, NOT GUESSED. sub_1875E8 is the EE-side sceCdRead:
        //
        //     dword_463340 = lbn;  dword_463344 = sectors;
        //     dword_463348 = buf;  byte_46334C/D/E = mode[0..2];
        //     dword_463350 = dword_464340;  dword_463354 = &dword_464400;
        //     cache_writeback_range(&dword_463340, 24);
        //     sceSifCallRpc(client=dword_464410, fno=1, mode=1 /*async*/,
        //                   send=&dword_463340, ssz=24, recv=0, rsz=0,
        //                   end_function=0x186310, end_arg=dword_464340)
        //
        // and the run-62 log reports that call field for field:
        //     sid=0x80000595 func=0x1 send=0x463340 ssz=0x18 recv=0x0 rsz=0x0
        //     s[0]=0x157484 s[1]=0x168 s[2]=0x1868a40 s[3]=0x0
        // 0x157484 == 1406084 == the LBN our own SearchFile returned for
        // \MOVIE\ATARI.SFD;1, and 0x168 == 360 sectors == 737280 bytes ==
        // exactly the size we reported. The guest is asking us to read back
        // the file we just told it about.
        //
        // rsz=0 means there is no reply buffer at all: completion is signalled
        // purely through end_function 0x186310, which is why [rpcend] fired 24
        // times in run 62 while the buffer stayed empty. The completion path
        // was already correct; there was simply no DATA behind it.
        //
        // fno 14 (0x0E) is the status poll, from sub_186B18:
        //     sceSifCallRpc(client, 14, 0 /*sync*/, 0,0, recv=dword_4632C0, 4)
        //     return MEMORY[0x204632C0];
        // Its callers gate on `!= 6`; sub_1875E8 refuses to issue the read at
        // all when it reads 6. Unserved, that word keeps whatever was there,
        // which is the silence-is-not-an-answer failure this project has now
        // paid for four separate times.
        //
        // [[feedback_no_iop_faking]] does not apply, for the reason already
        // recorded on the SearchFile handler above: cdvdfsv lives in the IOP
        // BIOS image, not on the disc, so there is no IRX to run in the R3000.
        // Nothing is invented -- the sectors are real ISO bytes via the same
        // reader that backs every other CD path in the runtime.
        uint8_t *sendPtr = sendBufAddr ? getMemPtr(m_rdram, sendBufAddr) : nullptr;
        uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;

        // sceCdStatus bit for "disc spinning, ready". Deliberately NOT 6
        // (PAUSE), the one value sub_1875E8 treats as "do not read".
        constexpr uint32_t kCdStatSpin = 0x02u;
        constexpr uint32_t kNcmdRead   = 1u;
        constexpr uint32_t kNcmdStatus = 14u;

        bool handled = false;
        bool readOk = false;
        uint32_t lbn = 0u, sectors = 0u, bufAddr = 0u, secSize = 2048u;

        // Run 63 evidence: 24 identical full-file reads inside a 6 s window,
        // every one of them readOk=1, and the ADX slot never left st=2/pau=1.
        // That is a retry loop, so the two things the guest could be reacting
        // to have to be measured rather than reasoned about:
        //   descAddr (send+0x10, == dword_464340, the 144-byte block
        //             sub_1875E8 cache-writebacks) and
        //   resAddr  (send+0x14, == &dword_464400, which sub_1875E8 zeroes
        //             immediately before the call -- i.e. a word it expects
        //             SOMEBODY to fill in).
        // dstHead is the read's own output: readOk=1 only says the reader
        // returned true, not that bytes arrived. A .SFD is an MPEG program
        // stream, so a healthy head starts 00 00 01 BA.
        uint32_t descAddr = 0u, resAddr = 0u, resWord = 0u, descW0 = 0u;
        uint64_t dstHead = 0u;

        if (rpcNum == kNcmdRead && sendPtr != nullptr && sendSize >= 0x10u)
        {
            std::memcpy(&lbn, sendPtr + 0x00, 4u);
            std::memcpy(&sectors, sendPtr + 0x04, 4u);
            std::memcpy(&bufAddr, sendPtr + 0x08, 4u);
            if (sendSize >= 0x18u)
            {
                std::memcpy(&descAddr, sendPtr + 0x10, 4u);
                std::memcpy(&resAddr, sendPtr + 0x14, 4u);
            }

            // Sector stride, straight out of sub_1875E8's own arithmetic:
            //   mode[2]==1 -> 2328, mode[2]==2 -> 2340, else 2048.
            const uint8_t modeSize = sendPtr[0x0E];
            secSize = (modeSize == 1u) ? 2328u : (modeSize == 2u) ? 2340u : 2048u;

            const uint64_t bytes = static_cast<uint64_t>(sectors) * secSize;
            uint8_t *dst = (bufAddr != 0u && sectors != 0u &&
                            bytes <= 0x02000000ull)
                               ? getMemPtr(m_rdram, bufAddr)
                               : nullptr;
            if (dst != nullptr)
            {
                readOk = ps2_iop_cdReadSectors(lbn, sectors, dst,
                                               static_cast<size_t>(bytes));
                std::memcpy(&dstHead, dst, sizeof(dstHead));
            }

            if (uint8_t *descPtr = descAddr ? getMemPtr(m_rdram, descAddr) : nullptr)
                std::memcpy(&descW0, descPtr, sizeof(descW0));
            if (uint8_t *resPtr = resAddr ? getMemPtr(m_rdram, resAddr) : nullptr)
                std::memcpy(&resWord, resPtr, sizeof(resWord));

            handled = true;
        }
        else if (rpcNum == kNcmdStatus)
        {
            if (recvPtr != nullptr && recvSize >= sizeof(uint32_t))
            {
                const uint32_t status = kCdStatSpin;
                std::memcpy(recvPtr, &status, sizeof(status));
            }
            resultPtr = recvBufAddr;
            handled = true;
        }

        // Cap policy, rewritten after run 63: a flat count-cap turned a
        // 6-second burst into a blind spot for the remaining 106 seconds, so
        // "the LBN never changed" was unprovable exactly when it mattered.
        // Instead: always print while the (fno,lbn,sectors) tuple is NEW, and
        // once it starts repeating print only a decimating heartbeat. A retry
        // loop therefore collapses to a few lines, while any change of target
        // -- the one thing worth seeing -- is never suppressed.
        // Per-fno slots, because the guest interleaves status polls with reads
        // -- a single "last command" slot would see fno flip 14,1,14,1 and
        // call every one of them new, suppressing nothing.
        static std::atomic<uint32_t> s_ncmdSeq{0u};
        static uint32_t s_lastLbn[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        static uint32_t s_lastSectors[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        static uint32_t s_repeatsBy[3] = {0u, 0u, 0u};

        const size_t slot = (rpcNum == kNcmdRead)     ? 0u
                            : (rpcNum == kNcmdStatus) ? 1u
                                                      : 2u;
        const uint32_t seq = s_ncmdSeq.fetch_add(1u, std::memory_order_relaxed);
        const bool changed =
            (lbn != s_lastLbn[slot]) || (sectors != s_lastSectors[slot]);
        if (changed)
        {
            s_lastLbn[slot] = lbn;
            s_lastSectors[slot] = sectors;
            s_repeatsBy[slot] = 0u;
        }
        else
        {
            ++s_repeatsBy[slot];
        }
        const uint32_t repeats = s_repeatsBy[slot];

        // 1,2,4,8... on repeats: dense at the start of a loop, then quiet.
        const bool heartbeat = (repeats & (repeats - 1u)) == 0u;
        if (changed || heartbeat)
        {
            std::fprintf(stderr,
                         "[iop:ncmd] seq=%u rep=%u %s fno=%u handled=%u lbn=%u"
                         " sectors=%u buf=0x%08X secSize=%u readOk=%u"
                         " dstHead=0x%016llX desc=0x%08X descW0=0x%08X"
                         " res=0x%08X resWord=0x%08X send=0x%08X/%u"
                         " recv=0x%08X/%u\n",
                         seq, repeats, changed ? "NEW" : "rpt", rpcNum,
                         handled ? 1u : 0u, lbn, sectors, bufAddr, secSize,
                         readOk ? 1u : 0u,
                         static_cast<unsigned long long>(dstHead), descAddr,
                         descW0, resAddr, resWord, sendBufAddr, sendSize,
                         recvBufAddr, recvSize);
        }

        // Only claim the packet when we actually did the work. An fno we do
        // not understand must keep falling through to the rpc-miss census,
        // otherwise the next blocker becomes invisible exactly the way this
        // one was.
        if (handled)
        {
            return true;
        }
    }

    if (sid == IOP_SID_CDVD_SCMD)
    {
        // cdvdman S-command RPC (sid 0x80000593).
        //
        // DECODED, NOT GUESSED -- run 68 convicted the previous version of this
        // handler. Every EE libcdvd S-command wrapper in SLUS_214.42 has the
        // same tail, e.g. sub_187898 (sceCdGetError):
        //
        //     if (!lockCmd(3)) return -1;
        //     if (sceSifCallRpc(dword_464DC0, /*fno*/4, 0,0,0,
        //                       &dword_464440, /*rsz*/4, 0,0) >= 0) {
        //         v1 = MEMORY[0x20464440];   // <-- recv word[0]
        //         unlockCmd();
        //         return v1;                 // <-- returned VERBATIM
        //     }
        //
        // sub_187930 (fno 12), sub_1879E8 (fno 22) and sub_187AA0 (variable
        // fno) are byte-for-byte the same shape. So recv word[0] is the
        // COMMAND'S VALUE, not a success flag. The old blanket "word[0] = 1
        // means ok" therefore made sceCdGetError() answer 1 -- and 1 is not
        // one of the two codes the game tolerates, so:
        //
        //     [srd] fn=187898 sceCdGetError ret=0x1
        //     [Deci2Call:kputs] SRD: Drive Error (sceCdGetError = 0x1)
        //     [srd] fn=12ecc0 errchk preStat=0x2 ret=0x1 objErr=0x1
        //
        // fired 248 times in run 68 while the underlying reads all came back
        // readOk=1. The bytes were always there; our own reply said the drive
        // had failed. Hence a per-fno table: a value-returning command must be
        // given its value, never a flag.
        //
        // [[feedback_no_iop_faking]] does not apply for the reason already
        // recorded on the SearchFile and N-command handlers above: cdvdman
        // lives in the IOP BIOS image, not on the disc, so there is no IRX to
        // run in the R3000 interpreter.
        constexpr uint32_t kScmdDvdDualInfo = 1u;   // sub_187B98, rsz=16
        constexpr uint32_t kScmdGetError    = 4u;   // sub_187898
        constexpr uint32_t kScmdStatus      = 12u;  // sub_187930

        // SCECdErNO. sub_12ECC0 tolerates only 0 / 0xFFFFFFFF (and 0x20 as a
        // warning); anything else is a hard drive error.
        constexpr uint32_t kCdErrNone = 0u;
        // SCECdStatSpin -- disc present and spinning. Deliberately NOT 1
        // (SCECdStatOpen, "tray open") and not 6 (SCECdStatRead), matching the
        // value the N-command status poll above already reports.
        constexpr uint32_t kCdStatSpin = 0x02u;

        bool knownFno = true;
        uint32_t word0 = 0u;
        switch (rpcNum)
        {
        case kScmdGetError:
            word0 = kCdErrNone;
            break;
        case kScmdStatus:
            word0 = kCdStatSpin;
            break;
        case kScmdDvdDualInfo:
            // sub_187B98 returns word[0] as a 0/1 success flag and hands
            // word[1] back to the caller as on_dual/layer1_start. word[1]
            // stays zeroed => "succeeded, single layer", correct for a flat
            // single-image disc.
            word0 = 1u;
            break;
        default:
            // An fno we have not decoded. Answering 1 is what broke
            // sceCdGetError, and answering 0 would be an equally blind guess,
            // so keep the historical value but make the guess VISIBLE -- an
            // undecoded S-command must never again fail silently.
            knownFno = false;
            word0 = 1u;
            break;
        }

        uint8_t *recvPtr = recvBufAddr ? getMemPtr(m_rdram, recvBufAddr) : nullptr;
        if (recvPtr && recvSize > 0)
        {
            std::memset(recvPtr, 0, recvSize);
            if (recvSize >= sizeof(uint32_t))
                std::memcpy(recvPtr, &word0, sizeof(word0));
        }

        // Census, not a cap: one line per DISTINCT fno, so a poll loop cannot
        // drown it and the absence of a line still means "never called".
        {
            static std::mutex s_scmdMutex;
            static std::set<uint32_t> s_scmdSeen;
            std::lock_guard<std::mutex> lk(s_scmdMutex);
            if (s_scmdSeen.insert(rpcNum).second)
            {
                std::fprintf(stderr,
                             "[iop:cdscmd] fno=%u %s -> word0=0x%08X"
                             " recv=0x%08X/%u send=0x%08X/%u%s\n",
                             rpcNum, knownFno ? "decoded" : "UNDECODED",
                             word0, recvBufAddr, recvSize, sendBufAddr,
                             sendSize,
                             knownFno ? ""
                                      : "  <-- GUESSED reply; the EE wrapper"
                                        " returns this word verbatim");
            }
        }

        resultPtr = recvBufAddr;
        return true;
    }

    if (traceMcserv && (sid == IOP_SID_MCSERV || sid == IOP_SID_MCSERV_LEGACY))
    {
        static std::atomic<uint32_t> s_mcservMissLogs{0u};
        if (s_mcservMissLogs.fetch_add(1u, std::memory_order_relaxed) < 96u)
        {
            std::fprintf(stderr,
                         "[iop:mcserv-route] miss sid=0x%08X rpc=0x%X\n",
                         sid, rpcNum);
        }
    }

    return false;
}
