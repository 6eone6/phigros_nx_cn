/* ---------------------------------------------------------------------------
 * firebase_stub.c -- minimal in-process replacement for the Firebase native
 * libs (libFirebaseCppApp / Analytics / Messaging / RemoteConfig).
 *
 * WHY THIS EXISTS
 * ---------------
 * The game's first scene runs a FirebaseManager whose bootstrap (StartInitializer
 * InitState::FirebaseLoading) will not advance until the managed dependency check
 * resolves to DependencyStatus.Available (== 0). On a real Android phone that is
 * answered by Google Play Services; on a Switch there is no Play Services, so even
 * a *perfectly working* libFirebaseCppApp would return UnavailableMissing (3) and
 * the game would hang at the exact same gate. The real .so files therefore cannot
 * ever reach the title screen here -- the only thing that works is to report
 * "Available", which is something only a stub can do.
 *
 * On top of that, the real libFirebaseCppApp detonates on load anyway: its
 * JNI_OnLoad logs via firebase::LogDebug through a GOT slot our loader leaves as
 * heap garbage (Instruction Abort at boot). So the real libs are pure liability.
 *
 * HOW IT WORKS
 * ------------
 * il2cpp resolves every [DllImport("FirebaseCppApp"/...)] P/Invoke through our
 * fake dlopen()/dlsym() (libc_shim.c). With the real .so files removed, dlsym for
 * any "Firebase_<module>_CSharp_*" / "SWIGRegister*" symbol lands here. We hand
 * back one of two trivial C stubs:
 *   - fb_stub_handle(): returns a non-null pointer into a shared zeroed buffer,
 *     so SWIG proxy objects (FirebaseApp, AppOptions, RemoteConfig, Future, ...)
 *     are non-null and the managed null-cPtr guards don't throw.
 *   - fb_stub_zero():   returns 0, for everything else (status ints, bools, void,
 *     getters that yield scalars).
 * Both ignore their arguments; on AArch64 a fixed-arity C function called with a
 * larger/var arg list is harmless (extra args sit in unread registers).
 *
 * The managed FirebaseApp.CheckDependencies path is synchronous (Task.Run over
 * CheckDependencies(), no Future polling), and returns Available(0) as long as a
 * FirebaseApp instance exists and nothing throws -- which our non-null
 * CreateInternal handle satisfies. Firebase here is cosmetic-only (RemoteConfig
 * banner/news textures); zeroed results just mean those images don't appear.
 *
 * Every lookup is logged so the run's debug.log enumerates exactly which Firebase
 * symbols the game asked for and which stub class we returned -- that's the tuning
 * signal if a specific call needs a different value.
 * ------------------------------------------------------------------------- */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"   /* debugPrintf */

/* Shared, zero-filled backing store handed out for every "object handle" the
 * SWIG layer expects. Big enough that any field reads SWIG/managed performs on a
 * proxy land on zeroed memory rather than faulting. One buffer for all types is
 * fine: our delete/dispose stubs are no-ops, so nothing is freed or aliased in a
 * way that matters. */
static uint8_t g_fb_obj[1024] __attribute__((aligned(16)));

static long  fb_stub_zero(void)   { return 0; }
static void *fb_stub_handle(void) { return g_fb_obj; }

/* The Firebase default app name constant (firebase::kDefaultAppName). The SWIG
 * string-returning name getters must hand back this exact, non-null C string:
 * the managed FirebaseApp uses it as the key into nameToProxy, and a null there
 * throws inside GetInstance() during the dependency check (faulting the task and
 * hanging the FirebaseLoading gate).
 *
 * CRITICAL: it must be a HEAP allocation, not a static. SWIG's C# string-return
 * marshaling frees the returned char* (real Firebase returns a freshly allocated
 * copy from its SWIG string helper). free() on a static/rodata pointer walks a
 * bogus malloc chunk header and faults (observed: Data Abort at 0x0 inside
 * CreateInternal's string marshal). Hand out a fresh malloc'd copy each call so
 * the marshaler's free() is valid; the getter is called rarely, so the worst
 * case (if a given call site does not free) is a few leaked bytes. */
