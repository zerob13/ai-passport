<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Fonts

Store reusable font files and generated font sources here.

- Use descriptive names that include the family, weight, size, and format when relevant.
- Document the source, license, character range, conversion command, and expected destination.
- Check Flash and internal-RAM impact before adding a font; the ESP32-C3 has no PSRAM.
- Do not commit fonts whose license does not permit redistribution.

## Index

| Asset | Purpose | Used by |
| --- | --- | --- |
| `charset_gb2312_l1.txt` | Character set: ASCII + GB2312 level-1 hanzi (3755) + full-width punctuation (A1–A3) | Font regeneration |
| `NotoSansSC-16.subset.ttf` | Subset of Noto Sans CJK SC used to generate the embedded font | Font regeneration |
| `source/` (git-ignored) | Original downloaded `NotoSansCJKsc-Regular.otf` (16 MB, OFL-1.1); re-download when regenerating | — |

## Embedded font: `ui_font_cjk_16`

- Family: Noto Sans CJK SC (Source Han Sans), Regular, 16 px, no hinting.
- Output: `main/ui_font_cjk_16.c` / `.h` (LVGL 9 `lv_font_t` `fmt_txt` format, 2 bpp
  anti-aliasing, RLE compressed). Compressed glyph raster ≈ 210 KB flash; the
  screen renders titles from the phone app (schedule / todo) with it.
- Characters: ASCII 0x20–0x7E, GB2312 level-1 hanzi (3755), GB2312
  punctuation/symbol rows A1–A3 (full-width forms). Missing glyphs fall back to
  the label's base font (montserrat), and vice versa at rendering time.
- Source / license: Google Noto CJK fonts, OFL-1.1
  (`https://github.com/googlefonts/noto-cjk`, `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`).
- Regeneration (from repo root, requires Python + Node):

```bash
# 1) charset + subset font (fonttools via uv)
uv run --with fonttools python tools/build_cjk_font.py \
  --in assets/fonts/source/NotoSansCJKsc-Regular.otf \
  --out assets/fonts/NotoSansSC-16.subset.ttf \
  --chars assets/fonts/charset_gb2312_l1.txt

# 2) LVGL 9 C font (lv_font_conv from GitHub master; pass symbols from the
#    charset file via a wrapper script so multi-byte args survive on Windows)
node <tmp>/run_lfcv.mjs <tmp>/lv_font_conv.js assets/fonts/charset_gb2312_l1.txt \
  --bpp 2 --size 16 --format lvgl \
  --font assets/fonts/NotoSansSC-16.subset.ttf \
  --lv-font-name ui_font_cjk_16 -o main/ui_font_cjk_16.c
```

Related: [software design](https://github.com/folotoy/ai-passport/blob/main/docs/software-design/passport-sync-app.md).