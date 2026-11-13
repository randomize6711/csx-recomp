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
  4. the shared-state audit (15.14): the pad bridge's loop bound 0x543A04 (IN_Init's 8 never read: the bridge stores
     16 before its only read) and its button mask 0x5439B8, written only by IN_Init and the bridge, IN_Init reached only
     from InitInput's 0x19E660, the two stores' hooks; the health HUD's damage icon size 0x548F34 / 0x548F38 written
     only by its Init and VidInit, a player's region; the string-intern pool's search key 0x5622B0 written only by the
     intern before its find, read only by the less functions; the console ring 0x744060 (201 lines of 0x4E) and its
     positions 0x551914..0x55191C named only by the console's code, in no player region and not protected.
  5. the same-frame reconnect (15.15): CL_CheckForResend 0x298F0 called only at 0x5D487, in the client turn after the
     input turn a queued "disconnect" runs in; its state-1 branch (sv.active: "localhost", state 2, the connect packet
     0x29310); every store of 2 into cls.state and how it is reached (the "connect" and "listen" commands, the multicast
     reply), "retry" only a console line, "reconnect" idle below state 3, the connect packet's four callers; the
     departure's hook after CL_Disconnect's store of 1; Cbuf_Execute's size re-read after each line; the two gates.
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


def refs(funcs, lo, hi):
    """every instruction naming an address in [lo, hi): {(fn, at, text)}"""
    out = set()
    for fn, ins in funcs.items():
        for a, s in ins:
            for m in re.findall(r'0x([0-9A-F]{6,8})\b', s):
                if lo <= int(m, 16) < hi:
                    out.add((fn, a, s))
    return out


def protected():
    inc = open(os.path.join(ROOT, 'src', 'game', 'client_context_protected.inc')).read()
    return [(int(a, 16), int(n, 16)) for a, n in re.findall(r'^\{0x([0-9A-F]+), 0x([0-9A-F]+),', inc, re.M)]


