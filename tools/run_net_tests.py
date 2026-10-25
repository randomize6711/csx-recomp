#!/usr/bin/env python3
"""Runs network tests between separate game processes (the headless harness), all in one private network namespace
(docs/pc_multiplayer.md "Testing"). Each test is a file tests/net/NAME.txt with one section per process:

    # what the test shows
    # netem: delay 50ms 10ms loss 2%        (optional: tc netem on the namespace's loopback, every packet both ways)
    # timeout: 600                          (optional, seconds per process)
    [host] --launch tests/launch/de_aztec_2humans --env FPS=30
    <tools/drive.py steps>
    [client] --after host:net:.server.endpoint --launch tests/launch/connect_local
    <tools/drive.py steps>
    [client2] --after client:entered.the.game ...

A section's options go to tools/drive.py (--launch, --env, --pc ...); --after ROLE:REGEX starts it once ROLE's log
has a line matching REGEX (the regex has no spaces: use . for them), --delay S after S seconds. [ROLE*N] is N copies
of the section (ROLE1 .. ROLEN), --stagger S starting them S seconds apart. --exec "COMMAND" runs a command in the
namespace instead of a game (a tool: tools/net_fuzz.py), passing when it exits 0. --freeze-after S stops the section's
game process S seconds after it started (SIGSTOP: a client that went silent, for the timeouts) and ends it when the
test does. The processes are numbered from 1 in
order: process k binds 127.0.0.k (CSX_NET_BIND) and is INSTANCE k (at most 8: its hard disk folder and system link
address); each runs in build/nruns/NAME/ROLE. A test passes when every process's scenario passes. Output: each
process's failed or noted steps, then PASS / FAIL per test.

Usage: python3 -I tools/run_net_tests.py [tests/net/NAME.txt ...]   (no argument: every tests/net/*.txt)
       --exe PATH (default build/harness/CounterStrike), --keep (leave the namespace's processes on failure: no)
"""
import functools, glob, os, re, shlex, subprocess, sys, time

print = functools.partial(print, flush=True)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse(path):
    header, sections, cur = {}, [], None
    for line in open(path):
        line = line.rstrip('\n')
        m = re.match(r'\[(\w+)(?:\*(\d+))?\](.*)$', line)
        if m:
            copies = int(m.group(2) or 0)
            cur = {'role': m.group(1), 'opts': shlex.split(m.group(3)), 'steps': [], 'copies': copies}
            sections.append(cur)
            continue
        if cur is None:
            h = re.match(r'#\s*(netem|timeout):\s*(.*)$', line)
            if h:
                header[h.group(1)] = h.group(2).strip()
            continue
        cur['steps'].append(line)
    expanded = []
    for sec in sections:
        if not sec['copies']:
            expanded.append(sec)
            continue
        opts = list(sec['opts'])
        stagger = 0.0
        if '--stagger' in opts:
            i = opts.index('--stagger')
            stagger = float(opts[i + 1])
            del opts[i:i + 2]
        for n in range(sec['copies']):
            o = list(opts)
            if stagger and n:
                o += ['--wait', str(stagger * n)]
            expanded.append({'role': f"{sec['role']}{n + 1}", 'opts': o, 'steps': sec['steps']})
    return header, expanded


def game_pid(run, exe):
    # the game process whose working folder is the run folder
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            if os.readlink(f'/proc/{pid}/cwd') == run and os.readlink(f'/proc/{pid}/exe') == os.path.realpath(exe):
                return int(pid)
        except OSError:
            continue
    return None


def log_text(run):
    # a game's log, or a tool's (--exec) output
    text = ''
    for name in ('run.out', 'drive.out'):
        try:
            with open(os.path.join(run, name), errors='replace') as f:
                text += f.read()
        except OSError:
            pass
    return text


