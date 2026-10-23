# MMU cache invalidation: stock 22.40 FW vs 22.68 FW vs mainline KM (2026-10-23)

Question: does the 22.68.54.30 FW, run on our 22.40.54.30 GE8300, invalidate the BIF MMU
caches correctly? Context: rare FRAG "Guilty lockup", BIF0 page fault at random VAs during
TQ 3D clears right after a scene freed lots of GPU memory.

Register names from vendor `hwdefs/km/rgx_cr_defs_km.h` (m1.9ED4893595):
- 0x3818 `SLC_CTRL_FLUSH_INVAL` (bit4 = DM_MMU), 0x3820 `SLC_STATUS0` (bit2 = FLUSH_INVAL_PENDING)
- 0x12A0 `BIF_CTRL_INVAL`: bit0 PT, bit1 PD, bit2 PC, bit3 TLB1
- 0x1348: not in the 1.9 defs; used for the PMTLB flag (bit4 of cmd flags), so PM TLB invalidate.
- KCCB flags (vendor rgx_fwif_km.h == mainline pvr_rogue_fwif.h, non-VIVT core):
  PT=0x1 PD=0x2 PC=0x4 PMTLB=0x10 TLB=0x18 INTERRUPT=0x4000000.

## 1. Stock 22.40 FW (VERIFIED, DWARF names)
`rgxfw_caches_mmu_flushinval` @c0005608 (arg $4 = flags), bank base `lui 0xcf40`:
```
c0005614  SLC_CTRL_FLUSH_INVAL(0x3818) = 0x10            ; DM_MMU
c000561e  rgxfw_try_crpoll_mask(0x3820, 0, 4)            ; wait FLUSH_INVAL_PENDING clear
c000562a  BIF_CTRL_INVAL(0x12a0) = flags & 7             ; PT|PD|PC, TLB1 bit dropped
c000562e  rgxfw_try_crpoll_mask(0x12a0, 0, flags & 7)    ; wait self-clear
c0005638  if (flags & 0x10 && gsPowCtl+8 & 2):           ; PMTLB, only if PM powered
c000564c    [0x1348] = 1 ; poll(0x1348, 0, 1)
c000565e  rgxfw_invalidate_mips_tlbs (0x9fc02759) ; mips_clean_dcache(0xcf000000, 0x8000)
```
`rgxfw_try_crpoll_mask` @c0006ebc: up to 0x7d0 iterations of `(CR & mask) == value`, else error path.
MMUCACHE is queued by `rgxfw_kernel_ccb_proc` and executed from
`rgxfw_irq_process_ready_queue` (call @c000b3f8, flags from cmd copy sp+0x2c), then:
write 1 to 0xbfc01080 + read back, read CR 0x20 (barrier), write the u16 sync value to the
sync address, raise host IRQ if flags bit26 (`ext $2,$2,0x1a,1` @c000b42e).

## 2. 22.68 FW (VERIFIED, stripped)
KCCB handler: `fw.asm` c0006fae..c0006fe2 (cmd+8 = cache_flags, cmd+0xc = sync fw addr,
cmd+0x10 = update value) calls c0003418 directly from KCCB processing (no ready queue), then
stores update value to sync addr (c0006fd0), IRQ if flags bit26 (c0006fd6).
c0003418, bank base `lui 0xcf80`:
```
c0003424  [0x3818] = 0x10 ; poll(0x3820, 0, 4)          ; 0x89c8(1, reg, val, mask)
c000343a  [0x12a0] = flags & 7 ; poll(0x12a0, 0, flags & 7)
c000344c  if (flags & 0x10 && [gp-0xcbc] & 2): [0x1348] = 1 ; poll(0x1348, 0, 1)
c0003478  [0xbfc01800] = 1 + readback, read CR 0x20, call 0x9fc02b9d (MIPS TLB inval)
```
Poll helper c00089c8: same 0x7d0-iteration loop as stock, timeout -> 0x882c (error path).
c00032a4 is the same body taking flags in $4 (internal callers); c0003304/3360/33bc are
fixed-flag variants (BIF_CTRL_INVAL = 4 = PC only) used at memctx (de)activation.

