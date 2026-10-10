#!/usr/bin/env python3
r"""Write a "100% unlocked" SDBZ save into the recomp memory card (ELF\mc0\BASLUS-21442SDBZ).

Save format (VERIFIED from the game's packer 0x32E1D0 + saved files, MD5 checks pass):
  file = md5(payload)[16] + u32 payload_len + payload + zero pad   (sys/BASLUS-21442SDBZ = 3072 B,
  char/char_bk = 5120 B).  sys payload = 2040 B, char payload = 4924 B = u32 hdr + 30 cards x 164 B.

What this sets (all read from the game's own unlock code, see memory project_unlock_flags):
  sys +1996  fighter flags  |= 0xFFF0   (all 18 fighters; 5 wish pairs + the Original-clear pair)
  sys +4     wish word      |= 0x7FE    (title code, wish-met bits, 5 narration voices,
                                         Customize wallpaper, King Kai's Planet stage)
  sys +8..19 the 12 wish states = 3 (what the community 100% save PCSX2\memcards\gpbear4.ps2 holds),
  sys +20/+21 = 5 / 20 (0x5EC300, summon count; same save)
Cards (Customize mode), default = --variants: one card per distinct skill-tree BRANCH VARIATION found in the
real 100% saves (SDBZ.psu + gpbear4.ps2: 60 cards -> 34 distinct tree paths over the 18 fighters), at most 30
(every fighter gets its first variant, the rest are filled most-variants-first). Names are
<FIGHTER>-A, -B, ... and each card gets a random selected colour (+9, 0..4, the range seen in real saves),
reproducible with --seed. --donor-cards = the older mode: copy the PSU's 30 cards unchanged.
Cards (old mode): all 30 card slots = the donor save's cards (30 cards, all 18 fighters,
colour mask +73 = 0x7F = all 7 colours, skill-tree data filled in). --merge keeps existing cards and
only fills empty slots; --no-cards leaves cards alone.

--all-variants: EVERY branch variation of every fighter (941 cards = 32 sets of 30; the game's file holds 30
cards, so the full list lives in Logs\save_sets\setNN). Live: a round-robin sampler of 60 cards, 30 in slot 1
(ELF\mc0) + 30 in slot 2 (ELF\mc1; the game's Save/Load menu picks the slot). --set N installs set N (+N+1
into slot 2) without rebuilding. Every real list is verified to be a member of the enumerated tree space.

NOT touched: options bytes, rankings/records, per-card EXP/skill bits (format not fully decoded).

  python build_scripts\save\make_100pct_save.py            # apply (backs up first)
  python build_scripts\save\make_100pct_save.py --dry-run  # show what would change
"""
import argparse, hashlib, os, shutil, struct, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SYS_LEN, CHAR_LEN, CARD, NCARD = 2040, 4924, 164, 30
def _empty_card():
    """what the game stores in an unused slot (seen in real saves; template ELF 0x4938A0)."""
    c = bytearray(CARD)
    c[0] = 3
    c[27] = 0xFF
    c[30] = 1
    c[43] = 1
    c[85:99] = bytes.fromhex('00040400040400000004000404' + '04')[:14]
    c[158] = 3
    return bytes(c)


TEMPLATE_EMPTY = _empty_card()


def read_pack(path, want_len):
    raw = open(path, 'rb').read()
    n = struct.unpack_from('<I', raw, 16)[0]
    pay = raw[20:20 + n]
    if n != want_len or hashlib.md5(pay).digest() != raw[:16]:
        sys.exit('%s: bad length/md5 (len=%d)' % (path, n))
    return raw, bytearray(pay)


def build(raw, pay):
    out = bytearray(raw)
    out[:16] = hashlib.md5(bytes(pay)).digest()
    out[16:20] = struct.pack('<I', len(pay))
    out[20:20 + len(pay)] = pay
    return bytes(out)


def psu_files(path):
    d = open(path, 'rb').read()
    off, files = 0, {}
    while off + 512 <= len(d):
        flags, _, size = struct.unpack_from('<HHI', d, off)
        name = d[off + 0x40:off + 0x60].split(b'\0')[0].decode('latin1')
        off += 512
        if flags & 0x20:
            continue
        files[name] = d[off:off + size]
        off += size + ((1024 - size % 1024) % 1024)
    return files


