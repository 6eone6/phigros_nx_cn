/* nx_dual_pointer.c -- see nx_dual_pointer.h.
 *
 * Two independent cursors (left/blue, right/red) for the Color Sheep Switch port.
 * Left stick + left Joy-Con gyro drive the blue cursor; right stick + right
 * Joy-Con gyro drive the red one. Both feed the game's multi-touch path as extra
 * fingers, so simultaneous taps become simultaneous touches.
 *
 * Structure and the GL-overlay / PNG-decode logic derive from the single-cursor
 * nx_pointer module; the per-side cursor state, split-Joy-Con gyro, and two-tint
 * drawing are new. USB mouse support is retained but compiled out (see below).
 */
#include <switch.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>
#include <png.h>

#include "nx_dual_pointer.h"

/* This port is touch + controller only. The original module's USB-mouse code is
 * kept in-tree (it is proven and may be wanted by another port) but compiled out.
 * Flip to 1 to bring it back -- it would drive whichever cursor you route it to. */
#define NXDP_ENABLE_MOUSE 0

/* ------------------------------------------------------------------ config */

static NxdpConfig s_cfg;
static int        s_ready = 0;

static void logf_(const char *fmt, ...) {
  if (!s_cfg.log) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  s_cfg.log(buf);
}

/* --------------------------------------------------------------- constants */

#define LEFT   0
#define RIGHT  1

#define STICK_MIN   2.0f
#define STICK_MAX  60.0f
#define GYRO_MIN    0.10f
#define GYRO_MAX    8.0f

/* Raw angular velocity -> px/frame gain (see nx_pointer notes: a normal turn is
 * |av| ~0.14, so this puts a comfortable turn around a dozen px/frame). */
#define GYRO_GAIN 120.0f

#define SETTINGS_DEBOUNCE_NS  3000000000ULL   /* 3 seconds */

/* ------------------------------------------------------------------- state */

static PadState s_pad;
static int   s_rotation     = 0;
static int   s_handle_touch = 0;

/* touch (handheld), only used when s_handle_touch != 0 */
static int   s_touch_active[16];
static int   s_touch_slots = 8;       /* real fingers tracked; see max_touch_slots */
static float s_touch_x[16], s_touch_y[16];

/* The two cursors. Index with LEFT / RIGHT. */
static float s_cx[2], s_cy[2];        /* position, render space   */
static int   s_tap_prev[2] = {0, 0};  /* tap held last frame      */
static int   s_tap_now[2]  = {0, 0};  /* tap held THIS frame (for finger query) */
#if PHI_VIRTUAL_CURSOR
static int   s_visible     = 1;       /* both cursors shown+active; toggled by '+' */
#else
/* Cursors removed (config.h PHI_VIRTUAL_CURSOR 0). Kept as a constant 0 rather
 * than #ifdef-ing every use: the compiler folds the branches away, the touch
 * path above is untouched, and turning the feature back on is a one-line
 * change instead of a merge. */
#define s_visible 0
#endif
/* In handheld the touchscreen is right there, so cursors are clutter; docked
 * they are the only way to point. Start hidden in handheld, shown when docked,
 * and keep following the dock state UNTIL the player presses '+' -- after that
 * their choice is explicit and sticks for the session. */
static int   s_vis_user_set = 0;      /* '+' pressed: stop auto-following mode */

static int nxdp_docked(void) {
  return appletGetOperationMode() == AppletOperationMode_Console;
}
static int   s_id[2];                 /* pointer id per cursor    */

/* tunables, adjusted live and persisted */
static float s_stick_speed;
static float s_gyro_sens;

/* gyro: one handle per controller style; per-side Joy-Con handles for dual. */
static HidSixAxisSensorHandle s_six_handheld;
static HidSixAxisSensorHandle s_six_fullkey;
static HidSixAxisSensorHandle s_six_dual[2];   /* [0]=left Joy-Con, [1]=right */
static int s_gyro_ready = 0;
static int s_gyro_on    = 0;
static int s_gyro_logged = 0;

/* settings persistence (debounced) */
static int s_settings_dirty = 0;
static u64 s_settings_tick  = 0;

/* d-pad auto-repeat */
static int s_dpad_hold = 0;

/* events emitted this frame */
static NxdpEvent s_ev[32];
static int       s_nev;

/* ------------------------------------------------------- settings file ---
 * Use the port's locked fopen/fclose when supplied: the engine's worker threads
 * do file I/O constantly and newlib's handle table is not thread-safe. */
static FILE *cfg_fopen(const char *path, const char *mode) {
  return s_cfg.fopen_fn ? s_cfg.fopen_fn(path, mode) : fopen(path, mode);
}
static int cfg_fclose(FILE *f) {
  return s_cfg.fclose_fn ? s_cfg.fclose_fn(f) : fclose(f);
}

