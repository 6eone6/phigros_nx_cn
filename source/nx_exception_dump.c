/* nx_exception_dump.c -- user exception handler: on any fault, dump symbolized
 * PC/LR + all GPRs + the faulting thread's stack frame + (for the UIGeometryJob
 * crash) the job data and element record, straight into debug.log. Then break so
 * Atmosphere's creport still fires. Every read is svcQueryMemory-guarded.
 *
 * Why: the recurring crash at libunity+0x871354 (UI::UIGeometryJob, str s0,[x8])
 * has a garbage destination pointer whose producer static analysis can't pin
 * down (two-mode 7KB function, partial [sp] slot visibility). This dumps the
 * ACTUAL frame: [sp+0x150] (the csel source), [sp+0x100/0x160/0x168] (channel
 * table), [sp+0x48/0x50] (element base + index), [sp+0x10] (out base),
 * [sp+0x28] (job data) -- ground truth in one crash.
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include "util.h"
#include "so_util.h"
#include "nx_patch_phigros.h"   /* PHI_IL2CPP_LIVENESS_* frame map (round 129) */
#include "jni_fake.h"              /* jni_approx_summary (round 130) */

/* libnx user exception handling: providing these symbols + the handler enables it */
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

static int xd_readable(uintptr_t addr, size_t len) {
  if (!addr || addr < 0x1000) return 0;
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == MemType_Unmapped) return 0;
    if ((mi.perm & Perm_R) == 0) return 0;
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

/* "libunity.so+0x871354" style annotation; falls back to raw hex */
static const char *xd_sym(u64 v, char *buf, size_t n) {
  so_module *m = so_find_module_by_addr((const void *)v);
  if (m)
    snprintf(buf, n, "%s+0x%lx", m->name, (unsigned long)(v - (uintptr_t)m->load_virtbase));
  else
    snprintf(buf, n, "%016lx", (unsigned long)v);
  return buf;
}

static void xd_dump_range(const char *tag, uintptr_t base, size_t bytes) {
  if (!xd_readable(base, bytes)) {
    debugPrintf("[xd] %s @%p: UNREADABLE\n", tag, (void *)base);
    return;
  }
  char s[64];
  for (size_t off = 0; off < bytes; off += 0x20) {
    const u64 *q = (const u64 *)(base + off);
    debugPrintf("[xd] %s+%03zx: %016lx %016lx %016lx %016lx\n",
                tag, off, (unsigned long)q[0], (unsigned long)q[1],
                (unsigned long)q[2], (unsigned long)q[3]);
    (void)s;
  }
}

/* Read an Il2CppClass's name/namespace.
 *
 * Layout (Unity 2022 il2cpp): +0x00 image, +0x08 gc_desc, +0x10 name,
 * +0x18 namespaze -- both const char*. The offsets we already rely on from the
 * faulting code corroborate this class layout (+0x130 typeHierarchyDepth,
 * +0x135 flags, +0xc8 typeHierarchy).
 *
 * The point: the CONTAINER in this crash has a perfectly valid klass, so naming
 * it says which managed type holds the field that points at metadata instead of
 * an object. That is the one thing still unknown. */
/* Print an il2cpp System.String field. Layout: +0x10 length (int32),
 * +0x14 UTF-16 chars. Used to read the identifying string field of the object
 * that owns the bad reference -- e.g. DialogueConfig.m_referenceId at +0x38 --
 * so the dump names the exact asset rather than just its type. */
static void xd_klass_name(const char *tag, u64 objptr);            /* fwd */
static void xd_managed_string(const char *tag, u64 strptr);        /* fwd */

/* Where does this pointer live? "type confusion" (a metadata pointer in a
 * managed field) and "dirty allocation" (leftover heap bytes) produce the exact
 * same fault, and the ONLY thing that separates them is which region the bad
 * value points into. Round 119 assumed the first without checking; say it. */
/* 0xDE is PHI_POISON_FREE's fill (nx_alloc.c). A pointer built out of poison
 * bytes is memory that was freed and then read -- use-after-free, named on
 * sight instead of inferred. A partly-poisoned word means the block was freed
 * and then partially rewritten. */
static const char *xd_poison_note(u64 v) {
  int n = 0;
  for (int i = 0; i < 8; i++) if ((u8)(v >> (i * 8)) == 0xDE) n++;
  if (n == 8) return "   <- ALL 0xDE: FREED MEMORY (use-after-free)";
  if (n >= 4)  return "   <- mostly 0xDE: freed then partly overwritten";
  return "";
}

