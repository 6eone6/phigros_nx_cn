/* imports.c -- dynamic-symbol resolution for libcrx.so + libc++_shared.so
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen (FF4 base)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 *
 * The MVGL engine pulls its C++ runtime from libc++_shared.so, so most std:: /
 * operator-new / __cxa imports resolve module-to-module. What is satisfied here:
 * a large libc subset (shimmed where bionic != newlib), GLES2 + EGL (mesa),
 * OpenSL ES (our shim), the libandroid NativeActivity API (android_native.c),
 * AAsset (data.c), AndroidBitmap (text2bitmap.c) and a few liblog/cxxabi helpers.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <malloc.h>
extern int z_strncasecmp(const char *, const char *, unsigned long);  /* NULL-safe, unity_imports.c */
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <errno.h>
#include <locale.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <switch.h>
#include "diag.h"

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "opensles.h"
#include "imports.h"
#include "android_native.h"
#include "unity_imports.h"

extern int *__errno(void);              // newlib

// ---------------------------------------------------------------------------
// liblog
// ---------------------------------------------------------------------------

/* Engine log lines that matter more than the rest, tagged so they stand out in a
 * long log. "Error setting value to 'X' on 'Y'" is the deserializer failing to
 * populate a field by reflection -- an object left part-initialised, which is the
 * upstream shape of every "non-object in a pointer slot" fault in this port. */
static void nx_flag_notable(const char *s) {
  if (!s) return;
  if (strstr(s, "Error setting value to"))
    debugPrintf("  ^^^ DESERIALIZE FAIL: a field was left unset -- object is "
                "part-initialised\n");
  else if (strstr(s, "Failed to load local player data"))
    debugPrintf("  ^^^ PLAYER DATA did not load\n");
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  (void)prio;
#if DEBUG_LOG
  va_list list; static char string[0x1000];
  va_start(list, fmt); vsnprintf(string, sizeof(string), fmt, list); va_end(list);
  debugPrintf("%s: %s\n", tag, string);
  nx_flag_notable(string);
#else
  (void)tag; (void)fmt;
#endif
  return 0;
}
int __android_log_write(int prio, const char *tag, const char *text) {
  (void)prio; debugPrintf("%s: %s\n", tag, text); return 0;
}
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list va) {
  (void)prio;
#if DEBUG_LOG
  static char string[0x1000]; vsnprintf(string, sizeof(string), fmt, va);
  debugPrintf("%s: %s\n", tag, string);
#else
  (void)tag; (void)fmt; (void)va;
#endif
  return 0;
}
void __assert2(const char *file, int line, const char *func, const char *expr) {
  debugPrintf("assert: %s:%d (%s): %s\n", file, line, func, expr); abort();
}

// ---------------------------------------------------------------------------
// stack protector / cxxabi
// ---------------------------------------------------------------------------

uint64_t __stack_chk_guard_fake = 0x0ull; /* match install_bionic_tls's zeroed tpidr+0x28 slot */
void __stack_chk_fail_fake(void) { debugPrintf("__stack_chk_fail\n"); abort(); }

int  __cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }
void __cxa_finalize_fake(void *dso) { (void)dso; }

// stdin/stdout/stderr point into the fake __sF block (see libc_shim.c)
FILE *stderr_fake = (FILE *)&fake_sF[2];

// ---------------------------------------------------------------------------
// pthread: bionic allocates the opaque types inline and zero-inits them, so we
// lazily back them with heap-allocated newlib objects stashed through the
// caller's pointer slot.
// ---------------------------------------------------------------------------

/* Heap-backed bionic pthread objects (round 55, adopted from ACPC).
 * A bionic pthread_mutex_t/cond_t slot is used as two 32-bit words: the
 * backing newlib object pointer is split low/high with the low bit marking
 * "ready". BIONIC_OBJECT_BUSY parks in words[0] while one thread builds the
 * object; concurrent callers spin. This keeps first-touch idempotent and the
 * encoding consistent across every lock/wait/signal. */
#define BIONIC_OBJECT_BUSY 0xfffffff0u
static int bionic_object_ready(uint32_t low) { return (low & 1u) != 0; }
static void *bionic_object_decode(const uint32_t *words, uint32_t low) {
  uintptr_t enc = (uintptr_t)low | ((uintptr_t)__atomic_load_n(&words[1], __ATOMIC_RELAXED) << 32);
  return (void *)(enc & ~(uintptr_t)1);
}
static void bionic_object_publish(void *storage, void *object) {
  uint32_t *words = storage;
  uintptr_t enc = (uintptr_t)object | 1u;
  __atomic_store_n(&words[1], (uint32_t)(enc >> 32), __ATOMIC_RELAXED);
  __atomic_store_n(&words[0], (uint32_t)enc, __ATOMIC_RELEASE);
}
static void *bionic_object_take(void *storage) {
  uint32_t *words = storage;
  for (;;) {
    uint32_t low = __atomic_load_n(&words[0], __ATOMIC_ACQUIRE);
    if (low == BIONIC_OBJECT_BUSY) { svcSleepThread(1000); continue; }
    if (!bionic_object_ready(low)) return NULL;
    uint32_t expected = low;
    if (__atomic_compare_exchange_n(&words[0], &expected, BIONIC_OBJECT_BUSY,
                                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return bionic_object_decode(words, low);
  }
}
static void bionic_object_clear(void *storage) {
  uint32_t *words = storage;
  __atomic_store_n(&words[1], 0, __ATOMIC_RELAXED);
  __atomic_store_n(&words[0], 0, __ATOMIC_RELEASE);
}
static int create_mutex(int recursive, pthread_mutex_t **out) {
  pthread_mutex_t *m = calloc(1, sizeof(*m));
  if (!m) return ENOMEM;
  int ret;
  if (recursive) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    ret = pthread_mutex_init(m, &a); pthread_mutexattr_destroy(&a);
  } else {
    ret = pthread_mutex_init(m, NULL);
  }
  if (ret != 0) { free(m); return ret; }
  *out = m; return 0;
}
int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  if (!uid) return EINVAL;
  pthread_mutex_t *m;
  int ret = create_mutex(attr && *attr == 1, &m);
  if (ret != 0) return ret;
  bionic_object_publish(uid, m);
  return 0;
}
int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (!uid) return EINVAL;
  pthread_mutex_t *m = bionic_object_take(uid);
  if (m) { pthread_mutex_destroy(m); free(m); bionic_object_clear(uid); }
  return 0;
}
static int ensure_mutex(pthread_mutex_t **uid, pthread_mutex_t **out) {
  if (!uid) return EINVAL;
  uint32_t *words = (uint32_t *)uid;
  for (;;) {
    uint32_t value = __atomic_load_n(&words[0], __ATOMIC_ACQUIRE);
    if (value == BIONIC_OBJECT_BUSY) { svcSleepThread(1000); continue; }
    if (bionic_object_ready(value)) { *out = bionic_object_decode(words, value); return 0; }
    uint32_t expected = value;
    if (!__atomic_compare_exchange_n(&words[0], &expected, BIONIC_OBJECT_BUSY,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      continue;
    pthread_mutex_t *m;
    int ret = create_mutex(value == 0x4000, &m);
    if (ret != 0) { __atomic_store_n(&words[0], value, __ATOMIC_RELEASE); return ret; }
    bionic_object_publish(uid, m);
    *out = m;
    return 0;
  }
}
/* Lightweight owner table: names a self-deadlock without per-op logging.
 * Small fixed set of recently-locked mutexes; enough to catch a producer
 * thread re-locking a lock it already holds. */
extern int gettid_fake(void);   /* libc_shim.c */
#define MTXOWN_N 64
#if MTXOWN_ENABLE
static void *mtxown_m[MTXOWN_N];
static int   mtxown_tid[MTXOWN_N];
static int   mtxown_depth[MTXOWN_N];
static Mutex mtxown_lk;
#endif
static void mtxown_acquired(void *m) {
#if !MTXOWN_ENABLE
  (void)m; return;
#else
  int tid = gettid_fake();
  mutexLock(&mtxown_lk);
  int free_i = -1;
  for (int i = 0; i < MTXOWN_N; i++) {
    if (mtxown_m[i] == m) { mtxown_tid[i] = tid; mtxown_depth[i]++; mutexUnlock(&mtxown_lk); return; }
    if (mtxown_m[i] == NULL && free_i < 0) free_i = i;
  }
  if (free_i >= 0) { mtxown_m[free_i] = m; mtxown_tid[free_i] = tid; mtxown_depth[free_i] = 1; }
  mutexUnlock(&mtxown_lk);
#endif
}
static void mtxown_released(void *m) {
#if !MTXOWN_ENABLE
  (void)m; return;
#else
  mutexLock(&mtxown_lk);
  for (int i = 0; i < MTXOWN_N; i++)
    if (mtxown_m[i] == m) { if (--mtxown_depth[i] <= 0) { mtxown_m[i] = NULL; mtxown_tid[i] = 0; } break; }
  mutexUnlock(&mtxown_lk);
#endif
}
static void mtxown_check_self(void *m) {
#if !MTXOWN_ENABLE
  (void)m; return;
#else
  int tid = gettid_fake();
  mutexLock(&mtxown_lk);
  for (int i = 0; i < MTXOWN_N; i++)
    if (mtxown_m[i] == m) {
      if (mtxown_tid[i] == tid)
        debugPrintf("[mtx] *** SELF-DEADLOCK: tid=%d re-locking mutex %p it already "
                    "holds (depth=%d) -- mutex needs to be recursive ***\n",
                    tid, m, mtxown_depth[i]);
      else {
        static int nrep = 0;
        if (nrep < 16) { nrep++;
          debugPrintf("[mtx] contended: tid=%d waits on %p held by tid=%d\n",
                      tid, m, mtxown_tid[i]); }
      }
      break;
    }
  mutexUnlock(&mtxown_lk);
#endif
}

int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  pthread_mutex_t *m; int ret = ensure_mutex(uid, &m);
  if (ret != 0) return ret;
  if (pthread_mutex_trylock(m) == 0) {   /* uncontended fast path */
    mtxown_acquired(m);
    return 0;
  }
  /* Contended. If THIS thread already owns it and the mutex is not
   * recursive, blocking here is a self-deadlock -- name it once instead of
   * hanging silently. */
  mtxown_check_self(m);
  diag_wait_enter(DIAG_W_MUTEX, m);
  int r = pthread_mutex_lock(m);
  diag_wait_exit();
  if (r == 0) mtxown_acquired(m);
  return r;
}
int pthread_mutex_trylock_fake(pthread_mutex_t **uid) { pthread_mutex_t *m; int ret = ensure_mutex(uid, &m); if (ret != 0) return ret; return pthread_mutex_trylock(m); }
int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  pthread_mutex_t *m; int ret = ensure_mutex(uid, &m);
  if (ret != 0) return ret;
  mtxown_released(m);
  return pthread_mutex_unlock(m);
}
int pthread_mutex_timedlock_fake(pthread_mutex_t **uid, const struct timespec *abs) {
  (void)abs;
  pthread_mutex_t *m; int ret = ensure_mutex(uid, &m);
  if (ret != 0) return ret;
  for (int i = 0; i < 1000; i++) {
    if (pthread_mutex_trylock(m) == 0) return 0;
    svcSleepThread(1000000ull);
  }
  return ETIMEDOUT;
}

