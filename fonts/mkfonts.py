"""mkfonts.py -- make Diamond's reading-view fonts (src/rfonts.c) from DejaVu.

    python3 mkfonts.py > ../src/rfonts.c

Each face is drawn by FreeType (through Pillow) at its pixel size, one glyph
at a time, and kept as 2-bit coverage (0 = background, 3 = full ink), four
pixels to a byte: at these sizes four shades look the same as sixteen.  The
characters are ASCII, Latin-1 and a few typographic ones (curly quotes,
dashes, the ellipsis, the bullet); the heading faces have ASCII and the
typographic ones only, and the reading view uses the bold face for the
rest.  DejaVu's licence
(LICENSE-DejaVu.txt) allows this."""
from PIL import Image, ImageDraw, ImageFont
import sys

FD = '/usr/share/fonts/truetype/dejavu/'
FACES = [  # name, file, size, with Latin-1
    ('body',       'DejaVuSans.ttf', 14, True),
    ('bold',       'DejaVuSans-Bold.ttf', 14, True),
    ('italic',     'DejaVuSans-Oblique.ttf', 14, True),
    ('bolditalic', 'DejaVuSans-BoldOblique.ttf', 14, True),
    ('mono',       'DejaVuSansMono.ttf', 13, True),
    ('h1',         'DejaVuSans-Bold.ttf', 22, False),
    ('h2',         'DejaVuSans-Bold.ttf', 18, False),
    ('h3',         'DejaVuSans-Bold.ttf', 16, False),
]
EXTRA = [0x2018, 0x2019, 0x201C, 0x201D, 0x2013, 0x2014, 0x2026, 0x2022]
CHARS = list(range(32, 127)) + list(range(160, 256)) + EXTRA

out = sys.stdout
out.write('/* rfonts.c -- made by fonts/mkfonts.py from DejaVu Sans; do not edit.\n'
          '   DejaVu fonts: see fonts/LICENSE-DejaVu.txt. */\n#include "rfonts.h"\n\n')
total = 0
for name, file, size, latin1 in FACES:
    f = ImageFont.truetype(FD + file, size)
    ascent, descent = f.getmetrics()
    glyphs, bits = [], bytearray()
    for cp in CHARS:
        if not latin1 and 160 <= cp < 256:
            glyphs.append((0, 0, 0, 0, 0, 0))     # not here: the bold face stands in
            continue
        ch = chr(cp)
        adv = f.getlength(ch)
        box = f.getbbox(ch)            # relative to the origin at the ascent line
        x0, y0, x1, y1 = box
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        im = Image.new('L', (max(w, 1), max(h, 1)), 0)
        if w and h:
            ImageDraw.Draw(im).text((-x0, -y0), ch, font=f, fill=255)
        px = list(im.get_flattened_data()) if w and h else []
        q = [min(3, (v * 3 + 127) // 255) for v in px]
        while len(q) % 4: q.append(0)
        off = len(bits)
        for i in range(0, len(q), 4):
            bits.append(q[i] << 6 | q[i + 1] << 4 | q[i + 2] << 2 | q[i + 3])
        glyphs.append((round(adv * 4), w, h, x0, y0, off))
    out.write('static const uint8_t %s_bits[%d] = {\n' % (name, max(1, len(bits))))
    for i in range(0, len(bits), 20):
        out.write('\t' + ''.join('%d,' % b for b in bits[i:i + 20]) + '\n')
    out.write('};\nstatic const rglyph_t %s_glyphs[%d] = {\n' % (name, len(glyphs)))
    for g in glyphs:
        out.write('\t{ %d, %d, %d, %d, %d, %d },\n' % g)
    out.write('};\nconst rfont_t rfont_%s = { %d, %d, %d, %s_glyphs, %s_bits };\n\n'
              % (name, ascent + descent, ascent, size, name, name))
    assert len(bits) < 65536
    total += len(bits) + len(glyphs) * 8
    print('%-10s %2d px: %6d bytes of pixels' % (name, size, len(bits)), file=sys.stderr)
print('total about %d bytes' % total, file=sys.stderr)
