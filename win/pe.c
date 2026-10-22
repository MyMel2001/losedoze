/* pe.c — PE image loader for the losedoze homemade Win32 subsystem.
 * Parses PE32 (i386) and PE32+ (AMD64) images, maps and relocates them, binds
 * imports to host builtin DLL export tables and to PE DLLs loaded from disk
 * (exports resolved from the PE export directory). An image executes only in
 * the host of its own architecture (w32host: PE32+, w32host32: PE32); a
 * cross-arch image is parsed and relocated but its imports are not bound.
 * Original code; PE format facts only. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#include "host.h"

#define MZ 0x5a4d
#define PE_SIG 0x00004550
#define PE32 0x10b
#define PE32P 0x20b
#define IMG_FILE_MACHINE_I386 0x14c
#define IMG_FILE_MACHINE_AMD64 0x8664
#define IMAGE_FILE_DLL_FLAG 0x2000
#define DIR_EXPORT 0
#define DIR_IMPORT 1
#define DIR_BASERELOC 5
#define REL_HIGHLOW 3
#define REL_DIR64 10
#define ORD32 0x80000000u
#define ORD64 0x8000000000000000ull
#define DLL_PROCESS_DETACH 0
#define DLL_PROCESS_ATTACH 1
#define SCN_MEM_EXECUTE 0x20000000u
#define SCN_MEM_READ 0x40000000u
#define SCN_MEM_WRITE 0x80000000u
#define DLL_MAGIC 0x4c4c5744u  /* "WDLL" */
#define PE_MAGIC 0x4d494550u   /* "PEIM" */
#define PE_HOST_BITS ((int)(8 * sizeof(void *))) /* this host's own width */

typedef struct DllEnt {
  W32 magic;
  char name[64];
  const char *const *funcs;
  void **fns;
} DllEnt;

struct PeImage {
  W32 magic;
  W8 *base;
  size_t size;
  void *entry;
  int relocated;
  int bits;                   /* 32 or 64 */
  int is_dll;
  int refcnt;
  W32 ord_base;
  W32 *exp_names;             /* RVAs of export names */
  W16 *exp_ords;              /* ordinal per name */
  W32 *exp_funcs;             /* RVAs of function entries */
  W32 nfunc, nnames;
  char name[256];
};

#define MAX_DLL 16
static DllEnt dlls[MAX_DLL];
static int ndll;
static PeImage *main_image;
static BOOL call_dllmain(PeImage *m, W32 reason);
#define MAX_MOD 16
static PeImage *mods[MAX_MOD];
static int nmod;

void pe_register_dll(const char *name, const char *const *funcs, void **fns) {
  if (ndll >= MAX_DLL) return;
  DllEnt *d = &dlls[ndll++];
  d->magic = DLL_MAGIC;
  snprintf(d->name, sizeof d->name, "%s", name);
  d->funcs = funcs; d->fns = fns;
}

static DllEnt *find_dll(const char *name) {
  for (int i = 0; i < ndll; i++)
    if (!strcasecmp(dlls[i].name, name)) return &dlls[i];
  return NULL;
}

HMODULE pe_dll_handle(const char *name) { return (HMODULE)find_dll(name); }

static W8 *pe_opt(PeImage *m) {
  return m->base + *(W32 *)(m->base + 0x3c) + 24;
}

/* nth data directory rva (size via *sz when non-NULL); bounds by magic. */
static W32 pe_ddir(PeImage *m, int idx, W32 *sz) {
  W8 *opt = pe_opt(m);
  int base = m->bits == 32 ? 96 : 112;
  W32 n = m->bits == 32 ? *(W32 *)(opt + 92) : *(W32 *)(opt + 108);
  if ((W32)idx >= n) { if (sz) *sz = 0; return 0; }
  W32 r = *(W32 *)(opt + base + idx * 8);
  if (sz) *sz = *(W32 *)(opt + base + idx * 8 + 4);
  return r;
}

static void *pe_rva(PeImage *m, W32 rva) {
  if (rva >= m->size) return NULL;
  return m->base + rva;
}

/* ---------------- exports ---------------- */

