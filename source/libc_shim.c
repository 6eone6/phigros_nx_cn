/* libc_shim.c -- bionic-compatible libc wrappers for libcrx.so + libc++_shared
 *
 * The Android engine and its C++ runtime are linked against bionic. Where the
 * bionic and newlib ABIs differ (struct layouts, flag values, missing
 * functions) we provide converting wrappers here; everything that matches is
 * passed straight through from imports.c. Online/IPC functionality (sockets,
 * fork/exec, dlopen of system libs) is dead on Switch and stubbed to fail
 * cleanly so the engine falls back to offline behaviour.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>  /* strncasecmp */
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include "config.h"
/* Both unconditional. asset_pack.c is always compiled and its query functions
 * answer no-pack harmlessly when the feature is off, so the DECLARATIONS must
 * always be visible -- several call sites below are not themselves gated. */
#include "asset_pack.h"
#include "nx_root.h"       /* runtime data root: works from any folder */
#include "phigros_jni.h"
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>
#include <EGL/egl.h>     /* eglGetProcAddress: resolve the full GLES API for dlsym */

#include "config.h"
#if PHI_ASSET_PACK
#include "asset_pack.h"
#endif
#include "util.h"
#include "error.h"
#include "imports.h"   /* dynlib_find_export (dlsym shim lookup) */
#include "so_util.h"
#include "libc_shim.h"
#include "android_native_unity.h"
#include "diag.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memcpy(dst, src, n); }
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) { (void)dstlen; return memmove(dst, src, n); }
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) { (void)dstlen; return memset(dst, c, n); }
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcat(dst, src); }
char *__strchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strchr(s, c); }
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return strcpy(dst, src); }
size_t __strlen_chk_fake(const char *s, size_t slen) { (void)slen; return strlen(s); }
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncat(dst, src, n); }
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return strncpy(dst, src, n); }
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen) { (void)dstlen; (void)srclen; return strncpy(dst, src, n); }
char *__strrchr_chk_fake(const char *s, int c, size_t slen) { (void)slen; return strrchr(s, c); }
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsnprintf(s, maxlen, fmt, va); }
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) { (void)flag; (void)slen; return vsprintf(s, fmt, va); }

int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsnprintf(s, maxlen, fmt, va);
  va_end(va);
  return r;
}
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va; va_start(va, fmt);
  int r = vsprintf(s, fmt, va);
  va_end(va);
  return r;
}

// fortified read helpers ignore the buffer-size guard
int   __open_2_fake(const char *path, int flags) { return open_fake(path, flags); }
long  __read_chk_fake(int fd, void *buf, size_t count, size_t buflen) { (void)buflen; return read(fd, buf, count); }
long  __pread_chk_fake(int fd, void *buf, size_t count, long off, size_t buflen) {
  (void)buflen;
  long cur = lseek(fd, 0, SEEK_CUR);
  if (cur < 0 || lseek(fd, off, SEEK_SET) < 0) return -1;
  long r = read(fd, buf, count);
  lseek(fd, cur, SEEK_SET);
  return r;
}
void  __FD_SET_chk_fake(int fd, void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) ((unsigned long *)set)[fd / (8 * sizeof(long))] |= (1ul << (fd % (8 * sizeof(long)))); }
int   __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) { (void)setlen; if (set && fd >= 0 && fd < 1024) return (((const unsigned long *)set)[fd / (8 * sizeof(long))] >> (fd % (8 * sizeof(long)))) & 1; return 0; }

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

// Native twin of the android.os.Build.* JNI fields. libunity calls this to
// detect API level / ABI / device; returning "" (the old stub) made it
// mis-detect the platform. Hand back Switch-sane values for the keys engines
// actually query; everything else stays empty (= property unset, the normal
// Android case). Return value is the value length, per bionic contract.
int __system_property_get_fake(const char *name, char *value) {
  if (!value) return 0;
  const char *v = "";
  if (name) {
    if      (!strcmp(name, "ro.build.version.sdk"))        v = "33";
    else if (!strcmp(name, "ro.build.version.release"))    v = "13";
    else if (!strcmp(name, "ro.build.version.codename"))   v = "REL";
    else if (!strcmp(name, "ro.product.cpu.abi"))          v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist"))      v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abilist64"))    v = "arm64-v8a";
    else if (!strcmp(name, "ro.product.cpu.abi2"))         v = "";
    else if (!strcmp(name, "ro.product.model"))            v = "Switch";
    else if (!strcmp(name, "ro.product.manufacturer"))     v = "Nintendo";
    else if (!strcmp(name, "ro.product.brand"))            v = "Nintendo";
    else if (!strcmp(name, "ro.product.name"))             v = "Switch";
    else if (!strcmp(name, "ro.product.device"))           v = "Switch";
    else if (!strcmp(name, "ro.product.board"))            v = "nx";
    else if (!strcmp(name, "ro.hardware"))                 v = "nx";
    else if (!strcmp(name, "ro.board.platform"))           v = "nx";
    else if (!strcmp(name, "ro.build.fingerprint"))        v = "Nintendo/Switch/Switch:13/REL/10007:user/release-keys";
    else if (!strcmp(name, "ro.build.characteristics"))    v = "default";
    else if (!strcmp(name, "ro.build.type"))               v = "user";
    else if (!strcmp(name, "ro.build.tags"))               v = "release-keys";
    else if (!strcmp(name, "ro.debuggable"))               v = "0";
    else if (!strcmp(name, "ro.secure"))                   v = "1";
    else if (!strcmp(name, "ro.kernel.qemu"))              v = "0";
    else if (!strcmp(name, "ro.opengles.version"))         v = "196610"; /* GLES 3.2 */
    else if (!strcmp(name, "dalvik.vm.heapsize"))          v = "512m";
    else if (!strcmp(name, "persist.sys.timezone"))        v = "UTC";
  }
  size_t n = strlen(v);
  if (n > 91) n = 91;            /* PROP_VALUE_MAX-1 */
  memcpy(value, v, n); value[n] = '\0';
  return (int)n;
}
unsigned long getauxval_fake(unsigned long type) { (void)type; return 0; }

int gettid_fake(void) {
  u64 tid = 1;
  if (R_SUCCEEDED(svcGetThreadId(&tid, CUR_THREAD_HANDLE)) && tid)
    return (int)(tid & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_GETTID            178
#define ARM64_SYS_FUTEX             98
#define ARM64_SYS_SCHED_SETAFFINITY 122
#define ARM64_SYS_PROCESS_VM_READV  270
#define ARM64_SYS_PROCESS_VM_WRITEV 271

// futex(2) emulation over libnx mutex+condvar. The il2cpp runtime synchronizes
// its GC, thread pool and locks with raw futex; returning ENOSYS made every
// waiter spin forever (the syscall(98) -> ENOSYS flood) and threading never made
// progress. Wait queues are hashed by uaddr into a bucket array; FUTEX_WAKE wakes
// the whole bucket (waiters re-check *uaddr, so over-broad wakes are harmless).
// The bucket mutex serializes compare-and-sleep against wakers so no wake is lost.
// Infinite waits are capped at 16ms and return as if woken: under load a wake can
// be missed (the Unity Job System / GC otherwise deadlock), and a bounded re-poll
// recovers it safely since the waiter re-checks *uaddr before proceeding.
#define FUTEX_WAIT        0
#define FUTEX_WAKE        1
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_CMD_MASK    0x7f  // strip FUTEX_PRIVATE_FLAG(128)/CLOCK_REALTIME(256)
#define FUTEX_CLOCK_REALTIME 256
/* round 71: futex outcome counters (reported in the 2s heartbeat). */
volatile unsigned long long g_fx_wait = 0, g_fx_eagain = 0, g_fx_slept = 0,
                            g_fx_wake = 0, g_fx_nosys = 0;
/* round 72: wall time the MAIN thread spends blocked in waits. */
volatile int g_main_tid = 0;
volatile unsigned long long g_mainwait_ns = 0, g_mainwait_n = 0;
volatile unsigned long long g_mainwait_cond_ns = 0, g_mainwait_cond_n = 0;
/* round 74: blocked time per thread id. Open-addressed, fixed size, no alloc. */
#define TWAIT_N 64
volatile int      g_twait_tid[TWAIT_N];
volatile unsigned long long g_twait_ns[TWAIT_N];
volatile unsigned long long g_twait_cnt[TWAIT_N];
void twait_add(int tid, unsigned long long ns) {
  unsigned h = ((unsigned)tid * 2654435761u) & (TWAIT_N - 1);
  for (unsigned i = 0; i < TWAIT_N; i++) {
    unsigned k = (h + i) & (TWAIT_N - 1);
    int cur = g_twait_tid[k];
    if (cur == tid) { __atomic_add_fetch(&g_twait_ns[k], ns, __ATOMIC_RELAXED);
                      __atomic_add_fetch(&g_twait_cnt[k], 1, __ATOMIC_RELAXED); return; }
    if (cur == 0) { int z = 0;
      if (__atomic_compare_exchange_n((int *)&g_twait_tid[k], &z, tid, 0,
                                      __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        __atomic_add_fetch(&g_twait_ns[k], ns, __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_twait_cnt[k], 1, __ATOMIC_RELAXED); return; }
      if (g_twait_tid[k] == tid) { __atomic_add_fetch(&g_twait_ns[k], ns, __ATOMIC_RELAXED);
                                   __atomic_add_fetch(&g_twait_cnt[k], 1, __ATOMIC_RELAXED); return; }
    }
  }
}
#define FUTEX_BUCKETS     256

static Mutex    futex_lock[FUTEX_BUCKETS];  // libnx Mutex/CondVar are u32; 0 == ready
static CondVar  futex_cond[FUTEX_BUCKETS];
static volatile uint32_t futex_gen[FUTEX_BUCKETS];  // wake generation: closes the
                                            // lost-wake window (see futex_impl)

#define FXOWN_N 64
static uintptr_t fxown_a[FXOWN_N];
static unsigned  fxown_c[FXOWN_N];
static Mutex     fxown_lk;
static int fxown_slot(uintptr_t a) {
  int victim = 0; unsigned lo = ~0u;
  for (int i = 0; i < FXOWN_N; i++) {
    if (fxown_a[i] == a) return i;
    if (fxown_c[i] < lo) { lo = fxown_c[i]; victim = i; }
  }
  fxown_a[victim] = a; fxown_c[victim] = 0; return victim;
}
static void fxown_spin(volatile int32_t *uaddr) {
  uintptr_t a = (uintptr_t)uaddr;
  if (a < 0x2400000000ULL) return;            /* skip job/loader region */
  mutexLock(&fxown_lk);
  int s = fxown_slot(a); unsigned c = ++fxown_c[s];
  mutexUnlock(&fxown_lk);
  if (c == 300)
    debugPrintf("[fxown] STUCK ua=%p tid=%d val=%08x (300 re-polls ~5s)\n", (void *)uaddr, gettid_fake(), (unsigned)*uaddr);
}
static void fxown_wake(volatile int32_t *uaddr) {
  uintptr_t a = (uintptr_t)uaddr;
  if (a < 0x2400000000ULL) return;
  unsigned c = 0;
  mutexLock(&fxown_lk);
  for (int i = 0; i < FXOWN_N; i++) if (fxown_a[i] == a) { c = fxown_c[i]; fxown_c[i] = 0; break; }
  mutexUnlock(&fxown_lk);
  if (c >= 300)
    debugPrintf("[fxown] WAKE ua=%p by tid=%d (was stuck %u re-polls)\n", (void *)uaddr, gettid_fake(), c);
}
#ifndef PHI_FUTEX_TRACE
#define PHI_FUTEX_TRACE 0
#endif
#if PHI_FUTEX_TRACE
/* Trace only the contended allocator region so the log stays legible. The
 * addresses that hang (0x..1cadb0, 0x..0004e4) are all above this line. */
static inline int fxtrace_addr(volatile int32_t *u) {
  return (uintptr_t)u >= 0x2400000000ULL;
}
#endif
static long futex_impl(volatile int32_t *uaddr, int op, int val, const struct timespec *to) {
  const int cmd = op & FUTEX_CMD_MASK;
  const unsigned h = (unsigned)(((uintptr_t)uaddr >> 4) & (FUTEX_BUCKETS - 1));
#if PHI_FUTEX_TRACE
  if (fxtrace_addr(uaddr) && (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET))
    debugPrintf("[fxt] WAIT enter ua=%p tid=%d val=%d *ua=%d\n",
                (void *)uaddr, gettid_fake(), val, (int)*uaddr);
  if (fxtrace_addr(uaddr) && (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET))
    debugPrintf("[fxt] WAKE      ua=%p tid=%d *ua=%d gen=%u\n",
                (void *)uaddr, gettid_fake(), (int)*uaddr, (unsigned)futex_gen[h]);
#endif
  if (cmd == FUTEX_WAIT || cmd == FUTEX_WAIT_BITSET) {
    /* If this is the collector and the world is stopped, the thread that would
     * wake it is paused. Release everyone before blocking. Defined with the rest
     * of the GC bridge further down; declared here because the futex path is the
     * one place the collector reliably blocks. */
    void gc_bail_if_collector_blocks(volatile int32_t *, int);
    long ret = 0;
    uint64_t __mw0 = armGetSystemTick();
    __atomic_add_fetch(&g_fx_wait, 1, __ATOMIC_RELAXED);
    mutexLock(&futex_lock[h]);
    if (*uaddr != val) {
      __atomic_add_fetch(&g_fx_eagain, 1, __ATOMIC_RELAXED);
      errno = EAGAIN; ret = -1;
    } else if (to) {
      /* PHIGROS_GC_REALWAIT_FIX_R1: Linux futex WAIT only blocks when
       * *uaddr still equals expected.  The old bridge resumed every
       * mutator BEFORE this check, turning harmless EAGAIN calls into
       * unsound mid-mark GC bailouts.  Bail only on a real sleep. */
      gc_bail_if_collector_blocks(uaddr, val);
      /* round 71: FUTEX_WAIT takes a RELATIVE timeout; FUTEX_WAIT_BITSET
       * takes an ABSOLUTE deadline. Treating the latter as relative made it
       * effectively infinite, so it could never time out. */
      u64 ns;
      if (cmd == FUTEX_WAIT_BITSET) {
        struct timespec now_ts;
        clock_gettime((op & FUTEX_CLOCK_REALTIME) ? CLOCK_REALTIME : CLOCK_MONOTONIC,
                      &now_ts);
        int64_t d = (int64_t)(to->tv_sec - now_ts.tv_sec) * 1000000000LL +
                    (int64_t)(to->tv_nsec - now_ts.tv_nsec);
        ns = (d > 0) ? (u64)d : 0ULL;
      } else {
      /* PHIGROS_GC_REALWAIT_FIX_R1: value matched under futex bucket lock. */
      gc_bail_if_collector_blocks(uaddr, val);
        ns = (u64)to->tv_sec * 1000000000ULL + (u64)to->tv_nsec;
      }
      __atomic_add_fetch(&g_fx_slept, 1, __ATOMIC_RELAXED);
      const u64 CAP = PHI_FUTEX_HOP_MAX;   /* round 70: re-poll ceiling (was 16ms) */
      if (ns > CAP) {
        /* Long timeout: sleep in 16ms hops until the value moves, a wake
         * bumps the generation, or the real deadline passes. Same structure
         * as the infinite path -- do NOT return after one hop, which spins
         * Baselib and contends futex_lock[h] against the waking thread. */
        u64 waited = 0;
        u64 hop = PHI_FUTEX_HOP_MIN;   /* round 63: adaptive re-poll */
        for (;;) {
          uint32_t g = futex_gen[h];
          diag_futex_spin(uaddr); fxown_spin(uaddr);
          u64 this_hop = hop;
          if (waited + this_hop > ns) this_hop = ns - waited;
          condvarWaitTimeout(&futex_cond[h], &futex_lock[h], this_hop);
          if (*uaddr != val) break;            /* satisfied */
          if (futex_gen[h] != g) break;        /* real wake raced us */
          waited += this_hop;
          if (waited >= ns) { errno = ETIMEDOUT; ret = -1; break; }
          if (hop < CAP) hop <<= 2;
        }
      } else if (R_FAILED(condvarWaitTimeout(&futex_cond[h], &futex_lock[h], ns))) {
        errno = ETIMEDOUT; ret = -1;
      }
    } else {
      __atomic_add_fetch(&g_fx_slept, 1, __ATOMIC_RELAXED);
      /* Infinite wait. Sample the generation, sleep in 16ms hops, and only
       * loop back if BOTH *uaddr still equals val AND no wake landed in the
       * gap. Previously this returned after one hop and relied on Baselib to
       * re-call -- which lost a wake that arrived between the value flip and
       * the sleep. */
      u64 hop = PHI_FUTEX_HOP_MIN;   /* round 63: adaptive re-poll */
      for (;;) {
        uint32_t g = futex_gen[h];
        diag_futex_spin(uaddr); fxown_spin(uaddr);
        condvarWaitTimeout(&futex_cond[h], &futex_lock[h], hop);
        if (*uaddr != val) break;           /* value changed -> done */
        if (futex_gen[h] != g) break;       /* a wake raced us -> re-check */
        if (hop < PHI_FUTEX_HOP_MAX) hop <<= 2;   /* round 70: up to the ceiling */
        if (hop > PHI_FUTEX_HOP_MAX) hop = PHI_FUTEX_HOP_MAX;
      }
    }
    mutexUnlock(&futex_lock[h]);
    { unsigned long long __d = armTicksToNs(armGetSystemTick() - __mw0);
      twait_add(gettid_fake(), __d); }
    if (g_main_tid && gettid_fake() == g_main_tid) {
      __atomic_add_fetch(&g_mainwait_ns,
                         armTicksToNs(armGetSystemTick() - __mw0), __ATOMIC_RELAXED);
      __atomic_add_fetch(&g_mainwait_n, 1, __ATOMIC_RELAXED);
    }
#if PHI_FUTEX_TRACE
    if (fxtrace_addr(uaddr))
      debugPrintf("[fxt] WAIT exit  ua=%p tid=%d ret=%ld *ua=%d\n",
                  (void *)uaddr, gettid_fake(), ret, (int)*uaddr);
#endif
    return ret;
  }
  if (cmd == FUTEX_WAKE || cmd == FUTEX_WAKE_BITSET) {
    __atomic_add_fetch(&g_fx_wake, 1, __ATOMIC_RELAXED);
    fxown_wake(uaddr);
    mutexLock(&futex_lock[h]);
    futex_gen[h]++;              /* record the wake so a waiter that has not
                                 * yet slept still observes it (lost-wake fix) */
    condvarWakeAll(&futex_cond[h]);
    mutexUnlock(&futex_lock[h]);
    return val > 0 ? val : 0; // approximate count woken
  }
  /* round 71: an unimplemented futex op silently loses a handoff. Count it,
   * and name the first few so they are not invisible. */
  __atomic_add_fetch(&g_fx_nosys, 1, __ATOMIC_RELAXED);
  { static int nlog = 0;
    if (nlog < 8) { nlog++;
      debugPrintf("[fx] UNIMPLEMENTED futex cmd=%d (op=0x%x) ua=%p\n",
                  cmd, (unsigned)op, (void *)uaddr); } }
  errno = ENOSYS;
  return -1;
}

/* newlib has no <sys/uio.h>; the kernel iovec layout is just {ptr, len}. */
struct nx_iovec { void *iov_base; size_t iov_len; };

/* Validate that [addr, addr+len) is mapped and readable via svcQueryMemory, so a
 * self process_vm_readv can copy safely instead of risking a fault. */
static int nx_addr_readable(uintptr_t addr, size_t len) {
  uintptr_t a = addr, end = addr + len;
  while (a < end) {
    MemoryInfo mi; u32 pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
    if (mi.type == 0) return 0;                 /* MemType_Unmapped */
    if ((mi.perm & Perm_R) == 0) return 0;      /* not readable */
    uintptr_t be = (uintptr_t)mi.addr + mi.size;
    if (be <= a) return 0;
    a = be;
  }
  return 1;
}

long syscall_fake(long number, ...) {
  switch (number) {
    case ARM64_SYS_GETTID: return gettid_fake();
    case ARM64_SYS_FUTEX: {
      va_list va; va_start(va, number);
      volatile int32_t *uaddr = va_arg(va, volatile int32_t *);
      const int op  = va_arg(va, int);
      const int val = va_arg(va, int);
      const struct timespec *to = va_arg(va, const struct timespec *);
      va_end(va);
      return futex_impl(uaddr, op, val, to);
    }
    case ARM64_SYS_SCHED_SETAFFINITY:
      return 0; // affinity hints are advisory; pretend success
    case ARM64_SYS_PROCESS_VM_READV:
    case ARM64_SYS_PROCESS_VM_WRITEV: {
      /* Self memory copy used as a fault-safe read/write probe. Stubbing it to
       * ENOSYS made the caller spin once per frame (a process_vm_readv flood),
       * wedging the boot path. Implement it for the own-process case: validate
       * each remote range with svcQueryMemory, then copy the readable parts. */
      va_list va; va_start(va, number);
      long pid                   = va_arg(va, long); (void)pid;
      const struct nx_iovec *liov   = va_arg(va, const struct nx_iovec *);
      unsigned long lcnt         = va_arg(va, unsigned long);
      const struct nx_iovec *riov   = va_arg(va, const struct nx_iovec *);
      unsigned long rcnt         = va_arg(va, unsigned long);
      va_end(va);
      int writing = (number == ARM64_SYS_PROCESS_VM_WRITEV);
      static int dbg = 0;
      if (dbg < 5) {
        dbg++;
        debugPrintf("[sys%ld] %s lcnt=%lu rcnt=%lu remote0=%p rlen0=%zu caller=%p\n",
                    number, writing ? "vm_writev" : "vm_readv", lcnt, rcnt,
                    rcnt ? riov[0].iov_base : NULL, rcnt ? riov[0].iov_len : 0,
                    __builtin_return_address(0));
      }
      ssize_t total = 0;
      unsigned long li = 0, ri = 0; size_t lo = 0, ro = 0;
      while (li < lcnt && ri < rcnt) {
        char *lp = (char *)liov[li].iov_base + lo;
        char *rp = (char *)riov[ri].iov_base + ro;
        size_t lrem = liov[li].iov_len - lo, rrem = riov[ri].iov_len - ro;
        size_t n = lrem < rrem ? lrem : rrem;
        char *probe = writing ? lp : rp;   /* the side being read-from must be readable */
        if (!nx_addr_readable((uintptr_t)probe, n)) {
          if (total == 0) { errno = EFAULT; return -1; }
          return total;
        }
        if (writing) memcpy(rp, lp, n); else memcpy(lp, rp, n);
        total += (ssize_t)n; lo += n; ro += n;
        if (lo == liov[li].iov_len) { li++; lo = 0; }
        if (ro == riov[ri].iov_len) { ri++; ro = 0; }
      }
      return total;
    }
  }
  debugPrintf("libc: syscall(%ld) -> ENOSYS\n", number);
  errno = ENOSYS;
  return -1;
}

void sincosf_fake(float x, float *s, float *c) { *s = sinf(x); *c = cosf(x); }
int sched_get_priority_max_fake(int policy) { (void)policy; return 0; }
int sched_get_priority_min_fake(int policy) { (void)policy; return 0; }
void android_set_abort_message_fake(const char *msg) { debugPrintf("abort message: %s\n", msg ? msg : "(null)"); }
size_t __ctype_get_mb_cur_max_fake(void) { return 1; }
int __register_atfork_fake(void) { return 0; }
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso) { (void)fn; (void)arg; (void)dso; return 0; }

#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE: return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN: return 3;
    // Report 512 MB (matches synthetic /proc/meminfo MemTotal) to make Unity's
    // DynamicHeap reserve fewer 256MB regions; real backing (arena/OC) holds more.
    case BIONIC_SC_PHYS_PAGES: return (512ll * 1024 * 1024) / 0x1000;
    default: return -1;
  }
}
long pathconf_fake(const char *path, int name) { (void)path; (void)name; return -1; }

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000

