/* nx_gpu_probe.c -- answer, at boot and in one log line, the question
 * "is this build getting HARDWARE ASTC, or is mesa silently software-decoding
 * every compressed texture?"
 *
 * BACKGROUND
 * ----------
 * libdrm_nouveau reports the GPU's ARCHITECTURE (0x120 = GM200) where mesa's
 * nvc0 driver expects the full CHIPSET id (0x12b = GM20B, the Switch's Tegra
 * X1). nvc0's format check is shaped like
 *
 *     if ((layout == ETC || layout == ASTC) && device->chipset != 0x12b && ...)
 *         return false;
 *
 * so mesa concludes the GPU cannot do ASTC -- on hardware that does it
 * natively. It does not error. It quietly falls back to software
 * decompression, expanding every ASTC texture to RGBA8: a 2048x2048 ASTC 6x6
 * is ~0.7 MB compressed and ~16 MB uncompressed, roughly 20x.
 *
 * The failure that follows is several layers removed from its cause:
 *   memalign in nouveau_bo_new returns NULL
 *     -> glTexStorage2D fails GL_OUT_OF_MEMORY (0x505), INCLUDING for Unity's
 *        render targets, not just art
 *     -> a render target with no backing store is an invalid attachment, so the
 *        FBO reports incomplete (0x8CD6, or 0x8CD7 if the attach is skipped)
 *     -> every draw into it is rejected with GL_INVALID_FRAMEBUFFER_OPERATION
 *        (0x506) -- the call returns, nothing rasterises
 *
 * Draw calls succeed, eglSwapBuffers succeeds, the viewport is right, and game
 * logic, audio and input are all unaffected, because none of them touch the
 * framebuffer. Only the pixels go missing. The Unity splash still renders,
 * because it draws to the default framebuffer, whose storage comes from EGL and
 * is always valid.
 *
 * WHAT THIS PROBE DOES
 * --------------------
 * Rather than infer any of that from a black screen, it measures it:
 *   1. logs GL_VENDOR / GL_RENDERER / GL_VERSION;
 *   2. reports whether GL_KHR_texture_compression_astc_ldr is advertised;
 *   3. uploads one real 512x512 ASTC 6x6 texture and measures the heap delta.
 *
 * Step 3 is the one that matters. The extension string can advertise ASTC while
 * mesa software-decodes behind it, so presence proves nothing. The allocation
 * does: 512x512 ASTC 6x6 is 116,736 bytes compressed and 1,048,576 bytes as
 * RGBA8. If the delta lands near the latter, the chipset id is wrong and this
 * build has the unpatched libdrm_nouveau.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <malloc.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include "util.h"      /* debugPrintf */

#ifndef GL_COMPRESSED_RGBA_ASTC_6x6_KHR
#define GL_COMPRESSED_RGBA_ASTC_6x6_KHR 0x93B4
#endif

#define PROBE_DIM   512
#define PROBE_BLK   6
/* ceil(512/6) == 86 blocks per axis, 16 bytes per block */
#define PROBE_BLOCKS ((PROBE_DIM + PROBE_BLK - 1) / PROBE_BLK)
#define PROBE_ASTC  ((size_t)PROBE_BLOCKS * PROBE_BLOCKS * 16)
#define PROBE_RGBA8 ((size_t)PROBE_DIM * PROBE_DIM * 4)

static size_t heap_in_use(void) {
  struct mallinfo mi = mallinfo();
  return (size_t)mi.uordblks;
}

void nx_gpu_probe(void) {
  const char *vend = (const char *)glGetString(GL_VENDOR);
  const char *rend = (const char *)glGetString(GL_RENDERER);
  const char *ver  = (const char *)glGetString(GL_VERSION);
  const char *ext  = (const char *)glGetString(GL_EXTENSIONS);

  debugPrintf("[gpuprobe] vendor=%s renderer=%s version=%s\n",
              vend ? vend : "?", rend ? rend : "?", ver ? ver : "?");

  int adv = (ext && strstr(ext, "GL_KHR_texture_compression_astc_ldr")) ? 1 : 0;
  debugPrintf("[gpuprobe] ASTC LDR advertised: %s\n", adv ? "yes" : "NO");
  if (!adv) {
    debugPrintf("[gpuprobe] *** mesa is not advertising ASTC at all. Textures will be\n"
                "[gpuprobe] *** software-decoded ~20x larger. This is the libdrm_nouveau\n"
                "[gpuprobe] *** chipset-id bug (reports 0x120 GM200, nvc0 wants 0x12b GM20B).\n");
  }

  /* The measurement. Extension presence does not prove hardware decode. */
  void *blob = calloc(1, PROBE_ASTC);
  if (!blob) { debugPrintf("[gpuprobe] probe alloc failed; skipping\n"); return; }

  while (glGetError() != GL_NO_ERROR) { }          /* drain */

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);

  size_t before = heap_in_use();
  glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_ASTC_6x6_KHR,
                         PROBE_DIM, PROBE_DIM, 0, (GLsizei)PROBE_ASTC, blob);
  GLenum err = glGetError();
  size_t after = heap_in_use();

  long delta = (long)after - (long)before;
  debugPrintf("[gpuprobe] ASTC 6x6 %dx%d upload: glGetError=0x%x heap delta=%ld bytes\n",
              PROBE_DIM, PROBE_DIM, err, delta);
  debugPrintf("[gpuprobe]   compressed would be %zu, RGBA8 would be %zu\n",
              PROBE_ASTC, PROBE_RGBA8);

  if (err == GL_OUT_OF_MEMORY) {
    debugPrintf("[gpuprobe] *** GL_OUT_OF_MEMORY on a 512x512 probe -- the GPU pool is\n"
                "[gpuprobe] *** already exhausted. Expect INCOMPLETE FBOs and a black screen.\n");
  } else if (delta > (long)(PROBE_RGBA8 * 3 / 4)) {
    debugPrintf("[gpuprobe] *** SOFTWARE ASTC DECODE DETECTED. The delta is ~RGBA8-sized,\n"
                "[gpuprobe] *** meaning mesa expanded the texture instead of handing it to\n"
                "[gpuprobe] *** the hardware. Your libdrm_nouveau is reporting chipset 0x120\n"
                "[gpuprobe] *** instead of 0x12b, and needs the GM20B patch. Every compressed\n"
                "[gpuprobe] *** texture in the game is being inflated ~20x.\n");
  } else if (delta < (long)(PROBE_ASTC * 2)) {
    debugPrintf("[gpuprobe] ASTC appears to be HARDWARE decoded (delta is compressed-sized).\n"
                "[gpuprobe] The libdrm_nouveau chipset fix is present and working.\n");
  } else {
    debugPrintf("[gpuprobe] inconclusive delta -- allocator may be pooling. Compare against\n"
                "[gpuprobe] the RGBA8 figure above rather than trusting this line.\n");
  }

  glDeleteTextures(1, &tex);
  free(blob);
  while (glGetError() != GL_NO_ERROR) { }
}
