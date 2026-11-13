#!/bin/sh
# mvc2g: link the game as a standalone wasm module, translate it to C with
# wasm2c (wabt), and build that C natively.  Then run both for N frames and
# compare the per-frame state hashes.
#
#   port/generic/build.sh [EM_BUILD_DIR] [OUT_DIR] [ISO] [FRAMES]
#
# Needs: the Emscripten build of mvc2_host (ninja -C build/port-em mvc2_host),
# emsdk in the environment, wabt (wasm2c), node, a C compiler (CC, default cc).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
EM=${1:-$ROOT/build/port-em}; OUT=${2:-$ROOT/build/port-generic}
ISO=${3:-$ROOT/private/disc/mvc2.iso}; N=${4:-1200}
W2C=${WASM2C_DIR:-/usr/share/wabt/wasm2c}
mkdir -p "$OUT"
cd "$EM"
# the mvc2_host link line, with the response file kept
rm -f mvc2_host.js && ninja -d keeprsp mvc2_host >/dev/null
# the entry point, the game's render and sound glue, the renderer on swgl and the
# mixer with no device (no GL, no SDL)
OBJS=
for f in generic/mvc2g.c host/fe/fe_render.c host/fe/fe_mesh.c host/fe/fe_char.c host/fe/fe_audio.c \
         render/r_gl.c render/r_char.c render/r_mesh.c render/decode.c render/r_font.c render/swgl.c \
         audio/adx.c audio/vag.c audio/hdbank.c audio/resample.c audio/synth.c audio/tsd.c audio/audio.c \
         audio/wav.c audio/dcse.c platform/settings.c; do
    o="$OUT/$(basename "$f" .c).o"
    emcc -O2 -DPORT_SWGL -DPORT_NO_AUDIO_DEVICE -I"$ROOT/port/stubs" -I"$ROOT/port/host" -I"$ROOT/port/platform" -I"$EM/glue" \
        -c "$ROOT/port/$f" -o "$o"
    OBJS="$OBJS $o"
done
sed "s#[^ ]*host_main\.c\.o#$OBJS#" CMakeFiles/mvc2_host.rsp > "$OUT/link.rsp"
ninja -t commands mvc2_host | tail -1 | sed -e 's/^: && //' -e 's/ && :$//' \
    -e 's/-sNODERAWFS=1/-sSTANDALONE_WASM=1 --no-entry/' -e 's/-sALLOW_MEMORY_GROWTH=1//' \
    -e 's/-sINITIAL_MEMORY=[0-9]*/-sINITIAL_MEMORY=201326592/' -e 's/-sEXIT_RUNTIME=1//' \
    -e 's/--profiling-funcs//' -e 's#-Wl,-Map=[^ ]*##' \
    -e "s#@CMakeFiles/mvc2_host.rsp#@$OUT/link.rsp#" -e "s#-o mvc2_host.js#-o $OUT/mvc2g.wasm#" > "$OUT/link.cmd"
sh "$OUT/link.cmd"
cd "$OUT"
wasm2c mvc2g.wasm -n mvc2g -o mvc2g_w2c.c
${CC:-cc} -O2 -w -c mvc2g_w2c.c -o mvc2g_w2c.o
${CC:-cc} -O2 -w -I"$W2C" -I. "$ROOT/port/generic/host_w2c.c" mvc2g_w2c.o \
    "$W2C/wasm-rt-impl.c" -lm -o mvc2g_native
if [ -f "$ISO" ]; then
    node "$ROOT/port/generic/run_node.mjs" mvc2g.wasm "$ISO" "$N" > wasm.txt 2> wasm.err
    ./mvc2g_native "$ISO" "$N" --hash > native.txt 2> native.err
    cmp wasm.txt native.txt && echo "mvc2g: wasm and translated C agree for $N frames"
fi
