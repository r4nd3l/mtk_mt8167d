# BRN65273 region-header buffers in libsrv_um.so (DDK 1.9, 32-bit ARM Thumb)

Binary: 40-sources/vendor-pvr-ddk/lib/libsrv_um.so. File offset == VA (single RX LOAD at 0).

## 1. Allocation: sub_19210 (0x19210..0x19288), called from PVRSRVCreateDeviceMemContext (0x190d0)

VERIFIED from disassembly (objdump) and Hopper pseudocode:

PVRSRVCreateDeviceMemContext(psDevConnection r4, &hDevMemCtx r8):
  - lock(r4+0x50); if (r4+0x54 refcount != 0) reuse, else allocate the devmem ctx (12 bytes) and store at r4+0x58.
  - 0x190f4: if (*(r4+0x5c) == NULL) sub_19210(*(ctx+8), "MMU INIA BRN65273", r4+0x5c)
  - 0x19106: if (*(r4+0x60) == NULL) sub_19210(*(ctx+8), "MMU INIB BRN65273", r4+0x60)
  - refcount++ ; *phDevMemCtx = *(r4+0x58)
  => TWO buffers, one per heap, held in the DEVICE CONNECTION struct at +0x5c (INIA) and +0x60 (INIB)
     as DEVMEM_MEMDESC* (not device VAs). Allocated once per connection (guarded by NULL check), i.e.
     once per process, before any render/transfer context exists. NOT per context.
  - Return value of sub_19210 is ignored: failure just leaves the slot NULL (the function stores NULL
     at 0x1925c on any error) and the connection is still usable.

sub_19210(hDevMemCtx, pszHeapName, DEVMEM_MEMDESC **ppsOut):
  0x1921a  bl 0x12076  = DevmemFindHeapByName(ctx, name, &hHeap)  (loops ctx->heaps[], strcmp)
           fail -> "BRN65273: Failed to get handle to MMU RgnHdr heap"
  0x1922a..0x19246  bl 0x12870 = DevmemAllocate-family (prints "DevmemSubAllocate" on error) with:
           r0 = 1 (leading bool, see below), r1 = hHeap,
           r2:r3 = uiSize  = 0x5 (64-bit {5,0})          <-- yes, FIVE bytes; rounds to one heap page
           [sp+0..7]  = uiAlign = 0x200 (64-bit {0x200,0})
           [sp+8]     = uiFlags = 0x3 = GPU_READABLE|GPU_WRITEABLE, GPU_UNCACHED(0<<8), CPU_UNCACHED,
                        NO cpu mapping bits, NO ZERO_ON_ALLOC(1<<31), NO POISON
           [sp+12]    = "BRN65273_RgnHeader"
           [sp+16]    = ppsOut
           fail -> "BRN65273: Failed to allocate device memory for region header"
  0x19268  bl 0x1312c = DevmemMapToDevice(*ppsOut, hHeap, &sDevVAddr /*sp+0x18*/)
           fail -> "BRN65273: Failed to map device memory for region header" (0x3c063) and
           0x19282 bl 0x122e4 = DevmemFree(*ppsOut), *ppsOut = NULL
  The returned sDevVAddr (stack slot sp+0x18) is NEVER read back by sub_19210: discarded.
  Nothing is written into the buffer by the CPU (no CPU_READABLE/WRITEABLE flags, no memset,
  no header words). The content is whatever the KM gives (flags lack ZERO_ON_ALLOC).

INFERRED: because this is the first (and only) allocation in a fresh, empty "MMU INIA/INIB"
heap (heap base 0xF800000000 / 0xF900000000, RA allocator), the device VA will be the heap base.
Vendor KM (rgxinit.c:3686) and FW get no explicit address for these: rgxfwutils.c only passes
PDS/USC exec bases for BRN65273. So the hardware/firmware must consume the fixed addresses
0xF800000000 / 0xF900000000 implicitly, or UM must write them into kick regs (checked below).

Arg-layout cross-check (VERIFIED): RGXAddRenderTarget 0x22924..0x22944 calls the same 0x12870 for the
BRN63142 "RgnHeader" with r0=1, r1=heap, r2:r3 = size (ip<<1 : 0), [sp]=align 0x40, [sp+8]=flags 3,
[sp+12]="RgnHeader", [sp+16]=&memdesc, and then 0x1312c MapToDevice with the VA output at sp+0x1f0 which
IS used afterwards. Same layout => BRN65273 alloc is really size 5, align 0x200, flags 3.