static int ensure_cond(pthread_cond_t **cnd, pthread_cond_t **out) {
  if (!cnd) return EINVAL;
  uint32_t *words = (uint32_t *)cnd;
  for (;;) {
    uint32_t value = __atomic_load_n(&words[0], __ATOMIC_ACQUIRE);
    if (value == BIONIC_OBJECT_BUSY) { svcSleepThread(1000); continue; }
    if (bionic_object_ready(value)) { *out = bionic_object_decode(words, value); return 0; }
    uint32_t expected = value;
    if (!__atomic_compare_exchange_n(&words[0], &expected, BIONIC_OBJECT_BUSY,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      continue;
    pthread_cond_t *c = calloc(1, sizeof(*c));
    if (!c) { __atomic_store_n(&words[0], value, __ATOMIC_RELEASE); return ENOMEM; }
    int ret = pthread_cond_init(c, NULL);
    if (ret != 0) { free(c); __atomic_store_n(&words[0], value, __ATOMIC_RELEASE); return ret; }
    bionic_object_publish(cnd, c);
    *out = c;
    return 0;
  }
}
int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  (void)attr;
  if (!cnd) return EINVAL;
  pthread_cond_t *c = calloc(1, sizeof(*c));
  if (!c) return ENOMEM;
  int ret = pthread_cond_init(c, NULL);
  if (ret != 0) { free(c); return ret; }
  bionic_object_publish(cnd, c);
  return 0;
}
int pthread_cond_broadcast_fake(pthread_cond_t **cnd) { pthread_cond_t *c; int ret = ensure_cond(cnd, &c); if (ret != 0) return ret; return pthread_cond_broadcast(c); }
int pthread_cond_signal_fake(pthread_cond_t **cnd) { pthread_cond_t *c; int ret = ensure_cond(cnd, &c); if (ret != 0) return ret; return pthread_cond_signal(c); }
int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (!cnd) return EINVAL;
  pthread_cond_t *c = bionic_object_take(cnd);
  if (c) { pthread_cond_destroy(c); free(c); bionic_object_clear(cnd); }
  return 0;
}
/* round 65: per-cond adaptive re-poll backoff table (infinite cond wait). */
#define CONDHOP_N 128
static struct { pthread_cond_t *c; uint64_t hop; } g_condhop[CONDHOP_N];
static Mutex g_condhop_lk;
static uint64_t *condhop_for(pthread_cond_t *c) {
  unsigned h = ((uintptr_t)c >> 5) & (CONDHOP_N - 1);
  mutexLock(&g_condhop_lk);
  for (unsigned i = 0; i < CONDHOP_N; i++) {
    unsigned k = (h + i) & (CONDHOP_N - 1);
    if (g_condhop[k].c == c) { mutexUnlock(&g_condhop_lk); return &g_condhop[k].hop; }
    if (g_condhop[k].c == 0) { g_condhop[k].c = c; g_condhop[k].hop = PHI_FUTEX_HOP_MIN;
                               mutexUnlock(&g_condhop_lk); return &g_condhop[k].hop; }
  }
  mutexUnlock(&g_condhop_lk); return 0;   /* table full -> local default */
}
int pthread_cond_wait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  pthread_cond_t *c; pthread_mutex_t *m;
  int ret = ensure_cond(cnd, &c); if (ret != 0) return ret;
  ret = ensure_mutex(mtx, &m); if (ret != 0) return ret;
  extern volatile int g_main_tid;
  extern volatile unsigned long long g_mainwait_ns, g_mainwait_n;
  uint64_t __cw0 = armGetSystemTick();
  diag_wait_enter(DIAG_W_COND, c);
  uint64_t *hopp = condhop_for(c);             /* round 65: adaptive re-poll */
  uint64_t hop = hopp ? *hopp : PHI_FUTEX_HOP_MIN;
  struct timespec cap;
  clock_gettime(CLOCK_MONOTONIC, &cap);
  cap.tv_sec  += (cap.tv_nsec + (long)hop) / 1000000000L;
  cap.tv_nsec   = (cap.tv_nsec + (long)hop) % 1000000000L;
  int r = pthread_cond_timedwait(c, m, &cap);  /* recover a missed/silent signal */
  if (hopp) {
    if (r == ETIMEDOUT) { uint64_t nh = hop << 2;
                          *hopp = (nh > PHI_FUTEX_HOP_MAX) ? PHI_FUTEX_HOP_MAX : nh; }
    else *hopp = PHI_FUTEX_HOP_MIN;             /* real signal -> responsive again */
  }
  diag_wait_exit();
  { extern void twait_add(int, unsigned long long);
    twait_add(gettid_fake(), armTicksToNs(armGetSystemTick() - __cw0)); }
  if (g_main_tid && gettid_fake() == g_main_tid) {
    extern volatile unsigned long long g_mainwait_cond_ns, g_mainwait_cond_n;
    __atomic_add_fetch(&g_mainwait_cond_ns,
                       armTicksToNs(armGetSystemTick() - __cw0), __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_mainwait_cond_n, 1, __ATOMIC_RELAXED);
  }
  return (r == ETIMEDOUT) ? 0 : r;            /* timeout -> spurious wake; caller re-checks */
}
// Bound every timed cond-wait to at most COND_WAIT_CAP_MS. The .so libs (libc++
// std::condition_variable, and Swappy's frame pacer) compute an ABSOLUTE deadline
// against CLOCK_MONOTONIC, but newlib/libnx's pthread_cond_timedwait may measure
// "now" against a different clock -- a mismatch turns a ~16 ms vsync wait into an
// effectively infinite one (the Swappy hang: engine wedged in condvarWaitTimeout,
// frame counter frozen, black screen). Re-deriving the deadline as
// now(MONOTONIC)+min(requested, CAP) guarantees the wait can't exceed the cap
// regardless of which clock newlib uses, so the pacer hits its timeout fallback
// and keeps pacing. Spurious/early wakeups are POSIX-legal (every correct waiter
// re-checks its predicate), so this is safe in general.
#define COND_WAIT_CAP_MS 16
int pthread_cond_timedwait_fake(pthread_cond_t **cnd, pthread_mutex_t **mtx, const struct timespec *t) {
  pthread_cond_t *c; pthread_mutex_t *m;
  int e = ensure_cond(cnd, &c); if (e != 0) return e;
  e = ensure_mutex(mtx, &m); if (e != 0) return e;
  struct timespec now, cap;
  clock_gettime(CLOCK_MONOTONIC, &now);
  long add = COND_WAIT_CAP_MS * 1000000L;
  cap.tv_sec  = now.tv_sec + (now.tv_nsec + add) / 1000000000L;
  cap.tv_nsec = (now.tv_nsec + add) % 1000000000L;
  // honor the caller's deadline if it's sooner than our cap; else clamp to cap
  const struct timespec *use = &cap;
  if (t && (t->tv_sec < cap.tv_sec ||
            (t->tv_sec == cap.tv_sec && t->tv_nsec <= cap.tv_nsec)))
    use = t;
  diag_wait_enter(DIAG_W_COND, c);
  int r = pthread_cond_timedwait(c, m, use);
  diag_wait_exit();
  return r;
}

int pthread_once_fake(volatile int *once, void (*init)(void)) {
  if (!once || !init) return -1;
  if (__sync_lock_test_and_set(once, 1) == 0) (*init)();
  return 0;
}

int pthread_mutexattr_init_fake(int *a) { if (a) *a = 0; return 0; }
int pthread_mutexattr_settype_fake(int *a, int t) { if (a) *a = t; return 0; }

// bionic pthread_attr_t is opaque storage we own; stash size/detach there
#define ATTR_MAGIC 0x41545452 /* 'ATTR' */
typedef struct { uint32_t magic; uint32_t detach; size_t stacksize; } OurAttr;

int pthread_attr_init_fake(void *a) { if (a) { OurAttr *o = a; o->magic = ATTR_MAGIC; o->detach = 0; o->stacksize = 0; } return 0; }
int pthread_attr_destroy_fake(void *a) { (void)a; return 0; }
int pthread_attr_setdetachstate_fake(void *a, int s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->detach = (uint32_t)s; } return 0; }
int pthread_attr_setstacksize_fake(void *a, size_t s) { if (a) { OurAttr *o = a; if (o->magic == ATTR_MAGIC) o->stacksize = s; } return 0; }
int pthread_attr_getstacksize_fake(const void *a, size_t *s) { if (s) { const OurAttr *o = a; *s = (a && o->magic == ATTR_MAGIC && o->stacksize) ? o->stacksize : (512 * 1024); } return 0; }
int pthread_attr_setschedparam_fake(void *a, const void *p) { (void)a; (void)p; return 0; }

typedef struct { void *(*entry)(void *); void *arg; int is_main; uint8_t tls[BIONIC_TLS_SIZE]; } ThreadStart;
static volatile int g_first_engine_thread_taken = 0;
/* round 76: libnx creates every pthread on the process default core, so all
 * ~20 engine threads share ONE core while the others idle. Give each thread an
 * affinity mask covering every core the process owns, and rotate the ideal core
 * so they spread. Runs on the new thread, so CUR_THREAD_HANDLE is correct. */
