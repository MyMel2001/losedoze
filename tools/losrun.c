/* losrun - launch a program from a Windows shortcut (.lnk).
 *
 * Usage: losrun <shortcut.lnk>
 *
 * Minimal LNK support: scan the file for the first path-like UTF-16LE string
 * that ends in .exe and launch it through the host layer.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int find_exe(const unsigned char *b, size_t n, char *out, size_t outn) {
    for (size_t i = 0; i + 1 < n; i++) {
        if (i % 2) continue;
        if (b[i] == 0) continue;
        unsigned char v = (unsigned char)(b[i] | (b[i + 1] << 8));
        if (!v) break;
        if (v != '/' && !isalpha(v)) continue;
        char buf[1024];
        size_t l = 0;
        size_t j = i;
        while (j + 1 < n) {
            unsigned char cv = (unsigned char)(b[j] | (b[j + 1] << 8));
            if (!cv) break;
            if (l + 1 < sizeof buf) buf[l++] = (char)cv;
            j += 2;
        }
        buf[l] = 0;
        size_t bl = strlen(buf);
        if (bl > 4 && strcasecmp(buf + bl - 4, ".exe") == 0) {
            strncpy(out, buf, outn - 1);
            out[outn - 1] = 0;
            return 1;
        }
    }
    return 0;
}

static void launch(const char *exe) {
    pid_t p = fork();
    if (p == 0) {
        setsid();
        int fd = open("/var/log/losedoze.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
        execlp(exe, exe, (char *)NULL);
        _exit(127);
    }
    int st;
    waitpid(p, &st, 0);
}

static int selftest(void);
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc < 2) { fprintf(stderr, "usage: losrun <shortcut.lnk>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "losrun: cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, f);
    buf[rd] = 0;
    fclose(f);
    char exe[1024];
    if (!find_exe(buf, (size_t)sz, exe, sizeof exe)) {
        fprintf(stderr, "losrun: no exe found in %s\n", argv[1]);
        return 1;
    }
    launch(exe);
    return 0;
}

static int selftest(void) {
    unsigned char buf[2048];
    memset(buf, 0, sizeof buf);
    const char *s = "C:\\losedoze\\apps\\hello.exe";
    for (size_t k = 0; s[k]; k++) { buf[2 * k] = (unsigned char)s[k]; buf[2 * k + 1] = 0; }
    char exe[1024];
    int ok = find_exe(buf, sizeof buf, exe, sizeof exe) &&
             (strlen(exe) == strlen(s)) && strcasecmp(exe, s) == 0;
    fprintf(stderr, "selftest %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 1;
}
