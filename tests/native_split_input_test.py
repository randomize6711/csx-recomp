#!/usr/bin/env python3
"""Tests native split-screen M4's input without the game (docs/native_splitscreen.md 18.2-18.4):
  1. the controller seats (src/common/pad_seats.h): builds and runs tests/native_split_input_test.cpp (clang++,
     sanitizers);
  2. every key's state per player (18.2), against the generated code: Key_Event 0x66510 is the only function naming
     keydown[] 0x6CAFE0 and key_repeats[] 0x6CDBE0; its only caller is the key trap 0xAEEE0 (the pad bridge's keys);
     the repeat check that drops a press; both tables in the per-player regions at 0x400 bytes, between keybindings[]
     0x6CABE0 and 0x6CB3E0 (which stay shared: the bindings are the process's);
  3. the window's input (src/linux/window_input.cpp): native split-screen's ports read the seats (no connection-order
     port mapping left), the rumble goes to the seat's controller, a disconnected seat's port stays connected.
Needs build/gen/game for 2 (skipped with a note without it). No game is started.

Usage: python3 -I tests/native_split_input_test.py
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, 'build', 'gen', 'game')
failures = []
checks = 0


def check(ok, what):
    global checks
    checks += 1
    if not ok:
        failures.append(what)
        print('FAIL', what)


def seats():
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, 'native_split_input_test')
        build = subprocess.run(['clang++', '-std=c++20', '-O1', '-g', '-Wall', '-Werror', '-fsanitize=address,undefined',
                                '-I', os.path.join(ROOT, 'src'), os.path.join(ROOT, 'tests', 'native_split_input_test.cpp'),
                                '-o', exe])
        check(build.returncode == 0, 'the seats test builds')
        if build.returncode == 0:
            check(subprocess.run([exe]).returncode == 0, 'the seats test passes')


def load():
    funcs = {}
    for f in sorted(glob.glob(os.path.join(GEN, 'x86_recomp.*.cpp'))):
        t = open(f).read()
        for m in re.finditer(r'X86_FUNC\(__imp__sub_([0-9A-F]{8})\)', t):
            j = t.find('X86_WEAK_FUNC', m.end())
            body = t[m.end():j if j > 0 else None]
            ins = [(int(a, 16), s) for a, s in re.findall(r'// ([0-9A-F]{8}) (.*)', body)]
            fn = int(m[1], 16)
            if fn not in funcs or len(ins) > len(funcs[fn]):
                funcs[fn] = ins
    return funcs


def regions():
    r = []
    inc = open(os.path.join(ROOT, 'src', 'game', 'client_context_regions.inc')).read()
    r += [(int(a, 16), int(s, 16)) for a, s in re.findall(r'\{0x([0-9A-F]+), 0x([0-9A-F]+), "', inc)]
    return r


def key_tables():
    if not glob.glob(os.path.join(GEN, 'x86_recomp.*.cpp')):
        print('note: build/gen/game missing: the key tables\' code checks skipped')
        return
    funcs = load()
    key_event = 0x66510
    for table, name in ((0x6CAFE0, 'keydown[]'), (0x6CDBE0, 'key_repeats[]')):
        pat = re.compile(r'0x0*%X\b' % table, re.I)
        users = {fn for fn, ins in funcs.items() for a, s in ins if pat.search(s)}
        check(users == {key_event}, f'{name} named only by Key_Event (users {sorted(hex(u) for u in users)})')
    callers = {(fn, a) for fn, ins in funcs.items() for a, s in ins if re.match(r'(call|jmp) 0x00066510$', s)}
    check({fn for fn, _ in callers} == {0xAEEE0}, f'Key_Event\'s only direct caller is the key trap 0xAEEE0 ({callers})')
    ke = dict(funcs.get(key_event, []))
    check(ke.get(0x6653A) == 'mov [ebp*4+0x6CAFE0], esi', 'keydown[key] = down at 0x6653A')
    check(ke.get(0x6658A) == 'mov [ebp*4+0x6CDBE0], edi', 'key_repeats[key] counted at 0x6658A')
    check(ke.get(0x665BD) == 'cmp eax, 0x01' and ke.get(0x665C0) == 'jnle 0x00066888',
          'a count above 1 drops the press (0x665BD..0x665C0)')
    check(ke.get(0x6654B) == 'mov eax, [ebp*4+0x6CABE0]', 'the bindings indexed by the same key')
    regs = regions()
    for lo, name in ((0x6CAFE0, 'keydown[]'), (0x6CDBE0, 'key_repeats[]')):
        check((lo, 0x400) in regs, f'{name} a per-player region of 0x400 bytes')
    inside = lambda va: any(a <= va < a + n for a, n in regs)
    check(not inside(0x6CABE0) and not inside(0x6CB3E0) and not inside(0x6CB7E0),
          'keybindings[], consolekeys and menubound stay the process\'s')


def menu_hooks():
    """docs 18.5 / 18.6: every hook of the own menus sits on the instruction its handler assumes"""
    if not glob.glob(os.path.join(GEN, 'x86_recomp.*.cpp')):
        print('note: build/gen/game missing: the menu hooks\' checks skipped')
        return
    funcs = load()
    ins = {a: t for f in funcs.values() for a, t in f}
    expect = {
        0x19E0B1: ('call 0x000CB170', 0x19E0B6, 'NativeSplitMenuPress'),
        0x19E10C: ('call 0x000CB540', 0x19E111, 'NativeSplitMenuRelease'),
        0x55F94: ('call 0x000CD7D0', 0x55F99, 'NativeSplitMenuDraw'),
        0x1462E1: ('mov dword ptr [0x0037CA8C], 0xFA0', 0x1462EB, 'NativeSplitRestartTimer'),
        0x1462F5: ('mov byte ptr [0x0055234C], 0x00', 0x1462FC, 'NativeSplitRestartBuyOpen'),
        0xD73B8: ('call 0x000DAD40', 0xD73BD, 'NativeSplitUiSaveProfile'),
    }
    toml = open(os.path.join(ROOT, 'config', 'game.toml')).read()
    for at, (text, after, name) in expect.items():
        check(ins.get(at) == text, f'0x{at:X} is "{text}" ({ins.get(at)})')
        nxt = min((a for a in ins if a > at), default=None)
        check(nxt == after, f'0x{at:X}: the next instruction is 0x{after:X} (0x{nxt or 0:X})')
        m = re.search(r'\[\[hook\]\]\nat = 0x%X\ncall = "([^"]+)"\non_true = 0x([0-9A-F]+)' % at, toml)
        check(m is not None and m.group(1) == name and int(m.group(2), 16) == after,
              f'the hook at 0x{at:X}: {name}, on_true 0x{after:X}')
    check(ins.get(0xC9FC0) == 'mov eax, [0x0074A27C]' and re.search(r'at = 0xC9FC0\ncall = "NativeSplitUiPause"\non_true = "ret"', toml),
          'the menus\' pause hooked at its entry (al 0, ret)')
    check(ins.get(0xD72A0) is not None and re.search(r'at = 0xD72A0\ncall = "NativeSplitGuestPause"\n\n', toml),
          'the guest pause menu redirect at the screen open\'s entry (ecx its name)')
    d = dict(funcs.get(0xD9440, []))
    check(any(t.startswith('call 0x000D72A0') for t in d.values()), 'xbui open reaches 0xD72A0')


def window_input():
    src = open(os.path.join(ROOT, 'src', 'linux', 'window_input.cpp')).read()
    check('seats::Table' in src, 'window_input keeps the seats')
    poll = src[src.find('Pad Poll(int port) {'):]
    poll = poll[:poll.find('\n}\n')]
    check('g_start_swallowed |= 1u << seat' in src and 'g_start_swallowed & (1u << port)' in poll and
          'p.buttons & ~16' in poll, 'the START that took a seat is kept from its port until released (run 23)')
    check('g_other_pads[size_t(port)' not in src, 'no connection-order port mapping left')
    m = re.search(r'void Rumble\(int port.*?\n}\n', src, re.S)
    check(m is not None and 'SeatPad' in m.group(0), 'the rumble goes to the seat\'s controller')


def main():
    seats()
    key_tables()
    menu_hooks()
    window_input()
    print(f'native_split_input_test: {checks} checks, {len(failures)} failed')
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