static void nx_spread_thread_core(void) {
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || !mask)
    return;
  int cores[4], n = 0;
  for (int i = 0; i < 4; i++) if (mask & (1ull << i)) cores[n++] = i;
  { static int logged = 0;
    if (!logged) { logged = 1;
      debugPrintf("[cpu] process core mask=0x%llx (%d core(s) usable)\n",
                  (unsigned long long)mask, n); } }
  if (n <= 1) return;                 /* nothing to spread across */
  static int rr = 0;
  int c = cores[(unsigned)__atomic_fetch_add(&rr, 1, __ATOMIC_RELAXED) % (unsigned)n];
  Result r = svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, (u32)mask);
  { static int nlog = 0;
    if (nlog < 8) { nlog++;
      debugPrintf("[cpu] thread -> ideal core %d mask=0x%x rc=0x%x\n",
                  c, (unsigned)mask, (unsigned)r); } }
}
static void *thread_trampoline(void *p) {
  ThreadStart *ts = (ThreadStart *)p;   /* leaked on purpose: tpidr points into ts->tls */
  /* FIRST statement, before any loader setup. Pairs with the create-side log.
   *
   * Unity waits on a start handshake and never checks pthread_create's return
   * (see pthread_create_fake), so "creation failed" and "created but never
   * scheduled" produce an identical symptom: a permanent black screen with the
   * main thread in pthread_cond_wait. These two logs tell them apart. Logging
   * before install_bionic_tls/nx_spread_thread_core/diag_thread_register also
   * means a thread that wedges inside OUR setup is still visible -- if it were
   * logged after them, that case would look exactly like never starting. */
  { static volatile int started = 0;
    const int k = __atomic_add_fetch(&started, 1, __ATOMIC_RELAXED);
    if (k <= 48) debugPrintf("[thr] started #%d entry=%p\n", k, (void *)ts->entry); }
  install_bionic_tls(ts->tls);          // this thread's OWN stack-guard block (tpidr_el0+0x28)
  nx_spread_thread_core();              /* round 76: use all available cores */
  diag_thread_register(ts->entry, ts->is_main);
  void *r = ts->entry(ts->arg);
  diag_thread_unregister();
  // the first engine thread is android_main(); when it returns the engine has
  // quit, so flag the UI loop to tear down.
  if (ts->is_main)
    android_mark_main_finished();
  return r;
}
int pthread_create_fake(pthread_t *thread, const void *bionic_attr, void *entry, void *arg) {
  ThreadStart *ts = malloc(sizeof(*ts));
  if (!ts) return -1;
  ts->entry = (void *(*)(void *))entry;
  ts->arg = arg;
  ts->is_main = (__sync_lock_test_and_set(&g_first_engine_thread_taken, 1) == 0);
  size_t stack = 0;
  if (bionic_attr) {
    const OurAttr *o = bionic_attr;
    if (o->magic == ATTR_MAGIC) stack = o->stacksize;
  }
  const size_t asked = stack;
  /* Stack floor. The old rule was a flat 2 MB for EVERY thread, which is why
   * the first build that logged this showed a steady drip of
   *     pthread_create FAILED rc=12 (ENOMEM) stack=2048 KB
   * from the 13th thread onward -- Unity creates ~25 threads here and most of
   * them (Job.Worker, Background Job.Worker) pass no attr at all, so we were
   * demanding 2 MB apiece for threads that Android gives far less. Every one of
   * those failures then succeeded on the 512 KB retry, which is the evidence
   * that 2 MB was never needed.
   *
   * So: the big floor now applies only to the first engine thread, which is
   * android_main and really is stack-hungry. Everything else gets what it asked
   * for, or 512 KB when it asked for nothing. The retry ladder below still
   * exists as a safety net. */
  if (ts->is_main) { if (stack < (2u << 20)) stack = 2u << 20; }
  else if (stack == 0)          stack = 512u << 10;
  else if (stack < (128u << 10)) stack = 128u << 10;

  /* WHY THIS RETRIES AND LOGS.
   *
   * Unity's PlatformThread::Create does NOT check the return value of
   * pthread_create. It creates the thread, takes a Monitor, and then loops:
   *
   *     bl   pthread_create          <- return value discarded
   *     bl   Monitor::Lock
   *   loop:
   *     ldr  w8, [thread, #0x20]     <- flag the NEW thread sets on entry
   *     cbz  w8, Monitor::Wait ; b loop
   *
   * so a failed creation is not an error there -- it is an infinite wait for a
   * handshake from a thread that does not exist. That is indistinguishable
   * from a hang, which is exactly the black screen this build produced:
   * UnityMain parked in pthread_cond_wait_fake for 75 s, re-checking a flag
   * ~6000x/s that could never change.
   *
   * Two changes:
   *  1. NEVER fail silently. One log line turns that 75-second mystery into an
   *     answer.
   *  2. Retry with smaller stacks. The 2 MB floor is OUR tuning, not Unity's
   *     request -- and we apply it to every thread, including the dozen
   *     Background Job.Workers that ask for far less. If the allocation is
   *     what failed, honouring the caller's size (then a 512 KB floor) gets the
   *     thread created instead of wedging the engine. A thread with the stack
   *     it asked for is strictly better than no thread at all. */
  static volatile int live = 0, peak = 0;
  size_t tries[3]; int nt = 0;
  tries[nt++] = stack;
  if (asked && asked != stack) tries[nt++] = asked;   /* what the caller wanted */
  if (stack != (512u << 10) && asked != (512u << 10)) tries[nt++] = 512u << 10;

  int r = 0;
  for (int i = 0; i < nt; i++) {
    pthread_attr_t attr; pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, tries[i]);
    r = pthread_create(thread, &attr, thread_trampoline, ts);
    pthread_attr_destroy(&attr);
    if (r == 0) {
      const int n = __atomic_add_fetch(&live, 1, __ATOMIC_RELAXED);
      if (n > peak) peak = n;
      if (i > 0)
        debugPrintf("[thr] pthread_create OK only after falling back to %u KB "
                    "(asked %u KB, floor %u KB) -- entry=%p live=%d\n",
                    (unsigned)(tries[i] >> 10), (unsigned)(asked >> 10),
                    (unsigned)(stack >> 10), entry, n);
      return 0;
    }
    debugPrintf("[thr] pthread_create FAILED rc=%d stack=%u KB entry=%p live=%d\n",
                r, (unsigned)(tries[i] >> 10), entry, live);
  }
  /* Out of options. Say so loudly: the caller is Unity, which will now wait
   * forever for a start handshake that can never arrive. */
  debugPrintf("[thr] *** THREAD CREATION FAILED COMPLETELY (rc=%d, entry=%p) ***\n"
              "[thr] *** live=%d peak=%d. Unity does not check this return   ***\n"
              "[thr] *** value, so it will now hang waiting for this thread. ***\n"
              "[thr] *** If you see a black screen after this line, THIS is  ***\n"
              "[thr] *** the cause -- not whatever ran last.                 ***\n",
              r, entry, live, peak);
  free(ts);
  return r;
}
int pthread_join_fake(pthread_t thread, void **retval) {
  diag_wait_enter(DIAG_W_JOIN, (const void *)(uintptr_t)thread);
  int r = pthread_join(thread, retval);
  diag_wait_exit();
  return r;
}
int pthread_setschedparam_fake(pthread_t t, int policy, const void *p) { (void)t; (void)policy; (void)p; return 0; }
int pthread_sigmask_fake(int how, const void *set, void *old) { (void)how; (void)set; (void)old; return 0; }
int pthread_kill_fake(pthread_t t, int sig) { (void)t; (void)sig; return 0; }

// ---------------------------------------------------------------------------
// pthread TLS keys, multiplexed over a single real newlib key.
// devkitA64 backs pthread keys with a tiny pool (~16 libnx TLS slots), but
// Unity's runtime creates dozens during init (46 call sites). The ~17th
// pthread_key_create returns EAGAIN, and libunity treats that as fatal
// (asserts the key was created, else BRK). bionic allows 128 keys; emulate
// that: one real key holds a per-thread value array for up to 128 fake keys.
// ---------------------------------------------------------------------------
#define FAKE_KEYS_MAX 128
static pthread_mutex_t g_key_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct { int used; void (*dtor)(void *); } g_key_table[FAKE_KEYS_MAX];
static pthread_key_t g_master_key;
static int g_master_key_ready;
typedef struct { void *values[FAKE_KEYS_MAX]; } KeyValues;

static void master_key_dtor(void *p) {
  KeyValues *kv = p;
  for (int iter = 0; iter < 4; iter++) {     // POSIX: rerun while dtors set new values
    int again = 0;
    for (int i = 0; i < FAKE_KEYS_MAX; i++) {
      void *v = kv->values[i];
      if (g_key_table[i].used && g_key_table[i].dtor && v) {
        kv->values[i] = NULL;
        g_key_table[i].dtor(v);
        again = 1;
      }
    }
    if (!again) break;
  }
  free(kv);
}

int pthread_key_create_fake(unsigned *key, void (*dtor)(void *)) {
  pthread_mutex_lock(&g_key_mutex);
  if (!g_master_key_ready) {
    if (pthread_key_create(&g_master_key, master_key_dtor) != 0) {
      pthread_mutex_unlock(&g_key_mutex);
      debugPrintf("pthread_key_create_fake: master key alloc failed\n");
      return EAGAIN;
    }
    g_master_key_ready = 1;
  }
  for (unsigned i = 0; i < FAKE_KEYS_MAX; i++) {
    if (!g_key_table[i].used) {
      g_key_table[i].used = 1;
      g_key_table[i].dtor = dtor;
      *key = i + 1;                 // 1-based: a zeroed key is invalid
      pthread_mutex_unlock(&g_key_mutex);
      return 0;
    }
  }
  pthread_mutex_unlock(&g_key_mutex);
  debugPrintf("pthread_key_create_fake: out of keys\n");
  return EAGAIN;
}

int pthread_key_delete_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX) return EINVAL;
  pthread_mutex_lock(&g_key_mutex);
  g_key_table[key - 1].used = 0;
  g_key_table[key - 1].dtor = NULL;
  pthread_mutex_unlock(&g_key_mutex);
  return 0;
}

void *pthread_getspecific_fake(unsigned key) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return NULL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  return kv ? kv->values[key - 1] : NULL;
}

int pthread_setspecific_fake(unsigned key, const void *value) {
  if (key == 0 || key > FAKE_KEYS_MAX || !g_master_key_ready) return EINVAL;
  KeyValues *kv = pthread_getspecific(g_master_key);
  if (!kv) {
    kv = calloc(1, sizeof(*kv));
    if (!kv) return ENOMEM;
    pthread_setspecific(g_master_key, kv);
  }
  kv->values[key - 1] = (void *)value;
  return 0;
}

// ---------------------------------------------------------------------------
// misc small shims
// ---------------------------------------------------------------------------

static int ret0_i(void) { return 0; }
static int retm1_i(void) { return -1; }
static unsigned ret0_u(void) { return 0; }
static int signal_stub(int s, void *h) { (void)s; (void)h; return 0; }
static int sigaction_stub(int s, const void *a, void *o) { (void)s; (void)a; (void)o; return 0; }
static int ioctl_stub(int fd, unsigned long req, ...) { (void)fd; (void)req; return -1; }
static int fcntl_stub(int fd, int cmd, ...) { (void)fd; (void)cmd; return 0; }
static int tcgetattr_stub(int fd, void *t) { (void)fd; if (t) memset(t, 0, 60); return 0; }
static int tcsetattr_stub(int fd, int opt, const void *t) { (void)fd; (void)opt; (void)t; return 0; }

// POSIX file ops that devkitA64 newlib/libnx may not provide -- implement or
// stub them locally so the link never depends on their presence.
static int access_impl(const char *path, int mode) {
  (void)mode; struct stat st; return stat(path, &st) == 0 ? 0 : -1;
}
static int chmod_stub(const char *path, int mode) { (void)path; (void)mode; return 0; }
static int truncate_stub(const char *path, long len) { (void)path; (void)len; return 0; }
/* No-op by design: nothing here can shrink a file, and growing is handled
 * for free by the mmap write-back (fbmap_flush writes the whole mapped
 * range, which extends the file). Logged because a silent success on the
 * il2cpp extraction path is exactly what hid the 0-byte global-metadata.dat. */
static int ftruncate_stub(int fd, long len) {
  static int n = 0;
  if (n < 8) { n++;
    debugPrintf("[io] ftruncate(fd=%d, %ld) -> no-op (mmap write-back sizes it)\n",
                fd, len); }
  (void)fd; (void)len; return 0;
}
static int fsync_stub(int fd) { (void)fd; return 0; }
static int dup2_stub(int a, int b) { (void)a; return b; }
extern long rc_pread_pub(int fd, void *buf, size_t count, unsigned long long off);
static long pread_impl(int fd, void *buf, size_t n, long off) {
  { long got = rc_pread_pub(fd, buf, n, (unsigned long long)off);
    if (got >= 0) return got; }
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, off, SEEK_SET) < 0) return -1;
  size_t total = 0;
  while (total < n) {                       /* loop: fsdev can short-read */
    long r = read(fd, (char *)buf + total, n - total);
    if (r <= 0) break;
    total += (size_t)r;
  }
  lseek(fd, cur, SEEK_SET);
  watch_dump("pread", fd, (long)off, (long)n, buf, (long)total);
  return (long)total;
}
static long pwrite_impl(int fd, const void *buf, size_t n, long off) {
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0) return -1;
  if (lseek(fd, off, SEEK_SET) < 0) return -1;
  long r = write(fd, buf, n);
  lseek(fd, cur, SEEK_SET);
  return r;
}
static int uname_fake(void *buf) { if (buf) memset(buf, 0, 390); return 0; }
static long sysconf_pass(int n) { return sysconf_fake(n); }
static char *g_tzname_fake[2] = { (char *)"UTC", (char *)"UTC" };
static int readlink_stub(const char *p, char *b, size_t n) { (void)p; (void)b; (void)n; errno = EINVAL; return -1; }
static int link_stub(const char *a, const char *b) { (void)a; (void)b; return -1; }
static int symlink_stub(const char *a, const char *b) { (void)a; (void)b; return -1; }
static int fchmod_stub(int fd, int m) { (void)fd; (void)m; return 0; }
static int fchmodat_stub(int d, const char *p, int m, int f) { (void)d; (void)p; (void)m; (void)f; return 0; }
static int utimensat_stub(int d, const char *p, const void *t, int f) { (void)d; (void)p; (void)t; (void)f; return 0; }
static long sendfile_stub(int o, int i, long *off, size_t c) { (void)o; (void)i; (void)off; (void)c; return -1; }
static void *fdopendir_stub(int fd) { (void)fd; return NULL; }
static char *inet_ntoa_stub(uint32_t in) { (void)in; static char s[] = "0.0.0.0"; return s; }
static void _exit_fake(int code) { (void)code; extern void NX_NORETURN __libnx_exit(int rc); __libnx_exit(0); }

