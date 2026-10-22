# HWRT (render target) setup in vendor DDK 1.9 libsrv_um.so, SIPF v1 (GE8300 22.40.54.30)

VERIFIED = read from the objdump of `RGXAddRenderTarget` (0x21e80..0x22c7c, exported, 3580 B).
INFERRED = my reading of what a value means. Disassembly slices live in
`/tmp/claude-1000/-home-matt-Documents-LinuxTablet/a5ba4b4c-17da-4a5a-a32d-7ab5baa67816/scratchpad/re-hwrt/addrt.asm`
(scratchpad, may be wiped; regenerate with
`arm-linux-gnueabihf-objdump -d --start-address=0x21e80 --stop-address=0x22c80 lib/libsrv_um.so`).

## 0. Entry points (VERIFIED)
- `RGXAddRenderTarget(r0=dev, r1=params, r2=&out_handle)` is the only HWRT builder in libsrv_um.
  No feature-bit test anywhere in it: compiled for exactly one parameter format (INFERRED: v1).
- Bridge calls it makes: `BridgeRGXCreateRenderTarget` = 0x21780 path (via 0x21780 "create
  parameter memory structures", not needed here), `BridgeRGXCreateHWRTData` = 0xcc18, called
  twice (loop at 0x225bc..0x226c2, r8 = 0..1, one per RT data).
- The 1.9 bridge (`generated/rgxta3d_bridge/common_rgxta3d_bridge.h`) has NO ISPMtileSize and NO
  region-header-size argument. Those are UM-side, written into the per-kick 3D command registers
  (same place the TQ writes them, see NOTES-brn65273.md: cmd+0x80 = ISP_MTILE_SIZE, cmd+0xa8 = ISP_RGN).

Params struct (sl): +0x00 flags (bit1, bit4), +0x04 global PB (PB+0x0c = PM MList bytes, PB+0x08 =
num/handle used for RgnHeader), +0x18 width, +0x1c height (both <= 4096), +0x20 msaa_x (u16),
+0x22 msaa_y (u16), +0x24 custom-sample-position flag, +0x28 custom positions, +0x30 layers (u16,
< 2048; passed to the bridge as ui16MaxRTs).

## 1. Tile / macrotile arithmetic (VERIFIED, 0x21fd4..0x2203a)
```
tiles_x = (width  + 15) >> 4            ; 0x21fde / 0x21ffc
tiles_y = (height + 15) >> 4            ; 0x21fd4 / 0x21ff0
sx = msaa_x (1|2), sy = msaa_y (1|2)    ; 0x21f54..0x21f98: (1,1)->TEAA 0, (1,2)->TEAA 4, (2,2)->TEAA 6, others rejected
tx = tiles_x * sx,  ty = tiles_y * sy   ; 0x22008 / 0x2200e  ("sample tiles")
num_tiles        = tiles_x * tiles_y    ; 0x22018   -> ui32MTileStride   (r9+0x5c)
num_sample_tiles = tx * ty              ; 0x2201c
```
Macrotile table written to r9+0x38..0x4c (vld1 from out+0x20 at 0x224ea, out zeroed by memset):
```
+0x38 = tiles_x   +0x3c = 0   +0x40 = 0      (x1, x2, x3)
+0x44 = tiles_y   +0x48 = 0   +0x4c = 0      (y1, y2, y3)
```
Register packing (0x22588..0x225ae, matches cr.xml TE_MTILE1/2 x1 [26:18], x2 [17:9], x3 [8:0]):
```
TE_MTILE1   = tiles_x << 18                       (x1 = num tiles X, x2 = x3 = 0)
TE_MTILE2   = tiles_y << 18                       (y1 = num tiles Y, y2 = y3 = 0)
TE_SCREEN   = (tiles_x - 1) | (tiles_y - 1) << 12 (0x2231c; NO round-up to 2)
PPP_SCREEN  = (width - 1) | (height - 1) << 16    (0x2232e..0x22336)
MTILE_STRIDE = tiles_x * tiles_y                  (r9+0x5c -> bridge [sp+0x44])
ISP_MTILE_SIZE = (tx & 0x3ff) << 16 | (ty & 0x3ff) (0x222f2..0x222fc -> r9+0x50; x [25:16], y [9:0])
TE_AA       = 0 / 4 (y) / 6 (x|y)                 (r9+0x30)
PPP_GRID_OFFSET = (flags & 2) ? 0x88 : 0          (0x21fe4; INFERRED: flipped/Y-origin variant)
```
So in v1 there is exactly ONE macrotile and it is the whole screen: 1 x 1 macrotiles of
tiles_x x tiles_y tiles. No 2x2 / 4x4 grouping, no 16-macrotile grid, no tile_max alignment.

## 2. Buffer sizes (VERIFIED)
```
rgn_per_rtdata = ALIGN(5 * num_sample_tiles, 64)              ; 0x2202e..0x22036  (5 bytes/tile)
   layers>1: rgn_per_rtdata = ALIGN(rgn, 4096) * layers, stride pages = ALIGN(rgn,4096)>>12 (r9+0x70)
RgnHeader alloc = 2 * rgn_per_rtdata, align 0x40, flags 3, name "RgnHeader" (0x22924..0x22944)
   RT data i gets base + i * rgn_per_rtdata (0x229cc..0x229ec)

tpc = 8 * P * P,  P = next_pow2(max(tx, ty))                  ; 0x22052..0x220ae  (8 B/tail ptr)
   layers>1: tpc = ALIGN(tpc, 4096) * layers; ui32TPCStride = ALIGN(tpc,4096)>>12 (r9+0x6c), else 0
ui32TPCSize = tpc                                             ; r9+0x130 -> bridge [sp+0x30]
TPC_RTC_Vheap alloc = ALIGN(tpc, 64) + (layers>1 ? 0x300 : 0) + 0x600, align 64, flags 0x80000003
   (0x22a0a..0x22a3e); TPC at base, VHEAP at base+ALIGN(tpc,64), RTC (layers>1) at VHEAP+0x600

MTA (macrotile array) size = 0 ([sp+0x17c] never written; ALIGN(2*0+15,16) = 16 B pad, 0x2279a)
MTA_MLIST alloc = 16 + 2 * mlist, mlist = PB+0x0c (0x227a4); MList[i] = base + 16 + i*mlist
   mlist (RGXCreateGlobalPB 0x21af2..0x21b1a) = ALIGN_UP(8 * (ceil(pb>>21) + ceil(pb>>30) + 1), 4096)
ISP merge (0x2233e..0x223a6): lower = 0.26795/{w,h}, upper = 1.73205/{w,h},
   scale = {w,h}*16/1.46410  (same constants as Mesa ROGUE_ISP_MERGE_*)
```

## 3. Worked example: 256 x 256, 1 sample, 1 layer
tiles 16x16, tx=ty=16, num_tiles=256.
```
TE_SCREEN      = 0x0000F00F
TE_MTILE1      = 0x00400000     (x1 = 16)
TE_MTILE2      = 0x00400000     (y1 = 16)
MTILE_STRIDE   = 256
ISP_MTILE_SIZE = 0x00100010     (16 x 16 tiles)
PPP_SCREEN     = 0x00FF00FF
TE_AA          = 0
rgn header / RT data = ALIGN(5*256, 64) = 1280 B  (alloc 2560 B, align 64)
TPC size       = 8 * 16 * 16 = 2048 B, TPC stride 0  (alloc 2048 + 0x600 = 0xE00)
MTA            = none
```

## 4. Diff vs mainline kernel (pvr_hwrt.c hwrt_init_common_fw_structure) and Mesa (pvr_arch_job_render.c)
Both assume `simple_parameter_format_version == 2` (kernel WARN_ON, Mesa assert) and compute,
for 256x256:
| item | kernel/Mesa (v2 rule) | vendor v1 (this binary) |
|---|---|---|
| mtile_x1/y1 | DIV_ROUND_UP(16,8)*2 = 4 | tiles_x = 16 |
| mtile_x2,x3,y2,y3 | 0 | 0 (same) |
| TE_MTILE1/2 | 4<<18 = 0x100000 | 16<<18 = 0x400000 |
| tile_max (TE_SCREEN) | ALIGN(n,2)-1 | n-1 (same when n even; differs for odd tile counts) |
| mtile_stride | 4*4 = 16 | 256 |
| ISP_MTILE_SIZE | 4<<16 \| 4 = 0x40004 | 0x100010 |
| macrotiles (rogue_get_num_macrotiles_xy) | 1x1 for v1 (already right) | 1x1 |
| rgn header bytes per RT data (Mesa pvr_rt_get_region_headers_stride_size) | (1*4)*(1*4)*5 = 80 -> ALIGN 64 = 128 | 1280 |
| TPC (pvr_rt_get_tail_ptr_stride_size) | pow2(4)^2*8 = 128 | 2048 |
| region_header_size ioctl (pvr_rt_get_isp_region_size) | 4*4*1*1 = 16 | n/a in 1.9 bridge (UM writes ISP_RGN per kick) |
| MTA | 0 (rogue_get_macrotile_array_size) | 0 (same) |
| ISP merge | same formulas | same |
| MList | per-freelist page-table formula | ALIGN(8*(pt+pd+1),4096) from global PB (equivalent in spirit) |

Everything in the right-hand column follows from ONE change: for v1 set
`mtile_x1 = num_tiles_x, mtile_y1 = num_tiles_y, tile_max = num_tiles-1` (kernel) and the same in
Mesa `pvr_arch_rt_mtile_info_init`; Mesa's rgn-header (5 B/tile, group_size 1) and TPC (8 B, no /4)
formulas then reproduce the vendor's 1280 / 2048 exactly. That the HW currently fills only the
top-left 64x64 px = 4x4 tiles = exactly one v2-sized macrotile is consistent with this.

## 5. Other per-RT things the vendor does
- Bridge argument order on the stack at 0x226a8 (for cross-checking): ... [sp+0x0c] PPPScreen,
  [0x10] PPPGridOffset, [0x18] PPPMultiSampleCtl(64), [0x20] TPCStride, [0x28] TailPtrs(64),
  [0x30] TPCSize, [0x34] TEScreen, [0x38] TEAA, [0x3c] TEMTILE1, [0x40] TEMTILE2, [0x44] MTileStride,
  [0x48..0x5c] ISPMerge L/U/S, [0x60] MaxRTs(u16), [0x64] ppsMemDesc, [0x68] puiHWRTData.
- Multi-sample control: 64-bit sample positions built in NEON (0x2217c..0x22216) from the table
  at 0x22b08/0x22b0c/0x22b10 (8-byte entries) unless params+0x24 says "custom positions at +0x28";
  a second, flipped copy is built at 0x22244..0x222ee (r9+0x28). Not decoded further.
- BRN63142 "RgnHeader" is this per-RT buffer (5 B/tile, 64-aligned); BRN65273 dummy INIA/INIB
  region-header init buffers are a separate per-device thing (NOTES-brn65273.md).
- ISP_RGN for the render kick is not set here (no bridge arg); NOTES-sipf-v1.md section 1g has
  the TQ-side value ([28:24] = min(entries,31)); the render path needs the same treatment.
