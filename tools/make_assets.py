#!/usr/bin/env python3
"""Render the clock digits, the built-in quotes and the quote glyphs into 1-bit bitmaps in ../assets.h.

The panel is pure black and white, so typography is baked here with FreeType and the board only
blits bitmaps. The built-in quotes show while the board is offline; live ones from Wikiquote are set on the
board (quote.h) from the same faces, rendered here letter by letter.
Run after editing quotes.txt:  python3 tools/make_assets.py [preview.png]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
FONTS = HERE / "fonts"
OUT = HERE.parent / "assets.h"

HERO_SIZE = 97
PERIOD_SIZE = 22  # the AM/PM after the minutes
HERO_BUDGET = 270  # px between the left margin and the weather tile
QUOTE_W, QUOTE_MAX_H = 376, 72  # quote.h has the same numbers
QUOTE_SIZES = (21, 20, 19, 18, 17, 16)
# Latin-1, plus the dashes, curly quotes and ellipsis Wikiquote uses. Anything else and the board skips the quote.
CHARSET = [*range(32, 127), *range(160, 256), 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D, 0x2026]
TAGS = {"Weight": "wght", "Optical size": "opsz", "Optical Size": "opsz"}


def font(name, size, **axes):
    f = ImageFont.truetype(str(FONTS / name), size)
    if axes:
        names = [a["name"].decode() if isinstance(a["name"], bytes) else a["name"] for a in f.get_variation_axes()]
        f.set_variation_by_axes([axes[TAGS[n]] for n in names])
    return f


def ink(img):
    """Grey image with ink=255 -> 1-bit image cropped to its ink."""
    bw = img.point(lambda p: 255 if p >= 128 else 0).convert("1")
    return bw.crop(bw.getbbox())


def xbm(bw):
    """XBM bytes: rows padded to whole bytes, least significant bit is the leftmost pixel."""
    w, h = bw.size
    px = bw.load()
    return [sum(1 << i for i in range(8) if x + i < w and px[x + i, y]) for y in range(h) for x in range(0, w, 8)]


def hero_glyphs():
    f = font("DMSerifDisplay-Regular.ttf", HERO_SIZE)
    asc, desc = f.getmetrics()
    cell = round(max(f.getlength(c) for c in "0123456789")) + 4  # tabular + a hair of tracking
    raw = {}
    for c in "0123456789:-":
        im = Image.new("L", (cell, asc + desc), 0)
        ImageDraw.Draw(im).text((cell / 2, asc), c, font=f, fill=255, anchor="ms")
        raw[c] = im
    top = min(raw[c].getbbox()[1] for c in "0123456789")
    bot = max(raw[c].getbbox()[3] for c in "0123456789")
    out = {c: raw[c].crop((0, top, cell, bot)).point(lambda p: 255 if p >= 128 else 0).convert("1") for c in "0123456789"}
    for c, pad in ((":", 6), ("-", 6)):  # centre these on the digit height, not the x-height
        g = ink(raw[c])
        box = Image.new("1", (g.width + 2 * pad, bot - top), 0)
        box.paste(g, (pad, (bot - top - g.height) // 2))
        out[c] = box
    # 12-hour hours only ever lead with a 1 (10, 11, 12), so it gets a narrow cell of its own.
    one = ink(raw["1"])
    lead = Image.new("1", (one.width + 4, bot - top), 0)
    lead.paste(one, (0, bot - top - one.height))
    out["lead1"] = lead
    p = font("DMSerifDisplay-Regular.ttf", PERIOD_SIZE)
    for word in ("AM", "PM"):  # tracked caps
        im = Image.new("L", (PERIOD_SIZE * 3, PERIOD_SIZE * 2), 0)
        d, x = ImageDraw.Draw(im), 2
        for ch in word:
            d.text((x, PERIOD_SIZE * 1.5), ch, font=p, fill=255, anchor="ls")
            x += p.getlength(ch) + 2
        out[word] = ink(im)
    widest = out["lead1"].width + 3 * out["0"].width + out[":"].width + 5 + max(out["AM"].width, out["PM"].width)
    assert widest <= HERO_BUDGET, f"12:00 PM is {widest}px, budget {HERO_BUDGET}px"
    return out


def wrap(text, f, width):
    lines, cur = [], ""
    for word in text.split():
        trial = f"{cur} {word}".strip()
        if f.getlength(trial) <= width or not cur:
            cur = trial
        else:
            lines.append(cur)
            cur = word
    return lines + [cur]


def italic(size):
    return font("Newsreader-Italic.ttf", size, opsz=min(size, 72), wght=480)


def byline():
    return font("Newsreader.ttf", 13, opsz=12, wght=500)


def glyphs(f, size):
    """Each character's ink, its offset from the pen on the baseline, and its advance in 1/16 px."""
    out = []
    for cp in CHARSET:
        im = Image.new("L", (size * 3, size * 3), 0)
        ImageDraw.Draw(im).text((size, size * 2), chr(cp), font=f, fill=255, anchor="ls")
        bw = im.point(lambda p: 255 if p >= 128 else 0).convert("1")
        box = bw.getbbox()
        out.append((cp, bw.crop(box) if box else None, (box[0] - size, box[1] - size * 2) if box else (0, 0),
                    round(f.getlength(chr(cp)) * 16)))
    return out


