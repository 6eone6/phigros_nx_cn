/* nx_patch_phigros.h -- in-memory libunity.so / libil2cpp.so patch table for
 * PHIGROS  (Unity 2022.3.62f2 / rev 7670c08855a9, arm64, IL2CPP).
 * Game libunity.so BuildID xxHash 80afbcbeffe6459a.
 *
 * ===========================================================================
 * DERIVED FOR THIS GAME  (see PORTING_PHIGROS.md)
 *   - 17/17 memory-region sites          PHI_PATCH_WORDS
 *   - il2cpp JavaVM globals              PHI_IL2CPP_VM_GLOBAL / HANDLER_*
 *   - Swappy frame-pacing gate           PHI_PACING_GETTER
 *   - Boehm GC stop-the-world bridge     GC_*_OFF_PHI
 *
 * INHERITED-BUT-NOT-DERIVED  (gated OFF -- the Fruit Ninja / PvZ values were
 * for a DIFFERENT libil2cpp and would corrupt this one):
 *   - il2cpp IsInst guard, liveness guard, DataBinding/Dialogue frame map,
 *     Time.get_* hooks, TimeManager hook, splash bypass, FMOD output-select.
 *   Every one is behind a PHI_HAVE_* gate set to 0, with the symptom that
 *   means you need it documented at the gate. Offsets are 0 so a stale value
 *   can never be written.
 * ===========================================================================
 */
#ifndef NX_PATCH_PHIGROS_H
#define NX_PATCH_PHIGROS_H

#include <stdint.h>

/* ===========================================================================
 * 1. libunity memory-region granularity  -- DERIVED, 17/17, HIGH confidence
 * ===========================================================================
 * Unity's block allocator reserves memory in 256MB-aligned regions. On a 4GB
 * Switch that granularity does not fit the so_loader address space, so the
 * allocator's region-size computation is rewritten to 64MB. Each entry
 * rewrites one 32-bit instruction word.
 *
 * DERIVATION
 *   1. A SYMBOLIZED reference of the SAME source revision
 *      (libunity2022362f2.so, 2022.3.62f2_7670c08855a9, 119,590 FUNC symbols,
 *      PROGBITS .text) named the allocator functions.
 *   2. Participating instructions were enumerated by DECODING operands, not by
 *      grepping for a constant: move-wide 0x10000000, MOVN 0x0FFFFFFF, UBFM
 *      LSR/LSL/UBFX at either width, ADD/SUB lsl #28, and logical-immediate
 *      0xFFFFFFFFF0000000 / 0x0FFFFFFF.  -> 21 candidates in 180 functions.
 *   3. Each was pinned by masked-context search (ADRP/ADR/B/BL/B.cond/CBZ/TBZ/
 *      LDR-literal immediates masked out). 15 resolved UNIQUELY at the minimum
 *      3-instruction half-window, ALL byte-identical to the reference.
 *   4. The 4 that did not resolve were the two template instantiations of
 *      TLSAllocator<N>::ThreadInitialize, which emit IDENTICAL code -- the
 *      release build folded them (ICF) into one copy, so a 2-way-ambiguous
 *      reference pattern can never resolve uniquely by construction. A raw
 *      symbol-free scan found exactly one (LSR #28, MOVZ 0x10000000) adjacent
 *      pair, at 0x447258, inside the allocator cluster. -> sites 0 and 1.
 *   5. VERIFICATION
 *      - 15/15 symbol-derived sites byte-identical to the reference;
 *      - an independent raw scan re-found 15/15 of them;
 *      - all 17 offsets re-read from the shipped binary: 0 mismatches;
 *      - lineage cross-check: `from` words 0xd35c9c2a / 0xcb0a7108 /
 *        0xd35c9e89 are byte-identical to PvZ sites 12/16/20 and Fruit Ninja
 *        sites 10/11/15, and the computed `to` values match theirs exactly.
 *
 * DO NOT ADD the out-of-region raw-scan hits (0x7f08cc, 0x82f204, 0xc4bff4,
 * 0x112bc04, 0x1136e08 ...). They are unrelated code that happens to use
 * 0x10000000 or >>28. Every real allocator site is in 0x446000-0x450000.
 *
 * TRANSFORMS
 *     MOVZ/MOVK   imm16 0x1000<<16 -> 0x0400<<16     256MB -> 64MB
 *     LSR  (UBFM) immr 28 -> 26, imms 63 unchanged   >>28 -> >>26
 *     UBFX (UBFM) immr 28 -> 26, imms -= 2           field WIDTH preserved
 *     SUB/ADD shifted  lsl #28 -> lsl #26
 *     AND  bitmask  immr/imms += 2   0xFFFFFFFFF0000000 -> 0xFFFFFFFFFC000000
 *
 * SAFETY: nx_patch_libunity() is VERIFY-FIRST. It reads each target word and
 * only patches if it already equals {from}; if ANY site mismatches it patches
 * NOTHING and logs loudly. A wrong offset is caught, not catastrophic.
 */
