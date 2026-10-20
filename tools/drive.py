#!/usr/bin/env python3
"""Drives the headless harness through a scenario by watching its log.

Usage: python3 -I tools/drive.py SCENARIO [--timeout S] [--env K=V ...] [--exe PATH] [--run DIR] [--launch DIR] [--wine]
                                [--pc NAME=VALUE ...]

--pc seeds the run folder's pc_settings.ini (Options > PC Options); without it every run starts with none.

--wine runs the Windows build (build/windows/dist/CounterStrike.exe unless --exe names another) under Wine, prefix
build/wineprefix, in a network namespace like the Linux runs. A title relaunch there starts a new Windows process, so
the run is alive while any process of the exe is.

The harness runs in build/run (network namespace, reflink game copy in build/gamedata). The scenario file has one step
per line, executed in order:
    wait draws>=N           until a frame logs at least N draws per frame (from the 60-frame renderer lines)
    wait draws==N           until the draws per frame equal N (also draws<=N)
    wait log REGEX          until a log line matches
    sleep S                 wait S seconds
    press BUTTONS [FRAMES]  press through the harness cmd file (buttons as in the pad script, joined by +)
    key NAME [SECONDS]      window build: press an SDL key (SDL key name; NAME#SCANCODE: the key a layout puts at
                            that position, e.g. Å#[ on a Swedish keyboard) through the window's keys file
    point X Y               window build: the mouse pointer to (X, Y) of the menus' 640x480, as SDL motion
    pointat X Y             window build: the mouse pointer to (X, Y) of the window (logical pixels), as SDL motion
    type TEXT               window build: TEXT as SDL text input (what a keyboard layout types: the console's input)
    windowshot NAME         window build: save the window's output as RUN/shots/NAME.bmp
    pressuntil BUTTONS COND press, wait for two fresh draw measurements, repeat until COND holds (at most 15)
    dump NAME               guest memory dump to build/run/dumps/NAME.bin (src/common/memory_dump.cpp)
    state NAME              log the local player's state (src/game/state_probe.cpp) and keep it as NAME
    track NAME SECONDS      follow the player's position that long and log how high it rose and how far it moved
    expect moved A B MIN MAX       horizontal distance between states A and B in [MIN, MAX] game units
    expect yaw A B DEG TOL         view yaw (0x552C10) changed by DEG (+ left) within TOL; expect pitch likewise
    expect turned A B MIN          yaw difference between states A and B (shortest way) at least MIN degrees
    expect shots A B N             the shot counter dropped by exactly N from A to B
    expect still A B               position and yaw equal in A and B
    expect tracked NAME rose|moved MIN MAX  a track step's height risen or distance moved in [MIN, MAX]
    expect fps MIN MAX             presented frames per second (the renderer's frame lines) in [MIN, MAX]
    expect weapon A ID             the weapon in hand at A (17 Glock, 29 knife, 4 HE grenade, 25 flashbang, 9 smoke)
    expect rose A B MIN            the player rose at least MIN units from A to B (a jump)
    expect heading A B MAXDEG [OFF] the move from A to B points along A's yaw (+ OFF degrees: 90 left, -90 right, 180
                                   back, -45 forward right) within MAXDEG degrees
    mouse DX DY             (window build) relative mouse motion in counts, through the window's mouse look
    count REGEX N           until N log lines match REGEX (no spaces: write . for them)
    look RIGHT UP           turn the view by degrees through the mouse look hook (src/game/mouse_look.cpp)
    pokefile FILE           every "VA FLOAT" line of FILE (relative to the repository) written at once
    poke VA FLOAT           write FLOAT to guest address VA (hex) once, src/common/memory_dump.cpp (diagnostics)
    click BUTTON [SECONDS]  (window build) a mouse button (1 left, 2 middle, 3 right) through SDL; wheel N: wheel notches
    pckey cmd TEXT          console text through Cbuf_AddText; pckey key KEYNUM DOWN: an engine key event (pc_keys.cpp)
    snap NAME               draw the next two frames and keep them as build/run/snaps/NAME_1.tga / NAME_2.tga
    expect region NAME X0 Y0 X1 Y1 MIN MAX  mean luminance of that box of snaps/NAME_2.tga (or shots/NAME.bmp) in [MIN, MAX]
    expect darkfrac NAME X0 Y0 X1 Y1 LUM MIN MAX  share of that box's pixels darker than LUM in [MIN, MAX]
    expect log REGEX        fail unless a log line matches
    expect nolog REGEX      fail if a log line matches
    expect count REGEX N    fail unless exactly N log lines match (no spaces: write . for them)
    expect menuaudio [SOUND]  the menu sound (menu/buy_hover.wav) audible at each mouse play in the run's capture
                            (needs CSX_AUDIO_WAV and CSX_LOG_MENU_SOUNDS=1; tools/check_menu_audio.py)
    expect image NAME MIN   fail unless snaps/NAME_2.tga (or the window shot shots/NAME.bmp) has a luminance standard deviation of at least MIN
                            (a blank or flat frame fails; game pixels are never stored in the repository)
Exit status 0 when every step passed, 1 otherwise. Prints one line per step.
"""
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(ROOT, 'build', 'run')
# named as GameInfo::images names the image: a title relaunch (quick reboot) execs this name
EXE = os.path.join(ROOT, 'build', 'harness', 'CounterStrike')
GAME = os.path.join(ROOT, 'build', 'gamedata')
FRAME_RE = re.compile(r'renderer: frame (\d+), (\d+) draws')


