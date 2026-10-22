# Vendor DDK 1.9 TQ (libsrv_um.so, 32-bit ARM) vs Mesa: SIPF v1 ISP stream notes

Working notes, appended incrementally. "VERIFIED" = read from decompiled/disassembled code.
"INFERRED" = my reading of what it means.

## 0. Where the code lives (VERIFIED)

- `RGXQueueTransfer.c` (Hopper pseudo-C of `RGXQueueTransfer`, 34k lines) is the SUBMISSION
  loop (per-blit error paths inlined, `tq_params.txt` capture, sync append, kick). It does NOT
  itself pack the ISP words. Hopper renders the constant 0 as `libgcc` throughout (relocation
  confusion); read `x == libgcc` as `x == 0`.
- Line ~1024-1026: `r6 = *(var_4B38 * 4 + 0x156818); r0 = (r6)(var_4B40, r8);` -> the
  per-blit "route" (r8[3], loaded by `ldrd sl, r1, [r8,#8]` at line ~21) indexes a function
  table at 0x156818. Table contents (little-endian Thumb addrs):
  idx0..2 -> 0x2d56c (stub), idx3 -> 0x2685c, idx4 -> 0x25a70, idx5 -> 0x26cac.
  (Entries from idx6 on jump into 0x40db2.. which is a different switch table of the TQ
  context-create code; ignore.)
- Helper subs (REA decompiled):
  - `sub_2ac54(cb, flag)`: commit/rollback of two write cursors of a TQ circular buffer
    (`cb[1]<->cb[2]`, `cb[0x28c]<->cb[0x28d]`). Not a dword append.
  - `sub_13366(mem, &cpu_ptr, &dev_vaddr64)`: returns CPU map ptr (`*(*mem+0x20)`) and
    64-bit dev vaddr (`mem+8`) of an allocation.
  - `sub_355e8(n, dst)`: NEON 32-byte copy guarded by `n-1 <= 0xfb`.
  - `sub_32038`: SyncUtilSubmitDataAppend (sync prims), `sub_37e58`/`sub_37b38`: PLT thunks.
- Relevant strings in the binary: "TQCB:ISP parameter stream" (0x40bb6), "TQCB:DMA pool",
  "TQCB:USC code", "TQCB:PDS code", "TQ3DParameters" (0x2ed28, 0x40b6d), "tq 3d" (0x40ba2),
  "TQRouteOverride" apphint (0x2b3f8).

## STATUS (2026-10-21, run 3)
Run 1 and 2 were killed by host OOM (Hopper/REA heavy). Resuming with objdump-based disassembly of the route-prepare callees 0x2685c / 0x25a70 / 0x26cac (table 0x156818). Nothing below this line is yet confirmed.

## 1. The ISP primitive-block builder = function 0x2a0f8..0x2a7d6 (VERIFIED, objdump)

Called from 0x27484 and 0x276ae (two sibling TQ3D prepare functions, 0x26cac and the one
after 0x27654; both reached from route entry 0x2685c). Signature (ARM EABI):
`int build_prim_block(ctx r0, state r1, pds_info r2, src r3, mappings[] [sp], num_mappings
[sp+4], mapping_offset [sp+8], uint64 *out_dev_addr [sp+12])`. Mapping struct = 32 bytes:
+0..+12 src rect (x0,y0,x1,y1), +16 dst.x0, +20 dst.y0, +24 dst.x1, +28 dst.y1.
Returns 0 ok, 3 = coordinate out of range, 9 = CB alloc failed. NO feature-bit test anywhere
in this function: it is compiled for exactly one parameter format (INFERRED: per-BVNC DDK
build, so what it writes IS the GE8300 / SIPF v1 layout).

### 1a. Size + allocation (0x2a108..0x2a14e)
tsp_words_per_vertex = has_source ? (src[0x38]==2 ? 3 : 2) : 0   (3 = 3D/volume source)
bytes = 4*( ALIGN2(num_mappings) + 8*num_mappings ) + 16*num_mappings*tsp_words_per_vertex + 48
     = idx(ALIGN_POT(n,2) dw) + ISP verts(8 dw) + TSP + (4 dw ISP state + 8 dw PDS state)
