/* cr3_stubs.c -- no-op bodies for the Chaos-Rings-specific helpers that
 * jni_fake.c's fallback handlers (t2b, mov, act) still reference.
 *
 * For ZOOKEEPER DX these handlers are DEAD CODE: jni_fake.c now delegates every
 * Unity/input class to unity_jni.c / unity_input.c before reaching them. They
 * only need to link, so everything here returns a benign default. This replaces
 * cr3_nx data.c / text2bitmap.c / movie_player.c / editbox.c, which would pull
 * in FreeType and FFmpeg that this port does not need.
 *
 * If a stub ever actually fires at runtime, a class slipped past the delegation
 * in unity_owns_class() / input_owns_class() -- add it there.
 */

#include <stddef.h>
#include "data.h"
#include "text2bitmap.h"
#include "movie_player.h"
#include "editbox.h"
#include "config.h"      /* GAME_HOME == nx_root(), resolved at runtime */
#include "nx_root.h"

/* ---- data layer ---- */
const char *data_dir(void) { return GAME_HOME; }  /* == nx_root(): any folder under /switch */

/* cr3 threading bookkeeping: imports.c's thread_trampoline calls this when the
 * first engine thread returns. Our main loop uses jni_quit_requested instead, so
 * this is a no-op. */
void android_mark_main_finished(void) {}

/* ---- text rendering (engine's dynamic text path; Unity doesn't use it) ---- */
FakeBitmap *text2bitmap_render(const char *text, int pixel_size) { (void)text; (void)pixel_size; return NULL; }
int  text2bitmap_measure_width (const char *text, int pixel_size) { (void)text; (void)pixel_size; return 0; }
int  text2bitmap_measure_height(const char *text, int pixel_size) { (void)text; (void)pixel_size; return 0; }
void text2bitmap_free(FakeBitmap *bmp) { (void)bmp; }

/* ---- FMV playback (unused) ---- */
void movie_set_db(const char *db_path) { (void)db_path; }
int  movie_play(const char *name, int looping) { (void)name; (void)looping; return 0; }
void movie_stop(void)   {}
void movie_pause(void)  {}
void movie_resume(void) {}
int  movie_is_playing(void) { return 0; }

/* ---- software keyboard: implemented for real in editbox.c (libnx swkbd) ---- */
