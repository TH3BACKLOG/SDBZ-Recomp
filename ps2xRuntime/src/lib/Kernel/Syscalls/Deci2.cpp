#include "System.h"
#include "Common.h"
#include "ps2_runtime.h"

namespace
{
    struct Deci2Session
    {
        uint16_t protocol = 0;
        uint32_t opt = 0;
        uint32_t handler = 0;
        uint32_t userArea = 0;
        bool locked = false;
    };

    std::mutex g_deci2Mutex;
    std::unordered_map<int32_t, Deci2Session> g_deci2Sessions;
    int32_t g_nextDeci2Socket = 1;
    std::atomic<uint32_t> g_deci2LogCount{0};

    static bool readDeci2U32(uint8_t *rdram, uint32_t guestAddr, uint32_t &value)
    {
        value = 0;

        const uint8_t *ptr = getConstMemPtr(rdram, guestAddr);
        if (!ptr)
        {
            return false;
        }

        std::memcpy(&value, ptr, sizeof(value));
        return true;
    }

    static void readDeci2Args(uint8_t *rdram, uint32_t argsAddr, uint32_t args[4])
    {
        for (uint32_t argIndex = 0; argIndex < 4; argIndex++)
        {
            uint32_t value = 0;
            readDeci2U32(rdram, argsAddr + (argIndex * sizeof(uint32_t)), value);
            args[argIndex] = value;
        }
    }

    static std::string sanitizeDeci2Text(std::string text)
    {
        for (char &ch : text)
        {
            const unsigned char value = static_cast<unsigned char>(ch);
            if (ch == '\r')
            {
                ch = '\n';
                continue;
            }

            if (ch != '\n' && ch != '\t' && !std::isprint(value))
            {
                ch = '.';
            }
        }

        return text;
    }

    static void logDeci2Text(const char *prefix, const std::string &text)
    {
        constexpr uint32_t kMaxDeci2TextLogs = 256u;
        const uint32_t logIndex = g_deci2LogCount.fetch_add(1u, std::memory_order_relaxed);
        if (logIndex >= kMaxDeci2TextLogs)
        {
            return;
        }

        const std::string safeText = sanitizeDeci2Text(text);
        std::cerr << prefix << safeText;
        if (safeText.empty() || safeText.back() != '\n')
        {
            std::cerr << std::endl;
        }
    }

    static int32_t allocateDeci2Socket(uint16_t protocol, uint32_t opt, uint32_t handler, uint32_t userArea)
    {
        std::lock_guard<std::mutex> lock(g_deci2Mutex);

        if (g_nextDeci2Socket <= 0)
        {
            g_nextDeci2Socket = 1;
        }

        const int32_t socket = g_nextDeci2Socket;
        Deci2Session session;
        session.protocol = protocol;
        session.opt = opt;
        session.handler = handler;
        session.userArea = userArea;
        session.locked = false;

        g_deci2Sessions[socket] = session;
        return socket;
    }

    static bool updateDeci2LockState(int32_t socket, bool locked)
    {
        std::lock_guard<std::mutex> lock(g_deci2Mutex);

        auto sessionIt = g_deci2Sessions.find(socket);
        if (sessionIt == g_deci2Sessions.end())
        {
            return false;
        }

        sessionIt->second.locked = locked;
        return true;
    }

    static bool closeDeci2Socket(int32_t socket)
    {
        std::lock_guard<std::mutex> lock(g_deci2Mutex);
        return g_deci2Sessions.erase(socket) != 0;
    }
}

namespace ps2_syscalls
{
    void Deci2Call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int32_t code = static_cast<int32_t>(getRegU32(ctx, 4));
        const uint32_t argsAddr = getRegU32(ctx, 5);

        uint32_t args[4] = {};
        readDeci2Args(rdram, argsAddr, args);