/* NULL-safe string ops. Unity's SystemInfo device-detection feeds the device
 * model/manufacturer (NULL in our fake JNI) straight into compare/search libc
 * calls; raw newlib then deref-NULL-crashes (this is what killed strcasecmp).
 * Treat NULL as an empty string: comparisons are unequal, searches find nothing,
 * length is zero -- so the device checks all fall through harmlessly. */
static int z_strcmp(const char *a, const char *b) {
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcmp(a, b);
}
static int z_strncmp(const char *a, const char *b, size_t n) {
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncmp(a, b, n);
}
static char *z_strstr(const char *h, const char *n) {
  if (!h || !n) return NULL;
  return strstr(h, n);
}
static char *z_strchr(const char *s, int c) { return s ? strchr(s, c) : NULL; }
static char *z_strrchr(const char *s, int c) { return s ? strrchr(s, c) : NULL; }
static size_t z_strlen(const char *s) { return s ? strlen(s) : 0; }

// engine-facing NDK functions implemented elsewhere ------------------------
// libandroid (android_native.c)
extern void *ALooper_prepare(int);
extern int   ALooper_addFd(void *, int, int, int, void *, void *);
extern int   ALooper_pollOnce(int, int *, int *, void **);
extern void  AInputQueue_attachLooper(void *, void *, int, void *, void *);
extern void  AInputQueue_detachLooper(void *);
extern int32_t AInputQueue_getEvent(void *, void **);
extern int32_t AInputQueue_preDispatchEvent(void *, void *);
extern void  AInputQueue_finishEvent(void *, void *, int);
extern int32_t AInputEvent_getType(const void *);
extern int32_t AMotionEvent_getAction(const void *);
extern size_t  AMotionEvent_getPointerCount(const void *);
extern int32_t AMotionEvent_getPointerId(const void *, size_t);
extern float   AMotionEvent_getX(const void *, size_t);
extern float   AMotionEvent_getY(const void *, size_t);
extern int32_t AKeyEvent_getKeyCode(const void *);
extern int32_t AKeyEvent_getFlags(const void *);
extern int32_t AKeyEvent_getRepeatCount(const void *);
extern int32_t ANativeWindow_setBuffersGeometry(void *, int32_t, int32_t, int32_t);
extern void *AConfiguration_new(void);
extern void  AConfiguration_fromAssetManager(void *, void *);
extern void  AConfiguration_getLanguage(void *, char *);
extern void  AConfiguration_getCountry(void *, char *);
extern void  AConfiguration_delete(void *);
extern int   ASensorEventQueue_enableSensor(void *, const void *);
extern int   ASensorEventQueue_disableSensor(void *, const void *);
extern int   ASensorEventQueue_setEventRate(void *, const void *, int32_t);
extern int   ASensorEventQueue_getEvents(void *, void *, size_t);
// AAsset (data.c)
extern void *AAssetManager_fromJava(void *, void *);
extern void *AAssetManager_open(void *, const char *, int);
extern const void *AAsset_getBuffer(void *);
extern int64_t AAsset_getLength(void *);
extern void  AAsset_close(void *);
// AndroidBitmap (text2bitmap.c)
extern int AndroidBitmap_getInfo(void *, void *, void *);
extern int AndroidBitmap_lockPixels(void *, void *, void **);
extern int AndroidBitmap_unlockPixels(void *, void *);

// ---------------------------------------------------------------------------
// GL shader fixups
// ---------------------------------------------------------------------------

// The MVGL "mono" composite shader assigns `monoCol` without declaring it.
// Android's GL driver tolerated it; mesa's stricter GLSL ES compiler rejects it,
// which kills the offscreen->screen copy and leaves the panel black. Inject the
// missing declaration at upload time.
static char *shader_fixups(const char *src) {
  const char *find = "monoCol = vec3";
  char *pos = strstr(src, find);
  if (!pos) return NULL;
  const char *repl = "vec3 monoCol = vec3";
  const size_t pre = (size_t)(pos - src), flen = strlen(find), rlen = strlen(repl);
  char *out = malloc(strlen(src) + (rlen - flen) + 1);
  if (!out) return NULL;
  memcpy(out, src, pre);
  memcpy(out + pre, repl, rlen);
  strcpy(out + pre + rlen, pos + flen);
  return out;
}
static void gl_ShaderSource_log(GLuint sh, GLsizei count, const GLchar *const *strs, const GLint *lens) {
  size_t total = 0;
  for (GLsizei i = 0; i < count; i++)
    total += (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
  char *buf = malloc(total + 1);
  if (!buf) { glShaderSource(sh, count, strs, lens); return; }
  size_t off = 0;
  for (GLsizei i = 0; i < count; i++) {
    size_t l = (lens && lens[i] >= 0) ? (size_t)lens[i] : strlen(strs[i]);
    memcpy(buf + off, strs[i], l); off += l;
  }
  buf[off] = 0;
  char *fixed = shader_fixups(buf);
  const GLchar *final_src = fixed ? fixed : buf;
  glShaderSource(sh, 1, &final_src, NULL);
  free(buf);
  free(fixed);
}

/* Completeness check on every FBO bind: the black screen is glerr=0x506
 * (GL_INVALID_FRAMEBUFFER_OPERATION) with draws>0, i.e. Unity renders into an
 * incomplete framebuffer and every draw is dropped. Report each offending FBO
 * once, with what is attached, so the unsupported format is identifiable. */
static unsigned g_tex0_reports;
typedef void (*pfn_ts2d)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (*pfn_rbs)(GLenum, GLenum, GLsizei, GLsizei);
static void gl_TexStorage2D_log(GLenum target, GLsizei levels, GLenum ifmt,
                                GLsizei w, GLsizei h) {
  static pfn_ts2d p; static int tried;
  if (!tried) { tried = 1; p = (pfn_ts2d)eglGetProcAddress("glTexStorage2D"); }
  GLint bound = 0; glGetIntegerv(0x8069 /*TEXTURE_BINDING_2D*/, &bound);
  { static unsigned seen; int watch = (bound >= 2300 && bound <= 2400);
    if (seen < 4) { seen++;
      debugPrintf("[gfx] texStorage tex=%d target=0x%x levels=%d ifmt=0x%x %dx%d\n",
                  (int)bound, (unsigned)target, (int)levels, (unsigned)ifmt, (int)w, (int)h); } }
  /* format substitution REVERTED: nvc0 supports ASTC natively on chipset
     * 0x12b (GM20B = Switch Tegra X1), and substituting HDR render-target
     * formats to RGBA8 did NOT fix the incomplete FBO -- the real cause was
     * GL_OUT_OF_MEMORY. Leave Unity's chosen formats alone so HDR range and
     * native ASTC are preserved. */
  if (w == 0 || h == 0) {
    if (g_tex0_reports < 3) { g_tex0_reports++;
      debugPrintf("[gfx] ZERO-SIZE texStorage tex=%d target=0x%x ifmt=0x%x %dx%d"
                  " -> clamping to %dx%d\n", (int)bound, (unsigned)target,
                  (unsigned)ifmt, (int)w, (int)h, screen_width, screen_height); }
    w = (GLsizei)screen_width; h = (GLsizei)screen_height;
  }
  if (levels < 1) levels = 1;
  if (p) p(target, levels, ifmt, w, h);
  if (bound >= 2300 && bound <= 2400) {
    GLenum e = glGetError();
    if (e) debugPrintf("[gfx] texStorage FAILED tex=%d ifmt=0x%x err=0x%x\n",
                       (int)bound, (unsigned)ifmt, (unsigned)e);
  }
}
static void gl_RenderbufferStorage_log(GLenum target, GLenum ifmt,
                                       GLsizei w, GLsizei h) {
  if (w == 0 || h == 0) {
    if (g_tex0_reports < 3) { g_tex0_reports++;
      debugPrintf("[gfx] ZERO-SIZE renderbuffer ifmt=0x%x %dx%d -> clamping to %dx%d\n",
                  (unsigned)ifmt, (int)w, (int)h, screen_width, screen_height); }
    w = (GLsizei)screen_width; h = (GLsizei)screen_height;
  }
  glRenderbufferStorage(target, ifmt, w, h);
}
static void gl_TexImage2D_log(GLenum target, GLint level, GLint ifmt,
                              GLsizei w, GLsizei h, GLint border,
                              GLenum fmt, GLenum type, const void *px) {
  if (level == 0 && (w == 0 || h == 0)) {
    GLint bound = 0; glGetIntegerv(0x8069 /*TEXTURE_BINDING_2D*/, &bound);
    if (g_tex0_reports < 20) {
      g_tex0_reports++;
      debugPrintf("[gfx] ZERO-SIZE tex=%d target=0x%x ifmt=0x%x %dx%d"
                  "  -> clamping to %dx%d\n",
                  (int)bound, (unsigned)target, (unsigned)ifmt, (int)w, (int)h,
                  screen_width, screen_height);
    }
    w = (GLsizei)screen_width; h = (GLsizei)screen_height;
    px = NULL;              /* caller's data (if any) no longer matches the size */
  }
  glTexImage2D(target, level, ifmt, w, h, border, fmt, type, px);
}
static void gl_FramebufferTexture2D_log(GLenum target, GLenum att, GLenum textarget,
                                        GLuint tex, GLint level) {
  glFramebufferTexture2D(target, att, textarget, tex, level);
  if (tex >= 2300 && tex <= 2400) {
    static unsigned n;
    if (n < 3) { n++;
      GLenum st = glCheckFramebufferStatus(target);
      debugPrintf("[gfx] fbTex2D att=0x%x textarget=0x%x tex=%u level=%d -> status=0x%x\n",
                  (unsigned)att, (unsigned)textarget, (unsigned)tex, (int)level, (unsigned)st);
    }
  }
}
static GLuint g_fbo_bad[32]; static int g_fbo_bad_n;
static void gl_BindFramebuffer_log(GLenum target, GLuint fb) {
  glBindFramebuffer(target, fb);
  if (!fb) return;                       /* default framebuffer is fine */
  GLenum st = glCheckFramebufferStatus(target);
  if (st == 0x8CD5 /*COMPLETE*/) return;
  for (int i = 0; i < g_fbo_bad_n; i++) if (g_fbo_bad[i] == fb) return;
  if (g_fbo_bad_n < 32) g_fbo_bad[g_fbo_bad_n++] = fb;
  GLint ct = 0, cn = 0, dt = 0, dn = 0, stt = 0, sn = 0;
  glGetFramebufferAttachmentParameteriv(target, 0x8CE0 /*COLOR0*/, 0x8CD0, &ct);
  glGetFramebufferAttachmentParameteriv(target, 0x8CE0,             0x8CD1, &cn);
  glGetFramebufferAttachmentParameteriv(target, 0x8D00 /*DEPTH*/,   0x8CD0, &dt);
  glGetFramebufferAttachmentParameteriv(target, 0x8D00,             0x8CD1, &dn);
  glGetFramebufferAttachmentParameteriv(target, 0x8D20 /*STENCIL*/, 0x8CD0, &stt);
  glGetFramebufferAttachmentParameteriv(target, 0x8D20,             0x8CD1, &sn);
  debugPrintf("[gfx] INCOMPLETE FBO=%u status=0x%x glErr=0x%x  color(type=0x%x name=%d)"
              "  depth(type=0x%x name=%d)  stencil(type=0x%x name=%d)\n",
              (unsigned)fb, (unsigned)st, (unsigned)glGetError(),
              (unsigned)ct, (int)cn,
              (unsigned)dt, (int)dn, (unsigned)stt, (int)sn);
}

// ---------------------------------------------------------------------------
// EGL surface-size fixup
// ---------------------------------------------------------------------------

// switch-mesa returns 0 for EGL_WIDTH/EGL_HEIGHT on the window surface even
// though it renders full-screen; the engine trusts that and sets a 0x0 viewport
// (nothing draws). Hand back the real panel size instead.
static EGLBoolean egl_QuerySurface_log(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val) {
  EGLBoolean r = eglQuerySurface(d, s, attr, val);
  if (val) {
    if (attr == 0x3057 /*EGL_WIDTH*/  && *val <= 0) { *val = screen_width;  r = EGL_TRUE; }
    if (attr == 0x3056 /*EGL_HEIGHT*/ && *val <= 0) { *val = screen_height; r = EGL_TRUE; }
  }
  return r;
}

// ---------------------------------------------------------------------------
// render diagnostics (black-screen triage): is the engine drawing into a real
// viewport and presenting? glViewport rect (distinct values only), draw-call
// count per presented frame, and the surface size eglSwapBuffers sees.
// ---------------------------------------------------------------------------
static unsigned long g_draw_calls = 0, g_swap_count = 0;
static int g_last_vp_w = -1, g_last_vp_h = -1;

static void gl_Viewport_log(GLint x, GLint y, GLsizei w, GLsizei h) {
  /* The game alternates two viewports every frame (low-res RT + upscale), so the
   * distinct-value guard alone still logs twice per frame -- a constant SD-write
   * drag now that the black screen is long fixed. Log only the first handful of
   * transitions (captures both sizes once at startup), then stay silent. */
  static int logged = 0;
  if ((int)w != g_last_vp_w || (int)h != g_last_vp_h) {
    g_last_vp_w = (int)w; g_last_vp_h = (int)h;
    if (logged < 8) { logged++;
      debugPrintf("[gfx] glViewport %d,%d %dx%d\n", (int)x, (int)y, (int)w, (int)h); }
  }
  glViewport(x, y, w, h);
}
static unsigned g_probe_reports;
static void gl_probe_draw(const char *what) {
  GLenum e = glGetError();
  if (e == 0 || g_probe_reports >= 4) return;
  g_probe_reports++;
  GLint fbo = 0; glGetIntegerv(0x8CA6 /*FRAMEBUFFER_BINDING*/, &fbo);
  GLenum st = glCheckFramebufferStatus(0x8D40);
  GLint ct = 0, cn = 0, dt = 0, dn = 0, stt = 0, sn = 0;
  glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD0, &ct);
  glGetFramebufferAttachmentParameteriv(0x8D40, 0x8CE0, 0x8CD1, &cn);
  glGetFramebufferAttachmentParameteriv(0x8D40, 0x8D00, 0x8CD0, &dt);
  glGetFramebufferAttachmentParameteriv(0x8D40, 0x8D00, 0x8CD1, &dn);
  glGetFramebufferAttachmentParameteriv(0x8D40, 0x8D20, 0x8CD0, &stt);
  glGetFramebufferAttachmentParameteriv(0x8D40, 0x8D20, 0x8CD1, &sn);
  debugPrintf("[gfx] DRAW-FAIL %s err=0x%x fbo=%d status=0x%x"
              "  color(0x%x,%d) depth(0x%x,%d) stencil(0x%x,%d)\n",
              what, (unsigned)e, (int)fbo, (unsigned)st,
              (unsigned)ct, (int)cn, (unsigned)dt, (int)dn, (unsigned)stt, (int)sn);
  {   /* name the internal format of each attached texture */
    /* glGetTexLevelParameteriv is GLES3.1/desktop-GL: resolve at runtime */
    typedef void (*pfn_gtlp)(GLenum, GLint, GLenum, GLint *);
    static pfn_gtlp p_gtlp; static int gtlp_tried;
    if (!gtlp_tried) { gtlp_tried = 1;
      p_gtlp = (pfn_gtlp)eglGetProcAddress("glGetTexLevelParameteriv"); }
    GLint prev = 0; glGetIntegerv(0x8069 /*TEXTURE_BINDING_2D*/, &prev);
    const GLint names[3] = { cn, dn, sn };
    const char *lbl[3]   = { "color", "depth", "stencil" };
    const GLint types[3] = { ct, dt, stt };
    for (int k = 0; k < 3; k++) {
      if (types[k] != 0x1702 /*GL_TEXTURE*/ || names[k] == 0) continue;
      glBindTexture(0x0DE1 /*TEXTURE_2D*/, (GLuint)names[k]);
      GLint ifmt = 0, tw = 0, th = 0;
      if (p_gtlp) p_gtlp(0x0DE1, 0, 0x1003 /*INTERNAL_FORMAT*/, &ifmt);
      if (p_gtlp) p_gtlp(0x0DE1, 0, 0x1000 /*WIDTH*/,  &tw);
      if (p_gtlp) p_gtlp(0x0DE1, 0, 0x1001 /*HEIGHT*/, &th);
      debugPrintf("[gfx]    %-7s tex=%d internalformat=0x%x %dx%d\n",
                  lbl[k], (int)names[k], (unsigned)ifmt, (int)tw, (int)th);
    }
    glBindTexture(0x0DE1, (GLuint)prev);
    glGetError();   /* swallow anything the probe itself provoked */
  }
}
static int gl_probe_now(void) {
  return g_swap_count == 60 && g_draw_calls < 2;
}
static void gl_DrawArrays_log(GLenum m, GLint f, GLsizei c) {
  int probe = gl_probe_now(); if (probe) glGetError();   /* clear */
  g_draw_calls++; glDrawArrays(m, f, c);
  if (probe) gl_probe_draw("glDrawArrays");
}
static void gl_DrawElements_log(GLenum m, GLsizei c, GLenum t, const void *i) {
  int probe = gl_probe_now(); if (probe) glGetError();   /* clear */
  g_draw_calls++; glDrawElements(m, c, t, i);
  if (probe) gl_probe_draw("glDrawElements");
}