typedef struct { uint32_t off, from, to; } NxPatchWord;

/* '=' byte-identical to the symbolized reference; '#' raw-scan (folded ICF). */
static const NxPatchWord PHI_PATCH_WORDS[] = {
  /*  0 # */ { 0x447258, 0xd35cfc28, 0xd35afc28 },  /* TLSAllocator<N>::ThreadInitialize (folded)   lsr  #28->#26        */
  /*  1 # */ { 0x44725c, 0x52a20009, 0x52a08009 },  /* TLSAllocator<N>::ThreadInitialize (folded)   movz 0x1000->0x0400  */
  /*  2 = */ { 0x4469b4, 0x12be0009, 0x12bf8009 },  /* LocalLowLevelAllocator::ReserveMemoryBlock   MOVN gran-1  <<< R2 */
  /*  3 = */ { 0x4469bc, 0x92648d36, 0x92669536 },  /* LocalLowLevelAllocator::ReserveMemoryBlock   and  ~(gran-1)       */
  /*  4 = */ { 0x448cb4, 0x52a20009, 0x52a08009 },  /* BucketAllocator::BucketAllocator             movz 0x1000->0x0400  */
  /*  5 = */ { 0x44af84, 0xd35cfd29, 0xd35afd29 },  /* DynamicHeapAllocator::DynamicHeapAllocator   lsr  #28->#26        */
  /*  6 = */ { 0x44af88, 0x52a2000a, 0x52a0800a },  /* DynamicHeapAllocator::DynamicHeapAllocator   movz 0x1000->0x0400  */
  /*  7 = */ { 0x44b430, 0x12be000a, 0x12bf800a },  /* DynamicHeapAllocator::RequestLargeAllocMem   MOVN gran-1  <<< R2 */
  /*  8 = */ { 0x44b438, 0x92648d36, 0x92669536 },  /* DynamicHeapAllocator::RequestLargeAllocMem   and  ~(gran-1)       */
  /*  9 = */ { 0x44d438, 0xd35cdc33, 0xd35ad433 },  /* VirtualAllocator::MarkMemoryBlocks           ubfx immr 28->26     */
  /* 10 = */ { 0x44d43c, 0xd35cfd15, 0xd35afd15 },  /* VirtualAllocator::MarkMemoryBlocks           lsr  #28->#26        */
  /* 11 = */ { 0x44d4cc, 0x52a20008, 0x52a08008 },  /* VirtualAllocator::ReserveMemoryBlock         movz 0x1000->0x0400  */
  /* 12 = */ { 0x44d808, 0xd35cfc28, 0xd35afc28 },  /* VirtualAllocator::GetMemoryBlockFromPointer  lsr  #28->#26        */
  /* 13 = */ { 0x44d818, 0x92646c28, 0x92667428 },  /* VirtualAllocator::GetMemoryBlockFromPointer  and 48-bit mask <<<R4 */
  /* 14 = */ { 0x44d820, 0xd35c9c2a, 0xd35a942a },  /* VirtualAllocator::GetMemoryBlockFromPointer  ubfx immr 28->26     */
  /* 15 = */ { 0x44d838, 0xd35cdc29, 0xd35ad429 },  /* VirtualAllocator::GetMemoryBlockFromPointer  ubfx immr 28->26     */
  /* 16 = */ { 0x44d83c, 0xf2a2000b, 0xf2a0800b },  /* VirtualAllocator::GetMemoryBlockFromPointer  movk 0x1000->0x0400  */
  /* 17 = */ { 0x44d87c, 0xcb0a7108, 0xcb0a6908 },  /* VirtualAllocator::GetMemoryBlockFromPointer  sub  lsl#28->lsl#26  */
  /* 18 = */ { 0x44d834, 0xb25c6feb, 0xb25e77eb },  /* VirtualAllocator::GetMemoryBlockFromPointer  -256*gran     <<<R5 */
  /* 19 = */ { 0x44d894, 0xd368fc28, 0xd366fc28 },  /* VirtualAllocator::GetBlockInfoFromPointer    L1 >>40->38   <<<R5 */
  /* 20 = */ { 0x44d8a4, 0xd35c9c29, 0xd35a9429 },  /* VirtualAllocator::GetBlockInfoFromPointer    ubfx immr 28->26     */
  /* 21 = */ { 0x44f558, 0xd368fc28, 0xd366fc28 },  /* MemoryManager::GetAllocatorContainingPtr     L1 >>40->38   <<<R5 */
  /* 22 = */ { 0x44f570, 0xd35c9e89, 0xd35a9689 },  /* MemoryManager::GetAllocatorContainingPtr     ubfx immr 28->26     */
};
#define PHI_PATCH_WORDS_N ((int)(sizeof(PHI_PATCH_WORDS)/sizeof(PHI_PATCH_WORDS[0])))

