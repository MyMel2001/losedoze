#!/usr/bin/env python3
"""Emit a PE32 (i386) fixture image for the losedoze Win32 subsystem.

Why generated and not linked: this host's binutils knows the i386pe emulation
but ships no ldscripts/i386pe.x linker script, and there is no lld/mingw, so a
real 32-bit PE cannot be linked here.  The fixture is still a genuine PE32
image: DOS stub, PE header with an i386 optional header, one .text section, a
kernel32.dll import descriptor (32-bit thunk array) and a HIGHLOW base
relocation.  w32host parses it (map, relocation, imports, exports) and refuses
to execute it, which is exactly the dual-arch contract:

    PE32 (i386)  -> parsed, relocated, imports bound, NOT executed
    PE32+ (AMD64)-> parsed and executed

usage: gen_pe32.py out.exe
"""
import struct
import sys

IMAGEBASE = 0x1000      # below the host's mmap_min_addr (64 Ki), so the image
                        # is always mapped elsewhere -> the HIGHLOW base
                        # relocation path always runs (mmap refuses 0x1000)
SEC_RVA = 0x1000
HDRSZ = 0x200
IMGSZ = 0x2000

text = bytearray(0x1000)


def put(off, data):
    text[off - SEC_RVA:off - SEC_RVA + len(data)] = data


# .text layout (rva -> bytes)
put(0x1000, b"\xb8\x00\x00\x00\x00\xc3")            # mov eax,0 ; ret
ILT_RVA, NAME_RVA, RELOC_RVA = 0x1008, 0x1010, 0x1020
IMP_RVA, TERM_RVA, DLL_RVA = 0x1030, 0x1044, 0x1058
RELOCBLK_RVA = 0x1070
put(ILT_RVA, struct.pack("<II", NAME_RVA, 0))        # import lookup/address table
put(NAME_RVA, struct.pack("<H", 0) + b"ExitProcess\x00")
put(RELOC_RVA, struct.pack("<I", IMAGEBASE))         # HIGHLOW target
put(IMP_RVA, struct.pack("<IIIII", ILT_RVA, 0, 0, DLL_RVA, ILT_RVA))
put(TERM_RVA, b"\x00" * 20)                          # descriptor terminator
put(DLL_RVA, b"kernel32.dll\x00")
SEC_VSIZE = 0x80

stub = b"MZ" + b"\x00" * 0x3a + struct.pack("<I", 0x40)
dos = stub

# PE32 optional header
opt = struct.pack(
    "<HBB" + "I" * 9 + "H" * 6 + "I" * 4 + "HH" + "I" * 6,
    0x10B, 0, 0,                    # magic, linker version
    0x200, SEC_VSIZE, 0,            # SizeOfCode/InitData/UninitData
    SEC_RVA, SEC_RVA, SEC_RVA,      # entry, BaseOfCode, BaseOfData
    IMAGEBASE,                      # ImageBase (32-bit)
    0x1000, 0x200,                  # SectionAlignment, FileAlignment
    4, 0, 0, 0, 4, 0,               # OS/image/subsystem versions
    0,                              # Win32VersionValue
    IMGSZ, HDRSZ, 0,                # SizeOfImage, SizeOfHeaders, checksum
    3,                              # subsystem: console
    0,                              # DllCharacteristics
    0x100000, 0x1000, 0x100000, 0x1000,  # stack/heap reserve+commit
    0, 16,                          # LoaderFlags, NumberOfRvaAndSizes
)
dirs = [(0, 0)] * 16
dirs[1] = (IMP_RVA, 40)                             # import directory
dirs[5] = (RELOCBLK_RVA, 12)                        # base reloc directory
opt += b"".join(struct.pack("<II", r, s) for r, s in dirs)
assert len(opt) == 224, len(opt)

# reloc block: page SEC_RVA, block size 12, one HIGHLOW(3) entry at offset 0x20
relocs = struct.pack("<IIHH", SEC_RVA, 12, (3 << 12) | (RELOC_RVA - SEC_RVA), 0)
assert len(relocs) == 12
put(RELOCBLK_RVA, relocs)
sec = b".text" + b"\x00" * 3 + struct.pack(
    "<IIIIIIHHI", SEC_VSIZE, SEC_RVA, 0x200, HDRSZ, 0, 0, 0, 0,
    0x60000020)  # CODE|EXECUTE|READ

hdr = dos + b"PE\x00\x00" + struct.pack(
    "<HHIIIHH", 0x14C, 1, 0, 0, 0, len(opt), 0x0102) + opt + sec
assert len(hdr) <= HDRSZ

body = bytearray(IMGSZ)
body[0:len(hdr)] = hdr
body[HDRSZ:len(text) + HDRSZ] = text

out = sys.argv[1] if len(sys.argv) > 1 else "hello32.exe"
open(out, "wb").write(bytes(body))
print("wrote %s: PE32 i386, entry 0x%x, import kernel32.dll!ExitProcess, "
      "1 HIGHLOW reloc" % (out, SEC_RVA))