static void clamp_settings(void) {
  if (s_stick_speed < STICK_MIN) s_stick_speed = STICK_MIN;
  if (s_stick_speed > STICK_MAX) s_stick_speed = STICK_MAX;
  if (s_gyro_sens   < GYRO_MIN)  s_gyro_sens   = GYRO_MIN;
  if (s_gyro_sens   > GYRO_MAX)  s_gyro_sens   = GYRO_MAX;
}

static void settings_path(char *out, size_t n) {
  snprintf(out, n, "%s/pointer.cfg", s_cfg.data_dir ? s_cfg.data_dir : ".");
}

static void settings_load(void) {
  if (!s_cfg.data_dir) return;
  char path[512];
  settings_path(path, sizeof path);

  FILE *f = cfg_fopen(path, "r");
  if (!f) { logf_("nxdp: no pointer.cfg -- using defaults\n"); return; }

  char line[128];
  while (fgets(line, sizeof line, f)) {
    float v;
    if      (sscanf(line, "stick=%f", &v) == 1) s_stick_speed = v;
    else if (sscanf(line, "gyro=%f",  &v) == 1) s_gyro_sens   = v;
  }
  cfg_fclose(f);
  clamp_settings();
  logf_("nxdp: settings loaded  stick=%.1f gyro=%.2f\n", s_stick_speed, s_gyro_sens);
}

void nxdp_save_settings(void) {
  if (!s_ready || !s_cfg.data_dir) return;
  char path[512];
  settings_path(path, sizeof path);

  FILE *f = cfg_fopen(path, "w");
  if (!f) { logf_("nxdp: could not write %s\n", path); s_settings_dirty = 0; return; }

  fprintf(f, "# nx_dual_pointer settings -- auto-saved, safe to edit\n");
  fprintf(f, "stick=%.2f\n", s_stick_speed);
  fprintf(f, "gyro=%.2f\n",  s_gyro_sens);
  cfg_fclose(f);

  s_settings_dirty = 0;
  logf_("nxdp: settings saved  stick=%.1f gyro=%.2f\n", s_stick_speed, s_gyro_sens);
}

static void settings_touch(void) {
  s_settings_dirty = 1;
  s_settings_tick  = armGetSystemTick();
}

static void settings_tick(void) {
  if (!s_settings_dirty) return;
  if (armTicksToNs(armGetSystemTick() - s_settings_tick) >= SETTINGS_DEBOUNCE_NS)
    nxdp_save_settings();
}

/* --------------------------------------------------- custom cursor (PNG) */

#define CURSOR_MAX_DIM 64

/* Raw file bytes slurped at init (single-threaded); decoded + uploaded lazily on
 * the render thread in nxdp_draw so the render thread never hits the filesystem. */
static uint8_t *s_png_bytes = NULL;
static size_t   s_png_len   = 0;

static GLuint s_cursor_tex = 0;
static int    s_cursor_w = 0, s_cursor_h = 0;
static int    s_png_tried = 0;

static void slurp_cursor_png(void) {
#if !PHI_VIRTUAL_CURSOR
  return;   /* no cursor in this build: skip the SD read entirely */
#else
  if (!s_cfg.data_dir) return;
  char path[512];
  snprintf(path, sizeof path, "%s/cursor.png", s_cfg.data_dir);

  FILE *f = cfg_fopen(path, "rb");
  if (!f) { logf_("nxdp: no cursor.png (using built-in arrow)\n"); return; }

  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len <= 0 || len > 4 * 1024 * 1024) { cfg_fclose(f); return; }

  s_png_bytes = malloc((size_t)len);
  if (!s_png_bytes) { cfg_fclose(f); return; }
  s_png_len = fread(s_png_bytes, 1, (size_t)len, f);
  cfg_fclose(f);

  if (s_png_len != (size_t)len) { free(s_png_bytes); s_png_bytes = NULL; s_png_len = 0; return; }
  logf_("nxdp: cursor.png loaded (%zu bytes), decoding on first frame\n", s_png_len);
#endif
}

typedef struct { const uint8_t *p; size_t len, off; } PngSrc;

static void png_read_mem(png_structp png, png_bytep out, png_size_t n) {
  PngSrc *s = (PngSrc *)png_get_io_ptr(png);
  if (s->off + n > s->len) { png_error(png, "short read"); return; }
  memcpy(out, s->p + s->off, n);
  s->off += n;
}

