#!/usr/bin/env python3
"""tools/csx_server_gui.py (N9) without the game: its keys are csx-server's (src/server/server_config.cpp), and with
a stand-in for csx-server (this file run with --fake: it writes a status file and a log, answers "admin status" as
the server's console does and quits on "quit") it saves a preset (owner-only, the password kept out of the state),
starts the server, sends console and administration commands, reads the players, refuses requests without the token
and stops the server.

  python3 -I tests/server_gui_test.py
"""
import importlib.util
import json
import os
import re
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def fake_server():
    """csx-server's console as the GUI sees it"""
    os.makedirs('logs', exist_ok=True)
    log = open('logs/server.log', 'a', buffering=1)
    with open(os.environ['CSX_SERVER_STATUS'], 'w') as f:
        f.write('ready 27015 abcd\n')
    log.write('[0.0] [info] server: ready\n')
    for line in sys.stdin:
        line = line.strip()
        log.write('[1.0] [info] server console: %s\n' % line)
        if line == 'admin status':
            log.write('[1.0] [info] admin console: #1 "Ace" 192.168.1.5:27005 0123abcd\n')
            log.write('[1.0] [info] admin console: #2 "Bot" BOT\n')
            log.write('[1.0] [info] admin console: 2 of 65 slots in use\n')
        if line == 'quit':
            with open(os.environ['CSX_SERVER_STATUS'], 'w') as f:
                f.write('ended\n')
            return


def main():
    if '--fake' in sys.argv:
        return fake_server()
    failures = []

    def check(ok, what):
        print(('ok   ' if ok else 'FAIL ') + what)
        if not ok:
            failures.append(what)

    spec = importlib.util.spec_from_file_location('gui', os.path.join(ROOT, 'tools', 'csx_server_gui.py'))
    gui = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gui)
    src = open(os.path.join(ROOT, 'src', 'server', 'server_config.cpp')).read()
    block = src[src.index('const std::vector<Key>& Keys()'):]
    names = set(re.findall(r'\{"([a-z_]+)",', block[:block.index('return k;')]))
    check(names == gui.KEY_NAMES, 'the GUI\'s keys are csx-server\'s (%s)' % sorted(names ^ gui.KEY_NAMES))

    with tempfile.TemporaryDirectory() as home:
        exe = os.path.join(home, 'fake-server')
        with open(exe, 'w') as f:
            f.write('#!/bin/sh\nexec %s -I %s --fake\n' % (sys.executable, os.path.abspath(__file__)))
        os.chmod(exe, 0o700)
        url_file = os.path.join(home, 'url')
        p = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'tools', 'csx_server_gui.py'), '--exe', exe,
                              '--game', home, '--home', home, '--port', '0', '--print-token-file', url_file],
                             stdout=subprocess.DEVNULL)
        try:
            for _ in range(100):
                if os.path.exists(url_file) and open(url_file).read().strip():
                    break
                time.sleep(0.05)
            url = open(url_file).read().strip()
            base, token = url.split('/?token=')

            def post(path, body, tok=token):
                req = urllib.request.Request(base + path + '?token=' + tok, data=json.dumps(body).encode(),
                                             method='POST')
                try:
                    return json.loads(urllib.request.urlopen(req).read())
                except urllib.error.HTTPError as e:
                    if e.code == 400:
                        return json.loads(e.read())
                    return {'status': e.code}

            def state():
                return json.loads(urllib.request.urlopen(base + '/api/state?token=' + token).read())

            check(base.startswith('http://127.0.0.1:'), 'listens on 127.0.0.1 only')
            check(post('/api/start', {'name': 'x'}, 'wrong') == {'status': 403}, 'a wrong token is refused')
            try:
                urllib.request.urlopen(base + '/api/state')
                check(False, 'no token is refused')
            except urllib.error.HTTPError as e:
                check(e.code == 403, 'no token is refused')
            r = post('/api/save', {'name': 'lan1', 'settings': {'name': 'LAN party', 'map': 'de_dust2',
                                                               'password': 'hunter22', 'game_mode': 'deathmatch',
                                                               'bogus': 'x'}})
            check(r.get('result') == 'saved', 'a preset saved')
            cfg = os.path.join(home, 'presets', 'lan1.cfg')
            check(stat.S_IMODE(os.stat(cfg).st_mode) == 0o600, 'the preset file is owner-only')
            text = open(cfg).read()
            check('game_mode deathmatch' in text and 'bogus' not in text, 'only csx-server keys are written')
            s = state()['presets'][0]
            check('password' not in s['settings'] and s['password_set'], 'the password stays out of the state')
            check('error' in post('/api/save', {'name': '../evil', 'settings': {}}), 'a path as a name is refused')
            check('error' in post('/api/save', {'name': 'b', 'settings': {'name': 'a"b'}}), 'quotes are refused')
            r = post('/api/save', {'name': 'lan1', 'settings': {'name': 'LAN party 2', 'password': ''}})
            check('password hunter22' in open(cfg).read(), 'an empty password field keeps the password')
            check(post('/api/start', {'name': 'lan1'}).get('result', '').startswith('started'), 'started')
            for _ in range(50):
                if state()['presets'][0]['status'].startswith('ready'):
                    break
                time.sleep(0.1)
            check(state()['presets'][0]['status'] == 'ready 27015 abcd', 'its status read')
            check(post('/api/admin', {'name': 'lan1', 'command': 'status'}).get('result') == 'sent', 'admin status sent')
            check('error' in post('/api/admin', {'name': 'lan1', 'command': 'quit'}), 'a non-admin command refused')
            check('";"' in post('/api/console', {'name': 'lan1', 'line': 'say a; quit'}).get('result', ''),
                  'a ";" refused')
            for _ in range(50):
                if state()['presets'][0]['players']:
                    break
                time.sleep(0.1)
            players = state()['presets'][0]['players']
            check(players == [[1, 'Ace', '192.168.1.5:27005 0123abcd'], [2, 'Bot', 'BOT']], 'players read: %s' % players)
            post('/api/console', {'name': 'lan1', 'line': 'sv_password secret9'})
            time.sleep(0.3)
            check(not any('secret9' in l for l in state()['presets'][0]['log']), 'a password not shown in the log')
            check(post('/api/stop', {'name': 'lan1'}).get('result') == 'stopped', 'stopped by quit')
            check(state()['presets'][0]['status'] == 'stopped', 'status stopped')
            check(post('/api/delete', {'name': 'lan1'}).get('result') == 'deleted', 'preset deleted')
        finally:
            p.terminate()
            p.wait(5)
    print('%d failures' % len(failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
