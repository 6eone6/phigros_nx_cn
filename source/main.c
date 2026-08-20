/* main.c -- ZOOKEEPER DX Switch wrapper entry point.
 *
 * Unity 2022.3 / IL2CPP. Loads libmain + libunity + libil2cpp, then drives the
 * lifecycle the Java UnityPlayer normally runs (initJni -> recreate GFX state ->
 * render loop), calling the native entry points recovered from libunity.so's
 * JNI_OnLoad (see unity_entrypoints.h). The engine owns its own EGL/GLES3 context
 * created from android_native_window(); SDL is audio/HID only.
 *
 * Heap + syscall scaffolding adapted from cr3_nx's main.c (MIT).
 */

#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>
#include <switch.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "nx_patch_phigros.h"  /* Phigros: recovered libunity patch + flags */
#include <dirent.h>
#include <strings.h>  /* strncasecmp */
#include "util.h"
#include "error.h"
#include <sys/statvfs.h>
/* Both unconditional. asset_pack.c is always compiled and its query functions
 * answer "no pack" harmlessly when the feature is off; phigros_assets.h is
 * pure data and check_data() needs it either way. Gating an include on a
 * feature flag while leaving call sites ungated is how the same bug bit
 * libc_shim.c -- see the note there. */
#include "asset_pack.h"
#include "phigros_assets.h"   /* GENERATED: measured asset layout */
#include "nx_root.h"          /* runtime data root -- works from any folder */
#include "so_util.h"
#include "imports.h"
#include "jni_fake.h"
#include "unity_jni.h"        /* unity_prefs_tick / unity_prefs_sync */
#include "android_native_unity.h"
#include "opensles.h"
#include "unity_entrypoints.h"
#include "diag.h"

/* Printed by the compiler on every build. With DEBUG_LOG 0 there is no boot log
 * line to carry the revision, so this is the only place a stale tree shows up:
 * if the build output does not name the rev you just unzipped, you are building
 * something else. */
#define NX_STR2(x) #x
#define NX_STR(x) NX_STR2(x)
#pragma message ("building " GAME_NAME "_nx source rev " PHI_SRC_REV " (DEBUG_LOG=" NX_STR(DEBUG_LOG) ")")

/* DATA_ROOT comes from config.h and is nx_root() -- a runtime call, not a
 * string literal. The local #define that used to sit here would have shadowed
 * it and pinned the loader back to one folder name. */
#define LIB_MAIN   "libmain.so"
#define LIB_UNITY  "libunity.so"
#define LIB_IL2CPP "libil2cpp.so"
#define LIB_NATIVEAUDIO "libnativeaudioe7.so"   /* Exceed7 NativeAudio (optional) */
#define LIB_FB_APP          "libFirebaseCppApp-11_9_0.so"
#define LIB_FB_ANALYTICS    "libFirebaseCppAnalytics.so"
#define LIB_FB_MESSAGING    "libFirebaseCppMessaging.so"
#define LIB_FB_REMOTECONFIG "libFirebaseCppRemoteConfig.so"

void unity_environment_init(const char *data_root);   /* unity_glue.c */

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

/* mmap arena. In overcommit mode (g_overcommit) this is a big *virtual* window in
 * the alias region: Unity's PROT_NONE pool reservations cost only address space
 * and physical pages are committed on demand via svcMapPhysicalMemory. In the
 * fallback path it's a fully heap-backed 256MB-aligned slab (the Switch has no
 * native overcommit). Consumed by mmap_fake/munmap_fake (libc_shim.c). */
void  *g_mmap_arena_base = NULL;
size_t g_mmap_arena_size = 0;
int    g_overcommit      = 0;          /* 1 = alias-region on-demand commit */
u64    g_alias_base = 0, g_alias_size = 0;
/* captured in __libnx_initheap for logging from main() (log file isn't open yet) */
unsigned g_oc_heap_mb = 0, g_oc_freed_mb = 0;
/* granular setup diagnostics so a failed gate tells us WHICH step bailed */
int      g_oc_hint_map = 0, g_oc_hint_unmap = 0;
unsigned g_oc_alias_mb = 0;
void    *g_oc_win = NULL;
int      g_oc_probe_tried = 0, g_oc_shrink_tried = 0;
/* stack-region overcommit arena armer (libc_shim.c) */
extern int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes);
unsigned g_oc_probe_rc = 0, g_oc_shrink_rc = 0;
unsigned long g_oc_win_addr = 0;
u64      g_oc_sysres = 0;   /* system resource size (0 => svcMapPhysicalMemory unusable) */

so_module main_mod, unity_mod, il2cpp_mod;
/* Phigros ships a fourth native module: Exceed7 NativeAudio, which carries the
 * whole rhythm-critical audio path (see phigros_jni.c). It is OPTIONAL at load
 * time -- a missing plugin costs you chart audio and hitsounds, not the boot. */
so_module nativeaudio_mod;
int       nativeaudio_loaded = 0;

/* defined in libc_shim.c; consumed by the GC stop-the-world bridge there */
extern uintptr_t g_il2cpp_base;

/* Replacement icall for UnityEngine.Application::get_internetReachability.
 * Returns NetworkReachability.NotReachable (0). See the frame-0 install site
 * for why (unblocks FirebaseManager.IsGetMessage / the boot coroutine). The
 * il2cpp icall ABI for this static getter is "int32_t func(MethodInfo*)"; we
 * ignore the hidden arg and just report no network. */
static int32_t nx_internet_reachability(void) { return 0; }

/* Replacement for Common.FirebaseManager.IsGetMessage (instance method, returns
 * bool). The FirebaseLoading boot state spins on this; force it complete and log
 * once so the run tells us whether state 5 is even reached. */
static int nx_rd(uintptr_t a);            /* fwd decl: readable-memory check  */
static uintptr_t nx_pd(uintptr_t a);      /* fwd decl: safe pointer deref      */
static int32_t nx_is_get_message(void) {
  static int once = 0;
  if (!once) { once = 1; debugPrintf("[hook] FirebaseManager.IsGetMessage reached -> forced 1\n"); }
  return 1;
}

/* Unity's native time base is frozen in our environment (it never advances the
 * engine clock -- likely it expects Android Choreographer frame timestamps we
 * don't deliver). Every managed Time.* accessor therefore reads a frozen value:
 * deltaTime==0, time/realtimeSinceStartup constant. That freezes all time-based
 * game logic -- DOTween (the boot logo fade), WaitForSeconds, etc. -- which is
 * what holds the black screen (the fade never completes, so boot never starts).
 * Work around it by driving our own monotonic frame clock and redirecting the
 * managed Time accessors to it. nx_time_tick() runs once per render-loop frame. */
static volatile float  g_unity_dt   = 1.0f / 60.0f;
static volatile double g_unity_time = 0.0;
static volatile uint32_t g_frame_count = 0;   /* Time.frameCount source */
static uint64_t g_time_prev_ns  = 0;
static uint64_t g_time_start_ns = 0;
/* Unity native vsync primitives (round 56). Android's Choreographer signals
 * these every frame; on Switch we must. Triple at libunity+0x110e8d0:
 * mutex @+0x0, cond @+0x28 (0x110e8f8), counter @+0x58 (0x110e928). Confirmed
 * the vsync waiter: its wait fn (0x5ff730) is called only from the frame-
 * pacing fn at 0x5d928c (TimeManager::Sync, GetActualTargetFrameRate,
 * DisplayInfo, Swappy getter). */
