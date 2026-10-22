# Per-kick TA/3D registers in vendor DDK 1.9 libsrv_um.so (SIPF v1, GE8300 22.40.54.30)

VERIFIED = read from objdump of `RGXKickTA` (0x236d8..0x24b88, exported) and `RGXAddRenderTarget`
(0x21e80..), slot names from the FW DWARF (`_RGXFWIF_TAREGISTERS_` size 104, `_RGXFWIF_3DREGISTERS_`
size 608, `_RGXFWIF_CMDTA_` 280, `_RGXFWIF_CMD3D_` 664). INFERRED = my interpretation.
Slices: scratchpad `re-kick3d/kickta.asm`, `re-kick3d/addrt.asm` (regenerate with objdump).

## 0. Where the blocks live (VERIFIED)
`RGXKickTA(r0=dev, r1=psKickTA, r2, r3)`. TA cmd = sp+0x260, 3D cmd = sp+0x378 (= TA+0x118).
`rt` = psKickTA+0x704 (the RT object r9 of RGXAddRenderTarget); `rc` = psKickTA+0x700 (render
context); `idx` = rt+0x60 (RT-data toggle 0/1); `rtdata[idx]` = rt + 80*idx.
CMDTA: +0xd8 flags, +0xe0 HWRTData = rtdata[idx]+0xa8, +0x108 DummyRgnHdrInit TA regs =
rc->dev(+0x180)+0x88 (BRN65273), +0x110 BRN61484LiveRT = psKickTA+0xa44.
CMD3D: +0x268 HWRTData (sp+0x5e0) same handle.

## 1. RGXFWIF_TAREGISTERS (TA cmd +0x00, 0x237ca..0x2386e) (VERIFIED)
| off | reg | vendor value |
|---|---|---|
| 0x00 | PM_MTILE_ARRAY | rtdata[idx]+0x90 (64-bit) = base of the MTA_MLIST alloc (the 16-byte MTA pad; same addr for both RT datas since MTA size 0) |
| 0x08 | PPP_CTRL | psKickTA+0x280 (client), hi 0 |
| 0x10 | PPP_WCLAMP | 0x3089705f (lo and hi both; = 1e-9f) |
| 0x18 | TE_PSG | rt+0x70 (region_stride in 4K pages; **0 for 1 layer**) \| bit18 zonlyrender = kick flag bit2 \| bit17 completeonterminate = (kickflags&3)==3. forcenewstate NOT set |
| 0x20 | TE_PSGREGION_ADDR | rtdata[idx]+0x88 = RgnHeader base + idx*rgn_per_rtdata (0x229dc/0x229e6) |
| 0x28 | VDM_CTRL_STREAM_BASE | psKickTA+0x10 (client) |
| 0x30 | VDM_CTRL_STREAM_TERMINATE | psKickTA+0x20 |
| 0x38 | VDM_SYNC_PDS_DATA_BASE | psKickTA+0x18 |
| 0x40 | TA_RTC_ADDR_BASE | rt+0x158 |
| 0x48 | TPU_BORDER_COLOUR_TABLE | psKickTA+0x2b8 >> 2 |
| 0x50 | TPU_YUV_CSC | psKickTA+0x2a8 >> 2 |
| 0x58 | TPU | psKickTA+0x2c0 |
| 0x68 | sTARegsCSwitch | memcpy 0x70 from rc+0x1b0, then +0x90/+0xc0 \|= USC_PIXEL_OUTPUT_CTRL |
No TE_SCREEN / TE_MTILE / PPP_SCREEN / TE_AA / TPC / VHEAP per kick: they live only in the HWRTData
(NOTES-hwrt-v1.md), the FW programs them from there.

## 2. RGXFWIF_3DREGISTERS (3D cmd +0x00, 0x239ae..0x23ad0) (VERIFIED)
| off | reg | vendor value |
|---|---|---|
| 0x00 | USC_PIXEL_OUTPUT_CTRL | from helper 0x2d572: ((8<<n)-8) \| (r5==4?5:4), n = min(10, min(5*r5*512,0x1400)/(r5*256)) |
| 0x48 | ISP_MTILE_BASE | rtdata[idx]+0x90 = **same address as PM_MTILE_ARRAY** |
| 0x50 | ISP_MTILE_SIZE | rt+0x50 = (tx&0x3ff)<<16 \| ty, tx=tiles_x*sx (16x16 -> 0x00100010) |
| 0x58 | ISP_GRIDOFFSET | rt+0x20 = same value as PPP_GRID_OFFSET ((flags&2)?0x88:0, 0 normally) |
| 0x60 | ISP_MULTISAMPLECTL | rt+0x28 (the flipped sample-pos copy) |
| 0x88 | ISP_AA | psKickTA+0x28c (client) |
| 0x90 | ISP_OCLQRY_BASE | psKickTA+0x2b0 & ~15, hi = byte +0x2b4 |
| 0xd8 | **ISP_RGN** | **rt+0x78 = tx*ty = num_sample_tiles** (r0 from 0x2201c stored at 0x22088 -> [sp+0x1d4] -> rt+0x78 at 0x22536; hi 0). 256x256 -> **256 (0x100)** |
| 0xe0 | ISP_CTL | psKickTA+0x290 (client) \| pipe_enable[15:12] = n-1 from helper \| bit16 sample_pos = rt flag bit0 \| bit18 (pt_upfront_depth_disable) if rc+0x40 \| bit19 overlap_check_mode always |
| 0x110..0x118 | PR_EVENT_PIXEL_PDS_DATA, EVENT_PIXEL_PDS_INFO | psKickTA+0x38/+0x3c/+0x40 |
| 0x120.. | PBE words | psKickTA+0x230.., +0x284.., +0x298.. |
| 0x218.., 0x238.. | PDS_BGND1.., PDS_PR_BGND1.. | psKickTA+0x28.., +0x48.. |
Nothing in the 3D block depends on the tile count except ISP_MTILE_SIZE and ISP_RGN.

