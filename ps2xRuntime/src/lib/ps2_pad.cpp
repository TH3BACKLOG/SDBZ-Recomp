#include "runtime/ps2_pad.h"
#include "ps2_host_backend.h"
#include "ps2_log.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr uint8_t kPadAnalogMarker = 0x73;
    constexpr uint8_t kPadStickCenter = 0x80;

    constexpr uint16_t PAD_LEFT = 0x0080u;
    constexpr uint16_t PAD_DOWN = 0x0040u;
    constexpr uint16_t PAD_RIGHT = 0x0020u;
    constexpr uint16_t PAD_UP = 0x0010u;
    constexpr uint16_t PAD_START = 0x0008u;
    constexpr uint16_t PAD_R3 = 0x0004u;
    constexpr uint16_t PAD_L3 = 0x0002u;
    constexpr uint16_t PAD_SELECT = 0x0001u;
    constexpr uint16_t PAD_SQUARE = 0x8000u;
    constexpr uint16_t PAD_CROSS = 0x4000u;
    constexpr uint16_t PAD_CIRCLE = 0x2000u;
    constexpr uint16_t PAD_TRIANGLE = 0x1000u;
    constexpr uint16_t PAD_R1 = 0x0800u;
    constexpr uint16_t PAD_L1 = 0x0400u;
    constexpr uint16_t PAD_R2 = 0x0200u;
    constexpr uint16_t PAD_L2 = 0x0100u;
}

bool PSPadBackend::readState(int /*port*/, int /*slot*/, uint8_t *data, size_t size)
{
    if (!data || size < 32)
        return false;

    std::memset(data, 0, 32);
    // Byte 0 is the SIO2 frame's validity byte: 0 = this frame carries data,
    // 0xFF = no pad answered. The game gates its whole button decode on it --
    // sub_109900 @ 0x109900 does `if ( scePadRead(...) && !buf[0] )` -- so a
    // non-zero byte 0 makes every read silently produce no buttons.
    data[0] = 0x00;
    data[1] = kPadAnalogMarker;
    data[2] = 0xFF;
    data[3] = 0xFF;
    data[4] = data[5] = data[6] = data[7] = kPadStickCenter;

    uint16_t btns = 0xFFFFu;
    constexpr int kGamepad = 0;
    const bool useGamepad = IsGamepadAvailable(kGamepad);
    auto clearBit = [&btns](uint16_t mask)
    { btns &= ~mask; };

    if (useGamepad)
    {
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_UP))
            clearBit(PAD_UP);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_DOWN))
            clearBit(PAD_DOWN);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))
            clearBit(PAD_LEFT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT))
            clearBit(PAD_RIGHT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN))
            clearBit(PAD_CROSS);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))
            clearBit(PAD_CIRCLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_LEFT))
            clearBit(PAD_SQUARE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_FACE_UP))
            clearBit(PAD_TRIANGLE);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_1))
            clearBit(PAD_L1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1))
            clearBit(PAD_R1);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_TRIGGER_2))
            clearBit(PAD_L2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_TRIGGER_2))
            clearBit(PAD_R2);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_RIGHT))
            clearBit(PAD_START);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_MIDDLE_LEFT))
            clearBit(PAD_SELECT);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_LEFT_THUMB))
            clearBit(PAD_L3);
        if (IsGamepadButtonDown(kGamepad, GAMEPAD_BUTTON_RIGHT_THUMB))
            clearBit(PAD_R3);

        float lx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_X);
        float ly = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_LEFT_Y);
        float rx = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_X);
        float ry = GetGamepadAxisMovement(kGamepad, GAMEPAD_AXIS_RIGHT_Y);
        data[6] = static_cast<uint8_t>(128 + lx * 127);
        data[7] = static_cast<uint8_t>(128 + ly * 127);
        data[4] = static_cast<uint8_t>(128 + rx * 127);
        data[5] = static_cast<uint8_t>(128 + ry * 127);
    }
    else
    {
        if (IsKeyDown(KEY_UP) || IsKeyDown(KEY_W))
            clearBit(PAD_UP);
        if (IsKeyDown(KEY_DOWN) || IsKeyDown(KEY_S))
            clearBit(PAD_DOWN);
        if (IsKeyDown(KEY_LEFT) || IsKeyDown(KEY_A))
            clearBit(PAD_LEFT);
        if (IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D))
            clearBit(PAD_RIGHT);
        if (IsKeyDown(KEY_X) || IsKeyDown(KEY_SPACE))
            clearBit(PAD_CROSS);
        if (IsKeyDown(KEY_C) || IsKeyDown(KEY_ESCAPE))
            clearBit(PAD_CIRCLE);
        if (IsKeyDown(KEY_Z) || IsKeyDown(KEY_KP_0))
            clearBit(PAD_SQUARE);
        if (IsKeyDown(KEY_V) || IsKeyDown(KEY_KP_1))
            clearBit(PAD_TRIANGLE);
        if (IsKeyDown(KEY_Q))
            clearBit(PAD_L1);
        if (IsKeyDown(KEY_E))
            clearBit(PAD_R1);
        if (IsKeyDown(KEY_LEFT_SHIFT))
            clearBit(PAD_L2);
        if (IsKeyDown(KEY_RIGHT_SHIFT))
            clearBit(PAD_R2);
        if (IsKeyDown(KEY_ENTER))
            clearBit(PAD_START);
        if (IsKeyDown(KEY_TAB))
            clearBit(PAD_SELECT);
    }

    // Unattended-run aid (default off): PS2X_PAD_AUTOPRESS=N pulses Cross, then
    // Circle, then Start for a few frames every N pad frames, so "press a button"
    // prompts do not park a run nobody is watching.
    static const uint32_t s_autoPeriod = []() -> uint32_t
    {
        const char *s = std::getenv("PS2X_PAD_AUTOPRESS");
        return (s && *s) ? static_cast<uint32_t>(std::strtoul(s, nullptr, 0)) : 0u;
    }();
    // PS2X_PAD_AUTOPRESS_HOLD=N (default 6) sets how many pad frames each pulse
    // is held. The game computes button edges once per GameUpdate, so when the
    // guest renders several vblanks per update a 6-frame pulse can fall entirely
    // between two updates and the press is never seen (Part 165: Auto-Save X
    // took 179 s, title Start never landed).
    static const uint32_t s_autoHold = []() -> uint32_t
    {
        const char *s = std::getenv("PS2X_PAD_AUTOPRESS_HOLD");
        const uint32_t v = (s && *s) ? static_cast<uint32_t>(std::strtoul(s, nullptr, 0)) : 6u;
        return v == 0u ? 6u : v;
    }();
    // PS2X_PAD_AUTOPRESS_SECS=N (default 0 = never stop) ends the pulses N wall
    // seconds after the first pad poll. Start in the pulse cycle opens the fight's
    // pause menu, so perf runs stop it once the match has begun.
    static const auto s_autoT0 = std::chrono::steady_clock::now();
    static const uint32_t s_autoSecs = []() -> uint32_t
    {
        const char *s = std::getenv("PS2X_PAD_AUTOPRESS_SECS");
        return (s && *s) ? static_cast<uint32_t>(std::strtoul(s, nullptr, 0)) : 0u;
    }();
    const bool autoExpired =
        s_autoSecs != 0u &&
        std::chrono::steady_clock::now() - s_autoT0 > std::chrono::seconds(s_autoSecs);
    if (s_autoPeriod >= 16u && !autoExpired)
    {
        static std::atomic<uint32_t> s_autoFrame{0u};
        const uint32_t frame = s_autoFrame.fetch_add(1u, std::memory_order_relaxed);
        const uint32_t phase = frame % s_autoPeriod;
        if (phase < std::min(s_autoHold, s_autoPeriod / 2u))
        {
            // PS2X_PAD_AUTOPRESS_BTNS=X|O|S letters pick the rotation (default "XOS");
            // "X" alone keeps menus advancing without Start pausing a fight.
            static const std::string s_cycle = []() -> std::string
            {
                const char *s = std::getenv("PS2X_PAD_AUTOPRESS_BTNS");
                std::string r;
                for (const char *p = (s && *s) ? s : "XOS"; *p; ++p)
                    if (*p == 'X' || *p == 'O' || *p == 'S')
                        r.push_back(*p);
                return r.empty() ? std::string("XOS") : r;
            }();
            const char c = s_cycle[(frame / s_autoPeriod) % s_cycle.size()];
            clearBit(c == 'X' ? PAD_CROSS : c == 'O' ? PAD_CIRCLE : PAD_START);
        }
    }

    // Host-side edge log: proves keys/autopress reach readState even when PADMAN
    // serves SIO2 polls (the [pad] change log in ps2x_pad_push_frame is then off).
    {
        static std::atomic<uint32_t> s_lastHostBtns{0xFFFFu};
        const uint32_t prev = s_lastHostBtns.exchange(btns, std::memory_order_relaxed);
        if (prev != btns)
            std::printf("[pad] host change btns=0x%04x -> 0x%04x\n", prev, static_cast<unsigned>(btns));
    }
    data[2] = static_cast<uint8_t>(btns & 0xFF);
    data[3] = static_cast<uint8_t>(btns >> 8);
    return true;
}