extern int pthread_mutex_lock_fake(pthread_mutex_t **);
extern int pthread_mutex_unlock_fake(pthread_mutex_t **);
extern int pthread_cond_broadcast_fake(pthread_cond_t **);
static pthread_mutex_t **g_vsync_mutex   = 0;
static pthread_cond_t  **g_vsync_cond    = 0;
static volatile uint64_t *g_vsync_counter = 0;
static uint64_t nx_now_ns(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void nx_time_tick(void) {
  uint64_t now = nx_now_ns();
  g_frame_count++;                    /* advance Time.frameCount once per frame */
  if (!g_time_start_ns) g_time_start_ns = now;
  if (g_time_prev_ns) {
    double dt = (double)(now - g_time_prev_ns) / 1e9;
    if (dt < 0) dt = 0;
    if (dt > 0.1) dt = 0.1;            /* clamp, mirrors Unity maximumDeltaTime */
    g_unity_dt = (float)dt;
    g_unity_time += dt;
  }
  g_time_prev_ns = now;
}
static float nx_delta_time(void) { return g_unity_dt; }
static float nx_time_f(void)     { return (float)g_unity_time; }
static int   nx_frame_count(void){ return (int)g_frame_count; }
uint32_t     port_frame_count(void){ return g_frame_count; } /* audio pump warmup gate */
static float nx_realtime_since_startup(void) {
  uint64_t now = nx_now_ns();
  if (!g_time_start_ns) g_time_start_ns = now;
  return (float)((double)(now - g_time_start_ns) / 1e9);
}

/* fbstub42: TimeManager::Update entry hook. The Switch port's player loop drives
 * Update with a frozen vsync timestamp as newTime, so deltaTime collapses to the
 * 1e-5 floor and every native time reader (the PreloadManager included) starves,
 * which is what wedges async scene loading (the resident-scene black screen). We
 * redirect Update's entry (libunity 0x446114) here, replay its tiny prologue
 * (frameCount++, aux counter++, pause check), then re-enter its body (0x446138 --
 * frameless, re-reads everything from x0) with newTime = GetTimeSinceStartup(),
 * the engine's own monotonic clock (0x446578) which DOES advance. Update then
 * derives all deltaTime variants and m_Time correctly. Offsets verified against
 * both the game binary and the symbolized 2022.3.62f2 reference engine. */
static void   (*g_unity_update_body)(void *, double) = NULL; /* 0x4e21bc Update body */
static void     *g_tm = NULL;                       /* captured TimeManager instance */
static void   *(*g_get_time_manager)(void) = NULL;  /* 0x4e2794 GetTimeManager() (subsystem 7) */
static uint64_t  g_clk_base_ns = 0;
static volatile uint64_t g_last_main_tick_ns = 0;
static Mutex     g_clock_lock;                       /* main-hook vs clock-thread */
static Thread    g_clock_thr;
#define CLOCK_STALL_NS 100000000ULL                  /* 100ms main silence => stalled */
/* Re-run Update's body with a wall-clock newTime so deltaTime/m_Time advance even
 * while UnityMain is parked in a synchronous scene-load (the frame-0 async hang). */
static void nx_clock_tick(void *tm) {
  uint64_t now = nx_now_ns();
  if (!g_clk_base_ns) g_clk_base_ns = now;
  double wall    = (double)(now - g_clk_base_ns) / 1e9;
  double sref    = *(volatile double *)((char *)tm + 0xe8);   /* m_StartupRef */
  double newTime = sref + wall;
  if (g_unity_update_body) g_unity_update_body(tm, newTime);
  /* Heartbeat: is the clock actually advancing the splash/menu transition?
   * Log time + frameCount every ~2s of wall clock. If time moves but the
   * screen stays black, the transition is not time-gated; if time is stuck,
   * this is the freeze. */
  { static uint64_t last_hb = 0;
    if (now - last_hb > 2000000000ull) {
      last_hb = now;
      extern volatile unsigned long long g_alloc_calls, g_alloc_ns;
      static unsigned long long a_calls0 = 0, a_ns0 = 0;
      unsigned long long ac = g_alloc_calls, an_ms = g_alloc_ns / 1000000ull;
      unsigned long long dac = ac - a_calls0, dan = an_ms - (a_ns0 / 1000000ull);
      a_calls0 = ac; a_ns0 = g_alloc_ns;
      extern volatile unsigned long long g_readdir_calls, g_readdir_ns, g_opendir_calls;
      static unsigned long long rd0 = 0, rdns0 = 0, od0 = 0;
      unsigned long long rd = g_readdir_calls, rdns = g_readdir_ns / 1000000ull, od = g_opendir_calls;
      unsigned long long drd = rd - rd0, drd_ms = rdns - (rdns0 / 1000000ull), dod = od - od0;
      rd0 = rd; rdns0 = g_readdir_ns; od0 = od;
      extern volatile unsigned long long g_read_calls, g_fsread_calls, g_fsread_ns, g_fsread_bytes, rc_hits, rc_fills;
      static unsigned long long r0 = 0, fr0 = 0, frns0 = 0, frby0 = 0, h0 = 0, f0 = 0;
      unsigned long long rdc = g_read_calls, frc = g_fsread_calls, frns = g_fsread_ns / 1000000ull;
      unsigned long long frkb = g_fsread_bytes / 1024ull, hh = rc_hits, ff = rc_fills;
      unsigned long long drc = rdc - r0, dfrc = frc - fr0, dfrns = frns - (frns0/1000000ull);
      unsigned long long dfrkb = frkb - (frby0/1024ull), dh = hh - h0, df = ff - f0;
      r0 = rdc; fr0 = frc; frns0 = g_fsread_ns; frby0 = g_fsread_bytes; h0 = hh; f0 = ff;
      extern volatile unsigned long long g_fx_wait, g_fx_eagain, g_fx_slept, g_fx_wake, g_fx_nosys;
      static unsigned long long x0 = 0, xe0 = 0, xs0 = 0, xw0 = 0, xn0 = 0;
      unsigned long long xw = g_fx_wait, xe = g_fx_eagain, xs = g_fx_slept,
                        xk = g_fx_wake, xn = g_fx_nosys;
      unsigned long long dxw = xw - x0, dxe = xe - xe0, dxs = xs - xs0,
                        dxk = xk - xw0, dxn = xn - xn0;
      x0 = xw; xe0 = xe; xs0 = xs; xw0 = xk; xn0 = xn;
      extern volatile unsigned long long g_mainwait_ns, g_mainwait_n;
      extern volatile unsigned long long g_mainwait_cond_ns, g_mainwait_cond_n;
      static unsigned long long mw0 = 0, mn0 = 0, cw0 = 0, cn0 = 0;
      unsigned long long mw = g_mainwait_ns / 1000000ull, mn = g_mainwait_n;
      unsigned long long cw = g_mainwait_cond_ns / 1000000ull, cn = g_mainwait_cond_n;
      unsigned long long dmw = mw - (mw0 / 1000000ull), dmn = mn - mn0;
      unsigned long long dcw = cw - (cw0 / 1000000ull), dcn = cn - cn0;
      mw0 = g_mainwait_ns; mn0 = mn; cw0 = g_mainwait_cond_ns; cn0 = cn;
      /* round 74: top blocked threads this window (tid=ms/waits). */
      { extern volatile int g_twait_tid[]; extern volatile unsigned long long g_twait_ns[];
        extern volatile unsigned long long g_twait_cnt[];
        static unsigned long long tns0[64], tcn0[64];
        char tb[220]; int tp = 0; tb[0] = 0;
        for (int pick = 0; pick < 5; pick++) {
          int best = -1; unsigned long long bestd = 0;
          for (int k = 0; k < 64; k++) {
            if (!g_twait_tid[k]) continue;
            unsigned long long d = g_twait_ns[k] - tns0[k];
            if (d > bestd) { bestd = d; best = k; }
          }
          if (best < 0 || bestd < 1000000ull) break;
          unsigned long long dc = g_twait_cnt[best] - tcn0[best];
          tp += snprintf(tb + tp, sizeof(tb) - (size_t)tp, " %d=%llums/%lluw",
                         g_twait_tid[best], bestd / 1000000ull, dc);
          tns0[best] = g_twait_ns[best]; tcn0[best] = g_twait_cnt[best];
          if (tp > 180) break;
        }
        for (int k = 0; k < 64; k++) { tns0[k] = g_twait_ns[k]; tcn0[k] = g_twait_cnt[k]; }
        if (tb[0]) debugPrintf("[blk] top:%s\n", tb);
        /* round 77: every ~10s dump EVERY thread, so threads that never reach
         * the top 5 (notably Loading.PreloadManager) are finally visible. */
        { static unsigned hb_n = 0; static unsigned long long all0[64];
          if ((++hb_n % 5) == 0) {
            for (int k = 0; k < 64; k++) {
              if (!g_twait_tid[k]) continue;
              unsigned long long d = g_twait_ns[k] - all0[k];
              debugPrintf("[blkall] tid=%d blocked=%llums\n",
                          g_twait_tid[k], d / 1000000ull);
            }
            for (int k = 0; k < 64; k++) all0[k] = g_twait_ns[k];
          } }
      }
      debugPrintf("[time] hb: Time.time=%llums wall=%llums frameCount=%llu "
                  "alloc_calls+=%llu alloc_ms+=%llu readdir+=%llu readdir_ms+=%llu opendir+=%llu "
                  "read+=%llu fsread+=%llu fsread_ms+=%llu fsread_KB+=%llu rc_hit+=%llu rc_fill+=%llu "
                  "fxwait+=%llu fxeagain+=%llu fxslept+=%llu fxwake+=%llu fxnosys+=%llu "
                  "mainblk_fx_ms+=%llu mainwaits_fx+=%llu "
                  "mainblk_cond_ms+=%llu mainwaits_cond+=%llu\n",
                  (unsigned long long)(newTime * 1000.0),
                  (unsigned long long)(wall * 1000.0),
                  (unsigned long long)*(volatile uint64_t *)((char *)tm + 0xc8),
                  dac, dan, drd, drd_ms, dod,
                  drc, dfrc, dfrns, dfrkb, dh, df,
                  dxw, dxe, dxs, dxk, dxn, dmw, dmn, dcw, dcn);
      { /* declared in libc_shim.h, which main.c does not include; a local
         * prototype avoids dragging the whole shim header in here. */
        extern void nx_arena_usage_mb(unsigned *, unsigned *, unsigned *);
        unsigned ar = 0, ac = 0, at = 0;
        nx_arena_usage_mb(&ar, &ac, &at);
        debugPrintf("[mem] arena reserved=%u MB committed=%u MB of %u MB\n",
                    ar, ac, at);
        /* Cross a pressure threshold and say so even in a release build. A
         * Unity out-of-memory shows up as a fault inside its own memory-report
         * formatter (registers full of "ALLOC_TYPETREE ... 0B | reserved"),
         * which tells you nothing about what filled up -- this does. */
        { /* How much of the session has gone into GC pauses. If total_ms is a
           * small fraction of wall time the stutter is not the collector. */
          extern void nx_gc_pause_stats(unsigned *, unsigned *, unsigned *);
          unsigned gn = 0, gt = 0, gm = 0;
          nx_gc_pause_stats(&gn, &gt, &gm);
          extern int nx_gc_bailouts(void);
          extern void nx_gc_capture_stats(unsigned *, unsigned *, unsigned *,
                                          unsigned *, unsigned *);
          unsigned cok = 0, cnt = 0, cnh = 0, cnc = 0, cbr = 0;
          nx_gc_capture_stats(&cok, &cnt, &cnh, &cnc, &cbr);
          /* captured = threads whose SP+registers reached the mark. The four
           * miss counters must stay 0: any of them means a thread was scanned
           * from a stale range, which is the r119..r137 bug. */
          { extern void nx_canary_stats(unsigned *, unsigned *, unsigned *);
            unsigned clive = 0, cev = 0, cslots = 0;
            nx_canary_stats(&clive, &cev, &cslots);
            /* Tail-canary coverage. A high evict count means the table is
             * thrashing and "no SMASH" proves little -- see nx_alloc.c. */
            static unsigned last_ev = 0;
            if (cev != last_ev || clive) {
              last_ev = cev;
              debugPrintf("[canary] live=%u of %u slots, evicted=%u%s\n",
                          clive, cslots, cev,
                          cev > cslots ? "   <- table thrashing: absence of SMASH is weak evidence" : "");
            } }
          if (gn) debugPrintf("[gc] pauses=%u total=%u ms max=%u ms bailouts=%d "
                              "captured=%u miss(thr/hnd/ctx/roots)=%u/%u/%u/%u\n",
                              gn, gt, gm, nx_gc_bailouts(), cok, cnt, cnh, cnc, cbr);
        }
        if (at) {
          static unsigned hi = 0;
          const unsigned pct = (unsigned)((uint64_t)ar * 100u / at);
          if (pct >= 50 && pct / 25 > hi / 25) {
            hi = pct;
            debugLogNote("[mem] arena %u%% reserved (%u of %u MB)\n", pct, ar, at);
          }
        } }
      debugLogFlush();
    } }
}
static void nx_time_update_hook(void *tm) {
  g_tm = tm;
  { static int once = 0; if (!once) { once = 1; debugPrintf("[time] Update hook first fire (tm=%p)\n", tm); } }
  { extern volatile int g_main_tid; extern int gettid_fake(void);
    if (!g_main_tid) g_main_tid = gettid_fake(); }   /* round 72: unambiguously main */
  g_last_main_tick_ns = nx_now_ns();                          /* main thread is live */
  *(volatile uint64_t *)((char *)tm + 0xc8) += 1;            /* frameCount++ (prologue) */
  *(volatile uint32_t *)((char *)tm + 0xd0) += 1;            /* aux counter++           */
  if (*(volatile uint8_t *)((char *)tm + 0xf8) != 0) return; /* paused -> early return  */
  mutexLock(&g_clock_lock);
  nx_clock_tick(tm);
  mutexUnlock(&g_clock_lock);
}
static void nx_clock_thread(void *arg) {
  (void)arg;
  static uint8_t clk_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(clk_tls);
  uint64_t last_vsync_ns = 0;
  while (!jni_quit_requested) {
    svcSleepThread(8000000ULL);                              /* ~8ms keep-alive */
    /* Choreographer stand-in (round 56): pulse Unity's native vsync counter +
     * cond at ~60Hz so frame-pacing advances (drives the splash fade). */
    { uint64_t _n = nx_now_ns();
      if (g_vsync_mutex && g_vsync_cond && g_vsync_counter &&
          (_n - last_vsync_ns) >= PHI_VSYNC_PERIOD_NS) {
        last_vsync_ns = _n;
        if (pthread_mutex_lock_fake(g_vsync_mutex) == 0) {
          ++*g_vsync_counter;
          pthread_cond_broadcast_fake(g_vsync_cond);
          pthread_mutex_unlock_fake(g_vsync_mutex);
        }
        static int _vlog = 0; if (_vlog < 2) { _vlog++; debugPrintf("[vsync] native vsync pulse armed (counter@%p)\n", (void*)g_vsync_counter); }
      } }
    /* Fetch the TimeManager singleton directly -- the entry hook at 0x4e2198 never
     * fires during PvZ's boot-coroutine save load (the player loop isn't ticking
     * Update yet), so g_tm stays NULL. GetTimeManager() returns it once the
     * subsystem exists; until then it's NULL and we skip. */
    void *tm = g_get_time_manager ? g_get_time_manager() : g_tm;
    { static void *seen = NULL; if (tm && tm != seen) { seen = tm; debugPrintf("[time] clock thread got TimeManager=%p (driving native clock)\n", tm); } }
    if (tm && g_unity_update_body &&
        (nx_now_ns() - g_last_main_tick_ns) > CLOCK_STALL_NS &&
        mutexTryLock(&g_clock_lock)) {                       /* only while main is silent */
      nx_clock_tick(tm);
#if LOG_VERBOSE
      { static unsigned _n = 0; if ((_n++ & 0x3f) == 0) debugPrintf("[time] clock thread driving Update (main stalled)\n"); }
#endif
      mutexUnlock(&g_clock_lock);
    }
  }
}
static void nx_install_time_fix(void) {
  uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
  g_unity_update_body = (void (*)(void *, double))(ub + PHI_TIME_UPDATE_BODY);   /* PvZ Update body */
  g_get_time_manager  = (void *(*)(void))(ub + PHI_TIME_GETMANAGER);            /* PvZ GetTimeManager() */
  /* Native vsync triple (round 56): stand in for Choreographer's per-frame
   * signal so Unity's frame-pacing wait (and the splash fade behind it) can
   * proceed. Offsets confirmed as the vsync waiter's mutex/cond/counter. */
  g_vsync_mutex   = (pthread_mutex_t **)(ub + 0x110e8d0);
  g_vsync_cond    = (pthread_cond_t  **)(ub + 0x110e8f8);
  g_vsync_counter = (volatile uint64_t *)(ub + 0x110e928);
  uint32_t stub[4] = {
    0x58000050u,  /* ldr x16, #8 */
    0xd61f0200u,  /* br  x16     */
    (uint32_t)((uintptr_t)&nx_time_update_hook & 0xffffffffu),
    (uint32_t)((uintptr_t)&nx_time_update_hook >> 32),
  };
  /* VERIFY-FIRST (added for Phigros). PvZ patched this unconditionally.
   * 0x4e2198 is PvZ 62f1c1's TimeManager::Update entry; in 2022.3.0f1 it is
   * an unrelated function, and a blind 16-byte stub write there corrupts it.
   * The expected word is PvZ's prologue -- it will not match here, so this
   * skips and logs. Re-derive before enabling: TimeManager::Update(double)
   * is at 0x7f7438 (size 456) in the symbolized 2022.3.0f1 reference, and
   * GetTimeManager() at 0x7f7a94 -- pin them with tools/match.py. */
  if (*(volatile uint32_t *)(ub + PHI_TIME_UPDATE_ENTRY) == PHI_TIME_UPDATE_WORD) {
    so_patch_code((void *)(ub + PHI_TIME_UPDATE_ENTRY), stub, sizeof stub);
  } else {
    debugPrintf("[time] SKIP Update hook: +0x%x = 0x%08x, not the expected "
                "prologue -- offsets are PvZ's, re-derive (PORTING sec 6)\n",
                (unsigned)PHI_TIME_UPDATE_ENTRY,
                *(volatile uint32_t *)(ub + PHI_TIME_UPDATE_ENTRY));
    return;
  }
  if (R_SUCCEEDED(threadCreate(&g_clock_thr, nx_clock_thread, NULL, NULL, 0x8000, 0x2C, -2)))
    threadStart(&g_clock_thr);
  debugPrintf("[boot] installed TimeManager::Update hook @libunity+PHI_TIME_UPDATE_ENTRY "
              "+ clock thread (newTime <- startupRef + wallclock)\n");
}

/* --- boot finish-flag probe -------------------------------------------------
 * State 6 (SetFinishFlag) walks A=*(il2cpp+0x21fd0718); B=*A; obj=*(B+0x20);
 * obj2=*(*(obj+0xc0)+0x10); holder=*(obj2+0xb8 then deref); and writes
 * *(holder+0x10)=1 -- but ONLY if holder is non-null (else it stays at state 6
 * forever). CheckInitializeFinish polls the same obj2 (+0xe0 / +0x135). We read
 * the live chain so we can see exactly which link is null / whether the finish
 * flag ever gets set, without any destructive patching. */
static int nx_rd(uintptr_t a) {
  MemoryInfo mi; u32 pi;
  if (a < 0x1000) return 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
  if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) return 0;
  return 1;
}
static uintptr_t nx_pd(uintptr_t a) { return nx_rd(a) ? *(volatile uintptr_t *)a : 0; }
__attribute__((unused)) static void nx_probe_finish(uintptr_t base) {
  uintptr_t A    = nx_pd(base + PHI_IL2CPP_FINISH_FLAG);
  uintptr_t B    = A    ? nx_pd(A)          : 0;
  uintptr_t obj  = B    ? nx_pd(B + 0x20)   : 0;
  uintptr_t c0   = obj  ? nx_pd(obj + 0xc0) : 0;
  uintptr_t obj2 = c0   ? nx_pd(c0 + 0x10)  : 0;
  uintptr_t b8   = obj2 ? nx_pd(obj2 + 0xb8): 0;
  uintptr_t hold = b8   ? nx_pd(b8)         : 0;
  int      flag  = (hold && nx_rd(hold + 0x10)) ? *(volatile uint8_t  *)(hold + 0x10) : -1;
  uint32_t e0    = (obj2 && nx_rd(obj2 + 0xe0)) ? *(volatile uint32_t *)(obj2 + 0xe0) : 0xffffffffu;
  int o135  = (obj  && nx_rd(obj + 0x135))  ? *(volatile uint8_t *)(obj + 0x135)  : -1;
  int o2135 = (obj2 && nx_rd(obj2 + 0x135)) ? *(volatile uint8_t *)(obj2 + 0x135) : -1;
  debugPrintf("[probe] A=%p B=%p obj=%p obj2=%p hold=%p | finishFlag=%d e0=0x%x obj.135=%d obj2.135=%d\n",
              (void *)A, (void *)B, (void *)obj, (void *)obj2, (void *)hold, flag, e0, o135, o2135);
}


/* libunity ~17M + libil2cpp ~36M + headroom for relocated segments */
#define SO_REGION_BYTES (160u * 1024 * 1024)

/* Reserve the virtual arena window at the TOP of the alias region (deep in the
 * 64GB region, where libnx never allocates) after verifying it is fully unmapped.
 * No physical backing yet -- pages are committed on demand. */
static void *overcommit_reserve_window(size_t size) {
  size = (size + MMAP_ARENA_ALIGN - 1) & ~(MMAP_ARENA_ALIGN - 1);
  if (!g_alias_base || g_alias_size < size + MMAP_ARENA_ALIGN) return NULL;
  u64 top = g_alias_base + g_alias_size;
  u64 win = (top - size) & ~(MMAP_ARENA_ALIGN - 1);
  u64 a = win;
  while (a < win + size) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return NULL;
    if (mi.type != MemType_Unmapped) return NULL;   /* collision -> bail */
    a = mi.addr + mi.size;
  }
  return (void *)win;
}

/* virtmemFindStack refuses large windows even when the stack region has room, so
 * scan the region directly via svcQueryMemory for the largest 256MB-aligned
 * unmapped hole (and log the whole map for diagnosis). svcMapMemory only aliases
 * into the stack region, so the OC window must live here. */
