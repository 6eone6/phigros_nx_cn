/* jni_fake.c -- fake JNI environment for the MVGL engine (libcrx.so)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <switch.h>

#include "config.h"
#include "util.h"
#include "jni_fake.h"
#include "phigros_jni.h"   /* NativeAudio + TapTap fakes (Phigros-specific) */
#include "data.h"
#include "text2bitmap.h"
#include "movie_player.h"
#include "editbox.h"
#include "android_native_unity.h"
#include "jni_unimpl.h"
#include "libc_shim.h"   /* managed_path: device-less paths for managed code */
#include "opensles.h"    /* audio_fmod_open/write: FMOD native-audio output sink */

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

void fmod_audio_start(void); // defined below; launched from FMODAudioDevice.start()

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'  heap object (freeable)
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'  pooled, never freed
  TAG_CLASS  = 0x434c5331, // 'CLS1'  pooled, never freed
  // text2bitmap.h BITMAP_TAG ('BMP1') is also handled by free_ref
};

typedef struct { uint32_t tag; char label[64]; } FakeObject;
typedef struct { uint32_t tag; char *utf; uint32_t pooled; } FakeString;
typedef struct { uint32_t tag; int len; void **items; } FakeObjArray;
typedef struct { uint32_t tag; int len; int elem_size; void *data; } FakePriArray;
typedef struct { uint32_t tag; char cls[96]; char name[64]; char sig[160]; } FakeID;
typedef struct { uint32_t tag; char name[96]; } FakeClass;

volatile int jni_quit_requested = 0;

/* Read a fake-object tag without trusting the pointer.
 * Returns 0 for anything that cannot be one of ours: NULL, misaligned
 * (every Fake* is at least 8-byte aligned), or a low address. 0 matches no
 * TAG_*, so callers keep their existing comparisons and simply take the
 * "not mine" branch instead of faulting. */
/* Every tag this loader creates. BITMAP_TAG ('BMP1', text2bitmap.h) is
 * easy to forget -- it is declared in another header, and omitting it would
 * both break GetObjectClass for bitmaps AND make every text-render ref warn. */
/* EVERY tag this loader creates, across ALL files -- not just this one.
 * Getting this list wrong is silent and destructive: nx_tag_of() returns 0
 * for an unlisted tag, so every comparison on that object takes its "not
 * ours" branch and the owning subsystem quietly stops working.
 *
 * Two were missed on the first pass because they live in other files:
 *   UJ_TAG 0x554a4831 ('UJH1') unity_jni.c:41   -- Unity JNI handles
 *   UI_TAG 0x55494531 ('UIE1') unity_input.c:11 -- input events
 * and BITMAP_TAG lives in text2bitmap.h. They are spelled by value here
 * rather than by symbol because those headers are not all in scope; if one
 * ever changes, the tag-warn line will name it in ASCII. */
static int nx_tag_known(uint32_t t) {
  return t == TAG_OBJECT || t == TAG_STRING || t == TAG_OBJARR ||
         t == TAG_PRIARR || t == TAG_ID     || t == TAG_CLASS  ||
         t == BITMAP_TAG ||
         t == 0x554a4831u /* UJ_TAG 'UJH1' unity_jni.c   */ ||
         t == 0x55494531u /* UI_TAG 'UIE1' unity_input.c */;
}

/* Warn about a reference we could not identify.
 * Guarding makes a bad pointer SURVIVABLE, not CORRECT: after nx_tag_of()
 * returns 0 the caller quietly takes its "not one of ours" branch, so a ref
 * we genuinely should have recognised now produces wrong behaviour instead
 * of a crash -- which is harder to diagnose, not easier. This makes it
 * visible. Rate-limited hard: these run on every DeleteLocalRef. */
static void nx_tag_warn(const void *p, uint32_t t, const char *why) {
  static uint32_t seen[16]; static int nseen = 0; static int nmisaligned = 0;
  if (!t) {                       /* unusable pointer, no tag to key on */
    if (nmisaligned < 8) { nmisaligned++;
      debugPrintf("[jni] tag-warn: %s ptr=%p (ref not created by us)\n", why, p); }
    return;
  }
  for (int i = 0; i < nseen; i++) if (seen[i] == t) return;
  if (nseen < 16) {
    seen[nseen++] = t;
    char a[5] = { (char)(t >> 24), (char)(t >> 16), (char)(t >> 8), (char)t, 0 };
    for (int i = 0; i < 4; i++) if (a[i] < 32 || a[i] > 126) a[i] = 46;
    /* Say WHERE it lives. Working that out by hand off the memory map is what
     * the r145 diagnosis actually turned on: 0xfffe241c0 was inside the newlib
     * heap region, which is what separated "stale or foreign jobject" from
     * "wild garbage" and pointed the investigation at ref lifetime. */
    unsigned koff = 0, run = 0; int res = 0;
    extern int nx_arena_describe(const void *, unsigned *, int *, unsigned *);
    const char *where = nx_arena_describe(p, &koff, &res, &run)
                          ? "our mmap arena" : "NOT our arena (newlib heap / foreign)";
    debugPrintf("[jni] tag-warn: %s ptr=%p tag=0x%08x ('%s') -- %s\n",
                why, p, t, a, where);
  }
}

static inline uint32_t nx_tag_of(const void *p) {
  uintptr_t v = (uintptr_t)p;
  if (!p) return 0;                 /* NULL is normal in JNI -- silent */
  /* 4, not 8: FakeID (324 bytes) and FakeClass (100) are pooled in arrays
   * whose stride is not a multiple of 8, so valid refs are only 4-aligned. */
  if ((v & 3u) || v < 0x1000u) { nx_tag_warn(p, 0, "unusable pointer"); return 0; }
  uint32_t t = *(volatile const uint32_t *)p;
  if (!nx_tag_known(t)) { nx_tag_warn(p, t, "unrecognised tag"); return 0; }
  return t;
}

// ---------------------------------------------------------------------------
// local reference registry (matches the engine's Push/PopLocalFrame brackets)
// ---------------------------------------------------------------------------

#define MAX_LOCALS 1048576
#define MAX_FRAMES 64
static void *locals[MAX_LOCALS];
static int locals_top = 0;
static int frames[MAX_FRAMES];
static int frame_top = 0;
static Mutex locals_lock;

static void *reg_local(void *ref) {
  if (ref) {
    mutexLock(&locals_lock);
    if (locals_top < MAX_LOCALS)
      locals[locals_top++] = ref;
    else {
      /* Dropping the ref leaks it rather than dangling it -- the safe
       * direction -- but a full table means DeleteLocalRef can no longer find
       * anything, so say so. */
      static int warned = 0;
      if (!warned) { warned = 1;
        debugPrintf("[jni] *** local-ref table full (%d): refs are no longer "
                    "tracked and will leak ***\n", MAX_LOCALS); }
    }
    mutexUnlock(&locals_lock);
  }
  return ref;
}

// interned-string pool: the engine re-creates the same constant strings (class
// names, the activity name) constantly; pool them by content so repeats don't
// fill the local-ref table. Pooled strings are never reg_local'd, and free_ref
// skips them (range check below).
/* 2048/512, matching the sibling ACPC port. Ours were 512/128. The string pool
 * degrades safely when full (a one-off malloc'd local below), but the OBJECT
 * pool did not: it aliased every further label to iobj_pool[0], so past 128
 * distinct classes every opaque object became the same object -- IsSameObject,
 * identity comparisons and per-object state all collapse, silently. */
/* GROWABLE, and therefore never overflows into the fragile path.
 *
 * This used to be a fixed array of 2048 FakeString STRUCTS. Past 2048 distinct
 * strings, jni_make_string fell back to a one-off reg_local() string, and the
 * comment called that "correct, just more churn". It is not correct: a local
 * reference dies when its frame is popped, and a pooled string never does. The
 * boot log shows the consequence exactly one screen apart --
 *     [jni] intern-string pool full (2048) ...        (line 1089)
 *     [prefs] putString SKIP empty key                (line 1198)
 * -- and from then on EVERY PlayerPrefs write was dropped, because the key
 * jstring had been reclaimed before we read it. The game could not save at all.
 * (The same lost-argument shape is described in the getProperty comment further
 * down, which worked around it rather than fixing it.)
 *
 * The array could not simply be made bigger by realloc, because callers hold
 * pointers INTO it. So it is now an array of POINTERS to individually
 * allocated strings: growing the index never moves a string. Pooled strings
 * are immortal, which is what makes them safe to hand out.
 *
 * Storage is bounded in practice by the number of DISTINCT strings the game
 * uses -- they are deduplicated on insert -- so this is not an unbounded leak.
 * The lookup is a hash bucket rather than the old linear strcmp scan, which at
 * 2048 entries was already O(n) on every single string the engine created. */
#define ISTR_BUCKETS 4096
typedef struct IStrNode { struct IStrNode *next; FakeString *s; uint32_t h; } IStrNode;
static IStrNode *istr_buckets[ISTR_BUCKETS];
static int istr_count = 0;

static uint32_t istr_hash(const char *u) {
  uint32_t h = 2166136261u;                 /* FNV-1a */
  while (*u) { h ^= (unsigned char)*u++; h *= 16777619u; }
  return h;
}

static void free_ref(void *ref) {
  if (!ref)
    return;
  /* Pooled strings are marked, not address-ranged: they are individually
   * allocated now, so there is no contiguous range to test. */
  if (nx_tag_of(ref) == TAG_STRING && ((const FakeString *)ref)->pooled)
    return;  // interned string -- pooled, never freed
  switch (nx_tag_of(ref)) {
    case TAG_STRING: { FakeString *s = ref; free(s->utf); free(s); break; }
    case TAG_PRIARR: { FakePriArray *a = ref; free(a->data); free(a); break; }
    case TAG_OBJARR: { FakeObjArray *a = ref; free(a->items); free(a); break; }
    case TAG_OBJECT: free(ref); break;
    case BITMAP_TAG: text2bitmap_free((FakeBitmap *)ref); break;
    default: break; // TAG_ID / TAG_CLASS are pooled
  }
}

static void delete_local(void *ref) {
  if (!ref)
    return;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--) {
    if (locals[i] == ref) {
      locals[i] = locals[--locals_top];
      free_ref(ref);
      break;
    }
  }
  mutexUnlock(&locals_lock);
}

// ---------------------------------------------------------------------------
// object constructors
// ---------------------------------------------------------------------------

// Intern objects by label -- one pooled object per class (TAG_CLASS so free_ref()
// leaves it alone, never reg_local'd) -- so the engine's frequent NewObject calls
// don't fill the local-ref table. Safe: our objects are opaque, stateless handles
// dispatched by method class, not by identity.
#define MAX_IOBJ 512
static FakeObject iobj_pool[MAX_IOBJ];
static int iobj_count = 0;
void *jni_make_object(const char *label) {
  const char *l = (label && label[0]) ? label : "obj";
  mutexLock(&locals_lock);
  void *r = NULL;
  for (int i = 0; i < iobj_count; i++)
    if (!strcmp(iobj_pool[i].label, l)) { r = &iobj_pool[i]; break; }
  if (!r) {
    if (iobj_count >= MAX_IOBJ) {
      /* Aliasing is the least-bad option left, but it must never be silent:
       * from here on distinct classes ARE the same object. */
      static int warned = 0;
      if (!warned) { warned = 1;
        debugPrintf("[jni] *** opaque-object pool exhausted at %d labels -- '%s' "
                    "and every later class now ALIAS to one object. Identity "
                    "comparisons are wrong from here. Raise MAX_IOBJ. ***\n",
                    MAX_IOBJ, l); }
      r = &iobj_pool[0];
    }
    else {
      FakeObject *o = &iobj_pool[iobj_count++];
      o->tag = TAG_CLASS;             // pooled: free_ref() ignores TAG_CLASS
      strncpy(o->label, l, sizeof(o->label) - 1);
      r = o;
    }
  }
  mutexUnlock(&locals_lock);
  return r;
}

void *jni_make_string(const char *utf) {
  const char *u = utf ? utf : "";
  const uint32_t h = istr_hash(u);
  IStrNode **head = &istr_buckets[h & (ISTR_BUCKETS - 1)];

  mutexLock(&locals_lock);
  for (IStrNode *n = *head; n; n = n->next)        // repeats reuse the pooled string
    if (n->h == h && !strcmp(n->s->utf, u)) {
      void *r = n->s; mutexUnlock(&locals_lock); return r;
    }

  FakeString *s = calloc(1, sizeof(*s));
  char *dup = strdup(u);
  IStrNode *n = calloc(1, sizeof(*n));
  if (!s || !dup || !n) {                          /* out of memory */
    free(s); free(dup); free(n);
    mutexUnlock(&locals_lock);
    return NULL;
  }
  s->tag = TAG_STRING;
  s->utf = dup;
  s->pooled = 1;                                   /* immortal; free_ref skips it */
  n->s = s; n->h = h; n->next = *head; *head = n;
  istr_count++;
  { static int next_report = 4096;                 /* growth is worth noticing */
    if (istr_count >= next_report) {
      next_report *= 2;
      debugPrintf("[jni] interned strings: %d distinct (pool grows; no string is "
                  "ever a fragile local)\n", istr_count);
    } }
  mutexUnlock(&locals_lock);
  return s;                                        // pooled, not reg_local'd
}

static void *make_pri_array_adopt(void *data, int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = data;
  return reg_local(a);
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && nx_tag_of(s) == TAG_STRING)
    return s->utf;
  return "";
}

// UTF-16 code-unit count of a modified-UTF-8 string (Java String.length()).
// ASCII -> byte count; astral planes count as a surrogate pair. Used by both
// GetStringLength and the String.length() upcall handler.
static juint utf16_len(const char *str) {
  const unsigned char *p = (const unsigned char *)(str ? str : "");
  juint n = 0;
  while (*p) {
    const unsigned char c = *p;
    juint adv; uint32_t cp;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) {
      if (!p[k]) { adv = k; break; }
      cp = (cp << 6) | (p[k] & 0x3F);
    }
    n += (cp >= 0x10000) ? 2u : 1u;
    p += adv;
  }
  return n;
}

// register a text2bitmap result in the local table so the engine's recycle /
// DeleteLocalRef frees it
static void *reg_bitmap(FakeBitmap *b) { return reg_local(b); }

// ---------------------------------------------------------------------------
// interned classes + singletons
// ---------------------------------------------------------------------------

#define MAX_CLASSES 128
static FakeClass class_pool[MAX_CLASSES];
static int class_count = 0;

static void *intern_class(const char *name) {
  for (int i = 0; i < class_count; i++)
    if (!strcmp(class_pool[i].name, name))
      return &class_pool[i];
  if (class_count >= MAX_CLASSES) {
    debugPrintf("JNI: *** class pool exhausted at '%s' -> collapsing to '%s' "
                "(distinct classes break instanceof!)\n", name, class_pool[0].name);
    return &class_pool[0];
  }
  FakeClass *c = &class_pool[class_count++];
  c->tag = TAG_CLASS;
  strncpy(c->name, name, sizeof(c->name) - 1);
  debugPrintf("JNI class: %s\n", c->name);
  return c;
}

/* Validating counterpart to class_name_of(). Same reasoning as safe_utf():
 * a fallback handler runs for calls nobody modelled, so its varargs are
 * untrusted. class_name_of() loads c->tag unconditionally, and doing that
 * to a stale stack slot is what produced
 *   esr=92000004 far=6374696f... with x1=434c5331 (TAG_CLASS) in the dump.
 * Check alignment and range before touching memory. */
/* Read a FakeString's text, tolerating a garbage or NULL argument.
 * JNI varargs are untrusted: if the signature says fewer args than we read,
 * or the caller passes a primitive where we expected an object, the value
 * is a stale stack slot. Probing its tag through a raw cast is what caused
 * the round-15/16 data abort, so validate before touching memory. */
static const char *safe_utf(void *p) {
  if (nx_tag_of(p) != TAG_STRING) return "";
  const char *u = ((FakeString *)p)->utf;
  return u ? u : "";
}

static const char *safe_class_name(void *p) {
  uintptr_t v = (uintptr_t)p;
  if (!p || (v & 3u) || v < 0x1000u) return "";   /* 4-aligned: see nx_tag_of */
  if (*(volatile uint32_t *)p != TAG_CLASS) return "";
  return ((FakeClass *)p)->name;
}

static const char *class_name_of(void *cls) {
  FakeClass *c = cls;
  return (c && nx_tag_of(c) == TAG_CLASS) ? c->name : "";
}

static FakeObject *g_activity_obj = NULL;   // the MyNativeActivity instance
static FakeObject *g_asset_mgr = NULL;      // android.content.res.AssetManager

void *jni_make_activity_object(void) {
  if (!g_activity_obj) {
    g_activity_obj = calloc(1, sizeof(*g_activity_obj));
    g_activity_obj->tag = TAG_CLASS; // pooled (never freed)
    strcpy(g_activity_obj->label, "MyNativeActivity");
  }
  return g_activity_obj;
}

static void *get_asset_manager_obj(void) {
  if (!g_asset_mgr) {
    g_asset_mgr = calloc(1, sizeof(*g_asset_mgr));
    g_asset_mgr->tag = TAG_CLASS;
    strcpy(g_asset_mgr->label, "AssetManager");
  }
  return g_asset_mgr;
}

// The engine fetches the ClassLoader every frame; hand back a cached singleton
// (pooled, never reg_local'd) so it doesn't fill the local-ref table.
static FakeObject *g_classloader = NULL;
static void *get_classloader_obj(void) {
  if (!g_classloader) {
    g_classloader = calloc(1, sizeof(*g_classloader));
    g_classloader->tag = TAG_CLASS;
    strcpy(g_classloader->label, "ClassLoader");
  }
  return g_classloader;
}

