#!/usr/bin/env python3
# Rewrite pmos_boot_uuid/pmos_root_uuid in an Android v0 boot.img cmdline and
# recompute the header id (SHA1), e.g. for a boot.img built in the PC's rootfs
# chroot, whose UUIDs are not the tablet's.
# usage: bootimg-set-uuids.py IN OUT BOOT_UUID ROOT_UUID
import hashlib, re, struct, sys

src, dst, boot_uuid, root_uuid = sys.argv[1:5]
b = bytearray(open(src, "rb").read())
assert b[:8] == b"ANDROID!"
ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page = struct.unpack_from("<8I", b, 8)
cmd = bytes(b[64:64 + 512])
new = re.sub(rb"pmos_boot_uuid=[0-9a-f-]{36}", b"pmos_boot_uuid=" + boot_uuid.encode(), cmd)
new = re.sub(rb"pmos_root_uuid=[0-9a-f-]{36}", b"pmos_root_uuid=" + root_uuid.encode(), new)
assert len(new) == 512 and new != cmd, "uuids not found or unchanged"
b[64:64 + 512] = new
extra = bytes(b[608:608 + 1024]).rstrip(b"\0")
assert b"uuid" not in extra, "extra cmdline also has uuids"
def blob(off, size):
    return bytes(b[off:off + size])
koff = page
roff = koff + (ksz + page - 1) // page * page
soff = roff + (rsz + page - 1) // page * page
h = hashlib.sha1()
for off, size in ((koff, ksz), (roff, rsz), (soff, ssz)):
    h.update(blob(off, size)); h.update(struct.pack("<I", size))
b[576:576 + 32] = h.digest().ljust(32, b"\0")
open(dst, "wb").write(b)
print(new.rstrip(b"\0").decode())