static void  *g_oc_win2    = NULL;   /* second-largest stack hole (OC window 2) */
static size_t g_oc_win2_sz = 0;
extern int oc_arena_add_window(void *window, size_t window_bytes);
static void *oc_find_stack_window(size_t want, size_t *out_size) {
  *out_size = 0;
  u64 sbase = 0, ssize = 0;
  svcGetInfo(&sbase, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ssize, InfoType_StackRegionSize,    CUR_PROCESS_HANDLE, 0);
  if (!sbase || !ssize) return NULL;
  u64 end = sbase + ssize, a = sbase, best_a = 0, best_l = 0, sec_a = 0, sec_l = 0;
  int holes = 0, mapped = 0;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
    u64 ms = mi.addr, me = mi.addr + mi.size;
    if (me <= a) break;                              /* no-progress guard */
    if (mi.type == MemType_Unmapped) {
      u64 hs = ms < sbase ? sbase : ms, he = me > end ? end : me;
      if (he > hs) {
        if (he - hs > best_l) { sec_l = best_l; sec_a = best_a; best_l = he - hs; best_a = hs; }
        else if (he - hs > sec_l) { sec_l = he - hs; sec_a = hs; }
        if (holes < 8)
          debugPrintf("[oc] stack hole %d: %p .. %p (%u MB)\n",
                      holes++, (void *)hs, (void *)he, (unsigned)((he - hs) >> 20));
      }
    } else mapped++;
    a = me;
  }
  debugPrintf("[oc] stack scan: base=%p size=%u MB, %d holes, %d mapped spans, largest=%u MB\n",
              (void *)sbase, (unsigned)(ssize >> 20), holes, mapped, (unsigned)(best_l >> 20));
  if (!best_a) return NULL;
  if (sec_a) {   /* stash the runner-up for OC window 2 */
    u64 a2 = (sec_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
    if (a2 < sec_a + sec_l) {
      u64 v2 = ((sec_a + sec_l) - a2) & ~(MMAP_ARENA_ALIGN - 1);
      if (v2 > want) v2 = want;
      if (v2) { g_oc_win2 = (void *)a2; g_oc_win2_sz = v2; }
    }
  }
  u64 aligned = (best_a + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
  if (aligned >= best_a + best_l) return NULL;
  u64 avail = ((best_a + best_l) - aligned) & ~(MMAP_ARENA_ALIGN - 1);
  if (!avail) return NULL;
  if (avail > want) avail = want;
  *out_size = avail;
  return (void *)aligned;
}

/* Try to set up alias-region overcommit, recording each step's outcome into the
 * g_oc_* globals (logged from main). Alias-region overcommit turned out to be
 * impossible on this process: svcMapPhysicalMemory requires a non-zero kernel
 * "system resource" pool (for page-table/block bookkeeping) and our title-override
 * process has none -> it returns InvalidState (0xfa01). The unsafe pool
 * (svcMapPhysicalMemoryUnsafe) is ~44MB and already consumed. So we just record the
 * diagnostics and stay on the fully heap-backed arena. (Confirmed via Atmosphere
 * kern_svc_physical_memory.cpp: `R_UNLESS(GetTotalSystemResourceSize() > 0,
 * ResultInvalidState())`.) */
static int overcommit_setup(void *addr, size_t size, size_t so_zone,
                            void **out_addr, size_t *out_fake) {
  (void)addr; (void)size; (void)so_zone; (void)out_addr; (void)out_fake;
  g_oc_hint_map   = envIsSyscallHinted(0x2c);
  g_oc_hint_unmap = envIsSyscallHinted(0x2d);
  svcGetInfo(&g_alias_base, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&g_alias_size, InfoType_AliasRegionSize,    CUR_PROCESS_HANDLE, 0);
  g_oc_alias_mb = (unsigned)(g_alias_size >> 20);
  svcGetInfo(&g_oc_sysres, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
  return 0;   /* no system resource -> svcMapPhysicalMemory unusable; heap-backed */
}

/* Reserve a slice of address space for the .so images; the rest is the newlib
 * heap the engine mallocs from. (Verbatim from cr3_nx.) */
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  const size_t MB = 1024 * 1024;
  size_t so_zone = SO_REGION_BYTES;
  if (so_zone > size / 2)
    so_zone = size / 2;

  extern char *fake_heap_start;
  extern char *fake_heap_end;

  /* Preferred path: alias-region overcommit. Secures the virtual window and
   * test-commits a page FIRST, then shrinks the heap to [newlib + so_zone] so the
   * freed physical (~2.5GB) is available for on-demand commits. Everything is
   * secured before the shrink so a failure can't strand us with a shrunk heap. */
  void *oc_addr; size_t oc_fake;
  if (overcommit_setup(addr, size, so_zone, &oc_addr, &oc_fake)) {
    fake_heap_start = (char *)oc_addr;
    fake_heap_end   = (char *)oc_addr + oc_fake;
    heap_so_base    = (void *)ALIGN_MEM((uintptr_t)oc_addr + oc_fake, 0x1000);
    heap_so_limit   = so_zone;
    return;
  }

  /* Fallback: fully heap-backed 256MB-aligned arena (no overcommit). */
  const size_t big_align    = MMAP_ARENA_ALIGN;
  const size_t newlib_floor = 448 * MB;   /* malloc + il2cpp managed/GC heap */
  size_t arena_sz = MMAP_ARENA_RESERVE;
  size_t fake_heap_size;

  if (size > so_zone + big_align + newlib_floor + 256 * MB) {
    size_t avail = size - so_zone - big_align - newlib_floor;
    if (arena_sz > avail) arena_sz = avail & ~(big_align - 1);   /* clamp to RAM */
    fake_heap_size = size - so_zone - arena_sz - big_align;       /* newlib gets the rest */
  } else {
    /* heap too small for a dedicated arena (e.g. applet mode): skip it; the mmap
     * allocator falls back to a memalign-backed bitmap arena. */
    fake_heap_size = (size > so_zone) ? size - so_zone : size / 2;
    arena_sz = 0;
  }

  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base  = (void *)ALIGN_MEM((uintptr_t)addr + fake_heap_size, 0x1000);
  heap_so_limit = so_zone;

  if (arena_sz) {
    g_mmap_arena_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + so_zone, big_align);
    g_mmap_arena_size = arena_sz;
  }
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77)) fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78)) fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73)) fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE) fatal_error("Own process handle is unavailable.");
}

void nx_gpu_probe(void);                      /* nx_gpu_probe.c */

static void nx_tree_stats(const char *path, unsigned *nfiles, uint64_t *nbytes) {
  DIR *d = opendir(path);
  if (!d) return;
  struct dirent *e; char child[1024];
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) nx_tree_stats(child, nfiles, nbytes);
    else { if (nfiles) (*nfiles)++; if (nbytes) *nbytes += (uint64_t)st.st_size; }
  }
  closedir(d);
}

/* Unity and IL2CPP WRITE into these at runtime (extracted metadata,
 * il2cpp/unity.ver). The pack is read-only and nx_rmtree() takes the whole
 * loose tree with it, so recreate the empty directories for those writes to
 * land in -- otherwise open(".../il2cpp/unity.ver") fails, Unity decides the
 * asset pack contains no Unity data, and raises its fatal dialog.
 * Ported from the Deus Ex GO loader (create_asset_skeleton). */
/* Dump a directory tree (one level deep) with sizes. The il2cpp extraction
 * has been failing invisibly; this makes the on-disk result part of the log. */
static void nx_dump_dir(const char *path, const char *tag) {
  DIR *d = opendir(path);
  if (!d) { debugPrintf("[dump] %s %s: (absent)\n", tag, path); return; }
  debugPrintf("[dump] %s %s:\n", tag, path);
  struct dirent *e; int n = 0;
  while ((e = readdir(d)) != NULL && n < 32) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char child[512];
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) != 0) { debugPrintf("[dump]    %-40s (stat failed)\n", e->d_name); }
    else if (S_ISDIR(st.st_mode)) { debugPrintf("[dump]    %-40s <dir>\n", e->d_name); }
    else { debugPrintf("[dump]    %-40s %lld bytes%s\n", e->d_name,
                       (long long)st.st_size,
                       st.st_size == 0 ? "   *** EMPTY ***" : ""); }
    n++;
  }
  closedir(d);
  if (!n) debugPrintf("[dump]    (empty)\n");
}

static void nx_create_asset_skeleton(void) {
  /* Relative suffixes, joined to the runtime root below. They cannot be a
   * static array of nx_rp() results: the root is resolved at runtime, so those
   * are not constant initialisers -- and nx_rp's rotating buffer pool would be
   * exhausted by seven live entries anyway. */
  static const char *dirs[] = {
    "/assets",
    "/assets/bin",
    "/assets/bin/Data",
    "/assets/bin/Data/Managed",
    "/assets/bin/Data/Managed/Metadata",
    "/assets/bin/Data/Managed/Resources",
    "/assets/bin/Data/Resources",
    "/assets/aa",
    "/assets/aa/Android",
    /* aa/ is recreated because Phigros keeps its Addressables content there and
     * some Unity paths expect the directory to exist even when every file in it
     * is served from the pack. Empty dirs are free. */
    /* NOT the il2cpp subdirectories: Unity stages into il2cpp_tmp and renames
     * into place,
     * and a rename onto an existing non-empty directory fails. Let Unity
     * create those itself (mkdir_fake is real and now logs). */
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++) {
    char d[768];
    snprintf(d, sizeof d, "%s%s", nx_root(), dirs[i]);
    mkdir(d, 0777);   /* EEXIST is fine */
  }
  debugPrintf("[pack] recreated writable dir skeleton (%u dirs)\n",
              (unsigned)(sizeof(dirs) / sizeof(*dirs)));
}

static void nx_rmtree(const char *path) {
  DIR *d = opendir(path);
  if (!d) { unlink(path); return; }
  struct dirent *e;
  char child[1024];
  while ((e = readdir(d)) != NULL) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    snprintf(child, sizeof child, "%s/%s", path, e->d_name);
    struct stat st;
    if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) nx_rmtree(child);
    else unlink(child);
  }
  closedir(d);
  rmdir(path);
}

/* mounted != 0 -> a pack is live; required files live inside it and the loose
 * tree has been deleted, so consult the pack before the on-disk stat(). */
static void check_data(int mounted) {
  char path[768];
  struct stat st;

  /* The three native libraries. Never packed.
   *
   * The error names the FULL path. The first report of this check firing was a
   * wrong FOLDER, not a missing file -- the .nro builds as phigros_nx.nro, so
   * people make switch/phigros_nx/ while GAME_FOLDER said "phigros", and every
   * path missed by one directory. An error saying only "missing libmain.so"
   * sends you hunting through an APK you already extracted correctly.
   * That whole failure mode is gone now (nx_root.c finds the folder), but the
   * path stays in the message: if this ever fires again it should say where. */
  const char *libs[] = { LIB_MAIN, LIB_UNITY, LIB_IL2CPP };
  for (unsigned i = 0; i < sizeof(libs)/sizeof(*libs); i++) {
    snprintf(path, sizeof path, "%s/%s", DATA_ROOT, libs[i]);
    if (stat(path, &st) == 0) continue;

    /* No folder-name guessing any more -- nx_root_init() already searched
     * /switch for the folder that actually holds libmain.so. If we are still
     * here, the file genuinely is not on the card. */
    fatal_error("Missing library:\n  %s\n\n"
                "Copy it from lib/arm64-v8a/ of your APK.\n\n"
                "Data root was chosen by: %s",
                path, nx_root_how());
  }

  /* Required assets, from the MEASURED layout (phigros_assets.h). Sizes come
   * from a known-good complete copy; a mismatch is a warning, not an error,
   * because your game version may legitimately differ. Absence is fatal. */
  for (int i = 0; i < PHI_REQUIRED_ASSETS_N; i++) {
    const PhiAssetReq *r = &PHI_REQUIRED_ASSETS[i];
    uint64_t have = 0;
    int found = 0;
#if PHI_ASSET_PACK
    if (mounted && asset_pack_stat_path(r->path, &have, NULL)) found = 1;
#else
    (void)mounted;
#endif
    if (!found) {
      snprintf(path, sizeof path, "%s/%s", DATA_ROOT, r->path);
      if (stat(path, &st) == 0) { found = 1; have = (uint64_t)st.st_size; }
    }
    if (!found)
      fatal_error("Missing data file:\n  %s/%s\n(%s)\n\n"
                  "Check your SD card layout -- see README.",
                  DATA_ROOT, r->path, r->what);
    if (r->size && have != r->size)
      debugPrintf("[assets] size differs for %s: %llu, reference copy has %llu "
                  "(different game version? not fatal)\n",
                  r->path, (unsigned long long)have, (unsigned long long)r->size);
  }

#if PHI_ASSET_PACK
  /* Whole-pack sanity: one number that catches a partial copy ANYWHERE, before
   * the two targeted counts below say which part. A real report showed 2572
   * entries against an expected 2958 -- the shortfall was entirely bin/Data,
   * but the total is what makes "something is missing" obvious at a glance in
   * a log, and it is one integer. */
  if (mounted) {
    const size_t n = asset_pack_entry_count();
    if (n && n != (size_t)PHI_ASSET_FILE_COUNT)
      debugPrintf("[assets] pack holds %zu entries; reference copy has %d "
                  "(%s)\n", n, PHI_ASSET_FILE_COUNT,
                  n < (size_t)PHI_ASSET_FILE_COUNT ? "INCOMPLETE COPY?"
                                                   : "different game version?");
  }
#endif

  /* ---- engine data inventory --------------------------------------------
   * The bundle check below counts SONGS. This counts the engine's own data --
   * scenes, resources, metadata under assets/bin/Data. They fail differently:
   * missing bundles means missing content, missing bin/Data means the engine
   * has nothing to build a scene FROM, so it renders an empty frame forever.
   * That is a black screen with a perfectly healthy render loop, and it is
   * indistinguishable from a port bug unless something counts the files.
   *
   * A real report had 55 of 441 here while all 2514 bundles were present, and
   * the loader booted happily into a black screen: the required-files list
   * above only names a handful of entry points, and every one of them happened
   * to be in the 55. Counting is what catches a partial copy. */
  {
    unsigned nd = 0;
    const char *src = "loose tree";
#if PHI_ASSET_PACK
    if (mounted) {
      src = "pack";
      void *pd = asset_pack_opendir_path("assets/bin/Data");
      if (pd) {
        while (asset_pack_readdir_path(pd, NULL, NULL) != NULL) nd++;
        asset_pack_closedir_path(pd);
      }
    }
#endif
    if (nd == 0) {
      snprintf(path, sizeof path, "%s/assets/bin/Data", DATA_ROOT);
      DIR *d = opendir(path);
      if (d) { struct dirent *e; src = "loose tree";
        while ((e = readdir(d)) != NULL) if (e->d_name[0] != '.') nd++;
        closedir(d); }
    }
    /* Fatal below half, because that is not a version difference -- it is an
     * incomplete copy, and every symptom it produces looks like a port bug. */
    if (nd * 2 < (unsigned)PHI_ASSET_DATA_COUNT)
      fatal_error("Incomplete game data.\n\n"
                  "assets/bin/Data has %u files; this build expects about %d.\n"
                  "All %d song bundles are fine -- it is the engine data that is\n"
                  "missing, so the game would boot to a BLACK SCREEN.\n\n"
                  "Re-copy the whole assets/ folder out of your APK, then delete\n"
                  "assets.nxpack and assets.nxpack.idx so it is rebuilt.\n"
                  "(counted via: %s)",
                  nd, PHI_ASSET_DATA_COUNT, PHI_AA_BUNDLE_COUNT, src);
    if (nd != (unsigned)PHI_ASSET_DATA_COUNT)
      debugPrintf("[assets] bin/Data has %u files, reference copy has %d "
                  "(different game version? not fatal)\n", nd, PHI_ASSET_DATA_COUNT);
    else
      debugPrintf("[assets] bin/Data %u/%d files (%s)\n", nd, PHI_ASSET_DATA_COUNT, src);
  }

  /* ---- Addressables bundle inventory ------------------------------------
   * The catalog references PHI_AA_BUNDLE_COUNT bundles and every one of them
   * was present in the reference copy -- 0 missing, 0 extra. So a short count
   * here means an incomplete copy, which is worth saying BEFORE the engine
   * starts rather than letting it fail on whichever bundle it happens to want
   * first (an error that surfaces as a hang deep in a loading screen).
   *
   * Counted, not stat()ed one by one: 2514 individual stat()s on SD would add
   * real time to every boot. A readdir of one directory is cheap. */
  {
    unsigned nb = 0;
    const char *src = "loose tree";

    /* MUST consult the pack when one is mounted. Once assets are packed the
     * loose assets/aa/Android directory no longer exists on disk, so a plain
     * opendir() here returns NULL, nb stays 0, and the check below would call
     * fatal_error -- refusing to boot on the second launch of a perfectly good
     * install, immediately after the pack build reported success. That is the
     * worst shape a bug can have: triggered by the thing that just worked. */
#if PHI_ASSET_PACK
    if (mounted) {
      src = "pack";
      void *pd = asset_pack_opendir_path(PHI_AA_BUNDLE_DIR);
      if (pd) {
        const char *nm;
        while ((nm = asset_pack_readdir_path(pd, NULL, NULL)) != NULL) {
          const size_t L = strlen(nm);
          if (L > 7 && !strcmp(nm + L - 7, PHI_AA_BUNDLE_EXT)) nb++;
        }
        asset_pack_closedir_path(pd);
      }
    }
#endif
    if (nb == 0) {                       /* not packed, or pack had no dir */
      snprintf(path, sizeof path, "%s/%s", DATA_ROOT, PHI_AA_BUNDLE_DIR);
      DIR *d = opendir(path);
      if (d) {
        struct dirent *e;
        src = "loose tree";
        while ((e = readdir(d)) != NULL) {
          const char *nm = e->d_name;
          const size_t L = strlen(nm);
          if (L > 7 && !strcmp(nm + L - 7, PHI_AA_BUNDLE_EXT)) nb++;
        }
        closedir(d);
      }
    }
    if (nb == 0) {
      fatal_error("No Addressables bundles found in:\n%s\n\n"
                  "Expected %d .bundle files. Almost all of the game's content\n"
                  "lives here -- copy the whole assets/aa/ tree from your APK.\n"
                  "(checked: %s)",
                  PHI_AA_BUNDLE_DIR, PHI_AA_BUNDLE_COUNT, src);
    } else if (nb < (unsigned)PHI_AA_BUNDLE_COUNT) {
      debugPrintf("[assets] WARNING: %u of %d bundles present in %s.\n"
                  "[assets] The copy looks incomplete. Expect a hang or a\n"
                  "[assets] missing-asset error once the game asks for one.\n",
                  nb, PHI_AA_BUNDLE_COUNT, PHI_AA_BUNDLE_DIR);
    } else {
      debugPrintf("[assets] %u/%d Addressables bundles present (%s)\n",
                  nb, PHI_AA_BUNDLE_COUNT, src);
    }
  }
}

