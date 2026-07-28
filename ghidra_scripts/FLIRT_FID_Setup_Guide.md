# PS2 FLIRT / FID Setup Guide
*Auto-identifying SDK functions in Ghidra for Super Dragon Ball Z*

---

## What Is This?

Ghidra's **Function ID (FID)** system is its equivalent of IDA Pro's FLIRT signatures. It works by fingerprinting known library functions (from the PS2 SDK, libc, math libs, etc.) and automatically matching them against your binary. When a match is found, Ghidra renames the `FUN_` label to the real function name — things like `memcpy`, `sqrtf`, `sprintf`, `rand`, and so on.

A single FID pass can identify dozens to hundreds of standard functions instantly, clearing a lot of noise before you start manual analysis.

---

## Option A — Use a Pre-Built FID Database (Fastest)

Community-built `.fidb` files exist for the PS2 EE (Emotion Engine) SDK. Search for these on GitHub:

- **`ps2 ghidra fidb`**
- **`ps2sdk ghidra fid`**
- **`ghidra ps2 ee signatures`**

Good starting points:
- `lab313ru/ghidra_ps2_ee_lang` — PS2 EE processor language plugin for Ghidra (install this first if Ghidra doesn't recognize PS2 EE binaries)
- PS2 decomp community Discord servers often share `.fidb` files directly

### Applying a `.fidb` File in Ghidra

1. Open your project in Ghidra and open the game binary.
2. Go to **Window → Function ID**
3. Click **"Attach existing FidDb..."**
4. Select the `.fidb` file you downloaded.
5. Go to **Tools → Function ID → Search current program**
6. Check **"Search all functions"** and click OK.
7. Ghidra will rename any matched functions automatically.

---

## Option B — Build Your Own FID Database from PS2 SDK

If you want to build signatures from the official open-source PS2 SDK yourself (more complete, more control):

### Prerequisites
- Ghidra installed
- PS2 toolchain installed (`ps2dev` / `ps2sdk`) — see https://github.com/ps2dev/ps2dev
- A Linux or WSL environment (easiest for ps2dev setup)

### Step 1 — Compile the PS2 SDK libraries

```bash
git clone https://github.com/ps2dev/ps2dev
cd ps2dev
./build-all.sh
```

This produces compiled `.a` static library files in `ps2sdk/`.

### Step 2 — Extract the `.a` files

The `.a` files contain the compiled object files Ghidra needs to fingerprint. Key libraries to target:

```
ps2sdk/ee/lib/libkern.a      # kernel / system calls
ps2sdk/ee/lib/libmath.a      # math functions (sin, cos, sqrt, etc.)
ps2sdk/ee/lib/libpad.a       # controller input
ps2sdk/ee/lib/libgraph.a     # graphics
ps2sdk/ee/lib/libdma.a       # DMA
ps2sdk/ee/lib/libc.a         # standard C library
```

### Step 3 — Create a Ghidra FID Database

1. In Ghidra, open a **CodeBrowser** on any PS2 binary (can be a dummy one).
2. Go to **Tools → Function ID → Create new empty FidDb...**
3. Name it `ps2sdk.fidb` and save it.
4. Go to **Tools → Function ID → Populate FidDb from programs...**
5. Point it at the compiled `.a` files from Step 2.
6. Let it process — this may take a few minutes.

### Step 4 — Apply to Your Game Binary

Follow the same steps as Option A above to attach and search with your new `.fidb`.

---

## Option C — Import Symbols from PCSX2

If you've been using PCSX2's debugger and have identified function addresses manually, you can export them and import them into Ghidra using the `BatchRename.py` script.

### Export from PCSX2
1. In PCSX2, open **Debug → Symbol Map**
2. Export the symbol list (or manually copy addresses + names)
3. Format as CSV matching the template: `address,name,comment`

### Import into Ghidra
See the `BatchRename.py` script and `rename_template.csv` in this folder.

---

## Recommended Order of Operations

1. **Install `ghidra_ps2_ee_lang`** — ensures Ghidra correctly disassembles PS2 MIPS (EE) binaries.
2. **Apply FID database** — auto-identifies all standard SDK/libc functions.
3. **Run BatchRename.py with your PCSX2 symbol log** — applies any addresses you've found at runtime.
4. **Manual analysis** — now you're only naming the game-specific functions, not the standard library ones.

---

## Tips

- Run FID **before** you start manually renaming. It's much easier to spot named library functions in the call graph when starting fresh.
- If FID matches look wrong (false positives), check **Window → Function ID → Show matches** and reject bad ones.
- After applying FID, re-run Ghidra's **Auto Analysis** — knowing more function names helps it resolve cross-references better.
- Keep a running CSV of every address you identify in PCSX2. Run `BatchRename.py` periodically to push them all into Ghidra at once rather than renaming one by one.

---

*Last updated: April 2026 | Super Dragon Ball Z PS2 Recomp*
