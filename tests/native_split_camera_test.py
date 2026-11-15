#!/usr/bin/env python3
"""Native split-screen M3.2 (docs/native_splitscreen.md 17): every player's persistent camera state is its own.

Static: the per-player regions (src/game/client_context_regions.inc and the hand regions in client_context.cpp) must
hold each address a player's camera keeps between frames, so one player's camera update can never overwrite
another's (a context switch copies them in and out). The process-wide view globals that stay shared are only valid
inside one pass: each is rebuilt from the passing player's own state before it is read (listed with the code that
rebuilds them). And the M3 hooks must not touch a region's camera address. No game is started.

Usage: python3 -I tests/native_split_camera_test.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# a player's camera, kept between frames (address, what, evidence)
PER_PLAYER = [
    (0x552C10, 'view yaw (the look routine and mouse look integrate into it)', 'src/game/mouse_look.cpp kYaw'),
    (0x552C14, 'view pitch', 'src/game/mouse_look.cpp kPitch'),
    (0x5439A0, "the look routine's state (response curve, acceleration)", 'regions: written by 19E160'),
    (0x5437D0, "the look routine's flags", 'regions: written by 19E160 19E660'),
    (0x377668, 'scr_fov_value (its FOV, zoom)', 'docs 16.1; written by 0x1CD12 per player'),
    (0x749D40, 'cl (view angles, punch angle, the view entity, cl.time)', "CL_ClearState's extent"),
    (0x74A244, "the screen fade (flashbang, damage)", 'cl.sf, docs 16.1'),
    (0x771224, 'cl.playernum', 'cl'),
    (0x5623D0, 'gHUD (the HUD and its sprites)', 'hand region'),
    (0x552B7C, 'gViewPort (its VGUI viewport)', 'regions'),
    (0x552B8C, 'the particle manager', 'docs 10.5'),
    (0x551890, 'cl_entities', 'docs 4'),
]
# of those, the camera itself (no native split code writes them; gHUD's list is cleared by a leaving player's own
# cleanup in its own context, docs 15.16)
CAMERA = {0x552C10, 0x552C14, 0x5439A0, 0x5437D0, 0x377668, 0x74A244}
# process-wide, rebuilt every pass from the passing player's state before any read (not a player's: not copied)
PER_PASS = [
    (0x6E7A20, 'r_refdef.vrect', 'SCR_CalcRefdef 0x55850 / the paint 0xB4560, every pass'),
    (0x6E7A90, 'r_refdef.vieworg / viewangles', "V_RenderView 0xB5D10 copies the pass's ref_params"),
    (0x6CE3C8, 'glwidth', 'GL_BeginRendering 0x56A10 every pass'),
    (0x56D690, "the client view's origin / angles of the pass", 'V_CalcRefdef 0x1A0F10 -> 0x19EF50 writes them first'),
    # docs 17: the leaf visibility (the entities' efrags, R_StoreEfrags against leaf visframe): all three shared with
    # the marks they guard (in the shared world model), so each pass marks for its own leaf: R_SetupFrame 0x4DE62..
    # 0x4DE81 (old = view, view = the pass's leaf), R_MarkLeaves 0x52380 (old != view: visframecount++, re-mark)
    (0x6E7FEC, 'r_viewleaf', 'R_SetupFrame 0x4DE81, every pass'),
    (0x6E7FF0, 'r_oldviewleaf', 'R_SetupFrame 0x4DE76, R_MarkLeaves 0x523E7'),
    (0x6E7AEC, 'r_visframecount', 'R_MarkLeaves 0x523E1: with the shared marks'),
]
# shared on purpose (the process's), never a player's camera
SHARED = [
    (0x3852F8, "the title's active pad (set per turn by native split, put back after)"),
    (0x5439B8, "the pad bridge's previous buttons (swapped per player's turn)"),
]
# docs 18.5 (own menus): the menus' input and state are each player's (its own screens, in its own turn)
MENU = [
    (0x5523C0, "the menus' button snapshot (the look routine's copy of the bridge's mask)"),
    (0x5523C4, "the menus' button snapshot of the frame before"),
    (0x552DD0, "the menus' direction x (0xCA670, the player's left stick)"),
    (0x552DD4, "the menus' direction y"),
    (0x5523D4, "the top screen (its stack)"),
    (0x385420, "the pie menu's highlighted slice"),
    (0x552368, "the menus' frame delta"),
]


def regions():
    out = []
    for line in open(os.path.join(ROOT, 'src/game/client_context_regions.inc')):
        m = re.match(r'\{0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+)', line)
        if m:
            out.append((int(m.group(1), 16), int(m.group(2), 16)))
    src = open(os.path.join(ROOT, 'src/game/client_context.cpp')).read()
    for m in re.finditer(r'\{0x([0-9A-Fa-f]+), 0x([0-9A-Fa-f]+)', src):
        out.append((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def inside(a, regs):
    return any(b <= a < b + n for b, n in regs)


def main():
    regs = regions()
    checks = failed = 0

    def check(ok, what):
        nonlocal checks, failed
        checks += 1
        if not ok:
            failed += 1
            print('FAIL:', what)

    for a, what, why in PER_PLAYER:
        check(inside(a, regs), 'per player: 0x%X %s (%s)' % (a, what, why))
    for a, what, why in PER_PASS:
        check(not inside(a, regs), 'per pass, shared: 0x%X %s (%s)' % (a, what, why))
    for a, what in SHARED:
        check(not inside(a, regs), 'shared: 0x%X %s' % (a, what))
    for a, what in MENU:
        check(inside(a, regs), 'per player (own menus): 0x%X %s' % (a, what))
    check(not inside(0x5523D0, regs), 'the screen list 0x5523D0 stays shared (the screens are the process\'s)')
    # the M3 / M3.2 code writes no player's camera address directly (the pad turn writes the shared pad index and mask)
    for name in ('src/game/native_split.cpp', 'src/game/pc_wide.cpp', 'src/game/pc_viewmodel.cpp'):
        text = open(os.path.join(ROOT, name)).read()
        writes = re.findall(r'W32\((0x[0-9A-Fa-f]+)', text)
        for w in writes:
            check(int(w, 16) not in CAMERA, '%s writes camera %s' % (name, w))
        check('kYaw' not in text and '0x552C10' not in text, '%s touches the view angles' % name)
    # the automatic team choice and the test controller set nothing of a player's state: menu lines and pad input only
    join = open(os.path.join(ROOT, 'src/game/native_split_join.h')).read()
    check('W32' not in join and 'kernel::' not in join, 'native_split_join.h writes no guest memory')
    print('native_split_camera_test: %d checks, %d failed' % (checks, failed))
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