// ---------------------------------------------------------------------------
// method/field ID pool (class-aware)
// ---------------------------------------------------------------------------

#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

/* ---- field writes (round 132) --------------------------------------------
 * Set*Field was entirely unimplemented. Slot 109 (SetIntField) is the ONLY
 * unimplemented slot the r131 run touched -- once -- and the write vanished
 * silently, which is the failure mode this whole port keeps paying for.
 *
 * Fields here are keyed by FieldID ALONE: field_int()/field_object() never look
 * at the object. That is a real simplification (two instances share one value),
 * but it is the existing model, so writes follow it rather than inventing a
 * second one. Reads consult the override first, so Set-then-Get returns what
 * was written instead of the synthesised default. IDs live in id_pool, so the
 * index is just the offset -- no lookup, and no way to key on a pointer that
 * was never ours. */
#define FLD_NONE 0
#define FLD_WORD 1     /* int / long / bool / byte / char / short */
#define FLD_OBJ  2
#define FLD_FLT  3
static uint64_t g_field_w[MAX_IDS];
static uint8_t  g_field_kind[MAX_IDS];

static int field_slot(const void *fid) {
  const FakeID *f = (const FakeID *)fid;
  if (!f || f < id_pool || f >= id_pool + MAX_IDS) return -1;
  if (nx_tag_of(fid) != TAG_ID) return -1;
  return (int)(f - id_pool);
}

/* A C string we are about to strcmp/strncpy. Same reasoning as nx_tag_of():
 * JNI hands us pointers we did not create. A NULL is already handled by the
 * callers; what is not is a pointer that holds string DATA instead of an
 * address, which is what the register dump showed. */
static const char *safe_cstr(const char *p, const char *what) {
  uintptr_t v = (uintptr_t)p;
  if (!p) return "";
  if (v < 0x1000u || v > 0x0000ffffffffffffull) {
    static int n = 0;
    if (n < 8) { n++;
      debugPrintf("[jni] bad %s pointer %p in get_id() -- not an address\n",
                  what, (void *)p); }
    return "";
  }
  return p;
}

static FakeID *get_id(const char *cls_in, const char *name_in, const char *sig_in) {
  const char *cls  = safe_cstr(cls_in,  "cls");
  const char *name = safe_cstr(name_in, "name");
  const char *sig  = safe_cstr(sig_in,  "sig");
  for (int i = 0; i < id_count; i++)
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted (%s.%s)\n", cls, name);
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  strncpy(id->cls, cls, sizeof(id->cls) - 1);
  strncpy(id->name, name, sizeof(id->name) - 1);
  strncpy(id->sig, sig, sizeof(id->sig) - 1);
  id->cls[sizeof(id->cls) - 1] = 0;
  id->name[sizeof(id->name) - 1] = 0;
  id->sig[sizeof(id->sig) - 1] = 0;   /* strncpy does NOT terminate on truncation */
  debugPrintf("JNI id: %s.%s %s\n", id->cls, id->name, id->sig);
  return id;
}

// ---------------------------------------------------------------------------
// dispatch helpers
// ---------------------------------------------------------------------------

static int sig_returns(const char *sig, const char *ret) {
  const char *rp = strchr(sig, ')');
  return rp && strstr(rp + 1, ret) == rp + 1;
}

static int name_has(const char *name, const char *sub) { return strstr(name, sub) != NULL; }

// --- Text2Bitmap ------------------------------------------------------------
// draw methods return a Bitmap; the first arg is the text String, the next int
// is the pixel size. measure methods return I (width or height by name).

static void *t2b_object(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  FakeBitmap *b = text2bitmap_render(text, size);
  (void)id;
  return b ? reg_bitmap(b) : NULL;
}

static juint t2b_int(const FakeID *id, va_list va) {
  const char *text = obj_str(va_arg(va, void *));
  const int size = va_arg(va, int);
  if (name_has(id->name, "Height"))
    return (juint)text2bitmap_measure_height(text, size);
  if (name_has(id->name, "Width"))
    return (juint)text2bitmap_measure_width(text, size);
  return (juint)text2bitmap_measure_width(text, size);
}

// --- MoviePlayer ------------------------------------------------------------

static const char *first_string_arg(const char *sig, va_list va); // defined below

static void mov_void(const FakeID *id, va_list va) {
  if (!strcmp(id->name, "SetMovieDB")) { movie_set_db(first_string_arg(id->sig, va)); return; }
  if (name_has(id->name, "Stop") || name_has(id->name, "stop")) { movie_stop(); return; }
  if (name_has(id->name, "Pause")) { movie_pause(); return; }
  if (name_has(id->name, "Resume")) { movie_resume(); return; }
  if (name_has(id->name, "Play") || name_has(id->name, "play") ||
      name_has(id->name, "Start")) {
    movie_play(first_string_arg(id->sig, va), 0); // the String arg is the movie name
    return;
  }
}

static juint mov_int(const FakeID *id, va_list va) {
  (void)va;
  if (name_has(id->name, "Playing") || name_has(id->name, "playing"))
    return (juint)movie_is_playing();
  return 0;
}

// --- MyNativeActivity / general activity ------------------------------------

// the in-archive base name the engine appends ".android.mvgl" to. "10007" is
// the APK versionCode, matching the shipped main.10007.android.mvgl.
#define MAIN_OBB_BASE "main.10007"

static const char *lang_code(void) {
  // Every locale answer in this file funnels through here -- getLanguage,
  // Locale.getCountry/getISO3*/toString/getDisplayName -- so one decision made
  // here is consistent everywhere. A mixed answer (English language, CN
  // country) is exactly the sort of thing that trips a region check.
  //
  // PHIGROS: Chinese locales are FORCED TO ENGLISH.
  //
  // Phigros gates its Chinese builds behind a mandatory TapTap account login.
  // That login needs a real Java TapTap SDK and a network session, neither of
  // which exists here, so a Chinese system language leaves the game sitting on
  // a sign-in screen it can never get past -- unplayable, with no in-game way
  // out, because the language menu is behind the login. English has no such
  // gate. So a Chinese Switch reports "en"/"US" and the game boots straight
  // into English.
  //
  // This is deliberately NOT "follow the system language" like the upstream
  // tree: following it faithfully produces a game you cannot start. Set
  // PHI_FORCE_ENGLISH_FOR_ZH to 0 in config.h to report the real locale --
  // useful if the login is ever solved, and honest about the trade-off rather
  // than silently overriding the player's system setting forever.
  static int zh = -1;
  if (zh < 0) {
    zh = 0;
    u64 code; SetLanguage sl;
    if (R_SUCCEEDED(setInitialize())) {
      if (R_SUCCEEDED(setGetSystemLanguage(&code)) && R_SUCCEEDED(setMakeLanguage(code, &sl)))
        zh = (sl == SetLanguage_ZHCN || sl == SetLanguage_ZHTW ||
              sl == SetLanguage_ZHHANS || sl == SetLanguage_ZHHANT);
      setExit();
    }
#if PHI_FORCE_ENGLISH_FOR_ZH
    if (zh) {
      zh = 0;
      debugPrintf("[lang] system language is Chinese -> reporting en/US.\n"
                  "[lang] The Chinese build requires a TapTap login that cannot\n"
                  "[lang] work without a JVM and a network session, and the\n"
                  "[lang] language menu sits behind it. English has no gate.\n"
                  "[lang] Set PHI_FORCE_ENGLISH_FOR_ZH 0 to report the real locale.\n");
    }
#endif
  }
  return zh ? "zh" : "en";
}

// Walk a JNI arg list per the signature and return the first non-empty String
// argument's text (used to seed the keyboard from ShowEditBox's initial text).
/* Count the declared arguments, and flag float/double among them. Used by the
 * "A" (jvalue[]) path to forward the right number of real varargs -- see the
 * JVA_* macros. `[` is a marker, not an argument; the element type after it is
 * the argument. Round 132. */
static int sig_arg_count(const char *sig, int *has_fp) {
  int n = 0;
  if (has_fp) *has_fp = 0;
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return 0;
  for (p++; *p && *p != ')'; p++) {
    if (*p == '[') continue;                      /* array marker */
    if (*p == 'L') { while (*p && *p != ';') p++; n++; continue; }
    if (*p == 'F' || *p == 'D') { if (has_fp) *has_fp = 1; }
    n++;
  }
  return n;
}

static const char *first_string_arg(const char *sig, va_list va) {
  const char *p = sig ? strchr(sig, '(') : NULL;
  if (!p) return "";
  for (p++; *p && *p != ')'; p++) {
    switch (*p) {
      case 'I': case 'Z': case 'B': case 'C': case 'S': (void)va_arg(va, int); break;
      case 'F': case 'D': (void)va_arg(va, double); break;
      case 'J': (void)va_arg(va, long long); break;
      case '[':
        (void)va_arg(va, void *);
        if (p[1] == 'L') { p++; while (*p && *p != ';') p++; } else if (p[1]) p++;
        break;
      case 'L': {
        const char *s = obj_str(va_arg(va, void *));
        while (*p && *p != ';') p++;
        if (s && s[0]) return s;
        break;
      }
      default: break;
    }
  }
  return "";
}

// EditBox / TextBox names the engine drives via JNI (both share our swkbd box)
static int is_editbox_show(const char *n)  { return name_has(n, "ShowEditBox")  || name_has(n, "OpenEditBox")  || name_has(n, "ShowTextBox") || name_has(n, "OpenTextBox")
                                                  || name_has(n, "setKeyboardVisible") || name_has(n, "SetKeyboardVisible")
                                                  || name_has(n, "showSoftInput")      || name_has(n, "ShowSoftInput")
                                                  || name_has(n, "openKeyboard")       || name_has(n, "OpenKeyboard"); }
static int is_editbox_open(const char *n)  { return name_has(n, "IsOpenEditBox") || name_has(n, "IsOpenTextBox"); }
static int is_editbox_text(const char *n)  { return name_has(n, "GetEditBoxText") || name_has(n, "GetTextBoxText")
                                                  || name_has(n, "getKeyboardText") || name_has(n, "GetKeyboardText")
                                                  || name_has(n, "getText"); }
static int is_editbox_close(const char *n) { return name_has(n, "CloseEditBox") || name_has(n, "CloseTextBox")
                                                  || name_has(n, "hideSoftInput") || name_has(n, "HideSoftInput")
                                                  || name_has(n, "closeKeyboard") || name_has(n, "CloseKeyboard"); }
/* Anything keyboard-shaped that we did NOT match: log once so the exact JNI name
 * this game uses is visible and can be added above. */
static void kbd_sniff(const char *cls, const char *n) {
  if (!name_has(n, "eyboard") && !name_has(n, "oftInput") && !name_has(n, "extBox")
      && !name_has(n, "ditBox") && !name_has(cls, "eyboard")) return;
  static unsigned seen; if (seen < 12) { seen++;
    debugPrintf("[kbd] unmatched: %s.%s\n", cls ? cls : "?", n ? n : "?"); }
}

/* jni_string_utf is defined far below (shared with unity_jni.c) and unity_jni.h
 * is included after this point, so forward-declare it for act_object's
 * getProperty()/locale arg reads. */
const char *jni_string_utf(void *jstr);

/* AudioManager.getProperty(PROPERTY_OUTPUT_*): the engine/FMOD read the native
 * sample rate / frames-per-buffer to size the audio path. Empty -> parse failure
 * -> a 0 config, which makes FMOD's OpenSL output init fail with "Error
 * initializing output device" (60) on the framesPerBuffer==0 guard. Hand back
 * Switch-sane values (48 kHz, 64 frames).
 *
 * The String key argument does NOT reliably reach us: getProperty is invoked
 * through a JNI call path whose positional argument is lost (observed: key
 * resolves to ""), so keying purely off the argument returned "" and FMOD parsed
 * framesPerBuffer 0 -> error 60. The game ALWAYS reads the PROPERTY_OUTPUT_*
 * static field immediately before the matching getProperty() call, so field_object
 * records which one in g_last_output_prop and we fall back to it when the key is
 * missing/unrecognised. 1 = sample rate, 2 = frames-per-buffer. */
static int g_last_output_prop = 0;

#define NX_STR2(x) #x
#define NX_STR(x) NX_STR2(x)

static void *getproperty_value(const char *key) {
  /* Locale properties. Java code that asks System.getProperty("user.language")
   * instead of going through Locale must get the SAME answer lang_code() gives,
   * or the game sees English text with a CN region and can still take a
   * region-gated path. One source of truth, every route. */
  if (key && (strstr(key, "user.language") || strstr(key, "user.locale")))
    return jni_make_string(lang_code());
  if (key && (strstr(key, "user.country") || strstr(key, "user.region")))
    return jni_make_string(!strcmp(lang_code(), "zh") ? "CN" : "US");

  /* Audio properties: AudioManager's PROPERTY_OUTPUT_SAMPLE_RATE and
   * PROPERTY_OUTPUT_FRAMES_PER_BUFFER. Unity configures FMOD's software MIXER
   * from these, so they decide how hard FMOD has to work.
   *
   * These come from PHI_FMOD_MIX_* and NOT from PHI_AUDIO_SAMPLE_RATE. The 24000
   * here is not a mismatch with the 48000 device rate -- it is a deliberate
   * downclock, and unifying the two reintroduced stuttering audio at once
   * (5.3 ms deadline and twice the samples instead of 10.7 ms). The shim
   * resamples on the way out; that is far cheaper than making FMOD mix at
   * double rate on three shared cores. See the note in config.h before
   * changing either. */
  int which = 0;
  if (key && strstr(key, "SAMPLE_RATE"))            which = 1;
  else if (key && strstr(key, "FRAMES_PER_BUFFER")) which = 2;
  else                                              which = g_last_output_prop;
  static int logged[3] = {0, 0, 0};
  if (which >= 0 && which <= 2 && !logged[which]) {
    logged[which] = 1;
    debugPrintf("[jni] getProperty(%s) -> %s\n", key ? key : "(null)",
                which == 1 ? NX_STR(PHI_FMOD_MIX_RATE)
                           : which == 2 ? NX_STR(PHI_FMOD_MIX_FRAMES) : "(empty)");
  }
  if (which == 1) return jni_make_string(NX_STR(PHI_FMOD_MIX_RATE));
  if (which == 2) return jni_make_string(NX_STR(PHI_FMOD_MIX_FRAMES));
  return jni_make_string("");
}

extern void *fake_env;
typedef void *(*jnibridge_invoke_fn)(void *, void *, long long, void *, void *, void *);
static jnibridge_invoke_fn g_jnibridge_invoke = 0;
static void *j_NewObjectArray(void *env, int len, void *cls, void *init);
/* jni_make_object pools by label, which would collapse every proxy to one object.
 * Give each proxy its own object that embeds its native ptr; identify proxies by
 * address range (safe -- never reads a field on a non-proxy jobject). */
#define MAX_PROXY_OBJ 512
typedef struct { uint32_t tag; uint32_t pad; long long ptr; } FakeProxy;
static FakeProxy g_proxy_pool[MAX_PROXY_OBJ];
static int g_proxy_pool_n = 0;
static FakeProxy *g_last_proxy = 0;
static void *proxy_make(long long ptr) {
  if (g_proxy_pool_n >= MAX_PROXY_OBJ) return jni_make_object("jniproxy");
  FakeProxy *p = &g_proxy_pool[g_proxy_pool_n++];
  p->tag = TAG_CLASS; p->ptr = ptr; g_last_proxy = p;   /* TAG_CLASS -> free_ref ignores it */
  return p;
}
static long long proxy_ptr_of(void *obj) {
  uintptr_t a = (uintptr_t)obj, lo = (uintptr_t)g_proxy_pool, hi = (uintptr_t)(g_proxy_pool + g_proxy_pool_n);
  if (a >= lo && a < hi && ((a - lo) % sizeof(FakeProxy)) == 0) return ((FakeProxy *)obj)->ptr;
  return 0;
}
/* Invoke one method on a proxy through Unity's bridge. ProxyObject dispatch checks
   the jclass matches the interface and the methodID == the method (pointer compare);
   j_FromReflectedMethod passes a TAG_ID methodID straight through so it matches. */
static void proxy_invoke(void *proxy, void *cls, void *method, void *args) {
  long long ptr = proxy_ptr_of(proxy);
  if (!ptr || !g_jnibridge_invoke) return;
#if LOG_VERBOSE
  debugPrintf("[jni] proxy invoke %p ptr=%llx .%s\n", proxy, (unsigned long long)ptr, ((FakeID *)method)->name);
#endif
  g_jnibridge_invoke(fake_env, proxy, ptr, cls, method, args);
}
static void proxy_run_runnable(void *runnable) {
  proxy_invoke(runnable, intern_class("java/lang/Runnable"), get_id("java/lang/Runnable", "run", "()V"), (void *)0);
}
/* Message.sendToTarget(): the factory's HandlerThread never pumps its Looper, so the
   message is never delivered to callback.handleMessage(msg). Deliver it ourselves to
   the most-recently created proxy (the Handler$Callback the factory just built). */