## 3. Worked example 256x256, 1 sample, 1 layer (vendor)
TE_PSG = 0x00020000 (completeonterminate; 0 otherwise), TE_PSGREGION_ADDR = RgnHeader base (1280 B
per RT data), ISP_MTILE_SIZE = 0x00100010, ISP_RGN = 0x100, ISP_GRIDOFFSET = 0, PM_MTILE_ARRAY =
ISP_MTILE_BASE = MTA_MLIST base (valid address, 16 B pad), ISP_CTL |= 0x80000 | pipe_enable.

## 4. Diff vs Mesa 26.2.5 (pvr_arch_job_render.c, UNPATCHED tree here: v2 assert at line 128)
| reg | Mesa / kernel | vendor | note |
|---|---|---|---|
| TE_PSG.region_stride | rgn_headers_stride/4096 = 1280/4096 = 0 (line 756) | 0 | same |
| TE_PSG.forcenewstate | quirk 52942 only | never | same unless quirk listed |
| TE_PSG.completeonterminate | geometry_terminate | (flags&3)==3 | equivalent (INFERRED) |
| ISP_RGN | kernel `common.rgn_header_size = args->region_header_size` (pvr_hwrt.c:336) = Mesa `pvr_rt_get_isp_region_size` = tiles_per_mtile_x*y*mtiles (line 168): **16 with unpatched mtile_x1=4, 256 only if Mesa's mtile_info fix sets mtile_x1=num_tiles_x** | tx*ty = 256 | **CANDIDATE #1**: if the v1 fix was applied in the kernel only, Mesa still passes 16 -> ISP processes 1/16 of the headers |
| ISP_MTILE_SIZE | kernel get_cr_isp_mtile_size_val: mtile_x[0] (x16 if fixed, 4 if not) | 16x16 | same iff kernel fix |
| ISP_MTILE_BASE / PM_MTILE_ARRAY | Mesa: mta_size 0 -> `mta_dev_addr = PVR_DEV_ADDR_INVALID` (line 401-405), FW gets no address | valid address (head of MTA_MLIST alloc) | **CANDIDATE #2** (INFERRED): v1 HW may still need a valid MTA base |
| rgn header alloc / stride | (tiles_per_mtile_x/1)*(tiles_per_mtile_y/1)*5 (line 349) = 80 -> 128 B unpatched | 1280 B | TE overruns the buffer if Mesa's mtile_info not fixed |
| TPC size | pow2(tiles_per_mtile)^2*8 = 128 unpatched | 2048 | same remark |
| ISP_GRIDOFFSET | not in Mesa stream, not in kernel fwif (FW-internal) | 0 | same |
| ISP_CTL | sample_pos=1, process_empty_tiles, dbias_is_int, skip_init_hdrs | sample_pos from rt flag, overlap_check_mode(bit19) always, pipe_enable | no tile dependence; bit19 differs (INFERRED harmless) |
| PPP_WCLAMP | Mesa writes its own | 1e-9f | not geometry |
Everything else (TE_SCREEN, TE_MTILE1/2, MTILE_STRIDE, PPP_SCREEN) is HWRTData-side, covered in
NOTES-hwrt-v1.md; no per-kick copies exist in this DDK.

## 5. Conclusion
The only two per-kick, tile-count-dependent registers are ISP_MTILE_SIZE (from HWRTData) and ISP_RGN
(from Mesa's `region_header_size` on mainline). The vendor's ISP_RGN for v1 is simply the number of
sample tiles. Check that Mesa's `pvr_arch_rt_mtile_info_init` (not just the kernel) produces
tiles_per_mtile = num_tiles for v1; the 8x8-of-16x16 symptom is consistent with ISP_RGN/MTILE_SIZE
still coming from mtile_x1 = DIV_ROUND_UP(16,8)*2 = 4 in one of the two places, i.e. one axis
factor of 4 in region count (16 vs 256 headers = 4x4... ) -- measure by dumping the frag job's
region_header_size and the hwrt's isp_mtile_size in the kernel. Secondary: give v1 a real
ISP_MTILE_BASE (vendor always does).
