# PlatformIO pre-build script: 4-bit smooth fonts in TFT_eSPI.
#
# Our VLW fonts (tools/gen_vlw.py, header version 12) keep glyph bitmaps at 4 bits per pixel, two
# pixels per byte, high nibble first, each glyph padded to a whole byte: half the flash of the
# stock 8-bit VLW. TFT_eSPI reads only 8-bit bitmaps, so this patches its installed copy in place
# (idempotent; a missing anchor fails the build instead of shipping garbled text). All fonts of
# this build must be 4-bit: fonts from the file system are not used.
Import("env")  # noqa: F821
import os

MARK = "UASB_VLW4"
lib = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"), "TFT_eSPI", "Extensions")  # noqa: F821

READ8 = "pixel = pgm_read_byte(gPtr + gBitmap[gNum] + x + gWidth[gNum] * y);"
READ4 = ("{ uint32_t i = x + gWidth[gNum] * y; uint8_t b = pgm_read_byte(gPtr + gBitmap[gNum] + (i >> 1));  // "
         + MARK + "\n          pixel = ((i & 1) ? (b & 0x0F) : (b >> 4)) * 17; }")

PATCHES = {
    "Smooth_font.cpp": [
        ("bitmapPtr += gWidth[gNum] * gHeight[gNum];",
         "bitmapPtr += (gWidth[gNum] * gHeight[gNum] + 1) / 2;  // " + MARK),
        (READ8, READ4),
    ],
    "Sprite.cpp": [(READ8, READ4)],
}

for name, patches in PATCHES.items():
    path = os.path.join(lib, name)
    src = open(path, encoding="utf-8").read()
    if MARK in src:
        continue
    for old, new in patches:
        if src.count(old) != 1:
            raise SystemExit("tft_patch.py: anchor not found once in %s: %s" % (path, old))
        src = src.replace(old, new)
    open(path, "w", encoding="utf-8").write(src)
    print("tft_patch.py: 4-bit fonts patched into", name)
