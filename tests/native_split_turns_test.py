#!/usr/bin/env python3
"""Checks against the generated code the facts docs/native_splitscreen.md 15.10's two corrections rest on (the eighth
run: player 2 left at client state 4; 126 cross-player frees in the shared scoreboard):
  1. the activation check 0x269A0: its rule (state 3 or 4, cls.signon == 2 -> 5; the image's only write of 5 into
     cls.state), its only call at 0x5D3A6 and CL_UpdateClientData 0x1CC10's at 0x5D3B0, both before the input turns
     (0x5D3B5); what they write (the player's own regions, or the balanced client-call flag 0x7D6D4C); the turn-start
     hook ClientContextsMoveBegin at 0x5D3B5, run at every loop-back of the input turns (0x5D3C2's on_false);
  2. the shared front end: the scoreboard's rows are the shared "Scoreboard" screen's (0x1B91E0 finds it by name in the
     screen list 0x5523D0, which no player's region holds); 0x1B9820 is called only at 0x1B935A, after the update's
     per-player work, and is stdcall with one argument; the eighth run's five free sites are free calls in 0xD5550,
     0xDA100, 0xDD4D0, 0xDD520, all reached from 0x1B9820, and the client game's only direct caller of 0xD5550 and
     0xDD520 is 0x1B9820; 0xCA6F0 queues "xbui %s" to the shared console buffer (Cbuf_AddText 0x3B970, 0x748660);
     0xCA0B0 closes the shared screen stack's screen; 0xD96A0 sets an element's text (stdcall, ret 4 at 0xD96B5);
     every gate's hook is in config/game.toml and in the generated code, with the skip target it needs.
  3. the lifecycle (15.12): the console buffer 0x748660 touched only by its init 0x3B900, Cbuf_AddText 0x3B970 (edi),
     Cbuf_InsertText 0x3B9D0 (eax), the wrapped insert 0x3BA80 (its stack argument), Cbuf_Execute 0x3BB50 and the
     exec command 0x3BF60, none of it a player's region; Cbuf_Execute's per-frame call at 0x5D3AB; the "xbui" command
     0xD7450 as registered; CL_Disconnect's stop-everything call at 0x28F4F; svc_disconnect's Host_EndGame call at
     0x30932 and its caller's `add esp, 0xC`; the voice channels 0x3855B8 touched only by the voice module, Voice_Idle
     called only at 0x5D46C in the client half, the channel and data calls of CL_ParseVoiceData; the two setjmp sites
     and their `test eax, eax`; every lifecycle hook in config/game.toml (NativeSplitCbufExecute before PcKeysFrame at
     0x3BB50) and in the generated code.
Needs build/gen/game (your own XBE); without it the checks are skipped with a note. No game is started.

Usage: python3 -I tests/native_split_turns_test.py
"""
import glob
import os
import re
import sys

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


def load():
    """every function's original instructions [(address, text)] and the generated text"""
    funcs, text = {}, ''
    for f in sorted(glob.glob(os.path.join(GEN, 'x86_recomp.*.cpp'))):
        t = open(f).read()
        text += t
        for m in re.finditer(r'X86_FUNC\(__imp__sub_([0-9A-F]{8})\)', t):
            j = t.find('X86_WEAK_FUNC', m.end())
            body = t[m.end():j if j > 0 else None]
            ins = [(int(a, 16), s) for a, s in re.findall(r'// ([0-9A-F]{8}) (.*)', body)]
            fn = int(m[1], 16)
            if fn not in funcs or len(ins) > len(funcs[fn]):
                funcs[fn] = ins
    return funcs, text


def callers(funcs):
    out = {}
    for fn, ins in funcs.items():
        for a, s in ins:
            m = re.match(r'(call|jmp) 0x([0-9A-F]{8})$', s)
            if m:
                t = int(m[2], 16)
                if m[1] == 'call' or (t in funcs and t != fn):
                    out.setdefault(t, set()).add((fn, a))
    return out


def regions():
    """the per-player regions: client_context.cpp's hand list and the generated table"""
    r = []
    src = open(os.path.join(ROOT, 'src', 'game', 'client_context.cpp')).read()
    hand = src[src.index('std::vector<ctx::Region> r = {'):src.index('// the rest, from the audit')]
    r += [(int(a, 16), int(s, 16)) for a, s in re.findall(r'\{0x([0-9A-F]+), 0x([0-9A-F]+), "', hand)]
    inc = open(os.path.join(ROOT, 'src', 'game', 'client_context_regions.inc')).read()
    r += [(int(a, 16), int(s, 16)) for a, s in re.findall(r'^\{0x([0-9A-F]+), 0x([0-9A-F]+),', inc, re.M)]
    return r


