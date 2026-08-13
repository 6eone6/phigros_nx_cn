/* phigros_jni.h -- game-specific JNI fakes for Phigros. See phigros_jni.c. */
#ifndef PHIGROS_JNI_H
#define PHIGROS_JNI_H

#include <stdint.h>
#include <stdarg.h>

void      phigros_jni_init(void);
int       phigros_owns_class(const char *cls);          /* 1 if we handle it */

void     *phigros_dispatch_object(void *recv, const void *id, va_list va);
uint64_t  phigros_dispatch_int   (void *recv, const void *id, va_list va);
void      phigros_dispatch_void  (void *recv, const void *id, va_list va);
float     phigros_dispatch_float (void *recv, const void *id, va_list va);

/* Guarded replacement for a NativeAudio playback export, or NULL if we do not
 * guard that symbol. Consulted by dlsym_fake before the raw export. */
void     *phigros_audio_guard(const char *sym);

/* Field defaults. Returns 1 and writes *out if this module owns the field. */
int       phigros_field_int(const void *id, uint64_t *out);

#endif /* PHIGROS_JNI_H */
