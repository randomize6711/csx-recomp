#!/usr/bin/env python3
"""Player slot patches: writes config/player_slots_patches.toml, the [[patch]] entries (SDK patch 0036) that move the
player tables to their guest tables and raise the player limits, from config/player_slots.toml (what each table is,
which of its references are end pointers, element -1 aliases or another object, and the hand-classified [[change]]
constants) and config/guest_tables.toml (where each table now lives, tools/guest_tables.py's layout). game.toml
includes the output, so the recompiled code computes with the new addresses and limits; the XBE is never changed.

A relocated table's references are every instruction operand (immediate or displacement) whose value lies in the
table: new = the guest table's address + the same offset. A value one element or less past the end must be listed
as an end pointer (shifted to the new end) or as another object's (left alone); a value up to one element before the
base (1-based indexing) must be listed as an alias (same offset) or another object's. Anything unlisted stops the
generator, so a reference nobody reviewed is never patched silently.

  python3 -I tools/slot_patches.py           write config/player_slots_patches.toml and print the summary
  python3 -I tools/slot_patches.py --check   fail when the file is not what the configs give (the tests use it)
  --listing FILE   use that listing (xbe_recomp --listing) instead of running xbe_recomp
"""
import argparse, os, struct, subprocess, sys, tempfile, tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import guest_tables  # noqa: E402

OUT = os.path.join(ROOT, 'config', 'player_slots_patches.toml')


class Error(Exception):
    pass


def read_listing(path):
    """{va: (bytes, text, [(kind, index, value)])}: every analysed instruction once, its original operands"""
    out = {}
    with open(path) as f:
        for line in f:
            col = line.rstrip('\n').split('\t')
            va = int(col[1], 16)
            if va in out:
                continue
            ops = []
            for c in col[4:]:
                if c.startswith('imm:'):
                    _, k, v, w = c.split(':')
                    ops.append(('imm', int(k), int(v, 16)))
                elif c.startswith('mem:'):
                    _, k, base, index, scale, disp = c.split(':')
                    ops.append(('disp', int(k), int(disp, 16)))
            out[va] = (col[2], col[3], ops)
    return out


def run_listing():
    """the listing of the program without the generated patches (their file emptied in a copy of config/), so the
    generator never depends on its own previous output"""
    tmp = tempfile.mkdtemp(prefix='slot_patches_')
    cfg = os.path.join(tmp, 'config')
    os.makedirs(cfg)
    for name in os.listdir(os.path.join(ROOT, 'config')):
        if name.endswith('.toml'):
            src = os.path.join(ROOT, 'config', name)
            open(os.path.join(cfg, name), 'w').write('' if name == os.path.basename(OUT) else open(src).read())
    os.symlink(os.path.join(ROOT, 'xdk'), os.path.join(tmp, 'xdk'))
    listing = os.path.join(tmp, 'listing.tsv')
    r = subprocess.run([os.path.join(ROOT, 'out', 'build', 'linux-tools', 'xbe_recomp'), os.path.join(cfg, 'game.toml'),
                        os.path.join(ROOT, 'assets', 'default.xbe'), os.path.join(tmp, 'gen'), '--listing', listing],
                       capture_output=True, text=True)
    if r.returncode:
        raise Error('xbe_recomp failed:\n' + r.stderr[-2000:])
    return tmp, listing


def spaced(hexbytes):
    return ' '.join(hexbytes[i:i + 2] for i in range(0, len(hexbytes), 2))


def evaluate(expr, names):
    if isinstance(expr, int):
        return expr
    try:
        v = eval(expr, {'__builtins__': {}}, dict(names))
    except Exception as e:
        raise Error('cannot evaluate %r: %s' % (expr, e))
    if not isinstance(v, int):
        raise Error('%r is not an integer' % expr)
    return v