def in_regions(va, regs):
    return any(a <= va < a + s for a, s in regs)


def hooks():
    t = open(os.path.join(ROOT, 'config', 'game.toml')).read()
    out = {}
    for block in t.split('[[hook]]')[1:]:
        at = re.search(r'^at = (0x[0-9A-F]+)', block, re.M)
        call = re.search(r'^call = "([^"]+)"', block, re.M)
        if not at or not call:
            continue
        on_true = re.search(r'^on_true = (\S+)', block, re.M)
        on_false = re.search(r'^on_false = (\S+)', block, re.M)
        out[int(at[1], 16)] = (call[1], on_true and on_true[1].strip('"'), on_false and on_false[1].strip('"'))
    return out


def reach(funcs, root):
    seen, todo = set(), [root]
    while todo:
        f = todo.pop()
        if f in seen or f not in funcs:
            continue
        seen.add(f)
        for _, s in funcs[f]:
            m = re.match(r'(?:call|jmp) 0x([0-9A-F]{8})$', s)
            if m and int(m[1], 16) != f:
                todo.append(int(m[1], 16))
    return seen


def ins_at(funcs, fn, at):
    return dict(funcs.get(fn, [])).get(at)


def abs_writes(ins):
    """the absolute addresses an instruction list writes (mov/and/or/inc/... [0xADDR], ...)"""
    out = []
    for a, s in ins:
        m = re.match(r'(mov|add|sub|and|or|xor|inc|dec|fstp|fst|fistp)\s+(?:dword ptr |word ptr |byte ptr )?\[0x([0-9A-F]{8})\]', s)
        if m:
            out.append((a, int(m[2], 16)))
    return out


def activation(funcs, cl, regs, hk, text):
    f = funcs.get(0x269A0, [])
    body = [s for _, s in f]
    check(body[:7] == ['mov eax, [0x007812E0]', 'cmp eax, 0x03', 'jz 0x000269AF', 'cmp eax, 0x04', 'jnz 0x000269DE',
                       'cmp dword ptr [0x00784800], 0x02', 'jnz 0x000269DE'],
          f'0x269A0: state 3 or 4 and cls.signon == 2, else nothing ({body[:7]})')
    check(ins_at(funcs, 0x269A0, 0x269BE) == 'mov dword ptr [0x007812E0], 0x05', '0x269A0 sets cls.state 5 at 0x269BE')
    fives = [(fn, a) for fn, ins in funcs.items() for a, s in ins if s == 'mov dword ptr [0x007812E0], 0x05']
    check({a for _, a in fives} == {0x269BE}, f'the image\'s only write of 5 into cls.state is 0x269BE ({fives})')
    w = abs_writes(f)
    check([v for _, v in w] == [0x7812E0] and in_regions(0x7812E0, regs), f'0x269A0 writes only cls.state ({w})')
    check({a for _, a in cl.get(0x269A0, ())} == {0x5D3A6}, f'0x269A0\'s only call is 0x5D3A6 ({cl.get(0x269A0)})')
    check({a for _, a in cl.get(0x1CC10, ())} == {0x5D3B0}, f'0x1CC10\'s only call is 0x5D3B0 ({cl.get(0x1CC10)})')
    check(ins_at(funcs, 0x1CC10, 0x1CC1C) == 'cmp dword ptr [0x007812E0], 0x05', '0x1CC10 runs only in state 5')
    w = abs_writes(funcs.get(0x1CC10, []))
    shared = [hex(v) for _, v in w if not in_regions(v, regs) and v != 0x7D6D4C]
    flag = [s for _, s in funcs.get(0x1CC10, []) if '[0x007D6D4C]' in s]
    check(not shared and flag == ['mov dword ptr [0x007D6D4C], 0x01', 'mov dword ptr [0x007D6D4C], 0x00'],
          f'0x1CC10 writes its player\'s regions, and the client-call flag set and cleared ({shared}, {flag})')
    hf = dict(funcs.get(0x5D2E0, []))
    check(hf.get(0x5D3A6) == 'call 0x000269A0' and hf.get(0x5D3B0) == 'call 0x0001CC10' and
          hf.get(0x5D3B5, '').startswith('cmp [0x005EF7C0]'), '_Host_Frame: the prelude just before the input turns')
    check(hk.get(0x5D3B5, (None,))[0] == 'ClientContextsMoveBegin', 'the input turn\'s start hook: ClientContextsMoveBegin')
    check(hk.get(0x5D3C2) == ('ClientContextsMoveNext', None, '0x5D3B5'), 'the input turns loop back to 0x5D3B5')
    check(hk.get(0x5D3F0, (None,))[0] == 'ClientContextsTurnBegin', 'the client half\'s start hook unchanged')
    m = re.search(r'loc_0005D3B5:\n\tX86_SAVE_FLAGS\(\);\n\tClientContextsMoveBegin\(ctx, base\);', text)
    check(bool(m), 'the generated code runs ClientContextsMoveBegin at loc_0005D3B5 (every player\'s turn start)')
    src = open(os.path.join(ROOT, 'src', 'game', 'native_split.cpp')).read()
    check('kActivationCheck = 0x269A0' in src and 'kUpdateClientData = 0x1CC10' in src and
          'nsplit::PreludeInTurn(c)' in src, 'the prelude calls the real functions, for contexts after the first only')
    check('W32(kClsState, 5' not in src and 'W32(kClsState, kClsActive' not in src and 'kSignonsComplete)' not in src,
          'nothing sets cls.state 5 or the signon by hand')