static int convert_open_flags(int flags) {
  int out = flags & 3;
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  return out;
}

// The engine addresses asset packs as "<packdir>/<file>.mvgl" but we ship the
// data flat in the game dir. If a read path with a subdirectory is missing,
// fall back to just its basename in the cwd (the game dir). Reads only -- never
// redirect a write -- and only when the basename actually exists.
static int basename_fallback(const char *path, char *out, size_t outsz) {
  const char *slash = strrchr(path, '/');
  if (!slash || !slash[1]) return 0;   // no subdir component to strip
  struct stat st;
  snprintf(out, outsz, "%s", slash + 1); // basename, resolved against the cwd
  return stat(out, &st) == 0;
}

// Create one directory, skipping paths newlib's mkdir() can't handle safely.
// A bare "device:" path (e.g. "sdmc:") makes newlib resolve to a device root
// with an empty in-device path and dereference a NULL devoptab -- a Data Abort
// reading devoptab->mkdir_r at +0x68. Refuse those (and null/empty).
static int safe_mkdir(const char *p) {
  if (!p || !*p) { errno = EINVAL; return -1; }
  const char *colon = strchr(p, ':');
  if (colon) {                       // has a "device:" prefix
    const char *in = colon + 1;      // the path inside the device
    while (*in == '/') in++;
    if (!*in) { errno = EEXIST; return 0; }  // "sdmc:" / "sdmc:/" -> root, skip
    // A single top-level component ("sdmc:/switch") also null-derefs newlib's
    // devoptab. Such dirs (the homebrew mount point) always pre-exist already.
    if (!strchr(in, '/')) { errno = EEXIST; return 0; }
  }
  return mkdir(p, 0777);
}

// mkdir -p: create `dir` and every missing parent. Save data lives in subdirs
// the engine only mkdir()s one level at a time, so a deeper missing parent left
// the whole chain (and the save write) failing.
//
// We must NOT try to create the game root or any ancestor of it ("sdmc:",
// "sdmc:/switch", "sdmc:/switch/zookeeper"): they already exist, they aren't
// ours, and newlib's mkdir() of a *top-level* path (one component under the
// device, e.g. "sdmc:/switch") null-derefs its devoptab -> Data Abort at the
// mkdir_r slot (+0x68). So begin the parent walk *after* GAME_HOME.
static void mkdir_p_dir(const char *dir) {
  if (!dir || !*dir) return;
  char tmp[512];
  if (snprintf(tmp, sizeof(tmp), "%s", dir) <= 0) return;
  size_t skip;
  const size_t glen = strlen(GAME_HOME);
  if (strncmp(tmp, GAME_HOME, glen) == 0 && (tmp[glen] == '/' || tmp[glen] == '\0')) {
    skip = glen;                                  // only create *under* the game root
  } else {
    const char *colon = strchr(tmp, ':');         // unknown base: at least skip "device:"
    skip = colon ? (size_t)(colon + 1 - tmp) : 0;
  }
  for (char *p = tmp + skip + 1; *p; p++)
    if (*p == '/') { *p = '\0'; safe_mkdir(tmp); *p = '/'; }
  if (tmp[skip]) safe_mkdir(tmp);
}
// create the parent directory chain of a file path
static void mkdir_parents(const char *filepath) {
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", filepath);
  char *last = strrchr(tmp, '/');
  if (!last || last == tmp) return;
  *last = '\0';
  mkdir_p_dir(tmp);
}

// mkdir wrapper: create the full chain and treat "already exists" as success
int rename_fake(const char *from, const char *to) {
  int r = rename(from ? from : "", to ? to : "");
  static int _rn = 0;
  if (_rn < 24) { _rn++;
    debugPrintf("[dir] rename %s -> %s : %d%s\n", from ? from : "(null)",
                to ? to : "(null)", r, r ? " *** FAILED ***" : ""); }
  return r;
}

int mkdir_fake(const char *path, unsigned mode) {
  (void)mode;
  if (!path || !*path) { errno = EINVAL; return -1; }
  mkdir_p_dir(path);
  int r = safe_mkdir(path);
  if (r != 0 && errno == EEXIST) r = 0;
  { static int _ml = 0;
    if (_ml < 32) { _ml++;
      debugPrintf("[dir] mkdir %s -> %d%s\n", path, r,
                  r ? " *** FAILED ***" : ""); } }
  return r;
}

int g_watch_fd = -1;   /* globalgamemanagers fd: trace its reads/seeks to debug header load */
void watch_dump(const char *tag, int fd, long a, long b, const void *buf, long got) {
  if (fd != g_watch_fd) return;
  char h[64]; int n = (got > 16 ? 16 : (got < 0 ? 0 : (int)got));
  int p = 0; for (int i = 0; i < n; i++) p += snprintf(h + p, sizeof(h) - p, "%02x ", ((const unsigned char *)buf)[i]);
  h[p] = 0;
  debugPrintf("[io] %s fd=%d a=%ld b=%ld -> %ld  [%s]\n", tag, fd, a, b, got, h);
}

/* lseek for arm64: off_t is already 64-bit, so this also services lseek64.
 * lseek64 was previously stubbed to return 0 (no seek) -- that made libunity's
 * archive reader see data.unity3d as empty/mis-positioned ("Unable to read
 * header from archive file"), since it lseek64(SEEK_END)s to size the file. */
long z_lseek(int fd, long off, int whence) {
#if PHI_ASSET_PACK
  if (asset_pack_fd_is(fd)) return asset_pack_lseek_fd(fd, off, whence);
#endif
  long r = lseek(fd, off, whence);
  if (fd == g_watch_fd) debugPrintf("[io] lseek fd=%d off=%ld whence=%d -> %ld\n", fd, off, whence, r);
  return r;
}

static const char *synthetic_proc(const char *path);  /* defined below */

// Serve /proc and /sys reads that arrive through raw open() (e.g.
// /proc/self/maps, which the engine opens to enumerate memory mappings).
// newlib's open() can't be memory-backed, so materialize the synthetic content
// into a small file under the game dir and hand back a real fd. Returns an fd,
// or -1 if `path` isn't a node we synthesize (caller proceeds normally).
static int synth_proc_open(const char *path) {
  if (!path) return -1;
  if (strncmp(path, "/proc/", 6) && strncmp(path, "/sys/", 5)) return -1;
  static char buf[16384];
  int len;
  if (!strcmp(path, "/proc/self/maps") || !strcmp(path, "/proc/self/smaps")) {
    len = so_dump_maps(buf, sizeof buf);
  } else {
    const char *s = synthetic_proc(path);
    if (!s) return -1;                                   // not /proc or /sys
    len = (int)strlen(s);
    if (len > (int)sizeof buf) len = (int)sizeof buf;
    memcpy(buf, s, (size_t)len);
  }
  char safe[160]; size_t j = 0;
  for (const char *p = path; *p && j < sizeof safe - 1; p++) safe[j++] = (*p == '/') ? '_' : *p;
  safe[j] = '\0';
  char tf[256];
  snprintf(tf, sizeof tf, "%s/.synth%s", GAME_HOME, safe);
  int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (wfd >= 0) { if (write(wfd, buf, (size_t)len) < 0) { /* best effort */ } close(wfd); }
  return open(tf, O_RDONLY);
}

/* Unity probes filesystem case-sensitivity on every boot: it creates
 * CASESENSITIVETEST<guid> (O_CREAT|O_EXCL) in the data root, re-opens it under
 * a different case, and never cleans up -- a fresh GUID name each launch, so
 * junk accumulates on the SD card. Redirect every such name (any case, any
 * guid) onto ONE fixed hidden scratch file. Different-case probes then hit the
 * same file, which is precisely the case-INSENSITIVE answer FAT gives anyway,
 * and main.c sweeps the scratch (plus any strays from older builds) at boot. */
static const char *casetest_redirect(const char *path) {
  const char *b = strrchr(path, '/');
  b = b ? b + 1 : path;
  if (strncasecmp(b, "CASESENSITIVETEST", 17) == 0)
    return nx_casetest_path();
  return path;
}

void rc_mark(int fd, int on);                    /* asset read cache (defined below) */
long rc_pread_pub(int fd, void *buf, size_t count, unsigned long long off);
/* round 59: remember the path behind each fd so the mmap/truncation log can
 * name the file, and so we can see exactly which scene/asset files load. */
#define FDPATH_N 256
static char  g_fdpath[FDPATH_N][160];
static Mutex g_fdpath_lk;
static void fdpath_set(int fd, const char *p) {
  if (fd < 0 || fd >= FDPATH_N || !p) return;
  mutexLock(&g_fdpath_lk);
  size_t n = strlen(p); const char *b = p;
  if (n >= sizeof(g_fdpath[0])) b = p + (n - (sizeof(g_fdpath[0]) - 1));
  strncpy(g_fdpath[fd], b, sizeof(g_fdpath[0]) - 1);
  g_fdpath[fd][sizeof(g_fdpath[0]) - 1] = 0;
  mutexUnlock(&g_fdpath_lk);
}
const char *fdpath_get(int fd) {
  if (fd < 0 || fd >= FDPATH_N) return "?";
  return g_fdpath[fd][0] ? g_fdpath[fd] : "?";
}
__attribute__((unused))   /* only used under PHI_TRACE_DATA_IO */
static int path_is_gamedata(const char *p) {
  return p && (strstr(p, "bin/Data") || strstr(p, "level") ||
               strstr(p, "assets") || strstr(p, "metadata") ||
               strstr(p, "sharedassets") || strstr(p, "unity3d"));
}
/* Reduce any spelling of a game path to the pack key "assets/...".
 * Three forms reach us and they must all work:
 *   sdmc:/switch/fruitninja/assets/...   (DATA_ROOT, what the loader builds)
 *   /switch/fruitninja/assets/...        (no device prefix -- what Unity sends)
 *   assets/...                           (relative to cwd)
 * Round 84 only handled the first, so il2cpp's open of
 * /switch/fruitninja/assets/bin/Data/Managed missed the pack and returned
 * -1, which is what triggered the fatal-error dialog. Returns NULL when the
 * path is not a packed game path. */
const char *nx_pack_relpath(const char *path) {
  if (!path) return NULL;
  const char *p = path;

  /* Addressables RuntimePath forms. The Unity Android player builds
   * streamingAssetsPath as jar:file://<apk>!/assets, and Addressables roots
   * every m_InternalId at that. There is no APK here -- getPackageCodePath()
   * returns a DIRECTORY -- so whichever of these two shapes libunity ends up
   * producing, normalise it down to the plain relative form.
   *
   * Measured: catalog.json contains no literal `jar:` or `file://` (all 2514
   * ids use the RuntimePath token), so this is defensive rather than known-
   * required. It costs two strstr on a path that is about to hit the SD card
   * anyway, and the failure it prevents -- Addressables silently resolving to
   * nothing and the boot hanging in a loading screen -- is expensive to
   * diagnose from a black screen. */
  { const char *bang = strstr(p, "!/");
    if (bang) p = bang + 2;                      /* jar-style ...!/assets/...  */
    else if (!strncmp(p, "file:", 5) && p[5] == '/' && p[6] == '/') p += 7; }
  /* NOTE: the scheme prefix is matched with two char compares rather than a
   * "file:" + slash-slash string literal on purpose. tools/buildcheck.py strips
   * block comments first and then line comments, without understanding string
   * literals -- so a slash-slash INSIDE a literal truncates the rest of the
   * line and leaves an unterminated quote, desyncing every check after it in
   * the file. Costs nothing here; saves a confusing false positive. */

  if (!strncmp(p, "sdmc:", 5)) p += 5;          /* libnx device prefix */

  /* Strip the data root in whichever form the caller used. The root is now
   * resolved at runtime (nx_root.c), so this is a comparison against a string
   * rather than a compile-time prefix -- the loader works from any folder
   * under /switch and Unity may hand us either the device-prefixed or the
   * device-stripped spelling of the same path. */
  {
    const char *root = nx_root();
    const char *rnd  = nx_root_nodev();
    const size_t rl = strlen(root), nl = strlen(rnd);
    if (rl && !strncmp(p, root, rl) && p[rl] == '/')      p += rl + 1;
    else if (nl && !strncmp(p, rnd, nl) && p[nl] == '/')  p += nl + 1;
  }
  /* Unity hands us the ABSOLUTE form "/assets/aa/..." as well as the relative
   * "assets/aa/...".
   *
   * Its Addressables path for a bundle is
   *     jar:file://<apk>!/assets/aa/Android/<hash>.bundle
   * and there is no APK here, so <apk> is empty and Unity's own jar-URL
   * handling reduces it to a leading-slash path. Requiring "assets/" with no
   * slash meant every one of the 2514 bundles fell through to a real open() of
   * a path that does not exist:
   *     [io] open(/assets/aa/Android/78100dbe....bundle,0x0) -> -1
   * which Addressables then reported as "Unable to open archive file" and a
   * RemoteProviderException -- so the game sat forever on its loading screen
   * with the render loop still ticking. Skipping the leading slash maps them
   * into the pack. There is no real "/assets" at the SD root to shadow. */
  while (*p == '/') p++;
  return !strncmp(p, "assets/", 7) ? p : NULL;
}

