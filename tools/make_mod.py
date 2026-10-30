#!/usr/bin/env python3
"""A mod (N8, docs/n8_mods.md, src/common/mods.h) made from a Counter-Strike 1.6 or Condition Zero skin pack: its
models and sprites copied into MODS/ID/files/ at the paths this game reads, and a manifest (mod.ini). The source is
read only.

  tools/make_mod.py SOURCE --id ID --name NAME --version VERSION [--author A] [--description D] [--kind cosmetic]
                    --game GAME_DIR --out MODS_DIR

SOURCE is a folder laid out as the PC game's (cstrike/ or czero/, or a skin pack's own folder): models/v_*.mdl,
p_*.mdl, w_*.mdl (the weapons: the same paths here), models/player/CLASS/CLASS.mdl (and CLASST.mdl, its textures)
and sprites/*.spr. This game keeps a player model per map (models\\player\\MAP\\CLASS.mdl, CS 1.6's urban is seal
here): a class's model goes to every map whose archive has that class. Sounds are not taken: this game plays its
sounds from its wave banks (Media\\Sound\\*.xwb), not from files (reported).

Never reads Condition Zero: Deleted Scenes (a source path holding czeror is refused).
"""
import argparse
import os
import shutil
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sxwad  # noqa: E402

CLASS_HERE = {'urban': 'seal'}  # CS 1.6 / CZ class -> this game's name


def mdl_version(path):
    with open(path, 'rb') as f:
        head = f.read(8)
    if len(head) < 8 or head[:4] not in (b'IDST', b'IDSQ'):
        return None
    return struct.unpack('<I', head[4:8])[0]


def player_classes(game):
    """class -> maps whose archive has models\\player\\MAP\\CLASS.mdl"""
    out = {}
    for name in sorted(os.listdir(game)):
        if not name.lower().endswith('.sxwad') or name.lower().startswith(('frontend', 'cachetree')):
            continue
        _, entries = sxwad.entries(os.path.join(game, name))
        for e in entries:
            p = e[0].lower().replace('\\', '/')
            if p.startswith('models/player/') and p.count('/') == 3 and p.endswith('.mdl'):
                _, _, m, f = p.split('/')
                out.setdefault(f[:-4], set()).add(m)
    return out


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('source')
    a.add_argument('--id', required=True)
    a.add_argument('--name', required=True)
    a.add_argument('--version', required=True)
    a.add_argument('--author', default='')
    a.add_argument('--description', default='')
    a.add_argument('--kind', default='cosmetic', choices=['cosmetic', 'content'])
    a.add_argument('--game', required=True)
    a.add_argument('--out', required=True)
    a.add_argument('--only', default='', help='comma separated file names to take (e.g. v_ak47.mdl,gign.mdl)')
    args = a.parse_args()
    src = os.path.abspath(args.source)
    if 'czeror' in src.lower().split(os.sep):
        sys.exit('refused: Condition Zero: Deleted Scenes content is not used')
    only = {x.strip().lower() for x in args.only.split(',') if x.strip()}
    models = os.path.join(src, 'models')
    dest = os.path.join(args.out, args.id)
    files = os.path.join(dest, 'files')
    if os.path.exists(dest):
        sys.exit(f'{dest} exists: remove it first or choose another id')
    classes = player_classes(args.game)
    taken, notes = [], []

    def put(from_path, rel):
        to = os.path.join(files, rel)
        os.makedirs(os.path.dirname(to), exist_ok=True)
        shutil.copyfile(from_path, to)
        taken.append(rel)

    if os.path.isdir(models):
        for f in sorted(os.listdir(models)):
            p = os.path.join(models, f)
            low = f.lower()
            if not os.path.isfile(p) or not low.endswith('.mdl') or not low.startswith(('v_', 'p_', 'w_')):
                continue
            if only and low not in only:
                continue
            v = mdl_version(p)
            if v != 10:
                notes.append(f'models/{f}: not a GoldSrc studio model (version {v}), left out')
                continue
            put(p, 'models/' + low)
        players = os.path.join(models, 'player')
        if os.path.isdir(players):
            for cls in sorted(os.listdir(players)):
                here = CLASS_HERE.get(cls.lower(), cls.lower())
                mdl = os.path.join(players, cls, cls + '.mdl')
                if not os.path.isfile(mdl) or (only and (cls.lower() + '.mdl') not in only):
                    continue
                maps = sorted(classes.get(here, ()))
                if not maps:
                    notes.append(f'models/player/{cls}: no map of this game has the class {here}')
                    continue
                if mdl_version(mdl) != 10:
                    notes.append(f'models/player/{cls}: not a GoldSrc studio model, left out')
                    continue
                tex = os.path.join(players, cls, cls + 'T.mdl')
                for m in maps:
                    put(mdl, f'models/player/{m}/{here}.mdl')
                    if os.path.isfile(tex):
                        put(tex, f'models/player/{m}/{here}t.mdl')
                notes.append(f'models/player/{cls}: as {here} on {len(maps)} maps ({", ".join(maps)})')
    sprites = os.path.join(src, 'sprites')
    if os.path.isdir(sprites) and not only:
        for f in sorted(os.listdir(sprites)):
            if f.lower().endswith('.spr'):
                put(os.path.join(sprites, f), 'sprites/' + f.lower())
    if os.path.isdir(os.path.join(src, 'sound')):
        notes.append('sound/: not taken (this game plays its sounds from its wave banks)')
    if not taken:
        sys.exit('nothing to take: no weapon, player model or sprite found')
    with open(os.path.join(dest, 'mod.ini'), 'w') as f:
        f.write(f'id = {args.id}\nname = {args.name}\nversion = {args.version}\nkind = {args.kind}\n'
                f'requires = 0.8.0\n')
        if args.author:
            f.write(f'author = {args.author}\n')
        if args.description:
            f.write(f'description = {args.description}\n')
        f.write(f'# made by tools/make_mod.py from {src}\n')
    print(f'{dest}: {len(taken)} files')
    for n in notes:
        print('  ' + n)


if __name__ == '__main__':
    main()
