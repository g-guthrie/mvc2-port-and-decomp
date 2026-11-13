#!/usr/bin/env python3
"""Patch wasm2c's output (wabt 1.0.34) for big-endian hosts.

    fix_w2c.py mvc2g_w2c.c

On a big-endian host wasm2c keeps the module's memory mirrored (the bytes of
addresses [a, a + n) are data[size - a - n .. size - a)), and its loads,
stores and data segments account for that.  Its memory.fill and memory.copy
do not: they index data[] with the wasm address.  This rewrites the two
helpers.  Little-endian builds are unchanged (the patch is under
WABT_BIG_ENDIAN, which a big-endian build must define).
"""
import sys

FILL_OLD = "  memset(mem->data + d, val, n);\n"
FILL_NEW = """#if WABT_BIG_ENDIAN
  memset(mem->data + mem->size - d - n, val, n);
#else
  memset(mem->data + d, val, n);
#endif
"""
COPY_OLD = "  memmove(dest->data + dest_addr, src->data + src_addr, n);\n"
COPY_NEW = """#if WABT_BIG_ENDIAN
  memmove(dest->data + dest->size - dest_addr - n, src->data + src->size - src_addr - n, n);
#else
  memmove(dest->data + dest_addr, src->data + src_addr, n);
#endif
"""


def main():
    path = sys.argv[1]
    s = open(path).read()
    if "mem->size - d - n" in s:
        return  # already patched
    if s.count(FILL_OLD) != 1 or s.count(COPY_OLD) != 1:
        sys.exit("fix_w2c.py: memory_fill / memory_copy not found as wabt 1.0.34 writes them")
    open(path, "w").write(s.replace(FILL_OLD, FILL_NEW).replace(COPY_OLD, COPY_NEW))


if __name__ == "__main__":
    main()
