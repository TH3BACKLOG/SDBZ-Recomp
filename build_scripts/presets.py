"""Shared EE field tables -- the ONE place a watch-field set is defined.

Why this file exists
--------------------
The same field set has to be spelled out in three places to answer the only
question that matters after a stall: "does hardware do this too?"

  1. build_scripts/pcsx2_sampler.py  -- reads them from a running PCSX2
  2. the runtime's PS2X_TRACE_WATCH  -- reads them from our own run
  3. build_scripts/differential.py   -- puts the two side by side

When those three lists were maintained separately, the comparison was a manual
chore, and *Reproduce On The Oracle Before Naming A Root Cause* got skipped --
which cost two root-cause headlines (parts 58 and 66). Defining the set once
and generating the other two forms from it is what makes the oracle check
cheap enough to actually run.

A preset entry is (name, address, width_bits). Names are the column headers on
both sides of the diff, so they must be identical -- that identity IS the join
key in differential.py.

Helpers:
    fields(preset)      -> [(name, addr, width), ...]
    watch_env(preset)   -> the PS2X_TRACE_WATCH string for our runtime
                           (32-bit fields only; the runtime sampler reads
                           words, so a narrower field would be reported wider
                           than it was declared here and silently disagree
                           with the oracle column)
"""

# Part-64's owed oracle measurement, field-for-field:
#   "does [0x441960] advance on hardware while [0x441924] == 1?"
# Names match the recomp's own [sofdec]/[thsync] samplers so the two outputs diff
# column-for-column.
#
# !! The part-64 reading key that used to live here was WRONG IN BOTH BRANCHES and
# !! has been deleted.  w6tick RISING is normal (hardware ~105/s); the recomp runs
# !! the same loop at ~186,000/s.  Rate, not motion, is the signal.
#
# !! RETRACTED as a ROOT CAUSE 2026-09-04 -- the observation below is real, the
# !! conclusion drawn from it was not. savepri does latch to 1/tid6, but it does
# !! so at t=143, and h44/h48 had already stopped moving at t~134. A suspect
# !! whose onset FOLLOWS the symptom's cannot be its cause. Kept as a measured
# !! window marker, not as an explanation. (analyze_run.py --onset now computes
# !! this ordering automatically, which is the whole reason it exists.)
#
# Reading key (part 65b, 09-04) -- the PRIORITY LATCH:
#   savepri is what CriLock (sub_11E598) stashed as the caller's ORIGINAL priority
#   at [0x449210], and savetid is that caller.  On hardware it tracks whoever holds
#   the lock and is always a REAL priority: observed 0x18 (24, tid 1) and 0x19
#   (25, tid 14).  In the recomp it reads 24/tid1 until the movie gate (g36 0->1),
#   then latches to 1/tid6 forever -- th6 pinned at the boost ceiling, main starved.
#   savepri == boost (1) is the bug, live.  nest is the lock's recursion count;
#   savepri==1 with nest==0 is a STALE saved value, not a lock stuck open.
#
# g36 is the window marker: at the main menu the CRI globals are all zero, so a
# sample with g36=0 and w6tick=0 says nothing.  Slice the CSV on g36 first.
PRESETS = {
    "sofdec": [
        ("w6tick", 0x441960, 32),   # per-iteration tick, sub_11EAC8 worker loop
        ("wbusy", 0x441924, 32),    # spinner's "worker busy"; cleared every iteration
        ("wdisp", 0x441934, 32),    # 1 while inside dispatch5(6)
        ("wexit", 0x4419D8, 32),    # nonzero => worker loop exits
        ("wtid", 0x44198C, 32),     # tid CreateThread'd by sub_11F0C8 (th6)
        ("savepri", 0x449210, 32),  # CriLock's saved "original" priority -- ==1 is the latch
        ("savetid", 0x449214, 32),  # tid that saved it
        # !! NAME COLLISION, resolved 2026-09-05. The hand-written [g36set]
        # !! probe printed a field it also called "nest", but that one reads
        # !! 0x45F670 (the SofDec streaming counter), NOT 0x441920. The two
        # !! disagree by design -- g36set reported nest=1 on all three calls
        # !! while this field reads 0 -- and reading that as a contradiction
        # !! would convict the tracer for being right. Both are watched now,
        # !! under names that cannot be confused.
        ("crinest", 0x441920, 32),  # CriLock recursion count
        ("svmnest", 0x45F670, 32),  # SofDec streaming nest; the [g36set] "nest"
        ("d5fn", 0x54EB10, 32),     # class-6 dispatch5 slot 0 fn (oracle: 0x154fa8)
        ("d5arg", 0x54EB18, 32),    # class-6 dispatch5 slot 0 arg (oracle: 0x4bd7d0)
        # Nested class dispatcher 0x13c448: tbl 0x54EBA0, fn=[tbl+cls*8],
        # arg=[tbl+cls*8+4]. cls=6 -> 0x54EBD0/0x54EBD4. This is the handler
        # 0x154950 tail-jumps to from INSIDE the g36 bracket (sub_155630),
        # so if g36 latches at 1 this is the call that never returned.
        ("c6fn", 0x54EBD0, 32),
        ("c6arg", 0x54EBD4, 32),
        ("g688", 0x45F688, 32),     # 0x154ff0's gate; nonzero => handler returns 0 early
        ("g674", 0x45F674, 32),     # 0x155210's gate; !=1 => handler returns 0 instantly
        ("tshook", 0x54EBF8, 32),   # 0x13c880's hook fn ptr; 0 => fallback test-and-set
        ("tsflag", 0x45F6D0, 32),   # the flag 0x13c880 test-and-sets (0x45F678+0x58)
        # 0x1651d8's 8-slot completion scan @0x461164. It returns 1 (=> handler
        # returns 0 => worker SLEEPS) only when all 8 slots report complete; any
        # incomplete slot returns 0 early and the worker spins.
        ("sl0", 0x461164, 32), ("sl1", 0x461168, 32),
        ("sl2", 0x46116C, 32), ("sl3", 0x461170, 32),
        ("sl4", 0x461174, 32), ("sl5", 0x461178, 32),
        ("sl6", 0x46117C, 32), ("sl7", 0x461180, 32),
        # The two fields both completion predicates read on slot 0's handle.
        # Handle has been 0x1B12CC0 in every sample on both sides (== `cur`).
        # 0x15b560:  h==0 or [h+72]==0  -> slot COMPLETE (skip)
        # 0x1651b0:  [h+72] in 1..4 AND [h+68] != 0 -> slot INCOMPLETE -> spin
        # RE-DERIVED 2026-09-05 from disasm of 0x1651b0 (was inherited):
        #   lw v1,72(a0); addiu v1,-1; sltiu v1,4; beq -> ret 1
        #   lw v0,68(a0); sltiu v0,1        -> ret (h44 == 0)
        # i.e. returns 0 (INCOMPLETE) iff h48 in 1..4 AND h44 != 0.
        # The inherited reading was correct; it is now verified.
        # h48 is a STATE, h44 is a "tick me" request flag. sub_165300 is
        # the pump: it bails unless h48 in 1..4 and h44 != 0, then CLEARS
        # h44 at 0x165338 and writes the next state back at 0x1653d4 via
        # the jump table at 0x4BF4F0 (state1 -> 0x165458, 2 -> 0x165488,
        # 3 -> 0x165820, 4 -> 0x1658c0, 5 -> 0x165930).
        # So h44 stuck at 1 is proof the pump NEVER RAN, not that it is slow.
        # Oracle 09-04 with the worker ASLEEP: h44=1, h48=0.
        ("h40", 0x1B12D00, 32), ("h44", 0x1B12D04, 32),
        ("h48", 0x1B12D08, 32), ("h4c", 0x1B12D0C, 32),
        ("h92", 0x1B12D1C, 32),     # [h+92]: bracket-C mirror, written at 0x1555c0
                                    # only when set_g36's $a0 != 0 -> names the bracket
        ("d6n", 0x0045EFE0, 32),    # run_class(6) dispatch counter, bumped at 0x13c5b0
        ("d6in", 0x0045F000, 32),   # run_class(6) in-callback flag (0x13c55c/0x13c568)
        ("d5n", 0x0045EFDC, 32),    # same counter for class 5 (thunk 0x13c6d0)
        # !! RETRACTED 2026-09-04 (part 68). The d6n-vs-w6tick ratio key that used
        # !! to live here was INVALID and its result tested nothing. It assumed the
        # !! nested run_class(6) reached via 0x154950 would bump [0x45EFC8+cls*4].
        # !! Disassembly of 0x13c448 shows that dispatcher is a bare single-slot
        # !! table walk (tbl 0x54EBA0, fnp=[tbl+cls*8]) that bumps NO counter -- the
        # !! bump lives at 0x13c5b0, inside the OTHER dispatcher 0x13c4f8. So
        # !! d6n ~= w6tick holds whether or not the bracket is entered, and the
        # !! measured 1:1 discriminated nothing.
        # !! Keep the columns: d6n is still the class-6 dispatch count via 0x13c4f8.
        # !! What is gone is the inference from its ratio.
        # !! The surviving evidence that g36 is latched is the part-63 HWWATCH run
        # !! (last g36=1 store, no matching clear for the rest of the run) plus the
        # !! part-68 [trace] result: three calls to 0x1555a0, all from bracket C
        # !! (ra 0x155650/0x155664), the third opening and never closing.
        # h4c is the COMMAND word read by the state-1 handler 0x165458:
        #   state 1 advances to 2 only when h4c is in {2,3,4,6}.
        # Catch this DURING the movie window -- once h48 reaches 0 the
        # handle is terminal and h4c has already been consumed (reads 0).
        ("boost", 0x4418F0, 32),    # priority-boost cell from the [thsync] probe
        ("g36", 0x45F69C, 32),      # guard-3 gate; 0->1 marks the stall window
        ("done", 0x460F04, 32),     # SofDec done flag (get_data_ptr()+6284)
        ("cur", 0x460F58, 32),      # last handle sif_is_bound saw
        # --- part 84 (09-06): the h44 PUMP PATH, decoded on PCSX2 in-phase.
        # 0x155210 body -> for i in 0..7: 0x155320(obj_i) where
        # obj_i = 0x45F6E4 + i*0x304 (only obj0 is live; obj1..7 are all-zero).
        # 0x155320 gates, in order, then tail-jumps 0x1553D8 -> 0x165250 -> 0x165300:
        #   0x155358  g674  == 1        (watched above)
        #   0x155384  [obj+0x00] == 1   -> o0st
        #   0x155394  [obj+0x60] != 1   -> o0bsy  (re-entrancy; set 1 for the
        #                                  duration of 0x1553D8 via 0x155518)
        #   0x1553a4  g36 != 1          <- THE g36 BAIL, found at last
        #   0x165250  0x15B560([obj+0x3C]) == 0 -> o0slt must be a live slot
        # then 0x165338 does `sw zero,0x44(s1)` -- the ONLY h44 clear.
        # Oracle in-phase: o0st=1, o0bsy=0 (1 only while servicing), o0slt=0x1B12CC0.
        ("o0st", 0x45F6E4, 32),
        ("o0bsy", 0x45F744, 32),
        ("o0slt", 0x45F720, 32),
        ("gamemode", 0x5E6B3C, 8),  # 0x00 = MainMenu; width unverified
    ],
}


def fields(preset):
    """Field list for a preset name. KeyError names the valid presets."""
    if preset not in PRESETS:
        raise KeyError("unknown preset %r; have: %s"
                       % (preset, ", ".join(sorted(PRESETS))))
    return PRESETS[preset]


def watch_env(preset):
    """PS2X_TRACE_WATCH string for a preset.

    Drops non-32-bit fields rather than truncating them: the runtime's WATCH
    sampler always reads a word, so emitting an 8-bit field here would produce
    a column that disagrees with the oracle for reasons that have nothing to
    do with the guest. Dropping it is visible in the diff as a missing field;
    silently widening it is not.
    """
    return ",".join("%s=0x%X" % (name, addr)
                    for name, addr, width in fields(preset) if width == 32)


if __name__ == "__main__":
    import sys
    name = sys.argv[1] if len(sys.argv) > 1 else "sofdec"
    print(watch_env(name))
