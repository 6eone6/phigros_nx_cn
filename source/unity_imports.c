/* unity_imports.c -- supplementary import table for the Unity/IL2CPP engine.
 *
 * libunity/libil2cpp import ~138 symbols cr3's table didn't provide. so_resolve
 * poisons anything unresolved, so a constructor calling one crashes during
 * init_array. Here we map them: real newlib/libm/EGL/zlib functions, our own
 * ANativeWindow/ALooper/ASensor implementations (android_native_unity.c), and
 * benign stubs for Android/Linux-only calls the game does not actually need.
 * Resolved as a second table by crx_resolve_imports().
 */
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <time.h>
#include <signal.h>
#include <EGL/egl.h>
#include <zlib.h>
#include <switch.h>
#include <unistd.h>      /* dup */
#include "imports.h"   /* DynLibFunction */
#include "diag.h"

extern int debugPrintf(char *text, ...);  /* util.c */
extern long z_lseek(int fd, long off, int whence);  /* libc_shim.c */
extern int  sem_getvalue_fake(void **s, int *val);  /* libc_shim.c */

/* our own implementations (android_native_unity.c); generic decls just to take
 * the address -- the real signatures live in that .c and resolve at link time */
extern void ALooper_acquire();
extern void ALooper_forThread();
extern void ALooper_release();
extern void ALooper_wake();
extern void ANativeWindow_acquire();
extern void ANativeWindow_fromSurface();
extern void ANativeWindow_getHeight();
extern void ANativeWindow_getWidth();
extern void ANativeWindow_release();
extern void ASensorEventQueue_hasEvents();
extern void ASensorManager_createEventQueue();
extern void ASensorManager_destroyEventQueue();
extern void ASensorManager_getDefaultSensor();
extern void ASensorManager_getInstance();
extern void ASensorManager_getSensorList();
extern void ASensor_getMinDelay();
extern void ASensor_getName();
extern void ASensor_getResolution();
extern void ASensor_getType();
extern void ASensor_getVendor();

/* _ctype_ table: char-class flags indexed directly by byte value (modern
 * bionic style). Padded to 384 so (_ctype_+1)[255] style access stays in-bounds.
 * Filled at startup so engine ctype lookups classify correctly. */
char z_ctype[384];
__attribute__((constructor)) static void z_ctype_init(void){
  for(int c=0;c<256;c++){
    unsigned char f=0;
    if(isupper(c))  f|=0x01;
    if(islower(c))  f|=0x02;
    if(isdigit(c))  f|=0x04;
    if(isspace(c))  f|=0x08;
    if(ispunct(c))  f|=0x10;
    if(iscntrl(c))  f|=0x20;
    if(isxdigit(c)) f|=0x40;
    if(c==' ')      f|=0x80;
    z_ctype[c]=(char)f;
  }
}

/* generic no-op stub: returns 0 (== success for the *attr_init/etc. callers) */
static long z_stub0(void){ return 0; }

/* Thread-name recorders: previously no-ops, now feed the diag registry so the
 * stall watchdog can print Unity's real thread names (Job.Worker N,
 * UnityGfxDeviceW, UnityMain, ...). Behaviourally still no-ops for the engine. */
#define Z_PR_SET_NAME 15
static long z_prctl(int option, unsigned long a2, unsigned long a3,
                    unsigned long a4, unsigned long a5){
  (void)a3; (void)a4; (void)a5;
  if (option == Z_PR_SET_NAME && a2) diag_set_name(NULL, (const char *)a2);
  return 0;
}
static int z_pthread_setname_np(void *thread, const char *name){
  diag_set_name(thread, name);
  return 0;
}

