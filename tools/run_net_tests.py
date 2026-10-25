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
namespace instead of a game (a tool: tools/net_fuzz.py), passing when it exits 0. --server "OPTIONS" runs the dedicated
server (build/server/csx-server, tools/build_server.sh) with those options instead of the game (drive.py --server). --file NAME=PATH copies PATH (from the
repository root) into the section's run folder as NAME before it starts (a key, admins.cfg). --freeze-after S stops the section's
game process S seconds after it started (SIGSTOP: a client that went silent, for the timeouts) and ends it when the
test does. The processes are numbered from 1 in
order: process k binds 127.0.0.k (CSX_NET_BIND) and is INSTANCE k (its hard disk folder and system link address;
System Link's discovery reaches instances 1 to 8 only, the PC transport any); each runs in build/nruns/NAME/ROLE (the folder is emptied first). A test passes when every process's
scenario passes. Output: each process's verdict (drive.py's RESULT: PASS, FAIL, TIMEOUT, CRASH; NOT STARTED when its
--after never happened) with its failed or noted steps, then the test's verdict, the worst of them, also written to
build/nruns/NAME/result.txt (RUNNING while the test runs, so a runner killed outright leaves no stale verdict);
INTERRUPTED when the runner is stopped (SIGINT, SIGTERM, SIGHUP).