def generate(slots, layout_cfg, insns):
    """[(va, operand, index, old, new, why)] sorted, and the summary lines"""
    layout = {name: (va, elem, count) for name, va, elem, count, align, what in guest_tables.layout(layout_cfg)}
    n = slots.get('slots', {}).get('max_players')
    if not n:
        raise Error('[slots] max_players is not set')
    hdr = open(os.path.join(ROOT, 'src', 'game', 'player_slots.h')).read()
    # the stable build's count is the header's default; an experimental one (tools/slots_variant.sh) defines
    # CSX_MAX_PLAYERS to its own when it compiles
    if n == 65 and '#define CSX_MAX_PLAYERS %d' % n not in hdr:
        raise Error('src/game/player_slots.h CSX_MAX_PLAYERS is not [slots] max_players = %d' % n)
    for t in layout_cfg.get('table', []):
        if 'text' in t and '-maxplayers' in t['text'] and '-maxplayers %d +maxplayers %d' % (n, n) not in t['text']:
            raise Error('guest table %s: its command line does not say -maxplayers %d +maxplayers %d' % (t['name'], n, n))
    # F32(x): the bits of x as a 32-bit float (an immediate the game pushes as a cvar value)
    names = {'N': n, 'PROTOCOL': slots['slots'].get('protocol_version', 46),
             'F32': lambda x: struct.unpack('<I', struct.pack('<f', x))[0]}
    names.update({name: va for name, (va, elem, count) in layout.items()})
    patches, summary = {}, []

    def add(va, kind, index, old, new, why):
        if va not in insns:
            raise Error('%08X is not an analysed instruction (%s)' % (va, why))
        key = (va, kind, index)
        if key in patches and patches[key][1] != new:
            raise Error('%08X operand %d patched twice with different values (%s; %s)' % (va, index, patches[key][2], why))
        patches[key] = (old, new, why)

    for t in slots.get('table', []):
        target = t.get('relocate')
        if not target:
            continue
        if target not in layout:
            raise Error('%s: guest table %s is not in the layout' % (t['name'], target))
        tva, elem, tcount = layout[target]
        base, stride, count = t['base'], t['stride'], t['count']
        new_count = t.get('new_count', tcount)
        if t.get('frame') is None and elem != stride:
            raise Error('%s: the guest table\'s element (0x%X) is not the stride 0x%X' % (t['name'], elem, stride))
        if t.get('frame') is None and new_count > tcount:
            raise Error('%s: %d elements do not fit the guest table\'s %d' % (t['name'], new_count, tcount))
        ends, below, others = set(t.get('ends', [])), set(t.get('below', [])), set(t.get('others', []))
        size, slack = stride * count, t.get('slack', stride)
        counts = {'direct': 0, 'end': 0, 'alias': 0}
        seen = set()
        for va in sorted(insns):
            bytes_, text, ops = insns[va]
            if va in others:
                continue  # another object's reference (or this table's address used as another one's end)
            for kind, index, v in ops:
                if base <= v < base + size + slack and va in ends:
                    # a loop's end: [base + count * stride + field) for a table indexed from 0, or inside the
                    # table for one with an unused element 0 ([18] indexed 1..17): moved by the elements added
                    what = 'end'
                    new = tva + (v - base) + (new_count - count) * stride
                elif base <= v < base + size:
                    what = 'direct'
                    new = tva + (v - base)
                elif base + size <= v < base + size + slack:
                    raise Error('%s: %08X "%s" refers to 0x%X, past the table\'s end: list it in ends or others'
                                % (t['name'], va, text, v))
                elif base - stride <= v < base:
                    if va not in below:
                        continue  # the object before the table (a listed alias is required to patch)
                    what = 'alias'
                    new = tva + (v - base)
                else:
                    continue
                seen.add(va)
                counts[what] += 1
                add(va, kind, index, v, new, '%s: %s 0x%X -> %s+0x%X' % (t['name'], what, v, target, new - tva))
        for va in sorted(others):
            if not any(base - stride <= v < base + size + slack for _, _, v in insns.get(va, ('', '', []))[2]):
                raise Error('%s: %08X is listed in others but refers to nothing in or near the table' % (t['name'], va))
        for va in sorted((ends | below) - seen):
            raise Error('%s: %08X is listed but refers to nothing in or near the table' % (t['name'], va))
        summary.append('%-24s -> %-24s %08X  %d direct, %d end pointers, %d aliases' % (
            t['name'], target, tva, counts['direct'], counts['end'], counts['alias']))

    nchange = 0
    for c in slots.get('change', []):
        va, kind = c['at'], c.get('operand', 'imm')
        if va not in insns:
            raise Error('change %08X is not an analysed instruction' % va)
        bytes_, text, ops = insns[va]
        cands = [o for o in ops if o[0] == kind and ('index' not in c or o[1] == c['index'])]
        if len(cands) != 1:
            raise Error('change %08X "%s": %d %s operands match (name one with index)' % (va, text, len(cands), kind))
        _, index, v = cands[0]
        old = evaluate(c['old'], names)
        if v != old & 0xFFFFFFFF and v != old:
            raise Error('change %08X "%s": the operand is 0x%X, the config expects 0x%X' % (va, text, v, old))
        new = evaluate(c['new'], names)
        add(va, kind, index, v, new & 0xFFFFFFFF, c['why'])
        nchange += 1
    summary.append('%d changes (limits, sizes, protocol fields)' % nchange)
    out = [(va, kind, index, old, new, why) for (va, kind, index), (old, new, why) in sorted(patches.items())]
    summary.insert(0, '%d operand patches for %d player slots' % (len(out), n))
    return out, summary


def render(patches, insns):
    lines = ['# Generated by tools/slot_patches.py from config/player_slots.toml and config/guest_tables.toml: do not edit.',
             '# The player tables moved to their guest tables and the player limits raised (docs/player_slots_plan.md);',
             '# each entry is checked against the image by xbe_recomp (SDK patch 0036) and recorded in patches.txt.', '']
    for va, kind, index, old, new, why in patches:
        bytes_, text, ops = insns[va]
        lines += ['[[patch]]', 'at = 0x%X' % va, 'bytes = "%s"' % spaced(bytes_), 'insn = "%s"' % text,
                  'operand = "%s"' % kind, 'index = %d' % index, 'old = 0x%X' % old, 'new = 0x%X' % new,
                  'why = "%s"' % why.replace('"', "'"), '']
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--slots', default=os.path.join(ROOT, 'config', 'player_slots.toml'))
    ap.add_argument('--layout', default=os.path.join(ROOT, 'config', 'guest_tables.toml'))
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--listing')
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args()
    tmp = None
    try:
        listing = a.listing
        if not listing:
            tmp, listing = run_listing()
        insns = read_listing(listing)
        slots = tomllib.load(open(a.slots, 'rb'))
        layout_cfg = tomllib.load(open(a.layout, 'rb'))
        patches, summary = generate(slots, layout_cfg, insns)
    except (Error, guest_tables.LayoutError) as e:
        print('slot_patches: %s' % e)
        return 1
    finally:
        if tmp:
            subprocess.run(['rm', '-rf', tmp])
    text = render(patches, insns)
    print('\n'.join(summary))
    if a.check:
        cur = open(a.out).read() if os.path.exists(a.out) else ''
        if cur != text:
            print('%s does not match the configs: run tools/slot_patches.py' % a.out)
            return 1
        return 0
    open(a.out, 'w').write(text)
    return 0


if __name__ == '__main__':
    sys.exit(main())
