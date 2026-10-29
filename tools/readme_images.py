#!/usr/bin/env python3
"""Build the README images in docs/images from real framebuffer captures (tools/grab.py PNGs).

  python3 tools/readme_images.py <captures-dir>

The folder holds card, buses, morning, driveby, room, forecast, photos, focus, setup and scene-0 ... scene-9 (one per
weather scene, in the order the serial command w steps through them, sun first). Capture them with the example
config.h so nobody's own stop or area ends up in the README; CONTRIBUTING.md has the commands. Needs Pillow.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

SRC = sys.argv[1]
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "images")
FONTS = os.path.join(os.path.dirname(__file__), "fonts")
PAPER, INK = (226, 224, 216), (30, 32, 34)  # the reflective panel's silver and its ink
THEMES = {
    "light": {"bg": (244, 241, 234), "title": (28, 28, 26), "body": (92, 88, 80), "bezel": (24, 25, 28)},
    "dark": {"bg": (17, 18, 20), "title": (236, 233, 225), "body": (160, 156, 148), "bezel": (44, 46, 50)},
}
SCENES = ["Sun", "Moon", "Sun and cloud", "Moon and cloud", "Cloudy", "Rain", "Heavy rain", "Thunder", "Haze", "Wind"]


def font(name, size):
    return ImageFont.truetype(os.path.join(FONTS, name), size)


def capture(name):
    return Image.open(os.path.join(SRC, name + ".png")).convert("L")  # 255 paper, 0 ink


def screen(name, scale=2, bits=None):
    """A capture as the panel looks: silver paper, dark ink, crisp pixels."""
    bits = bits or capture(name)
    rgb = Image.merge("RGB", [bits.point(lambda v, p=p, i=i: p if v else i) for p, i in zip(PAPER, INK)])
    return rgb.resize((rgb.width * scale, rgb.height * scale), Image.NEAREST)


def rounded(im, radius, border=None):
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, im.width - 1, im.height - 1), radius, fill=255)
    out = im.convert("RGBA")
    out.putalpha(mask)
    if border:
        ImageDraw.Draw(out).rounded_rectangle((0, 0, im.width - 1, im.height - 1), radius, outline=border)
    return out


def device(name, theme):
    """The screen inside a plain bezel, roughly the board's proportions."""
    shot = screen(name)
    pad = 34
    bezel = Image.new("RGBA", (shot.width + 2 * pad, shot.height + 2 * pad), (0, 0, 0, 0))
    ImageDraw.Draw(bezel).rounded_rectangle((0, 0, bezel.width - 1, bezel.height - 1), 30, fill=theme["bezel"])
    bezel.alpha_composite(rounded(shot, 6), (pad, pad))
    return bezel


def hero(mode):
    t = THEMES[mode]
    im = Image.new("RGBA", (1800, 860), t["bg"])
    d = ImageDraw.Draw(im)
    dev = device("card", t)
    im.alpha_composite(dev, (im.width - dev.width - 90, (im.height - dev.height) // 2))
    x = 96
    d.text((x, 214), "DESK CARD FIRMWARE  ·  ESP32-S3", font=font("Newsreader.ttf", 24), fill=t["body"])
    d.text((x - 6, 250), "daycard", font=font("DMSerifDisplay-Regular.ttf", 168), fill=t["title"])
    tagline = font("Newsreader-Italic.ttf", 40)
    for i, line in enumerate(["The time, the weather and your next bus,", "on a reflective screen that reads like paper."]):
        d.text((x, 470 + i * 52), line, font=tagline, fill=t["title"])
    d.text((x, 600), "Singapore buses and weather  ·  offline voice  ·  MIT", font=font("Newsreader.ttf", 26),
           fill=t["body"])
    im.convert("RGB").save(os.path.join(OUT, f"hero-{mode}.png"), optimize=True)


def tiles():
    for name in ["card", "buses", "morning", "driveby", "room", "forecast", "photos", "focus", "setup"]:
        rounded(screen(name), 14, border=(190, 187, 178)).save(os.path.join(OUT, name + ".png"), optimize=True)
    rounded(screen("scene-5"), 14, border=(190, 187, 178)).save(os.path.join(OUT, "rain.png"), optimize=True)


def views_gif():
    names = ["buses", "morning", "driveby", "room", "forecast", "scene-5", "photos", "focus", "card"]  # the hero already shows the card
    frames = [screen(n).convert("P", palette=Image.ADAPTIVE, colors=2) for n in names]
    frames[0].save(os.path.join(OUT, "views.gif"), save_all=True, append_images=frames[1:], duration=1800, loop=0,
                   optimize=True)


def weather_strip():
    """Each scene's weather picture, cropped from the card, with its name."""
    cell, tall, label = 230, 226, font("Newsreader.ttf", 26)
    im = Image.new("RGB", (cell * 5, 2 * (tall + 44)), PAPER)
    d = ImageDraw.Draw(im)
    for i, name in enumerate(SCENES):
        art = screen(f"scene-{i}").crop((568, 46, 798, 46 + tall))  # below the status row, above the world clocks
        x, y = (i % 5) * cell, (i // 5) * (tall + 44)
        im.paste(art, (x, y))
        w = d.textlength(name, font=label)
        d.text((x + (cell - w) / 2, y + tall + 4), name, font=label, fill=INK)
    rounded(im, 14, border=(190, 187, 178)).save(os.path.join(OUT, "weather.png"), optimize=True)


def footer(name, top=232, rule=240):
    """The card's footer. The umbrella rises above the footer rule, so keep that band but erase the quote and the
    rule itself, except where the umbrella crosses it."""
    bits = capture(name).crop((0, top, 400, 300))
    px, r = bits.load(), rule - top
    for x in range(400):
        for y in range(r):
            if x < 330:  # the quote's author line; the cat lives to the right of it
                px[x, y] = 255
        if px[x, r - 1] == 255 and px[x, r + 1] == 255:
            px[x, r] = 255
    return screen(name, bits=bits)


def cat_strip():
    """The cat's footer in four moods: sitting, sheltering, napping, out for a walk."""
    rows = [footer(n) for n in ["card", "scene-5", "scene-8", "scene-9"]]
    h = rows[0].height
    im = Image.new("RGB", (2 * 800 + 4, 2 * h + 4), (190, 187, 178))
    for i, r in enumerate(rows):
        im.paste(r, ((i % 2) * 804, (i // 2) * (h + 4)))
    rounded(im, 14).save(os.path.join(OUT, "cat.png"), optimize=True)


os.makedirs(OUT, exist_ok=True)
hero("light"), hero("dark"), tiles(), views_gif(), weather_strip(), cat_strip()
print("wrote", ", ".join(sorted(os.listdir(OUT))))
