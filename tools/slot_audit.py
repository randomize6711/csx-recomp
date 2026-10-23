#!/usr/bin/env python3
"""Player slot audit: every instruction in the generated program that references a structure, constant or protocol
field tied to the 17 player slots (config/player_slots.toml), as a machine-readable inventory.

It reads xbe_recomp's --listing (every analysed instruction, by function: the code the generated program contains,
decoded by the same decoder [[patch]] uses), so references in bytes analysis never decoded as code are not listed:
they are never executed. Static search finds addresses and constants; code that reaches a structure through a
register computed elsewhere is found only by the heuristics marked "inferred", so a clean report is not proof of
complete coverage.

  python3 -I tools/slot_audit.py                      write analysis/player_slots_inventory.json, print the summary
  python3 -I tools/slot_audit.py --check FILE         also fail on references not in FILE (or gone from it)
  python3 -I tools/slot_audit.py --require-patched T  fail unless every direct reference of table T is [[patch]]ed
  --listing FILE   use that listing instead of running xbe_recomp; --manifest, --out to override the paths
"""
import argparse, json, os, subprocess, sys, tempfile, tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Insn:
    __slots__ = ('va', 'funcs', 'mnemonic', 'imms', 'mems', 'patched', 'text')

    def __init__(self, va, mnemonic, imms, mems, patched, text):
        self.va, self.funcs, self.mnemonic, self.imms, self.mems, self.patched, self.text = va, set(), mnemonic, imms, mems, patched, text


ORDER = {}  # function -> its instruction addresses in xbe_recomp's body order


def read_listing(path):
    """{va: Insn} from xbe_recomp --listing: func, va, bytes, text, operands..., [patched]"""
    insns = {}
    ORDER.clear()
    with open(path) as f:
        for line in f:
            col = line.rstrip('\n').split('\t')
            func, va = int(col[0], 16), int(col[1], 16)
            i = insns.get(va)
            if i is None:
                imms, mems, patched = [], [], False
                for c in col[4:]:
                    if c == 'patched':
                        patched = True
                    elif c.startswith('imm:'):
                        _, idx, v, w = c.split(':')
                        imms.append((int(idx), int(v, 16), int(w)))
                    elif c.startswith('mem:'):
                        _, idx, base, index, scale, disp = c.split(':')
                        mems.append((int(idx), base, index, int(scale), int(disp, 16)))
                text = col[3]
                i = insns[va] = Insn(va, text.split(' ')[0], imms, mems, patched, text)
            i.funcs.add(func)
            ORDER.setdefault(func, []).append(va)
    return insns


