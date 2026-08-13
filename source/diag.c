/* diag.c -- see diag.h. Frame-1 black-hang instrumentation.
 *
 * This software may be modified and distributed under the terms of the MIT
 * license. See the LICENSE file for details.
 */
#define _GNU_SOURCE
#include <switch.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

#include "diag.h"
#include "util.h"   /* debugPrintf */
#include "so_util.h" /* so_find_module_by_addr for backtrace symbolication */

/* ------------------------------------------------------------------ tunables */
#define DIAG_MAX_THREADS   96
#define DIAG_POLL_NS       (1000ull * 1000ull * 1000ull)   /* watchdog tick: 1s */
#define DIAG_STALL_NS      (6000ull * 1000ull * 1000ull)   /* declare stall: 6s  */
#define DIAG_REDUMP_NS     (6000ull * 1000ull * 1000ull)   /* re-dump cadence    */

typedef struct {
  volatile int          in_use;
  uint64_t              tid;            /* svcGetThreadId — matches crash reports */
  Handle                handle;         /* real thread handle for svcGetThreadContext3 */
  pthread_t             pth;            /* host handle, for setname target match  */
  char                  name[32];
  const void           *entry;
  int                   is_main_engine;
  /* live wait beacon */
  volatile int          wait_kind;
  volatile const void  *wait_obj;
  volatile uint64_t     wait_since;     /* tick (raw) when current wait began     */
  /* liveness counters */
  volatile uint64_t     waits_total;
  volatile uint64_t     wakes_total;
  volatile uint64_t     futex_spins;
  volatile uint64_t     last_active;    /* tick of last beacon activity           */
} DiagThread;

static DiagThread       g_threads[DIAG_MAX_THREADS];
static Mutex            g_reg_lock;     /* zero-init libnx Mutex == unlocked       */
static __thread DiagThread *self;       /* this thread's slot (host TLS)           */

static volatile int      g_frame = -1;
static volatile uint64_t g_last_progress;   /* tick of last diag_frame()           */
static volatile int      g_wd_started;
static Thread            g_wd_thread;

/* ------------------------------------------------------------------ helpers */
static inline uint64_t now_tick(void) { return armGetSystemTick(); }
static inline uint64_t tick_to_ns(uint64_t t) { return armTicksToNs(t); }

/* How many registered threads are NOT parked in a wait right now, excluding
 * the caller. The GC bridge acks stop-the-world without suspending anyone,
 * on the assumption that this is always 0 during a mark. */
/* Pause every registered thread except the caller. Returns how many were
 * paused and fills `out` with their handles for diag_resume_list().
 * Used only by the GC stop-the-world bridge. */
int diag_pause_others(Handle *out, int max) {
  int n = 0;
  for (int i = 0; i < DIAG_MAX_THREADS && n < max; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use || !t->handle) continue;
    if (pthread_equal(t->pth, pthread_self())) continue;   /* never self */
    if (R_SUCCEEDED(svcSetThreadActivity(t->handle, ThreadActivity_Paused)))
      out[n++] = t->handle;
  }
  return n;
}

void diag_resume_list(const Handle *list, int n) {
  for (int i = 0; i < n; i++)
    svcSetThreadActivity(list[i], ThreadActivity_Runnable);
}

int diag_running_threads(void) {
  int n = 0;
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    if (pthread_equal(t->pth, pthread_self())) continue;   /* the acker */
    if (t->wait_kind == 0) n++;                            /* 0 == running */
  }
  return n;
}

static const char *wait_kind_name(int k) {
  switch (k) {
    case DIAG_W_COND:   return "cond_wait";
    case DIAG_W_JOIN:   return "join";
    case DIAG_W_SEM:    return "sem_wait";
    case DIAG_W_MUTEX:  return "mutex_lock";
    case DIAG_W_RWLOCK: return "rwlock";
    case DIAG_W_FUTEX:  return "futex_spin";
    default:            return "running";
  }
}

