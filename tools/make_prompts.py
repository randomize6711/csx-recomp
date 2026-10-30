#!/usr/bin/env python3
"""Button prompt artwork (N5D): one glyph per controller button and style, drawn here in the original Counter-Strike
Xbox legend's manner (a round button with a darker rim, a soft highlight, a bold letter or symbol; the title's legends
are the reference: launcher.xpr's Cancel_Accept_Reset), written as PNGs (to look at) and as one XPR1 the game loads
into its menu texture registry (src/game/pc_prompts.cpp).

  tools/make_prompts.py OUTDIR

Textures: CSXP_<slot> (what the menus draw; the game copies the chosen style's pixels into them) and
CSXP_<style>_<slot>, 64x64, A8R8G8B8 swizzled (the XPR1 layout tools/sxwad.py reads). Styles: pc (key
caps and the mouse), xbox (the original controller), x360, series (Xbox Series X|S), ps (DualShock 4), dualsense,
switch, steam (the Steam Controller), wii (Wii Remote and Nunchuk). Slots: the SDL gamepad buttons the game uses
(a b x y back start lb rb lt rt ls rs dup ddown dleft dright); for pc the key caps of the keys the menus take for
them, plus key (a blank cap) and m1 m2 m3 (the mouse's buttons). A button a controller lacks is drawn as its name on a
plain bar (the text fallback).
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

N = 64
FONT = '/usr/share/fonts/noto/NotoSans-Bold.ttf'


def font(size):
    try:
        return ImageFont.truetype(FONT, size)
    except OSError:
        return ImageFont.load_default()


def rgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def shade(c, f):
    return tuple(max(0, min(255, int(v * f))) for v in c)


def disc(fill, rim=None, letter=None, ink=(255, 255, 255), shape=None, shape_ink=None, size=None):
    """A round button: the title's legend buttons are filled discs with a darker rim and a light top."""
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rim = rim or shade(fill, 0.55)
    d.ellipse((2, 2, N - 3, N - 3), fill=rim + (255,))
    d.ellipse((6, 6, N - 7, N - 7), fill=fill + (255,))
    d.ellipse((12, 9, N - 13, N // 2), fill=shade(fill, 1.25) + (90,))  # the highlight
    if letter:
        f = font(size or 34)
        box = d.textbbox((0, 0), letter, font=f)
        w, h = box[2] - box[0], box[3] - box[1]
        d.text(((N - w) / 2 - box[0], (N - h) / 2 - box[1] + 1), letter, font=f, fill=ink + (255,))
    if shape:
        draw_shape(d, shape, shape_ink or ink)
    return im


def draw_shape(d, shape, ink):
    c, r, w = N / 2, 14, 5
    col = ink + (255,)
    if shape == 'cross':
        d.line((c - r, c - r, c + r, c + r), fill=col, width=w)
        d.line((c - r, c + r, c + r, c - r), fill=col, width=w)
    elif shape == 'circle':
        d.ellipse((c - r, c - r, c + r, c + r), outline=col, width=w)
    elif shape == 'square':
        d.rectangle((c - r + 1, c - r + 1, c + r - 1, c + r - 1), outline=col, width=w)
    elif shape in ('left', 'right'):
        k = -1 if shape == 'left' else 1
        d.polygon([(c + k * 13, c), (c - k * 9, c - 13), (c - k * 9, c + 13)], fill=col)
    elif shape == 'triangle':
        d.polygon([(c, c - r - 1), (c + r + 2, c + r - 3), (c - r - 2, c + r - 3)], outline=col, width=w)


def pill(label, fill, ink=(255, 255, 255), size=22, tall=False):
    """Shoulder buttons, triggers, Back / Start: a rounded bar with its name, as the title's legends write theirs."""
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bottom = (10, N - 11) if tall else (16, N - 17)
    d.rounded_rectangle((2, top, N - 3, bottom), radius=10, fill=shade(fill, 0.55) + (255,))
    d.rounded_rectangle((5, top + 3, N - 6, bottom - 3), radius=8, fill=fill + (255,))
    f = font(size)
    box = d.textbbox((0, 0), label, font=f)
    w, h = box[2] - box[0], box[3] - box[1]
    d.text(((N - w) / 2 - box[0], (N - h) / 2 - box[1]), label, font=f, fill=ink + (255,))
    return im


def dpad(direction, fill=(70, 74, 78), ink=(255, 255, 255)):
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    a, b = 22, 42
    for box in ((a, 4, b, N - 5), (4, a, N - 5, b)):
        d.rectangle(box, fill=shade(fill, 0.6) + (255,))
    for box in ((a + 3, 7, b - 3, N - 8), (7, a + 3, N - 8, b - 3)):
        d.rectangle(box, fill=fill + (255,))
    c = N / 2
    arrow = {'dup': [(c, 8), (c + 9, 20), (c - 9, 20)], 'ddown': [(c, N - 9), (c + 9, N - 21), (c - 9, N - 21)],
             'dleft': [(8, c), (20, c - 9), (20, c + 9)], 'dright': [(N - 9, c), (N - 21, c - 9), (N - 21, c + 9)]}
    d.polygon(arrow[direction], fill=ink + (255,))
    return im


def stick(label, fill=(60, 64, 68)):
    im = disc(fill, letter=label, size=24)
    return im


def keycap(label='', size=None):
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((3, 6, N - 4, N - 4), radius=9, fill=(96, 104, 112, 255))
    d.rounded_rectangle((7, 8, N - 8, N - 11), radius=7, fill=(222, 226, 230, 255))
    if label:
        f = font(size or (26 if len(label) <= 2 else 17 if len(label) == 3 else 13))
        box = d.textbbox((0, 0), label, font=f)
        w, h = box[2] - box[0], box[3] - box[1]
        d.text(((N - w) / 2 - box[0], (N - 6 - h) / 2 - box[1]), label, font=f, fill=(40, 46, 52, 255))
    return im


def mouse(button):
    im = Image.new('RGBA', (N, N), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((14, 6, N - 15, N - 5), radius=17, fill=(222, 226, 230, 255), outline=(96, 104, 112, 255),
                        width=3)
    d.line((N / 2, 8, N / 2, 28), fill=(96, 104, 112, 255), width=2)
    d.line((16, 28, N - 17, 28), fill=(96, 104, 112, 255), width=2)
    hl = (233, 160, 40, 255)
    if button == 'm1':
        d.rounded_rectangle((17, 9, N / 2 - 2, 26), radius=6, fill=hl)
    elif button == 'm2':
        d.rounded_rectangle((N / 2 + 2, 9, N - 18, 26), radius=6, fill=hl)
    else:
        d.rounded_rectangle((N / 2 - 3, 11, N / 2 + 3, 24), radius=3, fill=hl)
    return im


GREEN, RED, BLUE, YELLOW = rgb('#3cb34b'), rgb('#d93a2f'), rgb('#2f74c9'), rgb('#efbe2c')
DARK, GREY = (46, 50, 54), (120, 126, 132)


def style(name):
    """name -> {slot: image}"""
    g = {}
    common_d = {k: dpad(k) for k in ('dup', 'ddown', 'dleft', 'dright')}
    if name == 'pc':  # the keys the menus take for the pad's buttons (src/linux/window_input.cpp)
        g.update(a=keycap('SPACE'), b=keycap('ESC'), x=keycap('R'), y=keycap('Q'), back=keycap('TAB'),
                 start=keycap('ENTER'), lb=keycap('PGUP', 12), rb=keycap('PGDN', 12), lt=keycap('PGUP', 12),
                 rt=keycap('PGDN', 12), ls=keycap('SHIFT', 12), rs=mouse('m3'), key=keycap())
        for k in ('dup', 'ddown', 'dleft', 'dright'):  # the arrow keys: a cap with its arrow drawn
            im = keycap()
            d = ImageDraw.Draw(im)
            c, y = N / 2, N / 2 - 3
            pts = {'dup': [(c, y - 11), (c + 11, y + 8), (c - 11, y + 8)], 'ddown': [(c, y + 11), (c + 11, y - 8), (c - 11, y - 8)],
                   'dleft': [(c - 12, y), (c + 8, y - 11), (c + 8, y + 11)], 'dright': [(c + 12, y), (c - 8, y - 11), (c - 8, y + 11)]}
            d.polygon(pts[k], fill=(40, 46, 52, 255))
            g[k] = im
        for m in ('m1', 'm2', 'm3'):
            g[m] = mouse(m)
        return g
    if name == 'xbox':  # the original controller: coloured face buttons, black and white, L and R triggers
        g.update(a=disc(GREEN, letter='A'), b=disc(RED, letter='B'), x=disc(BLUE, letter='X'),
                 y=disc(YELLOW, letter='Y', ink=(60, 50, 10)), lb=disc((240, 240, 240)),
                 rb=disc((24, 24, 24)), lt=pill('L', GREY, size=28, tall=True),
                 rt=pill('R', GREY, size=28, tall=True), back=pill('BACK', (170, 174, 178), (30, 30, 30), 15),
                 start=pill('START', (170, 174, 178), (30, 30, 30), 13), ls=stick('LS'), rs=stick('RS'))
    elif name == 'x360':
        g.update(a=disc(GREEN, letter='A'), b=disc(RED, letter='B'), x=disc(BLUE, letter='X'), y=disc(YELLOW, letter='Y'),
                 lb=pill('LB', DARK), rb=pill('RB', DARK), lt=pill('LT', DARK, tall=True), rt=pill('RT', DARK, tall=True),
                 back=pill('BACK', DARK, size=15), start=pill('START', DARK, size=13), ls=stick('LS'), rs=stick('RS'))
    elif name == 'series':  # dark buttons, coloured letters
        g.update(a=disc(DARK, letter='A', ink=GREEN), b=disc(DARK, letter='B', ink=RED), x=disc(DARK, letter='X', ink=BLUE),
                 y=disc(DARK, letter='Y', ink=YELLOW), lb=pill('LB', DARK), rb=pill('RB', DARK),
                 lt=pill('LT', DARK, tall=True), rt=pill('RT', DARK, tall=True), back=pill('VIEW', DARK, size=15),
                 start=pill('MENU', DARK, size=15), ls=stick('LS'), rs=stick('RS'))
    elif name in ('ps', 'dualsense'):
        if name == 'ps':  # DualShock 4: black buttons, coloured symbols
            base, inks = (30, 32, 36), {'cross': rgb('#7fa9e6'), 'circle': rgb('#e8706a'), 'square': rgb('#de8fc6'),
                                        'triangle': rgb('#4fcfa3')}
            back, start = 'SHARE', 'OPTIONS'
        else:  # DualSense: white buttons, grey symbols
            base, inks = (232, 234, 238), {k: (110, 114, 120) for k in ('cross', 'circle', 'square', 'triangle')}
            back, start = 'CREATE', 'OPTIONS'
        g.update(a=disc(base, shape='cross', shape_ink=inks['cross']), b=disc(base, shape='circle', shape_ink=inks['circle']),
                 x=disc(base, shape='square', shape_ink=inks['square']), y=disc(base, shape='triangle', shape_ink=inks['triangle']),
                 lb=pill('L1', DARK), rb=pill('R1', DARK), lt=pill('L2', DARK, tall=True), rt=pill('R2', DARK, tall=True),
                 back=pill(back, DARK, size=11), start=pill(start, DARK, size=10), ls=stick('L3', DARK), rs=stick('R3', DARK))
    elif name == 'switch':  # the buttons by position: south is B, east A, west Y, north X
        g.update(a=disc(DARK, letter='B'), b=disc(DARK, letter='A'), x=disc(DARK, letter='Y'), y=disc(DARK, letter='X'),
                 lb=pill('L', DARK, size=26), rb=pill('R', DARK, size=26), lt=pill('ZL', DARK, tall=True),
                 rt=pill('ZR', DARK, tall=True), back=disc(DARK, letter='−', size=34), start=disc(DARK, letter='+'),
                 ls=stick('LS'), rs=stick('RS'))
    elif name == 'steam':  # the Steam Controller: A B X Y in colour on dark, bumpers, triggers, the arrows either side
        g.update(a=disc(DARK, letter='A', ink=GREEN), b=disc(DARK, letter='B', ink=RED), x=disc(DARK, letter='X', ink=BLUE),
                 y=disc(DARK, letter='Y', ink=YELLOW), lb=pill('LB', DARK), rb=pill('RB', DARK),
                 lt=pill('LT', DARK, tall=True), rt=pill('RT', DARK, tall=True), back=disc(DARK, shape='left'),
                 start=disc(DARK, shape='right'), ls=stick('LS'), rs=stick('RP'))
    elif name == 'wii':  # Wii Remote and Nunchuk: A, B (the trigger), 1, 2, minus, plus; the Nunchuk's C and Z
        white = (236, 238, 240)
        g.update(a=disc(white, letter='A', ink=(80, 84, 90)), b=pill('B', (150, 154, 160), size=28, tall=True),
                 x=disc(white, letter='1', ink=(80, 84, 90)), y=disc(white, letter='2', ink=(80, 84, 90)),
                 lb=disc(white, letter='C', ink=(80, 84, 90)), rb=pill('Z', (150, 154, 160), size=28, tall=True),
                 back=disc(white, letter='−', ink=(80, 84, 90), size=34), start=disc(white, letter='+', ink=(80, 84, 90)))
    g.update(common_d)
    # a button the controller lacks: its name on a plain bar (the text fallback, drawn)
    for slot, label in (('lt', 'LT'), ('rt', 'RT'), ('ls', 'LS'), ('rs', 'RS'), ('lb', 'LB'), ('rb', 'RB')):
        g.setdefault(slot, pill(label, GREY))
    return g


STYLES = ['pc', 'xbox', 'x360', 'series', 'ps', 'dualsense', 'switch', 'steam', 'wii']


def swizzle(im):
    """A8R8G8B8, Xbox swizzled (a square power of two: x and y bits interleaved, x first)"""
    px = im.load()
    out = bytearray(N * N * 4)
    for y in range(N):
        for x in range(N):
            i = 0
            for bit in range(6):
                i |= ((x >> bit) & 1) << (2 * bit) | ((y >> bit) & 1) << (2 * bit + 1)
            r, g, b, a = px[x, y]
            out[i * 4:i * 4 + 4] = bytes((b, g, r, a))
    return bytes(out)


def write_xpr(path, textures):
    """XPR1 (tools/sxwad.py xpr): header, (name offset, resource offset) pairs from byte 12 ended by 0, the names, the
    20-byte D3D texture headers, then the data from the header's size on"""
    count = len(textures)
    names = b''
    name_offs = []
    pairs_end = 16 + 8 * count + 4
    for name, _ in textures:
        name_offs.append(pairs_end + len(names) - 12)
        names += name.encode() + b'\0'
    res_start = (pairs_end + len(names) + 3) & ~3
    header_size = (res_start + 20 * count + 0x7FF) & ~0x7FF
    fmt = 0x29 | (0x06 << 8) | (1 << 16) | (6 << 20) | (6 << 24)  # 2D, A8R8G8B8, one level, 64x64
    head = bytearray(header_size)
    data = bytearray()
    for i, (name, im) in enumerate(textures):
        struct.pack_into('<II', head, 16 + 8 * i, name_offs[i], res_start + 20 * i - 12)
        struct.pack_into('<IIIII', head, res_start + 20 * i, 0x00040001, len(data), 0, fmt, 0)
        data += swizzle(im)
    head[pairs_end - 4:pairs_end] = b'\0\0\0\0'
    head[pairs_end:pairs_end + len(names)] = names
    struct.pack_into('<4sIII', head, 0, b'XPR1', header_size + len(data), header_size, count)
    with open(path, 'wb') as f:
        f.write(head + data)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(os.path.join(out, 'png'), exist_ok=True)
    textures = []
    # the slots the menus draw (CSXP_<slot>): the game copies the chosen style's into them (Xbox's to start with)
    for slot, im in style('xbox').items():
        textures.append((f'CSXP_{slot}', im))
    for s in STYLES:
        for slot, im in style(s).items():
            name = f'CSXP_{s}_{slot}'
            im.save(os.path.join(out, 'png', name + '.png'))
            textures.append((name, im))
    write_xpr(os.path.join(out, 'csx_prompts.xpr'), textures)
    print(f'{len(textures)} glyphs in {os.path.join(out, "csx_prompts.xpr")}')


if __name__ == '__main__':
    main()