def atomic_write(path, data):
    tmp = path + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(data)
    os.replace(tmp, path)


FIGHTER = {1: 'GOKU', 2: 'VEGETA', 3: 'PICCOLO', 4: 'KRILLIN', 5: 'GOHAN', 9: 'FRIEZA', 10: 'CHI-CHI',
           11: 'CYBORG FRIEZA', 12: 'TRUNKS', 22: 'CELL', 23: 'ANDROID16', 24: 'ANDROID17',
           25: 'ANDROID18', 26: 'MAJIN BUU', 27: 'ULTIMATE GOHAN', 28: 'VIDEL', 29: 'MAJIN VEGETA',
           30: 'KING PICCOLO'}


def cards_of(payload):
    out = []
    for i in range(NCARD):
        c = bytes(payload[4 + i * CARD:4 + (i + 1) * CARD])
        if c[4] & 4:
            out.append(c)
    return out


def gp4_char(path):
    """first valid char payload inside a PS2 memory-card image (ECC stripped when present)."""
    d = open(path, 'rb').read()
    if len(d) == 528 * 16384:
        d = b''.join(d[i:i + 512] for i in range(0, len(d), 528))
    for off in range(16, len(d) - 5000, 4):
        if struct.unpack_from('<I', d, off)[0] == CHAR_LEN:
            pay = d[off + 4:off + 4 + CHAR_LEN]
            if hashlib.md5(pay).digest() == d[off - 16:off]:
                return bytearray(pay)
    sys.exit('no char save found in ' + path)


def build_variants(pools, seed):
    import random
    rng = random.Random(seed)
    var = {}                                   # fighter id -> {tree-path key: card}
    for c in pools:
        fid = c[27]
        if fid not in FIGHTER:
            continue
        key = bytes(c[99:100 + c[99]])         # [row count, per-row branch bytes]
        var.setdefault(fid, {}).setdefault(key, c)
    order = sorted(var)
    chosen = [(fid, sorted(var[fid])[0]) for fid in order]            # every fighter once
    extras = [(fid, k) for fid in order for k in sorted(var[fid])[1:]]
    extras.sort(key=lambda t: (len(var[t[0]]) - 1) * -1)              # most-variant fighters first, stable
    left = NCARD - len(chosen)
    chosen += extras[:left]
    dropped = extras[left:]
    cards = []
    per = {}
    for fid, key in sorted(chosen, key=lambda t: (t[0], t[1])):
        c = bytearray(var[fid][key])
        n = per.get(fid, 0); per[fid] = n + 1
        name = ('%s-%s' % (FIGHTER[fid], chr(ord('A') + n))).encode('ascii')[:16]
        c[10:27] = name + b'\0' * (17 - len(name))
        c[73] = 0x7F
        c[30] = 7            # Dragon Balls
        cards.append(c)
    # random selected colour; spread inside one fighter where possible
    byf = {}
    for c in cards:
        byf.setdefault(c[27], []).append(c)
    for fid, cs in byf.items():
        pick = rng.sample(range(5), min(len(cs), 5))
        for c, col in zip(cs, pick):
            c[9] = col
        for c in cs[5:]:
            c[9] = rng.randrange(5)
    return cards, dropped, var


