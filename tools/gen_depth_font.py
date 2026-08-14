# gen_depth_font.py — generate an LVGL v8 font C file (4bpp, uncompressed)
# containing only the glyphs the Depth screen needs, at a large pixel size.
#
# The emitter lives in tools/lvgl_font.py and is shared with
# gen_latin_supplement.py.
#
# Usage: python gen_depth_font.py <ttf> <size_px> <out.c> <font_symbol>
import sys
from lvgl_font import emit_c

# Glyphs needed by DepthScreen: space, '-', '.', digits, unit letters m/f/t
CHARS = [' ', '-', '.', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'f', 'm', 't']


def main():
    ttf, size, out, symbol = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]

    src, stats = emit_c(ttf, size, CHARS, symbol, "gen_depth_font.py")
    with open(out, "w", encoding="utf-8") as f:
        f.write(src)

    print("Wrote %s" % out)
    print("  glyphs=%(glyphs)d  bitmap_bytes=%(bitmap_bytes)d  line_height=%(line_height)d  "
          "base_line=%(base_line)d  ascender=%(ascender)d descender=%(descender)d" % stats)


main()
