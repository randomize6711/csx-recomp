#!/usr/bin/env python3
"""Tests tools/shared_changes.py (docs/native_splitscreen.md 15.14): the five kinds a changed shared run is reported as
(protected, player, scratch, process, unresolved), on made-up objects; a fallback reason is unresolved, never process;
a run in no object is unresolved; the review lines name every protected, player and unresolved run. With the region
model's inputs present (build/region_model, your own XBE) also the tenth run's 127 runs against the real model: the
console ring and its positions process-wide, the pool's search key and the particle vector scratch, the health HUD's
damage icon size a player's, the pad bridge process-wide, nothing unresolved. No game is started.

Usage: python3 -I tests/shared_changes_test.py
"""
import contextlib
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import shared_changes as SC  # noqa: E402

failures = []
checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print('FAIL', what)


class Obj:
    def __init__(self, lo, n, ownership, why='', writers=()):
        self.lo, self.hi, self.ownership, self.why, self.writers = lo, lo + n, ownership, why, set(writers)


def made_up():
    objs = [Obj(0x1000, 4, 'process', 'start-up'), Obj(0x1010, 4, 'immutable'), Obj(0x1020, 8, 'player', 'hud'),
            Obj(0x1030, 4, 'shared-mutable', 'the console (docs 15.14): ring'),
            Obj(0x1040, 4, 'shared-mutable', 'a search key'), Obj(0x1050, 4, 'server', 'server'),
            Obj(0x1060, 4, 'shared-mutable', 'its writers are not reached statically (no player\'s code ...)'),
            Obj(0x1070, 4, 'unresolved', 'mixed'), Obj(0x1080, 4, None)]
    runs = [(0x1000, 1), (0x1010, 1), (0x1024, 2), (0x1030, 4), (0x1040, 2), (0x1050, 1), (0x1060, 1), (0x1070, 1),
            (0x1080, 1), (0x2000, 3)]
    rows = SC.classify(runs, objs, scratch={0x1040: 'key'}, fallbacks=('its writers are not reached statically',))
    kinds = {r['va']: r['kind'] for r in rows}
    check(kinds[0x1000] == 'protected' and kinds[0x1010] == 'protected', 'process / immutable: protected')
    check(kinds[0x1024] == 'player', 'a player object: player')
    check(kinds[0x1030] == 'process' and kinds[0x1050] == 'process', 'shared-mutable / server: process-wide')
    check(kinds[0x1040] == 'scratch', 'a listed scratch object: scratch')
    check(kinds[0x1060] == 'unresolved', 'a fallback reason: unresolved, not process-wide')
    check(kinds[0x1070] == 'unresolved' and kinds[0x1080] == 'unresolved', 'unresolved / no ownership: unresolved')
    check(kinds[0x2000] == 'unresolved' and rows[-1]['object'] is None, 'in no object: unresolved')
    check({r['va']: r['subsystem'] for r in rows}[0x1030] == 'console', 'the console named as its subsystem')
    check(SC.kind_of('shared-mutable', 'x', 0x1040, {}, ()) == 'process', 'scratch only when listed')
    check(SC.kind_of('process', 'x', 0x1040, {0x1040: ''}, ()) == 'protected', 'a protected object is never scratch')


def report_lines():
    import region_model as M
    real = M.model
    M.model = lambda: ([Obj(0x1000, 4, 'process'), Obj(0x1020, 4, 'shared-mutable', 'ok')], [], {}, {})
    try:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            SC.main(['0x1000:1', '0x1020:2', '0x3000:4'])
    finally:
        M.model = real
    t = out.getvalue()
    check('review: 2 runs (5 bytes) protected, player or unresolved' in t, 'the review count')
    check('review: 0x00001000 1 byte(s) protected' in t and 'review: 0x00003000 4 byte(s) unresolved' in t,
          'every reviewed run named')
    check('review: 0x00001020' not in t, 'a process-wide run is not in the review')
    check('  process: 1 runs, 2 bytes' in t and '  unresolved: 1 runs, 4 bytes' in t, 'the kinds\' totals')


# the tenth run's 127 runs (build/run/logs/cs_001.log, CSX_NATIVE_SPLIT_CHANGES=1): 115 console ring lines from
# 0x7441E6 every 0x4E bytes, and these twelve
RUN10 = [(0x543A04, 1), (0x548F34, 1), (0x548F38, 1), (0x551918, 1), (0x55191C, 1), (0x56212A, 1), (0x56212E, 1),
         (0x562130, 4), (0x562135, 1), (0x56213C, 2), (0x5622B0, 2), (0x5BD8A8, 8)]


def real_model():
    if not os.path.exists(os.path.join(ROOT, 'build', 'region_model', 'accesses.json')):
        print('note: build/region_model missing: the real model\'s checks are skipped')
        return
    import region_model as M
    with contextlib.redirect_stdout(io.StringIO()):
        objs, errors, _, _ = M.model()
    check(not errors, 'the region model builds without errors')
    runs = RUN10 + [(0x7441E6 + 0x4E * i, 40) for i in range(115)]
    rows = {r['va']: r for r in SC.classify(runs, objs)}
    want = {0x543A04: 'process', 0x548F34: 'player', 0x548F38: 'player', 0x551918: 'process', 0x55191C: 'process',
            0x56212A: 'process', 0x56212E: 'process', 0x562130: 'process', 0x562135: 'process', 0x56213C: 'process',
            0x5622B0: 'scratch', 0x5BD8A8: 'scratch'}
    for va, k in want.items():
        check(rows[va]['kind'] == k, f'0x{va:X}: {k} ({rows[va]["kind"]}, {rows[va]["ownership"]})')
    ring = [rows[0x7441E6 + 0x4E * i] for i in range(115)]
    check(all(r['kind'] == 'process' and r['object'] == [0x744060, 0x3D3E] and r['subsystem'] == 'console'
              for r in ring), 'every console line: the one ring 0x744060+0x3D3E, process-wide')
    check(rows[0x551918]['subsystem'] == 'console' and rows[0x55191C]['subsystem'] == 'console',
          'the ring\'s positions: the console\'s')
    check(rows[0x56212A]['subsystem'] == 'string-intern pool' and rows[0x5BD8A8]['subsystem'] == 'particle manager',
          'the pool\'s and the particle manager\'s diagnostics kept')
    check(not [r for r in rows.values() if r['kind'] == 'unresolved'], 'nothing of the tenth run unresolved')
    # a byte just past the ring and one before it stay in their own objects
    edge = {r['va']: r for r in SC.classify([(0x747D9E, 1), (0x74405F, 1)], objs)}
    check(edge[0x747D9E]['object'] != [0x744060, 0x3D3E] and edge[0x74405F]['object'] != [0x744060, 0x3D3E],
          'the ring ends at 0x747D9E')


def main():
    made_up()
    report_lines()
    real_model()
    print(f'shared_changes_test: {checks} checks, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
