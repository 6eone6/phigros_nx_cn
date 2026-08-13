/* phigros_assets.h -- GENERATED. Real asset layout for Phigros, measured from
 * a complete extracted copy of the game's assets/ tree.
 *
 * This replaces the guesswork the tree shipped with. The Fruit Ninja base
 * probed for `data.unity3d` and a list of `.splitN` parts; the first draft of
 * this port probed five plausible Addressables catalog locations. Neither was
 * right, and a wrong probe here does not fail loudly -- it lets the engine
 * start and then hang waiting for content that is not where it looked.
 *
 * MEASURED LAYOUT
 *   assets/bin/Data/           441 files     330.0 MB   classic multi-file layout
 *     (no data.unity3d, no .splitN parts -- both confirmed absent)
 *   assets/aa/                   3 files       3.7 MB   Addressables spine
 *   assets/aa/Android/        2514 files    2457.8 MB   content bundles
 *   TOTAL                     2958 files      2.79 GB
 *
 * ADDRESSABLES: FULLY LOCAL -- this was the biggest open risk and it is closed.
 *   - catalog.json contains ZERO http/https URLs, zero `jar:`, zero `file://`.
 *   - every m_InternalId is rooted at the RuntimePath token, i.e. resolved
 *     against the local runtime path, never fetched.
 *   - catalog references 2514 bundles; 2514 are present; 0 missing, 0 extra.
 *   So there is no UnityWebRequest that can hang the boot waiting on a remote
 *   catalog. The remaining Addressables risk is purely path resolution --
 *   see the RuntimePath note in libc_shim.c's nx_pack_relpath().
 */
#ifndef PHIGROS_ASSETS_H
#define PHIGROS_ASSETS_H

#include <stdint.h>

/* ---- files that MUST exist, with their known sizes -------------------
 * size 0 means "presence only, do not size-check" (user may have a
 * different game version). A size mismatch is reported as a WARNING, a
 * missing file as a fatal error. */
typedef struct { const char *path; uint64_t size; const char *what; } PhiAssetReq;
static const PhiAssetReq PHI_REQUIRED_ASSETS[] = {
  { "assets/bin/Data/globalgamemanagers", 171340ull, "Unity engine entry point" },
  { "assets/bin/Data/globalgamemanagers.assets", 184376ull, "engine boot assets" },
  { "assets/bin/Data/Managed/Metadata/global-metadata.dat", 10310660ull, "IL2CPP metadata" },
  { "assets/bin/Data/unity_app_guid", 36ull, "app guid" },
  { "assets/aa/catalog.json", 3735915ull, "Addressables catalog" },
  { "assets/aa/settings.json", 1594ull, "Addressables settings" },
  { "assets/aa/AddressablesLink/link.xml", 6063ull, "Addressables link map" },
};
#define PHI_REQUIRED_ASSETS_N ((int)(sizeof(PHI_REQUIRED_ASSETS)/sizeof(PHI_REQUIRED_ASSETS[0])))

/* ---- Addressables ---------------------------------------------------- */
#define PHI_AA_ROOT          "assets/aa"
#define PHI_AA_CATALOG       "assets/aa/catalog.json"
#define PHI_AA_SETTINGS      "assets/aa/settings.json"
#define PHI_AA_BUNDLE_DIR    "assets/aa/Android"
#define PHI_AA_BUNDLE_COUNT  2514   /* referenced by catalog AND on disk */
#define PHI_AA_BUNDLE_EXT    ".bundle"

/* ---- totals, for the staging sanity check ---------------------------- */
#define PHI_ASSET_FILE_COUNT 2958
#define PHI_ASSET_TOTAL_BYTES 2791569590ull

#endif /* PHIGROS_ASSETS_H */