static const char *xd_where(u64 v, char *buf, size_t n) {
  so_module *m = so_find_module_by_addr((const void *)v);
  if (m) {
    snprintf(buf, n, "inside %s (+0x%lx) -- METADATA/CODE, not a heap object",
             m->name, (unsigned long)(v - (uintptr_t)m->load_virtbase));
    return buf;
  }
  { extern int nx_arena_describe(const void *, unsigned *, int *, unsigned *);
    unsigned koff = 0, run = 0; int res = 0;
    if (nx_arena_describe((const void *)(uintptr_t)v, &koff, &res, &run)) {
      snprintf(buf, n, "our arena +%u KB, page %s, run %u pages",
               koff, res ? "RESERVED" : "FREE", run);
      return buf;
    } }
  snprintf(buf, n, "not a module and not our arena (newlib heap / GC block?)");
  return buf;
}

/* The LivenessState walk (Resources.UnloadUnusedAssets / FindObjectsOfTypeAll)
 * faulting inside add_process_object. Recover the CONTAINER and the field index
 * from the caller's frame -- see the frame map in nx_patch_phigros.h. */
/* The ConvertFromTo fault. Frame map and its cross-check: nx_patch_phigros.h.
 * Twenty rounds of this fault have described it as "from == 1"; none has said
 * WHICH binding. DataBinding carries the answer in three string fields. */
/* A fault INSIDE our own r129 veneer. The r133 log showed one and the dump
 * could not even name it -- it printed a bare `pc=00000059ffecd3b8` because the
 * veneer is in our NRO, not in a tracked module.
 *
 * This decodes better than any other site in the walk, because the veneer has
 * not yet touched the caller's registers: at a fault before .Lfnlg_orig,
 *   x0  = the candidate value read from the field
 *   x16 = its klass (already masked), x17 = klass+0x135
 *   x20 = traverse_object_instance's object -- THE CONTAINER, directly
 *   x22 = its field word index
 * No stack arithmetic, no frame assumptions.
 *
 * What this has to separate: r119/r128 had a NON-OBJECT in the field. The r133
 * fault has a perfectly good arena pointer in the field whose POINTEE has a
 * garbage klass -- corruption one level deeper. Whether that pointee sits on a
 * reused arena range is the question, so both addresses get classified. */
static void xd_veneer_triage(ThreadExceptionDump *ctx) {
#if PHI_HAVE_LIVENESS_GUARD
  extern void fn_liveness_guard(void);
  const uintptr_t vb = (uintptr_t)&fn_liveness_guard;
  const u64 pc = ctx->pc.x;
  if (pc < vb || pc >= vb + 0x40) return;
  char w[128], b[96];
  const u64 val = ctx->cpu_gprs[0].x, klass = ctx->cpu_gprs[16].x;
  const u64 cont = ctx->cpu_gprs[20].x, idx = ctx->cpu_gprs[22].x;

  debugPrintf("[xd] --- fault INSIDE the r129 liveness veneer, +0x%llx ---\n",
              (unsigned long long)(pc - vb));
  debugPrintf("[xd]   +0x18 is `ldrh w17,[klass+0x135]`: the guard cleared "
              "klass!=NULL and then dereferenced klass anyway\n");
  debugPrintf("[xd]   CONTAINER %s  field word %lu = +0x%lx\n",
              xd_sym(cont, b, sizeof b), (unsigned long)idx, (unsigned long)(idx * 8));
  debugPrintf("[xd]     container lives: %s\n", xd_where(cont, w, sizeof w));
  xd_klass_name("  container", cont);
  debugPrintf("[xd]   field value %016llx%s\n", (unsigned long long)val,
              xd_poison_note(val));
  debugPrintf("[xd]     value lives  : %s\n", xd_where(val, w, sizeof w));
  debugPrintf("[xd]   its klass   %016llx%s\n", (unsigned long long)klass,
              xd_poison_note(klass));
  debugPrintf("[xd]     klass lives  : %s\n", xd_where(klass, w, sizeof w));
  if ((klass & 0xffffffffull) == 0 && klass)
    debugPrintf("[xd]     klass has all 32 low bits ZERO -- 4 GB aligned, which "
                "no allocator returns. Looks like a torn 64-bit store: high half "
                "written, low half not\n");
  if (xd_readable((uintptr_t)val, 0x40)) xd_dump_range("  value", (uintptr_t)val, 0x40);
  if (xd_readable((uintptr_t)cont, 0x80)) {
    xd_dump_range("  container", (uintptr_t)cont, 0x80);
    xd_managed_string("  +0x38 (string)", *(const u64 *)(uintptr_t)(cont + 0x38));
  }
#else
  (void)ctx;
#endif
}