int open_fake(const char *path, int flags, ...) {
  path = casetest_redirect(path);
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  const int cvt = convert_open_flags(flags);
  const int writing = (flags & 3) != 0 || (flags & LINUX_O_CREAT);
  if (!writing) {
    // /dev/urandom + /dev/random: Switch has no /dev node, but Mono/.NET (RNG
    // seeds, Guid.NewGuid, hashtable randomization) and asset crypto open these.
    // A failing open (-1) leaves those paths without entropy and can stall the
    // scene/asset load. Materialize a buffer of real CSPRNG bytes (libnx
    // randomGet) into a file and hand back a real fd so read() just works.
    if (!strcmp(path, "/dev/urandom") || !strcmp(path, "/dev/random")) {
      static char rbuf[65536];
      randomGet(rbuf, sizeof rbuf);
      char tf[256];
      snprintf(tf, sizeof tf, "%s/.synth_dev_random", GAME_HOME);
      int wfd = open(tf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (wfd >= 0) { if (write(wfd, rbuf, sizeof rbuf) < 0) { /* best effort */ } close(wfd); }
      int rfd = open(tf, O_RDONLY);
      debugPrintf("[io] open(%s,0x%x) -> %d [urandom]\n", path, flags, rfd);
      return rfd;
    }
    // synthetic /proc, /sys (incl. self/maps)
    int sfd = synth_proc_open(path);
    if (sfd >= 0) { debugPrintf("[io] open(%s,0x%x) -> %d [synthetic]\n", path, flags, sfd); return sfd; }
  }
#if PHI_ASSET_PACK
  /* round 79: serve read-only game files from the pack when mounted. */
  if (!writing && asset_pack_active()) {
    const char *rel = nx_pack_relpath(path);
    if (rel) {
      int pfd = asset_pack_open_path(rel);
      if (pfd >= 0) { fdpath_set(pfd, path); return pfd; }
    }
  }
#endif
  u64 __open_t0 = armGetSystemTick();
  int fd = open(path, cvt, mode);
  unsigned long long __open_us = armTicksToNs(armGetSystemTick() - __open_t0) / 1000ull;
  if (fd < 0 && writing) {
    // save files: the target subdir may not exist yet -- create it and retry
    mkdir_parents(path);
    fd = open(path, cvt, mode);
  }
  if (fd < 0 && (flags & 3) == 0 && !(flags & LINUX_O_CREAT)) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      fd = open(alt, cvt, mode);
  }
  if (fd >= 0) {
    rc_mark(fd, ((flags & 3) == 0));      /* cache read-only files only */
    fdpath_set(fd, path);
#if PHI_TRACE_DATA_IO
    if (path_is_gamedata(path)) {
      struct stat _ds;
      long long _sz = (fstat(fd, &_ds) == 0) ? (long long)_ds.st_size : -1;
      debugPrintf("[io] DATA open %s -> fd=%d size=%lld open_us=%llu\n",
                  path, fd, _sz, __open_us);
    }
#else
    (void)__open_us;
#endif
    struct stat _st;
#if LOG_VERBOSE
    if (fstat(fd, &_st) == 0)
      debugPrintf("[io] open(%s,0x%x) -> %d size=%lld\n", path, flags, fd, (long long)_st.st_size);
    else
      debugPrintf("[io] open(%s,0x%x) -> %d size=?\n", path, flags, fd);
#else
    (void)_st;
#endif
#if TRACE_BUNDLE_IO
    if (strstr(path, "globalgamemanagers")) { g_watch_fd = fd; debugPrintf("[io] >>> watching fd=%d (globalgamemanagers)\n", fd); }
#endif
  } else {
    debugPrintf("[io] open(%s,0x%x) -> %d\n", path, flags, fd);
  }
  return fd;
}
int openat_fake(int dirfd, const char *path, int flags, ...) {
  (void)dirfd;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) { va_list va; va_start(va, flags); mode = va_arg(va, int); va_end(va); }
  // Delegate to open_fake so /dev/urandom, synthetic /proc + /sys, save-dir
  // creation and basename fallback all apply (some libc paths route open->openat).
  return open_fake(path, flags, mode);
}
int unlinkat_fake(int dirfd, const char *path, int flags) { (void)dirfd; (void)flags; return unlink(path); }

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct bionic_stat {
  uint64_t st_dev; uint64_t st_ino; uint32_t st_mode; uint32_t st_nlink;
  uint32_t st_uid; uint32_t st_gid; uint64_t st_rdev; uint64_t __pad1;
  int64_t st_size; int32_t st_blksize; int32_t __pad2; int64_t st_blocks;
  struct bionic_timespec st_atim; struct bionic_timespec st_mtim; struct bionic_timespec st_ctim;
  uint32_t __unused4; uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev; out->st_ino = in->st_ino; out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink; out->st_uid = in->st_uid; out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev; out->st_size = in->st_size; out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime; out->st_mtim.tv_sec = in->st_mtime; out->st_ctim.tv_sec = in->st_ctime;
}

int stat_fake(const char *path, struct bionic_stat *st) {
#if PHI_ASSET_PACK
  if (asset_pack_active()) {
    /* Same normaliser as open/opendir/fopen. This function had the round-84
     * key bug AND the round-86 prefix bug, so every stat() on a game asset
     * failed -- which is what stopped Unity's extraction walk. */
    const char *rel = nx_pack_relpath(path);
    if (rel) {
      uint64_t sz = 0, ino = 0; int isdir = 0;
      if (asset_pack_stat_path_info(rel, &sz, &ino, &isdir)) {
        if (st) { memset(st, 0, sizeof *st);
          st->st_size = (long long)sz; st->st_ino = ino;
          st->st_mode = isdir ? 0040755 : 0100644;
          st->st_nlink = 1; st->st_blksize = 4096; }
        return 0;
      }
    }
  }
#endif
  path = casetest_redirect(path);
  struct stat real; int r = stat(path, &real);
  if (r != 0) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt))) r = stat(alt, &real);
  }
  if (r == 0) convert_stat(&real, st);
  if (r != 0) {
    static int _sl = 0;
    if (_sl < 24) { _sl++;
      debugPrintf("[io] stat FAILED %s (errno=%d)\n", path ? path : "(null)", errno); }
  }
  return r;
}
int fstat_fake(int fd, struct bionic_stat *st) {
#if PHI_ASSET_PACK
  if (asset_pack_fd_is(fd)) {
    uint64_t sz = 0, ino = 0; int isdir = 0;
    if (asset_pack_fstat_fd(fd, &sz, &ino, &isdir) && st) {
      memset(st, 0, sizeof *st);
      st->st_size = (long long)sz; st->st_ino = ino;
      st->st_mode = isdir ? 0040755 : 0100644;
      st->st_nlink = 1; st->st_blksize = 4096;
      st->st_blocks = (long long)((sz + 511) / 512);
      return 0;
    }
  }
#endif
  struct stat real; const int r = fstat(fd, &real);
  if (r == 0) convert_stat(&real, st);
  if (r != 0) {
    static int _fl = 0;
    if (_fl < 24) { _fl++;
      debugPrintf("[io] fstat FAILED fd=%d (%s) errno=%d\n",
                  fd, fdpath_get(fd), errno); }
  }
  return r;
}
int lstat_fake(const char *path, struct bionic_stat *st) { return stat_fake(path, st); }

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino; int64_t d_off; uint16_t d_reclen; uint8_t d_type; char d_name[256];
};

volatile unsigned long long g_readdir_calls = 0, g_readdir_ns = 0, g_opendir_calls = 0;
void *opendir_fake(const char *path) {
  __atomic_add_fetch(&g_opendir_calls, 1, __ATOMIC_RELAXED);
  static int _ol = 0; if (_ol < 24) { _ol++; debugPrintf("[dir] opendir %s\n", path ? path : "(null)"); }
  const char *prel = nx_pack_relpath(path);   /* packed dirs first */
  void *pd = prel ? asset_pack_opendir_path(prel) : NULL;
  if (pd) { if (_ol <= 24) debugPrintf("[dir]   -> pack ok\n"); return pd; }
  void *rd = opendir(path);
  if (_ol <= 24) debugPrintf("[dir]   -> %s (disk)\n", rd ? "ok" : "FAILED");
  return rd;
}
int closedir_fake(void *dirp) {
  if (asset_pack_dir_is(dirp)) return asset_pack_closedir_path(dirp);
  return closedir((DIR *)dirp);
}
void *readdir_fake(void *dirp) {
  static struct bionic_dirent out; // not thread-safe (matches bionic readdir)
  if (asset_pack_dir_is(dirp)) {              /* packed directory iterator */
    __atomic_add_fetch(&g_readdir_calls, 1, __ATOMIC_RELAXED);
    memset(&out, 0, sizeof(out));
    const char *nm = asset_pack_readdir_path(dirp, &out.d_type, &out.d_ino);
    if (!nm) return NULL;
    out.d_reclen = sizeof(out);
    snprintf(out.d_name, sizeof(out.d_name), "%s", nm);
    { static int _rl = 0;
      if (_rl < 48) { _rl++;
        debugPrintf("[dir]   entry type=%u %s\n", out.d_type, out.d_name); } }
    return &out;
  }
  uint64_t __rt = armGetSystemTick();
  struct dirent *e = readdir((DIR *)dirp);
  __atomic_add_fetch(&g_readdir_ns, armTicksToNs(armGetSystemTick() - __rt), __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_readdir_calls, 1, __ATOMIC_RELAXED);
  if (!e) return NULL;
  memset(&out, 0, sizeof(out));
  out.d_ino = e->d_ino;
  out.d_reclen = sizeof(out);
  out.d_type = e->d_type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->d_name);
  return &out;
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C-locale versions
// ---------------------------------------------------------------------------

void *newlocale_fake(int mask, const char *locale, void *base) { (void)mask; (void)locale; (void)base; return (void *)1; }
void freelocale_fake(void *loc) { (void)loc; }
void *uselocale_fake(void *loc) { (void)loc; return (void *)1; }

#define WRAP_ISW_L(fn) int fn##_l_fake(int wc, void *loc) { (void)loc; return fn(wc); }
WRAP_ISW_L(iswalpha) WRAP_ISW_L(iswblank) WRAP_ISW_L(iswcntrl) WRAP_ISW_L(iswdigit)
WRAP_ISW_L(iswlower) WRAP_ISW_L(iswprint) WRAP_ISW_L(iswpunct) WRAP_ISW_L(iswspace)
WRAP_ISW_L(iswupper) WRAP_ISW_L(iswxdigit) WRAP_ISW_L(towlower) WRAP_ISW_L(towupper)

int strcoll_l_fake(const char *a, const char *b, void *loc) { (void)loc; return strcoll(a, b); }
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc) { (void)loc; return strxfrm(dst, src, n); }
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc) { (void)loc; return strftime(s, max, fmt, (const struct tm *)tm); }
long double strtold_l_fake(const char *s, char **end, void *loc) { (void)loc; return strtold(s, end); }
long long strtoll_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoll(s, end, base); }
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) { (void)loc; return strtoull(s, end, base); }
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc) { (void)loc; return wcscoll(a, b); }
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc) { (void)loc; return wcsxfrm(dst, src, n); }

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) { if (dst) dst[i] = (unsigned char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0; const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) { if (dst) dst[i] = (char)s[i]; i++; }
  if (dst && i < len) { dst[i] = 0; *src = NULL; }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p) return ENOMEM;
  *out = p;
  return 0;
}

// --- anonymous mmap arena (page-granular; supports sub-range munmap) ----------
//
// Switch has no mmap. Unity reserves big *256MB-aligned* pools by over-mmapping a
// larger region then munmapping the unaligned head/tail to keep an aligned middle.
// A plain malloc/free-per-mmap frees the WHOLE block when the head is trimmed (the
// trim's addr == the registered base) and the kept aligned middle is then reused
// out from under the engine -> the TLSF allocator's free block reads back zeroed
// (next_free == NULL) and faults. So we manage a dedicated arena (carved 256MB-
// aligned in __libnx_initheap) with a per-page used-bitmap: mmap = find a free run
// of pages and mark them; munmap = clear exactly the pages of the sub-range. Big
// requests are handed back 256MB-aligned so Unity only ever trims the tail.
// File-backed maps (Unity streams BGM/SE this way) are served from the same arena.
// ------------------------------------------------------------------------------
extern void  *g_mmap_arena_base;   // set by __libnx_initheap (main.c)
extern size_t g_mmap_arena_size;
extern int    g_overcommit;        // 1 = alias-region on-demand commit
extern u64    g_alias_base, g_alias_size;

#define BIONIC_MAP_SHARED    0x01
#define BIONIC_MAP_ANONYMOUS 0x20
#define BIONIC_MAP_FIXED     0x10
#define MMAP_PAGE       0x1000u
#define MMAP_BIG_ALIGN  MMAP_ARENA_ALIGN
#define MMAP_BIG_THRESH ((size_t)64 * 1024 * 1024)
#define BIONIC_PROT_NONE 0x0
#define BIONIC_PROT_WRITE 0x2
#define BIONIC_MADV_DONTNEED 4

static uint8_t *mmap_arena;    // 256MB-aligned usable base (published last)
static size_t   mmap_usable;   // usable bytes
static size_t   mmap_pages;    // usable / page
static uint8_t *mmap_used;     // 1 byte/page bitmap: reserved (address space)
/* Pages per allocation, recorded on the FIRST page of each run (0 elsewhere).
 *
 * Without this the arena had no idea where one allocation ended and the next
 * began: mmap_arena_free() cleared ceil(len/PAGE) bits from wherever the caller
 * pointed, using the CALLER's length. A munmap longer than we actually reserved
 * -- which the big-allocation pass 2 deliberately creates, since it truncates to
 * what fits and hands back a smaller *got -- clears bits belonging to the NEXT
 * allocation. Those pages are then handed out again while still live, and the
 * MAP_ANONYMOUS memset() zeroes real objects.
 *
 * A managed object whose header reads 0 is exactly what the ModeSelect crash has
 * been every time: GC mark loop, klass == 0, object inside this arena. */
static uint32_t *mmap_runlen;

/* Ring of recent arena alloc/free events, dumped by the crash handler for the
 * faulting page. "RESERVED" only tells us a page is allocated NOW -- it cannot
 * distinguish "corrupted under its owner" from "freed and legitimately handed to
 * somebody else, who zeroed it". Those need opposite fixes, and the history is
 * the only thing that separates them. */
#define ARENA_EV_N 256
typedef struct { uint32_t page, pages, seq; uint8_t op; } ArenaEv;   /* op: 1=alloc 2=free */
static ArenaEv  g_arena_ev[ARENA_EV_N];
static uint32_t g_arena_ev_i, g_arena_ev_seq;

static void arena_ev(uint8_t op, size_t page, size_t pages) {
  ArenaEv *e = &g_arena_ev[g_arena_ev_i++ & (ARENA_EV_N - 1)];
  e->op = op; e->page = (uint32_t)page; e->pages = (uint32_t)pages;
  e->seq = ++g_arena_ev_seq;
}

/* Who asked for a big arena block?
 *
 * The faulting pointer always lands in one specific 869-page (3.39 MB) run,
 * allocated once at sequence 20 and never freed -- so it is neither corrupted
 * nor recycled. What it IS remains unknown, and that is now the missing piece:
 * the object there has a "klass" whose flags are 0 and whose typeHierarchy is
 * garbage, i.e. it is not a managed object at all and something is passing it to
 * il2cpp's type check.
 *
 * Record the caller so the next log names the subsystem that owns the region. */
static void arena_big_alloc_note(size_t pages, const void *addr, const void *caller) {
  if (pages < 256) return;                       /* >= 1 MB only */
  static unsigned n = 0;
  if (n >= 16) return;
  n++;
  debugPrintf("[mmap] arena BIG alloc: %zu pages (%zu KB) at %p seq %u  caller=%p\n",
              pages, (pages * MMAP_PAGE) >> 10, addr, g_arena_ev_seq, caller);
}

/* Print every recorded event whose range covers `p`. Caller: the crash dumper. */
void nx_arena_history(const void *p) {
  if (!mmap_arena || !mmap_used) return;
  const uint8_t *a = (const uint8_t *)p;
  if (a < mmap_arena || (size_t)(a - mmap_arena) >= mmap_usable) return;
  const size_t page = (size_t)(a - mmap_arena) / MMAP_PAGE;
  debugPrintf("[xd] arena history for page %zu (+%u KB):\n",
              page, (unsigned)(((size_t)(a - mmap_arena)) >> 10));
  int shown = 0;
  for (unsigned k = 0; k < ARENA_EV_N; k++) {
    const ArenaEv *e = &g_arena_ev[(g_arena_ev_i + k) & (ARENA_EV_N - 1)];
    if (!e->seq) continue;
    if (page < e->page || page >= (size_t)e->page + e->pages) continue;
    debugPrintf("[xd]   seq %-6u %-5s page %-7u x %u pages\n",
                e->seq, e->op == 1 ? "ALLOC" : "FREE", e->page, e->pages);
    shown++;
  }
  if (!shown) debugPrintf("[xd]   (no events -- allocated before the ring wrapped)\n");
  else debugPrintf("[xd]   >1 ALLOC above means the range was REUSED: the reference is "
                   "stale, not corrupted\n");
}
static uint8_t *mmap_committed;// 1 byte/page bitmap: physically committed (overcommit only)
static size_t   g_committed_pages;   // running count of committed pages
static size_t   g_commit_peak;       // high-water mark (pages)

/* Reserved (address space) and committed (physical) arena pages, in MB, for
 * the heartbeat. Cheap: a scan of the reserve bitmap, called once per 2s. */
/* Describe an address relative to the arena, for the crash dumper.
 *
 * The ModeSelect fault is always a reference into this arena pointing at memory
 * that reads as all zeros. Two very different causes, needing opposite fixes:
 *
 *   reserved -> the page IS a live allocation, so something zeroed it under the
 *               owner: an allocator bug on our side
 *   FREE     -> the page was never handed out, so the reference itself is
 *               garbage: nothing wrote zeros, it simply was never anything
 *
 * Guessing between them has now cost several rounds. Returns 0 if not ours. */
int nx_arena_describe(const void *p, unsigned *page_off_kb, int *reserved,
                      unsigned *run_pages) {
  if (!mmap_arena || !mmap_used) return 0;
  const uint8_t *a = (const uint8_t *)p;
  if (a < mmap_arena || (size_t)(a - mmap_arena) >= mmap_usable) return 0;
  const size_t off  = (size_t)(a - mmap_arena);
  const size_t page = off / MMAP_PAGE;
  if (page_off_kb) *page_off_kb = (unsigned)(off >> 10);
  if (reserved)    *reserved    = mmap_used[page] ? 1 : 0;
  /* walk back to the start of the run this page belongs to */
  if (run_pages) {
    *run_pages = 0;
    for (size_t i = page + 1; i-- > 0; ) {
      if (mmap_runlen[i]) { *run_pages = mmap_runlen[i]; break; }
      if (!mmap_used[i]) break;                 /* hole: not inside a run */
    }
  }
  return 1;
}

void nx_arena_usage_mb(unsigned *reserved_mb, unsigned *committed_mb,
                       unsigned *total_mb) {
  size_t used = 0;
  if (mmap_used) for (size_t i = 0; i < mmap_pages; i++) if (mmap_used[i]) used++;
  if (reserved_mb)  *reserved_mb  = (unsigned)((used * MMAP_PAGE) >> 20);
  if (committed_mb) *committed_mb = (unsigned)((g_committed_pages * MMAP_PAGE) >> 20);
  if (total_mb)     *total_mb     = (unsigned)((mmap_pages * MMAP_PAGE) >> 20);
}
static Mutex    g_mmap_lock;   // zero-init == valid unlocked libnx mutex

// --- overcommit commit/decommit (caller holds g_mmap_lock) -------------------
// svcMapPhysicalMemory zero-fills and draws from the freed physical limit; it
// FAILS on already-mapped pages, so we only ever commit pages we track as
// uncommitted, in contiguous runs. Out-of-physical is logged, not fatal.
static void arena_commit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    Result rc = svcMapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE);
    if (R_SUCCEEDED(rc)) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 1;
      g_committed_pages += run;
      if (g_committed_pages > g_commit_peak) {
        size_t prev = g_commit_peak;
        g_commit_peak = g_committed_pages;
        if ((g_commit_peak >> 16) != (prev >> 16))   // new 256MB high-water mark
          debugPrintf("[mmap] committed peak %u MB (live %u MB)\n",
                      (unsigned)((g_commit_peak * MMAP_PAGE) >> 20),
                      (unsigned)((g_committed_pages * MMAP_PAGE) >> 20));
      }
    } else {
      debugPrintf("[mmap] COMMIT FAIL %u KB @ 0x%lx rc=0x%x (committed %u MB peak %u MB)\n",
                  (unsigned)((run * MMAP_PAGE) >> 10), (unsigned long)a, rc,
                  (unsigned)((g_committed_pages * MMAP_PAGE) >> 20),
                  (unsigned)((g_commit_peak * MMAP_PAGE) >> 20));
    }
    i += run ? run : 1;
  }
}

static void arena_decommit_locked(size_t first, size_t cnt) {
  size_t i = 0;
  while (i < cnt) {
    if (!mmap_committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && mmap_committed[first + i + run]) run++;
    u64 a = (u64)(uintptr_t)(mmap_arena + (first + i) * MMAP_PAGE);
    if (R_SUCCEEDED(svcUnmapPhysicalMemory((void *)a, (u64)run * MMAP_PAGE))) {
      for (size_t k = 0; k < run; k++) mmap_committed[first + i + k] = 0;
      g_committed_pages -= run;
    }
    i += run ? run : 1;
  }
}

// translate [addr,addr+len) to a clamped page range within the arena; returns 0 if
// outside the arena (e.g. a newlib-fallback pointer), else 1 with *first/*cnt set.
static int arena_page_range(void *addr, size_t len, size_t *first, size_t *cnt) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return 0;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return 0;
  size_t f = off / MMAP_PAGE;
  size_t c = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (f + c > mmap_pages) c = mmap_pages - f;
  *first = f; *cnt = c;
  return 1;
}

