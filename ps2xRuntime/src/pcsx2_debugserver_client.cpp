#include "pcsx2_debugserver_client.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdlib>
#include <cstdio>
#include <chrono>

// Throttled diagnostic: on a readRegisters parse failure, dump the first ~120
// bytes of the raw reply at most once/second. Makes a future reply-shape
// mismatch visible in one line instead of inferred from a reconnect storm.
void PCSX2DebugServerClient::logParseFailure(const std::string& json) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last{};
    auto now = clock::now();
    if (now - last < std::chrono::seconds(1)) return;
    last = now;
    std::string head = json.substr(0, 120);
    fprintf(stderr, "[DebugServer] readRegisters parse failed; reply head: %s\n",
            head.c_str());
}

PCSX2DebugServerClient::~PCSX2DebugServerClient() {
    disconnect();
}

bool PCSX2DebugServerClient::connect(const std::string& host, uint16_t port) {
    disconnect();

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    m_wsaInitialized = true;

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) { WSACleanup(); m_wsaInitialized = false; return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        closesocket(sock); WSACleanup(); m_wsaInitialized = false; return false;
    }

    // Short connect timeout: local-loopback debug socket — fail fast if absent.
    u_long nonBlocking = 1;
    ioctlsocket(sock, FIONBIO, &nonBlocking);
    int rc = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sock); WSACleanup(); m_wsaInitialized = false; return false;
    }
    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);
    timeval tv{3, 0};
    if (select(0, nullptr, &writeSet, nullptr, &tv) <= 0) {
        closesocket(sock); WSACleanup(); m_wsaInitialized = false; return false;
    }
    u_long blocking = 0;
    ioctlsocket(sock, FIONBIO, &blocking);

    m_socket = static_cast<uintptr_t>(sock);
    return true;
}

void PCSX2DebugServerClient::disconnect() {
    if (m_socket != static_cast<uintptr_t>(-1)) {
        closesocket(static_cast<SOCKET>(m_socket));
        m_socket = static_cast<uintptr_t>(-1);
    }
    if (m_wsaInitialized) {
        WSACleanup();
        m_wsaInitialized = false;
    }
}

std::string PCSX2DebugServerClient::recvLine(int timeoutMs) {
    if (!isConnected()) return "";
    SOCKET sock = static_cast<SOCKET>(m_socket);

    std::string line;
    char buf[512];
    for (;;) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock, &readSet);
        timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) return "";

        int n = recv(sock, buf, sizeof(buf), 0);
        if (n <= 0) return "";
        line.append(buf, n);

        auto nl = line.find('\n');
        if (nl != std::string::npos) return line.substr(0, nl);
    }
}

std::string PCSX2DebugServerClient::sendCommand(const std::string& cmd, Cpu cpu) {
    return sendCommand(cmd, 2000, cpu);
}

std::string PCSX2DebugServerClient::sendCommand(const std::string& cmd, int timeoutMs, Cpu cpu) {
    if (!isConnected()) { m_lastResult = LastResult::Error; return ""; }
    std::string json = "{\"cmd\":\"" + cmd + "\",\"cpu\":\"" + cpuStr(cpu) + "\"}\n";
    int sent = send(static_cast<SOCKET>(m_socket), json.c_str(), static_cast<int>(json.size()), 0);
    if (sent != static_cast<int>(json.size())) {
        m_lastResult = LastResult::Error;
        disconnect();
        return "";
    }
    std::string reply = recvLine(timeoutMs);
    if (reply.empty()) {
        // Either a timeout (server still working, e.g. step()'s up-to-5000ms
        // wait) or a hard socket error. Either way the reply — if it arrives
        // late — must not be left sitting on the socket to desync the next
        // unrelated read (e.g. SyncFromPCSX2's status poll), so drop the
        // connection now rather than reuse a possibly-corrupted socket.
        m_lastResult = LastResult::TimedOut;
        disconnect();
        return "";
    }
    m_lastResult = LastResult::Ok;
    return reply;
}

bool PCSX2DebugServerClient::jsonOk(const std::string& json) {
    return json.find("\"ok\":true") != std::string::npos;
}

bool PCSX2DebugServerClient::extractHex(const std::string& json, const std::string& key, uint64_t& out) {
    // The real DebugServer sends BARE hex with no "0x" prefix (e.g.
    // "pc":"00178938"), and hi/lo are full 128-bit (32 hex chars) — the earlier
    // "\":\"0x" anchor matched nothing, so pc extraction failed and readRegisters
    // returned false every frame. Accept both prefixed and bare hex, and keep the
    // low 64 bits of anything wider.
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return false;
    std::string hex = json.substr(pos, end - pos);
    if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex.erase(0, 2);
    if (hex.empty()) return false;
    if (hex.size() > 16) hex = hex.substr(hex.size() - 16); // low 64 bits
    out = std::strtoull(hex.c_str(), nullptr, 16);
    return true;
}

