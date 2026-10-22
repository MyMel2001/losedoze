#!/usr/bin/env python3
"""Generate PE import libraries for the builtin DLLs (single source of truth:
the X-macro export lists in win/w32host.c). Uses llvm-lib (short import format).

usage: gen_importlibs.py win/w32host.c outdir
emits: outdir/{ntdll,kernel32,user32,gdi32}.def + lib*.a
"""
import re
import subprocess
import sys

MACROS = {"NT_LIST": "ntdll.dll", "K32_LIST": "kernel32.dll",
          "U32_LIST": "user32.dll", "G32_LIST": "gdi32.dll",
          "GL32_LIST": "opengl32.dll"}


def main(src_path, outdir):
    src = open(src_path).read()
    libs = []
    for macro, dll in MACROS.items():
        m = re.search(r"#define\s+%s\(X\)\s*\\\n(.*?)\n\n" % macro, src, re.S)
        if not m:
            sys.exit("macro %s not found" % macro)
        names = re.findall(r"X\((\w+)\)", m.group(1))
        deff = "%s/%s.def" % (outdir, dll.split(".")[0])
        with open(deff, "w") as f:
            f.write("LIBRARY %s\nEXPORTS\n" % dll)
            for n in names:
                f.write(n + "\n")
        lib = "%s/lib%s.a" % (outdir, dll.split(".")[0])
        subprocess.run(["llvm-lib", "/def:" + deff, "/machine:x64",
                        "/out:" + lib], check=True, capture_output=True)
        libs.append(lib)
    print("import libs:", " ".join(libs))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
