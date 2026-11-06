#!/usr/bin/env python3
"""
gen_font.py - 8x16 像素终端字体位图生成器

从 Terminus (ter-u16n) PCF 位图字体提取 256 字符的 8x16 单色位图，
生成 kernel/font.c 兼容数组（256 字符 × 16 字节 = 4096 字节）。

使用方式：
  python3 script/gen_font.py

Terminus 字体作者 Dimitar Toshkov Zhekov 已声明 fontdata 为 Public Domain。
本项目使用 xfonts-terminus 包中的 ter-u16n（Unicode 编码，normal 字重）。
"""

from PIL import ImageFont, ImageDraw, Image
import os

FONT_PATH = "/usr/share/fonts/X11/misc/ter-u16n_unicode.pcf.gz"
CELL_W, CELL_H = 8, 16


def render_char(code_point, font):
    """将 Unicode code point 渲染为 8x16 单色位图，返回 16 字节数组。"""
    # Terminus PCF 是位图字体，render 到 8x16 画布就是精确位图
    img = Image.new('L', (CELL_W, CELL_H), 0)
    draw = ImageDraw.Draw(img)
    draw.text((0, 0), chr(code_point), font=font, fill=255)
    # 不需要阈值降采样——位图字体本身就是精确像素
    result = []
    for y in range(CELL_H):
        byte = 0
        for x in range(CELL_W):
            if img.getpixel((x, y)) > 128:
                byte |= (1 << (7 - x))
        result.append(byte)
    return result


def char_pixels_to_ascii(pixels):
    lines = []
    for byte in pixels:
        line = ''.join('@' if byte & (1 << (7 - c)) else '.' for c in range(8))
        lines.append(line)
    return '\n'.join(lines)


def main():
    font = ImageFont.truetype(FONT_PATH, 16)

    def is_printable(cp):
        if 0x20 <= cp <= 0x7E:
            return True
        # ISO-8859-1 printable range
        if 0xA0 <= cp <= 0xFF:
            try:
                return chr(cp).isprintable()
            except:
                pass
        return False

    header = """/*
 * font.c - 终端字体位图数据
 *
 * 8x16 像素的 ASCII 字符位图，用于 framebuffer 渲染。
 * 由 script/gen_font.py 从 Terminus 16px 位图字体提取。
 *
 * Terminus fontdata is Public Domain.
 * Source: ter-u16n (xfonts-terminus), Dimitar Toshkov Zhekov.
 * https://terminus-font.sourceforge.net/
 */

#include "font.h"

const unsigned char fontdata_8x16[4096] = {
"""

    footer = """};

"""
    lines = [header]

    for cp in range(256):
        if is_printable(cp):
            pixels = render_char(cp, font)
        else:
            pixels = [0] * CELL_H

        ch = chr(cp) if 0x20 <= cp <= 0x7E else '?'
        comment = f"\t/* {cp} 0x{cp:02X} '{ch}' */\n"
        lines.append(comment)

        for byte in pixels:
            binary = format(byte, '08b')
            lines.append(f"\t0x{byte:02x}, /* {binary} */\n")

    lines.append(footer)

    out_path = os.path.normpath(os.path.join(
        os.path.dirname(__file__), '..', 'kernel', 'font.c'
    ))
    with open(out_path, 'w') as f:
        f.writelines(lines)

    # 打印对比
    for ch in "AMW0gq":
        pixels = render_char(ord(ch), font)
        print(f"=== '{ch}' (0x{ord(ch):02X}) ===")
        for byte in pixels:
            line = ''.join('@' if byte & (1 << (7 - c)) else '.' for c in range(8))
            print(f"  0x{byte:02x}  {line}")
        print()

    total_data = sum(1 for l in lines if l.strip().startswith('0x'))
    print(f"Generated {out_path}: {total_data} bytes for {256} characters")


if __name__ == '__main__':
    main()
