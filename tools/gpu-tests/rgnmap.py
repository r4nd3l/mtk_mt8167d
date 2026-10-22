# Print the 16x16 tile map of region-header byte0 (and full 5 bytes for non-trivial ones).
# Layout: idx = 16*(4*(ty//4)+tx//4) + 4*(ty%4) + tx%4  (TE 4x4-tile blocks, 16 tiles wide)
import sys
t = open(sys.argv[1]).read().split("\n")
i = [k for k, l in enumerate(t) if l.startswith("DUMP rgn size")][0]
hx = ""
for l in t[i + 1:]:
    if l.startswith("DUMP"): break
    hx += l.strip()
b = bytes.fromhex(hx)
N = int(sys.argv[2]) if len(sys.argv) > 2 else 16
S = int(sys.argv[3]) if len(sys.argv) > 3 else N  # dataset tiles per row
for ty in range(N):
    row = []
    for tx in range(N):
        k = 16 * ((S // 4) * (ty // 4) + tx // 4) + 4 * (ty % 4) + tx % 4
        row.append("%02x" % b[k * 5])
    print(" ".join(row))
print("distinct headers:", sorted({b[k*5:k*5+5].hex() for k in range(N*N)})[:20])