static void handler_deliver_message(void *callback) {
  void *msg  = jni_make_object("android/os/Message");
  void *args = j_NewObjectArray(fake_env, 1, (void *)0, msg);
  proxy_invoke(callback, intern_class("android/os/Handler$Callback"),
               get_id("android/os/Handler$Callback", "handleMessage", "(Landroid/os/Message;)Z"), args);
}
static uint64_t g_frame_ns = 0;   /* frameTimeNanos for boxed-Long unboxing */
static void *g_frame_cb = 0;      /* registered Choreographer FrameCallback proxy */
static void deliver_doframe(void *cb) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  g_frame_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  void *boxed = jni_make_object("java/lang/Long");
  void *args  = j_NewObjectArray(fake_env, 1, (void *)0, boxed);
  static int logged = 0;
  if (logged < 3) { logged++; debugPrintf("[jni] doFrame tick -> %p (vsync pump)\n", cb); }
  proxy_invoke(cb, intern_class("android/view/Choreographer$FrameCallback"),
               get_id("android/view/Choreographer$FrameCallback", "doFrame", "(J)V"), args);
}
/* drain thread: runs posted work off the main thread (no self-deadlock / re-entrancy) */
#define RUNQ_N 128
static void *g_runq[RUNQ_N]; static int g_runq_kind[RUNQ_N]; static int g_runq_head = 0, g_runq_tail = 0;
static Mutex g_runq_lk; static CondVar g_runq_cv; static int g_runq_started = 0;
static void run_drain_thread(void *arg) {
  (void)arg;
  static uint8_t drain_tls[BIONIC_TLS_SIZE] __attribute__((aligned(16)));
  install_bionic_tls(drain_tls);
  for (;;) {
    mutexLock(&g_runq_lk);
    if (g_runq_head == g_runq_tail) {
      if (g_frame_cb) condvarWaitTimeout(&g_runq_cv, &g_runq_lk, PHI_VSYNC_PERIOD_NS); /* vsync tick */
      else            condvarWait(&g_runq_cv, &g_runq_lk);
    }
    void *o = 0; int k = 0, have = 0;
    if (g_runq_head != g_runq_tail) {
      o = g_runq[g_runq_head]; k = g_runq_kind[g_runq_head];
      g_runq_head = (g_runq_head + 1) % RUNQ_N; have = 1;
    }
    void *fcb = g_frame_cb; g_frame_cb = 0;   /* one-shot: doFrame re-registers */
    mutexUnlock(&g_runq_lk);
    if (have) { if (k == 0) proxy_run_runnable(o); else handler_deliver_message(o); }
    if (fcb) deliver_doframe(fcb);
  }
}
static Thread g_runq_thr;
static void runq_post(void *obj, int kind) {
  if (!obj) return;
  mutexLock(&g_runq_lk);
  if (!g_runq_started) {
    g_runq_started = 1;
    if (R_SUCCEEDED(threadCreate(&g_runq_thr, run_drain_thread, NULL, NULL, 0x8000, 0x2C, -2)))
      threadStart(&g_runq_thr);
  }
  int nt = (g_runq_tail + 1) % RUNQ_N;
  if (nt != g_runq_head) { g_runq[g_runq_tail] = obj; g_runq_kind[g_runq_tail] = kind; g_runq_tail = nt; }
  condvarWakeOne(&g_runq_cv);
  mutexUnlock(&g_runq_lk);
}
static void post_runnable(void *runnable) { runq_post(runnable, 0); }
static void post_message(void) { runq_post(g_last_proxy, 1); }
static int g_msg_what = 0;   /* captured from Handler.obtainMessage(what) for msg.what reads */
unsigned g_kbd_trace;      /* set when swkbd closes; counts down as we log */
/* Unity soft-input natives (captured at RegisterNatives, invoked on close). */
void *g_u_setInputString, *g_u_setInputSel, *g_u_softClosed,
     *g_u_softCancel, *g_u_kbdVisible;
/* Push the swkbd result into Unity exactly as its Java keyboard would. */
void kbd_push_result(const char *text, int cancelled) {
  typedef void (*fn_str)(void *, void *, void *);
  typedef void (*fn_ii)(void *, void *, int, int);
  typedef void (*fn_v)(void *, void *);
  typedef void (*fn_z)(void *, void *, int);
  void *cls = intern_class("com/unity3d/player/UnityPlayer");
  if (!text) text = "";
  int len = (int)strlen(text);
  debugPrintf("[kbd] push \"%s\" cancelled=%d (str=%p closed=%p)\n",
              text, cancelled, g_u_setInputString, g_u_softClosed);
  if (!cancelled && g_u_setInputString)
    ((fn_str)g_u_setInputString)(fake_env, cls, jni_make_string(text));
  if (!cancelled && g_u_setInputSel)
    ((fn_ii)g_u_setInputSel)(fake_env, cls, len, len);
  if (g_u_kbdVisible) ((fn_z)g_u_kbdVisible)(fake_env, cls, 0);
  if (cancelled) { if (g_u_softCancel) ((fn_v)g_u_softCancel)(fake_env, cls); }
  else           { if (g_u_softClosed) ((fn_v)g_u_softClosed)(fake_env, cls); }
}
/* ---- JNI approximation ledger (round 130) --------------------------------
 *
 * What Android does that we do not: it FAILS LOUDLY at this boundary. A missing
 * class throws ClassNotFoundException, a missing method NoSuchMethodError, a
 * wrong argument IllegalArgumentException -- and Unity's AndroidJNISafe wrapper
 * calls ExceptionCheck() after every single JNI call and turns a pending Java
 * exception into a managed AndroidJavaException, at the call site, naming the
 * method.
 *
 * Our j_ExceptionCheck answers "no exception" to every one of those, always.
 * So when a handler here returns the wrong thing -- an empty string where a
 * path belonged (r127's persistentDataPath), an opaque object where a String
 * belonged, NULL where an array belonged -- Unity believes the call succeeded,
 * stores the value into a typed managed field, and the failure surfaces minutes
 * later in another module as "a non-object reached a pointer operation". That
 * is the fault family this port has been chasing for thirty rounds: not a worse
 * bug rate than Android, just vastly worse localisation.
 *
 * We do NOT start throwing. Raising into Unity's managed runtime from a fake
 * JNI is its own hazard, and every throw site would be another guess. What is
 * free is making the boundary loud in the other direction: record every call we
 * answer with a catch-all, and say so.
 *
 * Two things make this worth more than a log line per call:
 *
 *  1. DEDUPED AND COUNTED. First occurrence logs; the rest increment. JNI
 *     catch-alls run per frame (see the MotionEvent traffic in any log), and an
 *     unthrottled log would cost more SD writes than the fault it is chasing.
 *
 *  2. THE ExceptionCheck PROBE. Unity only calls ExceptionCheck() through
 *     AndroidJNISafe -- i.e. on the calls whose result it actually inspects.
 *     Arming a marker on a catch-all and consuming it in j_ExceptionCheck tells
 *     us which approximations sit on a path where Android WOULD have raised.
 *     Those are the ones whose wrong answer becomes a stored value; the rest are
 *     calls nobody looks at. That is the difference between 40 things we fake
 *     and the 3 that matter, and it costs one compare in the hot path.
 *
 * Attribution is armed at the top of each dispatch_* and consumed by the next
 * ExceptionCheck ON THE SAME THREAD (keyed on the TLS pointer), so a check that
 * follows a *handled* call cannot be misattributed to an earlier approximation.
 */
#define JNI_APPROX_MAX 96

typedef struct {
  /* Sized to match FakeID exactly (cls[96]/name[64]/sig[160]). Shorter fields
   * would truncate -- and a truncated signature merges distinct overloads into
   * one ledger entry, which is the one way this diagnostic could lie. */
  char cls[96], name[64], sig[160];
  const char *kind;          /* static literal: never freed, never copied */
  unsigned    hits;
  unsigned char inspected;   /* Unity ran ExceptionCheck straight after this */
} JniApprox;

#if PHI_JNI_LOUD
static JniApprox g_approx[JNI_APPROX_MAX];
static int       g_approx_n    = 0;
static int       g_approx_drop = 0;   /* distinct sites past the table */
/* armed by a catch-all, consumed by the next same-thread ExceptionCheck.
 * The thread key is the address of a thread-local byte -- no libnx symbol
 * needed, and diag.c already relies on __thread working in this toolchain. */
static __thread char  g_approx_tls_key;
static volatile int   g_approx_last     = -1;
static volatile void *g_approx_last_tls = NULL;
#endif  /* PHI_JNI_LOUD: both settings must compile */

/* Arm/disarm per call. Without this an ExceptionCheck after a perfectly
 * handled call would credit itself to whatever we last approximated. */
static inline void jni_approx_arm(void) {
#if PHI_JNI_LOUD
  g_approx_last = -1;
  g_approx_last_tls = NULL;
#endif
}

/* String-keyed core. Exported as jni_note_approx() so the Unity/input
 * dispatchers in other TUs can record their own catch-alls without needing the
 * FakeID layout -- unity_dispatch_object's terminal `jni_make_object(cls)` is
 * the single most important site in the ledger (it is the r127 shape, and it
 * never returns NULL so it never shows up as NULL-OBJ). */
void jni_note_approx(const char *kind, const char *cl, const char *nm,
                     const char *sg) {
#if !PHI_JNI_LOUD
  (void)kind; (void)cl; (void)nm; (void)sg;
#else
  if (!kind || !cl || !nm || !sg) return;
  /* Compare the TEXT, never the addresses: FakeID's fields are per-instance
   * char arrays (the -Waddress note above act_object's NULL path). A
   * first-char prefilter keeps the scan off strcmp for all but a candidate. */
  int idx = -1;
  for (int i = 0; i < g_approx_n; i++) {
    if (g_approx[i].name[0] != nm[0]) continue;
    if (strcmp(g_approx[i].name, nm)) continue;
    if (strcmp(g_approx[i].cls, cl))  continue;
    if (strcmp(g_approx[i].sig, sg))  continue;
    idx = i; break;
  }
  if (idx < 0) {
    if (g_approx_n >= JNI_APPROX_MAX) {
      if (++g_approx_drop == 1)
        debugPrintf("[jniapx] ledger full at %d sites -- further distinct "
                    "approximations counted only\n", JNI_APPROX_MAX);
      return;
    }
    idx = g_approx_n;
    snprintf(g_approx[idx].cls,  sizeof g_approx[0].cls,  "%s", cl);
    snprintf(g_approx[idx].name, sizeof g_approx[0].name, "%s", nm);
    snprintf(g_approx[idx].sig,  sizeof g_approx[0].sig,  "%s", sg);
    g_approx[idx].kind = kind;
    g_approx[idx].hits = 0;
    g_approx[idx].inspected = 0;
    /* Deliberately lock-free. Two threads claiming the same slot can garble one
     * entry's text or double-log it; neither can go out of bounds (idx is
     * bounded by the check above and snprintf is size-capped), so the worst case
     * is a cosmetically wrong diagnostic line, never a fault. A mutex here would
     * be held across debugPrintf, which is the deadlock shape round 105 spent a
     * session on. */
    g_approx_n = idx + 1;              /* publish the count last */
    debugPrintf("[jniapx] %-12s %s.%s%s\n", kind, cl, nm, sg);
  }
  g_approx[idx].hits++;
  g_approx_last = idx;
  g_approx_last_tls = (void *)&g_approx_tls_key;
#endif
}

static void jni_approx(const char *kind, const FakeID *id) {
  jni_note_approx(kind, id->cls, id->name, id->sig);
}

/* Called from j_ExceptionCheck. On Android this is where the failure would have
 * been reported; here it tells us Unity was LOOKING. */
static inline void jni_approx_checked(void) {
#if PHI_JNI_LOUD
  const int idx = g_approx_last;
  if (idx < 0 || idx >= g_approx_n) return;
  if (g_approx_last_tls != (void *)&g_approx_tls_key) return;   /* another thread's call */
  g_approx_last = -1;
  if (!g_approx[idx].inspected) {
    g_approx[idx].inspected = 1;
    debugPrintf("[jniapx] INSPECTED  %s.%s%s -- AndroidJNISafe checked this "
                "call; on Android a wrong answer would have raised HERE\n",
                g_approx[idx].cls, g_approx[idx].name, g_approx[idx].sig);
  }
#endif
}

void jni_approx_summary(const char *why) {
#if !PHI_JNI_LOUD
  (void)why;
#else
  debugPrintf("[jniapx] ===== JNI approximation ledger (%s) =====\n",
              why ? why : "?");
  if (!g_approx_n) { debugPrintf("[jniapx] (nothing approximated)\n"); return; }
  debugPrintf("[jniapx] %-12s %8s %4s  method\n", "kind", "hits", "insp");
  for (int i = 0; i < g_approx_n; i++)
    debugPrintf("[jniapx] %-12s %8u %4s  %s.%s%s\n",
                g_approx[i].kind, g_approx[i].hits,
                g_approx[i].inspected ? "YES" : "-",
                g_approx[i].cls, g_approx[i].name, g_approx[i].sig);
  if (g_approx_drop)
    debugPrintf("[jniapx] + %d distinct site(s) past the ledger\n", g_approx_drop);
  debugPrintf("[jniapx] INSPECTED sites are the ones Unity reads back. A wrong "
              "answer there is what becomes a bad stored value.\n");
  debugPrintf("[jniapx] ==========================================\n");
#endif
}