## 2. Where the VA goes: NOWHERE in UM (VERIFIED)
 - Only xrefs to sub_19210 are 0x19100 / 0x19112 (CreateDeviceMemContext). Only the heap-name strings
   xref there. The memdesc slots conn+0x5c / conn+0x60 are read only by PVRSRVDestroyDeviceMemContext
   (0x19320: 0x1936a / 0x1937a -> 0x12250 ReleaseDevVirtAddr + 0x122e4 DevmemFree).
 - No code in libsrv_um materialises 0xF8/0xF9 as a high word: the only "#248"/"#249" hits (0x18bdc,
   0x1cb6c) are string bytes in literal pools, not instructions.
 - RGXCreateTransferContext (0x2aca8) looks up heaps "PDS Code and Data", "USC Code", "TQ3DParameters",
   "General" (0x2ad92/0x2adde/0x2ae08/0x2ae3c) and caches base VAs via PVRSRVDevmemGetHeapBaseDevVAddr
   (0x194cc) in the TQ context at +0x6588 (PDS), +0x6580 (USC), +0x6590 (TQ3DParameters). It never
   looks up INIA/INIB. RGXCreateRenderContext (0x24d94) only does the BRN "Dummy Stencil Store" (0x252f8).
 - TQ3D kick regs (sub_26cac, 0x27660..0x27698): (cs_lo,cs_hi) = ctrl-stream VA at sp+0x3f0 MINUS the
   TQ3DParameters heap base (ctx+0x6590/+0x6594, 64-bit subtract). regs+0x88 = off_lo & ~3;
   regs+0x8c = (u8)off_hi; regs+0x90 = {0, heapbase_hi & 0xfc}; regs+0x78 = 2 (ISP_RENDER);
   => regs+0x88 is a heap-RELATIVE 40-bit control-stream address and regs+0x90 is the 4GB-aligned
   TQ3DParameters heap base (bits 34..39 only). Under BRN65273 the TQ heap base is 0, so regs+0x90 == 0.
   It is NOT the INIA/INIB address. (Second similar store at 0x2684e in sub_267b0: regs+0x90 = {x|0xa0000,0}, a
   different TQ path, also not INIA.)

### TQ kick regs decoded via FW DWARF (VERIFIED: rgx.fw.signed.22.40.54.30 is an unstripped MIPS ELF with
DWARF; struct _RGXFWIF_TRANSFERREGISTERS_, size 264, every field an 8-byte union uTransReg_*):
  +0x00 ISP_BGOBJVALS  +0x08 ISP_BGOBJDEPTH  +0x10 USC_PIXEL_OUTPUT_CTRL  +0x18..0x30 USC_CLEAR_REGISTER0..3
  +0x38 PDS_BGND0_BASE +0x40 PDS_BGND1_BASE +0x48 PDS_BGND2_BASE +0x50 PDS_BGND3_SIZEINFO
  +0x58 TPU_BORDER_COLOUR_TABLE +0x60 TPU_YUV_CSC_COEFFICIENTS +0x68 ISP_RENDER +0x70 ISP_MTILE_SIZE
  +0x78 ISP_MTILE_BASE +0x80 ISP_PIXEL_BASE +0x88 ISP_RENDER_ORIGIN +0x90 ISP_CTL +0x98 ISP_RGN +0xa0 ISP_AA
  +0xa8 EVENT_PIXEL_PDS_INFO +0xb0 EVENT_PIXEL_PDS_CODE +0xb8 EVENT_PIXEL_PDS_DATA +0xc0 PBE_WORDX_MRTY[]
The vendor UM offsets (regs+0x78 = 2, +0x88 cs base, +0x90, +0xa8 ISP_RGN) are cmd-relative with the regs
struct at cmd+0x10: cmd+0x78 = ISP_RENDER, cmd+0x80 = ISP_MTILE_SIZE (store at 0x26828 confirms),
cmd+0x88 = ISP_MTILE_BASE (heap-relative, 4-byte aligned), cmd+0x90 = ISP_PIXEL_BASE = TQ3DParameters
heap base with bits 0..33 cleared (4GB-aligned), cmd+0xa8 = ISP_RGN. So regs+0x90 is ISP_PIXEL_BASE,
not an INIA/INIB pointer. Under BRN65273 the TQ heap base is 0 => ISP_PIXEL_BASE = 0 and the MTILE base
is an absolute address in the 0..1GB PCE (that is why the BRN moves TQ3DParameters to 0).

## FW side (VERIFIED from DWARF): struct _RGXFW_CTL_ (FW-private, size 1856) has member
`bBRN65273AfterInit` at +1808 (0x710). FW also has RGXFWIF_DUMMY_RGNHDR_INIT_TAREGISTERS /
sDummyRgnHdrInitTARegs (TA cmd) and the log string "TE Region headers base 0x%08x%08x (RGNHDR Init: %d)".

