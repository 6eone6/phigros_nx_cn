/* imports_phigros_plugin.c -- the last 17 undefined symbols.
 *
 * PROVENANCE
 * Diffing every undefined symbol of libmain.so + libunity.so + libil2cpp.so
 * against every resolver table in the tree gives 486 distinct symbols, 484 of
 * which are already covered. That two-symbol gap is a direct consequence of
 * the version match: this game is Unity 2022.3.62f2, the exact revision the
 * loader core was built against. (Fruit Ninja, 62 patch releases earlier, was
 * missing 70.)
 *
 *   CORE (libmain + libunity + libil2cpp)  -- 2 symbols
 *     AImage_getWidth   dead: the AImageReader/Camera2 capture path is
 *                       registered by libunity but never driven. Grouped with
 *                       the other AImage_* stubs in imports_phigros_extra.c,
 *                       which simply did not happen to include the width
 *                       getter. Pure link-satisfying stub.
 *     strcasestr        real, and implemented below. newlib has it behind
 *                       _GNU_SOURCE but the loader core does not export it.
 *
 *   PLUGIN (libnativeaudioe7 + libtapsdkcore)  -- 15 symbols
 *     The four SL_IID_* interface IDs NativeAudio needs are ALREADY provided
 *     by opensles.c's SL_IID(n) macro table -- they are data symbols, easy to
 *     miss when grepping for resolver entries, and they do not belong here.
 *
 *     epoll_* / eventfd  TapSDK's event loop. libtapsdkcore.so is
 *                        deliberately NOT loaded (see main.c), so in the
 *                        shipping configuration these are never referenced.
 *                        They are provided anyway so that anyone who does try
 *                        to load it gets a clean failure instead of an
 *                        unresolved-symbol abort during relocation. They fail
 *                        rather than pretend: an epoll_wait that returns 0
 *                        forever is a spin, and a spin is worse than an error.
 *
 *     the rest           real libc, implemented properly.
 *
 * Every stub logs once so debug.log tells you if one is unexpectedly live.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/statvfs.h>

#include "so_util.h"   /* DynLibFunction */
#include "util.h"      /* debugPrintf   */

#define NOTSUP1(sym) do {                                                 \
    static int _w = 0;                                                    \
    if (!_w) { _w = 1;                                                    \
      debugPrintf("[stub] %s called -- unsupported on Switch\n", sym); }   \
  } while (0)

/* ---- core gap ---------------------------------------------------------- */

/* AImageReader path is never driven; see the AImage_* group in
 * imports_phigros_extra.c. Returning 0 keeps any accidental caller from
 * dividing by a garbage width. */
static int32_t AImage_getWidth_fake(void *image, int32_t *width) {
  (void)image;
  NOTSUP1("AImage_getWidth");
  if (width) *width = 0;
  return -1;   /* AMEDIA_ERROR_UNKNOWN-ish; any non-zero is "failed" */
}

/* Real implementation. Case-insensitive strstr; returns haystack if needle is
 * empty, which is what glibc/BSD do. */
static char *strcasestr_impl(const char *h, const char *n) {
  if (!h || !n) return NULL;
  if (!*n) return (char *)h;
  const size_t nl = strlen(n);
  for (; *h; h++) {
    if (tolower((unsigned char)*h) == tolower((unsigned char)*n) &&
        strncasecmp(h, n, nl) == 0)
      return (char *)h;
  }
  return NULL;
}

/* ---- plugin gap: real libc -------------------------------------------- */

static long lrint_impl(double x) { return (long)rint(x); }

/* getentropy: the Switch has a CSPRNG. randomGet() is libnx's; if a future
 * libnx renames it this is the one line to fix. */
extern void randomGet(void *buf, size_t len);
static int getentropy_impl(void *buf, size_t len) {
  if (!buf) { errno = EFAULT; return -1; }
  if (len > 256) { errno = EIO; return -1; }   /* POSIX cap */
  randomGet(buf, len);
  return 0;
}

