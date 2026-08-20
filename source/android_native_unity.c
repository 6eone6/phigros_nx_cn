/* android_native_unity.c -- the 27 NDK symbols libunity.so imports, for the
 * ZOOKEEPER DX Switch port. Unity is NOT a NativeActivity, so unlike cr3_nx's
 * android_native.c there is no ANativeActivity glue / android_main / AInputQueue
 * here: the engine is driven by the JNI-registered natives (see main.c). We only
 * provide the raw NDK functions libunity calls directly:
 *
 *   ANativeWindow_acquire/_release/_fromSurface/_setBuffersGeometry/
 *                _getWidth/_getHeight/_getFormat      -> libnx NWindow
 *   ALooper_prepare/_acquire/_release/_pollOnce/_wake/_forThread
 *                                                     -> condvar wait/wake
 *   ASensorManager_ , ASensorEventQueue_ , ASensor_   -> "no sensors"
 *
 * IMPORTANT context-ownership note: the engine creates its OWN EGL context from
 * the ANativeWindow (cr3_nx's main.c creates none). The host must NOT create an
 * SDL_GL / EGL context. Use SDL for audio + HID only. Delete the
 * SDL_GL_SetAttribute/SDL_GL_CreateContext/SDL_GL_SwapWindow calls from the
 * earlier main_skeleton.c; the engine calls eglSwapBuffers itself.
 *
 * Needs devkitA64 + libnx (switch.h) + switch-mesa. Not host-compilable.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <switch.h>
#include <GLES3/gl3.h> /* docked cursor overlay */
#include "util.h"   /* debugPrintf */
#include "config.h" /* screen_width / screen_height */

#ifndef AWINDOW_FORMAT_RGBA_8888
#define AWINDOW_FORMAT_RGBA_8888 1
#endif

/* opaque NDK types -> concrete libnx instances */
typedef struct ANativeWindow ANativeWindow;     /* == NWindow* at runtime */
typedef struct ALooper       ALooper;

/* ==========================================================================
 * dock-aware screen state (also read by unity_jni.c's Display getters)
 * ========================================================================== */
static u32 g_w = 1280, g_h = 720;   /* PvZ Fusion: LANDSCAPE, matches Switch panel (was Zookeeper TATE 720x1280) */

void android_native_update_mode(void){
  /* Latched: read the mode ONCE at launch and hold it for the session. The
   * render size feeds the graphics surface and the engine builds its render
   * target against it, so it cannot change underneath either of them --
   * which is also why config.txt says to dock BEFORE launching. */
  static int latched = 0;
  if (latched) return;
  latched = 1;

  const int docked = (appletGetOperationMode() == AppletOperationMode_Console);
  int h = docked ? config.docked_res : config.handheld_res;
  if (h != 720 && h != 1080) h = docked ? 1080 : 720;   /* belt and braces */
  if (h == 1080) { g_w = 1920; g_h = 1080; }
  else           { g_w = 1280; g_h = 720;  }
  debugPrintf("[boot] render %ux%u (%s, config %d)\n",
              (unsigned)g_w, (unsigned)g_h, docked ? "docked" : "handheld", h);
  /* Landscape throughout: keep the engine-reported surface size equal to the
   * real window so the render target matches what we present (no mismatch). */
  screen_width = (int)g_w; screen_height = (int)g_h;
}
u32 android_native_width(void)  { return g_w; }
u32 android_native_height(void) { return g_h; }

/* ==========================================================================
 * ANativeWindow  ->  libnx NWindow
 * ========================================================================== */
/* fbstub45: pin the displayed region to exactly the dimensions Unity renders
 * into. nwindowSetDimensions may allocate a width-aligned (e.g. 720 -> 768)
 * swapchain buffer; without a matching crop the compositor can scan the extra
 * uninitialized columns, which shows up as the image being "cut off" / garbage
 * on the right edge. Cropping to (0,0,bw,bh) guarantees only the rendered
 * content is presented (stretched to the panel, no cutoff). */
/* PvZ Fusion is landscape, always: the game ships no portrait mode, and the
 * Switch panel is landscape too, so the render maps straight onto it. The
 * inherited TATE path -- a compositor rotation picked by config.portrait, from
 * the VLN reference port -- is gone along with the option that drove it.
 * Nothing rotates anywhere in this port now, so clear the buffer transform
 * explicitly rather than inheriting whatever a previous producer left set. */