/* Decode to RGBA8 and upload. Returns 1 on success. Render thread only. */
static int cursor_upload_png(void) {
  if (!s_png_bytes) return 0;

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  if (!png) return 0;
  png_infop info = png_create_info_struct(png);
  if (!info) { png_destroy_read_struct(&png, NULL, NULL); return 0; }

  uint8_t   *pixels = NULL;
  png_bytep *rows   = NULL;
  if (setjmp(png_jmpbuf(png))) {
    free(pixels); free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    logf_("nxdp: cursor.png decode failed -- using built-in arrow\n");
    return 0;
  }

  PngSrc src = { s_png_bytes, s_png_len, 0 };
  png_set_read_fn(png, &src, png_read_mem);
  png_read_info(png, info);

  const png_uint_32 w = png_get_image_width(png, info);
  const png_uint_32 h = png_get_image_height(png, info);
  if (w == 0 || h == 0 || w > CURSOR_MAX_DIM || h > CURSOR_MAX_DIM) {
    logf_("nxdp: cursor.png is %ux%u -- max is %dx%d, using built-in arrow\n",
          (unsigned)w, (unsigned)h, CURSOR_MAX_DIM, CURSOR_MAX_DIM);
    png_destroy_read_struct(&png, &info, NULL);
    return 0;
  }

  /* normalise anything to 8-bit RGBA so transparency always works */
  const int ct = png_get_color_type(png, info);
  const int bd = png_get_bit_depth(png, info);
  if (bd == 16)                       png_set_strip_16(png);
  if (ct == PNG_COLOR_TYPE_PALETTE)   png_set_palette_to_rgb(png);
  if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
  if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png);
  png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
  png_read_update_info(png, info);

  const size_t stride = (size_t)w * 4;
  pixels = malloc(stride * h);
  rows   = malloc(sizeof(png_bytep) * h);
  if (!pixels || !rows) { png_error(png, "oom"); }
  for (png_uint_32 y = 0; y < h; y++) rows[y] = pixels + y * stride;
  png_read_image(png, rows);
  png_read_end(png, NULL);
  png_destroy_read_struct(&png, &info, NULL);
  free(rows);

  /* Save the texture binding we disturb -- this runs inside the engine's context. */
  GLint prev_active = 0, prev_tex = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

  glGenTextures(1, &s_cursor_tex);
  glBindTexture(GL_TEXTURE_2D, s_cursor_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  GLint prev_align = 4;
  glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_align);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, pixels);
  glPixelStorei(GL_UNPACK_ALIGNMENT, prev_align);
  free(pixels);

  glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
  glActiveTexture((GLenum)prev_active);

  s_cursor_w = (int)w;
  s_cursor_h = (int)h;
  logf_("nxdp: custom cursor %dx%d ready (tex=%u)\n", s_cursor_w, s_cursor_h, s_cursor_tex);
  return 1;
}

/* ------------------------------------------------------------------- init */

void nxdp_init(const NxdpConfig *cfg) {
  if (s_ready) return;
  memset(&s_cfg, 0, sizeof s_cfg);
  if (cfg) s_cfg = *cfg;

  if (s_cfg.panel_w <= 0)  s_cfg.panel_w = 1280;
  if (s_cfg.panel_h <= 0)  s_cfg.panel_h = 720;
  s_rotation     = s_cfg.rotation;
  s_handle_touch = s_cfg.handle_touch;
  if (s_cfg.screen_w <= 0) s_cfg.screen_w = 720;
  if (s_cfg.screen_h <= 0) s_cfg.screen_h = 1280;

  s_id[LEFT]  = (s_cfg.left_id  > 0) ? s_cfg.left_id  : 100;
  s_id[RIGHT] = (s_cfg.right_id > 0) ? s_cfg.right_id : 101;

#if PHI_VIRTUAL_CURSOR
  s_visible      = nxdp_docked();     /* hidden in handheld by default */
#endif
  s_vis_user_set = 0;

  s_touch_slots = (s_cfg.max_touch_slots > 0) ? s_cfg.max_touch_slots : 8;
  if (s_touch_slots > 16) s_touch_slots = 16;

  /* A cursor id inside the finger range would make a cursor and a real finger
   * fight over one pointer -- shout rather than fail quietly. */
  for (int c2 = 0; c2 < 2; c2++)
    if (s_id[c2] < s_touch_slots)
      logf_("[nxdp] WARNING: cursor id %d collides with finger ids 0..%d\n",
            s_id[c2], s_touch_slots - 1);

  s_stick_speed = (s_cfg.stick_speed > 0.0f) ? s_cfg.stick_speed : 14.0f;
  s_gyro_sens   = 1.0f;

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&s_pad);
  if (s_handle_touch) hidInitializeTouchScreen();

  /* Six-axis handles. Dual Joy-Con exposes TWO sensors (left + right); grab both
   * so each cursor can be driven by its own Joy-Con. */
  Result rc0 = hidGetSixAxisSensorHandles(&s_six_handheld, 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
  Result rc1 = hidGetSixAxisSensorHandles(&s_six_fullkey,  1, HidNpadIdType_No1,      HidNpadStyleTag_NpadFullKey);
  Result rc2 = hidGetSixAxisSensorHandles(s_six_dual,       2, HidNpadIdType_No1,      HidNpadStyleTag_NpadJoyDual);
  if (R_SUCCEEDED(rc0) && R_SUCCEEDED(rc1) && R_SUCCEEDED(rc2)) {
    hidStartSixAxisSensor(s_six_handheld);
    hidStartSixAxisSensor(s_six_fullkey);
    hidStartSixAxisSensor(s_six_dual[0]);
    hidStartSixAxisSensor(s_six_dual[1]);
    s_gyro_ready = 1;
  } else {
    logf_("nxdp: gyro unavailable (sensor handles failed)\n");
  }

  /* start both cursors at mid-screen, nudged apart so they don't overlap */
  s_cx[LEFT]  = s_cfg.screen_w * 0.35f;  s_cy[LEFT]  = s_cfg.screen_h * 0.5f;
  s_cx[RIGHT] = s_cfg.screen_w * 0.65f;  s_cy[RIGHT] = s_cfg.screen_h * 0.5f;

  settings_load();
  slurp_cursor_png();

  s_ready = 1;
  logf_("nxdp: init %dx%d (panel %dx%d) stick=%.1f  ids L=%d R=%d\n",
        s_cfg.screen_w, s_cfg.screen_h, s_cfg.panel_w, s_cfg.panel_h,
        s_stick_speed, s_id[LEFT], s_id[RIGHT]);
}