// commit [addr,len) on demand (mprotect RW / anon mmap). no-op if not overcommit.
static void arena_commit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_commit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// decommit [addr,len) (mprotect PROT_NONE / munmap). reclaims physical. Safe
// because re-use of a decommitted page goes through mprotect(RW) -> recommit.
static void arena_decommit_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) arena_decommit_locked(first, cnt);
  mutexUnlock(&g_mmap_lock);
}

// madvise(MADV_DONTNEED): zero the committed pages but KEEP them committed. The
// Switch has no fault handler, so decommitting here would crash if the engine
// re-touches without an intervening mprotect(RW) (allowed on Linux). Zeroing
// preserves the "reads back as zero after DONTNEED" contract safely.
static void arena_dontneed_range(void *addr, size_t len) {
  if (!g_overcommit) return;
  size_t first, cnt;
  mutexLock(&g_mmap_lock);
  if (arena_page_range(addr, len, &first, &cnt)) {
    for (size_t i = 0; i < cnt; ) {
      if (!mmap_committed[first + i]) { i++; continue; }
      size_t run = 0;
      while (i + run < cnt && mmap_committed[first + i + run]) run++;
      memset(mmap_arena + (first + i) * MMAP_PAGE, 0, run * MMAP_PAGE);
      i += run;
    }
  }
  mutexUnlock(&g_mmap_lock);
}

// ===========================================================================
// Stack-region overcommit (OC) arena.
// Boot probe established: svcMapMemory can alias heap pages into the STACK
// region (the alias region is rejected, kernel err 0xdc01), and Unity reserves
// ~2.8GB of PROT_NONE blocks while committing only ~80MB via mprotect(RW) with
// ZERO decommits. So we satisfy the big PROT_NONE reservations from a cheap
// stack-region address window and alias a small bump-allocated heap commit-pool
// in on mprotect(RW). Tried BEFORE the heap-backed arena for big anon PROT_NONE
// maps; anything else (and overflow when the window fills) falls through to the
// heap-backed arena, so if OC setup fails the engine runs exactly as before.
// Because decommits are never observed, the pool is a no-reclaim bump allocator.
// ===========================================================================
#define OC_NWIN 2
#define OC_NOSRC 0xFFFFFFFFu
typedef struct {
  uint8_t  *base;               // window base (64MB-aligned)
  size_t    pages;              // size in pages
  uint8_t  *used;               // 1/page: reserved by an mmap
  uint8_t  *committed;          // 1/page: physically backed via svcMapMemory
  uint32_t *srcpg;              // 1/page: which pool page backs it (OC_NOSRC = none)
} OcWin;
static OcWin    oc_win[OC_NWIN];
static int      oc_nwin;        // 0 => OC disabled
static uint8_t *oc_pool;        // shared commit-pool base (heap, page-aligned)
static size_t   oc_pool_pages;  // pool capacity in pages
static size_t   oc_pool_bump;   // next never-used pool page (bump)
static uint32_t *oc_pool_next;  // free-list links, one per pool page
static uint32_t oc_pool_freehead = OC_NOSRC;   // recycled pool pages
static size_t   oc_pool_freecnt;
static size_t   oc_live_pages;  // committed pages (diagnostic)

// Called once from main() after the newlib heap exists. window = a reserved
// stack-region range; pool = a heap buffer. Returns 1 if OC is armed.
static int oc_add_window_locked(void *window, size_t bytes) {
  if (oc_nwin >= OC_NWIN || !window || !bytes) return 0;
  size_t wp = bytes / MMAP_PAGE;
  uint8_t *u = (uint8_t *)calloc(wp, 1);
  uint8_t *c = (uint8_t *)calloc(wp, 1);
  uint32_t *sp = (uint32_t *)malloc(wp * sizeof(uint32_t));
  if (!u || !c || !sp) { free(u); free(c); free(sp); return 0; }
  for (size_t k = 0; k < wp; k++) sp[k] = OC_NOSRC;
  oc_win[oc_nwin].base = (uint8_t *)window; oc_win[oc_nwin].pages = wp;
  oc_win[oc_nwin].used = u; oc_win[oc_nwin].committed = c;
  oc_win[oc_nwin].srcpg = sp;
  oc_nwin++;
  return 1;
}
int oc_arena_init(void *window, size_t window_bytes, void *pool, size_t pool_bytes) {
  if (!window || !pool || !window_bytes || !pool_bytes) return 0;
  mutexLock(&g_mmap_lock);
  int ok = oc_add_window_locked(window, window_bytes);
  if (ok) {
    oc_pool = (uint8_t *)pool; oc_pool_pages = pool_bytes / MMAP_PAGE;
    oc_pool_bump = 0; oc_live_pages = 0;
    oc_pool_next = (uint32_t *)malloc(oc_pool_pages * sizeof(uint32_t));
    oc_pool_freehead = OC_NOSRC; oc_pool_freecnt = 0;
    if (!oc_pool_next) ok = 0;   /* recycling is mandatory: without it the pool leaks */
  }
  mutexUnlock(&g_mmap_lock);
  return ok;
}
int oc_arena_add_window(void *window, size_t window_bytes) {   /* second hole */
  mutexLock(&g_mmap_lock);
  int ok = (oc_nwin > 0) && oc_add_window_locked(window, window_bytes);
  mutexUnlock(&g_mmap_lock);
  return ok;
}

static OcWin *oc_win_of(void *addr) {
  for (int w = 0; w < oc_nwin; w++)
    if ((uint8_t *)addr >= oc_win[w].base &&
        (uint8_t *)addr <  oc_win[w].base + oc_win[w].pages * MMAP_PAGE)
      return &oc_win[w];
  return NULL;
}
static int oc_contains(void *addr) { return oc_win_of(addr) != NULL; }

// Reserve address space in the OC window. Mirrors mmap_arena_alloc_locked's
// 256MB-aligned tail-overflow so Unity's 511MB over-map nets one 256MB slot.
// caller holds g_mmap_lock.
static void *oc_alloc_locked(size_t len, size_t *got) {
  *got = 0;
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE; if (!need) need = 1;
  const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;
  size_t kept = need > step ? need - step : need;
  for (int w = 0; w < oc_nwin; w++) {                          // pass 1: full over-map fits
    OcWin *W = &oc_win[w];
    for (size_t i = 0; i + need <= W->pages; i += step) {
      size_t run = 0; while (run < need && !W->used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) W->used[i + k] = 1;
        *got = need * MMAP_PAGE; return W->base + i * MMAP_PAGE;
      }
    }
  }
  for (int w = 0; w < oc_nwin; w++) {                          // pass 2: tail slot
    OcWin *W = &oc_win[w];
    for (size_t i = 0; i < W->pages; i += step) {
      if (i + need <= W->pages) continue;
      size_t avail = W->pages - i; if (avail < kept) continue;
      size_t run = 0; while (run < avail && !W->used[i + run]) run++;
      if (run == avail) {
        for (size_t k = 0; k < avail; k++) W->used[i + k] = 1;
        *got = avail * MMAP_PAGE; return W->base + i * MMAP_PAGE;
      }
    }
  }
  return NULL;
}

// Commit [addr,len): alias contiguous bump-pool runs into the reserved OC range
// via svcMapMemory (which remaps the pool source away -- we only access via the
// OC address). Already-committed pages are skipped. caller holds g_mmap_lock.
/* Set by oc_commit_locked when it cannot back a range; reported by the CALLER
 * once it has released g_mmap_lock. Never log from inside: debugLogNote writes
 * to SD and takes the log lock, and taking the log lock under the mmap lock is
 * precisely the ABBA deadlock round 32 removed. */
static volatile int    oc_fail_kind;          /* 0 none, 1 pool dry, 2 map fail */
static volatile size_t oc_fail_need, oc_fail_live, oc_fail_pool;
static volatile int    oc_note_pending;       /* 16 MB commit milestone         */
static volatile size_t oc_note_live, oc_note_bump, oc_note_pool, oc_note_free;

static void oc_report_fail_unlocked(void) {
  if (oc_note_pending) {
    oc_note_pending = 0;
    debugPrintf("[oc] committed %zu MB (pool %zu/%zu MB, recycled %zu MB free)\n",
                oc_note_live, oc_note_bump, oc_note_pool, oc_note_free);
  }
  const int k = oc_fail_kind;
  if (!k) return;
  oc_fail_kind = 0;
  static int n = 0;
  if (n >= 4) return;
  n++;
  if (k == 1)
    debugLogNote("[oc] commit-pool EXHAUSTED: needed %zu pages "
                 "(live %zu MB of %zu MB pool) -- raise OC_POOL_BYTES in config.h\n",
                 oc_fail_need, oc_fail_live, oc_fail_pool);
  else
    debugLogNote("[oc] svcMapMemory FAILED backing %zu pages "
                 "(live %zu MB of %zu MB pool)\n",
                 oc_fail_need, oc_fail_live, oc_fail_pool);
}

/* Returns 1 if every requested page is now backed, 0 if the pool ran dry.
 * It used to return void: a dry pool silently left pages uncommitted while
 * mprotect_fake still reported success, so the game got memory it could not
 * touch. Callers must now propagate ENOMEM instead. */
static int oc_commit_locked(void *addr, size_t len) {
  OcWin *W = oc_win_of(addr);
  if (!W) return 1;                     /* not ours: nothing to commit */
  size_t first = ((uint8_t *)addr - W->base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first + cnt > W->pages) cnt = W->pages - first;
  size_t i = 0;
  while (i < cnt) {
    if (W->committed[first + i]) { i++; continue; }
    size_t run = 0;
    while (i + run < cnt && !W->committed[first + i + run]) run++;

    /* PHIGROS_STABILITY_R21_OC_CHUNK
     * Avoid one large svcMapMemory alias operation.  The captured boot
     * crash failed on a 4096-page (16 MiB) backing map even though only
     * 105/512 MiB of the OC pool was live.  Existing accounting below
     * advances i/oc_pool_bump, so limiting this run makes the same
     * commit happen as a sequence of <=1 MiB mappings. */
    {
      const size_t oc_map_chunk_pages = (1u * 1024u * 1024u) / MMAP_PAGE;
      if (run > oc_map_chunk_pages) run = oc_map_chunk_pages;
    }
    if (oc_pool_bump + run > oc_pool_pages) {
      /* not enough never-used pages: serve what we can from the free list */
      if (oc_pool_freecnt == 0) {
        /* Recorded, not logged: the caller emits it after releasing the mmap
         * lock. This is the single most useful line there is when the game dies
         * of a Unity out-of-memory -- OC_POOL_BYTES is inherited tuning (sized
         * from PvZ Fusion's 551 MB live), so if it fires, raise it in config.h. */
        oc_fail_kind = 1; oc_fail_need = run;
        oc_fail_live = (oc_live_pages * MMAP_PAGE) >> 20;
        oc_fail_pool = (oc_pool_pages * MMAP_PAGE) >> 20;
        return 0;
      }
      run = 1;   /* free-list pages are not contiguous: one at a time */
    }
    uint32_t srcpg;
    if (run == 1 && oc_pool_freehead != OC_NOSRC) {
      srcpg = oc_pool_freehead;
      oc_pool_freehead = oc_pool_next[srcpg];
      oc_pool_freecnt--;
    } else {
      srcpg = (uint32_t)oc_pool_bump;
    }
    void *dst = W->base + (first + i) * MMAP_PAGE;
    void *src = oc_pool + (size_t)srcpg * MMAP_PAGE;
    Result rc = svcMapMemory(dst, src, (u64)run * MMAP_PAGE);
    if (R_FAILED(rc)) {
      oc_fail_kind = 2; oc_fail_need = run;
      oc_fail_live = (oc_live_pages * MMAP_PAGE) >> 20;
      oc_fail_pool = (oc_pool_pages * MMAP_PAGE) >> 20;
      if (srcpg != (uint32_t)oc_pool_bump) {   /* give the recycled page back */
        oc_pool_next[srcpg] = oc_pool_freehead; oc_pool_freehead = srcpg; oc_pool_freecnt++;
      }
      return 0;
    }
    memset(dst, 0, run * MMAP_PAGE);   // freshly committed anon must read as zero
    for (size_t k = 0; k < run; k++) {
      W->committed[first + i + k] = 1;
      W->srcpg[first + i + k] = srcpg + (uint32_t)k;
    }
    if (srcpg == (uint32_t)oc_pool_bump) oc_pool_bump += run;
    oc_live_pages += run;
    /* Pre-existing hazard, deferred with the rest: this ran under g_mmap_lock,
     * and logging takes the log lock and writes to SD -- the same lock order
     * that deadlocked in round 32. It only fires on a 16 MB boundary, which is
     * presumably why it never hit, but rare is not safe. */
    if (((oc_live_pages * MMAP_PAGE) >> 24) != (((oc_live_pages - run) * MMAP_PAGE) >> 24)) {
      oc_note_live = (oc_live_pages * MMAP_PAGE) >> 20;
      oc_note_bump = (oc_pool_bump  * MMAP_PAGE) >> 20;
      oc_note_pool = (oc_pool_pages * MMAP_PAGE) >> 20;
      oc_note_free = (oc_pool_freecnt * MMAP_PAGE) >> 20;
      oc_note_pending = 1;
    }
    i += run;
  }
  /* KEPT when the round-159 36-bit work was reverted (round 160), because this
   * is not an address-space fix: the function is declared `int` and the success
   * path had no `return` at all, so it handed back whatever happened to be in
   * w0. That is undefined behaviour on any Switch. It is currently harmless
   * only because both callers discard the result -- a landmine for whoever
   * stops discarding it. `-Wall` reports `control reaches end of non-void
   * function` as a warning, not an error, so it built for 150 rounds. */
  return 1;
}

// munmap of an OC range: reclaim only UNCOMMITTED pages (the tail-overflow slack
// Unity trims after each over-map). Committed pages stay reserved+mapped (a small
// bounded leak) so a later reservation can't collide with a live alias.
/* Unmap committed pages and hand their pool pages back for reuse. */
static void oc_decommit_locked(void *addr, size_t len) {
  OcWin *W = oc_win_of(addr);
  if (!W || !oc_pool_next) return;
  size_t off   = (size_t)((uint8_t *)addr - W->base);
  size_t first = (off + MMAP_PAGE - 1) / MMAP_PAGE;   /* partial head page stays */
  size_t lastx = (off + len) / MMAP_PAGE;
  if (lastx > W->pages) lastx = W->pages;
  size_t i = first;
  while (i < lastx) {
    if (!W->committed[i]) { i++; continue; }
    size_t run = 1;                                   /* batch contiguous dst+src */
    while (i + run < lastx && W->committed[i + run] &&
           W->srcpg[i + run] == W->srcpg[i] + run) run++;
    void *dst = W->base + i * MMAP_PAGE;
    void *src = oc_pool + (size_t)W->srcpg[i] * MMAP_PAGE;
    if (R_SUCCEEDED(svcUnmapMemory(dst, src, (u64)run * MMAP_PAGE))) {
      for (size_t k = 0; k < run; k++) {
        uint32_t sp = W->srcpg[i + k];
        oc_pool_next[sp] = oc_pool_freehead; oc_pool_freehead = sp; oc_pool_freecnt++;
        W->committed[i + k] = 0; W->srcpg[i + k] = OC_NOSRC;
        if (oc_live_pages) oc_live_pages--;
      }
    }
    i += run;
  }
}
static void oc_free_locked(void *addr, size_t len) {
  OcWin *W = oc_win_of(addr);
  if (!W) return;
  size_t first = ((uint8_t *)addr - W->base) / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (first + cnt > W->pages) cnt = W->pages - first;
  oc_decommit_locked(addr, cnt * MMAP_PAGE);   /* recycle the backing pages */
  for (size_t i = 0; i < cnt; i++)
    if (!W->committed[first + i]) W->used[first + i] = 0;
}

// caller holds g_mmap_lock
static void mmap_arena_init_locked(void) {
  if (mmap_arena) return;
  uint8_t *base; size_t usable;
  if (g_mmap_arena_base) {
    base   = (uint8_t *)g_mmap_arena_base;   // dedicated, already 256MB-aligned
    usable = g_mmap_arena_size;
  } else {
    // fallback (small heap / applet): memalign a modest arena (< 2GB newlib limit)
    const size_t want = (size_t)768 * 1024 * 1024 + MMAP_BIG_ALIGN;
    uint8_t *raw = memalign(MMAP_PAGE, want);
    if (!raw) fatal_error("mmap arena alloc (%u MB) failed", (unsigned)(want >> 20));
    base   = (uint8_t *)(((uintptr_t)raw + (MMAP_BIG_ALIGN - 1)) & ~(MMAP_BIG_ALIGN - 1));
    usable = (size_t)768 * 1024 * 1024;
  }
  size_t pages  = usable / MMAP_PAGE;
  uint8_t *used = (uint8_t *)calloc(pages, 1);
  if (!used) fatal_error("mmap bitmap alloc failed");
  mmap_runlen = (uint32_t *)calloc(pages, sizeof(uint32_t));
  if (!mmap_runlen) fatal_error("mmap runlen alloc failed");
  if (g_overcommit) {
    mmap_committed = (uint8_t *)calloc(pages, 1);
    if (!mmap_committed) fatal_error("mmap commit-bitmap alloc failed");
  }
  mmap_usable = usable; mmap_pages = pages; mmap_used = used;
  mmap_arena  = base;   // publish last (alloc/free key off this)
  debugPrintf("[mmap] arena: %u MB %s at %p\n", (unsigned)(usable >> 20),
              g_overcommit ? "virtual (alias, on-demand commit)" : "256MB-aligned heap-backed",
              base);
}

// caller holds g_mmap_lock.
// Returns the mapped base and writes the number of bytes ACTUALLY reserved
// (in-arena) to *got. For big alignment over-maps (Unity reserves block+align,
// then munmaps the unaligned head/tail), the request is much larger than the
// ~256MB block Unity actually keeps. Normally we reserve the whole over-map and
// let the tail-munmap give it back. But for the LAST 256MB slot the full over-map
// runs past the arena end, so a plain "need contiguous pages" search fails even
// though the kept block fits. In that case we reserve only [slot, arena_end) -- the
// kept block lives there; Unity's tail-munmap targets addresses beyond our arena
// and is a harmless no-op. This removes the transient peak so each block costs
// exactly its 256MB slot (floor(arena/256MB) blocks fit, no 2x headroom needed).
static void *mmap_arena_alloc_locked(size_t len, size_t *got) {
  size_t need = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  if (!need) need = 1;
  if (len >= MMAP_BIG_THRESH) {
    const size_t step = MMAP_BIG_ALIGN / MMAP_PAGE;   // 256MB in pages
    size_t kept = need > step ? need - step : need;   // pages Unity actually keeps
    // pass 1: full over-map fits within the arena (normal case for all but the last slot)
    for (size_t i = 0; i + need <= mmap_pages; i += step) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        mmap_runlen[i] = (uint32_t)need;
        arena_ev(1, i, need);
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
    }
    // pass 2: tail slot -- the over-map would spill past the arena end, but the kept
    // block fits in [slot, arena_end). Reserve only that; the spill is trimmed away.
    for (size_t i = 0; i < mmap_pages; i += step) {
      if (i + need <= mmap_pages) continue;          // handled by pass 1
      size_t avail = mmap_pages - i;
      if (avail < kept) continue;                    // kept block wouldn't fit
      size_t run = 0;
      while (run < avail && !mmap_used[i + run]) run++;
      if (run == avail) {
        for (size_t k = 0; k < avail; k++) mmap_used[i + k] = 1;
        mmap_runlen[i] = (uint32_t)avail;
        arena_ev(1, i, avail);
        *got = avail * MMAP_PAGE;                     // only the in-arena portion
        return mmap_arena + i * MMAP_PAGE;
      }
    }
  } else {
    for (size_t i = 0; i + need <= mmap_pages; ) {
      size_t run = 0;
      while (run < need && !mmap_used[i + run]) run++;
      if (run == need) {
        for (size_t k = 0; k < need; k++) mmap_used[i + k] = 1;
        mmap_runlen[i] = (uint32_t)need;
        arena_ev(1, i, need);
        *got = need * MMAP_PAGE;
        return mmap_arena + i * MMAP_PAGE;
      }
      i += run + 1;
    }
  }
  *got = 0;
  return NULL;
}