static DiagThread *slot_alloc(void) {
  mutexLock(&g_reg_lock);
  DiagThread *t = NULL;
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (!g_threads[i].in_use) { t = &g_threads[i]; break; }
  }
  if (t) {
    memset(t, 0, sizeof(*t));
    t->in_use = 1;
    t->pth = pthread_self();
    t->handle = threadGetCurHandle();   /* real handle, usable from the watchdog */
    uint64_t tid = 0;
    if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) t->tid = tid;
    t->last_active = now_tick();
  }
  mutexUnlock(&g_reg_lock);
  return t;
}

/* Return this thread's slot, lazily allocating one if it ran code we didn't
 * trampoline (e.g. the process main thread). Never returns NULL unless the
 * registry is full (then beacons silently no-op). */
static DiagThread *diag_self(void) {
  if (self) return self;
  DiagThread *t = slot_alloc();
  if (t && t->name[0] == 0) {
    /* default label until a real name arrives */
    snprintf(t->name, sizeof(t->name), "T%llu", (unsigned long long)t->tid);
  }
  self = t;
  return t;
}

/* ------------------------------------------------------------------ public */
void diag_thread_register(const void *entry, int is_main_engine) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->entry = entry;
  t->is_main_engine = is_main_engine;
  if (is_main_engine && t->name[0] == 'T')   /* keep until Unity renames it */
    snprintf(t->name, sizeof(t->name), "engine_main");
}

void diag_thread_unregister(void) {
  if (!self) return;
  mutexLock(&g_reg_lock);
  self->in_use = 0;
  mutexUnlock(&g_reg_lock);
  self = NULL;
}

void diag_set_name(void *target_pthread, const char *name) {
  if (!name) return;
  DiagThread *t = NULL;
  if (target_pthread) {
    pthread_t want = (pthread_t)target_pthread;
    mutexLock(&g_reg_lock);
    for (int i = 0; i < DIAG_MAX_THREADS; i++) {
      if (g_threads[i].in_use && pthread_equal(g_threads[i].pth, want)) { t = &g_threads[i]; break; }
    }
    mutexUnlock(&g_reg_lock);
  }
  if (!t) t = diag_self();   /* PR_SET_NAME / self-naming case */
  if (!t) return;
  strncpy(t->name, name, sizeof(t->name) - 1);
  t->name[sizeof(t->name) - 1] = 0;
}

void diag_wait_enter(int kind, const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  t->wait_kind  = kind;
  t->wait_obj   = obj;
  t->wait_since = now_tick();
  t->waits_total++;
  t->last_active = t->wait_since;
}

void diag_wait_exit(void) {
  DiagThread *t = self;          /* exit without a prior enter is harmless */
  if (!t) return;
  t->wait_kind = DIAG_W_NONE;
  t->wait_obj  = NULL;
  t->wakes_total++;
  t->last_active = now_tick();
}

void diag_futex_spin(const void *obj) {
  DiagThread *t = diag_self();
  if (!t) return;
  /* publish as a futex wait but keep counting spins so the watchdog can tell
   * "alive but never satisfied" from "hard-parked". Reset wait_since only on
   * the transition *into* a futex wait (or onto a different uaddr), so the
   * dumped "parked secs" measures the current spin episode. */
  int was_futex = (t->wait_kind == DIAG_W_FUTEX && t->wait_obj == obj);
  t->wait_kind  = DIAG_W_FUTEX;
  t->wait_obj   = obj;
  uint64_t now = now_tick();
  if (!was_futex) t->wait_since = now;
  t->futex_spins++;
  t->last_active = now;
}

void diag_frame(int frame) {
  g_frame = frame;
  g_last_progress = now_tick();
}

/* ------------------------------------------------------------------ watchdog */
static uint64_t prev_waits[DIAG_MAX_THREADS];
static uint64_t prev_wakes[DIAG_MAX_THREADS];
static uint64_t prev_spins[DIAG_MAX_THREADS];

/* ---- CPU-context snapshot: see *where in libunity/il2cpp* a thread is wedged.
 * The shim beacons only show which sync primitive a thread sits in; when the
 * hang is inside native engine code (our case: main thread parked inside
 * Unity_nativeRender), this backtrace is what actually pinpoints it. */

/* Our own NRO code region, resolved once via svcQueryMemory on a local fn. */
static uint64_t g_nro_base, g_nro_size;
/* Set per-thread by snapshot_thread: enable the stack scan only for the main /
 * loader threads so the dump stays readable. */
