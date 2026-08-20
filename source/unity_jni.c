/* unity_jni.c -- real handlers for ZOOKEEPER DX's IMPLEMENT classes.
 * Style/idioms follow cr3_nx's jni_fake.c. See unity_jni.h for how it plugs in.
 *
 * NOT compile-tested (no devkitA64 here). Method names/signatures are the
 * standard Android framework API, so they're concrete; the spots that need
 * checking on hardware are marked CHECK. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "asset_pack.h"
#include "libc_shim.h"   /* nx_pack_relpath */
#include "unity_jni.h"
#include "jni_fake.h"   /* jni_note_approx: ledger (round 130) */
#include "util.h"
#include "config.h"   /* GAME_PACKAGE, GAME_FOLDER -- added by edit 10 */

/* FakeID layout we read (must match jni_fake.c). Only the char[] fields. */
struct FakeID { uint32_t tag; char cls[96]; char name[64]; char sig[160]; };

extern int screen_width, screen_height;   /* real panel size (config.c) */
#define SCREEN_W_HANDHELD 720   /* fbstub45 PORTRAIT (stable) */
#define SCREEN_H_HANDHELD 1280
#define SCREEN_W_DOCKED   1080
#define SCREEN_H_DOCKED   1920
#define REFRESH_HZ        60

static char g_root[192];            /* e.g. /switch/zookeeper                 */
static char g_assets[208];          /* g_root "/assets"                      */

static int  has(const char *s, const char *sub){ return strstr(s,sub)!=NULL; }
static int  ret_is(const char *sig,const char *t){ const char*p=strchr(sig,')'); return p && strstr(p+1,t)==p+1; }

/* --------------------------------------------------------------------------
 * stateful handle objects (streams / fds / prefs) -- jni_fake's objects are
 * label-only, so we carry our own. Opaque to the engine; recovered via recv.
 * -------------------------------------------------------------------------- */
enum { UJ_TAG = 0x554a4831 /*'UJH1'*/ };
enum { UJ_INPUTSTREAM, UJ_AFD, UJ_FD, UJ_PREFS, UJ_EDITOR, UJ_GENERIC,
       UJ_MAP, UJ_SET, UJ_ITER, UJ_ENTRY, UJ_BOXED };

typedef struct {
  uint32_t tag; int kind;
  FILE *fp;                 /* InputStream                                  */
  int   fd;                 /* AFD / FD                                     */
  long  off, len;           /* AFD region                                  */
  int   idx;                /* UJ_ITER cursor / UJ_ENTRY kv index           */
  char  btype;              /* UJ_BOXED: 'I' int 'L' long 'B' bool 'F' float*/
  long long bival;          /* UJ_BOXED int/long/bool payload               */
  double bfval;             /* UJ_BOXED float payload                       */
} UHandle;

static UHandle *uh_new(int kind){
  UHandle *h = calloc(1,sizeof *h);
  h->tag = UJ_TAG; h->kind = kind; h->fd = -1; return h;
}
/* jni_fake.c free_ref() should call this for UJ_TAG; until then streams that
 * the engine forgets to close() leak. Optional hardening, see header note. */
void unity_handle_free(void *p){
  UHandle *h = p; if (!h || h->tag!=UJ_TAG) return;
  if (h->fp) fclose(h->fp);
  if (h->fd>=0) {
    int closed = 0;
#if PHI_ASSET_PACK
    /* pack handles must go back to the pack, never to libnx close() */
    if (asset_pack_fd_is(h->fd)) { asset_pack_close_fd(h->fd); closed = 1; }
#endif
    if (!closed) close(h->fd);
  }
  free(h);
}
static int is_uh(void *p,int kind){ UHandle*h=p; return h && h->tag==UJ_TAG && h->kind==kind; }

/* --------------------------------------------------------------------------
 * asset path: AssetManager.open("bin/Data/x") -> g_root/assets/bin/Data/x
 * (the engine's *direct* fopen of the data path is handled separately by the
 *  libc_shim fopen redirect + the Context path getters below.)
 * -------------------------------------------------------------------------- */
static void asset_path(char *out,size_t n,const char *name){
  while (name && (name[0]=='/' )) name++;
  snprintf(out,n,"%s/%s",g_assets,name?name:"");
}

