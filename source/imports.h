/* imports.h -- .so import resolution
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

extern FILE *stderr_fake;
extern DynLibFunction dynlib_functions[];
uintptr_t dynlib_find_export(const char *name);  /* search shim table (for dlsym) */
extern size_t dynlib_numfunctions;

void update_imports(void);

// relocate `mod` and resolve its imports against dynlib_functions[] (and the
// other already-loaded modules). Used for both libc++_shared.so and libcrx.so.
void crx_resolve_imports(so_module *mod);

#endif