class Log:
    def __init__(self):
        self.lines = []
        self.path = None
        self.prev = None  # (frame, draws) of the previous renderer line
        self.per_frame = 0.0
        self.samples = 0  # renderer lines seen, each a new draws-per-frame measurement

    def poll(self):
        logs = os.path.join(RUN, 'logs')
        # a relaunched instance (same process, exec) starts a new log file: follow the newest
        if os.path.isdir(logs):
            names = sorted(os.listdir(logs))
            if names and (self.path is None or os.path.join(logs, names[-1]) != self.path):
                if self.path is not None:
                    self.lines.extend(self.f.readlines())
                    self.lines.append('--- relaunch: ' + names[-1] + '\n')
                    self.prev = None
                self.path = os.path.join(logs, names[-1])
                self.f = open(self.path, 'r', errors='replace')
        if self.path is None:
            return
        for line in self.f.readlines():
            self.lines.append(line)
            m = FRAME_RE.search(line)
            if m:
                frame, draws = int(m.group(1)), int(m.group(2))
                if self.prev and frame > self.prev[0]:
                    self.per_frame = (draws - self.prev[1]) / (frame - self.prev[0])
                    self.samples += 1
                self.prev = (frame, draws)

    def match(self, rx):
        r = re.compile(rx)
        return any(r.search(l) for l in self.lines)


def send_file(name, text):
    tmp = os.path.join(RUN, name + '.tmp')
    with open(tmp, 'w') as f:
        f.write(text + '\n')
    os.replace(tmp, os.path.join(RUN, name))


