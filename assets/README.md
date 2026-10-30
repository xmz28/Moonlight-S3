# Moonlight UI bitmap font

© 2014-2021 Adobe (http://www.adobe.com/).

Raster derivative of Noto Sans CJK SC Regular, licensed under SIL OFL 1.1.
The derivative is named Moonlight UI. See ../licenses/NotoSansCJK-OFL.txt.
Source: https://github.com/notofonts/noto-cjk/blob/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf

`ui_font.bin` stores 21,439 glyphs with 2-bit coverage and zero-byte RLE.
ASCII/Latin-1, common CJK ideographs U+4E00..U+9FFF, CJK punctuation and fullwidth ASCII are covered.
Source OTF is cached under ignored `.pio/font-cache/`; generate with `python tools/generate_ui_font.py`.
Bitmap data live in the dedicated 3 MiB ui_font Flash data partition at 0x610000, read one glyph at a time only while drawing idle UI.
They are not embedded into application rodata, because the shared SDK enables PSRAM XIP.
A 16-entry glyph metadata cache and a 1024-byte compressed-glyph scratch buffer are used. No extra full-screen framebuffer or dynamic UI framework is allocated.
PlatformIO build/upload and factory-image creation include this data partition through tools/font_partition.py.
