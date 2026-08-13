/* libc_shim.h -- bionic-compatible libc wrappers for libcrx.so + libc++_shared
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __LIBC_SHIM_H__
#define __LIBC_SHIM_H__

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>

// Return `p` with any leading "device:" (e.g. "sdmc:") stripped, so the result
// is a Unix-rooted path ("/switch/zookeeper"). Paths handed to managed code
// (Mono/IL2CPP) MUST be device-less: ":" isn't a Unix root marker, so a
// "sdmc:/..." path is treated as relative and Path.Combine() concatenates it
// after a relative asset path -> "assets/bin/Data/sdmc:/switch/zookeeper",
// which newlib then mis-parses (embedded device) and null-derefs. newlib still
// resolves device-less absolute paths via the default device (sdmc, set by our
// boot chdir), so file I/O is unaffected.
const char *managed_path(const char *p);

// fortify
void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen);
void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen);
char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen);
char *__strchr_chk_fake(const char *s, int c, size_t slen);
char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen);
size_t __strlen_chk_fake(const char *s, size_t slen);
char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dstlen, size_t srclen);
char *__strrchr_chk_fake(const char *s, int c, size_t slen);
int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va);
int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va);
int __snprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, ...);
int __sprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, ...);
int  __open_2_fake(const char *path, int flags);
long __read_chk_fake(int fd, void *buf, size_t count, size_t buflen);
long __pread_chk_fake(int fd, void *buf, size_t count, long off, size_t buflen);
void __FD_SET_chk_fake(int fd, void *set, size_t setlen);
int  __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen);

// misc bionic
int __system_property_get_fake(const char *name, char *value);
unsigned long getauxval_fake(unsigned long type);
int gettid_fake(void);
long syscall_fake(long number, ...);
void sincosf_fake(float x, float *s, float *c);
int sched_get_priority_max_fake(int policy);
int sched_get_priority_min_fake(int policy);
void android_set_abort_message_fake(const char *msg);
size_t __ctype_get_mb_cur_max_fake(void);
int __register_atfork_fake(void);
int __cxa_thread_atexit_impl_fake(void (*fn)(void *), void *arg, void *dso);
/* Interposed C++ throw: logs the managed exception type, then forwards to
 * libil2cpp's real __cxa_throw. Never returns. */
__attribute__((noreturn))
void nx_cxa_throw(void *ex, void *tinfo, void (*dtor)(void *));
unsigned nx_throw_count(void);
long sysconf_fake(int name);
long pathconf_fake(const char *path, int name);

// fs
int open_fake(const char *path, int flags, ...);
int mkdir_fake(const char *path, unsigned mode);
int rename_fake(const char *from, const char *to);
int openat_fake(int dirfd, const char *path, int flags, ...);
int unlinkat_fake(int dirfd, const char *path, int flags);
struct bionic_stat;
int stat_fake(const char *path, struct bionic_stat *st);
int fstat_fake(int fd, struct bionic_stat *st);
int lstat_fake(const char *path, struct bionic_stat *st);
void *readdir_fake(void *dirp);
void *opendir_fake(const char *path);
int   msync_fake(void *addr, size_t length, int flags);
/* Reduce any spelling of a game path to the pack key "assets/...",
 * or NULL if it is not a packed game path. Shared so every pack lookup
 * uses one normaliser -- see rounds 84/86/88. */
const char *nx_pack_relpath(const char *path);
/* stdio here is serialised on an internal lock -- always use this rather
 * than raw fmemopen() from other translation units. */
FILE *fmemopen_locked(void *buf, size_t n, const char *mode);
int   closedir_fake(void *dirp);
/* arena usage in MB (reserved address space / committed physical / total) */
void  nx_arena_usage_mb(unsigned *reserved_mb, unsigned *committed_mb,
                        unsigned *total_mb);
char *realpath_fake(const char *path, char *resolved);
int strerror_r_fake(int err, char *buf, size_t len);
int statvfs_fake(const char *path, void *buf);
int statfs_fake(const char *path, void *buf);
FILE *fopen_fake(const char *path, const char *mode);

/* Mutex-locked open/close over newlib's process-wide FILE table. The engine's
 * worker threads reach it through fopen_fake/fclose_fake; anything else in the
 * port that opens files while the game is running (nx_pointer's pointer.cfg
 * writes) must use these so the two never collide. */
FILE *nx_fopen_locked(const char *path, const char *mode);
int   nx_fclose_locked(FILE *f);

// locale
void *newlocale_fake(int mask, const char *locale, void *base);
void freelocale_fake(void *loc);
void *uselocale_fake(void *loc);
int iswalpha_l_fake(int wc, void *loc); int iswblank_l_fake(int wc, void *loc);
int iswcntrl_l_fake(int wc, void *loc); int iswdigit_l_fake(int wc, void *loc);
int iswlower_l_fake(int wc, void *loc); int iswprint_l_fake(int wc, void *loc);
int iswpunct_l_fake(int wc, void *loc); int iswspace_l_fake(int wc, void *loc);
int iswupper_l_fake(int wc, void *loc); int iswxdigit_l_fake(int wc, void *loc);
int towlower_l_fake(int wc, void *loc); int towupper_l_fake(int wc, void *loc);
int strcoll_l_fake(const char *a, const char *b, void *loc);
size_t strxfrm_l_fake(char *dst, const char *src, size_t n, void *loc);
size_t strftime_l_fake(char *s, size_t max, const char *fmt, const void *tm, void *loc);
long double strtold_l_fake(const char *s, char **end, void *loc);
long long strtoll_l_fake(const char *s, char **end, int base, void *loc);
unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc);
int wcscoll_l_fake(const wchar_t *a, const wchar_t *b, void *loc);
size_t wcsxfrm_l_fake(wchar_t *dst, const wchar_t *src, size_t n, void *loc);
size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps);
size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps);

