#!/usr/bin/env python3
"""Drives the headless harness through a scenario by watching its log.

Usage: python3 -I tools/drive.py SCENARIO [--timeout S] [--env K=V ...]

The harness runs in build/run (network namespace, reflink game copy in build/gamedata). The scenario file has one step
per line, executed in order:
    wait draws>=N           until a frame logs at least N draws per frame (from the 60-frame renderer lines)
    wait draws==N           until the draws per frame equal N
    wait log REGEX          until a log line matches
    sleep S                 wait S seconds
    press BUTTONS [FRAMES]  press through the harness cmd file (buttons as in the pad script, joined by +)
    pressuntil BUTTONS COND press every 2 s until COND (draws>=N or draws==N) holds, at most 15 times
    snap NAME               draw the next two frames and keep them as build/run/snaps/NAME_1.tga / NAME_2.tga
    expect log REGEX        fail unless a log line matches
    expect nolog REGEX      fail if a log line matches
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
EXE = os.path.join(ROOT, 'build', 'harness', 'csx_headless')
GAME = os.path.join(ROOT, 'build', 'gamedata')
FRAME_RE = re.compile(r'renderer: frame (\d+), (\d+) draws')


class Log:
    def __init__(self):
        self.lines = []
        self.path = None
        self.prev = None  # (frame, draws) of the previous renderer line
        self.per_frame = 0.0

    def poll(self):
        logs = os.path.join(RUN, 'logs')
        if self.path is None and os.path.isdir(logs):
            names = sorted(os.listdir(logs))
            if names:
                self.path = os.path.join(logs, names[0])
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
                self.prev = (frame, draws)

    def match(self, rx):
        r = re.compile(rx)
        return any(r.search(l) for l in self.lines)


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
    i = 1
    while i < len(args):
        if args[i] == '--timeout':
            timeout = float(args[i + 1]); i += 2
        elif args[i] == '--env':
            k, v = args[i + 1].split('=', 1); env[k] = v; i += 2
        else:
            sys.exit('unknown argument ' + args[i])
    steps = [l.split() for l in open(scenario) if l.strip() and not l.startswith('#')]

    for d in ('logs', 'hdd', 'cap', 'snaps'):
        shutil.rmtree(os.path.join(RUN, d), ignore_errors=True)
    os.makedirs(os.path.join(RUN, 'snaps'))
    out = open(os.path.join(RUN, 'run.out'), 'w')
    proc = subprocess.Popen(['unshare', '-rn', EXE, GAME], cwd=RUN, env=env, stdout=out, stderr=subprocess.STDOUT)
    log, start, ok = Log(), time.time(), True

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
                    m = re.match(r'draws(>=|==)(\d+)', st[1])
                    n = int(m.group(2))
                    good = wait_until(lambda: log.per_frame >= n if m.group(1) == '>=' else round(log.per_frame) == n)
            elif kind == 'sleep':
                time.sleep(float(st[1])); good = alive()
            elif kind == 'press':
                frames = st[2] if len(st) > 2 else '8'
                send(f'{st[1]} {frames}\n')
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'cmd')))
            elif kind == 'pressuntil':
                m = re.match(r'draws(>=|==)(\d+)', st[2])
                n = int(m.group(2))
                cond = lambda: log.per_frame >= n if m.group(1) == '>=' else round(log.per_frame) == n
                good = False
                for _ in range(15):
                    send(f'{st[1]} 8\n')
                    t0 = time.time()
                    while time.time() - t0 < 2.0 and alive():
                        log.poll()
                        time.sleep(0.2)
                    log.poll()
                    if cond():
                        good = True
                        break
                    if not alive():
                        break
            elif kind == 'snap':
                open(os.path.join(RUN, 'snap.tmp'), 'w').close()
                os.replace(os.path.join(RUN, 'snap.tmp'), os.path.join(RUN, 'snap'))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'snap')))
                time.sleep(0.5)
                for k in (1, 2):
                    src = os.path.join(RUN, f'snap_{k}.tga')
                    if os.path.exists(src):
                        shutil.move(src, os.path.join(RUN, 'snaps', f'{st[1]}_{k}.tga'))
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
