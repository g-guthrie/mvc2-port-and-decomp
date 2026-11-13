/*
 * mvc2g: the game as one self-contained module with a small host interface
 * (the doomgeneric shape).  The module is the wasm32 build; a host either
 * runs the .wasm or compiles its wasm2c translation, which is plain C for
 * any CPU and pointer size.
 *
 * Host provides:  page_disc_read(offset, n, dst)   bytes of the disc image
 * Module exports: mvc2g_boot(disc_size)            init path, 0 = ok
 *                 mvc2g_frame()                    one 60 Hz game step
 *                 mvc2g_state_hash()               FNV-1a of EE BSS + heap
 *                 mvc2g_game_state()               g[0] << 16 | g[1] << 8 | g[2]
 *
 *                 mvc2g_pad(port, buttons)         PS2 pad bits (platform/pad.h), before a step
 *                 mvc2g_draw(on)                   0: the next steps draw nothing (frame skip)
 *                 mvc2g_canvas()                   the picture: 640x480 RGBA8, top row first
 *                 mvc2g_audio(frames)              that many 48 kHz stereo frames, signed 16-bit
 *
 * The picture is drawn by the port's renderer on render/swgl.c (no GPU), the
 * sound is mixed by port/audio (no device): the host only shows the canvas
 * and plays the samples.  One game step is 1/60 s, i.e. 800 frames of sound.
 */
#include <stdint.h>
#include <emscripten.h>
#include "../platform/fs.h"
#include "../host/host_loop.h"
#include "../render/render.h"
#include "../render/swgl.h"

#include "../audio/audio.h"

void fe_render_init(void);
int fe_audio_init(int open_device);

extern unsigned char mvc2_ee_bss[];
extern unsigned char *host_ee_heap_base(void);
int host_disc_open_source(FsSource src);

#define EE_BSS_SIZE (0x00634380u - 0x004C2580u)
#define EE_HEAP_SIZE (28u << 20)

/* No settings store here: every setting keeps its default. */
const char *settings_get(const char *key) { (void)key; return 0; }

EMSCRIPTEN_KEEPALIVE int mvc2g_boot(double disc_size)
{
    FsSource s;
    mvc2_trace_state = 0;
    fs_source_open_page(&s, (uint64_t)disc_size);
    if (host_disc_open_source(s)) return -1;
    swgl_install();
    swgl_window(FL_SCREEN_W, FL_SCREEN_H);
    if (r_init()) return -2;
    fe_render_init();
    fe_audio_init(0);
    mvc2_boot(1);
    return 0;
}

EMSCRIPTEN_KEEPALIVE void mvc2g_pad(int port, int buttons)
{
    PadRaw p = { (uint16_t)buttons, 0x80, 0x80, 0x80, 0x80, 1, 0 };
    host_pad_set(port, &p);
}

/* A device too slow to draw every frame still runs every game step: it turns
 * drawing off for the steps it will not show.  The game state is the same
 * either way. */
EMSCRIPTEN_KEEPALIVE void mvc2g_draw(int on) { r_set_discard(!on); }

EMSCRIPTEN_KEEPALIVE const int16_t *mvc2g_audio(int frames)
{
    enum { MAX = AUDIO_RATE / 10 };
    static float mix[MAX * 2];
    static int16_t pcm[MAX * 2];
    if (frames > MAX) frames = MAX;
    if (frames < 0) frames = 0;
    audio_render(mix, frames);
    for (int i = 0; i < 2 * frames; i++) {
        float x = mix[i] * 32767.f;
        pcm[i] = (int16_t)(x > 32767.f ? 32767.f : x < -32768.f ? -32768.f : x);
    }
    return pcm;
}

EMSCRIPTEN_KEEPALIVE const uint8_t *mvc2g_canvas(void)
{
    static uint8_t rgba[FL_SCREEN_W * FL_SCREEN_H * 4];
    r_read_canvas(rgba);
    return rgba;
}

EMSCRIPTEN_KEEPALIVE void mvc2g_frame(void) { mvc2_frame(); }
EMSCRIPTEN_KEEPALIVE int mvc2g_game_state(void) { return mvc2_game_state(); }

static uint32_t fnv(uint32_t h, const unsigned char *p, uint32_t n)
{
    while (n--) h = (h ^ *p++) * 16777619u;
    return h;
}

EMSCRIPTEN_KEEPALIVE uint32_t mvc2g_state_hash(void)
{
    uint32_t h = fnv(2166136261u, mvc2_ee_bss, EE_BSS_SIZE);
    unsigned char *heap = host_ee_heap_base();
    return heap ? fnv(h, heap, EE_HEAP_SIZE) : h;
}