/* Path handed to *managed* code (persistentDataPath / dataPath via the Context
 * getters below). It must be Unix-rooted ("/switch/zookeeper"): Mono/IL2CPP on
 * Android uses Unix path rules where ":" is NOT a root marker, so a "sdmc:/..."
 * path is treated as *relative* and Path.Combine() concatenates it after a
 * relative asset path -> "assets/bin/Data/sdmc:/switch/zookeeper". newlib then
 * sees the embedded "sdmc:" mid-path, mis-parses the device, and null-derefs
 * (Data Abort at the devoptab mkdir_r slot, +0x68). Stripping the device prefix
 * fixes this; newlib still resolves the device-less absolute path via the
 * default device (sdmc), set by our boot chdir, so file I/O is unaffected. Our
 * internal g_root/g_assets keep the explicit "sdmc:" prefix. */
static const char *managed_root(void);

/* Exposed: jni_fake's generic ("A") path needs the same string, because Unity
 * asks for the Context path getters with the class erased to java/lang/Object,
 * where the Context-gated handler below never sees them. */
const char *nx_managed_root(void){ return managed_root(); }

static const char *managed_root(void){
  const char *c = strchr(g_root, ':');
  return (c && c[1] == '/') ? c + 1 : g_root;
}

/* ==========================================================================
 * SharedPreferences  (Unity PlayerPrefs == save data)  -> g_root/prefs.kv
 * flat "type\tkey\tvalue" lines; value url-ish escaped on tab/newline.
 * ========================================================================== */
typedef struct { char type; char *key; char *val; } KV;
static KV   *g_kv = NULL; static int g_kv_n=0, g_kv_cap=0; static int g_kv_dirty=0;
static int g_kv_flush_delay = 0;   /* frames remaining before a deferred apply lands */

static char *prefs_file(char *buf,size_t n){ snprintf(buf,n,"%s/prefs.kv",g_root); return buf; }

static void kv_set(char type,const char*key,const char*val){
  for (int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)){
    g_kv[i].type=type; free(g_kv[i].val); g_kv[i].val=strdup(val); g_kv_dirty=1; return; }
  if (g_kv_n==g_kv_cap){ g_kv_cap=g_kv_cap?g_kv_cap*2:32; g_kv=realloc(g_kv,g_kv_cap*sizeof(KV)); }
  g_kv[g_kv_n].type=type; g_kv[g_kv_n].key=strdup(key); g_kv[g_kv_n].val=strdup(val);
  g_kv_n++; g_kv_dirty=1;
}
static KV *kv_get(const char*key){ for(int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)) return &g_kv[i]; return NULL; }
static void kv_remove(const char*key){
  for(int i=0;i<g_kv_n;i++) if(!strcmp(g_kv[i].key,key)){
    free(g_kv[i].key);free(g_kv[i].val); g_kv[i]=g_kv[--g_kv_n]; g_kv_dirty=1; return; }
}
static void kv_clear(void){ for(int i=0;i<g_kv_n;i++){free(g_kv[i].key);free(g_kv[i].val);} g_kv_n=0; g_kv_dirty=1; }

static void esc(FILE*f,const char*s){ for(;*s;s++){ if(*s=='\\'||*s=='\t'||*s=='\n'){fputc('\\',f);
  fputc(*s=='\t'?'t':*s=='\n'?'n':'\\',f);} else fputc(*s,f);} }
static char *unesc(char*s){ char*o=s,*w=s; for(;*o;o++){ if(*o=='\\'&&o[1]){o++;
  *w++=(*o=='t')?'\t':(*o=='n')?'\n':*o;} else *w++=*o;} *w=0; return s; }

static void prefs_load(void){
  char p[256]; FILE*f=fopen(prefs_file(p,sizeof p),"rb");
  if(!f){ debugPrintf("[prefs] load: no save file at %s (errno=%d)\n", p, errno); return; }
  char line[2048];
  while (fgets(line,sizeof line,f)){
    char *nl=strchr(line,'\n'); if(nl)*nl=0;
    if(!line[0]) continue;
    char type=line[0]; char *k=line+2;            /* "T\tkey\tval"          */
    char *t1=strchr(k,'\t'); if(!t1) continue; *t1=0; char*v=t1+1;
    kv_set(type, unesc(k), unesc(v));
  }
  fclose(f); g_kv_dirty=0;
  debugPrintf("[prefs] load: %d entries from %s\n", g_kv_n, p);
}
/* Write via a temp file and rename, so a power-off mid-write cannot leave a
 * half-written save. The reported prefs.kv ended in a torn line for exactly
 * that reason: the file is rewritten whole, hundreds of times, so the odds of
 * being interrupted inside one of those writes are not small. A rename is
 * atomic, so the old save survives intact until the new one is complete. */
