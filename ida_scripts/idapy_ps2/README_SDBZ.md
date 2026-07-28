# IDAPy-PS2 scripts (vendored for SDBZ Recomp)

Source: https://github.com/grimdoomer/IDAPy-PS2 (grimdoomer, master, fetched 2026-07-24)

IDA Python scripts to auto-label PS2 binaries. Run via **File → Script file** in IDA 7.0+
on an already-loaded binary (they do NOT load binaries or add arch support).

## Scripts

| Script | Use on | What it does |
|--------|--------|--------------|
| `LabelIOPImports.py` | an **IOP module** (.IRX) | Names import/export tables from `IOP/<module>.json` (ps2sdk-derived). **Our main win** — automates the manual IRX-labeling backlog (5 modules left). |
| `LabelExecutableSyscalls.py` | the **EE game ELF** (SLUS_214.42) | Names EE usermode syscall stubs. |
| `LabelKernelSyscalls.py` | an **EE kernel image** at 0x80000000 | Names kernel syscall table. (We don't have a kernel image loaded — least relevant.) |

Depends on `Ps2Kernel.py` (bundled) and the `IOP/*.json` export tables (bundled, 46 modules
incl. sifcmd/sifman/libsd/mcman/mcserv/padman/sio2man — the ones SDBZ actually uses).

## Local changes
- `LabelIOPImports.py`: added missing `import os` (upstream references `os.path` without importing it).

## Relevance
- IRX-labeling backlog only ([[project_irx_labeling_workflow]]). Does NOT touch the live
  SIF-RPC `$ra=0x1` boot blocker.
- `IOP/sifcmd.json` has the real SIF-RPC export names (sceSifBindRpc/CallRpc/RegisterRpc) —
  handy cross-reference even when reading decompiles by hand.
