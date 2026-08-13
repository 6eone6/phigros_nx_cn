/* phigros_jni.c -- game-specific JNI fakes for PHIGROS.
 *
 * Two families of Java class that this game reaches and that the generic
 * jni_fake.c substrate knows nothing about.
 *
 * =========================================================================
 * 1. com/Exceed7/NativeAudio/NativeAudio      -- LOAD-BEARING
 * =========================================================================
 * Phigros does NOT play gameplay audio through Unity/FMOD. It uses Exceed7
 * NativeAudio (libnativeaudioe7.so), which talks straight to OpenSL ES,
 * because a rhythm game cannot tolerate Unity mixer latency. The C# surface
 * splits in two:
 *
 *   PLAYBACK  -- direct [DllImport("nativeaudioe7")]: getNativeSource,
 *                prepareAudio, playAudioWithNativeSourceIndex, stopAudio,
 *                setVolume, setPan, getPlaybackTime, setPlaybackTime, pause,
 *                resume, sendByteArray, unloadAudio, lengthByAudioBuffer.
 *                These are REAL exports of the plugin and resolve through the
 *                loader's dlsym shim. Nothing to do here.
 *
 *   INIT      -- through `AndroidJavaClass androidNativeAudio`, calling the
 *                static Java methods Initialize / LoadAudio /
 *                GetDeviceAudioInformation / Dispose on this class. There is
 *                no JVM, so those land here.
 *
 * The Java Initialize() does little more than call the module's own
 * createEngine() and remember the device's audio parameters, so that is what
 * we do -- via the real plugin, not a simulation.
 *
 * GetDeviceAudioInformation() returns a NativeAudio$DeviceAudioInformation
 * with two int fields. THIS IS YOUR LATENCY BUDGET. NativeAudio sizes its
 * buffers from these numbers, and Phigros derives its audio offset from
 * NativeAudio's reported latency. Reporting Android's typical 48000/192 when
 * the Switch is running something else makes every chart feel offset in a way
 * no amount of in-game calibration fully fixes. PHI_AUDIO_* in config.h are
 * the knobs; tune on hardware.
 *
 * =========================================================================
 * 2. com/taptap/... , com/tapsdk/antiaddiction... , com/tds/...  -- NEUTRALISED
 * =========================================================================
 * Phigros links TapTap login + LeanCloud + the anti-addiction SDK. The game
 * has a working offline mode, so none of this is load-bearing -- but it must
 * fail FAST and NEGATIVE rather than hang. A NULL from FindClass would raise a
 * managed exception; a stub that answers "not logged in / no restriction /
 * no network" lets the C# take its offline branch immediately.
 *
 * This deliberately does NOT fake a successful login, an entitlement, or a
 * purchase. It cannot verify one, so it asserts nothing.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "so_util.h"
#include "jni_fake.h"      /* fake_env */
#include "phigros_jni.h"

/* jni_fake.c's FakeID, repeated here rather than exported: this module only
 * ever reads it. Keep in sync with jni_fake.c if that layout changes --
 * tools/buildcheck.py checks the sizes agree. */
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } PhiFakeID;

static int has(const char *h, const char *n) { return h && n && strstr(h, n) != NULL; }

/* ---------------------------------------------------------------------- */
/* NativeAudio                                                            */
/* ---------------------------------------------------------------------- */

static int   g_na_inited   = 0;
static int   g_na_rate     = PHI_AUDIO_SAMPLE_RATE;
static int   g_na_buffer   = PHI_AUDIO_BUFFER_FRAMES;

/* Resolved lazily from the loaded plugin, so this file has no link-time
 * dependency on it: if the user did not stage libnativeaudioe7.so we log once
 * and keep going with UI audio only, rather than refusing to boot. */
/* The plugin's OWN JNI entry points. On Android the Java class
 * com.Exceed7.NativeAudio.NativeAudio declares these as `native` methods and
 * the JVM binds them by name mangling; with no JVM we call them directly, which
 * is strictly better than reimplementing what they do.
 *
 * Verified against the shipped libnativeaudioe7.so export table:
 *     Java_com_Exceed7_NativeAudio_NativeAudio_initialize
 *     Java_com_Exceed7_NativeAudio_NativeAudio_disposeIfAllocated
 *     Java_com_Exceed7_NativeAudio_NativeAudio_sendWavByteArray
 * (An earlier revision guessed at a bare `createEngine`. That symbol does
 * exist, but it is the internal helper, not the entry point the Java layer
 * calls, and it takes different arguments.) */