// ---------------------------------------------------------------------------
// padman per-frame push emulation (Stage 5.12)
//
// SDBZ statically links its own libpad, so it never reaches the Kernel/Stubs/
// Pad.cpp path above via the SDK -- it goes guest libpad -> SIF RPC -> IOP
// padman. Our HLE in ps2_iop.cpp answers modversion/init/portopen, but real
// padman pushes the per-frame pad state IOP->EE *asynchronously* on vsync
// (SIF CMD 0x80000019), which we never did. The game's 256-byte pad buffer
// therefore kept its portopen-time init forever: state=5 (EXECCMD), reqState=2
// (BUSY), length=0, buttons=0xFF -- so scePadRead returned 0 and every button
// read as released. No input path to the guest existed at all.
//
// This writes that push directly into the guest's buffer. Every offset below
// was decoded from the guest's own libpad in ida_scripts/decompiles_SLUS_214_42.txt,
// not guessed:
//
//   0x568990 + port*112 + slot*28          = per-port/slot entry (scePadPortOpen
//                                            @ 0x188080 builds it)
//     +0   -> the game's 64B-aligned 256-byte pad data buffer
//     +16  -> open flag (== 1 once portopen succeeded)
//
//   The 256-byte buffer is two 128-byte halves. The internal reader
//   (0x188320) picks a half with `*(int*)(buf+88) < *(int*)(buf+216)`:
//   STRICTLY-less, so equal counters select half 0. We fill the stale half,
//   then bump its counter strictly past the other one so the flip is atomic
//   from the guest's point of view.
//
//   Within a 128-byte half:
//     +0..31 -> 32-byte pad status; exactly the format readState() emits
//       +0   -> frame validity: 0 = data present, 0xFF = no pad answered
//       +1   -> pad id byte: (id << 4) | halfwords. 0x73 = DualShock2 analog,
//               6 data bytes, no pressure. NOT 0x79 -- that id makes the game
//               overwrite its own "button held" flags with our zeroed pressures
//     +88    -> frame counter (signed int, selects the newer half)
//     +96    -> data length; also scePadRead's (0x1884D0) return value
//     +100   -> ex-mode info flag. scePadInfoMode (0x188970) returns 0 for
//               MODECUREXID/MODECUROFFS/MODETABLE when this equals +114, which
//               is the honest answer for an HLE pad with no mode table
//     +101   -> current pad id byte; scePadInfoMode(MODECURID) returns it >> 4.
//               Leaving it 0 strands the game's pad manager (sub_109900 @
//               0x109900) in state 0 forever, so scePadRead is never called
//     +112   -> state; 6 = PAD_STATE_STABLE
//     +113   -> reqState; 0 = COMPLETE, 2 = BUSY
//     +114   -> pad present / info valid (scePadSetActDirect @ 0x188B60 and
//               scePadInfoMode both require == 1)
//
//   Guest pad manager sub_109900 @ 0x109900, decoded:
//     scePadGetState -> 2/6 enters the state machine, 1/5 waits, else resets.
//     State 0 caches padId = scePadInfoMode(MODECURID); id 7 -> state 70,
//     id 4 -> state 40, anything else (including 0) -> state 99. States 70/71/72
//     negotiate DS2 mode and funnel to 99 on any failure. State 99 is the
//     steady read state; it requires cachedId == (readBuf[1] >> 4).
//
//   scePadGetState (0x188548) special-cases `state==6 && reqState==2` and
//   returns 5 (EXECCMD/busy) -- so writing +112=6 alone is NOT enough,
//   +113 must be cleared to 0 as well.
//
// registerFunction() is not usable here: the game reaches scePadRead by `jal`,
// which the recompiler emits as a direct C++ fn_ call that bypasses the
// dispatch registry.
// ---------------------------------------------------------------------------