/* Release pages, but NEVER past the end of the allocation being freed.
 *
 * The old version cleared ceil(len/PAGE) bits from wherever the caller pointed,
 * trusting the caller's length. Two ways that frees somebody else's pages:
 *
 *   - a munmap longer than we reserved. The big-allocation pass 2 truncates to
 *     what fits in the arena and returns a smaller *got, so the caller's own
 *     length is legitimately larger than the run.
 *   - a munmap of a sub-range that runs past the end of its own allocation.
 *
 * Either way the neighbouring run's pages go back on the free list while it is
 * still live; the next mmap hands them out and the MAP_ANONYMOUS memset() zeroes
 * whatever was there. An object header zeroed that way reads klass == 0, which
 * is the ModeSelect crash exactly. */
static unsigned g_arena_free_clamped = 0;

static void mmap_arena_free(void *addr, size_t len) {
  if (!mmap_arena || (uint8_t *)addr < mmap_arena) return;
  size_t off = (uint8_t *)addr - mmap_arena;
  if (off >= mmap_usable) return;
  size_t first = off / MMAP_PAGE;
  size_t cnt   = (len + MMAP_PAGE - 1) / MMAP_PAGE;
  size_t asked = cnt;
  mutexLock(&g_mmap_lock);
  if (!mmap_runlen[first]) {
    /* Not the start of any allocation. Round 112 clamped only at the far end, so
     * a free landing MID-RUN still released everything from there to the next
     * run start -- the tail of a live allocation. Those pages went back on the
     * list, the next mmap took them, and the MAP_ANONYMOUS memset punched a
     * zeroed hole INSIDE memory the owner was still using.
     *
     * That is what the round-113 dump showed: the faulting object and a
     * perfectly valid one 107 KB away were in the SAME 869-page run, the page
     * marked RESERVED. A live allocation with a hole in it.
     *
     * Refuse. Address space leaks (192 MB arena, ~42 MB in use) and that costs
     * nothing; handing out live memory costs a crash. */
    const int mid = mmap_used[first] ? 1 : 0;
    mutexUnlock(&g_mmap_lock);
    if (mid) {
      static unsigned n = 0;
      if (++n <= 8)
        debugLogNote("[mmap] arena free REFUSED at %p (+%u KB): mid-run, not an "
                     "allocation start -- would have freed a live range (#%u)\n",
                     addr, (unsigned)(off >> 10), n);
    }
    return;
  }
  if (cnt > mmap_runlen[first]) cnt = mmap_runlen[first];
  mmap_runlen[first] = 0;
  size_t done = 0;
  for (size_t k = 0; k < cnt && first + k < mmap_pages; k++) {
    /* a run length recorded mid-way means a DIFFERENT allocation starts here */
    if (k && mmap_runlen[first + k]) break;
    mmap_used[first + k] = 0;
    done++;
  }
  arena_ev(2, first, done);
  const int clamped = (done < asked);
  if (clamped) g_arena_free_clamped++;
  mutexUnlock(&g_mmap_lock);
  if (clamped && g_arena_free_clamped <= 8)
    debugLogNote("[mmap] arena free CLAMPED at %p: caller asked %zu pages, run owns %zu "
                 "-- the rest belong to a live allocation (#%u)\n",
                 addr, asked, done, g_arena_free_clamped);
}

// Stopgap: when the 256MB-block arena is exhausted by Unity's 9 pools, small
// (sub-threshold) il2cpp/GC mmaps still need to land somewhere. Serve them from
// newlib's free heap via memalign and track each so munmap can free it. This is
// not real overcommit (it consumes physical newlib heap), but it unblocks the
// sub-1MB il2cpp allocations that were failing and surfaces il2cpp's true mmap
// appetite in the log to size the proper fix.
#define MMAP_FALLBACK_MAX 4096
static struct { void *ptr; size_t len; } g_fb[MMAP_FALLBACK_MAX];
static int   g_fb_n = 0;
static size_t g_fb_bytes = 0;
static Mutex g_fb_lock;

static void *mmap_fallback(size_t length, int flags, int fd, long offset) {
  void *q = memalign(MMAP_PAGE, length);
  if (!q) return NULL;
  long got = 0;
  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(q, 0, length);
  } else {
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < length) { long r = read(fd, (char *)q + got, length - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < length) memset((char *)q + got, 0, length - got);
  }
  mutexLock(&g_fb_lock);
  if (g_fb_n < MMAP_FALLBACK_MAX) { g_fb[g_fb_n].ptr = q; g_fb[g_fb_n].len = length; g_fb_n++; g_fb_bytes += length; }
  const size_t total = g_fb_bytes;
  mutexUnlock(&g_fb_lock);
  debugPrintf("[mmap] fallback %u KB -> %p  anon=%d fd=%d off=0x%lx got=%ld (total %u MB)\n",
              (unsigned)(length >> 10), q, !!(flags & BIONIC_MAP_ANONYMOUS), fd, offset, got,
              (unsigned)(total >> 20));
  return q;
}

// returns 1 and frees if addr was a fallback allocation
static int mmap_fallback_free(void *addr) {
  mutexLock(&g_fb_lock);
  for (int i = 0; i < g_fb_n; i++) {
    if (g_fb[i].ptr == addr) {
      free(addr);
      g_fb_bytes -= g_fb[i].len;
      g_fb[i] = g_fb[--g_fb_n];
      mutexUnlock(&g_fb_lock);
      return 1;
    }
  }
  mutexUnlock(&g_fb_lock);
  return 0;
}

/* ---- write-back for writable file-backed maps ---------------------------
 * mmap_fake reads a file into RAM; nothing maps it. Without this, a
 * MAP_SHARED|PROT_WRITE map is a scratch buffer and the file never changes.
 * That is exactly how the il2cpp metadata extraction writes its output. */
#define FBMAP_N 8
static struct { void *addr; size_t len; long off; int used; char path[192]; }
  g_fbmap[FBMAP_N];

static void fbmap_track(void *addr, size_t len, int prot, int flags, int fd, long off) {
  if (!addr || fd < 0 || !len) return;
  if (flags & BIONIC_MAP_ANONYMOUS) return;
  if (!(prot & BIONIC_PROT_WRITE)) return;    /* read-only: nothing to flush */
  if (!(flags & BIONIC_MAP_SHARED)) return;   /* MAP_PRIVATE must not hit disk */
#if PHI_ASSET_PACK
  if (asset_pack_fd_is(fd)) return;           /* the pack is read-only */
#endif
  const char *fp = fdpath_get(fd);
  if (!fp || !fp[0] || fp[0] == 0x3f) return; /* "?" -> path unknown, cannot flush */
  mutexLock(&g_mmap_lock);
  for (int i = 0; i < FBMAP_N; i++) {
    if (!g_fbmap[i].used) {
      g_fbmap[i].used = 1; g_fbmap[i].addr = addr;
      g_fbmap[i].len = len; g_fbmap[i].off = off;
      snprintf(g_fbmap[i].path, sizeof g_fbmap[i].path, "%s", fp);
      mutexUnlock(&g_mmap_lock);
      debugPrintf("[mmap] writable file map %p len=%zu -> %s (will flush)\n",
                  addr, len, fp);
      return;
    }
  }
  mutexUnlock(&g_mmap_lock);
  debugPrintf("[mmap] *** write-back table full; %s will NOT be flushed ***\n", fp);
}

/* Write a tracked map back to its file. drop != 0 releases the slot. */
static void fbmap_flush(void *addr, int drop) {
  char path[192]; size_t len = 0; long off = 0; int hit = -1;
  mutexLock(&g_mmap_lock);
  for (int i = 0; i < FBMAP_N; i++) {
    if (g_fbmap[i].used && g_fbmap[i].addr == addr) {
      hit = i; len = g_fbmap[i].len; off = g_fbmap[i].off;
      snprintf(path, sizeof path, "%s", g_fbmap[i].path);
      if (drop) g_fbmap[i].used = 0;
      break;
    }
  }
  mutexUnlock(&g_mmap_lock);
  if (hit < 0) return;
  /* Re-open by the exact string the app used: that open already succeeded,
   * so it is a spelling this filesystem accepts. */
  int wfd = open(path, O_WRONLY);
  if (wfd < 0) {
    debugPrintf("[mmap] flush FAILED: open(%s,O_WRONLY) errno=%d\n", path, errno);
    return;
  }
  size_t done = 0;
  if (lseek(wfd, off, SEEK_SET) >= 0) {
    while (done < len) {
      long w = write(wfd, (const char *)addr + done, len - done);
      if (w <= 0) break;
      done += (size_t)w;
    }
  }
  close(wfd);
  debugPrintf("[mmap] flushed %zu/%zu bytes -> %s%s\n", done, len, path,
              done < len ? "  *** SHORT WRITE ***" : "");
}

/* msync(): flush without releasing the mapping. */
int msync_fake(void *addr, size_t length, int flags) {
  (void)length; (void)flags;
  fbmap_flush(addr, 0);
  return 0;
}

void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset) {
  (void)addr; (void)prot;
  if (length == 0) length = 1;

  if ((flags & BIONIC_MAP_FIXED) && addr && (flags & BIONIC_MAP_ANONYMOUS)) {
    static unsigned fixed_n = 0;
    if (oc_contains(addr)) {
      int oc_ok = 1;
      mutexLock(&g_mmap_lock);
      if (prot != BIONIC_PROT_NONE) oc_ok = oc_commit_locked(addr, length);
      else                          oc_decommit_locked(addr, length);  /* recycle */
      mutexUnlock(&g_mmap_lock);
      /* REVERTED (round 105). Returning ENOMEM here looked more correct -- a
       * partly-backed range is not really a successful mapping -- but it broke
       * boot: oc_commit_locked() gives up MID-LOOP, so one failed run failed the
       * whole request even though most of the range was already backed. Unity
       * then fed the failure into tlsf_add_pool, which rejected the size
       * ("Memory size must be between 0x28 and 0x100000000 bytes") and crashed
       * writing to NULL in libunity+0x4b0c28.
       * Tolerating a partial commit is what shipped for 100 rounds and boots, so
       * keep it and just say so. */
      (void)oc_ok;
      oc_report_fail_unlocked();          /* safe here: g_mmap_lock is released */
      if (prot != BIONIC_PROT_NONE)
        memset(addr, 0, length);          /* FIXED anon: whole range reads zero */
      if (fixed_n < 3)
        debugPrintf("[mmap] MAP_FIXED %s %u KB @ %p (OC)\n",
                    prot == BIONIC_PROT_NONE ? "decommit" : "commit",
                    (unsigned)(length >> 10), addr);
      fixed_n++;
      return addr;                        /* bionic: FIXED returns addr */
    }
    if (g_mmap_arena_base &&
        (uint8_t *)addr >= (uint8_t *)g_mmap_arena_base &&
        (uint8_t *)addr + length <= (uint8_t *)g_mmap_arena_base + g_mmap_arena_size) {
      if (prot != BIONIC_PROT_NONE) memset(addr, 0, length);   /* arena is committed */
      if (fixed_n < 3)
        debugPrintf("[mmap] MAP_FIXED %u KB @ %p (arena) -> in place\n", (unsigned)(length >> 10), addr);
      fixed_n++;
      return addr;
    }
    debugPrintf("[mmap] MAP_FIXED %u KB @ %p prot=0x%x UNOWNED -> legacy path\n",
                (unsigned)(length >> 10), addr, prot);
  }

  // Big anonymous PROT_NONE reservations -> stack-region OC arena: cheap address
  // space now, physical aliased in on the later mprotect(RW). On a full window we
  // fall through to the heap-backed arena below (no behaviour change there).
  if (oc_nwin && length >= MMAP_BIG_THRESH &&
      (flags & BIONIC_MAP_ANONYMOUS) && prot == BIONIC_PROT_NONE) {
    size_t ocres = 0;
    mutexLock(&g_mmap_lock);
    void *op = oc_alloc_locked(length, &ocres);
    mutexUnlock(&g_mmap_lock);
    if (op) {
      debugPrintf("[mmap] %u MB (prot=0x0 anon=1) -> %p  [OC reserve %u MB]\n",
                  (unsigned)(length >> 20), op, (unsigned)(ocres >> 20));
      return op;   // reserved-only; committed lazily via mprotect_fake
    }
    debugPrintf("[mmap] OC window full for %u MB -> heap-backed arena\n",
                (unsigned)(length >> 20));
  }

  size_t reserved = 0;
  mutexLock(&g_mmap_lock);
  mmap_arena_init_locked();
  void *p = mmap_arena_alloc_locked(length, &reserved);
  mutexUnlock(&g_mmap_lock);
  if (p) arena_big_alloc_note((reserved ? reserved : length) / MMAP_PAGE, p,
                              __builtin_return_address(0));
  if (length >= MMAP_BIG_THRESH)
    debugPrintf("[mmap] %u MB (prot=0x%x anon=%d) -> %p  [reserved %u MB]\n",
                (unsigned)(length >> 20), prot, !!(flags & BIONIC_MAP_ANONYMOUS), p,
                (unsigned)(reserved >> 20));
  /* A file-backed map must be contiguous and fully readable. When the arena can
   * only give a tail-overflow reservation (reserved < length) we'd read just
   * `fill` bytes and leave the tail unfilled -- silently truncating the file in
   * RAM. For global-metadata.dat that nulls out System.Object (Class::Init NULL).
   * Hand any short-reserved file map to newlib, which backs the whole length. */
  if (p && !(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0 && reserved < length) {
    mutexLock(&g_mmap_lock);
    mmap_arena_free(p, length);
    mutexUnlock(&g_mmap_lock);
    debugPrintf("[mmap] file fd=%d len=%zu: arena tail-overflow (reserved=%zu) -> newlib\n",
                fd, length, reserved);
    p = NULL;
  }
  if (!p) {
    // Arena exhausted (Unity's 9 pools fill it). Route the request to newlib's free
    // heap regardless of size -- il2cpp's resource-extraction maps can exceed 64MB,
    // and rejecting them is what NULL-derefs the engine. Only a genuinely huge map
    // (> newlib free) will fail, and we log that distinctly.
    void *q = mmap_fallback(length, flags, fd, offset);
    if (q) {
      /* The arena is full. This still works -- newlib backs it -- but it is
       * real memory pressure and it used to be invisible, so a log with no
       * "arena full" line could not be read as "memory was fine". */
      static unsigned nfb = 0;
      if (nfb < 16) { nfb++;
        debugLogNote("[mmap] arena full -> newlib fallback for %u KB "
                     "(fallback #%u) -- memory pressure\n",
                     (unsigned)(length >> 10), nfb); }
      return q;
    }
    debugPrintf("[mmap] arena FULL and newlib fallback FAILED for %u MB (out of RAM)\n",
                (unsigned)(length >> 20));
    errno = ENOMEM; return (void *)-1;
  }

  // Never touch beyond what we actually reserved in-arena (tail over-maps reserve
  // less than the requested length; the spill lives past the arena and is trimmed).
  size_t fill = length < reserved ? length : reserved;

  if (g_overcommit) {
    // PROT_NONE reservation: address space only, no physical -- the whole point.
    // The engine commits the sub-ranges it uses later via mprotect(RW).
    if (prot == BIONIC_PROT_NONE) return p;
    // Otherwise commit now (anon RW, file maps): svcMapPhysicalMemory zero-fills,
    // so anon needs no memset; file maps read their contents over the zeros.
    arena_commit_range(p, fill);
    if (!(flags & BIONIC_MAP_ANONYMOUS) && fd >= 0) {
      long got = 0, cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0)
        while ((size_t)got < fill) { long r = read(fd, (char *)p + got, fill - got); if (r <= 0) break; got += r; }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    fbmap_track(p, length, prot, flags, fd, offset);   /* newlib-backed path */
    return p;
  }

  if (flags & BIONIC_MAP_ANONYMOUS) {
    memset(p, 0, fill);   // anonymous memory must read back as zero
  } else {
    // File-backed mapping: pull [offset, offset+fill) into RAM (no real mmap).
    long got = 0;
    if (fd >= 0) {
      long cur = lseek(fd, 0, SEEK_CUR);
      if (lseek(fd, offset, SEEK_SET) >= 0) {
        while ((size_t)got < fill) {
          long r = read(fd, (char *)p + got, fill - (size_t)got);
          if (r <= 0) break;
          got += r;
        }
      }
      if (cur >= 0) lseek(fd, cur, SEEK_SET);
    }
    if ((size_t)got < fill) memset((char *)p + got, 0, fill - (size_t)got);
    if (fd >= 0)
      debugPrintf("[mmap] file map fd=%d (%s) len=%zu reserved=%zu fill=%zu got=%ld%s\n",
                  fd, fdpath_get(fd), length, reserved, fill, got,
                  /* Compare against the FILE, not the page-rounded map length.
                   * `fill` is rounded up to a page, so a perfectly complete read
                   * of an 11,124,332-byte file into an 11,124,736-byte mapping
                   * was being flagged TRUNCATED every boot. Reading to EOF and
                   * zero-filling the tail is exactly what mmap should do; the
                   * warning sent me looking at asset integrity twice for nothing. */
                  ((size_t)got + MMAP_PAGE < fill) ? "  *** SHORT READ ***" : "");
    fbmap_track(p, length, prot, flags, fd, offset);   /* arena path */
  }
  return p;
}

int munmap_fake(void *addr, size_t length) {
  fbmap_flush(addr, 1);   /* MUST run before the memory is released */
  if (mmap_fallback_free(addr)) return 0;   // newlib fallback allocation
  if (oc_contains(addr)) {                   // stack-region OC reservation
    mutexLock(&g_mmap_lock);
    oc_free_locked(addr, length);
    mutexUnlock(&g_mmap_lock);
    return 0;
  }
  arena_decommit_range(addr, length);       // reclaim physical (overcommit only)
  mmap_arena_free(addr, length);            // unreserve address space
  return 0;
}

// In overcommit mode mprotect drives commit/decommit: RW/R commits physical at
// the alias address, PROT_NONE decommits it (safe -- reuse re-mprotects to RW).
// In heap-backed mode the arena is always RW so this is a no-op.
int mprotect_fake(void *addr, size_t len, int prot) {
  /* Diagnostic (fires even heap-backed): measure Unity's commit pattern so we can
   * confirm it commits PROT_NONE reservations via mprotect(RW) and size the
   * overcommit commit-pool. Tracks cumulative RW-commit vs PROT_NONE-decommit
   * bytes that fall inside the mmap arena (= Unity's live committed footprint). */
  {
    static size_t rw_b = 0, none_b = 0;
    static unsigned rw_n = 0, none_n = 0, oth_n = 0;
    int in_arena = g_mmap_arena_base &&
                   (uint8_t *)addr >= (uint8_t *)g_mmap_arena_base &&
                   (uint8_t *)addr <  (uint8_t *)g_mmap_arena_base + g_mmap_arena_size;
    if (prot == BIONIC_PROT_NONE)       { none_n++; if (in_arena) none_b += len; }
    else if (prot & BIONIC_PROT_WRITE)  { rw_n++;   if (in_arena) rw_b   += len; }
    else                                  oth_n++;
    if (TRACE_MPROT && (len >= 4u * 1024 * 1024 || ((rw_n + none_n) & 0x7F) == 0))
      debugPrintf("[mprot] addr=%p len=%zuKB prot=0x%x arena=%d | RW %u/%zuMB NONE %u/%zuMB oth %u  net=%zdMB\n",
                  addr, len >> 10, prot, in_arena, rw_n, rw_b >> 20, none_n, none_b >> 20, oth_n,
                  (ssize_t)(rw_b - none_b) >> 20);
  }
  if (oc_contains(addr)) {
    // OC reservation being committed/decommitted. PROT_NONE releases the backing
    // pool page for reuse (see oc_decommit_locked); RW commits it.
    {
      int oc_ok = 1;
      mutexLock(&g_mmap_lock);
      if (prot == BIONIC_PROT_NONE) oc_decommit_locked(addr, len);   /* recycle */
      else                          oc_ok = oc_commit_locked(addr, len);
      mutexUnlock(&g_mmap_lock);
      /* REVERTED with the mmap path above: failing the whole mprotect on a
       * partial commit is what stopped the game booting. Report and continue. */
      (void)oc_ok;
      oc_report_fail_unlocked();          /* safe here: g_mmap_lock is released */
    }
    return 0;
  }
  if (!g_overcommit) return 0;
  if (prot == BIONIC_PROT_NONE) arena_decommit_range(addr, len);
  else                          arena_commit_range(addr, len);
  return 0;
}
// madvise(MADV_DONTNEED): overcommit zeroes-but-keeps (see arena_dontneed_range);
// heap-backed leaves pages as-is (always RW-backed).
int madvise_fake(void *addr, size_t len, int advice) {
  if (g_overcommit && advice == BIONIC_MADV_DONTNEED) arena_dontneed_range(addr, len);
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!path) return NULL;          /* POSIX: realpath(NULL,..) is an error, not a crash */
  if (!resolved) resolved = malloc(0x1000);
  strcpy(resolved, path);
  return resolved;
}
int strerror_r_fake(int err, char *buf, size_t len) { snprintf(buf, len, "%s", strerror(err)); return 0; }
int statvfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x70); return 0; }
int statfs_fake(const char *path, void *buf) { (void)path; memset(buf, 0, 0x78); return 0; }

