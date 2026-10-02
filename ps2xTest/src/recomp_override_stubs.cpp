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

// Called by recovered bodies in ps2xRuntime/src/lib/Kernel/recovered/, which are
// part of ps2_runtime; the callees are generated runner bodies.
void sub_00175B68_0x175b68(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_00175B68_0x175b68");
}

void array_call_dtor_0x171bb0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("array_call_dtor_0x171bb0");
}

void mem_copy_0x18e250(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("mem_copy_0x18e250");
}

// Callees of the recovered sub_293200 (ps2_runtime.lib); same abort-stub rule as above.
void blend_state_get_field410_0x1c60e0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("blend_state_get_field410_0x1c60e0");
}

void blend_state_is_idle_0x1c62a0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("blend_state_is_idle_0x1c62a0");
}

void blend_state_start_0x1c6320(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("blend_state_start_0x1c6320");
}

void blend_state_stop_0x1c62d0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("blend_state_stop_0x1c62d0");
}

void camera_fade_is_active_0x2c1bb0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("camera_fade_is_active_0x2c1bb0");
}

void camera_fade_set_0x2c1830(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("camera_fade_set_0x2c1830");
}

void camera_set_mode_0x2c1c20(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("camera_set_mode_0x2c1c20");
}

void math_acos_f_0x185bd8(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("math_acos_f_0x185bd8");
}

void math_asin_f_0x185e78(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("math_asin_f_0x185e78");
}

void mem_fill_z_31_clone_18_0x2926d0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("mem_fill_z_31_clone_18_0x2926d0");
}

void noop_wrapper____0x1f59c0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("noop_wrapper____0x1f59c0");
}

void noop_wrapper____0x240710(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("noop_wrapper____0x240710");
}

void obj_change_state_0x1cbe80(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("obj_change_state_0x1cbe80");
}

void obj_enable_sphere_collide_0x1e6290(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("obj_enable_sphere_collide_0x1e6290");
}

void obj_get_sub_entry_field_0x23a080(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("obj_get_sub_entry_field_0x23a080");
}

void obj_set_fields_0x1e6260(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("obj_set_fields_0x1e6260");
}

void singleton_sub_2be0_clone_01_0x23ad00(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("singleton_sub_2be0_clone_01_0x23ad00");
}

void sound_play_3d_0x2fd640(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sound_play_3d_0x2fd640");
}

void sub_0019E440_0x19e440(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_0019E440_0x19e440");
}

void sub_001CB750_0x1cb750(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_001CB750_0x1cb750");
}

void sub_001CD9D0_0x1cd9d0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_001CD9D0_0x1cd9d0");
}

void sub_001CDAF0_0x1cdaf0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_001CDAF0_0x1cdaf0");
}

void sub_00292450_0x292450(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_00292450_0x292450");
}

void sub_00292570_0x292570(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_00292570_0x292570");
}

void sub_00294900_0x294900(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_00294900_0x294900");
}

void sub_00294A90_0x294a90(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_00294A90_0x294a90");
}

void sub_00294B40_0x294b40(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("sub_00294B40_0x294b40");
}

void table_entry_get_stride8_0x3a32f0(uint8_t *, R5900Context *, PS2Runtime *)
{
    ps2xTestGuestStub("table_entry_get_stride8_0x3a32f0");
}

