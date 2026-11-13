#!/bin/bash
# Which of the original game's functions run in fuzzed play (testing).
#
#   port/generic/coverage.sh [FRAMES_PER_SEED] [SEEDS...]
#
# Needs port/generic/build.sh to have run (it leaves the link line in
# build/port-generic).  Relinks the module with function names kept,
# translates it, builds the C with gcov counters, plays each seed with
# drawing off (host_w2c.c --fuzz SEED --nodraw) and prints the summary of
# coverage_report.py.  Results stay in build/port-cov.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
G=$ROOT/build/port-generic; O=$ROOT/build/port-cov
ISO=${ISO:-$ROOT/private/disc/mvc2.iso}; W2C=${WASM2C_DIR:-/usr/share/wabt/wasm2c}
N=${1:-60000}; shift || true
SEEDS=${*:-1 2 3 4 5 6}
mkdir -p "$O"
sed -e "s#$G/mvc2g.wasm#$O/mvc2g.wasm --profiling-funcs#" "$G/link.cmd" > "$O/link.cmd"
(cd "${EM:-$ROOT/build/port-em}" && sh "$O/link.cmd")
cd "$O"
rm -f ./*.gcda ./*.gcno
wasm2c mvc2g.wasm -n mvc2g -o mvc2g_w2c.c
${CC:-gcc} -O1 --coverage -w -c mvc2g_w2c.c -o mvc2g_w2c.o
${CC:-gcc} -O1 --coverage -w -I"$W2C" -I. "$ROOT/port/generic/host_w2c.c" mvc2g_w2c.o "$W2C/wasm-rt-impl.c" -lm -o mvc2g_cov
for seed in $SEEDS; do ./mvc2g_cov "$ISO" "$N" --fuzz "$seed" --nodraw 2>&1 | grep "mvc2g\]"; done
gcov -f -n mvc2g_w2c.c > gcov_funcs.txt 2>/dev/null || true
python3 "$ROOT/port/generic/coverage_report.py" gcov_funcs.txt "$ROOT/config/symbol_addrs.txt"
