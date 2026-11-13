#!/usr/bin/env python3
"""How regular is the per-character and effect code? (ROADMAP.md, "Simplifying the inside")

    python3 port/tools/state_code_survey.py [START END]     # default 0x191000 0x3D0000

Reads config/matches.csv and the credited C sources; nothing is compiled.
For every function in the address range it finds the body, counts its
statements and the functions it calls, and prints the shapes and how
concentrated the calls are.  A callee outside the range is engine code.
"""
import collections
import csv
import re
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main():
    lo = int(sys.argv[1], 16) if len(sys.argv) > 2 else 0x191000
    hi = int(sys.argv[2], 16) if len(sys.argv) > 2 else 0x3D0000
    rows = [r for r in csv.DictReader(open(ROOT / "config/matches.csv")) if lo <= int(r["address"], 16) < hi]
    in_range = {r["name"] for r in rows}
    sources = {}

    def body_of(r):
        if r["source"] not in sources:
            try:
                sources[r["source"]] = (ROOT / r["source"]).read_text()
            except OSError:
                sources[r["source"]] = ""
        src = sources[r["source"]]
        m = re.search(r"(?:^|\n)[^\n;{}()#]*\b" + re.escape(r["name"]) + r"\s*\([^;{]*\)\s*\{", src)
        if not m:
            return None
        k = src.index("{", m.start())
        start, depth = k, 0
        while k < len(src):
            if src[k] == "{":
                depth += 1
            elif src[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        return src[start:k + 1]

    kinds, calls, stmts = collections.Counter(), collections.Counter(), []
    for r in rows:
        b = body_of(r)
        if b is None:
            kinds["source not located"] += 1
            continue
        b = re.sub(r"/\*.*?\*/", "", b, flags=re.S)
        b = re.sub(r"extern[^;]*;", "", b)
        for c in re.findall(r"\b(func_[0-9A-F]{8}|[a-z_0-9]+_[0-9A-F]{8})\s*\(", b):
            if c != r["name"]:
                calls[c] += 1
        n = b.count(";")
        stmts.append(n)
        if re.fullmatch(r"\{\s*(return\s*[^;]*;)?\s*\}", b.strip()):
            kinds["empty or constant return"] += 1
        elif re.search(r"jtbl_[0-9A-F]{8}\s*\[", b) and n <= 2:
            kinds["table dispatch head"] += 1
        elif n <= 4:
            kinds["1-4 statements"] += 1
        elif n <= 12:
            kinds["5-12 statements"] += 1
        elif n <= 40:
            kinds["13-40 statements"] += 1
        else:
            kinds["over 40 statements"] += 1

    print("functions in 0x%X..0x%X: %d" % (lo, hi, len(rows)))
    for k, v in kinds.most_common():
        print("  %-28s %6d  %4.1f%%" % (k, v, 100 * v / len(rows)))
    print("statements per function: median %g, mean %.1f" % (statistics.median(stmts), statistics.mean(stmts)))
    total = sum(calls.values())
    print("call sites: %d, distinct callees: %d" % (total, len(calls)))
    acc = 0
    for i, (_, n) in enumerate(calls.most_common()):
        acc += n
        if i + 1 in (10, 25, 50, 100, 200, 500):
            print("  top %3d callees cover %4.1f%% of call sites" % (i + 1, 100 * acc / total))
    engine = [(c, n) for c, n in calls.most_common(400) if c not in in_range]
    print("engine callees among the top 400: %d, covering %.1f%% of call sites"
          % (len(engine), 100 * sum(n for _, n in engine) / total))
    print("most called:", ", ".join("%s %d" % cn for cn in calls.most_common(12)))


if __name__ == "__main__":
    main()