namespace
{
    constexpr uint32_t kPadTableBase = 0x00568990u; // libpad per-port entry table
    constexpr uint32_t kPadPortStride = 112u;
    constexpr uint32_t kPadSlotStride = 28u;
    constexpr int kPadMaxPorts = 2;
    constexpr int kPadMaxSlots = 4;

    constexpr uint32_t kRdramMask = 0x1FFFFFFFu;
    constexpr uint32_t kRdramSize = 0x02000000u;

    // Counters are compared as signed int by the guest; wrap well short of
    // overflow. At 60 Hz this point is never reached in a real session, but a
    // wrapped pair must still satisfy "target is strictly greater".
    constexpr int32_t kPadCounterWrap = 0x40000000;

    PSPadBackend g_padPushBackend; // PSPadBackend is stateless / default-constructible
}

// Defined in ps2xIOP/src/emulator/iop_emulator.cpp (SIO2 pad HLE).
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad_buttons;
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad_analog;
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad_served;
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad2_buttons;
extern "C" std::atomic<uint32_t> g_ps2x_sio2_pad2_present;

namespace
{

    inline uint32_t padRead32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v = 0;
        std::memcpy(&v, rdram + (addr & kRdramMask), sizeof(v));
        return v;
    }

    inline void padWrite32(uint8_t *rdram, uint32_t addr, uint32_t v)
    {
        std::memcpy(rdram + (addr & kRdramMask), &v, sizeof(v));
    }

    // A guest pointer is only usable if the whole 256-byte buffer is inside
    // RDRAM and it carries libpad's documented 64-byte alignment.
    inline bool padBufferLooksValid(uint32_t buf)
    {
        const uint32_t phys = buf & kRdramMask;
        if (phys == 0 || (phys & 0x3Fu) != 0)
            return false;
        return phys + 256u <= kRdramSize;
    }
}

// ---------------------------------------------------------------------------
// PS2X_PAD_SCRIPT=<file>: scene-aware scripted input for unattended graphics
// sweeps (build_scripts\gfx_tour.ps1 -Script). One step per line, '#' comments:
//   wait app=<Class> | app!=<Class> | mode=<n> | p1=<n> | ticks=<n>  [timeout=<ticks>]
//   press <B[+B..]> [hold=<ticks>] [gap=<ticks>] [pad=2] [until <cond> [timeout=<ticks>]]
//   repeat <n> ... end
//   label <name>      goto <name> [<times>] [if app=|app!=|mode=|p1=<..>]
//   poke <addr> <value> [w=1|2|4]
//   poke [<ptr>]+<off> <value> [w=..]   writes to word-at-<ptr> + <off> (heap objects;
//                                       skipped + logged if that word is not a RAM pointer)
//   log <text>
// Buttons: X O T Q(square) S(start) SEL U D L R L1 R1 L2 R2 L3 R3.
// pad=2 presses on a second, scripted controller: port 1 is plugged in (SIO2 HLE
// and the direct libpad push) when any step uses pad=2 or PS2X_PAD2=1; else it
// stays unplugged as before.
// Ticks are guest vsyncs (GSRegisters::vsyncTick, the PS2X_GSCAP tick), so a
// press is held across GameUpdates however slowly the guest runs. A wait that
// times out (default 3600 ticks) logs and moves on: a sweep never parks.
//
// Scene = app chain from CAppMain 0x63fdf0: [obj+4] -> next object -> vtable
// slot 2 (+0x08) returns the class name, via `lui/addiu` -> string or
// `lw off(gp)` (gp 0x503070) -> pointer -> string. All 54 CApp vtables in the
// ELF decode this way (static scan 10-03). Following +4 past the current app
// is a guess (H): it stops at the first object that does not decode.
// mode = byte 0x5e6b3c (sweep1 10-03: 1 once Original is picked), p1 = byte
// 0x5b4070 (sweep1: the character-select cursor's fighter, Goku 1, Krillin 4).
// [scene] logs on every change (also without a script when PS2X_GSCAP or
// PS2X_SCENE_LOG is set); [padscript] logs every step.
// ---------------------------------------------------------------------------
namespace
{
    namespace padscript
    {
        constexpr uint32_t kAppMain = 0x0063FDF0u;
        constexpr uint32_t kGp = 0x00503070u;
        constexpr uint32_t kModeByte = 0x005E6B3Cu;
        constexpr uint32_t kP1Byte = 0x005B4070u;
        constexpr uint32_t kRam = 0x02000000u;
        constexpr uint32_t kDefaultTimeout = 3600u;
        constexpr int kSceneLogCap = 4000;