def shared_audit(funcs, cl, regs, text):
    """docs 15.14: the tenth run's four unexplained shared changes and the console ring, against the generated code"""
    prot = protected()
    every = {}
    for fn, ins in funcs.items():
        for a, s in ins:
            every.setdefault(a, s)
    # 1. the pad bridge's loop bound 0x543A04: IN_Init's 8 is never read
    r = {(a, s) for _, a, s in refs(funcs, 0x543A04, 0x543A08)}
    check(r == {(0x19DF25, 'mov dword ptr [0x00543A04], 0x08'), (0x19E067, 'mov dword ptr [0x00543A04], 0x10'),
                (0x19E13A, 'mov eax, [0x00543A04]')},
          f'0x543A04: IN_Init stores 8, the bridge stores 16 and reads it, nothing else ({sorted(hex(a) for a, _ in r)})')
    body = dict(funcs.get(0x19DFD0, []))
    early = [(a, x) for a, x in body.items() if a < 0x19E067]
    targets = [int(m, 16) for _, x in early for m in re.findall(r'^j[a-z]+ 0x([0-9A-F]{8})$', x)]
    check(body and all(t <= 0x19E067 for t in targets) and 0x19E067 < 0x19E13A in body,
          'the bridge 0x19DFD0 stores 16 on every path before its only read (no branch before 0x19E067 passes it)')
    check(not any(t in (0x19E070, 0x19E071) or 0x19E067 < t <= 0x19E13A for t in cl if t != 0x19DFD0),
          'nothing calls into the bridge past its entry')
    to_in_init = {a for a, x in every.items() if re.match(r'(call|jmp|j[a-z]+) 0x0019DF20$', x)}
    check(to_in_init == {0x19EA9E} and every.get(0x19EA9E) == 'jmp 0x0019DF20',
          f'IN_Init 0x19DF20 is reached only by the jump at 0x19EA9E (0x19E660\'s) ({sorted(hex(a) for a in to_in_init)})')
    check(every.get(0x190C0C) == 'call 0x0019E660', 'InitInput calls 0x19E660 at 0x190C0C')
    # 2. the bridge's previous button mask 0x5439B8
    w = {a for a, v in abs_writes([(a, s) for a, s in every.items()]) if v == 0x5439B8}
    check(w == {0x19DF43, 0x19E149}, f'0x5439B8 is written only by IN_Init and the bridge ({sorted(hex(a) for a in w)})')
    for at, want, nxt in ((0x19DF25, 'mov dword ptr [0x00543A04], 0x08', 0x19DF2F),
                          (0x19DF43, 'mov dword ptr [0x005439B8], 0x00', 0x19DF4D)):
        check(every.get(at) == want and nxt in every, f'IN_Init\'s store at 0x{at:X} ({want}); the next instruction 0x{nxt:X}')
    hl = all_hooks()
    for w in ((0x19DF25, 'NativeSplitPadBridgeStore', '0x19DF2F'), (0x19DF43, 'NativeSplitPadBridgeStore', '0x19DF4D')):
        check(w in hl, f'hook {w}')
    check('NativeSplitPadBridgeStore(ctx, base)' in text, 'NativeSplitPadBridgeStore is in the generated code')
    for target in ('loc_0019DF2F', 'loc_0019DF4D'):
        check(f'goto {target};' in text and f'{target}:' in text, f'the skip target {target} exists')
    for va in (0x543A04, 0x5439B8):
        check(not in_regions(va, regs), f'0x{va:X} is the process\'s (no player region)')
    # 3. the health HUD's damage icon size 0x548F34 / 0x548F38: a player's
    r = {(a, s) for _, a, s in refs(funcs, 0x548F34, 0x548F3C)}
    check({a for a, s in r if s.startswith('mov [')} == {0x1D3A32, 0x1D3A38, 0x1D3B14, 0x1D3B27},
          'giDmgHeight / giDmgWidth are written only by the health HUD\'s Init and VidInit')
    check({a for a, s in r if not s.startswith('mov [')} == {0x1D481F, 0x1D4951, 0x1D49B0},
          'and read only by its damage draw')
    check(every.get(0x1A6BC7) == 'mov dword ptr [eax+0x160], 0x35E7F4', 'gHUD+0x160 gets the health HUD\'s vtable')
    check(in_regions(0x548F34, regs) and in_regions(0x548F38, regs) and not in_regions(0x548F3C, regs) and
          not in_regions(0x548F30, regs), 'both are a player\'s region, exactly (the studio API before them stays the process\'s)')
    # 4. the string-intern pool's search key 0x5622B0 / 0x5622B4
    r = refs(funcs, 0x5622B0, 0x5622B8)
    wr = {a for _, a, s in r if s.startswith('mov [')}
    rd = {a for _, a, s in r if not s.startswith('mov [')}
    check(wr == {0x115D2E, 0x115D34}, f'the search key is written only by the intern 0x115D10 ({sorted(hex(a) for a in wr)})')
    check(rd == {0x115BA9, 0x115BB1, 0x115C29, 0x115C2F}, 'and read only by the two less functions')
    check(every.get(0x115D3A) == 'mov dword ptr [esp+0x0C], 0xFFFFFFFF' and every.get(0x115D42) == 'call 0x00115DC0',
          'the intern stores the key, then finds with the key node -1')
    check(not in_regions(0x5622B0, regs), 'the search key is shared (no player region)')
    # 5. the console's text ring and its positions: the one console's
    ring = refs(funcs, 0x744060, 0x747D9E)
    check({a for _, a, _ in ring} == {0x40650, 0x40665, 0x40950, 0x409DB, 0x40B79, 0x40E52},
          f'the ring is named only by Con_Init, its line add, the scroll, the draw and condump '
          f'({sorted(hex(a) for _, a, _ in ring)})')
    check(every.get(0x40665) == 'cmp eax, 0x747D50' and every.get(0x40662) == 'add eax, 0x4E',
          'Con_Init clears 200 lines of 0x4E bytes')
    check(every.get(0x40A0D) == 'cmp esi, 0xC8' and every.get(0x40A19) == 'jle 0x00040A2D',
          'the line add uses line 200 too (jle): 201 lines, to 0x747D9E')
    check(every.get(0x40885) == 'call 0x000409C0' and {a for _, a in cl.get(0x409C0, set())} == {0x40885},
          'the line add is called only by Con_Printf (0x40885)')
    check({a for _, a in cl.get(0x40650, set())} == {0x5DB33}, 'Con_Init is called only by Host_Init (0x5DB33)')
    w = {a for a, v in abs_writes([(a, s) for a, s in every.items()]) if v in (0x551914, 0x551918, 0x55191C)}
    check(all(0x40650 <= a < 0x40A40 for a in w) and len(w) == 13,
          f'the ring\'s positions are written only by the console\'s own code ({sorted(hex(a) for a in w)})')
    for lo, n in ((0x744060, 0x3D3E), (0x551914, 0xC)):
        check(not any(a < lo + n and lo < a + s for a, s in regs), f'0x{lo:X}+0x{n:X} is in no player region')
        check(not any(a < lo + n and lo < a + s for a, s in prot), f'0x{lo:X}+0x{n:X} is not protected')



