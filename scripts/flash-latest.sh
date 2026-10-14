#!/bin/sh
# Install the newest built linux/device/openmttools apks on the running tablet, write the boot.img
# that boot-deploy just built on the tablet (/boot/boot.img, NOT the stale pmbootstrap chroot one), reboot.
set -e
P=~/.local/var/pmbootstrap
A=$P/packages/edge/aarch64
T=${T:-user@172.16.42.1}  # T=tabe7-wifi for the Wi-Fi fallback
pick() { ls -v $A/$1-[0-9]*.apk | tail -1; }
F="$(pick linux-lenovo-tabe7) $(pick device-lenovo-tabe7) $(pick device-lenovo-tabe7-nonfree-firmware) $(pick openmttools)"
echo "$F" | tr ' ' '\n'
scp $F $T:/tmp/
ssh $T "set -e; cd /tmp
# NEVER --force-broken-world: on 10-14 it removed ~450 packages (the whole UI)
sudo apk add --no-network --allow-untrusted $(for f in $F; do printf './%s ' $(basename $f); done)
sudo dd if=/boot/boot.img of=/dev/disk/by-partlabel/boot bs=1M conv=fsync
sudo cmp -n \$(stat -c %s /boot/boot.img) /boot/boot.img /dev/disk/by-partlabel/boot && echo BOOT_OK
sync; sudo reboot"
