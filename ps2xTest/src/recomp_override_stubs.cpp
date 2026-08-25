// Link shims for the unit-test executable.
//
// ps2_runtime (STATIC) contains src/lib/game_overrides.cpp, which registers a
// handful of *generated* recompiled function bodies by address:
//
//     runtime.registerFunction(0x00180D30u, &fn_180D30_0x180d30);
//
// Those bodies live in ps2xRuntime/src/runner/*.cpp, which CMake globs only
// into the ps2EntryRunner executable -- never into the ps2_runtime library.
// The symbols therefore resolve for the runner and for nothing else, so any
// other consumer of ps2_runtime fails to link with LNK2019.
//
// Linking the real runner TUs here is not viable: each one references further
// fn_* bodies and the transitive closure is most of the generated codebase,
// which would make the test executable as slow to build as the runner and
// defeat the purpose of having a fast unit-test harness.
//
// The tests never execute guest code, so aborting stubs are correct. If a
// future test does reach one of these, it aborts with a clear message rather
// than silently doing nothing.
//
// NOTE: every new entry in game_overrides.cpp that points at a generated body
// will add another unresolved symbol here. Add a matching stub below.

#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct R5900Context;
class PS2Runtime;

namespace
{
    [[noreturn]] void ps2xTestGuestStub(const char *name)
    {
        std::fprintf(stderr,
                     "[ps2x_tests] fatal: guest function '%s' was called.\n"
                     "The unit-test executable links only stub bodies for recompiled\n"
                     "guest code. Tests must drive runtime subsystems directly.\n",
                     name);
        std::abort();
    }
}

void fn_180D30_0x180d30(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("fn_180D30_0x180d30");
}

void fn_1A4500_0x1a4500(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("fn_1A4500_0x1a4500");
}

void fn_1BF2E0_0x1bf2e0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("fn_1BF2E0_0x1bf2e0");
}

void fn_22C8F0_0x22c8f0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("fn_22C8F0_0x22c8f0");
}

// Not an fn_* name, but the same situation: the Stage 5.10 [poolbase] probe in
// game_overrides.cpp wraps this generated body, so ps2_runtime references it.
void singleton_get_camera_0x199db0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("singleton_get_camera_0x199db0");
}