/* load a module, advance the .so arena, resolve its imports against the table */
static int load_module(so_module *mod, const char *name) {
  char path[768];
  snprintf(path, sizeof path, "%s/%s", DATA_ROOT, name);
  if (so_load(mod, path, heap_so_base, heap_so_limit) < 0)
    return -1;
  size_t used = ALIGN_MEM(mod->load_size, 0x1000);
  heap_so_base = (char *)heap_so_base + used;
  heap_so_limit -= used;
  debugPrintf("[mod] %-14s virtbase=%p size=0x%zx  (resolve: addr - virtbase = vaddr)\n",
              name, mod->load_virtbase, mod->load_size);
  crx_resolve_imports(mod);   /* so_resolve(mod, dynlib_functions, ...) */
  /* NOTE: so_patch_stack_canaries() intentionally NOT called. Per-thread bionic
   * TLS (install_bionic_tls) makes the engine's stack-protector guard consistent,
   * so the canary checks pass on their own. NOPing 2000+ b.ne sites risked a
   * false-positive in non-canary code (e.g. allocator list logic) -> corruption. */
  return 0;
}

/* engine entry points (unity_entrypoints.h), resolved post-finalize */
static fn_initJni  Unity_initJni;
static fn_gfxstate Unity_nativeRecreateGfxState;
static fn_v        Unity_nativeSendSurfaceChanged;
static fn_z        Unity_nativeRender;
static fn_inject   Unity_nativeInjectEvent;
static fn_v        Unity_nativeResume;
static fn_vz       Unity_nativeFocusChanged;
static fn_z        Unity_nativeDone;
static fn_v        Unity_nativeApplicationUnload;

/* ---------------------------------------------------------------------------
 * In-memory libunity patch (ported from VLN's nx_patch_unity_regions): the SD
 * card now ships the STOCK libunity.so and the boot patches it after load,
 * instead of distributing a pre-modified binary. 23 instruction words, from a
 * byte-exact diff of the known-good patched .so vs stock (Unity 2022.3.62f2):
 * 21 sites relax the allocator's memory-region granularity 256MB->64MB so the
 * engine fits the so_loader address space on a 4GB Switch, plus two branch
 * forces (0x5d24cc cond->uncond, 0x5d4e98 ldr->skip) from the same known-good
 * build. Verify-first like VLN: every original word must match before anything
 * is written; a fully pre-patched .so is detected and accepted; any other
 * mismatch leaves the binary untouched (different Unity build) with a loud log.
 * ------------------------------------------------------------------------- */