## 4. HOW THE HW/FW CONSUMES THE BUFFERS (VERIFIED from FW disassembly; llvm-objdump-16 --mattr=+micromips)
FW is microMIPS; CR MMIO window base in FW VA = 0xcf400000; gsRGXFWCtl = 0xc002c440.
 - rgxfw_rgx_sidekick_init (0xc00005ac): 0xc000063a `sw $18(=1), 0x710($16=gsRGXFWCtl)` =>
   gsRGXFWCtl.bBRN65273AfterInit = TRUE unconditionally at FW init (store is in a bnez16 delay slot).
 - rgxfw_framework_kick_cdm (0xc0014554) [symbol name per ELF; it is the generic kick framework path]:
   0xc001456a loads bBRN65273AfterInit; if set, runs ONE-SHOT (0xc0014578..0xc001468c) then clears it:
     $22 = *(kick+0x28) & ~3        -> pointer to the 64-bit page-catalogue base of the kicked context
     CR 0x0CB0/0x0CB4 = 0 ; CR 0x0CE0 = 1 ; CR 0x0CB8 = 1 ; poll CR 0x0CB8 mask 1
     for idx in 0..7 (BIF_CAT_BASE0..7 at CR 0x1200 + 8*idx):
        save CR BIF_CAT_BASEidx ; CR BIF_CAT_BASEidx = PC base of the kicked context
        CR 0x1240/0x1244 (BIF_CAT_BASE_INDEX) : low &= ~7, |= idx
        CR 0x0C18 = 0 ; CR 0x0C20 = 1 ; CR 0x0C38:0x0C3C = {0x00000000, 0x000000F8}  (= 0xF8_00000000, INIA)
        CR 0x0C68 = 1 ; poll CR 0x0C68 mask 1
        CR 0x0C18 = 0 ; CR 0x0C20 = 1 ; CR 0x0C38:0x0C3C = {0x00000000, 0x000000F9}  (= 0xF9_00000000, INIB)
        CR 0x0C68 = 1 ; poll CR 0x0C68 mask 1
        restore CR BIF_CAT_BASEidx
     gsRGXFWCtl.bBRN65273AfterInit = 0
   The constants 0xf8 / 0xf9 are at 0xc00145d6 (`addiu $11,$zero,0xf8`) and 0xc0014644 (`addiu $11,$zero,0xf9`),
   written to CR 0x0C3C (high word), low word 0. This is the ONLY place the INIA/INIB addresses exist
   outside the heap tables. No UM or KM code passes them anywhere; the FW hardcodes the heap bases.
   => Consumer: the GPU's BIF/MMU, driven by a CR register sequence (0x0C18/0x0C20/0x0C38/0x0C68) that
   makes the hardware perform an access at GPU VA 0xF8_00000000 and 0xF9_00000000 under every page
   catalogue, once after FW init. The access must hit a VALID page or it faults (what we see on mainline).

Register naming for the one-shot (INFERRED; no full Rogue CR map in the local trees, mainline
pvr_rogue_cr_defs*.h only has TE_AA 0x0C00, TE_SCREEN 0x0C18, PPP 0x0CD0 in this block):
 0x0C18 = TE_SCREEN (verified name), 0x0C20 = TE_PSG (region_stride field, value 1 => stride 1 page),
 0x0C38 = TE_PSGREGION_ADDR (64-bit region header base; the only 64-bit reg in the sequence and it
 receives exactly 0xF8_00000000 / 0xF9_00000000), 0x0C68 = TE region-header-init trigger/status
 ("RGNHDR Init" in the FW log string "TE Region headers base 0x%08x%08x (RGNHDR Init: %d)"), polled to 0.
 0x0CB0/0x0CB8/0x0CE0 unidentified (VDM/TA block, written 0 / 1 / 1 before the loop).
 Reading: the TE's hardware region-header initialiser is pointed at the INIA then INIB buffers and
 fired once per BIF_CAT_BASE slot. The buffers themselves need only EXIST as valid, GPU-RW mapped
 pages at those two fixed VAs; nothing reads their content back from the CPU side.

## 3. Anything else keyed on BRN65273 in libsrv_um.so (VERIFIED)
Only the 6 strings listed in the task. No feature-bit test: UM keys on the presence of the heap NAME in
the heap table the KM exposes (DevmemFindHeapByName), so on a kernel without those heaps the whole thing
silently no-ops (the failure print is DebugPrintf level 2 and the return value is ignored).
"BRN Dummy Stencil Store" (0x403ea, RGXCreateRenderContext 0x252f8) is BRN62850 (FW symbol
rgxfw_3d_brn62850_dummy_stencil_store, gsRGXFWCtl.bBRN62850AfterInit at +0x70c). "RgnHeader" (0x22b98,
RGXAddRenderTarget) is the BRN63142 per-RT region header in the "RgnHdr BRN63142" heap. Both unrelated.

## Consequence for mainline (INFERRED)
The fault at 0xF8_00000000 on the first fragment job is the FW's own post-init one-shot (or the TE
writing region headers at that base) hitting an unmapped page because mainline's heap table has no INIA/
INIB heaps and nothing maps a page there. Mesa and the mainline KM have zero BRN65273 handling
(grep over luca-src drm/imagination and mesa src/imagination: no 65273). Minimal fix candidates:
 (a) KM: on quirk 65273, reserve the two 1 GB heaps and map one GPU-RW page at 0xF800000000 and
     0xF900000000 in every VM context (what the vendor UM does per connection via the 5-byte alloc);
 (b) also move General/Non4K/PDS/USC/VisTest/TQ heaps to the BRN65273 bases (each heap inside a single
     1 GB PCE; TQ3DParameters at 0) since the FW/KM pass PDS/USC exec bases and the FW assumes them.
Files: fw-22.40.54.30/rgx.fw.micromips.asm (llvm-objdump-16 --mattr=+micromips), funcs.txt, dwarf-info.txt.gz