static void xd_databinding_triage(ThreadExceptionDump *ctx, u64 rel_pc) {
  extern uintptr_t g_il2cpp_base;
  if (rel_pc != (u64)PHI_IL2CPP_GETTYPE_LEAF) return;
  if ((ctx->lr.x - (u64)g_il2cpp_base) != (u64)PHI_IL2CPP_CFT_LR) {
    /* The GetType leaf is shared. Only ConvertFromTo's frame is mapped, so for
     * any other caller say so rather than decoding the wrong stack. */
    debugPrintf("[xd] --- GetType fault, but lr is not ConvertFromTo+0xe4 "
                "(libil2cpp+0x%llx): frame unknown, not decoding ---\n",
                (unsigned long long)(ctx->lr.x - (u64)g_il2cpp_base));
    return;
  }
  const uintptr_t sp = (uintptr_t)ctx->sp.x;
  char b[96];

  debugPrintf("[xd] --- DataBinding triage: raw value reached GetType ---\n");
  debugPrintf("[xd]   value (from) = %016llx  -- not an object; GetType read [%llx]\n",
              (unsigned long long)ctx->cpu_gprs[19].x,
              (unsigned long long)ctx->far.x);

  if (!xd_readable(sp + PHI_CFT_SP_VALUE, 8)) { debugPrintf("[xd]   frame unreadable\n"); return; }
  { const u64 framed = *(const u64 *)(sp + PHI_CFT_SP_VALUE);
    if (framed != ctx->cpu_gprs[19].x)
      debugPrintf("[xd]   NOTE frame value %016llx != x19 -- frame map is stale, "
                  "treat the fields below with suspicion\n",
                  (unsigned long long)framed); }

  if (xd_readable(sp + PHI_CFT_SP_DATABINDING, 8)) {
    const u64 db = *(const u64 *)(sp + PHI_CFT_SP_DATABINDING);
    debugPrintf("[xd]   DataBinding %s\n", xd_sym(db, b, sizeof b));
    xd_klass_name("  DataBinding", db);
    if (xd_readable((uintptr_t)db, 0x40)) {
      /* These three name the binding in the scene: which property is being
       * driven, and from which data path. That is the whole point. */
      xd_managed_string("  m_propertyName",
                        *(const u64 *)(uintptr_t)(db + PHI_DATABINDING_PROPNAME));
      xd_managed_string("  m_dataPath",
                        *(const u64 *)(uintptr_t)(db + PHI_DATABINDING_DATAPATH));
      xd_managed_string("  m_stringFormat",
                        *(const u64 *)(uintptr_t)(db + PHI_DATABINDING_STRFMT));
    }
  }

  /* toType is a System.RuntimeType; its Il2CppType -> klass -> name says what
   * the value was being converted TO, which pins down the expected box type. */
  { const u64 rt = ctx->cpu_gprs[20].x;
    if (xd_readable((uintptr_t)rt, 0x18)) {
      const u64 ty = *(const u64 *)(uintptr_t)(rt + PHI_REFLECTIONTYPE_TYPE);
      if (xd_readable((uintptr_t)ty, 8)) {
        const u64 k = *(const u64 *)(uintptr_t)ty;   /* Il2CppType.data.klass */
        if (xd_readable((uintptr_t)k, 0x20)) {
          const u64 np = *(const u64 *)(uintptr_t)(k + 0x10);
          debugPrintf("[xd]   toType -> %.60s\n",
                      (np && xd_readable((uintptr_t)np, 1))
                        ? (const char *)(uintptr_t)np : "?");
        }
      }
    } }
  debugPrintf("[xd]   a value type reached an `object` parameter unboxed; the "
              "binding named above is the one to look at in the scene\n");

  /* Round 133: the origin. Walk two more frames to Dialogue.SetDataSourceValues
   * and dump what it was actually reading -- see nx_patch_phigros.h for the
   * chain. This is the same DialogueConfig.m_dialoguePieces the r119/r129
   * liveness crashes were about, so the two are one bug.
   *
   * What we are testing: a `Sprite` reference field holding 1. A reference that
   * fails to resolve should be NULL, and this code null-checks everywhere
   * (GetCurrentDialoguePiece guards both the config and the array). It is not
   * safe against a non-null non-object. If several reference fields hold small
   * ascending integers, they are unresolved PPtr instance IDs -- i.e. the
   * deserializer left the raw ID instead of resolving it or writing null. */
  if (!xd_readable(sp + PHI_SDS_SP_PIECE, 8)) return;
  { const u64 dlg   = *(const u64 *)(sp + PHI_SDS_SP_DIALOGUE);
    const u64 piece = *(const u64 *)(sp + PHI_SDS_SP_PIECE);
    debugPrintf("[xd]   --- origin: Dialogue.SetDataSourceValues ---\n");
    debugPrintf("[xd]   Dialogue      %s\n", xd_sym(dlg, b, sizeof b));
    xd_klass_name("  Dialogue", dlg);
    debugPrintf("[xd]   DialoguePiece %s\n", xd_sym(piece, b, sizeof b));
    xd_klass_name("  DialoguePiece", piece);

    if (xd_readable((uintptr_t)piece, 0x68)) {
      xd_dump_range("  piece", (uintptr_t)piece, 0x68);
      /* Every reference field of DialoguePiece, flagged when it is too small to
       * be a pointer. m_characterIcon is +0x10 and is the one that faulted. */
      static const struct { unsigned off; const char *nm; } refs[] = {
        { 0x10, "m_characterIcon (Sprite)" }, { 0x18, "m_animatorParam (string)" },
        { 0x20, "m_dialogueText" },           { 0x38, "m_sound (ISound)" },
        { 0x48, "m_screenNameForProgressionList" },
        { 0x50, "m_screenNameForProgression (string)" },
      };
      for (unsigned i = 0; i < sizeof refs / sizeof refs[0]; i++) {
        const u64 v = *(const u64 *)(uintptr_t)(piece + refs[i].off);
        debugPrintf("[xd]     +0x%02x %-34s = %016llx%s\n", refs[i].off, refs[i].nm,
                    (unsigned long long)v,
                    (v && v < 0x10000ull) ? "   <- NOT A POINTER (instance id?)" : "");
      }
    }

    /* And the array itself, which is where r119 and r129 hit. */
    if (xd_readable((uintptr_t)dlg, 0x40)) {
      const u64 cfg = *(const u64 *)(uintptr_t)(dlg + PHI_DIALOGUE_CONFIG);
      const int  idx = *(const int *)(uintptr_t)(dlg + PHI_DIALOGUE_INDEX);
      debugPrintf("[xd]   m_dialogueConfig %s  index=%d\n", xd_sym(cfg, b, sizeof b), idx);
      xd_klass_name("  DialogueConfig", cfg);
      if (xd_readable((uintptr_t)cfg, 0x58)) {
        xd_managed_string("  config m_referenceId", *(const u64 *)(uintptr_t)(cfg + 0x38));
        const u64 arr = *(const u64 *)(uintptr_t)(cfg + PHI_DLGCONFIG_PIECES);
        debugPrintf("[xd]   m_dialoguePieces %s\n", xd_sym(arr, b, sizeof b));
        xd_klass_name("  pieces[]", arr);
        if (xd_readable((uintptr_t)arr, 0x40)) {
          const unsigned n = *(const unsigned *)(uintptr_t)(arr + PHI_IL2CPP_ARRAY_LEN);
          debugPrintf("[xd]     length=%u\n", n);
          for (unsigned i = 0; i < n && i < 6; i++) {
            const uintptr_t ep = (uintptr_t)arr + PHI_IL2CPP_ARRAY_DATA + i * 8u;
            if (!xd_readable(ep, 8)) break;
            const u64 e = *(const u64 *)ep;
            debugPrintf("[xd]     [%u] = %016llx%s\n", i, (unsigned long long)e,
                        (e && e < 0x10000ull) ? "   <- NOT A POINTER" : "");
          }
        }
      }
    }
  }
}