static int nx_patch_libunity(uintptr_t ub) {
  /* PvZ Fusion 3.6.1 (Unity 2022.3.62f1c1): tables live in nx_patch_phigros.h.
   * VERIFY-FIRST like the original -- every {from} word must match before ANY
   * write; a fully pre-patched .so is accepted; any mismatch patches nothing. */
  const NxPatchWord *P = PHI_PATCH_WORDS;
  const int N = PHI_PATCH_WORDS_N;
  int stock = 0, patched = 0;
  for (int i = 0; i < N; i++) {
    uint32_t cur = *(volatile uint32_t *)(ub + P[i].off);
    if (cur == P[i].from) stock++;
    else if (cur == P[i].to) patched++;
    else {
      debugPrintf("[patch] libunity word mismatch @+0x%x: have 0x%08x want 0x%08x -> SKIP all (offset table may be off for this build)\n",
                  (unsigned)P[i].off, cur, P[i].from);
      return 0;
    }
  }
  if (patched == N) {
    debugPrintf("[patch] libunity already pre-patched (%d sites) -- ok\n", N);
  } else if (stock != N) {
    debugPrintf("[patch] libunity PARTIALLY patched (%d/%d) -> SKIP (won't mix builds)\n", patched, N);
    return 0;
  } else {
    for (int i = 0; i < N; i++)
      so_patch_code((void *)(ub + P[i].off), &P[i].to, sizeof P[i].to);
    debugPrintf("[patch] libunity region granularity 256MB->64MB patched (%d sites)\n", N);
  }
  /* Optional branch forces (only when you have located them; see nx_patch_phigros.h). */
  if (PHI_BRANCH_FORCES_N > 0) {
    const NxPatchWord *B = PHI_BRANCH_FORCES;
    for (int i = 0; i < PHI_BRANCH_FORCES_N; i++) {
      uint32_t cur = *(volatile uint32_t *)(ub + B[i].off);
      if (cur == B[i].from)
        so_patch_code((void *)(ub + B[i].off), &B[i].to, sizeof B[i].to);
      else if (cur != B[i].to)
        debugPrintf("[patch] branch-force @+0x%x mismatch (have 0x%08x) -> skip\n",
                    (unsigned)B[i].off, cur);
    }
    debugPrintf("[patch] libunity branch forces applied (%d sites)\n", PHI_BRANCH_FORCES_N);
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * PvZ 62f1c1 first-boot il2cpp hacks. With the GC stop-the-world bridge fixed for
 * PvZ (libc_shim.c pthread_kill_gc now reads the correct suspend/restart/ack
 * globals), the Boehm GC works, so -- exactly like the VLN port -- we do NOT try to
 * disable it. An earlier attempt to il2cpp_gc_disable() mid-il2cpp_init deadlocked
 * on a GC lock held by a not-yet-running helper thread (verified on hardware: hung
 * right after set_mode, UnityMain parked in a futex lock). The GC now runs normally
 * and the bridge keeps its POSIX-signal stop-the-world from hanging. All this does
 * is redirect Time.get_* to our frame clock. Called once at boot, before the loop.
 * ------------------------------------------------------------------------- */
/* Is this address mapped and readable? A patch must never fault merely by
 * CHECKING its site -- round 120 killed boot doing exactly that, reading
 * il2cpp+0x159dfcc before the module's text was mapped. Verifying costs one
 * syscall and turns an unmapped site into a skipped patch. */
static int nx_addr_readable(uintptr_t a) {
  MemoryInfo mi; u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
  if (mi.type == MemType_Unmapped) return 0;
  if (!(mi.perm & Perm_R)) return 0;
  return 1;
}

#if PHI_HAVE_LIVENESS_GUARD
/* ---- liveness guard veneer (round 129) -----------------------------------
 * Entered in place of il2cpp's add_process_object(obj, state). Derivation and
 * rationale: nx_patch_phigros.h. Register facts this depends on, all read
 * off the disassembly rather than assumed:
 *
 *   x0 = obj, x1 = state (LivenessState*), state->filter at +8
 *   obj->klass at +0, low bit = LivenessState's mark bit
 *   klass->typeHierarchy at +0xc8, ->typeHierarchyDepth at +0x130,
 *   ->flags halfword at +0x135, has_references = bit 5
 *
 * x16/x17 are IP0/IP1 and x8 is caller-saved, so all three are free at a
 * function entry; x0/x1/x19/x20/x21/x30 are untouched on the path that falls
 * through to the original body. Both early exits return w0 = 0, which is what
 * the two original early exits (null object, already-marked) also return. */
/* HIDDEN visibility is load-bearing, not tidiness. The veneer addresses this
 * with adrp/:lo12:, and we build -fPIE: a default-visibility symbol is formally
 * preemptible, which is the case where GNU ld rejects R_AARCH64_ADR_PREL_PG_HI21
 * ("may be overridden"). Hidden makes it non-preemptible, so the pair is always
 * a valid link-time resolution. */
__attribute__((visibility("hidden"))) uint64_t g_fn_liveness_body = 0;
extern void fn_liveness_guard(void);

__asm__(
  ".text\n"
  ".balign 4\n"
  ".global fn_liveness_guard\n"
  ".type   fn_liveness_guard, %function\n"
  "fn_liveness_guard:\n"
  "    cbz   x0, .Lfnlg_zero\n"                  /* !object                   */
  "    ldr   x16, [x0]\n"                        /* obj->klass, tagged        */
  "    tbnz  w16, #0, .Lfnlg_zero\n"             /* IS_MARKED                 */
  "    ands  x16, x16, #0xfffffffffffffffe\n"    /* GET_CLASS                 */
  "    b.eq  .Lfnlg_zero\n"                      /* klass == NULL  (r119)     */
  "    add   x17, x16, #0x135\n"                 /* flags: offset is odd, so  */
  "    ldrh  w17, [x17]\n"                       /*  ldrh cannot fold it      */
  "    tbnz  w17, #5, .Lfnlg_orig\n"             /* has_references: unchanged */
  "    ldr   x17, [x1, #8]\n"                    /* state->filter             */
  "    cbz   x17, .Lfnlg_orig\n"                 /* no filter: unchanged      */
  "    ldr   x16, [x16, #0xc8]\n"                /* klass->typeHierarchy      */
  "    cbz   x16, .Lfnlg_zero\n"                 /* NEW: never set up -> false */
  ".Lfnlg_orig:\n"
  "    stp   x30, x21, [sp, #-0x20]!\n"          /* the four clobbered        */
  "    stp   x20, x19, [sp, #0x10]\n"            /*  prologue instructions,   */
  "    ldr   x8,  [x0]\n"                        /*  minus the cbz we did     */
  "    adrp  x16, g_fn_liveness_body\n"
  "    add   x16, x16, :lo12:g_fn_liveness_body\n"
  "    ldr   x16, [x16]\n"
  "    br    x16\n"                              /* -> add_process_object+0x10 */
  ".Lfnlg_zero:\n"
  "    mov   w0, wzr\n"
  "    ret\n"
  ".size fn_liveness_guard, .-fn_liveness_guard\n"
);
#endif

/* REVERTED (round 123). A per-frame attempt to switch off
 * RemoteConfigObject.ApplyRemoteConfigObjects via il2cpp_class_from_name() and
 * friends crashed on the FIRST loop iteration: zero frames rendered, no log line
 * from the function itself, fault at libil2cpp+0x1578a68 reading klass+0x135 --
 * i.e. inside il2cpp, walking metadata that is not ready until the engine's
 * first nativeRender has run.
 *
 * I called that change "fail-safe by construction" because every return value
 * was checked. That was wrong: checking what a function RETURNS does nothing
 * when the function faults internally. Round 120 was the same mistake one level
 * out (the check itself was a memory access); here the CALL itself is the unsafe
 * act, and no amount of validating its result helps.
 *
 * The proven technique in this port is the opposite: patch the faulting
 * instruction, having verified its bytes first (round 119). If the
 * IDataSource.ConvertFromTo crash needs addressing, that is the way to do it --
 * not by driving the il2cpp API from outside the engine's own lifecycle. */

static int g_boot_hacks_done = 0;
static void nx_boot_il2cpp_hacks(void) {
  if (g_boot_hacks_done) return;
  g_boot_hacks_done = 1;
  uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;

  /* redirect managed Time.get_* to our frame clock (PvZ RVAs, metadata-derived) */
  struct { uint32_t off; void *fn; const char *nm; } th[] = {
    { PHI_IL2_get_realtimeSinceStartup, (void *)&nx_realtime_since_startup, "realtimeSinceStartup" },
    { PHI_IL2_get_deltaTime,            (void *)&nx_delta_time,             "deltaTime" },
    { PHI_IL2_get_unscaledDeltaTime,    (void *)&nx_delta_time,             "unscaledDeltaTime" },
    { PHI_IL2_get_smoothDeltaTime,      (void *)&nx_delta_time,             "smoothDeltaTime" },
    { PHI_IL2_get_time,                 (void *)&nx_time_f,                 "time" },
    { PHI_IL2_get_unscaledTime,         (void *)&nx_time_f,                 "unscaledTime" },
    { PHI_IL2_get_timeSinceLevelLoad,   (void *)&nx_time_f,                 "timeSinceLevelLoad" },
    { PHI_IL2_get_frameCount,           (void *)&nx_frame_count,            "frameCount" },
  };
  for (unsigned i = 0; i < sizeof(th) / sizeof(th[0]); i++) {
    uint32_t cur = *(volatile uint32_t *)(b + th[i].off);
    if (cur != PHI_TIME_THUNK_WORD) {
      debugPrintf("[boot] SKIP Time.get_%s @il2cpp+0x%x: 0x%08x is not the "
                  "expected icall-thunk prologue -- re-dump (PORTING sec 6)\n",
                  th[i].nm, th[i].off, cur);
      continue;
    }
    uint32_t s[4];
    s[0] = 0x58000050u; s[1] = 0xd61f0200u;   /* ldr x16,#8 ; br x16 */
    memcpy(&s[2], &th[i].fn, 8);
    so_patch_code((void *)(b + th[i].off), s, sizeof s);
    debugPrintf("[boot] hooked Time.get_%s @il2cpp+0x%x\n", th[i].nm, th[i].off);
  }
#if PHI_FORCE_SPLASH_FINISH
  /* Force SplashScreen.m_isSplashFinished (round 57): the end-of-fade Animation
   * Event that calls SplashFinished() does not fire on our port, so patch the
   * coroutine's flag load (ldrb w8,[x21,#0x48]) to mov w8,#1. */
  { uint32_t *site = (uint32_t *)(b + PHI_IL2_SPLASH_FINISHED_CHECK);
    if (*site == 0x394122a8u) {
      uint32_t mov1 = 0x52800028u;
      so_patch_code((void *)site, &mov1, sizeof mov1);
      debugPrintf("[boot] forced splash-finish gate @il2cpp+0x%x (was ldrb, now mov w8,#1)\n",
                  (unsigned)PHI_IL2_SPLASH_FINISHED_CHECK);
    } else {
      debugPrintf("[boot] SKIP splash-finish force @il2cpp+0x%x: 0x%08x != expected ldrb 0x394122a8\n",
                  (unsigned)PHI_IL2_SPLASH_FINISHED_CHECK, *site);
    } }
#endif
#if PHI_BYPASS_UNITY_SPLASH
  /* Bypass Unity native splash (round 58): GetShouldShowSplashScreen -> 0 so
   * the engine never holds on its own splash screen. libunity, not il2cpp. */
  { uintptr_t ub2 = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *gs = (uint32_t *)(ub2 + PHI_GET_SHOULD_SHOW_SPLASH);
    if (gs[0] == 0xf81f0ffeu && gs[1] == 0x941aaa72u) {
      uint32_t ret0[2] = { 0x52800000u, 0xd65f03c0u };  /* mov w0,#0 ; ret */
      so_patch_code((void *)gs, ret0, sizeof ret0);
      so_flush_caches(&unity_mod);
      debugPrintf("[boot] bypassed Unity native splash (GetShouldShowSplashScreen->0) @libunity+0x%x\n",
                  (unsigned)PHI_GET_SHOULD_SHOW_SPLASH);
    } else if (gs[0] == 0x52800000u && gs[1] == 0xd65f03c0u) {
      debugPrintf("[boot] Unity native splash already bypassed\n");
    } else {
      debugPrintf("[boot] SKIP native-splash bypass @libunity+0x%x: 0x%08x 0x%08x != expected\n",
                  (unsigned)PHI_GET_SHOULD_SHOW_SPLASH, gs[0], gs[1]);
    } }
#endif
#if PHI_PRELOAD_NO_EARLY_EXIT
  /* Round 75: keep retrying SingleStep for the whole budget instead of
   * abandoning the frame when the integrate queue is briefly empty. */
  { uintptr_t ub4 = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *eb = (uint32_t *)(ub4 + PHI_PRELOAD_EXIT_BRANCH);
    uint32_t nop = 0xd503201fu;
    if (eb[0] == 0x36000160u) {
      so_patch_code((void *)eb, &nop, sizeof nop);
      so_flush_caches(&unity_mod);
      debugPrintf("[boot] preload early-exit NOPed @libunity+0x%x\n",
                  (unsigned)PHI_PRELOAD_EXIT_BRANCH);
    } else if (eb[0] == nop) {
      debugPrintf("[boot] preload early-exit already NOPed\n");
    } else {
      debugPrintf("[boot] SKIP preload early-exit @libunity+0x%x: 0x%08x != expected\n",
                  (unsigned)PHI_PRELOAD_EXIT_BRANCH, eb[0]);
    } }
#endif
#if PHI_PRELOAD_BUDGET_MS
  /* Raise the async-load per-frame budget (round 68). See config.h. */
  { uintptr_t ub3 = (uintptr_t)unity_mod.load_virtbase;
    volatile uint32_t *tb = (uint32_t *)(ub3 + PHI_PRELOAD_BUDGET_TABLE_LOAD);
    volatile uint32_t *df = (uint32_t *)(ub3 + PHI_PRELOAD_BUDGET_DEFAULT);
    uint32_t movN = 0x52800015u | (((uint32_t)PHI_PRELOAD_BUDGET_MS & 0xffffu) << 5);
    if (tb[0] == 0xb8687935u && df[0] == 0x52800095u) {
      so_patch_code((void *)tb, &movN, sizeof movN);
      so_patch_code((void *)df, &movN, sizeof movN);
      so_flush_caches(&unity_mod);
      debugPrintf("[boot] async-load budget -> %d ms/frame @libunity+0x%x\n",
                  (int)PHI_PRELOAD_BUDGET_MS, (unsigned)PHI_PRELOAD_BUDGET_TABLE_LOAD);
    } else if (tb[0] == movN && df[0] == movN) {
      debugPrintf("[boot] async-load budget already raised\n");
    } else {
      debugPrintf("[boot] SKIP async-load budget @libunity+0x%x: 0x%08x 0x%08x != expected\n",
                  (unsigned)PHI_PRELOAD_BUDGET_TABLE_LOAD, tb[0], df[0]);
    } }
#endif
  so_flush_caches(&il2cpp_mod);   /* make the Time-hook code patches live */
  debugPrintf("[boot] Time.get_* hooks installed; GC left running (handled by bridge)\n");
}

int main(int argc, char *argv[]) {
  /* FIRST. Everything downstream -- the log path, the chdir, every asset
   * path -- is relative to this, so it has to be resolved before the first
   * debugPrintf opens debug.log. Works from any folder under /switch. */
  nx_root_init(argc, argv);
  (void)argc; (void)argv;
  socketInitializeDefault();
  /* debug.log is opened O_APPEND, so one file can hold several runs. Say so at
   * the top of each: a log with two boots in it looks like one run that created
   * twice as many of everything, and that misread cost real time once already. */
  /* Stamp the source revision and build time. Three logs in a row turned out to
   * be the same stale file from an older .nro, which is invisible unless the
   * build says who it is. __DATE__/__TIME__ change on every rebuild, so two
   * logs from "the same build" that disagree here are not the same build. */
  debugPrintf("\n[boot] ======== phigros_nx start (region64mb) ========\n"
              "[boot] rev %s  built %s %s\n"
              "[boot] (debug.log APPENDS -- more than one banner means the file "
              "holds multiple runs)\n",
              PHI_SRC_REV, __DATE__, __TIME__);

  /* Load config.txt. When the file is
   * missing, autogenerate a documented one with the defaults; when it holds
   * retired options (`portrait` among them now), rewrite it without them. */
  {
    int crc = read_config(nx_rp("/" CONFIG_NAME));
    if (crc != 0) {
      write_config(nx_rp("/" CONFIG_NAME));
      debugPrintf("[boot] %s config.txt (handheld_res=%d docked_res=%d)\n",
                  crc < 0 ? "created" : "rewrote",
                  config.handheld_res, config.docked_res);
    } else {
      debugPrintf("[boot] config: handheld_res=%d docked_res=%d\n",
                  config.handheld_res, config.docked_res);
    }
  }

  /* Sweep Unity's case-sensitivity probe files: CASESENSITIVETEST<guid> strays
   * from older builds, plus the single hidden scratch the probe is redirected
   * to now (libc_shim.c casetest_redirect). */
  {
    DIR *dd = opendir(DATA_ROOT);
    int swept = 0;
    if (dd) {
      struct dirent *de;
      while ((de = readdir(dd))) {
        if (strncasecmp(de->d_name, "CASESENSITIVETEST", 17) == 0 ||
            strcmp(de->d_name, ".casetest") == 0) {
          char pth[320]; snprintf(pth, sizeof pth, "%s/%s", nx_root(), de->d_name);
          if (unlink(pth) == 0) swept++;
        }
      }
      closedir(dd);
    }
    if (swept) debugPrintf("[boot] swept %d case-sensitivity probe file(s)\n", swept);
  }

  /* CWD fix (mirrors MMX enter_data_dir): title-override / hbloader leaves the
   * working dir at the .nro folder or the SD root, NOT the game dir. Unity &
   * il2cpp read many files through *relative* paths ("assets/bin/Data/...") and
   * our basename_fallback stats relative to cwd, so a wrong cwd silently yields
   * empty/missing reads -> NULL il2cpp classes. chdir into DATA_ROOT so every
   * relative read resolves under sdmc:/switch/zookeeper. (Absolute "sdmc:/..."
   * reads are unaffected.) */
  {
    char cwd[256] = {0};
    getcwd(cwd, sizeof cwd);
    int rc = chdir(DATA_ROOT);
    char cwd2[256] = {0};
    getcwd(cwd2, sizeof cwd2);
    struct stat st;
    /* NOTE: this probe runs BEFORE the pack is mounted (mount happens ~200
     * lines below), so on a packed install these are expected to read 0 --
     * the loose files are gone and the pack is not up yet. The label below
     * says which source answered so the line cannot be misread as "the data
     * is missing", which is how it wasted time during the round-85/86 hunt.
     * The pack lookups are kept for the case where mounting moves earlier. */
    int reach_assets = stat("assets/bin/Data/globalgamemanagers", &st) == 0;
    int reach_meta   = stat("assets/bin/Data/Managed/Metadata/global-metadata.dat", &st) == 0;
    int reach_guid   = stat("assets/bin/Data/unity_app_guid", &st) == 0;
#if PHI_ASSET_PACK
    const int _packed = asset_pack_active();
    if (_packed) {
      if (!reach_assets) reach_assets = asset_pack_stat_path("assets/bin/Data/globalgamemanagers", NULL, NULL);
      if (!reach_meta)   reach_meta   = asset_pack_stat_path("assets/bin/Data/Managed/Metadata/global-metadata.dat", NULL, NULL);
      if (!reach_guid)   reach_guid   = asset_pack_stat_path("assets/bin/Data/unity_app_guid", NULL, NULL);
    }
#else
    const int _packed = 0;
#endif
  debugPrintf("[boot] data root: %s\n[boot]   found via: %s\n",
              nx_root(), nx_root_how());
    debugPrintf("[boot] cwd was '%s' -> chdir(%s)=%d -> '%s'\n", cwd, DATA_ROOT, rc, cwd2);
    debugPrintf("[boot] reachable(%s): globalgamemanagers=%d metadata=%d unity_app_guid=%d\n",
                _packed ? "pack" : "rel, pre-mount", reach_assets, reach_meta, reach_guid);
  }

  /* Force libunity to RE-EXTRACT il2cpp resources every boot. Observed: when
   * extraction is skipped (il2cpp/unity.ver present), il2cpp mmaps the extracted
   * global-metadata.dat and crashes in Class::Init(NULL); when extraction RUNS,
   * il2cpp uses the full source it reads for the copy and gets past that point.
   * The extracted copy is bad because our shim doesn't flush a writable
   * file-backed mmap back to disk, so it lands truncated. Removing the extracted
   * markers makes libunity redo the extraction each boot (uses the good source).
   * Proper fix = flush writable file-backed mmaps on munmap (tracked separately). */
  {
    /* Wipe BOTH trees, not three hand-listed files: a stale 0-byte
     * global-metadata.dat left directly under il2cpp/ was surviving every
     * boot because none of the three paths below matched it. */
    nx_rmtree(nx_rp("/il2cpp"));
    nx_rmtree(nx_rp("/il2cpp_tmp"));
    int a = unlink(nx_rp("/il2cpp/unity.ver"));
    int b = unlink(nx_rp("/il2cpp/Metadata/global-metadata.dat"));
    int c = unlink(nx_rp("/il2cpp/Resources/mscorlib.dll-resources.dat"));
    debugPrintf("[boot] force re-extract: unlink unity.ver=%d metadata=%d resources=%d\n", a, b, c);
  }

  check_syscalls();
  debugPrintf("[boot] syscalls ok\n");
  {
    extern char *fake_heap_start, *fake_heap_end;
    debugPrintf("[boot] mem layout: newlib=%u MB, mmap arena=%u MB @ %p\n",
                (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024)),
                (unsigned)(g_mmap_arena_size / (1024 * 1024)), g_mmap_arena_base);
    if (g_overcommit)
      debugPrintf("[boot] OVERCOMMIT on: heap shrunk to %u MB, freed %u MB physical; "
                  "arena reserved virtual @ %p (commit on demand)\n",
                  g_oc_heap_mb, g_oc_freed_mb, g_mmap_arena_base);
    else
      debugPrintf("[boot] OVERCOMMIT off (heap-backed): system_resource=%u MB "
                  "(svcMapPhysicalMemory needs >0; unsafe pool exhausted). map_hint=%d alias=%u MB\n",
                  (unsigned)(g_oc_sysres >> 20), g_oc_hint_map, g_oc_alias_mb);
  }

  /* Overcommit feasibility probe. Proper PROT_NONE overcommit on Switch needs a
   * physical-backing primitive (svcMapMemory in the stack region, or
   * svcMapPhysicalMemory in the alias region) plus a region large enough to hold
   * Unity's multi-GB reservations. MMX found svcMapMemory caps at ~2-3 pools (the
   * stack region is ~1GB). Log the region sizes + which mapping svc are granted so
   * we can size/choose the real overcommit (or rule it out) from real numbers. */
  {
    struct { const char *nm; int a, s; } R[] = {
      { "alias", InfoType_AliasRegionAddress, InfoType_AliasRegionSize },
      { "heap",  InfoType_HeapRegionAddress,  InfoType_HeapRegionSize  },
      { "stack", InfoType_StackRegionAddress, InfoType_StackRegionSize },
    };
    for (unsigned i = 0; i < 3; i++) {
      u64 a = 0, s = 0;
      svcGetInfo(&a, R[i].a, CUR_PROCESS_HANDLE, 0);
      svcGetInfo(&s, R[i].s, CUR_PROCESS_HANDLE, 0);
      debugPrintf("[probe] region %-5s base=0x%lx size=%u MB\n",
                  R[i].nm, (unsigned long)a, (unsigned)(s >> 20));
    }
    u64 tot = 0, used = 0;
    svcGetInfo(&tot,  InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize,  CUR_PROCESS_HANDLE, 0);
    debugPrintf("[probe] mem total=%u MB used=%u MB free=%u MB\n",
                (unsigned)(tot >> 20), (unsigned)(used >> 20),
                (unsigned)((tot - used) >> 20));
    debugPrintf("[probe] svc hinted: MapPhysicalMemory(0x2c)=%d UnmapPhysical(0x2d)=%d "
                "MapMemory(0x24)=%d UnmapMemory(0x25)=%d\n",
                envIsSyscallHinted(0x2c), envIsSyscallHinted(0x2d),
                envIsSyscallHinted(0x24), envIsSyscallHinted(0x25));
  }

  /* Decisive probe: does svcMapMemory accept a dst in the (unmapped upper) HEAP
   * region? The stack region works but is only ~2GB (~8 regions). The heap region
   * is 8GB; its upper ~5GB sits unmapped above our heap. If svcMapMemory works
   * there too, we can host Unity's 256MB pools in ~7GB of backable address space
   * (~28 regions) without any libunity patching. Pure diagnostic: map 1 page,
   * verify the sentinel reads back, unmap. */
  {
    u64 hbase = 0, hsize = 0;
    svcGetInfo(&hbase, InfoType_HeapRegionAddress, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&hsize, InfoType_HeapRegionSize,    CUR_PROCESS_HANDLE, 0);
    u64 probe = 0, a = hbase, end = hbase + hsize;
    while (a < end) {
      MemoryInfo mi; u32 pi;
      if (R_FAILED(svcQueryMemory(&mi, &pi, a))) break;
      if (mi.addr + mi.size <= a) break;
      if (mi.type == MemType_Unmapped && mi.size >= MMAP_ARENA_ALIGN) {
        u64 al = (mi.addr + (MMAP_ARENA_ALIGN - 1)) & ~(MMAP_ARENA_ALIGN - 1);
        if (al + 0x1000 <= mi.addr + mi.size) { probe = al; break; }
      }
      a = mi.addr + mi.size;
    }
    if (probe) {
      void *src = memalign(0x1000, 0x1000);
      if (src) {
        *(volatile u32 *)src = 0xABCD1234;
        Result rc = svcMapMemory((void *)probe, src, 0x1000);
        if (R_SUCCEEDED(rc)) {
          u32 v = *(volatile u32 *)probe;
          Result u = svcUnmapMemory((void *)probe, src, 0x1000);
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx rc=0x%x read=0x%x unmap=0x%x WORKS=%d\n",
                      (unsigned long)probe, rc, v, u, v == 0xABCD1234);
          if (R_SUCCEEDED(u)) free(src);
        } else {
          debugPrintf("[heapprobe] heap-region svcMapMemory @ 0x%lx FAILED rc=0x%x\n",
                      (unsigned long)probe, rc);
          free(src);
        }
      }
    } else {
      debugPrintf("[heapprobe] no unmapped 256MB-aligned spot found in heap region\n");
    }
  }

  /* Arm the stack-region overcommit arena. The boot probe confirmed svcMapMemory
   * aliases heap pages into the stack region; Unity reserves ~2.8GB of PROT_NONE
   * pools but commits only ~80MB. Reserve a 1280MB stack-region window (cheap
   * address space) + a 256MB heap commit-pool; the OC arena (libc_shim.c) then
   * holds Unity's big reservations there and aliases pool pages in on mprotect.
   * Any failure leaves OC disabled and the engine runs on the heap-backed arena. */
  {
    void *pool = NULL;
    size_t winsz = 0;
    void *win = oc_find_stack_window(OC_WINDOW_BYTES, &winsz);
    VirtmemReservation *rv = NULL;
    if (win && winsz) {
      virtmemLock();
      rv = virtmemAddReservation(win, winsz);   // keep libnx thread stacks out
      virtmemUnlock();
    }
    if (win && rv && winsz) {
      /* Step down rather than give up. A single memalign() of the full pool used
       * to be all-or-nothing: if it did not fit, OC was DISABLED and the engine
       * fell back to the heap-backed arena alone -- which at its current size has
       * no chance. That made OC_POOL_BYTES dangerous to raise, because the
       * failure mode of asking for too much was a dead boot rather than a
       * smaller pool. Now the ladder retries in 128 MB steps down to
       * OC_POOL_MIN_BYTES, so raising the constant can only ever help. */
      size_t pool_sz = OC_POOL_BYTES;
      for (;;) {
        pool = memalign(0x1000, pool_sz);
        if (pool || pool_sz <= OC_POOL_MIN_BYTES) break;
        pool_sz -= (size_t)128 * 1024 * 1024;
        debugPrintf("[oc] pool %u MB did not fit -- retrying at %u MB\n",
                    (unsigned)((pool_sz + (128u << 20)) >> 20), (unsigned)(pool_sz >> 20));
      }
      if (pool && oc_arena_init(win, winsz, pool, pool_sz)) {
        debugPrintf("[oc] ARMED: window %u MB @ %p, pool %u MB @ %p, heap-backed arena %u MB "
                    "(total reserve %u MB)%s\n",
                    (unsigned)(winsz >> 20), win, (unsigned)(pool_sz >> 20), pool,
                    (unsigned)(g_mmap_arena_size >> 20),
                    (unsigned)((winsz + g_mmap_arena_size) >> 20),
                    pool_sz < OC_POOL_BYTES ? "  [stepped down from OC_POOL_BYTES]" : "");
        if (g_oc_win2 && g_oc_win2_sz) {
          virtmemLock();
          VirtmemReservation *rv2 = virtmemAddReservation(g_oc_win2, g_oc_win2_sz);
          virtmemUnlock();
          if (rv2 && oc_arena_add_window(g_oc_win2, g_oc_win2_sz))
            debugPrintf("[oc] ARMED window 2: %u MB @ %p (total window VA %u MB)\n",
                        (unsigned)(g_oc_win2_sz >> 20), g_oc_win2,
                        (unsigned)((winsz + g_oc_win2_sz) >> 20));
        }
      }
      else
        debugPrintf("[oc] DISABLED: pool=%p init failed -> heap-backed only\n", pool);
    } else {
      debugPrintf("[oc] DISABLED: no usable stack hole (win=%p sz=%u MB rv=%p) -> heap-backed only\n",
                  win, (unsigned)(winsz >> 20), (void *)rv);
    }
  }

  /* fbstub45 PORTRAIT: ZOOKEEPER is a portrait game. Report a portrait surface
   * (W<H) everywhere Unity reads dimensions so the engine renders upright. The
   * compositor stretches the portrait buffer onto the landscape panel. (Stable;
   * the 640x1137 native-render attempt crashed early in boot -- the game's
   * pipeline appears to depend on its Screen.SetResolution low-res path.) */
  if (appletGetOperationMode() == AppletOperationMode_Console) { screen_width = 1080; screen_height = 1920; }
  else                                                         { screen_width = 720;  screen_height = 1280; }

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0)
    debugPrintf("SDL_Init failed: %s\n", SDL_GetError());

