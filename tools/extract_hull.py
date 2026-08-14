# Extract a hull deck-plan outline (transparent PNG, black contour) into the
# WindScreen ST[][2] station table {y, half-beam} in BS units, bow->stern.
# The image is PORTRAIT: the hull's long axis is vertical. We scan each ROW for
# the left/right contour edges -> half-beam(y); the pointed end is the bow.
import os
from PIL import Image

PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "bootskontur_transparent.png")
img = Image.open(PATH).convert("RGBA")
W, H = img.size
px = img.load()

def outline(x, y):
    return px[x, y][3] > 100  # non-transparent = drawn contour

rows = {}
for y in range(H):
    xsr = [x for x in range(W) if outline(x, y)]
    if xsr:
        rows[y] = (min(xsr), max(xsr))

ys = sorted(rows)
y_lo, y_hi = ys[0], ys[-1]
length = y_hi - y_lo

def hb(y):
    xmin, xmax = rows[y]
    return (xmax - xmin) / 2.0

# Bow = pointed end: smaller half-beam a few % in from the tip.
hb_top = hb(min(rows, key=lambda y: abs(y - (y_lo + 0.04 * length))))
hb_bot = hb(min(rows, key=lambda y: abs(y - (y_hi - 0.04 * length))))
bow_top = hb_top < hb_bot   # True -> bow at the top (y_lo)

N = 18
prof = []
for i in range(N):
    t = i / (N - 1)                          # 0 = bow, 1 = stern
    yf = (y_lo + t * length) if bow_top else (y_hi - t * length)
    y = min(rows, key=lambda c: abs(c - yf))
    prof.append((t, hb(y)))

max_hb = max(h for _, h in prof)
aspect = length / (2 * max_hb)

print(f"// img {W}x{H}  length_px={length}  maxHB_px={max_hb:.0f}  aspect(L:B)={aspect:.2f}  bow_top={bow_top}")
print(f"// tip hb: top~{hb_top:.0f}px  bottom~{hb_bot:.0f}px")

LEN = 64.0                          # BS units (half-len 32*3.8=122px < R_INNER 138)
sb = (LEN / aspect / 2) / max_hb    # px -> BS for the half-beam
print(f"// scale LEN={LEN}BS  maxHB={max_hb*sb:.2f}BS")
print(f"    static const float ST[{N}][2] = {{")
line = "        "
for i, (t, h) in enumerate(prof):
    line += f"{{{(-LEN/2 + t*LEN):6.1f}f, {h*sb:5.2f}f}}, "
    if i % 3 == 2:
        print(line.rstrip()); line = "        "
if line.strip():
    print(line.rstrip())
print("    };")
print(f"    // -> ST[{N}], hp[{2*N}], loops 0..{N}")