/* ===========================================================================
 * 1b. REGION SIZE HELD AS DATA -- the revision-3 hypothesis
 * ===========================================================================
 * Revisions 1 and 2 produced BYTE-IDENTICAL crashes: libunity+0x44b578, write
 * to reservation_base - 64MB. Decoding that address is what cracked it:
 *
 *     reservation base            0x6d44000000   (from the [mmap] log line)
 *     base & ~(256MB-1)           0x6d40000000   <- the faulting address
 *     base & ~(64MB-1)            0x6d44000000   <- what a PATCHED path gives
 *
 * The code that executed did a 256MB align-down. No patched instruction can
 * produce that, so the granularity the allocator actually used did not come
 * from any instruction in the table above.
 *
 * The .text table is complete. Revision 3 re-enumerated candidates INSIDE the
 * game's own allocator functions (revisions 1-2 enumerated in the symbolized
 * reference, which is a different build with different inlining, so anything
 * present only in the game was invisible) using a corrected UBFM rule -- the
 * old rule tested `immr==28 && imms>=2`, which on 32-bit matches `lsl w,w,#4`
 * and made the earlier whole-binary scans meaningless. That pass found NO
 * additional sites. So the remaining 256MB must be a stored value.
 *
 * libunity carries exactly two 64-bit 0x10000000 constants in .data.rel.ro:
 *     +0x119ab88   +0x11a8eb0
 * plus 32-bit copies at +0x11a8eac/+0x11a8eb0 and several in .rodata. These
 * are NOT relocated pointers -- the module is only ~19 MB, so a link-time
 * address of 256MB is out of range, which means they are genuine constants.
 *
 * PHI_DATA_REGION_PROBE logs these words at boot, before and after libunity's
 * constructors run, so the next debug.log says whether the allocator reads its
 * granularity from one of them. PHI_PATCH_DATA_REGION additionally rewrites
 * them to 64MB, verify-first.
 *
 * The patch is a HYPOTHESIS, not a derivation, so it is OFF by default: these
 * words are not name-pinned to the memory manager and could belong to anything
 * (a texture budget, a mesh limit). Run once with the probe alone and read the
 * log before enabling the write. */