static int  (*p_initialize)(void *env, void *clazz, int a0, int a1, int a2) = NULL;
static void (*p_dispose)(void *env, void *clazz) = NULL;
static int    g_na_resolved = 0;

extern so_module nativeaudio_mod;      /* main.c, may be unloaded */
extern int      nativeaudio_loaded;    /* main.c */

static void na_resolve(void) {
  if (g_na_resolved) return;
  g_na_resolved = 1;
  if (!nativeaudio_loaded) {
    debugPrintf("[na] libnativeaudioe7.so not loaded -- NativeAudio init is a no-op.\n"
                "[na] Chart audio and hitsounds will be SILENT. Stage the plugin\n"
                "[na] from lib/arm64-v8a/ of your APK next to the .nro.\n");
    return;
  }
  p_initialize = (int (*)(void *, void *, int, int, int))
      so_try_find_addr_rx(&nativeaudio_mod, "Java_com_Exceed7_NativeAudio_NativeAudio_initialize");
  p_dispose = (void (*)(void *, void *))
      so_try_find_addr_rx(&nativeaudio_mod, "Java_com_Exceed7_NativeAudio_NativeAudio_disposeIfAllocated");
  debugPrintf("[na] resolved initialize=%p disposeIfAllocated=%p\n",
              (void *)p_initialize, (void *)p_dispose);
  if (!p_initialize)
    debugPrintf("[na] *** initialize entry NOT FOUND -- audio will be silent and\n"
                "[na] *** the first playback call will fault on a null engine.\n");
}

/* ---------------------------------------------------------------------- */
/* Playback guards                                                        */
/* ---------------------------------------------------------------------- */
/* C# reaches the playback entry points through DllImport -> dlopen/dlsym, so
 * they bypass this file entirely and land straight in the plugin. Every one of
 * them indexes the source table without checking it, so if initialize failed
 * the first note played is a null dereference.
 *
 * These wrappers are handed out by dlsym instead of the raw exports. When the
 * table is present they tail-call the real function and cost one load; when it
 * is not, they return harmlessly. A rhythm game with no sound is bad, but it
 * still boots, still takes input, and still lets everything else be tested --
 * which a crash on the title screen does not. */
/* Both verified against the shipped libnativeaudioe7.so, not assumed:
 *   prepareAudio 0x3b64  ldr x9,[x10,#0x58] -> global 0x2a058  (source table)
 *   initialize   0x3488  str w20,[x8,#0x60] -> global 0x2a060  (source count)
 * and 0x2a058 lies inside .bss (0x2a018-0x2a490), so it is zero until the
 * plugin allocates it -- which is exactly what makes it a reliable "did the
 * engine come up" flag. If the plugin is ever updated, re-derive these. */
#define NA_SOURCE_TABLE_OFF 0x2a058
/* 0x2a060 holds arg0. An earlier revision called it the source count; the
 * previous boot printed 256 there, which was the value passed as arg0 -- it is
 * the sample rate. Naming it correctly matters because it is what the log
 * reports back as proof the engine is configured right. */
#define NA_RATE_OFF         0x2a060

