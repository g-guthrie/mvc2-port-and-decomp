# mvc2g: the universal build

One self-contained module that is the whole game, and a handful of functions
a device has to supply. The shape is doomgeneric's: a port to a new device is
those functions and nothing else.

It is the port's wasm32 build with the picture drawn on the CPU
([`render/swgl.c`](../render/swgl.c), no GPU) and the sound mixed with no
audio device, linked as a standalone module. A device either runs the
`.wasm`, or compiles its `wasm2c` translation, which is plain C for any CPU
and pointer size. The game's 32-bit pointers become offsets into one block of
memory, so there is no 64-bit port to do.

Why it exists and where it is going: [ROADMAP.md](../../ROADMAP.md).

## What a device supplies

| | The module calls / the host does |
|---|---|
| Disc | `page_disc_read(offset, n, dst)`: bytes of the user's disc image |
| Picture | show `mvc2g_canvas()`: 640x480 RGBA8, top row first |
| Sound | play `mvc2g_audio(800)` once per step: 48 kHz stereo, signed 16-bit |
| Buttons | `mvc2g_pad(port, bits)` before a step (PS2 pad bits, `platform/pad.h`) |
| Time | call `mvc2g_frame()` 60 times a second |

The module's full import list is 13 functions; the other 12 are WASI stubs
(`fd_write` for the log, a clock, an empty environment) and three unused
file calls. [`host_w2c.c`](host_w2c.c) implements all of them in about 60
lines.

Exports: `mvc2g_boot(disc_size)`, `mvc2g_frame()`, `mvc2g_pad()`,
`mvc2g_canvas()`, `mvc2g_audio()`, `mvc2g_draw(on)` (0 = run steps without
drawing: frame skip, and fast test runs), `mvc2g_state_hash()` (FNV-1a of the
EE BSS and heap), `mvc2g_game_state()`.

## Building and testing

Needs the Emscripten build of `mvc2_host` (`ninja -C build/port-em
mvc2_host`), emsdk in the environment, `wabt` (`wasm2c`), node and a C
compiler. Set-up for a fresh machine is in [AGENTS.md](../../AGENTS.md).

```sh
port/generic/build.sh                 # link, translate, build, compare (about 7 minutes)
build/port-generic/mvc2g_native DISC FRAMES [--hash] [--mash] [--fuzz SEED] [--nodraw]
                                      [--wav FILE.wav] [--shot FRAME:FILE.ppm]
EMBED=1 port/generic/build.sh         # also private/generic/mvc2g_personal (below)
port/generic/coverage.sh              # which of the game's functions run in fuzzed play
```

`build.sh` runs the `.wasm` under node (`run_node.mjs`) and the translated C
natively for 1200 frames and compares the state hash of every frame. They
must be equal.

`--mash` presses START and CROSS in turn (title to a fight); `--fuzz SEED`
does that outside fights and presses random directions and attacks inside
them.

**Personal build.** `EMBED=1` links your disc image into the program, so the
device needs no file access at all: picture, sound, buttons and time. The
result contains the game. It is written to `private/generic/` (ignored) and
is for the disc's owner only; never commit or share it. It uses `.incbin`,
so it needs GCC or Clang.

## State (2026-11-13)

| | |
|---|---|
| Logic | The translated C on x86-64 gives the same state hash as the wasm for 1200 frames (boot, title, attract), with the renderer and mixer inside |
| Picture | Draws title, menus, character select and fights; checked against the GL renderer in the desktop build ([SWGL.md](../docs/SWGL.md)) |
| Sound | A 50 s run is silent for the 4 s of boot and has signal after. Not compared with the desktop mix |
| Buttons | Player 1 and 2 pad bits; no analog sticks |
| Speed, x86-64, translated C, `-O2` | Fight frames (`--fuzz 3 --time-from 1900`, 1300 frames): 23.3 ms per frame, of which the game step and sound are 1.5 ms. From boot through menus into a fight (`--mash`, 3000 frames): 21.5 ms. 60 fps needs 16.7 ms |
| Other CPUs | Not built yet (arm64, a big-endian target) |

Known gaps:

- No settings store in the module: every setting keeps its default, and the
  arcade-rule switches are whatever the defaults are.
- No memory card persistence (`mc_persist` is a no-op here).
- The texture-coordinate divide and the vertex stage use floats; the pixel
  pipeline is integer. The same wasm gives the same floats everywhere, but a
  translation built with fused multiply-add or x87 would not: build with
  `-ffp-contract=off` on such targets.
- `wasm2c` from wabt 1.0.34 is what was used. The translation costs the
  rasteriser little: fight frames draw in about 21.8 ms translated (x86-64)
  against 20.0 ms for the same C compiled directly (i386). The rasteriser
  itself is what has to get faster.

## Files

| | |
|---|---|
| `mvc2g.c` | The entry points above |
| `host_w2c.c` | Native host for the translated C: test options, WAV and PPM output, the embedded-disc variant |
| `run_node.mjs` | Node host for the `.wasm` (state hashes only) |
| `build.sh` | Link, translate, build, compare |
| `coverage.sh`, `coverage_report.py` | Function coverage of the original game's code over fuzzed play |