#define PHI_DATA_REGION_PROBE   0   /* question answered by revision 4; keep for future builds */
#define PHI_PATCH_DATA_REGION   0   /* answered: those words are NOT the granularity -- see R4 */
static const uint32_t PHI_DATA_REGION_OFFS[] = { 0x119ab88, 0x11a8eb0, 0x11a8eac };
#define PHI_DATA_REGION_N ((int)(sizeof(PHI_DATA_REGION_OFFS)/sizeof(PHI_DATA_REGION_OFFS[0])))
/* If the loader logs fewer than 19 "[patch] ok" lines, or logs a mismatch, the
 * table is stale for your copy of the game. Re-run:
 *     python3 tools/derive_patch_table.py <symbolized-ref.so> <your-libunity.so>
 * A partial patch is the dangerous case -- half-transformed allocator
 * arithmetic is exactly what caused the revision-1 boot crash -- so the patcher
 * is all-or-nothing: any mismatch and it writes NOTHING. */

/* ---- branch forces: none located, none needed for first boot ------------ */
#define PHI_HAVE_BRANCH_FORCES 0
static const NxPatchWord PHI_BRANCH_FORCES[] = { { 0, 0, 0 } };  /* placeholder */
#define PHI_BRANCH_FORCES_N 0

/* ===========================================================================
 * 2. il2cpp JavaVM globals  -- DERIVED, HIGH confidence
 * ===========================================================================
 * libil2cpp's own JNI_OnLoad (0x1949f34) must not be called: its first action
 * is a log through a GOT slot the loader mis-binds. Its essential effects are
 * two .bss stores, replicated directly by main.c:
 *
 *   0x01949f34  stp  x30, x19, [sp,#-0x10]!
 *   0x01949f40  mov  x19, x0             ; x19 = JavaVM*
 *   0x01949f50  bl   0x3a1bc00           ; <- the unsafe log call we SKIP
 *   0x01949f54  adrp x0,  0x1949000
 *   0x01949f58  adrp x8,  0x3fa8000
 *   0x01949f5c  add  x0, x0, #0xf78      ; x0 = 0x1949f78 (handler fn)
 *   0x01949f60  str  x19, [x8, #0x480]   ; g_vm = vm        -> 0x3fa8480
 *   0x01949f64  bl   0x19bf0b4           ; setter:  adrp x8, 0x3fa9000
 *                                        ;          str  x0,[x8,#0x420] -> 0x3fa9420
 *   0x01949f68  mov  w0,#6 ; movk w0,#1,lsl #16    ; JNI_VERSION_1_6
 *
 * Both targets verified inside libil2cpp .bss (0x3f985e0-0x41bfb80).
 * Structurally identical to PvZ's +0x3c09c18/+0x3c0abe8 and Fruit Ninja's
 * +0x34f1e80/+0x34f2730 pairs; the VALUES are ours.
 */
#define PHI_IL2CPP_VM_GLOBAL      0x3fa8480  /* g_javavm            (.bss) */
#define PHI_IL2CPP_HANDLER_SLOT   0x3fa9420  /* g_jni_handler_fnptr (.bss) */
#define PHI_IL2CPP_HANDLER_FN     0x1949f78  /* value stored into the slot */
#define PHI_HAVE_IL2CPP_VM        1

