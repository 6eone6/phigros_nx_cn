/* imports_phigros_extra.c -- Unity-surface stubs INHERITED from the Fruit
 * Ninja tree and RE-VERIFIED against Phigros.
 *
 * These 75 entries were originally derived for Fruit Ninja. 68 of them are
 * genuinely referenced by THIS game's libmain/libunity/libil2cpp as well --
 * unsurprising, since both are Unity 2022.3 arm64 IL2CPP builds that pull in
 * the same MediaNDK / AImageReader / AHardwareBuffer surface. The 7 that
 * Phigros does not reference (ALooper_pollAll, socketpair, cosh, sinh, tanh,
 * environ) are harmless: a resolver entry only fires when something asks for
 * it. They are kept rather than pruned so this file stays diffable against
 * the upstream tree.
 *
 * The symbols Phigros needs that this file does NOT provide are in
 * imports_phigros_plugin.c -- two for the core libs, the rest for the
 * NativeAudio / TapSDK plugins.
 *
 * They fall into exactly three groups, and the first two are DEAD on Switch:
 *
 *  1. MediaNDK (54 syms: AMediaCodec/Extractor/Format/DataSource + 15 key
 *     strings) -- Unity's VideoPlayer hardware-decode backend. The game DOES
 *     reference VideoPlayer (7 managed call sites: set_url, set_targetTexture,
 *     Prepare, Play, Stop, set_playOnAwake, set_waitForFirstFrame), so these
 *     stubs are reached. They are written to fail FAST and CLEANLY so Unity
 *     raises a prepare error rather than blocking. If the game hangs waiting on
 *     prepareCompleted, escalate to the il2cpp hook in
 *     PORTING_PHIGROS.md sec 7 -- do not try to make these work.
 *
 *  2. AImageReader / AHardwareBuffer / ANativeWindow_toSurface (13 syms) --
 *     the Camera2/ARCore capture path. The game's managed code has ZERO
 *     references to WebCamTexture, UnityEngine.XR or ARCore, so these can
 *     never be reached. Pure link-satisfying stubs.
 *
 *  3. ALooper_pollAll + socketpair (2 syms) -- real, and implemented.
 *
 *  4. cosh / sinh / tanh / environ (4 syms) -- real newlib symbols that the
 *     loader core does not resolve. See the note above the table.
 *
 * Every stub logs once through NOTSUP() so debug.log tells you if one is
 * unexpectedly live.
 *
 * Wire this into the loader the same way imports_pvz_extra.c was:
 *   extern DynLibFunction phigros_extra_functions[];
 *   extern size_t         phigros_extra_numfunctions;
 * and append it to the combined table in imports.c (patches/patch_sources.py
 * does this). The type and the *_functions / *_numfunctions naming are the
 * loader core's, not a choice -- imports.c concatenates these arrays by hand.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <math.h>
#include "so_util.h"   /* DynLibFunction */
#include "util.h"      /* debugPrintf -- the loader core's logger.
                        * NOT diag.h: that header exists but declares no
                        * printf-style logger, and there is no log_printf
                        * anywhere in the tree. */

#ifndef AMEDIA_ERROR_UNSUPPORTED
#define AMEDIA_ERROR_UNSUPPORTED (-1010)
#endif

/* log-once-per-symbol so a live stub is loud but not spammy */
#define NOTSUP(sym) do {                                              \
    static int _warned = 0;                                           \
    if (!_warned) { _warned = 1;                                      \
      debugPrintf("[stub] %s called -- unsupported on Switch\n", sym); }\
  } while (0)

