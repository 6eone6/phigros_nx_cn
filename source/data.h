/* data.h -- Chaos Rings 3 data layer (loose-file AAsset + path resolution)
 *
 * CR3 ships its data as Media.Vision ".mvgl" archives plus Play Asset Delivery
 * packs, not FF4's single encrypted obb. The engine (MVGL::Utilities::Fios)
 * builds paths like "<dir>/main.10007.android.mvgl" and opens the big archives
 * with fopen, while small assets (the F0001/F0002 fonts) come through
 * AAssetManager_open + AAsset_getBuffer. We point every Android dir-path query
 * at the game folder and serve loose files from there.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __DATA_H__
#define __DATA_H__

#include <stddef.h>
#include <stdint.h>

// derive and remember the game directory from argv[0] (the .nro path).
void data_init(const char *argv0);

// the absolute game directory, e.g. "sdmc:/switch/chaosring3". Never NULL.
const char *data_dir(void);

// build "<game dir>/<name>" into out. returns out.
char *data_path(const char *name, char *out, size_t outsz);

// 1 if the named file exists in the game directory.
int data_exists(const char *name);

// --- AAsset NDK API over loose files ----------------------------------------
struct AAssetManager;
void  *AAssetManager_fromJava(void *env, void *assetManager);
void  *AAssetManager_open(void *mgr, const char *filename, int mode);
const void *AAsset_getBuffer(void *asset);
int64_t AAsset_getLength(void *asset);
int64_t AAsset_getLength64(void *asset);
int    AAsset_read(void *asset, void *buf, size_t count);
long   AAsset_seek(void *asset, long off, int whence);
void   AAsset_close(void *asset);

#endif