/* ===========================================================================
 * 3. Swappy frame-pacing gate  -- DERIVED, HIGH confidence
 * ===========================================================================
 * This build registers Swappy natives (nOnChoreographer @0xc8d5d4,
 * nOnRefreshPeriodChanged @0xc8f8d4, nSetSupportedRefreshPeriods @0xc8f6f4),
 * so the frame-0 wall the PvZ log describes applies verbatim: engine init
 * parks in pthread_join because Swappy's ChoreographerFilter never gets a
 * Choreographer callback (there is no Java Choreographer here).
 *
 * `Swappy::IsEnabledAndActive()` was named in the symbolized reference at
 * 0x126f690 and pinned into this binary at 0x69c12c: 10-instruction masked
 * window, unique; immediate votes 3/3; opcode shape 20/24; and the pinned
 * disassembly is instruction-for-instruction identical to the reference apart
 * from the ADRP page and the two .bss byte offsets (0x231/0x230 here vs
 * 0xa11/0xa10 there) -- exactly the build-specific fields that are masked.
 *
 *   0x0069c12c  stp   x30, x19, [sp,#-0x10]!     <- the verify-first sentinel
 *   0x0069c130  adrp  x19, 0x1268000
 *   0x0069c134  ldrb  w8, [x19,#0x231]
 *   ...
 *
 * main.c overwrites it with `mov w0,#0 ; ret` after checking the first word is
 * 0xA9BF4FFE. Returning "pacing not active" is how the Zookeeper base already
 * boots -- it never enables Swappy.
 */
#define PHI_PACING_GETTER     0x69c12c   /* Swappy::IsEnabledAndActive() */

/* ===========================================================================
 * 4. Boehm GC stop-the-world bridge  -- DERIVED, HIGH confidence
 * ===========================================================================
 * libil2cpp has exactly TWO callers of pthread_kill -- GC_suspend_all and
 * GC_start_world -- which is the signature this derivation depends on.
 *
 *   GC_suspend_all  @ ~0x19e1f20:
 *     adrp x22,0x41bc000 ; add x22,x22,#0xd00      ; GC_threads[] -> 0x41bcd00
 *     ldr  x26,[x22, x21, lsl #3]                  ; 8-byte stride table walk
 *     ldr  x0, [x26,#8]                            ; GC_thread.id     +0x08
 *     ldr  x8, [x26,#0x10]                         ; .last_stop_count +0x10
 *     ldr  x9, [x23,#0xcd0]                        ; GC_stop_count -> 0x41bccd0
 *     ldr  w1, [x24,#0x4e4]                        ; suspend sig   -> 0x3f984e4
 *     bl   pthread_kill
 *
 *   GC_start_world  @ ~0x19e2180:
 *     ldr  w8, [x23,#0x4e0]                        ; retry_signals -> 0x3f984e0
 *     ldr  x9, [x26,#0xcd0] ; orr x9,x9,#1         ; GC_stop_count | 1
 *     ldr  w1, [x24,#0x4e8]                        ; restart sig   -> 0x3f984e8
 *     bl   pthread_kill
 *
 * Three consecutive 4-byte globals at +0x4e0/+0x4e4/+0x4e8 -- the same layout
 * PvZ found at 0x3bfbd40/44/48 and Fruit Ninja at 0x34e4680/84/88.
 *
 * The ack semaphore is il2cpp+0x41bcce0, confirmed THREE independent ways:
 *   sem_init    @0x19e22b8 : adrp x0, 0x41bc000 ; add x0,x0,#0xce0
 *   sem_getvalue@0x19e2aa4 : same materialisation
 *   poll loop   @0x19e2af8 : adrp x21,0x41bc000 ; add x21,x21,#0xce0
 * and it is that same x21 passed to sem_getvalue at 0x19e2b28.
 *
 * NOTE -- the Fruit Ninja header documents a double-add bug here (a derivation
 * script that wrote the computed value back into the SOURCE register applied
 * `add x0,x0,#0xc8` twice). The script used for these values keeps a separate
 * materialised-value map, so `add` is applied exactly once. Values were also
 * re-read from disassembly by hand, above.
 *
 * SECTIONS: the three signal words land in .data (0x3d38168-0x3f985d8) -- they
 * have nonzero initialisers, GC_suspend_sig defaults to 0x1e (SIGRTMIN-ish),
 * which is why they are not in .bss. The semaphore, stop-count and thread table
 * are in .bss (0x3f985e0-0x41bfb80). Fruit Ninja's header records the same
 * split. tools/verify_offsets.py checks each against the right section.
 *
 * If you ever see: black screen, watchdog "last frame=0", UnityMain parked in
 * GC_stop_world -> our sem_wait, and NO "[gc] stop-world suspend sig=N" line
 * in debug.log -- these have gone stale (game updated?). Re-run
 * tools/derive_gc_bridge.py.
 */
