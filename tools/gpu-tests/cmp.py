#!/usr/bin/env python3
# Diff two PPM frames: python3 cmp.py gpu.ppm ref.ppm [diff.png]
# Prints the share of pixels whose max channel difference is > 2 and writes a
# diff image (white = mismatch) so the location of a bug is visible.
import sys
from PIL import Image, ImageChops

a, b = (Image.open(p).convert("RGB") for p in sys.argv[1:3])
assert a.size == b.size, (a.size, b.size)
d = ImageChops.difference(a, b)
bad = sum(1 for px in d.getdata() if max(px) > 2)
n = a.size[0] * a.size[1]
print(f"mismatching pixels: {bad}/{n} ({100.0 * bad / n:.2f}%)")
if len(sys.argv) > 3:
    d.point(lambda v: 255 if v > 2 else 0).save(sys.argv[3])
sys.exit(1 if bad > n // 1000 else 0)
