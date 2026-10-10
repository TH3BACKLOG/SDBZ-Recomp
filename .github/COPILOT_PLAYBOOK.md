# Copilot Playbook — SDBZ Recomp

Ordered work plans for making progress on this project without a Claude session in the loop.

**Read [copilot-instructions.md](copilot-instructions.md) first.** It carries the hard rules. This file assumes them and does not repeat them.

---

## 0. How to start any task

Do these four things before writing a single line, every time. Skipping them is how the last few sessions went sideways.

1. **Read the agent memory index** — `C:\Users\mwlab\.claude\projects\f--SDBZ-Recomp\memory\MEMORY.md`. One line per fact. Open the entries relevant to your task. It records closed investigations and retracted theories that the code cannot tell you about.
2. **Read the newest session block of `PS2_PROJECT_STATE.md`** (top of file, ~500 KB — read a range, never the whole thing). The sub-phase tracker there is authoritative for what is open.
3. **Read `command_log.md`** in the memory directory for exact paths and invocations. Never reconstruct a path from memory.
4. **Pick the lowest-numbered plan below that is not marked done.** If the user named a task, do that instead.

### The single most important habit

**Measure before you theorise, and measure the final value.** This project has produced a long list of confident wrong answers, every one of them from reasoning about code instead of observing a run. The corrections that stuck:

- A **capped probe looks exactly like a probe that never fired.** If the validity gate reports `[cap]` for a tag, the *absence* of that tag later in the log proves nothing. This has happened three times.
- A counter sampled **mid-pipeline** describes a value that may never be stored. Pair any masked scan with a full-width one.
- **"Who wrote X to Y" is answered with the hardware data breakpoint**, not with greps or store-site bisection. Bisection produced four retractions in ten days.
- **Absence of a log line is not absence of the event** until you have read the emit site and confirmed it was reachable and uncapped.
- **Watch for stale object files.** Twice now, a source edit silently failed to recompile and a "verified" fix was never in the binary. If a probe's output fields do not match the source you just wrote, force-touch the source mtime and rebuild before believing anything.

---

## 1. The only code pattern you need

Almost all work here is *adding a diagnostic probe*, not changing behaviour. There is exactly one sanctioned shape for it, and it exists to avoid touching a `.h` file (which triggers a multi-hour rebuild of ~30,000 TUs).

**All state is file-scope in a `.cpp`. Nothing new goes in a header, ever.**

```cpp
// Inside the file's anonymous namespace, in ps2xRuntime/src/lib/<file>.cpp
if (ps2_diag::enabled())
{
    static std::atomic<uint64_t> s_n{0};
    const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);

    // Cap at the EMIT SITE, never at the sink. An uncapped cerr in a retry
    // loop once produced a 225 MB log and burned 42% of a run.
    if (n < 24)
    {
        RUNTIME_LOG("[mytag] n=" << std::dec << n
            << " field=0x" << std::hex << someValue
            << (n == 23 ? " [cap]" : ""));
    }
}
```

Rules that go with it:

- **Emit `[cap]` on the last line** you will print. The launcher's validity gate scrapes for it and will warn you that the tag's later absence is meaningless. Without it you will misread your own run.
- **Need to share state between two `.cpp` files?** Declare it `extern` in the consumer, define it in the producer. Do **not** create a header.
- **`<iomanip>` is not included in `ps2_gs_gpu.cpp`.** Hand-roll hex there rather than adding an include.
- **A declaration inside a `case` label needs brace-wrapping** or it will not compile.
- Behavioural fixes — as opposed to probes — go in **`game_overrides.cpp` only**. Never in `fn_*.cpp` or anywhere under `src/runner/`.

---

## 2. How to read a run

Hand the user this pair of commands (you do not run them — see the hard rules):

```powershell
cd "F:\SDBZ Recomp"
.\build.ps1 RelWithDebInfo 6
```

```powershell
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger `
    -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe"