static void nx_window_set_geom(NWindow *w, u32 bw, u32 bh) {
  Result rc = nwindowSetDimensions(w, bw, bh);
  nwindowSetCrop(w, 0, 0, bw, bh);
  nwindowSetTransform(w, 0);                 /* landscape: no rotation */
  u32 aw = 0, ah = 0;
  nwindowGetDimensions(w, &aw, &ah);
  debugPrintf("[gfx] window geom: requested %ux%u (rc=0x%x), nwindow reports %ux%u, crop 0,0,%u,%u (landscape, no rotation)\n",
              bw, bh, rc, aw, ah, bw, bh);
}

ANativeWindow *android_native_window(void){
  NWindow *w = nwindowGetDefault();
  nx_window_set_geom(w, g_w, g_h);
  return (ANativeWindow *)w;
}
void     ANativeWindow_acquire(ANativeWindow *w){ (void)w; }                 /* singleton: refcount no-op */
void     ANativeWindow_release(ANativeWindow *w){ (void)w; }
ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface){
  (void)env; (void)surface; return android_native_window();               /* one surface == our window */
}
int32_t  ANativeWindow_getWidth (ANativeWindow *w){ (void)w; return (int32_t)g_w; }
int32_t  ANativeWindow_getHeight(ANativeWindow *w){ (void)w; return (int32_t)g_h; }
int32_t  ANativeWindow_getFormat(ANativeWindow *w){ (void)w; return AWINDOW_FORMAT_RGBA_8888; }
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height, int32_t format){
  (void)format;
  /* The NX window is a FIXED-SIZE display. Resizing the real window to a
   * non-native size (e.g. the game's saved 640x1137 low-res, applied EARLY at
   * startup when Unity knows it from PlayerPrefs) makes mesa build a 640x1137
   * swapchain whose buffers the NX display path never consumes -> the first
   * eglSwapBuffers blocks forever (the boot-2 hang). Android devices without
   * hardware resolution scaling behave exactly like this fix: the resize
   * "succeeds" (returns 0) but readback (getWidth/getHeight, eglQuerySurface)
   * still shows the native size, which is precisely how Unity detects
   * "Hardware resolution scaling not supported" and falls back to its software
   * blit -- the same path that already works when SetResolution happens late
   * at frame 2. So: accept only the native geometry; report success for the
   * rest so Unity's own fallback engages. */
  if (width > 0 && height > 0) {
    if ((u32)width != g_w || (u32)height != g_h) {
      debugPrintf("[gfx] setBuffersGeometry %dx%d REJECTED (fixed-size window stays %ux%u; engine will blit-scale)\n",
                  width, height, g_w, g_h);
      return 0;
    }
    nx_window_set_geom((NWindow *)w, (u32)width, (u32)height);
  }
  return 0;
}

/* ==========================================================================
 * ALooper -- Unity uses it as a per-thread wait/wake primitive (not real fd
 * polling), so a condvar-backed looper is sufficient. If the engine turns out
 * to register real fds, port cr3_nx's fake-fd PollItem layer in here.
 * ========================================================================== */
#define ALOOPER_POLL_WAKE     (-1)
#define ALOOPER_POLL_TIMEOUT  (-3)
#define MAX_LOOPERS 16

struct ALooper { Mutex m; CondVar cv; int signaled; int refs; u32 owner; int used; };
static struct ALooper g_loopers[MAX_LOOPERS];
static Mutex g_loopers_lock;
static int   g_loopers_init = 0;

static void loopers_once(void){ if(!g_loopers_init){ mutexInit(&g_loopers_lock); g_loopers_init=1; } }

