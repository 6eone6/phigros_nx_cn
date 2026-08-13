/* nx_root.c -- find the game folder at RUNTIME instead of hardcoding it.
 *
 * WHY
 * The tree used to bake the data root in at compile time:
 *     #define DATA_ROOT "sdmc:/switch/" GAME_FOLDER
 * so the loader only ever worked from one folder name. The .nro builds as
 * phigros_nx.nro, so the natural thing to do is make a switch/phigros_nx/
 * folder -- and then every path misses by one directory and the loader reports
 * a missing libmain.so that is sitting right there. That is a bad failure: it
 * points at the APK extraction (which was fine) instead of the folder name.
 *
 * HOW
 * Three strategies, in order, first hit wins:
 *
 *   1. argv[0]. hbmenu passes the full path of the .nro it launched, e.g.
 *      sdmc:/switch/anything/phigros_nx.nro -- so its directory IS the game
 *      folder, whatever it is called. This is the normal path and it works for
 *      any name, any depth, including nested folders and non-ASCII names.
 *
 *   2. Scan one level under sdmc:/switch/ for a directory containing
 *      libmain.so. Covers the case where argv[0] is missing or relative
 *      (some forwarders, some netloader setups).
 *
 *   3. The compile-time default, so behaviour is unchanged if both fail.
 *
 * A candidate only counts if libmain.so is actually in it. Detecting a folder
 * that merely exists would trade one confusing failure for another.
 *
 * The root is resolved ONCE, before the log opens, and never changes. Callers
 * get a stable const char * (nx_root) or a formatted path from a small
 * rotating buffer pool (nx_rp). The pool is for boot-path use; nx_root itself
 * is a plain pointer and is safe to use from the file-I/O shims on any thread.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "nx_root.h"

#define NX_ROOT_MAX 512

static char g_root[NX_ROOT_MAX]      = DATA_ROOT_DEFAULT;
static char g_root_nodev[NX_ROOT_MAX] = "";
static char g_casetest[NX_ROOT_MAX + 16] = "";
static int  g_inited = 0;
static const char *g_how = "compile-time default";

/* Does this directory hold the game? libmain.so is the cheapest proof. */
static int looks_like_game_dir(const char *dir) {
  char p[NX_ROOT_MAX + 32];
  struct stat st;
  if (!dir || !*dir) return 0;
  snprintf(p, sizeof p, "%s/libmain.so", dir);
  return stat(p, &st) == 0;
}

/* Strip the libnx device prefix ("sdmc:") for the paths we hand to managed
 * code -- newlib resolves a device-less absolute path through the default
 * device, and the devoptab mkdir_r slot faults on the prefixed form. */
static void make_nodev(void) {
  const char *c = strchr(g_root, ':');
  const char *p = (c && c[1] == '/') ? c + 1 : g_root;
  snprintf(g_root_nodev, sizeof g_root_nodev, "%s", p);
}

void nx_root_init(int argc, char **argv) {
  char cand[NX_ROOT_MAX];

  if (g_inited) return;
  g_inited = 1;

  /* --- 1. the directory the .nro was launched from ---------------------- */
  if (argc > 0 && argv && argv[0] && argv[0][0]) {
    snprintf(cand, sizeof cand, "%s", argv[0]);
    char *slash = strrchr(cand, '/');
    if (slash && slash != cand) {
      *slash = '\0';
      if (looks_like_game_dir(cand)) {
        snprintf(g_root, sizeof g_root, "%s", cand);
        g_how = "argv[0] (folder the .nro was launched from)";
        goto done;
      }
    }
  }

  /* --- 2. scan one level under /switch ---------------------------------- */
  {
    static const char *bases[] = { "sdmc:/switch", "/switch" };
    for (unsigned b = 0; b < sizeof(bases)/sizeof(*bases); b++) {
      DIR *d = opendir(bases[b]);
      if (!d) continue;
      struct dirent *e;
      int hits = 0;
      while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(cand, sizeof cand, "%s/%s", bases[b], e->d_name);
        if (!looks_like_game_dir(cand)) continue;
        if (++hits == 1) snprintf(g_root, sizeof g_root, "%s", cand);
        /* Keep scanning rather than taking the first hit blindly: if there are
         * two staged copies, readdir order decides which one runs, which is a
         * confusing way to load yesterday's assets. Say so in the log. */
      }
      closedir(d);
      if (hits == 1) { g_how = "scan of /switch (one folder has libmain.so)"; goto done; }
      if (hits > 1)  { g_how = "scan of /switch -- WARNING: several folders "
                               "contain libmain.so, picked the first"; goto done; }
    }
  }

  /* --- 3. compile-time default ------------------------------------------ */
  snprintf(g_root, sizeof g_root, "%s", DATA_ROOT_DEFAULT);
  g_how = "compile-time default (nothing else matched)";

done:
  make_nodev();
  snprintf(g_casetest, sizeof g_casetest, "%s/.casetest", g_root);
}

const char *nx_root(void)        { if (!g_inited) nx_root_init(0, NULL); return g_root; }
const char *nx_root_nodev(void)  { if (!g_inited) nx_root_init(0, NULL); return g_root_nodev; }
const char *nx_root_how(void)    { return g_how; }
const char *nx_casetest_path(void){ if (!g_inited) nx_root_init(0, NULL); return g_casetest; }

/* Formatted path under the root. Rotating pool so a few can be live in one
 * expression (e.g. two args to one printf). Boot-path use only -- anything on
 * the file-I/O hot path should use nx_root() and format into its own buffer. */
const char *nx_rp(const char *rel) {
  #define NX_RP_SLOTS 8
  static char pool[NX_RP_SLOTS][NX_ROOT_MAX + 256];
  static unsigned turn = 0;
  char *b = pool[turn++ % NX_RP_SLOTS];
  if (!g_inited) nx_root_init(0, NULL);
  if (!rel) rel = "";
  snprintf(b, NX_ROOT_MAX + 256, "%s%s%s",
           g_root, (*rel && *rel != '/') ? "/" : "", rel);
  return b;
}
