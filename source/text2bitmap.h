/* text2bitmap.h -- native replacement for local.mediav.Text2Bitmap
 *
 * The engine renders all of its dynamic text (names, numbers, messages) by
 * asking the Java Text2Bitmap helper to draw a string into an android.graphics
 * Bitmap, then reads the pixels back through the AndroidBitmap NDK API and
 * uploads them as a GL texture. We reproduce that with FreeType over the Switch
 * shared system fonts (with CJK fallback) and hand back a FakeBitmap the
 * AndroidBitmap_* shims understand.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __TEXT2BITMAP_H__
#define __TEXT2BITMAP_H__

#include <stdint.h>

#define BITMAP_TAG 0x424d5031u /* 'BMP1' */

// The engine's TextRenderer reads every text bitmap as 1 byte/pixel (it allocs
// width*height and memcpys width*height -- verified by disassembly), i.e. it
// expects ANDROID_BITMAP_FORMAT_A_8 and ignores stride/format. So we hand back
// an 8-bit coverage (alpha) bitmap, not RGBA_8888.
#define ANDROID_BITMAP_FORMAT_A_8 8

typedef struct {
  uint32_t  tag;       // BITMAP_TAG, so the JNI local-ref table can free it
  int       w, h;
  int       stride;    // bytes per row (== w for A_8)
  int       format;    // ANDROID_BITMAP_FORMAT_A_8
  uint8_t  *pixels;    // w*h, 1 byte per pixel = glyph coverage
} FakeBitmap;

// load FreeType + the shared system fonts. Safe to call before plInitialize
// only if plInitialize already ran; main.c orders this correctly.
void text2bitmap_init(void);

// render UTF-8 `text` at `pixel_size` into a tightly-fitted RGBA bitmap (white
// glyphs, coverage in alpha; the engine tints via its shader). Caller/engine
// frees it via the JNI bitmap recycle / DeleteLocalRef path. NULL on failure.
FakeBitmap *text2bitmap_render(const char *text, int pixel_size);

// measure the rendered width / line height in pixels at `pixel_size`.
int text2bitmap_measure_width(const char *text, int pixel_size);
int text2bitmap_measure_height(const char *text, int pixel_size);

// free a bitmap created by text2bitmap_render.
void text2bitmap_free(FakeBitmap *bmp);

// AndroidBitmap NDK API over FakeBitmap (registered in imports.c)
int AndroidBitmap_getInfo(void *env, void *jbitmap, void *info);
int AndroidBitmap_lockPixels(void *env, void *jbitmap, void **addrPtr);
int AndroidBitmap_unlockPixels(void *env, void *jbitmap);

#endif
