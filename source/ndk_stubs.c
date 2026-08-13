/* ndk_stubs.c -- no-op bodies for the NDK functions imports.c registers that
 * cr3_nx's dropped android_native.c / text2bitmap.c used to define.
 *
 * ZOOKEEPER DX (Unity IL2CPP) never calls these: it loads assets through the
 * Java AssetManager (handled in unity_jni.c) and feeds input via JNI
 * nativeInjectEvent (unity_input.c), not the AInputQueue/AMotionEvent NDK path.
 * They exist only so the imports.c table links. Signatures match the extern
 * declarations in imports.c exactly.
 */

#include <stdint.h>
#include <stddef.h>

/* AInputQueue / AInputEvent / AMotionEvent / AKeyEvent */
void    AInputQueue_attachLooper(void *a, void *b, int c, void *d, void *e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void    AInputQueue_detachLooper(void *a) { (void)a; }
int32_t AInputQueue_getEvent(void *a, void **b) { (void)a;(void)b; return -1; }   /* no events */
int32_t AInputQueue_preDispatchEvent(void *a, void *b) { (void)a;(void)b; return 0; }
void    AInputQueue_finishEvent(void *a, void *b, int c) { (void)a;(void)b;(void)c; }
int32_t AInputEvent_getType(const void *a) { (void)a; return 0; }
int32_t AMotionEvent_getAction(const void *a) { (void)a; return 0; }
size_t  AMotionEvent_getPointerCount(const void *a) { (void)a; return 0; }
int32_t AMotionEvent_getPointerId(const void *a, size_t b) { (void)a;(void)b; return 0; }
float   AMotionEvent_getX(const void *a, size_t b) { (void)a;(void)b; return 0.0f; }
float   AMotionEvent_getY(const void *a, size_t b) { (void)a;(void)b; return 0.0f; }
int32_t AKeyEvent_getKeyCode(const void *a) { (void)a; return 0; }
int32_t AKeyEvent_getFlags(const void *a) { (void)a; return 0; }
int32_t AKeyEvent_getRepeatCount(const void *a) { (void)a; return 0; }

/* AConfiguration */
void *AConfiguration_new(void) { return NULL; }
void  AConfiguration_fromAssetManager(void *a, void *b) { (void)a;(void)b; }
void  AConfiguration_getLanguage(void *a, char *out) { (void)a; if (out) { out[0]='e'; out[1]='n'; } }
void  AConfiguration_getCountry(void *a, char *out) { (void)a; if (out) { out[0]='U'; out[1]='S'; } }
void  AConfiguration_delete(void *a) { (void)a; }

/* AAsset (Unity uses the Java AssetManager instead) */
void *AAssetManager_fromJava(void *a, void *b) { (void)a;(void)b; return NULL; }
void *AAssetManager_open(void *a, const char *b, int c) {
  (void)a;(void)c;
  extern int debugPrintf(char *, ...);
  debugPrintf("[io] NDK AAssetManager_open(%s) -> NULL (stub!)\n", b ? b : "(null)");
  return NULL;
}
const void *AAsset_getBuffer(void *a) { (void)a; return NULL; }
int64_t AAsset_getLength(void *a) { (void)a; return 0; }
void  AAsset_close(void *a) { (void)a; }

/* AndroidBitmap (engine dynamic-text path; unused) */
int AndroidBitmap_getInfo(void *a, void *b, void *c) { (void)a;(void)b;(void)c; return -1; }
int AndroidBitmap_lockPixels(void *a, void *b, void **c) { (void)a;(void)b;(void)c; return -1; }
int AndroidBitmap_unlockPixels(void *a, void *b) { (void)a;(void)b; return 0; }
