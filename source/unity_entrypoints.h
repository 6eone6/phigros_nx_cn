/* unity_entrypoints_phigros.h -- UnityPlayer native methods recovered from
 * libunity.so's JNI_OnLoad  (PHIGROS, Unity 2022.3.62f2, source revision
 * 7670c08855a9, arm64 / IL2CPP, BuildID xxHash 80afbcbeffe6459a).
 *
 * Auto-extracted from THIS build's libunity.so by walking the
 * R_AARCH64_RELATIVE relocation map for JNINativeMethod{name,sig,fnPtr}
 * triples (30,031 relocs -> 52 native methods in 8 tables). The 29-method
 * table at 0x11e3390 is the UnityPlayer drive surface listed below.
 *
 * VERSION NOTE -- THIS IS THE GOOD CASE.
 * The loader-core lineage (Zookeeper DX) was built against Unity 2022.3.62f2.
 * This engine IS 2022.3.62f2, same source revision. There is NO version gap,
 * unlike the Fruit Ninja port (2022.3.0f1, 62 patch releases earlier). Every
 * OFF_* the core references is present here except two, both handled below.
 *
 * IMPORTANT: these are LINK-TIME addresses for THIS EXACT libunity.so. If the
 * game updates, re-run tools/entrypoints_phigros.py libunity.so.
 * Runtime address = unity_mod.load_virtbase + offset (the .so links at base 0).
 */
#ifndef UNITY_ENTRYPOINTS_H
#define UNITY_ENTRYPOINTS_H

#include <stdint.h>
#include "so_util.h"

/* ---- JNI_OnLoad (from .dynsym, not the reloc walk) ---------------------- */
#define OFF_JNI_OnLoad                    0x6c61b0 /* (JavaVM*,reserved)->jint */

/* ---- drive-critical ----------------------------------------------------- */
#define OFF_initJni                       0x6c53ac /* (env,thiz,Context)                  */
#define OFF_nativeRecreateGfxState        0x6c55e0 /* (env,thiz,int,Surface)  set surface */
#define OFF_nativeSendSurfaceChangedEvent 0x6c5648 /* (env,thiz)                          */
#define OFF_nativeRender                  0x6c56a0 /* (env,thiz)->Z  per-frame; false=stop*/
#define OFF_nativeInjectEvent             0x6c5700 /* (env,thiz,InputEvent,int)->Z  INPUT  */
#define OFF_nativePause                   0x6c5448 /* (env,thiz)->Z                       */
#define OFF_nativeResume                  0x6c54ac /* (env,thiz)                          */
#define OFF_nativeFocusChanged            0x6c558c /* (env,thiz,Z)                        */
#define OFF_nativeDone                    0x6c53b8 /* (env,thiz)->Z  shutdown             */
#define OFF_nativeApplicationUnload       0x6c553c /* (env,thiz)                          */
#define OFF_nativeLowMemory               0x6c54f4 /* (env,thiz)                          */
#define OFF_nativeOrientationChanged      0x6c60f8 /* (env,thiz,int,int)                  */

/* ---- messaging / misc --------------------------------------------------- */
#define OFF_nativeUnitySendMessage        0x6c5d0c /* (env,thiz,String,String,byte[])     */
#define OFF_nativeMuteMasterAudio         0x6c5f1c /* (env,thiz,Z)                        */
#define OFF_nativeGetNoWindowMode         0x6c6158 /* (env,thiz)->Z                       */
#define OFF_nativeIsAutorotationOn        0x6c5ebc /* (env,thiz)->Z                       */
#define OFF_nativeSetLaunchURL            0x6c5f78 /* (env,thiz,String)                   */
#define OFF_nativeHidePreservedContent    0x6c60b0 /* PRESENT in 62f2 (absent in FN's 0f1)*/

/* ---- soft keyboard (all 8 present) -------------------------------------- */
#define OFF_nativeSetInputArea                0x6c59f4
#define OFF_nativeSetKeyboardIsVisible        0x6c5a74
#define OFF_nativeSetInputString              0x6c5acc
#define OFF_nativeSetInputSelection           0x6c5b6c
#define OFF_nativeSoftInputClosed             0x6c5cbc
#define OFF_nativeSoftInputCanceled           0x6c5bd4
#define OFF_nativeSoftInputLostFocus          0x6c5c24
#define OFF_nativeReportKeyboardConfigChanged 0x6c5c74
#define OFF_nativeGetSoftInputType            0x69b1d0 /* table @0x11e3178          */

