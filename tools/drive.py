#!/usr/bin/env python3
"""Drives the headless harness through a scenario by watching its log.

Usage: python3 -I tools/drive.py SCENARIO [--timeout S] [--env K=V ...] [--exe PATH] [--run DIR] [--launch DIR] [--wine]
                                [--pc NAME=VALUE ...] [--netns PID]

--pc seeds the run folder's pc_settings.ini (Options > PC Options); without it every run starts with none.

--netns PID runs the game in the network namespace of process PID (a holder tools/run_net_tests.py makes) instead of
a private one of its own, so several games reach each other there; give each an INSTANCE (--env INSTANCE=N: it binds
127.0.0.N and keeps its hard disk in hdd, hdd2 .. hdd8).

--server "OPTIONS" runs the dedicated server (build/server/csx-server unless --exe names another, tools/build_server.sh)
as "csx-server --game GAME OPTIONS" in the run folder: it prepares its own disk and match (no --launch); the steps
that read the log, players, pckey cmd (its console) work as with a game.

--wine runs the Windows build (build/windows/dist/CounterStrike.exe unless --exe names another) under Wine, prefix
build/wineprefix, in a network namespace like the Linux runs. A title relaunch there starts a new Windows process, so
the run is alive while any process of the exe is.

The harness runs in build/run (network namespace, reflink game copy in build/gamedata). The scenario file has one step
per line, executed in order:
    wait draws>=N           until a frame logs at least N draws per frame (from the 60-frame renderer lines)
    wait draws==N           until the draws per frame equal N (also draws<=N)
    wait log REGEX          until a log line matches
    wait newlog REGEX       until a line written since the last input step (press, key, click, pckey...) or mark
                            matches
    mark                    the log's place for the next wait newlog (a state reached again with no input)
    pad N attach|detach|button NAME [SECONDS]|axis NAME VALUE   split-screen compositor: test controller N (SDL
                            gamepad names: a, b, x, y, start, back, dpup, leftx, righty, lefttrigger...)
    spawn COMMAND...        a helper process (from the repository root) in the game's network namespace, loopback up
                            there; ended with the scenario (tools/master's csx-fakeservers for the browser's window tests)
    sleep S                 wait S seconds
    press BUTTONS [FRAMES]  press through the harness cmd file (buttons as in the pad script, joined by +)
    key NAME [SECONDS]      window build: press an SDL key (SDL key name; NAME#SCANCODE: the key a layout puts at
                            that position, e.g. Å#[ on a Swedish keyboard) through the window's keys file
    point X Y               window build: the mouse pointer to (X, Y) of the menus' 640x480, as SDL motion
    winctl fullscreen 1|0   window build: the window made full screen or windowed from outside (a compositor's key);
                            split-screen compositor: winctl kill N SIGNAL sends player N's process SIGNAL (a crash)
    @N STEP                 split-screen: STEP's request files in player N's folder (RUN/splitN), e.g. @2 state a
    pointat X Y             window build: the mouse pointer to (X, Y) of the window (logical pixels), as SDL motion
    type TEXT               window build: TEXT as SDL text input (what a keyboard layout types: the console's input)
    windowshot NAME         window build: save the window's output as RUN/shots/NAME.bmp
    windowburst NAME N      Vulkan window: the next N presented frames as RUN/shots/NAME_0.bmp .. NAME_<N-1>.bmp
    pressuntil BUTTONS COND press, wait for two fresh draw measurements, repeat until COND holds (at most 15)
    dump NAME               guest memory dump to build/run/dumps/NAME.bin (src/common/memory_dump.cpp)
    state NAME              log the local player's state (src/game/state_probe.cpp) and keep it as NAME
    exposure NAME           the next "dynamic gamma" log line (CSX_LOG_DYNGAMMA=1, src/game/dynamic_gamma.cpp), kept as NAME
    fade NAME               the last screen fade once it has ended (CSX_LOG_FADE=1, src/game/screen_fade.cpp), kept as NAME:
                            alpha, hold and length (planned, from its start), lasted and real (as it ran: client, real s),
                            overrun (lasted - length) and drift (real - lasted)
    track NAME SECONDS      follow the player's position that long and log how high it rose and how far it moved
    expect moved A B MIN MAX       horizontal distance between states A and B in [MIN, MAX] game units
    expect yaw A B DEG TOL         view yaw (0x552C10) changed by DEG (+ left) within TOL; expect pitch likewise
    expect turned A B MIN          yaw difference between states A and B (shortest way) at least MIN degrees
    expect shots A B N             the shot counter dropped by exactly N from A to B
    expect still A B               position and yaw equal in A and B
    expect steady NAME N           the window burst NAME_0..N-1 has no frame showing the picture before last again (flicker)
    expect tracked NAME rose|moved MIN MAX  a track step's height risen or distance moved in [MIN, MAX]
    expect fps MIN MAX             presented frames per second (the renderer's frame lines) in [MIN, MAX]
    expect mousespread MAX         the last whole seconds of a mouse stream (every look pass moved, src/game/
                                   mouse_look.cpp) turned evenly: spread (deviation over mean per pass) at most MAX
    expect exposure NAME FIELD MIN MAX  an exposure step's FIELD (dark, light, mean: what the title measured, 0..1; low,
                                   high, middle: its gamma ramp's points) in [MIN, MAX]
    expect exposurediff A B FIELD MIN MAX  FIELD of exposure B less that of exposure A in [MIN, MAX]
    expect exposuretrack NAME TOL  the ramp settled on what was measured: low within TOL of dark, middle of mean
    expect fade NAME FIELD MIN MAX  a fade step's FIELD in [MIN, MAX]
    expect fadediff A B FIELD MIN MAX  FIELD of fade B less that of fade A in [MIN, MAX]
    expect setting NAME VALUE      the run's pc_settings.ini holds NAME = VALUE (what PC Options saved)
    expect apart NAME N M MIN MAX  clients N and M horizontally MIN..MAX apart in players step NAME
    expect bodymoved A B N MIN MAX client N moved MIN..MAX (horizontally) from players step A to B
    expect health NAME N MIN MAX   client N's health in players step NAME
    expect bodyz NAME N MIN MAX    client N's origin height in players step NAME
    expect healthdrop A B N MIN MAX  client N's health fell MIN..MAX from players step A to B
    expect spawnapart NAME MIN     every two clients at least MIN apart in players step NAME
    expect playercount NAME MIN MAX  the number of clients (the player and the bots) in players step NAME
    expect bodiesmoved A B MIN N   at least N clients moved MIN or more from players step A to B
    expect teams MAXDIFF [MIN]     the players' last joined teams (the game's log): CT and T within MAXDIFF, each
                                   at least MIN
    expect effects BLOOM GAMMA     within 5 s the cvars the optional Xbox effects are read cl_hdrbloom BLOOM and
                                   cl_dynamicgamma GAMMA (as logged, %g: 0, 0.5, 1; needs CSX_LOG_EFFECTS=1)
    expect movetime MIN MAX        the movement commands' milliseconds per real second (the last two "pc frame" lines,
                                   src/game/pc_frame.cpp) in [MIN, MAX]: the game's movement keeps real time
    expect weapon A ID             the weapon in hand at A (17 Glock, 29 knife, 4 HE grenade, 25 flashbang, 9 smoke)
    expect sameweapon A B 1|0      the weapon in hand at A and B the same (1) or different (0)
    expect rose A B MIN            the player rose at least MIN units from A to B (a jump)
    expect heading A B MAXDEG [OFF] the move from A to B points along A's yaw (+ OFF degrees: 90 left, -90 right, 180
                                   back, -45 forward right) within MAXDEG degrees
    mouse DX DY             (window build) relative mouse motion in counts, through the window's mouse look
    mousestream DX DY N HZ  (window build) N relative motions of DX DY counts at HZ a second from an SDL timer
                            thread (they arrive between the window's polls, as a mouse's do); waits until all are sent
    count REGEX N           until N log lines match REGEX (no spaces: write . for them)
    look RIGHT UP           turn the view by degrees through the mouse look hook (src/game/mouse_look.cpp)
    place N X Y Z           client N (0 the player, then the bots) put at that origin once, no velocity
    players NAME            every active client's origin and health logged and kept as NAME
    bring N DIST            client N put DIST units in front of client 0 along its view, on the server (network tests)
    aim                     the view turned to the nearest other player this client knows (network tests)
    spawns NAME             the map's spawn points per team, the edicts in use and maxclients logged
    setview X Y Z YAW PITCH the player held at that origin (server side, no velocity) with those view angles
                            (src/game/state_probe.cpp): a fixed camera for picture tests; setview off lets go
    pokefile FILE           every "VA FLOAT" line of FILE (relative to the repository) written at once
    poke VA FLOAT           write FLOAT to guest address VA (hex) once, src/common/memory_dump.cpp (diagnostics)
    click BUTTON [SECONDS]  (window build) a mouse button (1 left, 2 middle, 3 right) through SDL; wheel N: wheel notches
    pckey cmd TEXT          console text through Cbuf_AddText; pckey key KEYNUM DOWN: an engine key event (pc_keys.cpp);
                            pckey set NAME VALUE: a PC setting changed and saved, as PC Options' Accept does
    pckey tap +CMD SECONDS  the game presses +CMD and releases it SECONDS later on its own clock (pc_keys.cpp), the
                            buy menu's tap (+buymenu, 0.05 to 0.5 s); fails unless the logged hold is SECONDS to + 0.1
    snap NAME               draw the next two frames and keep them as build/run/snaps/NAME_1.tga / NAME_2.tga
    expect region NAME X0 Y0 X1 Y1 MIN MAX  mean luminance of that box of snaps/NAME_2.tga (or shots/NAME.bmp) in [MIN, MAX]
    expect darkfrac NAME X0 Y0 X1 Y1 LUM MIN MAX  share of that box's pixels darker than LUM in [MIN, MAX]
    expect bluefrac NAME X0 Y0 X1 Y1 MIN MAX  share of that box's pixels whose blue exceeds red by 12 or more in
                                   [MIN, MAX] (the HUD's blue-grey numbers over a dark weapon; ground and weapon have none)
    expect chroma NAME X0 Y0 X1 Y1 MIN MAX  mean chroma (max - min of RGB) of that box in [MIN, MAX]: how much of a
                            surface's colour shows through a blended layer over it (water)
    expect probealpha NAME X Y MIN MAX  the first SRCALPHA blended draw at back buffer pixel X,Y in each of snap NAME's
                            frames has a source alpha in [MIN, MAX] (reference renderer, run with --env PROBES=X,Y)
    expect regiondiff A B X0 Y0 X1 Y1 MIN MAX  mean luminance of that box in B less that in A (snaps or window shots)
    expect seam NAME X Y MAX       the box from the top left corner to X, Y no more than MAX brighter across its right
                                   and bottom edges (the bloom's leaked scissor, FINDINGS.md "Bloom")
    expect coverage PRE SHOT MIN SPREAD  a white full-screen fade reaches the whole frame evenly: in nine patches touching
                            its edges (centre, the four edges' middles, the four corners; 2% of the frame's height
                            square, medians) the fade's opacity (SHOT - PRE) / (255 - PRE) is at least MIN, and within
                            SPREAD of each other; a patch already near white in PRE (HUD) only has to stay as bright
    expect log REGEX        fail unless a log line matches
    expect quiet REGEX S    wait S seconds; fail if a log line logged meanwhile matches (no spaces: write . for them)
    expect nolog REGEX      fail if a log line matches
    expect count REGEX N    fail unless exactly N log lines match (no spaces: write . for them)
    expect before A B       in this instance's log (since the last relaunch) a line matching A comes before the first
                            one matching B, and both are there (no spaces: write . for them)
    expect menuaudio [SOUND]  the menu sound (menu/buy_hover.wav) audible at each mouse play in the run's capture
                            (needs CSX_AUDIO_WAV and CSX_LOG_MENU_SOUNDS=1; tools/check_menu_audio.py)
    expect image NAME MIN   fail unless snaps/NAME_2.tga (or the window shot shots/NAME.bmp) has a luminance standard deviation of at least MIN
                            (a blank or flat frame fails; game pixels are never stored in the repository)
Exit status 0 when every step passed, 1 otherwise. Prints one line per step.
"""
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
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
        self.partial = ''  # a line the game has not finished writing (its log is flushed in blocks, not lines)
        self.run = RUN     # the run folder whose log this is (an @N step's RUN is a player's folder, not the log's)

    def poll(self):
        logs = os.path.join(self.run, 'logs')
        # a relaunched instance (same process, exec) starts a new log file: follow the newest
        if os.path.isdir(logs):
            names = sorted(os.listdir(logs))
            if names and (self.path is None or os.path.join(logs, names[-1]) != self.path):
                if self.path is not None:
                    self.lines.extend(self.read(final=True))
                    self.lines.append('--- relaunch: ' + names[-1] + '\n')
                    self.prev = None
                self.path = os.path.join(logs, names[-1])
                self.f = open(self.path, 'r', errors='replace')
        if self.path is None:
            return
        for line in self.read():
            self.lines.append(line)
            m = FRAME_RE.search(line)
            if m:
                frame, draws = int(m.group(1)), int(m.group(2))
                if self.prev and frame > self.prev[0]:
                    self.per_frame = (draws - self.prev[1]) / (frame - self.prev[0])
                    self.samples += 1
                self.prev = (frame, draws)

    def read(self, final=False):
        # the whole lines written since the last read; a line cut by a flush waits for its end (the old instance's
        # last one, at a relaunch, is taken as it is)
        parts = (self.partial + self.f.read()).split('\n')
        self.partial = '' if final else parts.pop()
        return [p + '\n' for p in parts if p or not final]

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
    netns = None
    server = None
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
        elif args[i] == '--netns':
            netns = args[i + 1]; i += 2
        elif args[i] == '--server':
            server = shlex.split(args[i + 1]); i += 2
            if EXE.endswith(os.path.join('harness', 'CounterStrike')):
                EXE = os.path.join(ROOT, 'build', 'server', 'csx-server')
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

    # an instance past the first keeps its hard disk in hddN (src/common: app::Instance)
    instance = int(env.get('INSTANCE', '0') or 0)
    hdd = 'hdd' + (str(instance) if instance > 1 else '')
    for d in ('logs', 'cap', 'snaps', 'dumps', 'shots') + tuple('hdd' + n for n in ('', '2', '3', '4', '5', '6', '7', '8', '9')):
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
            shutil.copytree(zcache, os.path.join(RUN, hdd, 'Z'))
        else:
            os.makedirs(os.path.join(RUN, hdd, 'Z'))
            print('note: no build/zcache (tools/make_zcache.sh): the match starts without the cached sound banks')
        shutil.copyfile(os.path.join(launch, 'launch.bin'), os.path.join(RUN, hdd, 'launch.bin'))
        shutil.copyfile(os.path.join(launch, 'releasemap.txt'), os.path.join(RUN, hdd, 'Z', 'releasemap.txt'))
    out = open(os.path.join(RUN, 'run.out'), 'w')
    if wine:
        game = subprocess.run(['winepath', '-w', GAME], env=env, capture_output=True, text=True).stdout.strip()
        cmd = ['unshare', '-n', '--map-current-user', 'wine', EXE, game]
    elif netns:
        cmd = ['nsenter', '--preserve-credentials', '-U', '-n', '-t', netns, EXE, GAME]
    else:
        cmd = ['unshare', '-rn', EXE, GAME]
    if server is not None:
        cmd = cmd[:-1] + ['--game', GAME] + server
    proc = subprocess.Popen(cmd, cwd=RUN, env=env, stdout=out, stderr=subprocess.STDOUT)
    threading.Thread(target=watch_processes, args=(proc.pid, os.path.join(RUN, 'procs.csv')), daemon=True).start()
    log, start, ok = Log(), time.time(), True
    verdict = 'FAIL'
    spawned = []  # spawn steps' helpers, ended with the scenario
    states = {}
    tracks = {}  # track steps: NAME -> rose, moved
    rosters = {}  # players steps: NAME -> {client: (x, y, z, health)}
    exposures = {}  # exposure steps: NAME -> dark, light, mean, low, high, middle
    fades = {}  # fade steps: NAME -> alpha, hold, length, lasted, real
    snap_lines = {}  # snap steps: NAME -> the log lines its frames wrote
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
        input_mark = 0  # the log's length at the last input step (wait newlog)
        run_base = RUN
        for st in steps:
            # @N STEP: the step's request files go to split-screen player N's own folder (RUN/splitN; @1 the run
            # folder): its state, look, place... (its log lines come back as "pN: ...")
            RUN = run_base
            if st[0].startswith('@'):
                if st[0] != '@1':
                    RUN = os.path.join(run_base, 'split' + st[0][1:])
                st = st[1:]
            kind = st[0]
            if kind in ('press', 'key', 'click', 'wheel', 'pckey', 'point', 'pointat', 'type', 'mark', 'pad'):
                input_mark = len(log.lines)
            if kind == 'mark':
                good = True
            elif kind == 'wait':
                if st[1] == 'log':
                    rx = ' '.join(st[2:])
                    good = wait_until(lambda: log.match(rx))
                elif st[1] == 'newlog':
                    # only lines written since the scenario's last input (press, key, click, pckey...): a state the
                    # log showed before, reached again
                    rx = re.compile(' '.join(st[2:]))
                    good = wait_until(lambda: any(rx.search(l) for l in log.lines[input_mark:]))
                else:
                    m = re.match(r'draws(>=|==|<=)(\d+)', st[1])
                    n, op = int(m.group(2)), m.group(1)
                    good = wait_until(lambda: log.per_frame >= n if op == '>=' else log.per_frame <= n if op == '<=' else round(log.per_frame) == n)
            elif kind == 'spawn':
                # spawn COMMAND...: a helper (tools/master's fake servers) in the game's network namespace, loopback
                # up there (a private namespace starts without it); its output in RUN/spawn_N.out
                target = netns or str(proc.pid)
                sh = 'ip link set lo up 2>/dev/null; exec ' + ' '.join(st[1:])
                spawned.append(subprocess.Popen(['nsenter', '--preserve-credentials', '-U', '-n', '-t', target, 'sh', '-c', sh],
                                                cwd=ROOT, stdout=open(os.path.join(RUN, f'spawn_{len(spawned)}.out'), 'w'),
                                                stderr=subprocess.STDOUT))
                good = True
            elif kind == 'sleep':
                # in steps of a quarter second: a game that ends during it is seen when it ends (procs.csv: its last state)
                end = time.time() + float(st[1])
                while time.time() < end and alive():
                    time.sleep(min(0.25, max(0.0, end - time.time())))
                good = alive()
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
            elif kind == 'pad':
                # pad N attach | detach | button NAME [SECONDS] | axis NAME VALUE: a test controller in the split-screen
                # compositor (src/linux/compositor.cpp), a virtual SDL gamepad routed to its player as a real one
                args = st[1:]
                if len(args) >= 3 and args[1] == 'button' and len(args) == 3:
                    args = args + ['0.15']
                send_file('pad', ' '.join(args))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'pad')))
            elif kind == 'winctl':
                # winctl fullscreen 1|0: window build, the window changed as a compositor would (settings untouched)
                send_file('winctl', ' '.join(st[1:]))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'winctl')))
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
            elif kind == 'pckey' and st[1] == 'tap':
                # pckey tap +CMD SECONDS: the game holds +CMD for SECONDS (src/game/pc_keys.cpp TapFrame: pressed and
                # released at the start of a Cbuf_Execute on the steady clock, right after the HUD's half-second
                # settings check, so the hold is the game's own and the title's buy menu reply does not meet the
                # check's commands; FINDINGS.md "Buy menu taps"); fails unless it logs a hold of SECONDS to + 0.1
                cmd, hold = st[2], float(st[3])
                rx = re.compile(r'pc keys: tap ' + re.escape(cmd) + r' held ([\d.]+) s')
                seen = sum(1 for l in log.lines if rx.search(l))
                send_file('pckey', 'tap %s %s' % (cmd, st[3]))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'pckey')))
                good = good and wait_until(lambda: sum(1 for l in log.lines if rx.search(l)) > seen)
                if good:
                    held = float([rx.search(l).group(1) for l in log.lines if rx.search(l)][seen])
                    print('     %s held %.3f s (asked %.3f)' % (cmd, held, hold))
                    good = hold <= held <= hold + 0.1
            elif kind == 'pckey':
                # pckey cmd TEXT... | pckey key KEYNUM DOWN: console text or an engine key event (src/game/pc_keys.cpp)
                send_file('pckey', ' '.join(st[1:]))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'pckey')))
            elif kind == 'windowshot':
                send_file('shot', st[1])
                good = wait_until(lambda: log.match('window: shot .*' + re.escape(st[1]) + r'\.bmp'))
            elif kind == 'windowburst':
                # windowburst NAME N: Vulkan window, the next N presented frames as RUN/shots/NAME_0.bmp...
                send_file('shot', 'burst:' + st[1] + ':' + st[2])
                last = st[1] + '_' + str(int(st[2]) - 1)
                good = wait_until(lambda: log.match('window: shot .*' + re.escape(last) + r'\.bmp'))
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
            elif kind == 'mousestream':
                done = sum(1 for l in log.lines if 'mouse stream done' in l)
                send_file('mouse', 'stream ' + ' '.join(st[1:5]))
                good = wait_until(lambda: sum(1 for l in log.lines if 'mouse stream done' in l) > done)
            elif kind == 'count':
                # count REGEX N: until N log lines match (the Nth round end, ...)
                rx = re.compile(st[1])
                n = int(st[2])
                good = wait_until(lambda: sum(1 for l in log.lines if rx.search(l)) >= n)
            elif kind == 'look':
                send_file('look', st[1] + ' ' + st[2])
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'look')))
            elif kind == 'setview':
                send_file('setview', ' '.join(st[1:6]))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'setview')))
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
            elif kind == 'place':
                # place N X Y Z: client N (0 the player, then the bots) put at that origin once (src/game/state_probe.cpp)
                send_file('place', ' '.join(st[1:5]))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'place'))) and \
                    wait_until(lambda: log.match(r'place: client ' + st[1] + ' at'))
            elif kind == 'players':
                # players NAME: every active client's origin and health, kept as NAME
                send_file('players', st[1])
                good = wait_until(lambda: log.match(r'players ' + re.escape(st[1]) + ' end'))
                if good:
                    rosters[st[1]] = {}
                    for l in log.lines:
                        m = re.search(r'players ' + re.escape(st[1]) + r' (\d+) (\S+) (\S+) (\S+) (\S+)\s*$', l)
                        if m:
                            rosters[st[1]][int(m.group(1))] = tuple(float(v) for v in m.groups()[1:])
                    print('     ' + '; '.join('%d: %.1f %.1f %.1f health %.0f' % ((n,) + v)
                                              for n, v in sorted(rosters[st[1]].items())))
            elif kind == 'bring':
                # bring N DIST: client N put DIST units in front of the host's player (server side)
                send_file('bring', st[1] + ' ' + st[2])
                good = wait_until(lambda: log.match(r'bring: (client|no such)'))
            elif kind == 'aim':
                # aim: the view turned to the nearest other player this client knows
                send_file('aim', '1')
                good = wait_until(lambda: log.match(r'aim: (player|no other)'))
            elif kind == 'spawns':
                # spawns NAME: the map's spawn points per team, the edicts in use and maxclients, logged
                send_file('spawns', st[1])
                good = wait_until(lambda: log.match(r'spawns ' + re.escape(st[1]) + ' ct'))
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
            elif kind == 'exposure':
                # exposure NAME: the first "dynamic gamma" line logged after this step began (the log read up to now
                # first: sleeps do not read it)
                log.poll()
                first = len(log.lines)
                num = r'(-?[0-9.]+)'
                rx = re.compile(r'dynamic gamma: measured dark %s light %s mean %s, ramp low %s high %s middle %s' % ((num,) * 6))
                def got():
                    for l in log.lines[first:]:
                        m = rx.search(l)
                        if m:
                            exposures[st[1]] = dict(zip(('dark', 'light', 'mean', 'low', 'high', 'middle'),
                                                        (float(v) for v in m.groups())))
                            return True
                    return False
                good = wait_until(got)
                if good:
                    print('     %s = %s' % (st[1], ' '.join('%s %.3f' % kv for kv in exposures[st[1]].items())))
            elif kind == 'fade':
                # fade NAME: the last "screen fade: started" line and the "ended" line after it
                num = r'(-?[0-9.]+)'
                begin = re.compile(r'screen fade: started at %s s, colour \w+ alpha (\d+), flags \w+, held until %s s, '
                                   r'ends at %s s' % (num, num, num))
                end = re.compile(r'screen fade: ended, lasted %s s \(%s s real\)' % (num, num))
                def got():
                    at = max((i for i, l in enumerate(log.lines) if begin.search(l)), default=None)
                    if at is None:
                        return False
                    e = next((end.search(l) for l in log.lines[at:] if end.search(l)), None)
                    if not e:
                        return False
                    b = begin.search(log.lines[at])
                    t0 = float(b.group(1))
                    f = {'alpha': float(b.group(2)), 'hold': float(b.group(3)) - t0, 'length': float(b.group(4)) - t0,
                         'lasted': float(e.group(1)), 'real': float(e.group(2))}
                    f['overrun'] = f['lasted'] - f['length']  # ran past the end it was sent with (client time)
                    f['drift'] = f['real'] - f['lasted']  # real time against client time
                    fades[st[1]] = f
                    return True
                good = wait_until(got)
                if good:
                    print('     %s = %s' % (st[1], ' '.join('%s %.3f' % kv for kv in fades[st[1]].items())))
            elif kind == 'expect' and st[1] in ('fade', 'fadediff'):
                # expect fade NAME FIELD MIN MAX, expect fadediff A B FIELD MIN MAX
                if st[1] == 'fade':
                    v, field, lo, hi = fades.get(st[2], {}).get(st[3]), st[3], st[4], st[5]
                else:
                    a, b = fades.get(st[2], {}).get(st[4]), fades.get(st[3], {}).get(st[4])
                    v, field, lo, hi = (b - a if a is not None and b is not None else None), st[4], st[5], st[6]
                good = v is not None and float(lo) <= v <= float(hi)
                if v is not None:
                    print(f'     {field} {v:+.3f} (want {lo}..{hi})')
            elif kind == 'expect' and st[1] == 'before':
                # expect before A B: this instance's first A line precedes its first B line (a relaunch starts a new log)
                log.poll()
                lines = log.lines
                for i in range(len(lines) - 1, -1, -1):
                    if lines[i].startswith('--- relaunch: '):
                        lines = lines[i + 1:]
                        break
                ra, rb = re.compile(st[2]), re.compile(st[3])
                ia = next((i for i, l in enumerate(lines) if ra.search(l)), None)
                ib = next((i for i, l in enumerate(lines) if rb.search(l)), None)
                good = ia is not None and ib is not None and ia < ib
                for name, i in (('A', ia), ('B', ib)):
                    print(f'     {name}: ' + (lines[i].rstrip() if i is not None else 'no line'))
            elif kind == 'expect' and st[1] == 'quiet':
                # expect quiet REGEX S: nothing matching for S seconds from now
                log.poll()
                first = len(log.lines)
                time.sleep(float(st[3]))
                log.poll()
                rx = re.compile(st[2])
                hits = [l for l in log.lines[first:] if rx.search(l)]
                good = not hits
                if hits:
                    print('     ' + hits[0].rstrip())
            elif kind == 'expect' and st[1] == 'exposuretrack':
                # expect exposuretrack NAME TOL: low follows the dark percentile and middle the mean
                e, tol = exposures.get(st[2]), float(st[3])
                good = e is not None and abs(e['low'] - e['dark']) <= tol and abs(e['middle'] - e['mean']) <= tol
                if e:
                    print(f"     low {e['low']:.3f} for dark {e['dark']:.3f}, middle {e['middle']:.3f} for mean {e['mean']:.3f}")
            elif kind == 'expect' and st[1] == 'exposurediff':
                # expect exposurediff A B FIELD MIN MAX
                a, b = exposures.get(st[2], {}).get(st[4]), exposures.get(st[3], {}).get(st[4])
                good = a is not None and b is not None and float(st[5]) <= b - a <= float(st[6])
                if a is not None and b is not None:
                    print(f'     {st[4]} {a:.3f} -> {b:.3f} ({b - a:+.3f}, want {st[5]}..{st[6]})')
            elif kind == 'expect' and st[1] == 'exposure':
                # expect exposure NAME FIELD MIN MAX
                v = exposures.get(st[2], {}).get(st[3])
                good = v is not None and float(st[4]) <= v <= float(st[5])
                if v is not None:
                    print(f'     {st[2]} {st[3]} {v:.3f} (want {st[4]}..{st[5]})')
            elif kind == 'expect' and st[1] == 'weapon':
                # expect weapon A ID: the weapon in hand at state A (17 Glock, 29 knife, 4 HE, 25 flashbang, 9 smoke)
                a = states.get(st[2])
                good = a is not None and len(a) > 7 and int(a[7]) == int(st[3])
                print(f'     weapon {int(a[7]) if a and len(a) > 7 else None}')
            elif kind == 'expect' and st[1] == 'sameweapon':
                # expect sameweapon A B 1|0: the weapon in hand at states A and B the same (1) or not (0)
                a, b = states.get(st[2]), states.get(st[3])
                good = a is not None and b is not None and len(a) > 7 and len(b) > 7 and \
                    (int(a[7]) == int(b[7])) == (st[4] == '1')
                print(f'     weapon {int(a[7]) if a and len(a) > 7 else None} -> {int(b[7]) if b and len(b) > 7 else None}')
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
                log.poll()
                first = len(log.lines)
                open(os.path.join(RUN, 'snap.tmp'), 'w').close()
                os.replace(os.path.join(RUN, 'snap.tmp'), os.path.join(RUN, 'snap'))
                good = wait_until(lambda: not os.path.exists(os.path.join(RUN, 'snap')))
                time.sleep(0.5)
                log.poll()
                snap_lines[st[1]] = log.lines[first:]  # what the snap's frames logged (PROBES lines)
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
            elif kind == 'expect' and st[1] == 'probealpha':
                # expect probealpha NAME X Y MIN MAX: the source alpha of the first SRCALPHA / INVSRCALPHA draw at
                # back buffer pixel X,Y in each of snap NAME's frames (the reference renderer's PROBES lines, the run
                # started with --env PROBES=X,Y) in [MIN, MAX]: a surface drawn with the world (water) comes before
                # the sprites and particles that may cross it
                rx = re.compile(r'probe %s,%s draw (\d+) src [-\d.]+ [-\d.]+ [-\d.]+ ([-\d.]+) -> .* blend 1 (\d+) (\d+)'
                                % (st[3], st[4]))
                found, last, first = [], -1, True
                for m in map(rx.search, snap_lines.get(st[2], [])):
                    if not m:
                        continue
                    draw = int(m.group(1))
                    if draw <= last:  # the draw count started again: the next frame
                        first = True
                    last = draw
                    if first and m.group(3, 4) == ('302', '303'):
                        found.append((draw, float(m.group(2))))
                        first = False
                print(f'     {st[2]} at {st[3]},{st[4]}: alpha ' + ', '.join(f'{a:.3f} (draw {d})' for d, a in found) +
                      f' (want {st[5]}..{st[6]})')
                good = bool(found) and all(float(st[5]) <= a <= float(st[6]) for _, a in found)
            elif kind == 'expect' and st[1] == 'chroma':
                # expect chroma NAME X0 Y0 X1 Y1 MIN MAX: mean chroma (max less min of RGB) of that box: how much of a
                # surface's colour shows through a blended layer over it (water: the more opaque, the greyer)
                from PIL import Image
                path = os.path.join(RUN, 'snaps', st[2] + '_2.tga')
                if not os.path.exists(path):
                    path = os.path.join(RUN, 'shots', st[2] + '.bmp')
                good = os.path.exists(path)
                if good:
                    box = tuple(int(v) for v in st[3:7])
                    px = list(Image.open(path).convert('RGB').crop(box).get_flattened_data())
                    chroma = sum(max(p) - min(p) for p in px) / max(1, len(px))
                    print(f'     {st[2]} {box}: mean chroma {chroma:.1f} (want {st[7]}..{st[8]})')
                    good = float(st[7]) <= chroma <= float(st[8])
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
            elif kind == 'expect' and st[1] == 'bluefrac':
                # expect bluefrac NAME X0 Y0 X1 Y1 MIN MAX: share of that box's pixels whose blue exceeds red by 12 or more
                from PIL import Image
                path = os.path.join(RUN, 'snaps', st[2] + '_2.tga')
                if not os.path.exists(path):
                    path = os.path.join(RUN, 'shots', st[2] + '.bmp')
                good = os.path.exists(path)
                if good:
                    box = tuple(int(v) for v in st[3:7])
                    px = list(Image.open(path).convert('RGB').crop(box).getdata())
                    frac = sum(1 for p in px if p[2] - p[0] >= 12) / max(1, len(px))
                    print(f'     {st[2]} {box}: {frac:.3f} of pixels blue over red (want {st[7]}..{st[8]})')
                    good = float(st[7]) <= frac <= float(st[8])
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
            elif kind == 'expect' and st[1] == 'regiondiff':
                # expect regiondiff A B X0 Y0 X1 Y1 MIN MAX: the box's mean luminance in B less that in A
                from PIL import Image, ImageStat
                box = tuple(int(v) for v in st[4:8])
                means = []
                for name in st[2:4]:
                    path = os.path.join(RUN, 'snaps', name + '_2.tga')
                    if not os.path.exists(path):
                        path = os.path.join(RUN, 'shots', name + '.bmp')
                    if os.path.exists(path):
                        means.append(ImageStat.Stat(Image.open(path).convert('L').crop(box)).mean[0])
                good = len(means) == 2 and float(st[8]) <= means[1] - means[0] <= float(st[9])
                if len(means) == 2:
                    print(f'     {box}: {means[0]:.1f} -> {means[1]:.1f} ({means[1] - means[0]:+.1f}, want {st[8]}..{st[9]})')
            elif kind == 'expect' and st[1] == 'seam':
                # expect seam NAME X Y MAX: the box from the shot's top left corner to X, Y is at most MAX brighter (mean
                # luminance, 6 pixels either side) across its right edge and across its bottom edge than outside
                from PIL import Image, ImageStat
                path = os.path.join(RUN, 'snaps', st[2] + '_2.tga')
                if not os.path.exists(path):
                    path = os.path.join(RUN, 'shots', st[2] + '.bmp')
                good = os.path.exists(path)
                if good:
                    im = Image.open(path).convert('L')
                    x, y = int(st[3]), int(st[4])
                    mean = lambda box: ImageStat.Stat(im.crop(box)).mean[0]
                    right = mean((x - 6, 0, x, y)) - mean((x, 0, x + 6, y))
                    bottom = mean((0, y - 6, x, y)) - mean((0, y, x, y + 6))
                    print(f'     {st[2]}: {right:+.1f} across x {x}, {bottom:+.1f} across y {y} (want at most {st[5]})')
                    good = right <= float(st[5]) and bottom <= float(st[5])
            elif kind == 'expect' and st[1] == 'coverage':
                # expect coverage PRE SHOT MIN SPREAD: the white fade's opacity in nine edge-touching patches
                from PIL import Image
                ims = []
                for name in st[2:4]:
                    path = os.path.join(RUN, 'snaps', name + '_2.tga')
                    if not os.path.exists(path):
                        path = os.path.join(RUN, 'shots', name + '.bmp')
                    if os.path.exists(path):
                        ims.append(Image.open(path).convert('L'))
                good = len(ims) == 2 and ims[0].size == ims[1].size
                if good:
                    w, h = ims[1].size
                    n = max(4, h // 50)
                    xs, ys = {'left': 0, 'centre': (w - n) // 2, 'right': w - n}, {'top': 0, 'middle': (h - n) // 2, 'bottom': h - n}
                    print(f'     {w}x{h}, patches {n}x{n}: {st[2]} -> {st[3]}, opacity (want {st[4]} or more, spread {st[5]})')
                    alphas = []
                    for yn, y in ys.items():
                        row = []
                        for xn, x in xs.items():
                            pre, shot = [sorted(im.crop((x, y, x + n, y + n)).getdata())[n * n // 2] for im in ims]
                            if 255 - pre < 60:  # already near white (a HUD element): it only has to stay as bright
                                patch_ok = shot >= pre - 5
                                row.append(f'{xn} {pre:3d} -> {shot:3d} bright')
                            else:
                                a = (shot - pre) / (255 - pre)
                                alphas.append(a)
                                patch_ok = a >= float(st[4])
                                row.append(f'{xn} {pre:3d} -> {shot:3d} {a:.2f}')
                            if not patch_ok:
                                row[-1] += ' FAIL'
                            good = good and patch_ok
                        print(f'       {yn:6s}  ' + ', '.join(row))
                    spread = max(alphas) - min(alphas) if alphas else 0.0
                    print(f'       opacity {min(alphas, default=0):.2f}..{max(alphas, default=0):.2f}, spread {spread:.2f}')
                    good = good and len(alphas) >= 5 and spread <= float(st[5])
            elif kind == 'expect' and st[1] == 'steady':
                # expect steady NAME N: the burst NAME_0..N-1 never flickers: no frame that differs from the one before
                # it but matches the one before that (two pictures shown in turn)
                from PIL import Image
                import numpy as np
                frames = [np.asarray(Image.open(os.path.join(RUN, 'shots', f'{st[2]}_{i}.bmp')).convert('L')
                                     .resize((160, 120)), dtype=float) for i in range(int(st[3]))]
                flips = [i for i in range(2, len(frames))
                         if np.abs(frames[i] - frames[i - 1]).mean() > 3 and np.abs(frames[i] - frames[i - 2]).mean() < 1]
                print(f'     {len(flips)} frames back to the picture before last' + (f' (first {flips[0]})' if flips else ''))
                good = not flips
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
            elif kind == 'expect' and st[1] == 'mousespread':
                # expect mousespread MAX: "mouse look" lines where every pass moved (the stream's whole seconds)
                time.sleep(1.2)
                log.poll()
                spreads = []
                for l in log.lines[-400:]:
                    m = re.search(r'mouse look: .*moved in (\d+) of (\d+) passes, .*spread ([0-9.]+)', l)
                    if m and m.group(1) == m.group(2) and int(m.group(2)) > 0:
                        spreads.append(float(m.group(3)))
                spreads = spreads[-2:]
                print('     spread %s' % ', '.join('%.2f' % v for v in spreads) if spreads else '     no whole second')
                good = bool(spreads) and all(v <= float(st[2]) for v in spreads)
            elif kind == 'expect' and st[1] == 'setting':
                # expect setting NAME VALUE: the saved pc_settings.ini line
                saved = {}
                if os.path.exists(settings):
                    for l in open(settings, encoding='utf-8'):
                        if '=' in l and not l.lstrip().startswith('#'):
                            k, v = l.split('=', 1)
                            saved[k.strip()] = v.strip()
                print('     %s = %s' % (st[2], saved.get(st[2], '(none)')))
                good = saved.get(st[2]) == ' '.join(st[3:])
            elif kind == 'expect' and st[1] in ('apart', 'bodymoved', 'bodyz', 'health', 'healthdrop', 'spawnapart',
                                                'playercount'):
                # expect apart NAME N M MIN MAX: clients N and M horizontally that far apart in players step NAME;
                # expect bodymoved A B N MIN MAX: client N moved that far (horizontally) from players A to B;
                # expect healthdrop A B N MIN MAX: client N's health fell that much from A to B;
                # expect health NAME N MIN MAX: client N's health; expect bodyz NAME N MIN MAX: its origin's height;
                # expect spawnapart NAME MIN: every two clients at
                # least MIN apart (no one spawned in another)
                def flat(a, b):
                    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5
                try:
                    if st[1] == 'apart':
                        r = rosters[st[2]]; v = flat(r[int(st[3])], r[int(st[4])]); lo, hi = st[5], st[6]
                    elif st[1] == 'bodymoved':
                        v = flat(rosters[st[2]][int(st[4])], rosters[st[3]][int(st[4])]); lo, hi = st[5], st[6]
                    elif st[1] == 'playercount':
                        v = len(rosters[st[2]]); lo, hi = st[3], st[4]
                    elif st[1] == 'healthdrop':
                        v = rosters[st[2]][int(st[4])][3] - rosters[st[3]][int(st[4])][3]; lo, hi = st[5], st[6]
                    elif st[1] in ('health', 'bodyz'):
                        v = rosters[st[2]][int(st[3])][3 if st[1] == 'health' else 2]; lo, hi = st[4], st[5]
                    else:
                        r = list(rosters[st[2]].values())
                        v = min((flat(a, b) for i, a in enumerate(r) for b in r[i + 1:]), default=float('inf'))
                        lo, hi = st[3], 'inf'
                    print('     %.1f (want %s..%s)' % (v, lo, hi))
                    good = float(lo) <= v <= float(hi)
                except KeyError as e:
                    print('     no such step or client: %s' % e)
                    good = False
            elif kind == 'expect' and st[1] == 'teams':
                # expect teams MAXDIFF [MIN]: the players' teams as the game's log last has them (game: "NAME<..>"
                # joined team "CT" / "TERRORIST"), the two within MAXDIFF of each other, each at least MIN
                log.poll()
                last = {}
                for l in log.lines:
                    m = re.search(r'game: "(.*?)<\d+><[^>]*><[^>]*>" joined team "(\w+)"', l)
                    if m:
                        last[m.group(1)] = m.group(2)
                ct = sum(v == 'CT' for v in last.values())
                tt = sum(v == 'TERRORIST' for v in last.values())
                print('     CT %d, T %d' % (ct, tt))
                good = abs(ct - tt) <= int(st[2]) and min(ct, tt) >= (int(st[3]) if len(st) > 3 else 0)
            elif kind == 'expect' and st[1] == 'bodiesmoved':
                # expect bodiesmoved A B MIN COUNT: at least COUNT clients moved MIN or more (horizontally) from
                # players step A to B (the bots walk)
                a, b = rosters.get(st[2], {}), rosters.get(st[3], {})
                moved = sorted(n for n in a if n in b and
                               ((a[n][0] - b[n][0]) ** 2 + (a[n][1] - b[n][1]) ** 2) ** 0.5 >= float(st[4]))
                print('     %d moved %s or more: %s' % (len(moved), st[4], ' '.join(map(str, moved))))
                good = len(moved) >= int(st[5])
            elif kind == 'expect' and st[1] == 'effects':
                # expect effects BLOOM GAMMA: within 5 s the last "pc effects" line (CSX_LOG_EFFECTS=1, src/game/
                # pc_bloom.cpp: the cvars' values each time one changes) reads cl_hdrbloom BLOOM, cl_dynamicgamma GAMMA
                def effects():
                    log.poll()
                    last = [m for m in (re.search(r'pc effects: cl_hdrbloom (\S+), cl_dynamicgamma (\S+)$', l)
                                        for l in log.lines) if m][-1:]
                    return last[0].groups() if last else None
                deadline = time.time() + 5
                while effects() != (st[2], st[3]) and time.time() < deadline:
                    time.sleep(0.2)
                now = effects()
                print('     cl_hdrbloom %s, cl_dynamicgamma %s' % now if now else '     no effects line')
                good = now == (st[2], st[3])
            elif kind == 'expect' and st[1] == 'movetime':
                # expect movetime MIN MAX: milliseconds of movement a second over the last two "pc frame" lines
                time.sleep(2.2)
                log.poll()
                vals = [float(m.group(1)) for m in
                        (re.search(r'pc frame: [0-9.]+ movement commands/s, ([0-9.]+) ms of movement', l)
                         for l in log.lines) if m][-2:]
                print('     movement %s ms a second' % ', '.join('%.0f' % v for v in vals) if vals else '     no frame line')
                good = len(vals) == 2 and all(float(st[2]) <= v <= float(st[3]) for v in vals)
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
                # why: the game ended (a crash shows as a signal), or the scenario's time ran out
                if not alive():
                    verdict = 'CRASH' if proc.returncode is not None and proc.returncode < 0 and \
                        proc.returncode not in (-2,) else 'FAIL'
                    print(f'     the game exited during this step: {exit_text(proc.returncode)}')
                elif time.time() - start >= timeout:
                    verdict = 'TIMEOUT'
                    print(f'     TIMEOUT: the scenario\'s {timeout:.0f} s ran out during this step')
                break
        RUN = run_base
    finally:
        for p in spawned:
            if p.poll() is None:
                p.terminate()
                try:
                    p.wait(5)
                except subprocess.TimeoutExpired:
                    p.kill()
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
        print(f'FAIL harness exited with {exit_text(proc.returncode)}')
        ok = False
        verdict = 'CRASH' if proc.returncode < 0 else 'FAIL'
    # one line a runner reads (tools/run_net_tests.py): PASS, FAIL (a step's check), TIMEOUT or CRASH
    print(f'RESULT {"PASS" if ok else verdict}')
    sys.stdout.flush()
    return 0 if ok else 1


def watch_processes(root, path):
    """procs.csv in the run folder: every half second the game's processes (it and its descendants: the compositor's
    players, a local server) with their state, resident memory, CPU time and threads, and the machine's available
    memory; then when the game's own process ended (a zombie, before drive reaps it) and how. What a process killed
    from outside (SIGKILL: no log line of its own) was doing, and whether memory ran short."""
    tick = os.sysconf('SC_CLK_TCK')
    t0 = time.time()

    def stat(pid):
        try:
            with open(f'/proc/{pid}/stat') as f:
                text = f.read()
        except OSError:
            return None
        comm = text[text.index('(') + 1:text.rindex(')')]
        rest = text[text.rindex(')') + 2:].split()
        return comm, rest  # rest[0] state, [1] ppid, [11] utime, [12] stime, [17] threads, [21] rss pages

    with open(path, 'w') as out:
        out.write('seconds,pid,ppid,name,state,rss_mb,cpu_s,threads,mem_available_mb\n')
        while True:
            try:
                with open('/proc/meminfo') as f:
                    avail = next(int(l.split()[1]) // 1024 for l in f if l.startswith('MemAvailable'))
            except (OSError, StopIteration):
                avail = -1
            procs = {}
            for d in os.listdir('/proc'):
                if d.isdigit():
                    st = stat(d)
                    if st:
                        procs[int(d)] = st
            tree, todo = [], [root]
            while todo:
                pid = todo.pop()
                if pid in procs:
                    tree.append(pid)
                    todo += [c for c, (_, r) in procs.items() if int(r[1]) == pid]
            now = time.time() - t0
            for pid in tree:
                comm, r = procs[pid]
                out.write('%.1f,%d,%s,%s,%s,%.0f,%.1f,%s,%d\n' % (now, pid, r[1], comm.replace(',', ' '), r[0],
                          int(r[21]) * os.sysconf('SC_PAGE_SIZE') / 1048576, (int(r[11]) + int(r[12])) / tick, r[17], avail))
            out.flush()
            if root not in procs or procs[root][1][0] in 'ZX':
                out.write('%.1f,%d,,,ended,,,,%d\n' % (now, root, avail))
                return
            time.sleep(0.5)


def exit_text(code):
    if code is None:
        return 'still running'
    if code < 0:
        try:
            return f'signal {signal.Signals(-code).name}'
        except ValueError:
            return f'signal {-code}'
    return f'exit code {code}'


def stop_on_signal(sig, frame):
    # SIGTERM / SIGHUP (a runner or session that stops) end the scenario through main's finally, which ends the game:
    # left running, a game kept polling its run folder and took the next run's step files (net_two_humans, 2026-10-25)
    raise KeyboardInterrupt


if __name__ == '__main__':
    for s in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, stop_on_signal)
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print('RESULT INTERRUPTED')
        sys.exit(130)