std::vector<uint64_t> PCSX2DebugServerClient::extractAllValues(const std::string& json,
                                                              size_t begin, size_t end) {
    std::vector<uint64_t> values;
    // Values are bare 128-bit hex (32 chars, no "0x"), e.g.
    // "value":"00000000000000000000000000178938". The old "\"value\":\"0x"
    // anchor never matched, and parsing 32 chars as one u64 would overflow —
    // keep the low 64 bits (the "display" field, which DOES carry "0x", is left
    // alone; we anchor on "value" only).
    const std::string needle = "\"value\":\"";
    if (end == std::string::npos) end = json.size();
    size_t pos = begin;
    while (pos < end && (pos = json.find(needle, pos)) != std::string::npos && pos < end) {
        pos += needle.size();
        auto q = json.find('"', pos);
        if (q == std::string::npos || q > end) break;
        std::string hex = json.substr(pos, q - pos);
        if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
            hex.erase(0, 2);
        if (hex.size() > 16) hex = hex.substr(hex.size() - 16); // low 64 bits of 128-bit reg
        values.push_back(std::strtoull(hex.c_str(), nullptr, 16));
        pos = q;
    }
    return values;
}

bool PCSX2DebugServerClient::pause() {
    return jsonOk(sendCommand("pause"));
}

bool PCSX2DebugServerClient::resume() {
    return jsonOk(sendCommand("resume"));
}

bool PCSX2DebugServerClient::step() {
    // Server-side handler blocks up to 5000ms waiting for the temp
    // breakpoint to hit before replying; the 2000ms default timeout used
    // elsewhere gives up before that, corrupting the socket for good.
    return jsonOk(sendCommand("step", 6000));
}

bool PCSX2DebugServerClient::stepOver() {
    return jsonOk(sendCommand("step_over", 6000));
}

bool PCSX2DebugServerClient::getStatus(Status& out) {
    std::string json = sendCommand("status");
    if (!jsonOk(json)) return false;
    uint64_t pc = 0;
    if (extractHex(json, "pc", pc)) out.pc = static_cast<uint32_t>(pc);
    out.paused = json.find("\"paused\":true") != std::string::npos;
    return true;
}

bool PCSX2DebugServerClient::setBreakpoint(uint32_t address, const std::string& condition, Cpu cpu) {
    if (!isConnected()) return false;
    char addrBuf[16];
    snprintf(addrBuf, sizeof(addrBuf), "%u", address);
    std::string json = "{\"cmd\":\"set_breakpoint\",\"cpu\":\"" + std::string(cpuStr(cpu)) + "\",\"address\":" + std::string(addrBuf);
    if (!condition.empty()) json += ",\"condition\":\"" + condition + "\"";
    json += "}\n";
    int sent = send(static_cast<SOCKET>(m_socket), json.c_str(), static_cast<int>(json.size()), 0);
    if (sent != static_cast<int>(json.size())) return false;
    return jsonOk(recvLine());
}

bool PCSX2DebugServerClient::removeBreakpoint(uint32_t address, Cpu cpu) {
    if (!isConnected()) return false;
    char addrBuf[16];
    snprintf(addrBuf, sizeof(addrBuf), "%u", address);
    std::string json = "{\"cmd\":\"remove_breakpoint\",\"cpu\":\"" + std::string(cpuStr(cpu)) + "\",\"address\":" + std::string(addrBuf) + "}\n";
    int sent = send(static_cast<SOCKET>(m_socket), json.c_str(), static_cast<int>(json.size()), 0);
    if (sent != static_cast<int>(json.size())) return false;
    return jsonOk(recvLine());
}

bool PCSX2DebugServerClient::clearAllBreakpoints(Cpu cpu) {
    return jsonOk(sendCommand("clear_breakpoints", cpu));
}

bool PCSX2DebugServerClient::setWatchpoint(uint32_t address, uint32_t size, const std::string& type, Cpu cpu) {
    if (!isConnected()) return false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%u,\"end\":%u", address, address + size);
    std::string json = "{\"cmd\":\"set_memcheck\",\"cpu\":\"" + std::string(cpuStr(cpu)) + "\",\"address\":" + std::string(buf) +
                        ",\"type\":\"" + type + "\",\"action\":\"break\"}\n";
    int sent = send(static_cast<SOCKET>(m_socket), json.c_str(), static_cast<int>(json.size()), 0);
    if (sent != static_cast<int>(json.size())) return false;
    return jsonOk(recvLine());
}

bool PCSX2DebugServerClient::removeWatchpoint(uint32_t address, uint32_t size, Cpu cpu) {
    if (!isConnected()) return false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%u,\"end\":%u", address, address + size);
    std::string json = "{\"cmd\":\"remove_memcheck\",\"cpu\":\"" + std::string(cpuStr(cpu)) + "\",\"address\":" + std::string(buf) + "}\n";
    int sent = send(static_cast<SOCKET>(m_socket), json.c_str(), static_cast<int>(json.size()), 0);
    if (sent != static_cast<int>(json.size())) return false;
    return jsonOk(recvLine());
}