def reconnect(funcs, cl, text):
    """docs 15.15: the eleventh run's same-frame reconnect and every way a client at cls.state 1 connects again"""
    order = dict(funcs.get(0x5D2E0, []))
    # CL_CheckForResend: called once a frame, in the client turn, after the input turn its disconnect ran in
    check(cl.get(0x298F0) == {(0x5D2E0, 0x5D487)}, f'CL_CheckForResend\'s only call: 0x5D487 ({cl.get(0x298F0)})')
    check(order.get(0x5D487) == 'call 0x000298F0' and order.get(0x5D4BC) == 'call 0x000B70D0',
          '_Host_Frame\'s client half: CL_CheckForResend at 0x5D487, 0xB70D0 at 0x5D4BC')
    hooks_at = {(a, c, t) for a, c, t in all_hooks()}
    check({(0x5D3B5, 'ClientContextsMoveBegin', None), (0x5D3F0, 'ClientContextsTurnBegin', None)} <= hooks_at,
          'the input turns start at 0x5D3B5 (its queued lines run there), the client turns at 0x5D3F0')
    check(0x5D3B5 < 0x5D3CD < 0x5D3F0 < 0x5D487 < 0x5D4BC < 0x5D54A and all(
        a in order for a in (0x5D3B5, 0x5D3CD, 0x5D3F0, 0x5D487, 0x5D4BC, 0x5D54A)),
          'the order: input turn (0x5D3B5), server frame (0x5D3CD), client turn (0x5D3F0) with 0x5D487 and 0x5D4BC, '
          'its loop-back (0x5D54A)')
    # its state-1 branch: disconnected with the server active -> "localhost", state 2, the connect packet
    want = {0x29904: 'mov eax, [0x007812E0]', 0x2990A: 'mov edi, 0x01', 0x2990F: 'cmp eax, edi',
            0x29911: 'jnz 0x00029986', 0x29913: 'mov eax, [0x005EF7C0]', 0x2991C: 'jz 0x00029CB5',
            0x2992C: 'mov dword ptr [0x007812E0], 0x02', 0x2996B: 'call 0x00029310', 0x29986: 'cmp eax, 0x02'}
    got = dict(funcs.get(0x298F0, []))
    check(all(got.get(a) == w for a, w in want.items()),
          'CL_CheckForResend: state 1 and sv.active -> state 2 and the connect packet; state 2 -> its retries')
    # every store of cls.state; the stores of 2 (a connect begun) are CL_CheckForResend's, "listen"'s and "connect"'s
    writes = {}
    for f, ins in funcs.items():
        for a, s in ins:
            if re.match(r'mov (?:dword ptr )?\[0x007812E0\], ', s):
                writes[a] = s.split(', ')[1]
    twos = {a for a, v in writes.items() if v == '0x02'}
    check(twos == {0x2992C, 0x27BC2, 0x29E1C}, f'cls.state = 2 only at 0x2992C, 0x27BC2, 0x29E1C ({sorted(map(hex, twos))})')
    check(cl.get(0x27AE0) == {(0x27BF0, 0x27C22), (0x27CE0, 0x27E4C)},
          '0x27AE0 (0x27BC2) reached by the "listen" command and the multicast reply only')
    regs_cmd = {}
    for f, ins in funcs.items():
        for i, (a, s) in enumerate(ins):
            if s == 'call 0x0003C760' and i >= 2:
                fn_ = re.match(r'push 0x([0-9A-F]+)$', ins[i - 2][1])
                if fn_:
                    regs_cmd[int(fn_[1], 16)] = a
    check(0x29CD0 in regs_cmd and 0x27BF0 in regs_cmd and 0x29170 in regs_cmd and 0x60680 in regs_cmd,
          '"connect" 0x29CD0, "listen" 0x27BF0, "retry" 0x29170, "reconnect" 0x60680: console commands (a held '
          'player\'s lines never run)')
    check(not cl.get(0x29CD0) and not cl.get(0x27BF0), 'the connect and listen commands have no direct caller')
    retry = funcs.get(0x29170, [])
    check(any(s == 'call 0x0003B970' for _, s in retry) and not any('0x007812E0' in s or s == 'call 0x00029310'
                                                                  for _, s in retry),
          '"retry" only adds a "connect" line (Cbuf_AddText): no state store, no connect packet')
    rc = dict(funcs.get(0x60680, []))
    check(rc.get(0x6069D) == 'jz 0x00060780' and rc.get(0x606A6) == 'jz 0x00060780' and
          rc.get(0x606AF) == 'jz 0x00060780', '"reconnect" does nothing at cls.state 0, 1 or 2')
    check(cl.get(0x29310) == {(0x298F0, 0x2996B), (0x298F0, 0x299D6), (0x27CE0, 0x27F1D), (0xB70D0, 0xB7130)},
          f'the connect packet 0x29310: CL_CheckForResend, the multicast reply, 0xB70D0 ({cl.get(0x29310)})')
    # the departure is recorded at 0x29029: after CL_Disconnect's store of state 1 (0x2901F), before the shutdown
    d = dict(funcs.get(0x28F20, []))
    check(d.get(0x2901F) == 'mov dword ptr [0x007812E0], 0x01' and d.get(0x29029) == 'jz 0x00029034' and
          d.get(0x2902C) == 'call 0x0005C9D0', 'CL_Disconnect: state 1 at 0x2901F, then 0x29029 (the hook), the shutdown')
    # Cbuf_Execute removes the line before running it and re-reads the size after: a size of 0 ends the run
    ce = dict(funcs.get(0x3BB50, []))
    check(ce.get(0x3BC32) == 'mov [0x00748670], ebx' and ce.get(0x3BC73) == 'call 0x0003CA10' and
          ce.get(0x3BC84) == 'mov ebx, [0x00748670]' and ce.get(0x3BC8C) == 'jnz 0x0003BB74',
          'Cbuf_Execute: the line taken out (0x3BC32), run (0x3BC73), the size re-read (0x3BC84)')
    # the two gates: in config/game.toml at the functions' entries, `ret` (no arguments: plain cdecl) in the code
    check((0x298F0, 'NativeSplitCheckResend', 'ret') in hooks_at and (0x29310, 'NativeSplitConnectPacket', 'ret')
          in hooks_at, 'the gates\' hooks at 0x298F0 and 0x29310 (on_true "ret")')
    check(got.get(0x298F0) == 'sub esp, 0x91C' and dict(funcs.get(0x29310, [])).get(0x29310) == 'push ebp',
          'both gated at their first instruction (nothing pushed yet)')
    for name in ('NativeSplitCheckResend', 'NativeSplitConnectPacket'):
        check(f'bool hook = {name}(ctx, base);' in text and
              re.search(re.escape(f'{name}(ctx, base);') + r'[^\n]*if \(hook\) \{ ctx\.esp \+= 4; return; \}', text),
              f'{name}: in the generated code, its on_true a return')
    check((0x29029, 'NativeSplitDisconnectServer', '0x29034') in hooks_at and
          (0x30932, 'NativeSplitServerDisconnected', '0x30937') in hooks_at,
          'the departure\'s hooks: CL_Disconnect 0x29029, svc_disconnect 0x30932')