// Synthetic /proc and /sys files. Unity reads /proc/meminfo (MemTotal) to size
// its allocator reservations and /proc/cpuinfo + /sys cpu range to count cores
// for the job system. We report ~1 GB (NOT the real ~3 GB) so the engine's big
// 256MB-block dynamic-heap reservations stay within our mmap arena -- the arena
// is the real backing and has headroom, but Unity must not try to reserve 3 GB
// of address space up front. 3 cores (homebrew gets 0-2).
static const char *synthetic_proc(const char *path) {
  if (!path) return NULL;
  if (!strcmp(path, "/proc/meminfo"))
    return "MemTotal:        524288 kB\n"
           "MemFree:         393216 kB\n"
           "MemAvailable:    393216 kB\n"
           "Buffers:              0 kB\n"
           "Cached:               0 kB\n"
           "SwapTotal:            0 kB\n"
           "SwapFree:             0 kB\n";
  if (!strcmp(path, "/proc/cpuinfo"))
    return "processor\t: 0\nprocessor\t: 1\nprocessor\t: 2\n"
           "Features\t: fp asimd aes pmull sha1 sha2 crc32\n"
           "CPU implementer\t: 0x41\nCPU architecture: 8\nCPU variant\t: 0x1\n"
           "CPU part\t: 0xd07\nCPU revision\t: 1\n";
  if (strstr(path, "cpu_capacity")) return "1024\n";
  if (strstr(path, "cpuinfo_max_freq") || strstr(path, "scaling_max_freq")) return "1785000\n";
  if (strstr(path, "cpuinfo_min_freq") || strstr(path, "scaling_min_freq")) return "1020000\n";
  if (strstr(path, "/cpu/possible") || strstr(path, "/cpu/present") || strstr(path, "/cpu/online"))
    return "0-2\n";
  if (!strncmp(path, "/proc/", 6) || !strncmp(path, "/sys/", 5)) return ""; // empty for the rest
  return NULL;
}

// ---------------------------------------------------------------------------
// Locked open/close. newlib's FILE table is shared process-wide, and the engine
// opens and closes bundle files from several worker threads at once. Anything
// ELSE in the port that touches that table -- nx_pointer writing pointer.cfg
// from the render thread, for one -- has to serialise against them, so both
// sides funnel through this one mutex. fopen/fclose are SD round-trips measured
// in milliseconds, so the lock costs nothing measurable.
//
// Exported (see the declarations in android_native_unity.c) for non-engine
// callers; fopen_fake/fclose_fake below are the engine's own entry points.
// ---------------------------------------------------------------------------
static Mutex g_stdio_lock;   // libnx Mutex: 0 == unlocked, no init needed

FILE *nx_fopen_locked(const char *path, const char *mode) {
  mutexLock(&g_stdio_lock);
  FILE *f = fopen(path, mode);
  mutexUnlock(&g_stdio_lock);
  return f;
}

int nx_fclose_locked(FILE *f) {
  mutexLock(&g_stdio_lock);
  int r = fclose(f);
  mutexUnlock(&g_stdio_lock);
  return r;
}

FILE *fmemopen_locked(void *buf, size_t n, const char *mode) {
  mutexLock(&g_stdio_lock);
  FILE *f = fmemopen(buf, n, mode);
  mutexUnlock(&g_stdio_lock);
  return f;
}

// a buffered fopen for the big .mvgl archives: the engine issues many small
// reads/seeks and the fsdev round-trips dominate without a large buffer.
FILE *fopen_fake(const char *path, const char *mode) {
  path = casetest_redirect(path);
  const char *synth = synthetic_proc(path);
  if (synth) {
    size_t n = strlen(synth);
    return fmemopen_locked((void *)strdup(synth), n ? n : 1, "r");
  }
  const int writing = strpbrk(mode, "wa+") != NULL;
  if (!writing) {                 // packed files are read-only
    void *data = NULL; size_t size = 0;
    const char *frel = nx_pack_relpath(path);
    if (frel && asset_pack_read_all_path(frel, &data, &size)) {
      FILE *pf = fmemopen_locked(data, size ? size : 1, "r");
      if (pf) { debugPrintf("[io] fopen(%s,%s) -> %p [pack]\n", path, mode, (void *)pf); return pf; }
      free(data);
    }
  }
  FILE *f = nx_fopen_locked(path, mode);
  if (!f && writing) {            // save file: create the subdir and retry
    mkdir_parents(path);
    f = nx_fopen_locked(path, mode);
  }
  if (!f && !writing && strchr(mode, 'r')) {
    char alt[320];
    if (basename_fallback(path, alt, sizeof(alt)))
      f = nx_fopen_locked(alt, mode);
  }
  if (!f)
    return NULL;
  debugPrintf("[io] fopen(%s,%s) -> %p\n", path, mode, (void *)f);
  if (strchr(mode, 'r')) {
    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".mvgl") == 0)
      setvbuf(f, NULL, _IOFBF, 256 * 1024);
  }
  return f;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr). libc++_shared wires
// std::cout/cerr/cin to &__sF[1]/[2]/[0]; these wrappers absorb writes to those
// fake FILEs and forward everything else to newlib.
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c (__sF / std{in,out,err})

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    const size_t total = size * n < sizeof(buf) - 1 ? size * n : sizeof(buf) - 1;
    memcpy(buf, ptr, total); buf[total] = '\0';
    debugPrintf("stdio: %s", buf);
#endif
    return n;
  }
  return fwrite(ptr, size, n, f);
}
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f)) return 0;
  return fread(ptr, size, n, f);
}
int fputc_fake(int c, FILE *f) { if (is_fake_file(f)) return c; return fputc(c, f); }
int fputs_fake(const char *s, FILE *f) { if (is_fake_file(f)) { debugPrintf("stdio: %s", s); return 0; } return fputs(s, f); }
int fflush_fake(FILE *f) { if (is_fake_file(f) || f == NULL) return 0; return fflush(f); }
int fclose_fake(FILE *f) { if (is_fake_file(f)) return 0; return nx_fclose_locked(f); }
int ferror_fake(FILE *f) { if (is_fake_file(f)) return 0; return ferror(f); }
int feof_fake(FILE *f) { if (is_fake_file(f)) return 1; return feof(f); }
int fileno_fake(FILE *f) { if (is_fake_file(f)) return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100; return fileno(f); }
int fseek_fake(FILE *f, long off, int whence) { if (is_fake_file(f)) return -1; return fseek(f, off, whence); }
long ftell_fake(FILE *f) { if (is_fake_file(f)) return -1; return ftell(f); }
int getc_fake(FILE *f) { if (is_fake_file(f)) return -1; return getc(f); }
int fgetc_fake(FILE *f) { if (is_fake_file(f)) return -1; return fgetc(f); }
char *fgets_fake(char *s, int n, FILE *f) { if (is_fake_file(f)) return NULL; return fgets(s, n, f); }
int ungetc_fake(int c, FILE *f) { if (is_fake_file(f)) return -1; return ungetc(c, f); }
void setbuf_fake(FILE *f, char *buf) { if (is_fake_file(f)) return; setbuf(f, buf); }

int fprintf_fake(FILE *f, const char *fmt, ...) {
  va_list va; va_start(va, fmt);
  int ret;
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
#else
    ret = 0;
#endif
  } else {
    ret = vfprintf(f, fmt, va);
  }
  va_end(va);
  return ret;
}
int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f)) {
#if DEBUG_LOG
    static char buf[0x400];
    int ret = vsnprintf(buf, sizeof(buf), fmt, va);
    debugPrintf("stdio: %s", buf);
    return ret;
#else
    return 0;
#endif
  }
  return vfprintf(f, fmt, va);
}

// ---------------------------------------------------------------------------
// fd routing: the native_app_glue command pipe lives in the fake-fd layer
// (android_native.c). Real files (small fds from open()) pass through to newlib.
// ---------------------------------------------------------------------------

/* ---------------- asset read cache ---------------- */
#define RC_BLOCK   (64u * 1024)
#define RC_NBLOCK  384u                 /* 384 * 64K = 24 MB */
#define RC_MAXFD   64
typedef struct { int fd; uint64_t blk; uint32_t len; } RcTag;
static RcTag    rc_tag[RC_NBLOCK];
static uint8_t *rc_data;
static Mutex    rc_lock;
static int      rc_state;               /* 0=untried 1=ready -1=disabled */
static uint8_t  rc_cacheable[RC_MAXFD];
volatile unsigned long long rc_hits, rc_fills;
volatile unsigned long long g_read_calls = 0, g_read_bytes = 0;
volatile unsigned long long g_fsread_calls = 0, g_fsread_ns = 0, g_fsread_bytes = 0;
void rc_mark(int fd, int on) {           /* called from open_fake/close_fake */
  if (fd < 0 || fd >= RC_MAXFD) return;
  mutexLock(&rc_lock);
  rc_cacheable[fd] = on ? 1 : 0;
  if (rc_state == 1)                     /* fd numbers get reused: drop stale blocks */
    for (unsigned i = 0; i < RC_NBLOCK; i++) if (rc_tag[i].fd == fd) rc_tag[i].fd = -1;
  mutexUnlock(&rc_lock);
}
static void rc_init(void) {
  if (rc_state) return;
  mutexLock(&rc_lock);
  if (!rc_state) {
    rc_data = (uint8_t *)malloc((size_t)RC_NBLOCK * RC_BLOCK);
    if (rc_data) {
      for (unsigned i = 0; i < RC_NBLOCK; i++) rc_tag[i].fd = -1;
      rc_state = 1;
      debugPrintf("[rc] asset read cache armed: %u blocks x %u KB = %u MB\n",
                  RC_NBLOCK, RC_BLOCK >> 10, (RC_NBLOCK * RC_BLOCK) >> 20);
    } else {
      rc_state = -1;
      debugPrintf("[rc] asset read cache DISABLED (alloc failed)\n");
    }
  }
  mutexUnlock(&rc_lock);
}
/* Serve `count` bytes at absolute `off`. Returns bytes served, or -1 to say
 * "not cacheable, caller should do it the plain way". */
static long rc_pread(int fd, void *buf, size_t count, uint64_t off) {
  /* Pack handles carry their own cache and their fds sit outside the rc_*
   * table's range assumptions -- serve them directly. Unity does nearly all
   * SerializedFile reads through pread, so without this the pack is bypassed
   * for the hot path and every read falls back to the SD loose tree. */
  if (asset_pack_fd_is(fd)) return asset_pack_pread_fd(fd, buf, count, (long)off);
  if (fd < 0 || fd >= RC_MAXFD) return -1;
  rc_init();
  if (rc_state != 1 || !rc_cacheable[fd] || count == 0) return -1;
  size_t done = 0;
  mutexLock(&rc_lock);
  while (done < count) {
    uint64_t abs = off + done;
    uint64_t blk = abs / RC_BLOCK;
    unsigned idx = (unsigned)(((blk * 2654435761ull) ^ (unsigned)fd) % RC_NBLOCK);
    uint8_t *slot = rc_data + (size_t)idx * RC_BLOCK;
    if (rc_tag[idx].fd != fd || rc_tag[idx].blk != blk) {   /* miss -> fill one block */
      if (lseek(fd, (long)(blk * RC_BLOCK), SEEK_SET) < 0) break;
      size_t got = 0;
      uint64_t __ft = armGetSystemTick();
      while (got < RC_BLOCK) {
        long r = read(fd, slot + got, RC_BLOCK - got);
        if (r <= 0) break;
        got += (size_t)r;
      }
      __atomic_add_fetch(&g_fsread_ns, armTicksToNs(armGetSystemTick() - __ft), __ATOMIC_RELAXED);
      __atomic_add_fetch(&g_fsread_bytes, got, __ATOMIC_RELAXED);
      __atomic_add_fetch(&g_fsread_calls, 1, __ATOMIC_RELAXED);
      rc_tag[idx].fd = fd; rc_tag[idx].blk = blk; rc_tag[idx].len = (uint32_t)got;
      rc_fills++;
    } else rc_hits++;
    uint32_t blen  = rc_tag[idx].len;
    uint32_t inblk = (uint32_t)(abs - blk * RC_BLOCK);
    if (inblk >= blen) break;                    /* EOF inside this block */
    size_t n = blen - inblk;
    if (n > count - done) n = count - done;
    memcpy((char *)buf + done, slot + inblk, n);
    done += n;
    if (blen < RC_BLOCK) break;                  /* short block == EOF */
  }
  if (0)
    debugPrintf("[rc] hits=%llu fills=%llu\n", rc_hits, rc_fills);
  mutexUnlock(&rc_lock);
  return (long)done;
}

long rc_pread_pub(int fd, void *buf, size_t count, unsigned long long off) {
  return rc_pread(fd, buf, count, (uint64_t)off);
}
long read_fake(int fd, void *buf, size_t count) {
  if (fakefd_is_fake(fd)) return fakefd_read(fd, buf, count);
#if PHI_ASSET_PACK
  if (asset_pack_fd_is(fd)) return asset_pack_read_fd(fd, buf, count);
#endif
  __atomic_add_fetch(&g_read_calls, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&g_read_bytes, count, __ATOMIC_RELAXED);
  /* fsdev can return fewer bytes than requested for a large read; il2cpp's
   * global-metadata.dat loader (and others) assume a single read() fills the
   * buffer. Loop until `count` is satisfied or we hit EOF/error so the metadata
   * is never silently truncated (a short read leaves System.Object et al.
   * unresolvable -> Class::Init(NULL)). */
  {   /* cached path: keeps read()'s file-position semantics intact */
    long cur = lseek(fd, 0, SEEK_CUR);
    if (cur >= 0) {
      long got = rc_pread(fd, buf, count, (uint64_t)cur);
      if (got >= 0) { lseek(fd, cur + got, SEEK_SET); return got; }
    }
  }
  size_t total = 0;
  while (total < count) {
    long r = read(fd, (char *)buf + total, count - total);
    if (r < 0) { if (total) break; return -1; }
    if (r == 0) break; /* EOF */
    total += (size_t)r;
  }
  if (count >= (1u << 20))
    debugPrintf("[io] read(fd=%d, %zu) -> %zu%s\n", fd, count, total,
                total < count ? "  *** SHORT READ ***" : "");
  watch_dump("read", fd, (long)count, 0, buf, (long)total);
  return (long)total;
}
long write_fake(int fd, const void *buf, size_t count) {
  if (fakefd_is_fake(fd)) return fakefd_write(fd, buf, count);
  return write(fd, buf, count);
}
int close_fake(int fd) {
#if PHI_ASSET_PACK
  if (asset_pack_fd_is(fd)) return asset_pack_close_fd(fd);
#endif
  rc_mark(fd, 0);                          /* fd numbers are reused: drop its blocks */
  if (fd == g_watch_fd) { debugPrintf("[io] <<< close watched fd=%d\n", fd); g_watch_fd = -1; }
  if (fakefd_is_fake(fd)) return fakefd_close(fd);
  return close(fd);
}
int pipe_fake(int fds[2]) { return fakefd_pipe(fds); }
int poll_fake(void *fds, unsigned long nfds, int timeout) { (void)fds; (void)nfds; (void)timeout; return 0; }
int select_fake(int n, void *r, void *w, void *e, void *t) { (void)n; (void)r; (void)w; (void)e; (void)t; return 0; }

// ---------------------------------------------------------------------------
// networking: online play (Mobage / Silicon Studio servers) is dead. Stub the
// socket layer so connections fail and the engine stays in offline mode.
// ---------------------------------------------------------------------------

int socket_fake(int d, int t, int p) { (void)d; (void)t; (void)p; errno = EAFNOSUPPORT; return -1; }
int connect_fake(int s, const void *a, unsigned l) { (void)s; (void)a; (void)l; errno = ECONNREFUSED; return -1; }
int bind_fake(int s, const void *a, unsigned l) { (void)s; (void)a; (void)l; errno = EACCES; return -1; }
int listen_fake(int s, int b) { (void)s; (void)b; return -1; }
int accept_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; errno = EINVAL; return -1; }
long send_fake(int s, const void *b, size_t l, int f) { (void)s; (void)b; (void)l; (void)f; errno = EPIPE; return -1; }
long recv_fake(int s, void *b, size_t l, int f) { (void)s; (void)b; (void)l; (void)f; return 0; }
long sendto_fake(int s, const void *b, size_t l, int f, const void *a, unsigned al) { (void)s; (void)b; (void)l; (void)f; (void)a; (void)al; errno = EPIPE; return -1; }
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al) { (void)s; (void)b; (void)l; (void)f; (void)a; (void)al; return 0; }
int shutdown_fake(int s, int how) { (void)s; (void)how; return 0; }
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l) { (void)s; (void)lv; (void)n; (void)v; (void)l; return 0; }
int getsockopt_fake(int s, int lv, int n, void *v, void *l) { (void)s; (void)lv; (void)n; (void)v; (void)l; return -1; }
int getsockname_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int getpeername_fake(int s, void *a, void *l) { (void)s; (void)a; (void)l; return -1; }
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res) { (void)node; (void)svc; (void)hints; if (res) *res = NULL; return -2 /* EAI_NONAME */; }
void freeaddrinfo_fake(void *res) { (void)res; }
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f) { (void)a; (void)al; (void)f; if (h && hl) h[0] = 0; if (s && sl) s[0] = 0; return -1; }
int gethostname_fake(char *name, size_t len) { if (name && len) snprintf(name, len, "switch"); return 0; }
void *getservbyname_fake(const char *n, const char *p) { (void)n; (void)p; return NULL; }
unsigned if_nametoindex_fake(const char *n) { (void)n; return 0; }
char *if_indextoname_fake(unsigned i, char *buf) { (void)i; if (buf) buf[0] = 0; return buf; }
static volatile int g_h_errno = 0;
int *__get_h_errno_fake(void) { return (int *)&g_h_errno; }