Allocated with `sub_2a8e4(ctx, 4 /*cb id*/, bytes, &cpu, &dev)`; dev addr returned to caller.
Identical to Mesa `pvr_isp_primitive_block_size()` SIPF branch (idx ALIGN_POT 2, 2 dw/vertex,
isp_state 4 dw, pds 8 dw) EXCEPT 3D sources: vendor 3 dw/vertex (u,v,w), Mesa 4 (u,v,w,0).

### 1b. Block order (VERIFIED): [ISP state 16 B][index words][ISP vertices][PDS 8 dw][TSP]
Same order as Mesa's SIPF branch of `pvr_isp_primitive_block()`.

### 1c. ISP state / vertex-format words (0x2a170..0x2a1a2)  <-- v1 DIFFERS FROM SIPF2
dword0 = ((num_isp_vertices-1) & 0xF) << 5  |  (tsp_bytes_per_isp_vertex & 0x1FF) << 10
         where tsp_bytes_per_isp_vertex = (16*n*tsp_words)/(4*n) via __aeabi_uidiv (0x37e38)
         bit layout: [4:0]=0, [8:5]=vf_vertex_total (4 bits), [9]=0,
                     [18:10]=varying size per vertex in BYTES (9 bits), [31:19]=0
dword1 = 0
dword2 = 0x002E0000          (constant; decode against TA_STATE_ISPA below)
dword3 = 0 only if (cpu+12)&7 != 0 (8-byte pad), i.e. always a pad when the block is 8-aligned
Compare Mesa VERTEX_FORMAT_WORD_SIPF2 (64-bit): vertex_total at [5:2], primitive_total [10:6],
varying_vertex_BITS [22:11] (12 bits), isp_vtx_raw bit37, tsp_vtx_raw bit38,
isp_state_size [41:39] (=2), isp_format_word [63:42]; Mesa then writes ISPCTL(=0) and ISPA.
Vendor v1: no primitive_total, no raw flags, no isp_state_size, no format word, size in bytes
at [18:10], vertex_total at [8:5]; dword1 is zero. (dword1 could be the 32-bit ISPCTL; see 1g.)

### 1d. Index words (0x2a1d6 NEON pair loop / 0x2a30c scalar) -- MATCHES Mesa INDEX_DATA_WORDS_SIPF
One 64-bit word per 2 mappings, base idx = 8*k. Evaluated for k=0:
  low dword  = 0x04470820: tri0 = (0,1,2) in [3:0],[8:5],[13:10]; tri1 = (3,2,1) in [19:16],[24:21],[29:26]
  high dword = 0x14c718a4: tri2 = (4,5,6); tri3 = (7,6,5)
Edge/backface flag bits (4, 9, 14, 15 of each 16-bit half) = 0. Byte-identical to what
`pvr_isp_prim_block_index_block()` produces. (Masks 0x3800/0x1c0/0x1a0 confirm the 4-bit
fields at 0-3, 5-8, 10-13.)

### 1e. ISP vertex words (0x2a3ce..0x2a474) -- MATCHES Mesa ISP_VERTEX_WORD_SIPF, bias 2048
For each dst coordinate c: v = c + 2048; REQUIRE (v >> 12) == 0, else return 3
  -> vendor accepts biased values 0..4095 only (Mesa accepts up to 8191(+1)).
fixed = v << 4 (13.4, frac = 0).  Masks: x & 0x1FFF0, (y<<17) & 0xFFE00000, hi = ubfx(y_fixed,15,2)
  low dword  = x_fixed[16:0] | y_fixed[14:0] << 17
  high dword = y_fixed[16:15]        (z = 0 in bits 2..31)
Vertex order per mapping (8 B each, 32 B/mapping): (x0,y0) (x1,y0) (x0,y1) (x1,y1)
= Mesa's top-left, top-right, bottom-left, bottom-right. Identical.