def run_listing(manifest_dir):
    out = tempfile.mkdtemp(prefix='slot_audit_')
    listing = os.path.join(out, 'listing.tsv')
    r = subprocess.run([os.path.join(ROOT, 'out', 'build', 'linux-tools', 'xbe_recomp'), os.path.join(ROOT, 'config', 'game.toml'),
                        os.path.join(ROOT, 'assets', 'default.xbe'), os.path.join(out, 'gen'), '--listing', listing],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit('xbe_recomp failed:\n' + r.stderr[-2000:])
    return out, listing


def area_of(va, ranges):
    for lo, hi, name in ranges:
        if lo <= va < hi:
            return name
    return None


def absolute_values(i):
    """(value, how) for every address-like operand: immediates and memory displacements"""
    for idx, v, w in i.imms:
        yield v, 'imm'
    for idx, base, index, scale, disp in i.mems:
        yield disp, 'disp' if base == '-' and index == '-' else 'indexed-disp'


def reg_relative(i):
    """displacements off a base register (a pointer the code holds), not absolute addresses"""
    for idx, base, index, scale, disp in i.mems:
        if base not in ('-', 'esp', 'ebp') or (base == 'ebp' and index != '-'):
            yield disp


def ref(i, kind, evidence, **extra):
    r = dict(va='%08X' % i.va, funcs=['%08X' % f for f in sorted(i.funcs)], mnemonic=i.mnemonic, kind=kind,
             evidence=evidence, patched=i.patched)
    r.update(extra)
    return r


def audit(manifest, insns):
    cfg = manifest.get('audit', {})
    ranges = [tuple(r) for r in cfg.get('ranges', [])]
    by_va = sorted(insns)
    tables = []
    for t in manifest.get('table', []):
        refs = []
        lo = t.get('base')
        if lo is not None:
            size = t['stride'] * t['count']
            slack = t.get('slack', t['stride'])
            spans = [(lo, lo + size, lo + size + slack, t['stride'])]
            spans += [(p[0], p[0] + p[1] * p[2], p[0] + p[1] * p[2], p[1]) for p in t.get('parts', [])]
            also = set(t.get('also', []))
            frame = t.get('frame')
            for va in by_va:
                i = insns[va]
                for v, how in absolute_values(i):
                    if v in also:
                        refs.append(ref(i, 'alias', 'verified', operand=how, value='%X' % v))
                        continue
                    if frame and frame['base'] <= v < frame['base'] + frame['stride'] * frame['count']:
                        continue  # the frame section below classifies the whole ring
                    for s_lo, s_end, s_hi, stride in spans:
                        if s_lo <= v < s_hi:
                            off = v - s_lo
                            kind = 'direct' if v < s_end else 'end-pointer'
                            refs.append(ref(i, kind, 'verified', operand=how, value='%X' % v, element=off // stride,
                                            offset='%X' % (off % stride)))
                            break
        if 'fields' in t:
            # an object reached through a register: its offsets, in the code that handles it (inferred: the same
            # offsets off another pointer in that code also match)
            code = t.get('code', [[0, 1 << 32]])
            for va in by_va:
                if not any(lo_ <= va < hi_ for lo_, hi_ in code):
                    continue
                i = insns[va]
                for d in reg_relative(i):
                    if any(f_lo <= d < f_hi for f_lo, f_hi in t['fields']):
                        refs.append(ref(i, 'field', 'inferred', offset='%X' % d))
                        break
        # stride multiplies: an imul by the element size; verified when the function also references the table
        direct_funcs = {f for r in refs if r['kind'] in ('direct', 'end-pointer', 'alias') for f in r['funcs']}
        for stride in t.get('strides', []):
            for va in by_va:
                i = insns[va]
                if i.mnemonic == 'imul' and any(v == stride for _, v, _ in i.imms):
                    near = sorted(set('%08X' % f for f in i.funcs) & direct_funcs)
                    refs.append(ref(i, 'stride', 'verified' if near else 'inferred', stride='%X' % stride))
        refs.sort(key=lambda r: (r['va'], r['kind']))
        counts = {}
        for r in refs:
            counts[r['kind']] = counts.get(r['kind'], 0) + 1
        direct = [r for r in refs if r['kind'] in ('direct', 'end-pointer', 'alias')]
        tables.append(dict(name=t['name'], area=t['area'], what=t['what'], plan=t['plan'], layout=t['layout'],
                           source=t.get('source', ''),
                           range=None if lo is None else ['%X' % lo, '%X' % (lo + t['stride'] * t['count'])],
                           counts=counts, functions=len({f for r in refs for f in r['funcs']}),
                           patched=sum(r['patched'] for r in direct), unpatched=sum(not r['patched'] for r in direct),
                           references=refs))

    frame_sec = frame_section(manifest, insns, by_va)

    sites, known = [], {}
    for s in manifest.get('site', []):
        i = insns.get(s['at'])
        known[s['at']] = s['role']
        sites.append(dict(va='%08X' % s['at'], role=s['role'], what=s['what'], evidence=s['evidence'], found=i is not None,
                          mnemonic=i.mnemonic if i else None, funcs=['%08X' % f for f in sorted(i.funcs)] if i else [],
                          values=['%X' % v for v, _ in absolute_values(i)] if i else [], patched=bool(i and i.patched)))

    consts = set(cfg.get('count_constants', []))
    candidates = []
    for k, va in enumerate(by_va):
        i = insns[va]
        area = area_of(va, ranges)
        if not area or va in known or i.mnemonic not in ('cmp', 'mov', 'push', 'sub', 'add', 'lea'):
            continue
        hits = [v for _, v, _ in i.imms if v in consts]
        if hits:
            nxt = [insns[by_va[j]] for j in range(k + 1, min(k + 4, len(by_va)))]
            pattern = classify_pattern(i, hits[0], nxt)
            byte_reg = i.text.split(' ', 1)[-1].split(',')[0].strip() in ('al', 'bl', 'cl', 'dl', 'ah', 'bh', 'ch', 'dh')
            candidates.append(dict(va='%08X' % va, area=area, mnemonic=i.mnemonic, value='%X' % hits[0],
                                   funcs=['%08X' % f for f in sorted(i.funcs)],
                                   note='byte register: a character or flag test more likely' if byte_reg and not pattern else '',
                                   status=('pattern: ' + pattern) if pattern else 'unreviewed'))

    unresolved = [dict(what=u['what'], source='manifest') for u in manifest.get('unresolved', [])]
    unresolved += [dict(what='site %s (%s) is not an analysed instruction' % (s['va'], s['what']), source='audit')
                   for s in sites if not s['found']]
    unresolved += [dict(what='table %s: no reference found' % t['name'], source='audit') for t in tables if not t['references']]
    open17 = [c['va'] for c in candidates if c['value'] == '11' and not c['note'] and c['status'] == 'unreviewed']
    if open17:
        unresolved.append(dict(what='%d compares or loads of 17 not classified: %s' % (len(open17), ' '.join(open17)),
                               source='audit'))
    frame_t = next((t for t in manifest.get('table', []) if 'frame' in t), {})
    reviewed = {'%08X' % f for f in frame_t.get('reviewed', [])}
    frame_sec['reviewed'] = sorted(reviewed)
    open_funcs = [f for f in frame_sec.get('frame_functions', []) if f not in reviewed]
    if open_funcs:
        unresolved.append(dict(what='frame ring: %d functions compute frame addresses and were not reviewed: %s'
                                    % (len(open_funcs), ' '.join(open_funcs)), source='audit'))
    traced = [r for r in frame_sec.get('frame_register_accesses', []) if r['kind'] == 'playerstate'
              and r['function'] not in reviewed]
    if traced:
        unresolved.append(dict(what='frame ring: %d player state accesses through a frame-derived register, in %s'
                                    % (len(traced), ' '.join(sorted({r['function'] for r in traced}))), source='audit'))
    strong = [r for r in frame_sec['pointer_candidates'] if r['signal'] == 'frame-pointer'
              and not set(r['funcs']) <= reviewed]
    if strong:
        unresolved.append(dict(what='frame ring, wider net: %d register-relative accesses past the first player state in frame '
                                    'functions (%s)' % (len(strong), ' '.join(sorted({f for r in strong for f in r['funcs']}))),
                               source='audit'))
    return dict(format=1, instructions=len(insns), functions=len({f for i in insns.values() for f in i.funcs}),
                tables=tables, frame_ring=frame_sec, sites=sites, candidates=candidates, unresolved=unresolved)


def classify_pattern(i, value, nxt):
    """what a count constant evidently is from the instructions after it, or '' (inferred from the code's shape;
    a [[site]] in the manifest overrides it)"""
    dst = i.text.split(' ', 1)[-1].split(',')[0].strip()
    texts = [n.text for n in nxt]
    if i.mnemonic == 'mov' and dst == 'ecx':
        if any(t.startswith(('rep movsd', 'rep stosd')) for t in texts):
            return 'block copy or clear of %d dwords' % value
        if any(t.startswith('repe cmpsb') for t in texts):
            return 'compare of %d bytes' % value
    if i.mnemonic == 'cmp' and texts and texts[0].startswith(('jnbe', 'ja ')) and len(texts) > 1 and (
            texts[1].startswith('jmp [') or texts[1].startswith('movzx')):
        return 'switch bound'
    if i.mnemonic == 'cmp' and texts and texts[0].startswith('jz') and len(texts) > 1 and texts[1] == 'cmp eax, 0x01':
        return 'weapon id compare (the Glock is weapon 17)'
    if i.mnemonic == 'mov' and texts and texts[0].startswith('mov dword ptr') and texts[0].endswith(', 0x%02X' % (value + 1)):
        return 'enumeration table (%d, %d, ...)' % (value, value + 1)
    if i.mnemonic == 'add' and texts and texts[0].startswith('cmp ') and value == 0x11:
        return 'pointer step of 17 bytes in a loop'
    return ''


def frame_section(manifest, insns, by_va):
    """the client frame ring: absolute references split into the player states and the frame's other fields; the
    functions that compute frame addresses; register-relative accesses that may reach a player state through a
    frame pointer (inferred)"""
    t = next((t for t in manifest.get('table', []) if 'frame' in t), None)
    if not t:
        return dict(absolute=[], frame_functions=[], pointer_candidates=[], pointer_walks=[])
    fr = t['frame']
    fb, fs, fc, ps = fr['base'], fr['stride'], fr['count'], fr['field']
    ps_end = ps + t['stride'] * t['count']
    absolute, funcs = [], set()
    for va in by_va:
        i = insns[va]
        for v, how in absolute_values(i):
            if fb <= v < fb + fs * fc:
                off = (v - fb) % fs
                part = 'playerstate' if ps <= off < ps_end else 'frame-field'
                extra = dict(frame=(v - fb) // fs, offset='%X' % off)
                if part == 'playerstate':
                    extra.update(element=(off - ps) // t['stride'], within='%X' % ((off - ps) % t['stride']))
                absolute.append(ref(i, part, 'verified', operand=how, **extra))
                funcs |= i.funcs
        if i.mnemonic == 'imul' and any(v == fs for _, v, _ in i.imms):
            funcs |= i.funcs
    traced = trace_frame_registers(insns, funcs, fb, fs, fc, ps, ps_end, t['stride'])
    candidates, walks = [], []
    for va in by_va:
        i = insns[va]
        if not i.funcs & funcs:
            continue
        for d in reg_relative(i):
            if ps <= d < ps_end:
                # past the first player state only a frame pointer reaches (high signal); within it an
                # entity_state pointer's own fields match too (low signal)
                strength = 'frame-pointer' if d >= ps + t['stride'] else 'state-or-frame'
                candidates.append(ref(i, 'register-relative', 'inferred', offset='%X' % d, signal=strength))
                break
        if i.mnemonic in ('add', 'lea', 'sub') and any(v == t['stride'] for _, v, _ in i.imms):
            walks.append(ref(i, 'pointer-walk', 'inferred', stride='%X' % t['stride']))
        elif i.mnemonic == 'lea' and any(d == t['stride'] for d in reg_relative(i)):
            walks.append(ref(i, 'pointer-walk', 'inferred', stride='%X' % t['stride']))
    counts = {}
    for r in absolute:
        counts[r['kind']] = counts.get(r['kind'], 0) + 1
    return dict(base='%X' % fb, frame_stride='%X' % fs, frames=fc, playerstate=['%X' % ps, '%X' % ps_end],
                counts=counts, frame_functions=['%08X' % f for f in sorted(funcs)], absolute=absolute,
                frame_register_accesses=traced, pointer_candidates=candidates, pointer_walks=walks)


REGS = ('eax', 'ebx', 'ecx', 'edx', 'esi', 'edi', 'ebp')


def trace_frame_registers(insns, funcs, fb, fs, fc, ps, ps_end, ps_stride):
    """memory accesses whose base or index register holds a frame address or a frame offset, by a straight-line
    trace through each frame function (inferred: branches and stack spills are not followed). A register becomes
    frame-derived from an imul by the frame stride, a load of an address in the ring, or a copy, add or lea of a
    frame-derived register; any other write, and a call for eax / ecx / edx, clears it"""
    out = []
    in_ring = lambda v: fb <= v < fb + fs * fc
    for f in sorted(funcs):
        taint = set()
        for va in ORDER.get(f, []):
            i = insns[va]
            ops = i.text.split(' ', 1)[1] if ' ' in i.text else ''
            dst = ops.split(',')[0].strip()
            src = ops.split(',', 1)[1].strip() if ',' in ops else ''
            for idx, base, index, scale, disp in i.mems:
                if base in taint or index in taint:
                    off = disp if not in_ring(disp) else (disp - fb) % fs
                    part = 'playerstate' if ps <= off < ps_end else 'frame-field'
                    out.append(ref(i, part, 'inferred', offset='%X' % off, function='%08X' % f,
                                   via=base if base in taint else index))
                    break
            if i.mnemonic == 'call':
                taint -= {'eax', 'ecx', 'edx'}
                continue
            if dst not in REGS:
                continue
            mem_tainted = any(b in taint or x in taint or in_ring(d) for _, b, x, _, d in i.mems)
            if i.mnemonic == 'imul' and any(v == fs for _, v, _ in i.imms):
                taint.add(dst)
            elif i.mnemonic == 'lea':
                taint.add(dst) if mem_tainted else taint.discard(dst)
            elif i.mnemonic == 'mov' and (src in taint or any(in_ring(v) for _, v, _ in i.imms)):
                taint.add(dst)
            elif i.mnemonic in ('add', 'sub') and (dst in taint or src in taint or any(in_ring(v) for _, v, _ in i.imms)):
                taint.add(dst)
            elif i.mnemonic in ('cmp', 'test', 'push'):
                pass
            else:
                taint.discard(dst)
    out.sort(key=lambda r: (r['va'], r['function']))
    return out


def keyset(inv):
    """every reference as a comparable key, for --check"""
    keys = set()
    for t in inv['tables']:
        keys |= {('table', t['name'], r['va'], r['kind']) for r in t['references']}
    fr = inv['frame_ring']
    for sec in ('absolute', 'pointer_candidates', 'pointer_walks'):
        keys |= {('frame', sec, r['va'], r['kind']) for r in fr.get(sec, [])}
    keys |= {('site', s['va'], s['found']) for s in inv['sites']}
    keys |= {('candidate', c['va'], c['value']) for c in inv['candidates']}
    return keys


def summary(inv):
    out = ['player slot audit: %d instructions in %d functions' % (inv['instructions'], inv['functions'])]
    out.append('%-24s %-14s %-13s %-9s %s' % ('table', 'area', 'plan', 'layout', 'references (functions; patched / unpatched direct)'))
    for t in inv['tables']:
        c = ', '.join('%s %d' % kv for kv in sorted(t['counts'].items())) or 'none'
        out.append('%-24s %-14s %-13s %-9s %s (%d; %d / %d)' % (t['name'], t['area'], t['plan'], t['layout'], c, t['functions'],
                                                              t['patched'], t['unpatched']))
    fr = inv['frame_ring']
    strong = sum(1 for r in fr['pointer_candidates'] if r.get('signal') == 'frame-pointer')
    tr = fr.get('frame_register_accesses', [])
    out.append('frame ring, traced registers: %d accesses through a frame-derived register (%d in the player states, %d '
               'other frame fields) in %d functions (inferred)' % (len(tr), sum(r['kind'] == 'playerstate' for r in tr),
                                                                   sum(r['kind'] == 'frame-field' for r in tr),
                                                                   len({r['function'] for r in tr})))
    out.append('frame ring: %s; %d functions compute frame addresses; register-relative player state candidates: %d past '
               'the first state (a frame pointer), %d within it (an entity state pointer too); %d pointer walks (all inferred)'
               % (', '.join('%s %d' % kv for kv in sorted(fr.get('counts', {}).items())), len(fr['frame_functions']), strong,
                  len(fr['pointer_candidates']) - strong, len(fr['pointer_walks'])))
    roles = {}
    for s in inv['sites']:
        roles.setdefault(s['role'], [0, 0])
        roles[s['role']][0 if s['found'] else 1] += 1
    out.append('sites: ' + ', '.join('%s %d%s' % (k, v[0], ' (%d missing)' % v[1] if v[1] else '') for k, v in sorted(roles.items())))
    per = {}
    for c in inv['candidates']:
        key = (c['value'], c['area'])
        per[key] = per.get(key, 0) + 1
    out.append('unreviewed constant candidates: ' + ', '.join('0x%s in %s %d' % (v, a, n) for (v, a), n in sorted(per.items())))
    out.append('unresolved:')
    out += ['  - ' + u['what'] for u in inv['unresolved']]
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--manifest', default=os.path.join(ROOT, 'config', 'player_slots.toml'))
    ap.add_argument('--listing')
    ap.add_argument('--out', default=os.path.join(ROOT, 'analysis', 'player_slots_inventory.json'))
    ap.add_argument('--check')
    ap.add_argument('--require-patched', action='append', default=[])
    a = ap.parse_args()
    manifest = tomllib.load(open(a.manifest, 'rb'))
    tmp = None
    listing = a.listing
    if not listing:
        tmp, listing = run_listing(os.path.dirname(a.manifest))
    try:
        inv = audit(manifest, read_listing(listing))
    finally:
        if tmp:
            subprocess.run(['rm', '-rf', tmp])
    text = json.dumps(inv, indent=1, sort_keys=True) + '\n'
    if a.out != '-':
        open(a.out, 'w').write(text)
    print(summary(inv))
    status = 0
    if a.check:
        ref_inv = json.load(open(a.check))
        new, gone = keyset(inv) - keyset(ref_inv), keyset(ref_inv) - keyset(inv)
        for k in sorted(new):
            print('unexpected reference (not in %s): %s' % (os.path.basename(a.check), ' '.join(map(str, k))))
        for k in sorted(gone):
            print('reference gone (in %s, not found now): %s' % (os.path.basename(a.check), ' '.join(map(str, k))))
        status |= bool(new or gone)
    for name in a.require_patched:
        t = next((t for t in inv['tables'] if t['name'] == name), None)
        if t is None:
            print('no table %s' % name)
            status = 1
            continue
        others = {'%08X' % v for v in next((m.get('others', []) for m in manifest.get('table', []) if m['name'] == name), [])}
        for r in t['references']:
            if r['kind'] in ('direct', 'end-pointer', 'alias') and not r['patched'] and r['va'] not in others:
                print('%s: %s %s not patched' % (name, r['va'], r['kind']))
                status = 1
    return status


if __name__ == '__main__':
    sys.exit(main())