#define GC_RETRY_SIGNALS_OFF_PHI 0x3f984e0 /* .data GC_retry_signals           */
#define GC_START_ACK_OFF_PHI     0x3f984e0 /* alias of GC_RETRY_SIGNALS        */
#define GC_SUSPEND_SIG_OFF_PHI   0x3f984e4 /* .data GC_suspend_all signal      */
#define GC_RESTART_SIG_OFF_PHI   0x3f984e8 /* .data GC_start_world signal      */
#define GC_ACK_SEM_OFF_PHI       0x41bcce0 /* .bss GC_suspend_ack_sem          */
#define GC_STOP_COUNT_OFF_PHI    0x41bccd0 /* .bss GC_stop_count (ldar)        */
#define GC_RESTART_SEM_OFF_PHI   0x41bccf0 /* .bss handler waits here -- INFERRED
                                            * by layout (ack + 0x10, the same
                                            * delta Fruit Ninja has). Not
                                            * independently confirmed; used
                                            * only by the handler path.        */
#define GC_THREADS_OFF_PHI       0x41bcd00 /* .bss GC_threads[]                */
/* GC_thread field offsets, read directly off the walk above. Same layout as
 * Fruit Ninja / acpc: next +0x00, id +0x08, last_stop_count +0x10,
 * stack_ptr +0x18, flags bytes at +0x20/+0x21. */
#define GC_THREAD_ID_OFF        0x08
#define GC_THREAD_STOPCNT_OFF   0x10
#define GC_THREAD_STACKPTR_OFF  0x18

/* ===========================================================================
 * 5. NOT DERIVED FOR THIS GAME -- all gated OFF
 * ===========================================================================
 * Everything below was derived against Fruit Ninja's or PvZ's libil2cpp. Those
 * offsets are meaningless here and would write into unrelated code. They are
 * kept only so the tree compiles unchanged; every gate is 0 and every offset
 * is 0, so nothing can be written even if a gate is flipped without also
 * supplying values.
 */

/* ---- il2cpp IsInst null-klass guard --------------------------------------
 * SYMPTOM THAT MEANS YOU NEED THIS: a fault reading a small constant address
 * (~0x135) with the faulting pc inside il2cpp's assignability check, reached
 * from an object whose klass pointer is NULL.
 * TO DERIVE: find the assignability check's `and x8,x8,#~1` that is dead on
 * the obj->klass==0 fall-through path, and a nearby `mov w0,wzr` + identical
 * epilogue within CBZ's +-1MB range. */
#define PHI_HAVE_ISINST_GUARD     0
#define PHI_IL2CPP_ISINST_AND     0u
#define PHI_IL2CPP_ISINST_AND_OLD 0u
#define PHI_IL2CPP_ISINST_GUARD   0u

/* ---- liveness / DataBinding / Dialogue frame map -------------------------
 * Fruit-Ninja-specific crash forensics (its DialogueConfig held an
 * Il2CppClass* where an array was expected). Phigros has no such class. These
 * feed nx_exception_dump.c's symbolizer only -- with the gate off it simply
 * prints less. The three layout constants below are engine-generic and are
 * kept live because the dumper uses them for ordinary array/class walks. */
#define PHI_HAVE_LIVENESS_GUARD      0
#define PHI_IL2CPP_LIVENESS_ADD      0u
#define PHI_IL2CPP_LIVENESS_BODY     0u
#define PHI_IL2CPP_LIVENESS_ADD_HI   0u
#define PHI_IL2CPP_LIVENESS_ADD_LO   0u
#define PHI_IL2CPP_LIVENESS_HASPAR   0u
#define PHI_IL2CPP_LIVENESS_LR_INST  0u
/* Expected prologue words at PHI_IL2CPP_LIVENESS_ADD; only read under the
 * gate above, which is 0. Zeros here can never match a real site, so the
 * verify-first check refuses rather than patching. */