static int g_scan_stack;
static void nro_range_init(void) {
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)(uintptr_t)&nro_range_init)) && mi.size) {
    g_nro_base = mi.addr; g_nro_size = mi.size;
  }
}
/* Write a symbolicated label for `addr` into buf: "libX+0xoff" / "NRO+0xoff"
 * / raw absolute. */
static void resolve_addr(char *buf, size_t n, uint64_t addr) {
  so_module *m = so_find_module_by_addr((const void *)(uintptr_t)addr);
  if (m) {
    /* m->name is the full sdmc path; the leading dirs are identical for every
     * loaded .so, so a fixed-width truncation makes libunity and libil2cpp
     * indistinguishable. Print the basename instead. */
    const char *base = m->name, *p;
    for (p = m->name; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(buf, n, "%.20s+0x%llx", base,   /* bound %s: resolve_addr buffers are 40B */
             (unsigned long long)(addr - (uint64_t)(uintptr_t)m->load_virtbase));
  } else if (g_nro_size && addr >= g_nro_base && addr < g_nro_base + g_nro_size) {
    snprintf(buf, n, "NRO+0x%llx", (unsigned long long)(addr - g_nro_base));
  } else {
    snprintf(buf, n, "0x%llx", (unsigned long long)addr);
  }
}

/* Buffered dump: everything collected while a thread is PAUSED is written here
 * and printed only AFTER the thread is resumed. Calling debugPrintf while the
 * target is paused deadlocks if the target owns the log mutex (it usually does
 * during boot, when every thread logs constantly) -- that was the fbstub88
 * whole-process freeze. Collect lock-free, resume, then log. */
static char   g_dump_buf[16384];
static size_t g_dump_len;
static void dump_emit(const char *fmt, ...) {
  if (g_dump_len >= sizeof g_dump_buf - 1) return;
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(g_dump_buf + g_dump_len, sizeof g_dump_buf - g_dump_len, fmt, ap);
  va_end(ap);
  if (n > 0) {
    g_dump_len += (size_t)n;
    if (g_dump_len > sizeof g_dump_buf - 1) g_dump_len = sizeof g_dump_buf - 1;
  }
}

static void dump_thread_context(const char *name, const ThreadContext *ctx) {
  char a[40], b[40];
  resolve_addr(a, sizeof a, ctx->pc.x);
  resolve_addr(b, sizeof b, ctx->lr);
  dump_emit("[wd]   %s  PC=%s  LR=%s\n", name, a, b);
  dump_emit("[wd]     SP=0x%llx FP=0x%llx X0=0x%llx X1=0x%llx X2=0x%llx\n",
              (unsigned long long)ctx->sp, (unsigned long long)ctx->fp,
              (unsigned long long)ctx->cpu_gprs[0].x,
              (unsigned long long)ctx->cpu_gprs[1].x,
              (unsigned long long)ctx->cpu_gprs[2].x);
  /* For a thread parked in svcArbitrateLock, X1 is typically the mutex address
   * and X0 the owner's thread-handle tag -> identifies who holds the lock. */
  /* Clean backtrace via the frame-pointer (x29) chain: [fp]=caller fp, [fp+8]=lr.
   * Bound every dereference to the thread's mapped stack so a wild fp can't fault
   * the watchdog itself. */
  uint64_t slo = 0, shi = 0;
  { MemoryInfo mi; u32 pi;
    if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, ctx->sp)) && mi.size) { slo = mi.addr; shi = mi.addr + mi.size; } }
  uint64_t fp = ctx->fp;
  for (int depth = 0; depth < 32 && (fp & 7) == 0; depth++) {
    if (slo) { if (fp < slo || fp + 16 > shi) break; }     /* stay in mapped stack */
    else if (fp < 0x1000) break;                            /* query failed: loose guard */
    if (!slo) {   /* stack query failed: verify fp is mapped before dereferencing */
      MemoryInfo fmi; u32 fpi;
      if (R_FAILED(svcQueryMemory(&fmi, &fpi, fp)) || !(fmi.perm & Perm_R)
          || fp + 16 > fmi.addr + fmi.size) break; }
    const uint64_t nextfp = ((const uint64_t *)(uintptr_t)fp)[0];
    const uint64_t lr     = ((const uint64_t *)(uintptr_t)fp)[1];
    if (!lr) break;
    char s[40]; resolve_addr(s, sizeof s, lr);
    dump_emit("[wd]     bt[%d] %s\n", depth, s);
    if (nextfp <= fp) break;   /* fp must climb up the stack */
    fp = nextfp;
  }
  /* Unity's hand-written wait stubs clobber the FP chain, so the bt[] above
   * often dead-ends in our glue. Raw-scan the top of the stack for any slot that
   * points into libunity / libil2cpp code -- those are return addresses the FP
   * walk missed, and they reveal what the thread is actually wedged inside.
   * Caller gates this (main/loader threads only) to keep the log readable. */
  if (g_scan_stack && slo) {
    uint64_t sp = ctx->sp & ~7ull;
    if (sp < slo) sp = slo;
    uint64_t top = sp + 0x2000;            /* ~1024 slots is plenty for the active frames */
    if (top > shi) top = shi;
    int printed = 0;
    for (uint64_t addr = sp; addr + 8 <= top && printed < 24; addr += 8) {
      uint64_t v = ((const uint64_t *)(uintptr_t)addr)[0];
      so_module *m = so_find_module_by_addr((const void *)(uintptr_t)v);
      if (!m) continue;
      if (!strstr(m->name, "unity") && !strstr(m->name, "il2cpp")) continue;  /* skip glue/main */
      /* A real return address points to the instruction *after* a call, so the
       * 4 bytes at v-4 must be BL (0b100101 imm26) or BLR (0xD63F0000 mask).
       * Without this check the scan reports jump-table targets, vtable pointers
       * and stale frames -- all of which look like code addresses but are NOT on
       * the live call chain. This filter is what makes the backtrace trustworthy. */
      { MemoryInfo pmi; u32 ppi;   /* v-4 may be an unmapped prev page -> probe safely */
        if (R_FAILED(svcQueryMemory(&pmi, &ppi, v - 4)) || !(pmi.perm & Perm_R)) continue; }
      uint32_t prev = ((const uint32_t *)(uintptr_t)(v - 4))[0];
      int is_bl  = (prev & 0xFC000000u) == 0x94000000u;
      int is_blr = (prev & 0xFFFFFC1Fu) == 0xD63F0000u;
      if (!is_bl && !is_blr) continue;
      char s[48]; resolve_addr(s, sizeof s, v);
      dump_emit("[wd]     ret@0x%-4llx %s%s\n", (unsigned long long)(addr - sp), s,
                  is_blr ? " (via blr)" : "");
      printed++;
    }
  }
}