def run_test(path, exe):
    name = os.path.splitext(os.path.basename(path))[0]
    header, sections = parse(path)
    timeout = header.get('timeout', '900')
    netem = header.get('netem', '')
    shell = 'ip link set lo up' + (f'; tc qdisc add dev lo root netem {netem}' if netem else '') + '; exec sleep 100000'
    holder = subprocess.Popen(['unshare', '-rn', 'sh', '-c', shell])
    time.sleep(1)
    procs, runs, started = {}, {}, {}
    freezes, frozen, freeze_roles = [], [], set()
    try:
        pending = list(enumerate(sections))
        t0 = time.time()
        while pending or any(p.poll() is None for r, p in procs.items() if r not in freeze_roles):
            for k, sec in list(pending):
                opts = list(sec['opts'])
                after = delay = None
                if '--after' in opts:
                    i = opts.index('--after')
                    after = opts[i + 1]
                    del opts[i:i + 2]
                if '--delay' in opts:
                    i = opts.index('--delay')
                    delay = float(opts[i + 1])
                    del opts[i:i + 2]
                wait = 0.0
                if '--wait' in opts:  # a copy's stagger: seconds after its condition held
                    i = opts.index('--wait')
                    wait = float(opts[i + 1])
                    del opts[i:i + 2]
                ready = True
                if after:
                    role, regex = after.split(':', 1)
                    ready = role in runs and re.search(regex, log_text(runs[role])) is not None
                if delay is not None:
                    ready = ready and time.time() - t0 >= delay
                if ready and wait:
                    sec.setdefault('held_since', time.time())
                    ready = time.time() - sec['held_since'] >= wait
                if not ready:
                    who = after.split(':', 1)[0] if after else None
                    if who and (started.get(who, 0) is None or (who in procs and procs[who].poll() is not None)):
                        print(f'  {sec["role"]}: never started ({after} did not happen)')
                        pending.remove((k, sec))
                        started[sec['role']] = None
                    continue
                run = os.path.join(ROOT, 'build', 'nruns', name, sec['role'])
                os.makedirs(run, exist_ok=True)
                freeze = None
                if '--freeze-after' in opts:
                    i = opts.index('--freeze-after')
                    freeze = float(opts[i + 1])
                    del opts[i:i + 2]
                if '--exec' in opts:
                    i = opts.index('--exec')
                    cmd = ['nsenter', '--preserve-credentials', '-U', '-n', '-t', str(holder.pid)] + shlex.split(opts[i + 1])
                else:
                    scen = os.path.join(run, 'scenario.txt')
                    with open(scen, 'w') as f:
                        f.write('\n'.join(sec['steps']) + '\n')
                    cmd = [sys.executable, '-I', os.path.join(ROOT, 'tools', 'drive.py'), scen, '--run', run, '--exe', exe,
                           '--netns', str(holder.pid), '--env', f'INSTANCE={min(k + 1, 8)}',
                           '--env', f'CSX_NET_BIND=127.0.0.{k + 1}', '--timeout', timeout] + opts
                out = open(os.path.join(run, 'drive.out'), 'w')
                procs[sec['role']] = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=ROOT)
                if freeze is not None:
                    freezes.append((time.time() + freeze, run))
                    freeze_roles.add(sec['role'])
                runs[sec['role']] = run
                started[sec['role']] = time.time()
                pending.remove((k, sec))
            for f in list(freezes):
                if time.time() >= f[0]:
                    pid = game_pid(f[1], exe)
                    if pid:
                        os.kill(pid, 19)  # SIGSTOP
                        frozen.append(pid)
                        print(f'  froze the game in {f[1]} (pid {pid})')
                    freezes.remove(f)
            time.sleep(0.5)
        # a frozen game never ends its scenario: it is ended now and its result not counted
        for pid in frozen:
            for sig in (9, 18):
                try:
                    os.kill(pid, sig)
                except OSError:
                    pass
        for r in freeze_roles:
            if r in procs:
                try:
                    procs[r].wait(20)
                except subprocess.TimeoutExpired:
                    procs[r].kill()
        ok = all(p.returncode == 0 for r, p in procs.items() if r not in freeze_roles) and \
            all(v is not None for v in started.values())
        for role, run in runs.items():
            print(f'  [{role}] exit {procs[role].returncode}')
            with open(os.path.join(run, 'drive.out')) as f:
                for line in f:
                    if line.startswith(('FAIL', 'note')) or not line.startswith('ok'):
                        print('    ' + line.rstrip())
        return ok
    finally:
        for pid in frozen:
            for sig in (9, 18):  # SIGKILL, then SIGCONT so it can die
                try:
                    os.kill(pid, sig)
                except OSError:
                    pass
        for p in procs.values():
            if p.poll() is None:
                p.terminate()
        holder.kill()
        holder.wait()


def main():
    args = sys.argv[1:]
    exe = os.path.join(ROOT, 'build', 'harness', 'CounterStrike')
    if '--exe' in args:
        i = args.index('--exe')
        exe = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    tests = args or sorted(glob.glob(os.path.join(ROOT, 'tests', 'net', '*.txt')))
    failed = []
    for t in tests:
        print(f'== {t}')
        start = time.time()
        ok = run_test(t, exe)
        print(f'{"PASS" if ok else "FAIL"} {t}  [{time.time() - start:.0f} s]')
        if not ok:
            failed.append(t)
    print(f'{len(tests) - len(failed)} of {len(tests)} passed' + (': failed ' + ', '.join(failed) if failed else ''))
    sys.exit(1 if failed else 0)


main()