def stale_slot(funcs):
    """docs 15.22: SV_DropClient (0x5C600) clears the three words ServerClientOf reads as a slot in use (+0, +4, +0xC):
    a dropped slot never names its port, so an add waits only for a slot no drop reached"""
    d = dict(funcs.get(0x5C600, []))
    stores = {a for a, s_ in d.items() if re.match(r'mov \[ebp(?:\+0x0[48C])?\], ebx$', s_)}
    check(d.get(0x5C62F) == 'xor ebx, ebx' and {0x5C70C, 0x5C71B, 0x5C70F} <= stores and
          not any(re.match(r'mov ebx, ', s_) or re.match(r'(pop|xchg) ebx', s_) for a, s_ in d.items() if 0x5C62F < a < 0x5C70C),
          'SV_DropClient: ebx 0 (0x5C62F), then +0, +4, +0xC of the client zeroed (0x5C70C, 0x5C71B, 0x5C70F)')
    src = open(os.path.join(ROOT, 'src', 'game', 'native_split.cpp')).read()
    sc = src[src.find('uint32_t ServerClientOf(int k) {'):]
    check('bool used = R32(cl) || R32(cl + 0x4) || R32(cl + 0xC);' in sc[:600], 'ServerClientOf reads +0, +4, +0xC')
    ap = src[src.find('void AddPlayer(X86Context& ctx, int k) {'):]
    check(0 <= ap.find('nsplit::AddWaitsForSlot(') < ap.find('g_ch.Forget(k);') < ap.find('csx::ClientContextAdd()'),
          'AddPlayer: the stale slot checked, its port emptied, before any context is made')


