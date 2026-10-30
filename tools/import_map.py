#!/usr/bin/env python3
"""Map import (N6B, docs/n6_maps.md): a GoldSrc (Counter-Strike 1.6 / Condition Zero) map made into an archive this
game loads, beside the game folder's own, never inside it.

  tools/import_map.py BSP [--nav NAV] [--wad DIR_OR_FILE ...] [--name NAME] [--template de_dust2] --game GAME_DIR
                      --out OUT_DIR

The game's maps are SXWad archives (tools/sxwad.py): the map's BSP (GoldSrc version 30, its texture lump holding only
names: the pictures are the archive's XPR), its bot navigation (D:\\cstrike\\maps\\NAME.nav), and a full set of the
models, sounds, sprites and configs a match uses. The engine's texture loader (Mod_LoadTextures, 0x49200..) looks a
texture up by name in the menus' registry (a map's XPR) and otherwise takes the pixels embedded in the BSP (the PC
engine's path, kept): so an imported map's BSP carries every texture embedded, those it took from WAD files copied
in from the WADs given (as a map compiler's -wadinclude does).

Steps: read and check the BSP (version, lumps in bounds, the entities: spawn points of both teams, the objectives
(bomb targets, hostages and rescue zones), buy zones, the sky's name); embed the WAD textures; take the template
archive's shared files (every entry but its own map's: the BSP, nav, PAS, light config, textures, detail textures,
loading screens); add the map's BSP and nav; write OUT_DIR/NAME.SXWad and OUT_DIR/NAME.json (what was found and
whether the map is playable: the game lists only maps whose report says "playable": true).
"""
import argparse
import json
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sxwad  # noqa: E402

LUMPS = ['entities', 'planes', 'textures', 'vertexes', 'visibility', 'nodes', 'texinfo', 'faces', 'lighting',
         'clipnodes', 'leafs', 'marksurfaces', 'edges', 'surfedges', 'models']


def read_bsp(path):
    b = open(path, 'rb').read()
    version = struct.unpack_from('<I', b, 0)[0]
    lumps = [struct.unpack_from('<II', b, 4 + 8 * i) for i in range(15)]
    for i, (o, l) in enumerate(lumps):
        if o + l > len(b):
            raise ValueError(f'lump {LUMPS[i]} past the file\'s end')
    return b, version, lumps


def entity_blocks(text):
    """the entity text's blocks, "{ ... }" each with its braces: braces inside quoted values (a decal's "{siteA")
    belong to the value"""
    out, start, quoted = [], None, False
    for i, c in enumerate(text):
        if c == '"':
            quoted = not quoted
        elif not quoted and c == '{':
            start = i
        elif not quoted and c == '}' and start is not None:
            out.append(text[start:i + 1])
            start = None
    return out