        enum class Op { Wait, Press, Goto, Label, Poke, Log };
        enum class CondKind { None, App, NotApp, Mode, P1, Ticks };

        struct Cond
        {
            CondKind kind = CondKind::None;
            std::string app;
            uint32_t value = 0;
        };

        struct Step
        {
            Op op = Op::Log;
            int line = 0;
            std::string text;
            Cond cond;
            uint32_t timeout = 0;
            uint16_t mask = 0;
            uint32_t hold = 10, gap = 10;
            uint32_t pad = 1;
            std::string label;
            int target = -1;
            uint32_t times = 0;
            uint32_t addr = 0, value = 0, width = 1;
            bool deref = false;
            uint32_t off = 0;
        };

        struct Scene
        {
            std::vector<std::string> chain;
            uint32_t vt = 0;
            uint8_t mode = 0, p1 = 0;
        };

        std::atomic<uint64_t> g_tick{0u};

        bool inRam(uint32_t a, uint32_t n = 4u)
        {
            a &= 0x1FFFFFFFu;
            return a >= 0x00100000u && a + n <= kRam;
        }

        uint32_t r32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v = 0;
            if (inRam(a))
                std::memcpy(&v, ram + (a & 0x1FFFFFFFu), 4);
            return v;
        }

        std::string cstr(const uint8_t *ram, uint32_t a)
        {
            std::string s;
            if (!inRam(a, 1u))
                return s;
            a &= 0x1FFFFFFFu;
            for (uint32_t i = 0; i < 48u && a + i < kRam; ++i)
            {
                const uint8_t c = ram[a + i];
                if (c == 0)
                    return s;
                if (c < 32 || c > 126)
                    return std::string();
                s.push_back(static_cast<char>(c));
            }
            return std::string();
        }

        // vtable slot 2 -> class name; "" when the first instructions do not
        // match either pattern. Cached per vtable (code never changes).
        std::string className(const uint8_t *ram, uint32_t vt)
        {
            static std::unordered_map<uint32_t, std::string> s_cache;
            if ((vt & 3u) != 0 || !inRam(vt + 8u))
                return std::string();
            const auto it = s_cache.find(vt);
            if (it != s_cache.end())
                return it->second;
            const uint32_t fn = r32(ram, vt + 8u);
            std::string name;
            if ((fn & 3u) == 0 && inRam(fn, 24u))
            {
                uint32_t hi[32] = {};
                bool hasHi[32] = {};
                for (uint32_t k = 0; k < 6u && name.empty(); ++k)
                {
                    const uint32_t w = r32(ram, fn + 4u * k);
                    const uint32_t op = w >> 26, rs = (w >> 21) & 31u, rt = (w >> 16) & 31u;
                    const int32_t simm = static_cast<int16_t>(w & 0xFFFFu);
                    if (op == 0x0Fu) // lui
                    {
                        hi[rt] = (w & 0xFFFFu) << 16;
                        hasHi[rt] = true;
                    }
                    else if (op == 0x09u && hasHi[rs]) // addiu rt, rs, imm
                        name = cstr(ram, hi[rs] + static_cast<uint32_t>(simm));
                    else if (op == 0x23u && rs == 28u) // lw rt, off(gp)
                        name = cstr(ram, r32(ram, kGp + static_cast<uint32_t>(simm)));
                }
            }
            if (name.rfind("CApp", 0) != 0)
                name.clear();
            // Cache only hits: an object being constructed can show a vtable whose
            // code is fine but whose name pointer is not set up yet (gp case).
            if (!name.empty())
                s_cache.emplace(vt, name);
            return name;
        }

        Scene readScene(const uint8_t *ram)
        {
            Scene sc;
            uint32_t obj = kAppMain;
            for (int depth = 0; depth < 4 && inRam(obj); ++depth)
            {
                const uint32_t vt = r32(ram, obj);
                std::string n = className(ram, vt);
                if (n.empty())
                    break;
                sc.chain.push_back(std::move(n));
                sc.vt = vt;
                const uint32_t next = r32(ram, obj + 4u);
                if (next == obj)
                    break;
                obj = next;
            }
            sc.mode = ram[kModeByte];
            sc.p1 = ram[kP1Byte];
            return sc;
        }

        std::string chainText(const Scene &sc)
        {
            std::string s;
            for (const auto &n : sc.chain)
                s += (s.empty() ? "" : ">") + n;
            return s.empty() ? std::string("none") : s;
        }

        uint16_t buttonMask(const std::string &tok)
        {
            static const std::pair<const char *, uint16_t> kNames[] = {
                {"X", PAD_CROSS}, {"O", PAD_CIRCLE}, {"T", PAD_TRIANGLE}, {"Q", PAD_SQUARE},
                {"S", PAD_START}, {"SEL", PAD_SELECT}, {"U", PAD_UP}, {"D", PAD_DOWN},
                {"L", PAD_LEFT}, {"R", PAD_RIGHT}, {"L1", PAD_L1}, {"R1", PAD_R1},
                {"L2", PAD_L2}, {"R2", PAD_R2}, {"L3", PAD_L3}, {"R3", PAD_R3}};
            uint16_t m = 0;
            std::stringstream ss(tok);
            std::string b;
            while (std::getline(ss, b, '+'))
            {
                bool found = false;
                for (const auto &kv : kNames)
                    if (b == kv.first)
                    {
                        m |= kv.second;
                        found = true;
                    }
                if (!found)
                    return 0;
            }
            return m;
        }