/* ---- MediaNDK: AMediaFormat key strings (DATA symbols, not functions) --- */
const char *AMEDIAFORMAT_KEY_CHANNEL_COUNT   = "channel-count";
const char *AMEDIAFORMAT_KEY_COLOR_FORMAT    = "color-format";
const char *AMEDIAFORMAT_KEY_COLOR_RANGE     = "color-range";
const char *AMEDIAFORMAT_KEY_COLOR_STANDARD  = "color-standard";
const char *AMEDIAFORMAT_KEY_DURATION        = "durationUs";
const char *AMEDIAFORMAT_KEY_ENCODER_DELAY   = "encoder-delay";
const char *AMEDIAFORMAT_KEY_FRAME_RATE      = "frame-rate";
const char *AMEDIAFORMAT_KEY_HEIGHT          = "height";
const char *AMEDIAFORMAT_KEY_LANGUAGE        = "language";
const char *AMEDIAFORMAT_KEY_MIME            = "mime";
const char *AMEDIAFORMAT_KEY_ROTATION        = "rotation-degrees";
const char *AMEDIAFORMAT_KEY_SAMPLE_RATE     = "sample-rate";
const char *AMEDIAFORMAT_KEY_SLICE_HEIGHT    = "slice-height";
const char *AMEDIAFORMAT_KEY_STRIDE          = "stride";
const char *AMEDIAFORMAT_KEY_WIDTH           = "width";

