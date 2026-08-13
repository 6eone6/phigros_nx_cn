/* jni_trace.h -- drop-in tracer: dump every class/method/signature the engine
 * asks for, so the unity_jni.c CHECK guesses get resolved from real data.
 *
 * Why: jni_fake.c routes calls by strcmp on method name/sig. The fastest way to
 * learn the exact names/sigs Unity uses (and the ORDER it calls them) is to log
 * them on the first on-hardware run, then grep the log. Beats guessing.
 *
 * INSTALL (three one-line edits in jni_fake.c):
 *
 *   #include "jni_trace.h"
 *
 *   // in j_FindClass(), before the return:
 *       jni_trace("FindClass", name, NULL, NULL);
 *
 *   // in j_GetMethodID() and j_GetFieldID(), before the return:
 *       jni_trace("Method", class_name_of(cls), name, sig);
 *
 * Output goes to BOTH nxlink (if you ran `nxlink -s`) and
 * /switch/phigros/jni_trace.log so you can pull it off the SD card.
 *
 * Turn off by defining JNI_TRACE_OFF before including, or just remove the calls.
 */
#ifndef JNI_TRACE_H
#define JNI_TRACE_H

#include <stdio.h>

#ifdef JNI_TRACE_OFF
static inline void jni_trace(const char *w, const char *c, const char *n, const char *s){
  (void)w;(void)c;(void)n;(void)s;
}
#else
static inline void jni_trace(const char *what, const char *cls,
                             const char *name, const char *sig){
  static FILE *log = NULL;
  static int   tried = 0;
  if (!log && !tried){ tried = 1; log = fopen("/switch/phigros/jni_trace.log", "a"); }

  if (name)  /* a method/field lookup */
    printf      ("[JNI] %-9s %s.%s %s\n", what, cls ? cls : "?", name, sig ? sig : "");
  else
    printf      ("[JNI] %-9s %s\n",       what, cls ? cls : "?");

  if (log){
    if (name) fprintf(log, "%-9s %s.%s %s\n", what, cls ? cls : "?", name, sig ? sig : "");
    else      fprintf(log, "%-9s %s\n",       what, cls ? cls : "?");
    fflush(log);  /* flush every line: a crash mid-bringup still leaves the trail */
  }
}
#endif /* JNI_TRACE_OFF */

#endif /* JNI_TRACE_H */
