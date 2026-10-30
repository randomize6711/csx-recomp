#!/usr/bin/env python3
"""The dedicated server's administration window (N9, docs/n9_server_gui.md): an optional graphical front end for
csx-server, in the web browser of this PC. The headless server runs as before without it.

  python3 -I tools/csx_server_gui.py [--exe build/server/csx-server] [--game build/gamedata] [--home build/servers]
                                     [--port 27080]

It listens on 127.0.0.1 only and prints its address with a secret token (each start a new one). Every request must
carry the token, so another user of the PC or a web page cannot use it. From there:
  - presets: a server's settings (csx-server's keys: name, port, map, rotation, bots, mode, rules, password,
    LAN only or listed on master servers...) saved as HOME/presets/NAME.cfg (owner-only: a password may be in it);
  - start and stop a preset's server (in HOME/run/NAME, its state folder: logs, identity key, bans);
  - its status (starting, ready on PORT with its identity key, ended, error), its log's last lines;
  - the console (engine commands) and the administration commands as the server's owner ("admin ..." on its
    console, src/game/server_admin.cpp): status (the player list), say, kick, ban, unban, map, restart, bots,
    password.
Nothing here weakens the server's own checks: remote players still administer only with an admins.cfg key, and the
GUI reaches only servers it started itself, through their console.
"""
import argparse
import hmac
import html
import json
import os
import re
import secrets
import signal
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# csx-server's keys (src/server/server_config.cpp Keys; tests/server_gui_test.py checks the two lists agree)
KEYS = [
    ('name', 'text', 'Server name'), ('port', 'number', 'UDP port'), ('map', 'text', 'First map'),
    ('map_rotation', 'text', 'Map rotation (comma separated)'), ('max_players', 'number', 'Player slots (1-65)'),
    ('bots', 'number', 'Bots'), ('bot_difficulty', 'number', 'Bot difficulty (0-3)'),
    ('game_mode', 'choice:classic,deathmatch,gungame', 'Game mode'), ('frag_limit', 'number', 'Frag limit (Deathmatch)'),
    ('gungame_order', 'text', 'Gun Game ladder (comma separated)'), ('gravity', 'number', 'Gravity (800 normal)'),
    ('flashlight', 'choice:on,off', 'Flashlights'), ('player_collision', 'choice:on,off', 'Player collision'),
    ('friendly_fire', 'choice:off,on', 'Friendly fire'), ('round_time', 'number', 'Round time (minutes)'),
    ('freeze_time', 'number', 'Freeze time (seconds)'), ('buy_time', 'number', 'Buy time (minutes)'),
    ('start_money', 'number', 'Start money'), ('max_rounds', 'number', 'Max rounds (0 none)'),
    ('time_limit', 'number', 'Time limit (minutes, 0 none)'), ('password', 'password', 'Password (empty: none)'),
    ('lan_only', 'choice:off,on', 'LAN only'), ('snapshot_rate', 'number', 'Snapshots a second'),
    ('master', 'text', 'Master servers (internet listing; empty: none)'),
]
KEY_NAMES = {k for k, _, _ in KEYS}
NAME_RE = re.compile(r'^[A-Za-z0-9_-]{1,32}$')


