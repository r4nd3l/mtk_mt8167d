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
| Bootloader | SBC + DAA enabled, unlockable via fastboot |

## Status

**Boots postmarketOS (Phosh) from eMMC and is usable as a daily tablet.**
Kernel: Luca Leonardo Scorcia's `x04g` 7.3-rc5 tree plus the patches in
`device/linux-lenovo-tabe7/`.

Working:

- Reliable boot from eMMC: pwrap `OP_TYPE`/`MSB_FIRST` fix, arch timer
  lost-event workaround (erratum DT flag), `console=tty0`, GPT irq NULL deref fix
- MT6392 charger: 900 mA, drops to 450 mA if VBUS sags; re-arms the charge
  path on every plug-in (the PMIC clears it on unplug); battery % from an OCV
  table, sampled during a 1 s charge pause once a minute while charging;
  charge-only mode when powered on by the cable
- Backlight (disp_pwm + mmsys clocks), simpledrm reported as a DSI connector
- Touch: flashless Goodix GT912F at 0x5d (not Silead), via goodix fw upload
- BMA253 accelerometer and rotation, cpufreq + thermal throttling, RTC
- Audio: speaker (the external amp sits on the left headphone channel, mono
  UCM), memif format fix, both volume keys
- Wi-Fi and Bluetooth: Luca's consys drivers ported to 7.3 with stock firmware;
  fixed a BT kthread busy loop and the firmware's empty LE command bitmap, so
  BLE keyboards pair and reconnect. The driver makes up a new MAC every boot,
  so reach the tablet as `lenovo-tabe7.local` (avahi)
- nftables firewall (needs `CONFIG_IPV6` for the inet table)
- Phosh with a patched phrog greeter (first PIN attempt no longer fails)

Not yet:

- GPU (PowerVR GE8300 needs 32-bit MIPS bus support in `drm/imagination`)
- USB OTG host mode: pulling up the IDDIG pin is right, but without VBUS
  sensing `usb-conn-gpio` then reports `USB_ROLE_NONE` and the gadget never
  binds (USB networking dies). Needs VBUS detection, e.g. from the charger.
- Headphone jack detection, mediatek-drm instead of simpledrm

### Firmware is not in this repository

`device/device-lenovo-tabe7/APKBUILD` expects these vendor files next to it;
they are not redistributable here. Extract them from your own device's stock
`vendor` image (Wi-Fi/BT, touch) or Imagination's linux-firmware fork (GPU):
`WIFI_RAM_CODE_8167`, `WMT_SOC.cfg`, `ROMv2_lm_patch_1_0_hdr.bin`,
`ROMv2_lm_patch_1_1_hdr.bin`, `gt912f-lenovo-tb7104f.fw`,
`gt912f-lenovo-tb7104f.cfg`, `rogue_22.67.54.30_v1.fw`, `rogue_22.68.54.30_v1.fw`.

## Building

Kernel is based on
Luca Leonardo Scorcia's `x04g` branch (7.3-rc5),
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

The packages in `device/` go into a pmaports checkout under
`device/testing/`. Once a pmOS install is running, `scripts/flash-latest.sh`
installs freshly built packages over ssh and writes the boot.img that
the tablet itself just generated (`T=<ssh host>` to use Wi-Fi instead of USB).

Or cross-compile directly and pack with `scripts/pack-boot.sh` — see the
comments in that script for required environment variables
(`KBUILD`, `WORKDIR`, `MKBOOTIMG`, `PMOS_BOOT_UUID`, `PMOS_ROOT_UUID`).

## Flashing

The bootloader can be unlocked with `fastboot flashing unlock`; confirm with
**Volume UP** on the tablet (Volume Down silently declines). After that,
`fastboot flash boot boot.img` works. Two LK quirks:

- LK only enumerates over USB if the cable is already plugged in when you
  enter fastboot mode (Volume UP + Power, then `[Fastboot Mode]`).
- LK fastboot can go mute (enumerates, every command times out), and charging
  does not fix it. Fastboot mode does not charge the battery either.

When fastboot is mute, [mtkclient](https://github.com/bkerler/mtkclient)
still works through BROM: unplug the battery, hold Volume UP + Volume Down,
plug in USB, reconnect the battery. "Detected regular mode" without "BROM mode
detected" means retry. After the write the board stays in DA mode
(`0e8d:2001`); run `mtk reset`, then unplug and reconnect the battery.

```sh
mtk w boot boot.img
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
