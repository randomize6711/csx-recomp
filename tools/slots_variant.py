#!/usr/bin/env python3
"""An experimental build with more player slots (N11, docs/n11_slots.md): 96 (128 is not supported yet: the voice
masks' layout would change, see the doc). The stable 65-slot build is never touched: the variant has its own
configuration (build/slotsN/config: config/ copied, the slot count, the protocol version and the per-player guest
tables' sizes changed, its operand patches generated), its own generated code (build/genN/game) and its own binaries
(build/harnessN/CounterStrike, build/serverN/csx-server). Its protocol version differs from the 65-slot build's
(47), so the two refuse each other at connect.

  python3 -I tools/slots_variant.py 96 [--configure-only]

Builds take a while; run them only when no test is running.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROTOCOL = {96: 48}


def configure(n):
    out = os.path.join(ROOT, 'build', 'slots%d' % n)
    cfg = os.path.join(out, 'config')
    if os.path.isdir(cfg):
        shutil.rmtree(cfg)
    os.makedirs(cfg)
    for name in os.listdir(os.path.join(ROOT, 'config')):
        if name.endswith('.toml'):
            shutil.copyfile(os.path.join(ROOT, 'config', name), os.path.join(cfg, name))
    xdk = os.path.join(out, 'xdk')
    if not os.path.islink(xdk):
        os.symlink(os.path.join(ROOT, 'xdk'), xdk)
    # the slot count and the protocol version
    p = os.path.join(cfg, 'player_slots.toml')
    s = open(p).read()
    s, k1 = re.subn(r'(?m)^max_players = 65$', 'max_players = %d' % n, s)
    s, k2 = re.subn(r'(?m)^protocol_version = 47$', 'protocol_version = %d' % PROTOCOL[n], s)
    if k1 != 1 or k2 != 1:
        sys.exit('player_slots.toml is not laid out as expected')
    open(p, 'w').write(s)
    # the per-player guest tables: N, N + 1, N + 2 elements; one frame's player states N x 0xFB; the command lines
    p = os.path.join(cfg, 'guest_tables.toml')
    s = open(p).read()
    counts = {65: n, 66: n + 1, 67: n + 2}
    s, k = re.subn(r'(?m)^count = (65|66|67)$', lambda m: 'count = %d' % counts[int(m.group(1))], s)
    s, f = re.subn(r'(?m)^elem = 0x3FBB(\s)', lambda m: 'elem = 0x%X%s' % (n * 0xFB, m.group(1)), s)
    s, c = re.subn(r'-maxplayers 65 \+maxplayers 65', '-maxplayers %d +maxplayers %d' % (n, n), s)
    if k < 10 or f != 1 or c != 2:
        sys.exit('guest_tables.toml is not laid out as expected (%d counts, %d frame, %d command lines)' % (k, f, c))
    if (n + 31) // 32 != 3:
        sys.exit('%d slots need %d dwords per voice mask: the masks\' layout (3 dwords) would change' % (n, (n + 31) // 32))
    open(p, 'w').write(s)
    # the guest tables' layout the C++ side maps (src/common/guest_tables_layout.h, generated from the layout): the
    # variant's own, found before src/ when it compiles
    inc = os.path.join(out, 'include', 'common')
    os.makedirs(inc, exist_ok=True)
    r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'guest_tables.py'), '--config',
                        os.path.join(cfg, 'guest_tables.toml'), '--header', os.path.join(inc, 'guest_tables_layout.h')])
    if r.returncode:
        sys.exit('the guest tables\' layout was not generated')
    r = subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'slot_patches.py'), '--slots',
                        os.path.join(cfg, 'player_slots.toml'), '--layout', os.path.join(cfg, 'guest_tables.toml'),
                        '--out', os.path.join(cfg, 'player_slots_patches.toml')])
    if r.returncode:
        sys.exit('the operand patches were not generated')
    return cfg


def build(n, cfg):
    gen = os.path.join(ROOT, 'build', 'gen%d' % n, 'game')  # named game: build.sh compiles src/<its name>
    os.makedirs(gen, exist_ok=True)
    env = dict(os.environ)
    run = lambda cmd, log: subprocess.run(cmd, cwd=ROOT, env=env, stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    if run(['tools/apply_sdk_patches.sh'], os.devnull).returncode:
        sys.exit('the SDK patches did not apply')
    r = run([os.path.join(ROOT, 'out', 'build', 'linux-tools', 'xbe_recomp'), os.path.join(cfg, 'game.toml'),
             os.path.join(ROOT, 'assets', 'default.xbe'), gen], os.path.join(ROOT, 'build', 'recomp%d.log' % n))
    if r.returncode:
        sys.exit('recomp failed: build/recomp%d.log' % n)
    inc = os.path.join(ROOT, 'build', 'slots%d' % n, 'include')
    cxx = ('clang++ -include unistd.h -I%s -DCSX_MAX_PLAYERS=%d -Wno-unused-command-line-argument -lsodium' % (inc, n))
    env.update(GEN_OPT='-O2', CXX=cxx)
    r = run(['bash', 'sdk/harness/build.sh', '-r', '.', '-g', gen, '-o', 'build/harness%d' % n, '-n', 'CounterStrike',
             '-x', '/src/linux/', '-x', '/src/vulkan/', '-x', '/src/server/'], os.path.join(ROOT, 'build', 'harness%d_build.log' % n))
    if r.returncode:
        sys.exit('harness build failed: build/harness%d_build.log' % n)
    env.update(HOST_OPT='-O2')
    r = run(['bash', 'sdk/harness/build.sh', '-r', '.', '-g', gen, '-o', 'build/server%d' % n, '-n', 'csx-server',
             '-G', 'build/harness%d/gen' % n, '-x', '/harness/main_test.cpp', '-x', '/harness/null_backend.cpp',
             '-x', '/harness/pad_script.cpp', '-x', '/harness/reference_renderer.cpp', '-x', '/src/linux/',
             '-x', '/src/vulkan/'], os.path.join(ROOT, 'build', 'server%d_build.log' % n))
    if r.returncode:
        sys.exit('server build failed: build/server%d_build.log' % n)
    print('built build/harness%d/CounterStrike and build/server%d/csx-server' % (n, n))


def main():
    if len(sys.argv) < 2 or not sys.argv[1].isdigit() or int(sys.argv[1]) not in PROTOCOL:
        sys.exit(__doc__)
    n = int(sys.argv[1])
    cfg = configure(n)
    print('configured %s' % cfg)
    if '--configure-only' not in sys.argv:
        build(n, cfg)


if __name__ == '__main__':
    main()