Usage: python3 -I tools/run_net_tests.py [tests/net/NAME.txt ...]   (no argument: every tests/net/*.txt)
       --exe PATH (default build/harness/CounterStrike), --keep (leave the namespace's processes on failure: no)
"""
import functools, glob, os, re, shlex, shutil, signal, subprocess, sys, time

print = functools.partial(print, flush=True)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER = os.path.join(ROOT, 'build', 'server', 'csx-server')


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


class Resources:
    """A game process's CPU, memory, threads and open file descriptors, sampled every 2 s into ROLE/resources.csv
    (a relaunch keeps the process: descriptors that grow from one launch to the next are a leak)."""
    TICK = os.sysconf('SC_CLK_TCK')

    def __init__(self, run, exe):
        self.run, self.exe, self.pid, self.rows, self.last = run, exe, None, [], 0.0
        self.first_fds = self.last_fds = None  # {fd: what it is}, at the first and the last sample

    def sample(self, now):
        if now - self.last < 2.0:
            return
        self.last = now
        if self.pid is None:
            self.pid = game_pid(self.run, self.exe)
            if self.pid is None:
                return
        try:
            with open(f'/proc/{self.pid}/stat') as f:
                fields = f.read().rsplit(')', 1)[1].split()
            cpu = (int(fields[11]) + int(fields[12])) / self.TICK  # utime + stime
            status = dict(l.split(':', 1) for l in open(f'/proc/{self.pid}/status') if ':' in l)
            rss = int(status['VmRSS'].split()[0]) / 1024
            hwm = int(status['VmHWM'].split()[0]) / 1024
            threads = int(status['Threads'])
            names = {}
            for fd in os.listdir(f'/proc/{self.pid}/fd'):
                try:
                    names[int(fd)] = os.readlink(f'/proc/{self.pid}/fd/{fd}')
                except OSError:
                    pass
            fds = len(names)
        except (OSError, KeyError, IndexError, ValueError):
            return
        if self.first_fds is None:
            self.first_fds = names
        self.last_fds = names
        self.rows.append((now, cpu, rss, hwm, threads, fds))

    def summary(self):
        if len(self.rows) < 2:
            return None
        with open(os.path.join(self.run, 'resources.csv'), 'w') as f:
            f.write('time,cpu_s,rss_mb,peak_rss_mb,threads,fds\n')
            t0 = self.rows[0][0]
            for r in self.rows:
                f.write(f'{r[0] - t0:.1f},{r[1]:.2f},{r[2]:.1f},{r[3]:.1f},{r[4]},{r[5]}\n')
        # ROLE/fds.txt: the descriptors open at the end, "(again)" on a file open more times than at the first sample
        # (a relaunch that kept the old image's handles shows as each of its files twice); sockets by kind only
        def kind(name):
            return 'socket' if name.startswith('socket:') else name
        before = {}
        for name in self.first_fds.values():
            before[kind(name)] = before.get(kind(name), 0) + 1
        seen = {}
        with open(os.path.join(self.run, 'fds.txt'), 'w') as f:
            for fd, name in sorted(self.last_fds.items()):
                seen[kind(name)] = seen.get(kind(name), 0) + 1
                extra = seen[kind(name)] > max(1, before.get(kind(name), 0)) and kind(name) != 'socket'
                f.write(f'{fd} {name}{"  (again)" if extra else ""}\n')
        (t0, c0, *_), (t1, c1, *_) = self.rows[0], self.rows[-1]
        peak = max((b[1] - a[1]) / (b[0] - a[0]) for a, b in zip(self.rows, self.rows[1:]) if b[0] > a[0])
        fds = [r[5] for r in self.rows]
        return (f'cpu {100 * (c1 - c0) / (t1 - t0):.0f}% of a core on average (peak {100 * peak:.0f}%), memory '
                f'{self.rows[-1][2]:.0f} MB (peak {max(r[3] for r in self.rows):.0f} MB), threads {self.rows[-1][4]}, '
                f'file descriptors {fds[0]} at the start, {max(fds)} at most, {fds[-1]} at the end')


def games_under(folder, exe):
    # game processes working in folder or below it (also one whose folder was deleted and made again since)
    found = []
    for pid in os.listdir('/proc'):
        if not pid.isdigit():
            continue
        try:
            cwd = os.readlink(f'/proc/{pid}/cwd').removesuffix(' (deleted)')
            if os.readlink(f'/proc/{pid}/exe').removesuffix(' (deleted)') == os.path.realpath(exe) and \
                    (cwd == folder or cwd.startswith(folder + os.sep)):
                found.append(int(pid))
        except OSError:
            continue
    return found


def end_games(folder, exe, why):
    for pid in games_under(folder, exe):
        print(f'  ended a game left running in {folder} (pid {pid}, {why})')
        for sig in (9, 18):  # SIGKILL, then SIGCONT for a frozen one
            try:
                os.kill(pid, sig)
            except OSError:
                pass


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


def role_verdict(run, code, frozen):
    # a game's verdict is its drive.py RESULT line (PASS, FAIL, TIMEOUT, CRASH); a tool's (--exec) its exit code; a
    # frozen game (--freeze-after) is ended by the runner, so only a crash before the freeze counts
    result = None
    try:
        for line in open(os.path.join(run, 'drive.out'), errors='replace'):
            if line.startswith('RESULT '):
                result = line.split()[1]
    except OSError:
        pass
    if frozen:
        return 'CRASH' if result == 'CRASH' else 'FROZEN'
    if result:
        return result
    if os.path.exists(os.path.join(run, 'scenario.txt')):
        return 'FAIL'  # drive.py ended without a verdict (killed)
    return 'PASS' if code == 0 else 'FAIL'


def write_result(name, verdict, verdicts):
    # build/nruns/NAME/result.txt: the verdict even when nobody saw the runner's output (a stopped session)
    d = os.path.join(ROOT, 'build', 'nruns', name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, 'result.txt'), 'w') as f:
        f.write(f'{verdict} {time.strftime("%Y-%m-%d %H:%M:%S")}\n')
        for role, v in verdicts.items():
            f.write(f'{role} {v}\n')


def run_test(path, exe):
    name = os.path.splitext(os.path.basename(path))[0]
    header, sections = parse(path)
    timeout = header.get('timeout', '900')
    netem = header.get('netem', '')
    shell = 'ip link set lo up' + (f'; tc qdisc add dev lo root netem {netem}' if netem else '') + '; exec sleep 100000'
    # a game an earlier run left (a killed runner) polls this test's folders by path: it would take this run's step files
    for e in (exe, SERVER):
        end_games(os.path.join(ROOT, 'build', 'nruns', name), e, 'from an earlier run')
    # an earlier run's logs would satisfy --after conditions before this run's processes have written anything
    shutil.rmtree(os.path.join(ROOT, 'build', 'nruns', name), ignore_errors=True)
    # left as it is if the runner is killed outright (SIGKILL): a test without a verdict, not an old one
    write_result(name, 'RUNNING', {})
    holder = subprocess.Popen(['unshare', '-rn', 'sh', '-c', shell])
    time.sleep(1)
    procs, runs, started, resources = {}, {}, {}, {}
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
                # --file NAME=PATH: PATH (from the repository root) copied into the run folder as NAME first
                while '--file' in opts:
                    i = opts.index('--file')
                    dest, src = opts[i + 1].split('=', 1)
                    shutil.copy(os.path.join(ROOT, src), os.path.join(run, dest))
                    os.chmod(os.path.join(run, dest), 0o600)
                    del opts[i:i + 2]
                role_exe = exe
                if '--server' in opts:
                    role_exe = SERVER
                    opts += ['--exe', SERVER]
                if '--exec' in opts:
                    i = opts.index('--exec')
                    cmd = ['nsenter', '--preserve-credentials', '-U', '-n', '-t', str(holder.pid)] + shlex.split(opts[i + 1])
                else:
                    scen = os.path.join(run, 'scenario.txt')
                    with open(scen, 'w') as f:
                        f.write('\n'.join(sec['steps']) + '\n')
                    cmd = [sys.executable, '-I', os.path.join(ROOT, 'tools', 'drive.py'), scen, '--run', run, '--exe', exe,
                           '--netns', str(holder.pid), '--env', f'INSTANCE={k + 1}',
                           '--env', f'CSX_NET_BIND=127.0.0.{k + 1}', '--timeout', timeout] + opts
                out = open(os.path.join(run, 'drive.out'), 'w')
                procs[sec['role']] = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT, cwd=ROOT)
                if freeze is not None:
                    freezes.append((time.time() + freeze, run))
                    freeze_roles.add(sec['role'])
                runs[sec['role']] = run
                started[sec['role']] = time.time()
                if '--exec' not in opts:
                    resources[sec['role']] = Resources(run, role_exe)
                pending.remove((k, sec))
            for r in resources.values():
                r.sample(time.time())
            for f in list(freezes):
                if time.time() >= f[0]:
                    pid = game_pid(f[1], exe) or game_pid(f[1], SERVER)
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
        verdicts = {}
        for role, run in runs.items():
            verdicts[role] = role_verdict(run, procs[role].returncode, role in freeze_roles)
            print(f'  [{role}] {verdicts[role]} (exit {procs[role].returncode})')
            res = resources.get(role)
            if res and (text := res.summary()):
                print(f'    {text}')
            with open(os.path.join(run, 'drive.out')) as f:
                for line in f:
                    if line.startswith(('FAIL', 'note')) or not line.startswith(('ok', 'RESULT')):
                        print('    ' + line.rstrip())
        for role, t in started.items():
            if t is None:
                verdicts[role] = 'NOT STARTED'
        # the test's verdict: the worst of its processes' (a crash outranks a timeout, a timeout a failed check)
        order = ['PASS', 'FROZEN', 'FAIL', 'NOT STARTED', 'TIMEOUT', 'CRASH']
        verdict = max(verdicts.values(), key=order.index) if verdicts else 'FAIL'
        if verdict == 'FROZEN':
            verdict = 'PASS'
        write_result(name, verdict, verdicts)
        return verdict
    except KeyboardInterrupt:
        write_result(name, 'INTERRUPTED', {r: 'INTERRUPTED' for r in runs})
        print(f'INTERRUPTED {path} (the runner was stopped: its processes are ended, no result)')
        raise
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
        for p in procs.values():
            try:
                p.wait(15)
            except subprocess.TimeoutExpired:
                p.kill()
        for e in (exe, SERVER):
            end_games(os.path.join(ROOT, 'build', 'nruns', name), e, 'after its scenario ended')
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
    # a SIGTERM or SIGHUP (a stopped session, a closed terminal) ends the processes and says so, as Ctrl+C does
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    failed = []
    for t in tests:
        print(f'== {t}')
        start = time.time()
        try:
            verdict = run_test(t, exe)
        except KeyboardInterrupt:
            sys.exit(130)
        print(f'{verdict} {t}  [{time.time() - start:.0f} s]')
        if verdict != 'PASS':
            failed.append(f'{os.path.basename(t)} ({verdict})')
    print(f'{len(tests) - len(failed)} of {len(tests)} passed' + (': ' + ', '.join(failed) if failed else ''))
    sys.exit(1 if failed else 0)


main()
