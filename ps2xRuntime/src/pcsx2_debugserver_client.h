#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Minimal native client for the custom PCSX2 DebugServer TCP plugin.
// Protocol: newline-delimited JSON over TCP, port 21512 by default.
// Vendored from ps2xStudio's PCSX2DebugServerClient (same protocol already
// proven working via the mcp__pcsx2__* tools) so the standalone debugger GUI
// can read EE registers without depending on PCSX2's internal cpuRegs struct
// layout — that struct changes across PCSX2 versions, this plugin doesn't.

class PCSX2DebugServerClient {
public:
    PCSX2DebugServerClient() = default;
    ~PCSX2DebugServerClient();

    // Which PS2 CPU a command targets. The DebugServer plugin's "cpu" JSON
    // field selects the register/breakpoint/memory bank; default EE keeps all
    // existing callers unchanged.
    enum class Cpu { EE, IOP };

    bool connect(const std::string& host = "127.0.0.1", uint16_t port = 21512);
    void disconnect();
    bool isConnected() const { return m_socket != static_cast<uintptr_t>(-1); }

    std::string sendCommand(const std::string& cmd, Cpu cpu = Cpu::EE);
    std::string sendCommand(const std::string& cmd, int timeoutMs, Cpu cpu = Cpu::EE);

    // Distinguishes "server replied ok:false" from "we never got a reply in
    // time" — both used to collapse into a single "Step failed" log line,
    // which made the recvLine()/server 5000ms-wait timeout mismatch
    // impossible to diagnose from the log alone.
    enum class LastResult { Ok, TimedOut, Error };
    LastResult lastResult() const { return m_lastResult; }

    // Raw reply from the most recent readRegisters(), for diagnostics when the
    // parse fails (the reply shape differs from what we anchor on). Empty until
    // readRegisters runs. Kept small — only readRegisters populates it.
    const std::string& lastRawReply() const { return m_lastRawReply; }

    bool pause();
    bool resume();
    bool step();
    bool stepOver();

    struct RawRegisters {
        uint32_t pc = 0;
        uint64_t hi = 0;
        uint64_t lo = 0;
        uint64_t gpr[32] = {};
        bool     valid = false;
    };
    bool readRegisters(RawRegisters& out, Cpu cpu = Cpu::EE);

    struct Status {
        uint32_t pc = 0;
        bool     paused = false;
    };
    // {"cmd":"status","cpu":"ee"} -> {"ok":true,"data":{"pc":..,"paused":..}}
    bool getStatus(Status& out);

    // Real hardware breakpoints/watchpoints, executed by PCSX2 itself (not
    // polled from outside) — see set_breakpoint/set_memcheck in PCSX2's
    // DebugServer plugin. condition is an optional MIPS expression string
    // (e.g. "v0 == 0x42"); pass "" for none.
    bool setBreakpoint(uint32_t address, const std::string& condition = "", Cpu cpu = Cpu::EE);
    bool removeBreakpoint(uint32_t address, Cpu cpu = Cpu::EE);
    bool clearAllBreakpoints(Cpu cpu = Cpu::EE);

    // type: "write" | "read" | "access" | "onchange"
    bool setWatchpoint(uint32_t address, uint32_t size, const std::string& type = "write", Cpu cpu = Cpu::EE);
    bool removeWatchpoint(uint32_t address, uint32_t size, Cpu cpu = Cpu::EE);

    // Dump `len` bytes of guest RAM into `out`. Uses the plugin's read_memory
    // command (support UNCONFIRMED — see .cpp). Returns false cleanly on any
    // missing/short/ok:false reply. For EE, ReadProcessMemory in the GUI is
    // preferred and faster; this is a fallback / the IOP path.
    bool readMemory(uint32_t address, uint8_t* out, uint32_t len, Cpu cpu = Cpu::EE);

private:
    static const char* cpuStr(Cpu cpu) { return cpu == Cpu::IOP ? "iop" : "ee"; }

    uintptr_t m_socket = static_cast<uintptr_t>(-1); // SOCKET, stored as uintptr_t to avoid winsock.h here
    bool m_wsaInitialized = false;
    LastResult m_lastResult = LastResult::Ok;
    std::string m_lastRawReply;

    std::string recvLine(int timeoutMs = 2000);

    static bool jsonOk(const std::string& json);
    static void logParseFailure(const std::string& json);
    static bool extractHex(const std::string& json, const std::string& key, uint64_t& out);
    static std::vector<uint64_t> extractAllValues(const std::string& json,
                                                  size_t begin = 0,
                                                  size_t end = std::string::npos);
};