static void xd_liveness_triage(ThreadExceptionDump *ctx, u64 rel_pc) {
  if (rel_pc < PHI_IL2CPP_LIVENESS_ADD_LO || rel_pc > PHI_IL2CPP_LIVENESS_ADD_HI) return;
  extern uintptr_t g_il2cpp_base;
  const uintptr_t sp = (uintptr_t)ctx->sp.x;
  const u64 rel_lr = ctx->lr.x - (u64)g_il2cpp_base;
  char w[128], b[96];

  debugPrintf("[xd] --- liveness triage: fault inside add_process_object ---\n");

  /* The candidate object is the first argument, still in x19. */
  const u64 obj = ctx->cpu_gprs[19].x;
  if (xd_readable((uintptr_t)obj, 8)) {
    const u64 k = (*(volatile const u64 *)(uintptr_t)obj) & ~(u64)1;
    debugPrintf("[xd]   candidate object %s\n", xd_sym(obj, b, sizeof b));
    debugPrintf("[xd]     value lives   : %s\n", xd_where(obj, w, sizeof w));
    debugPrintf("[xd]     its klass ptr : %016lx -> %s\n",
                (unsigned long)k, xd_where(k, w, sizeof w));
    if (xd_readable((uintptr_t)k, 0x140)) {
      const u64 hier = *(volatile const u64 *)(uintptr_t)(k + PHI_IL2CPP_KLASS_TYPEHIER);
      const u8  dep  = *(volatile const u8  *)(uintptr_t)(k + PHI_IL2CPP_KLASS_DEPTH);
      debugPrintf("[xd]     typeHierarchy=%016lx depth=%u%s\n",
                  (unsigned long)hier, dep,
                  hier ? "" : "   <- NULL with a non-zero depth: NOT a set-up class");
    }
  }

  /* state->filter: which class the walk is selecting for. UnityEngine.Object
   * has depth 2, so a depth of 2 here confirms an UnloadUnusedAssets-style walk. */
  { const u64 state = ctx->cpu_gprs[20].x;
    if (xd_readable((uintptr_t)state, 0x10)) {
      const u64 filt = *(volatile const u64 *)(uintptr_t)(state + 8);
      if (xd_readable((uintptr_t)filt, 0x140)) {
        const u64 fnamep = *(volatile const u64 *)(uintptr_t)(filt + 0x10);
        const char *fname = (fnamep && xd_readable((uintptr_t)fnamep, 1))
                          ? (const char *)(uintptr_t)fnamep : "?";
        debugPrintf("[xd]   walk filter   : %016lx %.60s depth=%u"
                    "  (UnityEngine.Object == depth 2)\n",
                    (unsigned long)filt, fname,
                    *(volatile const u8 *)(uintptr_t)(filt + PHI_IL2CPP_KLASS_DEPTH));
      } else {
        debugPrintf("[xd]   walk filter   : %016lx (unreadable)\n", (unsigned long)filt);
      }
    } }

  /* The container. Only decodable when we know which caller we came from. */
  if (rel_lr == PHI_IL2CPP_LIVENESS_LR_INST && xd_readable(sp + 0x40, 0x10)) {
    const u64 container = ((const u64 *)(sp + 0x40))[0];
    const u64 idx       = ctx->cpu_gprs[22].x;
    debugPrintf("[xd]   CONTAINER %s  field word %lu = +0x%lx\n",
                xd_sym(container, b, sizeof b),
                (unsigned long)idx, (unsigned long)(idx * 8));
    debugPrintf("[xd]     container lives: %s\n", xd_where(container, w, sizeof w));
    xd_klass_name("  container", container);      /* names the managed type */
    if (xd_readable((uintptr_t)container, 0x80)) {
      xd_dump_range("  container", (uintptr_t)container, 0x80);
      /* Identify the asset, not just the type. DialogueConfig.m_referenceId
       * is at +0x38; if the container turns out to be something else the raw
       * dump above still shows every field. */
      xd_managed_string("  +0x38 (string)",
                        *(volatile const u64 *)(uintptr_t)(container + 0x38));
    }
  } else {
    debugPrintf("[xd]   caller lr=libil2cpp+0x%llx is not traverse_object_instance"
                " -- frame layout unknown, raw window follows\n",
                (unsigned long long)rel_lr);
    xd_dump_range("  frame", sp, 0x60);
  }
}

