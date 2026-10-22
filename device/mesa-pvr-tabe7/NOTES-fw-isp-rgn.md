# FW ISP region / mtile register analysis (2026-10-22)

## Stock 22.40.54.30 FW (DDK 1.9, DWARF)
- `_RGXFWIF_HWRTDATA_` (328 B) has NO rgn_header_size / isp_mtile_size fields.
  TE fields: ui32TEScreen@276 MTileStride@280 TEAA@284 TEMTILE1@288 TEMTILE2@292
  ISPMerge*@296..316, psRTACtl@228.
- 3D cmd regs (`u3DReg_*`, 8 B each): ISP_MTILE_BASE@72(0x48) ISP_MTILE_SIZE@80(0x50)
  ISP_RGN@216(0xd8) ISP_CTL@224.
- Register bank mapped at 0xCF400000 in FW. Offsets: ISP_RENDER 0xF08, ISP_RENDER_ORIGIN 0xF10,
  ISP_MTILE_SIZE 0xF18, ISP_MTILE_BASE 0xF20, ISP_RGN 0xF28, ISP_CTL 0xF38.
- `rgxfw_3d_kick3d` @c00016f0, normal path c0001838..c0001924 ($16 = 3D regs, $17 = HWRTDATA
  = cmd+0x268, $22 = HWRTDATA.psRTACtl):
  - ISP_MTILE_SIZE(0xF18) = regs.ISP_MTILE_SIZE           (plain copy)
  - ISP_RGN(0xF28)        = regs.ISP_RGN                  (plain copy)
  - ISP_MTILE_BASE(0xF20) = regs.ISP_MTILE_BASE + RTACtl[+4](layer idx) * ALIGN(5*regs.ISP_RGN, 4096)
  - ISP_RENDER_ORIGIN(0xF10)=0, ISP_RENDER(0xF08)=0 here (later rewritten)
  - No division / tile-group logic, no SIPF version check. FW computes nothing about tile counts.
- Second block c00022b4 = fast-render path (MTILE_SIZE=0x00010001, ISP_RENDER=3), not normal draws.
- Stock `rgxfw_ta_kickta` (c0004558, ~c00046aa..c0004738, $17=HWRTDATA, $16=TA cmd regs):
  TE_AA C00=TEAA@284, TE_MTILE1 C08=@288, TE_MTILE2 C10=@292, TE_SCREEN C18=TEScreen@276,
  TE_MTILE_STRIDE C20=MTileStride@280, TE_PSG C28=cmd, TE_PSGREGION_BASE C38=cmd (UM-supplied),
  TE_TPC_BASE C40=sTailPtrs, C48=TPCStride, PPP_SCREEN C98=PPPScreen, PPP_MSCTL C80, GRIDOFFSET C78.
  All plain copies.

## 22.68.54.30 open FW (stripped, no symbols; disasm fw-22.68.54.30/fw.asm, fw.s)
- Register bank at 0xCF800000 (lui 0xcf80). Same CR offsets.
- 3D kick, c000e66e..c000e71a ($23 = HWRTDATA, $6/$5 = HWRTDATA[0xa4] & 0xc0fffffc = common,
  $fp+0x38 = rogue_fwif_frag_regs):
  - ISP_MTILE_SIZE(F18) = common[0x50] = isp_mtile_size        (plain copy)
  - ISP_RGN(F28)        = common[0x4c] = rgn_header_size       (plain copy)
  - ISP_MTILE_BASE(F20) = hwrt[0x100](rgn_header_dev_addr) + hwrt[0xd4](rta_ctl.current_render_target)
                          * ALIGN(5*rgn_header_size, 4096)   (5 B = SIPF v1 header size, Mesa rogue_get_region_header_size)
  - ISP_AA(F30)=frag.isp_aa, ISP_BGOBJDEPTH(F80)=frag.isp_bgobjdepth, ISP_RENDER(F08)=0, ORIGIN(F10)=0
  - no shifts/divides on any of these, no SIPF-version branch.
- Geom kick c0011060..c00110d8 ($9 = common, $17 = hwrt, $20 = geom cmd):
  PPP_SCREEN C98=common[4], PPP_MSCTL C80=common[8] (or flipped common[0x10] if teaa!=0 && cmd flag 4),
  TE_PSG C28=geom regs, TE_PSGREGION_BASE C38=hwrt[0x100], TE_TPC_BASE C40=hwrt[0xf0],
  TE_AA C00=common[0x28], MTILE1 C08=[0x2c], MTILE2 C10=[0x30], TE_SCREEN C18=[0x20], MTILE_STRIDE C20=[0x24].
- Every offset matches mainline pvr_rogue_fwif.h / _check.h exactly -> no struct layout mismatch.
- Transfer (TQ) kick c000f90e / c000fb90: MTILE_SIZE/RGN/MTILE_BASE copied from transfer regs.

## Conclusion
Neither FW halves anything; FW is a pass-through. The ceil(n/2) clipping must come from the values
themselves or from other state (hypothesis: ISP in sample space, e.g. ISP_AA / TE_AA / PPP MSCTL
mismatch, which makes a 16x16 ISP tile cover 8x8 px).
