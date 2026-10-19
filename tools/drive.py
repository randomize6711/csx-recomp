#!/usr/bin/env python3
"""Drives the headless harness through a scenario by watching its log.

Usage: python3 -I tools/drive.py SCENARIO [--timeout S] [--env K=V ...] [--exe PATH] [--run DIR] [--launch DIR]

The harness runs in build/run (network namespace, reflink game copy in build/gamedata). The scenario file has one step
per line, executed in order:
    wait draws>=N           until a frame logs at least N draws per frame (from the 60-frame renderer lines)
    wait draws==N           until the draws per frame equal N (also draws<=N)
    wait log REGEX          until a log line matches
    sleep S                 wait S seconds
    press BUTTONS [FRAMES]  press through the harness cmd file (buttons as in the pad script, joined by +)
    key NAME [SECONDS]      window build: press an SDL key (SDL key name) through the window's keys file
    windowshot NAME         window build: save the window's output as RUN/shots/NAME.bmp
    pressuntil BUTTONS COND press, wait for two fresh draw measurements, repeat until COND holds (at most 15)
    dump NAME               guest memory dump to build/run/dumps/NAME.bin (src/common/memory_dump.cpp)
    state NAME              log the local player's state (src/game/state_probe.cpp) and keep it as NAME
    expect moved A B MIN MAX       horizontal distance between states A and B in [MIN, MAX] game units
    expect turned A B MIN          yaw difference between states A and B (shortest way) at least MIN degrees
    expect shots A B N             the shot counter dropped by exactly N from A to B
    expect still A B               position and yaw equal in A and B
    expect heading A B MAXDEG      the move from A to B points along A's yaw within MAXDEG degrees
    snap NAME               draw the next two frames and keep them as build/run/snaps/NAME_1.tga / NAME_2.tga
    expect log REGEX        fail unless a log line matches
    expect nolog REGEX      fail if a log line matches
    expect image NAME MIN   fail unless snaps/NAME_2.tga has a luminance standard deviation of at least MIN
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
    i = 1
    while i < len(args):
        if args[i] == '--timeout':
            timeout = float(args[i + 1]); i += 2
        elif args[i] == '--exe':  # another front end, e.g. build/window/CounterStrike
            global EXE
            EXE = os.path.abspath(args[i + 1]); i += 2
            env.setdefault('CSX_TEST_INPUT_ONLY', '1')  # a window front end: only the scenario's keys reach the title
        elif args[i] == '--run':
            global RUN
            RUN = os.path.abspath(args[i + 1]); i += 2
        elif args[i] == '--env':
            k, v = args[i + 1].split('=', 1); env[k] = v; i += 2
        elif args[i] == '--launch':
            launch = os.path.abspath(args[i + 1]); i += 2
        else:
            sys.exit('unknown argument ' + args[i])
    steps = [l.split() for l in open(scenario) if l.strip() and not l.startswith('#')]

    for d in ('logs', 'hdd', 'cap', 'snaps', 'dumps', 'shots'):
        shutil.rmtree(os.path.join(RUN, d), ignore_errors=True)
    os.makedirs(os.path.join(RUN, 'snaps'))
    if launch:
        # what the front end leaves for its relaunch into a match: the launch data page (launch.bin, kept by the host as
        # launch_seen.bin) and the match's command line on the cache partition (Z:\releasemap.txt); with both the
        # title boots straight into that match
        os.makedirs(os.path.join(RUN, 'hdd', 'Z'))
        shutil.copyfile(os.path.join(launch, 'launch.bin'), os.path.join(RUN, 'hdd', 'launch.bin'))
        shutil.copyfile(os.path.join(launch, 'releasemap.txt'), os.path.join(RUN, 'hdd', 'Z', 'releasemap.txt'))
    out = open(os.path.join(RUN, 'run.out'), 'w')
    proc = subprocess.Popen(['unshare', '-rn', EXE, GAME], cwd=RUN, env=env, stdout=out, stderr=subprocess.STDOUT)
    log, start, ok = Log(), time.time(), True
    states = {}

    def alive():
        return proc.poll() is None

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
            elif kind == 'state':
                send_file('state', st[1])
                rx = re.compile(r'state ' + re.escape(st[1]) + r' (\S+) (\S+) (\S+) (\S+) (-?\d+)')
                def got():
                    for l in log.lines:
                        m = rx.search(l)
                        if m:
                            states[st[1]] = [float(v) for v in m.groups()]
                            return True
                    return False
                good = wait_until(got)
                if good:
                    print('     ' + st[1] + ' = ' + ' '.join(str(v) for v in states[st[1]]))
            elif kind == 'expect' and st[1] in ('moved', 'turned', 'shots', 'still', 'heading'):
                a, b = states.get(st[2]), states.get(st[3])
                good = a is not None and b is not None
                if good and st[1] == 'moved':
                    dist = ((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5
                    good = float(st[4]) <= dist <= float(st[5])
                    print(f'     moved {dist:.1f}')
                elif good and st[1] == 'turned':
                    dy = abs((b[3] - a[3] + 180) % 360 - 180)
                    good = dy >= float(st[4])
                    print(f'     turned {dy:.1f} degrees')
                elif good and st[1] == 'shots':
                    good = a[4] - b[4] == int(st[4])
                    print(f'     shot counter {int(a[4])} -> {int(b[4])}')
                elif good and st[1] == 'still':
                    good = a[:4] == b[:4]
                elif good and st[1] == 'heading':
                    import math
                    move = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
                    off = abs((move - a[3] + 180) % 360 - 180)
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
                good = os.path.exists(path) and ImageStat.Stat(Image.open(path).convert('L')).stddev[0] >= float(st[3])
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
        if alive():
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
    if proc.returncode not in (None, 0, -2, 130) and ok:
        print(f'FAIL harness exited with {proc.returncode}')
        ok = False
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