static struct ALooper *looper_for(u32 tid, int create){
  loopers_once();
  mutexLock(&g_loopers_lock);
  for (int i=0;i<MAX_LOOPERS;i++) if (g_loopers[i].used && g_loopers[i].owner==tid){
    struct ALooper *l=&g_loopers[i]; mutexUnlock(&g_loopers_lock); return l; }
  if (create) for (int i=0;i<MAX_LOOPERS;i++) if (!g_loopers[i].used){
    struct ALooper *l=&g_loopers[i];
    l->used=1; l->owner=tid; l->signaled=0; l->refs=1;
    mutexInit(&l->m); condvarInit(&l->cv);
    mutexUnlock(&g_loopers_lock); return l; }
  mutexUnlock(&g_loopers_lock);
  return NULL;
}
static u32 cur_tid(void){ return (u32)(uintptr_t)threadGetCurHandle(); }

ALooper *ALooper_prepare(int opts){ (void)opts; return (ALooper *)looper_for(cur_tid(), 1); }
ALooper *ALooper_forThread(void){  return (ALooper *)looper_for(cur_tid(), 0); }
void     ALooper_acquire(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); L->refs++; mutexUnlock(&L->m);} }
void     ALooper_release(ALooper *l){ struct ALooper *L=(void*)l; if(L){ mutexLock(&L->m); if(--L->refs<=0) L->used=0; mutexUnlock(&L->m);} }

void ALooper_wake(ALooper *l){
  struct ALooper *L=(void*)l; if(!L) return;
  mutexLock(&L->m); L->signaled=1; condvarWakeAll(&L->cv); mutexUnlock(&L->m);
}
int ALooper_pollOnce(int timeoutMillis, int *outFd, int *outEvents, void **outData){
  struct ALooper *L = (void*)looper_for(cur_tid(), 1);
  if (outFd) *outFd=0;
  if (outEvents) *outEvents=0;
  if (outData) *outData=NULL;
  mutexLock(&L->m);
  if (!L->signaled){
    if (timeoutMillis==0){ mutexUnlock(&L->m); return ALOOPER_POLL_TIMEOUT; }
    if (timeoutMillis<0)  condvarWait(&L->cv,&L->m);
    else condvarWaitTimeout(&L->cv,&L->m,(u64)timeoutMillis*1000000ull);
  }
  int was = L->signaled; L->signaled=0;
  mutexUnlock(&L->m);
  return was ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}
/* Unity rarely uses these two, but provide them for completeness. */
int ALooper_addFd(ALooper *l,int fd,int ident,int events,void *cb,void *data){
  (void)l;(void)fd;(void)ident;(void)events;(void)cb;(void)data; return 1; }
int ALooper_removeFd(ALooper *l,int fd){ (void)l;(void)fd; return 1; }

/* ==========================================================================
 * Sensors -- report none. (CR3 imported no ASensorManager; Unity does, so these
 * must exist and return a clean empty state rather than be missing symbols.)
 * ========================================================================== */
void *ASensorManager_getInstance(void){ static int x; return &x; }
void *ASensorManager_getInstanceForPackage(const char *p){ (void)p; return ASensorManager_getInstance(); }
int   ASensorManager_getSensorList(void *m, void **list){ (void)m; if(list)*list=NULL; return 0; }
void *ASensorManager_getDefaultSensor(void *m, int type){ (void)m;(void)type; return NULL; }
void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data){
  (void)m;(void)looper;(void)ident;(void)cb;(void)data; static int q; return &q; }
int   ASensorManager_destroyEventQueue(void *m, void *q){ (void)m;(void)q; return 0; }

int   ASensorEventQueue_enableSensor (void *q, const void *s){ (void)q;(void)s; return -1; }
int   ASensorEventQueue_disableSensor(void *q, const void *s){ (void)q;(void)s; return 0; }
int   ASensorEventQueue_setEventRate (void *q, const void *s, int32_t us){ (void)q;(void)s;(void)us; return 0; }
int   ASensorEventQueue_getEvents    (void *q, void *ev, size_t n){ (void)q;(void)ev;(void)n; return 0; }
int   ASensorEventQueue_hasEvents    (void *q){ (void)q; return 0; }

const char *ASensor_getName      (const void *s){ (void)s; return ""; }
const char *ASensor_getVendor    (const void *s){ (void)s; return ""; }
int         ASensor_getType      (const void *s){ (void)s; return 0; }
float       ASensor_getResolution(const void *s){ (void)s; return 0.0f; }
int         ASensor_getMinDelay  (const void *s){ (void)s; return 0; }