# ---------------------------------------------------------------------------------------------
# --all-variants: every branch variation of every fighter's skill tree, from the game's own table.
#
# VERIFIED (static, decompile of 0x2C6DE0 / 0x368B60 + the 34 real tree lists in SDBZ.psu and
# gpbear4.ps2): per fighter a 128-byte block at 0x491D50 + 0x80*k (k = index of the fighter id in
# FIGHTER_IDS; switch 0x2C6990). byte 14 = tier-row count, rows are 16 bytes at 15 + 16*r (r >= 1):
#   +0 node0 id (0xFF = none), +1 node0 flag, +2 node1 id, +3 node1 flag,
#   +4/+8/+12 three 4-byte column cells (count, skill, kind, state); count 0 = column not offered.
# The card's list at +99 is [n, entry...] with one entry per existing node in (row, j) order.
# A FORK node has flag 0x08: its entry is (column | 8), column in {1, 2, 4} = cell 0/1/2, and it can only
# take columns whose cell count is non-zero. Every plain node's entry is the column of the nearest
# fork above it. Node ids 0x15 / 0x17 are the "rare skill" nodes: treated as plain here (the real
# saves carry 0x28 flags / free columns there; those are not reproduced).
# The oracle check below proves every real list is one member of the enumerated space.
# ---------------------------------------------------------------------------------------------
FIGHTER_IDS = [1, 2, 3, 4, 5, 9, 10, 11, 12, 22, 23, 24, 25, 26, 27, 28, 29, 30]
TREE_BASE = 0x491D50
SPECIAL_NODE_IDS = (0x15, 0x17)
SHORT_NAME = {'CYBORG FRIEZA': 'CYBORG FRIEZA', 'ULTIMATE GOHAN': 'ULT GOHAN', 'KING PICCOLO': 'KING PICCOLO',
              'MAJIN VEGETA': 'MAJIN VEGETA'}
LEARN_FIELDS = ((54, 1), (55, 1), (56, 4), (60, 2), (64, 4), (68, 4))   # card offset, byte length
_ELF = None


def elf_bytes(addr, n):
    global _ELF
    if _ELF is None:
        d = open(os.path.join(ROOT, 'ELF', 'SLUS_214.42'), 'rb').read()
        phoff = struct.unpack_from('<I', d, 0x1C)[0]
        phentsize, phnum = struct.unpack_from('<HH', d, 0x2A)
        segs = []
        for i in range(phnum):
            t, off, va, _pa, fs, _ms, _fl, _al = struct.unpack_from('<8I', d, phoff + i * phentsize)
            if t == 1:
                segs.append((va, off, fs))
        _ELF = (d, segs)
    d, segs = _ELF
    for va, off, fs in segs:
        if va <= addr < va + fs:
            return d[off + addr - va: off + addr - va + n]
    sys.exit('address %#x not in the ELF image' % addr)


def tree_nodes(fid):
    r = elf_bytes(TREE_BASE + 0x80 * FIGHTER_IDS.index(fid), 0x80)
    out = []
    for row in range(1, r[14]):
        base = 15 + 16 * row
        opts = [c for c in range(3) if base + 4 + 4 * c < 128 and r[base + 4 + 4 * c] != 0]
        for j in range(2):
            if base + 2 * j + 1 >= 128 or r[base + 2 * j] == 0xFF:
                continue
            nid = r[base + 2 * j]
            out.append({'row': row, 'j': j, 'id': nid, 'opts': opts,
                        'fork': r[base + 2 * j + 1] == 8 and nid not in SPECIAL_NODE_IDS,
                        'special': nid in SPECIAL_NODE_IDS})
    return out


COLBIT = (1, 2, 4)


def variant_lists(fid):
    """every list [n, entries...] of the fighter's tree, as (choice tuple, bytes)."""
    import itertools
    nodes = tree_nodes(fid)
    forks = [n for n in nodes if n['fork']]
    for combo in itertools.product(*[n['opts'] for n in forks]):
        ent, prev, k = [], 1, 0
        for n in nodes:
            if n['fork']:
                prev = COLBIT[combo[k]]
                k += 1
                ent.append(prev | 8)
            else:
                ent.append(prev)
        yield combo, bytes([len(nodes)] + ent)


def list_choice(fid, lst):
    """fork column choices (tuple of 0..2) of a real card list; None if it does not fit the tree."""
    nodes = tree_nodes(fid)
    ent = lst[1:1 + lst[0]]
    if len(ent) != len(nodes):
        return None
    out = []
    for n, e in zip(nodes, ent):
        if n['fork']:
            if not (e & 8) or (e & 7) not in COLBIT or COLBIT.index(e & 7) not in n['opts']:
                return None
            out.append(COLBIT.index(e & 7))
    return tuple(out)


