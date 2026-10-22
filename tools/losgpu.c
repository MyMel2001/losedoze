/* losgpu.c — guest GPU probe.
 *
 * Lists the DRM devices and the in-tree kernel driver bound to each: the
 * open-source GPU stack is bochs (QEMU std VGA), virtio-gpu, i915/xe, amdgpu,
 * nouveau and radeon. Markers:
 *
 *   LOSEDOZE GPU OK card0=virtio_gpu card1=bochs_drm modes=3 connectors=2
 *   LOSEDOZE GPU SKIP (no DRM card)
 *
 * SKIP is honest: no DRM device means no display at all.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *driver_of(const char *card, char *buf, size_t n) {
  char p[600];
  snprintf(p, sizeof p, "/sys/class/drm/%s/device/driver", card);
  ssize_t r = readlink(p, buf, n - 1);
  if (r <= 0) return "none";
  buf[r] = 0;
  char *s = strrchr(buf, '/');
  return s ? s + 1 : buf;
}

int main(void) {
  DIR *d = opendir("/sys/class/drm");
  if (!d) { printf("LOSEDOZE GPU SKIP (no /sys/class/drm)\n"); return 0; }
  char cards[512]; cards[0] = 0;
  int nc = 0, nconn = 0, modes = 0;
  struct dirent *e;
  while ((e = readdir(d))) {
    if (strncmp(e->d_name, "card", 4)) continue;
    char dev[600];
    snprintf(dev, sizeof dev, "/sys/class/drm/%s/device", e->d_name);
    if (access(dev, F_OK)) continue;            /* render node, not a card */
    char one[300], dbuf[256];
    snprintf(one, sizeof one, "%s=%s", e->d_name,
             driver_of(e->d_name, dbuf, sizeof dbuf));
    if (nc) strncat(cards, " ", sizeof cards - strlen(cards) - 1);
    strncat(cards, one, sizeof cards - strlen(cards) - 1);
    nc++;
  }
  rewinddir(d);
  while ((e = readdir(d))) {                    /* connectors: <card>-<name> */
    char dash = 0;
    for (char *p = e->d_name + 4; *p; p++) if (*p == '-') { dash = 1; break; }
    if (!dash) continue;
    nconn++;
    char mp[600];
    snprintf(mp, sizeof mp, "/sys/class/drm/%s/modes", e->d_name);
    FILE *f = fopen(mp, "r");
    if (!f) continue;
    char b[128]; int n = 0;
    while (fgets(b, sizeof b, f)) n++;
    fclose(f);
    if (n > modes) modes = n;
  }
  closedir(d);
  if (!nc) { printf("LOSEDOZE GPU SKIP (no DRM card)\n"); return 0; }
  printf("losgpu: %s modes=%d connectors=%d\n", cards, modes, nconn);
  printf("LOSEDOZE GPU OK %s modes=%d connectors=%d\n", cards, modes, nconn);
  return 0;
}
