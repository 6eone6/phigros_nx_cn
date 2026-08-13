/* util.h -- misc utility functions
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __UTIL_H__
#define __UTIL_H__

#include <stdint.h>

int debugPrintf(char *text, ...);
void debugLogForceOn(void); /* enable logging even in a DEBUG_LOG 0 build      */
int  debugLogNote(char *text, ...);  /* rare important event; always written    */
void debugLogFlush(void);   /* force-flush the log buffer (crash handler) */
void debugLogClose(void);   /* flush + fsync + close: crash path only     */
int  debugLogTryLock(void); /* GC stop-world safety check                 */
void debugLogUnlock(void);

void cpu_boost(int on);

// libff4.so reads its stack-protector canary from tpidr_el0 + 0x28.

int ret0(void);
int retm1(void);

/* Point TPIDR_EL0 at a zeroed per-thread block so the engine's stack-protector
 * prologues (which read the canary from TPIDR_EL0+0x28) have a valid, stable
 * bionic TLS. `buf` must outlive the thread. libnx keeps its own thread state in
 * TPIDR_RO_EL0, so commandeering TPIDR_EL0 is safe. EACH THREAD NEEDS ITS OWN
 * BLOCK -- sharing one corrupts the guard slot across threads. */
#define BIONIC_TLS_SIZE 0x400
#define BIONIC_TLS_TP_OFFSET 0x200   /* tp points into the block: headroom for any negative bionic TLS slots */
void install_bionic_tls(void *buf);

static inline void* armGetTlsRw(void) {
  void* ret;
  __asm__ ("mrs %x[data], s3_3_c13_c0_2" : [data] "=r" (ret));
  return ret;
}

static inline void armSetTlsRw(void *addr) {
  __asm__  ("msr s3_3_c13_c0_2, %0" : : "r"(addr));
}

static inline uint64_t umin(uint64_t a, uint64_t b) {
  return (a < b) ? a : b;
}

#endif
