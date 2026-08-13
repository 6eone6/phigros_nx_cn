/* unity_jni.h -- real handlers for the IMPLEMENT classes, ZOOKEEPER DX
 *
 * Plugs into cr3_nx's jni_fake.c. Provides working behaviour for the eight
 * classes that need it (the rest stay no-op stubs in jni_fake.c):
 *
 *   android/content/res/AssetManager      -> open()/openFd() real files
 *   java/io/InputStream                   -> read()/available()/close()
 *   android/content/res/AssetFileDescriptor + java/io/FileDescriptor (mmap path)
 *   android/content/SharedPreferences     -> PlayerPrefs read   (save data)
 *   android/content/SharedPreferences$Editor -> PlayerPrefs write
 *   android/view/Display + DisplayManager + Configuration + Resources -> screen
 *   android/content/Context               -> data/files paths
 *   com/unity3d/player/UnityPlayer        -> host queries (mostly no-op)
 *
 * ===========================================================================
 * INTEGRATION (four small edits to cr3_nx's jni_fake.c):
 *
 * 1) Forward the receiver to the dispatchers. Change CALL_VARIADIC and the
 *    void variant so `recv` reaches dispatch_*:
 *        #define CALL_VARIADIC(fn, ret_t, dispatch) \
 *          static ret_t fn(void*env,void*recv,FakeID*id,...){ (void)env; \
 *            va_list va; va_start(va,id); ret_t r=dispatch(recv,id,va); \
 *            va_end(va); return r; } \
 *          static ret_t fn##V(void*env,void*recv,FakeID*id,va_list va){ \
 *            (void)env; return dispatch(recv,id,va); }
 *    and give dispatch_object/int/float/void a leading `void *recv` param.
 *
 * 2) At the top of each dispatch_* in jni_fake.c, delegate Unity classes:
 *        if (unity_owns_class(id->cls)) return unity_dispatch_object(recv,id,va);
 *    (same for _int/_long->_int, _void; _float falls through to act_float.)
 *
 * 3) Register the class list at startup (after mutexInit in jni_init):
 *        unity_jni_init(nx_root());   // runtime data root, any folder
 *    and intern every UNITY_JNI_CLASSES[] name so FindClass never returns NULL.
 *
 * 4) Expose three tiny accessors from jni_fake.c (the FakePriArray/FakeString
 *    structs are static there). Add these and declare them below:
 *        void *jni_bytearray_data(void *arr, int *len_out);   // ->data,->len
 *        const char *jni_string_utf(void *jstr);              // FakeString.utf
 *        // jni_make_string / jni_make_object are already non-static.
 * ===========================================================================
 */
#ifndef UNITY_JNI_H
#define UNITY_JNI_H

#include <stdarg.h>
#include <stdint.h>

/* FakeID is defined in jni_fake.c; we only read .cls/.name/.sig (all char[]). */

void  unity_jni_init(const char *data_root);
int   unity_owns_class(const char *cls);          /* 1 if this module handles it */
/* Unix-rooted path given to managed code (persistentDataPath / dataPath).
 * Needed by jni_fake's generic path, where the class arrives erased. */
const char *nx_managed_root(void);

void     *unity_dispatch_object(void *recv, const void *id, va_list va);
uint64_t  unity_dispatch_int   (void *recv, const void *id, va_list va); /* int/bool/long */
void      unity_dispatch_void  (void *recv, const void *id, va_list va);

/* Boxed PlayerPrefs values returned by getAll() iteration. jni_fake.c routes
 * unbox calls (intValue/longValue/booleanValue/floatValue) and IsInstanceOf by
 * RECEIVER so only our own boxed handles are affected (other code's Integer/etc.
 * objects are untouched). */
int       unity_is_boxed   (void *recv);                 /* 1 if recv is our boxed value */
uint64_t  unity_boxed_int  (void *recv);                 /* int/long/bool payload */
float     unity_boxed_float (void *recv);                /* float payload */
int       unity_isinstance (void *obj, const char *clazz); /* 1/0 for boxed, -1 = not ours */

/* provided by jni_fake.c (see integration note 4) */
extern void       *jni_make_string(const char *utf);
extern void       *jni_make_object(const char *label);
extern void       *jni_bytearray_data(void *arr, int *len_out);
extern const char *jni_string_utf(void *jstr);

#endif /* UNITY_JNI_H */
