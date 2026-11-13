# Marvel vs. Capcom 2 — port and decompilation

[![Progress report](https://github.com/g-guthrie/mvc2-port-and-decomp/actions/workflows/report.yml/badge.svg)](https://github.com/g-guthrie/mvc2-port-and-decomp/actions/workflows/report.yml)
[![Code progress](https://decomp.dev/g-guthrie/mvc2-ps2-decomp.svg?mode=shield&measure=code&label=PS2%20code)](https://decomp.dev/g-guthrie/mvc2-ps2-decomp)

*Marvel vs. Capcom 2: New Age of Heroes*, rebuilt from source, on the way to
one file that runs anywhere.

**The aim** is a universal port: the actual game as one self-contained module
plus a handful of functions per device, the same on everything, the way
people ask "can it run Doom?". It plays by the arcade's rules. The plan and
where it stands are in [ROADMAP.md](ROADMAP.md).

| Part | Where | State | How it is checked |
| --- | --- | --- | --- |
| **PS2 decompilation** (NTSC-U `SLUS_204.86`) | [`src/`](src), [`config/`](config), [`tools/`](tools) | 99.93% of code, 99.99% of data | Rebuilds the retail ELF byte for byte |
| **Arcade (NAOMI) decompilation** | [`naomi/`](naomi) | In progress: 58.4% of the main image, 36.9% as verified C. Not needed for the port; worked on when a question about arcade behaviour needs it | Rebuilds the retail ROM bytes it covers |
| **The port**: the game's own code, no emulator, in the browser and on the desktop | [`port/`](port) | Playable end to end | 1.1 million fuzzed frames equal to the retail PS2 game; compared with the arcade ROM |
| **The universal build** (`mvc2g`): one module, five host functions, no GPU | [`port/generic/`](port/generic) | Picture, sound and buttons work; not yet 60 fps | The plain-C translation equals the wasm frame for frame; picture within 2 levels of the GL renderer on 99.9% of fight pixels |

No game data is in this repository. Everything runs from your own disc.

**New here?** Read [ROADMAP.md](ROADMAP.md) for the goal and the current
state, then [AGENTS.md](AGENTS.md) for the map of the repository, how to set
up a machine, and the rules of each part.

## The port

The port compiles the decompiled PS2 C for a 32-bit host and replaces only the
console's hardware: graphics (WebGL2), sound (ADX streams and sound banks
through Web Audio), controllers, memory card and disc. Everything the player
does runs through the game's own code.

It plays by the **arcade's rules**. The PS2 build was made from the arcade
program; where the two differ (timing, super-freeze order, flying-screen
edges, Servbot hit-stop, float rounding and more), the port follows the arcade.
The differences were found by comparing the two programs and confirmed by
running the arcade ROM next to the port ([ARCADE_DIFF.md](port/docs/ARCADE_DIFF.md)).

There are no settings that make it worse. It always runs with:

- **Zero added input lag**: input is read the frame it happens, and run-ahead
  shows each press one frame sooner, ahead of the arcade cabinet. The display
  path is low latency too.
- Everything unlocked, no load times, sharp native-resolution output.
- Keyboard and gamepad for two players with full remapping (Xbox,
  PlayStation and Switch pads), and a touch overlay on phones.
- The Dreamcast's higher-rate sound effects, if you also give it your
  Dreamcast disc. The PS2's are 8 kHz; the Dreamcast's are 11–16 kHz.
- Saves that persist, and offline play once the disc is cached.

**Play.** Build the web version, or open a hosted copy, and choose your
`SLUS-20486` disc image once; it is cached in the browser. `?disc=URL` and
`?dcse=URL` load the discs without the picker. Controls and building are in
the [player guide](port/docs/USER_GUIDE.md).

**Verified against retail.** An oracle runs the retail PS2 executable in an
emulator and compares the port with it frame by frame: the whole game state,
plus every sound command. 416 random fight seeds and 36 complete arcade runs
(about 1.1 million frames, through Abyss to the ending) match exactly, and so
do 410,208 sound commands ([ORACLE_FUZZ.md](port/docs/ORACLE_FUZZ.md)). These
runs switch the arcade rules off, so that retail PS2 is a fair reference.

| Document | Contents |
| --- | --- |
| [USER_GUIDE.md](port/docs/USER_GUIDE.md) | Playing, controls, building, test switches |
| [ARCADE_DIFF.md](port/docs/ARCADE_DIFF.md) | PS2 vs arcade: what differs and what the port does |
| [ORACLE_FUZZ.md](port/docs/ORACLE_FUZZ.md) | The retail comparison and its results |
| [BOOT_NOTES.md](port/docs/BOOT_NOTES.md) | How the port runs the game's code; every fix, with its evidence |
| [PS2_GRIPES.md](port/docs/PS2_GRIPES.md) | Known complaints about the PS2 version, and what the port does about each |
| [RENDERER.md](port/docs/RENDERER.md), [AUDIO.md](port/docs/AUDIO.md) | The graphics and sound layers |
| [PORT_PLAN.md](port/docs/PORT_PLAN.md) | The port's phases |

## The universal build

[`port/generic`](port/generic/README.md) links the game, the renderer and the
sound mixer into one module that needs only: disc bytes in, a 640x480 picture
out, sound samples out, buttons in, and a call 60 times a second. The picture
is drawn on the CPU ([SWGL.md](port/docs/SWGL.md)), so no GPU or graphics
library is involved. The module is translated to plain C with `wasm2c`, which
compiles for any CPU and pointer size; the translation is checked against the
wasm by a state hash every frame.

A personal build can carry your disc inside it, so a device needs no file
access at all. That build contains the game and is never committed or shared.

| Document | Contents |
| --- | --- |
| [ROADMAP.md](ROADMAP.md) | The goal, what "identical to the arcade" means, what is done, what is next |
| [port/generic/README.md](port/generic/README.md) | The host interface, building, testing, current numbers and gaps |
| [SWGL.md](port/docs/SWGL.md) | The software renderer: how it works, accuracy and speed |

## PS2 decompilation

[![Matching C progress](assets/progress.svg)](DECOMP.md)

Every matched function is C compiled with a period toolchain (Metrowerks MWCC
2.4/3.0/3.0.3, SN GCC 2.95.2, EE GCC 2.96 or Sony EE GCC 2.9), and checked byte
for byte against retail. The few routines that were hand-written assembly in
the original (crt0, the exception handler, libvu0 kernels, the cache flush,
newlib's r5900 string routines) are `.S` sources. The hybrid build links all
of it and reproduces the retail ELF exactly (SHA-256 `48ebf907…6f13d7`).

| Track | Progress |
| --- | --- |
| Code | **99.93%** (3,291,484 / 3,293,696 bytes; 20,181 / 20,678 functions) |
| Initialized data | **99.99%** (647,988 / 648,064 bytes) |
| Retail ELF rebuild | Byte-exact |

The last 2,212 bytes of code are blocked by compiler behaviour that no
available build reproduces; [REMAINING.md](REMAINING.md) gives the evidence
for each. Setup, building and the matching rules are in [DECOMP.md](DECOMP.md)
and [AGENTS.md](AGENTS.md). CI checks the catalog, the build script and the
tools. It cannot compile, because the compilers are not redistributable.

## NAOMI decompilation

The arcade program, compiled with the original Hitachi SH C compiler and
matched against the retail ROM. It is far from done, so the port runs the PS2
program. The arcade ROM is the reference for how the port plays. See
[naomi/README.md](naomi/README.md).

## Layout

| Path | Holds |
| --- | --- |
| `ROADMAP.md` | The goal and the state of the universal port |
| `port/` | The port: host layer, renderer, audio, front ends, web shell |
| `port/generic/` | The universal build (`mvc2g`), its hosts and its tests |
| `port/tools/oracle*`, `port/tools/naomi_oracle` | The comparisons: retail PS2 state and sound, retail picture and sound, the arcade ROM |
| `src/`, `config/`, `tools/`, `tests/` | PS2 decompilation: C sources, symbol and match catalogs, build and verification tools |
| `naomi/` | NAOMI decompilation, self-contained |
| `docs/` | PS2 decompilation notes; `docs/recovery/` holds the dated recovery records |
| `build-inputs/`, `private/` | Where you supply the retail image and compilers (ignored) |

## Legal

Disc images, ROMs, extracted assets, generated retail assembly, compilers and
SDK files are not in this repository and must be supplied locally. The port
contains no game data: it reads your own disc image at run time. This is a
research project and is not affiliated with Capcom, Marvel, Sony, Sega or
their licensors.
