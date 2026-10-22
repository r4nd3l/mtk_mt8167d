# TE macrotile split: geometry-path CR writes, stock 22.40 vs our 22.68 FW

VERIFIED = read from disassembly/DWARF. INFERRED = interpretation.
CR bank: 0xCF400000 (22.40), 0xCF800000 (22.68). Offsets below are CR offsets.
Helper scripts (scratchpad, may be wiped): crw.py lists every `sw` whose base reg came from
`lui 0xcf40/0xcf80`; set-diff of offsets between the two FWs.

## 1. Stock 22.40 rgxfw_ta_kickta (c0004558), VERIFIED
$16 = TA cmd, TA regs struct at $16+0x38 (DWARF `RGXFWIF_TAREGISTERS`), $17 = HWRTDATA (DWARF).
| CR | source | c0 addr |
|---|---|---|
| 0x248/24C PM_VHEAP_TABLE | *(HWRT.psParentRenderTarget)+0 (VHeapTable) | c0004658 |
| **0x240/244 PM_MTILE_ARRAY** | **uTAReg_PM_MTILE_ARRAY (TA regs +0x00), per kick; vendor UM = MTA pad = ISP_MTILE_BASE** | c0004668 |
| 0x250=1, 0x418/41C/420 | freelist (PM TA fstack load), poll 0x250 | c0004682.. |
| C88 PPP_CTRL | TA regs +0x08 | c00046b6 |
| C90/C94 PPP_WCLAMP | TA regs +0x10 (vendor 0x3089705f x2) | c00046be |
| C98 PPP_SCREEN | HWRT.ui32PPPScreen (+0xf0) | c00046cc |
| **C78 PPP_GRID_OFFSET** | **HWRT.ui32PPPGridOffset (+0xf4), vendor 0 (0x88 only if flipped)** | c00046d4 |
| C80/C84 PPP_MULTISAMPLECTL | HWRT.ui64PPPMultiSampleCtl (+0xf8) | c00046dc |
| C28 TE_PSG | TA regs +0x18 | c00046ec |
| **C48** | **HWRT.ui32TPCStride (+0x100), unconditional** | c00046f4 |
| C38/C3C TE_PSGREGION_ADDR | TA regs +0x20 | c00046f8 |
| C40/C44 TE_TPC_ADDR | HWRT.sTailPtrsDevVAddr (+0x108) | c000470c |
| C00 TE_AA | HWRT.ui32TEAA (+0x11c) | c0004714 |
| C08 / C10 TE_MTILE1/2 | HWRT.ui32TEMTILE1/2 (+0x120/+0x124) | c0004720/28 |
| C18 TE_SCREEN | HWRT.ui32TEScreen (+0x114) | c0004730 |
| C20 MTILE_STRIDE | HWRT.ui32MTileStride (+0x118) | c0004738 |
| 0x408 VDM_CTRL_STREAM_BASE | TA regs +0x28 | c000473c |
| 0x470 VDM_SYNC_PDS_DATA_BASE | TA regs +0x38 | c000474c |
| 0x17A0 TPU_BORDER_COLOUR_TABLE | TA regs +0x48 | c0004750 |
| 0x1788 TPU_YUV_CSC (if !=0) | TA regs +0x50 | c0004bc6 |
| 0x1780 TPU | TA regs +0x58 | c000476c |
| CB0/CB4 (TA_RTC_ADDR_BASE, if !=0) | TA regs +0x40 | c0004bde |
| CE0=1, D20, CB8=1 poll, C68=1 | constants | c000479a.. |
No FW computation from TE_SCREEN/PPP_SCREEN anywhere (HWRT +0x114..+0x124 only read in kickta,
copied verbatim). HWRT.ui32TPCSize (+0x110) is never written to a CR in kickta.

rgxfw_pm_setup_context (c000e4b8): 0x2B0 = (a<<1)|b, 0x2E0 = HWRT.psPMMListDevVAddr,
0x1248/1250/1260 (ctx0) or 0x1268/1270/1280 (ctx1) = VCECatBase[0]/TECatBase[0]/AlistCatBase,
0x3D0 ctx select, 0x2C8/0x278 = MList/AList stack pointers, 0x190/0x288 triggers. Only index [0]
of the 4 cat bases is used. 22.68 (c0012b30..c0012d5c) does the same from the same fields. No
PM register with macrotile meaning written here in either FW.

## 2. Our 22.68 geometry kick (c0010fde..c0011180), VERIFIED
$20 = cmd-0x28 (geom regs at $20+0x38), $17 = HWRTDATA, $9 = HWRTDATA_COMMON.
C88 = ppp_ctrl, C98 = common.screen_pixel_max, C80 = common.multi_sample_ctl (flipped if teaa!=0 &&
flag 4), C28 = te_psg, C38 = hwrt.rgn_header_dev_addr (+0x100), C40 = hwrt.tail_ptrs (+0xf0),
C00/C08/C10/C18/C20 = common te_aa/te_mtile1/2/te_screen/mtile_stride, 0x17A0, 0x1780, 0x248 =
hwrt.vheap_table (+0xc8). C48 = common.tpc_stride ONLY on the rtc_dev_addr!=0 branch (c00114b0).

