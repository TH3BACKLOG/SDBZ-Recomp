#include "Common.h"
#include "SIF.h"
#include "../Syscalls/RPC.h"
#include "../Syscalls/Thread.h"
#include "runtime/ps2_address.h"

#include <map>
#include <string>
#include <cstdlib>

// S2.1: real ARKD_DVD.IRX loader (ps2_iop_irx_loader.cpp). The game loads its
// IRX modules through its OWN embedded loadfile RPC client, which is serviced by
// the echo path below -- NOT ps2_iop::handleRPC and NOT the sceSifLoadModule
// syscall. So the ONLY place the module path "cdrom0:\ARKD_DVD.IRX;1" flows
// through our code is the loadfile CALL send buffer in deliverSifRpcReply. We
// trigger the loader there. Declared here (no header touched).
extern bool ps2_iop_loadArkdIrx(PS2Runtime *runtime, const std::string &modulePath);
// S2.2d-1: run one registered ARKD RPC service func in the persistent IopCpu and
// copy its reply into the guest recv buffer. Returns true iff it delivered real
// data (defined in ps2_iop_irx_loader.cpp; no header touched, per project rules).
extern bool ps2_iop_runArkdService(PS2Runtime *runtime,
                                   uint32_t sid, uint32_t rpcNum,
                                   const uint8_t *sendData, uint32_t sendSize,
                                   uint8_t *recvOut, uint32_t recvSize);

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                if (!src || !dst)
                {
                    break;
                }
                *dst = *src;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::mutex g_sifHeapMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        std::unordered_map<uint32_t, uint32_t> g_sifCmdHandlers;
        std::map<uint32_t, uint32_t> g_sifHeapAllocations;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        uint32_t allocateSifDmaTransferId()
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            return id;
        }

        // --- SIF-RPC bind-reply delivery (unblocks WaitSema@0x174ce0 boot spin) ---
        //
        // SDBZ hosts its own EE-side SIF-RPC client. It issues the RPC BIND request as
        // an IOP-bound packet through sceSifSetDma (cid 0x80000009, sid 0x5), then parks
        // in WaitSema (trampoline 0x174ce0) waiting for the IOP's bind acknowledgement.
        // Nothing in our runtime ever delivered that reply, so the game's EE-side SIF
        // receive queue at 0x561600 stayed empty and the WaitSema never woke.
        //
        // Layout pinned from the game's own code (decompiles_SLUS_214_42.txt):
        //   * dword_5616D8 = 0x20561600  -> receive-queue base; *(u8*)0x561600 is the
        //     "packet pending / size" byte the dispatcher sub_178068 polls (returns
        //     immediately while zero -> the spin).
        //   * sub_178068 copies the queued packet into a local and, for cid<0, indexes
        //     system handler table dword_5616E4 (=0x561700, 32 slots x 12B: fn/arg/gp)
        //     by (cid & 0x7FFFFFFF); slot 9 holds _request_bind = sub_178938.
        //   * sub_178938 reads reply words [5]/[7]/[8] (server buf / client buf / rpc id)
        //     and then re-sends _request_end (cid 0x80000008 -> sub_178560), whose
        //     LABEL_10 does SignalSema (0x174cd0) on the client sema -> WaitSema wakes.
        //
        // We do NOT invent IOP state: every field written back is echoed from the bind
        // request the EE itself supplied (buf/cbuf/sid). We then drive the game's own
        // dispatcher sub_178068 (0x178068) so its real callbacks run and signal the sema.
        constexpr uint32_t kSifRxQueueAddr = 0x561600u;    // physical EE address
        constexpr uint32_t kSifCmdRpcBind = 0x80000009u;   // outbound: transport cid for BIND
        constexpr uint32_t kSifCmdRpcEnd = 0x80000008u;    // outbound: transport cid for END re-send
        constexpr uint32_t kSifCmdRpcCall = 0x8000000Au;   // outbound: transport cid for RPC CALL
        constexpr uint32_t kSifDispatcherFn = 0x00178068u;  // game EE SIF RX dispatcher
        constexpr uint32_t kSifDispatcherEnd = 0x001781B0u; // one past its last instruction
        constexpr uint32_t kSifDispatcherMaxResume = 64u;   // bound on mid-body resumes

        // The dispatcher (sub_178068) routes on packet WORD[8] (byte offset 0x20),
        // masked with 0x7FFFFFFF -> a slot in the system handler table dword_5616E4.
        // Registrations (state_sub_unk_b):
        //   slot 8  (0x80000008) -> sub_178560 _request_end  (SignalSema + client teardown)
        //   slot 9  (0x80000009) -> sub_178938 _request_bind (re-sends 0x80000008 to the IOP)
        // A real IOP RPC reply lands with WORD[8] == 0x80000008 so it routes to
        // _request_end and wakes the parked WaitSema@0x174ce0. Echoing the game's own
        // outbound packet verbatim leaves WORD[8] == 0x80000009 -> re-routes to
        // _request_bind -> another re-send -> infinite bind loop. So on delivery we set
        // WORD[8] to the request-completion discriminator (a fixed libsifrpc protocol
        // constant, identical for every PS2 title -- not an invented IOP data value).
        constexpr uint32_t kSifDiscWordIdx = 8u;             // WORD[8] == byte offset 0x20
        constexpr uint32_t kSifRpcEndDiscriminator = 0x80000008u;

        // One captured outbound RPC system-command packet (0x8000000x). We echo the game's
        // OWN packet back into its RX queue and let its dispatcher route it -- no invented
        // IOP state, just the loopback the real IOP would have produced.
        struct SifRpcReplyPacket
        {
            uint32_t words[16] = {0}; // 64-byte packet as the game built it
            uint32_t cid = 0u;        // words[2]
            bool valid = false;
        };

        // --- Stage 1 ARKD DVD-read RPC trace (observe-only) ---------------------
        // The game boots to a stable main loop but hangs waiting on ARKD DVD-read
        // RPC results that never arrive. ARKD's CALLs ride this echo path (they
        // never reach ps2_iop::handleRPC). Stage 1 does NOT synthesize any result
        // (honors no-IOP-faking): it only OBSERVES -- forces a full packet dump for
        // ARKD traffic and, in the CALL branch, invokes handleRPC in a throwaway
        // capacity to log whether any handler exists yet. Everything is gated behind
        // PS2_ARKD_TRACE so it is zero-cost and cannot regress the boot when unset.
        inline bool arkdTraceEnabled()
        {
            static const bool s_on = (std::getenv("PS2_ARKD_TRACE") != nullptr);
            return s_on;
        }

        // Known ARKD SIF-RPC client control blocks + DVD cmd/reply buffers (EE
        // physical, from PS2_PROJECT_STATE.md). Clients appear as WORD[7] of a
        // packet; the 48-byte cmd buffer is 0x5A97D0 and the 16-byte reply buffer
        // is 0x5AA7D0. A packet touching any of these is ARKD-related.
        inline bool isArkdPacket(const uint32_t w[16])
        {
            const uint32_t client = w[7];
            if (client == 0x005A9330u || client == 0x005A9358u ||
                client == 0x005A9380u || client == 0x005A93A8u)
            {
                return true;
            }
            for (uint32_t k = 0; k < 16u; ++k)
            {
                if (w[k] == 0x005A97D0u || w[k] == 0x005AA7D0u)
                {
                    return true;
                }
            }
            return false;
        }

        // Track clientObj(WORD[7]) -> bound sid(WORD[8]) captured at BIND time, so a
        // later CALL on the same client can be routed to handleRPC with its real sid
        // (a CALL packet carries the rpc func in WORD[8], not the sid). Lock-free
        // small linear table; only written under PS2_ARKD_TRACE.
        struct ArkdBindSlot
        {
            std::atomic<uint32_t> clientObj{0u};
            std::atomic<uint32_t> sid{0u};
        };
        ArkdBindSlot g_arkdBinds[16];

        // --- RPC CALL send-payload capture -----------------------------------
        // sceSifCallRpc does NOT put the send-data pointer in the call packet.
        // It emits the payload as a SEPARATE DMA entry in the same list, ahead
        // of the 0x40-byte control packet, e.g. for ARKD sid 0x503 fno 0x102:
        //     i=0 src=0x5a97d0 size=0x30   <- payload
        //     i=1 src=0x20561900 size=0x40 <- SifRpcCallPkt
        // The packet's own layout is (confirmed empirically against this game,
        // NOT the older WORD[5]/WORD[6] guess):
        //     w[5] = pkt_addr   (the packet's OWN address -- self-referential)
        //     w[6] = rpc_id     (per-call sequence counter: 0x1a, 0x1b, ...)
        //     w[9] = send_size  (0x30 here; 0 when the call sends nothing)
        // So we record each list's entries and let the CALL handler match the
        // entry whose size == w[9] to recover the real payload address.
        struct SifDmaListEntry { uint32_t src; uint32_t size; };
        thread_local SifDmaListEntry g_sifListEntries[32];
        thread_local uint32_t        g_sifListCount = 0u;

        // Returns the EE address of the payload for a CALL with send_size
        // `want`, or 0 if this list carried no matching entry. `pktSrc` is
        // excluded so the control packet can never be mistaken for payload.
        inline uint32_t sifFindSendPayload(uint32_t want, uint32_t pktSrc)
        {
            if (want == 0u) { return 0u; }
            for (uint32_t i = 0; i < g_sifListCount; ++i)
            {
                if (g_sifListEntries[i].size == want &&
                    g_sifListEntries[i].src  != pktSrc)
                {
                    return g_sifListEntries[i].src;
                }
            }
            return 0u;
        }

        inline void arkdRecordBind(uint32_t clientObj, uint32_t sid)
        {
            if (clientObj == 0u)
            {
                return;
            }
            for (auto &slot : g_arkdBinds)
            {
                if (slot.clientObj.load(std::memory_order_relaxed) == clientObj)
                {
                    slot.sid.store(sid, std::memory_order_relaxed);
                    return;
                }
            }
            for (auto &slot : g_arkdBinds)
            {
                uint32_t expect = 0u;
                if (slot.clientObj.compare_exchange_strong(
                        expect, clientObj, std::memory_order_relaxed))
                {
                    slot.sid.store(sid, std::memory_order_relaxed);
                    return;
                }
            }
        }

        inline uint32_t arkdLookupSid(uint32_t clientObj)
        {
            for (auto &slot : g_arkdBinds)
            {
                if (slot.clientObj.load(std::memory_order_relaxed) == clientObj)
                {
                    return slot.sid.load(std::memory_order_relaxed);
                }
            }
            return 0u;
        }
        // --- end Stage 1 ARKD trace helpers -------------------------------------

        // Copy the reply packet verbatim into the EE RX queue and run the game's dispatcher
        // so its registered callbacks fire. 0x80000009 -> _request_bind (re-sends 0x80000008);
        // 0x80000008 -> _request_end -> SignalSema on the parked WaitSema.
        void deliverSifRpcReply(uint8_t *rdram,
                                R5900Context *ctx,
                                PS2Runtime *runtime,
                                const SifRpcReplyPacket &req)
        {
            if (!runtime || !req.valid)
            {
                return;
            }

            // Bound re-entrancy: BIND -> (re-send) END is 2 levels; cap generously so a
            // pathological RPC storm can't blow the host stack.
            static thread_local uint32_t s_deliverDepth = 0u;
            if (s_deliverDepth >= 8u)
            {
                static std::atomic<uint32_t> s_depthLogs{0u};
                if (s_depthLogs.fetch_add(1u, std::memory_order_relaxed) < 8u)
                {
                    std::cerr << "[SifRpcReply] re-entrancy cap hit (cid=0x" << std::hex
                              << req.cid << std::dec << "), dropping reply" << std::endl;
                }
                return;
            }

            uint8_t *q = getMemPtr(rdram, kSifRxQueueAddr);
            if (!q)
            {
                return;
            }

            // Start from the game's own 64-byte packet so client ptr (word[7]) and
            // server fields survive verbatim -- we invent no IOP data. Word[0] low byte
            // doubles as the pending/size byte the dispatcher reads first; force it
            // non-zero.
            uint32_t pkt[16];
            std::memcpy(pkt, req.words, sizeof(pkt));
            if ((pkt[0] & 0xFFu) == 0u)
            {
                pkt[0] = (pkt[0] & ~0xFFu) | 0x40u;
            }

            // v4 (source-grounded, decompiles_SLUS_214_42.txt). Two discriminator words:
            //   * dispatcher sub_178068 routes on WORD[2] (byte offset 8);
            //   * handler _request_end sub_178560 (0x178560) routes on WORD[8].
            // The retry loop sub_17CF50 breaks ONLY when dword_5649A4 (= client_block[9])
            // becomes nonzero, and the SOLE writer is _request_end's register branch:
            //   WORD[8]==0x80000009 -> v3[9]=WORD[9]; v3[5]=WORD[10]; then SignalSema.
            // (v3 = WORD[7] = client control block = 0x20564980.)
            //
            // The real IOP, after registering the bind, returns exactly one completion
            // packet routed to _request_end carrying the register sub-discriminator and a
            // NONZERO server id. v3's bug: an outbound BIND was routed to _request_bind
            // (which only re-sends END, never sets the flag) and an outbound END carried
            // WORD[8]=0x80000008 (hits NEITHER branch of _request_end -> flag stays 0).
            //
            // Synthesize that single completion reply for the outbound BIND. Every field
            // is echoed from the request the guest itself sent -- no invented IOP data:
            //   WORD[2] = 0x80000008  -> dispatcher routes to _request_end
            //   WORD[7] = req WORD[7] -> client control block (survives from memcpy)
            //   WORD[8] = 0x80000009  -> _request_end register branch
            //   WORD[9] = req WORD[4] -> the bind sid the client asked for (the server
            //                            id; nonzero, opaque token the guest just echoes
            //                            back to the IOP in later sceSifCallRpc via +52).
            //   WORD[10]= req WORD[5] -> the client rpc buffer ptr (v3[5], echoed).
            // The guest's own outbound END packets are then redundant -- they no longer
            // need a loopback because the flag is already set by this BIND completion.
            // Track the client object bound to the system LOADFILE service (sid
            // 0x80000006) so its version-handshake CALL (func 255) is recognisable
            // below. In a BIND packet the sid rides in WORD[8] and the client object
            // in WORD[7] -- both read from the guest's own packet, nothing invented.
            static std::atomic<uint32_t> s_loadfileClientObj{0u};
            if (req.cid == kSifCmdRpcBind && req.words[kSifDiscWordIdx] == 0x80000006u)
            {
                s_loadfileClientObj.store(req.words[7], std::memory_order_relaxed);
            }

            // --- S2.1: real ARKD_DVD.IRX load trigger ------------------------
            // Fire off the FIRST ARKD service BIND (sid 0x500..0x503 in WORD[8]).
            // By the time the game binds an ARKD service it has already "loaded"
            // the module (echo path faked the loadfile completion), so this is a
            // reliable, trace-independent trigger that does NOT depend on parsing
            // a path out of a send buffer. The module path is a fixed ISO
            // constant (reference_iso_layout); the loader strips cdrom0:/;ver.
            // Idempotent: the loader itself guards against a second load.
            if (req.cid == kSifCmdRpcBind)
            {
                const uint32_t bindSid = req.words[kSifDiscWordIdx];
                if (bindSid >= 0x500u && bindSid <= 0x503u)
                {
                    static std::atomic<bool> s_arkdIrxTried{false};
                    bool expected = false;
                    if (s_arkdIrxTried.compare_exchange_strong(expected, true))
                    {
                        const std::string modPath = "cdrom0:\\ARKD_DVD.IRX;1";
                        std::cerr << "[iop:irx] ARKD BIND sid=0x" << std::hex << bindSid
                                  << std::dec << " -> loading real ARKD_DVD.IRX ("
                                  << modPath << ")" << std::endl;
                        ps2_iop_loadArkdIrx(runtime, modPath);
                    }
                }
            }
            // --- end S2.1 load trigger ---------------------------------------

            // Stage 1: record clientObj->sid for every BIND so a later CALL can be
            // routed to handleRPC with its real bound sid (see arkdRecordBind).
            if (arkdTraceEnabled() && req.cid == kSifCmdRpcBind)
            {
                arkdRecordBind(req.words[7], req.words[kSifDiscWordIdx]);
                if (isArkdPacket(req.words))
                {
                    std::cerr << "[ARKD:BIND] client=0x" << std::hex << req.words[7]
                              << " sidW8=0x" << req.words[kSifDiscWordIdx]
                              << " sidW4=0x" << req.words[4] << std::dec << std::endl;
                }
            }

            if (req.cid == kSifCmdRpcCall)
            {
                // Outbound RPC CALL (0x8000000A) -> synthesize the IOP's call completion.
                // _request_end (sub_178560) callback branch: WORD[8]==0x8000000A reads
                // v3=WORD[7] (client block); if v3[7] (completion callback) set, invokes
                // v3[7](v3[8]); then LABEL_10 SignalSema iff v3[2] >= 0 -> wakes the sync
                // WaitSema inside rpc_call (0x178BE8). We deliver NO result data (the IOP
                // server's output is not invented here); this only unblocks the wait.
                pkt[2] = kSifCmdRpcEnd;                 // dispatcher -> _request_end
                pkt[kSifDiscWordIdx] = kSifCmdRpcCall;  // WORD[8] -> callback branch
                // WORD[7] (client block ptr) survives verbatim from the memcpy.

                // --- Stage 1 observe-only bridge to handleRPC (ARKD only) --------
                // We do NOT apply any result here (no IOP faking) -- the existing
                // echo synthesis below stays authoritative so the stable main loop
                // is preserved. This only LOGS the real runtime contract so Stage 2
                // (real ARKD_DVD.IRX in R3000) knows exactly what to satisfy.
                if (arkdTraceEnabled() && isArkdPacket(req.words))
                {
                    const uint32_t client = req.words[7];
                    const uint32_t rpcNum = req.words[kSifDiscWordIdx]; // func number
                    const uint32_t recvAddr = req.words[10];
                    const uint32_t recvSize = req.words[11];
                    uint32_t sid = arkdLookupSid(client);              // bound at BIND
                    // Send payload. The old WORD[5]/WORD[6] guess is WRONG and was
                    // feeding ARKD the control packet instead of the real payload:
                    // w[5] is the packet's own address and w[6] is a per-call
                    // sequence counter (observed 0x1a then 0x1b on consecutive
                    // calls). The true size is w[9], and the data itself rides a
                    // separate DMA entry in the same list -- see
                    // sifFindSendPayload. sendAddr stays 0 for calls that send
                    // nothing (w[9]==0), which is correct: fno=0x2 has w[9]==0 and
                    // emits no companion entry.
                    const uint32_t sendSize = req.words[9];
                    const uint32_t sendAddr =
                        sifFindSendPayload(sendSize, req.words[5]);

                    static std::atomic<uint32_t> s_arkdCallLogs{0u};
                    const uint32_t n =
                        s_arkdCallLogs.fetch_add(1u, std::memory_order_relaxed);
                    if (n < 64u)
                    {
                        std::cerr << "[ARKD:CALL] client=0x" << std::hex << client
                                  << " sid=0x" << sid << " func=0x" << rpcNum
                                  << " send=0x" << sendAddr << " ssz=0x" << sendSize
                                  << " recv=0x" << recvAddr << " rsz=0x" << recvSize
                                  << std::dec;
                        for (uint32_t k = 0; k < 16u; ++k)
                        {
                            std::cerr << " w[" << k << "]=0x" << std::hex
                                      << req.words[k] << std::dec;
                        }
                        std::cerr << std::endl;

                        // Hexdump first 48 bytes of the send buffer (classic ARKD
                        // 48-byte DVD cmd) if it points at readable EE memory.
                        if (const uint8_t *sb = getConstMemPtr(rdram, sendAddr))
                        {
                            std::cerr << "[ARKD:CALL] send[0..48]=";
                            for (uint32_t b = 0; b < 48u; ++b)
                            {
                                std::cerr << std::hex << static_cast<int>(sb[b]) << " "
                                          << std::dec;
                            }
                            std::cerr << std::endl;
                        }

                        // Observe-only handleRPC: throwaway out-params, result never
                        // written back to the guest. Logs whether ANY handler claims
                        // this sid today (expected: false -> the Stage 2 gap).
                        uint32_t obsResult = 0u;
                        bool obsSignal = false;
                        runtime->iop().init(rdram);
                        const bool handled = runtime->iop().handleRPC(
                            runtime, sid, rpcNum, sendAddr, sendSize, recvAddr,
                            recvSize, obsResult, obsSignal);
                        std::cerr << "[ARKD:CALL] handleRPC -> "
                                  << (handled ? "TRUE" : "false")
                                  << " result=0x" << std::hex << obsResult
                                  << std::dec << " signal=" << (obsSignal ? 1 : 0)
                                  << std::endl;
                    }
                }
                // --- end Stage 1 observe-only bridge -----------------------------

                // --- S2.2d-2: real service bridge (gated PS2_ARKD_SERVICE=1) -----
                // For a bound ARKD DVD-read CALL (sid 0x503), run the real service
                // func 0x042700 in the embedded R3000 and drop its reply into the
                // guest recv buffer BEFORE the echo synthesis below signals the
                // waiter. Requires PS2_ARKD_IRX_RUN=1 (so the services are
                // registered). ps2_iop_runArkdService only writes recvPtr on a
                // clean, data-delivering run; on any failure it is a no-op, so
                // today's observe-only behavior is preserved (no IOP faking) and
                // the boot cannot regress.
                {
                    static const bool s_svc =
                        (std::getenv("PS2_ARKD_SERVICE") != nullptr);
                    if (s_svc && isArkdPacket(req.words))
                    {
                        const uint32_t client = req.words[7];
                        const uint32_t sid    = arkdLookupSid(client);
                        // The EE's RPC exchange begins at sid 0x500 (observed live
                        // 2026-07-19), not 0x503, so a 0x503-only gate never fires
                        // and the EE stalls on the unanswered 0x500 CALL. Service
                        // all four registered ARKD sids (0x500..0x503): runArkdService
                        // is sid-generic (looks up svc.func by sid) and no-ops on any
                        // failure, so it cannot regress boot / fake data.
                        if (sid >= 0x500u && sid <= 0x503u)
                        {
                            const uint32_t rpcNum   = req.words[kSifDiscWordIdx];
                            // Real send payload -- w[9] size + companion DMA entry.
                            // This is the copy that reaches the IOP service buffer,
                            // so the old w[5]/w[6] read was handing ARKD the control
                            // packet: its first word became dword_BF50, the SIF DMA
                            // destination, which is why the TOC was being written to
                            // EE address 0.
                            const uint32_t sendSize = req.words[9];
                            const uint32_t sendAddr =
                                sifFindSendPayload(sendSize, req.words[5]);
                            const uint32_t recvAddr = req.words[10];
                            const uint32_t recvSize = req.words[11];
                            const uint8_t *sendPtr =
                                sendAddr ? getConstMemPtr(rdram, sendAddr) : nullptr;
                            uint8_t *recvPtr =
                                recvAddr ? getMemPtr(rdram, recvAddr) : nullptr;
                            // Fire on service identity, not reply size. ARKD's
                            // init calls (sid 0x500 fno 0x12/0x11 = max-files,
                            // pitch) carry a send payload but request rsize=0,
                            // so gating on recvSize silently dropped them and
                            // left the driver unconfigured for the later 0x503
                            // read. recvSize==0 is handled downstream: it only
                            // skips the reply copy-out.
                            ps2_iop_runArkdService(runtime, sid, rpcNum,
                                                   sendPtr, sendSize,
                                                   recvPtr, recvSize);
                        }
                    }
                }
                // --- end S2.2d-2 real service bridge -----------------------------

                // System LOADFILE (sid 0x80000006) version handshake. sub_17D4A0
                // (d4a0) issues sceSifCallRpc(clientObj, 255, ...) then copies its
                // 4-byte recv buffer dword_564B40 -> dword_564D68; the gate at
                // sub_17D5A0 (d5a0) compares 564D68 against dword_461B5C ("3000" =
                // 0x30303033) and boot spins on -65540 until it matches. The loadfile
                // module is a PS2 ROM/kernel service (NOT a game IRX), so the runtime
                // supplies its fixed protocol version string -- same category as the
                // libsifrpc discriminator constants above, not invented game-IOP data.
                // recv buffer = req WORD[10] (0x00564B40), size = req WORD[11] (4);
                // both taken from the guest's own CALL packet, matching the decompile.
                const uint32_t loadfileObj =
                    s_loadfileClientObj.load(std::memory_order_relaxed);
                if (loadfileObj != 0u && req.words[7] == loadfileObj &&
                    req.words[kSifDiscWordIdx] == 0xFFu)
                {
                    const uint32_t recvAddr = req.words[10];
                    const uint32_t recvSize = req.words[11];
                    if (recvAddr != 0u && recvSize >= sizeof(uint32_t))
                    {
                        if (uint8_t *recvPtr = getMemPtr(rdram, recvAddr))
                        {
                            const uint32_t kLoadfileVersion = 0x30303033u; // "3000"
                            std::memcpy(recvPtr, &kLoadfileVersion, sizeof(kLoadfileVersion));

                            static std::atomic<uint32_t> s_sigLogs{0u};
                            if (s_sigLogs.fetch_add(1u, std::memory_order_relaxed) < 8u)
                            {
                                std::cerr << "[SifRpcReply:LOADFILE] func255 recv=0x"
                                          << std::hex << recvAddr
                                          << " <- 0x30303033 (\"3000\")" << std::dec
                                          << std::endl;
                            }
                        }
                    }
                }
            }
            else if (req.cid == kSifCmdRpcEnd)
            {
                // Outbound END re-send: mirror it back to _request_end unchanged so any
                // in-flight teardown/SignalSema still runs. Its register branch won't fire
                // (WORD[8] already 0x80000009 in the request), but the flag is set by the
                // BIND completion above, so this is harmless bookkeeping.
                pkt[2] = kSifCmdRpcEnd;
            }
            else
            {
                // Outbound BIND -> synthesize the IOP's register completion.
                pkt[2] = kSifCmdRpcEnd;          // dispatcher -> _request_end
                pkt[kSifDiscWordIdx] = kSifCmdRpcBind; // WORD[8] -> register branch
                pkt[9] = req.words[4];           // client_block[9] = sid (nonzero flag)
                // client_block[5] is the IOP server's receive buffer: rpc_call
                // (0x178BE8) uses it as the DEST of every send-payload DMA. On real
                // hardware the IOP returns its own IOP-side buffer here. Echoing the
                // request's WORD[5] (the EE packet-pool buffer 0x20561900) made the
                // guest DMA its payloads over its OWN SIF packet pool -- corrupting
                // pool link words (later packets carried string garbage in w[3]) and
                // eventually dispatching to the poisoned pointer 0x20561900. Use the
                // IOP-bound sentinel instead: the DMA path skips the EE-side copy for
                // non-copyable dests, and our RPC HLE reads payloads from src anyway.
                pkt[10] = 0xFFFFFFFFu;           // client_block[5] = IOP-bound sentinel
            }
            std::memcpy(q, pkt, sizeof(pkt));
            // Ensure the pending byte is set last (dispatcher gate).
            q[0] = static_cast<uint8_t>(pkt[0] & 0xFFu);

            PS2Runtime::RecompiledFunction dispatcher = runtime->lookupFunction(kSifDispatcherFn);
            if (!dispatcher)
            {
                static std::atomic<uint32_t> s_noDispatchLogs{0u};
                if (s_noDispatchLogs.fetch_add(1u, std::memory_order_relaxed) < 8u)
                {
                    std::cerr << "[SifRpcReply] dispatcher 0x" << std::hex
                              << kSifDispatcherFn << " not in function table" << std::dec
                              << std::endl;
                }
                return;
            }

            static std::atomic<uint32_t> s_replyLogs{0u};
            if (s_replyLogs.fetch_add(1u, std::memory_order_relaxed) < 24u)
            {
                std::cerr << "[SifRpcReply] deliver cid=0x" << std::hex << req.cid
                          << " -> run dispatcher 0x" << kSifDispatcherFn << std::dec
                          << std::endl;
            }

            // --- Phase 1 diagnostic (env-gated PS2_SIF_DIAG, instrumentation only) ---
            // Dumps the queue packet, the RPC_SERVER_DATA struct (0x5616D8), and the
            // head of both dispatch tables so the slot that decodes to 0x20561900 is
            // named. Also arms a derail detector after the dispatcher returns. No field
            // is written; boot behavior is unchanged when PS2_SIF_DIAG is unset.
            static const bool s_sifDiag = (std::getenv("PS2_SIF_DIAG") != nullptr);
            if (s_sifDiag)
            {
                static std::atomic<uint32_t> s_diagLogs{0u};
                if (s_diagLogs.fetch_add(1u, std::memory_order_relaxed) < 24u)
                {
                    auto rdW = [rdram](uint32_t addr) -> uint32_t {
                        const uint8_t *p = getConstMemPtr(rdram, addr);
                        uint32_t v = 0u;
                        if (p) { std::memcpy(&v, p, sizeof(v)); }
                        return v;
                    };
                    std::cerr << std::hex << "[SIF_DIAG] cid=0x" << req.cid << " pkt=";
                    for (int i = 0; i < 16; ++i) { std::cerr << "0x" << pkt[i] << ((i < 15) ? "," : ""); }
                    std::cerr << std::endl;
                    constexpr uint32_t kServerData = 0x5616D8u;
                    const uint32_t sysBase = rdW(kServerData + 0x0Cu);
                    const uint32_t sysCnt = rdW(kServerData + 0x10u);
                    const uint32_t usrBase = rdW(kServerData + 0x14u);
                    const uint32_t usrCnt = rdW(kServerData + 0x18u);
                    std::cerr << "[SIF_DIAG] rx=0x" << rdW(kServerData + 0x00u)
                              << " sysBase=0x" << sysBase << " sysCnt=0x" << sysCnt
                              << " usrBase=0x" << usrBase << " usrCnt=0x" << usrCnt << std::endl;
                    // The old 4-slot cap hid the tail of a 0x20-entry table, which is
                    // where the derail target has to live -- sys[0..3] are all sane.
                    // Walk the whole table, but print only slots that carry something:
                    // a non-zero fn, or anything already inside the packet-pool window
                    // that the derail detector below watches for.
                    auto dumpTable = [&](const char *name, uint32_t base, uint32_t cnt) {
                        const uint32_t n = (cnt > 64u) ? 64u : cnt; // sanity bound only
                        for (uint32_t s = 0u; s < n; ++s) {
                            const uint32_t e  = base + s * 12u;
                            const uint32_t fn = rdW(e);
                            const bool poisoned = (fn >= 0x20560000u && fn < 0x20570000u);
                            if (fn == 0u && !poisoned) { continue; }
                            std::cerr << "[SIF_DIAG] " << name << "[" << std::dec << s << std::hex
                                      << "] fn=0x" << fn << " arg=0x" << rdW(e + 4u)
                                      << " gp=0x" << rdW(e + 8u)
                                      << (poisoned ? "  <== POISONED" : "") << std::endl;
                        }
                    };
                    dumpTable("sys", sysBase, sysCnt);
                    dumpTable("usr", usrBase, usrCnt);
                    std::cerr << std::dec;
                }
            }

            // Run the guest dispatcher on the current context. It reads 0x561600 and routes
            // by cid. Re-entrancy is fine: if _request_bind re-sends 0x80000008 during this
            // call, that outbound descriptor is captured and delivered by the nested
            // sceSifSetDma (bounded by s_deliverDepth).
            //
            // The recompiled dispatcher is a resumable coroutine: its copy loop yields via
            // `if (runtime->shouldPreemptGuestExecution()) { return; }` with ctx->pc parked
            // mid-body and its 0xA0 stack frame still allocated. The real dispatch loop would
            // re-enter it through its entry switch; we are not the dispatch loop, so a single
            // plain call let that yield unwind out through handleSyscall with $sp permanently
            // shifted by -0xa0. Drive it in a bounded mini dispatch loop instead, and enter
            // with ctx->pc at the function start rather than the caller's syscall pc.
            const uint32_t savedPc = ctx ? ctx->pc : 0u;
            if (ctx)
            {
                ctx->pc = kSifDispatcherFn;
            }

            ++s_deliverDepth;
            uint32_t resumes = 0u;
            for (;;)
            {
                dispatcher(rdram, ctx, runtime);

                // Outside the function body => it ran to its `jr $ra` (or derailed). Done.
                if (!ctx || ctx->pc < kSifDispatcherFn || ctx->pc >= kSifDispatcherEnd)
                {
                    break;
                }

                if (++resumes >= kSifDispatcherMaxResume)
                {
                    static std::atomic<uint32_t> s_resumeLogs{0u};
                    if (s_resumeLogs.fetch_add(1u, std::memory_order_relaxed) < 8u)
                    {
                        std::cerr << "[SifRpcReply] dispatcher resume cap hit at pc=0x"
                                  << std::hex << ctx->pc << " (cid=0x" << req.cid << ")"
                                  << std::dec << std::endl;
                    }
                    break;
                }
            }
            --s_deliverDepth;

            // Phase 1 derail detector: if the dispatcher's jalr landed in the EE SIF
            // packet-pool staging window, lookupFunction failed and the runner's jalr
            // wrapper returned with ctx->pc parked at the derail target.
            if (s_sifDiag && ctx)
            {
                const uint32_t pc = ctx->pc;
                if (pc >= 0x20560000u && pc < 0x20570000u)
                {
                    std::cerr << "[SIF_DIAG:DERAIL] pc=0x" << std::hex << pc
                              << " cid=0x" << req.cid << std::dec << std::endl;
                }
            }

            // Delivery is transparent to the interrupted guest flow: hand the caller's pc
            // back so the syscall stub resumes at its own `jr $ra`.
            if (ctx)
            {
                ctx->pc = savedPc;
            }
        }
        // --- end SIF-RPC reply delivery ---

        uint32_t alignIopHeapSize(uint32_t size)
        {
            return (size + (kIopHeapAlign - 1u)) & ~(kIopHeapAlign - 1u);
        }

        uint32_t allocateSifHeapBlock(uint32_t requestSize)
        {
            const uint32_t alignedSize = alignIopHeapSize(requestSize);
            if (alignedSize == 0u)
            {
                return 0u;
            }

            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            uint32_t candidate = kIopHeapBase;
            for (const auto &[addr, size] : g_sifHeapAllocations)
            {
                if (candidate + alignedSize <= addr)
                {
                    break;
                }

                const uint32_t blockEnd = alignIopHeapSize(addr + size);
                if (blockEnd > candidate)
                {
                    candidate = blockEnd;
                }
            }

            if (candidate < kIopHeapBase || candidate + alignedSize > kIopHeapLimit)
            {
                return 0u;
            }

            g_sifHeapAllocations[candidate] = alignedSize;
            g_iopHeapNext = candidate + alignedSize;
            return candidate;
        }

        bool freeSifHeapBlock(uint32_t addr)
        {
            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            const auto it = g_sifHeapAllocations.find(addr);
            if (it == g_sifHeapAllocations.end())
            {
                return false;
            }

            g_sifHeapAllocations.erase(it);
            if (g_sifHeapAllocations.empty())
            {
                g_iopHeapNext = kIopHeapBase;
            }
            return true;
        }

        void resetSifHeapState()
        {
            std::lock_guard<std::mutex> lock(g_sifHeapMutex);
            g_sifHeapAllocations.clear();
            g_iopHeapNext = kIopHeapBase;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canCopyGuestByteRange(const uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }

            if (sizeBytes == 0u)
            {
                return true;
            }

            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t srcByteAddr = srcAddr + i;
                const uint32_t dstByteAddr = dstAddr + i;

                if (!isCopyableGuestAddress(srcByteAddr) || !isCopyableGuestAddress(dstByteAddr))
                {
                    return false;
                }

                const uint8_t *src = getConstMemPtr(rdram, srcByteAddr);
                const uint8_t *dst = getConstMemPtr(rdram, dstByteAddr);
                if (!src || !dst)
                {
                    return false;
                }
            }

            return true;
        }

        bool copyGuestByteRange(uint8_t *rdram, uint32_t dstAddr, uint32_t srcAddr, uint32_t sizeBytes)
        {
            if (!canCopyGuestByteRange(rdram, dstAddr, srcAddr, sizeBytes))
            {
                return false;
            }

            if (sizeBytes == 0u)
            {
                return true;
            }

            const uint64_t srcBegin = srcAddr;
            const uint64_t srcEnd = srcBegin + static_cast<uint64_t>(sizeBytes);
            const uint64_t dstBegin = dstAddr;
            const bool copyBackward = (dstBegin > srcBegin) && (dstBegin < srcEnd);

            if (copyBackward)
            {
                for (uint32_t i = sizeBytes; i > 0u; --i)
                {
                    const uint32_t index = i - 1u;
                    const uint8_t *src = getConstMemPtr(rdram, srcAddr + index);
                    uint8_t *dst = getMemPtr(rdram, dstAddr + index);
                    if (!src || !dst)
                    {
                        return false;
                    }
                    *dst = *src;
                }
                return true;
            }

            for (uint32_t i = 0; i < sizeBytes; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                uint8_t *dst = getMemPtr(rdram, dstAddr + i);
                if (!src || !dst)
                {
                    return false;
                }
                *dst = *src;
            }
            return true;
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        resetSifHeapState();
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = handler;
        // DIAGNOSTIC (bind-reply investigation): log every system/user SIF command
        // handler the game registers. These are the cids the IOP must eventually
        // deliver to wake the sub_178068 dispatcher stuck at WaitSema@0x174ce0.
        // Bounded, not gated on AGRESSIVE_LOGS. Remove once the delivery pipe lands.
        {
            static std::atomic<uint32_t> s_addCmdLogs{0u};
            if (s_addCmdLogs.fetch_add(1u, std::memory_order_relaxed) < 64u)
            {
                std::cerr << "[SifAddCmdHandler] cid=0x" << std::hex << cid
                          << " handler=0x" << handler
                          << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
            }
        }
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, allocateSifHeapBlock(reqSize));
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, allocateSifHeapBlock(size));
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        (void)getRegU32(ctx, 4); // trid

        // Transfers are applied immediately by sceSifSetDma in this runtime.
        setReturnS32(ctx, -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, freeSifHeapBlock(addr) ? 0 : -1);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, freeSifHeapBlock(addr) ? 0 : -1);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;

        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        ps2_syscalls::prepareSoundDriverStatusTransfer(rdram, srcAddr, size);

        if (!copyGuestByteRange(rdram, dstAddr, srcAddr, size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        ps2_syscalls::finalizeSoundDriverStatusTransfer(rdram, srcAddr, dstAddr, size);

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        resetSifHeapState();
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The real sceSifRpcLoop is `while (1) { SleepThread(); serve; }` -
        // the RPC server thread spends its life parked in SleepThread, woken
        // only by SIF RPC deliveries. All SIF RPC traffic here is HLE'd
        // host-side, so no WakeupThread ever targets this thread: park it via
        // the real SleepThread syscall. If this ever returns without blocking,
        // ctx->pc stays at the loop entry, so the dispatch loop re-enters this
        // stub forever and the RPC server fiber monopolizes the N=1 guest
        // executor, starving every other guest thread. Any
        // title that starts a SIF RPC server thread (pad, memcard, audio,
        // filesystem) and then drives its main thread would hang forever.
        // ctx->pc is intentionally left at the loop entry: a stray
        // wakeup simply re-enters and sleeps again, exactly like the real
        // loop.
        ps2_syscalls::SleepThread(rdram, ctx, runtime);
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;

        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            }
        });

        if (!dmatAddr || count == 0u || count > 32u)
        {
            static std::atomic<uint32_t> s_argGuardLogs{0u};
            if (s_argGuardLogs.fetch_add(1u, std::memory_order_relaxed) < 16u)
            {
                std::cerr << "[sceSifSetDma:GUARD] reject dmat=0x" << std::hex << dmatAddr
                          << " count=" << std::dec << count
                          << " ra=0x" << std::hex << getRegU32(ctx, 31)
                          << std::dec << " -> 0" << std::endl;
            }
            setReturnS32(ctx, 0);
            return;
        }

        // DIAGNOSTIC (bind-reply investigation): unconditional bounded dump of every
        // transfer descriptor so we can see whether the SIF-RPC bind request travels
        // through this DMA path during the WaitSema@0x174ce0 spin, and read the RPC
        // control-packet contents (to decode the game's registered bind sema id).
        // Remove once the bind-reply hook is placed. Not gated on AGRESSIVE_LOGS (that
        // would need a header edit + full rebuild).
        {
            static std::atomic<uint32_t> s_dtxDiagLogs{0u};
            if (s_dtxDiagLogs.fetch_add(1u, std::memory_order_relaxed) < 64u)
            {
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t eAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
                    const uint8_t *e = getConstMemPtr(rdram, eAddr);
                    Ps2SifDmaTransfer x{};
                    if (e) std::memcpy(&x, e, sizeof(x));
                    const uint32_t sz = static_cast<uint32_t>(x.size);
                    std::cerr << "[sceSifSetDma:DTX] i=" << i
                              << " src=0x" << std::hex << x.src
                              << " dest=0x" << x.dest
                              << " size=0x" << sz
                              << " attr=0x" << static_cast<uint32_t>(x.attr);
                    // First 8 words of the EE-side src payload (the RPC control packet).
                    const uint8_t *sp = getConstMemPtr(rdram, x.src);
                    if (sp && sz >= 4u)
                    {
                        std::cerr << " pkt[";
                        const uint32_t words = (sz < 32u ? sz : 32u) / 4u;
                        for (uint32_t w = 0; w < words; ++w)
                        {
                            uint32_t v{};
                            std::memcpy(&v, sp + w * 4u, 4u);
                            std::cerr << (w ? " " : "") << "0x" << v;
                        }
                        std::cerr << "]";
                    }
                    std::cerr << " ra=0x" << getRegU32(ctx, 31) << std::dec << std::endl;
                }
            }
        }

        // Record this list's entries before processing any of them, so that when
        // the RPC control packet is delivered (which happens from inside the loop
        // below) the CALL handler can still see the payload entry that preceded
        // it. Unbounded -- the DTX dump above is diagnostic-only and caps at 64.
        g_sifListCount = 0u;
        for (uint32_t i = 0; i < count && i < 32u; ++i)
        {
            const uint32_t eAddr =
                dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            if (const uint8_t *e = getConstMemPtr(rdram, eAddr))
            {
                Ps2SifDmaTransfer x{};
                std::memcpy(&x, e, sizeof(x));
                g_sifListEntries[g_sifListCount].src  = x.src;
                g_sifListEntries[g_sifListCount].size = static_cast<uint32_t>(x.size);
                ++g_sifListCount;
            }
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        Ps2SifDmaTransfer failXfer{};
        const char *failWhy = "none";
        SifRpcReplyPacket rpcReply{}; // set if an outbound RPC system command is seen this call
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                failWhy = "entryPtr";
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                failXfer = xfer;
                failWhy = "sizeTooBig";
                break;
            }

            // SIF DMA to the IOP: `dest` is an IOP-side address (or a sentinel
            // like 0xffffffff), NOT EE RAM. Only require the EE-side `src` to be
            // readable. If `dest` is a copyable EE address we perform the copy
            // (EE->EE / loopback); otherwise the transfer is IOP-bound and the
            // EE-side copy is skipped, but the transfer is still accepted so the
            // guest receives a valid nonzero id and advances. Rejecting these
            // caused the SIF-RPC bind spin loop at 0x100008.
            if (!isCopyableGuestAddress(xfer.src) || !getConstMemPtr(rdram, xfer.src))
            {
                ok = false;
                failXfer = xfer;
                failWhy = "srcUnreadable";
                break;
            }

            // Detect an outbound SIF-RPC system command (cid word[2] == 0x80000008 END or
            // 0x80000009 BIND) so we can loop the game's own packet back into its RX queue
            // as the reply the real IOP would have returned. Capture the whole 64-byte
            // packet verbatim -- no invented IOP state.
            if (!rpcReply.valid && sizeBytes >= 0x20u)
            {
                if (const uint8_t *sp = getConstMemPtr(rdram, xfer.src))
                {
                    uint32_t w[16]{};
                    const uint32_t copyBytes =
                        (sizeBytes < sizeof(w)) ? sizeBytes : static_cast<uint32_t>(sizeof(w));
                    std::memcpy(w, sp, copyBytes);
                    if (w[2] == kSifCmdRpcBind || w[2] == kSifCmdRpcEnd ||
                        w[2] == kSifCmdRpcCall)
                    {
                        std::memcpy(rpcReply.words, w, sizeof(w));
                        rpcReply.cid = w[2];
                        rpcReply.valid = true;

                        // DIAGNOSTIC (v5): dump the full 16-word outbound RPC system
                        // packet verbatim so we can read the real reply field layout.
                        // Two goals here:
                        //   * find the sid-0x80000006 (LOADFILE) BIND: a BIND packet
                        //     carries the target server sid in WORD[8].
                        //   * find the func-255 CALL that follows it and locate its recv
                        //     buffer word. d4a0 passes recv=&dword_564B40 (0x00564B40)
                        //     with the client data at dword_564D40 (0x00564D40), so any
                        //     packet word equal to one of those pinpoints the layout.
                        // Tag such packets [SifRpcPkt:SIG] and never rate-limit them; the
                        // generic dump stays bounded so the log doesn't flood.
                        bool sigRelated = (w[8] == 0x80000006u);
                        for (uint32_t k = 0; k < 16u && !sigRelated; ++k)
                        {
                            if (w[k] == 0x00564B40u || w[k] == 0x00564D40u)
                            {
                                sigRelated = true;
                            }
                        }
                        // Stage 1: never rate-limit ARKD DVD-read packets when tracing.
                        if (arkdTraceEnabled() && isArkdPacket(w))
                        {
                            sigRelated = true;
                        }
                        static std::atomic<uint32_t> s_pktLogs{0u};
                        if (sigRelated || s_pktLogs.fetch_add(1u, std::memory_order_relaxed) < 8u)
                        {
                            std::cerr << (sigRelated ? "[SifRpcPkt:SIG] src=0x" : "[SifRpcPkt] src=0x")
                                      << std::hex << xfer.src
                                      << " cid=0x" << w[2] << std::dec;
                            for (uint32_t k = 0; k < 16u; ++k)
                            {
                                std::cerr << " w[" << k << "]=0x" << std::hex << w[k] << std::dec;
                            }
                            std::cerr << std::endl;
                        }
                    }
                }
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                const uint32_t xferSize = static_cast<uint32_t>(xfer.size);

                // Only copy when the destination is a real EE address (EE->EE
                // loopback). IOP-bound transfers (non-copyable dest, e.g.
                // 0xffffffff) skip the EE-side copy — the SIF/RPC layer delivers
                // the packet to the IOP — but are still treated as delivered.
                const bool destIsEeRam =
                    isCopyableGuestAddress(xfer.dest) &&
                    canCopyGuestByteRange(rdram, xfer.dest, xfer.src, xferSize);

                if (destIsEeRam)
                {
                    if (!copyGuestByteRange(rdram, xfer.dest, xfer.src, xferSize))
                    {
                        ok = false;
                        break;
                    }
                }

                ps2_syscalls::noteDtxSifDmaTransfer(
                    rdram,
                    xfer.src,
                    xfer.dest,
                    static_cast<uint32_t>(xfer.size));
            }
        }

        if (!ok)
        {
            static std::atomic<uint32_t> s_failLogs{0u};
            if (s_failLogs.fetch_add(1u, std::memory_order_relaxed) < 16u)
            {
                std::cerr << "[sceSifSetDma:FAIL] why=" << failWhy
                          << " dmat=0x" << std::hex << dmatAddr
                          << " count=" << std::dec << count
                          << " src=0x" << std::hex << failXfer.src
                          << " dest=0x" << failXfer.dest
                          << " size=0x" << static_cast<uint32_t>(failXfer.size)
                          << " attr=0x" << static_cast<uint32_t>(failXfer.attr)
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << " -> 0" << std::endl;
            }
            setReturnS32(ctx, 0);
            return;
        }

        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        {
            static std::atomic<uint32_t> s_okLogs{0u};
            if (s_okLogs.fetch_add(1u, std::memory_order_relaxed) < 16u)
            {
                std::cerr << "[sceSifSetDma:OK] dmat=0x" << std::hex << dmatAddr
                          << " count=" << std::dec << count
                          << " pending=" << pendingCount
                          << " ra=0x" << std::hex << getRegU32(ctx, 31)
                          << std::dec << " -> nonzero id" << std::endl;
            }
        }
        const int32_t dmaId = static_cast<int32_t>(allocateSifDmaTransferId());
        setReturnS32(ctx, dmaId);

        // If this call carried an outbound SIF-RPC system command, loop the reply back now
        // so the chain advances: 0x80000009 (BIND) -> _request_bind re-sends 0x80000008,
        // whose own sceSifSetDma captures + delivers it (re-entrant) -> _request_end ->
        // SignalSema wakes WaitSema@0x174ce0. The dispatcher runs guest callbacks inline on
        // this ctx (may clobber $v0/args), so re-establish the DMA id return afterwards.
        if (rpcReply.valid)
        {
            deliverSifRpcReply(rdram, ctx, runtime, rpcReply);
            setReturnS32(ctx, dmaId);
        }
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }
}
