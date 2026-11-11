#!/usr/bin/env python3
"""The native split-screen scenarios' failure detection for ownership violations (docs/native_splitscreen.md 15.16),
without the game: each scenario's `expect nolog` patterns, read and matched as tools/drive.py does (the step split on
spaces and joined again, re.search over every log line), fed log lines:

  - the twelfth run's blocked foreign free, verbatim (the line the leave scenario missed), and the earlier runs' lines
    of each kind (an untagged binary's): every one fails each scenario
  - a line made from each [own:...] error format in the source (its printf fields filled in): every one fails each
    scenario, and every tag the source logs is one the docs list
  - the lines a passing leave logs (a cleanup's kept blocks, its unreleased objects reported, the shared-vector watch,
    the retention breakdown): none fails the leave scenario

No game or scenario is started.

Usage: python3 -I tests/scenario_ownership_test.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCENARIOS = ['native_split_2p_leave.txt', 'native_split_2p.txt', 'native_split_2p_p1_leaves.txt']
failures = []
checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print('FAIL', what)


def nologs(name):
    """the scenario's `expect nolog` regexes as drive.py reads them (main: l.split(); the expect step's ' '.join)"""
    path = os.path.join(ROOT, 'tests', 'scenarios', name)
    steps = [l.split() for l in open(path) if l.strip() and not l.startswith('#')]
    return [re.compile(' '.join(st[2:])) for st in steps if st[:2] == ['expect', 'nolog']]


def fails(rxs, line):
    return [r.pattern for r in rxs if r.search(line)]


# lines of earlier runs (build/run/logs, kept in the session's notes), the prefix as the log writes it
P = '[   65.089] [error] [tad74] '
HISTORICAL = {
    'run 12: a blocked foreign free': P + "native split: player 2 would free (skipped: a leak, not a block freed under "
                                          "its owner) block 0x00C93FF0 of the process's initialisation (return 0x000B532E)",
    'run 13: the root panel\'s children grown in player 2\'s turn': '[   65.191] [error] [t15e1] native split: player 2 '
        'would free (skipped: a leak, not a block freed under its owner) block 0x00C93FF0 of the process\'s initialisation '
        '(return 0x000B532E; the block\'s 16 bytes from site 0xB52DA, serial 10003) [own:free-blocked]',
    'a foreign free done': P + "native split: the process frees block 0x02180330 of player 2 (return 0x001A7AF9)",
    'a foreign realloc': P + "native split: player 2 moves block 0x0211C670 of the process's initialisation (realloc to "
                             "0x02200000, return 0x000B532E)",
    'the shared front end': P + "native split: the shared front end would free block 0x02180330 of player 2 (skipped: a "
                                "player's object, not the shared UI's; return 0x001A7AF9)",
    'a foreign destroy': P + "native split: player 2 would destroy the client object 0x552C70 0x0211C670 of the process's "
                             "initialisation: not destroyed, the reference dropped",
    'protected state': P + "native split: player 2's initialisation changed protected start-up state at 0x00548F34 (4 "
                           "byte(s); the pad bridge)",
    'the pool broken': P + "native split: player 2's initialisation broke the string-intern pool 0x00562118: count",
    'the server write guard': '[   12.000] [warn] [tad74] client context guard: the server frame wrote cl at 0x00771224 '
                              '(+0x0, 4 bytes changed in the span to +0x4): 0x00000000 -> 0x00000001',
}
PASSING = [
    "native split: player 2's cleanup: 2638 of its blocks (874650 bytes) named by another (kept), 56049 bytes named by "
    "nobody else",
    "native split: player 2's object 0x02180330 kept: named by another",
    "native split: player 2's object 0x0211C670 kept: a shared kind",
    "native split: player 2's cleanup keeps block 0x02180330 (16 bytes, site 0x1A6E0F): named by another (return "
    "0x001BADAD)",
    "[   33.442] [error] [tad74] native split: player 2 left 2708 objects (930443 bytes) unreleased, allocated at: "
    "0x1CFD0 x32 0xB52DA x1264",
    "native split: player 2 left: its server slot dropped, 21 of its objects released, 2708 left (930443 bytes), its "
    "context removed; left behind by leaving players so far 930443 bytes in 1 leave(s)",
    "native split: player 2's cleanup keeps 2600 block(s), 870000 bytes: named by a kept block of its own",
    "native split:   kept through the word at 0x005622B0 (the string-intern pool's search key): 1 block(s), 32 bytes",
    "native split: player 2's cleanup: the string-intern pool's search key names one of its blocks (kept: named)",
    "[   72.000] [warn] [tad74] native split: the VGUI font list (0x005620A8, 40 of 64 entries) is on block 0x02200000 "
    "of player 2 (site 0xB52DA): a process-wide vector grown in a player's turn [watch:shared-vector]",
    "native split: ownership: 0 double frees skipped, 0 stale names of left-behind blocks in fresh contexts, 0 cleanup "
    "calls refused",
    "native split: 0 packets dropped by full channels, 0 read outside their player's turn, 0 foreign frees (0 skipped), "
    "149 reallocs moved (0 of another owner's block), 33822 heap blocks tracked",
    "native split: player 2's object 0x02155010 kept: an object it deletes is not released (named by another, not its "
    "own or a shared kind)",
    # docs 15.17: the VGUI hierarchy's lines of a passing leave
    "native split: player 2's turn grows the process's panel 0x00909CC0's vector (header 0x00909CEC: 4 of 4 entries, to "
    "5; buffer 0x00C93FF0 of the process) as the process (return 0x000B5246) [watch:shared-vector]",
    "native split: player 2's turn sets the parent of panel 0x02B99870 (player 2's) to the process's panel 0x00909CC0: 3 "
    "of 4 children, buffer 0x00C93FF0 (the process's) [watch:shared-parent]",
    "native split: player 2's viewport 0x02B99870 detached from the process's panel 0x00909CC0 (removeChild 0x10ADC0): "
    "its children 3 -> 2 of 4, buffer 0x00C93FF0 unchanged; the viewport not deleted (no destructor: left behind) "
    "[watch:shared-parent]",
    "native split: the VGUI hierarchy: 1 process-wide vectors grown as the process (0 old buffers of a player kept), 3 "
    "panels given a parent of the process's, 3 viewports detached (0 kept on their parent), 1 shared parents watched, 0 "
    "stale children",
    "native split:   kept through the word at 0x005D1748: 1 block(s), 637500 bytes (it names block 0x02400000, 637500 "
    "bytes from site 0x66943)",
]


def tagged_formats():
    """(tag, a line made from the format) for every [own:...] log in the sources"""
    out = []
    for f in ('native_split.cpp', 'client_context.cpp'):
        src = open(os.path.join(ROOT, 'src', 'game', f)).read()
        for m in re.finditer(r'LOG[EW]\(((?:\s*"(?:[^"\\]|\\.)*")+)', src):
            fmt = ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', m[1]))
            tag = re.search(r'\[own:([a-z-]+)\]', fmt)
            if not tag:
                continue
            line = re.sub(r'%(?:\d*|\.\d+)(?:ll|z|l)?([duxXsf])',
                          lambda x: {'s': 'player 2', 'f': '1.0'}.get(x[1], '0x0211C670' if x[1] in 'xX' else '2'), fmt)
            out.append((tag[1], P + line.replace('%%', '%')))
    return out


def main():
    rx = {s: nologs(s) for s in SCENARIOS}
    for s in SCENARIOS:
        check(any(r.pattern == r'\[own:[a-z-]+\]' for r in rx[s]), f'{s}: fails on any [own:...] tag')
        for what, line in HISTORICAL.items():
            check(bool(fails(rx[s], line)), f'{s}: an untagged {what} line fails it')
        made = tagged_formats()
        for tag, line in made:
            check(bool(fails(rx[s], line)), f'{s}: a [own:{tag}] line fails it ({line[:90]})')
    tags = {t for t, _ in tagged_formats()}
    want = {'free-blocked', 'free-foreign', 'free-double', 'realloc-foreign', 'ui-free', 'destroy-foreign', 'protected',
            'guard-write', 'not-own', 'boot-pointer', 'stale-left', 'cleanup-refused', 'vector-free', 'stale-child'}
    check(want <= tags, f'every ownership kind is tagged in the sources (missing {sorted(want - tags)})')
    docs = open(os.path.join(ROOT, 'docs', 'native_splitscreen.md')).read()
    check(all(f'[own:{t}]' in docs for t in tags), f'the docs list every tag ({sorted(t for t in tags if f"[own:{t}]" not in docs)})')
    # the twelfth run's line, named: the leave scenario of b6de6e0 passed it; now it fails
    old = re.compile(r'native split: (player [0-9]|the process) (frees|would free) block')
    check(not old.search(HISTORICAL['run 12: a blocked foreign free']), 'the old pattern missed the twelfth run\'s line')
    check(bool(fails(rx['native_split_2p_leave.txt'], HISTORICAL['run 12: a blocked foreign free'])),
          'the leave scenario now fails on the twelfth run\'s line')
    # docs 15.17: the leave scenario asks for the detach, and fails on a viewport kept on its parent
    leave = open(os.path.join(ROOT, 'tests', 'scenarios', 'native_split_2p_leave.txt')).read()
    check(re.search(r"^expect log native split: player 2's viewport 0x\[0-9A-F\]\+ detached from the process's panel",
                    leave, re.M) is not None, 'the leave scenario expects player 2\'s viewport detached')
    kept = P + "native split: player 2's viewport 0x02B99870 not detached: not among its parent's children (parent 0x00909CC0 of the process, 3 of 4 children, listed 0)"
    check(bool(fails(rx['native_split_2p_leave.txt'], kept)), 'a viewport not detached fails the leave scenario')
    for line in PASSING:
        check(not fails(rx['native_split_2p_leave.txt'], line), f'a passing leave\'s line does not fail it: {line[:80]}')
    print(f'scenario_ownership_test: {checks} checks, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
