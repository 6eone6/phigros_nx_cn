/* nx_dual_pointer.h -- two independent on-screen cursors for the Color Sheep
 * Switch port. Derived from the single-cursor nx_pointer module, rebuilt for
 * two simultaneous pointers that feed the game's existing multi-touch path.
 *
 * LEFT cursor  (blue): left stick, or the left Joy-Con's gyro.
 * RIGHT cursor (red):  right stick, or the right Joy-Con's gyro.
 * Both are live at all times, so a tap from each in the same frame delivers two
 * simultaneous fingers to the game (real two-finger input, e.g. pinch).
 *
 * Controls
 * --------
 *   Touchscreen   handheld only, always live (host still owns real touch)
 *   '+'           toggle both cursors on / off (hidden = not drawn, not tapping)
 *                 Default follows the dock state: OFF in handheld (the
 *                 touchscreen is right there), ON when docked. Pressing '+'
 *                 makes the choice explicit and stops the auto-follow.
 *   '-'           toggle gyro pointing on / off (affects both cursors)
 *   Left stick    move the LEFT (blue) cursor
 *   Right stick   move the RIGHT (red) cursor
 *   L             recenter the LEFT cursor to mid-screen
 *   R             recenter the RIGHT cursor to mid-screen
 *   ZL            tap with the LEFT cursor
 *   ZR            tap with the RIGHT cursor
 *   D-pad U/D     adjust the sensitivity of whatever is driving the cursors
 *                 (gyro if gyro is on, else the sticks) -- one control for both
 *   Gyro          tilt/turn to point: left Joy-Con drives the blue cursor, right
 *                 Joy-Con drives the red one. Yaw -> X, pitch -> Y.
 *
 * Custom cursor
 * -------------
 * If <data_dir>/cursor.png exists and is at most 64x64, it is used as the cursor
 * shape (alpha respected). The SAME image is drawn for both cursors, tinted blue
 * for the left and red for the right. If no PNG is present, a built-in vector
 * arrow is drawn, also tinted per side.
 *
 * Settings
 * --------
 * Stick and gyro sensitivities live in <data_dir>/pointer.cfg, loaded at startup
 * and saved 3 seconds after the last change (so a burst of D-pad presses costs
 * one write, not twenty).
 *
 * USB mouse support from the original module is retained in the source but
 * compiled out (NXDP_ENABLE_MOUSE 0); this port is touch + controller only.
 */
#ifndef NX_DUAL_POINTER_H
#define NX_DUAL_POINTER_H

#include <stdint.h>
#include <stdio.h>   /* FILE, for the optional locked-I/O hooks */

/* Pointer phases -- same values the port's touch path already uses
 * (NXP_DOWN/MOVE/UP == began/moved/ended). */
enum { NXDP_DOWN = 1, NXDP_MOVE = 2, NXDP_UP = 3 };

/* One pointer event, in Unity screen space (bottom-left origin, game px). */
typedef struct { int id; float x, y; int phase; } NxdpEvent;

typedef struct {
  int   screen_w, screen_h;      /* render space (Color Sheep: 720x1280 portrait) */
  int   panel_w,  panel_h;       /* touch panel space; 0 => 1280x720              */
  const char *data_dir;          /* where cursor.png / pointer.cfg live           */

  /* Pointer ids for the two cursors. Two constraints, and they pull opposite
   * ways:
   *   1. must not collide with a real finger id (fingers get 0..max_touch_slots-1)
   *   2. must stay SMALL. Unity maps an Android pointer id into a fixed-size
   *      touch pool; a large id (the old 100/101 default) is silently dropped,
   *      so the cursor draws and moves but its taps never reach the game.
   * So cap max_touch_slots and put the cursors just above it -- e.g. 8 slots
   * (fingers 0..7) with left_id 8 and right_id 9.
   * 0 => left uses 100, right uses 101 (kept for hosts that index differently). */
  int   left_id;
  int   right_id;

  /* Real touch fingers to track, 1..16. 0 => 8. Keep this plus the two cursor
   * ids inside whatever maximum the host's event path allows. */
  int   max_touch_slots;

  float stick_speed;             /* px/frame at full deflection; 0 => 14          */

  /* Screen rotation for TATE ports (Color Sheep renders portrait while the panel
   * is landscape). Cursors live in RENDER space; physical device deltas
   * (stick/gyro) and touch are rotated to match:
   *   0 = none, 1 = render rotated 90 CW, 2 = render rotated 90 CCW.             */
  int   rotation;

  /* If 0, the module does NOT read the touchscreen (the host feeds real touch
   * itself, as Color Sheep does). Nonzero => module reads touch too.            */
  int   handle_touch;

  void (*log)(const char *msg);  /* optional; may be NULL                        */

  /* OPTIONAL locked file I/O. so-loader ports must serialise newlib file calls
   * (devkitPro's handle table is not thread-safe and the engine's workers hammer
   * it). Pass the port's locked fopen/fclose; leave NULL for plain fopen/fclose. */
  FILE *(*fopen_fn)(const char *path, const char *mode);
  int   (*fclose_fn)(FILE *f);
} NxdpConfig;

/* Call once, EARLY (before the engine spawns threads): reads cursor.png off the
 * SD card. Decode + GL upload happen lazily on the render thread in nxdp_draw. */
void nxdp_init(const NxdpConfig *cfg);

/* Once per frame, before nxdp_poll(). Reads pad/touch and builds events. */
void nxdp_update(void);

/* Drain this frame's pointer events (both cursors + any touch). Returns count. */
int  nxdp_poll(NxdpEvent *out, int max);

/* Draw both cursors. Call with the engine's GL context current, just before the
 * real eglSwapBuffers. Saves/restores all GL state it touches. */
void nxdp_draw(void);

/* Queries */
void  nxdp_left_pos(float *x, float *y);
void  nxdp_right_pos(float *x, float *y);
float nxdp_stick_speed(void);
float nxdp_gyro_sens(void);
int   nxdp_gyro_enabled(void);

/* Nonzero while the cursors are shown and active ('+' toggles them off).
 * nxdp_draw() already early-returns when hidden; this lets a host skip the GL
 * state save/restore it wraps the call in. */
int   nxdp_cursor_visible(void);

/* Cursor-as-finger snapshot, for a host that owns the touch->engine merge itself
 * (e.g. Color Sheep, whose feed already maps real fingers and derives phases by
 * matching ids frame to frame). Instead of dr0aining pre-phased events via
 * nxdp_poll, the host calls nxdp_update() then this, and appends the returned
 * fingers to its own touch list. A cursor contributes a finger ONLY while it is
 * tapping (ZL/ZR held); id is the cursor's pointer id so phases track correctly.
 * Writes up to `max` fingers into out[], returns the count (0..2). */
typedef struct { int id; float x, y; } NxdpFinger;
int   nxdp_tapping_fingers(NxdpFinger *out, int max);

/* Flush any pending settings write now (e.g. on shutdown). */
void  nxdp_save_settings(void);

#endif /* NX_DUAL_POINTER_H */