/* ---- aliases / absent --------------------------------------------------- */
/* The core calls nativeSendSurfaceChanged; this build only has the *Event
 * spelling, which is the same function. Same alias the core already uses. */
#define OFF_nativeSendSurfaceChanged      OFF_nativeSendSurfaceChangedEvent

/* nativeRestartActivityIndicator: ABSENT in this build. Declared by the core
 * but never called from main.c -- dead. Do NOT call. */
#define OFF_nativeRestartActivityIndicator 0x0

/* ---- engine entry-point signatures --------------------------------------
 * These describe the JNI calling convention (env, thiz, ...) that main.c drives
 * the engine through. Carried over unchanged from the loader core -- the
 * signatures come from the JNINativeMethod table above and are identical
 * across the whole 2022.3 line. */
typedef void     (*fn_initJni)(void*,void*,void*);
typedef void     (*fn_gfxstate)(void*,void*,int32_t,void*);
typedef void     (*fn_v)(void*,void*);
typedef uint8_t  (*fn_z)(void*,void*);
typedef void     (*fn_vz)(void*,void*,int32_t);
typedef uint8_t  (*fn_inject)(void*,void*,void*,int32_t);
typedef void     (*fn_orient)(void*,void*,int32_t,int32_t);

/* link-time offset -> runtime address (the .so links at base 0) */
#define UNITY_RESOLVE(mod, off) ((void*)((uintptr_t)(mod).load_virtbase + (off)))

/* ===========================================================================
 * Drive sequence (what the Java UnityPlayer does; main.c does it here):
 *
 *   initJni(env, thiz, fake_context);                   // early init
 *   nativeRecreateGfxState(env, thiz, 0, fake_surface); // give it the surface
 *   nativeSendSurfaceChangedEvent(env, thiz);           // engine builds GL state
 *   for (;;) {
 *       // input: nativeInjectEvent(env, thiz, motionEvent, deviceId);
 *       //        Phigros is finger-indexed multitouch -- see PORTING sec 7.
 *       if (!nativeRender(env, thiz)) break;            // false == engine wants out
 *   }
 *   nativeApplicationUnload(env, thiz);  nativeDone(env, thiz);
 * =========================================================================== */

/* ---- Swappy frame pacing (registered; we force-disable, see patch header) */
#define OFF_nOnChoreographer              0xc8d5d4
#define OFF_nOnRefreshPeriodChanged       0xc8f8d4
#define OFF_nSetSupportedRefreshPeriods   0xc8f6f4

/* ---- AndroidJavaProxy bridge (table @0x11e3800) -------------------------
 * TapTap SDK C# code registers AndroidJavaProxy callbacks. With no JVM these
 * are never driven, but jni_fake must not fault if libil2cpp reaches them.  */
#define OFF_nativeProxyInvoke             0x3ee6a0
#define OFF_nativeProxyFinalize           0x3ee7d4
#define OFF_nativeProxyLogJNIInvokeException 0x3ee81c
#define OFF_nativeProxyJNIFreeGCHandle    0x3ee898

/* ---- FMOD java bridge (table @0x11ef990) -- not driven; OpenSL shim used  */
#define OFF_fmodGetInfo                   0xe87d90
#define OFF_fmodProcess                   0xe87e58
#define OFF_fmodProcessMicData            0xe87ee4

/* ---- FYI: registered but never invoked by this port ----------------------
 *   ARCore        initializeARCore/pauseARCore/resumeARCore  @0x11e3178
 *   Camera2       initCamera2Jni/deinitCamera2Jni/...        @0x11e32b0
 *   HFP audio     nativeStatusQueryResult/initHFPStatusJni   @0x11e31f0
 *   volume/orient onAudioVolumeChanged/nativeUpdateOrientationLockState
 * Listed so nobody re-hunts them.                                          */

#endif /* UNITY_ENTRYPOINTS_H */
