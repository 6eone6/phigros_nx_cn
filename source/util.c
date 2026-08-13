/* util.c -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>      /* open(), O_WRONLY/O_CREAT/O_APPEND -- malloc-free logging */

#include "util.h"
#include "nx_root.h"
#include "config.h"

/* ===========================================================================
 * File logging.
 *
 * DEBUG_LOG 0 means ABSOLUTE SILENCE: debug.log is never created, not even by
 * a crash. The whole block below is compiled out and every entry point becomes
 * an empty stub, so there is no file, no formatting cost and no SD traffic of
 * any kind. DEBUG_LOG 1 restores normal logging.
 * =========================================================================== */
#if DEBUG_LOG

/* Log buffer state. g_log_buf accumulates formatted lines; log_flush_locked
 * writes it out in one syscall. debugLogFlush() is the public, lock-taking
 * flush the crash handler calls so a fault never loses the buffered tail. */
static Mutex g_log_lock;    // libnx Mutex: 0 == unlocked, no init needed
static int  g_log_fd = -1;
static char g_log_buf[8192];
static int  g_log_used = 0;
static void log_flush_locked(void) {
  int off = 0;
  while (off < g_log_used) {
    ssize_t w = write(g_log_fd, g_log_buf + off, (size_t)(g_log_used - off));
    if (w <= 0) break;
    off += (int)w;
  }
  g_log_used = 0;
}
void debugLogFlush(void) {
  mutexLock(&g_log_lock);
  if (g_log_fd >= 0) log_flush_locked();
  mutexUnlock(&g_log_lock);
}

/* Flush AND close, so the bytes are committed to the card rather than left
 * in fsdev with the size not yet written back. For the crash path only: the
 * process is about to abort, so the close costs nothing and the next
 * debugPrintf would reopen anyway (g_log_fd is reset to -1). */
/* For the GC stop-the-world check: prove the log lock is not held by a
 * thread we just paused. Returns nonzero if acquired (caller must unlock). */
int debugLogTryLock(void) { return mutexTryLock(&g_log_lock); }
void debugLogUnlock(void) { mutexUnlock(&g_log_lock); }

void debugLogClose(void) {
  mutexLock(&g_log_lock);
  if (g_log_fd >= 0) {
    log_flush_locked();
    fsync(g_log_fd);
    close(g_log_fd);
    g_log_fd = -1;
  }
  mutexUnlock(&g_log_lock);
}

// Thread-safe, file-only logger. Lines are formatted with vsnprintf into a
// fixed buffer (no heap -- must not allocate under g_log_lock) and coalesced
// into g_log_buf, flushed in one write() on overflow or via debugLogFlush().
// Serialised with a mutex because the engine logs from several worker
// threads. No nxlink/socket: this must work on bare hardware.
// The log lands in the game dir (main() chdir()s there at startup).
/* g_log_lock is declared at the top of the log-buffer block above. */

/* Malloc-free logger. CRITICAL: debugPrintf is called from inside the
 * allocator wrappers (canary/quarantine/gpu-fail reports) and from dlsym
 * during FMOD init, so it must NOT allocate while holding g_log_lock -- if
 * it did, g_log_lock would invert against the allocator lock and deadlock
 * (observed: FMOD OutputOpenSL::init -> dlsym_fake -> debugPrintf parked on
 * g_log_lock while the whole engine stalled). newlib fopen/vfprintf/fflush
 * all malloc; vsnprintf into a fixed buffer + write() does not. */
static volatile int g_log_on = 1;

/* Called by the exception handler so a fault is always recorded. */
void debugLogForceOn(void) { g_log_on = 1; }

/* A rare but important event -- written even in a DEBUG_LOG 0 release build.
 * Reserved for things that happen at most a handful of times in a session
 * (memory pressure, a forced GC resume), never anything per-frame: this writes
 * straight through to the card. Flushed immediately, because the interesting
 * cases are the ones followed by a crash. */
int debugLogNote(char *text, ...) {
  const int was = g_log_on;
  g_log_on = 1;
  va_list ap; va_start(ap, text);
  char line[512];
  int n = vsnprintf(line, sizeof line, text, ap);
  va_end(ap);
  if (n > 0) debugPrintf("%s", line);
  debugLogFlush();
  g_log_on = was;
  return n;
}

int debugPrintf(char *text, ...) {
  if (!g_log_on) return 0;
  {
  static char line[2048];
  va_list list;
  mutexLock(&g_log_lock);
  if (g_log_fd < 0) g_log_fd = open(LOG_NAME, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (g_log_fd >= 0) {
    va_start(list, text);
    int n = vsnprintf(line, sizeof line, text, list);   // no heap for %d/%s/%p/%x
    va_end(list);
    if (n > 0) {
      if (n > (int)sizeof line - 1) n = (int)sizeof line - 1;   // truncated, not overflowed
      /* Coalesce into g_log_buf; flush in one write() when it would overflow.
       * Cuts SD syscalls ~50-100x vs per-line write. The exception handler
       * calls debugLogFlush() so a crash dump is never left in the buffer. */
      if (g_log_used + n > (int)sizeof g_log_buf) log_flush_locked();
      if (n > (int)sizeof g_log_buf) {            // single line bigger than buffer
        ssize_t off = 0;
        while (off < n) {
          ssize_t w = write(g_log_fd, line + off, (size_t)(n - off));
          if (w <= 0) break;
          off += w;
        }
      } else {
        memcpy(g_log_buf + g_log_used, line, (size_t)n);
        g_log_used += n;
      }
    }
  }
  mutexUnlock(&g_log_lock);
  }
  return 0;
}

#else   /* DEBUG_LOG 0 -- silent build: nothing is ever opened or written */

int  debugPrintf(char *text, ...)   { (void)text; return 0; }
int  debugLogNote(char *text, ...)  { (void)text; return 0; }
void debugLogForceOn(void)          { }
void debugLogFlush(void)            { }
void debugLogClose(void)            { }
/* The GC stop-the-world safety check asks whether the log lock is free before
 * it trusts a pause. With logging compiled out the lock does not exist and can
 * never be held, so the honest answer is "free". */
int  debugLogTryLock(void)          { return 1; }
void debugLogUnlock(void)           { }

#endif  /* DEBUG_LOG */

// Per-thread bionic TLS. The engine reads its stack canary from tpidr_el0+0x28;
// every thread that runs engine code needs its OWN zeroed block here. A single
// shared block races: one thread's TLS writes (including the guard slot) corrupt
// another thread's in-flight canary, tripping a false __stack_chk_fail. `buf`
// must outlive the thread (TPIDR_EL0 points into it until the thread exits).
void install_bionic_tls(void *buf) {
  memset(buf, 0, BIONIC_TLS_SIZE);
  armSetTlsRw((uint8_t *)buf + BIONIC_TLS_TP_OFFSET);
}

// boost the CPU to 1785MHz while loading
void cpu_boost(int on) {
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

int ret0(void) { return 0; }

int retm1(void) { return -1; }