static void parse_exports(PeImage *m) {
  W32 sz = 0;
  W32 rva = pe_ddir(m, DIR_EXPORT, &sz);
  if (!rva || sz < 40) return;
  W8 *e = m->base + rva;
  if (rva + 40 > m->size) return;
  m->nfunc = *(W32 *)(e + 20);
  m->ord_base = *(W32 *)(e + 16);
  W32 nnames = *(W32 *)(e + 24);
  W32 f = *(W32 *)(e + 28), nn = *(W32 *)(e + 32), no = *(W32 *)(e + 36);
  if (f >= m->size || no >= m->size || nn >= m->size) return;
  if (m->nfunc > 4096 || nnames > 4096) return;
  if (m->nfunc && f + m->nfunc * 4 > m->size) return;
  if (nnames && (no + nnames * 2 > m->size || nn + nnames * 4 > m->size)) return;
  m->exp_funcs = (W32 *)(m->base + f);
  m->exp_ords = (W16 *)(m->base + no);
  m->exp_names = (W32 *)(m->base + nn);
  m->nnames = nnames;
}

FARPROC pe_get_export(HMODULE h, const char *name) {
  if (!h || !name || !*name) return NULL;
  W32 mag = *(W32 *)h;
  if (mag == DLL_MAGIC) {
    DllEnt *d = (DllEnt *)h;
    for (int i = 0; d->funcs[i]; i++)
      if (!strcmp(d->funcs[i], name)) return (FARPROC)d->fns[i];
    return NULL;
  }
  if (mag != PE_MAGIC) return NULL;
  PeImage *m = (PeImage *)h;
  if (name[0] == '#') {  /* by ordinal */
    unsigned long long ord = strtoull(name + 1, NULL, 10);
    W32 idx = (W32)ord - m->ord_base;
    if (ord >= m->ord_base && idx < m->nfunc) {
      void *p = pe_rva(m, m->exp_funcs[idx]);
      if (p) return (FARPROC)p;
    }
    return NULL;
  }
  for (W32 i = 0; i < m->nnames; i++) {
    const char *n = (const char *)pe_rva(m, m->exp_names[i]);
    if (!n) continue;
    if (!strcmp(n, name)) {
      void *p = pe_rva(m, m->exp_funcs[m->exp_ords[i]]);
      if (p) return (FARPROC)p;
    }
  }
  return NULL;
}

HMODULE pe_module(PeImage *m) { return (HMODULE)m; }

const char *pe_image_name(PeImage *m) { return m ? m->name : "?"; }

void *pe_entry(PeImage *m) {
  if (!m) return NULL;
  /* an image runs only in the host of its own architecture: 32-bit code
   * cannot execute in a 64-bit process (and vice versa) */
  return m->bits == PE_HOST_BITS ? m->entry : NULL;
}

/* ---------------- relocs / imports ---------------- */

static int apply_relocs(PeImage *m, W64 old_base) {
  W32 sz = 0;
  W32 ddir = pe_ddir(m, DIR_BASERELOC, &sz);
  if (!ddir || !sz) return -1; /* none present */
  W8 *p = m->base + ddir, *end = p + sz;
  W64S delta = (W64S)((W64)m->base - old_base);
  while (p + 8 <= end) {
    W32 pg = *(W32 *)p, blk = *(W32 *)(p + 4);
    if (blk < 8) break;
    for (W32 i = 8; i + 2 <= blk; i += 2) {
      W16 e = *(W16 *)(p + i);
      unsigned type = e >> 12, off = e & 0xfff;
      if (type == 0) continue;                      /* padding */
      if (m->bits == 64) {
        if (type != REL_DIR64) return -2;           /* unsupported relocation */
        W64 *slot = (W64 *)(m->base + pg + off);
        *slot = (W64)((W64S)*slot + delta);
      } else {
        if (type != REL_HIGHLOW) return -2;         /* i386: 32-bit high/low */
        W32 *slot = (W32 *)(m->base + pg + off);
        *slot = (W32)((W32S)*slot + (W32S)delta);
      }
    }
    p += blk;
  }
  return 0;
}