static void prefs_flush(void){
  if(!g_kv_dirty){ debugPrintf("[prefs] flush: nothing dirty (%d entries)\n", g_kv_n); return; }
  char p[256], tmp[288];
  prefs_file(p,sizeof p);
  snprintf(tmp,sizeof tmp,"%s.tmp",p);
  FILE*f=fopen(tmp,"wb");
  if(!f){ debugPrintf("[prefs] flush FAILED: fopen(%s,wb) errno=%d\n", tmp, errno); return; }
  for(int i=0;i<g_kv_n;i++){ fputc(g_kv[i].type,f); fputc('\t',f);
    esc(f,g_kv[i].key); fputc('\t',f); esc(f,g_kv[i].val); fputc('\n',f); }
  const int werr = ferror(f);
  if (fclose(f) != 0 || werr) {
    debugPrintf("[prefs] flush FAILED while writing %s -- save left untouched\n", tmp);
    remove(tmp); return;
  }
  remove(p);                       /* FAT rename does not replace */
  if (rename(tmp,p) != 0) {
    debugPrintf("[prefs] flush FAILED: rename(%s->%s) errno=%d\n", tmp, p, errno);
    return;
  }
  g_kv_dirty=0; g_kv_flush_delay=0;
  debugPrintf("[prefs] flush: wrote %d entries to %s\n", g_kv_n, p);
}

/* Android's Editor.apply() is ASYNCHRONOUS: it commits to memory and writes
 * back in the background, coalescing bursts. Only commit() is synchronous.
 * This shim used to flush on BOTH, and the flush rewrites the entire file --
 * so a game storing its song database one key at a time cost O(N^2) SD writes.
 * A real boot log shows it plainly:
 *
 *     423 full rewrites, entries growing 86 -> 419
 *     8.7 MB written to SD to store a 42 KB file
 *
 * On a console reading from an SD card that is not a slow save, it is a boot
 * that looks frozen. Deferring apply() to a debounce restores the documented
 * behaviour and collapses those 423 rewrites into a handful. commit() still
 * writes immediately, because callers are entitled to rely on that. */
void unity_prefs_tick(void){
  /* Counted in FRAMES rather than wall time: no timing API, no clock to get
   * wrong, and the caller is the frame loop by construction. ~15 frames is a
   * quarter-second at 60fps and comfortably longer than the bursts the game
   * emits, so a run of applies collapses into one write. */
  if(!g_kv_dirty || g_kv_flush_delay <= 0) return;
  if(--g_kv_flush_delay > 0) return;
  prefs_flush();
}
void unity_prefs_sync(void){ if(g_kv_dirty) prefs_flush(); }   /* shutdown/pause */

/* ==========================================================================
 * getAll() boxed values: turn a stored KV into the Java object Unity expects.
 * Strings come back as a native FakeString (Unity reads them via
 * GetStringUTFChars); primitives come back as our UJ_BOXED handle, which
 * jni_fake.c recognises by receiver for IsInstanceOf + intValue/longValue/
 * booleanValue/floatValue. Type char matches kv_set(): S/I/L/B/F.
 * ========================================================================== */
static void *uh_box_from_kv(const KV *kv){
  if (!kv) return jni_make_string("");
  switch (kv->type){
    case 'I': case 'L': case 'B': {
      UHandle *h = uh_new(UJ_BOXED);
      h->btype = kv->type;
      h->bival = strtoll(kv->val, NULL, 10);
      if (kv->type=='B') h->bival = (kv->val[0]=='1'||kv->val[0]=='t'||kv->val[0]=='T') ? 1 : 0;
      return h;
    }
    case 'F': {
      UHandle *h = uh_new(UJ_BOXED);
      h->btype = 'F'; h->bfval = strtod(kv->val, NULL);
      return h;
    }
    default: /* 'S' and anything else -> string */
      return jni_make_string(kv->val);
  }
}

/* Receiver-keyed accessors used by jni_fake.c (see unity_jni.h). */
int unity_is_boxed(void *p){
  UHandle *h = p; return (h && h->tag==UJ_TAG && h->kind==UJ_BOXED) ? 1 : 0;
}
uint64_t unity_boxed_int(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0;
  if (h->btype=='F') return (uint64_t)(long long)h->bfval;
  return (uint64_t)h->bival;
}
float unity_boxed_float(void *p){
  UHandle *h = p; if (!unity_is_boxed(h)) return 0.0f;
  return (h->btype=='F') ? (float)h->bfval : (float)h->bival;
}
/* 1/0 if obj is one of our boxed primitives and matches/!matches clazz; -1 if
 * obj is not ours (jni_fake.c then applies its own rules). */