        bool parseCond(const std::string &kv, Cond &c)
        {
            const auto eq = kv.find('=');
            if (eq == std::string::npos)
                return false;
            std::string key = kv.substr(0, eq);
            const std::string val = kv.substr(eq + 1);
            if (val.empty())
                return false;
            if (key == "app")
                c.kind = CondKind::App, c.app = val;
            else if (key == "app!")
                c.kind = CondKind::NotApp, c.app = val;
            else if (key == "mode")
                c.kind = CondKind::Mode, c.value = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 0));
            else if (key == "p1")
                c.kind = CondKind::P1, c.value = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 0));
            else if (key == "ticks")
                c.kind = CondKind::Ticks, c.value = static_cast<uint32_t>(std::strtoul(val.c_str(), nullptr, 0));
            else
                return false;
            return true;
        }

        bool condMet(const Cond &c, const Scene &sc, uint64_t elapsed)
        {
            switch (c.kind)
            {
            case CondKind::App:
                return std::find(sc.chain.begin(), sc.chain.end(), c.app) != sc.chain.end();
            case CondKind::NotApp:
                return std::find(sc.chain.begin(), sc.chain.end(), c.app) == sc.chain.end();
            case CondKind::Mode:
                return sc.mode == c.value;
            case CondKind::P1:
                return sc.p1 == c.value;
            case CondKind::Ticks:
                return elapsed >= c.value;
            default:
                return false;
            }
        }

        // Parses lines [i, end) into out; returns false on the first error.
        bool parse(const std::vector<std::pair<int, std::string>> &lines, size_t &i, std::vector<Step> &out, bool inRepeat)
        {
            while (i < lines.size())
            {
                const int ln = lines[i].first;
                const std::string &text = lines[i].second;
                ++i;
                std::vector<std::string> tok;
                {
                    std::stringstream ss(text);
                    std::string t;
                    while (ss >> t)
                        tok.push_back(t);
                }
                if (tok.empty())
                    continue;
                const std::string &cmd = tok[0];
                auto fail = [&](const char *why)
                {
                    std::printf("[padscript] ERROR line %d: %s: %s\n", ln, why, text.c_str());
                    return false;
                };
                Step s;
                s.line = ln;
                s.text = text;
                if (cmd == "end")
                {
                    if (!inRepeat)
                        return fail("end without repeat");
                    return true;
                }
                if (cmd == "repeat")
                {
                    const uint32_t n = tok.size() > 1 ? static_cast<uint32_t>(std::strtoul(tok[1].c_str(), nullptr, 0)) : 0u;
                    std::vector<Step> body;
                    if (!parse(lines, i, body, true))
                        return false;
                    for (uint32_t k = 0; k < n; ++k)
                        out.insert(out.end(), body.begin(), body.end());
                    continue;
                }
                if (cmd == "wait")
                {
                    s.op = Op::Wait;
                    s.timeout = kDefaultTimeout;
                    for (size_t k = 1; k < tok.size(); ++k)
                    {
                        if (tok[k].rfind("timeout=", 0) == 0)
                            s.timeout = static_cast<uint32_t>(std::strtoul(tok[k].c_str() + 8, nullptr, 0));
                        else if (!parseCond(tok[k], s.cond))
                            return fail("bad wait condition");
                    }
                    if (s.cond.kind == CondKind::None)
                        return fail("wait needs app=/app!=/mode=/p1=/ticks=");
                    if (s.cond.kind == CondKind::Ticks && text.find("timeout=") == std::string::npos)
                        s.timeout = 0; // a plain tick wait is its own timeout
                }
                else if (cmd == "press")
                {
                    s.op = Op::Press;
                    if (tok.size() < 2 || (s.mask = buttonMask(tok[1])) == 0)
                        return fail("bad button list");
                    bool until = false;
                    for (size_t k = 2; k < tok.size(); ++k)
                    {
                        const std::string &t = tok[k];
                        if (t == "until")
                            until = true, s.timeout = kDefaultTimeout;
                        else if (t.rfind("hold=", 0) == 0)
                            s.hold = std::max(1ul, std::strtoul(t.c_str() + 5, nullptr, 0));
                        else if (t.rfind("gap=", 0) == 0)
                            s.gap = static_cast<uint32_t>(std::strtoul(t.c_str() + 4, nullptr, 0));
                        else if (t == "pad=1" || t == "pad=2")
                            s.pad = static_cast<uint32_t>(t[4] - '0');
                        else if (t.rfind("timeout=", 0) == 0)
                            s.timeout = static_cast<uint32_t>(std::strtoul(t.c_str() + 8, nullptr, 0));
                        else if (!until || !parseCond(t, s.cond))
                            return fail("bad press option");
                    }
                    if (until && s.cond.kind == CondKind::None)
                        return fail("until needs a condition");
                }
                else if (cmd == "label" && tok.size() == 2)
                    s.op = Op::Label, s.label = tok[1];
                else if (cmd == "goto" && tok.size() >= 2)
                {
                    s.op = Op::Goto, s.label = tok[1];
                    size_t k = 2;
                    if (k < tok.size() && tok[k] != "if")
                        s.times = static_cast<uint32_t>(std::strtoul(tok[k++].c_str(), nullptr, 0));
                    if (k < tok.size())
                    {
                        if (tok[k] != "if" || k + 2 != tok.size() || !parseCond(tok[k + 1], s.cond) ||
                            s.cond.kind == CondKind::Ticks)
                            return fail("goto takes [<times>] [if app=|app!=|mode=|p1=]");
                    }
                }
                else if (cmd == "poke" && tok.size() >= 3)
                {
                    s.op = Op::Poke;
                    const char *a = tok[1].c_str();
                    s.deref = (*a == '[');
                    char *end = nullptr;
                    s.addr = static_cast<uint32_t>(std::strtoul(a + (s.deref ? 1 : 0), &end, 0));
                    if (s.deref)
                    {
                        if (*end != ']')
                            return fail("poke [<ptr>]+<off> needs ]");
                        ++end;
                        if (*end == '+' || *end == '-')
                        {
                            const bool neg = (*end == '-');
                            const uint32_t o = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
                            s.off = neg ? 0u - o : o;
                        }
                        if (*end != '\0')
                            return fail("bad poke [<ptr>]+<off>");
                    }
                    s.value = static_cast<uint32_t>(std::strtoul(tok[2].c_str(), nullptr, 0));
                    if (tok.size() > 3 && tok[3].rfind("w=", 0) == 0)
                        s.width = static_cast<uint32_t>(std::strtoul(tok[3].c_str() + 2, nullptr, 0));
                    if ((s.width != 1u && s.width != 2u && s.width != 4u) ||
                        !inRam(s.addr, s.deref ? 4u : s.width))
                        return fail("bad poke");
                }
                else if (cmd == "log")
                    s.op = Op::Log;
                else
                    return fail("unknown step");
                out.push_back(std::move(s));
            }
            if (inRepeat)
            {
                std::printf("[padscript] ERROR: repeat without end\n");
                return false;
            }
            return true;
        }

        struct Engine
        {
            bool active = false;
            bool sceneLog = false;
            bool finished = false;
            bool usesPad2 = false;
            std::vector<Step> steps;
            std::vector<uint32_t> gotoCount;
            size_t pc = 0;
            bool entered = false;
            uint64_t stepStart = 0;
            Scene last;
            bool haveLast = false;
            int sceneLogged = 0;

            Engine()
            {
                const char *path = std::getenv("PS2X_PAD_SCRIPT");
                sceneLog = (path && *path) || std::getenv("PS2X_GSCAP") || std::getenv("PS2X_SCENE_LOG");
                if (!path || !*path)
                    return;
                std::ifstream f(path);
                if (!f)
                {
                    std::printf("[padscript] ERROR cannot open %s\n", path);
                    return;
                }
                std::vector<std::pair<int, std::string>> lines;
                std::string l;
                for (int n = 1; std::getline(f, l); ++n)
                {
                    const auto hash = l.find('#');
                    if (hash != std::string::npos)
                        l.erase(hash);
                    while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t'))
                        l.pop_back();
                    if (!l.empty())
                        lines.emplace_back(n, l);
                }
                size_t i = 0;
                if (!parse(lines, i, steps, false))
                {
                    steps.clear();
                    return;
                }
                for (size_t k = 0; k < steps.size(); ++k)
                {
                    if (steps[k].op != Op::Goto)
                        continue;
                    for (size_t j = 0; j < steps.size(); ++j)
                        if (steps[j].op == Op::Label && steps[j].label == steps[k].label)
                            steps[k].target = static_cast<int>(j);
                    if (steps[k].target < 0)
                    {
                        std::printf("[padscript] ERROR line %d: unknown label %s\n", steps[k].line, steps[k].label.c_str());
                        steps.clear();
                        return;
                    }
                }
                gotoCount.assign(steps.size(), 0u);
                active = !steps.empty();
                for (const Step &s : steps)
                    usesPad2 = usesPad2 || (s.op == Op::Press && s.pad == 2u);
                std::printf("[padscript] loaded %s: %zu step(s)\n", path, steps.size());
                std::fflush(stdout);
            }

            void next()
            {
                ++pc;
                entered = false;
            }

            // Buttons to hold this frame, active-high: pad 1 in bits 0..15, pad 2 in 16..31.
            uint32_t frame(uint8_t *ram, uint64_t tick)
            {
                const Scene sc = readScene(ram);
                if (sceneLog && (!haveLast || sc.chain != last.chain || sc.mode != last.mode || sc.p1 != last.p1))
                {
                    if (sceneLogged < kSceneLogCap)
                    {
                        ++sceneLogged;
                        std::printf("[scene] tick=%llu app=%s vt=0x%x mode=%u p1=0x%02x\n",
                                    static_cast<unsigned long long>(tick), chainText(sc).c_str(), sc.vt,
                                    static_cast<unsigned>(sc.mode), static_cast<unsigned>(sc.p1));
                        if (sceneLogged == kSceneLogCap)
                            std::printf("[scene] cap: %d lines\n", kSceneLogCap);
                        std::fflush(stdout);
                    }
                    last = sc;
                    haveLast = true;
                }
                if (!active)
                    return 0;
                for (int guard = 0; guard < 64; ++guard) // instant steps run in one frame
                {
                    if (pc >= steps.size())
                    {
                        if (!finished)
                        {
                            finished = true;
                            std::printf("[padscript] end tick=%llu\n", static_cast<unsigned long long>(tick));
                            std::fflush(stdout);
                        }
                        return 0;
                    }
                    Step &s = steps[pc];
                    if (!entered)
                    {
                        entered = true;
                        stepStart = tick;
                        if (s.op != Op::Label)
                        {
                            std::printf("[padscript] step %zu line %d tick=%llu: %s\n", pc, s.line,
                                        static_cast<unsigned long long>(tick), s.text.c_str());
                            std::fflush(stdout);
                        }
                    }
                    const uint64_t elapsed = tick - stepStart;
                    switch (s.op)
                    {
                    case Op::Label:
                    case Op::Log:
                        next();
                        continue;
                    case Op::Poke:
                    {
                        uint32_t dst = s.addr;
                        if (s.deref)
                        {
                            const uint32_t ptr = r32(ram, s.addr);
                            dst = ptr + s.off;
                            if (ptr == 0u || !inRam(dst, s.width))
                            {
                                std::printf("[padscript] poke skipped: [0x%x]=0x%x not a RAM pointer tick=%llu\n",
                                            s.addr, ptr, static_cast<unsigned long long>(tick));
                                next();
                                continue;
                            }
                            std::printf("[padscript] poke [0x%x]=0x%x +0x%x -> 0x%x old=%d\n", s.addr, ptr, s.off,
                                        dst, static_cast<int>(r32(ram, dst & ~3u)));
                        }
                        std::memcpy(ram + (dst & 0x1FFFFFFFu), &s.value, s.width); // little-endian low bytes
                        next();
                        continue;
                    }
                    case Op::Goto:
                        if ((s.cond.kind == CondKind::None || condMet(s.cond, sc, 0u)) &&
                            (s.times == 0u || gotoCount[pc] < s.times))
                        {
                            ++gotoCount[pc];
                            pc = static_cast<size_t>(s.target);
                            entered = false;
                        }
                        else
                            next();
                        continue;
                    case Op::Wait:
                        if (condMet(s.cond, sc, elapsed))
                        {
                            next();
                            continue;
                        }
                        break;
                    case Op::Press:
                    {
                        const uint32_t mask = static_cast<uint32_t>(s.mask) << (s.pad == 2u ? 16 : 0);
                        if (s.cond.kind == CondKind::None)
                        {
                            if (elapsed < s.hold)
                                return mask;
                            if (elapsed < static_cast<uint64_t>(s.hold) + s.gap)
                                return 0;
                            next();
                            continue;
                        }
                        if (condMet(s.cond, sc, elapsed))
                        {
                            next();
                            continue;
                        }
                        if (!(s.timeout != 0u && elapsed >= s.timeout))
                            return (elapsed % (static_cast<uint64_t>(s.hold) + s.gap)) < s.hold ? mask : 0u;
                        break;
                    }
                    }
                    // Wait / press-until still pending: time out or keep waiting.
                    if (s.timeout != 0u && elapsed >= s.timeout)
                    {
                        std::printf("[padscript] TIMEOUT step %zu line %d tick=%llu app=%s mode=%u: %s\n", pc, s.line,
                                    static_cast<unsigned long long>(tick), chainText(sc).c_str(),
                                    static_cast<unsigned>(sc.mode), s.text.c_str());
                        std::fflush(stdout);
                        next();
                        continue;
                    }
                    return 0;
                }
                return 0;
            }
        };
    }
}

