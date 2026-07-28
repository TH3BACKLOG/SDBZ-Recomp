# Super Dragon Ball Z — Ghidra Naming & Coding Guidelines
*PS2 Recomp Project Reference*

---

## 1. Core Naming Structure

All labels follow this pattern:

```
[module]_[subsystem]_[action/description]
```

- Use **snake_case** (lowercase, underscores between words)
- Be descriptive but concise
- Avoid abbreviations unless they're obvious (`pos`, `vel`, `hp`, `anim`)

**Examples:**
```
battle_player_update
char_goku_ki_charge
ui_hud_healthbar_draw
input_check_button_held
```

---

## 2. Module Prefixes

Use these prefixes to group code by game system:

| Prefix       | System                                      |
|--------------|---------------------------------------------|
| `battle_`    | Core fight loop, round logic, match state   |
| `char_`      | Character-specific logic (per-fighter code) |
| `player_`    | Player controller / input binding logic     |
| `input_`     | Raw controller input reading                |
| `move_`      | Move execution, startup/active/recovery     |
| `anim_`      | Animation playback, blending, transitions   |
| `cam_`       | Camera tracking, zoom, angles               |
| `fx_`        | Visual effects (ki blasts, hit sparks, etc.)|
| `ui_`        | HUD, menus, character select, life bars     |
| `audio_`     | Music, sound effects, voice lines           |
| `physics_`   | Collision, hitboxes, hurtboxes, knockback   |
| `ki_`        | Ki gauge, energy, charging mechanics        |
| `ai_`        | CPU opponent behavior / difficulty          |
| `story_`     | Arcade/story mode flow, cutscenes           |
| `net_`       | Online/network code (PS2 Online)            |
| `mem_`       | Memory allocation, pool management          |
| `gfx_`       | Renderer, draw calls, GS/VU interface       |
| `file_`      | File loading, asset streaming, pak files    |
| `init_`      | Initialization routines (called at startup) |
| `debug_`     | Debug/dev leftover functions                |
| `unk_`       | Unknown — purpose not yet identified        |

---

## 3. Character Sub-Prefixes

For `char_` functions, add the character name as a sub-prefix:

```
char_goku_[action]
char_vegeta_[action]
char_gohan_[action]
char_piccolo_[action]
char_trunks_[action]
char_cell_[action]
char_frieza_[action]
char_buu_[action]
char_broly_[action]
char_krillin_[action]
char_yamcha_[action]
char_tien_[action]
```

**Examples:**
```
char_goku_init_stats
char_vegeta_dragon_finisher
char_cell_regenerate
```

If a function is shared across all characters, use `char_base_` or just `char_`:
```
char_base_take_damage
char_base_guard_break
```

---

## 4. Common Action Words

Use consistent verbs for actions:

| Verb         | Use for                                      |
|--------------|----------------------------------------------|
| `init`       | Setup / first-time initialization            |
| `update`     | Per-frame logic tick                         |
| `draw`       | Rendering / display                          |
| `load`       | Loading from file or memory                  |
| `reset`      | Resetting state to default                   |
| `check`      | Boolean check / condition test               |
| `get`        | Returning a value                            |
| `set`        | Writing a value                              |
| `calc`       | Calculation / math operation                 |
| `spawn`      | Creating an entity or effect                 |
| `destroy`    | Removing / freeing an entity                 |
| `play`       | Playing audio or animation                   |
| `stop`       | Stopping audio or animation                  |
| `handle`     | Event handler                                |
| `process`    | Processing input or state                    |
| `transition` | State machine transition                     |

---

## 5. Variable Naming

### Global Variables
Prefix with `g_` followed by module:
```c
g_battle_round_timer
g_char_p1_health
g_ki_gauge_max
g_ui_selected_char
```

### Pointers / Structs
Use `p_` prefix for pointers:
```c
p_player1_state
p_current_move
```

### Unknown Variables
Until identified, use:
```c
g_unk_8034A210      // unknown global at that address
unk_field_0x14      // unknown struct field at offset 0x14
```

### Integer Types
Use the standard decomp type aliases (not bare C types):

| Alias  | Meaning             |
|--------|---------------------|
| `s8`   | signed 8-bit int    |
| `u8`   | unsigned 8-bit int  |
| `s16`  | signed 16-bit int   |
| `u16`  | unsigned 16-bit int |
| `s32`  | signed 32-bit int   |
| `u32`  | unsigned 32-bit int |
| `f32`  | 32-bit float        |

---

## 6. Status / Progress Tracking in Ghidra