/* cr3 dead-handler stub: no orientation sensor -> report level. */
void android_get_orientation(float *x, float *y, float *z){
  if (x) *x = 0.0f;
  if (y) *y = 0.0f;
  if (z) *z = 0.0f;
}

/* ==========================================================================
 * HID -> Unity input, via nx_dual_pointer.
 *
 * nx_dual_pointer owns every pointing device on the console -- touchscreen,
 * both sticks, both Joy-Con gyros -- plus TWO on-screen cursors and their
 * settings file. Left stick / left Joy-Con gyro drive the blue cursor, right
 * stick / right Joy-Con gyro the red one; ZL and ZR tap independently, so
 * both can be down in the same frame and the game sees two real fingers.
 * It hands back device-independent NxdpEvents (id, x, y, phase); everything
 * below is the translation from those into the fake Android MotionEvents that
 * nativeInjectEvent expects (unity_input.c), and the B -> Back key mapping,
 * which is a game binding rather than a pointer concern.
 *
 * This replaces the port's original stick-cursor/dot-overlay implementation.
 * ========================================================================== */
#include <stdio.h>
#include "unity_input.h"
#include "nx_dual_pointer.h"
#include "nx_root.h"

/* Locked stdio from libc_shim.c. nx_dual_pointer writes pointer.cfg from the render
 * thread while the engine's workers are opening bundle files on their own
 * threads; both sides go through these so they never touch newlib's FILE table
 * at the same time. See the note above fopen_fn in nx_dual_pointer.h. */
FILE *nx_fopen_locked(const char *path, const char *mode);
int   nx_fclose_locked(FILE *f);

/* Our own pad, used ONLY for B -> Back. nx_dual_pointer keeps a separate PadState of
 * its own; that is fine, because padUpdate() snapshots HID shared memory into
 * whichever struct you hand it, so each PadState tracks its own press/release
 * edges independently. */
static PadState g_pad;

static void nxp_log_line(const char *msg){ debugPrintf("%s", msg); }

void android_native_input_init(void){
  NxdpConfig c;
  memset(&c, 0, sizeof c);

  /* Render size is fixed for the session (chosen at launch from config.txt),
   * so the module never needs to be told about a mid-session change. */
  c.screen_w        = (int)g_w;
  c.screen_h        = (int)g_h;
  c.panel_w         = 1280;          /* the touch panel reports in its own     */
  c.panel_h         = 720;           /* 1280x720 space at every resolution     */
  c.data_dir        = GAME_HOME;     /* == nx_root(), resolved at runtime      */

  /* Pointer ids must be SMALL. Unity maps an Android pointer id into a
   * fixed-size touch pool, so the module's 100/101 defaults were dropped
   * there -- the cursors drew and moved but their taps never arrived.
   *
   * PHIGROS DIFFERS FROM FRUIT NINJA HERE. Fruit Ninja gave real fingers 0..7
   * and the cursors 8/9, fitting inside 10. That is wrong for a rhythm game:
   * the judgement path is finger-indexed and hard charts use every finger the
   * panel reports, so capping fingers at 8 would silently drop the 9th and
   * 10th -- as a missed note, with nothing in the log to explain it.
   *
   * So: all 10 panel touches get ids 0..9, cursors move to 10/11, and
   * UI_MAX_POINTERS is 12. See PHI_TOUCH_SLOTS in config.h. */
  c.max_touch_slots = PHI_TOUCH_SLOTS;          /* 10 */
  c.left_id         = PHI_TOUCH_SLOTS;          /* 10 */
  c.right_id        = PHI_TOUCH_SLOTS + 1;      /* 11 */

  c.stick_speed     = 0.0f;          /* 0 => library default, then pointer.cfg */
  c.rotation        = 0;             /* Phigros is landscape; no TATE          */
  c.handle_touch    = 1;             /* module owns touch, as nx_pointer did   */
  c.log             = nxp_log_line;
  c.fopen_fn        = nx_fopen_locked;
  c.fclose_fn       = nx_fclose_locked;

  nxdp_init(&c);

  /* Pad for the Back key. nxdp_init has already called padConfigureInput. */
  padInitializeDefault(&g_pad);

  /* input_log_fn intentionally left unset: the MotionEvent-getter trace is a
   * debug aid (set it to debugPrintf to re-enable). Leaving it off keeps the
   * touch path log-silent so taps don't stutter on slow SD writes. */
}

