# swgl: the software renderer

[`port/render/swgl.c`](../render/swgl.c) implements, on the CPU, the GL ES 3
calls that `r_gl.c` and `r_mesh.c` make. The renderer code is unchanged and
there is still one drawing path; only what is underneath the `gl*` calls
differs. It is what the universal build ([port/generic](../generic/README.md))
draws with, and the desktop app can use it for testing.

## How it works

- `swgl_install()` points every `p_gl*` entry (`platform/gl.h`) at the
  software implementation. With `-DPORT_SWGL` there is no GL driver at all
  and `swgl.c` owns the pointers.
- GLSL is not interpreted. The renderer has three programs and each is
  written out in C: batch (`k_vs`/`k_fs` in `r_gl.c`), mesh (`k_mvs`/`k_mfs`
  in `r_mesh.c`) and present. A program is recognised by the attribute names
  bound to it. **A change to a shader needs the same change in `swgl.c`.**
- Rasteriser: pixel centres, top-left fill rule, 8 sub-pixel bits, clipping
  against `-w <= z <= w`, 24-bit depth, RGBA8 colour, rows bottom up.
- The pixel pipeline is integer. Each row's first and last pixel come from
  the edge functions directly. Depth, colour, fog and texel coordinates step
  in fixed point. Colours reach the blend with 8 fractional bits and are
  rounded once, when stored.
- Only texture coordinates need a perspective divide. It is done at the ends
  of pieces of at most 16 pixels (once per row for 2D draws), and a piece is
  halved until linear stepping stays within 1/32 texel of the exact
  coordinate at its middle.
- **Texel tie rule.** Character sprites are drawn scaled (16 texels over 26
  pixels), so every 13th column has a pixel centre exactly on a texel
  boundary. GL decides that by float rounding, which varies per triangle and
  per driver. `swgl.c` always takes the higher texel (`TEXEL_TIE`).
- A triangle with an ADC vertex is skipped whole.

## Testing it in the desktop app

```sh
MVC2_SWGL=1 SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy ./build/port-i386/mvc2 --hidden \
    --size 640x480 --shot-dir OUT --script port/tools/scripts/arcade_fight.txt DISC
```

Screenshots work in that mode; the window is not updated.
`MVC2_SWGL_STATS=N` draws nothing before canvas frame N, then times the draw
calls and prints the mean per frame at exit.

## Accuracy against the GL renderer (Mesa llvmpipe), 2026-11-13

Same scripts, same frames, i386 desktop build. "Within 2" is the share of
pixels whose largest channel difference is at most 2 levels.

After the 8.8 blend (`arcade_fight.txt`):

| Screenshot | Identical | Within 1 | Within 2 | More than 32 off |
|---|---|---|---|---|
| character select | 48.4% | 94.95% | 99.76% | 1 px |
| versus | 70.2% | 99.64% | 99.99% | 0 |
| fight0 | 67.2% | 99.58% | 99.91% | 134 px |
| fight_assist | 62.2% | 99.36% | 99.94% | 53 px |
| fight1 | 67.2% | 99.59% | 99.94% | 108 px |

Before the 8.8 blend, 50 screenshots of seven scripts (gallery,
training_pause, vs_stage2, charsel, long_fight, button_config, vs_two_pads):
48 were at 99.17% or better within 2 and the two character-select shots were
at 95.6% and 97.8% (stacked translucent layers, fixed by the 8.8 blend).
That sweep has not been repeated since.

What still differs:

- **Texel ties** in scaled sprites: the pixels more than 32 levels off in
  fights sit on columns 13 pixels apart, where GL and the tie rule pick
  different texels. Checked on one frame by column spacing, not pixel by
  pixel.
- **13 pixels** in every `button_config` screenshot, constant, not traced.
- One-level differences across about a third of the picture: 8-bit bilinear
  weights and blend rounding.

The GL renderer is itself the measured one against reference renders
([RENDERER.md](RENDERER.md): characters exact, stages 99.6 to 100% within 2
levels). `swgl` has not been compared with the retail picture directly.

## Speed

Drawing per frame, i386 desktop build, `arcade_fight.txt`:

| Change | ms |
|---|---|
| First version (float shading per pixel) | 215 |
| Spans, gradients, 8-bit sampling, no libm floor in the sampler | 37 |
| Exact row spans, fixed-point stepping, divide per piece | 26 |
| 32-bit blend, fog and floor | 16.9 |
| Scalar-only inner loop | 16.3 |

A fight frame is about 5,000 triangles, 68,000 sprite fragments and 540,000
stage-mesh fragments (bilinear, palette). In the translated build the
rasteriser is 82% of the time and the game step 1 to 2 ms.