Use Ghidra's **plate comments** (above a function) to track decompilation status:

```
// [STATUS: stub]       — Function identified, not yet decompiled
// [STATUS: named]      — Renamed and understood, not fully decompiled
// [STATUS: decompiled] — Decompiled and cleaned up
// [STATUS: verified]   — Confirmed correct via PCSX2 testing
// [STATUS: ported]     — PC port implementation written
```

Use Ghidra **bookmarks** with these same category names to filter later.

---

## 7. C Code Style Guidelines

These rules apply when writing or cleaning up decompiled C code.

### Blank Lines for Readability

Use a single blank line to separate:
- variable declarations from executable code
- conditional blocks and loops from other code
- cases within a switch statement
- function definitions

Never use more than one blank line.

```c
// Bad ❌
s32 a = 0;
printf("%d\n", a);

// Good ✅
s32 a = 0;

printf("%d\n", a);
```

```c
// Bad ❌
switch (a) {
case 1:
    a += 1;
    break;
case 2:
    a -= 1;
    break;
}

// Good ✅
switch (a) {
case 1:
    a += 1;
    break;

case 2:
    a -= 1;
    break;
}
```

```c
// Bad ❌
void func_a() { ... }
void func_b() { ... }

// Good ✅
void func_a() { ... }

void func_b() { ... }
```

### Mark Fallthroughs and Empty Blocks

Unmarked fallthroughs look like bugs. Always comment them explicitly.
Empty blocks should also be commented to show the emptiness is intentional.

```c
// Fallthrough ✅
switch (a) {
case 1:
    a += 1;
    /* fallthrough */

case 2:
    a -= 1;
    break;
}

// Empty block ✅
void my_func() {
    // Do nothing
}
```

### Don't Use Unnecessary Unsigned Literals

Unsigned suffixes (`U`) are unnecessary when assigning to a typed variable.

```c
// Bad ❌
u32 a = 16U;

// Good ✅
u32 a = 16;
```

### Remove Auto-Generated Comments

m2c and other decompilers sometimes add noise comments (e.g. `/* irregular */` on switch statements). Remove these — they don't belong in cleaned-up code.

```c
// Bad ❌
switch (bits->bitdepth) { /* irregular */

// Good ✅
switch (bits->bitdepth) {
```

### Use Decimal for Bit Shifts

Shift amounts should be written in decimal, not hex.

```c
// Bad ❌
s8 a = (color >> 0x18) & 0xFF;

// Good ✅
s8 a = (color >> 24) & 0xFF;
```

### Initialize Local Variables In-Place

```c
// Bad ❌
s32 a;
s32 b;

a = 10;
b = 5;

// Good ✅
s32 a = 10;
s32 b = 5;
```

### Empty Parameter Lists Use `()` Not `(void)`

```c
// Bad ❌
s32 get_offset(void);

// Good ✅
s32 get_offset();
```

---

## 8. Unknown / Unidentified Functions

Until a function is understood, keep the Ghidra default name but add a plate comment:

```c
// [STATUS: stub] — called from battle_update, likely hitbox check
FUN_001a3c40(...)
```

When you identify it, rename it fully and update the status tag. Do **not** guess at names — a wrong name is worse than `unk_`.

---

## 9. Quick Reference — Renaming Examples

| Before (Ghidra default) | After (named)                    |
|-------------------------|----------------------------------|
| `FUN_001a3c40`          | `battle_player_update_state`     |
| `FUN_0023ff10`          | `char_goku_kamehameha_startup`   |
| `FUN_00089b20`          | `ui_hud_draw_ki_gauge`           |
| `FUN_002c1100`          | `anim_play_intro_sequence`       |
| `FUN_00031440`          | `input_check_button_held`        |
| `FUN_0011e890`          | `physics_calc_knockback`         |
| `DAT_003a1220`          | `g_battle_round_timer`           |
| `DAT_003a1230`          | `g_char_p1_health`               |

---

## 10. Tips for Working with Ghidra + PCSX2

- **Use PCSX2's debugger** to set breakpoints on functions found in Ghidra — when the game hits that address, you'll know exactly what triggers it.
- **Cross-reference EE addresses** — PCSX2 memory maps to the same addresses you see in Ghidra. `FUN_001a3c40` in Ghidra is the same address to watch in PCSX2's memory viewer.
- **Log registers at breakpoints** to understand function arguments and help name parameters correctly.
- **Label data first** — identifying global variables (health, timer, state flags) often makes the surrounding functions obvious.

---

*Last updated: April 2026 | Super Dragon Ball Z PS2 Recomp*