/* Flush a pending sensitivity change on the way out. Normally nx_dual_pointer
 * saves by itself 3s after the last adjustment; this catches the case where
 * the player quits inside that window. */
void android_native_input_shutdown(void){
  nxdp_save_settings();
}

/* inject signature == recovered nativeInjectEvent: (env,thiz,InputEvent,int)->Z */
typedef uint8_t (*inject_fn)(void*,void*,void*,int);

/* ---- live pointer set ----------------------------------------------------
 * Android hands the engine the FULL set of pointers that are currently down on
 * every event; the action word says what happened, and for the multi-pointer
 * variants its high byte says which INDEX in that array it happened to. So we
 * have to keep the set ourselves rather than forwarding events one at a time.
 * Fingers and the cursor coexist here, which is what makes "touch the screen
 * while the cursor is up" behave like real multitouch instead of a fight. */
#define NXG_MAX UI_MAX_POINTERS

static int   g_live_id[NXG_MAX];
static float g_live_x [NXG_MAX];
static float g_live_y [NXG_MAX];
static int   g_live_n = 0;
static int   g_live_down_frame[NXG_MAX];   /* frame each live pointer went DOWN */
static int   g_frame_no = 0;

/* An UP held back so its DOWN gets a frame of its own -- see the note in
 * android_native_feed_hid. Small: only pointers that went down and up inside a
 * single frame land here, and each is released on the very next one. */
static struct { int id; float x, y; } g_defer_up[NXG_MAX];
static int   g_defer_n = 0;

/* Events carried to the next frame because their pointer has already completed
 * a DOWN this frame -- see the note in android_native_feed_hid. */
static NxdpEvent g_carry[NXDP_MAX_EVENTS];
static int       g_carry_n = 0;

static int live_find(int id){
  for (int i = 0; i < g_live_n; i++)
    if (g_live_id[i] == id) return i;
  return -1;
}

/* Count rejected injections. nativeInjectEvent returns false when the engine
 * declines an event -- a full input queue, or a state where it will not accept
 * one -- and this used to be discarded, so a dropped tap left no trace at all
 * and looked like a hardware or shim fault. */
static unsigned g_inj_ok, g_inj_rejected;

static unsigned g_carried_total;

void android_native_input_stats(unsigned *ok, unsigned *rejected,
                                unsigned *carried, unsigned *saturated){
  if (ok) *ok = g_inj_ok;
  if (rejected) *rejected = g_inj_rejected;
  if (carried) *carried = g_carried_total;
  if (saturated) *saturated = nxdp_touch_saturated();
}

static void live_remove(int idx){
  for (int k = idx + 1; k < g_live_n; k++){
    g_live_id[k-1]         = g_live_id[k];
    g_live_x [k-1]         = g_live_x [k];
    g_live_y [k-1]         = g_live_y [k];
    g_live_down_frame[k-1] = g_live_down_frame[k];
  }
  g_live_n--;
}

static void emit(inject_fn inject, void *env, void *thiz, int action){
  if (g_live_n <= 0) return;
  const uint8_t accepted = inject(env, thiz,
         unity_motionevent(action, g_live_n, g_live_id, g_live_x, g_live_y), 0);
  if (accepted) g_inj_ok++;
  else {
    g_inj_rejected++;
    static int warned = 0;
    if (!warned) { warned = 1;
      debugPrintf("[input] nativeInjectEvent REJECTED an event (action=0x%x). "
                  "The engine is refusing input -- most likely its queue is "
                  "full. Injected ok=%u rejected=%u\n",
                  action, g_inj_ok, g_inj_rejected); }
  }
}