### 1f. PDS state, 8 dwords (0x2a486..0x2a53c) -- same as Mesa pvr_isp_prim_block_pds_state
[0] SHADERBASE, [1] TEXUNICODEBASE, [2] SIZEINFO1, [3] VARYINGBASE, [4] TEXTUREDATABASE,
[5] UNIFORMDATABASE (vendor writes real addr, Mesa 0), [6] SIZEINFO2 with bit14
(pds_tri_merge_disable) ALWAYS set and usc_sharedsize = ALIGN16(x)/16 at [31:23],
[7] not written (pad to 64-bit). All addresses are heap-relative (`- ctx[0x6588]`) & ~15.

### 1g. TSP data (0x2a540..0x2a7c6)
Only if has_source. Per mapping 4 vertices x (u,v) floats (2D) or (u,v,w) (3D, src[0x38]==2);
vertex order permuted by rotation bits [17:16] of src[0x10] via table {0,2,0,3,1,3,1,2};
0.5 texel offsets via vmov.f32 0.5. End: `tst r1,#7; addne r1,#4` -> pad block to 8 bytes.

## 2. ISP control stream (VERIFIED, sub_26cac 0x273cc..0x27522)  <-- v1 DIFFERS FROM BOTH Mesa formats

Chunk ("region array") = 128 bytes, allocated at 0x27400 (`movs r2,#128; bl 2a8e4`), 16 entries
of 8 bytes each (`[sp+0x44] = 16` at 0x27468; each primitive entry advances the write ptr by 8,
0x274a6). Mesa also uses 32 dwords per region array, but byte/dword granular entries.

### 2a. PRIMITIVE entry (0x27494..0x274e6), two dwords written with one `strd r5, r0, [r3]`:
  dword0 (header) = ((1 << (2*n)) - 1) << 8      ; n = mappings in this block, 2 prims each
                  | (((2*n - 1) >> 2) & 7) << 5   ; computed as ((16n+24)&0xE0) + 0xE0, uxtb
                  | 1 << 3                         ; constant
      bit layout: [0] = 0 (1 = LINK entry, see 2b)    [1] = 0, set to 1 on the LAST entry = TERMINATE (0x2751c..0x27522: `*last |= 2`)
                  [2] = 0   [3] = 1 (constant for a primitive entry; INFERRED "prim base present"/valid)
                  [7:5] = (num_prims - 1) >> 2 (INFERRED: mask length in nibbles - 1, or prim_total>>2)
                  [15:8] = primitive mask, one bit per triangle, 2 per mapping, LSB first
                  [31:16] = 0
  dword1 (prim base) = ((hi32(addr) << 30) | (lo32(addr) >> 2)) | 1
      i.e. heap-relative 40-bit dev address >> 2 (classic-style shift 2, NOT the SIPF2 shift 3),
      with bit 0 forced to 1 (block is 8-byte aligned so bit0 of addr>>2 is free).
  Address = prim block dev addr minus heap base (`ldrd r2,r1,[sp+0x30]`, 64-bit subtraction).

### 2b. LINK entry (0x27428..0x27438), written into the slot saved in [sp+0x54]:
  dword0 = 1                                            (bit0 = LINK)
  dword1 = ((hi32 << 30) | (lo32 >> 2)) & ~1            (next 128-byte chunk addr >> 2, bit0 = 0)
  Mesa classic: 1 dword, cs_type=2 at [31:30], addr>>7 at [29:3].  Mesa SIPF2: 1 dword,
  ctrl_type=1 at [1:0], addr>>6 at [31:2].  Vendor v1: 2 dwords, type in bit0 of dword0,
  addr>>2 in dword1.  NONE of the Mesa variants match.

### 2c. TERMINATE: no separate word. Bit 1 of dword0 of the LAST primitive entry is set
  (0x2751a: `ldr r1,[sp+0x54]; ldr r0,[r1]; orr r0,#2; str r0,[r1]`). Mesa classic writes a
  separate cs_type=3<<30 dword; SIPF2 writes a separate byte 0x03. v1: flag in the last entry.

### 2d. First chunk: `strd r0(addr&~3), r1(hi&0xff)` into a 64-bit reg slot (0x27446..0x27460)
  = stream base register (CR_ISP_MTILE_BASE equivalent), 40-bit address, low 2 bits cleared.

