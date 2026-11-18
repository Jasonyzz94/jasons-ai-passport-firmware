#!/usr/bin/env python3
"""Regenerate the toolbox's LVGL bitmap fonts from Noto Sans SC.

The fonts are generated once and committed under assets/fonts/. Rerun this
script whenever a UI string introduces a character that the glyph gate
(tools/check_glyphs.py) reports as missing.

Usage:
    python3 tools/gen_fonts.py --regular NotoSansSC-Regular.otf \
        --bold NotoSansSC-Bold.otf [--converter path/to/lv_font_conv]

Source fonts: Noto Sans SC (SIL Open Font License 1.1), from
https://github.com/notofonts/noto-cjk (Sans/SubsetOTF/SC). The OTF files are
not committed; only the generated LVGL sources are.

Converter: lv_font_conv 1.5.3 (npm). Fonts are 4 bpp and uncompressed.

Character inventories:
* toolbox_cn_16 (Regular 16 px, body text): printable ASCII, all 3755 GB2312
  level-1 hanzi (decoded from the GB2312 code table, so the set is exact and
  reproducible), common CJK punctuation/symbols, and every non-ASCII character
  used by a string literal under main/. Network text (AI replies, dashboard
  cards) is rendered with this font; characters outside it show the LVGL
  placeholder box.
* toolbox_cn_14 (Regular 14 px, hints/captions): same inventory as
  toolbox_cn_16, because captions also show network text (city names etc.).
* toolbox_cn_20b (Bold 20 px, titles): printable ASCII + every non-ASCII
  character used by main/ string literals.
* toolbox_num_36b (Bold 36 px): printable ASCII, for medium numbers/labels.
* toolbox_num_64b (Bold 64 px): digits, colon, a few signs and note letters
  A-G/#, for big clocks, counters and the tuner.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "assets" / "fonts"
SRC_DIRS = [ROOT / "main"]

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
EXTRA_SYMBOLS = (
    "，。、；：？！“”‘’（）《》〈〉【】「」『』—…·～￥％＋－×÷＝"
    "°℃←→↑↓●○■□▲△▼▽◆◇★☆♪♫✓✕"
)

STRING_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)


def gb2312_level1() -> str:
    chars = []
    for hi in range(0xB0, 0xD8):
        for lo in range(0xA1, 0xFF):
            if hi == 0xD7 and lo > 0xF9:
                break
            chars.append(bytes([hi, lo]).decode("gb2312"))
    return "".join(chars)


def source_chars() -> str:
    found: set[str] = set()
    for base in SRC_DIRS:
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".c", ".h"}:
                continue
            text = COMMENT_RE.sub("", path.read_text(encoding="utf-8"))
            for literal in STRING_RE.findall(text):
                decoded = re.sub(
                    r"\\u([0-9a-fA-F]{4})", lambda m: chr(int(m.group(1), 16)), literal
                )
                found.update(ch for ch in decoded if ord(ch) >= 0x80)
    return "".join(sorted(found))


def run(converter: list[str], font: Path, size: int, name: str, symbols: str) -> None:
    out = OUT_DIR / f"{name}.c"
    cmd = converter + [
        "--font", str(font),
        "--symbols", symbols,
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "--no-compress",
        "--lv-font-name", name,
        "--lv-include", "lvgl.h",
        "--output", str(out),
    ]
    subprocess.run(cmd, check=True)
    print(f"{name}: {len(set(symbols))} requested -> {out.relative_to(ROOT)}")


def unique(text: str) -> str:
    seen: dict[str, None] = {}
    for ch in text:
        seen.setdefault(ch, None)
    return "".join(seen)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--regular", type=Path, required=True)
    parser.add_argument("--bold", type=Path, required=True)
    parser.add_argument("--converter", default=None,
                        help="lv_font_conv executable (default: lv_font_conv on PATH)")
    args = parser.parse_args()

    exe = args.converter or shutil.which("lv_font_conv")
    if not exe:
        print("lv_font_conv not found; install lv_font_conv@1.5.3 or pass --converter",
              file=sys.stderr)
        return 2
    converter = [exe]
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    ui = source_chars()
    run(converter, args.regular, 16, "toolbox_cn_16",
        unique(ASCII + EXTRA_SYMBOLS + gb2312_level1() + ui))
    run(converter, args.regular, 14, "toolbox_cn_14",
        unique(ASCII + "▲▼" + EXTRA_SYMBOLS + gb2312_level1() + ui))
    run(converter, args.bold, 20, "toolbox_cn_20b", unique(ASCII + "°℃·…—" + ui))
    run(converter, args.bold, 36, "toolbox_num_36b", unique(ASCII + "°℃·"))
    run(converter, args.bold, 64, "toolbox_num_64b", "0123456789:-.% ABCDEFG#")
    return 0


if __name__ == "__main__":
    sys.exit(main())