/* Armed while the real eglSwapBuffers is executing, so the watchdog can report
 * "inside eglSwapBuffers for N ms" and the bufq wrappers below can trace the
 * exact libnx buffer-queue call the present is blocked in. */
volatile u64 g_swap_enter_tick = 0;

extern void android_native_draw_cursor(void);   /* docked cursor overlay (android_native_unity.c) */

static EGLBoolean egl_SwapBuffers_log(EGLDisplay d, EGLSurface s) {
  g_swap_count++;
  int trace = (g_swap_count <= 3) || (g_swap_count % 120 == 0);   /* boot + heartbeat */
  if (trace) {
    EGLint w = -1, h = -1;
    eglQuerySurface(d, s, 0x3057, &w);
    eglQuerySurface(d, s, 0x3056, &h);
    unsigned ge = (unsigned)glGetError();
    debugPrintf("[gfx] swap #%lu surface=%dx%d draws=%lu glerr=0x%x vp=%dx%d\n",
                g_swap_count, (int)w, (int)h, g_draw_calls, ge,
                g_last_vp_w, g_last_vp_h);
    if (ge == 0x0506) {   /* GL_INVALID_FRAMEBUFFER_OPERATION */
      GLint fbo = 0, st;
      glGetIntegerv(0x8CA6 /*GL_FRAMEBUFFER_BINDING*/, &fbo);
      st = (GLint)glCheckFramebufferStatus(0x8D40 /*GL_FRAMEBUFFER*/);
      debugPrintf("[gfx]   FBO=%d status=0x%x  (0x8CD5=COMPLETE 0x8CD6=ATTACH\n"
                  "[gfx]    0x8CD7=MISSING_ATTACH 0x8CD9=DIMENSIONS 0x8CDD=UNSUPPORTED\n"
                  "[gfx]    0x8D56=MULTISAMPLE)\n", (int)fbo, (unsigned)st);
    }
  }
  g_draw_calls = 0;
  android_native_draw_cursor();          /* overlay the docked cursor, then present */
  /* First-present hang mitigation + probe: force the frame's GPU work to
   * complete before handing the surface to mesa's present. If the present
   * was blocked waiting on that work, this unblocks it; if the work itself
   * never finishes, the hang moves into glFinish (armed tick still shows a
   * stall, but NOT "inside eglSwapBuffers"), which localises the wedge to
   * rendering rather than present. Gated to the first few swaps so steady
   * state keeps the GPU pipelined. */
  if (g_swap_count <= PHI_SWAP_FINISH_N) {
    typedef void (*fn_glfinish_t)(void);
    static fn_glfinish_t pglFinish = NULL;
    if (!pglFinish) pglFinish = (fn_glfinish_t)eglGetProcAddress("glFinish");
    if (pglFinish) {
      u64 ft = armGetSystemTick();
      g_swap_enter_tick = ft;            /* so a hang HERE is still caught */
      pglFinish();
      g_swap_enter_tick = 0;
      if (trace) debugPrintf("[gfx] swap #%lu glFinish done in %llu ms\n",
                             g_swap_count,
                             (unsigned long long)(armTicksToNs(armGetSystemTick() - ft) / 1000000ull));
    }
  }
  if (trace) debugLogFlush();   /* persist swap #N + glFinish line BEFORE the
                                 * present, so a hang inside it is visible */
  g_swap_enter_tick = armGetSystemTick();
  EGLBoolean r = eglSwapBuffers(d, s);
  g_swap_enter_tick = 0;
  if (trace) {
    debugPrintf("[gfx] swap #%lu eglSwapBuffers returned %d (err=0x%x)\n",
                g_swap_count, (int)r, (unsigned)eglGetError());
    debugLogFlush();   /* the present outcome is the diagnostic we keep
                        * losing to the buffer; persist it now */
  }
  return r;
}

/* ======================================================================= bufq
 * Link-time wraps (-Wl,--wrap) of the libnx NWindow buffer-queue calls that
 * mesa's EGL uses to implement the swapchain. A present that never returns is
 * blocked in one of these; the enter-without-exit line names it, and the rc of
 * the preceding calls shows why (e.g. a rejected queue -> a dequeue that waits
 * forever for a buffer the consumer never releases). Logging is gated: the
 * first few calls of each kind, plus anything while a swap is in flight. */
static int g_bufq_lines = 0;
#define BUFQ_LOG_CAP 96
static int bufq_should_log(int *counter) {
  if (g_bufq_lines >= BUFQ_LOG_CAP) return 0;
  if (*counter < 8) { (*counter)++; g_bufq_lines++; return 1; }
  if (g_swap_enter_tick) { g_bufq_lines++; return 1; }
  return 0;
}

Result __real_nwindowDequeueBuffer(NWindow *nw, s32 *out_slot, NvMultiFence *out_fence);
Result __wrap_nwindowDequeueBuffer(NWindow *nw, s32 *out_slot, NvMultiFence *out_fence) {
  static int n = 0; int lg = bufq_should_log(&n);
  if (lg) debugPrintf("[bufq] dequeue enter (swap#%lu)\n", g_swap_count);
  Result r = __real_nwindowDequeueBuffer(nw, out_slot, out_fence);
  if (lg) debugPrintf("[bufq] dequeue -> rc=0x%x slot=%d\n", r, out_slot ? (int)*out_slot : -1);
  return r;
}