static void *act_object(const FakeID *id, va_list va) {
  if (g_kbd_trace) { g_kbd_trace--;
    debugPrintf("[kbd] after-kbd obj call: %s.%s sig=%s\n",
                id->cls ? id->cls : "?", id->name ? id->name : "?",
                id->sig ? id->sig : "?"); }
  /* A String-returning keyboard call: show it, then hand back what was typed.
   * This is the path this game uses -- it never polls a getter afterwards. */
  if (is_editbox_show(id->name) && sig_returns(id->sig, "Ljava/lang/String;")) {
    editbox_show(first_string_arg(id->sig, va), 64);
    const char *t = editbox_text();
    debugPrintf("[kbd] show-returns-text %s.%s -> \"%s\"\n",
                id->cls ? id->cls : "?", id->name, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  if (is_editbox_text(id->name)) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text getter(obj) %s.%s -> \"%s\"\n",
                id->cls ? id->cls : "?", id->name, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  /* JNIBridge.newInterfaceProxy(long nativePtr, Class[] ifaces) -> remember the
   * proxy so a later Handler.post(runnable) can invoke its run() (see post_runnable). */
  if (name_has(id->name, "newInterfaceProxy")) {
    long long ptr = va_arg(va, long long);
    void *proxy = proxy_make(ptr);
    debugPrintf("[jni] newInterfaceProxy ptr=%llx -> proxy=%p\n", (unsigned long long)ptr, proxy);
    return proxy;
  }
  /* Handler.obtainMessage(what): must return a REAL Message or the game's C++
   * wrapper null-checks and silently skips msg.sendToTarget() -- the UI-manager
   * factory then waits forever for handleMessage's side effect. Capture 'what'
   * so the callback's msg.what field read sees the right value. */
  if (name_has(id->name, "obtainMessage")) {
    if (id->sig[1] == 'I') g_msg_what = va_arg(va, int);
    debugPrintf("[jni] obtainMessage what=%d -> Message\n", g_msg_what);
    return jni_make_object("android/os/Message");
  }
  if (name_has(id->name, "getLooper") || name_has(id->name, "getMainLooper"))
    return jni_make_object("android/os/Looper");
  if (name_has(id->name, "getInstance") && name_has(id->cls, "Choreographer"))
    return jni_make_object("android/view/Choreographer");
  /* Unity launch args: libunity reads currentActivity.getIntent().getStringExtra("unity").
   * Serve -job-worker-count 0 to run jobs inline on main: diagnostic for the
   * UIGeometryJob garbage-input crash (if it persists inline, the corruption
   * predates the job and the crash stack shows the producer; if it vanishes,
   * it's a job/fence lifetime race) -- and a potential playable workaround.
   * The JobSystem log line 'Creating JobQueue using job-worker-count value %d'
   * confirms the effective value. */
  if (name_has(id->name, "getIntent"))
    return jni_make_object("android/content/Intent");
  if (name_has(id->name, "getExtras"))
    return jni_make_object("android/os/Bundle");
  if (name_has(id->cls, "Bundle") && (name_has(id->name, "getString") || name_has(id->name, "getCharSequence"))) {
    debugPrintf("[jni] Bundle.%s -> launch args served\n", id->name);
    return jni_make_string("");
  }
  if (name_has(id->name, "getStringExtra")) {
    const char *k = first_string_arg(id->sig, va);
    if (k && !strcmp(k, "unity")) {
      debugPrintf("[jni] getStringExtra(unity) -> launch args served\n");
      return jni_make_string("");
    }
    return NULL;
  }
  /* Uri.encode/decode: Unity round-trips PlayerPrefs keys through these. We are
   * the storage, so identity (return the input string) round-trips correctly and
   * keeps keys non-empty. Must precede the generic handlers. */
  if (name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return va_arg(va, void *);
  if (name_has(id->name, "AssetManager") || sig_returns(id->sig, "Landroid/content/res/AssetManager;"))
    return get_asset_manager_obj();
  if (name_has(id->name, "ClassLoader") || sig_returns(id->sig, "Ljava/lang/ClassLoader;"))
    return get_classloader_obj();
  if (sig_returns(id->sig, "Ljava/lang/Class;"))
    return intern_class("java/lang/Object");
  // version / package / device / storage strings
  if (name_has(id->name, "VersionName")) return jni_make_string("2.1.6");
  if (name_has(id->name, "PackageName")) return jni_make_string(GAME_PACKAGE);
  if (name_has(id->name, "DeviceModel")) return jni_make_string("Switch");
  // archive name getters: the engine builds "<dir>/<name>.android.mvgl" for 5
  // slots (main + patch + 3 asset packs). We map them to the 5 shipped archives
  // (main.10007 + the four CRDB media DBs) so all of them mount.
  if (name_has(id->name, "ObbMainFileName"))  return jni_make_string(MAIN_OBB_BASE);
  if (name_has(id->name, "ObbPatchFileName")) return jni_make_string("CRDBbgm");
  if (name_has(id->name, "AssetPack1"))       return jni_make_string("CRDBvoice");
  if (name_has(id->name, "AssetPack2"))       return jni_make_string("CRDBse");
  if (name_has(id->name, "AssetPack3"))       return jni_make_string("CRDBmov");
  if (name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(va_arg(va, void *)));
  if (name_has(id->name, "Language") || name_has(id->name, "language"))
    return jni_make_string(lang_code());
  // Environment.getExternalStorageState() must return the SAME token as the
  // Environment.MEDIA_MOUNTED field ("mounted", see field_object) or the engine
  // decides external storage is unavailable and the save path never initialises.
  if (name_has(id->cls, "os/Environment")) {
    if (name_has(id->name, "ExternalStorageState")) return jni_make_string("mounted");
    if (name_has(id->name, "Directory")) return jni_make_object("java/io/File"); /* ->getAbsolutePath */
  }
  // Locale.getCountry/getISO3*/toString/getDisplayName: the game reads the
  // default locale (UnityPlayer.getDefault) for its language pick; "" here left
  // the locale blank in the log. Mirror lang_code()'s ja/en choice. Guarded by
  // the Locale class so we don't hijack toString()/getCountry on other objects.
  if (name_has(id->cls, "Locale")) {
    int zh = !strcmp(lang_code(), "zh");
    if (!strcmp(id->name, "getCountry"))     return jni_make_string(zh ? "CN" : "US");
    if (!strcmp(id->name, "getISO3Language"))return jni_make_string(zh ? "zho" : "eng");
    if (!strcmp(id->name, "getISO3Country")) return jni_make_string(zh ? "CHN" : "USA");
    if (!strcmp(id->name, "toString") || name_has(id->name, "getDisplayName") ||
        name_has(id->name, "getDisplayLanguage"))
      return jni_make_string(zh ? "zh_CN" : "en_US");
  }
  if (name_has(id->name, "DataPath") || name_has(id->name, "StoragePath") ||
      name_has(id->name, "FilesDir") || name_has(id->name, "RootPath") ||
      name_has(id->name, "ObbDir") || name_has(id->name, "AssetPath") ||
      name_has(id->name, "Path"))
    return jni_make_string(managed_path(data_dir()));
  // text the user typed on the Switch software keyboard
  if (is_editbox_text(id->name)) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text getter %s.%s -> \"%s\"\n",
                id->cls ? id->cls : "?", id->name, t ? t : "(null)");
    return jni_make_string(t);
  }
  kbd_sniff(id->cls, id->name);   /* a jstring getter lands here, not in act_void */
  // asset-pack names ("" is fine: the engine appends the hardcoded CRDB* name,
  // and the data layer's basename fallback finds the flat file regardless)
  // Android object getters that must NOT be null, or the engine aborts the
  // chain. getPackageInfo()/getApplicationInfo() are how Unity reaches
  // PackageInfo.versionName/versionCode (-> Application.version); returning null
  // here is why the version stayed blank even with field access fixed -- Unity
  // got a null PackageInfo and never read the field. Hand back live (opaque)
  // objects; the subsequent field reads then resolve via field_object/field_int.
  if (name_has(id->name, "getPackageInfo"))     return jni_make_object("android/content/pm/PackageInfo");
  if (name_has(id->name, "getApplicationInfo")) return jni_make_object("android/content/pm/ApplicationInfo");
  if (name_has(id->name, "getPackageManager"))  return jni_make_object("android/content/pm/PackageManager");
  if (name_has(id->name, "getResources"))       return jni_make_object("android/content/res/Resources");
  if (name_has(id->name, "getConfiguration"))   return jni_make_object("android/content/res/Configuration");
  /* ---- java.lang.reflect: Unity's AndroidJavaObject marshalling ------
   * NARROW on purpose. An earlier revision returned a live object for ANY
   * signature ending in Class/Method/Field/Constructor, and that was worse
   * than the NULLs: the engine took those fakes as real and wedged before
   * frame 0 (3840 frames -> 0). Only add a case with a log line behind it. */
  /* ---- ARRAY returns: empty array, never NULL ---------------------------
   * This is a Java-semantics fix, not a guess. `null.length` throws;
   * `new T[0].length` is 0 and every for-each over it is skipped. An empty
   * array is the CORRECT way to say "none available" and cannot be
   * mistaken for a real device/route/id the way an opaque object can.
   * Covers the enumeration calls the log flagged:
   *   AudioManager.getDevices(I)[Landroid/media/AudioDeviceInfo;
   *   InputDevice.getDeviceIds()[I
   * Deliberately AFTER the specific handlers above, so anything we model
   * properly still wins. */
  if (id->sig[0] && sig_returns(id->sig, "[")) {
    int esz = 1;
    const char *r = strchr(id->sig, ')');
    if (r && r[1] == '[') {
      switch (r[2]) {
        case 'J': case 'D': esz = 8; break;
        case 'I': case 'F': esz = 4; break;
        case 'S': case 'C': esz = 2; break;
        default:  esz = (r[2] == 'L' || r[2] == '[') ? 8 : 1; break;
      }
    }
    return make_pri_array_adopt(NULL, 0, esz);
  }

  /* Locale.getDefault() -- Unity derives Application.systemLanguage from the
   * Java locale. Gate on the RETURN TYPE, not the class: a class-wide rule
   * also swallows getLanguage()/getCountry(), which must return Strings.
   * Handing back a Locale there made Unity call GetStringUTFChars on an
   * object and fault dereferencing its text as a pointer
   *   esr=92000004 (data abort) far=6374696f... ("ctio", ASCII)
   * The String catch-all further down handles those correctly. */
  if (name_has(id->cls, "java/util/Locale") &&
      (sig_returns(id->sig, "Ljava/util/Locale;") ||
       !strcmp(id->name, "getDefault")))   /* exact: getDefault only */
    return jni_make_object("java/util/Locale");

  /* Object.getClass() can NEVER be null in Java -- returning NULL here made
   * managed code NPE immediately (log: NULL-return at 6147, first NRE at
   * 6160). Unity asks for it through the generic path, so the signature
   * reads ()Ljava/lang/Object; rather than ()Ljava/lang/Class;.
   * act_object() has no receiver, so this cannot vary the class by object --
   * that is fine: the one case that needs a specific class (Bitmap) is
   * answered by j_GetObjectClass on the JNI env path, not by a method call. */
  if (!strcmp(id->name, "getClass"))       /* exact: not getClassName */
    return intern_class("java/lang/Object");

  /* Context path getters, and File.getAbsolutePath on their result.
   *
   * unity_jni.c handles these, but only behind has(cls,"content/Context") and
   * has(cls,"java/io/File"). Unity asks for them through the generic path, so
   * the id arrives as
   *     java/lang/Object.getFilesDir()Ljava/lang/Object;
   * and neither gate matches. They fell through to the opaque-object default,
   * so getFilesDir() returned a plain Object and getAbsolutePath() on it did
   * too -- Application.persistentDataPath ended up as a handle instead of a
   * path, and the game could not read or write its save:
   *     Failed to load local player data with error:
   *       Error setting value to 'm_version' on 'InventoryPlayerDataObject'
   * with five more like it, leaving those objects part-initialised. That is
   * the upstream shape of the "non-object in a pointer slot" faults this port
   * has been chasing.
   *
   * Exact strcmp, not name_has: "getPath" would also swallow Uri.getPath and
   * friends, which are not ours to answer. */
  if (!strcmp(id->name, "getFilesDir")  || !strcmp(id->name, "getCacheDir") ||
      !strcmp(id->name, "getDataDir")   || !strcmp(id->name, "getExternalFilesDir") ||
      !strcmp(id->name, "getExternalCacheDir"))
    return jni_make_object("java/io/File");

  /* getAbsolutePath is deliberately NOT handled here: act_object has no
   * receiver, so a name-only rule would answer for EVERY File in the game --
   * including the ones Unity uses for its own il2cpp extraction paths. Doing
   * that hung the engine at frame 0 (round 128). It is handled in
   * dispatch_object() instead, which does have the receiver. */

  /* Activity.getApplication() -> the Application object. Same reasoning as
   * getApplicationContext (round 25): an Application IS a Context, and this
   * loader serves every Context role from its one fake Activity. */
  if (!strcmp(id->name, "getApplication"))  /* exact: not ...Context/...Info */
    return jni_make_activity_object();

  /* Window.getAttributes() -> WindowManager$LayoutParams. This chain only
   * became reachable because round 12 started returning a live Window from
   * getWindow(); finish it rather than leaving a half-built path. */
  if (name_has(id->name, "getAttributes"))
    return jni_make_object("android/view/WindowManager$LayoutParams");

  /* Unity ReflectionHelper member resolution.
   *
   * THE ARGUMENT COUNTS DIFFER -- this is what crashed round 15:
   *   getFieldID  (Class, String name, String sig, boolean)  -> Field
   *   getMethodID (Class, String name, String sig, boolean)  -> Method
   *   getConstructorID (Class, String sig)                   -> Constructor
   * Reading three object args for all three walked off the end of the
   * constructor call's va_list and then dereferenced the garbage:
   *   esr=92000004 far=6374696f... (ASCII "ctio" read as a pointer)
   * Read only what the signature actually has, and never dereference an
   * argument without checking it looks like a tagged object first. */
  if (name_has(id->name, "getFieldID") || name_has(id->name, "getMethodID") ||
      name_has(id->name, "getConstructorID")) {
    int is_ctor = name_has(id->name, "getConstructorID");
    void *acls = va_arg(va, void *);
    void *a1   = va_arg(va, void *);
    void *a2   = is_ctor ? NULL : va_arg(va, void *);
    const char *cn = safe_class_name(acls);
    const char *s1 = safe_utf(a1);
    const char *s2 = safe_utf(a2);
    const char *mn = is_ctor ? "<init>" : s1;
    const char *ms = is_ctor ? s1 : s2;
    static int nlog = 0;
    if (nlog < 12) { nlog++;
      debugPrintf("[jni] %s -> id %s.%s%s\n", id->name,
                  (cn && *cn) ? cn : "?", (mn && *mn) ? mn : "?", ms); }
    return get_id((cn && *cn) ? cn : "java/lang/Object",
                  (mn && *mn) ? mn : "?", ms ? ms : "");
  }
  if (name_has(id->name, "getDeclaringClass"))
    return jni_make_object("java/lang/Class");
  /* Activity.getWindow() -- Unity walks getWindow().getDecorView() for the
   * display/insets setup. NULL there stops the display config cold. */
  /* Context accessors: an Activity is a Context on Android, and this
   * loader already serves every Context role from the one fake Activity
   * (getPackageName / getAssets / getFilesDir all resolve off it). NULL
   * here made Unity NPE at first scene load and abandon render-target
   * setup -- the black screen with correct draw calls and glErr=0x0. */
  if (name_has(id->name, "getApplicationContext") ||
      name_has(id->name, "getBaseContext") ||
      name_has(id->name, "getApplicationCon"))
    return jni_make_activity_object();

  if (name_has(id->name, "getWindow"))
    return jni_make_object("android/view/Window");
  if (name_has(id->name, "getDecorView"))
    return jni_make_object("android/view/View");

  if (sig_returns(id->sig, "Ljava/lang/String;")) {
    /* An empty string is a LIE with a plausible type. This is exactly the shape
     * of the r127 bug: persistentDataPath came back as something String-ish and
     * wrong, and nothing complained until the save path failed far away. */
    jni_approx("EMPTY-STRING", id);
    return jni_make_string("");  // UUID, asset-pack name, etc.
  }

  /* Anything still unhandled returns NULL, which managed code will
   * usually dereference. The ledger replaces the old private warned[] table:
   * same one-line-per-method behaviour, but now counted, ExceptionCheck-probed
   * and reprinted in full on any crash dump. */
  if (sig_returns(id->sig, "L") || sig_returns(id->sig, "[")) {
    jni_approx("NULL-OBJ", id);
  }
  (void)va;
  return NULL;
}

static juint act_int(const FakeID *id, va_list va) {
  if (name_has(id->cls, "Handler") && name_has(id->name, "post")) {   /* post/postDelayed(Runnable) */
    post_runnable(va_arg(va, void *)); return 1;
  }
  /* Bundle.containsKey(key): the launch-args probe -- true for the 'unity' extra so
   * Unity proceeds to Bundle.getString(unity) (see act_object) and applies args. */
  if (name_has(id->cls, "Bundle") && name_has(id->name, "containsKey")) {
    const char *k = first_string_arg(id->sig, va);
    /* PVZ_UNITY_LAUNCH_ARGS is empty now: the -job-worker-count 0 injection was a
     * crash-era experiment. Starving the job workers stops Unity building
     * culling/batching/UI geometry -> frames advance but nothing renders.
     * Report the extra as ABSENT so Unity uses its normal defaults. */
    int hit = 0; (void)k;
    debugPrintf("[jni] Bundle.containsKey(%s) -> %d\n", k ? k : "?", hit);
    return hit ? 1 : 0;
  }
  // java.lang.Integer.parseInt(String[,radix]) / Long.parseLong: FMOD's audio
  // path parses getProperty()'s "48000"/"64" results through these. The old
  // act_int fall-through returned 0 -> framesPerBuffer parsed to 0 -> FMOD's
  // OpenSL output init failed with "Error initializing output device" (60).
  if (name_has(id->name, "parseInt") || name_has(id->name, "parseLong")) {
    const char *s = first_string_arg(id->sig, va);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u\n", id->name, s ? s : "", v);
    return v;
  }
  /* Display$Mode getters (round 149). Without these the mode we now return
   * would report a 0x0 panel, which is no better than the opaque handle. */
  if (!strcmp(id->name, "getPhysicalWidth"))  return (juint)screen_width;
  if (!strcmp(id->name, "getPhysicalHeight")) return (juint)screen_height;
  if (!strcmp(id->name, "getModeId"))         return 1;
  if (is_editbox_open(id->name)) return (juint)editbox_is_open();
  // some builds expose Show/Open as an int (success) call rather than void
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 32); return 1; }
  // Play Asset Delivery: with NO Play Core on Switch, the engine MUST take the
  // "missing" path, where it treats every asset pack as install-time/local and
  // reads assets synchronously from the APK/bundle. Returning false (the old
  // default) tells Unity Play Core IS present, so it uses the ASYNC AssetPackManager
  // path -- it calls getAssetPackState() with a callback that we never invoke and
  // then waits forever for the pack, so the resident scene never loads and
  // ResidentSystem.Awake never runs (the live boot stall). Return true.
  if (name_has(id->name, "playCoreApiMissing")) return 1;
  // The other Google check whose default (0 == ConnectionResult.SUCCESS) wrongly
  // means "Play Services available": isGooglePlayServicesAvailable(). Return
  // SERVICE_MISSING (1) so the Play Games plugin cleanly disables itself instead
  // of trying to sign in against GMS that isn't there. (Not hit during boot --
  // Play Games activates on user action -- but correct for when it is.)
  if (name_has(id->name, "isGooglePlayServicesAvailable")) return 1; /* ConnectionResult.SERVICE_MISSING */
  (void)va;
  // every other "is something open / clicked / ok" probe -> false/0
  jni_approx("INT-0", id);
  return 0;
}

static float act_float(const FakeID *id, va_list va) {
  (void)va;
  float x, y, z;
  android_get_orientation(&x, &y, &z);
  if (name_has(id->name, "OrientationX")) return x;
  if (name_has(id->name, "OrientationY")) return y;
  if (name_has(id->name, "OrientationZ")) return z;
  /* Display.getRefreshRate() -- round 131. Was falling through to 0.0 Hz, six
   * times and INSPECTED. Unity feeds this to Screen.currentResolution and its
   * frame pacing; zero is not a value a real display ever reports. Answer with
   * the panel rate we already pace to (PHI_VSYNC_PERIOD_NS), so the managed side
   * and the swap loop agree instead of disagreeing silently. */
  if (name_has(id->name, "getRefreshRate"))
    return (float)(1000000000.0 / (double)PHI_VSYNC_PERIOD_NS);
  jni_approx("FLOAT-0", id);
  return 0.0f;
}

static void act_void(const FakeID *id, va_list va) {
  if (name_has(id->name, "runOnUiThread")) { post_runnable(va_arg(va, void *)); return; }
  if (name_has(id->name, "sendToTarget")) { post_message(); return; }
  if (name_has(id->name, "postFrameCallback")) {
    void *cb = va_arg(va, void *);
    mutexLock(&g_runq_lk);
    if (!g_runq_started) {
      g_runq_started = 1;
      if (R_SUCCEEDED(threadCreate(&g_runq_thr, run_drain_thread, NULL, NULL, 0x8000, 0x2C, -2)))
        threadStart(&g_runq_thr);
    }
    g_frame_cb = cb;
    condvarWakeOne(&g_runq_cv);
    mutexUnlock(&g_runq_lk);
    static int fclog = 0; if (fclog < 3) { fclog++; debugPrintf("[jni] postFrameCallback cb=%p (vsync pump on)\n", cb); }
    return;
  }
  if (name_has(id->name, "removeFrameCallback")) {
    mutexLock(&g_runq_lk); g_frame_cb = 0; mutexUnlock(&g_runq_lk); return;
  }
  if (is_editbox_show(id->name)) { editbox_show(first_string_arg(id->sig, va), 64); return; }
  if (is_editbox_close(id->name)) { editbox_close(); return; }
  kbd_sniff(id->cls, id->name);
  (void)va;
  if (!strcmp(id->name, "finish") || name_has(id->name, "appEnd") ||
      name_has(id->name, "exitApp")) {
    jni_quit_requested = 1;
    return;                 /* HANDLED -- returning here only skips the ledger */
  }
  // openStore / sendBroadcast / IME open / Mobage / web view: no-op
  jni_approx("VOID-NOP", id);
}