## 3. Difference that would matter on a 22.40 core? (VERIFIED: none)
Byte-for-byte the same register sequence: same SLC DM_MMU flush + wait, same
`BIF_CTRL_INVAL = flags & 7` + wait for self-clear, same PM TLB gating, same IRQ/sync semantics.
No MMU_CTRL (0xE000-range, VIVT-SLC) register is touched, so 22.68 does not use a register
that 22.40 lacks. Both FWs drop the TLB1 bit (0x8); identical behaviour, so not the cause.
The only structural difference: 22.68 runs the invalidate inline in KCCB processing, stock via
the ready queue. Both run before later KCCB kicks, so ordering w.r.t. jobs is preserved.
**Conclusion: the FW MMU-cache path is fine. Look elsewhere.**

## 4. Flags: mainline vs vendor KM
Vendor (`mmu_common.c` `_SetupPxE` -> `RGXMMUCacheInvalidate`, rgxmem.c:79): every PxE write
ORs the flag of the level written: PT entry -> PT|TLB, PD entry -> PD, PC entry -> PC. When an
unmap frees a page table it un-wires the parent entry through `_SetupPxE` (mmu_common.c:1500),
so PD/PC invalidation IS requested, and the table memory is freed only after the FW sync says
the invalidate ran (`_CleanupThread_FreeMMUMapping`).

Mainline (`pvr_mmu.c`, VERIFIED in code):
- Map: inserting an L0/L1 table raises sync to LEVEL_1/LEVEL_2 (load_tables, ~l.2122) -> PD/PC sent. OK.
- Unmap: `pvr_page_destroy` only ever requires LEVEL_0 (= PT|TLB|INTERRUPT). When the L0 table
  empties, `pvr_page_table_l0_remove` -> `pvr_page_table_l1_remove` clears the **PD entry**
  (and possibly `l2_remove` clears the **PC entry**) but nothing raises the sync level. Result:
  the flush after unmap sends **no PD and no PC invalidate**, then frees the L0/L1 table pages
  (`pvr_page_table_l0_free`) back to the page allocator. The BIF PD/PC cache can still point at
  the freed page; when it is reused, a GPU walk of that VA range reads garbage PTEs.
  (The error path at ~l.2111 does `l2_remove` + LEVEL_2, so the intent is clear; the normal
  unmap path just misses it.) Page-table pages are WC-mapped on non-coherent devices, so CPU
  cache cleaning is not an issue.
- INFERRED impact: a stale PDE/PCE only matters for GPU accesses to a VA range that is now
  unmapped (re-mapping the range sends PD/PC again). So it turns a GPU use-after-free into
  "random garbage" instead of a clean fault at a sane address. Plausible match, not proof.

## 5. Fault decode (VERIFIED against vendor defs, interpretation INFERRED)
MMU status 0x3041: FAULT=1, RO=0, DATA_TYPE=2, page size 4K, CAT_BASE(PC)=3.
Req status 0x00014e3b9c2f81c0: ADDRESS 0x3b9c2f81c0, **RNW=0 (write)**, TAG_ID 0xE = **MCU**,
TAG_SB 0x14 -> group enc 5 = **PDSRW** (1-cluster core). So a PDS-program store to an address
it read from memory. The garbage is a pointer the PDS loaded, i.e. data in a buffer (or stale
translation of it) is wrong, rather than a bad MMU walk of the faulting address itself.

## Recommendation (testable)
1. Kernel, one line, cheap: make unmap flush all MMU levels. In `pvr_mmu_op_context_destroy`:
   ```c
   if (flush_caches && !op_ctx->map.sgt) {
           pvr_mmu_flush_request_all(op_ctx->mmu_ctx->pvr_dev); /* PT|PD|PC|TLB */
           pvr_mmu_flush_exec(op_ctx->mmu_ctx->pvr_dev, true);
   }
   ```
   (Proper upstream fix: `pvr_mmu_op_context_require_sync(op_ctx, LEVEL_1)` in
   `pvr_page_table_l1_remove` and LEVEL_2 in `pvr_page_table_l2_remove`.) Worth upstreaming either way.
2. If the fault rate does not change, the MMU is not the cause: the PDSRW write points at
   Mesa/TQ handing the GPU a buffer that is freed or reused while a queued TQ clear still
   reads it (UMD lifetime / VM_UNBIND-before-fence bug). Next step there: log
   `pvr_mmu_unmap` ranges with timestamps and check whether the TQ job's PDS data/USC buffers
   were unmapped or recycled just before the fault.
No FW-side change is needed.
