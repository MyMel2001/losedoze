/* k93util.h - tiny shared helpers (clean-room, POSIX only) */
#ifndef K93UTIL_H
#define K93UTIL_H
#include <stddef.h>
#include <sys/types.h>

void *k93_xmalloc(size_t n);
char *k93_read_file(const char *path, size_t *len);           /* NUL-terminated */
int   k93_write_file(const char *path, const void *data, size_t len, int append);
int   k93_mkdir_p(const char *path, mode_t mode);
int   k93_copy_file(const char *src, const char *dst, mode_t mode);
int   k93_copy_tree(const char *src, const char *dst);        /* recursive, regular files+dirs+symlinks */
int   k93_rm_tree(const char *path);
char *k93_strdup(const char *s);
size_t k93_strlcpy(char *d, const char *s, size_t n);
int   k93_starts_with(const char *s, const char *pfx);
char *k93_trim(char *s);
void  k93_log(const char *tag, const char *fmt, ...);
const char *k93_cmdline_arg(const char *key);                 /* value of key=... from /proc/cmdline, else NULL if bare key */
int   k93_cmdline_flag(const char *key);                      /* 1 if bare key present */
long  k93_sysinfo_long(const char *path);                     /* read a long from a sysfs/proc file, -1 on error */
int   k93_human_size(char *out, size_t n, unsigned long long bytes);
int   k93_calc_eval(const char *expr, double *out);   /* four-function + - * / ( ) */
int   k93_spell_known(const char *word);              /* 1 if dictionary knows it */
int   k93_spell_unknown(const char *text, char *out, size_t cap); /* unknown words */

#endif
