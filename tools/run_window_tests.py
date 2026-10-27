#!/usr/bin/env python3
"""Runs window scenarios (tests/scenarios/vk_*.txt) in the Vulkan window build, each in its own headless gamescope
(nothing on the desktop, no host input: CSX_TEST_INPUT_ONLY as drive.py sets it), no audio device (CSX_AUDIO=null).

Usage: python3 -I tools/run_window_tests.py [--exe PATH] [--tag TAG] [SCENARIO ...]   (default: the mouse, binding, console, movement, spectator, display, loading, VSync and raw mouse window tests)

Each scenario's header (its leading # lines) gives its gamescope size ("gamescope --backend headless -W W -H H") and
its refresh rate when given ("... -H H -r HZ"), and its drive.py options (--env NAME=VALUE, --pc NAME=VALUE, --launch
DIR); in values $PWD is the repository, $RUN the run folder and \\n a line break. The run folder is
build/wruns/<scenario> (<scenario>_<TAG> with --tag); --exe runs another build of the window front end (a benchmark's
variant). Exit status non-zero when any scenario failed; a summary at the end. Do not run it while a build or
tests/run_regression.sh is running.
"""
import os, signal, time
import re
import shlex
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = ['vk_menu_pointer_43.txt', 'vk_menu_pointer_169.txt', 'vk_menu_pointer_219.txt',
           'vk_menu_pointer_43_full.txt', 'vk_menu_mouse_fe.txt', 'vk_menu_mouse.txt', 'vk_bind_capture.txt',
           'vk_console.txt', 'vk_movement.txt', 'vk_spectator.txt',
           'vk_display_mode.txt', 'vk_menu_pointer_pause.txt', 'vk_loading.txt', 'vk_vsync.txt',
           'vk_vsync_play.txt', 'vk_mouse_raw.txt', 'vk_auto_exposure.txt', 'vk_auto_exposure_rates.txt',
           'vk_flashbang.txt', 'vk_flashbang_rates.txt', 'vk_flashbang_cover_43.txt', 'vk_flashbang_cover_1610.txt',
           'vk_flashbang_cover_169.txt', 'vk_flashbang_cover_219.txt', 'vk_water_43.txt', 'vk_water_169.txt',
           'vk_water_219.txt']


def options(path):
    header = []
    for line in open(path, encoding='utf-8'):
        if not line.startswith('#'):
            break
        header.append(line[1:].strip())
    text = ' '.join(header)
    m = re.search(r'gamescope --backend headless -W (\d+) -H (\d+)(?: -r (\d+))?', text)
    size = (m.group(1), m.group(2), m.group(3)) if m else ('1280', '960', None)
    opts = []
    for h in header:  # the option lines only (the prose may hold quotes)
        if not h.startswith('--'):
            continue
        words = shlex.split(h[:-1] if h.endswith('\\') else h, posix=True)
        for i, w in enumerate(words[:-1]):
            if w in ('--env', '--pc', '--launch'):
                opts += [w, words[i + 1].replace('$PWD', ROOT).replace('\\n', '\n')]
    # "timeout: N" in the header: the scenario's seconds (900 otherwise)
    t = re.search(r'(?:^| )timeout: (\d+)', text)
    opts += ['--timeout', t.group(1)] if t else []
    return size, opts


def main():
    args = sys.argv[1:]
    exe = os.path.join(ROOT, 'build', 'window', 'CounterStrike')
    tag = ''
    while args and args[0] in ('--exe', '--tag') and len(args) > 1:
        if args[0] == '--exe':
            exe = os.path.abspath(args[1])
        else:
            tag = '_' + args[1]
        args = args[2:]
    names = args or DEFAULT
    if not os.access(exe, os.X_OK):
        sys.exit('no %s: tools/build_window.sh first' % exe)
    results = []
    for name in names:
        path = name if os.path.exists(name) else os.path.join(ROOT, 'tests', 'scenarios', name)
        base = os.path.splitext(os.path.basename(path))[0] + tag
        run = os.path.join(ROOT, 'build', 'wruns', base)
        subprocess.run(['rm', '-rf', run])
        os.makedirs(run)
        (w, h, hz), opts = options(path)
        limit = int(opts[opts.index('--timeout') + 1]) if '--timeout' in opts else 900
        opts = [o.replace('$RUN', run) for o in opts]
        status = os.path.join(run, 'drive_status')
        drive = [sys.executable, '-I', os.path.join(ROOT, 'tools', 'drive.py'), path, '--exe', exe, '--run', run,
                 '--timeout', '900', '--env', 'CSX_RENDERER=vulkan', '--env', 'FPS=30', '--env', 'CSX_AUDIO=null'] + opts
        inner = ' '.join(shlex.quote(a) for a in drive) + '; echo $? > ' + shlex.quote(status)
        print('== %s (gamescope %sx%s)' % (base, w, h))
        sys.stdout.flush()
        with open(os.path.join(run, 'drive.out'), 'w') as out:
            # its own process group: a run past its time is ended whole (gamescope, its reaper, the game and the
            # players; killing gamescope alone left a compositor blocked on the gone display)
            g = subprocess.Popen(['gamescope', '--backend', 'headless', '-W', w, '-H', h, '-w', w, '-h', h] +
                                 (['-r', hz] if hz else []) + ['--', 'sh', '-c', inner], cwd=ROOT, stdout=out,
                                 stderr=subprocess.DEVNULL, start_new_session=True)
            try:
                g.wait(timeout=limit + 300)
            except subprocess.TimeoutExpired:
                print('  the run went past %d s: its process group ended' % (limit + 300))
                for sig in (signal.SIGTERM, signal.SIGKILL):
                    try:
                        os.killpg(g.pid, sig)
                    except ProcessLookupError:
                        break
                    time.sleep(5)
                g.wait()
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
