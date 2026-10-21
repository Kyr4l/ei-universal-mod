#!/usr/bin/env python3
"""Set the IMAGE_FILE_LARGE_ADDRESS_AWARE (0x0020) bit in a PE file's COFF
file header Characteristics field, without altering anything else.

Usage: apply_laa_patch.py <input.exe> <output.exe>
"""
import shutil
import struct
import sys

LAA_BIT = 0x0020


def patch(src: str, dst: str) -> None:
    shutil.copyfile(src, dst)
    with open(dst, "r+b") as f:
        data = bytearray(f.read())
        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        chars_off = e_lfanew + 4 + 18  # COFF header: Machine,NumSections,TimeDate,PtrSyms,NumSyms,OptHdrSize,(Characteristics)
        characteristics = struct.unpack_from("<H", data, chars_off)[0]
        if characteristics & LAA_BIT:
            print("LARGE_ADDRESS_AWARE already set; nothing to do")
            return
        struct.pack_into("<H", data, chars_off, characteristics | LAA_BIT)
        f.seek(0)
        f.write(data)
    print(f"Patched {dst}: Characteristics 0x{characteristics:04x} -> 0x{characteristics | LAA_BIT:04x}")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    patch(sys.argv[1], sys.argv[2])