/* ---------------------------------------------------------------- helpers */

static void clamp_cursor(int c) {
  if (s_cx[c] < 0) s_cx[c] = 0;
  if (s_cy[c] < 0) s_cy[c] = 0;
  if (s_cx[c] > s_cfg.screen_w - 1) s_cx[c] = (float)(s_cfg.screen_w - 1);
  if (s_cy[c] > s_cfg.screen_h - 1) s_cy[c] = (float)(s_cfg.screen_h - 1);
}

static void push(int id, float x, float y, int phase) {
  if (s_nev >= (int)(sizeof s_ev / sizeof s_ev[0])) return;
  NxdpEvent *e = &s_ev[s_nev++];
  e->id = id; e->x = x; e->y = y; e->phase = phase;
}

/* Rotate a physical device delta (panel coords: +x right, +y down) into render
 * space, matching the compositor transform the host applied. Same mapping as the
 * single-cursor module. Pass 0 for a controller held upright (detached Joy-Cons). */
static void rot_delta(int rot, float dpx, float dpy, float *dgx, float *dgy) {
  switch (rot) {
    case 1:  *dgx =  dpy; *dgy = -dpx; break;
    case 2:  *dgx = -dpy; *dgy =  dpx; break;
    default: *dgx =  dpx; *dgy =  dpy; break;
  }
}

/* Rotation for stick/gyro input: only when the display is rotated AND this frame's
 * input came from the attached handheld controller (which turns with the screen).
 * Detached Joy-Cons / docked are upright -> straight through. */
static int ctrl_rot(void) {
  return (s_rotation && padIsHandheld(&s_pad)) ? s_rotation : 0;
}

static void do_touch(void) {
  if (!s_handle_touch) return;
  HidTouchScreenState ts = {0};
  hidGetTouchScreenStates(&ts, 1);

  const int slots = s_touch_slots;    /* fingers get ids 0..slots-1 */
  int now[16] = {0};
  int count = ts.count > slots ? slots : ts.count;

  const float sw = (float)s_cfg.screen_w, sh = (float)s_cfg.screen_h;
  const float pw = (float)s_cfg.panel_w,  ph = (float)s_cfg.panel_h;

  for (int i = 0; i < count; i++) {
    float px = (float)ts.touches[i].x, py = (float)ts.touches[i].y;
    float x, y;
    if      (s_rotation == 1) { x =        py * (sw/ph); y = (pw-px) * (sh/pw); }
    else if (s_rotation == 2) { x = (ph-py) * (sw/ph); y =     px  * (sh/pw); }
    else                      { x =   px * (sw/pw);     y =     py  * (sh/ph); }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > sw - 1) x = sw - 1;
    if (y > sh - 1) y = sh - 1;
    now[i] = 1;
    /* real fingers keep their own small ids (0..n), distinct from cursor ids */
    push(i, x, y, s_touch_active[i] ? NXDP_MOVE : NXDP_DOWN);
    s_touch_x[i] = x; s_touch_y[i] = y;
  }
  for (int i = 0; i < slots; i++) {
    if (s_touch_active[i] && !now[i])
      push(i, s_touch_x[i], s_touch_y[i], NXDP_UP);
    s_touch_active[i] = now[i];
  }
}

/* D-pad U/D adjusts sensitivity of whatever is currently driving the cursors:
 * gyro if gyro is on, else the sticks. One control, both cursors. Auto-repeats. */
