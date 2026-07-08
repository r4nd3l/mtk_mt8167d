# postmarketOS / mainline Linux port — Lenovo Tab E7 (TB-7104F)

Work-in-progress mainline Linux bring-up for the Lenovo Tab E7 (TB-7104F),
a budget 7" Android tablet built around a MediaTek MT8167D SoC.

## Hardware

| | |
|---|---|
| SoC | MediaTek MT8167D (quad-core Cortex-A35, PowerVR GE8300 GPU) |
| PMIC | MediaTek MT6392 |
| Board | Lenovo `HXF-M94V-0` reference design (shared with TB-7304F / TB-7703F) |
| eMMC | 8 GB |
| RAM | 1 GB (declared in DTS; unconfirmed against physical population) |
| Display | 7" IPS, 1024x600 |
| Bootloader | Locked, SBC + DAA enabled |

## Status

**Not yet booting to a usable rootfs.** Current state:

- Kernel + DTS + config fragment build and flash successfully via `mtkclient`
  (see [Flashing](#flashing) — the locked/SBC+DAA bootloader is bypassed via
  the Kamakiri exploit chain, no unlock needed).
- After fixing the Android boot image offsets (see below), the kernel now
  runs for 45-54 seconds — far past earlier attempts — with a USB gadget
  (`idProduct=2001`) coming up and staying enumerated for that whole window.
- It still doesn't reach a working console/rootfs. The kernel is configured
  with `pstore`/`ramoops` (`device/linux-lenovo-tabe7/tabe7.fragment`) reserved
  at `0x44400000` (1 MB) specifically so a panic from this stage can be read
  back via `mtk.py peek 0x44400000 0x100000` without needing UART or a
  working console — that's the next debugging step.
- Physical UART pad location and signal voltage (1.8V vs 3.3V) on this board
  are still unconfirmed.

### The boot-address bug (fixed)

Early bring-up used naive Android boot image offsets (`--base 0x40000000`,
`--second_offset 0x14000000`, etc.) copied from generic MediaTek examples.
This **looked like** a kernel crash — device cycled through the MediaTek BROM
USB ID (`idProduct=2000`) every ~2.7 seconds indefinitely — but the BROM cycle
itself is normal (MediaTek's BROM always waits briefly for a flash tool before
continuing the boot chain; see
[mtkclient](https://github.com/bkerler/mtkclient)). The real bug was that the
DTB was being loaded to the wrong physical address, so the kernel could never
find its own device tree and silently died before doing anything observable.

MediaTek 64-bit bootloaders expect `base + 0x78000`, with that same `0x78000`
subtracted back out of every other offset. The corrected values (confirmed
against a sibling MT8167 Lenovo Tab E7 variant, the TB-7304F) are baked into
`device/device-lenovo-tabe7/deviceinfo` and `scripts/pack-boot.sh`:

```
base:     0x40078000
kernel:   0x00008000
ramdisk:  0x14f88000
second:   0x00e88000   (DTB)
tags:     0x13f88000
```

If you're porting a related MT8167/MT8516 Lenovo tablet and see an
unchanging ~2.7s BROM USB cycle no matter what you change in your DTS or
kernel config, check this first.

## Building

Kernel is based on
[Luca Leonardo Scorcia's `x04g-7.2-rc1` branch](https://github.com/lscorcia/linux/tree/x04g-7.2-rc1),
which has working MT6392 PMIC, eMMC, SD, and USB support for MT8167 devices,
plus this project's board-specific DTS and config fragment on top.

Via [pmbootstrap](https://postmarketos.org) (requires `pmbootstrap_min_version`
as pinned in the upstream `pmaports.cfg` — check current requirements):

```sh
pmbootstrap init          # select device lenovo-tabe7
pmbootstrap build linux-lenovo-tabe7 device-lenovo-tabe7
pmbootstrap install
pmbootstrap flasher flash_kernel
```

Or cross-compile directly and pack with `scripts/pack-boot.sh` — see the
comments in that script for required environment variables
(`KBUILD`, `WORKDIR`, `MKBOOTIMG`, `PMOS_BOOT_UUID`, `PMOS_ROOT_UUID`).

## Flashing

The TB-7104F ships with a locked bootloader (SBC + DAA enabled), so
`fastboot flash` / `fastboot oem unlock` are not available. All flashing here
goes through [mtkclient](https://github.com/bkerler/mtkclient), which bypasses
this via the Kamakiri exploit chain automatically:

```sh
python3 mtk.py w boot boot.img
```

**Before you touch a locked device: back up every partition first.**
`mtkclient` can read the full partition set back out:

```sh
python3 mtk.py rl backup-dir/
```

This project's own testing only ever wrote to the `boot` partition — never
`system`, `vendor`, or `userdata` — specifically so the original Android
install stays recoverable from a single `boot` partition backup.

## Credits

- [Luca Leonardo Scorcia](https://github.com/lscorcia) — `x04g-7.2-rc1` kernel
  branch, the MT8167 PMIC/eMMC/USB bring-up this port is built on, and
  diagnosis of the BROM USB cycle behavior.
- [Val Packett](https://sr.ht/~valpackett/) — TB-7304F mainline port; the
  corrected boot image offsets used here came directly from her working
  `deviceinfo` for that sibling device.
- The [postmarketOS](https://postmarketos.org) and
  [#x04g:mainlining.org](https://matrix.to/#/#x04g:mainlining.org) Matrix
  community.

## License

Kernel patches and config fragment: GPL-2.0-only, matching the Linux kernel.
Device port package (`device-lenovo-tabe7`): MIT, per postmarketOS convention.
See individual `APKBUILD` files.
