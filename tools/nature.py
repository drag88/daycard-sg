#!/usr/bin/env python3
"""Fetch the hand-picked nature photos (Unsplash photos via Lorem Picsum, Unsplash License) as 400x300
baseline JPEGs, named "<id> - <photographer>.jpg" so the frame can credit them. Then copy them to the board:

  python3 tools/nature.py out_dir && python3 tools/upload.py out_dir/*.jpg

Picsum and Unsplash only serve progressive JPEGs, which the ESP32-S3's ROM decoder can't read, hence the
re-encode here rather than downloading on the board.
"""
import io
import json
import sys
import unicodedata
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PIL import Image

# Picked by eye from the whole Picsum catalogue: landscapes, water, forests, sky, a few animals. No cities or people.
NATURE = [
    10, 11, 12, 13, 15, 16, 17, 28, 29, 46, 54, 62, 66, 70, 71, 77, 81, 83, 110,
    116, 118, 121, 124, 128, 136, 152, 162, 166, 168, 179, 186, 189, 190, 191, 196, 200, 218, 219, 227,
    229, 231, 235, 243, 244, 247, 254, 255, 256, 271, 272, 287, 289, 295, 296, 306, 323, 327, 335,
    350, 353, 368, 374, 377, 381, 393, 404, 406, 412, 416, 423, 426, 433, 434, 440, 443,
    450, 456, 461, 465, 466, 468, 471, 472, 474, 477, 482, 483, 484, 498, 502, 506, 508, 509, 511, 516, 519,
    523, 538, 543, 547, 558, 559, 562,
    564, 565, 566, 568, 571, 573, 574, 575, 577, 582, 599, 603, 606, 612, 614, 626, 634, 640, 645, 648, 651,
    658, 664, 666, 676, 678, 684, 685,
    688, 698, 699, 701, 702, 704, 715, 718, 730, 731, 733, 741, 767, 773, 779, 780, 783, 787, 790, 807, 809, 830,
    844, 846, 849, 851, 853, 866, 871, 872, 876, 877, 878, 894, 902, 903, 905, 906, 907, 908, 909, 910, 913,
    916, 919, 921, 923, 924, 927, 928, 929, 931, 932, 937, 938, 943, 944, 957,
    958, 961, 964, 969, 973, 974, 976, 977, 982, 984, 986, 987, 989, 1015, 1016, 1018, 1019, 1020, 1021, 1022,
    1024, 1028, 1035, 1036, 1039, 1041, 1043, 1044, 1045, 1049, 1051, 1052, 1053, 1057, 1061, 1064,
    1074, 1084,
]


def ascii_name(s):
    return "".join(c for c in unicodedata.normalize("NFKD", s) if c.isascii() and (c.isalnum() or c in " .-'"))


def main(out):
    out.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen("https://picsum.photos/v2/list?page=1&limit=100", timeout=30):
        pass  # fail fast if Picsum is down
    authors = {}
    for page in range(1, 12):
        with urllib.request.urlopen(f"https://picsum.photos/v2/list?page={page}&limit=100", timeout=30) as r:
            authors.update({int(i["id"]): i["author"] for i in json.load(r)})

    def fetch(pid):
        with urllib.request.urlopen(f"https://picsum.photos/id/{pid}/400/300", timeout=60) as r:
            img = Image.open(io.BytesIO(r.read())).convert("RGB")
        name = out / f"{pid} - {ascii_name(authors.get(pid, 'Unsplash'))}.jpg"
        img.save(name, "JPEG", quality=88, progressive=False, optimize=True)  # baseline, for the ROM decoder
        return name

    with ThreadPoolExecutor(8) as ex:
        files = list(ex.map(fetch, NATURE))
    print(f"{len(files)} photos in {out}")


if __name__ == "__main__":
    main(Path(sys.argv[1] if len(sys.argv) > 1 else "nature"))
