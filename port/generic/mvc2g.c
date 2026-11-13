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
 *                 mvc2g_canvas()                   the picture: 640x480 RGBA8, top row first
 *
 * The picture is drawn by the port's renderer on render/swgl.c (no GPU).
 * Sound follows.
 */
#include <stdint.h>
#include <emscripten.h>
#include "../platform/fs.h"
#include "../host/host_loop.h"
#include "../render/render.h"
#include "../render/swgl.h"

void fe_render_init(void);

extern unsigned char mvc2_ee_bss[];
extern unsigned char *host_ee_heap_base(void);
int host_disc_open_source(FsSource src);

#define EE_BSS_SIZE (0x00634380u - 0x004C2580u)
#define EE_HEAP_SIZE (28u << 20)

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
    mvc2_boot(1);
    return 0;
}

EMSCRIPTEN_KEEPALIVE void mvc2g_pad(int port, int buttons)
{
    PadRaw p = { (uint16_t)buttons, 0x80, 0x80, 0x80, 0x80, 1, 0 };
    host_pad_set(port, &p);
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
