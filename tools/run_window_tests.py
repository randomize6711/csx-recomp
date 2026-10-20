#!/usr/bin/env python3
"""Runs window scenarios (tests/scenarios/vk_*.txt) in the Vulkan window build, each in its own headless gamescope
(nothing on the desktop, no host input: CSX_TEST_INPUT_ONLY as drive.py sets it), no audio device (CSX_AUDIO=null).

Usage: python3 -I tools/run_window_tests.py [SCENARIO ...]   (default: the mouse, binding, console, movement, spectator, display and loading window tests)

Each scenario's header (its leading # lines) gives its gamescope size ("gamescope --backend headless -W W -H H") and
its drive.py options (--env NAME=VALUE, --pc NAME=VALUE, --launch DIR); in values $PWD is the repository and \\n a
line break. The run folder is build/wruns/<scenario>. Exit status non-zero when any scenario failed; a summary at
the end. Do not run it while a build or tests/run_regression.sh is running.
"""
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = ['vk_menu_pointer_43.txt', 'vk_menu_pointer_169.txt', 'vk_menu_pointer_219.txt',
           'vk_menu_pointer_43_full.txt', 'vk_menu_mouse_fe.txt', 'vk_menu_mouse.txt', 'vk_bind_capture.txt',
           'vk_console.txt', 'vk_movement.txt', 'vk_spectator.txt',
           'vk_display_mode.txt', 'vk_menu_pointer_pause.txt', 'vk_loading.txt']


def options(path):
    header = []
    for line in open(path, encoding='utf-8'):
        if not line.startswith('#'):
            break
        header.append(line[1:].strip())
    text = ' '.join(header)
    m = re.search(r'gamescope --backend headless -W (\d+) -H (\d+)', text)
    size = (m.group(1), m.group(2)) if m else ('1280', '960')
    opts = []
    for h in header:  # the option lines only (the prose may hold quotes)
        if not h.startswith('--'):
            continue
        words = shlex.split(h[:-1] if h.endswith('\\') else h, posix=True)
        for i, w in enumerate(words[:-1]):
            if w in ('--env', '--pc', '--launch'):
                opts += [w, words[i + 1].replace('$PWD', ROOT).replace('\\n', '\n')]
    return size, opts


def main():
    names = sys.argv[1:] or DEFAULT
    exe = os.path.join(ROOT, 'build', 'window', 'CounterStrike')
    if not os.access(exe, os.X_OK):
        sys.exit('no %s: tools/build_window.sh first' % exe)
    results = []
    for name in names:
        path = name if os.path.exists(name) else os.path.join(ROOT, 'tests', 'scenarios', name)
        base = os.path.splitext(os.path.basename(path))[0]
        run = os.path.join(ROOT, 'build', 'wruns', base)
        subprocess.run(['rm', '-rf', run])
        os.makedirs(run)
        (w, h), opts = options(path)
        status = os.path.join(run, 'drive_status')
        drive = [sys.executable, '-I', os.path.join(ROOT, 'tools', 'drive.py'), path, '--exe', exe, '--run', run,
                 '--timeout', '900', '--env', 'CSX_RENDERER=vulkan', '--env', 'FPS=30', '--env', 'CSX_AUDIO=null'] + opts
        inner = ' '.join(shlex.quote(a) for a in drive) + '; echo $? > ' + shlex.quote(status)
        print('== %s (gamescope %sx%s)' % (base, w, h))
        sys.stdout.flush()
        with open(os.path.join(run, 'drive.out'), 'w') as out:
            subprocess.run(['gamescope', '--backend', 'headless', '-W', w, '-H', h, '-w', w, '-h', h, '--',
                            'sh', '-c', inner], cwd=ROOT, stdout=out, stderr=subprocess.DEVNULL, timeout=1200)
        lines = open(os.path.join(run, 'drive.out'), encoding='utf-8', errors='replace').read().splitlines()
        for l in lines:
            if l.startswith('FAIL') or (l.startswith('     ') and not l.startswith('            ')):
                print('  ' + l)
        code = open(status).read().strip() if os.path.exists(status) else 'none'
        ok = code == '0'
        print('  %s (%d steps)' % ('passed' if ok else 'FAILED (status %s)' % code,
                                  sum(1 for l in lines if l.startswith(('ok', 'FAIL')))))
        results.append((base, ok))
    print('\n'.join('%s %s' % ('ok  ' if ok else 'FAIL', n) for n, ok in results))
    sys.exit(0 if all(ok for _, ok in results) else 1)


if __name__ == '__main__':
    main()