/* Pause just long enough to snapshot, RESUME before printing (so the watchdog
 * can't deadlock on a stdio/heap lock the paused thread was holding). */
/* Handle of the calling thread, or 0 if it is not registered. */
/* pthread_t -> kernel Handle. Needed by the per-thread GC capture (round 143):
 * Boehm calls pthread_kill(t, sig) once per thread and we have to pause and
 * snapshot exactly THAT thread. The sibling port casts pthread_t to a libnx
 * Thread* directly; ours is newlib's, so the mapping has to come from here. */
Handle diag_handle_for_pthread(pthread_t id) {
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (t->in_use && t->handle && pthread_equal(t->pth, id)) return t->handle;
  }
  return 0;
}

Handle diag_self_handle(void) {
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (t->in_use && pthread_equal(t->pth, pthread_self())) return t->handle;
  }
  return 0;
}

/* Dump one thread's PC/LR/registers. Used by the GC watchdog to say WHERE the
 * collector is stuck when a stop-the-world overruns -- guessing at which lock it
 * is has now cost two rounds. svcGetThreadContext3 needs the thread paused, so
 * pause it for the read and put it straight back; it is wedged anyway. */
void diag_dump_thread(Handle h, const char *label) {
  if (!h) { debugPrintf("[wd] %s: no handle registered\n", label); return; }
  ThreadContext ctx;
  const int was_running = R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused));
  g_dump_len = 0; g_dump_buf[0] = 0;
  if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h))) {
    dump_thread_context(label, &ctx);
  } else {
    dump_emit("[wd]   %s: svcGetThreadContext3 failed\n", label);
  }
  if (was_running) svcSetThreadActivity(h, ThreadActivity_Runnable);
  debugPrintf("%s", g_dump_buf);
  g_dump_len = 0; g_dump_buf[0] = 0;
}

