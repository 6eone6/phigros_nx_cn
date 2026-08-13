/* android_native_unity.h -- declarations for the NDK surface implemented in
 * android_native_unity.c (replaces cr3_nx's android_native.h for this port).
 * Lets imports.c register these and main.c call the lifecycle helpers. */
#ifndef ANDROID_NATIVE_UNITY_H
#define ANDROID_NATIVE_UNITY_H

#include <stdint.h>

typedef struct ANativeWindow ANativeWindow;
typedef struct ALooper       ALooper;

/* host helpers (called from main.c) */
void  android_native_input_init(void);
void  android_native_input_shutdown(void);  /* flushes a pending pointer.cfg save */
void  android_native_update_mode(void);
ANativeWindow *android_native_window(void);
uint32_t android_native_width(void);
uint32_t android_native_height(void);
void  android_native_feed_hid(uint8_t (*inject)(void*,void*,void*,int),
                              void *env, void *thiz);
void  android_native_draw_cursor(void);   /* cursor overlay; called by the swap wrapper */

/* NDK functions libunity imports (register these in imports.c) */
void     ANativeWindow_acquire(ANativeWindow *);
void     ANativeWindow_release(ANativeWindow *);
ANativeWindow *ANativeWindow_fromSurface(void *, void *);
int32_t  ANativeWindow_getWidth(ANativeWindow *);
int32_t  ANativeWindow_getHeight(ANativeWindow *);
int32_t  ANativeWindow_getFormat(ANativeWindow *);
int32_t  ANativeWindow_setBuffersGeometry(ANativeWindow *, int32_t, int32_t, int32_t);

ALooper *ALooper_prepare(int);
ALooper *ALooper_forThread(void);
void     ALooper_acquire(ALooper *);
void     ALooper_release(ALooper *);
void     ALooper_wake(ALooper *);
int      ALooper_pollOnce(int, int *, int *, void **);
int      ALooper_addFd(ALooper *, int, int, int, void *, void *);
int      ALooper_removeFd(ALooper *, int);

void *ASensorManager_getInstance(void);
void *ASensorManager_getInstanceForPackage(const char *);
int   ASensorManager_getSensorList(void *, void **);
void *ASensorManager_getDefaultSensor(void *, int);
void *ASensorManager_createEventQueue(void *, void *, int, void *, void *);
int   ASensorManager_destroyEventQueue(void *, void *);
int   ASensorEventQueue_enableSensor(void *, const void *);
int   ASensorEventQueue_disableSensor(void *, const void *);
int   ASensorEventQueue_setEventRate(void *, const void *, int32_t);
int   ASensorEventQueue_getEvents(void *, void *, size_t);
int   ASensorEventQueue_hasEvents(void *);
const char *ASensor_getName(const void *);
const char *ASensor_getVendor(const void *);
int         ASensor_getType(const void *);
float       ASensor_getResolution(const void *);
int         ASensor_getMinDelay(const void *);

void android_get_orientation(float *x, float *y, float *z); /* cr3 dead-handler stub */

/* fake-fd pipe layer (fakefd.c) -- libc_shim.c routes pipe()/read/write/close here */
int  fakefd_is_fake(int fd);
int  fakefd_pipe(int fds[2]);
long fakefd_read(int fd, void *buf, unsigned long n);
long fakefd_write(int fd, const void *buf, unsigned long n);
int  fakefd_close(int fd);

#endif /* ANDROID_NATIVE_UNITY_H */
