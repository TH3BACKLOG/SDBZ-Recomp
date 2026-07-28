# BatchRename.py
# Ghidra script — batch rename functions and labels from a CSV file.
#
# CSV format (first row is a header and will be skipped automatically):
#
#   address,name,comment
#   0x001a3c40,battle_player_update_state,updates player state each frame
#   0x0023ff10,char_goku_kamehameha_startup
#   001a3c40,battle_player_update_state   <- 0x prefix is optional
#
# Columns:
#   address  (required) - hex address of the function or label
#   name     (required) - new name to apply
#   comment  (optional) - plate comment to attach above the function
#
# Behavior:
#   - If a function exists at the address, it is renamed.
#   - If no function exists, a named label is created at that address.
#   - If a comment column is present, it is set as a plate comment.
#   - Prints a summary and popup when done.
#
# Installation:
#   Copy this file to: <Ghidra>/Ghidra/Features/Base/ghidra_scripts/
#   Then in Ghidra: Window > Script Manager > find BatchRename > Run
#
# @category PS2-Recomp
# @author SDBZ Recomp

import csv
from ghidra.program.model.symbol import SourceType


def parse_address(addr_str):
    """Parse a hex address string with or without 0x prefix."""
    clean = addr_str.strip().lower().replace('0x', '')
    return toAddr(int(clean, 16))


def is_header_row(row):
    """Return True if this row looks like a header (non-hex first column)."""
    if not row:
        return False
    first = row[0].strip().lower()
    return first in ('address', 'addr', 'offset')


def run():
    csv_file = askFile("Select rename CSV", "Open")
    if csv_file is None:
        popup("No file selected. Aborting.")
        return

    renamed = 0
    labeled = 0
    skipped = 0
    error_log = []

    with open(csv_file.absolutePath, 'r') as f:
        reader = csv.reader(f)
        rows = list(reader)

    if not rows:
        popup("CSV file is empty.")
        return

    # Skip header row if present
    start = 1 if is_header_row(rows[0]) else 0

    for i, row in enumerate(rows[start:], start=start + 1):
        # Skip blank or short rows
        if not row or len(row) < 2:
            skipped += 1
            continue

        addr_str = row[0].strip()
        name = row[1].strip()
        comment = row[2].strip() if len(row) > 2 else ''

        if not addr_str or not name:
            skipped += 1
            continue

        try:
            addr = parse_address(addr_str)
            func = getFunctionAt(addr)

            if func is not None:
                old_name = func.getName()
                func.setName(name, SourceType.USER_DEFINED)
                if comment:
                    setPlateComment(addr, comment)
                print("[RENAMED]  row {:>4} | {} | {} -> {}".format(i, addr_str, old_name, name))
                renamed += 1
            else:
                createLabel(addr, name, True, SourceType.USER_DEFINED)
                if comment:
                    setPlateComment(addr, comment)
                print("[LABEL]    row {:>4} | {} | {}".format(i, addr_str, name))
                labeled += 1

        except Exception as e:
            msg = "row {:>4} | {} | ERROR: {}".format(i, addr_str, str(e))
            error_log.append(msg)
            print("[ERROR]    " + msg)

    # --- Summary ---
    summary = (
        "=== Batch Rename Complete ===\n"
        "Functions renamed : {}\n"
        "Labels created    : {}\n"
        "Rows skipped      : {}\n"
        "Errors            : {}"
    ).format(renamed, labeled, skipped, len(error_log))

    if error_log:
        summary += "\n\nErrors:\n" + "\n".join(error_log)

    print("\n" + summary)
    popup(summary)