int unity_isinstance(void *p, const char *clazz){
  UHandle *h = p; if (!h || h->tag!=UJ_TAG || h->kind!=UJ_BOXED) return -1;
  if (!clazz) return 0;
  switch (h->btype){
    case 'I': return strstr(clazz,"Integer") ? 1 : 0;
    case 'L': return strstr(clazz,"Long")    ? 1 : 0;
    case 'F': return strstr(clazz,"Float")   ? 1 : 0;
    case 'B': return strstr(clazz,"Boolean") ? 1 : 0;
  }
  return 0;
}

/* ==========================================================================
 * class ownership + dispatch
 * ========================================================================== */
int unity_owns_class(const char *cls){
  return has(cls,"AssetManager") || has(cls,"java/io/InputStream") ||
         has(cls,"AssetFileDescriptor") || has(cls,"java/io/FileDescriptor") ||
         has(cls,"SharedPreferences") || has(cls,"SharedPreferences$Editor") ||
         has(cls,"java/util/Map") || has(cls,"java/util/Set") ||
         has(cls,"java/util/Iterator") || has(cls,"java/util/HashMap") ||
         has(cls,"view/Display") || has(cls,"DisplayManager") ||
         has(cls,"res/Configuration") || has(cls,"res/Resources") ||
         has(cls,"DisplayMetrics") || has(cls,"content/Context") ||
         has(cls,"unity3d/player/UnityPlayer");
}