// ---------------------------------------------------------------------------
// process control: fork/exec/etc. are unavailable; report failure.
// ---------------------------------------------------------------------------

int fork_fake(void) { errno = ENOSYS; return -1; }
int execvp_fake(const char *f, char *const argv[]) { (void)f; (void)argv; errno = ENOSYS; return -1; }
int waitpid_fake(int pid, int *status, int opts) { (void)pid; (void)opts; if (status) *status = 0; errno = ECHILD; return -1; }
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
int getpid_fake(void) { return 1; }
int sched_yield_fake(void) { svcSleepThread(0); return 0; }
// bionic struct passwd layout (pw_dir at +0x20, as the engine derefs).
struct bionic_passwd {
  char *pw_name;     /* 0x00 */
  char *pw_passwd;   /* 0x08 */
  uint32_t pw_uid;   /* 0x10 */
  uint32_t pw_gid;   /* 0x14 */
  char *pw_gecos;    /* 0x18 */
  char *pw_dir;      /* 0x20 */
  char *pw_shell;    /* 0x28 */
};
void *getpwuid_fake(int uid) {
  (void)uid;
  static struct bionic_passwd pw;
  static char nm[] = "switch", sh[] = "/bin/sh", empty[] = "";
  /* pw_dir is filled at CALL time: the data root is resolved at runtime now, so
   * it is not a compile-time constant. */
  static char dir[512];
  snprintf(dir, sizeof dir, "%s", nx_root_nodev());
  pw.pw_name = nm; pw.pw_passwd = empty; pw.pw_uid = 0; pw.pw_gid = 0;
  pw.pw_gecos = empty; pw.pw_dir = dir; pw.pw_shell = sh;
  return &pw;
}

// Unity computes its home/cache dir via getenv("HOME") (then getpwuid fallback).
// Serve the writable game root for HOME/TMPDIR; delegate everything else to newlib.
const char *managed_path(const char *p) {
  if (!p) return p;
  const char *c = strchr(p, ':');
  return (c && c[1] == '/') ? c + 1 : p;     // "sdmc:/switch/.." -> "/switch/.."
}
char *getenv_fake(const char *name) {
  if (name) {
    if (!strcmp(name, "HOME"))   return (char *)managed_path(GAME_HOME);
    if (!strcmp(name, "TMPDIR")) return (char *)managed_path(GAME_HOME);
  }
  return getenv(name);
}
// Report a Unix-rooted cwd ("/switch/zookeeper", no "sdmc:") so managed Path
// APIs don't treat it as relative in Path.Combine. newlib's *internal* cwd is
// unchanged, so relative file resolution still works via the default device.
char *getcwd_fake(char *buf, size_t size) {
  char *r = getcwd(buf, size);
  if (!r) return r;
  const char *c = strchr(r, ':');
  if (c && c[1] == '/') memmove(r, c + 1, strlen(c + 1) + 1);  // drop "sdmc:"
  return r;
}
int getrusage_fake(int who, void *usage) { (void)who; if (usage) memset(usage, 0, 144); return 0; }

// ---------------------------------------------------------------------------
// dlopen/dlsym over the already-loaded modules (no real dynamic loading).
// dlsym lets the engine look up its own exports / our shims.
// ---------------------------------------------------------------------------

void *dlopen_fake(const char *name, int flags) { (void)flags; debugPrintf("dlopen(%s)\n", name ? name : "(self)"); return (void *)0x1; }
int dlclose_fake(void *h) { (void)h; return 0; }
const char *dlerror_fake(void) { return NULL; }
void *dlsym_fake(void *handle, const char *symbol) {
  (void)handle;
  if (!symbol) return NULL;
  /* Firebase SWIG stub resolver (firebase_stub.c) -- see step 2b below. */
  extern void *firebase_stub_lookup(const char *symbol);
  /* NativeAudio playback exports are resolved STRAIGHT THROUGH. An earlier
   * revision returned guarded wrappers here; they mangled arguments because
   * their signatures were guessed. See phigros_jni.c. */
  /* 1) a real export from a loaded module (il2cpp/unity/main) */
  void *p = so_resolve_external(symbol);
  if (p) return p;
  /* 2) one of our libc/GLES/EGL shims (the engine dlopen()s libGLESv2.so etc.
   *    and dlsym()s glGetString/glGetIntegerv, which are shims, not exports) */
  uintptr_t shim = dynlib_find_export(symbol);
  if (shim) { debugPrintf("dlsym(%s) -> %p [shim]\n", symbol, (void *)shim); return (void *)shim; }
  /* 2b) Firebase SWIG P/Invokes. The real Firebase .so files are intentionally
   *     NOT loaded (they crash our loader at boot and, lacking Play Services,
   *     could never report DependencyStatus.Available on a Switch anyway). We
   *     answer the managed SDK's native lookups with trivial stubs so the
   *     dependency check resolves to Available(0) and the bootstrap advances. */
  void *fb = firebase_stub_lookup(symbol);
  if (fb) return fb;
  /* 3) the full GLES/EGL API (~150 entry points) lives in mesa, beyond our
   *    static table -- resolve any gl or egl symbol via eglGetProcAddress. */
  if (!strncmp(symbol, "gl", 2) || !strncmp(symbol, "egl", 3)) {
    p = (void *)eglGetProcAddress(symbol);
    if (p) { debugPrintf("dlsym(%s) -> %p [egl]\n", symbol, p); return p; }
  }
  debugPrintf("dlsym(%s) -> NULL\n", symbol);
  return NULL;
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks, semaphores, timed locks
// ---------------------------------------------------------------------------

typedef struct { RwLock lock; } FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) { FakeRwLock *l = calloc(1, sizeof(*l)); rwlockInit(&l->lock); *storage = l; }
  return *storage;
}
int pthread_rwlock_rdlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; diag_wait_enter(DIAG_W_RWLOCK,l); rwlockReadLock(l); diag_wait_exit(); return 0; }
int pthread_rwlock_wrlock_fake(void **rw) { RwLock *l=&get_rwlock(rw)->lock; diag_wait_enter(DIAG_W_RWLOCK,l); rwlockWriteLock(l); diag_wait_exit(); return 0; }
int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock)) rwlockWriteUnlock(&l->lock);
  else rwlockReadUnlock(&l->lock);
  return 0;
}

/* Round 160. `sem_t` here is just storage: sem_init allocates a FakeSem and
 * writes the pointer into the caller's first 8 bytes, and every other sem_*
 * dereferences that pointer. The only check was `*s != NULL`, which is not a
 * check at all -- any non-zero garbage in those 8 bytes gets dereferenced.
 *
 * A 39-bit-mode log caught it: immediately after
 *   "Cannot create FMOD::Sound instance for audio clip BladeCloudSwipe01"
 * the crash was sem_post_fake -> semaphoreSignal -> mutexLock with
 * far = 0x3465363564613065, which is the ASCII "e0ad5e64" -- a hex string where
 * the FakeSem pointer belonged (the FMOD error text was still live in x13-x15).
 * So FMOD called sem_post on memory that had never been sem_init'd by us, or
 * had since been reused for text.
 *
 * The magic makes that detectable, and the alignment test rejects this exact
 * value with no dereference at all: 0x...65 & 7 == 5. Same rule as the JNI
 * pointer checks -- validate by VALUE first, only then read. */
/* Defined further down with the GC bridge; the semaphore shim sits above it. */
static int nx_addr_readable_shim(uintptr_t a, size_t n);

#define FAKESEM_MAGIC 0x53454d31u   /* 'SEM1' */
typedef struct { uint32_t magic; Semaphore sem; } FakeSem;

/* Slots we have already proved readable. Round 162 audit: the first version of
 * sem_resolve did TWO svcQueryMemory syscalls on EVERY sem_post/sem_wait/
 * sem_trywait. Those are hot paths -- FMOD's mixer posts per audio buffer, and
 * sem_timedwait polls every millisecond, so a 30 s wait would have burned
 * ~60000 syscalls. That is a performance regression I introduced while fixing a
 * crash, which is its own kind of bug.
 *
 * Caching is sound here because the danger is an address that was never ours,
 * not one that stops being mapped: newlib keeps small allocations mapped after
 * free (PHI_POISON_FREE overwrites them with 0xDE in place), so a slot proved
 * readable once stays readable, and a freed FakeSem simply fails the magic test.
 * Racy by design -- a torn read costs at most one redundant syscall. */
#define SEMCACHE_N 16
static void *volatile g_sem_seen[SEMCACHE_N];
static volatile unsigned g_sem_seen_w;

static int sem_slot_known(void **s) {
  for (unsigned i = 0; i < SEMCACHE_N; i++)
    if (g_sem_seen[i] == (void *)s) return 1;
  return 0;
}

/* Resolve a caller's sem_t to one of OUR semaphores, or NULL. Never faults. */
static FakeSem *sem_resolve(void **s) {
  if (!s) return NULL;
  const int known = sem_slot_known(s);
  if (!known && !nx_addr_readable_shim((uintptr_t)s, sizeof(void *))) return NULL;
  FakeSem *fs = (FakeSem *)*s;
  const uintptr_t v = (uintptr_t)fs;
  if (!v || (v & 7u) || v < 0x1000u) return NULL;      /* by value, no deref */
  if (!known) {
    if (!nx_addr_readable_shim(v, sizeof(FakeSem))) return NULL;
    if (fs->magic != FAKESEM_MAGIC) return NULL;       /* don't cache strangers */
    const unsigned w = g_sem_seen_w++;
    g_sem_seen[w % SEMCACHE_N] = (void *)s;
    return fs;
  }
  if (fs->magic != FAKESEM_MAGIC) return NULL;         /* destroyed or reused */
  return fs;
}

/* A sem_t we do not recognise. Doing nothing can stall whoever waits on it;
 * dereferencing it kills the console. Say so, rate-limited, and carry on. */
static void sem_reject(const char *who, void **s) {
  static int n = 0;
  if (n < 8) { n++;
    debugPrintf("[sem] %s on a sem_t we never initialised: slot=%p holds %p "
                "-- ignored (dereferencing it would fault)\n",
                who, (void *)s, (s && nx_addr_readable_shim((uintptr_t)s, 8)) ? *s : NULL); }
}
int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  if (!s) return -1;
  FakeSem *fs = calloc(1, sizeof(*fs));
  if (!fs) { *s = NULL; errno = ENOMEM; return -1; }   /* was an unchecked deref */
  semaphoreInit(&fs->sem, value);
  fs->magic = FAKESEM_MAGIC;      /* last: a racing reader sees it fully built */
  *s = fs;
  return 0;
}
int sem_destroy_fake(void **s) {
  FakeSem *fs = sem_resolve(s);
  if (fs) { fs->magic = 0; *s = NULL; free(fs); }   /* clear first: see free_ref */
  else if (s && *s) { *s = NULL; }                  /* not ours -- drop, never free */
  return 0;
}
int sem_post_fake(void **s) {
  FakeSem *fs = sem_resolve(s);
  if (fs) semaphoreSignal(&fs->sem);
  else if (s && *s) sem_reject("sem_post", s);
  return 0;
}
int sem_wait_fake(void **s) {
  FakeSem *fs = sem_resolve(s);
  if (!fs) { if (s && *s) sem_reject("sem_wait", s); return 0; }
  Semaphore *sm = &fs->sem;
  diag_wait_enter(DIAG_W_SEM, sm); semaphoreWait(sm); diag_wait_exit();
  return 0;
}
int sem_trywait_fake(void **s) {
  FakeSem *fs = sem_resolve(s);
  if (fs && semaphoreTryWait(&fs->sem)) return 0;
  if (!fs && s && *s) sem_reject("sem_trywait", s);
  errno = EAGAIN; return -1;
}
int sem_getvalue_fake(void **s, int *val) {
  FakeSem *fs = sem_resolve(s);
  if (val) *val = fs ? (int)fs->sem.count : 0;
  return 0;
}
// no native timed wait on libnx Semaphore; poll with a short backoff to the
// deadline. The engine uses it as a yield-with-timeout in its task scheduler.
/* Milliseconds from now until an absolute CLOCK_REALTIME deadline, clamped.
 * Round 161: both timed-wait shims used to ignore `abs` entirely and give up
 * after a hard-coded 1000 ms. A caller asking for five seconds got a spurious
 * ETIMEDOUT at one -- and a spurious timeout is not a harmless approximation
 * here: it tells the caller an operation failed that did not. FMOD's
 * "An error occured that wasn't supposed to" (FMOD_ERR_INTERNAL) in the r160
 * log is exactly what a component does when a wait it expected to succeed
 * reports a timeout.
 *
 * A NULL deadline means "wait forever" in POSIX; we cap it rather than hang,
 * because a stuck console is worse than a late timeout. */
static unsigned nx_abs_to_ms(const struct timespec *abs, unsigned cap_ms) {
  if (!abs) return cap_ms;
  struct timespec now;
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) return cap_ms;
  long long ms = (long long)(abs->tv_sec - now.tv_sec) * 1000LL
               + (long long)(abs->tv_nsec - now.tv_nsec) / 1000000LL;
  if (ms <= 0) return 0;
  return (ms > (long long)cap_ms) ? cap_ms : (unsigned)ms;
}

/* Public wrapper so imports.c's pthread_mutex_timedlock can share the logic. */
unsigned nx_abs_to_ms_pub(const struct timespec *abs, unsigned cap_ms) {
  return nx_abs_to_ms(abs, cap_ms);
}

int sem_timedwait_fake(void **s, const struct timespec *abs) {
  /* 30 s cap: long enough that no legitimate wait is cut short, short enough
   * that a lost post cannot wedge the console forever. */
  unsigned ms = nx_abs_to_ms(abs, 30000u);
  if (sem_trywait_fake(s) == 0) return 0;
  while (ms--) {
    svcSleepThread(1000000ull);            // 1 ms
    if (sem_trywait_fake(s) == 0) return 0;
  }
  errno = ETIMEDOUT;
  return -1;
}

/* --- Boehm GC stop-the-world bridge -------------------------------------
 * il2cpp's Boehm GC stops the world by sending every other thread a suspend
 * signal via pthread_kill; each target's signal handler sem_posts an ack and
 * parks in sigsuspend, and GC_stop_world / GC_start_world sem_wait on those
 * acks. POSIX signals are never delivered on Switch (pthread_kill is a no-op),
 * so the acks never arrive and the first collection hangs forever inside
 * GC_stop_world -- the verified boot wall.
 *
 * sem_post/sem_wait themselves work here (real libnx Semaphore underneath), so
 * we make pthread_kill itself post the ack that the never-delivered handler
 * would have posted. Every thread the GC suspends is already parked in our own
 * shim (idle worker / background waits), so not literally suspending them is
 * fine for the brief mark window. The signal numbers, the start-world ack gate
 * and the ack semaphore are all il2cpp globals -- offsets recovered by
 * disassembling this exact 62f2 libil2cpp's GC_stop_world and suspend handler:
 *   suspend signal   = *(int*)(il2cpp + 0x2376cac)   (sigaddset + pthread_kill arg)
 *   restart signal   = *(int*)(il2cpp + 0x2376cb0)
 *   start-world ack? = *(int*)(il2cpp + 0x2376ca8)   (handler's 2nd sem_post gate)
 *   ack semaphore    =  (void**)(il2cpp + 0x25936c0)  (FakeSem* storage; handler
 *                                                       does sem_post on this)
 * Before GC init these globals are zero: suspend/restart sigs read 0 (never
 * match a real signal) and the ack-sem storage is NULL (sem_post_fake no-ops),
 * so this is inert until the GC is actually up. */
uintptr_t g_il2cpp_base = 0;

#include "config.h"   /* PHI_HAVE_GC_BRIDGE */

/* Boehm GC stop-the-world bridge.
 *
 * DERIVED FOR THIS GAME and ENABLED (PHI_HAVE_GC_BRIDGE 1). The four values
 * come from nx_patch_phigros.h, where the derivation is documented: this
 * libil2cpp has exactly two pthread_kill callers (GC_suspend_all and
 * GC_start_world), and all four Boehm sem_* sites agree on one semaphore.
 *
 * If you ever see: black screen, watchdog "last frame=0", UnityMain parked
 * in GC_stop_world -> our sem_wait, and NO "[gc] stop-world suspend sig=N"
 * line in debug.log -- then these offsets have gone stale (game updated?)
 * and need re-deriving. PORTING sec 6d. */
#include "nx_patch_phigros.h"   /* GC_*_OFF_PHI, derived for this game */
#define GC_SUSPEND_SIG_OFF GC_SUSPEND_SIG_OFF_PHI
#define GC_RESTART_SIG_OFF GC_RESTART_SIG_OFF_PHI
#define GC_START_ACK_OFF   GC_START_ACK_OFF_PHI
#define GC_ACK_SEM_OFF     GC_ACK_SEM_OFF_PHI

/* ---- real stop-the-world ------------------------------------------------
 * See round 105. Pause every mutator for the mark window, but only if we can
 * prove afterwards that no paused thread is holding a shim lock the
 * collector might need -- otherwise resume and retry, and after a few
 * failures fall back to the old ack-only behaviour. */
#define GC_MAX_PAUSE 64
static Handle   g_gc_paused[GC_MAX_PAUSE];
static int      g_gc_paused_n = 0;
static int      g_gc_paused_n_last = 0;   /* count in the pause being measured */
static volatile int g_gc_stopped = 0;
static int      g_gc_bailouts = 0;   /* collector needed a paused thread */
static volatile uint64_t g_gc_stop_tick = 0;   /* when the pause began */
static Handle g_gc_collector = 0;              /* who owns the current stop-world */
static pthread_t g_gc_collector_pth;           /* same, cheap to compare */

/* The collector is about to block on a futex while the world is stopped. The
 * wake can only come from a thread we paused, so it will never arrive -- that is
 * the 500..1358 ms wedge in the round-109/110 logs, ending in a forced resume
 * that cut the mark short and corrupted the heap.
 *
 * Bail out instead: un-pause everyone the instant the collector needs one of
 * them. The mark then finishes with the world running, which is unsound in the
 * same way the old ack-only behaviour was -- but it is over in microseconds
 * rather than a second, and it only happens on the collections that actually
 * need a mutator. The other ~32 of 33 still get a real, sound stop-the-world at
 * ~1.4 ms. */
static void gc_resume_all(void);
void gc_bail_if_collector_blocks(volatile int32_t *uaddr, int expected) {
  (void)expected;
  if (!g_gc_stopped) return;                       /* hot path: one load */
  if (!pthread_equal(pthread_self(), g_gc_collector_pth)) return;
  /* Capture BEFORE resuming; log AFTER. Nothing may be logged between the pause
   * and the resume -- a paused thread may hold the log lock. */
  const void *ua = (const void *)uaddr;
  const unsigned uv = uaddr ? (unsigned)*uaddr : 0u;
  gc_resume_all();
  g_gc_bailouts++;
  /* Round 136. This is the one fact the fix needs and the only one we have
   * never printed.
   *
   * With per-thread capture (round 144) this should be rare: threads are paused
   * individually and the collector no longer waits on a bulk pause. It is kept
   * as a last-resort valve because a wedged console is worse than an unsound
   * mark, and it says which futex it was so the cause stays findable.
   *
   * A mark that finishes with mutators running misses references, so live
   * objects get swept and their memory reused -- which is precisely the r134
   * shape: a valid arena pointer whose pointee's klass belongs to something
   * else. Every run that produced that family of corruption had bailouts >= 1;
   * the one boot that had bailouts == 0 crashed somewhere else entirely.
   *
   * For a pthread mutex the futex word holds the owner's TID in its low bits,
   * so `val` names the thread as well as the lock. Once this line identifies
   * the address, the lock can be pre-checked before the capture opens an
   * epoch. */
  debugPrintf("[gc] *** BAILOUT #%d (collector tid=%d): blocked on futex ua=%p val=%08x "
              "with the world stopped -- resumed mid-mark, that collection is "
              "UNSOUND (live objects can be swept). Pre-check this lock in "
              "the futex above. ***\n", g_gc_bailouts, gettid_fake(), ua, uv);
}
/* Pause statistics. "The GC fix causes stuttering" needs a number before it can
 * be acted on: a 3 ms mark is inherent to a stop-the-world collector and is not
 * what anyone would notice, whereas 100 ms is three dropped frames and means
 * something is wrong rather than merely unavoidable. */