def suffix(i):
    s = ''
    i += 1
    while i:
        i, r = divmod(i - 1, 26)
        s = chr(65 + r) + s
    return s


def build_all_variants(pools, seed):
    """-> (cards list in fighter order, report lines). Learned flags come from the nearest real card."""
    import random
    rng = random.Random(seed)
    cards, rep = [], []
    for fid in FIGHTER_IDS:
        real = [c for c in pools if c[27] == fid]
        if not real:
            rep.append('%-15s no real card to copy from - skipped' % FIGHTER[fid])
            continue
        # oracle: every real list must be a member of the enumerated space
        space = {combo for combo, _ in variant_lists(fid)}
        bad = [bytes(c[99:100 + c[99]]).hex(' ') for c in real if list_choice(fid, bytes(c[99:100 + c[99]])) not in space]
        if bad:
            sys.exit('oracle check failed for %s: %d real lists not in the enumerated tree space, e.g. %s'
                     % (FIGHTER[fid], len(bad), bad[0]))
        refs = [(list_choice(fid, bytes(c[99:100 + c[99]])), c) for c in real]
        base_name = SHORT_NAME.get(FIGHTER[fid], FIGHTER[fid])
        n = 0
        for combo, lst in variant_lists(fid):
            # nearest real card (fewest differing forks) donates every non-tree byte
            dist, ref = min(((sum(a != b for a, b in zip(combo, rc)), c) for rc, c in refs), key=lambda t: t[0])
            c = bytearray(ref)
            c[99:139] = lst + bytes(40 - len(lst))
            name = ('%s-%s' % (base_name, suffix(n))).encode('ascii')
            c[10:27] = name + b'\0' * (17 - len(name))
            c[73] = 0x7F
            c[30] = 7        # Dragon Balls (7 on 48/60 real cards): every card can call Shenron
            c[9] = rng.randrange(5)
            cards.append(bytes(c))
            n += 1
        rep.append('%-15s %4d variations (%d real paths verified)' % (FIGHTER[fid], n, len({r for r, _ in refs})))
    return cards, rep


def pack_cards(base_payload, cards):
    pay = bytearray(base_payload[:4]) + bytearray(NCARD * CARD)
    for i in range(NCARD):
        pay[4 + i * CARD:4 + (i + 1) * CARD] = cards[i] if i < len(cards) else TEMPLATE_EMPTY
    return pay


def set_dir():
    return os.path.join(ROOT, 'Logs', 'save_sets')


def install_card_file(mcdir, craw, payload, label):
    cnew = build(craw, payload)
    atomic_write(os.path.join(mcdir, 'char'), cnew)
    atomic_write(os.path.join(mcdir, 'char_bk'), cnew)
    read_pack(os.path.join(mcdir, 'char'), CHAR_LEN)
    print('installed %s -> %s (md5 ok)' % (label, mcdir))