bool PCSX2DebugServerClient::readRegisters(RawRegisters& out, Cpu cpu) {
    if (!isConnected()) return false;
    // category=0 == GPR; without it the server dumps every register category
    // and the naive "first 32 values" parse would pick up the wrong bank.
    std::string cmdJson = "{\"cmd\":\"read_registers\",\"cpu\":\"" + std::string(cpuStr(cpu)) + "\",\"category\":0}\n";
    int sent = send(static_cast<SOCKET>(m_socket), cmdJson.c_str(), static_cast<int>(cmdJson.size()), 0);
    if (sent != static_cast<int>(cmdJson.size())) return false;
    std::string json = recvLine();
    m_lastRawReply = json;
    if (!jsonOk(json)) { logParseFailure(json); return false; }

    uint64_t pc = 0;
    if (!extractHex(json, "pc", pc)) { logParseFailure(json); return false; }
    out.pc = static_cast<uint32_t>(pc);
    extractHex(json, "hi", out.hi);
    extractHex(json, "lo", out.lo);

    // Confine the GPR value scan to the "GPR" register array so array position
    // == GPR index (matching the shipped PCSX2-MCP client's data.GPR.regs[]).
    // A flat scan across the whole reply picks up pc/status/other-category
    // "value" fields and shifts every GPR (sp -> garbage like 0x1D).
    size_t gprKey  = json.find("\"GPR\"");
    size_t arrStart = (gprKey == std::string::npos) ? std::string::npos
                                                    : json.find('[', gprKey);
    size_t arrEnd   = (arrStart == std::string::npos) ? std::string::npos
                                                      : json.find(']', arrStart);
    if (arrStart == std::string::npos || arrEnd == std::string::npos) {
        logParseFailure(json);
        return false;
    }

    auto values = extractAllValues(json, arrStart, arrEnd);
    if (values.size() < 32) { logParseFailure(json); return false; }
    for (int i = 0; i < 32; ++i) out.gpr[i] = values[i];

    out.valid = true;
    return true;
}

// read_memory: support is UNCONFIRMED against the shipped DebugServer plugin.
// Built defensively — any missing/short/ok:false reply returns false without
// touching `out`, and the socket is dropped on timeout by sendCommand's peer.
// Accepts either a hex-blob reply  {"data":"deadbeef.."}  or a JSON byte array
// {"data":[222,173,..]} — whichever the plugin happens to emit.
bool PCSX2DebugServerClient::readMemory(uint32_t address, uint8_t* out, uint32_t len, Cpu cpu) {
    if (!isConnected() || !out || len == 0) return false;

    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\"cmd\":\"read_memory\",\"cpu\":\"%s\",\"address\":%u,\"size\":%u}\n",
             cpuStr(cpu), address, len);
    std::string cmdJson(buf);
    int sent = send(static_cast<SOCKET>(m_socket), cmdJson.c_str(), static_cast<int>(cmdJson.size()), 0);
    if (sent != static_cast<int>(cmdJson.size())) return false;

    std::string json = recvLine();
    if (!jsonOk(json)) return false;

    auto dpos = json.find("\"data\":");
    if (dpos == std::string::npos) return false;
    dpos += 7;
    while (dpos < json.size() && (json[dpos] == ' ' || json[dpos] == '\t')) ++dpos;
    if (dpos >= json.size()) return false;

    uint32_t written = 0;
    if (json[dpos] == '"') {
        // hex string form: "data":"aabbcc.."
        ++dpos;
        auto end = json.find('"', dpos);
        if (end == std::string::npos) return false;
        for (size_t i = dpos; i + 1 < end && written < len; i += 2) {
            auto hexNibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hexNibble(json[i]), lo = hexNibble(json[i + 1]);
            if (hi < 0 || lo < 0) break;
            out[written++] = static_cast<uint8_t>((hi << 4) | lo);
        }
    } else if (json[dpos] == '[') {
        // JSON byte-array form: "data":[170,187,..]
        ++dpos;
        while (dpos < json.size() && json[dpos] != ']' && written < len) {
            while (dpos < json.size() && (json[dpos] == ' ' || json[dpos] == ',')) ++dpos;
            if (dpos >= json.size() || json[dpos] == ']') break;
            char* endp = nullptr;
            long v = std::strtol(json.c_str() + dpos, &endp, 0);
            if (endp == json.c_str() + dpos) break;
            out[written++] = static_cast<uint8_t>(v & 0xFF);
            dpos = static_cast<size_t>(endp - json.c_str());
        }
    } else {
        return false;
    }

    // Zero any bytes the reply came up short on so callers see a defined buffer.
    for (uint32_t i = written; i < len; ++i) out[i] = 0;
    return written > 0;
}