Result __real_nwindowQueueBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence);
Result __real_nwindowCancelBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence);
Result __wrap_nwindowQueueBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence) {
  static int n = 0; int lg = bufq_should_log(&n);
  if (lg) {
    u32 w = 0, h = 0; nwindowGetDimensions(nw, &w, &h);
    debugPrintf("[bufq] queue slot=%d enter (swap#%lu) win=%ux%u crop=(%d,%d,%d,%d) scale=%u xform=%u fences=%d\n",
                (int)slot, g_swap_count, w, h,
                (int)nw->crop.left, (int)nw->crop.top, (int)nw->crop.right, (int)nw->crop.bottom,
                (unsigned)nw->scaling_mode, (unsigned)nw->transform,
                fence ? (int)fence->num_fences : -1);
    if (fence)
      for (u32 i = 0; i < fence->num_fences && i < 4; i++)
        debugPrintf("[bufq]   fence[%u] id=0x%x value=0x%x\n",
                    i, fence->fences[i].id, fence->fences[i].value);
  }
  Result r = __real_nwindowQueueBuffer(nw, slot, fence);
  if (R_FAILED(r)) {
    /* The consumer REJECTING a present (observed: rc=0x95d = Binder BAD_VALUE
     * = -EINVAL from nvnflinger's QueueBuffer) must not kill the process --
     * mesa's error path escalates it to a fatal (the 2349-0004 screen). The
     * prime suspect is the per-frame fence, so first retry the same slot with
     * an EMPTY fence (libnx sends num_fences=0; the slot stays dequeued on
     * failure, so the retry is legal). If the consumer still refuses, cancel
     * the buffer and report success: one dropped frame instead of a dead boot. */
    debugPrintf("[bufq] queue slot=%d REJECTED rc=0x%x (swap#%lu) -> retry with empty fence\n",
                (int)slot, r, g_swap_count);
    Result r2 = __real_nwindowQueueBuffer(nw, slot, NULL);
    debugPrintf("[bufq] queue retry -> rc=0x%x\n", r2);
    if (R_SUCCEEDED(r2)) return r2;
    Result r3 = __real_nwindowCancelBuffer(nw, slot, NULL);
    debugPrintf("[bufq] queue unrecoverable; cancel rc=0x%x -> dropping frame, reporting success\n", r3);
    return 0;
  }
  if (lg) debugPrintf("[bufq] queue slot=%d -> rc=0x0\n", (int)slot);
  return r;
}