def send(text):
    # the harness polls for the file often: write it whole elsewhere and rename, or it may read it empty and drop it
    tmp = os.path.join(RUN, 'cmd.tmp')
    with open(tmp, 'w') as f:
        f.write(text)
    os.replace(tmp, os.path.join(RUN, 'cmd'))


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)
    scenario, timeout, env = args[0], 600.0, dict(os.environ)
    launch = None
    wine = False
    pc = []
    i = 1
    while i < len(args):
        if args[i] == '--timeout':
            timeout = float(args[i + 1]); i += 2
        elif args[i] == '--exe':  # another front end, e.g. build/window/CounterStrike
            global EXE
            EXE = os.path.abspath(args[i + 1]); i += 2
            env.setdefault('CSX_TEST_INPUT_ONLY', '1')  # a window front end: only the scenario's keys reach the title
            env.setdefault('CSX_AUDIO', 'null')  # silent unless a scenario run asks for the device (--env CSX_AUDIO=sdl)
        elif args[i] == '--run':
            global RUN
            RUN = os.path.abspath(args[i + 1]); i += 2
        elif args[i] == '--env':
            k, v = args[i + 1].split('=', 1); env[k] = v; i += 2
        elif args[i] == '--launch':
            launch = os.path.abspath(args[i + 1]); i += 2
        elif args[i] == '--pc':
            pc.append(args[i + 1]); i += 2
        elif args[i] == '--wine':
            wine = True; i += 1
            if EXE.endswith(os.path.join('harness', 'CounterStrike')):
                EXE = os.path.join(ROOT, 'build', 'windows', 'dist', 'CounterStrike.exe')
            env.setdefault('CSX_TEST_INPUT_ONLY', '1')
            env.setdefault('CSX_AUDIO', 'null')
            env.setdefault('WINEPREFIX', os.path.join(ROOT, 'build', 'wineprefix'))
            env.setdefault('WINEDEBUG', '-all')
        else:
            sys.exit('unknown argument ' + args[i])
    steps = [l.split() for l in open(scenario) if l.strip() and not l.startswith('#')]

    for d in ('logs', 'hdd', 'cap', 'snaps', 'dumps', 'shots'):
        shutil.rmtree(os.path.join(RUN, d), ignore_errors=True)
    os.makedirs(os.path.join(RUN, 'snaps'))
    # PC settings: none unless the run seeds them (src/common/pc_settings.cpp format, "name = value")
    settings = os.path.join(RUN, 'pc_settings.ini')
    if os.path.exists(settings):
        os.remove(settings)
    if pc:
        with open(settings, 'w') as f:
            f.write(''.join(kv.replace('=', ' = ', 1) + '\n' for kv in pc))
    if launch:
        # what the front end leaves for its relaunch into a match: the launch data page (launch.bin, kept by the host as
        # launch_seen.bin) and the match's command line on the cache partition (Z:\releasemap.txt); with both the
        # title boots straight into that match
        # the cache partition as the front end leaves it (it copies the HD sound banks and more there on boot; a
        # direct start without them has no weapon or player sounds): build/zcache, made by tools/make_zcache.sh
        zcache = os.path.join(ROOT, 'build', 'zcache')
        if os.path.isdir(zcache):
            shutil.copytree(zcache, os.path.join(RUN, 'hdd', 'Z'))
        else:
            os.makedirs(os.path.join(RUN, 'hdd', 'Z'))
            print('note: no build/zcache (tools/make_zcache.sh): the match starts without the cached sound banks')
        shutil.copyfile(os.path.join(launch, 'launch.bin'), os.path.join(RUN, 'hdd', 'launch.bin'))
        shutil.copyfile(os.path.join(launch, 'releasemap.txt'), os.path.join(RUN, 'hdd', 'Z', 'releasemap.txt'))
    out = open(os.path.join(RUN, 'run.out'), 'w')
    if wine:
        game = subprocess.run(['winepath', '-w', GAME], env=env, capture_output=True, text=True).stdout.strip()
        cmd = ['unshare', '-n', '--map-current-user', 'wine', EXE, game]
    else:
        cmd = ['unshare', '-rn', EXE, GAME]
    proc = subprocess.Popen(cmd, cwd=RUN, env=env, stdout=out, stderr=subprocess.STDOUT)
    log, start, ok = Log(), time.time(), True
    states = {}
    tracks = {}  # track steps: NAME -> rose, moved
    exe_name = os.path.basename(EXE)

    def wine_alive():
        # Wine processes carry the Windows path of their exe as argv[0]
        for pid in os.listdir('/proc'):
            if pid.isdigit():
                try:
                    with open(f'/proc/{pid}/cmdline', 'rb') as f:
                        argv0 = f.read().split(b'\0')[0].decode(errors='replace')
                except OSError:
                    continue
                if argv0.replace('\\', '/').endswith('/' + exe_name):
                    return True
        return False

    def alive():
        return proc.poll() is None or (wine and wine_alive())

    def wait_until(cond):
        while time.time() - start < timeout:
            log.poll()
            if cond():
                return True
            if not alive():
                return False
            time.sleep(0.2)
        return False

    try:
        for st in steps:
            kind = st[0]
            if kind == 'wait':
                if st[1] == 'log':
                    rx = ' '.join(st[2:])
                    good = wait_until(lambda: log.match(rx))
                else:
                    m = re.match(r'draws(>=|==|<=)(\d+)', st[1])
                    n, op = int(m.group(2)), m.group(1)
                    good = wait_until(lambda: log.per_frame >= n if op == '>=' else log.per_frame <= n if op == '<=' else round(log.per_frame) == n)
            elif kind == 'sleep':
                time.sleep(float(st[1])); good = alive()
            elif kind == 'press':
                frames = st[2] if len(st) > 2 else '3'
                send(f'{st[1]} {frames}\n')
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'cmd')))
            elif kind == 'key':
                send_file('keys', st[1] + ' ' + (st[2] if len(st) > 2 else '0.12'))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'keys')))
            elif kind == 'type':
                send_file('text', ' '.join(st[1:]))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'text')))
            elif kind == 'pointat':
                # pointat X Y: window build, the pointer to that window pixel (the test computes the menu point)
                send_file('mouse', 'window ' + st[1] + ' ' + st[2])
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'mouse')))
            elif kind == 'point':
                # point X Y: window build, the pointer to that point of the menus' 640x480 (SDL motion through the window)
                send_file('mouse', 'menu ' + st[1] + ' ' + st[2])
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'mouse')))
            elif kind in ('click', 'wheel'):
                # click BUTTON [SECONDS] (1 left, 2 middle, 3 right) / wheel N: window build, through SDL's event queue
                send_file('mouse', ('button ' + st[1] + ' ' + (st[2] if len(st) > 2 else '0.12')) if kind == 'click'
                          else 'wheel ' + st[1])
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'mouse')))
            elif kind == 'pckey':
                # pckey cmd TEXT... | pckey key KEYNUM DOWN: console text or an engine key event (src/game/pc_keys.cpp)
                send_file('pckey', ' '.join(st[1:]))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'pckey')))
            elif kind == 'windowshot':
                send_file('shot', st[1])
                good = wait_until(lambda: log.match('window: shot .*' + re.escape(st[1]) + r'\.bmp'))
            elif kind == 'pressuntil':
                m = re.match(r'draws(>=|==)(\d+)', st[2])
                n = int(m.group(2))
                cond = lambda: log.per_frame >= n if m.group(1) == '>=' else round(log.per_frame) == n
                good = False
                for _ in range(15):
                    send(f'{st[1]} 3\n')
                    # two fresh measurements after the press (the renderer logs every 60 frames), at most 20 s
                    seen, t0 = log.samples, time.time()
                    while log.samples < seen + 2 and time.time() - t0 < 20 and alive():
                        log.poll()
                        time.sleep(0.2)
                    if cond():
                        good = True
                        break
                    if not alive():
                        break
            elif kind == 'dump':
                tmp = os.path.join(RUN, 'memdump.tmp')
                with open(tmp, 'w') as f:
                    f.write(st[1] + '\n')
                os.replace(tmp, os.path.join(RUN, 'memdump'))
                good = wait_until(lambda: log.match('memdump: .*' + re.escape(st[1]) + r'\.bin written'))
            elif kind == 'mouse':
                send_file('mouse', st[1] + ' ' + st[2])
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'mouse')))
            elif kind == 'count':
                # count REGEX N: until N log lines match (the Nth round end, ...)
                rx = re.compile(st[1])
                n = int(st[2])
                good = wait_until(lambda: sum(1 for l in log.lines if rx.search(l)) >= n)
            elif kind == 'look':
                send_file('look', st[1] + ' ' + st[2])
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'look')))
            elif kind == 'pokefile':
                send_file('poke', open(os.path.join(ROOT, st[1])).read().strip())
                good = wait_until(lambda: log.match(r'poke: .*writes'))
            elif kind == 'poke':
                send_file('poke', st[1] + ' ' + st[2])
                good = wait_until(lambda: log.match('poke: ' + st[1].upper().lstrip('0X').rjust(8, '0')))
            elif kind == 'track':
                # track NAME SECONDS: the position followed that long (state_probe.cpp); its "track" line printed
                send_file('state', st[1] + ' ' + st[2])
                rx = re.compile(r'track ' + re.escape(st[1]) + r' over')
                good = wait_until(lambda: any(rx.search(l) for l in log.lines))
                for l in log.lines:
                    if rx.search(l):
                        print('     ' + l[l.index('track'):])
                        m = re.search(r'rose (-?[0-9.]+) at most, moved ([0-9.]+)', l)
                        tracks[st[1]] = {'rose': float(m.group(1)), 'moved': float(m.group(2))}
            elif kind == 'state':
                send_file('state', st[1])
                rx = re.compile(r'state ' + re.escape(st[1]) + r' (\S+) (\S+) (\S+) (\S+) (-?\d+)(?: (\S+) (\S+))?(?: (-?\d+))?')
                def got():
                    for l in log.lines:
                        m = rx.search(l)
                        if m:
                            states[st[1]] = [float(v) for v in m.groups() if v is not None]
                            return True
                    return False
                good = wait_until(got)
                if good:
                    print('     ' + st[1] + ' = ' + ' '.join(str(v) for v in states[st[1]]))
            elif kind == 'expect' and st[1] == 'weapon':
                # expect weapon A ID: the weapon in hand at state A (17 Glock, 29 knife, 4 HE, 25 flashbang, 9 smoke)
                a = states.get(st[2])
                good = a is not None and len(a) > 7 and int(a[7]) == int(st[3])
                print(f'     weapon {int(a[7]) if a and len(a) > 7 else None}')
            elif kind == 'expect' and st[1] in ('moved', 'turned', 'shots', 'still', 'heading', 'yaw', 'pitch', 'rose'):
                a, b = states.get(st[2]), states.get(st[3])
                good = a is not None and b is not None
                if good and st[1] == 'rose':
                    good = b[2] - a[2] >= float(st[4])
                    print(f'     rose {b[2] - a[2]:.1f}')
                elif good and st[1] == 'moved':
                    dist = ((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5
                    good = float(st[4]) <= dist <= float(st[5])
                    print(f'     moved {dist:.1f}')
                elif good and st[1] == 'turned':
                    dy = abs((b[3] - a[3] + 180) % 360 - 180)
                    good = dy >= float(st[4])
                    print(f'     turned {dy:.1f} degrees')
                elif good and st[1] in ('yaw', 'pitch'):
                    # the view angles the look routine keeps: signed change, yaw wrapped (positive to the left)
                    i = 5 if st[1] == 'yaw' else 6
                    good = len(a) > i and len(b) > i
                    d = (b[i] - a[i] + 180) % 360 - 180 if st[1] == 'yaw' else b[i] - a[i]
                    good = good and abs(d - float(st[4])) <= float(st[5])
                    print(f'     view {st[1]} changed {d:+.3f} degrees (want {st[4]} +- {st[5]})')
                elif good and st[1] == 'shots':
                    good = a[4] - b[4] == int(st[4])
                    print(f'     shot counter {int(a[4])} -> {int(b[4])}')
                elif good and st[1] == 'still':
                    good = a[:4] == b[:4]
                elif good and st[1] == 'heading':
                    import math
                    move = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
                    want = a[3] + (float(st[5]) if len(st) > 5 else 0.0)  # OFFSET: 90 left, -90 right, 180 back
                    off = abs((move - want + 180) % 360 - 180)
                    good = off <= float(st[4])
                    print(f'     moved toward {move:.1f}, yaw {a[3]:.1f}, off by {off:.1f} degrees')
            elif kind == 'snap':
                open(os.path.join(RUN, 'snap.tmp'), 'w').close()
                os.replace(os.path.join(RUN, 'snap.tmp'), os.path.join(RUN, 'snap'))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'snap')))
                time.sleep(0.5)
                for k in (1, 2):
                    src = os.path.join(RUN, f'snap_{k}.tga')
                    if os.path.exists(src):
                        shutil.move(src, os.path.join(RUN, 'snaps', f'{st[1]}_{k}.tga'))
            elif kind == 'expect' and st[1] == 'image':
                from PIL import Image, ImageStat
                path = os.path.join(RUN, 'snaps', st[2] + '_2.tga')
                if not os.path.exists(path):  # a window shot of that name
                    path = os.path.join(RUN, 'shots', st[2] + '.bmp')
                good = os.path.exists(path) and ImageStat.Stat(Image.open(path).convert('L')).stddev[0] >= float(st[3])
                if good:
                    print(f'     {st[2]}: luminance spread {ImageStat.Stat(Image.open(path).convert("L")).stddev[0]:.1f}')
            elif kind == 'expect' and st[1] == 'darkfrac':
                # expect darkfrac NAME X0 Y0 X1 Y1 LUM MIN MAX: share of that box's pixels darker than LUM
                from PIL import Image
                path = os.path.join(RUN, 'snaps', st[2] + '_2.tga')
                if not os.path.exists(path):
                    path = os.path.join(RUN, 'shots', st[2] + '.bmp')
                good = os.path.exists(path)
                if good:
                    box = tuple(int(v) for v in st[3:7])
                    px = list(Image.open(path).convert('L').crop(box).getdata())
                    frac = sum(1 for v in px if v < int(st[7])) / max(1, len(px))
                    print(f'     {st[2]} {box}: {frac:.3f} of pixels below {st[7]} (want {st[8]}..{st[9]})')
                    good = float(st[8]) <= frac <= float(st[9])
            elif kind == 'expect' and st[1] == 'region':
                # expect region NAME X0 Y0 X1 Y1 MIN MAX: mean luminance of that box of the snap (or window shot)
                from PIL import Image, ImageStat
                path = os.path.join(RUN, 'snaps', st[2] + '_2.tga')
                if not os.path.exists(path):
                    path = os.path.join(RUN, 'shots', st[2] + '.bmp')
                good = os.path.exists(path)
                if good:
                    box = tuple(int(v) for v in st[3:7])
                    mean = ImageStat.Stat(Image.open(path).convert('L').crop(box)).mean[0]
                    print(f'     {st[2]} {box}: mean luminance {mean:.1f} (want {st[7]}..{st[8]})')
                    good = float(st[7]) <= mean <= float(st[8])
            elif kind == 'expect' and st[1] == 'tracked':
                # expect tracked NAME rose|moved MIN MAX: a track step's result
                v = tracks.get(st[2], {}).get(st[3])
                good = v is not None and float(st[4]) <= v <= float(st[5])
            elif kind == 'expect' and st[1] == 'fps':
                # expect fps MIN MAX: presented frames per second over the last renderer lines (about two seconds)
                time.sleep(2.5)
                log.poll()
                pts = []
                for l in log.lines:
                    m = re.match(r'\[\s*([0-9.]+)\].*renderer: frame (\d+)', l)
                    if m:
                        pts.append((float(m.group(1)), int(m.group(2))))
                rate = None
                if len(pts) >= 3:
                    (t0, f0), (t1, f1) = pts[-3], pts[-1]
                    rate = (f1 - f0) / (t1 - t0) if t1 > t0 and f1 > f0 else None
                print(f'     presented {rate:.1f} frames per second' if rate else '     no frame count')
                good = rate is not None and float(st[2]) <= rate <= float(st[3])
            elif kind == 'expect' and st[1] == 'menuaudio':
                # expect menuaudio [SOUND]: the menu sound audible at every mouse play in the run's audio capture
                # (CSX_AUDIO_WAV, CSX_LOG_MENU_SOUNDS=1; tools/check_menu_audio.py)
                time.sleep(1.5)
                log.poll()
                wav = env.get('CSX_AUDIO_WAV', '')
                cmd = [sys.executable, '-I', os.path.join(ROOT, 'tools', 'check_menu_audio.py'), wav,
                       os.path.join(RUN, 'run.out')] + (['--sound', st[2]] if len(st) > 2 else [])
                r = subprocess.run(cmd, capture_output=True, text=True)
                for l in (r.stdout + r.stderr).splitlines():
                    print('     ' + l)
                good = r.returncode == 0
            elif kind == 'expect' and st[1] == 'count':
                # expect count REGEX N: exactly N log lines match now (no spaces in REGEX: write . for them)
                time.sleep(1.2)  # the log flushes every second
                log.poll()
                rx = re.compile(st[2])
                have = sum(1 for l in log.lines if rx.search(l))
                good = have == int(st[3])
                if not good:
                    print(f'     {have} lines match')
            elif kind == 'expect':
                time.sleep(1.2)  # the log flushes every second
                log.poll()
                rx = ' '.join(st[2:])
                good = log.match(rx) if st[1] == 'log' else not log.match(rx)
            else:
                sys.exit('unknown step ' + ' '.join(st))
            print(f'{"ok  " if good else "FAIL"} {" ".join(st)}  [{time.time() - start:6.1f}s, {log.per_frame:.0f} draws/frame]')
            sys.stdout.flush()
            if not good:
                ok = False
                break
    finally:
        if wine:
            if alive():
                subprocess.run(['wineserver', '-k'], env=env)
            proc.wait(10)
        elif alive():
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
    if not wine and proc.returncode not in (None, 0, -2, 130) and ok:
        print(f'FAIL harness exited with {proc.returncode}')
        ok = False
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