#if PHI_ASSET_PACK
  /* round 82: handle the asset pack FIRST. If a pack exists, mount it and
   * skip joining/building (the loose tree is gone). Otherwise join splits,
   * build the pack from the joined files, and delete the loose tree. Doing
   * this before check_data() means its stat()s resolve through the pack. */
  int _pack_mounted = 0;
  if (asset_pack_open_existing(DATA_ROOT)) {
    _pack_mounted = 1;
    nx_create_asset_skeleton();   /* every boot: runtime writes need these */
    debugPrintf("[pack] mounted existing pack (%zu entries)\n",
                asset_pack_entry_count());
  }
#else
  int _pack_mounted = 0; (void)_pack_mounted;
#endif

  /* Loose-asset warning. Deliberately AFTER the pack mount above: it depends
   * on _pack_mounted, and printing it earlier is what produced a boot log
   * reading "Asset data is NOT repacked" three lines above "mounted existing
   * pack (2958 entries)". One stat, no directory scan. */
  { char mp[768]; struct stat mst;
    snprintf(mp, sizeof mp, "%s/assets/bin/Data/merged0.assets", DATA_ROOT);
    if (stat(mp, &mst) == 0) {
      debugPrintf("[data] repacked assets detected (merged chunks present)\n");
    } else if (!_pack_mounted) {
      /* Only warn when the assets really are loose. This used to print
       * unconditionally, so the last boot log showed "Asset data is NOT
       * repacked" three lines above "mounted existing pack (2958 entries)" --
       * alarming, and wrong. The nxpack achieves the same goal as the PC
       * repack tools, so with a pack mounted there is nothing to warn about. */
      debugPrintf("\n"
        "[data] ****************************************************************\n"
        "[data] * Asset data is NOT packed.                                    *\n"
        "[data] * The first scene load will take a very long time, because the *\n"
        "[data] * game loads thousands of separate resource files and each     *\n"
        "[data] * load costs far more here than it does on Android.            *\n"
        "[data] * Set PHI_ASSET_PACK 1 in config.h (it is on by default) and   *\n"
        "[data] * let the loader build assets.nxpack on the next boot.         *\n"
        "[data] ****************************************************************\n\n");
    } }

  /* Split-asset join: REMOVED for Phigros. Fruit Ninja shipped four files
   * chopped into 1 MiB .splitN parts that Unity reassembles in Java; this
   * game does not, so there is nothing to join and nx_splitjoin.c is not in
   * this tree. (If a future Phigros build ever ships .splitN parts, the file
   * is self-contained and can be lifted back from fruitninja_nx.) */
#if PHI_ASSET_PACK
  /* Build the pack on first boot, now that splits are joined. */
  if (!_pack_mounted) {
    debugPrintf("[pack] no pack yet; building (first boot, may take minutes)\n");
    { uint64_t tot = 0; unsigned nf = 0;
      nx_tree_stats(nx_rp("/assets"), &nf, &tot);
      debugPrintf("[pack] loose tree: %u files, %llu MB\n",
                  nf, (unsigned long long)(tot >> 20));
      struct statvfs vfs;
      if (statvfs(DATA_ROOT, &vfs) == 0) {
        uint64_t freeb = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        debugPrintf("[pack] SD free: %llu MB (need ~%llu MB for the pack)\n",
                    (unsigned long long)(freeb >> 20),
                    (unsigned long long)(tot >> 20));
        if (freeb < tot + (64u << 20)) {
          /* Not fatal: the build is verify-before-commit and writes to .tmp,
           * so running out of room leaves the loose tree intact and the loader
           * carries on unpacked. Say that plainly rather than implying data
           * loss -- and give the actual number, since "free some space" is not
           * actionable when the requirement is nearly 3 GB. */
          debugPrintf("[pack] *** NOT ENOUGH FREE SPACE ***\n"
                      "[pack] have %llu MB, need ~%llu MB (pack and loose tree\n"
                      "[pack] coexist until the build is verified).\n"
                      "[pack] The build will fail safely and the game will run\n"
                      "[pack] from loose files -- slower, but nothing is lost.\n"
                      "[pack] Set PHI_ASSET_PACK 0 to stop retrying every boot.\n",
                      (unsigned long long)(freeb >> 20),
                      (unsigned long long)(tot >> 20));
        }
      } }
    debugPrintf("[pack] build: starting\n");
    if (!asset_pack_build(nx_rp("/assets"), DATA_ROOT)) {
      debugPrintf("[pack] build FAILED: %s (keeping loose files)\n",
                  asset_pack_error());
    } else if (asset_pack_open_existing(DATA_ROOT)) {
      _pack_mounted = 1;
      debugPrintf("[pack] built + mounted pack (%zu entries)\n",
                  asset_pack_entry_count());
      startup_status_update("Removing unpacked asset files");
      debugPrintf("[pack] removing loose assets/ tree\n");
      nx_rmtree(nx_rp("/assets"));
      nx_create_asset_skeleton();   /* runtime writes need somewhere to go */
    }
    startup_status_end();   /* release the progress console */
  }
