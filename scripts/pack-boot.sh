#!/bin/bash
# Pack a boot.img for the Lenovo Tab E7 (TB-7104F, MT8167) from a kernel build tree.
#
# Usage:
#   KBUILD=/path/to/linux/build/dir WORKDIR=/path/to/workdir ./pack-boot.sh [output-name]
#
# Requires `mkbootimg` on PATH (AOSP system/core/mkbootimg — build it from AOSP
# source, or grab a prebuilt from your device's open-source release package).
#
# The 0x78000-offset base/kernel/ramdisk/second/tags values below are required
# for MediaTek 64-bit bootloaders on this SoC — see device/device-lenovo-tabe7/
# deviceinfo for why (naive 0x40000000-style offsets place the DTB at the wrong
# physical address and the kernel silently never finds it).
#
# pmos_boot_uuid / pmos_root_uuid below are PLACEHOLDERS — replace with the
# actual filesystem UUIDs of your boot/root partitions (`blkid`).

set -e
MKBOOTIMG="${MKBOOTIMG:-mkbootimg}"
KBUILD="${KBUILD:?Set KBUILD to your kernel build directory}"
WORKDIR="${WORKDIR:?Set WORKDIR to the directory holding your ramdisk}"
OUT=${1:-$WORKDIR/boot.img}

KERNEL=$KBUILD/arch/arm64/boot/Image.gz
DTB=$KBUILD/arch/arm64/boot/dts/mediatek/mt8167-lenovo-tabe7.dtb
RAMDISK=$WORKDIR/ramdisk-pmos.cpio.gz

echo "Kernel:   $KERNEL ($(wc -c < $KERNEL) bytes)"
echo "DTB:      $DTB ($(wc -c < $DTB) bytes)"
echo "Ramdisk:  $RAMDISK ($(wc -c < $RAMDISK) bytes)"

$MKBOOTIMG \
  --kernel  "$KERNEL" \
  --ramdisk "$RAMDISK" \
  --second  "$DTB" \
  --base          0x40078000 \
  --kernel_offset  0x00008000 \
  --ramdisk_offset 0x14f88000 \
  --second_offset  0x00e88000 \
  --tags_offset    0x13f88000 \
  --pagesize 2048 \
  --cmdline "panic=0 console=tty0 console=ttyS0,921600 console=ttyGS0 bootopt=64S3,32N2,32N2 pmos_boot_uuid=${PMOS_BOOT_UUID:-REPLACE-WITH-BOOT-UUID} pmos_root_uuid=${PMOS_ROOT_UUID:-REPLACE-WITH-ROOT-UUID} pmos_rootfsopts=defaults earlycon=uart8250,mmio32,0x11005000" \
  -o "$OUT"

echo ""
echo "Boot image: $OUT ($(wc -c < $OUT) bytes)"
python3 -c "
import struct
with open('$OUT','rb') as f: d=f.read(600)
ks,ka,rs,ra,ss,sa,t,ps = struct.unpack_from('<IIIIIIII',d,8)
print(f'  kernel={ks}B @ 0x{ka:08x}  ramdisk={rs}B @ 0x{ra:08x}  dtb={ss}B @ 0x{sa:08x}')
"
