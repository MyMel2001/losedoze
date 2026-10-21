/* losyscall - query NT syscall numbers collected from public syscall tables.
 *
 * Usage:
 *   losyscall <name>          show number of <name> for every release
 *   losyscall <name> <arch>   x64 (default) or x86
 *   losyscall -l [subsystem]  list subsystem syscalls (nt, win32k)
 *
 * Reference/diagnostic tool: NT syscall numbers change every Windows build,
 * so this data helps when debugging Wine/Proton or anti-cheat traces.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "syscalls.inc"

static const NtSyscall *tabs[4];
static const char *const *rels[4];
static const char *tsub[4], *tarch[4];
static int ntabs;

static void add_tabs(const char *sub, const char *arch,
                     const NtSyscall *t, int n,
                     const char *const *r) {
    (void)n;
    tabs[ntabs] = t; rels[ntabs] = r; tsub[ntabs] = sub; tarch[ntabs] = arch;
    ntabs++;
}

#define ADD(sub, arch) add_tabs(#sub, #arch, tab_##sub##_##arch, \
    sizeof(tab_##sub##_##arch)/sizeof(NtSyscall), rel_##sub##_##arch)

static int selftest(void) {
    struct { const NtSyscall *t; const char *const *r; const char *n; const char *rel;
             unsigned short want; } cases[] = {
        { tab_nt_x64, rel_nt_x64, "NtAcceptConnectPort",
          "Windows 11 and Server (11 25H2)", 0x0002 },
        { tab_nt_x86, rel_nt_x86, "NtOpenFile",
          "Windows 10 (22H2)", 0x00f4 },
        { tab_win32k_x64, rel_win32k_x64, "NtUserCreateWindowEx",
          "Windows 11 and Server (11 25H2)", 0x106f },
    };
    int fail = 0;
    for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        int ri = -1, found = 0;
        unsigned short got = 0;
        for (int k = 0; cases[c].r[k]; k++)
            if (!strcmp(cases[c].r[k], cases[c].rel)) ri = k;
        for (int j = 0; cases[c].t[j].name; j++)
            if (!strcmp(cases[c].t[j].name, cases[c].n)) { found = 1; got = cases[c].t[j].num[ri]; }
        if (ri < 0 || !found || got != cases[c].want) {
            fprintf(stderr, "selftest FAIL: %s @ %s got 0x%04x want 0x%04x\n",
                    cases[c].n, cases[c].rel, got, cases[c].want);
            fail = 1;
        }
    }
    fprintf(stderr, "selftest %s\n", fail ? "FAIL" : "OK");
    return fail;
}

int main(int argc, char **argv) {
    ADD(nt, x64); ADD(nt, x86); ADD(win32k, x64); ADD(win32k, x86);

    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return selftest();

    if (argc < 2) {
        fprintf(stderr, "usage: losyscall <name> [x64|x86] | losyscall -l [nt|win32k]\n");
        return 2;
    }

    if (!strcmp(argv[1], "-l")) {
        const char *sub = argc > 2 ? argv[2] : NULL;
        for (int i = 0; i < ntabs; i++) {
            int cnt = 0;
            while (tabs[i][cnt].name) cnt++;
            if (!sub || !strcasecmp(sub, tsub[i])) {
                printf("%s/%s: %d entries\n", tsub[i], tarch[i], cnt);
                for (int j = 0; j < cnt; j++)
                    printf("  %s\n", tabs[i][j].name);
            }
        }
        return 0;
    }

    const char *want_arch = argc > 2 ? argv[2] : "x64";
    int found = 0;
    for (int i = 0; i < ntabs; i++) {
        if (strcasecmp(tarch[i], want_arch)) continue;
        for (int j = 0; tabs[i][j].name; j++) {
            if (strcasecmp(tabs[i][j].name, argv[1])) continue;
            found = 1;
            printf("%s (%s, %s)\n", tabs[i][j].name, tsub[i], tarch[i]);
            for (int k = 0; rels[i][k]; k++) {
                unsigned short n = tabs[i][j].num[k];
                if (n) printf("  %-45s 0x%04x\n", rels[i][k], n);
            }
        }
    }
    if (!found) {
        fprintf(stderr, "losyscall: '%s' not found in %s tables\n", argv[1], want_arch);
        return 1;
    }
    return 0;
}
