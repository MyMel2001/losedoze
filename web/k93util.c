/* k93util.c - shared helpers */
#define _GNU_SOURCE
#include "k93util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

void *k93_xmalloc(size_t n)
{
    void *p = calloc(1, n ? n : 1);
    if (!p) { fprintf(stderr, "k93: out of memory (%zu)\n", n); exit(200); }
    return p;
}

char *k93_strdup(const char *s) { return s ? strdup(s) : NULL; }

size_t k93_strlcpy(char *d, const char *s, size_t n)
{
    size_t l = s ? strlen(s) : 0;
    if (n) {
        size_t c = l < n - 1 ? l : n - 1;
        if (s) memcpy(d, s, c);
        d[c] = 0;
    }
    return l;
}

int k93_starts_with(const char *s, const char *pfx)
{
    return s && pfx && strncmp(s, pfx, strlen(pfx)) == 0;
}

char *k93_trim(char *s)
{
    if (!s) return s;
    char *e;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    if (!*s) return s;
    e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r')) *e-- = 0;
    return s;
}

char *k93_read_file(const char *path, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) { close(fd); return NULL; }
    size_t n = (size_t)st.st_size;
    char *buf = k93_xmalloc(n + 1);
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

int k93_write_file(const char *path, const void *data, size_t len, int append)
{
    int fd = open(path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
    if (fd < 0) return -1;
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) { close(fd); return -1; }
        p += w; len -= (size_t)w;
    }
    close(fd);
    return 0;
}

int k93_mkdir_p(const char *path, mode_t mode)
{
    char tmp[1024];
    k93_strlcpy(tmp, path, sizeof tmp);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1; *p = '/'; }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST) return -1;
    return 0;
}

int k93_copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    struct stat st;
    if (fstat(in, &st) < 0) { close(in); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : (st.st_mode & 07777));
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t r;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        char *p = buf;
        while (r > 0) {
            ssize_t w = write(out, p, (size_t)r);
            if (w <= 0) { close(in); close(out); return -1; }
            p += w; r -= w;
        }
    }
    close(in);
    close(out);
    return r < 0 ? -1 : 0;
}

int k93_copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        if (k93_mkdir_p(dst, 0755) < 0) return -1;
        DIR *d = opendir(src);
        if (!d) return -1;
        struct dirent *e;
        int rc = 0;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char s2[2048], d2[2048];
            snprintf(s2, sizeof s2, "%s/%s", src, e->d_name);
            snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name);
            if (k93_copy_tree(s2, d2) < 0) rc = -1;
        }
        closedir(d);
        return rc;
    }
    if (S_ISLNK(st.st_mode)) {
        char t[1024];
        ssize_t n = readlink(src, t, sizeof t - 1);
        if (n < 0) return -1;
        t[n] = 0;
        unlink(dst);
        return symlink(t, dst);
    }
    return k93_copy_file(src, dst, st.st_mode & 07777);
}

int k93_rm_tree(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (!d) return -1;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char p2[2048];
            snprintf(p2, sizeof p2, "%s/%s", path, e->d_name);
            k93_rm_tree(p2);
        }
        closedir(d);
        return rmdir(path);
    }
    return unlink(path);
}

void k93_log(const char *tag, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stderr, "[%s] %s: %s\n", ts, tag, line);
    FILE *f = fopen("/var/log/k93.log", "a");
    if (f) { fprintf(f, "[%s] %s: %s\n", ts, tag, line); fclose(f); }
}

const char *k93_cmdline_arg(const char *key)
{
    static char val[256];
    size_t klen = strlen(key);
    size_t len = 0;
    char *cl = k93_read_file("/proc/cmdline", &len);
    if (!cl) return NULL;
    char *p = cl, *res = NULL;
    while (p && *p) {
        char *sp = strchr(p, ' ');
        if (sp) *sp = 0;
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            k93_strlcpy(val, p + klen + 1, sizeof val);
            res = val;
            break;
        }
        p = sp ? sp + 1 : NULL;
    }
    free(cl);
    return res;
}

int k93_cmdline_flag(const char *key)
{
    size_t len = 0;
    char *cl = k93_read_file("/proc/cmdline", &len);
    if (!cl) return 0;
    int found = 0;
    char *p = cl;
    size_t klen = strlen(key);
    while (p && *p) {
        char *sp = strchr(p, ' ');
        if (sp) *sp = 0;
        if (!strcmp(p, key) || (!strncmp(p, key, klen) && p[klen] == '=')) { found = 1; break; }
        p = sp ? sp + 1 : NULL;
    }
    free(cl);
    return found;
}

long k93_sysinfo_long(const char *path)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strtol(buf, NULL, 0);
}

int k93_human_size(char *out, size_t n, unsigned long long bytes)
{
    const char *u[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double v = (double)bytes;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    if (i == 0) return snprintf(out, n, "%llu %s", bytes, u[0]);
    return snprintf(out, n, "%.1f %s", v, u[i]);
}
