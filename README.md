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
  path on every plug-in (the PMIC clears it on unplug); battery % from a
  software gauge after the vendor SW_FG "OAM" algorithm (no current sense:
  current inferred from OCV(model) - V over the vendor R table, integrated;
  shown % moves at most 1 %/min and only in the charge direction; host
  simulation in `scripts/gauge-sim`); charge-only mode when powered on by
  the cable
- Backlight (disp_pwm + mmsys clocks), simpledrm reported as a DSI connector
- Touch: flashless Goodix GT912F at 0x5d (not Silead), via goodix fw upload;
  the upload needs a `goodix_int_sync()` before the first status read (Luca's
  patch, going upstream)
- Rear camera (GC2375H, 2 MP): `mt8167-camsv` drives the MT8167 SENINF
  (MIPI CSI-2 receiver) and the ISP pass1 RAW path straight into memory, no
  ISP processing: V4L2 capture of 1600x1200 RAW10 at 30 fps. The register
  recipe comes from MediaTek's "FrmB" userspace ISP code (MT8163 mtkcam tree):
  CAM_EN|TG1_EN|PAK_EN, the three CAM_CTL mux registers, IMGO FIFO; the
  SMI larb1 MMU is bypassed (physical DMA, no iommus: attaching the M4U
  garbles the LK framebuffer while the panel runs on simpledrm) and the
  SENINF mux runs in one-pixel mode. Sensor drivers `gc2375h` and `gc030a`
  share one CSI port, `media-ctl -l` picks the active one. The driver
  outputs 8-bit Bayer (the IMGO "PAK" RAW10 is MediaTek bitstream packing,
  not CSI2P) and sets `V4L2_CAP_IO_MC` so libcamera can filter formats
- Camera userspace: libcamera's simple pipeline with the software ISP in CPU
  mode (`device/libcamera`: the camsv entry in `supportedDevices`, and
  `/etc/libcamera/configuration.yaml` since the eGL path on llvmpipe is too
  slow). The sensors expose `rotation`/`orientation` from DT as V4L2
  controls (`v4l2_ctrl_new_fwnode_properties`; note `orientation` is a
  number in DT, 0 front / 1 back). GNOME Snapshot shows a live, upright
  viewfinder through PipeWire: a WirePlumber drop-in hides the raw camsv
  V4L2 node (Bayer, no ISP) and makes the back camera the default source.
  The software ISP output is capped at 1024x576 (the panel is 1024x600,
  the GTK software renderer is the bottleneck, and the CPU debayer runs the
  SoC to 80 C). libcamera patch 0010 fixes an upstream bug where stopping
  the camera with frames inside the software ISP left requests waiting for
  IPA metadata forever and killed WirePlumber with the queuedRequests_
  assertion ("No Camera Found" on the next launch)
- Charging LED (GPIO56) follows the MT6392 battery supply through a LED
  trigger (`linux,default-trigger`)
- BMA253 accelerometer and rotation, cpufreq + thermal throttling, RTC
- Audio: speaker (the external amp sits on the left headphone channel, mono
  UCM), memif format fix, both volume keys; headphone plug detection on GPIO19
  (vendor ACCDET in EINT mode), PipeWire switches speaker/headphones on its own
- Wi-Fi and Bluetooth: Luca's consys drivers ported to 7.3 with stock firmware;
  fixed a BT kthread busy loop and the firmware's empty LE command bitmap, so
  BLE keyboards pair and reconnect. Stable Wi-Fi and BT addresses: the
  drivers read the vendor NVRAM records through request_firmware(), so the
  init script writes short WIFI/BT_Addr files under /lib/firmware with
  locally administered addresses derived from the SoC efuse ID
- nftables firewall (needs `CONFIG_IPV6` for the inet table)
- USB OTG: dr_mode otg with the IDDIG pull fixed (R1R0 pin, `MTK_PUPD_SET_R1R0_01`;
  a bare bias-pull-up made musb fail to probe). The board has no VBUS boost, so
  host mode needs a powered OTG Y-cable or hub; "USB host mode" in the app grid
  (`usb-host-mode`) switches the role without root
- Wi-Fi scans are rate-limited while connected (1 per 30 s): GNOME Settings
  scanned every 7-8 s and this firmware drops the link on online scans
- Phosh with a patched phrog greeter (first PIN attempt no longer fails)

- GPU: the PowerVR GE8300 (BVNC 22.40.54.30, MIPS firmware, 32-bit physical
  bus) runs Imagination's 22.68.54.30 firmware under mainline
  `drm/imagination` with Mesa's open Vulkan driver. vkmark runs clean and
  GTK4 apps render with GSK Vulkan by default (`gpu-ui on|off`). What was
  missing, reverse engineered from the vendor DDK 1.9 and its firmware
  (`device/mesa-pvr-tabe7/NOTES-*.md`):
  - kernel 0028: on a 32-bit bus the MIPS wrapper register bank address is
    the physical GPU register base (mainline refused such cores outright)
  - kernel 0029: BRN 65273, the firmware's TE region header initialiser
    touches fixed addresses 0xF800000000 / 0xF900000000, so every VM
    context maps a page there (`brn65273` module parameter)
  - firmware: the open 22.68 firmware writes a fixed 0x10000 to
    CR_ISP_PIXEL_BASE for FAST_2D transfers; `device-lenovo-tabe7` patches
    it to the transfer heap base at install time
  - Mesa (`device/mesa-pvr-tabe7`): simple internal parameter format **v1**
    (upstream only has classic and v2), PPP clip in 16 px blocks,
    ISP_BGOBJDEPTH always F32, and CPU-written buffers bypass the GPU SLC
    (the SoC is not coherent and the kernel never flushes it). Also shader
    compiler fixes that make opaque fills about 10x faster.
  Presenting still goes through a CPU copy (`MESA_VK_WSI_DEBUG=sw`), because
  the compositor runs pixman on simpledrm. GPU tests: `tools/gpu-tests/`.
  `device/mesa-clc` builds the native `mesa_clc`/`pco_clc` tools that a
  cross build of the Imagination driver needs.

Not yet:

- GPU buffers on screen without a CPU copy: needs mediatek-drm
- mediatek-drm (work in progress, modules blacklisted by default): the
  EK79007 panel driver (`panel-lenovo-tb7104f.c`, reset is GPIO66) binds and
  modesets, but DSI LP commands time out and CPU0 locks up shortly after.
  ovl0 behind the M4U also keeps reading LK's framebuffer layers; patch 0035
  guards mtk_iommu against direct mappings before the first attach
- Front camera (GC030A): the sensor streams but the receiver sees nothing,
  it sits on the other CSI port and that routing is not found yet (its
  PipeWire node is hidden until then)
- Camera tuning: no sensor-specific IPA tuning file yet, exposure/gain come
  from the generic software ISP AGC

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
`device/testing/`, except `device/libcamera`, which holds only the pkgrel
bump and the extra patch applied on top of pmaports' `temp/libcamera`. Once a pmOS install is running, `scripts/flash-latest.sh`
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
