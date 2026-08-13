/* unity_jni_surface.h -- ZOOKEEPER DX (Unity 2022.3.62f2 / IL2CPP)
 *
 * The Java classes libunity.so resolves via FindClass at runtime, enumerated
 * from the binary. Feed these into jni_fake.c's class pool. Each entry is
 * tagged IMPLEMENT (needs real behaviour) or STUB (return defaults / no-op).
 *
 * This is a data table, not working code -- it tells you exactly which classes
 * the fake JNIEnv must recognise so FindClass never returns NULL (a NULL class
 * is what makes these engines abort early).
 */
#ifndef UNITY_JNI_SURFACE_H
#define UNITY_JNI_SURFACE_H

enum { JNI_STUB = 0, JNI_IMPLEMENT = 1 };

typedef struct { const char *name; int mode; const char *note; } unity_jni_class;

static const unity_jni_class UNITY_JNI_CLASSES[] = {
  /* ---- IMPLEMENT: small, real behaviour ---- */
  { "android/content/res/AssetManager",     JNI_IMPLEMENT, "open()/openFd() -> read staged files from SD" },
  { "android/content/SharedPreferences",    JNI_IMPLEMENT, "PlayerPrefs save data; getX/contains" },
  { "android/content/SharedPreferences$Editor", JNI_IMPLEMENT, "putX/remove/commit -> persist to SD" },
  { "android/view/Display",                 JNI_IMPLEMENT, "getRealSize/getRefreshRate -> Switch values" },
  { "android/hardware/display/DisplayManager", JNI_IMPLEMENT, "getDisplay -> single display" },
  { "android/content/res/Configuration",    JNI_IMPLEMENT, "density/locale -> static" },
  { "android/content/res/Resources",        JNI_IMPLEMENT, "getConfiguration/getDisplayMetrics" },
  { "com/unity3d/player/UnityPlayer",       JNI_IMPLEMENT, "host callbacks; mostly no-op + display queries" },

  /* ---- STUB: return benign defaults / no-op ---- */
  { "android/app/Activity",                 JNI_STUB, NULL },
  { "android/app/NativeActivity",           JNI_STUB, NULL },
  { "android/app/AlertDialog",              JNI_STUB, NULL },
  { "android/app/AlertDialog$Builder",      JNI_STUB, NULL },
  { "android/app/Dialog",                   JNI_STUB, NULL },
  { "android/app/Presentation",             JNI_STUB, NULL },
  { "android/content/Context",              JNI_STUB, "getFilesDir/getPackageName may need a string back" },
  { "android/content/Intent",               JNI_STUB, NULL },
  { "android/content/IntentFilter",         JNI_STUB, NULL },
  { "android/content/ComponentName",        JNI_STUB, NULL },
  { "android/content/ContentResolver",      JNI_STUB, NULL },
  { "android/content/DialogInterface",      JNI_STUB, NULL },
  { "android/content/pm/PackageManager",    JNI_STUB, "getPackageInfo -> versionName/Code strings" },
  { "android/content/pm/PackageInfo",       JNI_STUB, NULL },
  { "android/content/pm/ApplicationInfo",   JNI_STUB, NULL },
  { "android/content/pm/ActivityInfo",      JNI_STUB, NULL },
  { "android/content/pm/FeatureInfo",       JNI_STUB, NULL },
  { "android/hardware/Sensor",              JNI_STUB, "no sensors" },
  { "android/hardware/SensorManager",       JNI_STUB, "getSensorList -> empty" },
  { "android/hardware/GeomagneticField",    JNI_STUB, NULL },
  { "android/hardware/input/InputManager",  JNI_STUB, "HID fed natively instead" },
  { "android/graphics/Bitmap",              JNI_STUB, NULL },
  { "android/graphics/Bitmap$Config",       JNI_STUB, NULL },
  { "android/graphics/Rect",                JNI_STUB, NULL },
  { "android/graphics/SurfaceTexture",      JNI_STUB, "no external/video textures" },
  { "com/unity3d/player/AudioVolumeHandler",JNI_STUB, NULL },
  { "com/unity3d/player/Camera2Wrapper",    JNI_STUB, "no camera" },
  { "com/unity3d/player/GoogleARCoreApi",   JNI_STUB, "no ARCore" },
  { "com/unity3d/player/HFPStatus",         JNI_STUB, "no bluetooth headset" },
  { "com/unity3d/player/OrientationLockListener", JNI_STUB, NULL },
  { "com/unity3d/player/ReflectionHelper",  JNI_STUB, NULL },
  { "com/unity3d/player/SoftInputProvider", JNI_STUB, "stub soft keyboard initially" },
  { "com/unity3d/player/PlayAssetDeliveryUnityWrapper", JNI_STUB, "verify no PAD-only assets" },
  { "com/unity3d/player/UnityCoreAssetPacksStatusCallbacks", JNI_STUB, NULL },
  { "com/unity3d/player/IAssetPackManagerDownloadStatusCallback", JNI_STUB, NULL },
  { "com/unity3d/player/IAssetPackManagerStatusQueryCallback", JNI_STUB, NULL },

  { NULL, 0, NULL }
};

/* NDK functions libunity.so imports from libandroid.so (shim these in
 * android_native.c / imports.c). Sensors are stubs. */
/*
   ANativeWindow_acquire  ANativeWindow_release  ANativeWindow_fromSurface
   ANativeWindow_setBuffersGeometry  ANativeWindow_getWidth/getHeight/getFormat
   ALooper_prepare  ALooper_acquire  ALooper_release  ALooper_pollOnce
   ALooper_wake  ALooper_forThread
   ASensorManager_*  ASensorEventQueue_*  ASensor_get{Name,Type,Resolution,MinDelay,Vendor}
*/

#endif /* UNITY_JNI_SURFACE_H */