/* ---------------- PE DLLs on disk ---------------- */

static PeImage *pe_mod_by_name(const char *name) {
  const char *base = strrchr(name, '/');
  base = base ? base + 1 : name;
  for (int i = 0; i < nmod; i++)
    if (!strcasecmp(mods[i]->name, name) ||
        !strcasecmp(strrchr(mods[i]->name, '/') ? strrchr(mods[i]->name, '/') + 1
                                                : mods[i]->name, base))
      return mods[i];
  return NULL;
}

/* module by name: builtin tables, then already-loaded PE images */
HMODULE pe_find_module(const char *name) {
  HMODULE h = pe_dll_handle(name);
  if (h) return h;
  if (!name) return NULL;
  return (HMODULE)pe_mod_by_name(name);
}

static int search_dirs(char *out, size_t cap, const char *dir, const char *name) {
  if (!dir || !*dir) return 0;
  snprintf(out, cap, "%s/%s", dir, name);
  return access(out, R_OK) == 0;
}

static PeImage *pe_load_dll(const char *name, const char **err);

HMODULE pe_load_library(const char *name, const char **err) {
  *err = NULL;
  if (!name || !*name) { *err = "null name"; return NULL; }
  /* already loaded? */
  PeImage *m = pe_mod_by_name(name);
  if (m) { m->refcnt++; return (HMODULE)m; }
  return (HMODULE)pe_load_dll(name, err);
}

static PeImage *pe_load_dll(const char *name, const char **err) {
  char path[512];
  const char *dirs[8];
  int nd = 0;
  if (strchr(name, '/') || strchr(name, '\\')) {
    PeImage *m = pe_mod_by_name(name);
    if (m) { m->refcnt++; return m; }
    if (access(name, R_OK) == 0) { snprintf(path, sizeof path, "%s", name); }
    else { *err = "not found"; return NULL; }
  } else {
    if (main_image) {                     /* next to the running EXE */
      char tmp[512];
      snprintf(tmp, sizeof tmp, "%s", main_image->name);
      char *slash = strrchr(tmp, '/');
      if (slash) { *slash = 0; dirs[nd++] = strdup(tmp); }
    }
    const char *env = getenv("LOSEDOZE_DLL_PATH");
    if (env) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "%s", env);
      for (char *p = strtok(tmp, ":"); p && nd < 6; p = strtok(NULL, ":"))
        if (*p) dirs[nd++] = strdup(p);
    }
    if (nd < 8) dirs[nd++] = strdup("/opt/losedoze/lib");
    if (nd < 8) dirs[nd++] = strdup("/opt/losedoze/apps");
    int found = 0;
    for (int i = 0; i < nd && !found; i++) {
      if (search_dirs(path, sizeof path, dirs[i], name)) found = 1;
    }
    if (!found) {
      size_t l = strlen(name);
      if (l < 4 || strcasecmp(name + l - 4, ".dll") != 0) {
        char with[520];
        snprintf(with, sizeof with, "%s.dll", name);
        for (int i = 0; i < nd && !found; i++)
          if (search_dirs(path, sizeof path, dirs[i], with)) found = 1;
      }
    }
    for (int i = 0; i < nd; i++) free((void *)dirs[i]);
    if (!found) { *err = "not found on search path"; return NULL; }
  }
  PeImage *m = pe_load(path, err);   /* reuse the full loader, mark as DLL */
  if (!m) return NULL;
  m->is_dll = 1;
  if (m->bits != PE_HOST_BITS) {   /* wrong-arch DLL: never run */
    *err = m->bits == 32
               ? "32-bit x86 DLL: needs the 32-bit host (w32host32)"
               : "64-bit DLL: needs the 64-bit host (w32host)";
    m->refcnt = 1;
    return m;
  }
  if (!call_dllmain(m, DLL_PROCESS_ATTACH)) {
    *err = "DllMain(DLL_PROCESS_ATTACH) refused";
    return m;
  }
  m->refcnt = 1;
  return m;
}