def player1_rejoin(funcs, cl, text):
    """docs 15.19: player 1 departs and rejoins in its kept context 0; what its own CL_Disconnect leaves for the
    reconnect (the server told, the connection's state cleared) and the code's order (src/game/native_split.cpp)"""
    d = dict(funcs.get(0x28F20, []))
    # connected: "dropclient\n" as a clc_stringcmd (3), sent three times on cls.netchan (0x7812E8) by Netchan_Transmit
    check(d.get(0x28FC6) == 'mov edx, 0x32BA5C' and d.get(0x28FCD) == 'mov byte ptr [esp+0x0C], 0x03' and
          all(d.get(a) == 'call 0x000698D0' for a in (0x28FEF, 0x29000, 0x29011)) and
          all(d.get(a) == 'push 0x7812E8' for a in (0x28FE8, 0x28FFB, 0x2900C)),
          'CL_Disconnect: the server told three times ("dropclient", clc_stringcmd, on cls.netchan 0x7812E8)')
    # then the connection's own state: cls.state 1, cls.signon 0, CL_ClearState, the netchan cleared
    check(d.get(0x2901F) == 'mov dword ptr [0x007812E0], 0x01' and d.get(0x29042) == 'mov [0x00784800], ebx' and
          d.get(0x29048) == 'call 0x00028E60' and d.get(0x29050) == 'mov esi, 0x7812E8' and
          d.get(0x29055) == 'call 0x00069630',
          'CL_Disconnect: state 1, cls.signon 0, CL_ClearState 0x28E60, Netchan_Clear 0x69630 (cls.netchan): a '
          'reconnect starts clean')
    ccs = dict(funcs.get(0x28E60, []))
    check(ccs.get(0x28E9F) == 'mov ecx, 0xA00' and ccs.get(0x28EA4) == 'rep stosd' and ccs.get(0x28E96) == 'mov eax, 0x787EE0',
          'CL_ClearState clears the client state (0x787EE0, 0x2800 bytes and more)')
    # the activation of a rejoined player 1: its context keeps the title's own prelude call (0x5D3A6)
    order = dict(funcs.get(0x5D2E0, []))
    check(order.get(0x5D3A6) == 'call 0x000269A0', 'player 1\'s activation 0x269A0 at 0x5D3A6, in its context')
    src = open(os.path.join(ROOT, 'src', 'game', 'native_split.cpp')).read()
    rj = src[src.find('bool RejoinPlayer1()'):]
    rj = rj[:rj.find('\n}\n')]
    check(rj.find('MayRejoin0(') >= 0 and rj.find('MayRejoin0(') < rj.find('g_players.Rejoin0()') and
          'g_ch.Forget(0)' in rj and 'g_cmds.Drop(0)' in rj and 'g_watch[0] = {}' in rj and 'CallGuest' not in rj,
          'RejoinPlayer1: the rule first; its watch, lines and packets cleared; no guest call (the title reconnects)')
    dp = src[src.find('void DepartPlayer1('):]
    dp = dp[:dp.find('\n}\n')]
    check('ServerClientOf(0)' in dp and 'CleanupFn::DropClient' in dp and dp.find('CleanupFn::DropClient') <
          dp.find('g_ch.Forget(0)') and 'RemovePlayer' not in dp and 'Released' not in dp,
          'DepartPlayer1: a slot still naming port 0 dropped, then its packets; its context never removed')
    fe = src[src.find('void NativeSplitFrameEnd('):]
    check(re.search(r'for \(int k = 0; k < nsplit::kMaxPlayers; k\+\+\) \{\s*const nsplit::Player& p = g_players.At\(k\);'
                    r'\s*if \(p.state != nsplit::State::Connecting\) continue;\s*if \(int\(PlayerDword\(k, kClsState\)\)', fe)
          is not None, 'the frame\'s end: a connecting player 1 (its rejoin) joins when its live cls.state is active')