/* ---- MediaNDK: codec / extractor / format / datasource ------------------ */
static int     AMediaCodec_configure(void) { NOTSUP("AMediaCodec_configure"); return AMEDIA_ERROR_UNSUPPORTED; }
static void*   AMediaCodec_createDecoderByType(void) { NOTSUP("AMediaCodec_createDecoderByType"); return NULL; }
static void    AMediaCodec_delete(void) { NOTSUP("AMediaCodec_delete"); }
static int64_t AMediaCodec_dequeueInputBuffer(void) { NOTSUP("AMediaCodec_dequeueInputBuffer"); return -1; }
static int64_t AMediaCodec_dequeueOutputBuffer(void) { NOTSUP("AMediaCodec_dequeueOutputBuffer"); return -1; }
static int     AMediaCodec_flush(void) { NOTSUP("AMediaCodec_flush"); return AMEDIA_ERROR_UNSUPPORTED; }
static void*   AMediaCodec_getInputBuffer(void) { NOTSUP("AMediaCodec_getInputBuffer"); return NULL; }
static void*   AMediaCodec_getOutputBuffer(void) { NOTSUP("AMediaCodec_getOutputBuffer"); return NULL; }
static void*   AMediaCodec_getOutputFormat(void) { NOTSUP("AMediaCodec_getOutputFormat"); return NULL; }
static int     AMediaCodec_queueInputBuffer(void) { NOTSUP("AMediaCodec_queueInputBuffer"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaCodec_releaseOutputBuffer(void) { NOTSUP("AMediaCodec_releaseOutputBuffer"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaCodec_setOutputSurface(void) { NOTSUP("AMediaCodec_setOutputSurface"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaCodec_start(void) { NOTSUP("AMediaCodec_start"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaCodec_stop(void) { NOTSUP("AMediaCodec_stop"); return AMEDIA_ERROR_UNSUPPORTED; }
static void    AMediaDataSource_delete(void) { NOTSUP("AMediaDataSource_delete"); }
static void*   AMediaDataSource_new(void) { NOTSUP("AMediaDataSource_new"); return NULL; }
static void    AMediaDataSource_setClose(void) { NOTSUP("AMediaDataSource_setClose"); }
static void    AMediaDataSource_setGetSize(void) { NOTSUP("AMediaDataSource_setGetSize"); }
static void    AMediaDataSource_setReadAt(void) { NOTSUP("AMediaDataSource_setReadAt"); }
static void    AMediaDataSource_setUserdata(void) { NOTSUP("AMediaDataSource_setUserdata"); }
static bool    AMediaExtractor_advance(void) { NOTSUP("AMediaExtractor_advance"); return false; }
static void    AMediaExtractor_delete(void) { NOTSUP("AMediaExtractor_delete"); }
static int64_t AMediaExtractor_getSampleTime(void) { NOTSUP("AMediaExtractor_getSampleTime"); return -1; }
static size_t  AMediaExtractor_getSampleTrackIndex(void) { NOTSUP("AMediaExtractor_getSampleTrackIndex"); return 0; }
static size_t  AMediaExtractor_getTrackCount(void) { NOTSUP("AMediaExtractor_getTrackCount"); return 0; }
static void*   AMediaExtractor_getTrackFormat(void) { NOTSUP("AMediaExtractor_getTrackFormat"); return NULL; }
static void*   AMediaExtractor_new(void) { NOTSUP("AMediaExtractor_new"); return NULL; }
static int     AMediaExtractor_readSampleData(void) { NOTSUP("AMediaExtractor_readSampleData"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaExtractor_seekTo(void) { NOTSUP("AMediaExtractor_seekTo"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaExtractor_selectTrack(void) { NOTSUP("AMediaExtractor_selectTrack"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaExtractor_setDataSource(void) { NOTSUP("AMediaExtractor_setDataSource"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaExtractor_setDataSourceCustom(void) { NOTSUP("AMediaExtractor_setDataSourceCustom"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AMediaExtractor_setDataSourceFd(void) { NOTSUP("AMediaExtractor_setDataSourceFd"); return AMEDIA_ERROR_UNSUPPORTED; }
static void    AMediaFormat_delete(void) { NOTSUP("AMediaFormat_delete"); }
static bool    AMediaFormat_getFloat(void) { NOTSUP("AMediaFormat_getFloat"); return false; }
static bool    AMediaFormat_getInt32(void) { NOTSUP("AMediaFormat_getInt32"); return false; }
static bool    AMediaFormat_getInt64(void) { NOTSUP("AMediaFormat_getInt64"); return false; }
static bool    AMediaFormat_getString(void) { NOTSUP("AMediaFormat_getString"); return false; }
static void    AMediaFormat_setInt32(void) { NOTSUP("AMediaFormat_setInt32"); }

/* ---- AImageReader + AHardwareBuffer (camera path -- 0 uses in game code) - */
static void    AHardwareBuffer_acquire(void) { NOTSUP("AHardwareBuffer_acquire"); }
static int     AHardwareBuffer_describe(void) { NOTSUP("AHardwareBuffer_describe"); return AMEDIA_ERROR_UNSUPPORTED; }
static void    AHardwareBuffer_release(void) { NOTSUP("AHardwareBuffer_release"); }
static int     AImageReader_acquireLatestImage(void) { NOTSUP("AImageReader_acquireLatestImage"); return AMEDIA_ERROR_UNSUPPORTED; }
static void    AImageReader_delete(void) { NOTSUP("AImageReader_delete"); }
static void*   AImageReader_getWindow(void) { NOTSUP("AImageReader_getWindow"); return NULL; }
static int     AImageReader_newWithUsage(void) { NOTSUP("AImageReader_newWithUsage"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AImageReader_setBufferRemovedListener(void) { NOTSUP("AImageReader_setBufferRemovedListener"); return AMEDIA_ERROR_UNSUPPORTED; }
static int     AImageReader_setImageListener(void) { NOTSUP("AImageReader_setImageListener"); return AMEDIA_ERROR_UNSUPPORTED; }
static void    AImage_delete(void) { NOTSUP("AImage_delete"); }
static void    AImage_deleteAsync(void) { NOTSUP("AImage_deleteAsync"); }
static int     AImage_getHardwareBuffer(void) { NOTSUP("AImage_getHardwareBuffer"); return AMEDIA_ERROR_UNSUPPORTED; }
static int64_t AImage_getTimestamp(void) { NOTSUP("AImage_getTimestamp"); return -1; }

/* ---- misc -------------------------------------------------------------- */
/* ALooper_pollAll -- hand-written below */
/* ANativeWindow_toSurface -- hand-written below */
/* socketpair -- hand-written below */
/* ---- ALooper_pollAll ---------------------------------------------------- *
 * The base resolves ALooper_acquire/forThread/release/wake but not pollAll.
 * Unity calls it from its Android event-pump thread. We have no Android
 * looper, so report "no events, timed out" and let the caller spin. Honour
 * the timeout so the thread cannot become a busy-wait.
 *   timeoutMillis < 0 == block forever; we cap it, because blocking forever
 *   on a queue nothing ever posts to is how you wedge a worker (this is the
 *   same failure mode as PORTING.md sec 6o's indefinite pthread_cond_wait).  */
#define ALOOPER_POLL_TIMEOUT (-3)
static int ALooper_pollAll_fake(int timeoutMillis, int *outFd,
                                int *outEvents, void **outData)
{
  if (outFd)     *outFd = -1;
  if (outEvents) *outEvents = 0;
  if (outData)   *outData = NULL;
  if (timeoutMillis < 0 || timeoutMillis > 16) timeoutMillis = 16; /* one frame */
  if (timeoutMillis > 0) usleep((useconds_t)timeoutMillis * 1000);
  return ALOOPER_POLL_TIMEOUT;
}

/* ---- ANativeWindow_toSurface ------------------------------------------- *
 * Would wrap a native window back into a Java Surface. Only the camera path
 * asks for this. Returning NULL is correct and safe here.                   */
static void *ANativeWindow_toSurface_fake(void *env, void *window)
{
  (void)env; (void)window;
  NOTSUP("ANativeWindow_toSurface");
  return NULL;
}

/* ---- socketpair --------------------------------------------------------- *
 * Present in libnx/newlib on recent devkitA64 but not in the base table.
 * If your toolchain lacks it, the #else path fails the call cleanly -- Unity
 * only uses it for an internal wake-up pipe it can live without.            */
static int socketpair_fake(int domain, int type, int protocol, int sv[2])
{
#ifdef __SWITCH__
  extern int socketpair(int, int, int, int[2]) __attribute__((weak));
  if (socketpair) return socketpair(domain, type, protocol, sv);
#endif
  (void)domain; (void)type; (void)protocol;
  if (sv) { sv[0] = -1; sv[1] = -1; }
  NOTSUP("socketpair");
  errno = ENOSYS;
  return -1;
}

/* ---- newlib symbols the loader CORE does not resolve ------------------- *
 * CAUTION, and the reason these are here:
 *
 * The Zookeeper loader core's own tables (imports.c + unity_imports.c) do NOT
 * contain cosh/sinh/tanh/environ. In the PvZ Fusion tree they are supplied by
 * `imports_pvz_extra.c` -- which is exactly the file THIS file replaces. So a
 * naive "swap imports_pvz_extra.c for imports_phigros_extra.c" would delete
 * them, and Phigros's libil2cpp.so imports all four:
 *
 *     cosh, sinh, tanh   C# System.Math hyperbolics
 *     environ            C# System.Environment
 *
 * Confirmed by nm -D --undefined-only on THIS game's libil2cpp.so. Without
 * these rows the loader fails to resolve them at module-load time.
 *
 * (PvZ additionally needed `nearbyintf`, imported by its 2022.3.62 libunity.
 * This engine -- 2022.3.0f1 -- does not import it, so it is deliberately NOT
 * carried over. Adding it back would be harmless but pointless.)
 *
 * If the LINKER ever reports `undefined reference` to one of these four, your
 * newlib build lacks it; replace that row with a one-line local impl, e.g.
 *     static double cosh_fake(double x) { return (exp(x) + exp(-x)) / 2.0; }
 */
extern char **environ;

/* ---- resolver table ----------------------------------------------------- */
DynLibFunction phigros_extra_functions[] = {
  /* newlib passthroughs the core table lacks (see note above) */
  { "cosh",    (uintptr_t)&cosh    },
  { "sinh",    (uintptr_t)&sinh    },
  { "tanh",    (uintptr_t)&tanh    },
  { "environ", (uintptr_t)&environ },

  { "AMEDIAFORMAT_KEY_CHANNEL_COUNT", (uintptr_t)&AMEDIAFORMAT_KEY_CHANNEL_COUNT },
  { "AMEDIAFORMAT_KEY_COLOR_FORMAT", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_FORMAT },
  { "AMEDIAFORMAT_KEY_COLOR_RANGE", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_RANGE },
  { "AMEDIAFORMAT_KEY_COLOR_STANDARD", (uintptr_t)&AMEDIAFORMAT_KEY_COLOR_STANDARD },
  { "AMEDIAFORMAT_KEY_DURATION", (uintptr_t)&AMEDIAFORMAT_KEY_DURATION },
  { "AMEDIAFORMAT_KEY_ENCODER_DELAY", (uintptr_t)&AMEDIAFORMAT_KEY_ENCODER_DELAY },
  { "AMEDIAFORMAT_KEY_FRAME_RATE", (uintptr_t)&AMEDIAFORMAT_KEY_FRAME_RATE },
  { "AMEDIAFORMAT_KEY_HEIGHT", (uintptr_t)&AMEDIAFORMAT_KEY_HEIGHT },
  { "AMEDIAFORMAT_KEY_LANGUAGE", (uintptr_t)&AMEDIAFORMAT_KEY_LANGUAGE },
  { "AMEDIAFORMAT_KEY_MIME", (uintptr_t)&AMEDIAFORMAT_KEY_MIME },
  { "AMEDIAFORMAT_KEY_ROTATION", (uintptr_t)&AMEDIAFORMAT_KEY_ROTATION },
  { "AMEDIAFORMAT_KEY_SAMPLE_RATE", (uintptr_t)&AMEDIAFORMAT_KEY_SAMPLE_RATE },
  { "AMEDIAFORMAT_KEY_SLICE_HEIGHT", (uintptr_t)&AMEDIAFORMAT_KEY_SLICE_HEIGHT },
  { "AMEDIAFORMAT_KEY_STRIDE", (uintptr_t)&AMEDIAFORMAT_KEY_STRIDE },
  { "AMEDIAFORMAT_KEY_WIDTH", (uintptr_t)&AMEDIAFORMAT_KEY_WIDTH },
  { "AMediaCodec_configure", (uintptr_t)&AMediaCodec_configure },
  { "AMediaCodec_createDecoderByType", (uintptr_t)&AMediaCodec_createDecoderByType },
  { "AMediaCodec_delete", (uintptr_t)&AMediaCodec_delete },
  { "AMediaCodec_dequeueInputBuffer", (uintptr_t)&AMediaCodec_dequeueInputBuffer },
  { "AMediaCodec_dequeueOutputBuffer", (uintptr_t)&AMediaCodec_dequeueOutputBuffer },
  { "AMediaCodec_flush", (uintptr_t)&AMediaCodec_flush },
  { "AMediaCodec_getInputBuffer", (uintptr_t)&AMediaCodec_getInputBuffer },
  { "AMediaCodec_getOutputBuffer", (uintptr_t)&AMediaCodec_getOutputBuffer },
  { "AMediaCodec_getOutputFormat", (uintptr_t)&AMediaCodec_getOutputFormat },
  { "AMediaCodec_queueInputBuffer", (uintptr_t)&AMediaCodec_queueInputBuffer },
  { "AMediaCodec_releaseOutputBuffer", (uintptr_t)&AMediaCodec_releaseOutputBuffer },
  { "AMediaCodec_setOutputSurface", (uintptr_t)&AMediaCodec_setOutputSurface },
  { "AMediaCodec_start", (uintptr_t)&AMediaCodec_start },
  { "AMediaCodec_stop", (uintptr_t)&AMediaCodec_stop },
  { "AMediaDataSource_delete", (uintptr_t)&AMediaDataSource_delete },
  { "AMediaDataSource_new", (uintptr_t)&AMediaDataSource_new },
  { "AMediaDataSource_setClose", (uintptr_t)&AMediaDataSource_setClose },
  { "AMediaDataSource_setGetSize", (uintptr_t)&AMediaDataSource_setGetSize },
  { "AMediaDataSource_setReadAt", (uintptr_t)&AMediaDataSource_setReadAt },
  { "AMediaDataSource_setUserdata", (uintptr_t)&AMediaDataSource_setUserdata },
  { "AMediaExtractor_advance", (uintptr_t)&AMediaExtractor_advance },
  { "AMediaExtractor_delete", (uintptr_t)&AMediaExtractor_delete },
  { "AMediaExtractor_getSampleTime", (uintptr_t)&AMediaExtractor_getSampleTime },
  { "AMediaExtractor_getSampleTrackIndex", (uintptr_t)&AMediaExtractor_getSampleTrackIndex },
  { "AMediaExtractor_getTrackCount", (uintptr_t)&AMediaExtractor_getTrackCount },
  { "AMediaExtractor_getTrackFormat", (uintptr_t)&AMediaExtractor_getTrackFormat },
  { "AMediaExtractor_new", (uintptr_t)&AMediaExtractor_new },
  { "AMediaExtractor_readSampleData", (uintptr_t)&AMediaExtractor_readSampleData },
  { "AMediaExtractor_seekTo", (uintptr_t)&AMediaExtractor_seekTo },
  { "AMediaExtractor_selectTrack", (uintptr_t)&AMediaExtractor_selectTrack },
  { "AMediaExtractor_setDataSource", (uintptr_t)&AMediaExtractor_setDataSource },
  { "AMediaExtractor_setDataSourceCustom", (uintptr_t)&AMediaExtractor_setDataSourceCustom },
  { "AMediaExtractor_setDataSourceFd", (uintptr_t)&AMediaExtractor_setDataSourceFd },
  { "AMediaFormat_delete", (uintptr_t)&AMediaFormat_delete },
  { "AMediaFormat_getFloat", (uintptr_t)&AMediaFormat_getFloat },
  { "AMediaFormat_getInt32", (uintptr_t)&AMediaFormat_getInt32 },
  { "AMediaFormat_getInt64", (uintptr_t)&AMediaFormat_getInt64 },
  { "AMediaFormat_getString", (uintptr_t)&AMediaFormat_getString },
  { "AMediaFormat_setInt32", (uintptr_t)&AMediaFormat_setInt32 },
  { "AHardwareBuffer_acquire", (uintptr_t)&AHardwareBuffer_acquire },
  { "AHardwareBuffer_describe", (uintptr_t)&AHardwareBuffer_describe },
  { "AHardwareBuffer_release", (uintptr_t)&AHardwareBuffer_release },
  { "AImageReader_acquireLatestImage", (uintptr_t)&AImageReader_acquireLatestImage },
  { "AImageReader_delete", (uintptr_t)&AImageReader_delete },
  { "AImageReader_getWindow", (uintptr_t)&AImageReader_getWindow },
  { "AImageReader_newWithUsage", (uintptr_t)&AImageReader_newWithUsage },
  { "AImageReader_setBufferRemovedListener", (uintptr_t)&AImageReader_setBufferRemovedListener },
  { "AImageReader_setImageListener", (uintptr_t)&AImageReader_setImageListener },
  { "AImage_delete", (uintptr_t)&AImage_delete },
  { "AImage_deleteAsync", (uintptr_t)&AImage_deleteAsync },
  { "AImage_getHardwareBuffer", (uintptr_t)&AImage_getHardwareBuffer },
  { "AImage_getTimestamp", (uintptr_t)&AImage_getTimestamp },
  { "ALooper_pollAll", (uintptr_t)&ALooper_pollAll_fake },
  { "ANativeWindow_toSurface", (uintptr_t)&ANativeWindow_toSurface_fake },
  { "socketpair", (uintptr_t)&socketpair_fake },
};
size_t phigros_extra_numfunctions =
    sizeof(phigros_extra_functions) / sizeof(phigros_extra_functions[0]);