class Server:
    """a preset's running csx-server (its console on a pipe)"""

    def __init__(self, gui, name):
        self.gui, self.name = gui, name
        self.proc = None
        self.lock = threading.Lock()

    @property
    def folder(self):
        return os.path.join(self.gui.home, 'run', self.name)

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def start(self):
        with self.lock:
            if self.running():
                return 'already running'
            os.makedirs(self.folder, exist_ok=True)
            status = os.path.join(self.folder, 'status')
            if os.path.exists(status):
                os.remove(status)
            env = dict(os.environ, CSX_SERVER_STATUS=status)
            cfg = self.gui.preset_path(self.name)
            self.proc = subprocess.Popen(
                [self.gui.exe, '--game', self.gui.game, '--config', cfg], cwd=self.folder, env=env,
                stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            return 'started (process %d)' % self.proc.pid

    def send(self, line):
        with self.lock:
            if not self.running():
                return 'not running'
            line = line.replace('\r', ' ').replace('\n', ' ').strip()
            if not line or len(line) > 300 or ';' in line:
                return 'one command a line, no ";"'
            try:
                self.proc.stdin.write((line + '\n').encode())
                self.proc.stdin.flush()
            except OSError as e:
                return 'not sent: %s' % e
            return 'sent'

    def stop(self):
        if not self.running():
            return 'not running'
        self.send('quit')
        for _ in range(50):
            if not self.running():
                return 'stopped'
            time.sleep(0.1)
        os.killpg(self.proc.pid, signal.SIGTERM)
        for _ in range(50):
            if not self.running():
                return 'stopped (terminated)'
            time.sleep(0.1)
        os.killpg(self.proc.pid, signal.SIGKILL)
        return 'killed'

    def status(self):
        try:
            text = open(os.path.join(self.folder, 'status')).read().strip()
        except OSError:
            text = ''
        if self.running():
            return text or 'starting'
        return 'stopped' + (' (%s)' % text if text and text != 'ended' else '')

    def log(self, lines=60):
        logs = os.path.join(self.folder, 'logs')
        try:
            files = sorted((os.path.join(logs, f) for f in os.listdir(logs) if f.endswith('.log')),
                           key=os.path.getmtime)
        except OSError:
            return []
        if not files:
            return []
        with open(files[-1], errors='replace') as f:
            out = f.readlines()[-lines:]
        # a password typed in the console is not shown back
        return [re.sub(r'(sv_password|password)\s+\S+', r'\1 ...', l.rstrip('\n')) for l in out]

    def players(self):
        """the last "admin status" reply in the log: [(slot, name, where)]"""
        out, block = [], []
        for l in self.log(400):
            m = re.search(r'admin console: #(\d+) "(.*)" (.*)$', l)
            if m:
                block.append((int(m.group(1)), m.group(2), m.group(3)))
            elif 'admin console:' in l and 'slots in use' in l:
                out, block = block, []
        return out


class Gui:
    def __init__(self, a):
        self.exe, self.game, self.home = os.path.abspath(a.exe), os.path.abspath(a.game), os.path.abspath(a.home)
        self.token = secrets.token_urlsafe(24)
        self.servers = {}
        os.makedirs(os.path.join(self.home, 'presets'), exist_ok=True)

    def preset_path(self, name):
        return os.path.join(self.home, 'presets', name + '.cfg')

    def presets(self):
        d = os.path.join(self.home, 'presets')
        return sorted(f[:-4] for f in os.listdir(d) if f.endswith('.cfg') and NAME_RE.match(f[:-4]))

    def read_preset(self, name):
        values = {}
        try:
            for l in open(self.preset_path(name)):
                l = l.strip()
                if l and not l.startswith('#') and ' ' in l:
                    k, v = l.split(' ', 1)
                    if k in KEY_NAMES:
                        values[k] = v.strip()
        except OSError:
            pass
        return values

    def write_preset(self, name, values):
        lines = ['# csx-server settings (tools/csx_server_gui.py preset %s)' % name]
        for k, _, _ in KEYS:
            v = str(values.get(k, '')).strip()
            if not v:
                continue
            if any(c in v for c in '\n\r";\\'):
                raise ValueError('%s: no quotes, ";", "\\" or line breaks' % k)
            lines.append('%s %s' % (k, v))
        path = self.preset_path(name)
        fd = os.open(path + '.new', os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        os.replace(path + '.new', path)

    def server(self, name):
        if name not in self.servers:
            self.servers[name] = Server(self, name)
        return self.servers[name]

    def state(self):
        out = []
        for p in self.presets():
            s = self.server(p)
            values = self.read_preset(p)
            values.pop('password', None)
            out.append({'name': p, 'running': s.running(), 'status': s.status(), 'settings': values,
                        'password_set': bool(self.read_preset(p).get('password')), 'players': s.players(),
                        'log': s.log(40)})
        return {'presets': out, 'keys': KEYS}


ADMIN = {'status', 'say', 'kick', 'ban', 'unban', 'map', 'restart', 'bots', 'bot_difficulty', 'password',
         'friendly_fire', 'round_time'}


def handler(gui):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):  # requests are not logged (a token is in them)
            pass

        def authorised(self, q):
            given = (q.get('token') or [''])[0] or self.headers.get('X-Token', '')
            return hmac.compare_digest(given.encode(), gui.token.encode())

        def reply(self, code, body, kind='application/json'):
            data = body.encode() if isinstance(body, str) else body
            self.send_response(code)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('Content-Security-Policy', "default-src 'self' 'unsafe-inline'")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            u = urlparse(self.path)
            q = parse_qs(u.query)
            if not self.authorised(q):
                return self.reply(403, 'the token is missing or wrong', 'text/plain')
            if u.path == '/':
                return self.reply(200, PAGE.replace('@TOKEN@', html.escape(gui.token)), 'text/html; charset=utf-8')
            if u.path == '/api/state':
                return self.reply(200, json.dumps(gui.state()))
            return self.reply(404, '{}')

        def do_POST(self):
            u = urlparse(self.path)
            if not self.authorised(parse_qs(u.query)):
                return self.reply(403, '{"error": "the token is missing or wrong"}')
            n = int(self.headers.get('Content-Length', '0'))
            if n > 65536:
                return self.reply(413, '{}')
            try:
                body = json.loads(self.rfile.read(n) or b'{}')
                name = str(body.get('name', ''))
                if not NAME_RE.match(name):
                    raise ValueError('a preset name is letters, digits, _ and - (32 at most)')
                result = self.act(u.path, name, body)
            except (ValueError, KeyError, OSError) as e:
                return self.reply(400, json.dumps({'error': str(e)}))
            return self.reply(200, json.dumps({'result': result}))

        def act(self, path, name, body):
            if path == '/api/save':
                values = {k: v for k, v in body.get('settings', {}).items() if k in KEY_NAMES}
                if 'password' not in values or values['password'] == '':
                    old = gui.read_preset(name).get('password', '')
                    if old and not body.get('clear_password'):
                        values['password'] = old
                gui.write_preset(name, values)
                return 'saved'
            if path == '/api/delete':
                if gui.server(name).running():
                    raise ValueError('stop it first')
                os.remove(gui.preset_path(name))
                return 'deleted'
            if not os.path.exists(gui.preset_path(name)):
                raise ValueError('no preset ' + name)
            s = gui.server(name)
            if path == '/api/start':
                return s.start()
            if path == '/api/stop':
                return s.stop()
            if path == '/api/console':
                return s.send(str(body.get('line', '')))
            if path == '/api/admin':
                cmd = str(body.get('command', '')).split(' ', 1)
                if cmd[0] not in ADMIN:
                    raise ValueError('not an administration command: ' + cmd[0])
                return s.send('admin ' + ' '.join(cmd))
            raise ValueError('no such action')

    return H


PAGE = r'''<!doctype html><html><head><meta charset="utf-8"><title>csx-server</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
:root{--bg:#1b2228;--panel:#26313a;--ink:#e6e1c8;--dim:#9aa4a8;--accent:#b1a936;--bad:#d8644a}
body{background:var(--bg);color:var(--ink);font:14px system-ui,sans-serif;margin:0;padding:16px}
h1{font-size:20px;color:var(--accent);margin:0 0 12px}h2{font-size:16px;margin:0}
.srv{background:var(--panel);border-radius:6px;padding:12px;margin-bottom:12px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.st{color:var(--dim)}
button{background:#3a4852;color:var(--ink);border:1px solid #556;border-radius:4px;padding:4px 10px;cursor:pointer}
button.go{border-color:var(--accent)}button.bad{border-color:var(--bad)}
input,select{background:#141a1f;color:var(--ink);border:1px solid #445;border-radius:3px;padding:3px 5px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:6px 12px;margin:8px 0}
.grid label{display:flex;justify-content:space-between;gap:6px}.grid input,.grid select{width:130px}
pre{background:#11161a;padding:8px;max-height:220px;overflow:auto;font-size:12px;white-space:pre-wrap}
table{border-collapse:collapse}td{padding:2px 8px}.msg{color:var(--accent)}
</style></head><body>
<h1>Counter-Strike dedicated servers</h1>
<div class="row"><input id="newname" placeholder="new preset name"><button onclick="create()">New preset</button>
<span class="msg" id="msg"></span></div><div id="list"></div>
<script>
const T='@TOKEN@';let keys=[],open={};
async function post(p,b){const r=await fetch(p+'?token='+T,{method:'POST',body:JSON.stringify(b)});
 const j=await r.json();document.getElementById('msg').textContent=j.error||j.result;refresh();}
function field(k,t,v){if(t.startsWith('choice:')){return '<select data-k="'+k+'">'+t.slice(7).split(',').map(o=>
 '<option'+(o==v?' selected':'')+'>'+o+'</option>').join('')+'</select>';}
 return '<input data-k="'+k+'" type="'+(t=='password'?'password':t=='number'?'number':'text')+'" value="'+
 (v||'').replace(/"/g,'&quot;')+'"'+(t=='password'?' placeholder="(unchanged)"':'')+'>';}
function esc(s){return String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
function render(st){keys=st.keys;const L=document.getElementById('list');L.innerHTML='';
 for(const p of st.presets){const d=document.createElement('div');d.className='srv';
  const pl=p.players.map(x=>'<tr><td>#'+x[0]+'</td><td>'+esc(x[1])+'</td><td>'+esc(x[2])+'</td><td>'+
   '<button class="bad" onclick="adm(\''+p.name+'\',\'kick '+x[0]+'\')">Kick</button> '+
   '<button class="bad" onclick="adm(\''+p.name+'\',\'ban '+x[0]+'\')">Ban</button></td></tr>').join('');
  d.innerHTML='<div class="row"><h2>'+esc(p.name)+'</h2><span class="st">'+esc(p.status)+'</span>'+
   (p.running?'<button class="bad" onclick="post(\'/api/stop\',{name:\''+p.name+'\'})">Stop</button>':
   '<button class="go" onclick="post(\'/api/start\',{name:\''+p.name+'\'})">Start</button>')+
   '<button onclick="toggle(\''+p.name+'\')">Settings</button>'+
   (p.running?'':'<button class="bad" onclick="if(confirm(\'Delete preset?\'))post(\'/api/delete\',{name:\''+p.name+'\'})">Delete</button>')+
   '</div><div class="grid" id="g_'+p.name+'" style="display:'+(open[p.name]?'grid':'none')+'">'+
   keys.map(k=>'<label>'+k[2]+' '+field(k[0],k[1],k[0]=='password'?'':p.settings[k[0]])+'</label>').join('')+
   '<label>Password '+(p.password_set?'set':'none')+' <input type="checkbox" data-clear="1"> clear</label>'+
   '<button class="go" onclick="save(\''+p.name+'\')">Save settings</button></div>'+
   (p.running?'<div class="row"><button onclick="adm(\''+p.name+'\',\'status\')">Refresh players</button>'+
   '<input id="say_'+p.name+'" placeholder="message"><button onclick="adm(\''+p.name+'\',\'say \'+v(\'say_'+p.name+'\'))">Say</button>'+
   '<input id="map_'+p.name+'" placeholder="map"><button onclick="adm(\''+p.name+'\',\'map \'+v(\'map_'+p.name+'\'))">Change map</button>'+
   '<button onclick="adm(\''+p.name+'\',\'restart 1\')">Restart round</button>'+
   '<input id="unban_'+p.name+'" placeholder="key or IP"><button onclick="adm(\''+p.name+'\',\'unban \'+v(\'unban_'+p.name+'\'))">Unban</button></div>'+
   '<table>'+pl+'</table><div class="row"><input id="con_'+p.name+'" size="40" placeholder="console command">'+
   '<button onclick="post(\'/api/console\',{name:\''+p.name+'\',line:v(\'con_'+p.name+'\')})">Run</button></div>':'')+
   '<pre>'+esc(p.log.join('\n'))+'</pre>';L.appendChild(d);}}
function v(id){return document.getElementById(id).value;}
function toggle(n){open[n]=!open[n];refresh();}
function adm(n,c){post('/api/admin',{name:n,command:c});}
function save(n){const g=document.getElementById('g_'+n),s={};g.querySelectorAll('[data-k]').forEach(e=>s[e.dataset.k]=e.value);
 post('/api/save',{name:n,settings:s,clear_password:g.querySelector('[data-clear]').checked});}
function create(){const n=v('newname');post('/api/save',{name:n,settings:{name:n,map:'de_dust2',port:'27015'}});open[n]=true;}
async function refresh(){const r=await fetch('/api/state?token='+T);render(await r.json());}
refresh();setInterval(()=>{if(!document.activeElement||document.activeElement.tagName!='INPUT')refresh();},3000);
</script></body></html>'''


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('--exe', default=os.path.join(ROOT, 'build', 'server', 'csx-server'))
    a.add_argument('--game', default=os.path.join(ROOT, 'build', 'gamedata'))
    a.add_argument('--home', default=os.path.join(ROOT, 'build', 'servers'))
    a.add_argument('--port', type=int, default=27080)
    a.add_argument('--print-token-file', default='', help=argparse.SUPPRESS)  # tests: the address written there
    args = a.parse_args()
    gui = Gui(args)
    httpd = ThreadingHTTPServer(('127.0.0.1', args.port), handler(gui))
    url = 'http://127.0.0.1:%d/?token=%s' % (httpd.server_address[1], gui.token)
    if args.print_token_file:
        fd = os.open(args.print_token_file, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w') as f:
            f.write(url + '\n')
    print('csx-server administration: open', url, flush=True)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        for s in gui.servers.values():
            if s.running():
                print('stopping', s.name, s.stop(), flush=True)


if __name__ == '__main__':
    main()