// ---------------------------------------------------------------------------
// top-level dispatch by class + return kind
// ---------------------------------------------------------------------------

/* ZOOKEEPER DX port: delegate Unity/input classes to our modules */
#include "unity_jni.h"
#include "unity_input.h"

static int is_t2b(const char *cls)  { return name_has(cls, "Text2Bitmap"); }
static int is_mov(const char *cls)  { return name_has(cls, "MoviePlayer"); }

// Breadcrumb: the game's own Java side is reached only through JNI upcalls.
// For Phigros the dex shows com/PigeonGames/Phigros/*, com/Exceed7/NativeAudio/*
// (handled in phigros_jni.c), com/taptap/sdk/* and com/tapsdk/antiaddiction/*
// -- but not which the C# actually invokes or in what order. Log each unique
// app-class upcall once so the first run that reaches the login/save stage
// tells us precisely what to implement, instead of guessing. Behaviour is
// unchanged: after logging, the call still falls through to the act_* handlers.
static void log_app_upcall(const FakeID *id) {
  /* Phigros app-owned Java namespaces. "kiteretsu" was Zookeeper's and matched
   * nothing here, which would have made this diagnostic silently useless. */
  if (!name_has(id->cls, "PigeonGames") &&
      !name_has(id->cls, "Exceed7")     &&
      !name_has(id->cls, "taptap")      &&
      !name_has(id->cls, "tapsdk")      &&
      !name_has(id->cls, "leancloud")   &&
      !name_has(id->cls, "tds/")) return;
  static const char *seen[64]; static int seen_n = 0;
  for (int i = 0; i < seen_n; i++) if (seen[i] == id->name) return; // interned name ptr
  if (seen_n < 64) seen[seen_n++] = id->name;
  debugPrintf("[jni] app upcall: %s.%s%s\n", id->cls, id->name, id->sig);
}

static void *dispatch_object(void *recv, const FakeID *id, va_list va) {
  jni_approx_arm();
  log_app_upcall(id);
  /* MotionEvent.obtain(MotionEvent): copy factory. The engine copies our
   * injected event and reads the copy after inject returns; return a real
   * UEvent copy so getSource/getX/getY on it hit our handlers (else they read
   * 0, getSource looks non-touch, and the event is dropped before getX/getY). */
  if (input_owns_class(id->cls) && !strcmp(id->name, "obtain") &&
      strstr(id->sig, "(Landroid/view/MotionEvent;)")) {
    void *src = va_arg(va, void *);
    return unity_motionevent_obtain(src);
  }
  /* String.getBytes([charset]) -> byte[] of the string's UTF-8 bytes. Unity's
   * PlayerPrefs key-encoding is key.getBytes() -> new String([B,charset) ->
   * Uri.encode(...); without real bytes the whole chain collapsed to "" and
   * every encoded-key pref collided. Route by the FakeString receiver. */
  if (recv && nx_tag_of(recv) == TAG_STRING && name_has(id->name, "getBytes")) {
    const char *u = ((FakeString *)recv)->utf; int n = (int)strlen(u);
    char *d = malloc(n > 0 ? n : 1); if (n) memcpy(d, u, n);
    return make_pri_array_adopt(d, n, 1);
  }
  /* Fluent APIs: when a method returns its own declaring class, Java
   * convention is `return this`. Scanner.useDelimiter is the one that bit
   * us -- Unity parses boot.config as
   *     new Scanner(stream, enc).useDelimiter("\\A").next()
   * and a NULL from useDelimiter breaks the chain silently. */
  /* Deliberately NOT a general "return type == declaring class -> return
   * recv" rule. For a STATIC call the receiver is the jclass, so a factory
   * like Foo.create()->Foo would hand back the class where an instance is
   * expected. Round 11 was exactly this mistake -- a plausible-looking lie
   * is worse than a NULL. Listed classes only, each one a real fluent API
   * that Unity actually chains. */
  /* AlertDialog$Builder is Unity's fatal-error popup. We cannot show a Java
   * dialog, but returning NULL from the chain is worse than useless: the
   * setters are fluent (each returns Builder), and show() returning NULL
   * leaves Unity waiting on a dialog that does not exist -- a silent hang at
   * 0 frames. Give the chain real objects and log loudly, so the underlying
   * error is visible in debug.log instead of being swallowed by a popup
   * nobody can see. */
  if (recv && name_has(id->cls, "AlertDialog$Builder") &&
      sig_returns(id->sig, "Landroid/app/AlertDialog;")) {
    debugPrintf("[jni] *** AlertDialog.show() -- Unity is reporting a FATAL "
                "startup error. The real cause is above this line. ***\n");
    return jni_make_object("android/app/AlertDialog");
  }

  if (recv &&
      (name_has(id->cls, "java/util/Scanner") ||
       name_has(id->cls, "java/lang/StringBuilder") ||
       name_has(id->cls, "java/lang/StringBuffer") ||
       name_has(id->cls, "AlertDialog$Builder"))) {
    char want[160];
    snprintf(want, sizeof want, "L%s;", id->cls);
    if (sig_returns(id->sig, want)) {
      static int logged = 0;
      if (!logged) { logged = 1;
        debugPrintf("[jni] fluent: %s.%s returns receiver\n", id->cls, id->name); }
      return recv;
    }
  }
  /* File.getAbsolutePath on a File WE handed back from getFilesDir/getCacheDir.
   * Routed by RECEIVER, not by method name: Unity calls getAbsolutePath on its
   * own Files too, and answering those with our data root corrupts the il2cpp
   * extraction paths and deadlocks startup.
   * The method's class is erased to java/lang/Object on the generic path, so
   * only the receiver can tell these apart. */
  if (recv && nx_tag_of(recv) == TAG_OBJECT &&
      (!strcmp(id->name, "getAbsolutePath") || !strcmp(id->name, "getCanonicalPath") ||
       !strcmp(id->name, "getPath"))) {
    const FakeObject *fo = (const FakeObject *)recv;
    /* unity_jni labels its Files "File"; the generic path above makes
     * "java/io/File". Accept both, and only those -- "AssetFileDescriptor"
     * also contains "File" and is not a path. */
    if (!strcmp(fo->label, "java/io/File") || !strcmp(fo->label, "File")) {
      extern const char *nx_managed_root(void);
      return jni_make_string(nx_managed_root());
    }
  }
  if (phigros_owns_class(id->cls)) return phigros_dispatch_object(recv, id, va);
  if (unity_owns_class(id->cls)) return unity_dispatch_object(recv, id, va);
  // any method returning a Bitmap is text rendering (Char2Bitmap / getShadowBitmap
  // / ...): the loaded class always reads back as java/lang/Object, so route by
  // return type rather than class name.
  const int wants_bitmap = sig_returns(id->sig, "Landroid/graphics/Bitmap;");
  return (is_t2b(id->cls) || wants_bitmap) ? t2b_object(id, va) : act_object(id, va);
}
static juint dispatch_int(void *recv, const FakeID *id, va_list va) {
  jni_approx_arm();
  log_app_upcall(id);
  // java.lang.String instance methods reached via CallIntMethod (Unity's
  // java::lang::String::length() does this to size path buffers). The receiver
  // is our FakeString; GetObjectClass reports it as java/lang/Object, so route
  // on the receiver tag + method name, NOT id->cls. Returning 0 here (the old
  // act_int fall-through) undersizes the OBB-path sprintf buffer and overflows.
  if (recv && nx_tag_of(recv) == TAG_STRING) {
    if (!strcmp(id->name, "length"))   return utf16_len(((FakeString *)recv)->utf);
    if (!strcmp(id->name, "hashCode")) return 0;
    if (!strcmp(id->name, "isEmpty"))  return ((FakeString *)recv)->utf[0] == '\0';
    /* String.equals(Object) -- round 131. The ledger caught this falling to
     * act_int, i.e. we answered "not equal" for EVERY string comparison, and
     * marked it INSPECTED (Unity read the result back). Compare the UTF-8 the
     * two FakeStrings carry. A non-FakeString argument is genuinely not equal
     * to a String, so 0 is the right answer there and no approximation. */
    if (!strcmp(id->name, "equals")) {
      const void *o = va_arg(va, void *);
      if (o == recv) return 1;
      if (!o || nx_tag_of(o) != TAG_STRING) return 0;
      return !strcmp(((FakeString *)recv)->utf, ((FakeString *)o)->utf);
    }
  }
  /* Boxed PlayerPrefs value (Integer/Long/Boolean) from getAll(): unbox by the
   * receiver so only our own boxes are affected. intValue/longValue/booleanValue
   * all land here (CallInt/Long/BooleanMethod -> dispatch_int). */
  if (unity_is_boxed(recv)) return unity_boxed_int(recv);
  /* MotionEvent/KeyEvent getters: the engine resolves these via
   * GetObjectClass(event) -> java/lang/Object, so id->cls is NOT the real
   * class. Route on the receiver tag (mirrors the FakeString case above), or
   * touch getters silently fall through to act_int and return 0. */
  if (input_owns_recv(recv)) return input_dispatch_int(recv, id, va);
  if (phigros_owns_class(id->cls)) return (juint)phigros_dispatch_int(recv, id, va);
  if (unity_owns_class(id->cls)) return unity_dispatch_int(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_int(recv, id, va);
  if (is_t2b(id->cls)) return t2b_int(id, va);
  if (is_mov(id->cls)) return mov_int(id, va);
  return act_int(id, va);
}
static float dispatch_float(void *recv, const FakeID *id, va_list va) {
  jni_approx_arm();
  if (unity_is_boxed(recv)) return unity_boxed_float(recv);   /* Float.floatValue */
  if (input_owns_recv(recv)) return input_dispatch_float(recv, id, va);
  if (input_owns_class(id->cls)) return input_dispatch_float(recv, id, va);
  return act_float(id, va);
}
static void dispatch_void(void *recv, const FakeID *id, va_list va) {
  jni_approx_arm();
  log_app_upcall(id);
  if (name_has(id->cls, "FMODAudioDevice")) {
    debugPrintf("[fmod] FMODAudioDevice.%s() CALLED\n", id->name);
    /* Path A (driving fmodProcess) is blocked: the mixer's source buffer is
     * allocated only by the AudioTrack output start sequence the Java run() loop
     * drives, which never runs here -- so fmodProcess always copies from a null
     * source (Data Abort at +0x28), confirmed even after a 120-frame warmup.
     * Pump left in the tree but disabled; audio is moving to FMOD OutputOpenSL
     * (Path B), which FMOD drives natively via opensles.c. */
    if (0 && !strcmp(id->name, "start")) fmod_audio_start();
  }
  if (phigros_owns_class(id->cls)) { phigros_dispatch_void(recv, id, va); return; }
  if (unity_owns_class(id->cls)) { unity_dispatch_void(recv, id, va); return; }
  if (is_mov(id->cls)) { mov_void(id, va); return; }
  act_void(id, va);
}

// ---------------------------------------------------------------------------
// JNIEnv function implementations
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }
static void *j_FindClass(void *env, const char *name) {
  (void)env;
  return intern_class(name ? name : "?");
}
static void *j_GetObjectClass(void *env, void *obj) {
  (void)env;
  if (obj && nx_tag_of(obj) == BITMAP_TAG) return intern_class("android/graphics/Bitmap");
  return intern_class("java/lang/Object");
}
static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}
static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env; return get_id(class_name_of(cls), name ? name : "", sig ? sig : "");
}

/* String(byte[][,charset]) constructor: Unity builds PlayerPrefs keys as
 * bytes -> new String(bytes, charset) -> Uri.encode(...). Returning a
 * content-less object made every encoded key empty, so all such prefs collided
 * under "" (corrupted Screenmanager resolution prefs -> bad resolution ->
 * crash). Decode the byte array (UTF-8) into a real FakeString. Other ctors are
 * unaffected (still a labelled object). */
static void *new_object_dispatch(void *cls, void *mid, void *first_arg) {
  const char *cn = class_name_of(cls);
  if (cn && strstr(cn, "java/lang/String")) {
    FakeID *m = mid;
    if (m && strstr(m->sig, "[B")) {              /* String([B...) */
      int len = 0; char *b = jni_bytearray_data(first_arg, &len);
      if (b && len > 0) { char *t = malloc(len + 1); memcpy(t, b, len); t[len] = 0;
        void *s = jni_make_string(t); free(t); return s; }
      return jni_make_string("");
    }
  }
  return jni_make_object(cn);
}

static void *j_NewObject(void *env, void *cls, void *mid, ...) {
  (void)env;
  va_list va; va_start(va, mid); void *a0 = va_arg(va, void *); va_end(va);
  return new_object_dispatch(cls, mid, a0);
}
static void *j_NewObjectV(void *env, void *cls, void *mid, va_list va) {
  (void)env; void *a0 = va_arg(va, void *);
  return new_object_dispatch(cls, mid, a0);
}

static void *j_NewGlobalRef(void *env, void *obj) {
  (void)env;
  mutexLock(&locals_lock);
  for (int i = locals_top - 1; i >= 0; i--)
    if (locals[i] == obj) { locals[i] = locals[--locals_top]; break; }
  mutexUnlock(&locals_lock);
  return obj;
}
static void j_DeleteGlobalRef(void *env, void *obj) { (void)env; free_ref(obj); }
static void j_DeleteLocalRef(void *env, void *obj) { (void)env; delete_local(obj); }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_IsSameObject(void *env, void *a, void *b) { (void)env; return a == b; }

/* IsInstanceOf (slot 32). The unimpl stub returned 0 (false), trapping the game
 * in a per-frame retry loop on the black screen: it does obj=jniCall(); if
 * (IsInstanceOf(obj, Expected)) proceed; else retry. We can't track the runtime
 * type of opaque fake jobjects, so answer optimistically: per the JNI spec a
 * NULL object is an instance of any class, and for our fake objects assuming the
 * cast succeeds lets the game move forward instead of spinning. Logged (capped)
 * so we can see which class it is keying on. */
static juint j_IsInstanceOf(void *env, void *obj, void *clazz) {
  (void)env;
  const char *cn = class_name_of(clazz);
  /* nativeInjectEvent classifies the event by instanceof KeyEvent / MotionEvent
   * and picks its handler accordingly. If we blindly return 1, the KeyEvent
   * check (which it does first) matches our touch event and it gets read as a
   * key (getKeyCode) and dropped. Answer by the handle's real kind. */
  if (input_owns_recv(obj)) {
    if (strstr(cn, "MotionEvent")) return input_recv_is_motion(obj) ? 1 : 0;
    if (strstr(cn, "KeyEvent"))    return input_recv_is_motion(obj) ? 0 : 1;
    /* InputEvent base class, or class names collapsed by pool overflow: both
     * kinds are InputEvents, so 1 is safe for the base; overflow is now logged. */
    return 1;
  }
  /* Boxed PlayerPrefs values from getAll(): Unity reads each value with
   * IsInstanceOf(value, Integer/Long/Float/Boolean/String) then unboxes. These
   * MUST be exact or every value is misread as the first type checked. */
  int ui = unity_isinstance(obj, cn);
  if (ui >= 0) return (juint)ui;
  if (obj && nx_tag_of(obj) == TAG_STRING) {
    if (strstr(cn, "String")) return 1;
    if (strstr(cn, "Integer") || strstr(cn, "Long") || strstr(cn, "Float") ||
        strstr(cn, "Double")  || strstr(cn, "Boolean") || strstr(cn, "Character") ||
        strstr(cn, "Short")   || strstr(cn, "Byte"))
      return 0;
    /* other classes: fall through to the optimistic answer below */
  }
  static int logn = 0;
  if (logn < 16) { logn++;
    debugPrintf("JNI: IsInstanceOf(obj=%p, clazz=%s) -> 1\n", obj, cn); }
  return 1;
}
static juint j_EnsureLocalCapacity(void *env, int cap) { (void)env; (void)cap; return 0; }

static juint j_PushLocalFrame(void *env, int cap) {
  (void)env; (void)cap;
  mutexLock(&locals_lock);
  if (frame_top < MAX_FRAMES)
    frames[frame_top++] = locals_top;
  mutexUnlock(&locals_lock);
  return 0;
}
static void *j_PopLocalFrame(void *env, void *result) {
  (void)env;
  mutexLock(&locals_lock);
  const int mark = frame_top > 0 ? frames[--frame_top] : 0;
  for (int i = mark; i < locals_top; i++)
    if (locals[i] != result)
      free_ref(locals[i]);
  locals_top = mark;
  if (result && locals_top < MAX_LOCALS)
    locals[locals_top++] = result;
  mutexUnlock(&locals_lock);
  return result;
}

// --- Call<type>Method (instance + static share class-aware dispatch) --------

#define CALL_VARIADIC(fn, ret_t, dispatch) \
  static ret_t fn(void *env, void *recv, FakeID *id, ...) { \
    (void)env; va_list va; va_start(va, id); \
    ret_t r = dispatch(recv, id, va); va_end(va); return r; } \
  static ret_t fn##V(void *env, void *recv, FakeID *id, va_list va) { \
    (void)env; return dispatch(recv, id, va); }

static uint64_t dispatch_long(void *recv, const FakeID *id, va_list va) {
  if (name_has(id->name, "nanoTime")) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
  }
  if (name_has(id->name, "currentTimeMillis")) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
  }
  if (!unity_is_boxed(recv) && name_has(id->name, "longValue")) return g_frame_ns;
  return (uint64_t)dispatch_int(recv, id, va);
}
CALL_VARIADIC(j_CallObjectMethod, void *, dispatch_object)
CALL_VARIADIC(j_CallIntMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallBooleanMethod, juint, dispatch_int)
CALL_VARIADIC(j_CallLongMethod, uint64_t, dispatch_long)
CALL_VARIADIC(j_CallFloatMethod, float, dispatch_float)