## 3. Differences (VERIFIED)
1. **PM_MTILE_ARRAY 0x240/0x244**: stock per TA kick from the client. 22.68 writes it ONCE, at
   FW init (c0001904/c000190c) = **0x00000065_C0000000** (constant; not in any mainline heap).
   22.68 never loads offset 0xf8 (hwrt.macrotile_array_dev_addr) anywhere (no lw/lwp 0xf8/0xfc).
2. **PPP_GRID_OFFSET C78 and ISP_GRIDOFFSET 0xFA0**: stock per kick from HWRT / 3D regs (vendor 0).
   22.68 writes both ONCE at init = 0x88 (c00018ea, c00018f2). Live C78 = 0x88 confirms.
3. **ISP_MERGE 0x11D0/11D8/11E0/11E8/11F0/11F8**: stock kick3d (c000192c..c0001954) writes
   HWRT.ISPMergeLowerX/LowerY/UpperX/UpperY/ScaleX/ScaleY. 22.68 never writes these offsets at all;
   common.isp_merge_* (+0x34..+0x48) are unused.
4. C48 conditional (above). PPP_WCLAMP C90/C94: init constant 0x3089705f x2 in 22.68 = vendor value.
5. Other stock-only offsets (0x1318/131C SLC, 0x1360 tiling, 0x1600 FBCDC, 0x17C0 TPU, 0xF70-F7C
   ISP mask load/store base): not geometry/macrotile related.

## 4. Test hypotheses (INFERRED, ranked)
Since 22.68 writes 0x240, C78, FA0 only at FW init and never per kick, a /dev/mem poke after FW
boot and before the render persists (until the next GPU power-off/FW re-init: re-poke after idle).
- H1 PM_MTILE_ARRAY: read 0x240/0x244 live (expect 0xC0000000/0x65). Poke = vendor value = the MTA
  pad address (same as ISP_MTILE_BASE, i.e. the mapped MTA/rgn buffer; 40-bit, low 32 at 0x240,
  high at 0x244). Alternative without pokes: map a zeroed 4K page at GPU VA 0x65_C000_0000 in the
  render VM. Check BIF fault regs for faults at 0x65C00xxxxx first: a fault there proves the PM uses it.
- H2 grid offset: poke C78 = 0 and FA0 = 0 (vendor non-flipped values).
- H3 ISP merge: poke 0x11D0..0x11F8 with Mesa's isp_merge_* floats (ISP-side only, cannot change TE
  header bits, but can matter for the "ISP stops after macrotile 0" half of the symptom).
- No FW code derives a macrotile boundary from screen size; the split must be HW-internal or from
  PM state (MTA) / memory contents, not from any CR the FW computes.

## 5. Hardware tests 2026-10-22 evening (live /dev/mem reads + pokes, 20-tools/vktri/gpuregs.c, gpupoke.c)
- Live 256x256 values equal vendor 1.9: C08=C10=0x400000, C18=0xf00f, C20=0x100, F18=0x100010, F28=0x100.
- 0x240 PM_MTILE_ARRAY live = 0x65C0000000 (as predicted). Poked to the MTA address (0x800016e000):
  no change; MTA stays all zero after the render (dumped with hwrtspy DUMP_VA). TE never writes it.
- C78/FA0 grid offset poked 0x88 -> 0: pixel count changes slightly (2601 -> 2550), split unchanged.
- Classic TE_MTILE values (confirmed live) -> region header map byte-identical. TE ignores TE_MTILE/STRIDE.
- Header byte0 map (16x16): 0x10 = prims in top-left 8x8, 0x12 = prims elsewhere (L shape), 0x14 at (7,7),
  0x07 at (15,15), 0x01/0x03 empty.
- **Workaround that works**: dataset at 2x the framebuffer (512x512 target, 256x256 viewport) -> the whole
  triangle renders correctly (`30-images/vktri-512-vp256.png`). Mesa patch 0008 (mesa-pvr-tabe7 r12) does
  this for SIPF v1. Root cause of the fixed split still unknown.

## 6. ROOT CAUSE FOUND (2026-10-22 22:10): TA terminate clip units
Not a macrotile at all. Region header byte0 bit 1 (0x02) = tile outside the TA clip region. The clip
came from the PPP TERMINATE state (`pvr_render_state_create_ppp_state`, pvr_arch_framebuffer.c):
`clip_right/bottom = DIV_ROUND_UP(fb_size, 32) - 1`. GE8300 22.40 reads those fields in **16 px blocks**,
so the clip ended at half the framebuffer per axis. Evidence: the split followed the framebuffer size,
not the dataset, render area, viewport or scissor. Mesa patch 0008 (mesa-pvr-tabe7 r13): 16 px blocks
when simple_parameter_format_version == 1. Result: full renders at 64/256/384/512/1024 and 1280x800,
pixel counts scale with area, no FW faults (`30-images/vktri-256-1024-r13.png`, `vktri-1280x800-r13.png`).
Bit 0x04 = last header of the clip-inside / clip-outside run (the "macrotile" reading was wrong).
