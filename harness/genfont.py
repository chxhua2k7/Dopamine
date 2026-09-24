#!/usr/bin/env python3
# Converts spleen-8x16.bdf (ASCII 0x20..0x7E) into a C header with one
# 16-byte bitmap per glyph (MSB = leftmost pixel).
import re, sys

bdf = open(sys.argv[1], encoding="utf-8").read().split("\n")
glyphs = {}
i = 0
while i < len(bdf):
    if bdf[i].startswith("STARTCHAR"):
        enc = None
        rows = []
        while not bdf[i].startswith("ENDCHAR"):
            if bdf[i].startswith("ENCODING"):
                enc = int(bdf[i].split()[1])
            if bdf[i].strip() == "BITMAP":
                i += 1
                while not bdf[i].startswith("ENDCHAR"):
                    rows.append(int(bdf[i].strip(), 16))
                    i += 1
                break
            i += 1
        if enc is not None and 0x20 <= enc <= 0x7E:
            assert len(rows) == 16, (enc, len(rows))
            glyphs[enc] = rows
    i += 1

assert len(glyphs) == 95, len(glyphs)

out = []
out.append("// bootlog_font.h - 8x16 bitmap font used by the verbose boot log")
out.append("//")
out.append("// Generated from Spleen 8x16 (https://github.com/fcambus/spleen)")
out.append("// Copyright (c) 2018-2026, Frederic Cambus")
out.append("// Spleen is released under the BSD 2-Clause license:")
out.append("//")
for line in open(sys.argv[2], encoding="utf-8").read().rstrip("\n").split("\n"):
    out.append(("// " + line).rstrip())
out.append("")
out.append("#ifndef BOOTLOG_FONT_H")
out.append("#define BOOTLOG_FONT_H")
out.append("")
out.append("#include <stdint.h>")
out.append("")
out.append("#define BOOTLOG_FONT_WIDTH 8")
out.append("#define BOOTLOG_FONT_HEIGHT 16")
out.append("#define BOOTLOG_FONT_FIRST_CHAR 0x20")
out.append("#define BOOTLOG_FONT_LAST_CHAR 0x7E")
out.append("")
out.append("// One entry per printable ASCII character (0x20..0x7E), 16 rows each,")
out.append("// bit 7 = leftmost pixel.")
out.append("static const uint8_t gBootlogFont[95][16] = {")
for enc in range(0x20, 0x7F):
    rows = glyphs[enc]
    label = chr(enc) if enc != 0x5C else "backslash"
    if enc == 0x2A: label = "asterisk"
    if enc == 0x2F: label = "slash"
    out.append("\t{ " + ", ".join("0x%02X" % r for r in rows) + " }, // 0x%02X '%s'" % (enc, label))
out.append("};")
out.append("")
out.append("#endif // BOOTLOG_FONT_H")
open(sys.argv[3], "w").write("\n".join(out) + "\n")

# quick visual check of a couple of glyphs
for ch in "Da":
    for r in glyphs[ord(ch)]:
        print("".join("#" if r & (0x80 >> b) else "." for b in range(8)))
    print()
