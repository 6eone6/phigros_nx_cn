/* jni_fake.h -- fake JNI environment for the MVGL engine (libcrx.so)
 *
 * libcrx.so is a NativeActivity app: the glue hands it the JavaVM + activity
 * jobject from the ANativeActivity struct, and the engine then drives three
 * Java helpers entirely through JNI:
 *   local/mediav/Text2Bitmap   -> draw text into an android.graphics.Bitmap
 *   local/mediav/MoviePlayer   -> FMV playback
 *   local/mediav/MyNativeActivity -> asset manager, orientation, IME, store...
 * We provide a functional JNIEnv/JavaVM so those calls resolve to native code.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the engine asks the activity to finish
extern volatile int jni_quit_requested;

void jni_init(void);

// the fake MyNativeActivity jobject handed to ANativeActivity.clazz
void *jni_make_activity_object(void);

// fake Java object / string constructors
void *jni_make_string(const char *utf);
void *jni_make_object(const char *label);
/* Build a real Il2Cpp-visible object array (TAG_OBJARR) of `len` slots all set
 * to `fill`. Use this instead of an opaque handle wherever a Java method's
 * signature says it returns an array -- GetArrayLength() tag-checks, so an
 * opaque handle silently reports length 0. */
void *jni_new_object_array(int len, void *fill);

/* Print the JNI approximation ledger: every call answered by a catch-all, with
 * hit counts and whether Unity read the result back (ExceptionCheck). Safe to
 * call from the exception handler -- it only walks a static table. No-op when
 * PHI_JNI_LOUD is 0. */
void jni_approx_summary(const char *why);

/* Record one JNI call answered by a catch-all. Callable from any dispatcher TU.
 * kind must be a string LITERAL (stored by pointer, never copied). Deduped on
 * (cls, name, sig); the next same-thread ExceptionCheck marks it INSPECTED. */
void jni_note_approx(const char *kind, const char *cls, const char *name,
                     const char *sig);

void *jni_new_empty_array(const char *sig);

#endif