static const uint32_t PHI_LIVENESS_PROLOGUE[4] = { 0u, 0u, 0u, 0u };
#define PHI_IL2CPP_CFT_LR            0u
#define PHI_IL2CPP_GETTYPE_LEAF      0u
#define PHI_IL2CPP_ARRAY_DATA        0x20u  /* Il2CppArray  -- engine-generic  */
#define PHI_IL2CPP_ARRAY_LEN         0x18u
#define PHI_IL2CPP_KLASS_DEPTH       0x12Au /* Il2CppClass  -- engine-generic  */
#define PHI_IL2CPP_KLASS_TYPEHIER    0x0B8u
#define PHI_REFLECTIONTYPE_TYPE      0x10u
#define PHI_CFT_SP_DATABINDING       0u
#define PHI_CFT_SP_VALUE             0u
#define PHI_DATABINDING_DATAPATH     0u
#define PHI_DATABINDING_PROPNAME     0u
#define PHI_DATABINDING_STRFMT       0u
#define PHI_DIALOGUE_CONFIG          0u
#define PHI_DIALOGUE_INDEX           0u
#define PHI_DLGCONFIG_PIECES         0u
#define PHI_SDS_SP_DIALOGUE          0u
#define PHI_SDS_SP_PIECE             0u

/* ---- Time.get_* icall hooks / TimeManager -------------------------------
 * SYMPTOM: Time.deltaTime reads 0 or grows unboundedly; animations frozen or
 * running at wild speed while the render loop is clearly ticking.
 * TO DERIVE: Il2CppDumper the UnityEngine.Time icall thunks, then pin
 * TimeManager::Update via the symbolized reference.
 * FOR THIS GAME: Phigros is a rhythm game. If you enable these, validate
 * against audio time, not wall clock -- a Time hook that looks fine in the
 * menus will still drift a chart. */
#define PHI_HAVE_TIME_HOOKS      0
#define PHI_HAVE_TIME_FIX        0
#define PHI_TIME_UPDATE_ENTRY    0u
#define PHI_TIME_UPDATE_BODY     0u
#define PHI_TIME_UPDATE_WORD     0u
#define PHI_TIME_THUNK_WORD      0u
#define PHI_TIME_GETMANAGER      0u
#define PHI_IL2_get_deltaTime            0u
#define PHI_IL2_get_smoothDeltaTime      0u
#define PHI_IL2_get_unscaledDeltaTime    0u
#define PHI_IL2_get_time                 0u
#define PHI_IL2_get_unscaledTime         0u
#define PHI_IL2_get_realtimeSinceStartup 0u
#define PHI_IL2_get_timeSinceLevelLoad   0u
#define PHI_IL2_get_frameCount           0u

/* ---- FMOD -> OpenSL output select ---------------------------------------
 * NOT derived. AudioManager::InitFMOD is named in the symbolized reference at
 * 0x14a6dcc (2272 bytes) but did NOT pin into this binary -- the dev-build
 * reference's prologue differs, so the fingerprint has no unique match.
 *
 * IMPORTANT FOR THIS GAME: Phigros drives gameplay audio through Exceed7
 * NativeAudio (libnativeaudioe7.so) straight over OpenSL ES, NOT through FMOD.
 * opensles.c serves that path directly. FMOD covers UI/menu audio only, so a
 * silent FMOD is survivable for a first boot in a way it would not be for
 * either reference port.
 *
 * SYMPTOM THAT MEANS YOU NEED THIS: menu/UI sounds silent while chart audio
 * and hitsounds work.
 * TO DERIVE: locate the FMOD System::setOutput call site and force enum 22
 * (FMOD_OUTPUTTYPE_AUDIOTRACK -> our OpenSL shim). */