static void do_dpad(u64 held, u64 pressed) {
  const u64 any = HidNpadButton_Up | HidNpadButton_Down;

  int repeat = 0;
  if (held & any) {
    s_dpad_hold++;
    if (s_dpad_hold > 24 && (s_dpad_hold % 3) == 0) repeat = 1;
  } else {
    s_dpad_hold = 0;
  }

  int step = 0;
  if (pressed & HidNpadButton_Up)   step = +1;
  if (pressed & HidNpadButton_Down) step = -1;
  if (repeat && (held & HidNpadButton_Up))   step = +1;
  if (repeat && (held & HidNpadButton_Down)) step = -1;
  if (!step) return;

  const float f = (step > 0) ? 1.15f : (1.0f / 1.15f);

  if (s_gyro_on) {
    s_gyro_sens *= f;
    clamp_settings();
    logf_("nxdp: gyro sensitivity = %.2f\n", s_gyro_sens);
  } else {
    s_stick_speed *= f;
    clamp_settings();
    logf_("nxdp: stick speed = %.1f px/frame\n", s_stick_speed);
  }
  settings_touch();
}

/* Read one Joy-Con's gyro for the dual-controller case. side = LEFT/RIGHT.
 * Joy-Con IMU frames are mirrored on both axes vs the Pro Controller. Returns 1
 * if a fresh sample was read. */
static int read_gyro_side(int side, HidSixAxisSensorState *out,
                          float *sign_x, float *sign_y) {
  if (!s_gyro_ready) return 0;
  const u64 style = padGetStyleSet(&s_pad);

  /* Handheld (Joy-Cons attached): a single IMU. Both cursors' gyro share it, so
   * in handheld the gyro nudges both cursors together -- detach the Joy-Cons for
   * independent per-cursor motion. */
  if (style & HidNpadStyleTag_NpadHandheld) {
    *sign_x = +1.0f; *sign_y = +1.0f;
    return hidGetSixAxisSensorStates(s_six_handheld, out, 1) > 0;
  }

  /* Pro Controller: one IMU, correct with both axes negated. Shared by both. */
  if (style & HidNpadStyleTag_NpadFullKey) {
    *sign_x = -1.0f; *sign_y = -1.0f;
    return hidGetSixAxisSensorStates(s_six_fullkey, out, 1) > 0;
  }

  /* Dual Joy-Con: the interesting case -- left cursor <- left Joy-Con IMU,
   * right cursor <- right Joy-Con IMU, each only if that side is connected. */
  if (style & HidNpadStyleTag_NpadJoyDual) {
    *sign_x = +1.0f; *sign_y = +1.0f;
    const u64 attr = padGetAttributes(&s_pad);
    if (side == LEFT) {
      if (attr & HidNpadAttribute_IsLeftConnected)
        return hidGetSixAxisSensorStates(s_six_dual[0], out, 1) > 0;
    } else {
      if (attr & HidNpadAttribute_IsRightConnected)
        return hidGetSixAxisSensorStates(s_six_dual[1], out, 1) > 0;
    }
  }
  return 0;
}

/* Motion pointing for one cursor. yaw -> X, pitch -> Y. */
static void do_gyro_side(int side) {
  if (!s_gyro_on) return;

  HidSixAxisSensorState st = {0};
  float sx = -1.0f, sy = -1.0f;
  if (!read_gyro_side(side, &st, &sx, &sy)) return;

  if (s_gyro_logged < 8) {
    s_gyro_logged++;
    logf_("nxdp: gyro[%s] av=(%.3f,%.3f,%.3f) signs=(%+.0f,%+.0f)\n",
          side == LEFT ? "L" : "R",
          st.angular_velocity.x, st.angular_velocity.y, st.angular_velocity.z, sx, sy);
  }

  const float g = GYRO_GAIN * s_gyro_sens;
  const float dx = sx * st.angular_velocity.y * g;   /* yaw   -> horizontal */
  const float dy = sy * st.angular_velocity.x * g;   /* pitch -> vertical   */

  if (fabsf(dx) > 0.05f || fabsf(dy) > 0.05f) {
    float dgx, dgy; rot_delta(ctrl_rot(), dx, dy, &dgx, &dgy);
    s_cx[side] += dgx;
    s_cy[side] += dgy;
    clamp_cursor(side);
  }
}

/* One cursor's stick + tap, emitting events. stick_idx: 0 = left, 1 = right pad
 * stick. tap_btn: the ZL/ZR button for this cursor. */