def shared_ui(funcs, cl, regs, hk, text):
    b = dict(funcs.get(0x1B91E0, []))
    check(b.get(0x1B921F) == 'mov edi, 0x33B188' and b.get(0x1B9224) == 'call 0x000D71E0' and
          b.get(0x1B922E) == 'mov eax, [eax+0x384]' and b.get(0x1B9234) == 'mov [esi+0xBC], eax',
          'the scoreboard panel\'s +0xBC: the "Scoreboard" screen\'s list, found by name (0x1B91E0)')
    d = [s for _, s in funcs.get(0xD71E0, [])]
    check(d[:2] == ['push esi', 'mov esi, [0x005523D0]'] and 'mov esi, [esi+0x378]' in d,
          '0xD71E0 walks the screen list 0x5523D0')
    check(not in_regions(0x5523D0, regs) and not in_regions(0x5523D4, regs) and not in_regions(0x5523D8, regs),
          'the screen list and stack (0x5523D0..0x5523D8) are in no player\'s region: shared')
    check({a for _, a in cl.get(0x1B9820, ())} == {0x1B935A}, f'0x1B9820\'s only call is 0x1B935A ({cl.get(0x1B9820)})')
    check(funcs.get(0x1B9820, [(0, '')])[-1][1] == 'ret 0x04', '0x1B9820 is stdcall with one argument (the panel)')
    u = dict(funcs.get(0x1B92F0, []))
    check(u.get(0x1B9304) == 'call 0x001B4E30' and u.get(0x1B9354) == 'call 0x001B9370' and
          u.get(0x1B9359) == 'push esi' and u.get(0x1B935A) == 'call 0x001B9820' and u.get(0x1B935F) == 'pop esi',
          'the update\'s per-player work (0x1B4E30, 0x1B9370) before the gated push/call; 0x1B935F balances it')
    reached = reach(funcs, 0x1B9820)
    for site, fn in ((0xD557A, 0xD5550), (0xD55AC, 0xD5550), (0xDA123, 0xDA100), (0xDD4FA, 0xDD4D0), (0xDD54C, 0xDD520)):
        ins = funcs.get(fn, [])
        prev = [s for a, s in ins if a < site]
        check(prev and prev[-1] == 'call 0x0022C609' and fn in reached,
              f'free site 0x{site:X}: a free in 0x{fn:X}, reached from 0x1B9820')
    for t in (0xD5550, 0xDD520):
        client = {f for f, _ in cl.get(t, ()) if 0x18B000 <= f < 0x1E0000}
        check(client == {0x1B9820}, f'the client game\'s only caller of 0x{t:X} is 0x1B9820 ({sorted(map(hex, client))})')
    c = [s for _, s in funcs.get(0xCA6F0, [])]
    check('push 0x33D9B4' in c and 'call 0x0003B970' in c, '0xCA6F0 queues "xbui %s" with Cbuf_AddText 0x3B970')
    q = [s for _, s in funcs.get(0x3B970, [])]
    check('push 0x748660' in q and not in_regions(0x748660, regs), 'Cbuf_AddText appends to 0x748660: shared')
    s = [x for _, x in funcs.get(0xCA0B0, [])]
    check(s[:1] == ['mov eax, [0x005523D8]'] and 'mov [0x005523D8], eax' in s, '0xCA0B0 changes the shared screen stack')
    check(ins_at(funcs, 0x1BA710, 0x1BA732) == 'call 0x000CA0B0' and ins_at(funcs, 0x1BA710, 0x1BA737) ==
          'mov byte ptr [0x0055234C], 0x00', '0x1BA732 calls 0xCA0B0; 0x1BA737 follows it')
    check(ins_at(funcs, 0xD96A0, 0xD96B5) == 'ret 0x04', '0xD96A0 is stdcall with one argument: its ret 4 at 0xD96B5')
    want = {0x1B9359: ('NativeSplitScoreboardRows', '0x1B935F', None), 0xD96A0: ('NativeSplitUiText', '0xD96B5', None),
            0xCAB40: ('NativeSplitUiMenu', 'ret', None), 0x1BA732: ('NativeSplitUiCloseScreen', '0x1BA737', None),
            0xCA6F0: ('NativeSplitUiCommand', 'ret', None), 0xDD520: ('NativeSplitUiElements', None, None),
            0xD5550: ('NativeSplitUiElements', None, None)}
    for at, w in want.items():
        check(hk.get(at) == w, f'hook at 0x{at:X}: {w} ({hk.get(at)})')
        check(f'{w[0]}(ctx, base)' in text, f'{w[0]} is in the generated code')
    for target in ('loc_001B935F', 'loc_000D96B5', 'loc_001BA737'):
        check(f'goto {target};' in text and f'{target}:' in text, f'the skip target {target} exists')