/* DERIVED. Unity picks the Android audio backend in AudioManager::InitNormal:
 *
 *     bl   AndroidAudio::GetAndroidAudioOutputType
 *     cmp  w0, #3 ; mov w8,#0x15 ; mov w9,#0x17 ; csel w8,w9,w8,eq
 *     cmp  w0, #2 ; mov w9,#0x16 ; csel w21,w9,w8,eq
 *     ldr  x0, [x19, #0x158]
 *     mov  w1, w21                 <- PHI_FMOD_OUTPUT_SITE
 *     bl   System::setOutput
 *
 * which also settles the enum this build uses: 0x15=21 AudioTrack,
 * 0x16=22 OPENSL, 0x17=23 AAudio. So the loader's movz w1,#22 is right, and a
 * plain FMOD_OUTPUTTYPE header (where OPENSL is 12) would have been wrong.
 *
 * WHY IT TOOK THREE ATTEMPTS: fingerprinting AudioManager::InitFMOD and
 * InitNormal both failed (6% opcode match -- the pin was garbage and I did not
 * patch on it). The 9-word sequence from the reference also found ZERO exact
 * matches here, because this build loads the FMOD system from [x19,#0x158]
 * where the reference uses [x19,#0x180] -- one differing word breaks an exact
 * match. Searching on the position-independent CONSTANTS instead (the 21/22/23
 * triple) found it immediately, and the 7-word mapping sequence occurs exactly
 * ONCE in the whole binary, so there is no ambiguity to resolve.
 *
 * Without this, Unity keeps AudioTrack output and hands its master mix to the
 * Java FMODAudioDevice -- which is a no-op here, so every Unity-side sound is
 * silent. Hitsounds still worked because Phigros routes those through Exceed7
 * NativeAudio, which drives our OpenSL shim directly. That split is exactly the
 * reported symptom: "all sound effects are perfect but no music plays". */
#define PHI_HAVE_FMOD_OPENSL        1
#define PHI_FMOD_OUTPUT_SITE        0x7c4844u
#define PHI_FMOD_WORDS_NUM          0
static const NxPatchWord PHI_FMOD_WORDS[] = { { 0, 0, 0 } };  /* placeholder */
#define PHI_HAVE_FMOD_BUFFER_BYPASS 0
#define PHI_FMOD_BUFFER_SITE        0u

/* ---- Unity splash bypass -------------------------------------------------
 * NOT derived. Fruit Ninja's GetShouldShowSplashScreen offset is meaningless
 * here. Phigros shows its own SaturnOS boot sequence; the native Unity splash
 * is short and harmless. Leave off unless it wedges.
 * SYMPTOM: boot parks forever on the Unity logo with the render loop ticking. */
#define PHI_BYPASS_UNITY_SPLASH        0
#define PHI_GET_SHOULD_SHOW_SPLASH     0u
#define PHI_FORCE_SPLASH_FINISH        0
#define PHI_IL2_SPLASH_FINISHED_CHECK  0u

/* ---- preload budget ------------------------------------------------------
 * Engine-generic knob (see config.h). The branch offset is game-specific and
 * is not derived; with it 0 the budget code degrades to a no-op. */
#define PHI_PRELOAD_EXIT_BRANCH        0u
#define PHI_PRELOAD_BUDGET_DEFAULT     0u
#define PHI_PRELOAD_BUDGET_TABLE_LOAD  0u

/* ---- il2cpp finish-flag diagnostic (PvZ offset -- not ours) ------------- */
#define PHI_HAVE_FINISH_PROBE    0
#define PHI_IL2CPP_FINISH_FLAG   0u

/* ---- managed-exception hook point ---------------------------------------
 * il2cpp_raise_exception is a REAL exported symbol in this build, so it can
 * never go stale -- resolve it by name with so_symbol() rather than hardcoding
 * an offset. Not installed by default. */

#endif /* NX_PATCH_PHIGROS_H */