static void snapshot_thread(DiagThread *t) {
  if (!t->handle || t->handle == threadGetCurHandle()) return;
  /* Only the main render/UI thread and the async loaders matter for the swap/load
   * hang; the ~30 idle Job.Worker/GC threads just slow the dump so it can't re-fire
   * on the real wedge. Skip their context entirely (they're in the summary table). */
  int want = (strstr(t->name, "Main") || strstr(t->name, "Preload") ||
              strstr(t->name, "AsyncRead") || t->is_main_engine);
  if (!want) return;
  ThreadContext ctx;
  Result pr = svcSetThreadActivity(t->handle, ThreadActivity_Paused);
  Result gr = R_SUCCEEDED(pr) ? svcGetThreadContext3(&ctx, t->handle) : pr;
  if (R_FAILED(gr)) {
    if (R_SUCCEEDED(pr)) svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
    debugPrintf("[wd]   %-16s (snapshot failed rc=0x%x)\n", t->name, gr);
    return;
  }
  /* Collect while PAUSED (consistent stack), but into the buffer only -- no
   * logging until the thread is resumed (see dump_emit above). */
  g_dump_len = 0; g_dump_buf[0] = 0;
  g_scan_stack = 1;
  dump_thread_context(t->name[0] ? t->name : "?", &ctx);
  g_scan_stack = 0;
  if (R_SUCCEEDED(pr)) svcSetThreadActivity(t->handle, ThreadActivity_Runnable);
  debugPrintf("%s", g_dump_buf);
}

static void dump_threads(int episode, uint64_t now) {
  uint64_t stalled_ns = tick_to_ns(now - g_last_progress);
  debugPrintf("\n[wd] ===== STALL #%d : no frame progress for %llu.%llus (last frame=%d) =====\n",
              episode, (unsigned long long)(stalled_ns / 1000000000ull),
              (unsigned long long)((stalled_ns % 1000000000ull) / 100000000ull), g_frame);
  {
    extern volatile u64 g_swap_enter_tick;   /* armed by egl_SwapBuffers_log (imports.c) */
    u64 t = g_swap_enter_tick;
    if (t) {
      u64 ms = armTicksToNs(armGetSystemTick() - t) / 1000000ull;
      debugPrintf("[wd] eglSwapBuffers IN FLIGHT for %llu ms (the present is blocked)\n",
                  (unsigned long long)ms);
    }
  }
  {
    extern volatile u64 g_swap_enter_tick;   /* armed by egl_SwapBuffers_log (imports.c) */
    u64 t = g_swap_enter_tick;
    if (t) {
      u64 ms = armTicksToNs(armGetSystemTick() - t) / 1000000ull;
      debugPrintf("[wd] >>> a thread is INSIDE eglSwapBuffers right now, for %llu ms <<<\n",
                  (unsigned long long)ms);
    }
  }
  debugPrintf("[wd] %-16s %-10s %-11s %-18s %7s  d_wait d_wake d_spin\n",
              "name", "tid", "state", "wait_obj", "secs");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    DiagThread *t = &g_threads[i];
    if (!t->in_use) continue;
    int kind = t->wait_kind;
    uint64_t since = t->wait_since;
    uint64_t parked_ns = (kind != DIAG_W_NONE && since) ? tick_to_ns(now - since) : 0;
    uint64_t dwait = t->waits_total - prev_waits[i];
    uint64_t dwake = t->wakes_total - prev_wakes[i];
    uint64_t dspin = t->futex_spins - prev_spins[i];
    prev_waits[i] = t->waits_total;
    prev_wakes[i] = t->wakes_total;
    prev_spins[i] = t->futex_spins;
    debugPrintf("[wd] %-16s %-10llu %-11s 0x%-16llx %3llu.%llu  %6llu %6llu %6llu%s\n",
                t->name[0] ? t->name : "?",
                (unsigned long long)t->tid,
                wait_kind_name(kind),
                (unsigned long long)(uintptr_t)t->wait_obj,
                (unsigned long long)(parked_ns / 1000000000ull),
                (unsigned long long)((parked_ns % 1000000000ull) / 100000000ull),
                (unsigned long long)dwait, (unsigned long long)dwake,
                (unsigned long long)dspin,
                t->is_main_engine ? "  <engine_main>" : "");
  }
  debugPrintf("[wd] legend: d_* = delta since previous dump (0/0/0 == hard-parked; "
              "d_spin>0 == alive on futex; d_wait>d_wake == entered a wait it hasn't left)\n");
  /* native backtrace: where each thread is wedged inside libunity/il2cpp/NRO */
  debugPrintf("[wd] --- thread CPU contexts (frame-pointer backtrace) ---\n");
  for (int i = 0; i < DIAG_MAX_THREADS; i++) {
    if (g_threads[i].in_use) snapshot_thread(&g_threads[i]);
  }
  debugPrintf("\n");
  debugLogFlush();   /* a hang produces no more log lines -- flush now or
                      * this dump is lost in the buffer (see round 48). */
}

