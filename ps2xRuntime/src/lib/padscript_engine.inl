// padscript_engine.inl -- the PS2X_PAD_SCRIPT engine, shared by two hosts:
//   * PS2Recomp:  ps2xRuntime/src/lib/ps2_pad.cpp (the only runtime TU that includes it)
//   * PCSX2:      F:\PCSX2-src pcsx2/DebugTools/PadScript.cpp (local branch sdbz-tools)
// so one sweep script drives the recomp and PCSX2 the same way, frame for frame.
// Moved out of ps2_pad.cpp unchanged on 10-08, except that output goes through
// PADSCRIPT_LOG / PADSCRIPT_FLUSH (PCSX2 has no console: it routes them to a log file).
//
// Not a header: include it once per TU, at file scope. Everything is in an anonymous
// namespace, so each host gets its own copy.

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef PADSCRIPT_LOG
#define PADSCRIPT_LOG std::printf
#endif
#ifndef PADSCRIPT_FLUSH
#define PADSCRIPT_FLUSH() std::fflush(stdout)
#endif

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
//   freeze <addr> <value> [w=..]   like poke, but re-applied every frame until `unfreeze`
//   freeze [<ptr>]+<off> <value>   (keeps health/ki topped up so a fuzzed fight does not end)
//   unfreeze                       drops every freeze
//   snap <name>                    dumps all EE RAM to $PS2X_SNAP_DIR/<name>.bin (max 48 per run);
//                                  build_scripts/ramdiff.py finds the changing words (cursor/stage ids)
//   fuzz ticks=<n> [seed=<n>] [pad=2] [btns=X+O+T+Q+R1+..] [hold=<a>-<b>] [gap=<a>-<b>] [max=<n>]
//        [until <cond>]            seeded random button mashing for n ticks (default pool: every
//                                  button except Start/Select/L3/R3; hold/gap in ticks, max = most
//                                  buttons held at once); ends early when <cond> is met
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
        // libpad button bits (active-high here; the wire is active-low). Same values as
        // ps2_pad.cpp's PAD_* -- repeated so the engine also compiles inside PCSX2.
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

        constexpr uint32_t kAppMain = 0x0063FDF0u;
        constexpr uint32_t kGp = 0x00503070u;
        constexpr uint32_t kModeByte = 0x005E6B3Cu;
        constexpr uint32_t kP1Byte = 0x005B4070u;
        constexpr uint32_t kRam = 0x02000000u;
        constexpr uint32_t kDefaultTimeout = 3600u;
        constexpr int kSceneLogCap = 4000;

        enum class Op { Wait, Press, Goto, Label, Poke, Log, Freeze, Unfreeze, Snap, Fuzz };
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
            std::vector<uint16_t> pool; // fuzz: single-button masks
            uint32_t holdLo = 2, holdHi = 8, gapLo = 2, gapHi = 8, seed = 1, maxBtn = 2;
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

        // "a-b" or "a" -> [lo, hi]
        bool parseRange(const char *t, uint32_t &lo, uint32_t &hi)
        {
            char *end = nullptr;
            lo = static_cast<uint32_t>(std::strtoul(t, &end, 0));
            hi = lo;
            if (*end == '-')
                hi = static_cast<uint32_t>(std::strtoul(end + 1, &end, 0));
            return *end == '\0' && hi >= lo;
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
                    PADSCRIPT_LOG("[padscript] ERROR line %d: %s: %s\n", ln, why, text.c_str());
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
                else if ((cmd == "poke" || cmd == "freeze") && tok.size() >= 3)
                {
                    s.op = (cmd == "poke") ? Op::Poke : Op::Freeze;
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
                else if (cmd == "unfreeze")
                    s.op = Op::Unfreeze;
                else if (cmd == "snap" && tok.size() == 2)
                {
                    s.op = Op::Snap;
                    s.label = tok[1];
                    for (char c : s.label)
                        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
                            return fail("snap name must be [A-Za-z0-9_-]");
                }
                else if (cmd == "fuzz")
                {
                    s.op = Op::Fuzz;
                    bool until = false;
                    for (size_t k = 1; k < tok.size(); ++k)
                    {
                        const std::string &t = tok[k];
                        if (t == "until")
                            until = true;
                        else if (t.rfind("ticks=", 0) == 0)
                            s.timeout = static_cast<uint32_t>(std::strtoul(t.c_str() + 6, nullptr, 0));
                        else if (t.rfind("seed=", 0) == 0)
                            s.seed = static_cast<uint32_t>(std::strtoul(t.c_str() + 5, nullptr, 0));
                        else if (t.rfind("max=", 0) == 0)
                            s.maxBtn = static_cast<uint32_t>(std::max(1ul, std::strtoul(t.c_str() + 4, nullptr, 0)));
                        else if (t == "pad=1" || t == "pad=2")
                            s.pad = static_cast<uint32_t>(t[4] - '0');
                        else if (t.rfind("hold=", 0) == 0)
                        {
                            if (!parseRange(t.c_str() + 5, s.holdLo, s.holdHi) || s.holdLo == 0)
                                return fail("bad fuzz hold=a-b");
                        }
                        else if (t.rfind("gap=", 0) == 0)
                        {
                            if (!parseRange(t.c_str() + 4, s.gapLo, s.gapHi))
                                return fail("bad fuzz gap=a-b");
                        }
                        else if (t.rfind("btns=", 0) == 0)
                        {
                            std::stringstream bs(t.substr(5));
                            std::string b;
                            while (std::getline(bs, b, '+'))
                            {
                                const uint16_t m = buttonMask(b);
                                if (m == 0)
                                    return fail("bad fuzz btns=");
                                s.pool.push_back(m);
                            }
                        }
                        else if (!until || !parseCond(t, s.cond))
                            return fail("bad fuzz option");
                    }
                    if (s.timeout == 0)
                        return fail("fuzz needs ticks=<n>");
                    if (until && s.cond.kind == CondKind::None)
                        return fail("until needs a condition");
                    if (s.pool.empty())
                        for (const char *bn : {"X", "O", "T", "Q", "R1", "R2", "L1", "L2", "U", "D", "L", "R"})
                            s.pool.push_back(buttonMask(bn));
                }
                else
                    return fail("unknown step");
                out.push_back(std::move(s));
            }
            if (inRepeat)
            {
                PADSCRIPT_LOG("[padscript] ERROR: repeat without end\n");
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
            std::vector<Step> frozen;
            int snapsTaken = 0;
            uint64_t fzRng = 1, fzPhaseEnd = 0;
            bool fzHolding = false;
            uint16_t fzMask = 0;

            uint32_t fzNext()
            {
                fzRng ^= fzRng >> 12;
                fzRng ^= fzRng << 25;
                fzRng ^= fzRng >> 27;
                return static_cast<uint32_t>((fzRng * 0x2545F4914F6CDD1Dull) >> 32);
            }

            static uint32_t fzSpan(uint32_t lo, uint32_t hi, uint32_t r)
            {
                return lo + (hi > lo ? r % (hi - lo + 1u) : 0u);
            }

            // One poke / frozen poke. verbose = the one-shot log lines.
            static void applyPoke(uint8_t *ram, const Step &s, uint64_t tick, bool verbose)
            {
                uint32_t dst = s.addr;
                if (s.deref)
                {
                    const uint32_t ptr = r32(ram, s.addr);
                    dst = ptr + s.off;
                    if (ptr == 0u || !inRam(dst, s.width))
                    {
                        if (verbose)
                            PADSCRIPT_LOG("[padscript] poke skipped: [0x%x]=0x%x not a RAM pointer tick=%llu\n",
                                        s.addr, ptr, static_cast<unsigned long long>(tick));
                        return;
                    }
                    if (verbose)
                        PADSCRIPT_LOG("[padscript] poke [0x%x]=0x%x +0x%x -> 0x%x old=%d\n", s.addr, ptr, s.off,
                                    dst, static_cast<int>(r32(ram, dst & ~3u)));
                }
                std::memcpy(ram + (dst & 0x1FFFFFFFu), &s.value, s.width); // little-endian low bytes
            }

            void takeSnap(const uint8_t *ram, const std::string &name, uint64_t tick)
            {
                if (snapsTaken >= 48)
                {
                    PADSCRIPT_LOG("[padscript] snap %s skipped: 48-per-run cap\n", name.c_str());
                    return;
                }
                const char *d = std::getenv("PS2X_SNAP_DIR");
                const std::filesystem::path dir = (d && *d) ? d : ".";
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                const std::filesystem::path file = dir / (name + ".bin");
                std::ofstream o(file, std::ios::binary | std::ios::trunc);
                o.write(reinterpret_cast<const char *>(ram), kRam);
                o.close();
                ++snapsTaken;
                PADSCRIPT_LOG("[padscript] snap %s -> %s tick=%llu ok=%d\n", name.c_str(), file.string().c_str(),
                            static_cast<unsigned long long>(tick), o.good() ? 1 : 0);
                PADSCRIPT_FLUSH();
            }

            Engine()
            {
                const char *path = std::getenv("PS2X_PAD_SCRIPT");
                sceneLog = (path && *path) || std::getenv("PS2X_GSCAP") || std::getenv("PS2X_SCENE_LOG");
                if (!path || !*path)
                    return;
                std::ifstream f(path);
                if (!f)
                {
                    PADSCRIPT_LOG("[padscript] ERROR cannot open %s\n", path);
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
                        PADSCRIPT_LOG("[padscript] ERROR line %d: unknown label %s\n", steps[k].line, steps[k].label.c_str());
                        steps.clear();
                        return;
                    }
                }
                gotoCount.assign(steps.size(), 0u);
                active = !steps.empty();
                for (const Step &s : steps)
                    usesPad2 = usesPad2 || (s.op == Op::Press && s.pad == 2u);
                PADSCRIPT_LOG("[padscript] loaded %s: %zu step(s)\n", path, steps.size());
                PADSCRIPT_FLUSH();
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
                        PADSCRIPT_LOG("[scene] tick=%llu app=%s vt=0x%x mode=%u p1=0x%02x\n",
                                    static_cast<unsigned long long>(tick), chainText(sc).c_str(), sc.vt,
                                    static_cast<unsigned>(sc.mode), static_cast<unsigned>(sc.p1));
                        if (sceneLogged == kSceneLogCap)
                            PADSCRIPT_LOG("[scene] cap: %d lines\n", kSceneLogCap);
                        PADSCRIPT_FLUSH();
                    }
                    last = sc;
                    haveLast = true;
                }
                if (!active)
                    return 0;
                for (const Step &fz : frozen)
                    applyPoke(ram, fz, tick, false);
                for (int guard = 0; guard < 64; ++guard) // instant steps run in one frame
                {
                    if (pc >= steps.size())
                    {
                        if (!finished)
                        {
                            finished = true;
                            PADSCRIPT_LOG("[padscript] end tick=%llu\n", static_cast<unsigned long long>(tick));
                            PADSCRIPT_FLUSH();
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
                            PADSCRIPT_LOG("[padscript] step %zu line %d tick=%llu: %s\n", pc, s.line,
                                        static_cast<unsigned long long>(tick), s.text.c_str());
                            PADSCRIPT_FLUSH();
                        }
                        if (s.op == Op::Fuzz)
                        {
                            fzRng = (static_cast<uint64_t>(s.seed) + 1u) * 0x9E3779B97F4A7C15ull;
                            fzHolding = false;
                            fzMask = 0;
                            fzPhaseEnd = tick;
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
                        applyPoke(ram, s, tick, true);
                        next();
                        continue;
                    case Op::Freeze:
                        applyPoke(ram, s, tick, true);
                        frozen.push_back(s);
                        next();
                        continue;
                    case Op::Unfreeze:
                        frozen.clear();
                        next();
                        continue;
                    case Op::Snap:
                        takeSnap(ram, s.label, tick);
                        next();
                        continue;
                    case Op::Fuzz:
                    {
                        if ((s.cond.kind != CondKind::None && condMet(s.cond, sc, elapsed)) || elapsed >= s.timeout)
                        {
                            next();
                            continue;
                        }
                        if (tick >= fzPhaseEnd)
                        {
                            if (fzHolding)
                            {
                                fzHolding = false;
                                fzMask = 0;
                                fzPhaseEnd = tick + fzSpan(s.gapLo, s.gapHi, fzNext());
                            }
                            else
                            {
                                uint16_t m = 0;
                                const uint32_t n = 1u + fzNext() % s.maxBtn;
                                for (uint32_t k = 0; k < n; ++k)
                                    m |= s.pool[fzNext() % s.pool.size()];
                                if ((m & PAD_UP) && (m & PAD_DOWN))
                                    m &= static_cast<uint16_t>(~PAD_DOWN);
                                if ((m & PAD_LEFT) && (m & PAD_RIGHT))
                                    m &= static_cast<uint16_t>(~PAD_RIGHT);
                                fzMask = m;
                                fzHolding = true;
                                fzPhaseEnd = tick + fzSpan(s.holdLo, s.holdHi, fzNext());
                            }
                        }
                        return static_cast<uint32_t>(fzMask) << (s.pad == 2u ? 16 : 0);
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
                        PADSCRIPT_LOG("[padscript] TIMEOUT step %zu line %d tick=%llu app=%s mode=%u: %s\n", pc, s.line,
                                    static_cast<unsigned long long>(tick), chainText(sc).c_str(),
                                    static_cast<unsigned>(sc.mode), s.text.c_str());
                        PADSCRIPT_FLUSH();
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
