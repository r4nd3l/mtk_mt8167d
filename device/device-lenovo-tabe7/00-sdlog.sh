#!/bin/sh
# Write kernel boot log for offline diagnostics.
# Runs in initramfs at /hooks time (after udev, before rootfs mount).
#
# Writes "HOOK_RAN\n" + dmesg to spare eMMC space via /dev/mmcblk0
# at absolute sector 5774304 (= byte 2956443648 on the eMMC).
# pmos_root ends at absolute sector 5771263, so this is safe.
#
# Read back:
#   sudo mtk ro 2956443648 131072 /tmp/klog.bin && strings /tmp/klog.bin
# If "HOOK_RAN" appears but no kernel log follows, dmesg was empty.
# If nothing appears at all, the hook never ran (init_2nd.sh didn't exec).

# Method 1 — eMMC via /dev/mmcblk0 absolute sector (most reliable)
if [ -b /dev/mmcblk0 ]; then
	{ printf 'HOOK_RAN\n'; dmesg; } | \
		dd of=/dev/mmcblk0 bs=512 seek=5774304 2>/dev/null
fi

# Method 2 — SD card (works once mmc1 pinctrl is added)
SDDEV=""
for dev in /dev/mmcblk1p1 /dev/mmcblk1; do
	[ -b "$dev" ] && SDDEV="$dev" && break
done
if [ -n "$SDDEV" ]; then
	mkdir -p /mnt/sdlog
	if mount -t vfat "$SDDEV" /mnt/sdlog 2>/dev/null || \
	   mount -t ext4 "$SDDEV" /mnt/sdlog 2>/dev/null; then
		{ printf 'HOOK_RAN\n'; dmesg; } > /mnt/sdlog/kernel-boot.log
		umount /mnt/sdlog
	fi
fi
