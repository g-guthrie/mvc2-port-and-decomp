# Working on this repository

**Start with [ROADMAP.md](ROADMAP.md)**: the goal (a universal port of the
game), what "identical to the arcade" means, what is done and what is next.

## Map

| Part | Where | Its rules | State |
|---|---|---|---|
| **PS2 decompilation** | `src/`, `config/`, `tools/`, `tests/`, `docs/` | this file (below) and [DECOMP.md](DECOMP.md) | Done but for 2,212 bytes ([REMAINING.md](REMAINING.md)) |
| **NAOMI decompilation** | `naomi/` (self-contained) | [naomi/AGENTS.md](naomi/AGENTS.md) | 58%; not a prerequisite for the port, worked on when a question about arcade behaviour needs it |
| **Port** (browser, desktop) | `port/` | [port/docs/BOOT_NOTES.md](port/docs/BOOT_NOTES.md) for how it runs the game's code; [USER_GUIDE.md](port/docs/USER_GUIDE.md) for building and test switches | Playable; verified against retail |
| **Universal build** (`mvc2g`) | `port/generic/`, `port/render/swgl.c` | [port/generic/README.md](port/generic/README.md), [port/docs/SWGL.md](port/docs/SWGL.md) | Works; not yet 60 fps |
| **Comparisons** | `port/tools/oracle` (retail PS2 state and sound), `port/tools/oracle_av` (retail picture and sound), `port/tools/naomi_oracle` (arcade ROM), `port/generic/build.sh` (wasm against its C translation), `port/generic/coverage.sh` | their READMEs and [ORACLE_FUZZ.md](port/docs/ORACLE_FUZZ.md) | |

Rules that hold everywhere:

- Player-facing behaviour follows the arcade. PS2 behaviour is only for the
  oracle comparison (`MVC2_PS2_EXACT`). Never add an option that makes the
  game worse.
- Nothing copyrighted is committed anywhere: no disc images, ROMs, extracted
  assets, compilers or SDK files, and no build that contains them (the
  personal `mvc2g` build stays in `private/`).
- The byte-exact decompilation trees (`src/`, `naomi/src/`) are not edited
  for the port's sake. Port glue lives under `port/`.
- Record a measurement where the next person will look for it (the document
  of that part), with the date and what was and was not checked.

## Setting up a machine for the port (Ubuntu 24.04, x86-64)

The port does not need the PS2 compilers or the retail ELF. It needs your
disc image (`.iso`, or a raw `.bin`) at `private/disc/mvc2.iso`; `private/`
is ignored.

In a Claude cloud session, pick the `mvc2-port-and-decomp` environment: its
set-up script runs the installs below at session start (`emsdk` and
`SDL2-src` land next to the checkout). The disc is never part of an
environment; it has to be supplied in each new session.

```sh
sudo dpkg --add-architecture i386 && sudo apt-get update
sudo apt-get install -y libsdl2-dev libsdl2-2.0-0:i386 libgl1-mesa-dri:i386 libegl1:i386 \
    libgles2:i386 gcc-multilib wasi-libc wabt
# Emscripten 6.0.13 and the SDL2 sources its port needs, next to the checkout
git clone --depth 1 https://github.com/emscripten-core/emsdk.git ../emsdk
(cd ../emsdk && ./emsdk install 6.0.13 && ./emsdk activate 6.0.13)
git clone --depth 1 -b release-2.32.10 https://github.com/libsdl-org/SDL.git ../SDL2-src
```

Then, from the checkout:

```sh
# desktop app (i386), about 15 minutes the first time
cmake -S port -B build/port-i386 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/port/cmake/i386-clang.cmake
ninja -C build/port-i386 mvc2
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy ./build/port-i386/mvc2 --hidden --size 640x480 \
    --shot-dir OUT --script port/tools/scripts/arcade_fight.txt private/disc/mvc2.iso

# browser build and the headless wasm host
source ../emsdk/emsdk_env.sh && export EMCC_LOCAL_PORTS=sdl2=$PWD/../SDL2-src
emcmake cmake -S port -B build/port-em -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build/port-em mvc2_host mvc2

# the universal build: link, translate to C, build, compare (about 7 minutes)
port/generic/build.sh
```

Things that cost time if you do not know them:

- Do not pass the i386 build's `host_sigs.json` to the wasm build
  (`-DMVC2_HOST_SIGS=`): the wasm link fails on a `memcpy` signature. Each
  target generates its own.
- The play-through scripts (`arcade_ending_rank`, `score_attack`,
  `shop_buy_save`) need `--cpu-hp 1 --draw-every 30`; `to_menu.txt` has no
  `quit`.
- The software renderer in the desktop app: `MVC2_SWGL=1` (screenshots only).
- `wabt`'s `wasm2c` names its accessors for exported globals after data
  symbols; they are not game functions (`coverage_report.py` drops them).

## Adding PS2 matches

The rest of this file is for adding PS2 matches. CI cannot compile, so the
checks here are what keep `main` building.

### Before starting

- `git pull origin main`, then `make hybrid PYTHON="$PYTHON"`. Work from a
  tree that reproduces the retail ELF, not from a stale checkout or branch.
- Use the names in `config/symbol_addrs.txt` as they are on `main`. Names
  get corrected over time (for example `0x119BC0` is `dptoli`, not
  `dptoul`), and a source that only matches under an old name is wrong.

### Adding a function

1. Put the C in `src/`, add its row to `config/matches.csv`, and add its
   compile + verify stanza to `tools/compile_matching.sh` **before** the
   final `tools/compile_matching_data.sh` line.
2. MWCC does not search the including file's directory. A stanza whose
   source includes a project header needs `-i src`.
3. Every compiled object must be verified (`tools/verify_object.py`) and
   written by exactly one stanza.
4. Run `make progress-check PYTHON="$PYTHON"`. It runs
   `tools/check_build_script.py` (stanza structure, includes, duplicates,
   unverified objects, data labels), build coverage, and the source-credit
   audit.
5. Run `make hybrid PYTHON="$PYTHON"`. Push only if it ends with
   `exact retail ELF` and sha256 `48ebf907…6f13d7`.

`make match` runs the stanzas in parallel and caches compiles under
`build/cache/`, so a rebuild after a small change takes about a minute.
Every failure is listed at the end. `make match-serial` runs the script with
`sh` if you need to rule out the runner.

### Never

- Match a caller against a stand-in callee. A `static` copy of a retail
  function is allowed only if it is that function's real body; the build
  verifies every such copy at the function's own address and fails on a
  stand-in, an empty body, or a simplified one.
- Credit assembly as C: no `FN(...)` macro bodies, `.word` streams, empty
  asm barriers, or unused register-pinned locals. The audit rejects them.
- Commit the retail image, compilers (Metrowerks, SN, EE GCC, Sony), or SDK
  files. They stay in `private/`.
- Leave work only on a local branch for long. Rebase it onto `main`, run the
  steps above, and push to `main`.
