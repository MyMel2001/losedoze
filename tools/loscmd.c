/* loscmd - batch script runner for Losedoze (.bat files).
 *
 * Usage: loscmd <script.bat>
 *
 * Supports the small cmd.exe subset the shell's demos and user scripts use:
 *   @echo off | @echo on, echo, rem, ver, title, cls, cd, dir, exit
 * Lines that name an executable (.exe) are launched through the host layer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define MAXLINES 256
#define LINE_MAX_LEN 1024

static char lines[MAXLINES][LINE_MAX_LEN];
static int nlines;
static int echo_on = 1;
static int script_exit;

static int read_script(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    nlines = 0;
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        size_t l = strlen(buf);
        while (l && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) buf[--l] = 0;
        if (nlines < MAXLINES) {
            strncpy(lines[nlines], buf, sizeof lines[0] - 1);
            lines[nlines][sizeof lines[0] - 1] = 0;
            nlines++;
        }
    }
    fclose(f);
    return 0;
}

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    return s;
}

static int starts(const char *t, const char *p) {
    size_t n = strlen(p);
    return strncasecmp(t, p, n) == 0 && (t[n] == 0 || t[n] == ' ' || t[n] == '\t');
}

static void run_exec(const char *tok) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int fd = open("/var/log/losedoze.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        execlp(tok, tok, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(p, &st, 0);
}

static void run_line(char *raw) {
    char line[LINE_MAX_LEN];
    strncpy(line, raw, sizeof line - 1);
    line[sizeof line - 1] = 0;
    char *t = trim(line);
    if (!*t) return;
    if (t[0] == '@' && (t[1] == 'e')) { echo_on = !strncasecmp(t, "@echo off", 9); return; }
    if (starts(t, "rem") || starts(t, "::")) return;
    if (starts(t, "echo")) {
        char *e = t + 4;
        while (*e == ' ' || *e == '\t') e++;
        if (echo_on) printf("%s\n", e);
        return;
    }
    if (starts(t, "ver")) { puts("Microsoft Windows [Version 10.0.19045.3803]"); return; }
    if (starts(t, "title")) return;
    if (starts(t, "cls")) { putchar('\f'); return; }
    if (starts(t, "cd")) {
        char *d = t + 2;
        while (*d == ' ' || *d == '\t') d++;
        if (*d) chdir(d);
        return;
    }
    if (starts(t, "dir")) {
        DIR *op = opendir(".");
        if (op) {
            struct dirent *e;
            while ((e = readdir(op))) {
                if (e->d_name[0] == '.') continue;
                printf("%s\r\n", e->d_name);
            }
            closedir(op);
        }
        return;
    }
    if (starts(t, "exit")) { script_exit = 1; return; }
    /* bare-word exec: <prog>.exe [args...] */
    char *tok = t;
    while (*tok && *tok != ' ' && *tok != '\t') tok++;
    size_t tl = (size_t)(tok - t);
    char exe[512];
    strncpy(exe, t, tl);
    exe[tl] = 0;
    size_t el = strlen(exe);
    if (el > 4 && !strcasecmp(exe + el - 4, ".exe")) run_exec(exe);
}

static int selftest(void);
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc < 2) { fprintf(stderr, "usage: loscmd <script.bat>\n"); return 2; }
    if (read_script(argv[1]) < 0) { fprintf(stderr, "loscmd: cannot open %s\n", argv[1]); return 1; }
    for (int i = 0; i < nlines; i++) {
        script_exit = 0;
        run_line(lines[i]);
        if (script_exit) break;
    }
    return 0;
}

static int selftest(void) {
    FILE *f = fopen("/tmp/loscmd_selftest.bat", "w");
    if (!f) return 1;
    fputs("@echo off\n", f);
    fputs("echo loscmd-selftest-ok\n", f);
    fputs("exit\n", f);
    fclose(f);
    if (freopen("/tmp/loscmd_selftest.out", "w", stdout) == NULL) return 1;
    if (read_script("/tmp/loscmd_selftest.bat") < 0) return 1;
    script_exit = 0;
    for (int i = 0; i < nlines; i++) {
        run_line(lines[i]);
        if (script_exit) break;
    }
    fclose(stdout);
    FILE *o = fopen("/tmp/loscmd_selftest.out", "r");
    char buf[256];
    int ok = 0;
    if (o) {
        size_t r = fread(buf, 1, sizeof buf - 1, o);
        buf[r] = 0;
        fclose(o);
        if (strstr(buf, "loscmd-selftest-ok")) ok = 1;
    }
    remove("/tmp/loscmd_selftest.bat");
    remove("/tmp/loscmd_selftest.out");
    fprintf(stderr, "selftest %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
