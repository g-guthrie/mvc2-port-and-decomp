# Roadmap: the universal port

## The goal

One file that is *Marvel vs. Capcom 2*, and a handful of functions per
device. "Can it run MvC2?" in the sense people ask it of Doom: the actual
game, the same on everything.

The game is a deterministic machine. Every 1/60 s it takes its state and the
buttons held and gives the next state, one picture and 800 samples of sound.
The ideal form of it is:

- **One immutable file: rules plus content.** The rules are small (the
  original's code is 3.3 MB of a 210 MB disc); the hand-drawn sprites, the
  stages and the recorded sound are the size floor.
- **A tiny machine to run it**, small enough to implement on any device in
  an afternoon: integer instructions, a framebuffer, a sound buffer, buttons,
  a clock.
- **Integers only**, so the output is bit-identical everywhere.
- **Shown equal to the original**, by proof where that is practical and by
  measured fuzzing where it is not.

## What "identical" means here

**The reference is the arcade (NAOMI, Rev A).** Owner decision, 2026-11-13.

What the project has established ([ARCADE_DIFF.md](port/docs/ARCADE_DIFF.md)
sections 10 and 11):

- The PS2 game's gameplay tables (animation scripts, boxes, attack records,
  AI) are byte for byte what the Rev A arcade program runs. Checked for 36 of
  59 characters; the rest could not be forced to load in the oracle.
- The gameplay code differs in a few places that affect play, and the port
  follows the arcade on each: Servbot's inputs in hit stop, the flying-screen
  edge, the super-freeze tick order, float rounding, the sine routine.
- The game code adds no input lag on either machine.

So the inside of the file is the PS2-derived program with the arcade's rules.
Finishing the NAOMI decompilation is **not** a prerequisite (owner decision,
2026-11-13). Work on a specific NAOMI function when a question about arcade
behaviour needs it.

Open against "arcade identical":

1. A side-by-side fight matches the arcade for 546 to 591 frames, then a
   hit-stop count differs by one and positions by one float ulp during
   Cyclops' super (BOOT_NOTES "Arcade oracle"). Not resolved.
2. 23 characters' tables are not checked against the arcade program.
3. 1,455 leads from the automatic screen of the per-character state code
   were not read by hand (ARCADE_DIFF 10.5).
4. The arcade picture (PowerVR) is not a target yet; the picture is the PS2
   game's. Sound effects are the PS2's 8 kHz unless a Dreamcast disc is given.

## Where it stands

| Step | State |
|---|---|
| The game's own code running natively ([port/](port)) | Done: playable end to end, verified against the retail PS2 game over 1.1 million fuzzed frames |
| One module with a five-function host ([port/generic](port/generic/README.md)) | Working: picture, sound and buttons; the plain-C translation matches the wasm frame for frame |
| No GPU needed ([SWGL.md](port/docs/SWGL.md)) | Working: 99.9% of fight pixels within 2 levels of the GL renderer |
| 60 fps in the translated build | Not yet: fight frames take 23.3 ms (16.7 needed); the game step itself is 1.5 ms, the rest is drawing |
| Other CPUs | arm64 and big-endian s390x builds of the translated C give the same 1200 state hashes as the wasm, under emulation. No real device yet |
| Personal build with the disc inside | Working (`EMBED=1`), for the disc's owner only |
| A clean inside | Measured, not started (below) |

## Simplifying the inside

The module today is the whole PS2 program, translated. The ambition is a
clean engine that produces the same game.

**Method.** Never a from-scratch rewrite. Replace one subsystem at a time,
run it next to the original on the same inputs, and compare every frame:
the draw list, the sound commands and the gameplay state, not only pixels.
The original stays as the oracle. The byte-exact decompilation trees are
never edited for this.

**What the game is made of** (`port/generic/coverage.sh`, six fuzzed arcade
sessions of 60,000 frames, 2026-11-13):

| Part of the original | Functions | Ran |
|---|---|---|
| Game code | 13,240 | 43% |
| Main loop and tasks | 56 | 84% |
| PS2 render library | 162 | 60% |
| Engine services, effects, sound client | 246 | 39% |
| CRI middleware | 147 | 30% |
| Sony runtime / libc | 167 | 35% |

- The shared gameplay core (0x176000 to 0x191000: player update, hits,
  physics, edges) is about 440 functions.
- The other roughly 12,000 game functions are small (median 15 lines of
  translated C, 77% under 30) and run in patches, by character and effect.
  They are per-character state code.
- So: a small engine to write by hand, and thousands of small state
  functions to convert mechanically into a table or script form. How regular
  those functions are has not been examined yet; that is the next question.

Limits of the measurement: functions, not branches; arcade mode only; player
1 always the same team; the count of functions present is a floor (the
optimiser merges small ones).

**How regular the state code is** (`port/tools/state_code_survey.py`, from
the credited C of the 15,493 functions in 0x191000 to 0x3D0000, 2026-11-13):

| Shape | Share |
|---|---|
| Table dispatch head (`jtbl[p[n]](p)`) | 15.4% |
| Empty or constant return | 1.5% |
| 1 to 4 statements | 22.8% |
| 5 to 12 statements | 29.2% |
| 13 to 40 statements | 25.5% |
| Over 40 statements | 2.0% |
| Source not located by the survey | 3.7% |

- Median 6 statements per function.
- 30,520 call sites. The ten most called functions take 46% of them and the
  top 25 take 60%. 142 engine functions account for 68% of all call sites.
- The two most called are the animation step (`func_00175700`, 4,796 sites)
  and the animation start (`func_001757E0`, 3,978).

So the per-character code is close to a script already: states selected
through tables, each a few calls to about 150 engine primitives with
constant arguments, and increments of state bytes. That is what makes a
mechanical conversion to a table or bytecode plausible. Not yet examined:
what the statements between the calls do (the survey counts them, it does
not classify them), and the 2% of large functions.

**Proof.** A proof assistant fits the small pure pieces (fixed-point math,
the damage formula, the input buffer, the animation-script interpreter) and
the specification of the tiny machine. For most functions the cheaper tools
come first: exhaustive checking where the input space is small, bounded model
checking of one C function against another.

## Order of work

1. 60 fps in the translated build.
2. ~~arm64 and a big-endian build, same hash comparison.~~ Done under
   emulation (2026-11-13). Next: a real device, and a 32-bit target.
3. Widen the fuzzer (other modes, all characters) and record coverage per
   character.
4. Classify the per-character state functions; try a mechanical conversion on
   one character.
5. The first hand-written engine piece, behind the same comparison.
6. Close the "arcade identical" list above.