#endif

  nx_dump_dir(nx_rp("/il2cpp"), "before-boot");
  nx_dump_dir(nx_rp("/il2cpp/Metadata"), "before-boot");
  check_data(_pack_mounted);

  /* load the three modules; libil2cpp resolves its engine calls against libunity
   * module-to-module during relocation. */
  debugPrintf("[boot] loading modules...\n");
  if (load_module(&main_mod,   LIB_MAIN)   < 0) fatal_error("Could not load %s", LIB_MAIN);
  debugPrintf("[boot] loaded libmain   @ virtbase %p\n", (void *)main_mod.load_virtbase);
  if (load_module(&unity_mod,  LIB_UNITY)  < 0) fatal_error("Could not load %s", LIB_UNITY);
  debugPrintf("[boot] loaded libunity  @ virtbase %p\n", (void *)unity_mod.load_virtbase);
  if (load_module(&il2cpp_mod, LIB_IL2CPP) < 0) fatal_error("Could not load %s", LIB_IL2CPP);
  debugPrintf("[boot] loaded libil2cpp @ virtbase %p\n", (void *)il2cpp_mod.load_virtbase);

  /* Exceed7 NativeAudio. Its OpenSL imports (SL_IID_ENGINE / _PLAY / _VOLUME /
   * _ANDROIDSIMPLEBUFFERQUEUE + slCreateEngine) bind against the same
   * opensles.c shim libunity's FMOD uses, so nothing extra is needed here.
   *
   * NOT fatal if absent: the C# playback calls are direct DllImports that will
   * simply fail to resolve, and phigros_jni.c reports NativeAudio as
   * uninitialised. You lose chart audio, not the boot -- which is the more
   * useful failure mode while bringing the port up.
   *
   * DELIBERATELY NOT LOADED: libtapsdkcore.so. Nothing in the core three libs
   * needs it; it exists only to serve the Java TapTap SDK, which is not here.
   * Loading it would drag in an epoll/eventfd event loop with nothing to pump
   * it -- exactly the class of wedge that cost the reference ports weeks. */
  if (load_module(&nativeaudio_mod, LIB_NATIVEAUDIO) < 0) {
    debugPrintf("[boot] libnativeaudioe7.so absent or failed to load.\n"
                "[boot] Chart audio and hitsounds will be SILENT; menus still work.\n");
  } else {
    nativeaudio_loaded = 1;
    debugPrintf("[boot] loaded nativeaudio @ virtbase %p\n",
                (void *)nativeaudio_mod.load_virtbase);
  }



  /* hand the il2cpp exec base to the GC stop-the-world bridge in libc_shim.c so
   * our pthread_kill can ack the Boehm GC's (undeliverable) suspend/restart
   * signals via its semaphore at il2cpp+0x25936c0. */
  g_il2cpp_base = (uintptr_t)il2cpp_mod.load_virtbase;

  /* Firebase native libs are intentionally NOT loaded. The first scene's
   * FirebaseManager only advances when the managed dependency check reports
   * DependencyStatus.Available(0); on a Switch there is no Google Play Services,
   * so the real libs could never report that (they return UnavailableMissing)
   * AND libFirebaseCppApp crashes our loader in its JNI_OnLoad logging path. We
   * instead answer the SDK's native P/Invoke lookups with stubs (firebase_stub.c
   * via dlsym_fake) that make the check resolve to Available. The 4 .so files can
   * be deleted from sdmc:/switch/zookeeper/. Firebase is cosmetic here (RemoteConfig
   * banner/news textures), so stubbing it costs only those images. */
  so_finalize(&main_mod);   so_flush_caches(&main_mod);
  so_finalize(&unity_mod);  so_flush_caches(&unity_mod);
  so_finalize(&il2cpp_mod); so_flush_caches(&il2cpp_mod);
  /* The FOURTH module needs this too. so_finalize() is what re-protects a
   * loaded image's text as executable and so_flush_caches() makes the icache
   * agree; without them the module loads, relocates and resolves perfectly and
   * then faults with an INSTRUCTION abort (esr EC=0x20) the first time anything
   * calls into it.
   *
   * That is precisely what happened when libnativeaudioe7 was added to the
   * load list but not to this list: the port booted, rendered three frames,
   * and died at libnativeaudioe7+0x2cdc the moment the C# side made its first
   * NativeAudio DllImport call -- a crash that looks like a bad plugin but is
   * really a missing finalize. If another module is ever added, add it here. */
  if (nativeaudio_loaded) {
    so_finalize(&nativeaudio_mod); so_flush_caches(&nativeaudio_mod);
  }
  debugPrintf("[boot] modules finalized + flushed: main, unity, il2cpp%s\n",
              nativeaudio_loaded ? ", nativeaudio" : " (nativeaudio absent)");

  /* Patch libunity AFTER finalize/flush -- the same point every other libunity
   * patch below runs at. so_patch_code aliases the target pages via
   * svcMapProcessMemory, but the module's segments must be finalized (mapped
   * with their final RX perms and relocated) first; doing it right after
   * load_module faulted at boot (fbstub94: stock .so on SD, PC in the patch
   * path before finalize). */
  nx_patch_libunity((uintptr_t)unity_mod.load_virtbase);

  /* Force FMOD to use its native OpenSL ES output instead of Unity's Java
   * AudioTrack driver.
   *
   * Mechanism (offsets in this paragraph are the Zookeeper 62f2 reference trace;
   * the PvZ 62f1c1 patch sites are the 0x7xxxxx offsets in the code below).
   * Unity's AudioManager FMOD init calls FMOD::System::setOutput with a
   * requested FMOD_OUTPUTTYPE in w1, derived at +0x6bea84 (mov w1,w21). FMOD's
   * setOutput walks the registered output list (+0xc80e84 loop) and matches the
   * requested type against each output's type field at output+0x78 (copied there
   * from the output description by the registrar +0xc744fc). The type constants,
   * read straight from each getDescriptionEx's desc+0x78:
   *     AudioTrack = 21 (0x15)   <- the default request; needs the JVM run loop
   *     OpenSL ES  = 22 (0x16)   <- callback-driven, self-driving via our shim
   * Default request is 21 (logged previously as "requested output: 21"), so the
   * Java AudioTrack output is selected and, with no JVM consumer, stays silent.
   *
   * fbstub63: rewrite the requested type at the setOutput call site from
   * "mov w1,w21" to "movz w1,#22", so Unity asks FMOD for OPENSL. FMOD finds the
   * registered OpenSL output (type 22), inits it -> dlopen(libOpenSLES.so) ->
   * slCreateEngine (our opensles.c shim) -> the engine drives its own callback
   * buffer queue. No Java handshake, correct lifecycle. The registration path is
   * left untouched (both AudioTrack and OpenSL register normally with their real
   * type fields). 0x2A1503E1 (mov w1,w21) -> 0x528002C1 (movz w1,#0x16). */
  {
    uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    uint32_t req_opensl = 0x528002C1u; /* movz w1, #22 (FMOD_OUTPUTTYPE OPENSL) */
    /* PvZ 62f1c1 offset 0x757bb8 (signature-matched from zk 0x6bea84). Verify the
     * expected `mov w1,w21` before writing -- belt-and-suspenders, cannot corrupt. */
    /* NOT DERIVED for Phigros: AudioManager::InitFMOD did not fingerprint into
     * this build, so PHI_FMOD_OUTPUT_SITE is 0 and the gate is off. Say that,
     * rather than reading libunity+0 and reporting the ELF magic as a version
     * mismatch -- which is what the inherited code did, and it reads like a
     * real problem when it is not one.
     * This costs little here: Phigros drives chart audio and hitsounds through
     * Exceed7 NativeAudio over OpenSL directly. FMOD is menus only. */
    if (!PHI_HAVE_FMOD_OPENSL || !PHI_FMOD_OUTPUT_SITE) {
      debugPrintf("[fmod] force-OpenSL not derived for this build -- skipped. "
                  "Menu/UI audio may be silent; chart audio is NativeAudio and "
                  "is unaffected (see PORTING sec 5).\n");
    } else if (*(volatile uint32_t *)(ub + PHI_FMOD_OUTPUT_SITE) == 0x2A1503E1u) {
      so_patch_code((void *)(ub + PHI_FMOD_OUTPUT_SITE), &req_opensl, sizeof req_opensl);
      debugPrintf("[fmod] output forced to OpenSL(22) @libunity+PHI_FMOD_OUTPUT_SITE\n");
    } else {
      debugPrintf("[fmod] SKIP force-OpenSL: +fmod = 0x%08x, not `mov w1,w21` -- "
                  "offset is stale, re-derive it\n",
                  *(volatile uint32_t *)(ub + PHI_FMOD_OUTPUT_SITE));
    }
    /* Frame-pacing (Swappy) force-disable. This build registers Swappy (9 JNI entrypoints);
     * its init brings up a Choreographer/vsync-driven thread pool that never completes
     * on Switch -- there is no Android Choreographer to deliver frame callbacks -- so
     * engine-init parks in a pthread_join at frame 0 (verified on hardware: UnityMain
     * state=join for 18s, workers hard-parked in Swappy's 0xd6xxxx wait). libunity+
     * 0x652354 is the cached "is frame-pacing enabled?" getter: 13 call sites, each
     * `bl 0x652354 ; tbz w0,#0,<skip>`. Forcing it to return 0 makes every site take
     * the disabled path -> plain eglSwapBuffers, no pacing threads, no join. This is
     * how the Zookeeper base already boots (it never enables Swappy). Verify-first:
     * patch only if the prologue is the expected `stp x30,x19,[sp,#-0x10]!`. */
    if (*(volatile uint32_t *)(ub + PHI_PACING_GETTER) == 0xA9BF4FFEu) {
      uint32_t off_pacing[2] = { 0x52800000u /* mov w0,#0 */, 0xD65F03C0u /* ret */ };
      so_patch_code((void *)(ub + PHI_PACING_GETTER), off_pacing, sizeof off_pacing);
      debugPrintf("[pace] frame-pacing (Swappy) force-disabled @libunity+PHI_PACING_GETTER\n");
    } else {
      debugPrintf("[pace] SKIP Swappy-disable: +pacing = 0x%08x, not `stp x30,x19` -- "
                  "libunity differs (see PORTING)\n", *(volatile uint32_t *)(ub + PHI_PACING_GETTER));
    }

    /* fbstub66: neutralise FMOD's OpenSL buffer-geometry validation.
     *
     * After slCreateEngine succeeds, FMOD's OpenSL init (+0xce67a0 -> continuation
     * +0xce6840) validates the output period against the DSP mixer buffer at
     * +0xce68d4..+0xce6924. It reads {sampleRate, framesPerBuffer} from the
     * AudioManager getProperty values (our jni_fake.c: 48000 / 64) and returns
     * FMOD error 60 ("Error initializing output device") if:
     *     sampleRate == 0, OR framesPerBuffer == 0, OR
     *     framesPerBuffer > (dspNumBuffers-1)*dspBufferLength  even after one halving.
     * dspNumBuffers (w20) / dspBufferLength (w21) come from the game's BAKED
     * AudioSettings (AudioSettings::GetDSPBufferSize, +0x646888) and pass straight
     * through the init wrapper +0xce63a8 unchanged. This title's baked buffer is
     * degenerate enough that even a 64-frame period fails the bound -- consistent
     * with dspNumBuffers == 1, which makes the bound (1-1)*len = 0 so NO positive
     * period can ever satisfy it. Reporting a smaller framesPerBuffer alone cannot
     * fix that case, so we also force the final bound check to pass.
     *
     * Patch the terminal compare-branch at +0xce6920 from "b.ls 0xce6930"
     * (0x54000089) to an unconditional "b 0xce6930" (0x14000004): the success path
     * at +0xce6930 then always runs and builds the audio player from the sane
     * sampleRate/channels. The buffer count it derives, N = (w20*w21)/period
     * (+0xce697c), stays >= 1 because we report a small 64-frame period. The two
     * zero-guards above (+0xce68ec/+0xce68fc) are left intact and pass (48000/64
     * are both non-zero). */
    uint32_t b_uncond = 0x14000004u; /* b +0x10 (was b.ls, 0x54000089) -- same local target */
    /* PvZ 62f1c1 offset 0xe11404 (signature-matched from zk 0xce6920). Verify a real b.ls. */
    if (PHI_HAVE_FMOD_BUFFER_BYPASS &&
        *(volatile uint32_t *)(ub + PHI_FMOD_BUFFER_SITE) == 0x54000089u) {
      so_patch_code((void *)(ub + PHI_FMOD_BUFFER_SITE), &b_uncond, sizeof b_uncond);
      debugPrintf("[fmod] OpenSL buffer-geometry check bypassed @libunity+0xe11404\n");
    } else {
      if (!PHI_HAVE_FMOD_BUFFER_BYPASS || !PHI_FMOD_BUFFER_SITE)
        debugPrintf("[fmod] buffer-geometry bypass not derived for this build -- skipped\n");
      else
      debugPrintf("[fmod] SKIP buffer-geometry bypass: +0xe11404 = 0x%08x, not b.ls -- "
                  "libunity differs from expected 62f1c1 (see PORTING sec 3)\n",
                  *(volatile uint32_t *)(ub + PHI_FMOD_BUFFER_SITE));
    }
  }

  /* Definitive probe: read F's canary b.ne at libunity 0xc44c34 in the EXECUTED
   * (removed with the probe itself). */
  /* PvZ debugging probe at libunity+0xc44c34 REMOVED: that address held a
   * specific canary branch in PvZ's 62f1c1 build. Reading it here prints a
   * meaningless word from an unrelated function and invites false
   * conclusions from debug.log. */

  /* The main thread runs init_array + the engine lifecycle; give it its own
   * stable bionic TLS for the stack-protector guard (tpidr_el0+0x28). */
  static uint8_t main_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(main_tls);

#if PHI_DATA_REGION_PROBE
  /* See nx_patch_phigros.h section 1b. Print the candidate stored-granularity
   * words before and after the constructors, so debug.log shows whether any of
   * them is the 256MB the allocator actually used. */
  { uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    for (int i = 0; i < PHI_DATA_REGION_N; i++) {
      uint32_t off = PHI_DATA_REGION_OFFS[i];
      debugPrintf("[rgn] pre-ctor  libunity+0x%07x = 0x%016llx (64) / 0x%08x (32)\n",
                  off, (unsigned long long)*(volatile uint64_t *)(ub + off),
                  *(volatile uint32_t *)(ub + off));
    }
#if PHI_PATCH_DATA_REGION
    for (int i = 0; i < PHI_DATA_REGION_N; i++) {
      uint32_t off = PHI_DATA_REGION_OFFS[i];
      volatile uint64_t *q = (volatile uint64_t *)(ub + off);
      if (*q == 0x10000000ull) {          /* verify-first, as everywhere else */
        uint64_t nv = 0x04000000ull;
        so_patch_code((void *)q, &nv, sizeof nv);
        debugPrintf("[rgn] patched libunity+0x%07x 256MB -> 64MB\n", off);
      } else {
        debugPrintf("[rgn] SKIP libunity+0x%07x: is 0x%016llx, not 256MB\n",
                    off, (unsigned long long)*q);
      }
    }
#endif
  }
#endif
  debugPrintf("[boot] running init arrays...\n");
  so_execute_init_array(&main_mod);
  so_execute_init_array(&unity_mod);
  so_execute_init_array(&il2cpp_mod);
  so_free_temp(&main_mod); so_free_temp(&unity_mod); so_free_temp(&il2cpp_mod);
#if PHI_DATA_REGION_PROBE
  { uintptr_t ub = (uintptr_t)unity_mod.load_virtbase;
    for (int i = 0; i < PHI_DATA_REGION_N; i++) {
      uint32_t off = PHI_DATA_REGION_OFFS[i];
      debugPrintf("[rgn] post-ctor libunity+0x%07x = 0x%016llx (64) / 0x%08x (32)\n",
                  off, (unsigned long long)*(volatile uint64_t *)(ub + off),
                  *(volatile uint32_t *)(ub + off));
    }
  }
