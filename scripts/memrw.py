#!/usr/bin/env python3
# Read/write 32-bit physical registers through /dev/mem (busybox has no devmem).
# usage: memrw.py ADDR [ADDR...]        read
#        memrw.py ADDR=VALUE [...]      write
# ponytail: one page mapping per access, fine for a handful of registers.
import mmap, os, struct, sys

fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
for arg in sys.argv[1:]:
    addr, _, val = arg.partition("=")
    a = int(addr, 0)
    page = a & ~(mmap.PAGESIZE - 1)
    m = mmap.mmap(fd, mmap.PAGESIZE, offset=page)
    off = a - page
    if val:
        m[off:off + 4] = struct.pack("<I", int(val, 0))
    print("%#010x = %#010x" % (a, struct.unpack("<I", m[off:off + 4])[0]))
    m.close()