static void xd_managed_string(const char *tag, u64 strptr) {
  if (!strptr || !xd_readable((uintptr_t)strptr, 0x18)) return;
  const int len = *(volatile const int *)(uintptr_t)(strptr + 0x10);
  if (len <= 0 || len > 128) { debugPrintf("[xd]   %s: (len %d)\n", tag, len); return; }
  if (!xd_readable((uintptr_t)(strptr + 0x14), (size_t)len * 2)) return;
  const volatile uint16_t *u = (const volatile uint16_t *)(uintptr_t)(strptr + 0x14);
  char out[132];
  int n = 0;
  for (int i = 0; i < len && n < (int)sizeof out - 1; i++) {
    const uint16_t c = u[i];
    out[n++] = (c >= 32 && c < 127) ? (char)c : '.';
  }
  out[n] = 0;
  debugPrintf("[xd]   %s = \"%s\"\n", tag, out);
}

static void xd_klass_name(const char *tag, u64 objptr) {
  if (!objptr || !xd_readable((uintptr_t)objptr, 8)) return;
  u64 k = *(volatile const u64 *)(uintptr_t)objptr;
  k &= ~(u64)1;                                   /* drop the tag bit */
  if (!k || !xd_readable((uintptr_t)k, 0x140)) {
    debugPrintf("[xd] %s klass=%016llx (unreadable -- not a class)\n",
                tag, (unsigned long long)k);
    return;
  }
  const u64 namep = *(volatile const u64 *)(uintptr_t)(k + 0x10);
  const u64 nsp   = *(volatile const u64 *)(uintptr_t)(k + 0x18);
  const u8  flags = *(volatile const u8  *)(uintptr_t)(k + 0x135);
  const u8  depth = *(volatile const u8  *)(uintptr_t)(k + 0x130);
  const u64 hier  = *(volatile const u64 *)(uintptr_t)(k + 0xc8);
  char nm[64] = "?", ns[64] = "?";
  if (namep && xd_readable((uintptr_t)namep, 1))
    snprintf(nm, sizeof nm, "%.60s", (const char *)(uintptr_t)namep);
  if (nsp && xd_readable((uintptr_t)nsp, 1))
    snprintf(ns, sizeof ns, "%.60s", (const char *)(uintptr_t)nsp);
  debugPrintf("[xd] %s klass=%016llx  %s%s%s  flags=0x%02x depth=%u hierarchy=%016llx%s\n",
              tag, (unsigned long long)k, ns[0] && ns[0] != '?' ? ns : "", ns[0] && ns[0] != '?' ? "." : "",
              nm, flags, depth, (unsigned long long)hier,
              hier < 0x1000 ? "   <- BOGUS: not a real class" : "");
}