static void j_CallVoidMethod(void *env, void *recv, FakeID *id, ...) {
  (void)env; va_list va; va_start(va, id); dispatch_void(recv, id, va); va_end(va);
}
static void j_CallVoidMethodV(void *env, void *recv, FakeID *id, va_list va) {
  (void)env; dispatch_void(recv, id, va);
}
/* CallNonvirtual<Object>Method{,V,A}: (env, obj, clazz, methodID, args) -- like the
 * virtual object call but with an extra clazz arg our dispatch ignores. Slot 65
 * (V form) is called on the PlayAssetDelivery/package path and was UNIMPL -> null. */
static void *j_CallNonvirtualObjectMethodV(void *env, void *recv, void *clazz, FakeID *id, va_list va) {
  (void)env; (void)clazz; return dispatch_object(recv, id, va);
}
static void *j_CallNonvirtualObjectMethod(void *env, void *recv, void *clazz, FakeID *id, ...) {
  (void)env; (void)clazz; va_list va; va_start(va, id);
  void *r = dispatch_object(recv, id, va); va_end(va); return r;
}
static void *j_CallNonvirtualObjectMethodA(void *env, void *recv, void *clazz, FakeID *id, const void *a) {
  (void)a; return j_CallNonvirtualObjectMethod(env, recv, clazz, id);
}

#define j_CallStaticObjectMethod   j_CallObjectMethod
#define j_CallStaticObjectMethodV  j_CallObjectMethodV
#define j_CallStaticIntMethod      j_CallIntMethod
#define j_CallStaticIntMethodV     j_CallIntMethodV
#define j_CallStaticBooleanMethod  j_CallBooleanMethod
#define j_CallStaticBooleanMethodV j_CallBooleanMethodV
#define j_CallStaticLongMethod     j_CallLongMethod
#define j_CallStaticLongMethodV    j_CallLongMethodV
#define j_CallStaticFloatMethod    j_CallFloatMethod
#define j_CallStaticFloatMethodV   j_CallFloatMethodV
#define j_CallStaticVoidMethod     j_CallVoidMethod
#define j_CallStaticVoidMethodV    j_CallVoidMethodV

// --- Call<type>MethodA / NewObjectA (jvalue[] args) -------------------------
/* Round 132 -- the audit's most serious finding.
 *
 * These wrappers used to drop the jvalue[] and call the VARIADIC form with no
 * varargs at all. The comment claimed the handlers "ignore positional args".
 * They do not. act_void does
 *      runOnUiThread     -> post_runnable(va_arg(va, void *))
 *      postFrameCallback -> g_frame_cb = va_arg(va, void *)
 * and the vsync pump CALLS g_frame_cb. act_int does the same for Handler.post,
 * and several handlers run first_string_arg() over the va_list. With no varargs
 * supplied, va_arg reads x3..x7 -- whatever the caller happened to leave there.
 * That is a garbage pointer being posted to a run queue and later invoked: the
 * "non-object value reaching a pointer operation" family, with the pointer
 * getting *called* rather than dereferenced.
 *
 * (It did not fire in the r131 log -- postFrameCallback arrived through the
 * varargs entry with a sane cb -- but nothing stops it arriving via "A".)
 *
 * Fix: count the declared arguments and forward exactly that many real varargs
 * from the jvalue array. Sound for object/int/long/boolean/byte/char/short:
 * a jvalue is 8 bytes, varargs slots are 8-byte aligned, and va_arg(va,int)
 * reads the low half on little-endian -- which is where jvalue.i lives.
 *
 * NOT sound for float/double arguments: a jvalue holds an unpromoted 4-byte
 * float, while varargs promote to double. When the signature has one, forward
 * ZERO args instead -- the existing explicit jvalue reads below already cover
 * the cases that matter, and zeros are recoverable where garbage is not.
 * Zeros are also what we fall back to for >6 args or a NULL array, so va_arg
 * can never again read an uninitialised register. */
#define JVA_N()                                                               \
  int jva_fp = 0;                                                             \
  int jva_n = a ? sig_arg_count(id->sig, &jva_fp) : 0;                        \
  const uint64_t *jv = (const uint64_t *)a;                                   \
  if (jva_fp || jva_n > 6) jva_n = 0;                                         \
  const uint64_t z = 0

#define JVA_DISPATCH(DO, FN, E, R, ID)                                        \
  switch (jva_n) {                                                            \
    case 1:  DO FN(E, R, ID, jv[0]); break;                                   \
    case 2:  DO FN(E, R, ID, jv[0], jv[1]); break;                            \
    case 3:  DO FN(E, R, ID, jv[0], jv[1], jv[2]); break;                     \
    case 4:  DO FN(E, R, ID, jv[0], jv[1], jv[2], jv[3]); break;              \
    case 5:  DO FN(E, R, ID, jv[0], jv[1], jv[2], jv[3], jv[4]); break;       \
    case 6:  DO FN(E, R, ID, jv[0], jv[1], jv[2], jv[3], jv[4], jv[5]); break;\
    default: DO FN(E, R, ID, z, z, z, z, z, z); break;                        \
  }

// SWIG bindings and AndroidJavaObject.CallStatic<T>()/Call<T>() marshal their
// arguments into a jvalue[] array and invoke the "A" variants. A va_list cannot
// be reconstructed from jvalue[] portably, so we forward to the variadic form
// with no varargs: the dispatch keys off the resolved method name and the
// object/value getters ignore positional args (defaulting to a non-null handle
// of the right class), which is what these init paths need. Previously these
// slots fell through to the unimplemented stub and returned 0/null, hanging the
// first scene (e.g. UNIMPL slot 116 == CallStaticObjectMethodA).
static void *j_CallObjectMethodA (void *e, void *r, FakeID *id, const void *a){
  // getProperty()'s String key lives in jvalue[0] and does NOT survive the
  // va_list-less forward below, so pull it directly. FMOD's OpenSL output reads
  // PROPERTY_OUTPUT_FRAMES_PER_BUFFER through this "A" path; without the key it
  // got "" -> framesPerBuffer 0 -> FMOD error 60.
  if (a && name_has(id->name, "getProperty"))
    return getproperty_value(jni_string_utf(((void *const *)a)[0]));
  if (a && name_has(id->cls, "net/Uri") && (name_has(id->name, "encode") || name_has(id->name, "decode")))
    return (void *)((void *const *)a)[0];        /* identity, jvalue path */

  /* Unity ReflectionHelper member resolution arrives HERE, not through the
   * varargs entry: AndroidJNISafe.CallStaticObjectMethod takes a
   * Span<jvalue>. Read the arguments from the array while we still have it.
   *   getFieldID / getMethodID (Class, String name, String sig, boolean)
   *   getConstructorID         (Class, String sig)
   * Note the different arities -- assuming three object args for all three
   * is what produced the round-16 data abort. */
  if (a && (name_has(id->name, "getFieldID") || name_has(id->name, "getMethodID") ||
            name_has(id->name, "getConstructorID"))) {
    void *const *jv = (void *const *)a;
    int is_ctor = name_has(id->name, "getConstructorID");
    const char *cn = safe_class_name(jv[0]);
    const char *s1 = safe_utf(jv[1]);
    const char *s2 = is_ctor ? "" : safe_utf(jv[2]);
    const char *mn = is_ctor ? "<init>" : s1;
    const char *ms = is_ctor ? s1 : s2;
    static int nlog = 0;
    if (nlog < 12) { nlog++;
      debugPrintf("[jni] %s [A] -> id %s.%s%s\n", id->name,
                  (cn && *cn) ? cn : "?", (mn && *mn) ? mn : "?", ms); }
    return get_id((cn && *cn) ? cn : "java/lang/Object",
                  (mn && *mn) ? mn : "?", ms ? ms : "");
  }

  { JVA_N(); JVA_DISPATCH(return, j_CallObjectMethod, e, r, id); }
}
static juint j_CallBooleanMethodA(void *e, void *r, FakeID *id, const void *a){
  JVA_N(); JVA_DISPATCH(return, j_CallBooleanMethod, e, r, id);
}
static juint j_CallIntMethodA    (void *e, void *r, FakeID *id, const void *a){
  // parseInt/parseLong via the jvalue[] path: read the String from jvalue[0].
  if (a && (name_has(id->name, "parseInt") || name_has(id->name, "parseLong"))) {
    const char *s = jni_string_utf(((void *const *)a)[0]);
    juint v = (juint)(s ? strtol(s, NULL, 10) : 0);
    debugPrintf("[jni] %s(\"%s\") -> %u [A]\n", id->name, s ? s : "", v);
    return v;
  }
  /* getPointerId(I)I comes through here; same jvalue-array problem as the float
   * accessors, so hand the index across rather than dropping it. */
  if (a && strstr(id->sig, "(I)")) input_set_a_index((int)((const int32_t *)a)[0]);
  { JVA_N(); JVA_DISPATCH(return, j_CallIntMethod, e, r, id); }
}
static uint64_t j_CallLongMethodA(void *e, void *r, FakeID *id, const void *a){
  JVA_N(); JVA_DISPATCH(return, j_CallLongMethod, e, r, id);
}
static float j_CallFloatMethodA  (void *e, void *r, FakeID *id, const void *a){
  /* MotionEvent.getX(I)F / getY(I)F reach us here, and forwarding to the varargs
   * version discards the jvalue array -- so the pointer index was read from an
   * empty va_list, clamped to 0, and every finger reported pointer 0's position.
   * Multi-touch collapsed to one finger; single touch worked by luck.
   * Hand the index across explicitly, exactly as the object path does. */
  if (a && strstr(id->sig, "(I)")) input_set_a_index((int)((const int32_t *)a)[0]);
  { JVA_N(); JVA_DISPATCH(return, j_CallFloatMethod, e, r, id); }
}
static void  j_CallVoidMethodA   (void *e, void *r, FakeID *id, const void *a){
  JVA_N(); JVA_DISPATCH((void), j_CallVoidMethod, e, r, id);
}
static void *j_NewObjectA        (void *e, void *cls, void *mid, const void *a){ (void)e;
  return new_object_dispatch(cls, mid, a ? ((void *const *)a)[0] : NULL); }
#define j_CallStaticObjectMethodA  j_CallObjectMethodA
#define j_CallStaticBooleanMethodA j_CallBooleanMethodA
#define j_CallStaticIntMethodA     j_CallIntMethodA
#define j_CallStaticLongMethodA    j_CallLongMethodA
#define j_CallStaticFloatMethodA   j_CallFloatMethodA
#define j_CallStaticVoidMethodA    j_CallVoidMethodA

// --- strings ----------------------------------------------------------------

static void *j_NewStringUTF(void *env, const char *utf) { (void)env; return jni_make_string(utf); }
static void *j_NewString(void *env, const uint16_t *u, int len) {
  (void)env;
  if (!u || len < 0) return jni_make_string("");
  char *tmp = malloc((size_t)len * 4 + 1);
  int o = 0;
  for (int i = 0; i < len; i++) { // naive UTF-16 -> UTF-8 (BMP)
    const uint32_t c = u[i];
    if (c < 0x80) tmp[o++] = (char)c;
    else if (c < 0x800) { tmp[o++] = 0xC0 | (c >> 6); tmp[o++] = 0x80 | (c & 0x3F); }
    else { tmp[o++] = 0xE0 | (c >> 12); tmp[o++] = 0x80 | ((c >> 6) & 0x3F); tmp[o++] = 0x80 | (c & 0x3F); }
  }
  tmp[o] = 0;
  void *s = jni_make_string(tmp);
  free(tmp);
  return s;
}
static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0; return obj_str(jstr);
}
static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) { (void)env; (void)jstr; (void)utf; }
static juint j_GetStringUTFLength(void *env, void *jstr) { (void)env; return strlen(obj_str(jstr)); }

// GetStringUTFRegion: the engine reads ALL its strings through this (not
// GetStringUTFChars), so it must work. Copies the [start, start+len) region as
// modified UTF-8 into buf. Our strings are ASCII (paths / archive names), where
// UTF-16 char offsets == UTF-8 byte offsets, so a byte copy is exact.
static void j_GetStringUTFRegion(void *env, void *jstr, int start, int len, char *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  memcpy(buf, s + start, (size_t)len);
  buf[len] = '\0';
}
// GetStringRegion: UTF-16 variant; widen ASCII bytes into jchar (uint16) buf.
static void j_GetStringRegion(void *env, void *jstr, int start, int len, uint16_t *buf) {
  (void)env;
  if (!buf) return;
  const char *s = obj_str(jstr);
  const int slen = (int)strlen(s);
  if (start < 0) start = 0;
  if (start > slen) start = slen;
  if (len < 0) len = 0;
  if (start + len > slen) len = slen - start;
  for (int i = 0; i < len; i++) buf[i] = (uint8_t)s[start + i];
}
// GetStringLength must return the UTF-16 code-unit count, not the byte count
// (CJK text is multi-byte in UTF-8); engine code sizes UTF-16 buffers with it.
/* GetStringChars (slot 165): return a NUL-terminated UTF-16 (jchar) buffer.
 * JNI lets us always report "is a copy"; the matching ReleaseStringChars
 * frees it. BMP + surrogate pairs, matching NewString's coverage. */
static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  const char *s = obj_str(jstr);
  juint n = utf16_len(s);
  uint16_t *out = (uint16_t *)malloc(((size_t)n + 1) * sizeof(uint16_t));
  if (!out) { if (is_copy) *is_copy = 1; return NULL; }
  const unsigned char *p = (const unsigned char *)(s ? s : "");
  juint o = 0;
  while (*p && o < n) {
    unsigned char c = *p; uint32_t cp; juint adv;
    if (c < 0x80)      { cp = c;        adv = 1; }
    else if (c < 0xE0) { cp = c & 0x1F; adv = 2; }
    else if (c < 0xF0) { cp = c & 0x0F; adv = 3; }
    else               { cp = c & 0x07; adv = 4; }
    for (juint k = 1; k < adv; k++) { if (!p[k]) { adv = k; break; } cp = (cp << 6) | (p[k] & 0x3F); }
    if (cp >= 0x10000 && o + 1 < n + 1) {
      cp -= 0x10000;
      out[o++] = (uint16_t)(0xD800 + (cp >> 10));
      out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
    } else {
      out[o++] = (uint16_t)cp;
    }
    p += adv;
  }
  out[o] = 0;
  if (is_copy) *is_copy = 1;
  return out;
}

/* ReleaseStringChars (slot 166): free what GetStringChars returned. */
static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  free((void *)chars);
}

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return utf16_len(obj_str(jstr));
}

// --- arrays -----------------------------------------------------------------

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (nx_tag_of(a) == TAG_PRIARR || nx_tag_of(a) == TAG_OBJARR))
    return a->len;
  return 0;
}

static void *new_pri_array(int len, int elem_size) {
  void *data = calloc(len ? len : 1, elem_size);
  return make_pri_array_adopt(data, len, elem_size);
}
static void *j_NewByteArray(void *env, int len) { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++) a->items[i] = init;
  return reg_local(a);
}
/* Public one-liner so other TUs can hand back a real array instead of an opaque
 * handle (round 149, used by Display.getSupportedModes). */
void *jni_new_object_array(int len, void *fill) {
  return j_NewObjectArray(NULL, len, NULL, fill);
}

static void *j_GetObjectArrayElement(void *env, void *arr, int i) {
  (void)env;
  FakeObjArray *a = arr;
  return (a && nx_tag_of(a) == TAG_OBJARR && i >= 0 && i < a->len) ? a->items[i] : NULL;
}
static void j_SetObjectArrayElement(void *env, void *arr, int i, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && nx_tag_of(a) == TAG_OBJARR && i >= 0 && i < a->len) a->items[i] = val;
}

/* Round 145. Never return NULL from here.
 *
 * The r144 crash: libunity called GetByteArrayElements (slot 184, seen as
 * `ldr x8,[x19]; ldr x8,[x8,#0x5c0]; blr x8` at libunity+0x5eb214) with a ref
 * we did not recognise -- the single tag-warn in an 11,526-line log, one line
 * before the fault. We correctly returned NULL. libunity's wrapper did not
 * check it, and its caller then ran
 *
 *     mov w8,w21 ; mov x9,x1 ; ldrb w10,[x19],#1 ; strb w10,[x9],#1
 *
 * copying 124 bytes of a Backtrace submission URL into address 0. far=0,
 * esr=92000045 (a WRITE), dead.
 *
 * NULL is what the JNI spec says for an invalid array, but the caller cannot
 * handle it, so returning it is a guaranteed crash while returning storage is
 * a survivable wrong answer. Hand back a zeroed scratch page instead: the copy
 * lands somewhere real, the game reads zeros, and the ledger says so.
 *
 * This does NOT fix why a bad ref reaches us. 0xfffe241c0 sat inside the newlib
 * heap region with first word 0xfacc1b90 -- a plausible allocation that is not
 * one of ours, i.e. a stale or foreign jobject, not wild garbage. That is still
 * open; this only stops it being fatal. */
