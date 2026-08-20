/* unity_input.c -- fake MotionEvent / KeyEvent backing nativeInjectEvent.
 * See unity_input.h for the method surface (taken from libunity.so) and wiring.
 * Style mirrors unity_jni.c. Host-compilable plain C (no libnx). */

#include <string.h>
#include <time.h>
#include "unity_input.h"

struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

enum { UI_TAG = 0x55494531 /*'UIE1'*/, KIND_MOTION, KIND_KEY };

typedef struct {
  uint32_t tag; int kind;
  int   action;                 /* raw action (masked | ptrindex<<8)        */
  int   count;
  int   ids[UI_MAX_POINTERS];
  float xs [UI_MAX_POINTERS];
  float ys [UI_MAX_POINTERS];
  int   keycode;                /* KeyEvent                                 */
  int64_t time_ms;              /* getEventTime: this event                 */
  int64_t down_ms;              /* getDownTime: start of THIS gesture       */
} UEvent;

/* Allocated from the SAME ring as obtain() copies, not a single static.
 *
 * This used to be one reused UEvent on the theory that "injection is
 * synchronous". The obtain() note below says otherwise -- the engine reads its
 * copy after inject returns -- and that only holds if obtain() is called for
 * every event. If the engine ever reads the handle we passed directly, a single
 * static has already been overwritten by the next event in the same frame.
 * Handing out a ring slot costs nothing and removes the assumption. */
static UEvent   g_ev_copies[UI_EVENT_COPIES];
static unsigned g_ev_copy_i;

static UEvent *ev_alloc(void){
  const unsigned slot = __atomic_fetch_add(&g_ev_copy_i, 1u, __ATOMIC_RELAXED);
  return &g_ev_copies[slot & UI_EVENT_COPY_MASK];
}

/* ---- touch diagnostics (see unity_input.h) ---- */
int (*input_log_fn)(char *fmt, ...) = 0;
int   input_log_budget = 0;
#define ILOG(...) do { if (input_log_fn && input_log_budget > 0) { \
                         input_log_budget--; input_log_fn(__VA_ARGS__); } } while (0)

/* Event time, straight from the monotonic clock.
 *
 * An earlier revision forced this strictly increasing -- nudging by 1 ms when
 * two events landed in the same millisecond -- on the theory that a consumer
 * might de-duplicate by timestamp. Modelling it killed the idea: at ~28 events
 * per frame that stamps 28 ms of time per 16.7 ms of wall clock, so eventTime
 * ran 1.68x fast and was 106 ms ahead after eight frames of sustained tapping.
 * For a rhythm game reading event times, drifting the clock during play is a
 * far worse failure than the one it guarded against -- and the premise was
 * weak anyway, since batched MotionEvents on real Android routinely share an
 * eventTime. Left alone deliberately. */
static int64_t now_ms(void){
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (int64_t)ts.tv_sec*1000 + ts.tv_nsec/1000000;
}

/* Time of the current gesture's first ACTION_DOWN, for getDownTime(). Android
 * reports the gesture start, not this event's time; returning the latter made
 * every gesture look zero-length. */
static int64_t g_gesture_down_ms = 0;
static int  has(const char *s,const char *sub){ return strstr(s,sub)!=NULL; }
static int  is_ev(void *p,int kind){ UEvent*e=p; return e && e->tag==UI_TAG && e->kind==kind; }

/* ---- constructors ------------------------------------------------------- */
void *unity_motionevent(int action,int count,const int *ids,const float *xs,const float *ys){
  UEvent *e=ev_alloc(); memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_MOTION; e->action=action; e->time_ms=now_ms();
  /* A bare ACTION_DOWN (not POINTER_DOWN) starts a new gesture. */
  if ((action & AMOTION_ACTION_MASK) == AMOTION_ACTION_DOWN)
    g_gesture_down_ms = e->time_ms;
  e->down_ms = g_gesture_down_ms ? g_gesture_down_ms : e->time_ms;
  if (count>UI_MAX_POINTERS) count=UI_MAX_POINTERS;
  e->count=count;
  for (int i=0;i<count;i++){ e->ids[i]=ids?ids[i]:i; e->xs[i]=xs?xs[i]:0; e->ys[i]=ys?ys[i]:0; }
  return e;
}
void *unity_keyevent(int action,int keycode){
  UEvent *e=ev_alloc(); memset(e,0,sizeof *e);
  e->tag=UI_TAG; e->kind=KIND_KEY; e->action=action; e->keycode=keycode; e->time_ms=now_ms();
  e->down_ms = e->time_ms;
  return e;
}

/* MotionEvent.obtain(MotionEvent src): Android's copy factory. nativeInjectEvent
 * copies our injected event into one IT owns and reads that copy *after* inject
 * returns (across frames), so we must hand back a real, separate UEvent copy --
 * not the handle we were passed. Both the handle and the copy now come from the
 * same ring (see ev_alloc), so either is safe to hold. */
void *unity_motionevent_obtain(void *src){
  UEvent *s = src;
  if (!s || s->tag!=UI_TAG) return src;          /* not ours -> passthrough     */
  /* Same ring, same atomic bump: obtain() runs on whichever thread the engine
   * drives input from, and two callers must never receive the same slot. */
  UEvent *d = ev_alloc();
  *d = *s;
  return d;
}

