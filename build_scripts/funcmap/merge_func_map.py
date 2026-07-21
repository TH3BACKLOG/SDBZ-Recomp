"""Merge IDA function boundaries with the recovered Ghidra names.

The lost Ghidra export cannot be regenerated -- but neither half of it is
actually lost, they are just in two places:

  boundaries (start/end/size) : SLUS_214.42.i64, IDA's own analysis.
                                ORIGINAL, not derived.
  names                       : ps2xRuntime/symbols.map, which
                                generate_symbols_map.ps1 built from the
                                runner/ filenames -- and those filenames were
                                produced BY the lost Ghidra export. So the
                                names are the original ones, one generation
                                removed.

Validated before merging: all 12,072 real names in symbols.map land EXACTLY on
an IDA function start, 0 mismatches. IDA and the lost export agree on every
function start they have in common, so joining them on address is sound rather
than a guess.

Precedence: symbols.map name wins where present (it is the original label);
IDA's name otherwise. IDA-only functions keep their sub_XXXXXX name -- those are
functions the Ghidra export either never named or never had.

Read-only inputs. Writes one CSV to the scratch dir.
"""
import csv
import os

IDA_CSV = os.path.join(os.getcwd(), "sdbz_func_map.csv")
SYMMAP = r"F:\SDBZ Recomp\ps2xRuntime\symbols.map"
OUT = os.path.join(os.getcwd(), "sdbz_func_map_merged.csv")

# --- IDA boundaries -------------------------------------------------------
funcs = []
with open(IDA_CSV) as f:
    r = csv.reader(f)
    next(r)
    for name, start, end, size in r:
        funcs.append([name, int(start, 16), int(end, 16), int(size, 16)])

# --- recovered names ------------------------------------------------------
names = {}
with open(SYMMAP) as f:
    for line in f:
        parts = line.split()
        if len(parts) != 2:
            continue
        addr, nm = parts
        if nm.startswith("sub_") or nm.startswith("fn_"):
            continue
        names.setdefault(int(addr, 16), nm)

renamed = 0
for fn in funcs:
    nm = names.get(fn[1])
    if nm and nm != fn[0]:
        fn[0] = nm
        renamed += 1

funcs.sort(key=lambda r: r[1])

with open(OUT, "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["name", "start", "end", "size"])
    for name, s, e, z in funcs:
        w.writerow([name, f"0x{s:08x}", f"0x{e:08x}", f"0x{z:x}"])

real = sum(1 for fn in funcs if not fn[0].startswith("sub_"))
print(f"wrote {OUT}")
print(f"  functions       : {len(funcs)}")
print(f"  named from map  : {renamed}")
print(f"  real names total: {real}")
print(f"  still sub_      : {len(funcs) - real}")
print()
for fn in funcs[:3]:
    print(f"  {fn[0]},0x{fn[1]:08x},0x{fn[2]:08x},0x{fn[3]:x}")
print("  ...")
for probe in (0x11F268, 0x284F10):
    hit = [fn for fn in funcs if fn[1] == probe]
    if hit:
        fn = hit[0]
        print(f"  {fn[0]},0x{fn[1]:08x},0x{fn[2]:08x},0x{fn[3]:x}")