static void drive_cursor(int side, int stick_idx, u64 tap_btn, u64 held) {
  /* stick motion */
  HidAnalogStickState s = padGetStickPos(&s_pad, stick_idx);
  if (s.x || s.y) {
    float dgx, dgy;
    rot_delta(ctrl_rot(), (s.x / 32767.0f) * s_stick_speed,
              -(s.y / 32767.0f) * s_stick_speed, &dgx, &dgy);   /* stick +y is up */
    s_cx[side] += dgx; s_cy[side] += dgy;
    clamp_cursor(side);
  }

  /* gyro for this side (no-op if gyro off or side disconnected) */
  do_gyro_side(side);

  /* tap: ZL for left, ZR for right */
  const int tap = (held & tap_btn) ? 1 : 0;
  s_tap_now[side] = tap;
  int phase = 0;
  if      ( tap && !s_tap_prev[side]) phase = NXDP_DOWN;
  else if ( tap &&  s_tap_prev[side]) phase = NXDP_MOVE;
  else if (!tap &&  s_tap_prev[side]) phase = NXDP_UP;
  s_tap_prev[side] = tap;
  if (phase) push(s_id[side], s_cx[side], s_cy[side], phase);
}

/* ----------------------------------------------------------------- update */

void nxdp_update(void) {
  if (!s_ready) return;
  padUpdate(&s_pad);

  const u64 held    = padGetButtons(&s_pad);
  const u64 pressed = padGetButtonsDown(&s_pad);

  /* Until the player says otherwise, follow the dock state: docked has no
   * touchscreen so the cursors are the only pointer, handheld has one so they
   * are just clutter. Dock or undock mid-session and they appear/disappear. */
#if PHI_VIRTUAL_CURSOR
  if (!s_vis_user_set) {
    const int want = nxdp_docked();
    if (want != s_visible) {
      s_visible = want;
      logf_("nxdp: cursors %s (%s)\n", want ? "ON" : "OFF",
            want ? "docked" : "handheld");
    }
  }

  /* '+' toggles both cursors on/off, so they aren't in the way when you're
   * playing on the touchscreen. Hidden means not drawn AND not tapping. */
  if (pressed & HidNpadButton_Plus) {
    s_visible = !s_visible;
    s_vis_user_set = 1;               /* explicit choice wins from here on */
    logf_("nxdp: cursors %s (manual)\n", s_visible ? "ON" : "OFF");
  }
#endif /* PHI_VIRTUAL_CURSOR */

  /* '-' toggles gyro pointing for both cursors. */
  if (pressed & HidNpadButton_Minus) {
    if (!s_gyro_ready) {
      logf_("nxdp: gyro unavailable on this controller\n");
    } else {
      s_gyro_on = !s_gyro_on;
      logf_("nxdp: gyro %s\n", s_gyro_on ? "ON" : "OFF");
    }
  }

  /* L recenters the LEFT cursor, R recenters the RIGHT -- handy for gyro aiming. */
  if (pressed & HidNpadButton_L) {
    s_cx[LEFT]  = s_cfg.screen_w * 0.5f; s_cy[LEFT]  = s_cfg.screen_h * 0.5f;
    logf_("nxdp: left cursor recentered\n");
  }
  if (pressed & HidNpadButton_R) {
    s_cx[RIGHT] = s_cfg.screen_w * 0.5f; s_cy[RIGHT] = s_cfg.screen_h * 0.5f;
    logf_("nxdp: right cursor recentered\n");
  }

  do_dpad(held, pressed);

  s_nev = 0;

  do_touch();                      /* only if handle_touch (host owns touch here) */

  if (s_visible) {
    /* Both cursors live. Left stick + ZL drive blue; right stick + ZR drive red.
     * Gyro (if on) adds to whichever side its Joy-Con maps to. */
    drive_cursor(LEFT,  0, HidNpadButton_ZL, held);
    drive_cursor(RIGHT, 1, HidNpadButton_ZR, held);
  } else {
    /* Hidden: don't move or tap. But if a trigger was down when we hid, emit the
     * Ended frame so the game doesn't see a finger stuck down, then clear tap
     * state so nxdp_tapping_fingers reports nothing. */
    for (int c = 0; c < 2; c++) {
      if (s_tap_prev[c]) push(s_id[c], s_cx[c], s_cy[c], NXDP_UP);
      s_tap_prev[c] = 0;
      s_tap_now[c]  = 0;
    }
  }

  settings_tick();
}

int nxdp_poll(NxdpEvent *out, int max) {
  int n = s_nev < max ? s_nev : max;
  if (n > 0) memcpy(out, s_ev, (size_t)n * sizeof(NxdpEvent));
  return n;
}

void  nxdp_left_pos(float *x, float *y)  { if (x) *x = s_cx[LEFT];  if (y) *y = s_cy[LEFT]; }
void  nxdp_right_pos(float *x, float *y) { if (x) *x = s_cx[RIGHT]; if (y) *y = s_cy[RIGHT]; }
float nxdp_stick_speed(void)             { return s_stick_speed; }
float nxdp_gyro_sens(void)               { return s_gyro_sens; }
int   nxdp_gyro_enabled(void)            { return s_gyro_on; }
int   nxdp_cursor_visible(void)          { return s_ready && s_visible; }