void android_native_feed_hid(inject_fn inject, void *env, void *thiz){
  /* No set-screen call: the render size is chosen once at launch from
   * config.txt (handheld_res / docked_res) and held for the session, so g_w/g_h
   * cannot change under the module. */
  nxdp_update();

  /* NXDP_MAX_EVENTS, not a literal: nxdp_poll returns min(available, max), so a
   * short array here silently throws away the tail of the frame's input.
   * Doubled so last frame's carry-over and this frame's events both fit. */
  NxdpEvent ev[NXDP_MAX_EVENTS * 2];
  int n = 0;
  for (int c = 0; c < g_carry_n; c++) ev[n++] = g_carry[c];   /* carried first */
  g_carry_n = 0;
  n += nxdp_poll(ev + n, (int)(sizeof ev / sizeof ev[0]) - n);

  /* MOVEs are batched: several pointers can move in one frame, and Android
   * expresses that as ONE action with every pointer's new position, not one
   * event each. A DOWN or UP closes the batch. */
  int move_pending = 0;

  /* Release any UP deferred from last frame FIRST.
   *
   * Do this before this frame's events, or a finger that lifts and lands again
   * quickly would produce a DOWN while the old pointer is still live -- the
   * "already down, treat as move" path below -- and the two taps would merge
   * into one drag. */
  for (int d = 0; d < g_defer_n; d++){
    const int idx = live_find(g_defer_up[d].id);
    if (idx < 0) continue;
    g_live_x[idx] = g_defer_up[d].x;
    g_live_y[idx] = g_defer_up[d].y;
    emit(inject, env, thiz,
         (g_live_n == 1)
           ? AMOTION_ACTION_UP
           : (AMOTION_ACTION_POINTER_UP | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
    live_remove(idx);
  }
  g_defer_n = 0;
  g_frame_no++;

  for (int i = 0; i < n; i++){
    const int   id = ev[i].id;
    const float x  = ev[i].x, y = ev[i].y;
    int idx = live_find(id);

    if (ev[i].phase == NXDP_MOVE){
      if (idx < 0) continue;                    /* never saw its DOWN */
      g_live_x[idx] = x; g_live_y[idx] = y;
      move_pending = 1;
      continue;
    }

    if (move_pending){
      emit(inject, env, thiz, AMOTION_ACTION_MOVE);
      move_pending = 0;
    }

    if (ev[i].phase == NXDP_DOWN){
      /* A DOWN for a pointer whose UP is still deferred means this finger has
       * tapped TWICE inside one frame (or a new finger took the slot the moment
       * the old one left). Flush the held UP now, before the new DOWN.
       *
       * Without this the deferral swallows the second tap: live_find would
       * still see the pointer, take the "already down" path below, and turn a
       * fresh tap into a MOVE. Two taps in, one tap out -- the very bug the
       * deferral exists to prevent, reintroduced from the other side. Draining
       * the HID ring makes this reachable, because a frame now carries several
       * samples rather than one. */
      /* A DOWN for a pointer that has ALREADY completed a tap this frame means
       * two taps landed inside one frame -- realistic in a dense chart, and
       * reachable now that a frame carries many HID samples.
       *
       * An earlier revision flushed the first tap's held UP and let the second
       * proceed. That kept both EVENTS but gave the first tap no frame of its
       * own, so the game never saw that finger alive and the note scored
       * nothing -- the same drop, one tap earlier.
       *
       * Instead, stop here and carry this event and everything after it to the
       * next frame. The first tap keeps this frame; the second gets the next
       * one. Order is preserved because the carry is replayed ahead of the new
       * events, and it is self-limiting: the carried DOWN starts a fresh frame,
       * so nothing accumulates. */
      int deferred_here = 0;
      for (int d = 0; d < g_defer_n; d++)
        if (g_defer_up[d].id == id) { deferred_here = 1; break; }

      if (deferred_here){
        if (move_pending){ emit(inject, env, thiz, AMOTION_ACTION_MOVE); move_pending = 0; }
        for (int k = i; k < n && g_carry_n < (int)(sizeof g_carry / sizeof g_carry[0]); k++)
          { g_carry[g_carry_n++] = ev[k]; g_carried_total++; }
        break;                                     /* rest of the frame waits */
      }
      if (idx >= 0){                            /* already down -- treat as move */
        g_live_x[idx] = x; g_live_y[idx] = y;
        move_pending = 1;
        continue;
      }
      if (g_live_n >= NXG_MAX) continue;        /* out of slots */
      idx = g_live_n++;
      g_live_id[idx] = id; g_live_x[idx] = x; g_live_y[idx] = y;
      g_live_down_frame[idx] = g_frame_no;
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_DOWN
             : (AMOTION_ACTION_POINTER_DOWN | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
    }
    else if (ev[i].phase == NXDP_UP){
      if (idx < 0) continue;
      g_live_x[idx] = x; g_live_y[idx] = y;

      /* If this pointer went DOWN in this same frame, hold its UP until the
       * next one.
       *
       * The engine turns injected MotionEvents into its touch list before the
       * game's next Update() runs. A DOWN and an UP delivered together
       * therefore leave the game polling a touch that has already ended -- it
       * never observes the finger arrive. Phigros judges on finger index
       * (FingerManagement -> CheckNote(fingerIndex)), so such a tap is real,
       * correctly delivered, and still scores nothing.
       *
       * Deferring the UP by one frame guarantees the finger is visible alive
       * for at least one full frame, which is what every ordinary tap already
       * gets. This has nothing to do with reading the HID ring: it bites any
       * tap shorter than a frame and has been present since the first build.
       * Draining the ring only made such taps reach this layer at all -- before
       * that they were dropped even earlier.
       *
       * The cost is bounded and one-sided: the release is reported up to one
       * frame late, and only for taps already shorter than a frame. Nothing is
       * delayed that the game could otherwise have seen sooner. */
      if (g_live_down_frame[idx] == g_frame_no && g_defer_n < NXG_MAX){
        g_defer_up[g_defer_n].id = id;
        g_defer_up[g_defer_n].x  = x;
        g_defer_up[g_defer_n].y  = y;
        g_defer_n++;
        continue;                       /* stays live; released next frame */
      }
      /* The lifting pointer is still IN the array for its own UP -- that is how
       * getActionIndex() identifies which one left. Remove it afterwards. */
      emit(inject, env, thiz,
           (g_live_n == 1)
             ? AMOTION_ACTION_UP
             : (AMOTION_ACTION_POINTER_UP | (idx << AMOTION_ACTION_PTR_IDX_SHIFT)));
      live_remove(idx);
    }
  }

  if (move_pending) emit(inject, env, thiz, AMOTION_ACTION_MOVE);

  /* ---- B -> Android Back, edge-triggered ---- */
  padUpdate(&g_pad);
  const u64 bdown = padGetButtonsDown(&g_pad);
  const u64 bup   = padGetButtonsUp(&g_pad);
  if (bdown & HidNpadButton_B)
    inject(env, thiz, unity_keyevent(AKEY_ACTION_DOWN, AKEYCODE_BACK), 0);
  if (bup & HidNpadButton_B)
    inject(env, thiz, unity_keyevent(AKEY_ACTION_UP,   AKEYCODE_BACK), 0);
}

/* ==========================================================================
 * Cursor overlay. nx_dual_pointer draws both cursors (built-in arrow, or
 * cursor.png if one is
 * on the SD card) and saves/restores the GL state it touches; the wrapper here
 * supplies the two things it cannot know about from inside the library.
 * Called by the swap wrapper in imports.c, right before eglSwapBuffers.
 * ========================================================================== */
void android_native_draw_cursor(void){
  if (!nxdp_cursor_visible()) return;

  /* 1. VAO. Unity leaves one of its own vertex-array objects bound, and under
   *    GLES3 a non-zero VAO forbids client-side vertex arrays -- which is
   *    exactly what the cursor draws with, so glVertexAttribPointer would raise
   *    INVALID_OPERATION and nothing would appear. Binding VAO 0 for the
   *    duration also means every attribute change lands in a scratch VAO
   *    instead of Unity's, so restoring the binding restores it exactly.
   * 2. Viewport. The cursor shader maps render-space pixels straight to NDC, so
   *    it needs the viewport to cover the whole window; the engine may well
   *    have left it set to some intermediate render target. */
  GLint prev_vao = 0, vp[4];
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
  glGetIntegerv(GL_VIEWPORT, vp);

  glBindVertexArray(0);
  glViewport(0, 0, (GLsizei)g_w, (GLsizei)g_h);

  nxdp_draw();

  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glBindVertexArray((GLuint)prev_vao);
}