/* Real thread stack-bounds query for the conservative il2cpp (Boehm) GC.
 * The GC marks each thread's stack from the current SP up to the stack base;
 * it learns those bounds via pthread_getattr_np()+pthread_attr_getstack().
 * These were no-op stubs, so the out-params were left as uninitialized garbage
 * and the mark phase scanned past the mapped stack top -> Data Abort ~0x70 above
 * the stack region. svcQueryMemory(SP) returns the exact mapped stack region for
 * the calling thread, so the GC stays in bounds. The GC calls getattr_np then
 * getstack back-to-back on the same (self) thread, so computing live from SP in
 * getstack is correct. */
int z_pthread_attr_getstack(const void *attr, void **stackaddr, size_t *stacksize){
  (void)attr;
  uintptr_t sp; __asm__ volatile("mov %0, sp" : "=r"(sp));
  void *base; size_t sz;
  MemoryInfo mi; u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)sp)) && mi.size){
    base = (void *)(uintptr_t)mi.addr;   /* low end of the mapped stack */
    sz   = (size_t)mi.size;              /* base+sz == real mapped top  */
  } else {
    base = (void *)(sp & ~0xFFFFFull);    /* conservative 1 MB fallback */
    sz   = 0x100000;
  }
  if (stackaddr) *stackaddr = base;
  if (stacksize) *stacksize = sz;
  return 0;
}
/* opaque to us; the bounds are produced live by z_pthread_attr_getstack */
int z_pthread_getattr_np(void *thread, void *attr){ (void)thread; (void)attr; return 0; }

/* dedicated stubs */
int z_getpagesize(void){ return 0x1000; }
int z_pthread_equal(unsigned long a, unsigned long b){ return a==b; }
int z_gettid(void){ return 1; }
int z_dup(int fd){
  /* il2cpp's MemoryMappedFile::Map dup()s the metadata fd; a -1 here sent the
   * loader down its error path, so global-metadata.dat never mapped and every
   * managed class (System.Object included) resolved to NULL. Do a real dup;
   * if fsdev can't, fall back to the same fd -- our mmap shim copies the file
   * into RAM eagerly at map time, so reusing/closing the fd afterwards is safe. */
  int n = dup(fd);
  if (n < 0) n = fd;
  debugPrintf("[io] dup(%d) -> %d\n", fd, n);
  return n;
}
/* Unity SystemInfo device-detection strcasecmp()s the device model/manufacturer
 * (from JNI android.os.Build.*) against names like "Amazon"/"KFTT". In our fake
 * JNI those getters return NULL, so the raw libc strcasecmp deref-NULL-crashes.
 * Treat NULL as an empty string that matches nothing, so the checks just fail. */
int z_strcasecmp(const char *a, const char *b){
  if (a == b) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strcasecmp(a, b);
}
int z_strncasecmp(const char *a, const char *b, unsigned long n){
  if (a == b || n == 0) return 0;
  if (!a) return -1;
  if (!b) return 1;
  return strncasecmp(a, b, n);
}
/* GNU-style basename: pointer to the final path component, no modification.
 * Was mapped to a stub returning NULL, which would crash any caller. */
char *z_basename(const char *path){
  if (!path || !*path) return (char *)".";
  const char *s = strrchr(path, '/');
  return (char *)(s ? s + 1 : path);
}
/* locale-aware ctype: the _l variants ignore the locale and defer to the base
 * function. Were stubbed to return 0 -- which made tolower_l(c)=NUL and every
 * isX_l() false, silently corrupting any locale-path string/number parsing. */
int z_isdigit_l (int c, void *l){ (void)l; return isdigit(c); }
int z_islower_l (int c, void *l){ (void)l; return islower(c); }
int z_isupper_l (int c, void *l){ (void)l; return isupper(c); }
int z_isxdigit_l(int c, void *l){ (void)l; return isxdigit(c); }
int z_tolower_l (int c, void *l){ (void)l; return tolower(c); }
int z_toupper_l (int c, void *l){ (void)l; return toupper(c); }
void z_sincos(double x,double*s,double*c){ if(s)*s=sin(x); if(c)*c=cos(x); }
void*z_memrchr(const void*s,int c,unsigned long n){ const unsigned char*p=(const unsigned char*)s+n; while(n--){ if(*--p==(unsigned char)c) return (void*)p; } return 0; }