Result __real_nwindowCancelBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence);
Result __wrap_nwindowCancelBuffer(NWindow *nw, s32 slot, const NvMultiFence *fence) {
  static int n = 0; int lg = bufq_should_log(&n);
  if (lg) debugPrintf("[bufq] cancel slot=%d enter\n", (int)slot);
  Result r = __real_nwindowCancelBuffer(nw, slot, fence);
  if (lg) debugPrintf("[bufq] cancel slot=%d -> rc=0x%x\n", (int)slot, r);
  return r;
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

/* round 61: allocator profiler. Time ONLY the real alloc call; the two
 * counter reads + relaxed atomics sit outside the measured span so g_alloc_ns
 * is a clean "wall time spent inside malloc/free". armGetSystemTick is a
 * register read, not a syscall. */
volatile unsigned long long g_alloc_calls = 0;
volatile unsigned long long g_alloc_ns    = 0;
static void *malloc_prof(size_t n) {
  uint64_t t = armGetSystemTick(); void *p = malloc(n);
  __atomic_add_fetch(&g_alloc_ns, armTicksToNs(armGetSystemTick() - t), __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_alloc_calls, 1, __ATOMIC_RELAXED); return p;
}
static void *calloc_prof(size_t a, size_t b) {
  uint64_t t = armGetSystemTick(); void *p = calloc(a, b);
  __atomic_add_fetch(&g_alloc_ns, armTicksToNs(armGetSystemTick() - t), __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_alloc_calls, 1, __ATOMIC_RELAXED); return p;
}
static void *realloc_prof(void *p, size_t n) {
  uint64_t t = armGetSystemTick(); void *r = realloc(p, n);
  __atomic_add_fetch(&g_alloc_ns, armTicksToNs(armGetSystemTick() - t), __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_alloc_calls, 1, __ATOMIC_RELAXED); return r;
}
static void free_prof(void *p) {
  uint64_t t = armGetSystemTick(); free(p);
  __atomic_add_fetch(&g_alloc_ns, armTicksToNs(armGetSystemTick() - t), __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_alloc_calls, 1, __ATOMIC_RELAXED);
}
DynLibFunction dynlib_functions[] = {
  // --- liblog / cxxabi / fortify markers ---
  { "__android_log_print", (uintptr_t)&__android_log_print },
  { "__android_log_write", (uintptr_t)&__android_log_write },
  { "__android_log_vprint", (uintptr_t)&__android_log_vprint },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "__assert2", (uintptr_t)&__assert2 },
  /* Round 150: interpose libil2cpp's own __cxa_throw. It is a DEFINED symbol
   * reached through the PLT, and so_resolve_symbol prefers this table over
   * module exports, so this needs no patching and no derived offset. Names
   * every managed exception -- see nx_cxa_throw in libc_shim.c. */
  { "__cxa_throw", (uintptr_t)&nx_cxa_throw },
  { "__cxa_atexit", (uintptr_t)&__cxa_atexit_fake },
  { "__cxa_finalize", (uintptr_t)&__cxa_finalize_fake },
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail_fake },
  { "__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake },
  { "__errno", (uintptr_t)&__errno },
  { "__get_h_errno", (uintptr_t)&__get_h_errno_fake },

  // --- fortify wrappers ---
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__snprintf_chk", (uintptr_t)&__snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&__sprintf_chk_fake },
  { "__open_2", (uintptr_t)&__open_2_fake },
  { "__read_chk", (uintptr_t)&__read_chk_fake },
  { "__pread_chk", (uintptr_t)&__pread_chk_fake },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },

  // --- bionic misc ---
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "__register_atfork", (uintptr_t)&__register_atfork_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "sysconf", (uintptr_t)&sysconf_pass },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "uname", (uintptr_t)&uname_fake },
  { "openlog", (uintptr_t)&ret0_i },
  { "closelog", (uintptr_t)&ret0_i },
  { "syslog", (uintptr_t)&ret0_i },
  { "abort", (uintptr_t)&abort },
  { "_exit", (uintptr_t)&_exit_fake },

  // --- memory ---
  { "malloc", (uintptr_t)&malloc_prof },
  { "calloc", (uintptr_t)&calloc_prof },
  { "realloc", (uintptr_t)&realloc_prof },
  { "free", (uintptr_t)&free_prof },
  { "memalign", (uintptr_t)&memalign },
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "mmap", (uintptr_t)&mmap_fake }, { "msync", (uintptr_t)&msync_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "madvise", (uintptr_t)&madvise_fake },

  // --- mem/str ---
  { "memchr", (uintptr_t)&memchr }, { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy }, { "memmove", (uintptr_t)&memmove },
  { "memset", (uintptr_t)&memset },
  { "strcat", (uintptr_t)&strcat }, { "strchr", (uintptr_t)&z_strchr },
  { "strcmp", (uintptr_t)&z_strcmp }, { "strcpy", (uintptr_t)&strcpy },
  { "strlen", (uintptr_t)&z_strlen }, { "strncasecmp", (uintptr_t)&z_strncasecmp },
  { "strncmp", (uintptr_t)&z_strncmp }, { "strncpy", (uintptr_t)&strncpy },
  { "strrchr", (uintptr_t)&z_strrchr }, { "strstr", (uintptr_t)&z_strstr },
  { "strtod", (uintptr_t)&strtod }, { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol }, { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll }, { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull }, { "atoi", (uintptr_t)&atoi },
  { "qsort", (uintptr_t)&qsort }, { "rand", (uintptr_t)&rand }, { "srand", (uintptr_t)&srand },
  { "isalnum", (uintptr_t)&isalnum }, { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper }, { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },

  // --- wide / multibyte / locale ---
  { "wcslen", (uintptr_t)&wcslen }, { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp }, { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof }, { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold }, { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul }, { "wcstoull", (uintptr_t)&wcstoull },
  { "btowc", (uintptr_t)&btowc }, { "wctob", (uintptr_t)&wctob },
  { "mbrlen", (uintptr_t)&mbrlen }, { "mbrtowc", (uintptr_t)&mbrtowc },
  { "mbtowc", (uintptr_t)&mbtowc }, { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "wcrtomb", (uintptr_t)&wcrtomb }, { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "setlocale", (uintptr_t)&setlocale }, { "localeconv", (uintptr_t)&localeconv },
  { "newlocale", (uintptr_t)&newlocale_fake }, { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "iswalpha_l", (uintptr_t)&iswalpha_l_fake }, { "iswblank_l", (uintptr_t)&iswblank_l_fake },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l_fake }, { "iswdigit_l", (uintptr_t)&iswdigit_l_fake },
  { "iswlower_l", (uintptr_t)&iswlower_l_fake }, { "iswprint_l", (uintptr_t)&iswprint_l_fake },
  { "iswpunct_l", (uintptr_t)&iswpunct_l_fake }, { "iswspace_l", (uintptr_t)&iswspace_l_fake },
  { "iswupper_l", (uintptr_t)&iswupper_l_fake }, { "iswxdigit_l", (uintptr_t)&iswxdigit_l_fake },
  { "towlower_l", (uintptr_t)&towlower_l_fake }, { "towupper_l", (uintptr_t)&towupper_l_fake },
  { "strcoll_l", (uintptr_t)&strcoll_l_fake }, { "strxfrm_l", (uintptr_t)&strxfrm_l_fake },
  { "strftime_l", (uintptr_t)&strftime_l_fake }, { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake }, { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "wcscoll_l", (uintptr_t)&wcscoll_l_fake }, { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l_fake },

  // --- printf family ---
  { "printf", (uintptr_t)&debugPrintf }, { "puts", (uintptr_t)&puts },
  { "snprintf", (uintptr_t)&snprintf }, { "sprintf", (uintptr_t)&sprintf },
  { "swprintf", (uintptr_t)&swprintf }, { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf }, { "vasprintf", (uintptr_t)&vasprintf },
  { "sscanf", (uintptr_t)&sscanf }, { "vsscanf", (uintptr_t)&vsscanf },

  // --- math ---
  { "acosf", (uintptr_t)&acosf }, { "asinf", (uintptr_t)&asinf },
  { "atan2f", (uintptr_t)&atan2f }, { "cosf", (uintptr_t)&cosf },
  { "sinf", (uintptr_t)&sinf }, { "tanf", (uintptr_t)&tanf },
  { "expf", (uintptr_t)&expf }, { "logf", (uintptr_t)&logf },
  { "powf", (uintptr_t)&powf }, { "pow", (uintptr_t)&pow },
  { "fmodf", (uintptr_t)&fmodf }, { "sincosf", (uintptr_t)&sincosf_fake },

  // --- time ---
  { "clock_gettime", (uintptr_t)&clock_gettime }, { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime }, { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime }, { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime }, { "time", (uintptr_t)&time },
  { "nanosleep", (uintptr_t)&nanosleep }, { "usleep", (uintptr_t)&usleep },
  { "tzset", (uintptr_t)&tzset }, { "tzname", (uintptr_t)&g_tzname_fake },
  { "getenv", (uintptr_t)&getenv_fake }, { "putenv", (uintptr_t)&putenv },

  // --- stdio (fake __sF aware) ---
  { "__sF", (uintptr_t)&fake_sF },
  { "stdin", (uintptr_t)&fake_sF[0] }, { "stdout", (uintptr_t)&fake_sF[1] }, { "stderr", (uintptr_t)&fake_sF[2] },
  { "fopen", (uintptr_t)&fopen_fake }, { "fclose", (uintptr_t)&fclose_fake },
  { "fread", (uintptr_t)&fread_fake }, { "fwrite", (uintptr_t)&fwrite_fake },
  { "fseek", (uintptr_t)&fseek_fake }, { "fseeko", (uintptr_t)&fseeko },
  { "ftell", (uintptr_t)&ftell_fake }, { "ftello", (uintptr_t)&ftello },
  { "fflush", (uintptr_t)&fflush_fake }, { "fprintf", (uintptr_t)&fprintf_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake }, { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake }, { "fgetc", (uintptr_t)&fgetc_fake },
  { "fgets", (uintptr_t)&fgets_fake }, { "getc", (uintptr_t)&getc_fake },
  { "getwc", (uintptr_t)&getc_fake }, { "fputwc", (uintptr_t)&fputc_fake },
  { "ungetc", (uintptr_t)&ungetc_fake }, { "ungetwc", (uintptr_t)&ungetc_fake },
  { "feof", (uintptr_t)&feof_fake }, { "ferror", (uintptr_t)&ferror_fake },
  { "fileno", (uintptr_t)&fileno_fake }, { "remove", (uintptr_t)&remove },
  { "rename", (uintptr_t)&rename_fake },

  // --- filesystem ---
  { "open", (uintptr_t)&open_fake }, { "openat", (uintptr_t)&openat_fake },
  { "close", (uintptr_t)&close_fake }, { "read", (uintptr_t)&read_fake },
  { "write", (uintptr_t)&write_fake }, { "pwrite", (uintptr_t)&pwrite_impl },
  { "pread", (uintptr_t)&pread_impl },
  { "lseek", (uintptr_t)&z_lseek }, { "pipe", (uintptr_t)&pipe_fake },
  { "poll", (uintptr_t)&poll_fake }, { "select", (uintptr_t)&select_fake },
  { "dup2", (uintptr_t)&dup2_stub }, { "fcntl", (uintptr_t)&fcntl_stub },
  { "ioctl", (uintptr_t)&ioctl_stub }, { "isatty", (uintptr_t)&isatty },
  { "tcgetattr", (uintptr_t)&tcgetattr_stub }, { "tcsetattr", (uintptr_t)&tcsetattr_stub },
  { "stat", (uintptr_t)&stat_fake }, { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake }, { "statfs", (uintptr_t)&statfs_fake },
  { "statvfs", (uintptr_t)&statvfs_fake }, { "access", (uintptr_t)&access_impl },
  { "mkdir", (uintptr_t)&mkdir_fake }, { "rmdir", (uintptr_t)&rmdir },
  { "unlink", (uintptr_t)&unlink }, { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "chdir", (uintptr_t)&chdir }, { "getcwd", (uintptr_t)&getcwd_fake },
  { "chmod", (uintptr_t)&chmod_stub }, { "fchmod", (uintptr_t)&fchmod_stub },
  { "fchmodat", (uintptr_t)&fchmodat_stub }, { "truncate", (uintptr_t)&truncate_stub },
  { "ftruncate", (uintptr_t)&ftruncate_stub }, { "fsync", (uintptr_t)&fsync_stub },
  { "link", (uintptr_t)&link_stub }, { "symlink", (uintptr_t)&symlink_stub },
  { "readlink", (uintptr_t)&readlink_stub }, { "utime", (uintptr_t)&ret0_i },
  { "utimensat", (uintptr_t)&utimensat_stub }, { "sendfile", (uintptr_t)&sendfile_stub },
  { "opendir", (uintptr_t)&opendir_fake }, { "closedir", (uintptr_t)&closedir_fake },
  { "readdir", (uintptr_t)&readdir_fake }, { "fdopendir", (uintptr_t)&fdopendir_stub },
  { "realpath", (uintptr_t)&realpath_fake },
  { "strerror", (uintptr_t)&strerror }, { "strerror_r", (uintptr_t)&strerror_r_fake },

  // --- signals / setjmp ---
  { "signal", (uintptr_t)&signal_stub }, { "sigaction", (uintptr_t)&sigaction_stub },
  { "sigaddset", (uintptr_t)&ret0_i }, { "sigemptyset", (uintptr_t)&ret0_i },
  { "setjmp", (uintptr_t)&setjmp }, { "longjmp", (uintptr_t)&longjmp },
  { "siglongjmp", (uintptr_t)&longjmp },

  // --- process / ids ---
  { "getpid", (uintptr_t)&getpid_fake }, { "getuid", (uintptr_t)&ret0_u },
  { "geteuid", (uintptr_t)&ret0_u }, { "getegid", (uintptr_t)&ret0_u },
  { "getpwuid", (uintptr_t)&getpwuid_fake }, { "getrusage", (uintptr_t)&getrusage_fake },
  { "fork", (uintptr_t)&fork_fake }, { "execvp", (uintptr_t)&execvp_fake },
  { "waitpid", (uintptr_t)&waitpid_fake }, { "kill", (uintptr_t)&kill_fake },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "sched_get_priority_max", (uintptr_t)&sched_get_priority_max_fake },
  { "sched_get_priority_min", (uintptr_t)&sched_get_priority_min_fake },

  // --- dynamic loader ---
  { "dlopen", (uintptr_t)&dlopen_fake }, { "dlclose", (uintptr_t)&dlclose_fake },
  { "dlerror", (uintptr_t)&dlerror_fake }, { "dlsym", (uintptr_t)&dlsym_fake },

  // --- networking (offline) ---
  { "socket", (uintptr_t)&socket_fake }, { "connect", (uintptr_t)&connect_fake },
  { "bind", (uintptr_t)&bind_fake }, { "listen", (uintptr_t)&listen_fake },
  { "accept", (uintptr_t)&accept_fake }, { "send", (uintptr_t)&send_fake },
  { "recv", (uintptr_t)&recv_fake }, { "sendto", (uintptr_t)&sendto_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake }, { "shutdown", (uintptr_t)&shutdown_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake }, { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "getsockname", (uintptr_t)&getsockname_fake }, { "getpeername", (uintptr_t)&getpeername_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake }, { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake }, { "gethostname", (uintptr_t)&gethostname_fake },
  { "getservbyname", (uintptr_t)&getservbyname_fake },
  { "if_nametoindex", (uintptr_t)&if_nametoindex_fake }, { "if_indextoname", (uintptr_t)&if_indextoname_fake },
  // inet_aton returns 0 on FAILURE (nonzero == success); use ret0_i so callers
  // see a clean failure. inet_pton returns <=0 on error, so retm1_i is correct.
  { "inet_aton", (uintptr_t)&ret0_i }, { "inet_pton", (uintptr_t)&retm1_i },
  { "inet_ntoa", (uintptr_t)&inet_ntoa_stub },

  // --- pthread ---
  { "pthread_create", (uintptr_t)&pthread_create_fake }, { "pthread_join", (uintptr_t)&pthread_join_fake },
  { "pthread_detach", (uintptr_t)&pthread_detach }, { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_self", (uintptr_t)&pthread_self }, { "pthread_kill", (uintptr_t)&pthread_kill_gc },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_fake }, { "pthread_key_delete", (uintptr_t)&pthread_key_delete_fake },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_fake }, { "pthread_setspecific", (uintptr_t)&pthread_setspecific_fake },
  { "pthread_once", (uintptr_t)&pthread_once_fake },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_fake },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_fake },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake },
  { "pthread_mutex_timedlock", (uintptr_t)&pthread_mutex_timedlock_fake },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake },
  { "pthread_mutexattr_destroy", (uintptr_t)&ret0_i },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_fake },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_fake },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_fake },
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_fake },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_fake },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake },
  { "sem_init", (uintptr_t)&sem_init_fake }, { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_post", (uintptr_t)&sem_post_fake }, { "sem_wait", (uintptr_t)&sem_wait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake },

  // --- EGL (mesa; QuerySurface wrapped to fix the 0x0 surface-size report) ---
  { "eglGetDisplay", (uintptr_t)&eglGetDisplay }, { "eglInitialize", (uintptr_t)&eglInitialize },
  { "eglTerminate", (uintptr_t)&eglTerminate }, { "eglGetConfigs", (uintptr_t)&eglGetConfigs },
  { "eglGetConfigAttrib", (uintptr_t)&eglGetConfigAttrib },
  { "eglCreateWindowSurface", (uintptr_t)&eglCreateWindowSurface },
  { "eglCreateContext", (uintptr_t)&eglCreateContext }, { "eglMakeCurrent", (uintptr_t)&eglMakeCurrent },
  { "eglSwapBuffers", (uintptr_t)&egl_SwapBuffers_log }, { "eglQuerySurface", (uintptr_t)&egl_QuerySurface_log },
  { "eglDestroyContext", (uintptr_t)&eglDestroyContext }, { "eglDestroySurface", (uintptr_t)&eglDestroySurface },

  // --- GLES2 (mesa) ---
  { "glActiveTexture", (uintptr_t)&glActiveTexture }, { "glAttachShader", (uintptr_t)&glAttachShader },
  { "glBindBuffer", (uintptr_t)&glBindBuffer }, { "glBindFramebuffer", (uintptr_t)&gl_BindFramebuffer_log },
  { "glTexImage2D", (uintptr_t)&gl_TexImage2D_log },
  { "glTexStorage2D", (uintptr_t)&gl_TexStorage2D_log },
  { "glFramebufferTexture2D", (uintptr_t)&gl_FramebufferTexture2D_log },
  { "glBindRenderbuffer", (uintptr_t)&glBindRenderbuffer }, { "glBindTexture", (uintptr_t)&glBindTexture },
  { "glBlendEquationSeparate", (uintptr_t)&glBlendEquationSeparate }, { "glBlendFunc", (uintptr_t)&glBlendFunc },
  { "glBufferData", (uintptr_t)&glBufferData }, { "glClear", (uintptr_t)&glClear },
  { "glClearColor", (uintptr_t)&glClearColor }, { "glClearDepthf", (uintptr_t)&glClearDepthf },
  { "glClearStencil", (uintptr_t)&glClearStencil }, { "glColorMask", (uintptr_t)&glColorMask },
  { "glCompileShader", (uintptr_t)&glCompileShader }, { "glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2D },
  { "glCreateProgram", (uintptr_t)&glCreateProgram }, { "glCreateShader", (uintptr_t)&glCreateShader },
  { "glCullFace", (uintptr_t)&glCullFace }, { "glDeleteBuffers", (uintptr_t)&glDeleteBuffers },
  { "glDeleteFramebuffers", (uintptr_t)&glDeleteFramebuffers }, { "glDeleteProgram", (uintptr_t)&glDeleteProgram },
  { "glDeleteRenderbuffers", (uintptr_t)&glDeleteRenderbuffers }, { "glDeleteShader", (uintptr_t)&glDeleteShader },
  { "glDeleteTextures", (uintptr_t)&glDeleteTextures }, { "glDepthFunc", (uintptr_t)&glDepthFunc },
  { "glDepthMask", (uintptr_t)&glDepthMask }, { "glDisable", (uintptr_t)&glDisable },
  { "glDisableVertexAttribArray", (uintptr_t)&glDisableVertexAttribArray }, { "glDrawArrays", (uintptr_t)&gl_DrawArrays_log },
  { "glDrawElements", (uintptr_t)&gl_DrawElements_log }, { "glEnable", (uintptr_t)&glEnable },
  { "glEnableVertexAttribArray", (uintptr_t)&glEnableVertexAttribArray },
  { "glFramebufferRenderbuffer", (uintptr_t)&glFramebufferRenderbuffer },
   { "glGenBuffers", (uintptr_t)&glGenBuffers },
  { "glGenFramebuffers", (uintptr_t)&glGenFramebuffers }, { "glGenRenderbuffers", (uintptr_t)&glGenRenderbuffers },
  { "glGenTextures", (uintptr_t)&glGenTextures }, { "glGetAttribLocation", (uintptr_t)&glGetAttribLocation },
  { "glGetError", (uintptr_t)&glGetError }, { "glGetProgramiv", (uintptr_t)&glGetProgramiv },
  { "glGetShaderiv", (uintptr_t)&glGetShaderiv }, { "glGetUniformLocation", (uintptr_t)&glGetUniformLocation },
  { "glLinkProgram", (uintptr_t)&glLinkProgram }, { "glPixelStorei", (uintptr_t)&glPixelStorei },
  { "glPolygonOffset", (uintptr_t)&glPolygonOffset }, { "glReadPixels", (uintptr_t)&glReadPixels },
  { "glRenderbufferStorage", (uintptr_t)&gl_RenderbufferStorage_log }, { "glScissor", (uintptr_t)&glScissor },
  { "glShaderSource", (uintptr_t)&gl_ShaderSource_log }, { "glStencilFunc", (uintptr_t)&glStencilFunc },
  { "glStencilMask", (uintptr_t)&glStencilMask }, { "glStencilOp", (uintptr_t)&glStencilOp },
  { "glTexParameteri", (uintptr_t)&glTexParameteri },
  { "glTexSubImage2D", (uintptr_t)&glTexSubImage2D }, { "glUniform1fv", (uintptr_t)&glUniform1fv },
  { "glUniform1i", (uintptr_t)&glUniform1i }, { "glUniform2fv", (uintptr_t)&glUniform2fv },
  { "glUniform3fv", (uintptr_t)&glUniform3fv }, { "glUniform4fv", (uintptr_t)&glUniform4fv },
  { "glUniformMatrix4fv", (uintptr_t)&glUniformMatrix4fv }, { "glUseProgram", (uintptr_t)&glUseProgram },
  { "glVertexAttribPointer", (uintptr_t)&glVertexAttribPointer }, { "glViewport", (uintptr_t)&gl_Viewport_log },

  // --- libandroid: NativeActivity API (android_native.c) ---
  { "ALooper_prepare", (uintptr_t)&ALooper_prepare }, { "ALooper_addFd", (uintptr_t)&ALooper_addFd },
  { "ALooper_pollOnce", (uintptr_t)&ALooper_pollOnce },
  { "AInputQueue_attachLooper", (uintptr_t)&AInputQueue_attachLooper },
  { "AInputQueue_detachLooper", (uintptr_t)&AInputQueue_detachLooper },
  { "AInputQueue_getEvent", (uintptr_t)&AInputQueue_getEvent },
  { "AInputQueue_preDispatchEvent", (uintptr_t)&AInputQueue_preDispatchEvent },
  { "AInputQueue_finishEvent", (uintptr_t)&AInputQueue_finishEvent },
  { "AInputEvent_getType", (uintptr_t)&AInputEvent_getType },
  { "AMotionEvent_getAction", (uintptr_t)&AMotionEvent_getAction },
  { "AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount },
  { "AMotionEvent_getPointerId", (uintptr_t)&AMotionEvent_getPointerId },
  { "AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX },
  { "AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY },
  { "AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode },
  { "AKeyEvent_getFlags", (uintptr_t)&AKeyEvent_getFlags },
  { "AKeyEvent_getRepeatCount", (uintptr_t)&AKeyEvent_getRepeatCount },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry },
  { "AConfiguration_new", (uintptr_t)&AConfiguration_new },
  { "AConfiguration_fromAssetManager", (uintptr_t)&AConfiguration_fromAssetManager },
  { "AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage },
  { "AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry },
  { "AConfiguration_delete", (uintptr_t)&AConfiguration_delete },
  { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor },
  { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor },
  { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate },
  { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents },

  // --- AAsset (data.c) ---
  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength },
  { "AAsset_close", (uintptr_t)&AAsset_close },

  // --- AndroidBitmap (text2bitmap.c) ---
  { "AndroidBitmap_getInfo", (uintptr_t)&AndroidBitmap_getInfo },
  { "AndroidBitmap_lockPixels", (uintptr_t)&AndroidBitmap_lockPixels },
  { "AndroidBitmap_unlockPixels", (uintptr_t)&AndroidBitmap_unlockPixels },

  // --- OpenSL ES (opensles.c) ---
  // FMOD's OpenSL output dlsym()s a fixed set of SL_IID_* at init and treats ANY
  // NULL as a fatal "Error loading file" (19) -- it aborts before even calling
  // slCreateEngine. (Observed: it asks for ENGINE, ANDROIDSIMPLEBUFFERQUEUE,
  // ANDROIDCONFIGURATION, PLAY, then RECORD -> NULL -> abort.) opensles.c already
  // DEF_IID()-defines all 49 as unique self-pointers, so export every one; the
  // exact request order then can't matter.
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  #define SL_IID(n) { "SL_IID_" #n, (uintptr_t)&SL_IID_##n }
  SL_IID(3DCOMMIT), SL_IID(3DDOPPLER), SL_IID(3DGROUPING), SL_IID(3DLOCATION),
  SL_IID(3DMACROSCOPIC), SL_IID(3DSOURCE), SL_IID(ANDROIDCONFIGURATION),
  SL_IID(ANDROIDEFFECT), SL_IID(ANDROIDEFFECTCAPABILITIES), SL_IID(ANDROIDEFFECTSEND),
  SL_IID(ANDROIDSIMPLEBUFFERQUEUE), SL_IID(AUDIODECODERCAPABILITIES), SL_IID(AUDIOENCODER),
  SL_IID(AUDIOENCODERCAPABILITIES), SL_IID(AUDIOIODEVICECAPABILITIES), SL_IID(BASSBOOST),
  SL_IID(BUFFERQUEUE), SL_IID(DEVICEVOLUME), SL_IID(DYNAMICINTERFACEMANAGEMENT),
  SL_IID(DYNAMICSOURCE), SL_IID(EFFECTSEND), SL_IID(ENGINE), SL_IID(ENGINECAPABILITIES),
  SL_IID(ENVIRONMENTALREVERB), SL_IID(EQUALIZER), SL_IID(LED), SL_IID(METADATAEXTRACTION),
  SL_IID(METADATATRAVERSAL), SL_IID(MIDIMESSAGE), SL_IID(MIDIMUTESOLO), SL_IID(MIDITEMPO),
  SL_IID(MIDITIME), SL_IID(MUTESOLO), SL_IID(NULL), SL_IID(OBJECT), SL_IID(OUTPUTMIX),
  SL_IID(PITCH), SL_IID(PLAY), SL_IID(PLAYBACKRATE), SL_IID(PREFETCHSTATUS),
  SL_IID(PRESETREVERB), SL_IID(RATEPITCH), SL_IID(RECORD), SL_IID(SEEK), SL_IID(THREADSYNC),
  SL_IID(VIBRA), SL_IID(VIRTUALIZER), SL_IID(VISUALIZATION), SL_IID(VOLUME),
  #undef SL_IID
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

// ---- extra libc imports needed by libFirebaseCpp* --------------------------
// ctype/wctype are macros in newlib, so wrap them to take addresses. inotify
// isn't supported on Switch -> report unavailable (Firebase falls back to
// polling / skips file watching).
#include <wctype.h>
#define FBX_CT(n)  static int    fbx_##n(int c){return n(c);}
#define FBX_WCT(n) static int    fbx_##n(wint_t c){return n(c);}
FBX_CT(isalpha) FBX_CT(islower) FBX_CT(toupper)
FBX_WCT(iswalpha) FBX_WCT(iswblank) FBX_WCT(iswcntrl) FBX_WCT(iswdigit)
FBX_WCT(iswlower) FBX_WCT(iswprint) FBX_WCT(iswpunct) FBX_WCT(iswspace)
FBX_WCT(iswupper) FBX_WCT(iswxdigit) FBX_WCT(towupper)
static int    fbx_strcoll(const char*a,const char*b){return strcoll(a,b);}
static char  *fbx_strpbrk(const char*a,const char*b){return strpbrk(a,b);}
static size_t fbx_strxfrm(char*a,const char*b,size_t n){return strxfrm(a,b,n);}
static int    fbx_wcscoll(const wchar_t*a,const wchar_t*b){return wcscoll(a,b);}
static size_t fbx_wcsxfrm(wchar_t*a,const wchar_t*b,size_t n){return wcsxfrm(a,b,n);}
static int    fbx_inotify_init(void){errno=ENOSYS;return -1;}
static int    fbx_inotify_add_watch(int fd,const char*p,uint32_t m){(void)fd;(void)p;(void)m;errno=ENOSYS;return -1;}
static mode_t fbx_umask_chk(mode_t m){(void)m;return 0;} // no umask on Switch
DynLibFunction firebase_extra_functions[] = {
  { "isalpha",(uintptr_t)&fbx_isalpha },{ "islower",(uintptr_t)&fbx_islower },
  { "toupper",(uintptr_t)&fbx_toupper },
  { "iswalpha",(uintptr_t)&fbx_iswalpha },{ "iswblank",(uintptr_t)&fbx_iswblank },
  { "iswcntrl",(uintptr_t)&fbx_iswcntrl },{ "iswdigit",(uintptr_t)&fbx_iswdigit },
  { "iswlower",(uintptr_t)&fbx_iswlower },{ "iswprint",(uintptr_t)&fbx_iswprint },
  { "iswpunct",(uintptr_t)&fbx_iswpunct },{ "iswspace",(uintptr_t)&fbx_iswspace },
  { "iswupper",(uintptr_t)&fbx_iswupper },{ "iswxdigit",(uintptr_t)&fbx_iswxdigit },
  { "towupper",(uintptr_t)&fbx_towupper },
  { "strcoll",(uintptr_t)&fbx_strcoll },{ "strpbrk",(uintptr_t)&fbx_strpbrk },
  { "strxfrm",(uintptr_t)&fbx_strxfrm },{ "wcscoll",(uintptr_t)&fbx_wcscoll },
  { "wcsxfrm",(uintptr_t)&fbx_wcsxfrm },
  { "inotify_init",(uintptr_t)&fbx_inotify_init },
  { "inotify_add_watch",(uintptr_t)&fbx_inotify_add_watch },
  { "__umask_chk",(uintptr_t)&fbx_umask_chk },
};
size_t firebase_extra_numfunctions = sizeof(firebase_extra_functions)/sizeof(*firebase_extra_functions);

void update_imports(void) { /* no runtime hook swaps needed */ }

/* Combined shim table (base + Unity supplementary), built once and shared by
 * both import resolution and dlsym(). */
extern DynLibFunction phigros_extra_functions[];  /* imports_phigros_extra.c  */
extern size_t phigros_extra_numfunctions;
extern DynLibFunction phigros_plugin_functions[]; /* imports_phigros_plugin.c */
extern size_t phigros_plugin_numfunctions;
static DynLibFunction *g_combined = NULL;
static int g_combined_n = 0;
static void build_combined(void) {
  if (g_combined) return;
  g_combined_n = (int)dynlib_numfunctions + unity_dynlib_numfunctions
               + (int)firebase_extra_numfunctions + (int)phigros_extra_numfunctions
               + (int)phigros_plugin_numfunctions;
  g_combined = malloc((size_t)g_combined_n * sizeof(DynLibFunction));
  size_t off = 0;
  memcpy(g_combined + off, dynlib_functions, dynlib_numfunctions * sizeof(DynLibFunction));
  off += dynlib_numfunctions;
  memcpy(g_combined + off, unity_dynlib_functions,
         (size_t)unity_dynlib_numfunctions * sizeof(DynLibFunction));
  off += unity_dynlib_numfunctions;
  memcpy(g_combined + off, firebase_extra_functions,
         firebase_extra_numfunctions * sizeof(DynLibFunction));
  off += firebase_extra_numfunctions;
  memcpy(g_combined + off, phigros_extra_functions,
         phigros_extra_numfunctions * sizeof(DynLibFunction));
  off += phigros_extra_numfunctions;
  memcpy(g_combined + off, phigros_plugin_functions,
         phigros_plugin_numfunctions * sizeof(DynLibFunction));
}

/* Search the shim table by name -- used by dlsym() for the GLES/EGL/libc
 * entry points the engine resolves dynamically (not module exports). */
uintptr_t dynlib_find_export(const char *name) {
  if (!name) return 0;
  build_combined();
  for (int i = 0; i < g_combined_n; i++)
    if (strcmp(name, g_combined[i].symbol) == 0)
      return g_combined[i].func;
  return 0;
}

void crx_resolve_imports(so_module *mod) {
  so_relocate(mod);
  /* Resolve against the cr3 base table plus the Unity supplementary table.
   * so_resolve poisons anything missing, so we must search both in a single
   * pass -- build a combined table once. */
  build_combined();
  so_resolve(mod, g_combined, g_combined_n, 1);
}