def quote_bitmap(text, author):
    by = byline()
    for size in QUOTE_SIZES:
        f = italic(size)
        quoted = f"“{text}”"
        lines = wrap(quoted, f, QUOTE_W)
        # Narrow the measure while the line count holds, so lines balance instead of leaving a lone word.
        width = QUOTE_W
        while len(lines) > 1 and len(wrap(quoted, f, width - 4)) == len(lines):
            width -= 4
        lines = wrap(quoted, f, width)
        lead = round(size * 1.2)
        height = len(lines) * lead + 20
        if len(lines) <= 3 and height <= QUOTE_MAX_H:
            break
    else:
        sys.exit(f"Quote too long for three lines: {text[:50]}...")
    im = Image.new("L", (QUOTE_W, height + 10), 0)
    d = ImageDraw.Draw(im)
    for i, line in enumerate(lines):
        d.text((QUOTE_W / 2, size + i * lead), line, font=f, fill=255, anchor="ms")
    d.text((QUOTE_W / 2, size + len(lines) * lead + 1), f"— {author}", font=by, fill=255, anchor="ms")
    return ink(im)


def main():
    quotes = [tuple(p.strip() for p in line.split("|")) for line in (HERE / "quotes.txt").read_text().splitlines()
              if line.strip() and not line.startswith("#")]
    hero = hero_glyphs()
    qbits = [quote_bitmap(t, a) for t, a in quotes]

    lines = ["// Generated by tools/make_assets.py from tools/quotes.txt. Don't edit by hand.", "#pragma once",
             "#include <stdint.h>", "", "struct Bitmap { uint16_t w, h; const uint8_t *bits; };",
             "struct Glyph { uint16_t code; uint8_t w, h; int8_t x, y; uint16_t adv, off; };  // adv in 1/16 px",
             "struct Font { const Glyph *glyphs; uint16_t count; const uint8_t *bits; uint8_t size; };", ""]

    def emit(name, bw):
        data = xbm(bw)
        rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
        lines.append(f"static const uint8_t {name}[] = {{\n  " + ",\n  ".join(rows) + "};")
        return f"{{{bw.width}, {bw.height}, {name}}}"

    digits = [emit(f"HERO_{c}", hero[c]) for c in "0123456789"]
    colon, dash = emit("HERO_COLON_BITS", hero[":"]), emit("HERO_DASH_BITS", hero["-"])
    lead1, am, pm = emit("HERO_LEAD1_BITS", hero["lead1"]), emit("HERO_AM_BITS", hero["AM"]), emit("HERO_PM_BITS", hero["PM"])
    qs = [emit(f"QUOTE_{i}", b) for i, b in enumerate(qbits)]

    def emit_font(name, f, size):
        data, table = [], []
        for cp, bw, (x, y), adv in glyphs(f, size):
            bits = xbm(bw) if bw else []
            w, h = bw.size if bw else (0, 0)
            table.append(f"{{{cp}, {w}, {h}, {x}, {y}, {adv}, {len(data)}}}")
            data += bits
        assert len(data) < 65536, name
        rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 24]) for i in range(0, len(data), 24)]
        lines.append(f"static const uint8_t {name}_BITS[] = {{\n  " + ",\n  ".join(rows) + "};")
        lines.append(f"static const Glyph {name}_GLYPHS[] = {{\n  " + ",\n  ".join(table) + "};")
        return f"{{{name}_GLYPHS, {len(table)}, {name}_BITS, {size}}}"

    quote_fonts = [emit_font(f"QFONT_{size}", italic(size), size) for size in QUOTE_SIZES]
    byline_font = emit_font("QFONT_BYLINE", byline(), 13)
    lines += ["", f"static const Bitmap HERO_DIGITS[10] = {{{', '.join(digits)}}};",
              f"static const Bitmap HERO_COLON = {colon};", f"static const Bitmap HERO_DASH = {dash};",
              f"static const Bitmap HERO_LEAD1 = {lead1};", f"static const Bitmap HERO_AM = {am};",
              f"static const Bitmap HERO_PM = {pm};",
              f"static const Bitmap QUOTES[] = {{\n  " + ",\n  ".join(qs) + "};",
              f"static const int QUOTE_COUNT = {len(qs)};",
              f"static const Font QUOTE_FONTS[] = {{{', '.join(quote_fonts)}}};  // largest first",
              f"static const Font BYLINE_FONT = {byline_font};", ""]
    OUT.write_text("\n".join(lines))
    print(f"wrote {OUT.name}: {len(qs)} quotes, digits {hero['0'].size}, colon {hero[':'].size}, "
          f"lead 1 {hero['lead1'].size}, PM {hero['PM'].size}")

    if len(sys.argv) > 1:  # contact sheet of every quote, for eyeballing
        sheet = Image.new("1", (QUOTE_W + 20, sum(b.height + 12 for b in qbits) + 10), 1)
        y = 10
        for b in qbits:
            sheet.paste(Image.eval(b, lambda p: 255 - p), (10 + (QUOTE_W - b.width) // 2, y))
            y += b.height + 12
        sheet.save(sys.argv[1])


if __name__ == "__main__":
    main()
