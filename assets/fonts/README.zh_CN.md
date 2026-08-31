<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 字库资源（Fonts）

本目录存放项目可复用的字库资源。每个字库子目录或单个字库文件，应附说明。

- 字库文件复制到本目录，并在本 `README.md` 记录字名、字号、支持字符集与版权信息。
- 字库占用 Flash 与内存，需在集成前评估 ESP32-C3 无 PSRAM 的限制。
- 不提交许可协议禁止再分发的字库。

## 索引

| 资产 | 用途 | 使用方 |
| --- | --- | --- |
| `charset_gb2312_l1.txt` | 字符集：ASCII + GB2312 一级汉字(3755) + 全角标点(A1–A3) | 字体再生成 |
| `NotoSansSC-16.subset.ttf` | 用于生成内置字体的 Noto Sans CJK SC 子集 | 字体再生成 |
| `source/`(git 忽略) | 下载的原始 `NotoSansCJKsc-Regular.otf`(16 MB,OFL-1.1);再生成时重新下载 | — |

## 内置字体:`ui_font_cjk_16`

- 字族:Noto Sans CJK SC(思源黑体),Regular,16 px,无 hinting。
- 输出:`main/ui_font_cjk_16.c` / `.h`(LVGL 9 `lv_font_t` `fmt_txt` 格式,
  2 bpp 抗锯齿,RLE 压缩)。压缩光栅约 210 KB Flash;手机推送的日程/Todo 标题用它渲染。
- 字符:ASCII 0x20–0x7E、GB2312 一级汉字(3755)、GB2312 符号区 A1–A3(全角形式)。
  缺字回退到所用标签的基础字体(montserrat),反之亦然(渲染期回退链)。
- 来源 / 许可:Google Noto CJK, OFL-1.1
  (`https://github.com/googlefonts/noto-cjk` 的 `Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf`)。
- 再生成(仓库根目录执行,需要 Python 与 Node):

```bash
# 1) 字符集 + 子集字体(fonttools,经 uv)
uv run --with fonttools python tools/build_cjk_font.py \
  --in assets/fonts/source/NotoSansCJKsc-Regular.otf \
  --out assets/fonts/NotoSansSC-16.subset.ttf \
  --chars assets/fonts/charset_gb2312_l1.txt

# 2) LVGL 9 C 字体(lv_font_conv 的 GitHub master 版;Windows 下多字节参数
#    需经包装脚本从文件读取 --symbols)
node <tmp>/run_lfcv.mjs <tmp>/lv_font_conv.js assets/fonts/charset_gb2312_l1.txt \
  --bpp 2 --size 16 --format lvgl \
  --font assets/fonts/NotoSansSC-16.subset.ttf \
  --lv-font-name ui_font_cjk_16 -o main/ui_font_cjk_16.c
```

相关:[软件设计文档](https://github.com/folotoy/ai-passport/blob/main/docs/software-design/passport-sync-app.zh_CN.md)。