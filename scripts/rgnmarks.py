import sys
t=open(sys.argv[1]).read().split("\n")
i=[k for k,l in enumerate(t) if l.startswith("DUMP rgn size")][0]
hx=""
for l in t[i+1:]:
    if l.startswith("DUMP"): break
    hx+=l.strip()
b=bytes.fromhex(hx)
n=len(b)//5
flags={}
for k in range(n):
    f=b[k*5]
    if f & 0x04: flags.setdefault("last(0x04)",[]).append(k)
nz=max([k for k in range(n) if b[k*5:k*5+5]!=bytes(5)]+[-1])
print("headers written:", nz+1, "markers:", flags)