static uint8_t g_pri_scratch[8192];

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env; if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  if (a && nx_tag_of(a) == TAG_PRIARR) return a->data;
  /* Shared and not thread-safe on purpose: this is a degradation path, and a
   * racing second caller getting the same zeroed bytes is still better than a
   * store to page 0. */
  memset(g_pri_scratch, 0, sizeof g_pri_scratch);
  jni_note_approx("ARR-SCRATCH", "?", "GetArrayElements", "(unrecognised array ref)");
  { static int nlog = 0;
    if (nlog < 8) { nlog++;
      debugPrintf("[jni] GetArrayElements on an unrecognised ref %p -> %u-byte "
                  "zeroed scratch instead of NULL (the caller does not "
                  "null-check and would copy into address 0)\n",
                  arr, (unsigned)sizeof g_pri_scratch); } }
  return g_pri_scratch;
}
static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}
static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && nx_tag_of(a) == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + (size_t)start * a->elem_size, (size_t)len * a->elem_size);
}
static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && nx_tag_of(a) == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + (size_t)start * a->elem_size, buf, (size_t)len * a->elem_size);
}

// --- fields -----------------------------------------------------------------
// The engine and the game DO read Java fields: android.os.Build.* (device id),
// Build.VERSION.SDK_INT (API gating), PackageInfo.versionName/versionCode (the
// Application.version the boot path logs as blank today), DisplayMetrics.*, and
// Configuration.*. Returning null/0 universally (the old stub) blanks the app
// version -- which can throw in version-parsing boot code -- and zeroes display
// metrics. Route every field read through a name-based dispatcher. fid is the
// FakeID GetFieldID handed back, so cls/name/sig are all available.
//
// Placeholders marked CHECK are safe defaults, not the shipped values; the game
// never verifies them against anything but a stubbed RemoteConfig, so exact
// numbers don't gate boot. Easy to correct once known.
/* PHIGROS. Both are PLACEHOLDERS -- read the real values off your own APK:
 *     aapt dump badging Phigros.apk | head -1
 * Nothing in the boot path verifies them, but Phigros surfaces versionName in
 * its own settings UI and any SDK that keys off it will see this string, so
 * shipping Zookeeper's (which the upstream tree still had here) is wrong. */
#define APP_VERSION_NAME "3.0.0"   /* CHECK YOUR MANIFEST */
#define APP_VERSION_CODE 1         /* CHECK YOUR MANIFEST */
#define NX_SDK_INT       33        /* Android 13 -- high enough to pass any minSdk gate  */

static int fld_is(const FakeID *id, const char *cls_sub, const char *name) {
  return name_has(id->cls, cls_sub) && !strcmp(id->name, name);
}

// One-line-per-unique-field diagnostic: tells the next run exactly which Java
// fields the game reads (and lets us confirm versionName/currentActivity/etc.
// are being exercised). Dedup by interned name pointer, like log_app_upcall.
static void log_field_read(const FakeID *id, char kind) {
  static const void *seen[128]; static int seen_n = 0;
  for (int i = 0; i < seen_n; i++) if (seen[i] == id->name) return;
  if (seen_n < 128) seen[seen_n++] = id->name;
  debugPrintf("[jni] field(%c): %s.%s %s\n", kind, id->cls, id->name, id->sig);
}

static void *field_object(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  { const int sl = field_slot(id);
    if (sl >= 0 && g_field_kind[sl] == FLD_OBJ) return (void *)(uintptr_t)g_field_w[sl]; }
  /* software-keyboard result: the engine reads it as a String field */
  if (n && (!strcmp(n, "text") || !strcmp(n, "mText") ||
            !strcmp(n, "inputText") || !strcmp(n, "m_Text") ||
            name_has(n, "KeyboardText") || name_has(n, "EditBoxText"))) {
    const char *t = editbox_text();
    debugPrintf("[kbd] text FIELD %s.%s -> \"%s\"\n", c ? c : "?", n, t ? t : "");
    return jni_make_string(t ? t : "");
  }
  // PackageInfo / ApplicationInfo version string
  if (!strcmp(n, "versionName")) return jni_make_string(APP_VERSION_NAME);
  // UnityPlayer statics: currentActivity is THE Activity -- null here NPEs every
  // UnityPlayer.currentActivity.getXxx() in managed code, so hand back a live
  // (opaque) Activity that our method dispatch then services.
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "currentActivity")) return jni_make_object("android/app/Activity");
    if (!strcmp(n, "MANUFACTURER"))    return jni_make_string("Nintendo");
  }
  // AudioManager.PROPERTY_OUTPUT_* are static String field keys the engine reads
  // just before AudioManager.getProperty(key) to size FMOD's audio path. Return
  // the real Android property-name strings AND record which one was read
  // (g_last_output_prop) so getProperty() can answer even when the key argument
  // is lost on the JNI call path (see getproperty_value). These are AudioManager
  // fields, NOT UnityPlayer -- gating them on the wrong class meant they never
  // matched, getProperty saw "", and FMOD got framesPerBuffer 0 -> error 60.
  if (name_has(c, "media/AudioManager")) {
    if (!strcmp(n, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER")) { g_last_output_prop = 2; return jni_make_string("android.media.property.OUTPUT_FRAMES_PER_BUFFER"); }
    if (!strcmp(n, "PROPERTY_OUTPUT_SAMPLE_RATE"))       { g_last_output_prop = 1; return jni_make_string("android.media.property.OUTPUT_SAMPLE_RATE"); }
  }
  // Context.*_SERVICE name constants -> the strings getSystemService() expects
  if (name_has(c, "content/Context")) {
    if (!strcmp(n, "AUDIO_SERVICE"))        return jni_make_string("audio");
    if (!strcmp(n, "DISPLAY_SERVICE"))      return jni_make_string("display");
    if (!strcmp(n, "WINDOW_SERVICE"))       return jni_make_string("window");
    if (!strcmp(n, "LOCATION_SERVICE"))     return jni_make_string("location");
    if (!strcmp(n, "CONNECTIVITY_SERVICE")) return jni_make_string("connectivity");
    if (!strcmp(n, "MEDIA_ROUTER_SERVICE")) return jni_make_string("media_router");
    if (!strcmp(n, "VIBRATOR_SERVICE"))     return jni_make_string("vibrator");
  }
  // Environment.MEDIA_MOUNTED MUST equal getExternalStorageState()'s return
  // ("mounted", set in act_object) or the storage check fails and save data is
  // disabled. Keep both in lockstep.
  if (name_has(c, "os/Environment")) {
    if (!strcmp(n, "MEDIA_MOUNTED"))           return jni_make_string("mounted");
    if (!strcmp(n, "MEDIA_MOUNTED_READ_ONLY")) return jni_make_string("mounted_ro");
  }
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "FEATURE_AUDIO_LOW_LATENCY")) return jni_make_string("android.hardware.audio.low_latency");
    if (!strcmp(n, "FEATURE_AUDIO_PRO"))         return jni_make_string("android.hardware.audio.pro");
  }
  // android.os.Build identity strings (all public static final String)
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "MODEL"))        return jni_make_string("Switch");
    if (!strcmp(n, "MANUFACTURER")) return jni_make_string("Nintendo");
    if (!strcmp(n, "BRAND"))        return jni_make_string("Nintendo");
    if (!strcmp(n, "DEVICE"))       return jni_make_string("Switch");
    if (!strcmp(n, "PRODUCT"))      return jni_make_string("Switch");
    if (!strcmp(n, "HARDWARE"))     return jni_make_string("nx");
    if (!strcmp(n, "BOARD"))        return jni_make_string("nx");
    if (!strcmp(n, "DISPLAY"))      return jni_make_string("nx");
    if (!strcmp(n, "ID"))           return jni_make_string("REL");
    if (!strcmp(n, "TYPE"))         return jni_make_string("user");
    if (!strcmp(n, "TAGS"))         return jni_make_string("release-keys");
    if (!strcmp(n, "FINGERPRINT"))  return jni_make_string("Nintendo/Switch/Switch:13/REL/10007:user/release-keys");
    if (!strcmp(n, "BOOTLOADER"))   return jni_make_string("unknown");
    if (!strcmp(n, "HOST"))         return jni_make_string("localhost");
    if (!strcmp(n, "USER"))         return jni_make_string("nx");
    if (!strcmp(n, "SERIAL"))       return jni_make_string("unknown");
    if (!strcmp(n, "RELEASE"))      return jni_make_string("13");        /* Build.VERSION.* */
    if (!strcmp(n, "CODENAME"))     return jni_make_string("REL");
    if (!strcmp(n, "INCREMENTAL"))  return jni_make_string("10007");
    if (!strcmp(n, "SECURITY_PATCH")) return jni_make_string("2023-01-01");
    if (!strcmp(n, "BASE_OS"))      return jni_make_string("");
  }
  // Any other String-typed field -> "" (non-null avoids NPEs in string ops).
  if (sig_returns(id->sig, "Ljava/lang/String;")) return jni_make_string("");
  // Any other object field stays null; array fields handled by the caller.
  return NULL;
}

static juint field_int(const FakeID *id) {
  const char *n = id->name, *c = id->cls;
  { const int sl = field_slot(id);      /* a write wins over the default */
    if (sl >= 0 && g_field_kind[sl] == FLD_WORD) return (juint)g_field_w[sl]; }
  /* Phigros: NativeAudio$DeviceAudioInformation. Consulted after the write
   * override (so a Set*Field still wins) but before the generic table. */
  { uint64_t pv; if (phigros_field_int(id, &pv)) return (juint)pv; }
  if (!strcmp(n, "what") && name_has(c, "Message")) return (juint)g_msg_what;
  if (!strcmp(n, "versionCode")) return APP_VERSION_CODE;
  // UnityPlayer integer statics
  if (name_has(c, "unity3d/player/UnityPlayer")) {
    if (!strcmp(n, "SDK_INT"))     return NX_SDK_INT;
    if (!strcmp(n, "densityDpi"))  return 320;
    if (!strcmp(n, "widthPixels")) return screen_width;   /* real panel (landscape) */
    if (!strcmp(n, "heightPixels"))return screen_height;
    if (!strcmp(n, "STREAM_MUSIC"))return 3;   /* AudioManager.STREAM_MUSIC      */
    if (!strcmp(n, "GET_DEVICES_OUTPUTS")) return 2; /* AudioManager.GET_DEVICES_OUTPUTS */
    if (!strcmp(n, "ROUTE_TYPE_LIVE_VIDEO")) return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_UNSPECIFIED"))       return -1;
    if (!strcmp(n, "SCREEN_ORIENTATION_LANDSCAPE"))         return 0;
    if (!strcmp(n, "SCREEN_ORIENTATION_PORTRAIT"))          return 1;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_LANDSCAPE")) return 8;
    if (!strcmp(n, "SCREEN_ORIENTATION_REVERSE_PORTRAIT"))  return 9;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_USER"))         return 13;
    if (!strcmp(n, "SCREEN_ORIENTATION_FULL_SENSOR"))       return 10;
  }
  if (name_has(c, "content/Context") && !strcmp(n, "MODE_PRIVATE")) return 0;
  if (name_has(c, "pm/PackageManager")) {
    if (!strcmp(n, "PERMISSION_GRANTED")) return 0;   /* == granted              */
    if (!strcmp(n, "PERMISSION_DENIED"))  return (juint)-1;
  }
  if (name_has(c, "os/Build")) {
    if (!strcmp(n, "SDK_INT"))          return NX_SDK_INT;
    if (!strcmp(n, "PREVIEW_SDK_INT"))  return 0;
  }
  if (name_has(c, "Configuration") && !strcmp(n, "orientation")) return 2;  /* LANDSCAPE */
  // DisplayMetrics integer fields (width/height/dpi)
  if (name_has(c, "DisplayMetrics")) {
    if (!strcmp(n, "widthPixels"))  return screen_width;   /* real panel (landscape) */
    if (!strcmp(n, "heightPixels")) return screen_height;
    if (!strcmp(n, "densityDpi"))   return 320;    /* xhdpi bucket                */
  }
  return 0;
}

/* DisplayMetrics.density / xdpi / ydpi / scaledDensity are float fields. 0 would
 * make dp->px scaling collapse, so hand back a sane xhdpi density (2.0). */
static float field_float(const FakeID *id) {
  { const int sl = field_slot(id);
    if (sl >= 0 && g_field_kind[sl] == FLD_FLT) {
      float f; uint32_t bits = (uint32_t)g_field_w[sl]; memcpy(&f, &bits, 4); return f; }
    if (sl >= 0 && g_field_kind[sl] == FLD_WORD) return (float)(int64_t)g_field_w[sl]; }
  const char *n = id->name;
  if (name_has(id->cls, "DisplayMetrics")) {
    if (!strcmp(n, "density") || !strcmp(n, "scaledDensity")) return 2.0f;
    if (!strcmp(n, "xdpi") || !strcmp(n, "ydpi"))             return 320.0f;
  }
  (void)fld_is;
  return 0.0f;
}

static void *j_GetObjectField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return NULL;
  log_field_read((const FakeID *)fid, 'O');
  { const FakeID *f = (const FakeID *)fid;   /* name the field the game really reads */
    if (editbox_text() && editbox_text()[0]) {
      static unsigned seen;
      if (seen < 16) { seen++;
        debugPrintf("[kbd] objfield read: %s.%s sig=%s\n",
                    f->cls ? f->cls : "?", f->name ? f->name : "?",
                    f->sig ? f->sig : "?"); } } }
  return field_object((const FakeID *)fid); }
static juint j_GetIntField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0;
  log_field_read((const FakeID *)fid, 'I');
  return field_int((const FakeID *)fid); }
static juint j_GetLongField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return (juint)field_int((const FakeID *)fid); }
static juint j_GetBooleanField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0; return field_int((const FakeID *)fid) ? 1 : 0; }
static float j_GetFloatField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0f; return field_float((const FakeID *)fid); }
static double j_GetDoubleField(void *env, void *obj, void *fid) {
  (void)env; (void)obj; if (!fid) return 0.0; return (double)field_float((const FakeID *)fid); }

/* Field writes -- round 132. One entry point per width because the JNI table
 * has one slot per width; they all land in the same word. A write to a FieldID
 * we never issued is dropped rather than indexed (field_slot bounds-checks and
 * tag-checks), because a foreign pointer here is exactly how this port has been
 * burned before. */
static void j_SetWordField(void *env, void *obj, void *fid, uint64_t v) {
  (void)env; (void)obj;
  const int sl = field_slot(fid);
  if (sl < 0) { debugPrintf("[jni] Set*Field on a foreign fieldID %p -- dropped\n", fid); return; }
  g_field_w[sl] = v; g_field_kind[sl] = FLD_WORD;
}
static void j_SetObjectField(void *env, void *obj, void *fid, void *v) {
  (void)env; (void)obj;
  const int sl = field_slot(fid);
  if (sl < 0) { debugPrintf("[jni] SetObjectField on a foreign fieldID %p -- dropped\n", fid); return; }
  g_field_w[sl] = (uint64_t)(uintptr_t)v; g_field_kind[sl] = FLD_OBJ;
}
static void j_SetFloatField(void *env, void *obj, void *fid, float v) {
  (void)env; (void)obj;
  const int sl = field_slot(fid);
  if (sl < 0) return;
  { float f = v; uint32_t bits; memcpy(&bits, &f, 4);
    g_field_w[sl] = bits; g_field_kind[sl] = FLD_FLT; }
}
static void j_SetDoubleField(void *env, void *obj, void *fid, double v) {
  j_SetFloatField(env, obj, fid, (float)v);
}

// --- reflection bridge (proxy support) --------------------------------------
// Unity's AndroidJavaProxy / JNIBridge.newInterfaceProxy converts the reflected
// Method/Field objects of an interface into jmethod/jfieldIDs via these. Slot 7
// (FromReflectedMethod) and slot 8 (FromReflectedField) were unimplemented, so
// the proxy couldn't bind its methods (the "UNIMPL slot 7" lines). We don't
// carry real reflection, but returning a non-null opaque ID lets the proxy set
// up and be stored; if such a proxy callback is ever actually invoked it routes
// through act_* and no-ops, which is the right behaviour for our stubbed events.
static void *j_FromReflectedMethod(void *env, void *m) {
  (void)env;
  if (m && nx_tag_of(m) == TAG_ID) return m;   /* proxy_run passes the real run() id */
  return get_id("java/lang/reflect/Method", "invoke", "()V"); }
static void *j_FromReflectedField(void *env, void *f) {
  (void)env; (void)f; return get_id("java/lang/reflect/Field", "field", "()V"); }
static void *j_ToReflectedMethod(void *env, void *cls, void *mid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return mid ? mid : jni_make_object("java/lang/reflect/Method"); }
static void *j_ToReflectedField(void *env, void *cls, void *fid, juint isStatic) {
  (void)env; (void)cls; (void)isStatic; return fid ? fid : jni_make_object("java/lang/reflect/Field"); }

// --- misc -------------------------------------------------------------------

/* JNI registers org/fmod/FMODAudioDevice's native bridge (fmodGetInfo /
 * fmodProcess / fmodProcessMicData) here -- these are file-local in libunity, so
 * RegisterNatives is the only place their addresses are exposed. Capture them so
 * a native playback thread can pull PCM from FMOD (the Java run() loop never runs
 * because we have no JVM). */
