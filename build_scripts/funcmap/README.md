# Function map reconstruction

`sdbz_func_map_merged.csv` is the `ghidra_output` input for `ps2_recomp`.
The original Ghidra export was lost along with `config.toml`. This directory
holds the map and the scripts that rebuilt it, so the loss cannot recur.

## Why this exists

`ghidra_output` wants CSV `name,start,end,size` with a header line the parser
discards. **Any line that does not split on exactly 4 commas is skipped
silently** — no warning, no error, exit code 0 (`elf_parser.cpp`,
`loadGhidraFunctionMap`). Two probe runs were pointed at `symbols.map`, which
is space-separated, so both ran with **zero** function-boundary data while
looking perfectly healthy.

`ps2xRuntime/symbols.map` is **not** an input. It is a RecompDebugger artifact
that `generate_symbols_map.ps1` produces *from* the runner tree.

`SLUS_214.42` is stripped (`.symtab` and `.strtab` both size 0), so no names
can be recovered from the ELF itself.

## How the map was rebuilt

Neither half of the lost export was actually gone — they were in two places:

| part | source | provenance |
|---|---|---|
| boundaries (`start`/`end`/`size`) | `ELF/SLUS_214.42.i64` | IDA's own analysis, original |
| names | `ps2xRuntime/symbols.map` | original Ghidra names, one generation removed (runner filenames were produced *by* the lost export) |

Joining them on address is sound rather than a guess: **all 12,072 real names in
`symbols.map` land exactly on an IDA function start, 0 mismatches.**

    python export_func_map.py    # idalib -> sdbz_func_map.csv  (16,912 funcs, 617 real names)
    python merge_func_map.py     # + symbols.map names -> sdbz_func_map_merged.csv (12,072 named)
    python patch_func_map.py     # + 4 hand-measured functions IDA missed (16,916)

`export_func_map.py` needs IDA Professional 9.3's `idalib`, and the `.i64` must
not be open in the GUI. It closes with `save=False` — read-only w.r.t. the
database.

## The four added functions

`patch_func_map.py` adds `0x4dd500 / 0x4dd570 / 0x4dd5b0 / 0x4dd5e0`. The
static-initializer pointer table at `0x004e6c90` holds 86 consecutive code
pointers; 82 are already defined functions and map coverage begins exactly at
the 5th target. Only the first four fall in a hole IDA never analysed.

All four have the identical shape, read out of the ELF:

    lui/addiu/sw ...       argument setup
    j 0x00171f10           tail call to the shared registration routine
    addiu $a2, $a2, imm    delay slot
    nop [nop ...]          padding to the next 0x10 boundary

so `end` is the word after the delay slot, padding excluded — the same
convention IDA uses for its neighbours here (`sub_4DE250` ends `0x4de444`, not
`0x4de450`).

Note a `jr $ra` scan flags all four as having no return. That is correct and
expected: they are tail calls and never execute a return of their own. The `j`
is the terminator. `probe_4dd5xx*.py` are the measurement scripts, kept as the
evidence trail.

## Effect

|  | no map | merged map | + 4 added |
|---|---|---|---|
| worst fallback promotion | 125,228 | 204 | 204 |
| dispatch entries | — | 377,270 | **377,274** |
| lost vs live table | 1,002 | 36 | **32** |
| reachable regressions | 264 | 10 | **6** |
| gained vs live | — | 1,821 | 1,821 |

The 6 remaining are interior addresses inside functions the table does have,
reachable but with no entry guard. Not yet resolved.

## Caveats

- The "live" `register_functions.cpp` is the hand-converted legacy file
  (Jul-14 format conversion of May-28 data), so live-vs-new is a
  **cross-generation** diff, not a clean before/after.
- Whether the interior addresses are actually *called* at runtime is
  unmeasured.
- Names are original-but-one-generation-removed; boundaries are IDA's own
  analysis. Neither is a guess, but they are two different provenances.