/* ---- object-returning calls --------------------------------------------- */
void *unity_dispatch_object(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;

  /* AssetManager.open(name) -> InputStream ; openFd(name) -> AssetFileDescriptor */
  if (has(cls,"AssetManager")){
    if (has(m,"openFd") || has(m,"openNonAssetFd")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[320]; asset_path(path,sizeof path,name);
      int fd = -1;
#if PHI_ASSET_PACK
      /* the pack holds these once the loose tree is gone */
      if (asset_pack_active()) {
        const char *rel = nx_pack_relpath(path);
        if (rel) fd = asset_pack_open_path(rel);
      }
#endif
      if (fd < 0) fd = open(path,O_RDONLY);
      debugPrintf("[io] JNI AssetManager.openFd(%s) -> %s [%s]\n", name, fd>=0?"ok":"MISSING", path);
      if(fd<0){ return NULL; /* CHECK: engine expects exception; NULL usually ok */ }
      long flen = -1;
#if PHI_ASSET_PACK
      /* pack fds are not POSIX fds -- size comes from the pack. Each branch
       * is a complete statement so the #if cannot split an if/else pair. */
      if (asset_pack_fd_is(fd)) {
        uint64_t psz = 0;
        if (asset_pack_fstat_fd(fd, &psz, NULL, NULL)) flen = (long)psz;
      }
#endif
      if (flen < 0) { struct stat st; flen = (fstat(fd,&st)==0) ? (long)st.st_size : 0; }
      UHandle*h=uh_new(UJ_AFD); h->fd=fd; h->off=0; h->len=flen; return h;
    }
    if (has(m,"open")){
      const char *name = jni_string_utf(va_arg(va,void*));
      char path[320]; asset_path(path,sizeof path,name);
      FILE*fp=NULL;
#if PHI_ASSET_PACK
      /* boot.config and friends live in the pack once the loose tree is
       * deleted; fopen() on the old path would return MISSING and Unity
       * treats a missing boot.config as a fatal data error. */
      if (asset_pack_active()) {
        const char *rel = nx_pack_relpath(path);
        void *data = NULL; size_t size = 0;
        if (rel && asset_pack_read_all_path(rel, &data, &size)) {
#if PHI_GC_NON_INCREMENTAL
          /* boot.config carries "gc-max-time-slice=3", which selects Unity's
           * INCREMENTAL collector. Blank the key here, on the copy handed to the
           * engine -- the file on the card is untouched. Padding with spaces
           * rather than deleting keeps the buffer length and every other offset
           * identical, so nothing else in the parse shifts. */
          if (name && strstr(name, "boot.config") && data && size) {
            /* plain scan rather than memmem(): that is a GNU extension and its
             * availability in devkitA64's newlib is not something to bet a build
             * on. The file is ~171 bytes. */
            static const char KEY[] = "gc-max-time-slice";
            const size_t klen = sizeof KEY - 1;
            char *k = NULL;
            if (size >= klen)
              for (size_t i = 0; i + klen <= size; i++)
                if (!memcmp((char *)data + i, KEY, klen)) { k = (char *)data + i; break; }
            if (k) {
              char *end = (char *)memchr(k, '\n', (char *)data + size - k);
              if (!end) end = (char *)data + size;
              memset(k, ' ', (size_t)(end - k));
              debugPrintf("[gc] boot.config: gc-max-time-slice removed "
                          "-> non-incremental collector\n");
            }
          }
#endif
          fp = fmemopen_locked(data, size ? size : 1, "rb");
          if (!fp) free(data);
        }
      }
#endif
      if (!fp) fp = fopen(path,"rb");
      debugPrintf("[io] JNI AssetManager.open(%s) -> %s [%s]\n", name, fp?"ok":"MISSING", path);
      if(!fp) return NULL;
      UHandle*h=uh_new(UJ_INPUTSTREAM); h->fp=fp; return h;
    }
    if (has(m,"list")) return jni_make_object("String[]"); /* CHECK: empty array */
    return jni_make_object("AssetManager");
  }

  /* AssetFileDescriptor.getFileDescriptor() -> FileDescriptor (carries the fd) */
  if (has(cls,"AssetFileDescriptor")){
    if (has(m,"getFileDescriptor") || has(m,"getParcelFileDescriptor")){
      UHandle*a=recv; UHandle*fd=uh_new(UJ_FD); fd->fd = is_uh(a,UJ_AFD)?a->fd:-1; return fd;
    }
    return jni_make_object("AssetFileDescriptor");
  }

  /* SharedPreferences.edit() -> Editor ; getString -> String ; getAll -> Map */
  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    if (has(m,"edit")) return uh_new(UJ_EDITOR);
    if (has(m,"getString")){
      const char *key = jni_string_utf(va_arg(va,void*));
      KV*kv=kv_get(key);
      return jni_make_string(kv?kv->val: (ret_is(id->sig,"Ljava/lang/String;")? "" : "") );
    }
    if (has(m,"getAll")){                 /* Unity PlayerPrefs LOAD entry point */
      debugPrintf("[prefs] getAll() -> Map of %d entries\n", g_kv_n);
      return uh_new(UJ_MAP);
    }
    if (has(m,"getStringSet")) return jni_make_object("Set");
    return jni_make_object("SharedPreferences");
  }

  /* ---- getAll() Map iteration: Map.entrySet/keySet -> Set -> Iterator -> ----
   * Entry{getKey,getValue}. The whole chain just walks the live g_kv list; the
   * Iterator carries a cursor, each Entry captures one index. Map.get(key) is
   * also handled in case Unity takes the keySet()+get() path on some build. */
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    if (has(m,"entrySet") || has(m,"keySet")) return uh_new(UJ_SET);
    if (has(m,"get")){ const char*k=jni_string_utf(va_arg(va,void*)); return uh_box_from_kv(kv_get(k)); }
    return uh_new(UJ_MAP);
  }
  if (has(cls,"java/util/Set")){
    if (has(m,"iterator")){ UHandle*it=uh_new(UJ_ITER); it->idx=0; return it; }
    return uh_new(UJ_SET);
  }
  if (has(cls,"java/util/Iterator")){
    if (has(m,"next")){                   /* return current entry, advance cursor */
      UHandle*it=recv;
      int i = is_uh(it,UJ_ITER) ? it->idx : 0;
      if (is_uh(it,UJ_ITER)) it->idx++;
      UHandle*e=uh_new(UJ_ENTRY); e->idx=i; return e;
    }
    return jni_make_object("java/util/Iterator");
  }
  if (has(cls,"java/util/Map") && has(cls,"Entry")){   /* java/util/Map$Entry */
    UHandle*e=recv; int i = is_uh(e,UJ_ENTRY) ? e->idx : -1;
    if (i<0 || i>=g_kv_n) return jni_make_string("");
    if (has(m,"getKey"))   return jni_make_string(g_kv[i].key);
    if (has(m,"getValue")) return uh_box_from_kv(&g_kv[i]);
    return jni_make_string("");
  }
  if (has(cls,"SharedPreferences$Editor")){
    /* putX all return the Editor (chained: editor.putInt(k,v).apply()), so the
     * engine reaches them via CallObjectMethod -> HERE, not the int path. All
     * four primitive puts MUST kv_set here or the write is silently dropped
     * (this was the save bug: int/bool prefs, incl. Unity's storage-version
     * marker, never persisted -> "Upgrading PlayerPrefs storage" every launch).
     * An EMPTY key means the key-encoding path (String([B)/Uri.encode) produced
     * nothing; storing under "" makes every such pref collide into one slot and
     * corrupts Screenmanager resolution -> bad res -> crash. Skip those. */
    if (has(m,"putString")){ const char*k=jni_string_utf(va_arg(va,void*));
      const char*v=jni_string_utf(va_arg(va,void*));
      if(!k[0]){ debugPrintf("[prefs] putString SKIP empty key\n"); return recv; }
      kv_set('S',k,v); debugPrintf("[prefs] putString '%s'\n", k); return recv; }
    if (has(m,"putInt")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); char b[32]; snprintf(b,sizeof b,"%d",v);
      if(!k[0]){ debugPrintf("[prefs] putInt SKIP empty key (=%d)\n",v); return recv; }
      kv_set('I',k,b); debugPrintf("[prefs] putInt '%s'=%d\n",k,v); return recv; }
    if (has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      if(!k[0]){ debugPrintf("[prefs] putLong SKIP empty key\n"); return recv; }
      kv_set('L',k,b); debugPrintf("[prefs] putLong '%s'=%lld\n",k,v); return recv; }
    if (has(m,"putFloat")){ const char*k=jni_string_utf(va_arg(va,void*));
      double v=va_arg(va,double); char b[32]; snprintf(b,sizeof b,"%.9g",v);
      if(!k[0]){ debugPrintf("[prefs] putFloat SKIP empty key\n"); return recv; }
      kv_set('F',k,b); debugPrintf("[prefs] putFloat '%s'=%g\n",k,v); return recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int);
      if(!k[0]){ debugPrintf("[prefs] putBoolean SKIP empty key (=%d)\n",v); return recv; }
      kv_set('B',k,v?"1":"0"); debugPrintf("[prefs] putBoolean '%s'=%d\n",k,v); return recv; }
    if (has(m,"remove")){ const char*k=jni_string_utf(va_arg(va,void*)); kv_remove(k); return recv; }
    if (has(m,"clear")){ kv_clear(); return recv; }
    return recv;
  }

  /* Display / DisplayManager / Resources / Context: object getters */
  if (has(cls,"DisplayManager") && has(m,"getDisplay")) return jni_make_object("Display");
  /* Display.getSupportedModes() -> Display$Mode[]. Round 149.
   *
   * This fell through to the terminal `jni_make_object(cls)` and handed Unity an
   * opaque handle where an ARRAY belongs -- flagged as OPAQUE-OBJ + INSPECTED in
   * the ledger since r131. GetArrayLength() then tag-checks it, does not see
   * TAG_OBJARR and answers 0, so Unity concludes the panel supports **no display
   * modes at all**. Survivable, but it is a lie, and it is the same
   * non-object-in-a-pointer-slot shape this port keeps paying for.
   *
   * Hand back a real one-element array holding a pooled Mode object; the int
   * getters below report the actual panel. */
  if (has(m,"getSupportedModes")) {
    void *mode = jni_make_object("android/view/Display$Mode");
    void *arr  = jni_new_object_array(1, mode);
    return arr ? arr : jni_make_object(cls);
  }
  if (has(m,"getMode")) return jni_make_object("android/view/Display$Mode");
  if (has(cls,"res/Resources")){
    if (has(m,"getConfiguration")) return jni_make_object("Configuration");
    if (has(m,"getDisplayMetrics")) return jni_make_object("DisplayMetrics");
    return jni_make_object("Resources");
  }
  if (has(cls,"content/Context")){
    /* path getters -> our staged dir, so engine fopen()s land on the SD card */
    if (has(m,"getFilesDir")||has(m,"getCacheDir")||has(m,"getDataDir")||has(m,"getExternalFilesDir"))
      return jni_make_object("File");                 /* File.getAbsolutePath -> g_root below */
    if (has(m,"getPackageName")) return jni_make_string(GAME_PACKAGE);
    if (has(m,"getPackageCodePath")||has(m,"getPackageResourcePath")) return jni_make_string(managed_root());
    if (has(m,"getAssets")) return jni_make_object("AssetManager");
    if (has(m,"getResources")) return jni_make_object("Resources");
    if (has(m,"getSystemService")) return jni_make_object("Service");
    return jni_make_object("Context");
  }
  if (has(cls,"java/io/File") && (has(m,"getAbsolutePath")||has(m,"getPath")||has(m,"toString")))
    return jni_make_string(managed_root());

  /* UnityPlayer host queries that return objects -> benign */
  if (has(cls,"UnityPlayer")) return jni_make_object("UnityPlayer");

  /* The r127 shape exactly: an opaque handle where a File or String belonged.
   * It never returns NULL, so nothing downstream can tell this apart from a
   * real object -- which is why it has to be in the ledger. */
  /* Round 158: an opaque object is never a valid stand-in for a Java ARRAY.
   * Covers getDeviceIds()[I, getObbDirs()[Ljava/io/File; and
   * getDevices(I)[Landroid/media/AudioDeviceInfo; -- all three reached this
   * terminal and were answered with a handle the game then passed to
   * GetArrayElements. */
  { void *empty = jni_new_empty_array(id->sig);
    if (empty) {
      jni_note_approx("EMPTY-ARRAY", cls, m, id->sig);
      return empty;
    } }
  jni_note_approx("OPAQUE-OBJ", cls, m, id->sig);
  return jni_make_object(cls); /* default: opaque handle, never NULL */
}