static uint64_t g_gc_pause_n = 0, g_gc_pause_ns = 0, g_gc_pause_max_ns = 0;
static volatile int g_gc_forced_resumes = 0;


/* gc_stop_world() and its retry/give-up budget are GONE (round 144). They
 * existed only to bulk-pause every thread from the first pthread_kill, which is
 * what left Boehm scanning stale stack ranges. Threads are now paused one at a
 * time by nx_gc_pause_and_capture(), which also gives the collector the SP and
 * registers it was missing. gc_resume_all() stays: it is the watchdog's rescue
 * path and the restart path's epoch close. */

/* Single-winner resume: the collector and the watchdog both call this. */
static void gc_resume_all(void) {
  if (__atomic_exchange_n(&g_gc_stopped, 0, __ATOMIC_ACQ_REL) == 0)
    return;                       /* the other one already resumed them */
  const uint64_t held = armTicksToNs(armGetSystemTick() - g_gc_stop_tick);
  diag_resume_list(g_gc_paused, g_gc_paused_n);   /* resume FIRST, measure after */
  g_gc_paused_n_last = g_gc_paused_n; /* PHIGROS_GC_REALWAIT_FIX_R1 diag */
  g_gc_paused_n = 0;

  g_gc_pause_n++;
  g_gc_pause_ns += held;
  if (held > g_gc_pause_max_ns) g_gc_pause_max_ns = held;
  /* One line per pause long enough to drop a frame at 60 Hz. Anything shorter is
   * invisible and not worth the SD traffic. */
  if (held > 16000000ull)
    debugPrintf("[gc] pause %llu.%02llu ms (%d threads) -- dropped ~%llu frame(s)\n",
                (unsigned long long)(held / 1000000ull),
                (unsigned long long)((held % 1000000ull) / 10000ull),
                g_gc_paused_n_last, (unsigned long long)(held / 16666667ull) + 1);
}

/* Totals, for the heartbeat: how much of the session went into GC pauses. */
int nx_gc_bailouts(void) { return g_gc_bailouts; }

/* Give-ups are gone with gc_stop_world (round 144): there is no bulk pause to
 * fail any more. What matters now is whether every thread got captured --
 * nx_gc_capture_stats() above. A non-zero miss count means some thread's stack
 * and registers were NOT handed to the mark, which is the old bug returning. */

void nx_gc_pause_stats(unsigned *count, unsigned *total_ms, unsigned *max_ms) {
  if (count)    *count    = (unsigned)g_gc_pause_n;
  if (total_ms) *total_ms = (unsigned)(g_gc_pause_ns / 1000000ull);
  if (max_ms)   *max_ms   = (unsigned)(g_gc_pause_max_ns / 1000000ull);
}


/* Called from the WATCHDOG thread, which is created with threadCreate() and
 * never registered with diag, so it is never one of the paused threads.
 * If a mark window overruns -- the likely cause being a paused thread holding
 * a lock the collector needs, i.e. a deadlock -- force everyone back to
 * runnable. The collection is then unsound and the process may still fault,
 * but that is exactly the old behaviour; a wedged console is not. */
void nx_gc_stopworld_watchdog(void) {
  if (!g_gc_stopped) return;
  const uint64_t held_ns = armTicksToNs(armGetSystemTick() - g_gc_stop_tick);
  /* 60 ms, was 500. gc_bail_if_collector_blocks() should now catch the case that
   * produced every wedge so far (collector blocking on a futex a paused thread
   * must service), so this is a backstop for something unforeseen -- and when a
   * backstop fires it should cost 4 frames, not 80. */
  if (held_ns < 60000000ull) return;

  /* Say WHERE the collector is stuck BEFORE letting it go -- once gc_resume_all()
   * runs the wedge is gone and the evidence with it. Two rounds have been spent
   * guessing which lock this is (round 100: mmap + log; round 108: newlib
   * malloc) and it is still wedging, so stop guessing and read it off the thread.
   * For a thread parked in svcArbitrateLock, X1 is the mutex address and X0 the
   * owner's handle tag: that names both the lock and who holds it. */
  diag_dump_thread(g_gc_collector, "GC-collector WEDGED");

  gc_resume_all();
  g_gc_forced_resumes++;
  /* A note, not a debugPrintf: this must appear in a DEBUG_LOG 0 build. It also
   * means the collection was cut short, so the heap may now be inconsistent --
   * exactly the sort of thing worth seeing just before a later fault. */
  debugLogNote("[gc] *** stop-world held %llu ms -- FORCING resume (#%d). A paused "
               "thread probably holds a lock the collector needs; that collection "
               "was cut short. ***\n",
               (unsigned long long)(held_ns / 1000000ull), g_gc_forced_resumes);
}

/* ---- per-thread suspend capture (round 144) -------------------------------
 *
 * Boehm is a CONSERVATIVE collector: the only reference to an object may live in
 * a callee-saved register or a live stack frame. On Android it gets that from a
 * signal -- GC_stop_world sends SIG_SUSPEND, the handler runs ON the mutator,
 * spills its registers, records its current SP in thread->stack_ptr, posts the
 * ack, and GC_push_all_stacks then scans [stack_ptr, stack_base).
 *
 * Horizon delivers no POSIX signals, so that handler never runs. The old bridge
 * ignored pthread_kill's `t` argument entirely and bulk-paused every thread on
 * the first call. The pause was real -- but stack_ptr was never written, so the
 * mark walked a STALE range with no registers, and an object reachable only from
 * a paused thread's frame or a register was invisible to it. Swept, memory
 * reused, and a surviving pointer then addressed a live object of the wrong
 * type: rounds 119/128/131/134/137, all one bug.
 *
 * So do the handler's job ourselves, per thread. Offsets and struct layout were
 * derived from THIS libil2cpp (see nx_patch_phigros.h) and are checked by
 * tools/verify_offsets.py.
 *
 * NO LOGGING in here. We return with the target still paused, and if it happens
 * to hold the debug-log lock, logging would deadlock against it. Failures go to
 * counters that the heartbeat prints. */

typedef struct NxGcThread {
  struct NxGcThread *next;              /* +0x00 */
  pthread_t          id;                /* +0x08 */
  volatile uintptr_t last_stop_count;   /* +0x10 */
  volatile uintptr_t stack_ptr;         /* +0x18 */
} NxGcThread;

static unsigned g_gc_cap_ok, g_gc_cap_nothread, g_gc_cap_nohandle,
                g_gc_cap_noctx, g_gc_cap_badroots;

/* GC_lookup_thread's index, read off our own binary at 0x160ea98:
 *     lsr x9,x0,#8 ; eor w8,w9,w1 ; eor w8,w8,w8,lsr#16 ; and x8,x8,#0xff
 * The eors are 32-bit there; folding in 64 bits gives the same low 8 bits, so
 * either form works -- this mirrors the instructions. */
static unsigned nx_gc_thread_index(pthread_t id) {
  uint32_t x = (uint32_t)(((uintptr_t)id >> 8) ^ (uintptr_t)id);
  x ^= x >> 16;
  return (unsigned)(x & 0xffu);
}

static NxGcThread *nx_gc_find_thread(pthread_t id) {
  if (!g_il2cpp_base || !id) return NULL;
  NxGcThread **table = (NxGcThread **)(g_il2cpp_base + GC_THREADS_OFF_PHI);
  NxGcThread *p = __atomic_load_n(&table[nx_gc_thread_index(id)], __ATOMIC_ACQUIRE);
  for (int guard = 0; p && guard < 1024; guard++, p = p->next)
    if (p->id == id) return p;          /* chain walk, bounded: never trust it */
  return NULL;
}

/* Pause one thread and spill its registers where the mark will find them.
 * Leaves the thread PAUSED on success -- gc_start_world's restart path resumes
 * it. Returns 0, or an errno for pthread_kill to hand back to Boehm. */
static int nx_gc_pause_and_capture(pthread_t id, NxGcThread *gc_thread) {
  const Handle h = diag_handle_for_pthread(id);
  if (!h) { g_gc_cap_nohandle++; return ESRCH; }
  if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused))) {
    g_gc_cap_nohandle++; return ESRCH;
  }
  ThreadContext ctx;
  memset(&ctx, 0, sizeof ctx);
  if (R_FAILED(svcGetThreadContext3(&ctx, h)) || (ctx.psr & 0x10u) || ctx.sp < 0x1000) {
    svcSetThreadActivity(h, ThreadActivity_Runnable);   /* AArch32 or no context */
    g_gc_cap_noctx++; return ESRCH;
  }

  /* Scratch just BELOW the live sp. Putting it there is the whole trick: the
   * stack grows down, so a scan from here upwards covers the spilled registers
   * AND every live frame. Writing above sp would clobber nothing useful and
   * cover nothing. */
  enum { ROOT_WORDS = 34 };
  const uintptr_t roots_addr =
      (uintptr_t)(ctx.sp - ROOT_WORDS * sizeof(uint64_t)) & ~(uintptr_t)0xf;
  MemoryInfo mi; u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, roots_addr)) ||
      (mi.perm & Perm_W) == 0 ||
      roots_addr < (uintptr_t)mi.addr ||
      (uintptr_t)ctx.sp > (uintptr_t)mi.addr + (uintptr_t)mi.size) {
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    g_gc_cap_badroots++; return ESRCH;   /* guard page or sp near a boundary */
  }

  uint64_t *roots = (uint64_t *)roots_addr;
  for (int i = 0; i < 29; i++) roots[i] = ctx.cpu_gprs[i].x;
  roots[29] = ctx.fp;
  roots[30] = ctx.lr;
  roots[31] = ctx.sp;
  roots[32] = ctx.pc.x;
  roots[33] = ctx.tpidr;
  __atomic_store_n(&gc_thread->stack_ptr, roots_addr, __ATOMIC_RELEASE);

  /* Register the handle so the watchdog can still rescue a wedged collection.
   * Threads are now paused one at a time and released by the restart signal; if
   * the collector dies between the two, they would stay paused forever. Opening
   * the epoch on the first capture keeps that safety net working. */
  if (!g_gc_stopped) {
    g_gc_paused_n = 0;
    g_gc_stop_tick = armGetSystemTick();
    g_gc_collector = diag_self_handle();
    g_gc_collector_pth = pthread_self();
    g_gc_stopped = 1;
  }
  if (g_gc_paused_n < GC_MAX_PAUSE) g_gc_paused[g_gc_paused_n++] = h;
  g_gc_cap_ok++;
  return 0;
}

void nx_gc_capture_stats(unsigned *ok, unsigned *no_thread, unsigned *no_handle,
                         unsigned *no_ctx, unsigned *bad_roots) {
  if (ok)        *ok        = g_gc_cap_ok;
  if (no_thread) *no_thread = g_gc_cap_nothread;
  if (no_handle) *no_handle = g_gc_cap_nohandle;
  if (no_ctx)    *no_ctx    = g_gc_cap_noctx;
  if (bad_roots) *bad_roots = g_gc_cap_badroots;
}

/* Boehm's stop/start-the-world, per thread. `t` is the thread being signalled
 * and it MATTERS -- the round-143 bug was `(void)t;` here. */
/* ---- managed-throw log via __cxa_throw interposition (round 150) ----------
 *
 * Every managed exception in il2cpp becomes a C++ throw: il2cpp_raise_exception
 * (@0x15ef8a8) is three instructions that tail into 0x159c1cc, which does
 * `__cxa_allocate_exception(8)`, stores the Il2CppException* into it, and
 * throws. That call goes through libil2cpp's **PLT** (0x30b3960 is inside
 * .plt), `__cxa_throw` is a DEFINED-and-therefore-preemptible symbol
 * (.dynsym @0x165e858), and so_resolve_symbol prefers our shim table over
 * module exports. So we can interpose it from imports.c with **no code
 * patching at all** -- no thunk, no derived offset, nothing to go stale.
 *
 * Which matters, because the obvious alternative is a trap: the exported
 * il2cpp_raise_exception is only 12 bytes -- il2cpp_exception_from_name_msg is
 * exported at 0x15ef8b4, four bytes into it -- so a 16-byte hook there would
 * silently overwrite the next function.
 *
 * Why bother: the r147 log has three NullReferenceExceptions, one 68 lines
 * before the crash, and Unity printed the message with NO stack trace. And the
 * r134 wild write faulted inside a cleanup LANDING PAD, i.e. during unwinding
 * from a throw. Naming every throw connects those two, and tests directly
 * whether the game is throwing in paths that only exist because we are offline.
 *
 * The exception object: __cxa_throw's first argument points at the allocated
 * C++ exception, whose first word is the Il2CppException* (`str x19,[x0]` at
 * 0x159c1ec). Its klass is at [0], and klass->name at klass+0x10. */
/* Local readability probe: this walks pointers out of a live C++ exception, so
 * every hop has to be checked before it is followed. */
static int nx_addr_readable_shim(uintptr_t a, size_t n) {
  MemoryInfo mi; u32 pi;
  if (!a || a < 0x1000) return 0;
  if (R_FAILED(svcQueryMemory(&mi, &pi, a))) return 0;
  if (mi.type == MemType_Unmapped || !(mi.perm & Perm_R)) return 0;
  return (a + n) <= ((uintptr_t)mi.addr + (uintptr_t)mi.size);
}

static void (*g_real_cxa_throw)(void *, void *, void (*)(void *)) = NULL;
static unsigned g_throw_n = 0;

__attribute__((noreturn))
void nx_cxa_throw(void *ex, void *tinfo, void (*dtor)(void *)) {
  g_throw_n++;
  /* Rate-limited and deduped by type name: a throw storm must not turn into an
   * SD-write storm. NO allocation and no locks here beyond debugPrintf's -- we
   * are on the throwing thread with a live exception in flight. */
  if (ex) {
    const char *name = NULL;
    if (nx_addr_readable_shim((uintptr_t)ex, 8)) {
      const void *mex = *(void *const *)ex;                 /* Il2CppException* */
      if (mex && nx_addr_readable_shim((uintptr_t)mex, 8)) {
        const uintptr_t klass = (*(const uintptr_t *)mex) & ~(uintptr_t)1;
        if (klass && nx_addr_readable_shim(klass, 0x20)) {
          const char *n = *(const char *const *)(klass + 0x10);
          if (n && nx_addr_readable_shim((uintptr_t)n, 1)) name = n;
        }
      }
    }
    static const char *seen[24]; static int nseen = 0;
    int dup = 0;
    for (int i = 0; i < nseen; i++)
      if (seen[i] == name || (seen[i] && name && !strcmp(seen[i], name))) { dup = 1; break; }
    if (!dup && nseen < 24) {
      seen[nseen++] = name;
      debugPrintf("[throw] managed exception #%u: %s\n",
                  g_throw_n, name ? name : "(type unreadable)");
    }
  }
  if (!g_real_cxa_throw) {
    extern so_module il2cpp_mod;
    g_real_cxa_throw = (void (*)(void *, void *, void (*)(void *)))
                       so_find_addr_rx(&il2cpp_mod, "__cxa_throw");
  }
  if (g_real_cxa_throw) g_real_cxa_throw(ex, tinfo, dtor);
  /* Unreachable: __cxa_throw never returns. If it ever did, or if the lookup
   * failed, stopping here is far better than falling off a noreturn function. */
  debugPrintf("[throw] *** __cxa_throw unavailable -- cannot propagate ***\n");
  for (;;) svcSleepThread(1000000000ULL);
}

unsigned nx_throw_count(void) { return g_throw_n; }

int pthread_kill_gc(pthread_t t, int sig) {
#if !PHI_HAVE_GC_BRIDGE
  /* Bridge off: every offset below is unverified for this game, and the suspend
   * path would sem_post through a pointer loaded from one of them. No-op. */
  (void)t; (void)sig;
  return 0;
#else
  uintptr_t b = g_il2cpp_base;
  if (b && sig) {
    int suspend_sig = *(volatile int *)(b + GC_SUSPEND_SIG_OFF);
    int restart_sig = *(volatile int *)(b + GC_RESTART_SIG_OFF);
    void **ack_sem  = (void **)(b + GC_ACK_SEM_OFF);

    if (sig == suspend_sig) {
      /* Boehm calls this ONCE PER THREAD. Do what its handler would have done
       * for exactly this thread, then ack for it. */
      static int logged_s = 0;
      if (!logged_s) { logged_s = 1;                 /* before any pause */
        void *slot = *ack_sem;
        debugPrintf("[gc] per-thread capture armed: sig=%d ack@il2cpp+0x%x "
                    "slot=%p %s  GC_threads@+0x%x\n",
                    sig, (unsigned)GC_ACK_SEM_OFF, slot,
                    slot ? "(ok)" : "*** NULL -- GC_ACK_SEM_OFF is WRONG ***",
                    (unsigned)GC_THREADS_OFF_PHI); }

      NxGcThread *gt = nx_gc_find_thread(t);
      if (!gt) { g_gc_cap_nothread++; return ESRCH; } /* not a GC thread */
      int err = nx_gc_pause_and_capture(t, gt);
      if (err) return err;
      /* Tell Boehm this thread acked for THIS stop epoch, exactly as the
       * handler does (stlr me->last_stop_count, GC_stop_count at +0xb8). */
      const uintptr_t stop_count = __atomic_load_n(
          (volatile uintptr_t *)(b + GC_STOP_COUNT_OFF_PHI), __ATOMIC_ACQUIRE);
      __atomic_store_n(&gt->last_stop_count, stop_count, __ATOMIC_RELEASE);
      sem_post_fake(ack_sem);
      return 0;
    }

    if (sig == restart_sig) {
      /* RESUME FIRST, look things up after. Bailing out before the resume --
       * which an earlier draft of this did -- leaves a thread paused forever if
       * its GC_threads entry went away between suspend and restart. A thread
       * that is already runnable takes this harmlessly. */
      const Handle h = diag_handle_for_pthread(t);
      if (h) svcSetThreadActivity(h, ThreadActivity_Runnable);
      if (g_gc_stopped) gc_resume_all();   /* single-winner; resumes then measures */
      NxGcThread *gt = nx_gc_find_thread(t);
      if (!gt || !h) return ESRCH;
      /* GC_retry_signals (+0x680): when set, the handler acks a SECOND time on
       * restart with last_stop_count | 1 (orr x8,x20,#1 at 0x160eb78). */
      if (*(volatile int *)(b + GC_RETRY_SIGNALS_OFF_PHI)) {
        const uintptr_t stop_count = __atomic_load_n(
            (volatile uintptr_t *)(b + GC_STOP_COUNT_OFF_PHI), __ATOMIC_ACQUIRE);
        __atomic_store_n(&gt->last_stop_count, stop_count | 1u, __ATOMIC_RELEASE);
        sem_post_fake(ack_sem);
      }
      return 0;
    }
  }
  return 0;   /* any other signal: no-op, as before */
#endif
}