## 3. ISP state words resolved (VERIFIED against Mesa ppp.xml STATE_ISPA)
dword2 = 0x002E0000 = STATE_ISPA { objtype=TRIANGLE(0) [31:28], passtype=OPAQUE(0) [26:24],
dwritedisable=1 bit21, dcmpmode=ALWAYS(7) [19:17] } = byte-identical to what Mesa's
`pvr_isp_prim_block_isp_state()` packs for read_bgnd=false. Hence the vendor v1 ISP-state
block is:  dw0 = 32-bit VERTEX FORMAT (v1), dw1 = STATE_ISPCTL (0), dw2 = STATE_ISPA, dw3 = pad.
Mesa SIPF2 writes: dw0-1 = 64-bit VERTEX_FORMAT_WORD_SIPF2, dw2 = ISPCTL, dw3 = ISPA.
=> On a v1 core Mesa's ISPA lands one dword too late, and the v1 format word is wrong.

## 4. Control-stream bookkeeping in sub_26cac (VERIFIED)
- 0x273de: `cmp r0,#4; movcc sl,r0` -> max 4 mappings (8 triangles) per primitive block
  (same as Mesa pvr_transfer_max_quads_per_pb() for SIPF).
- [sp+0x54] (link slot) init 0 at 0x26e7c; [sp+0x34] (was_linked) init 0 at 0x26ee2;
  [sp+0x40] (num prim entries) init 0 at 0x26ef0, ++ per block at 0x2748c.
- Free-entry count [sp+0x44] = 16 after each 128-byte chunk alloc; a new chunk is allocated
  when the count would drop to <= 1 (0x273e8 `cmp r1,#1; bhi`). The LINK pair is then written
  to [sp+0x54], which the code set to the slot of the LAST WRITTEN primitive (0x274ce).
  Read literally this overwrites the 15th primitive entry and leaves slot 15 unused; INFERRED:
  either a vendor latent bug that never triggers (>15 prim blocks = >60 mappings per TQ) or
  my trace of [sp+0x54] misses a store. Hardware-format conclusion (LINK = {1, addr>>2})
  is unaffected.
- Registers (r3 = regs struct, [sp+0x78]) written for a stream (0x27446..0x27460, sibling
  0x27682..0x27698): regs+0x78 = {2,0} (64-bit; INFERRED CR_ISP_RENDER mode FAST_2D),
  regs+0x88 = {stream_addr & ~3, hi & 0xff} (64-bit; CR_ISP_MTILE_BASE, heap-relative),
  regs+0x90 = {0, hi & 0xfc}, regs+0xa8 = { min(num_prim_entries, 31) << 24 , 0 }, or
  0x1f000000 (=31) when the stream was linked (0x2752a..0x27550).
  INFERRED: regs+0xa8 is CR_ISP_RGN with a 5-bit "control stream size in entries" field at
  [28:24]; Mesa writes isp_rgn = 0 for a non-SIPF2 core (pvr_arch_job_transfer.c ~4575).

## 5. Kick registers (VERIFIED values, register identity INFERRED from Mesa cr.xml)
regs+0x78 = 2        -> CR_ISP_RENDER.mode_type = FAST_2D (2)            (Mesa does the same)
regs+0x88 (64-bit) = heap-relative stream base, bits [1:0] cleared, hi & 0xff
                     -> CR_ISP_MTILE_BASE addr [39:2]                    (Mesa does the same)
regs+0xa8 = min(num_ctrl_stream_entries, 31) << 24, or 31 << 24 if the stream was linked
                     -> CR_ISP_RGN_SIPF.cs_size_ipf_creq_pf [28:24] (Mesa cr.xml line 497-510,
                        MAX 31). Mesa (pvr_arch_job_transfer.c ~4575) writes CR_ISP_RGN = 0 on a
                        core without simple_internal_parameter_format_v2, i.e. cs_size = 0.
                        On v1 the field evidently means "entries in the first 128-byte chunk;
                        31 = full/linked", so Mesa tells the ISP the stream has zero entries.