        switch (code)
        {
        case 1: // sceDeci2Open(protocol, opt, handler)
        {
            const uint16_t protocol = static_cast<uint16_t>(args[0] & 0xFFFFu);
            const uint32_t opt = args[1];
            const uint32_t handler = args[2];
            const uint32_t userArea = args[3];

            const int32_t socket = allocateDeci2Socket(protocol, opt, handler, userArea);
            setReturnS32(ctx, socket);
            return;
        }
        case 2: // sceDeci2Close(socket)
        {
            const int32_t socket = static_cast<int32_t>(args[0]);
            setReturnS32(ctx, closeDeci2Socket(socket) ? KE_OK : KE_ERROR);
            return;
        }
        // WE dont need to do thouses
        case 3:  // sceDeci2ReqSend(socket, dest)
        case -7: // sceDeci2ExReqSend(socket, dest)
        {
            // Diagnostic: this is the boot TTY flush path. Log the handle the
            // game passes and the packet text sitting in the Deci2 TTY buffer
            // (0x2056130C per IDA putbuf @0x1763F8) so we can see WHAT the game
            // is trying to print and confirm the loop is the printf flush.
            {
                static std::atomic<uint32_t> s_reqSendLogs{0u};
                if (s_reqSendLogs.fetch_add(1u, std::memory_order_relaxed) < 64u)
                {
                    constexpr uint32_t kDeci2TtyBuffer = 0x2056130Cu;
                    std::string text;
                    text.reserve(128u);
                    for (uint32_t byteIndex = 0; byteIndex < 128u; byteIndex++)
                    {
                        const uint8_t ch = rdram[(kDeci2TtyBuffer + byteIndex) & PS2_RAM_MASK];
                        if (ch == 0u)
                        {
                            break;
                        }
                        text.push_back(static_cast<char>(ch));
                    }
                    std::cerr << "[Deci2Call:reqsend] code=" << code
                              << " a0=0x" << std::hex << args[0]
                              << " a1=0x" << args[1] << std::dec << std::endl;
                    logDeci2Text("[Deci2Call:reqsend:tty] ", text);

                    // When the TTY line is the loadmodule-failure message, dump the
                    // four EE words that drive the pre-RPC signature gate in
                    // noop_sub_dc78 (0x17DC78) -> wrap_mem_compare_n_c_0 (0x17D5A0).
                    // The gate REJECTS (returns -65540) iff:
                    //   [0x564D68] != [0x461B5C] && [0x564D68] != *[0x461C34] &&
                    //   [0x461B5C] != *[0x461C34]. dword_564D68 is the loadfile
                    // module signature fetched via sid 0x80000006 rpc func 0xFF in
                    // noop_sub_d4a0; it is BSS-zero here because that RPC never ran
                    // (d4a0 short-circuits on dword_461C30 >= 0). Confirm empirically.
                    if (text.rfind("Can't load module", 0) == 0)
                    {
                        static std::atomic<uint32_t> s_gateLogs{0u};
                        if (s_gateLogs.fetch_add(1u, std::memory_order_relaxed) < 4u)
                        {
                            uint32_t initFlag = 0, sigA = 0, ptrC34 = 0, sigBVal = 0, fetched = 0;
                            readDeci2U32(rdram, 0x00461C30u, initFlag); // d4a0 init guard
                            readDeci2U32(rdram, 0x00461B5Cu, sigA);     // expected sig A
                            readDeci2U32(rdram, 0x00461C34u, ptrC34);   // off_461C34 (pointer)
                            readDeci2U32(rdram, ptrC34, sigBVal);       // *off_461C34 = expected sig B
                            readDeci2U32(rdram, 0x00564D68u, fetched);  // fetched loadfile signature
                            std::cerr << std::hex
                                      << "[loadgate] initFlag@461C30=0x" << initFlag
                                      << " sigA@461B5C=0x" << sigA
                                      << " ptr@461C34=0x" << ptrC34
                                      << " sigB@*461C34=0x" << sigBVal
                                      << " fetched@564D68=0x" << fetched
                                      << std::dec << std::endl;
                        }
                    }
                }
            }

            setReturnS32(ctx, KE_OK);
            return;
        }
        case 4: // sceDeci2Poll(socket)
        {
            // No host DECI2 channel is attached. On real hardware the DECI2
            // driver's completion ISR clears the per-socket "request pending"
            // word once the async send finishes; here that ISR never runs, so
            // the crt0 poll loop (runner fn_1763F8 @ 0x176518->0x176528) spins
            // forever waiting on that word to return to 0.
            //
            // The driver state block is at a fixed BSS address: $s5 = lui 0x56 =>
            // 0x00560000, $s0 = $s5 + 0x12D0 = 0x005612D0, and the pending word
            // the loop reloads is 0xC($s0) = 0x005612DC. Mark the request complete
            // by clearing it so the poll loop can exit.
            constexpr uint32_t kDeci2StateBlock = 0x005612D0u;
            constexpr uint32_t kDeci2PendingWord = kDeci2StateBlock + 0x0Cu;

            // Diagnostic: log the state block once so we can confirm the offset
            // empirically on the same run that applies the fix.
            {
                static std::atomic<uint32_t> s_pollFlagLogs{0u};
                if (s_pollFlagLogs.fetch_add(1u, std::memory_order_relaxed) < 64u)
                {
                    uint32_t w0 = 0, w4 = 0, w8 = 0, wc = 0, w10 = 0;
                    readDeci2U32(rdram, kDeci2StateBlock + 0x00u, w0);
                    readDeci2U32(rdram, kDeci2StateBlock + 0x04u, w4);
                    readDeci2U32(rdram, kDeci2StateBlock + 0x08u, w8);
                    readDeci2U32(rdram, kDeci2StateBlock + 0x0Cu, wc);
                    readDeci2U32(rdram, kDeci2StateBlock + 0x10u, w10);
                    std::cerr << "[Deci2Call:poll] block@0x" << std::hex << kDeci2StateBlock
                              << " +0=0x" << w0 << " +4=0x" << w4 << " +8=0x" << w8
                              << " +C=0x" << wc << " +10=0x" << w10 << std::dec << std::endl;

                    // Also dump the handle the game actually passes (args[0]) so we
                    // can tell whether the drain word lives at 0xC(handle) (~0x46000C
                    // per 07-17b) rather than the fixed BSS latch above.
                    const uint32_t handle = args[0];
                    uint32_t h0 = 0, h4 = 0, h8 = 0, hc = 0, h10 = 0;
                    readDeci2U32(rdram, handle + 0x00u, h0);
                    readDeci2U32(rdram, handle + 0x04u, h4);
                    readDeci2U32(rdram, handle + 0x08u, h8);
                    readDeci2U32(rdram, handle + 0x0Cu, hc);
                    readDeci2U32(rdram, handle + 0x10u, h10);
                    std::cerr << "[Deci2Call:poll] handle@0x" << std::hex << handle
                              << " +0=0x" << h0 << " +4=0x" << h4 << " +8=0x" << h8
                              << " +C=0x" << hc << " +10=0x" << h10 << std::dec << std::endl;
                }
            }

            if (uint8_t *pending = getMemPtr(rdram, kDeci2PendingWord))
            {
                const uint32_t zero = 0u;
                std::memcpy(pending, &zero, sizeof(zero));
            }

            setReturnS32(ctx, KE_OK);
            return;
        }
        case -5: // sceDeci2ExRecv(socket, buf, len)
        {
            // No host debugger channel is attached, so report "no bytes available".
            setReturnS32(ctx, 0);
            return;
        }
        case -6: // sceDeci2ExSend(socket, buf, len)
        {
            const uint32_t bufferAddr = args[1];
            const uint32_t length = args[2] & 0xFFFFu;

            if (bufferAddr != 0u && length != 0u)
            {
                const uint32_t clampedLength = std::min<uint32_t>(length, 512u);
                std::string text;
                text.reserve(clampedLength);
                for (uint32_t byteIndex = 0; byteIndex < clampedLength; byteIndex++)
                {
                    text.push_back(static_cast<char>(rdram[(bufferAddr + byteIndex) & PS2_RAM_MASK]));
                }
                logDeci2Text("[Deci2Call:send] ", text);
            }

            // Treat send as successful and return the amount accepted.
            setReturnS32(ctx, static_cast<int32_t>(length));
            return;
        }
        case -8: // sceDeci2ExLock(socket)
        {
            const int32_t socket = static_cast<int32_t>(args[0]);
            setReturnS32(ctx, updateDeci2LockState(socket, true) ? KE_OK : KE_ERROR);
            return;
        }
        case -9: // sceDeci2ExUnLock(socket)
        {
            const int32_t socket = static_cast<int32_t>(args[0]);
            setReturnS32(ctx, updateDeci2LockState(socket, false) ? KE_OK : KE_ERROR);
            return;
        }
        case 16: // kputs(char *s)
        {
            constexpr size_t kMaxDeci2KputsBytes = 4096u;
            const uint32_t textAddr = args[0];
            const std::string text = readGuestCStringBounded(rdram, textAddr, kMaxDeci2KputsBytes);

            logDeci2Text("[Deci2Call:kputs] ", text);
            setReturnS32(ctx, static_cast<int32_t>(text.size()));
            return;
        }
        default:
        {
            static std::atomic<uint32_t> s_unknownDeci2Logs{0u};
            constexpr uint32_t kMaxUnknownDeci2Logs = 64u;
            const uint32_t logIndex = s_unknownDeci2Logs.fetch_add(1u, std::memory_order_relaxed);
            if (logIndex < kMaxUnknownDeci2Logs)
            {
                std::cerr << "[Deci2Call:unknown]"
                          << " code=" << code
                          << " args=0x" << std::hex << argsAddr
                          << " a0=0x" << args[0]
                          << " a1=0x" << args[1]
                          << " a2=0x" << args[2]
                          << " a3=0x" << args[3]
                          << " pc=0x" << ctx->pc
                          << std::dec << std::endl;
            }

            setReturnS32(ctx, KE_OK);
            return;
        }
        }
    }
}