int nxdp_tapping_fingers(NxdpFinger *out, int max) {
  int n = 0;
  for (int c = 0; c < 2 && n < max; c++) {
    if (s_tap_now[c]) {
      out[n].id = s_id[c];
      out[n].x  = s_cx[c];
      out[n].y  = s_cy[c];
      n++;
    }
  }
  return n;
}

/* ======================= GL overlay ====================================== */

static GLuint s_prog = 0;
static GLint  s_u_screen, s_u_origin, s_u_scale, s_u_colour, s_u_tex, s_u_use_tex;
static int    s_gl_failed = 0;

/* Built-in arrow: tip at (0,0), y down, drawn as a fan from the tip. */
static const GLfloat s_arrow[] = {
   0.0f,  0.0f,   0.0f, 16.0f,   4.0f, 12.0f,   7.0f, 18.0f,
  10.0f, 16.5f,   7.0f, 10.5f,  12.0f, 10.0f,
};
#define ARROW_VERTS 7

/* Per-cursor tints. Left = blue, right = red. */
static const GLfloat TINT[2][3] = {
  { 0.20f, 0.45f, 1.00f },   /* LEFT  blue  */
  { 1.00f, 0.25f, 0.25f },   /* RIGHT red   */
};

static GLuint mkshader(GLenum t, const char *src) {
  GLuint s = glCreateShader(t);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) { glDeleteShader(s); return 0; }
  return s;
}

static int gl_init(void) {
  if (s_prog) return 1;
  if (s_gl_failed) return 0;

  static const char *vs =
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "uniform vec2 uScreen;\n"
    "uniform vec2 uOrigin;\n"
    "uniform float uScale;\n"
    "void main() {\n"
    "  vUV = aUV;\n"
    "  vec2 p = uOrigin + aPos * uScale;\n"
    "  gl_Position = vec4((p.x/uScreen.x)*2.0-1.0, 1.0-(p.y/uScreen.y)*2.0, 0.0, 1.0);\n"
    "}\n";
  /* For a textured cursor we MULTIPLY the sampled texel by uColour, so a single
   * greyscale/white cursor.png is tinted blue or red per side while its own alpha
   * and shading are preserved. For the built-in arrow uColour is the flat colour. */
  static const char *fs =
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform vec4 uColour;\n"
    "uniform sampler2D uTex;\n"
    "uniform float uUseTex;\n"
    "void main() {\n"
    "  if (uUseTex > 0.5) gl_FragColor = texture2D(uTex, vUV) * uColour;\n"
    "  else               gl_FragColor = uColour;\n"
    "}\n";

  GLuint v = mkshader(GL_VERTEX_SHADER, vs), f = mkshader(GL_FRAGMENT_SHADER, fs);
  if (!v || !f) { s_gl_failed = 1; logf_("nxdp: cursor shader compile failed\n"); return 0; }

  GLuint p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glBindAttribLocation(p, 0, "aPos");
  glBindAttribLocation(p, 1, "aUV");
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);

  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) { glDeleteProgram(p); s_gl_failed = 1; logf_("nxdp: cursor link failed\n"); return 0; }

  s_prog      = p;
  s_u_screen  = glGetUniformLocation(p, "uScreen");
  s_u_origin  = glGetUniformLocation(p, "uOrigin");
  s_u_scale   = glGetUniformLocation(p, "uScale");
  s_u_colour  = glGetUniformLocation(p, "uColour");
  s_u_tex     = glGetUniformLocation(p, "uTex");
  s_u_use_tex = glGetUniformLocation(p, "uUseTex");
  return 1;
}

/* A vertex attribute's FULL state -- see nx_pointer notes: saving only the
 * enabled flag corrupts the engine's subsequent draws, because
 * glVertexAttribPointer also records size/type/stride/pointer and the bound
 * ARRAY_BUFFER. Save and restore all of it. */
typedef struct {
  GLint enabled, size, type, norm, stride, buf;
  void *ptr;
} AttribState;

static void attrib_save(GLuint i, AttribState *a) {
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a->enabled);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a->size);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a->type);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a->norm);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a->stride);
  glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a->buf);
  glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a->ptr);
}

static void attrib_restore(GLuint i, const AttribState *a) {
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)a->buf);
  if (a->size > 0)
    glVertexAttribPointer(i, a->size, (GLenum)a->type,
                          (GLboolean)(a->norm ? GL_TRUE : GL_FALSE),
                          a->stride, a->ptr);
  if (a->enabled) glEnableVertexAttribArray(i);
  else            glDisableVertexAttribArray(i);
}

/* Draw one cursor at (cx,cy) with tint rgb. Assumes program/viewport/blend are
 * already set up by nxdp_draw; only touches per-cursor uniforms + the draw. */
