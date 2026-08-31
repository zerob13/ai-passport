#!/usr/bin/env python3
"""Build the embedded CJK font pipeline for the AI Passport app.

Steps (run from repo root):
  1. Generate a charset text file (GB2312 level-1 hanzi + ASCII + fullwidth
     punctuation) from an input CJK TTF/OTF.
  2. Subset the font to that charset with fonttools (pyftsubset).
  3. (manual, Node needed) convert the subset to an LVGL 9 C font with
     lv_font_conv (see the printed command).

Usage:
    uv run --with fonttools python tools/build_cjk_font.py \
        --in assets/fonts/source/NotoSansCJKsc-Regular.otf \
        --out assets/fonts/NotoSansSC-16.subset.ttf \
        --chars assets/fonts/charset_gb2312_l1.txt
"""

import argparse
import sys
from pathlib import Path


def gb2312_hanzi_l1() -> list[str]:
    """Decode the GB2312 level-1 hanzi block (rows B0..D7, 3755 chars)."""
    chars: list[str] = []
    # 每个汉字区: 区号(0xB0-0xD7) × 位号(0xA1-0xFE),行内最后一个位号不连续
    for row in range(0xB0, 0xD8):
        for cell in range(0xA1, 0xFF):
            b = bytes([row, cell])
            if row == 0xD7 and cell > 0xF9:
                break  # 一级汉字到 D7F9 结束
            try:
                chars.append(b.decode("gb2312"))
            except UnicodeDecodeError:
                pass
    if len(chars) != 3755:
        raise RuntimeError(f"unexpected GB2312 L1 count: {len(chars)}")
    return chars


def gb2312_punct_rows() -> list[str]:
    """Fullwidth punctuation / symbols rows A1..A3 (per GB2312)."""
    chars: list[str] = []
    for row in range(0xA1, 0xA4):
        for cell in range(0xA1, 0xFF):
            b = bytes([row, cell])
            try:
                chars.append(b.decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return chars


def build_charset() -> list[str]:
    ascii_chars = [chr(c) for c in range(0x20, 0x7F)]  # 0x20..0x7E
    result = list(dict.fromkeys(ascii_chars + gb2312_punct_rows() + gb2312_hanzi_l1()))
    result.sort()
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--in", dest="src", help="input CJK font (otf/ttf)")
    ap.add_argument("--out", help="output subset font (ttf)")
    ap.add_argument("--chars", help="output charset text file path")
    args = ap.parse_args()
    if not args.chars and not args.out:
        ap.error("at least one of --chars / --out is required")

    charset = build_charset()
    text = "".join(charset)

    if args.chars:
        Path(args.chars).write_text(text, encoding="utf-8")
        print(f"charset: {len(charset)} chars -> {args.chars}")

    if args.out:
        if not args.src:
            ap.error("--in is required for --out")
        from fontTools import subset  # noqa: PLC0415
        opts = subset.Options()
        opts.text = text
        opts.layout_features = ["*"]
        opts.notdef_outline = True
        opts.recommended_glyphs = False
        opts.name_IDs = ["*"]
        opts.name_legacy = False
        opts.name_languages = ["*"]
        font = subset.load_font(args.src, opts)
        subsetter = subset.Subsetter(options=opts)
        subsetter.populate(text=text)
        subsetter.subset(font)
        subset.save_font(font, args.out, opts)
        print(f"subset font -> {args.out}")

    print(
        "next: npx lv_font_conv --no-prefilter --bpp 2 --size 16 "
        "--format lvgl --font <subset.ttf> --symbols-file <charset.txt> "
        "-o main/fonts/ui_font_cjk_16.c"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())