BOOL pe_release(HMODULE h) {
  if (!h) return W32_FALSE;
  if (*(W32 *)h != PE_MAGIC) return W32_TRUE;   /* builtin: no-op */
  PeImage *m = (PeImage *)h;
  if (--m->refcnt > 0) return W32_TRUE;
  call_dllmain(m, DLL_PROCESS_DETACH);
  if (nmod) {
    for (int i = 0; i < nmod; i++)
      if (mods[i] == m) { mods[i] = mods[--nmod]; break; }
  }
  munmap(m->base, m->size);
  free(m);
  return W32_TRUE;
}

/* ---------------- imports ---------------- */

static HMODULE import_resolve(const char *dllname, const char **err) {
  HMODULE h = pe_dll_handle(dllname);
  if (h) return h;
  return pe_load_library(dllname, err);
}

static int bind_imports(PeImage *m) {
  /* cross-arch: the image parses, but its IAT must not be filled with this
   * host's (differently-ABI'd) function pointers */
  if (m->bits != PE_HOST_BITS) return 0;
  W32 sz = 0;
  W32 ddir = pe_ddir(m, DIR_IMPORT, &sz);
  if (!ddir || !sz) return 0;
  W8 *d = m->base + ddir, *end = d + sz;
  for (; d + 20 <= end; d += 20) {
    W32 oft = *(W32 *)d, name_rva = *(W32 *)(d + 12), ft = *(W32 *)(d + 16);
    if (!oft && !name_rva && !ft) break;
    if (name_rva >= m->size || ft >= m->size || (oft && oft >= m->size)) {
      fprintf(stderr, "pe: import descriptor out of range (name=%x ft=%x)\n",
              name_rva, ft);
      return -4;
    }
    const char *dllname = (const char *)(m->base + name_rva);
    const char *err = NULL;
    HMODULE mod = import_resolve(dllname, &err);
    if (!mod) {
      fprintf(stderr, "pe: unknown import dll '%s': %s\n", dllname,
              err ? err : "not found");
      return -1;
    }
    W64 *iat = (W64 *)(m->base + ft);
    W64 *int_ = (W64 *)(m->base + (oft ? oft : ft));
    for (size_t i = 0;; i++) {
      W64 t = 0;
      if (m->bits == 64) {
        t = int_[i];
        if (!t) break;
        if (t & ORD64) {
          fprintf(stderr, "pe: %s: import by ordinal %llu unsupported\n",
                  dllname, (unsigned long long)(t & 0xffff));
          return -2;
        }
      } else {
        W32 t32 = *(W32 *)((W8 *)int_ + i * 4);
        if (!t32) break;
        t = t32;
        if (t32 & ORD32) {
          fprintf(stderr, "pe: %s: import by ordinal %u unsupported\n", dllname,
                  t32 & 0xffff);
          return -2;
        }
      }
      const char *fname = (const char *)(m->base + (W32)t + 2); /* hint + name */
      if ((W32)t + 2 >= m->size) return -4;
      FARPROC fn = pe_get_export(mod, fname);
      if (!fn) {
        fprintf(stderr, "pe: missing import %s!%s\n", dllname, fname);
        return -3;
      }
      if (m->bits == 64) iat[i] = (W64)fn;
      else *(W32 *)((W8 *)iat + i * 4) = (W32)(W64)fn; /* 4-byte IAT slot */
    }
  }
  return 0;
}

static void set_section_prot(PeImage *m) {
  W32 lfanew = *(W32 *)(m->base + 0x3c);
  W16 nsec = *(W16 *)(m->base + lfanew + 6);
  W16 optsz = *(W16 *)(m->base + lfanew + 20);
  W8 *sec = m->base + lfanew + 24 + optsz;
  for (int i = 0; i < nsec; i++, sec += 40) {
    W32 va = *(W32 *)(sec + 12), vsz = *(W32 *)(sec + 8), ch = *(W32 *)(sec + 36);
    if (!va) continue;
    int pr = PROT_READ;
    if (ch & SCN_MEM_EXECUTE) pr |= PROT_EXEC;
    if (ch & SCN_MEM_WRITE) pr |= PROT_WRITE;
    W64 s = (W64)(va & ~0xfffu), e = (va + (vsz ? vsz : 4096) + 0xfff) & ~0xfffu;
    if (e > m->size) e = m->size;
    if (e > s) mprotect(m->base + s, (size_t)(e - s), pr);
  }
}