/* fstatfs -> statvfs. Only the free/total fields are ever read (TapSDK sizes a
 * cache directory with it), and those map across directly. */
struct nx_statfs {
  uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail;
  uint64_t f_files, f_ffree, f_fsid, f_namelen, f_frsize, f_flags, f_spare[4];
};
static int fstatfs_impl(int fd, struct nx_statfs *out) {
  (void)fd;
  if (!out) { errno = EFAULT; return -1; }
  memset(out, 0, sizeof *out);
  out->f_bsize = out->f_frsize = 4096;
  out->f_namelen = 255;
  NOTSUP1("fstatfs (reporting an empty filesystem)");
  return 0;
}

/* posix_fadvise is advisory by definition -- succeeding while doing nothing is
 * a conforming implementation, not a stub. */
static int posix_fadvise_impl(int fd, long off, long len, int advice) {
  (void)fd; (void)off; (void)len; (void)advice;
  return 0;
}

/* pause(): sleep until a signal that will never arrive. Blocking forever here
 * would wedge whichever thread called it, so cap it and return EINTR -- the
 * caller's loop then gets a chance to notice shutdown. */
static int pause_impl(void) {
  NOTSUP1("pause");
  usleep(16000);            /* one frame, not forever */
  errno = EINTR;
  return -1;
}

static int pthread_rwlock_destroy_impl(pthread_rwlock_t *l) {
  (void)l;
  return 0;
}

/* ---- plugin gap: TapSDK event loop (never loaded; fail cleanly) -------- */

static int epoll_create_fake(int size) {
  (void)size; NOTSUP1("epoll_create"); errno = ENOSYS; return -1;
}
static int epoll_create1_fake(int flags) {
  (void)flags; NOTSUP1("epoll_create1"); errno = ENOSYS; return -1;
}
static int epoll_ctl_fake(int ep, int op, int fd, void *ev) {
  (void)ep; (void)op; (void)fd; (void)ev;
  NOTSUP1("epoll_ctl"); errno = ENOSYS; return -1;
}
/* Returning 0 would be a busy-spin at the call site. -1/ENOSYS makes the
 * caller's error path run instead, which is the outcome we want. */
static int epoll_wait_fake(int ep, void *ev, int max, int timeout) {
  (void)ep; (void)ev; (void)max; (void)timeout;
  NOTSUP1("epoll_wait"); errno = ENOSYS; return -1;
}
static int eventfd_fake(unsigned initval, int flags) {
  (void)initval; (void)flags;
  NOTSUP1("eventfd"); errno = ENOSYS; return -1;
}

/* ---- table ------------------------------------------------------------- */
DynLibFunction phigros_plugin_functions[] = {
  /* core gap */
  { "AImage_getWidth",         (uintptr_t)&AImage_getWidth_fake },
  { "strcasestr",              (uintptr_t)&strcasestr_impl },
  /* real libc */
  { "lrint",                   (uintptr_t)&lrint_impl },
  { "getentropy",              (uintptr_t)&getentropy_impl },
  { "fstatfs",                 (uintptr_t)&fstatfs_impl },
  { "posix_fadvise",           (uintptr_t)&posix_fadvise_impl },
  { "pause",                   (uintptr_t)&pause_impl },
  { "pthread_rwlock_destroy",  (uintptr_t)&pthread_rwlock_destroy_impl },
  /* TapSDK event loop */
  { "epoll_create",            (uintptr_t)&epoll_create_fake },
  { "epoll_create1",           (uintptr_t)&epoll_create1_fake },
  { "epoll_ctl",               (uintptr_t)&epoll_ctl_fake },
  { "epoll_wait",              (uintptr_t)&epoll_wait_fake },
  { "eventfd",                 (uintptr_t)&eventfd_fake },
};
size_t phigros_plugin_numfunctions =
    sizeof(phigros_plugin_functions) / sizeof(phigros_plugin_functions[0]);
