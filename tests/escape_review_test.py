#!/usr/bin/env python3
"""Tests the address-escape review (tools/escape_review.py, docs/native_splitscreen.md 15.8) without the game:
  1. synthetic recompiled code: the seventh run's blind spot (a pointer moved through ebp and kept past a branch, as
     0x115D10 does with the string-intern pool): the model's reading misses the callee's writes, the review's finds
     them; a single-assignment register is kept past a label only in the review; ebp as a frame pointer is not a
     pointer; the walk finds an address passed in a register, as a stack argument, to a call through a pointer, and
     returned by a getter;
  2. the real review (your own XBE: needs build/gen/game and build/region_model; skipped with a note without them):
     the pool is the first object (the run's evidence), its three receivers, its potential writes (the strings vector,
     the root, the count: missed by the model) and its fields' evidence; the neighbour 0x562148 (the file system:
     address stored in 0x438C2C and returned by 0x114430, low confidence, its protected vtable field with no potential
     write); the report changes no table (the fingerprints as before).
No game is started.

Usage: python3 -I tests/escape_review_test.py
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import check_tables as CT  # noqa: E402
import escape_review as ER  # noqa: E402
import region_model as M  # noqa: E402
from region_model_test import Code  # noqa: E402

failures = []
checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print('FAIL', what)


def reg(c, at, text, dst, expr):
    c.ins(at, text, f'{{ ctx.{dst} = (uint32_t)({expr}); }}')


def synthetic():
    POOL = 0x562118
    c = Code()
    # writer(ecx): mov esi, ecx; jz; mov [esi+4], eax: esi assigned once (push / pop aside), the write after a branch
    c.func(0x1000)
    c.ins(0x1000, 'push esi', '{ X86_PUSH32(ctx.esi); }')
    reg(c, 0x1001, 'mov esi, ecx', 'esi', 'ctx.ecx')
    c.ins(0x1003, 'jz 0x00001008', '{ if (zf) goto loc_00001008; }')
    c.label(0x1008)
    c.via(0x1008, 'esi', 4)
    c.ins(0x100B, 'pop esi', '{ ctx.esi = X86_POP32(); }')
    c.ins(0x100C, 'ret', '{ return; }')
    # intern(ebx): jnz; lea ebp, [ebx+0x18]; mov ecx, ebp; call writer: ebp, not a frame here
    c.func(0x2000)
    c.ins(0x2000, 'jnz 0x00002010', '{ if (!zf) goto loc_00002010; }')
    c.ins(0x2002, 'push ebp', '{ X86_PUSH32(ctx.ebp); }')
    reg(c, 0x2003, 'lea ebp, [ebx+0x18]', 'ebp', 'ctx.ebx + 0x18u')
    reg(c, 0x2006, 'mov ecx, ebp', 'ecx', 'ctx.ebp')
    c.call(0x2008, 0x1000)
    c.ins(0x200D, 'pop ebp', '{ ctx.ebp = X86_POP32(); }')
    c.label(0x2010)
    c.ins(0x2010, 'ret', '{ return; }')
    # a frame function: [ebp+8] is its argument, ebp never a pointer
    c.func(0x3000)
    c.ins(0x3000, 'push ebp', '{ X86_PUSH32(ctx.ebp); }')
    reg(c, 0x3001, 'mov ebp, esp', 'ebp', 'ctx.esp')
    c.via(0x3003, 'ebp', 8)
    c.ins(0x3006, 'pop ebp', '{ ctx.ebp = X86_POP32(); }')
    c.ins(0x3007, 'ret', '{ return; }')
    # the callers: a register, a stack argument, a pointer call, a getter
    c.func(0x4000)
    c.const(0x4000, 'ebx', POOL)
    c.call(0x4005, 0x2000)
    c.push(0x400A, 0x562148)
    c.call(0x400F, 0x5000)
    c.const(0x4014, 'ecx', 0x562148)
    c.ins(0x4019, 'call [eax+0x10]', '{ X86_PUSH32(0x401Cu); x86_icall(ctx, base, X86_MEM32(ctx.eax + 0x10u)); }')
    c.func(0x4100)
    c.const(0x4100, 'eax', 0x562148)
    c.ins(0x4105, 'ret', '{ return; }')
    src = [c.lines]
    plain = M.pointer_summaries(src)
    wide = M.pointer_summaries(src, review=True)
    pw = {(o, w) for o, w, k in plain.get(0x2000, {}).get(('reg', 'ebx'), ()) if k == 'w'}
    ww = {(o, w) for o, w, k in wide.get(0x2000, {}).get(('reg', 'ebx'), ()) if k == 'w'}
    check((0x1C, 4) not in pw, f'the model\'s reading misses the write through ebp ({pw})')
    check((0x1C, 4) in ww, f'the review\'s reading finds it: ebx+0x18 through ebp, +4 by the writer ({ww})')
    w1 = {(o, w) for o, w, k in wide.get(0x1000, {}).get(('reg', 'ecx'), ()) if k == 'w'}
    p1 = {(o, w) for o, w, k in plain.get(0x1000, {}).get(('reg', 'ecx'), ()) if k == 'w'}
    check((4, 4) in w1 and (4, 4) not in p1, 'a register assigned once keeps its pointer past a label in the review only')
    check(('reg', 'ebp') not in wide.get(0x3000, {}), 'ebp as a frame is never a pointer argument')
    passings, returns = ER.walk(src)
    got = {(fn, callee, pid, v) for fn, at, callee, pid, v in passings}
    check((0x4000, 0x2000, ('reg', 'ebx'), POOL) in got, 'the walk: the pool passed in ebx')
    check((0x4000, 0x5000, ('arg', 0), 0x562148) in got, 'the walk: an address passed as a stack argument')
    check((0x4000, None, ('reg', 'ecx'), 0x562148) in got, 'the walk: an address passed to a call through a pointer')
    check((0x4100, 0x4105, 0x562148) in returns, 'the walk: a getter returning the address')


def real():
    if not os.path.isdir(M.GEN) or not os.path.exists(os.path.join(M.OUT, 'model.json')):
        print('note: build/gen/game or build/region_model missing: the real review is skipped')
        return
    before = CT.expected()
    summary, report, protected = ER.review(write=False)
    by = {r['base']: r for r in report}
    pool = by.get('0x562118')
    check(report and report[0]['base'] == '0x562118' and pool['priority'] == 'run',
          'the pool is the first object: a run saw its fields change')
    check(pool and pool['size'] == 0x28 and pool['constructor'] == '0x23D170' and pool['destructor'] == ['0x23FDA0'],
          'its size (the constructor\'s fields), constructor and destructor')
    check(pool and [r.split()[0] for r in pool['receivers']] == ['0x114680', '0x1148A0', '0x114C10'],
          f'its receivers ({pool and pool["receivers"]})')
    pot = ' '.join(pool['potential_writes']) if pool else ''
    for f in ('+0x18/4', '+0x1C/4', '+0x24/4', '+0x10/2', '+0x12/2'):
        check(f'{f} by 0x114680 via 0x115D10 (missed by the model)' in pot, f'potential write {f}, missed by the model')
    observed = [f for f in pool['fields'] if f['observed']] if pool else []
    check(len(observed) == 3 and all(f['evidence'] == 'shared-mutable' and not f['protected'] for f in observed),
          'the three observed fields: reviewed (evidence), no longer protected')
    consts = [f for f in pool['fields'] if f['at'] in (0x562118, 0x562124, 0x562138)] if pool else []
    check(len(consts) == 3 and all(not f['evidence'] and not f['potential'] for f in consts),
          'the constant fields: not reclassified, no potential write')
    fs = by.get('0x562148')
    check(fs and fs['confidence'] == 'low' and any('[0x438C2C]' in x for x in fs['stored']) and
          fs['returned'] == ['0x114430'], 'the file system 0x562148: stored in 0x438C2C, returned by 0x114430: low')
    vt = [f for f in fs['fields'] if f['at'] == 0x562148] if fs else []
    check(vt and vt[0]['protected'] and not vt[0]['potential'], 'its protected vtable field: no potential write')
    check(summary['address escapes (flagged)'] == len(report) and summary['constructed objects'] >= len(report),
          'the summary counts its objects')
    check(all(h['range'] not in ('0x562130+4', '0x562134+4', '0x56213C+4') for h in protected),
          'the pool\'s fields are not protected candidates any more')
    check(CT.expected() == before, 'the review changes no table (the fingerprints as before)')
    print(f'note: {summary["constructed objects"]} constructed objects, {len(report)} flagged, '
          f'{len(protected)} protected ranges with potential writes by other code')


def main():
    synthetic()
    real()
    print(f'escape_review_test: {checks} checks, {len(failures)} failed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