static PeImage *load_file(const char *path, const char **err, int as_dll) {
  *err = NULL;
  int fd = open(path, O_RDONLY);
  if (fd < 0) { *err = "open"; return NULL; }
  off_t fsz = lseek(fd, 0, SEEK_END);
  if (fsz < 0x200) { *err = "size"; close(fd); return NULL; }
  W8 *file = mmap(NULL, (size_t)fsz, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (file == MAP_FAILED) { *err = "mmap file"; return NULL; }

  if (*(W16 *)file != MZ) { *err = "not MZ"; goto fail; }
  W32 lfanew = *(W32 *)(file + 0x3c);
  if (lfanew + 24 > (W64)fsz || *(W32 *)(file + lfanew) != PE_SIG) {
    *err = "not PE"; goto fail;
  }
  W8 *nt = file + lfanew;
  W16 machine = *(W16 *)(nt + 4), nsec = *(W16 *)(nt + 6);
  W16 optsz = *(W16 *)(nt + 20);
  W16 chars = *(W16 *)(nt + 18);
  W8 *opt = nt + 24;
  W16 magic = *(W16 *)opt;
  int bits = 0;
  if (machine == IMG_FILE_MACHINE_AMD64 && magic == PE32P) bits = 64;
  else if (machine == IMG_FILE_MACHINE_I386 && magic == PE32) bits = 32;
  else { *err = "not a PE32 (i386) / PE32+ (AMD64) image"; goto fail; }
  W32 entry_rva = *(W32 *)(opt + 16);
  W64 pref = bits == 64 ? *(W64 *)(opt + 24) : *(W32 *)(opt + 28);
  W32 imgsz = *(W32 *)(opt + 56), hdrsz = *(W32 *)(opt + 60);
  if (!imgsz || imgsz > 256u * 1024 * 1024) { *err = "bad SizeOfImage"; goto fail; }

  W8 *base = NULL;
  /* the preferred base is only tried for the host's own architecture: on a
   * 32-bit host a PE32+ ImageBase does not even fit a pointer */
  if (pref && bits == PE_HOST_BITS) {
    base = mmap((void *)pref, imgsz, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base == MAP_FAILED) base = NULL;
  }
  if (!base) {
    base = mmap(NULL, imgsz, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) { *err = "mmap image"; goto fail; }
  }
  memset(base, 0, imgsz);
  memcpy(base, file, hdrsz < (W32)fsz ? hdrsz : (W32)fsz);

  W8 *sec = opt + optsz;
  for (int i = 0; i < nsec; i++, sec += 40) {
    W32 vsz = *(W32 *)(sec + 8), va = *(W32 *)(sec + 12);
    W32 rsz = *(W32 *)(sec + 16), praw = *(W32 *)(sec + 20);
    if (!va || va + vsz > imgsz) continue;
    size_t n = rsz;
    if (praw + n > (size_t)fsz) n = (size_t)fsz > praw ? (size_t)fsz - praw : 0;
    if (n > vsz) n = vsz;
    if (n) memcpy(base + va, file + praw, n);
  }
  munmap(file, (size_t)fsz);

  PeImage *m = calloc(1, sizeof *m);
  if (!m) { *err = "oom"; return NULL; }
  m->magic = PE_MAGIC;
  m->base = base;
  m->size = imgsz;
  m->bits = bits;
  m->is_dll = (chars & IMAGE_FILE_DLL_FLAG) != 0 || as_dll;
  snprintf(m->name, sizeof m->name, "%s", path);

  if ((W64)base != pref) {
    int r = apply_relocs(m, pref ? pref : (W64)base);
    /* a cross-arch image without .reloc is mapped wherever it landed — it is
     * parsed for report only, never executed */
    if (r == -2 || (r == -1 && bits == PE_HOST_BITS)) {
      *err = r == -1 ? "no base relocations: image cannot be rebased"
                     : "unsupported base relocation type";
      goto failimg;
    }
    if (r == 0) m->relocated = 1;
  }
  /* publish the main image before binding imports: the EXE's own directory is
   * where the DLLs it imports live (pe_load_dll searches it). */
  if (!as_dll && !main_image) main_image = m;
  parse_exports(m);
  if (bind_imports(m) != 0) { *err = "imports"; goto failimg; }
  m->entry = entry_rva ? base + entry_rva : NULL;
  set_section_prot(m);
  if (nmod < MAX_MOD) mods[nmod++] = m;
  return m;

failimg:
  if (main_image == m) main_image = NULL;
  munmap(m->base, m->size);
  free(m);
  return NULL;
fail:
  munmap(file, (size_t)fsz);
  return NULL;
}

PeImage *pe_load(const char *path, const char **err) {
  return load_file(path, err, 0);
}

/* ---------------- report / inspection ---------------- */

const char *pe_refuse(PeImage *m) {
  if (!m) return "?";
  if (m->bits != PE_HOST_BITS)
    return m->bits == 32
               ? "32-bit x86 (PE32) image: parsed, but its code needs a 32-bit "
                 "host: run it with w32host32"
               : "64-bit (PE32+) image: parsed, but its code needs a 64-bit "
                 "host: run it with w32host";
  return "image has no entry point";
}

void pe_inspect(PeImage *m) {
  W8 *opt = pe_opt(m);
  W64 pref = m->bits == 64 ? *(W64 *)(opt + 24) : *(W32 *)(opt + 28);
  W16 chars = *(W16 *)(m->base + *(W32 *)(m->base + 0x3c) + 18);
  W16 nsec = *(W16 *)(m->base + *(W32 *)(m->base + 0x3c) + 6);
  printf("pe: %s: PE32%s %d-bit (machine 0x%x magic 0x%x)%s\n", m->name,
         m->bits == 64 ? "+" : "", m->bits,
         m->bits == 64 ? IMG_FILE_MACHINE_AMD64 : IMG_FILE_MACHINE_I386,
         m->bits == 64 ? PE32P : PE32,
         (chars & IMAGE_FILE_DLL_FLAG) ? " DLL" : "");
  printf("pe: entry 0x%llx image 0x%zx %u sections: mapped at 0x%llx "
         "(preferred 0x%llx)%s\n",
         (unsigned long long)(m->entry ? (W64)((W8 *)m->entry - m->base) : 0),
         m->size, nsec, (unsigned long long)(W64)m->base,
         (unsigned long long)pref,
         m->relocated ? " relocated" : "");
  W32 sz = 0;
  W32 ddir = pe_ddir(m, DIR_IMPORT, &sz);
  if (ddir && sz) {
    W8 *d = m->base + ddir;
    printf("pe: imports:");
    for (; d + 20 <= m->base + ddir + sz; d += 20) {
      W32 name_rva = *(W32 *)(d + 12);
      if (!name_rva) break;
      printf(" %s", (const char *)(m->base + name_rva));
    }
    /* cross-arch: parsed, but this host's function pointers never go in */
    printf("%s\n", m->bits == PE_HOST_BITS ? ""
                                           : " (not bound: wrong architecture)");
  }
  if (m->nnames) {
    printf("pe: exports:");
    for (W32 i = 0; i < m->nnames; i++)
      printf(" %s", (const char *)pe_rva(m, m->exp_names[i]));
    printf("\n");
  }
  printf("pe: %d-bit code: %s\n", m->bits,
         m->bits == PE_HOST_BITS
             ? "executable by this host"
             : (m->bits == 32 ? "executable by w32host32" : "executable by w32host"));
  if (m->bits == 32) printf("LOSEDOZE PE32 PARSE OK %s\n", m->name);
}

static BOOL call_dllmain(PeImage *m, W32 reason) {
  typedef BOOL(W32API * DllMain)(void *hinst, W32 reason, void *reserved);
  if (m->bits != PE_HOST_BITS || !m->entry) return W32_TRUE;
  return ((DllMain)m->entry)(m->base, reason, NULL);
}