static void watchdog_main(void *unused) {
  (void)unused;
  int episode = 0;
  uint64_t last_dump = 0;
  /* prime so we don't false-trigger before the first frame */
  if (g_last_progress == 0) g_last_progress = now_tick();
  extern volatile u64 g_swap_enter_tick;   /* armed by egl_SwapBuffers_log */
  uint64_t swap_dumped = 0;
  for (;;) {
    svcSleepThread(DIAG_POLL_NS);
    /* Never paused (this thread is not in the registry), so it can always
     * rescue a stop-the-world that has wedged. Must run before anything
     * else here: the rest of the tick may take locks. */
    nx_gc_stopworld_watchdog();
    uint64_t now = now_tick();
    uint64_t idle = now - g_last_progress;
    /* A present blocked inside eglSwapBuffers is the render thread stuck in
     * mesa; it may not trip the frame-stall promptly. Catch it fast: if a
     * swap has been in flight > 2s and we have not dumped for it, dump once
     * so UnityMain's in-mesa backtrace reaches disk. */
    u64 se = g_swap_enter_tick;
    if (se) {
      u64 in_flight_ms = armTicksToNs(armGetSystemTick() - se) / 1000000ull;
      if (in_flight_ms > 2000 && swap_dumped != se) {
        dump_threads(++episode, now);
        swap_dumped = se;
        last_dump = now;
      }
    } else {
      swap_dumped = 0;
    }
    /* round 62: bounded periodic sample during the loading phase */
    { static uint64_t last_probe = 0; static int probes = 0;
      if (probes < 12 &&
          (last_probe == 0 || tick_to_ns(now - last_probe) >= 4000000000ull)) {
        last_probe = now; probes++;
        debugPrintf("\n[wd] ---- periodic load-probe #%d ----\n", probes);
        dump_threads(1000 + probes, now);
        last_dump = now;
      }
    }
    if (tick_to_ns(idle) >= DIAG_STALL_NS) {
      if (last_dump == 0 || tick_to_ns(now - last_dump) >= DIAG_REDUMP_NS) {
        dump_threads(++episode, now);
        last_dump = now;
      }
    } else {
      /* progress resumed: reset so a later stall dumps fresh */
      last_dump = 0;
    }
  }
}

void diag_watchdog_start(void) {
  if (g_wd_started) return;
  g_wd_started = 1;
  nro_range_init();
  if (g_last_progress == 0) g_last_progress = now_tick();
  /* libnx thread: deliberately NOT via the pthread shim under test.
   * 16 KiB stack, priority 0x2C (same band as main), default core. */
  Result rc = threadCreate(&g_wd_thread, watchdog_main, NULL, NULL, 0x4000, 0x2C, -2);
  if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&g_wd_thread)))
    debugPrintf("[wd] watchdog armed (stall=%llus, poll=1s)\n",
                (unsigned long long)(DIAG_STALL_NS / 1000000000ull));
  else
    debugPrintf("[wd] watchdog FAILED to start rc=0x%x\n", rc);
}