/* ---- int / boolean / long calls ----------------------------------------- */
uint64_t unity_dispatch_int(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;

  /* getAll() iteration: Iterator.hasNext + a few Map predicates. (Integer/Long/
   * Boolean unboxing is routed by RECEIVER in jni_fake.c, not here.) */
  if (has(cls,"java/util/Iterator") && has(m,"hasNext")){
    UHandle*it=recv; return (uint64_t)((is_uh(it,UJ_ITER) && it->idx < g_kv_n) ? 1 : 0);
  }
  if (has(cls,"java/util/Map") && !has(cls,"Entry")){
    if (has(m,"size"))    return (uint64_t)g_kv_n;
    if (has(m,"isEmpty")) return (uint64_t)(g_kv_n==0);
    if (has(m,"containsKey")){ const char*k=jni_string_utf(va_arg(va,void*)); return (uint64_t)(kv_get(k)?1:0); }
  }

  /* InputStream.read() / read([B) / read([B,off,len) / available / skip */
  if (has(cls,"java/io/InputStream")){
    UHandle*h=recv; if(!is_uh(h,UJ_INPUTSTREAM)||!h->fp) return (uint64_t)-1;
    if (has(m,"available")){ long cur=ftell(h->fp); fseek(h->fp,0,SEEK_END);
      long end=ftell(h->fp); fseek(h->fp,cur,SEEK_SET); return (uint64_t)(end-cur); }
    if (has(m,"skip")){ long nskip=(long)va_arg(va,long long); fseek(h->fp,nskip,SEEK_CUR); return (uint64_t)nskip; }
    if (has(m,"close")){ fclose(h->fp); h->fp=NULL; return 0; }
    if (has(m,"read")){
      if (strstr(id->sig,"([B")){                     /* read(byte[][,off,len]) */
        void *arr = va_arg(va,void*);
        int alen=0; char *buf = jni_bytearray_data(arr,&alen);
        int off=0, len=alen;
        if (strstr(id->sig,"([BII)")){ off=va_arg(va,int); len=va_arg(va,int); }
        size_t got=fread(buf+off,1,(size_t)len,h->fp);
        return got? (uint64_t)got : (uint64_t)-1;     /* -1 == EOF, per InputStream */
      }
      int c=fgetc(h->fp); return (uint64_t)(c==EOF? -1 : c); /* read() one byte */
    }
    return 0;
  }

  /* SharedPreferences getters (Int/Long/Boolean + contains) */
  if (has(cls,"SharedPreferences") && !has(cls,"Editor")){
    if (has(m,"contains")){ const char*k=jni_string_utf(va_arg(va,void*)); return kv_get(k)?1:0; }
    if (has(m,"getInt")||has(m,"getLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long def=(long long)va_arg(va,long long); KV*kv=kv_get(k);
      return (uint64_t)(kv? strtoll(kv->val,NULL,10) : def); }
    if (has(m,"getBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int def=va_arg(va,int); KV*kv=kv_get(k); return (uint64_t)(kv? (kv->val[0]=='1'||kv->val[0]=='t') : def); }
    return 0;
  }
  /* Editor.putInt/Long/Boolean(...)Z?  most return the Editor (object), but
   * commit() returns Z. Route the primitive puts here since the key+value are
   * primitive-shaped, then have the engine ignore the int return. CHECK. */
  if (has(cls,"SharedPreferences$Editor")){
    if (has(m,"commit")){ debugPrintf("[prefs] commit\n"); prefs_flush(); return 1; }
    if (has(m,"putInt")||has(m,"putLong")){ const char*k=jni_string_utf(va_arg(va,void*));
      long long v=(long long)va_arg(va,long long); char b[32]; snprintf(b,sizeof b,"%lld",v);
      kv_set(has(m,"putLong")?'L':'I',k,b); return (uint64_t)(uintptr_t)recv; }
    if (has(m,"putBoolean")){ const char*k=jni_string_utf(va_arg(va,void*));
      int v=va_arg(va,int); kv_set('B',k,v?"1":"0"); return (uint64_t)(uintptr_t)recv; }
    return (uint64_t)(uintptr_t)recv;
  }

  /* AssetFileDescriptor.getStartOffset()/getLength()/getDeclaredLength() (long) */
  if (has(cls,"AssetFileDescriptor")){
    UHandle*a=recv;
    if (has(m,"getStartOffset")) return (uint64_t)(is_uh(a,UJ_AFD)?a->off:0);
    if (has(m,"getLength")||has(m,"getDeclaredLength")) return (uint64_t)(is_uh(a,UJ_AFD)?a->len:0);
    return 0;
  }
  /* FileDescriptor: some engines read the raw int via a field, not a call.
   * If Unity calls FileDescriptor.getInt$()/getFd(), hand back the fd. CHECK. */
  if (has(cls,"java/io/FileDescriptor")){ UHandle*f=recv; return (uint64_t)(is_uh(f,UJ_FD)?(unsigned)f->fd:0); }

  /* Display / DisplayMetrics / Configuration ints */
  if (has(cls,"view/Display")||has(cls,"DisplayMetrics")||has(cls,"DisplayManager")){
    /* real panel size (landscape for PvZ); already docked/handheld aware */
    if (has(m,"getWidth")||has(m,"WidthPixels")||has(m,"getRawWidth"))  return screen_width;
    if (has(m,"getHeight")||has(m,"HeightPixels")||has(m,"getRawHeight"))return screen_height;
    if (has(m,"orientation")) return 2;   /* Configuration.ORIENTATION_LANDSCAPE */
    if (has(m,"getRotation")) return 0;   /* Surface.ROTATION_0 */
    if (has(m,"getDisplayId")) return 0;
    return 0;
  }
  return 0;
}

/* ---- void calls --------------------------------------------------------- */
void unity_dispatch_void(void *recv, const void *id_, va_list va){ const struct FakeID *id = id_;
  const char *cls=id->cls, *m=id->name;
  if (has(cls,"java/io/InputStream") && has(m,"close")){ UHandle*h=recv;
    if(is_uh(h,UJ_INPUTSTREAM)&&h->fp){fclose(h->fp);h->fp=NULL;} return; }
  if (has(cls,"AssetFileDescriptor") && has(m,"close")){ UHandle*a=recv;
    if(is_uh(a,UJ_AFD)&&a->fd>=0){close(a->fd);a->fd=-1;} return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"apply")){
    /* Defer, per Android's contract -- see unity_prefs_tick. Each apply pushes
     * the deadline out, so a burst of N writes costs one flush, not N. */
    g_kv_flush_delay = 15;          /* ~250 ms at 60fps; each apply re-arms it */
    return; }
  if (has(cls,"SharedPreferences$Editor") && has(m,"putString")){ /* if routed here as void */
    const char*k=jni_string_utf(va_arg(va,void*)); const char*v=jni_string_utf(va_arg(va,void*));
    kv_set('S',k,v); return; }
  if (has(cls,"UnityPlayer")){
    /* setOrientation/lowMemory/configurationChanged/etc. -> no-op */
    return;
  }
  (void)recv;(void)va;
}

/* ========================================================================== */
void unity_jni_init(const char *data_root){
  /* data_root is nx_root() from main.c; the fallback only matters if that
   * were ever NULL, and should stay consistent with nx_root.c. */
  snprintf(g_root,sizeof g_root,"%s",data_root && *data_root ? data_root : DATA_ROOT_DEFAULT);
  snprintf(g_assets,sizeof g_assets,"%s/assets",g_root);
  prefs_load();
  /* caller (jni_fake.c jni_init) should also intern every UNITY_JNI_CLASSES[]
   * name so FindClass returns non-NULL classes. */
}