#endif
  debugPrintf("[boot] init arrays done\n");

  /* fake JNI + our environment, then HID */
  jni_init();
  unity_environment_init(DATA_ROOT);
  android_native_update_mode();
  android_native_input_init();
  debugPrintf("[boot] jni + env + hid ready\n");

  /* resolve UnityPlayer natives (load_virtbase + recovered offsets) */
  Unity_initJni                  = (fn_initJni) UNITY_RESOLVE(unity_mod, OFF_initJni);
  Unity_nativeRecreateGfxState   = (fn_gfxstate)UNITY_RESOLVE(unity_mod, OFF_nativeRecreateGfxState);
  Unity_nativeSendSurfaceChanged = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeSendSurfaceChangedEvent);
  Unity_nativeRender             = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeRender);
  Unity_nativeInjectEvent        = (fn_inject)  UNITY_RESOLVE(unity_mod, OFF_nativeInjectEvent);
  Unity_nativeResume             = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeResume);
  Unity_nativeFocusChanged       = (fn_vz)      UNITY_RESOLVE(unity_mod, OFF_nativeFocusChanged);
  Unity_nativeDone               = (fn_z)       UNITY_RESOLVE(unity_mod, OFF_nativeDone);
  Unity_nativeApplicationUnload  = (fn_v)       UNITY_RESOLVE(unity_mod, OFF_nativeApplicationUnload);
  debugPrintf("[boot] entry points resolved (initJni=%p render=%p)\n",
              (void *)Unity_initJni, (void *)Unity_nativeRender);

  /* re-assert the guard right before handing control to the engine, so no
   * intervening libnx/jni setup left tpidr in an unexpected state */
  install_bionic_tls(main_tls);

  /* drive the lifecycle the Java UnityPlayer would */
  extern void *fake_env, *fake_unityplayer_thiz, *fake_context_obj, *fake_surface_obj;
  extern void *fake_vm;

  /* Call libunity's real JNI_OnLoad(fake_vm) FIRST. It runs jni::Initialize(),
   * which caches the JavaVM into libunity's internal JNI manager; without this
   * ScopedJNI/LocalScope inside initJni get a NULL JNIEnv and crash. It also
   * AttachCurrentThread()s and RegisterNatives() for each subsystem (our fake
   * env handles FindClass/RegisterNatives as safe no-ops). */
  {
    typedef int (*fn_jnionload)(void *vm, void *reserved);
    fn_jnionload Unity_JNI_OnLoad = (fn_jnionload)UNITY_RESOLVE(unity_mod, OFF_JNI_OnLoad);
    debugPrintf("[boot] calling JNI_OnLoad(fake_vm)...\n");
    int jver = Unity_JNI_OnLoad(fake_vm, NULL);
    debugPrintf("[boot] JNI_OnLoad returned 0x%x\n", jver);
  }

  /* Register the JavaVM with the il2cpp runtime. il2cpp caches the VM in a
   * global it later checks; without it, il2cpp logs "Java VM not initialized"
   * and every managed AndroidJNI / AndroidJavaObject call (the Twitter SDK +
   * the SWIG-wrapped AppUtil module the first scene initializes) fails, hanging
   * scene load.
   *
   * We do NOT call libil2cpp's JNI_OnLoad: its first action is a log via
   * __android_log_print, whose GOT slot in libil2cpp is mis-bound (resolves to
   * a heap address -> Instruction Abort). Its only *essential* effects are two
   * global stores (verified by disassembling THIS PvZ 62f1c1 libil2cpp's
   * JNI_OnLoad @ 0x188e168): cache the VM at il2cpp+0x3c09c18, and store the JNI
   * handler fn-ptr (il2cpp+0x188e1ac, which the reg-fn @0x18ea05c writes) at
   * il2cpp+0x3c0abe8. Both land in PvZ's RW segment. Replicate the two stores. */
  {
    uintptr_t b = (uintptr_t)il2cpp_mod.load_virtbase;
#if PHI_HAVE_IL2CPP_VM
    /* Phigros values, derived by disassembling THIS libil2cpp's
     * JNI_OnLoad @0x1588e1c -- see nx_patch_phigros.h. Both targets are
     * in libil2cpp's .bss. These are NOT PvZ's addresses. */
    *(void **)(b + PHI_IL2CPP_VM_GLOBAL)    = fake_vm;
    *(void **)(b + PHI_IL2CPP_HANDLER_SLOT) = (void *)(b + PHI_IL2CPP_HANDLER_FN);
#endif
    debugPrintf("[boot] il2cpp JavaVM global set (vm=%p)\n", fake_vm);
#if PHI_HAVE_ISINST_GUARD
    /* Guard il2cpp's assignability check against a NULL klass (round 119).
     * DialogueConfig.m_dialoguePieces holds an Il2CppClass* instead of an array
     * and the check faults reading klass+0x135.
     *
     * Applied HERE, not at "[boot] loaded libil2cpp": at that point the module's
     * text is not yet mapped, and merely READING the site to verify it faulted
     * (round 120 -- far == il2cpp_base + 0x159dfcc, exactly this address). This
     * is the same place the JavaVM globals and Time hooks are written, which is
     * proven safe. */
    { volatile uint32_t *site = (uint32_t *)(b + PHI_IL2CPP_ISINST_AND);
      if (!nx_addr_readable((uintptr_t)site)) {
        debugPrintf("[boot] SKIP IsInst guard: libil2cpp+0x%x not mapped yet\n",
                    (unsigned)PHI_IL2CPP_ISINST_AND);
      } else if (site[0] == PHI_IL2CPP_ISINST_AND_OLD) {
        uint32_t guard = PHI_IL2CPP_ISINST_GUARD;
        so_patch_code((void *)site, &guard, sizeof guard);
        so_flush_caches(&il2cpp_mod);
        debugPrintf("[boot] il2cpp IsInst null-klass guard installed @libil2cpp+0x%x\n",
                    (unsigned)PHI_IL2CPP_ISINST_AND);
      } else if (site[0] == PHI_IL2CPP_ISINST_GUARD) {
        debugPrintf("[boot] il2cpp IsInst guard already installed\n");
      } else {
        debugPrintf("[boot] SKIP IsInst guard @libil2cpp+0x%x: 0x%08x != 0x%08x\n",
                    (unsigned)PHI_IL2CPP_ISINST_AND, site[0],
                    (unsigned)PHI_IL2CPP_ISINST_AND_OLD);
      }
    }
#endif

#if PHI_HAVE_LIVENESS_GUARD
    /* Installed HERE for the same reason as the r119 guard above: this is the
     * point where libil2cpp's text is known mapped (round 120).
     *
     * NOT hook_arm64(): it stores straight into module text, which so_finalize
     * mapped RX. Build its 16-byte thunk and write it through so_patch_code's
     * writable alias instead. */
    { volatile uint32_t *site = (uint32_t *)(b + PHI_IL2CPP_LIVENESS_ADD);
      if (!nx_addr_readable((uintptr_t)site)) {
        debugPrintf("[boot] SKIP liveness guard: libil2cpp+0x%x not mapped yet\n",
                    (unsigned)PHI_IL2CPP_LIVENESS_ADD);
      } else if (site[0] == 0x58000051u) {
        debugPrintf("[boot] liveness guard already installed\n");
      } else if (site[0] != PHI_LIVENESS_PROLOGUE[0] || site[1] != PHI_LIVENESS_PROLOGUE[1] ||
                 site[2] != PHI_LIVENESS_PROLOGUE[2] || site[3] != PHI_LIVENESS_PROLOGUE[3]) {
        /* Loud, and patch NOTHING -- a moved add_process_object must not be
         * half-overwritten. Report all four words so the site can be re-derived
         * from the log alone. */
        debugPrintf("[boot] SKIP liveness guard @libil2cpp+0x%x: prologue is "
                    "%08x %08x %08x %08x, expected %08x %08x %08x %08x\n",
                    (unsigned)PHI_IL2CPP_LIVENESS_ADD,
                    site[0], site[1], site[2], site[3],
                    PHI_LIVENESS_PROLOGUE[0], PHI_LIVENESS_PROLOGUE[1],
                    PHI_LIVENESS_PROLOGUE[2], PHI_LIVENESS_PROLOGUE[3]);
      } else {
        uint32_t thunk[4];
        const uint64_t dst = (uint64_t)(uintptr_t)&fn_liveness_guard;
        g_fn_liveness_body = (uint64_t)(b + PHI_IL2CPP_LIVENESS_BODY);
        thunk[0] = 0x58000051u;   /* ldr x17, #8  */
        thunk[1] = 0xd61f0220u;   /* br  x17      */
        memcpy(&thunk[2], &dst, sizeof dst);
        if (so_patch_code((void *)site, thunk, sizeof thunk) == 0) {
          so_flush_caches(&il2cpp_mod);
          debugPrintf("[boot] il2cpp liveness typeHierarchy guard installed "
                      "@libil2cpp+0x%x -> %p (body 0x%llx)\n",
                      (unsigned)PHI_IL2CPP_LIVENESS_ADD, (void *)(uintptr_t)dst,
                      (unsigned long long)g_fn_liveness_body);
        } else {
          g_fn_liveness_body = 0;
          debugPrintf("[boot] liveness guard: so_patch_code FAILED, not installed\n");
        }
      }
    }
#endif

  /* Round 146, from the ACPC port. il2cpp_gc_set_mode(1) is
   * IL2CPP_GC_MODE_ENABLED -- disassembled at libil2cpp+0x1593660: mode 1 does
   * `if (GC_is_disabled()) GC_enable()` then
   * `set_disable_automatic_collection(false)`. It is a defensive re-assert, not
   * a change: our logs already show collections running. Cheap insurance that
   * nothing has left automatic collection switched off, via a real exported
   * symbol so there is no offset to go stale. */
  { typedef void (*fn_set_mode)(int);
    fn_set_mode set_mode = (fn_set_mode)so_find_addr_rx(&il2cpp_mod, "il2cpp_gc_set_mode");
    if (set_mode) { set_mode(1); debugPrintf("[gc] il2cpp_gc_set_mode(ENABLED)\n"); } }

#if PHI_GC_DISABLE
    /* Round 141 -- the decisive experiment, not a fix.
     *
     * Every guard since r119 has been placed where the bad value is READ. If the
     * corruption is premature reclamation -- the collector freeing something
     * still reachable, and the memory being handed out again -- then it is
     * created in the sweep and no amount of validation at the read side touches
     * it. Turning collection off answers that in one session:
     *
     *   corruption GONE  -> the collector is freeing live objects. The cause is
     *                       almost certainly that Boehm never gets a snapshot of
     *                       paused threads' registers and stack pointers: on
     *                       Android its SIG_SUSPEND handler runs ON each mutator
     *                       and spills them, and Horizon has no POSIX signals, so
     *                       we pause with svcSetThreadActivity and that handler
     *                       never runs.
     *   corruption STAYS -> the GC is exonerated after 20 rounds of suspicion,
     *                       and the hunt moves to the wild write we already have
     *                       direct evidence of (r134 boot 1: `str x1,[x0]` with
     *                       x0 = UTF-16 text).
     *
     * il2cpp_gc_disable is a real exported symbol, so this needs no derived
     * offset and cannot go stale. Memory grows unbounded while it is on -- the
     * pool is 1 GB and the game's live set is far smaller, so a few minutes of
     * play is fine, but do not ship with this set. */
    { uintptr_t f = so_find_addr_rx(&il2cpp_mod, "il2cpp_gc_disable");
      if (f) { ((void (*)(void))f)();
        debugPrintf("[boot] *** GC DISABLED (PHI_GC_DISABLE): no collection will "
                    "run this session. Diagnostic only -- memory grows. ***\n"); }
      else debugPrintf("[boot] PHI_GC_DISABLE set but il2cpp_gc_disable not found\n"); }
#endif


#if PHI_HAVE_TIME_HOOKS
    nx_boot_il2cpp_hacks();   /* Time.get_* hooks -- REQUIRES FN-derived offsets */
#else
    /* DISABLED. nx_boot_il2cpp_hacks() patches CODE at six hardcoded
     * libil2cpp offsets that belong to PvZ Fusion, not this game. Running
     * it here would rewrite six unrelated functions inside Phigros's
     * libil2cpp. Re-derive the six UnityEngine.Time getters against this
     * game's global-metadata.dat before enabling (PORTING sec 6). */
#endif
  }

  /* (Firebase native libs are not loaded; their JNI_OnLoad is neither needed nor
   * safe to call -- see the boot-time note above. The managed SDK's native calls
   * are answered by firebase_stub.c through dlsym_fake.) */

  debugPrintf("[boot] calling initJni...\n");
  Unity_initJni(fake_env, fake_unityplayer_thiz, fake_context_obj);
  debugPrintf("[boot] initJni returned; nativeRecreateGfxState...\n");
  Unity_nativeRecreateGfxState(fake_env, fake_unityplayer_thiz, 0, fake_surface_obj);
  nx_gpu_probe();   /* hardware-vs-software ASTC, one line in debug.log */
  debugPrintf("[boot] gfx state created; sendSurfaceChanged...\n");
  Unity_nativeSendSurfaceChanged(fake_env, fake_unityplayer_thiz);
  debugPrintf("[boot] surface change sent; resuming + focusing player loop\n");

  /* CRITICAL: on Android the Unity player loop only advances Update/coroutines/
   * animation when the app is RESUMED and FOCUSED. The Java UnityPlayer drives
   * this from onResume()/onWindowFocusChanged(). We had been calling only
   * initJni + gfx + render, so the engine stayed paused: it loaded the boot
   * scene and ran Awake/Start once (hence Firebase init), then rendered a frozen
   * frame forever without ticking a single Update or coroutine -- which is why
   * StartInitializer.InitUpdate was never called. Issue the resume + focus
   * transitions the lifecycle normally would before the render loop. */
  Unity_nativeResume(fake_env, fake_unityplayer_thiz);
  Unity_nativeFocusChanged(fake_env, fake_unityplayer_thiz, 1 /* hasFocus */);
  debugPrintf("[boot] resumed + focus=true; entering render loop\n");
#if PHI_HAVE_TIME_FIX
#if PHI_HAVE_TIME_FIX
  nx_install_time_fix();   /* hook Update + clock thread before the first nativeRender */
#else
  /* DISABLED: nx_install_time_fix() uses PvZ libunity offsets (0x4e2198 /
   * 0x4e21bc / 0x4e2794). Enable only after re-deriving all three. */
#endif
#endif

  diag_thread_register(NULL, 0);
  diag_set_name(NULL, "NX_UIMain");   // the thread that drives nativeRender
  /* Watchdog re-enabled (fbstub88) with the snapshot-ordering fix: stacks are
   * now walked while the target thread is PAUSED (the fbstub86 self-crash came
   * from resuming first and walking a live stack). 6s thresholds. Its job now:
   * catch the first-present hang and dump the thread blocked in eglSwapBuffers. */
  diag_watchdog_start();

  int frame = 0;
  while (appletMainLoop() && !jni_quit_requested) {
    diag_frame(frame);   // heartbeat: lets the watchdog see progress (or its absence)
    nx_time_tick();      // advance our managed-Time clock once per frame
    /* fbstub42: the engine clock is now fixed at its true source by the
     * TimeManager::Update entry hook (installed at boot, see nx_install_time_fix):
     * Update is re-driven each frame with newTime = GetTimeSinceStartup(), so all
     * deltaTime variants advance and the PreloadManager can integrate. No per-frame
     * field poking needed here. */
    android_native_update_mode();
    android_native_feed_hid((uint8_t (*)(void*,void*,void*,int))Unity_nativeInjectEvent,
                            fake_env, fake_unityplayer_thiz);
    if (!Unity_nativeRender(fake_env, fake_unityplayer_thiz)) break;
    if (frame == 0) {
      /* fbstub42: install the native engine-clock fix first thing. Drives
       * TimeManager::Update with a live newTime so deltaTime / m_Time advance for
       * native readers (PreloadManager), unblocking async scene loads. */
      /* nx_install_time_fix() moved before the render loop (installs too late here:
       * PvZ's first nativeRender (their symptom) blocks on the scene load and never returns). */
      /* Time.get_* hooks are installed at boot (nx_boot_il2cpp_hacks, right after
       * the JavaVM global is set). This call is a self-skipping backstop only. */
#if PHI_HAVE_TIME_HOOKS
      nx_boot_il2cpp_hacks();
#endif
    }
    /* Deferred PlayerPrefs write-back. Costs a tick compare per frame and turns
     * a burst of apply() calls into one file write instead of one each. */
    unity_prefs_tick();

    if (frame < 5 || (frame % 120) == 0) {
      /* Audio counters alongside the frame counter. Printed HERE, on the main
       * thread, because the audio callback must never touch the log -- file I/O
       * from an audio thread is what stopped CloverPit's console booting. The
       * numbers separate the two failure shapes without another guess:
       *   consumed >> enq  -> blocks replayed (aliasing)
       *   dry / short high -> the queue runs empty (underrun) */
      char ast[512];   /* 96 truncated the rate list and hid a player leak */
      phi_audio_stats(ast, sizeof ast);
      unsigned iok = 0, irej = 0, icar = 0, isat = 0;
      android_native_input_stats(&iok, &irej, &icar, &isat);
      debugPrintf("[boot] frame %d rendered  [input] inj=%u rej=%u carry=%u sat=%u  "
                  "[audio] %s\n", frame, iok, irej, icar, isat, ast);
    }
#if PHI_HAVE_FINISH_PROBE
    if (frame == 90 || frame == 300 || frame == 600 || frame == 1200)
      nx_probe_finish((uintptr_t)il2cpp_mod.load_virtbase);
#endif
    frame++;
  }

  /* Commit any sensitivity change made inside the last 3 seconds -- the pointer
   * normally debounces its own save, so quitting quickly after a D-pad tweak
   * would otherwise lose it. */
  android_native_input_shutdown();

  /* Land any deferred apply() BEFORE the engine tears down. A deferred write
   * that is never flushed is a lost save, which is a far worse bug than the
   * slow writes the deferral fixes. */
  unity_prefs_sync();

  Unity_nativeApplicationUnload(fake_env, fake_unityplayer_thiz);
  Unity_nativeDone(fake_env, fake_unityplayer_thiz);

  opensles_shutdown();
  SDL_Quit();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);
  return 0;
}