static void draw_one(int use_tex, float cx, float cy, const GLfloat rgb[3]) {
  glUniform2f(s_u_origin, cx, cy);

  if (use_tex) {
    const GLfloat w = (GLfloat)s_cursor_w, h = (GLfloat)s_cursor_h;
    const GLfloat quad[] = { 0,0,  w,0,  0,h,  w,h };
    const GLfloat uv[]   = { 0,0,  1,0,  0,1,  1,1 };
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glUniform1i(s_u_tex, 0);
    glUniform1f(s_u_use_tex, 1.0f);
    glUniform1f(s_u_scale, (GLfloat)s_cfg.screen_h / 1280.0f);
    glUniform4f(s_u_colour, rgb[0], rgb[1], rgb[2], 1.0f);   /* tint multiply */
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, s_arrow);
    glUniform1f(s_u_use_tex, 0.0f);

    const GLfloat sc = 2.4f * ((GLfloat)s_cfg.screen_h / 1280.0f);
    glUniform1f(s_u_scale, sc * 1.22f);
    glUniform4f(s_u_colour, 0.0f, 0.0f, 0.0f, 0.85f);        /* dark outline */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);

    glUniform1f(s_u_scale, sc);
    glUniform4f(s_u_colour, rgb[0], rgb[1], rgb[2], 1.0f);   /* tinted fill */
    glDrawArrays(GL_TRIANGLE_FAN, 0, ARROW_VERTS);
  }
}

void nxdp_draw(void) {
  if (!s_ready) return;
  if (!s_visible) return;      /* toggled off with '+' */
  if (!gl_init()) return;

  /* decode + upload cursor.png on first draw (needs a live GL context) */
  if (!s_png_tried) {
    s_png_tried = 1;
    if (s_png_bytes) {
      if (!cursor_upload_png()) { s_cursor_tex = 0; }
      free(s_png_bytes); s_png_bytes = NULL; s_png_len = 0;
    }
  }

  /* ---- save every bit of state we touch ---- */
  AttribState a0, a1;
  attrib_save(0, &a0);
  attrib_save(1, &a1);

  GLint prev_prog = 0, prev_buf = 0;
  GLint bs_rgb = 0, bd_rgb = 0, bs_a = 0, bd_a = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_buf);
  glGetIntegerv(GL_BLEND_SRC_RGB, &bs_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &bd_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &bs_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &bd_a);
  const GLboolean was_blend   = glIsEnabled(GL_BLEND);
  const GLboolean was_depth   = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean was_cull    = glIsEnabled(GL_CULL_FACE);
  const GLboolean was_scissor = glIsEnabled(GL_SCISSOR_TEST);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(s_prog);

  /* Draw in the full framebuffer (== render space == tap space), then restore
   * the game's viewport -- see nx_pointer notes on the swap-time sub-viewport. */
  GLint prev_vp[4] = {0,0,0,0};
  glGetIntegerv(GL_VIEWPORT, prev_vp);
  glViewport(0, 0, s_cfg.screen_w, s_cfg.screen_h);
  glUniform2f(s_u_screen, (GLfloat)s_cfg.screen_w, (GLfloat)s_cfg.screen_h);

  /* Texture units: for the PNG path, read+switch to unit 0, restore afterwards. */
  GLint prev_active = 0, tex0 = 0;
  const int use_tex = (s_cursor_tex != 0);
  if (use_tex) {
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex0);
    glBindTexture(GL_TEXTURE_2D, s_cursor_tex);
  }

  /* Both cursors, always. Left (blue) then right (red). */
  draw_one(use_tex, s_cx[LEFT],  s_cy[LEFT],  TINT[LEFT]);
  draw_one(use_tex, s_cx[RIGHT], s_cy[RIGHT], TINT[RIGHT]);

  if (use_tex) {
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex0);
    glActiveTexture((GLenum)prev_active);
  }

  /* ---- restore ---- */
  glViewport(prev_vp[0], prev_vp[1], prev_vp[2], prev_vp[3]);
  attrib_restore(0, &a0);
  attrib_restore(1, &a1);
  glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prev_buf);
  glUseProgram((GLuint)prev_prog);
  glBlendFuncSeparate((GLenum)bs_rgb, (GLenum)bd_rgb, (GLenum)bs_a, (GLenum)bd_a);
  if (!was_blend) glDisable(GL_BLEND); else glEnable(GL_BLEND);
  if (was_depth)   glEnable(GL_DEPTH_TEST);
  if (was_cull)    glEnable(GL_CULL_FACE);
  if (was_scissor) glEnable(GL_SCISSOR_TEST);
}

#if NXDP_ENABLE_MOUSE
/* --- Retained USB-mouse driver from the single-cursor module -----------------
 * Compiled out for Color Sheep (touch + controller only). If re-enabled, route
 * its delta/tap into one cursor (say RIGHT) inside nxdp_update, and add mouse
 * presence back into the gyro gate. Kept verbatim for provenance; see the
 * original nx_pointer.c for the full commentary on why it reads the whole HID
 * sample buffer rather than the newest state. */
#endif