// Guest vsync tick for the pad script; the present loop calls this right before
// ps2x_pad_push_frame (ps2_runtime.cpp).
extern "C" void ps2x_pad_script_set_tick(uint64_t tick)
{
    padscript::g_tick.store(tick, std::memory_order_relaxed);
}

// Emulates one padman vsync push for every port/slot the guest has opened.
// Called once per presented host frame from PS2Runtime's present loop -- the
// same thread raylib polls input on, so IsKeyDown()/IsGamepadButtonDown() are
// read on the thread that owns them.
extern "C" void ps2x_pad_push_frame(uint8_t *rdram)
{
    if (!rdram)
        return;

    static int s_pushLogged = 0;
    static int s_changeLogged = 0;
    static uint16_t s_lastButtons[kPadMaxPorts][kPadMaxSlots] = {};
    static bool s_seenPort[kPadMaxPorts][kPadMaxSlots] = {};
    constexpr int kPushLogCap = 8;
    constexpr int kChangeLogCap = 64;

    // Read the host pad once per frame (readState also advances the autopress
    // cycle) and publish it to the IOP's SIO2 pad HLE (iop_emulator.cpp), which
    // answers the disc PADMAN.IRX's DualShock2 polls. Once PADMAN is actually
    // being served, it owns libpad's buffer (it SIF-DMAs each frame itself), so
    // this direct push stands down instead of racing it (Part 165).
    uint8_t hostStatus[32];
    const bool hostOk = g_padPushBackend.readState(0, 0, hostStatus, sizeof(hostStatus));
    static bool s_pad2 = false;
    uint16_t pad2Buttons = 0xFFFFu; // active-low, libpad wire order
    {
        // Scripted presses (PS2X_PAD_SCRIPT) merge into the host pad, active-low,
        // before it is published to both the SIO2 HLE and the direct push.
        static padscript::Engine s_script;
        static const bool s_pad2Env = [] { const char *e = std::getenv("PS2X_PAD2"); return e && *e == '1'; }();
        const uint32_t press = s_script.frame(rdram, padscript::g_tick.load(std::memory_order_relaxed));
        const uint16_t press1 = static_cast<uint16_t>(press & 0xFFFFu);
        if (hostOk && press1)
        {
            hostStatus[2] &= static_cast<uint8_t>(~press1 & 0xFFu);
            hostStatus[3] &= static_cast<uint8_t>(~press1 >> 8);
        }
        pad2Buttons = static_cast<uint16_t>(~(press >> 16) & 0xFFFFu);
        if (!s_pad2 && (s_script.usesPad2 || s_pad2Env))
        {
            s_pad2 = true;
            g_ps2x_sio2_pad2_present.store(1u, std::memory_order_relaxed);
            RUNTIME_LOG("[pad] scripted pad 2 plugged into port 1\n");
        }
        g_ps2x_sio2_pad2_buttons.store(pad2Buttons, std::memory_order_relaxed);
    }
    if (hostOk)
    {
        g_ps2x_sio2_pad_buttons.store(static_cast<uint32_t>(hostStatus[2] | (hostStatus[3] << 8)),
                                      std::memory_order_relaxed);
        g_ps2x_sio2_pad_analog.store(static_cast<uint32_t>(hostStatus[4]) |
                                         (static_cast<uint32_t>(hostStatus[5]) << 8) |
                                         (static_cast<uint32_t>(hostStatus[6]) << 16) |
                                         (static_cast<uint32_t>(hostStatus[7]) << 24),
                                     std::memory_order_relaxed);
    }
    if (g_ps2x_sio2_pad_served.load(std::memory_order_relaxed) != 0u)
    {
        // 10-07 (pad2a probe, VERIFIED in gsdump/pad2a/run_log.txt): with port 1 plugged in, PADMAN reaches
        // command 0x42 and this handoff disabled the direct push, but PADMAN then logged 109x "VBLANK
        // OVERLAP" and no input reached the game (CAppWarning never advanced; scripted X was seen by the
        // host side as btns 0xbfff). Without port 1 the game ran on the direct push all along. So keep the
        // direct push whenever pad 2 is present (or PS2X_PAD_KEEP_DIRECT=1) until the SIO2 completion path
        // is fixed. HYP: PADMAN's transfer-done event never fires in the SIO2 HLE.
        static const bool s_keepDirect = [] { const char *e = std::getenv("PS2X_PAD_KEEP_DIRECT"); return e && *e == '1'; }();
        static bool s_handoffLogged = false;
        if (!s_handoffLogged)
        {
            s_handoffLogged = true;
            if (s_pad2 || s_keepDirect)
                RUNTIME_LOG("[pad] PADMAN is serving SIO2 pad polls -- direct buffer push KEPT (pad 2 present / PS2X_PAD_KEEP_DIRECT)\n");
            else
                RUNTIME_LOG("[pad] PADMAN is serving SIO2 pad polls -- direct buffer push disabled\n");
        }
        if (!s_pad2 && !s_keepDirect)
            return;
    }

    for (int port = 0; port < kPadMaxPorts; ++port)
    {
        for (int slot = 0; slot < kPadMaxSlots; ++slot)
        {
            const uint32_t entry =
                kPadTableBase + static_cast<uint32_t>(port) * kPadPortStride + static_cast<uint32_t>(slot) * kPadSlotStride;

            if (padRead32(rdram, entry + 16) != 1u) // open flag
                continue;

            const uint32_t buf = padRead32(rdram, entry + 0);
            if (!padBufferLooksValid(buf))
            {
                if (s_pushLogged < kPushLogCap)
                {
                    ++s_pushLogged;
                    RUNTIME_LOG("[pad] port=" << port << " slot=" << slot
                                              << " REJECTED buffer=0x" << std::hex << buf << std::dec << "\n");
                }
                continue;
            }

            const int32_t c0 = static_cast<int32_t>(padRead32(rdram, buf + 88));
            const int32_t c1 = static_cast<int32_t>(padRead32(rdram, buf + 216));

            // Guest picks half1 only when c0 < c1 (ties -> half0). Fill the
            // half it is NOT currently reading.
            const uint32_t liveHalf = (c0 < c1) ? 1u : 0u;
            const uint32_t targetHalf = 1u - liveHalf;
            const uint32_t block = buf + targetHalf * 128u;

            uint8_t status[32];
            // Port 1+ gets a valid but idle pad: leaving an opened port in its
            // portopen BUSY state would strand any guest loop that waits for
            // every opened pad to reach PAD_STATE_STABLE.
            const bool isHostPad = (port == 0 && slot == 0);
            if (isHostPad)
            {
                if (!hostOk)
                    continue;
                std::memcpy(status, hostStatus, sizeof(status));
            }
            else
            {
                std::memset(status, 0, sizeof(status));
                status[0] = 0x00; // valid frame, nothing pressed
                status[1] = kPadAnalogMarker;
                const bool isPad2 = s_pad2 && port == 1 && slot == 0;
                status[2] = isPad2 ? static_cast<uint8_t>(pad2Buttons & 0xFFu) : 0xFF;
                status[3] = isPad2 ? static_cast<uint8_t>(pad2Buttons >> 8) : 0xFF;
                status[4] = status[5] = status[6] = status[7] = kPadStickCenter;
            }

            std::memcpy(rdram + ((block + 0) & kRdramMask), status, sizeof(status));
            padWrite32(rdram, block + 96, 32u);        // length / scePadRead return
            rdram[(block + 100) & kRdramMask] = 1;     // ex-mode info unavailable
            rdram[(block + 101) & kRdramMask] = kPadAnalogMarker; // current pad id byte
            rdram[(block + 112) & kRdramMask] = 6;     // state    = PAD_STATE_STABLE
            rdram[(block + 113) & kRdramMask] = 0;     // reqState = COMPLETE
            rdram[(block + 114) & kRdramMask] = 1;     // pad present / info valid

            int32_t next = ((c0 > c1) ? c0 : c1) + 1;
            if (next >= kPadCounterWrap)
            {
                // Restart the pair rather than let the signed compare wrap.
                padWrite32(rdram, buf + (liveHalf * 128u) + 88, 0u);
                next = 1;
            }

            // Publish the filled half last: the guest EE thread runs on a
            // different host thread, so the data stores must land first.
            std::atomic_thread_fence(std::memory_order_release);
            padWrite32(rdram, block + 88, static_cast<uint32_t>(next));

            const uint16_t buttons = static_cast<uint16_t>(status[2] | (status[3] << 8));

            if (!s_seenPort[port][slot])
            {
                s_seenPort[port][slot] = true;
                s_lastButtons[port][slot] = buttons;
                if (s_pushLogged < kPushLogCap)
                {
                    ++s_pushLogged;
                    RUNTIME_LOG("[pad] first push port=" << port << " slot=" << slot
                                                         << " buf=0x" << std::hex << buf
                                                         << " half=" << std::dec << targetHalf
                                                         << " ctr=" << next
                                                         << " btns=0x" << std::hex << buttons << std::dec << "\n");
                    if (s_pushLogged == kPushLogCap)
                        RUNTIME_LOG("[pad] cap: first-push log capped at " << kPushLogCap << "\n");
                }
            }
            else if (buttons != s_lastButtons[port][slot])
            {
                s_lastButtons[port][slot] = buttons;
                if (s_changeLogged < kChangeLogCap)
                {
                    ++s_changeLogged;
                    RUNTIME_LOG("[pad] change port=" << port << " slot=" << slot
                                                     << " btns=0x" << std::hex << buttons << std::dec
                                                     << " half=" << targetHalf << " ctr=" << next << "\n");
                    if (s_changeLogged == kChangeLogCap)
                        RUNTIME_LOG("[pad] cap: change log capped at " << kChangeLogCap << "\n");
                }
            }
        }
    }
}