## 6. Format selection (VERIFIED: none at runtime)
Neither 0x2a0f8 (prim block) nor the control-stream code in sub_26cac/0x2773c tests a feature
bit; the bias/overflow check in 0x2a0f8 compares against a hard-coded register value and the
entry layouts are hard-coded. INFERRED: DDK 1.9 is built per BVNC with RGX_FEATURE_* as
compile-time defines, so this libsrv_um.so contains ONLY the GE8300 (SIPF v1) encoder.
The only runtime switch is the route table at 0x156818 (3D vs 2D/TLA/SW paths).

## 7. Summary: v1 (vendor) vs Mesa classic vs Mesa SIPF2
| item | vendor v1 (GE8300) | Mesa classic | Mesa SIPF2 |
|---|---|---|---|
| ctrl stream entry | 8 bytes (2 dw), 16 per 128-B chunk | 1-2 dw, 32 dw chunk | byte granular |
| PRIM header dw0 | bit3=1, [7:5]=(nprims-1)>>2, [15:8]=prim mask (2 bits/mapping) | cs_type 0<<30, state_read, state_size, prim_total, mask_fmt, base_pres, base_offset | byte: type0, base_size bit1, mask bytes [3:2], valid tiles [7:4]; then base; then mask byte |
| PRIM base dw1 | (addr>>2) \| 1 | addr>>2 | addr>>3 |
| LINK | dw0=1, dw1=(addr>>2)&~1 | 2<<30 \| addr>>7 at [29:3] | byte 1 \| (addr>>6)<<2 |
| TERMINATE | bit1 of last PRIM dw0 | separate dw 3<<30 | separate byte 3 |
| ISP state (16 B) | VF32, ISPCTL(0), ISPA(0x2e0000), pad | ISPCTL, ISPA, comp-size, comp0, comp1 | VF64, ISPCTL, ISPA |
| vertex format word | 32-bit: [8:5]=vtx_total, [18:10]=varying bytes/vertex | ISP_COMPRESSION_WORD_1 | 64-bit SIPF2 word |
| index words | 64-bit, 4 x (3 x 4-bit) | 6-bit, 3 dw / 4 tris | identical to v1 |
| ISP vertex | 17/17/30, 13.4, bias 2048, range check v<4096 | 24/24/32 | identical to v1 (range 8192) |
| PDS state | 8 dw, tri_merge_disable always set | 7 dw | 8 dw |
| TSP 3D source | 3 dw/vertex (u,v,w) | 4 dw (u,v,w,0) | 4 dw |
| CR_ISP_RGN | [28:24] = min(entries,31) | 0 | bit0 = linked (ipf_creq_pf) |
| max mappings/block | 4 | 16 | 4 |

## 8. Minimal Mesa change list (INFERRED from the above, untested)
1. pvr_isp_prim_block_isp_state(): for SIPF v1 write ONE dword
   `((nverts-1)&0xF)<<5 | (tsp_bytes_per_vertex&0x1FF)<<10`, then ISPCTL, ISPA, pad dword.
2. pvr_isp_ctrl_stream(): v1 entries are 8 bytes: dw0 = 0x8 | (((2n-1)>>2)<<5) | (((1<<(2n))-1)<<8),
   dw1 = ((prim_blk_addr >> 2) | 1) (40-bit addr: hi<<30 | lo>>2). Link = {1, (next>>2)&~1}.
   No terminate word: OR 0x2 into dw0 of the last primitive entry. Reserve 1 slot/chunk for link.
3. Set CR_ISP_RGN [28:24] = MIN(num_entries, 31) (31 when linked).
4. pvr_isp_prim_block_tsp_vertex_block(): 3D source -> 3 dwords per vertex, no zero pad word.
5. pvr_isp_primitive_block_size(): adjust for (4).
Everything else in the SIPF branch (index words, vertex words, PDS block, order, 4 quads/block,
8-byte alignment) already matches the vendor.

Addendum: the string "22.40.54.30" sits at 0x2d2cc next to "capture bvnc=%s" (0x2d2b8): the library is built for this one BVNC (supports section 6).