typedef struct { const char *name; const char *sig; void *fn; } JNINativeMethod_;
void *g_fmod_getinfo = 0, *g_fmod_process = 0, *g_fmod_micdata = 0;
static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env;
  const char *cn = class_name_of(cls);
  const JNINativeMethod_ *m = methods;
  int is_fmod = name_has(cn, "fmod") || name_has(cn, "FMOD");
  debugPrintf("[jni] RegisterNatives %s (%d methods)%s\n", cn, n, is_fmod ? "  <-- fmod" : "");
  if (m && name_has(cn, "unity3d/player/UnityPlayer")) {
    for (int i = 0; i < n; i++) {
      if (!m[i].name) continue;
      if (!strcmp(m[i].name, "nativeSetInputString"))    g_u_setInputString  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetInputSelection")) g_u_setInputSel = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputClosed"))   g_u_softClosed  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSoftInputCanceled")) g_u_softCancel  = m[i].fn;
      else if (!strcmp(m[i].name, "nativeSetKeyboardIsVisible")) g_u_kbdVisible = m[i].fn;
    }
    debugPrintf("[kbd] captured Unity soft-input natives: str=%p sel=%p closed=%p cancel=%p vis=%p\n",
                g_u_setInputString, g_u_setInputSel, g_u_softClosed,
                g_u_softCancel, g_u_kbdVisible);
  }
  if (m) {   /* dump the whole table once per class: the keyboard callback is in here */
    static unsigned dumped;
    for (int i = 0; i < n && dumped < 200; i++, dumped++)
      debugPrintf("[natives] %s.%s %s -> %p\n", cn,
                  m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", m[i].fn);
  }
  if ((name_has(cn, "jnibridge") || name_has(cn, "JNIBridge")) && m) {
    for (int i = 0; i < n; i++) if (m[i].name && name_has(m[i].name, "invoke")) g_jnibridge_invoke = (jnibridge_invoke_fn)m[i].fn;
    debugPrintf("[jni] captured JNIBridge invoke=%p\n", (void *)g_jnibridge_invoke);
  }
  if (is_fmod && m) {
    for (int i = 0; i < n; i++) {
      debugPrintf("[jni]   %s %s -> %p\n",
                  m[i].name ? m[i].name : "?", m[i].sig ? m[i].sig : "?", m[i].fn);
      if (!m[i].name) continue;
      if      (!strcmp(m[i].name, "fmodGetInfo"))        g_fmod_getinfo = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcess"))        g_fmod_process = m[i].fn;
      else if (!strcmp(m[i].name, "fmodProcessMicData")) g_fmod_micdata = m[i].fn;
    }
    debugPrintf("[fmod] captured getInfo=%p process=%p micData=%p\n",
                g_fmod_getinfo, g_fmod_process, g_fmod_micdata);
  }
  return 0;
}
static juint j_GetJavaVM(void *env, void **vm) { (void)env; *vm = fake_vm; return JNI_OK; }
/* Always "no exception" -- see the ledger note above act_object. We cannot
 * honestly answer anything else without inventing throws, but we CAN record
 * that Unity asked, which is what makes the ledger's INSPECTED column real. */
static juint j_ExceptionCheck(void *env) { (void)env; jni_approx_checked(); return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_void1(void *env) { (void)env; }

// ---------------------------------------------------------------------------
// FMOD native-audio pump
// ---------------------------------------------------------------------------
// fmodProcess(env, this, ByteBuffer) renders one fixed-size FMOD mixer block
// (size comes from the output singleton set up at start(), NOT from the buffer
// capacity) straight into env->GetDirectBufferAddress(ByteBuffer), then returns
// 0. We have no JVM, so the Java FMODAudioDevice.run() loop never calls it --
// this native thread does instead, and pushes the PCM to the SDL sink.
//
// The only JNIEnv entry fmodProcess uses is GetDirectBufferAddress (slot 230);
// fmodGetInfo(which) uses none. So the shim only has to hand back our staging
// buffer and feed the captured function pointers a non-NULL `this`/buffer token.

#define FMOD_STAGING_BYTES (64 * 1024)   // generous: must exceed one mixer block
static unsigned char g_fmod_staging[FMOD_STAGING_BYTES];
static int  g_fmod_bb_token   = 0;       // stand-in jobject for the ByteBuffer
static int  g_fmod_this_token = 0;       // stand-in jobject for `this`
static int  g_fmod_started    = 0;

typedef int (*fmod_getinfo_fn)(void *env, void *thiz, int which);
typedef int (*fmod_process_fn)(void *env, void *thiz, void *bytebuffer);

// slot 230: every ByteBuffer we ever pass is our own staging buffer.
static void *j_GetDirectBufferAddress(void *env, void *buf) {
  (void)env; (void)buf; return g_fmod_staging;
}
// slot 231 (defensive -- the disasm shows fmodProcess never calls it).
static long j_GetDirectBufferCapacity(void *env, void *buf) {
  (void)env; (void)buf; return (long)FMOD_STAGING_BYTES;
}

// Discover how many bytes fmodProcess actually wrote, once, by sentinel-fill.
// Silence (0x0000) still differs from the 0xCD fill, so a silent first block is
// detected correctly.
static int probe_block_bytes(fmod_process_fn process, int frame_bytes) {
  memset(g_fmod_staging, 0xCD, FMOD_STAGING_BYTES);
  process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
  int last = -1;
  for (int i = FMOD_STAGING_BYTES - 1; i >= 0; i--) {
    if (g_fmod_staging[i] != 0xCD) { last = i; break; }
  }
  if (last < 0) return 0;
  int bytes = last + 1;
  if (frame_bytes > 0)                    // round up to a whole frame
    bytes = ((bytes + frame_bytes - 1) / frame_bytes) * frame_bytes;
  if (bytes > FMOD_STAGING_BYTES) bytes = FMOD_STAGING_BYTES;
  return bytes;
}

static int16_t block_peak(int bytes) {
  const int16_t *s = (const int16_t *)g_fmod_staging;
  int n = bytes / 2; int16_t peak = 0;
  for (int i = 0; i < n; i++) {
    int16_t v = s[i] < 0 ? (int16_t)-s[i] : s[i];
    if (v > peak) peak = v;
  }
  return peak;
}

static void *fmod_audio_thread(void *arg) {
  (void)arg;
  fmod_getinfo_fn getinfo = (fmod_getinfo_fn)g_fmod_getinfo;
  fmod_process_fn process = (fmod_process_fn)g_fmod_process;
  if (!process) { debugPrintf("[fmod] pump: no process ptr, abort\n"); return NULL; }

  int rate = 48000, channels = 2;
  if (getinfo) {
    int r = getinfo(fake_env, &g_fmod_this_token, 0);
    int c = getinfo(fake_env, &g_fmod_this_token, 1);
    debugPrintf("[fmod] getInfo: [0]=%d [1]=%d [2]=%d [3]=%d [4]=%d\n",
                r, c, getinfo(fake_env, &g_fmod_this_token, 2),
                getinfo(fake_env, &g_fmod_this_token, 3),
                getinfo(fake_env, &g_fmod_this_token, 4));
    if (r >= 8000 && r <= 192000) rate = r;
    if (c == 1 || c == 2 || c == 6) channels = c;
  }
  const int frame_bytes = channels * 2; // S16

  // CRITICAL: start() fires before Unity's render loop has driven a single
  // System::update(), so the FMOD mixer's DSP buffers aren't allocated yet --
  // calling fmodProcess now faults (null deref deep in the mix/copy path). Wait
  // for the engine to tick a batch of frames (each drives a System::update that
  // finalizes the mixer) before the first call. A faulting call can't be caught
  // (no working SEH here), so this warmup is the only protection.
  extern uint32_t port_frame_count(void);
  #define FMOD_WARMUP_FRAMES 120u
  uint32_t f0 = port_frame_count();
  debugPrintf("[fmod] warmup: waiting %u frames (start frame=%u)\n", FMOD_WARMUP_FRAMES, f0);
  for (int guard = 0; guard < 1500; guard++) {            // ~15s hard cap
    if (port_frame_count() - f0 >= FMOD_WARMUP_FRAMES) break;
    svcSleepThread(10000000ULL);                          // 10 ms
  }
  debugPrintf("[fmod] warmup done at frame=%u, probing\n", port_frame_count());

  // start() may still be wiring the FMOD output singleton; fmodProcess writes
  // nothing until it's live. Retry the probe briefly before giving up.
  int block = 0;
  for (int tries = 0; tries < 100 && block <= 0; tries++) {
    block = probe_block_bytes(process, frame_bytes);
    if (block <= 0) svcSleepThread(10000000ULL); // 10 ms
  }
  debugPrintf("[fmod] pump start: %d Hz, %d ch, block=%d bytes (%d frames)\n",
              rate, channels, block, block / (frame_bytes ? frame_bytes : 1));
  if (block <= 0) {
    debugPrintf("[fmod] pump: fmodProcess wrote nothing after retries, abort\n");
    return NULL;
  }

  int dev_rate = audio_fmod_open(rate, channels);
  if (!dev_rate) { debugPrintf("[fmod] pump: device open failed, abort\n"); return NULL; }

  // pace to realtime via the device queue; target ~4 blocks buffered.
  const uint32_t hi = (uint32_t)block * 6;
  const uint32_t lo = (uint32_t)block * 3;
  long iters = 0;
  for (;;) {
    while (audio_fmod_queued() > hi)
      svcSleepThread(2000000ULL); // 2 ms
    // refill toward the low watermark
    do {
      process(fake_env, &g_fmod_this_token, &g_fmod_bb_token);
      uint32_t q = audio_fmod_write(g_fmod_staging, block);
      if (iters < 4) {
        debugPrintf("[fmod] block %ld: peak=%d queued=%u\n",
                    iters, (int)block_peak(block), q);
      }
      iters++;
      if (q > hi) break;
    } while (audio_fmod_queued() < lo);
    svcSleepThread(2000000ULL); // 2 ms
  }
  return NULL;
}

// Called from dispatch_void when FMODAudioDevice.start() fires (pointers are
// already captured by then -- RegisterNatives precedes start()).
void fmod_audio_start(void) {
  if (g_fmod_started) return;
  if (!g_fmod_process) { debugPrintf("[fmod] start(): process ptr not captured yet\n"); return; }
  g_fmod_started = 1;
  pthread_t th;
  if (pthread_create(&th, NULL, fmod_audio_thread, NULL) != 0) {
    debugPrintf("[fmod] pthread_create failed\n");
    g_fmod_started = 0;
    return;
  }
  pthread_detach(th);
  debugPrintf("[fmod] native playback thread launched\n");
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
/* ZOOKEEPER DX port: accessors so unity_jni.c/unity_input.c can read into the
 * (otherwise static) FakeString / FakePriArray without duplicating the structs. */
void *jni_bytearray_data(void *arr, int *len_out) {
  FakePriArray *a = arr;
  if (a && nx_tag_of(a) == TAG_PRIARR) { if (len_out) *len_out = a->len; return a->data; }
  if (len_out) *len_out = 0;
  return NULL;
}
const char *jni_string_utf(void *jstr) {
  FakeString *s = jstr;
  return (s && nx_tag_of(s) == TAG_STRING) ? s->utf : "";
}

void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args; if (env) *env = fake_env; return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version; if (env) *env = fake_env; return JNI_OK;
}
static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  mutexInit(&locals_lock);

  jni_fill_unimpl(env_table); // indexed stubs: log the exact unimplemented slot

  env_table[4]   = (void *)j_GetVersion;
  env_table[6]   = (void *)j_FindClass;
  env_table[7]   = (void *)j_FromReflectedMethod;    // was UNIMPL (proxy bind)
  env_table[8]   = (void *)j_FromReflectedField;
  env_table[9]   = (void *)j_ToReflectedMethod;
  env_table[12]  = (void *)j_ToReflectedField;
  env_table[15]  = (void *)j_ExceptionOccurred;
  env_table[16]  = (void *)j_void1; // ExceptionDescribe
  env_table[17]  = (void *)j_void1; // ExceptionClear
  env_table[19]  = (void *)j_PushLocalFrame;
  env_table[20]  = (void *)j_PopLocalFrame;
  env_table[21]  = (void *)j_NewGlobalRef;
  env_table[22]  = (void *)j_DeleteGlobalRef;
  env_table[23]  = (void *)j_DeleteLocalRef;
  env_table[24]  = (void *)j_IsSameObject;
  env_table[25]  = (void *)j_NewLocalRef;
  env_table[26]  = (void *)j_EnsureLocalCapacity;
  env_table[28]  = (void *)j_NewObject;
  env_table[29]  = (void *)j_NewObjectV;
  env_table[31]  = (void *)j_GetObjectClass;
  env_table[32]  = (void *)j_IsInstanceOf;
  env_table[33]  = (void *)j_GetMethodID;
  env_table[34]  = (void *)j_CallObjectMethod;
  env_table[35]  = (void *)j_CallObjectMethodV;
  env_table[37]  = (void *)j_CallBooleanMethod;
  env_table[38]  = (void *)j_CallBooleanMethodV;
  env_table[49]  = (void *)j_CallIntMethod;
  env_table[50]  = (void *)j_CallIntMethodV;
  env_table[52]  = (void *)j_CallLongMethod;
  env_table[53]  = (void *)j_CallLongMethodV;
  env_table[55]  = (void *)j_CallFloatMethod;
  env_table[56]  = (void *)j_CallFloatMethodV;
  env_table[61]  = (void *)j_CallVoidMethod;
  env_table[62]  = (void *)j_CallVoidMethodV;
  // "A" (jvalue[]) variants -- instance
  env_table[30]  = (void *)j_NewObjectA;
  env_table[36]  = (void *)j_CallObjectMethodA;
  env_table[39]  = (void *)j_CallBooleanMethodA;
  env_table[51]  = (void *)j_CallIntMethodA;
  env_table[54]  = (void *)j_CallLongMethodA;
  env_table[57]  = (void *)j_CallFloatMethodA;
  env_table[63]  = (void *)j_CallVoidMethodA;
  env_table[64]  = (void *)j_CallNonvirtualObjectMethod;    // was UNIMPL
  env_table[65]  = (void *)j_CallNonvirtualObjectMethodV;   // was UNIMPL slot 65 (PAD path)
  env_table[66]  = (void *)j_CallNonvirtualObjectMethodA;   // was UNIMPL
  env_table[94]  = (void *)j_GetFieldID;
  env_table[95]  = (void *)j_GetObjectField;
  env_table[96]  = (void *)j_GetBooleanField;        // GetBooleanField
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;           // GetLongField
  env_table[102] = (void *)j_GetFloatField;          // GetFloatField
  env_table[113] = (void *)j_GetMethodID;            // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[132] = (void *)j_CallStaticLongMethod;
  env_table[133] = (void *)j_CallStaticLongMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  // "A" (jvalue[]) variants -- static (SWIG / AndroidJavaObject.CallStatic<T>)
  env_table[116] = (void *)j_CallStaticObjectMethodA;
  env_table[119] = (void *)j_CallStaticBooleanMethodA;
  env_table[131] = (void *)j_CallStaticIntMethodA;
  env_table[134] = (void *)j_CallStaticLongMethodA;
  env_table[137] = (void *)j_CallStaticFloatMethodA;
  env_table[143] = (void *)j_CallStaticVoidMethodA;
  env_table[144] = (void *)j_GetFieldID;             // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;         // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;        // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;            // GetStaticIntField
  env_table[151] = (void *)j_GetLongField;           // GetStaticLongField
  env_table[152] = (void *)j_GetFloatField;          // GetStaticFloatField
  env_table[163] = (void *)j_NewString;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[164] = (void *)j_GetStringLength;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  for (int i = 183; i <= 190; i++) env_table[i] = (void *)j_GetPriArrayElements;
  for (int i = 191; i <= 198; i++) env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++) env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++) env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[219] = (void *)j_GetJavaVM;
  env_table[220] = (void *)j_GetStringRegion;
  env_table[221] = (void *)j_GetStringUTFRegion; // engine reads every string via this
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef;            // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteGlobalRef;         // DeleteWeakGlobalRef
  /* Round 132: widths that were falling to the unimplemented stub. Indices
   * verified against the canonical JNINativeInterface layout, not guessed. */
  env_table[97]  = (void *)j_GetIntField;             // GetByteField   (w0)
  env_table[98]  = (void *)j_GetIntField;             // GetCharField   (w0)
  env_table[99]  = (void *)j_GetIntField;             // GetShortField  (w0)
  env_table[103] = (void *)j_GetDoubleField;          // GetDoubleField (d0!)
  env_table[104] = (void *)j_SetObjectField;
  env_table[105] = (void *)j_SetWordField;            // SetBooleanField
  env_table[106] = (void *)j_SetWordField;            // SetByteField
  env_table[107] = (void *)j_SetWordField;            // SetCharField
  env_table[108] = (void *)j_SetWordField;            // SetShortField
  env_table[109] = (void *)j_SetWordField;            // SetIntField  <- the one r131 hit
  env_table[110] = (void *)j_SetWordField;            // SetLongField
  env_table[111] = (void *)j_SetFloatField;
  env_table[112] = (void *)j_SetDoubleField;
  env_table[147] = (void *)j_GetIntField;             // GetStaticByteField
  env_table[148] = (void *)j_GetIntField;             // GetStaticCharField
  env_table[149] = (void *)j_GetIntField;             // GetStaticShortField
  env_table[153] = (void *)j_GetDoubleField;          // GetStaticDoubleField
  env_table[154] = (void *)j_SetObjectField;          // SetStaticObjectField
  env_table[155] = (void *)j_SetWordField;            // SetStaticBooleanField
  env_table[156] = (void *)j_SetWordField;            // SetStaticByteField
  env_table[157] = (void *)j_SetWordField;            // SetStaticCharField
  env_table[158] = (void *)j_SetWordField;            // SetStaticShortField
  env_table[159] = (void *)j_SetWordField;            // SetStaticIntField
  env_table[160] = (void *)j_SetWordField;            // SetStaticLongField
  env_table[161] = (void *)j_SetFloatField;           // SetStaticFloatField
  env_table[162] = (void *)j_SetDoubleField;          // SetStaticDoubleField
  env_table[228] = (void *)j_ExceptionCheck;
  env_table[230] = (void *)j_GetDirectBufferAddress;  // fmodProcess drains via this
  env_table[231] = (void *)j_GetDirectBufferCapacity; // defensive (unused by fmodProcess)

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon
}