/* ---- ownership ---------------------------------------------------------- */
int input_owns_class(const char *cls){
  return has(cls,"view/MotionEvent") || has(cls,"view/KeyEvent") ||
         has(cls,"view/InputEvent");
}
/* Route by receiver, not class name: GetObjectClass() on our event handle
 * reports java/lang/Object (jni_fake only special-cases Bitmap), so class-name
 * routing misses every getter the engine resolves via GetObjectClass(event).
 * The tag is unique to our UEvent handle, so this is exact. */
int input_owns_recv(const void *recv){
  const UEvent *e = recv; return e && e->tag==UI_TAG;
}
/* For instanceof classification by nativeInjectEvent: true if our handle is a
 * MotionEvent (vs KeyEvent). Caller must have checked input_owns_recv first. */
int input_recv_is_motion(const void *recv){
  const UEvent *e = recv; return e && e->tag==UI_TAG && e->kind==KIND_MOTION;
}

/* getX/getY/getPressure/... come as ()F or (I)F -- pull the pointer index when
 * the signature carries one. */
/* Index supplied by the JNI "A" (jvalue array) path, where there ARE no varargs.
 * -1 = not set. See input_set_a_index(). */
static int s_a_index = -1;
void input_set_a_index(int idx){ s_a_index = idx; }

static int ptr_index(const struct FakeID *id, va_list va){
  /* MotionEvent.getX/getY arrive through CallFloatMethodA, whose jvalue array
   * used to be discarded -- so va_arg here read an empty list, the garbage was
   * clamped to 0, and EVERY pointer reported pointer 0's coordinates. One finger
   * worked (index 0 is right by luck); two collapsed onto the first. Same class
   * of bug as round 24, in the input path this time.
   *
   * When the A path supplied the index, use it and clear it. */
  if (s_a_index >= 0) { const int i = s_a_index; s_a_index = -1; return i; }
  if (strstr(id->sig,"(I)")) { int idx=va_arg(va,int); return idx; }
  return 0;
}

/* ---- int / long getters ------------------------------------------------- */
uint64_t input_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  ILOG("    [in.i] %s  (cls=%s)\n", m, id->cls);
  if (!e || e->tag!=UI_TAG) return 0;

  /* shared InputEvent base */
  if (has(m,"getDeviceId")) return 0;
  if (has(m,"getSource"))   return (uint64_t)(e->kind==KIND_MOTION?AINPUT_SOURCE_TOUCHSCREEN:AINPUT_SOURCE_KEYBOARD);
  /* Distinct: getDownTime is the gesture start, getEventTime is now. Checked in
   * this order because "getDownTime" also contains "getDown"-like substrings
   * and has() is a substring match. */
  if (has(m,"getDownTime"))  return (uint64_t)e->down_ms;   /* long */
  if (has(m,"getEventTime")) return (uint64_t)e->time_ms;   /* long */
  if (has(m,"getMetaState")) return 0;
  if (has(m,"getFlags"))     return 0;

  if (e->kind==KIND_MOTION){
    if (has(m,"getActionMasked")) return (uint64_t)(e->action & AMOTION_ACTION_MASK);
    if (has(m,"getActionIndex"))  return (uint64_t)((e->action>>AMOTION_ACTION_PTR_IDX_SHIFT)&0xff);
    if (has(m,"getAction"))       return (uint64_t)e->action;
    if (has(m,"getPointerCount")) return (uint64_t)e->count;
    /* via ptr_index so the jvalue-array path supplies the index too -- reading
     * va_arg directly here had the same empty-va_list problem as getX/getY. */
    if (has(m,"getPointerId")){ int i=ptr_index(id, va); return (uint64_t)((i>=0&&i<e->count)?e->ids[i]:0); }
    if (has(m,"getToolType"))     return AMOTION_TOOL_TYPE_FINGER;
    if (has(m,"getButtonState"))  return 0;
    if (has(m,"getHistorySize"))  return 0;     /* no batched history -> engine skips getHistorical* */
    return 0;
  }
  /* KeyEvent */
  if (has(m,"getKeyCode"))     return (uint64_t)e->keycode;
  if (has(m,"getAction"))      return (uint64_t)e->action;
  if (has(m,"getRepeatCount")) return 0;
  if (has(m,"getUnicodeChar")||has(m,"GetUnicodeChar")) return 0;
  return 0;
}

/* ---- float getters ------------------------------------------------------ */
float input_dispatch_float(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  UEvent *e = recv; const char *m=id->name;
  ILOG("    [in.f] %s  (cls=%s)\n", m, id->cls);
  if (!e || e->tag!=UI_TAG || e->kind!=KIND_MOTION) return 0.0f;
  int i = ptr_index(id, va);
  if (i<0 || i>=e->count) i=0;
  if (has(m,"getRawX")||(has(m,"getX"))) return e->count? e->xs[i] : 0.0f;
  if (has(m,"getRawY")||(has(m,"getY"))) return e->count? e->ys[i] : 0.0f;
  if (has(m,"getPressure"))   return 1.0f;
  if (has(m,"getSize"))       return 0.1f;
  if (has(m,"getOrientation"))return 0.0f;
  return 0.0f;
}