static void *fb_stub_default_name(void) {
  static const char name[] = "__FIRAPP_DEFAULT";
  char *p = (char *)malloc(sizeof(name));
  if (p) memcpy(p, name, sizeof(name));
  return p;
}

/* Does this dlsym name belong to the Firebase SWIG surface we're replacing? */
static int fb_is_firebase_symbol(const char *s) {
  if (!s) return 0;
  if (strstr(s, "_CSharp_"))                 return 1;  /* Firebase_<Mod>_CSharp_* */
  if (!strncmp(s, "Firebase_", 9))           return 1;
  if (!strncmp(s, "SWIGRegister", 12))       return 1;  /* exception/string cb reg */
  if (!strncmp(s, "SWIG", 4) && strstr(s, "Firebase")) return 1;
  return 0;
}

/* A symbol whose managed return is an object/handle/pointer (must be non-null so
 * the proxy's cPtr guard passes). SWIG factory/accessor naming conventions:
 *   new_X, X_CreateInternal, ...Create..., ...GetInstance..., DefaultInstance,
 *   GetReference..., ...Future... (future handles), ..._SWIGUpcast (base ptr). */
static int fb_returns_handle(const char *s) {
  /* Future completion pollers must read as 0: kFutureStatusComplete == 0 and
   * "no error" == 0. Returning a non-null pointer here would make any Task that
   * polls a Future (RemoteConfig/Messaging fetches) spin forever. Catch these
   * before the "Future" handle rule below. */
  if (strstr(s, "GetStatus"))      return 0;
  if (strstr(s, "GetError"))       return 0;
  /* The SWIG Future surface uses lowercase property getters: FutureBase_status()
   * must read kFutureStatusComplete(0) and FutureBase_error() "no error"(0), or
   * the Future->Task bridge polls forever (e.g. RemoteConfig SetDefaultsAsync).
   * These contain "Future" so must be caught before the "Future"->handle rule. */
  if (strstr(s, "FutureBase_status")) return 0;
  if (strstr(s, "FutureBase_error"))  return 0;   /* incl. error_message: null is fine when error==0 */
  if (strstr(s, "new_"))            return 1;
  if (strstr(s, "Create"))         return 1;   /* CreateInternal / Create__SWIG_* */
  if (strstr(s, "GetInstance"))    return 1;
  if (strstr(s, "DefaultInstance"))return 1;
  if (strstr(s, "Instance"))       return 1;   /* *_Instance, GetInstanceInternal */
  if (strstr(s, "App_get"))        return 1;   /* RemoteConfig/Messaging .App -> the app object */
  if (strstr(s, "GetReference"))   return 1;
  if (strstr(s, "Future"))         return 1;   /* future handle objects */
  if (strstr(s, "SWIGUpcast"))     return 1;   /* base-class pointer cast */
  if (strstr(s, "GetTask"))        return 1;
  return 0;
}

/* Resolve a Firebase SWIG symbol to a stub, or NULL if it's not ours.
 * Called from dlsym_fake() after the real-module / shim-table lookups miss. */
void *firebase_stub_lookup(const char *symbol) {
  if (!fb_is_firebase_symbol(symbol)) return NULL;
  /* App-identity name getters must return the non-null default-app name string,
   * not 0/null -- see fb_stub_default_name above. Covers DefaultName_get,
   * NameInternal_get and the FirebaseApp Name getter. */
  if (strstr(symbol, "DefaultName") ||
      strstr(symbol, "NameInternal") ||
      strstr(symbol, "_Name_get")    ||
      strstr(symbol, "get_Name")) {
    debugPrintf("[fb-stub] %s -> \"__FIRAPP_DEFAULT\"\n", symbol);
    return (void *)&fb_stub_default_name;
  }
  if (fb_returns_handle(symbol)) {
    debugPrintf("[fb-stub] %s -> handle\n", symbol);
    return (void *)&fb_stub_handle;
  }
  debugPrintf("[fb-stub] %s -> 0\n", symbol);
  return (void *)&fb_stub_zero;
}