```

Then read the output **in this order**:

1. **The tag census** the launcher prints after the run. It counts every tag in the raw log. This answers "did my probe fire at all" before you grep anything. A count of `0` for a tag you just added means the probe is dead — wrong call site, unreachable branch, or a stale `.obj` — not that the game misbehaved.
2. **The validity gate.** If it prints `RUN VALIDITY FAILURE`, resolve that before drawing any conclusion from the run. It catches the three failure modes that produce a healthy-looking log full of nothing.
3. **`build_scripts/analyze_run.py`**, e.g. `python build_scripts/analyze_run.py --gs`. Query this rather than reading the log.
4. **Only then** grep `run_log.txt` for specifics.

Two traps specific to this log:

- **`run_log.txt` is UTF-16.** Use `Select-String -Encoding unicode`, or you will match nothing and conclude the run failed.
- **`RUNTIME_LOG` records get concatenated onto one physical line.** Line counts therefore undercount badly. To count occurrences, read the file raw and regex `\[tag\][^\[]*` — do not count lines. (The launcher's console splitter and tag census both already do this correctly.)

---

## Plan A — Find who should be filling EE RAM `0x89d400` ★ ACTIVE

**Why.** `[gifsrc]` shows a 256 KB Path3 texture upload every frame from EE RAM `0x89d400` that is entirely zero, with the whole surrounding 384 KB (`0x88d400`–`0x8fd400`) zero as well. That is not a blank glyph sheet — it is a buffer nothing ever wrote. Stage 5.9 is an asset-load failure, not a render failure.

**Status.** The mechanism is wired and unrun. `PS2X_HWWATCH_ADDR` was added to `game_overrides.cpp` on 2026-08-04: it overrides the address the existing arm sites publish, so DR0 parks on a fixed target for the whole run instead of following a guest-derived pointer.

**Run it** (hand to the user):

```powershell
$env:PS2X_HWWATCH = '1'
$env:PS2X_HWWATCH_ADDR = '0x89d400'
$env:PS2X_HWWATCH_VAL = '0xFFFFFFFF'   # record every store, not just 0x1
& "F:\SDBZ Recomp\launch_recomp.ps1" -Determinism 0 -RunSeconds 90 -NoDebugger `
    -Exe "F:\SDBZ Recomp\build\ps2xRuntime\RelWithDebInfo\ps2EntryRunner.exe"
```

⚠️ **Clear those three env vars before any run whose timing matters.** The armer thread suspends every thread in the process 4×/second and cost 19.06s of CPU in a 96s run — about 20% of wall clock. Perf numbers from a `HWWATCH` run are void.

**Read the result** from the `HWSTAT` heartbeat and `<probe>.hwwatch.txt`:

| Outcome | Meaning | Next |
|---|---|---|
| `hits > 0` | Someone writes it. The symbolized backtrace names the producer. | Follow the writer; ask why it writes zeros or writes too late. |
| `hits = 0`, `skipped > 0` | Writes land but never with the filtered value. | You forgot `PS2X_HWWATCH_VAL=0xFFFFFFFF`. Re-run. |
| `hits = 0` **and** `skipped = 0` | **The region is never written at all.** | Go to Plan B. The asset load never targets this buffer. |

The armed-and-silent case is only meaningful because `HWSTAT` proves the watchpoint was live. That heartbeat is why "no hits" can be read as a result rather than as a dead probe.

**Exit test.** You can name either the function that writes `0x89d400`, or state with `HWSTAT` evidence that nothing does.

---

## Plan B — Trace the asset load path for the font sheet

**Do this only if Plan A returns "never written."** Otherwise it is a wasted session.

**The question.** Something should be reading the font/glyph asset off the disc and decompressing it into `0x89d400`. Find where that chain stops.

**Order of investigation** — cheapest first:

1. **Confirm the file is even requested.** Grep the run log for `ARKD:` and `iop:` records around the read that should fetch the font asset. `ARKD_DVD.IRX` is the real module running in the R3000 interpreter, so its records are ground truth. Note that `[ARKD:run]` and `[ARKD:CALL]` are **capped at 32 and 64** — raise the caps at their emit sites if you need to see past them, and do not read their absence as "it never happened."
2. **Confirm the read completes.** A request that is issued but never completed looks identical to a request never made, if you only look at the destination buffer.
3. **Confirm the destination.** If the read completes into a *different* address than `0x89d400`, then the bug is in whoever computes the upload source address, not in the loader — and that flips the whole investigation to the GIF side.
4. **Check `PS2_CD_ROOT` resolution.** The pseudo-disc root must resolve the asset path. A silently-unresolved file is a very cheap explanation and should be ruled out before anything expensive.

**Do not** hand-write IOP output values to make this progress. The IOP modules run for real; faking a result invalidates every downstream measurement. This rule has been restated across multiple sessions.

**Exit test.** You can point at the exact step in disc-read → decompress → store where the font data stops arriving.

---

## Plan C — A/B against PCSX2 for any "is this normal?" question

**Use this whenever you are about to call recomp behaviour anomalous.** Never make that claim without a real PCSX2 number to compare against — it is the single most common source of wrong conclusions in this project.

The PCSX2 MCP tools are available (`pcsx2_connect`, `pcsx2_pause`, `pcsx2_read_memory`, `pcsx2_read_registers`, `pcsx2_set_breakpoint`, …). Never ask the user to look at PCSX2 for you.

Recipe for Stage 5.9 specifically:

1. `pcsx2_connect`, boot SDBZ to MainMenu, `pcsx2_pause`.
2. `pcsx2_read_memory` at `0x89d400`. **If it is populated on real PCSX2 and zero for us, that is the whole bug, localized to the load path.** If it is zero there too, our upload source address is wrong and Plan B step 3 applies.
3. Take the comparison to the *final* value, not an intermediate — see the measurement discipline in §0.

Two gotchas recorded from previous sessions:

- **Confirm which target you are attached to.** Real PCSX2 and `ps2EntryRunner` are indistinguishable by EE address alone.
- **Breakpoint and watchpoint hit counts stay 0 in the PCSX2 debugger even when they fire.** Check PC or memory contents instead of trusting the counter.

**Exit test.** A recorded ground-truth number in `PS2_PROJECT_STATE.md` next to the recomp number.

---

## Plan D — Maintenance work, safe to pick up any time

These are genuinely useful and carry no risk of derailing an investigation. Good default when the active plan is blocked on a run you cannot execute.

- **Raise or remove saturated probe caps.** Four probes currently saturate every run: `iop:import` (6), `ARKD:run` (32), `ARKD:CALL` (64), `[gsreg] dispfb2` (64). Each one makes its own tag's later absence meaningless. Raising a cap is a one-line `.cpp` edit and directly buys back diagnostic resolution.
- **Fold findings into `PS2_PROJECT_STATE.md`.** Append to the newest session block. Record *patterns*, not events — `"X causes Y, fix with Z"`. This is what makes the next session cheaper, and it is routinely skipped.
- **Extend the tag census.** `launch_recomp.ps1` prints per-tag counts after each run. Adding derived checks there (e.g. "tag X fired but tag Y did not, which is contradictory") is cheap and catches misreadings early.
- **`ps2xTest` suites.** Fast iteration without a full game run:
  ```powershell
  & "F:\SDBZ Recomp\build.ps1" Debug -Test
  & "F:\SDBZ Recomp\build\ps2xTest\Debug\ps2x_tests.exe" Sif
  ```
  **Always pass a filter** — a bare invocation hangs and silently skips ~40 suites (BUG-031). Baseline is 27/28 passing; the single failure (BUG-032, `sceSifSetDma` multi-descriptor validation) is untriaged.

---

## Plan E — Do not start these

Listed explicitly because they look like reasonable next steps and are not.

- **`ps2xStudio`** — out of scope, permanently. "The debugger" means `RecompDebugger.exe` or PCSX2's own.
- **The "Attach to Recomp" live-data IPC** — broken, and the user deferred it. Do not resume unless they raise it.
- **A GIF packet recorder / offline replay tool** — proposed and explicitly declined.
- **Anything in the closed-threads table** in `copilot-instructions.md`.
- **`ps2_recomp` regeneration** — overwrites all ~30,000 runner files and forces a multi-hour rebuild. Ask first, always.

---

## 3. Weird choices to avoid

Concrete failure modes this project has actually hit. Each one cost real time.

- **Do not patch `fn_*.cpp` or anything under `src/runner/`.** It is machine output; the recompiler overwrites you. This is the project owner's standing rule and there are no exceptions.
- **Do not hand-edit `fn_forward_decls.h`.** It is regenerated from a regex scan of `game_overrides.cpp` on every build. Add the missing stub to `game_overrides.cpp` instead.
- **Do not fix a symptom you have not traced.** Ask "is the translation wrong, or is the environment incomplete?" — it is the environment ~95% of the time.
- **Do not act on an apparent contradiction between two probes without reading both emit sites.** The `tme=0` false lead burned a window: two probes read *different* variables (the PRIM register vs. the per-primitive value) and disagreeing was correct behaviour.
- **Do not conclude from a truncated paste.** If terminal output is cut off, go to `run_log.txt` for the full record.
- **Do not run `build.ps1` or `launch_recomp.ps1` yourself.** Hand the user the command. Same for `deepseek_report/draft/chat.ps1`.
- **Do not clean, and do not delete build artifacts.** The `build/` tree holds both arms of a Debug↔RelWithDebInfo control experiment. Ask before removing anything.
- **Do not write to the agent memory directory.** If you learn something durable, say so in your reply so the user can have it recorded.

## 4. How to write your response

The user is **dyslexic**. Short, bullet-heavy, low-density. Terse beats thorough. Lead with the finding, not the method. Batch requests — give every disassembly address at once rather than drip-feeding.