void __libnx_exception_handler(ThreadExceptionDump *ctx) {
  /* RE-ENTRANCY GUARD.
   *
   * This dumper walks pointers it got FROM a crash -- job data, GC thread
   * lists, stack words. Some of those are exactly the garbage that caused the
   * crash, so the dumper itself can fault. Without a guard the second fault
   * re-enters here, faults again at the same place, and the result is an
   * unbounded recursion that overflows the thread stack and comes out as an
   * Atmosphere "User Break" with a stack trace that is just this function
   * repeated ~32 times. That is what happened on the first packed boot: the
   * real fault was in libunity, but the console-level report pointed only at
   * __libnx_exception_handler.
   *
   * Depth 1: dump everything. Depth 2: dump the registers only -- no pointer
   * walking, since a pointer walk is what faulted. Depth 3+: get out of the
   * way and let the process die with the original fault intact. */
  static volatile int xd_depth = 0;
  const int depth = ++xd_depth;
  if (depth > 2) { xd_depth--; return; }
  if (depth == 2) {
    debugLogForceOn();
    debugPrintf("[xd] !! fault INSIDE the exception dumper "
                "(pc=%016lx far=%016lx esr=%08x)\n",
                (unsigned long)ctx->pc.x, (unsigned long)ctx->far.x, ctx->esr);
    debugPrintf("[xd] !! the dump above is truncated; the FIRST [xd] block is "
                "the real fault\n");
    debugLogFlush();
    xd_depth--;
    return;
  }

  /* A release build (DEBUG_LOG 0) logs nothing during play, but a fault is
   * always worth recording -- switch logging on so this dump reaches the card.
   * debug.log then contains just the [xd] block, which is the useful part. */
  debugLogForceOn();
  debugLogFlush();   /* persist everything logged before the fault */
  char b1[96], b2[96], b3[96];
  debugPrintf("[xd] ================= USER EXCEPTION =================\n");
  debugPrintf("[xd] desc=0x%x pc=%s far=%016lx esr=%08x\n",
              ctx->error_desc, xd_sym(ctx->pc.x, b1, sizeof b1),
              (unsigned long)ctx->far.x, ctx->esr);
  debugPrintf("[xd] lr=%s sp=%016lx fp=%016lx\n",
              xd_sym(ctx->lr.x, b2, sizeof b2),
              (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
  for (int i = 0; i < 28; i += 4)
    debugPrintf("[xd] x%-2d %016lx  x%-2d %016lx  x%-2d %016lx  x%-2d %016lx\n",
                i, (unsigned long)ctx->cpu_gprs[i].x,
                i + 1, (unsigned long)ctx->cpu_gprs[i + 1].x,
                i + 2, (unsigned long)ctx->cpu_gprs[i + 2].x,
                i + 3, (unsigned long)ctx->cpu_gprs[i + 3].x);
  debugPrintf("[xd] x28 %016lx\n", (unsigned long)ctx->cpu_gprs[28].x);

  /* frame-pointer backtrace (same walk as the watchdog) */
  uintptr_t fp = (uintptr_t)ctx->fp.x;
  for (int d = 0; d < 12 && fp; d++) {
    if (!xd_readable(fp, 16)) break;
    uintptr_t nfp = ((uintptr_t *)fp)[0];
    uintptr_t rlr = ((uintptr_t *)fp)[1];
    if (!rlr) break;
    debugPrintf("[xd]   bt[%d] %s\n", d, xd_sym(rlr, b3, sizeof b3));
    if (nfp <= fp) break;
    fp = nfp;
  }

  /* the whole stack frame: every [sp+slot] the crash block reads */
  uintptr_t sp = (uintptr_t)ctx->sp.x;
  xd_dump_range("SP", sp, 0x200);

  /* UIGeometryJob specifics (harmless if this is a different crash):
   * [sp+0x48]=element base, [sp+0x50]=element index (stride 0x70),
   * [sp+0x28]=job data, [sp+0x10]=output base. */
  if (xd_readable(sp + 0x58, 8)) {
    uintptr_t elem_base = ((uintptr_t *)(sp + 0x48))[0];
    uintptr_t elem_idx  = ((uintptr_t *)(sp + 0x50))[0];
    uintptr_t jobdata   = ((uintptr_t *)(sp + 0x28))[0];
    uintptr_t outbase   = ((uintptr_t *)(sp + 0x10))[0];
    debugPrintf("[xd] elem_base=%016lx idx=%lu jobdata=%016lx outbase=%016lx\n",
                (unsigned long)elem_base, (unsigned long)elem_idx,
                (unsigned long)jobdata, (unsigned long)outbase);
    if (elem_idx < 0x10000 && elem_base)
      xd_dump_range("ELEM", elem_base + elem_idx * 0x70, 0x70);
    if (jobdata) xd_dump_range("JOB", jobdata, 0x60);
  }
  /* Name the fault site against the guards we have installed, so a repeat is
   * instantly recognisable instead of needing another round of disassembly.
   * Every crash in this family has been "a value that is not an object reaches
   * an il2cpp type operation"; the sites differ, the shape does not. */
  { extern uintptr_t g_il2cpp_base;
    const u64 ib = (u64)g_il2cpp_base;
    const u64 rel_pc = ib ? (ctx->pc.x - ib) : ~0ull;
    if (ib && rel_pc < 0x370a000ull) {
      const char *what =
        (rel_pc == 0x159dfd4ull) ? "liveness klass->flags (the r119 site -- guard did not take)" :
        (rel_pc == (u64)PHI_IL2CPP_LIVENESS_HASPAR)
                                 ? "liveness HasParent klass->typeHierarchy[] "
                                   "(PAST the r119 guard -- guard took, value differed)" :
        (rel_pc == (u64)PHI_IL2CPP_GETTYPE_LEAF) ? "Object.GetType leaf   (DataBinding/ConvertFromTo path)" :
        (rel_pc == 0x1578a68ull) ? "class metadata walk   (il2cpp not ready)" :
        (rel_pc == 0x15b614cull) ? "type/cast check" :
        (rel_pc == 0x1609580ull) ? "tagged-pointer chase loop" : NULL;
      debugPrintf("[xd] site: libil2cpp+0x%llx%s%s\n",
                  (unsigned long long)rel_pc, what ? "  = " : "  (new site)", what ? what : "");
      if (ctx->far.x < 0x1000ull)
        debugPrintf("[xd] shape: NON-OBJECT reached a type operation "
                    "(far=0x%llx -- a small value used as an object pointer)\n",
                    (unsigned long long)ctx->far.x);
      xd_liveness_triage(ctx, rel_pc);
      xd_databinding_triage(ctx, rel_pc);
    }
  }

  /* Null-klass triage. A tiny `far` means "null + field offset": the object
   * pointer was fine but a pointer INSIDE it was zero. Print the candidate
   * object and a slice of its page, so the next report says whether the page
   * was wiped wholesale or just this one header is wrong. */
  if (ctx->far.x < 0x10000) {
    debugPrintf("[xd] --- null-deref triage (far=0x%lx = null + field offset) ---\n",
                (unsigned long)ctx->far.x);
    const u64 cand[4] = { ctx->cpu_gprs[0].x, ctx->cpu_gprs[1].x,
                          ctx->cpu_gprs[19].x, ctx->cpu_gprs[20].x };
    const char *nm[4] = { "x0", "x1", "x19", "x20" };
    for (int i = 0; i < 4; i++) {
      const uintptr_t p = (uintptr_t)cand[i];
      if (p < 0x1000 || !xd_readable(p, 0x40)) continue;
      char b[96];
      debugPrintf("[xd] %s=%s\n", nm[i], xd_sym(cand[i], b, sizeof b));
      xd_dump_range(nm[i], p, 0x40);
      /* how much of the surrounding page is zero? all-zero => wiped page,
       * a lone zero header => stale or freed reference */
      /* Is this OUR arena memory, and if so was it ever handed out? */
      { extern int nx_arena_describe(const void *, unsigned *, int *, unsigned *);
        unsigned koff = 0, run = 0; int res = 0;
        if (nx_arena_describe((const void *)p, &koff, &res, &run))
          debugPrintf("[xd] %s is ARENA +%u KB : page %s, run %u pages%s\n",
                      nm[i], koff, res ? "RESERVED (live allocation)" : "FREE (never handed out)",
                      run, res ? "  -> something zeroed a live range"
                               : "  -> the reference itself is garbage");
        else
          debugPrintf("[xd] %s is not in our arena\n", nm[i]);
        /* The history is what separates "corrupted under its owner" from
         * "freed and handed to somebody else". RESERVED alone cannot. */
        { extern void nx_arena_history(const void *);
          nx_arena_history((const void *)p); }
        /* Name the type. For the container this identifies the managed class
         * whose field is wrong; for the bad pointer it confirms it is not one. */
        xd_klass_name(nm[i], cand[i]);
        /* For the object that OWNS the bad field, dump enough of it to see every
         * field and decode the string ones. DialogueConfig's layout puts
         * m_referenceId at +0x38 and the broken m_dialoguePieces at +0x40. */
        if (xd_readable(p, 0x80)) {
          xd_dump_range(nm[i], p + 0x40, 0x40);
          const u64 s38 = *(volatile const u64 *)(uintptr_t)(p + 0x38);
          xd_managed_string("+0x38 (string)", s38);
          const u64 s50 = *(volatile const u64 *)(uintptr_t)(p + 0x50);
          xd_managed_string("+0x50 (string)", s50);
        } }
      const uintptr_t pg = p & ~(uintptr_t)0xFFF;
      if (xd_readable(pg, 0x1000)) {
        unsigned zeros = 0;
        const volatile u64 *w = (const volatile u64 *)pg;
        for (unsigned k = 0; k < 512; k++) if (w[k] == 0) zeros++;
        debugPrintf("[xd] %s page %016lx: %u/512 words zero (%s)\n",
                    nm[i], (unsigned long)pg, zeros,
                    zeros >= 500 ? "PAGE WIPED" :
                    zeros >= 64  ? "mostly zero" : "looks live");
      }
    }
  }

  /* Every fault in this family is a bad value that entered through the JNI
   * boundary and was accepted as truth. Printing the ledger here means the
   * crash log itself lists everything we approximated up to that point, and
   * which of those Unity actually read back -- no separate run needed. */
  /* GC soundness, in the same artifact as the corruption. A bailout means some
   * earlier collection finished its mark with mutators running, which is how a
   * live object gets swept and its memory reused -- the r134 shape. Reading this
   * off a heartbeat that may be thousands of lines earlier was costing a round
   * every time. */
  { extern int nx_gc_bailouts(void);
    extern void nx_gc_capture_stats(unsigned *, unsigned *, unsigned *,
                                    unsigned *, unsigned *);
    unsigned cok = 0, cnt = 0, cnh = 0, cnc = 0, cbr = 0;
    nx_gc_capture_stats(&cok, &cnt, &cnh, &cnc, &cbr);
    const int b = nx_gc_bailouts();
    const unsigned miss = cnt + cnh + cnc + cbr;
    debugPrintf("[xd] GC: captured=%u miss(thr/hnd/ctx/roots)=%u/%u/%u/%u "
                "bailouts=%d%s\n", cok, cnt, cnh, cnc, cbr, b,
                (miss || b) ? "   <- a thread's stack+registers did NOT reach the "
                              "mark; swept-while-reachable is on the table"
                            : "   (every thread was captured)");
  }
  { extern unsigned nx_throw_count(void);
    const unsigned tn = nx_throw_count();
    /* A managed throw unwinds through C++ cleanup landing pads, and the r134
     * wild write faulted inside one. If this is non-zero the fault may be on an
     * unwind path rather than in steady-state code. */
    debugPrintf("[xd] managed throws this session: %u%s\n", tn,
                tn ? "   <- see the [throw] lines for types; a fault may be on an unwind path"
                   : ""); }
  xd_veneer_triage(ctx);
  jni_approx_summary("at fault");
  debugPrintf("[xd] ============== END EXCEPTION DUMP ==============\n");

  /* Commit the dump BEFORE aborting. Without this the whole thing stays in
   * the log buffer and dies with the process -- which is exactly what
   * happened to the ModeSelect crash: Atmosphere caught the break with
   * "[xd] ===" still sitting in a register, and debug.log had no [xd] line
   * at all, so the faulting address was unrecoverable. */
  debugLogClose();

  /* re-raise so the process still aborts and Atmosphere writes its report */
  svcBreak(BreakReason_Panic, 0, 0);
  for (;;) svcSleepThread(1000000000ULL);
  debugLogFlush();
  xd_depth--;
}
