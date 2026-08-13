/* nx_root.h -- runtime data-root detection. See nx_root.c.
 *
 * The loader works from ANY folder under /switch: the root is whatever
 * directory the .nro was launched from (or, failing that, whichever folder
 * under /switch actually contains libmain.so).
 */
#ifndef NX_ROOT_H
#define NX_ROOT_H

/* Call once, first thing in main(), before the log opens. */
void        nx_root_init(int argc, char **argv);

const char *nx_root(void);           /* "sdmc:/switch/<folder>", no trailing / */
const char *nx_root_nodev(void);     /* same, device prefix stripped           */
const char *nx_root_how(void);       /* how it was found, for the boot log     */
const char *nx_casetest_path(void);  /* "<root>/.casetest"                     */

/* "<root>/rel" in a rotating buffer. Boot-path use; not for hot file I/O. */
const char *nx_rp(const char *rel);

#endif /* NX_ROOT_H */