def ensure_mc1(mc0):
    """slot 2 = sibling folder mc1 (Kernel/Stubs/MemoryCard.cpp maps port 1 there)."""
    mc1 = os.path.join(os.path.dirname(os.path.dirname(mc0)), 'mc1', os.path.basename(mc0))
    os.makedirs(mc1, exist_ok=True)
    for n in ('sys', 'BASLUS-21442SDBZ', 'icon.sys', 'view.ico', 'char', 'char_bk'):
        src, dst = os.path.join(mc0, n), os.path.join(mc1, n)
        if os.path.exists(src) and not os.path.exists(dst):
            shutil.copy2(src, dst)
    return mc1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mc', default=os.path.join(ROOT, 'ELF', 'mc0', 'BASLUS-21442SDBZ'))
    ap.add_argument('--donor', default=os.path.join(ROOT, 'PCSX2', 'memcards', 'SDBZ.psu'))
    ap.add_argument('--no-cards', action='store_true')
    ap.add_argument('--donor-cards', action='store_true', help='copy the PSU cards unchanged instead of building variants')
    ap.add_argument('--seed', type=int, default=1009)
    ap.add_argument('--gp4', default=os.path.join(ROOT, 'PCSX2', 'memcards', 'gpbear4.ps2'))
    ap.add_argument('--merge', action='store_true', help='keep existing cards, fill only empty slots')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--all-variants', action='store_true',
                    help='one card for EVERY branch variation of every fighter (from the game tree tables); '
                         'cards go into 30-card sets in Logs\\save_sets, set 1 -> mc0, set 2 -> mc1')
    ap.add_argument('--set', type=int, default=0,
                    help='install saved set N into mc0 and set N+1 into mc1 (no rebuild, cards only)')
    a = ap.parse_args()
    second_set = None

    if not a.dry_run:
        t = subprocess.run(['tasklist'], capture_output=True, text=True).stdout.lower()
        if 'ps2entryrunner' in t:
            sys.exit('close the game first (it re-saves over this card on exit)')

    sraw, sp = read_pack(os.path.join(a.mc, 'sys'), SYS_LEN)
    craw, cp = read_pack(os.path.join(a.mc, 'char'), CHAR_LEN)
    if a.set:
        names = sorted(n for n in os.listdir(set_dir())) if os.path.isdir(set_dir()) else []
        want = ['set%02d' % a.set] + (['set%02d' % (a.set + 1)] if 'set%02d' % (a.set + 1) in names else [])
        if want[0] not in names:
            sys.exit('no %s in %s (run --all-variants first; have %s)' % (want[0], set_dir(), names))
        if a.dry_run:
            print('dry run - would install', want)
            return
        stamp = time.strftime('%Y%m%d-%H%M%S')
        bdir = os.path.join(ROOT, 'Logs', 'save_backups', stamp)
        os.makedirs(bdir)
        for n in ('char', 'char_bk'):
            shutil.copy2(os.path.join(a.mc, n), bdir)
        for k, s in enumerate(want):
            _, spay = read_pack(os.path.join(set_dir(), s, 'char'), CHAR_LEN)
            target = a.mc if k == 0 else ensure_mc1(a.mc)
            install_card_file(target, craw, spay, s)
        return
    donor = psu_files(a.donor)
    _, dsp = read_pack_bytes(donor['sys'], SYS_LEN)
    _, dcp = read_pack_bytes(donor['char'], CHAR_LEN)

    # --- sys
    old_fl = struct.unpack_from('<I', sp, 1996)[0]
    old_wd = struct.unpack_from('<I', sp, 4)[0]
    struct.pack_into('<I', sp, 1996, old_fl | 0xFFF0)
    struct.pack_into('<I', sp, 4, old_wd | 0x7FE)
    sp[8:20] = b'' * 12
    sp[20] = 5
    sp[21] = 20
    print('fighter flags %#x -> %#x' % (old_fl, old_fl | 0xFFF0))
    print('wish word     %#x -> %#x' % (old_wd, old_wd | 0x7FE))
    print('wish states   ->', bytes(sp[8:24]).hex(' '))

    # --- cards
    if not a.no_cards:
        if a.donor_cards:
            empty = list(range(NCARD)) if not a.merge else [i for i in range(NCARD) if not (cp[4 + i * CARD + 4] & 4)]
            src = [i for i in range(NCARD) if dcp[4 + i * CARD + 4] & 4]
            n = 0
            for slot, si in zip(empty, src):
                cp[4 + slot * CARD:4 + (slot + 1) * CARD] = dcp[4 + si * CARD:4 + (si + 1) * CARD]
                cp[4 + slot * CARD + 73] = 0x7F
                n += 1
            print('cards (donor copy): written %d' % n)
        elif a.all_variants:
            pools = cards_of(dcp) + cards_of(gp4_char(a.gp4))
            allc, rep = build_all_variants(pools, a.seed)
            print('\n'.join(rep))
            sets = [allc[i:i + NCARD] for i in range(0, len(allc), NCARD)]
            # live slots get a SAMPLER (round-robin over the fighters, so every fighter shows up with
            # several variations); the complete by-fighter sets are stored for --set N.
            byf = {}
            for c in allc:
                byf.setdefault(c[27], []).append(c)
            rr = [lst[i] for i in range(max(len(v) for v in byf.values())) for lst in byf.values() if i < len(lst)]
            sampler = [rr[:NCARD], rr[NCARD:2 * NCARD]]
            print('all variations: %d cards = %d by-fighter sets of %d (Logs\\save_sets\\setNN, install with --set N)'
                  % (len(allc), len(sets), NCARD))
            print('live now: SAMPLER of %d cards (round-robin over fighters) -> slot 1 (mc0) %d + slot 2 (mc1) %d'
                  % (len(sampler[0]) + len(sampler[1]), len(sampler[0]), len(sampler[1])))
            if not a.dry_run:
                sd = set_dir()
                os.makedirs(sd, exist_ok=True)
                for k, chunk in enumerate(sets, 1):
                    d = os.path.join(sd, 'set%02d' % k)
                    os.makedirs(d, exist_ok=True)
                    atomic_write(os.path.join(d, 'char'), build(craw, pack_cards(cp, chunk)))
                    with open(os.path.join(d, 'cards.txt'), 'w') as f:
                        for c in chunk:
                            f.write('%s\n' % bytes(c[10:27]).split(b'\0')[0].decode())
                print('sets written to', sd)
            cp[:] = pack_cards(cp, sampler[0])
            second_set = sampler[1] if sampler[1] else None
        else:
            pools = cards_of(dcp) + cards_of(gp4_char(a.gp4))
            cards, dropped, var = build_variants(pools, a.seed)
            fresh = bytearray(cp[:4]) + bytearray(NCARD * CARD)
            for i in range(NCARD):                      # unused slots = empty card
                fresh[4 + i * CARD:4 + (i + 1) * CARD] = TEMPLATE_EMPTY
            for i, c in enumerate(cards):
                fresh[4 + i * CARD:4 + (i + 1) * CARD] = c
            cp[:] = fresh
            print('variant cards: %d written (%d fighters, %d distinct tree paths found), %d not fitting 30 slots'
                  % (len(cards), len(var), sum(len(v) for v in var.values()), len(dropped)))
            for c in cards:
                print('   %-18s colour %d  tree %s' % (c[10:27].split(b'\0')[0].decode(), c[9], list(c[100:100 + c[99]])))
            for fid, key in dropped:
                print('   (left out) %s variant %s' % (FIGHTER[fid], list(key[1:])))

    if a.dry_run:
        print('dry run - nothing written')
        return
    stamp = time.strftime('%Y%m%d-%H%M%S')
    bdir = os.path.join(ROOT, 'Logs', 'save_backups', stamp)
    os.makedirs(bdir)
    for n in ('sys', 'BASLUS-21442SDBZ', 'char', 'char_bk'):
        shutil.copy2(os.path.join(a.mc, n), bdir)
    print('backup ->', bdir)
    snew, cnew = build(sraw, sp), build(craw, cp)
    atomic_write(os.path.join(a.mc, 'sys'), snew)
    atomic_write(os.path.join(a.mc, 'BASLUS-21442SDBZ'), snew)
    atomic_write(os.path.join(a.mc, 'char'), cnew)
    atomic_write(os.path.join(a.mc, 'char_bk'), cnew)
    for n, ln in (('sys', SYS_LEN), ('char', CHAR_LEN)):
        read_pack(os.path.join(a.mc, n), ln)
    print('written + re-verified (md5 ok)')
    if second_set is not None:
        mc1 = ensure_mc1(a.mc)
        for n in ('sys', 'BASLUS-21442SDBZ'):
            shutil.copy2(os.path.join(a.mc, n), os.path.join(mc1, n))
        install_card_file(mc1, craw, pack_cards(cp, second_set), 'sampler part 2 (slot 2)')


def read_pack_bytes(raw, want_len):
    n = struct.unpack_from('<I', raw, 16)[0]
    pay = raw[20:20 + n]
    if n != want_len or hashlib.md5(pay).digest() != raw[:16]:
        sys.exit('donor save: bad length/md5')
    return raw, bytearray(pay)


if __name__ == '__main__':
    main()