def front_end_input(funcs, cl, regs, hk, text):
    """docs 17.9 (run 18): the shared front end's input, the title's code it rests on and the hooks that keep it the
    driver's: the look routine 0x19E160 copies the bridge's mask 0x5439B8 into the front end's snapshot 0x5523C0 /
    0x5523C4 and the active pad's left stick into 0x552DD0 / 0x552DD4 (0xCA670), the pie menu (0xCD470 / 0xCD680,
    under 0xCD7D0, the shared draw at 0x55F94) reads them; the bridge hands presses to 0xCB170 and releases to
    0xCB540. Player 2 connected while player 1 navigates: its controller must reach none of it"""
    look = dict(funcs.get(0x19E160, []))
    check(look.get(0x19E191) == 'mov esi, [0x003852F8]' and look.get(0x19E19D) == 'mov edx, [0x005523D4]' and
          look.get(0x19E1AE) == 'test edx, edx' and look.get(0x19E1BA) == 'mov ecx, [eax+0x384F90]' and
          look.get(0x19E1C0) == 'mov eax, [eax+0x384F8C]' and look.get(0x19E1C8) == 'call 0x000CA670',
          'the look routine: with a screen open, 0xCA670 gets the active pad\'s left stick (0x384F8C / 0x384F90)')
    check(look.get(0x19E1CD) == 'mov ecx, [0x005523C0]' and look.get(0x19E1D3) == 'mov eax, [0x005439B8]' and
          look.get(0x19E1DB) == 'mov [0x005523C4], ecx' and look.get(0x19E1E1) == 'mov [0x005523C0], eax',
          'the look routine: the front end\'s snapshot from the bridge\'s mask (0x5523C0 now, 0x5523C4 before)')
    check(look.get(0x19E1EA) == 'movzx eax, byte ptr [edx+0xC0]' and look.get(0x19E1F3) == 'jz 0x0019E642' and
          look.get(0x19E1F9) == 'test bl, bl' and look.get(0x19E1E8) == 'jz 0x0019E1F9',
          'the look routine: a screen that takes the controls stops the look; 0x19E1F9 the path without a screen')
    ca = dict(funcs.get(0xCA670, []))
    check(ca.get(0xCA678) == 'mov [0x00552DD0], eax' and ca.get(0xCA684) == 'mov [0x00552DD4], ecx' and
          ca.get(0xCA6AA) == 'mov eax, [0x005523C0]', '0xCA670: the menus\' direction, the D-pad from the snapshot')
    check({(f, a) for f, a in cl.get(0xCA670, set())} == {(0x19E160, 0x19E1C8)}, '0xCA670 called only by the look routine')
    # the only writers of the snapshot and the direction
    w = {}
    for f, ins in funcs.items():
        for a, v in abs_writes(ins):
            if v in (0x5523C0, 0x5523C4, 0x552DD0, 0x552DD4):
                w.setdefault(v, set()).add((f, a))
    check(w.get(0x5523C0) == {(0x19E160, 0x19E1E1)} and w.get(0x5523C4) == {(0x19E160, 0x19E1DB)},
          'the snapshot written only by the look routine (%s)' % {hex(k): sorted(hex(a) for _, a in v) for k, v in w.items()})
    check({f for f, _ in w.get(0x552DD0, set()) | w.get(0x552DD4, set())} == {0xCA670},
          'the menus\' direction written only by 0xCA670')
    for v in (0x5523C0, 0x5523C4, 0x552DD0, 0x552DD4, 0x5439B8, 0x3852F8):
        check(not in_regions(v, regs), 'the front end\'s input 0x%X is the process\'s (no player region)' % v)
    # the pie menu reads them, under the game menus' update 0xCD7D0, the shared draw at 0x55F94 (the driver's turn)
    pie = dict(funcs.get(0xCD470, []))
    check(pie.get(0xCD4F3) == 'fld [0x00552DD4]' and pie.get(0xCD4F9) == 'fld [0x00552DD0]' and
          pie.get(0xCD4FF) == 'fpatan', 'the pie menu 0xCD470: its slice from the direction')
    choose = dict(funcs.get(0xCD680, []))
    check(choose.get(0xCD689) == 'mov eax, [0x005523C0]' and choose.get(0xCD690) == 'and ecx, 0x01',
          'the pie menu 0xCD680: its choice on A in the snapshot')
    check({f for f, _ in cl.get(0xCD470, set())} == {0xCD7D0} and {f for f, _ in cl.get(0xCD680, set())} == {0xCD7D0},
          'the pie menu\'s parts run only under 0xCD7D0')
    check(dict(funcs.get(0x55E10, [])).get(0x55F94) == 'call 0x000CD7D0' and
          hk.get(0x55F94, (None,))[0] == 'NativeSplitSharedDraw', '0xCD7D0 the shared draw at 0x55F94 (the driver\'s turn)')
    # the bridge: presses to 0xCB170, releases to 0xCB540; the paths without a screen
    br = dict(funcs.get(0x19DFD0, []))
    check(br.get(0x19E0A7) == 'mov eax, [0x005523D4]' and br.get(0x19E0B1) == 'call 0x000CB170' and
          br.get(0x19E0AE) == 'jz 0x0019E0CD' and br.get(0x19E0CD) == 'mov al, [0x005619E4]',
          'the bridge: a press to the screen 0xCB170; 0x19E0CD the key event\'s path')
    check(br.get(0x19E101) == 'mov ecx, [0x005523D4]' and br.get(0x19E10C) == 'call 0x000CB540' and
          br.get(0x19E109) == 'jz 0x0019E114' and br.get(0x19E114) == 'mov eax, [0x00551854]',
          'the bridge: a release to 0xCB540; 0x19E114 the key event\'s path')
    check(br.get(0x19E149) == 'mov [0x005439B8], edi', 'the bridge keeps its mask (swapped per routed turn)')
    # the hooks
    want = {0x19E1AE: ('NativeSplitLookMenu', '0x19E1F9'), 0x19E0A7: ('NativeSplitPadMenuPress', '0x19E0CD'),
            0x19E101: ('NativeSplitPadMenuRelease', '0x19E114')}
    for at, (name, target) in want.items():
        h = hk.get(at)
        check(h is not None and h[0] == name and h[1] == target, 'hook %s at 0x%X (on_true %s)' % (name, at, target))
        check(name in text, 'hook %s in the generated code' % name)
    # the rule and where the routing stays (src): the hooks ask FeedsSharedFrontEnd for the acting player; the routing of
    # an added player's controller into its turn is kept (its look and buttons its own)
    src = open(os.path.join(ROOT, 'src', 'game', 'native_split.cpp')).read()
    body = src[src.find('bool NativeSplitLookMenu('):src.find('void WatchFrontEndInput() {')]
    check('FrontEndWithheld(k)' in body and body.count('InputPlayer()') == 3 and 'W32(' not in body,
          'the three hooks: the acting player against the driver, nothing written')
    fw = src[src.find('bool FrontEndWithheld('):]
    check('nsplit::FeedsSharedFrontEnd(k, g_ui.driver)' in fw[:fw.find('\n')], 'FrontEndWithheld is the pure rule')
    pt = src[src.find('void NativeSplitPadTurnBegin()'):src.find('void NativeSplitPadTurnEnd()')]
    check('W32(kActivePad, port)' in pt and 'W32(kBridgeMask, g_pad_turn.mask[k])' in pt,
          'an added player\'s controller still routed into its own turns')