def entities(text):
    return [dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', block)) for block in entity_blocks(text)]


def wad_lumps(path):
    """WAD3 lumps: name (upper) -> bytes, for miptex (type 0x43)"""
    b = open(path, 'rb').read()
    if b[:4] != b'WAD3':
        return {}
    n, at = struct.unpack_from('<II', b, 4)
    out = {}
    for i in range(n):
        off, disk, size, typ, comp, _, _, name = struct.unpack_from('<IIIBBBB16s', b, at + 32 * i)
        if typ == 0x43 and comp == 0:
            out[name.split(b'\0')[0].decode('latin1').upper()] = b[off:off + disk]
    return out


def replace_entities(b, text):
    """the BSP with its entity lump (lump 0) replaced by text (the other lumps as they are, moved after it)"""
    version = struct.unpack_from('<I', b, 0)[0]
    lumps = [struct.unpack_from('<II', b, 4 + 8 * i) for i in range(15)]
    parts, table, at = [], [], 4 + 8 * 15
    for i, (lo, ll) in enumerate(lumps):
        data = text.encode('latin1') + b'\0' if i == 0 else b[lo:lo + ll]
        table.append((at, len(data)))
        parts.append(data + b'\0' * (-len(data) % 4))
        at += len(parts[-1])
    return struct.pack('<I', version) + b''.join(struct.pack('<II', *t) for t in table) + b''.join(parts)


def without_models(text, missing):
    """the entity text without the entities whose model is one of missing (decorations: a map's props)"""
    out, dropped = [], 0
    for block in entity_blocks(text):
        kv = dict(re.findall(r'"([^"]*)"\s+"([^"]*)"', block))
        if kv.get('model', '').replace('\\', '/').lower() in missing:
            dropped += 1
            continue
        out.append(block)
    return '\n'.join(out) + '\n', dropped


def embed_textures(b, lumps, wads):
    """the texture lump rebuilt with every miptex holding its pixels (from the WADs where the BSP has only a name);
    the BSP's other lumps moved after it. Returns (new bsp, embedded names, missing names)"""
    o, l = lumps[2]
    tl = b[o:o + l]
    n = struct.unpack_from('<I', tl, 0)[0]
    offs = struct.unpack_from(f'<{n}i', tl, 4)
    mips, embedded, missing = [], [], []
    for off in offs:
        if off < 0:
            mips.append(None)
            continue
        name = tl[off:off + 16].split(b'\0')[0].decode('latin1')
        w, h = struct.unpack_from('<II', tl, off + 16)
        data_offs = struct.unpack_from('<4I', tl, off + 24)
        if data_offs[0]:  # pixels in the BSP: the miptex as it is (to the end of its palette)
            size = data_offs[3] + (w // 8) * (h // 8) + 2 + 768 + 2
            mips.append(tl[off:off + size])
            continue
        src = None
        for wad in wads:
            src = wad.get(name.upper())
            if src:
                break
        if src:
            mips.append(name.encode('latin1').ljust(16, b'\0')[:16] + src[16:])  # the BSP's name for it
            embedded.append(name)
        else:
            mips.append(tl[off:off + 40])
            missing.append(name)
    # the new lump: count, offsets, the miptex blocks (each 4-aligned)
    head = 4 + 4 * n
    body = b''
    new_offs = []
    for m in mips:
        if m is None:
            new_offs.append(-1)
            continue
        new_offs.append(head + len(body))
        body += m + b'\0' * (-len(m) % 4)
    newtl = struct.pack(f'<I{n}i', n, *new_offs) + body
    # the BSP again: the lumps in their old order, each 4-aligned
    parts = []
    at = 4 + 8 * 15
    table = []
    for i, (lo, ll) in enumerate(lumps):
        data = newtl if i == 2 else b[lo:lo + ll]
        table.append((at, len(data)))
        parts.append(data + b'\0' * (-len(data) % 4))
        at += len(parts[-1])
    out = struct.pack('<I', 30) + b''.join(struct.pack('<II', *t) for t in table) + b''.join(parts)
    return out, embedded, missing


def swizzle(rgba, n):
    """A8R8G8B8 (B, G, R, A bytes) of an n x n RGBA image, Xbox swizzled (x and y bits interleaved, x first)"""
    out = bytearray(n * n * 4)
    bits = n.bit_length() - 1
    for y in range(n):
        for x in range(n):
            i = 0
            for bit in range(bits):
                i |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
            r, g, b, a = rgba[(y * n + x) * 4:(y * n + x) * 4 + 4]
            out[i * 4:i * 4 + 4] = bytes((b, g, r, a))
    return bytes(out)


def merge_xpr(template, extra):
    """an XPR1 (tools/sxwad.py xpr): the template's resources as they are, then extra [(name, n, swizzled data)]
    textures (A8R8G8B8, n x n); the template's data first, at the same offsets"""
    magic, total, header, count = struct.unpack_from('<4sIII', template, 0)
    entries = []
    for i in range(count):
        name_off, res_off = struct.unpack_from('<II', template, 16 + 8 * i)
        name = template[12 + name_off:].split(b'\0')[0]
        res = template[12 + res_off:12 + res_off + 20]
        entries.append((name, res))
    data = bytearray(template[header:total])
    for name, n, pixels in extra:
        bits = n.bit_length() - 1
        fmt = 0x29 | (0x06 << 8) | (1 << 16) | (bits << 20) | (bits << 24)
        res = struct.pack('<IIIII', 0x00040001, len(data), 0, fmt, 0)
        entries.append((name.encode('latin1'), res))
        data += pixels
        data += b'\0' * (-len(data) % 128)
    count = len(entries)
    pairs_end = 16 + 8 * count + 4
    names = b''
    name_offs = []
    for name, _ in entries:
        name_offs.append(pairs_end + len(names) - 12)
        names += name + b'\0'
    res_start = (pairs_end + len(names) + 3) & ~3
    head_size = (res_start + 20 * count + 0x7FF) & ~0x7FF
    head = bytearray(head_size)
    for i, (name, res) in enumerate(entries):
        struct.pack_into('<II', head, 16 + 8 * i, name_offs[i], res_start + 20 * i - 12)
        head[res_start + 20 * i:res_start + 20 * i + 20] = res
    head[pairs_end:pairs_end + len(names)] = names
    struct.pack_into('<4sIII', head, 0, b'XPR1', head_size + len(data), head_size, count)
    return bytes(head + data)


def sky_textures(sky, gfx_dirs, report):
    """the sky's six faces (gfx/env/SKY{bk,dn,ft,lf,rt,up}.tga) as this game names them in a map's XPR
    (skies/SKYbk.tga ...), 256 x 256"""
    from PIL import Image
    out = []
    for face in ('bk', 'dn', 'ft', 'lf', 'rt', 'up'):
        found = None
        for d in gfx_dirs:
            env = os.path.join(d, 'gfx', 'env')
            if not os.path.isdir(env):
                continue
            for f in os.listdir(env):
                if f.lower() == f'{sky}{face}.tga'.lower():
                    found = os.path.join(env, f)
        if not found:
            report['notes'].append(f'sky face {sky}{face}.tga not found: that face is not drawn')
            continue
        im = Image.open(found).convert('RGBA').resize((256, 256), Image.LANCZOS)
        out.append((f'skies/{sky.lower()}{face}.tga', 256, swizzle(im.tobytes(), 256)))
    return out


def write_sxwad(path, files):
    """files: [(archive name, bytes)]: the SXWad layout tools/sxwad.py reads (zlib each, stored as is when it does
    not shrink)"""
    stored = []
    for name, data in files:
        if data is None:  # a placeholder entry (kept as the template has it)
            stored.append((name, None, b''))
            continue
        z = zlib.compress(data, 9)
        stored.append((name, data, z if len(z) < len(data) else data))
    table = bytearray()
    for name, data, s in stored:
        nb = name.encode('latin1')
        size = 0xFFFFFFFF if data is None else len(data)
        table += struct.pack('<IIII', size, len(s), 0, len(nb)) + nb + b'\0'
        table += b'\0' * (-len(table) % 4)
    table_end = 16 + len(table)
    out = bytearray(struct.pack('<IIII', 1, len(stored), table_end, 0))
    at = 16
    offset = table_end
    for name, data, s in stored:
        nb = name.encode('latin1')
        struct.pack_into('<I', table, at - 16 + 8, offset)
        at += 16 + len(nb) + 1
        at = (at + 3) & ~3
        offset += len(s)
    out += table
    for _, _, s in stored:
        out += s
    with open(path, 'wb') as f:
        f.write(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('bsp')
    ap.add_argument('--nav')
    ap.add_argument('--wad', action='append', default=[])
    ap.add_argument('--name')
    ap.add_argument('--template', default='', help='the archive whose shared files are taken (de_dust2 for a bomb map, '
                    'cs_italy for a hostage map when not given: its hostage model)')
    ap.add_argument('--gfx', action='append', default=[], help='a mod folder with gfx/env (the sky)')
    ap.add_argument('--drop-missing-models', action='store_true',
                    help='leave out the entities whose model or sprite is found nowhere (decorations), noted in the report')
    ap.add_argument('--game', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    name = (a.name or os.path.splitext(os.path.basename(a.bsp))[0]).lower()
    report = {'name': name, 'source': os.path.abspath(a.bsp), 'problems': [], 'notes': []}
    b, version, lumps = read_bsp(a.bsp)
    report['bsp_version'] = version
    if version != 30:
        report['problems'].append(f'BSP version {version}: only GoldSrc version 30 maps load')
    ents = entities(b[lumps[0][0]:lumps[0][0] + lumps[0][1]].decode('latin1', 'replace'))
    world = ents[0] if ents else {}
    classes = [e.get('classname', '') for e in ents]
    ct = classes.count('info_player_start')
    t = classes.count('info_player_deathmatch')
    bomb = classes.count('func_bomb_target') + classes.count('info_bomb_target')
    hostages = classes.count('hostage_entity') + classes.count('monster_scientist')
    rescue = classes.count('func_hostage_rescue') + classes.count('info_hostage_rescue')
    buy = classes.count('func_buyzone')
    report.update(spawns={'ct': ct, 't': t}, bomb_targets=bomb, hostages=hostages, rescue_zones=rescue, buy_zones=buy,
                  sky=world.get('skyname', ''), entities=len(ents))
    kind = 'bomb' if bomb else 'hostage' if hostages else 'unknown'
    report['kind'] = kind
    if not ct or not t:
        report['problems'].append(f'spawn points: {ct} Counter-Terrorist, {t} Terrorist (both teams need some)')
    if kind == 'unknown':
        report['problems'].append('no objective: no bomb target and no hostage')
    if kind == 'hostage' and not rescue:
        report['notes'].append('no rescue zone entity: the game makes rescue zones at the Counter-Terrorist spawns')
    if not buy:
        report['notes'].append('no buy zone entity: the game makes buy zones at the spawn points')
    wads = []
    for w in a.wad:
        if os.path.isdir(w):
            wads += [wad_lumps(os.path.join(w, f)) for f in sorted(os.listdir(w)) if f.lower().endswith('.wad')]
        elif os.path.isfile(w):
            wads.append(wad_lumps(w))
    newbsp, embedded, missing = embed_textures(b, lumps, wads)
    report['textures_embedded_from_wads'] = len(embedded)
    if missing:
        report['problems'].append(f'{len(missing)} textures in no WAD given: ' + ', '.join(missing[:12]))
    nav = None
    if a.nav and os.path.isfile(a.nav):
        nav = open(a.nav, 'rb').read()
    else:
        report['notes'].append('no navigation file: bots cannot play it (bot_nav_analyze in a match could make one)')
    report['bots'] = nav is not None
    # the models and sprites the map's entities name (env_sprite, cycler, props...): from the source's game folders
    # (the BSP's ../ and the folders given with --wad / --gfx), unless the template has them; one not found makes the
    # map unplayable (the engine stops a map whose model is missing)
    wanted = sorted({e[k].replace('\\', '/').lower() for e in ents for k in ('model',)
                     if k in e and e[k].lower().startswith(('models/', 'sprites/')) and
                     e[k].lower().endswith(('.mdl', '.spr'))})
    roots = [os.path.dirname(os.path.dirname(os.path.abspath(a.bsp)))] + list(a.wad) + list(a.gfx or [])
    entity_files, entity_missing = [], []
    for w in wanted:
        if 'czeror' in w:
            continue
        found = None
        for r in roots:
            p = os.path.join(r, *w.split('/'))
            if os.path.isdir(r) and 'czeror' not in os.path.abspath(p).lower().split(os.sep) and os.path.isfile(p):
                found = p
                break
        if found:
            entity_files.append((w.replace('/', '\\'), open(found, 'rb').read()))
        else:
            entity_missing.append(w)
    report['entity_models'] = len(entity_files)
    # the template's shared files
    if not a.template:
        a.template = 'cs_italy' if kind == 'hostage' else 'de_dust2'
    report['template'] = a.template
    tpath = None
    for f in os.listdir(a.game):
        if f.lower() == a.template + '.sxwad':
            tpath = os.path.join(a.game, f)
    if not tpath:
        sys.exit(f'no template archive {a.template}.SXWad in {a.game}')
    tb, tes = sxwad.entries(tpath)
    own = a.template.lower()
    files = []
    for e in tes:
        n = e[0]
        low = n.lower().replace('/', '\\')
        # the template map's own files (its BSP, navigation, textures, detail, loading screens, light config) are
        # left out; its player models (models\player\TEMPLATE\...) stay: the imported map uses them
        if own in low and not low.startswith('models\\player\\'):
            continue
        data = sxwad.data(tb, e)
        if low == 'playermodels.txt' and data is not None:
            # the map's player (and hostage) models: the template's line under the imported map's name (the game
            # looks the map up there; a map it does not find gets models\player\default\..., which no archive has)
            text = data.decode('latin1')
            line = next((l for l in text.splitlines() if l.split() and l.split()[0].lower() == own), None)
            if line:
                text = text.rstrip('\r\n') + '\r\n' + name + '\t' + ' '.join(line.split()[1:]) + '\r\n'
                data = text.encode('latin1')
                report['player_models'] = line.split()[1:]
            else:
                report['problems'].append(f'the template {a.template} has no playermodels.txt line')
        files.append((n, data))
    have = {f[0].lower().replace('/', '\\') for f in files}
    for n, d in entity_files:
        if n.lower() not in have:
            files.append((n, d))
    still = [w for w in entity_missing if w.replace('/', '\\') not in have]
    if still and a.drop_missing_models:
        text, dropped = without_models(b[lumps[0][0]:lumps[0][0] + lumps[0][1]].split(b'\0')[0].decode('latin1'), set(still))
        newbsp = replace_entities(newbsp, text)
        report['notes'].append(f'{dropped} entities left out, their models not found (--drop-missing-models): ' +
                               ', '.join(still[:12]))
    elif still:
        report['problems'].append(f'{len(still)} models or sprites the map names were not found: ' + ', '.join(still[:12]) +
                                  ' (--drop-missing-models leaves those entities out)')
    # the map's textures: the template's (the models' among them) and its sky's
    txpr = [e for e in tes if e[0].lower().endswith(f'textures\\{own}.xpr')]
    if txpr:
        sky = world.get('skyname', '')
        extra = sky_textures(sky, a.gfx, report) if sky else []
        files.append((f'd:\\media\\textures\\{name}.xpr', merge_xpr(sxwad.data(tb, txpr[0]), extra)))
        report['sky_faces'] = len(extra)
    files.append((f'maps\\{name}.bsp', newbsp))
    if nav:
        # a version 5 navigation file records its BSP's size (the bots refuse it when the BSP's differs): the BSP
        # rebuilt here (its textures embedded) has another size, the same geometry
        if len(nav) >= 12 and struct.unpack_from('<II', nav, 0) == (0xFEEDFACE, 5):
            nav = nav[:8] + struct.pack('<I', len(newbsp)) + nav[12:]
            report['notes'].append('the navigation file\'s BSP size set to the rebuilt BSP\'s')
        files.append((f'D:\\cstrike\\maps\\{name}.nav', nav))
    os.makedirs(a.out, exist_ok=True)
    write_sxwad(os.path.join(a.out, name + '.SXWad'), files)
    report['playable'] = not report['problems']
    report['archive_entries'] = len(files)
    with open(os.path.join(a.out, name + '.json'), 'w') as f:
        json.dump(report, f, indent=2)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