// stdio over fake __sF
extern uint8_t fake_sF[3][0x100];
size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f);
size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f);
int fputc_fake(int c, FILE *f);
int fputs_fake(const char *s, FILE *f);
int fflush_fake(FILE *f);
int fclose_fake(FILE *f);
int ferror_fake(FILE *f);
int feof_fake(FILE *f);
int fileno_fake(FILE *f);
int fseek_fake(FILE *f, long off, int whence);
long ftell_fake(FILE *f);
int getc_fake(FILE *f);
int fgetc_fake(FILE *f);
char *fgets_fake(char *s, int n, FILE *f);
int ungetc_fake(int c, FILE *f);
void setbuf_fake(FILE *f, char *buf);
int fprintf_fake(FILE *f, const char *fmt, ...);
int vfprintf_fake(FILE *f, const char *fmt, va_list va);

// memory
int posix_memalign_fake(void **out, size_t align, size_t size);
void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd, long offset);
int munmap_fake(void *addr, size_t length);
int mprotect_fake(void *addr, size_t len, int prot);
int madvise_fake(void *addr, size_t len, int advice);

// fd routing (fake pipe vs real files)
long read_fake(int fd, void *buf, size_t count);
long z_lseek(int fd, long off, int whence);   /* real lseek; also services lseek64 */
extern int g_watch_fd;
void watch_dump(const char *tag, int fd, long a, long b, const void *buf, long got);
long write_fake(int fd, const void *buf, size_t count);
int  close_fake(int fd);
int  pipe_fake(int fds[2]);
int  poll_fake(void *fds, unsigned long nfds, int timeout);
int  select_fake(int n, void *r, void *w, void *e, void *t);

// networking (offline stubs)
int socket_fake(int d, int t, int p);
int connect_fake(int s, const void *a, unsigned l);
int bind_fake(int s, const void *a, unsigned l);
int listen_fake(int s, int b);
int accept_fake(int s, void *a, void *l);
long send_fake(int s, const void *b, size_t l, int f);
long recv_fake(int s, void *b, size_t l, int f);
long sendto_fake(int s, const void *b, size_t l, int f, const void *a, unsigned al);
long recvfrom_fake(int s, void *b, size_t l, int f, void *a, void *al);
int shutdown_fake(int s, int how);
int setsockopt_fake(int s, int lv, int n, const void *v, unsigned l);
int getsockopt_fake(int s, int lv, int n, void *v, void *l);
int getsockname_fake(int s, void *a, void *l);
int getpeername_fake(int s, void *a, void *l);
int getaddrinfo_fake(const char *node, const char *svc, const void *hints, void **res);
void freeaddrinfo_fake(void *res);
int getnameinfo_fake(const void *a, unsigned al, char *h, unsigned hl, char *s, unsigned sl, int f);
int gethostname_fake(char *name, size_t len);
void *getservbyname_fake(const char *n, const char *p);
unsigned if_nametoindex_fake(const char *n);
char *if_indextoname_fake(unsigned i, char *buf);
int *__get_h_errno_fake(void);

// process control (stubs)
int fork_fake(void);
int execvp_fake(const char *f, char *const argv[]);
int waitpid_fake(int pid, int *status, int opts);
int kill_fake(int pid, int sig);
int getpid_fake(void);
int sched_yield_fake(void);
void *getpwuid_fake(int uid);
char *getenv_fake(const char *name);
char *getcwd_fake(char *buf, size_t size);
int getrusage_fake(int who, void *usage);

// dynamic loader
void *dlopen_fake(const char *name, int flags);
int dlclose_fake(void *h);
const char *dlerror_fake(void);
void *dlsym_fake(void *handle, const char *symbol);

// pthread extras
int pthread_rwlock_rdlock_fake(void **rw);
int pthread_rwlock_wrlock_fake(void **rw);
int pthread_rwlock_unlock_fake(void **rw);
int sem_init_fake(void **s, int pshared, unsigned int value);
int sem_destroy_fake(void **s);
int sem_post_fake(void **s);
int sem_wait_fake(void **s);
int sem_trywait_fake(void **s);
int sem_timedwait_fake(void **s, const struct timespec *abs);
int sem_getvalue_fake(void **s, int *val);

/* Boehm GC stop-the-world bridge (see libc_shim.c). Set g_il2cpp_base to the
 * loaded libil2cpp exec base once it is mapped; pthread_kill_gc then answers the
 * GC's suspend/restart signals by posting the ack semaphore the (undeliverable)
 * signal handler would have posted. */
extern uintptr_t g_il2cpp_base;
int pthread_kill_gc(pthread_t t, int sig);

#endif