def pc_input(funcs, cl, text):
    """docs 17.12 (run 20): the window's keyboard and mouse are player 1's. pc_keys' menu mode (keys as the pad, the
    pointer, the mouse let go) and every PC screen's keyboard input act on the shared front end's screen only while
    player 1 drives it; player 2's screen (its buy menu after player 1's rejoin) leaves them player 1's game's. The title's
    Key_Event (0x66510) does not look at the front end's screen (0x5523D4), so play keys reach the game with player 2's
    menu open"""
    ke = dict(funcs.get(0x66510, []))
    check(ke.get(0x66516) == 'mov edx, [0x00551DF0]' and ke.get(0x66543) == 'mov ecx, [0x007D6D28]' and
          ke.get(0x6655E) == 'call ecx', 'Key_Event 0x66510: key_dest 0x551DF0, the client\'s key hook [0x7D6D28]')
    hook_writes = [(f, a, i) for f, body in funcs.items() for a, i in body if '[0x007D6D28],' in i and i.startswith('mov')]
    check([i for _, _, i in hook_writes] == ['mov dword ptr [0x007D6D28], 0x18F8F0'],
          f'the key hook\'s only store is 0x18F8F0 ({hook_writes})')
    kh = [i for _, i in funcs.get(0x18F8F0, [])]
    check('mov eax, [0x00552B7C]' in kh and 'call 0x001B4FB0' in kh, 'the key hook 0x18F8F0: the VGUI viewport 0x1B4FB0')
    for fn in (0x66510, 0x18F8F0, 0x1B4FB0):
        body = ' '.join(i for _, i in funcs.get(fn, []))
        check(bool(body) and '0x5523D4' not in body and '0x005523D4' not in body,
              f'0x{fn:X} reads no front-end screen (0x5523D4): its open screen does not swallow play keys')
    src = lambda f: open(os.path.join(ROOT, f), encoding='utf-8').read()
    keys, mouse, menu = src('src/game/pc_keys.cpp'), src('src/game/pc_mouse.cpp'), src('src/game/pc_menu.cpp')
    browser, maps, ns = src('src/game/pc_browser.cpp'), src('src/game/pc_mapsearch.cpp'), src('src/game/native_split.cpp')
    win, view = src('src/linux/window_input.cpp'), src('src/game/native_split_view.h')
    check('inline bool PcInputDrivesMenus(bool native_split, int driver) { return !native_split || '
          'FeedsSharedFrontEnd(0, driver); }' in view, 'the rule: no native split-screen, or player 1 the driver')
    check('bool NativeSplitPcDrivesMenus() { return nsplit::PcInputDrivesMenus(NativeSplitOn(), g_ui.driver); }' in ns,
          'native_split.cpp asks the rule with the shared front end\'s driver')
    check('bool PcDrivesScreen() { return NativeSplitPcDrivesMenus(); }' in keys, 'csx::PcDrivesScreen is the rule')
    i = keys.find('void PcKeysFrame(')
    body = keys[i:keys.find('\n}\n', i)]
    check('const bool own = csx::PcDrivesScreen();' in body and
          'bool menu = screen && !PcScreenPassThrough(name) && own;' in body and
          'g_buy_menu = menu &&' in body, 'pc_keys: menu mode (and the buy menu\'s keys) only on a screen player 1 drives')
    check('if (uint32_t now = menu || !own ? 0 : screen; now != through)' in body,
          'pc_keys: another driver\'s screen is not logged as a pass-through screen')
    check('pc keys: playing on: the screen %s is player %d\'s' in body and 'pc keys: player %d\'s screen closed' in body,
          'pc_keys logs another driver\'s screen opening and closing (on change only: no cap)')
    i = mouse.find('void PcMouseFrame(')
    mb = mouse[i:]
    check(mb.find('if (!csx::PcDrivesScreen()) screen = 0;') > mb.find('uint32_t screen = R32(kCurrentScreen);') >= 0 and
          mb.find('if (!csx::PcDrivesScreen()) screen = 0;') < mb.find('SetTyping(false, "the screen closed")'),
          'pc_mouse: another driver\'s screen is no screen for the pointer and typing')
    check('std::string name = csx::PcDrivesScreen() ? ScreenName() : std::string();' in menu,
          'pc_menu: no key capture or number typing on another driver\'s screen')
    check('!csx::ConsoleOpen() && !csx::TextEntryOpen() && csx::PcDrivesScreen();' in browser,
          'pc_browser: its keys only on a screen player 1 drives')
    check('bool on = l != nullptr && !csx::ConsoleOpen() && csx::PcDrivesScreen();' in maps,
          'pc_mapsearch: its keys only on a screen player 1 drives')
    check('kCurrentScreen' not in win and '5523D4' not in win,
          'window_input reads no screen itself: its menu decisions are pc_keys\' (MenuOpen, BuyMenuOpen) and the PC screens\'')


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
    shared_audit(funcs, cl, regs, text)
    reconnect(funcs, cl, text)
    player1_rejoin(funcs, cl, text)
    stale_slot(funcs)
    front_end_input(funcs, cl, regs, hk, text)
    pc_input(funcs, cl, text)
    print(f'native_split_turns_test: {checks} checks, {len(failures)} failed')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
