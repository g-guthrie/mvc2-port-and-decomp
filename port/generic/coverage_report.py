#!/usr/bin/env python3
"""Summarise `gcov -f` output of the translated module by region of the original program.

    coverage_report.py gcov_funcs.txt config/symbol_addrs.txt [--csv OUT.csv]

A function called only through a table lives in its `byn$fpcast-emu$` thunk
after binaryen inlines it, so a thunk and its base name count as one function.
wasm2c's accessors for exported globals (named after data symbols) and the
port's own functions have no address in the code range and are left out.
The optimiser merges and inlines small functions, so the count of functions
present is a floor.
"""
import collections
import csv
import re
import sys

THUNK = "byn0x24fpcast0x2Demu0x24"
REGIONS = [
    (0x120000, "Sony runtime / libc"),
    (0x3D6000, "game code"),
    (0x3DB000, "main loop, tasks"),
    (0x3F1000, "PS2 render library"),
    (0x410000, "CRI middleware"),
    (0x424200, "engine services, effects, sound client"),
]


def region(a):
    for end, name in REGIONS:
        if a < end:
            return name
    return None


def main():
    gcov, syms = sys.argv[1], sys.argv[2]
    out_csv = sys.argv[sys.argv.index("--csv") + 1] if "--csv" in sys.argv else None
    addr = {}
    for line in open(syms):
        m = re.match(r"\s*([A-Za-z_0-9.$]+)\s*=\s*0x([0-9A-Fa-f]+)", line)
        if m:
            addr.setdefault(m.group(1), int(m.group(2), 16))
    funcs = {}
    for name, pct, n in re.findall(r"Function 'w2c_mvc2g_([^']+)'\nLines executed:([0-9.]+)% of (\d+)", open(gcov).read()):
        if name.startswith(THUNK):
            name = name[len(THUNK):]
        name = re.sub(r"_0x5Fhs$", "", name)
        n = int(n)
        ran = float(pct) * n / 100
        e = funcs.setdefault(name, [0, 0.0, 0])
        if n > e[0]:
            e[0], e[1] = n, ran  # the larger body is the function itself
        if ran > 0:
            e[2] = 1
    rows = []
    for name, (lines, ran_lines, ran) in funcs.items():
        a = addr.get(name)
        m = re.search(r"_([0-9A-F]{8})$", name)
        if a is None and m:
            a = int(m.group(1), 16)
        if a is None or not 0x100000 <= a < 0x424200:
            continue
        rows.append((a, name, lines, ran_lines, ran))
    rows.sort()
    agg = collections.OrderedDict((name, [0, 0, 0, 0.0]) for _, name in REGIONS)
    for a, _, lines, ran_lines, ran in rows:
        g = agg[region(a)]
        g[0] += 1
        g[1] += ran
        g[2] += lines
        g[3] += ran_lines
    print("%-42s %9s %8s %7s %12s" % ("region", "functions", "ran", "", "lines ran"))
    tot = [0, 0, 0, 0.0]
    for name, g in agg.items():
        if g[0]:
            print("%-42s %9d %8d %6.1f%% %11.1f%%" % (name, g[0], g[1], 100 * g[1] / g[0], 100 * g[3] / g[2]))
        tot = [x + y for x, y in zip(tot, g)]
    print("%-42s %9d %8d %6.1f%% %11.1f%%" % ("all", tot[0], tot[1], 100 * tot[1] / tot[0], 100 * tot[3] / tot[2]))
    if out_csv:
        with open(out_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["address", "name", "translated_lines", "lines_ran", "ran"])
            for a, name, lines, ran_lines, ran in rows:
                w.writerow(["0x%08X" % a, name, lines, "%.0f" % ran_lines, ran])


if __name__ == "__main__":
    main()