DynLibFunction unity_dynlib_functions[] = {
  { "ALooper_acquire", (uintptr_t)&ALooper_acquire },
  { "ALooper_forThread", (uintptr_t)&ALooper_forThread },
  { "ALooper_release", (uintptr_t)&ALooper_release },
  { "ALooper_wake", (uintptr_t)&ALooper_wake },
  { "ANativeWindow_acquire", (uintptr_t)&ANativeWindow_acquire },
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release },
  { "ASensorEventQueue_hasEvents", (uintptr_t)&ASensorEventQueue_hasEvents },
  { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue },
  { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue },
  { "ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor },
  { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance },
  { "ASensorManager_getSensorList", (uintptr_t)&ASensorManager_getSensorList },
  { "ASensor_getMinDelay", (uintptr_t)&ASensor_getMinDelay },
  { "ASensor_getName", (uintptr_t)&ASensor_getName },
  { "ASensor_getResolution", (uintptr_t)&ASensor_getResolution },
  { "ASensor_getType", (uintptr_t)&ASensor_getType },
  { "ASensor_getVendor", (uintptr_t)&ASensor_getVendor },
  { "_ZTH15gDeferredAction", (uintptr_t)&z_stub0 },
  { "__system_property_find", (uintptr_t)&z_stub0 },
  { "__system_property_read", (uintptr_t)&z_stub0 },
  { "_ctype_", (uintptr_t)&z_ctype[0] },
  { "acos", (uintptr_t)&acos },
  { "asin", (uintptr_t)&asin },
  { "atan", (uintptr_t)&atan },
  { "atan2", (uintptr_t)&atan2 },
  { "atanf", (uintptr_t)&atanf },
  { "atol", (uintptr_t)&atol },
  { "basename", (uintptr_t)&z_basename },
  { "bsearch", (uintptr_t)&bsearch },
  { "cbrtf", (uintptr_t)&cbrtf },
  { "clearerr", (uintptr_t)&clearerr },
  { "clock", (uintptr_t)&clock },
  { "clock_getres", (uintptr_t)&z_stub0 },
  { "cos", (uintptr_t)&cos },
  { "difftime", (uintptr_t)&difftime },
  { "div", (uintptr_t)&div },
  { "dladdr", (uintptr_t)&z_stub0 },
  { "dup", (uintptr_t)&z_dup },
  { "eglChooseConfig", (uintptr_t)&eglChooseConfig },
  { "eglCreatePbufferSurface", (uintptr_t)&eglCreatePbufferSurface },
  { "eglGetCurrentContext", (uintptr_t)&eglGetCurrentContext },
  { "eglGetCurrentSurface", (uintptr_t)&eglGetCurrentSurface },
  { "eglGetError", (uintptr_t)&eglGetError },
  { "eglGetProcAddress", (uintptr_t)&eglGetProcAddress },
  { "eglQueryString", (uintptr_t)&eglQueryString },
  { "eglSurfaceAttrib", (uintptr_t)&eglSurfaceAttrib },
  { "eglSwapInterval", (uintptr_t)&eglSwapInterval },
  { "exit", (uintptr_t)&exit },
  { "exp", (uintptr_t)&exp },
  { "exp2f", (uintptr_t)&exp2f },
  { "fdopen", (uintptr_t)&fdopen },
  { "flock", (uintptr_t)&z_stub0 },
  { "fmod", (uintptr_t)&fmod },
  { "fnmatch", (uintptr_t)&z_stub0 },
  { "fscanf", (uintptr_t)&fscanf },
  { "futimens", (uintptr_t)&z_stub0 },
  { "gethostbyaddr", (uintptr_t)&z_stub0 },
  { "gethostbyname", (uintptr_t)&z_stub0 },
  { "getpagesize", (uintptr_t)&z_getpagesize },
  { "getpriority", (uintptr_t)&z_stub0 },
  { "getpwuid_r", (uintptr_t)&z_stub0 },
  { "gettid", (uintptr_t)&z_gettid },
  { "hypot", (uintptr_t)&hypot },
  { "inet_addr", (uintptr_t)&z_stub0 },
  { "inet_ntop", (uintptr_t)&z_stub0 },
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "isdigit_l", (uintptr_t)&z_isdigit_l },
  { "islower_l", (uintptr_t)&z_islower_l },
  { "isupper_l", (uintptr_t)&z_isupper_l },
  { "isxdigit_l", (uintptr_t)&z_isxdigit_l },
  { "ldexp", (uintptr_t)&ldexp },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "lldiv", (uintptr_t)&lldiv },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "log2", (uintptr_t)&log2 },
  { "log2f", (uintptr_t)&log2f },
  { "logb", (uintptr_t)&logb },
  { "lrand48", (uintptr_t)&z_stub0 },
  { "lseek64", (uintptr_t)&z_lseek },
  { "madvise", (uintptr_t)&z_stub0 },
  { "memrchr", (uintptr_t)&z_memrchr },
  { "modf", (uintptr_t)&modf },
  { "modff", (uintptr_t)&modff },
  { "prctl", (uintptr_t)&z_prctl },
  { "pthread_atfork", (uintptr_t)&z_stub0 },
  { "pthread_attr_getstack", (uintptr_t)&z_pthread_attr_getstack },
  { "pthread_condattr_destroy", (uintptr_t)&z_stub0 },
  { "pthread_condattr_init", (uintptr_t)&z_stub0 },
  { "pthread_condattr_setclock", (uintptr_t)&z_stub0 },
  { "pthread_equal", (uintptr_t)&z_pthread_equal },
  { "pthread_getattr_np", (uintptr_t)&z_pthread_getattr_np },
  { "pthread_rwlock_init", (uintptr_t)&z_stub0 },
  { "pthread_setname_np", (uintptr_t)&z_pthread_setname_np },
  { "ptrace", (uintptr_t)&z_stub0 },
  { "raise", (uintptr_t)&raise },
  { "recvmsg", (uintptr_t)&z_stub0 },
  { "scalbn", (uintptr_t)&scalbn },
  { "sched_getaffinity", (uintptr_t)&z_stub0 },
  { "sched_setaffinity", (uintptr_t)&z_stub0 },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sendmsg", (uintptr_t)&z_stub0 },
  { "setbuf", (uintptr_t)&setbuf },
  { "setenv", (uintptr_t)&setenv },
  { "setpriority", (uintptr_t)&z_stub0 },
  { "setvbuf", (uintptr_t)&setvbuf },
  { "sigaltstack", (uintptr_t)&z_stub0 },
  { "sigdelset", (uintptr_t)&z_stub0 },
  { "sigfillset", (uintptr_t)&z_stub0 },
  { "sigsuspend", (uintptr_t)&z_stub0 },
  { "sin", (uintptr_t)&sin },
  { "sincos", (uintptr_t)&z_sincos },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "srand48", (uintptr_t)&z_stub0 },
  { "strcasecmp", (uintptr_t)&z_strcasecmp },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strftime", (uintptr_t)&strftime },
  { "strlcpy", (uintptr_t)&strlcpy },
  { "strnlen", (uintptr_t)&strnlen },
  { "strspn", (uintptr_t)&strspn },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "tan", (uintptr_t)&tan },
  { "tolower_l", (uintptr_t)&z_tolower_l },
  { "toupper_l", (uintptr_t)&z_toupper_l },
  { "towlower", (uintptr_t)&towlower },
  { "unsetenv", (uintptr_t)&unsetenv },
  { "utimes", (uintptr_t)&z_stub0 },
  { "vprintf", (uintptr_t)&vprintf },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
};
int unity_dynlib_numfunctions = (int)(sizeof(unity_dynlib_functions)/sizeof(unity_dynlib_functions[0]));