def all_hooks():
    """every hook in config order: [(at, call, on_true)]"""
    t = open(os.path.join(ROOT, 'config', 'game.toml')).read()
    out = []
    for block in t.split('[[hook]]')[1:]:
        at = re.search(r'^at = (0x[0-9A-F]+)', block, re.M)
        call = re.search(r'^call = "([^"]+)"', block, re.M)
        on_true = re.search(r'^on_true = (\S+)', block, re.M)
        if at and call:
            out.append((int(at[1], 16), call[1], on_true and on_true[1].strip('"')))
    return out


def lifecycle(funcs, cl, regs, text):
    # the console buffer: who touches cmd_text (0x748660..0x748674)
    users = set()
    for fn, ins in funcs.items():
        for a, s in ins:
            for m in re.findall(r'0x00?(74866[0-9A-F]|748670)\b', s):
                users.add(fn)
    check(users == {0x3B900, 0x3B970, 0x3B9D0, 0x3BA80, 0x3BB50, 0x3BF60},
          f'cmd_text is touched only by the buffer\'s own functions ({sorted(hex(u) for u in users)})')
    check(not any(in_regions(v, regs) for v in (0x748640, 0x748660, 0x748668, 0x748670)),
          'cmd_text and cmd_wait are shared (no player region)')
    check(funcs[0x3B970][0][1] == 'mov cl, [edi]', 'Cbuf_AddText reads its text from edi')
    check(ins_at(funcs, 0x3B9D0, 0x3B9D8) == 'mov edi, eax', 'Cbuf_InsertText takes its text in eax')
    check(ins_at(funcs, 0x3BA80, 0x3BA81) == 'mov ebp, [esp+0x08]' and funcs[0x3BA80][0][1] == 'push ebp',
          'the wrapped insert takes a stack argument ([esp+4] at its entry)')
    check(ins_at(funcs, 0x3BA80, 0x3BAEC) == 'mov edi, 0x329E28' and ins_at(funcs, 0x3BA80, 0x3BAF6) == 'mov edi, ebp',
          'it inserts newline, the text, newline')
    check(ins_at(funcs, 0x5D2E0, 0x5D3AB) == 'call 0x0003BB50', 'Cbuf_Execute once a frame at 0x5D3AB, before the turns')
    check(ins_at(funcs, 0xD7710, 0xD7724) == 'push 0xD7450' or 'push 0xD7450' in [s for _, s in funcs.get(0xD7710, [])],
          'the xbui command is 0xD7450 (Cmd_AddCommand in 0xD7710)')
    # the disconnect
    check(ins_at(funcs, 0x28F20, 0x28F37) == 'mov ebx, 0x553ED8' and ins_at(funcs, 0x28F20, 0x28F4F) == 'call 0x00106380' and
          ins_at(funcs, 0x28F20, 0x28F54) is not None, 'CL_Disconnect stops the one sound object\'s sounds at 0x28F4F')
    check(ins_at(funcs, 0x30910, 0x3092D) == 'push 0x32CF90' and ins_at(funcs, 0x30910, 0x30932) == 'call 0x0005BC40' and
          ins_at(funcs, 0x30910, 0x30937) == 'add esp, 0x0C', 'svc_disconnect calls Host_EndGame at 0x30932; 0x30937 pops')
    check(ins_at(funcs, 0x290D0, 0x290E0) == 'call 0x0005C9D0' and ins_at(funcs, 0x290D0, 0x290D0) == 'call 0x00028F20',
          'the disconnect command: CL_Disconnect, then Host_ShutdownServer')
    # the voice channels
    def users(v):
        return {fn for fn, ins in funcs.items() if any(re.search(r'0x0*' + v + r'\b', s) for _, s in ins)}
    vusers = users('3855B8')
    check(vusers == {0x101E70, 0x101FD0, 0x102620, 0x1026A0, 0x102760},
          f'the voice channels 0x3855B8 are the voice module\'s ({sorted(hex(u) for u in vusers)})')
    check(users('38AC08') == {0x101EB0}, 'their codecs (+0x5650, 0x38AC08) are released only by 0x101EB0')
    check({f for f, _ in cl.get(0x101EB0, ())} == {0x101FD0, 0x5DDA0}, 'by Voice_Idle and Host_Shutdown (0x5DDA0) only')
    check({a for _, a in cl.get(0x101FD0, ())} == {0x5D46C} and 0x5D3F0 < 0x5D46C < 0x5D54A,
          'Voice_Idle is called only at 0x5D46C, in the players\' client half')
    check({a for _, a in cl.get(0x102620, ())} >= {0x31C8F} and {a for _, a in cl.get(0x102760, ())} >= {0x31CA0},
          'CL_ParseVoiceData assigns a channel and adds its data')
    check(not in_regions(0x3855B8, regs), 'the voice channels are shared (no player region)')
    # the setjmp sites
    for fn, call, at in ((0x5D2E0, 0x5D2EC, 0x5D2F4), (0x5D5D0, 0x5D5E8, 0x5D5F0)):
        check(ins_at(funcs, fn, call) == 'call 0x0022EC0C' and ins_at(funcs, fn, at) == 'test eax, eax',
              f'the setjmp at 0x{call:X} and its test at 0x{at:X}')
    hl = all_hooks()
    want = [(0x3B970, 'NativeSplitCbufAdd', 'ret'), (0x3B9D0, 'NativeSplitCbufInsert', 'ret'),
            (0x3BA80, 'NativeSplitCbufInsertLine', 'ret'), (0x3BB50, 'NativeSplitCbufExecute', 'ret'),
            (0xD7450, 'NativeSplitUiXbui', 'ret'), (0x28F4F, 'NativeSplitDisconnectSound', '0x28F54'),
            (0x30932, 'NativeSplitServerDisconnected', '0x30937'), (0x101FD0, 'NativeSplitVoiceIdle', 'ret'),
            (0x102620, 'NativeSplitVoiceAssign', 'ret'), (0x102760, 'NativeSplitVoiceData', 'ret'),
            (0x5C9D0, 'NativeSplitShutdownServer', 'ret'), (0x5D2F4, 'ClientContextsAbortLanding', None),
            (0x5D5F0, 'ClientContextsAbortLanding', None)]
    for w in want:
        check(w in hl, f'hook {w}')
        check(f'{w[1]}(ctx, base)' in text, f'{w[1]} is in the generated code')
    at = [c for a, c, _ in hl if a == 0x3BB50]
    check(at[:2] == ['NativeSplitCbufExecute', 'PcKeysFrame'], f'at 0x3BB50 the routing runs before PcKeysFrame ({at})')
    for target in ('loc_00028F54', 'loc_00030937'):
        check(f'goto {target};' in text and f'{target}:' in text, f'the skip target {target} exists')


def main():
    if not os.path.isdir(GEN) or not glob.glob(os.path.join(GEN, 'x86_recomp.*.cpp')):
        print('note: build/gen/game missing: the checks are skipped')
        print('native_split_turns_test: 0 checks, 0 failed')
        return 0
    funcs, text = load()
    cl = callers(funcs)
    regs = regions()
    hk = hooks()
    activation(funcs, cl, regs, hk, text)
    shared_ui(funcs, cl, regs, hk, text)
    lifecycle(funcs, cl, regs, text)
    print(f'native_split_turns_test: {checks} checks, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