static int na_ready(void) {
  if (!nativeaudio_loaded) return 0;
  const uintptr_t nab = (uintptr_t)nativeaudio_mod.load_virtbase;
  return *(void *const volatile *)(nab + NA_SOURCE_TABLE_OFF) != NULL;
}
static void na_guard_warn(const char *fn) {
  static int warned = 0;
  if (warned) return;
  warned = 1;
  debugPrintf("[na] %s called before the engine came up -- suppressing this and\n"
              "[na] all later playback calls. Audio is silent by design here;\n"
              "[na] without the guard this would be a null dereference.\n", fn);
}
#define NA_GUARD(name, ret, params, args, rv)                                  \
  static ret na_##name params {                                                \
    if (!na_ready()) { na_guard_warn(#name); return rv; }                       \
    typedef ret (*fn_t) params;                                                 \
    static fn_t real = NULL;                                                    \
    if (!real) real = (fn_t)so_try_find_addr_rx(&nativeaudio_mod, #name);       \
    if (!real) { na_guard_warn(#name); return rv; }                             \
    return real args;                                                           \
  }
NA_GUARD(prepareAudio, int, (int a, int b), (a, b), -1)
NA_GUARD(getNativeSource, int, (int a), (a), -1)
NA_GUARD(playAudioWithNativeSourceIndex, int, (int a, int b, float c, float d, float e),
         (a, b, c, d, e), -1)
NA_GUARD(stopAudio, int, (int a), (a), -1)
NA_GUARD(setVolume, int, (int a, float b), (a, b), -1)
NA_GUARD(setPan, int, (int a, float b), (a, b), -1)
NA_GUARD(getPlaybackTime, int, (int a), (a), 0)
NA_GUARD(setPlaybackTime, int, (int a, float b), (a, b), -1)

void *phigros_audio_guard(const char *sym) {
  if (!sym || !nativeaudio_loaded) return NULL;
  struct { const char *n; void *f; } tbl[] = {
    { "prepareAudio",                   (void *)&na_prepareAudio },
    { "getNativeSource",                (void *)&na_getNativeSource },
    { "playAudioWithNativeSourceIndex", (void *)&na_playAudioWithNativeSourceIndex },
    { "stopAudio",                      (void *)&na_stopAudio },
    { "setVolume",                      (void *)&na_setVolume },
    { "setPan",                         (void *)&na_setPan },
    { "getPlaybackTime",                (void *)&na_getPlaybackTime },
    { "setPlaybackTime",                (void *)&na_setPlaybackTime },
  };
  for (unsigned i = 0; i < sizeof(tbl)/sizeof(*tbl); i++)
    if (!strcmp(sym, tbl[i].n)) return tbl[i].f;
  return NULL;
}

int phigros_owns_class(const char *cls) {
  if (!cls) return 0;
  return has(cls, "Exceed7/NativeAudio")
      || has(cls, "taptap/sdk")
      || has(cls, "tapsdk/antiaddiction")
      || has(cls, "tapsdk/bootstrap")
      || has(cls, "taptap/bootstrap")
      || has(cls, "tds/common")
      || has(cls, "tds/TapDB");
}

/* NativeAudio.Initialize(...) / Dispose() / LoadAudio(...) ---------------- */
void phigros_dispatch_void(void *recv, const void *idv, va_list va) {
  const PhiFakeID *id = (const PhiFakeID *)idv;
  (void)recv; (void)va;
  if (!id) return;

  if (has(id->cls, "Exceed7/NativeAudio")) {
    /* NOTE: Initialize is NOT here. Its Java signature is (IIZ)I -- it returns
     * an int, so the JNI layer dispatches it through Call*IntMethod, never
     * through the void path. An earlier revision handled it here, so it fell
     * through to the int dispatcher's default and returned 0 without ever
     * starting the audio engine: silent audio, and then a null-engine fault
     * (read of 0x24) on the first playback call from C#. See
     * phigros_dispatch_int below. */
    if (!strcmp(id->name, "Dispose")) {
      na_resolve();
      if (p_dispose) p_dispose(fake_env, NULL);
      g_na_inited = 0;
      debugPrintf("[na] Dispose -> disposeIfAllocated\n");
      return;
    }
    /* LoadAudio(path) is only used for the StreamingAssets overload, which
     * this game does not take (it loads from AudioClip -> sendByteArray, a
     * direct DllImport). Log once so we find out if that is wrong. */
    { static int warned = 0;
      if (!warned) { warned = 1;
        debugPrintf("[na] unhandled NativeAudio.%s -- if audio misbehaves, "
                    "start here\n", id->name); } }
    return;
  }

  /* TapTap / anti-addiction: every void entry point is a no-op. Init, login,
   * logout, callback registration, exit -- none of it can succeed here. */
  return;
}

/* GetDeviceAudioInformation() and every TapTap object-returning call -------
 * The returned object is a labelled fake; its int fields resolve through
 * phigros_field_int() below. */
void *phigros_dispatch_object(void *recv, const void *idv, va_list va) {
  const PhiFakeID *id = (const PhiFakeID *)idv;
  (void)recv; (void)va;
  if (!id) return NULL;

  if (has(id->cls, "Exceed7/NativeAudio")) {
    if (!strcmp(id->name, "GetDeviceAudioInformation"))
      return jni_make_object("com/Exceed7/NativeAudio/NativeAudio$DeviceAudioInformation");
    return jni_make_object(id->cls);
  }

  /* TapTap: a null AccessToken / Profile is exactly "not logged in", which is
   * the state the offline path expects. Returning a labelled object instead
   * would make the C# think a session exists and then fault reading it. */
  if (has(id->cls, "taptap") || has(id->cls, "tapsdk") || has(id->cls, "tds"))
    return NULL;

  return NULL;
}

/* int / bool / long returns ---------------------------------------------- */
uint64_t phigros_dispatch_int(void *recv, const void *idv, va_list va) {
  const PhiFakeID *id = (const PhiFakeID *)idv;
  (void)recv; (void)va;
  if (!id) return 0;

  if (has(id->cls, "Exceed7/NativeAudio")) {
    /* Initialize(int, int, boolean) -> int.  THE audio entry point.
     *
     * The three arguments are forwarded exactly as C# passed them rather than
     * substituted with PHI_AUDIO_*: the managed side derives them from
     * GetDeviceAudioInformation (which we already answer), so second-guessing
     * it here would desynchronise the two. PHI_AUDIO_* shapes what we REPORT;
     * this call just passes on what C# concluded. */
    if (!strcmp(id->name, "Initialize")) {
      /* C# argument order comes from NativeAudio.InitializationOptions
       * (dump.cs): androidAudioTrackCount, androidMinimumBufferSize,
       * preserveOnMinimize. */
      const int track_count = va_arg(va, int);
      const int min_buffer  = va_arg(va, int);
      const int preserve    = va_arg(va, int);  /* jboolean promoted to int */

      /* THE JAVA LAYER IS NOT A THIN WRAPPER -- IT REORDERS AND RESOLVES.
       *
       * Two earlier revisions forwarded C#'s three values straight into the
       * native entry point. That is wrong, and the plugin's own code says so:
       *
       *     0x3320  mov  w19, w4     ; w19 = THIRD native arg
       *     0x352c  cmp  w19, #1
       *     0x354c  str  wzr, [x24,#0x18]   ; source count = 0
       *     0x3550  b.lt #0x37d8            ; <1 -> skip the whole loop
       *     0x3554  sxtw x8, w19            ; else it is the LOOP BOUND
       *
       * The third native argument is the AUDIO TRACK COUNT. C#'s third value is
       * preserveOnMinimize, which was false -- so we passed 0, the loop over
       * "create an audio player" never executed, and the source table stored at
       *     0x3690  str x0, [x23, #0x58]
       * inside that loop was never allocated. prepareAudio then indexed a NULL
       * table and faulted reading +0x24. Silent audio and the crash were one bug.
       *
       * The other two native arguments are a buffer size (arg0, used as
       * arg0*1000 for a config field) and a positive divisor (arg1, stored
       * sign-extended and used by prepareAudio as `udiv x10, x11, x10`).
       * androidMinimumBufferSize is a MINIMUM (-1 = "no minimum"), not the
       * buffer itself, and a divisor of -1 would make that udiv nonsense -- so
       * on Android the Java layer asks AudioManager for the device's real
       * values and passes those. We have no AudioManager, so PHI_AUDIO_* in
       * config.h stands in for the device, which is exactly what those knobs
       * are for. The minimum is honoured when it asks for more.
       *
       * Not "verbatim", then -- but not invented either: the track count is
       * the caller's, and the two device values are the ones we already report
       * through GetDeviceAudioInformation, so both halves stay consistent. */
      na_resolve();
      if (!p_initialize) {
        debugPrintf("[na] Initialize(tracks=%d) but no native entry -- SILENT\n",
                    track_count);
        g_na_inited = 0;
        return 0;
      }
      int buffer = g_na_buffer;
      if (min_buffer > 0 && min_buffer > buffer) buffer = min_buffer;
      const int rate = g_na_rate;
      (void)preserve;   /* we never minimise; nothing to preserve */

      debugPrintf("[na] Initialize: C# asked for tracks=%d minBuffer=%d preserve=%d\n"
                  "[na]   -> native initialize(rate=%d, buffer=%d, tracks=%d)\n",
                  track_count, min_buffer, preserve, rate, buffer, track_count);
      if (track_count < 1)
        debugPrintf("[na] WARNING: track count %d means the plugin creates no\n"
                    "[na] audio players at all and audio stays silent.\n", track_count);

      /* ARGUMENT ORDER: (sampleRate, bufferSize, trackCount).
       *
       * Confirmed by our own OpenSL shim's log after the previous build, which
       * is the nice thing about shimming the layer underneath:
       *     [fmod] OpenSL fmt: type=2 ch=2 rate=256Hz ...
       *     [fmod] OpenSL CreateAudioPlayer: 256 Hz, 2 ch
       * 256 was the buffer size we passed as arg0. OpenSL's samplesPerSec field
       * is in MILLIhertz, and the plugin fills it with arg0*1000
       *     0x34ac  mov w8, #0x3e8      ; 1000
       *     0x34b8  mul w8, w20, w8     ; arg0 * 1000
       * so arg0 is a sample rate in Hz, not a buffer. The engine came up and
       * the players were created -- at 256 Hz, which is why there was still no
       * sound. arg1 is the buffer: prepareAudio divides a clip length by it
       * (udiv against the global stored from arg1) to get a buffer count. */
      const int rc = p_initialize(fake_env, NULL, rate, buffer, track_count);

      /* Check the plugin's OWN state rather than believing the return value.
       * prepareAudio indexes a source table held in a module global:
       *     ldr    x9, [x10, #0x58]     ; table base
       *     smaddl x9, w1, #0x28, x9    ; + index*40
       *     ldr    w10, [x9, #0x24]     ; <- faulted at 0x24, so table was 0
       * If that table is still NULL after initialize, audio did not come up and
       * the first playback call from C# will fault on a null dereference. Far
       * better to know that here, by name, than from a raw fault address. */
      const uintptr_t nab = (uintptr_t)nativeaudio_mod.load_virtbase;
      const void *tbl = *(void *const volatile *)(nab + NA_SOURCE_TABLE_OFF);
      g_na_inited = (tbl != NULL);
      debugPrintf("[na] initialize -> %d ; source table=%p ; plugin rate=%d\n",
                  rc, tbl,
                  *(const volatile int *)(nab + NA_RATE_OFF));
      if (!tbl)
        debugPrintf("[na] *** engine did NOT come up: source table is NULL.\n"
                    "[na] *** Audio will be silent. Playback calls are guarded\n"
                    "[na] *** (see na_guard in phigros_jni.c) so this is silence,\n"
                    "[na] *** not a crash.\n");
      else
        debugPrintf("[na] audio engine up\n");
      return (uint64_t)(uint32_t)rc;
    }
    if (!strcmp(id->name, "IsInitialized")) return (uint64_t)g_na_inited;
    return 0;
  }
  /* TapTap: 0 == false == "not logged in", "not restricted", "no age gate". */
  return 0;
}

float phigros_dispatch_float(void *recv, const void *idv, va_list va) {
  (void)recv; (void)idv; (void)va;
  return 0.0f;
}

/* Field defaults for DeviceAudioInformation ------------------------------
 * Field names are those of NativeAudio's Java class. Both spellings of the
 * buffer field are accepted because the plugin has used both across versions
 * and we cannot see which this build shipped without decompiling the dex. */
int phigros_field_int(const void *idv, uint64_t *out) {
  const PhiFakeID *id = (const PhiFakeID *)idv;
  if (!id || !out) return 0;
  if (!has(id->cls, "Exceed7/NativeAudio")) return 0;

  if (!strcmp(id->name, "sampleRate") ||
      !strcmp(id->name, "outputSampleRate")) { *out = (uint64_t)g_na_rate;   return 1; }
  if (!strcmp(id->name, "bufferSize")  ||
      !strcmp(id->name, "framesPerBurst") ||
      !strcmp(id->name, "outputBufferSize")) { *out = (uint64_t)g_na_buffer; return 1; }
  if (!strcmp(id->name, "nativeSourceCount")) { *out = PHI_AUDIO_NATIVE_SOURCES; return 1; }
  return 0;
}

void phigros_jni_init(void) {
  g_na_rate   = PHI_AUDIO_SAMPLE_RATE;
  g_na_buffer = PHI_AUDIO_BUFFER_FRAMES;
  debugPrintf("[na] device audio info will report %d Hz / %d frames "
              "(config.h PHI_AUDIO_*)\n", g_na_rate, g_na_buffer);
}
