/*
 * A playable host for the universal build: SDL2 window, sound, keyboard and
 * game controllers around the translated C.  No GL: the module draws the
 * picture itself and this file only shows it.
 *
 *   cc -O2 -w -I/usr/share/wabt/wasm2c -Ibuild/port-generic $(sdl2-config --cflags) \
 *      port/generic/host_sdl.c build/port-generic/mvc2g_w2c.o /usr/share/wabt/wasm2c/wasm-rt-impl.c \
 *      -lm $(sdl2-config --libs) -o mvc2g_sdl
 *   ./mvc2g_sdl DISC [--scale N] [--fullscreen] [--frames N] [--shot FILE.ppm] [--mash]
 *   (--frames, --shot and --mash are for testing: stop after N steps, save the
 *   last picture, press START and CROSS in turn to get from the title into a fight)
 *
 * It is the whole of what a device has to provide (port/generic/README.md):
 * disc bytes, a picture, sound, buttons, and a call 60 times a second.
 *
 * Speed: the game always runs 60 steps a second.  When the device cannot draw
 * that many pictures, steps are run without drawing (mvc2g_draw(0)) and only
 * the newest one is shown, so a slow device shows fewer frames, not a slower
 * game.
 *
 * Keys: arrows; A S light/heavy punch; Z X light/heavy kick; D C assists;
 * Q W the two spare buttons; Enter start; right Shift select; Esc quits.
 * Controllers by position: south = light kick, east = heavy kick, west =
 * light punch, north = heavy punch, bumpers = assists.  The first controller
 * is player 1, the second player 2.
 */
#define MVC2G_NO_MAIN
#include "host_w2c.c"
#include <SDL.h>

/* PS2 pad bits (port/platform/pad.h) */
enum {
    P_SELECT = 0x0001, P_START = 0x0008, P_UP = 0x0010, P_RIGHT = 0x0020, P_DOWN = 0x0040, P_LEFT = 0x0080,
    P_L2 = 0x0100, P_R2 = 0x0200, P_L1 = 0x0400, P_R1 = 0x0800,
    P_TRIANGLE = 0x1000, P_CIRCLE = 0x2000, P_CROSS = 0x4000, P_SQUARE = 0x8000
};

static unsigned keyboard_bits(void)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    static const struct { SDL_Scancode sc; unsigned bit; } map[] = {
        { SDL_SCANCODE_UP, P_UP }, { SDL_SCANCODE_DOWN, P_DOWN }, { SDL_SCANCODE_LEFT, P_LEFT },
        { SDL_SCANCODE_RIGHT, P_RIGHT }, { SDL_SCANCODE_A, P_SQUARE }, { SDL_SCANCODE_S, P_TRIANGLE },
        { SDL_SCANCODE_Z, P_CROSS }, { SDL_SCANCODE_X, P_CIRCLE }, { SDL_SCANCODE_D, P_L1 },
        { SDL_SCANCODE_C, P_R1 }, { SDL_SCANCODE_Q, P_L2 }, { SDL_SCANCODE_W, P_R2 },
        { SDL_SCANCODE_RETURN, P_START }, { SDL_SCANCODE_RSHIFT, P_SELECT },
    };
    unsigned b = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (k[map[i].sc]) b |= map[i].bit;
    return b;
}

static unsigned controller_bits(SDL_GameController *c)
{
    static const struct { SDL_GameControllerButton btn; unsigned bit; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, P_CROSS }, { SDL_CONTROLLER_BUTTON_B, P_CIRCLE },
        { SDL_CONTROLLER_BUTTON_X, P_SQUARE }, { SDL_CONTROLLER_BUTTON_Y, P_TRIANGLE },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, P_L1 }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, P_R1 },
        { SDL_CONTROLLER_BUTTON_START, P_START }, { SDL_CONTROLLER_BUTTON_BACK, P_SELECT },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, P_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, P_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, P_LEFT }, { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, P_RIGHT },
    };
    unsigned b = 0;
    int x, y;
    if (!c) return 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (SDL_GameControllerGetButton(c, map[i].btn)) b |= map[i].bit;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000) b |= P_L2;
    if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000) b |= P_R2;
    x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (x < -14000) b |= P_LEFT;
    if (x > 14000) b |= P_RIGHT;
    if (y < -14000) b |= P_UP;
    if (y > 14000) b |= P_DOWN;
    return b;
}

int main(int argc, char **argv)
{
    static w2c_mvc2g m;
    static u8 px[640 * 480 * 4], pcm[800 * 4];
#ifdef MVC2G_EMBED_DISC
    struct w2c_env env = { 0, &m };
    int a = 1;
#else
    struct w2c_env env = { argc > 1 ? fopen(argv[1], "rb") : 0, &m };
    int a = 2;
#endif
    struct w2c_wasi__snapshot__preview1 wasi = { &m };
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_AudioDeviceID dev = 0;
    SDL_AudioSpec want, have;
    SDL_GameController *pads[2] = { 0, 0 };
    int scale = 2, fullscreen = 0, quit = 0, mash = 0;
    long frames = -1, step = 0, shown = 0, skipped = 0;
    const char *shot = 0;
    double disc_size;
    Uint64 freq, next;

#ifndef MVC2G_EMBED_DISC
    if (!env.disc) { fprintf(stderr, "usage: %s DISC [--scale N] [--fullscreen] [--frames N] [--shot FILE.ppm]\n", argv[0]); return 1; }
    fseeko(env.disc, 0, SEEK_END);
    disc_size = (double)ftello(env.disc);
#else
    disc_size = (double)(mvc2g_disc_end - mvc2g_disc);
#endif
    for (; a < argc; a++) {
        if (!strcmp(argv[a], "--scale") && a + 1 < argc) scale = atoi(argv[++a]);
        else if (!strcmp(argv[a], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[a], "--frames") && a + 1 < argc) frames = strtol(argv[++a], 0, 0);
        else if (!strcmp(argv[a], "--shot") && a + 1 < argc) shot = argv[++a];
        else if (!strcmp(argv[a], "--mash")) mash = 1;
    }
    if (scale < 1) scale = 1;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("Marvel vs. Capcom 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640 * scale, 480 * scale,
                           SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    ren = win ? SDL_CreateRenderer(win, -1, 0) : 0;
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, 640, 480) : 0;
    if (!tex) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(ren, 640, 480);
    SDL_zero(want);
    want.freq = 48000; want.format = AUDIO_S16LSB; want.channels = 2; want.samples = 1024;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
    else fprintf(stderr, "no sound: %s\n", SDL_GetError());

    wasm_rt_init();
    wasm2c_mvc2g_instantiate(&m, &env, &wasi);
    w2c_mvc2g_0x5Finitialize(&m);
    if (w2c_mvc2g_mvc2g_boot(&m, disc_size)) { fprintf(stderr, "the disc image was not recognised\n"); return 2; }

    freq = SDL_GetPerformanceFrequency();
    next = SDL_GetPerformanceCounter();
    while (!quit && (frames < 0 || step < frames)) {
        SDL_Event e;
        Uint64 now;
        int due, i;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)) quit = 1;
            if (e.type == SDL_CONTROLLERDEVICEADDED) {
                for (i = 0; i < 2; i++)
                    if (!pads[i]) { pads[i] = SDL_GameControllerOpen(e.cdevice.which); break; }
            }
            if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                for (i = 0; i < 2; i++)
                    if (pads[i] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == e.cdevice.which) {
                        SDL_GameControllerClose(pads[i]);
                        pads[i] = 0;
                    }
            }
        }
        /* how many 1/60 s steps are due; when behind, at most 4, and only the last is drawn */
        now = SDL_GetPerformanceCounter();
        if (now < next) {
            SDL_Delay((Uint32)((next - now) * 1000 / freq));
            continue;
        }
        due = (int)((now - next) * 60 / freq) + 1;
        if (due > 4) { due = 4; next = now; }
        for (i = 0; i < due && (frames < 0 || step < frames); i++, step++) {
            const int draw = i == due - 1;
            const int ph = (int)(step % 40);
            const unsigned auto_bits = !mash || w2c_mvc2g_mvc2g_game_state(&m) == 0x020102 ? 0
                                       : ph < 4 ? P_START : ph >= 20 && ph < 24 ? P_CROSS : 0;
            w2c_mvc2g_mvc2g_pad(&m, 0, keyboard_bits() | controller_bits(pads[0]) | auto_bits);
            w2c_mvc2g_mvc2g_pad(&m, 1, controller_bits(pads[1]));
            w2c_mvc2g_mvc2g_draw(&m, draw);
            w2c_mvc2g_mvc2g_frame(&m);
            if (dev && SDL_GetQueuedAudioSize(dev) < 48000) { /* never more than a quarter second queued */
                m_get(&m, w2c_mvc2g_mvc2g_audio(&m, 800), pcm, sizeof pcm);
                SDL_QueueAudio(dev, pcm, sizeof pcm);
            } else {
                w2c_mvc2g_mvc2g_audio(&m, 800); /* keep the mixer in step */
            }
            if (!draw) skipped++;
            next += freq / 60;
        }
        m_get(&m, w2c_mvc2g_mvc2g_canvas(&m), px, sizeof px);
        SDL_UpdateTexture(tex, NULL, px, 640 * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
        SDL_RenderPresent(ren);
        shown++;
    }
    fprintf(stderr, "[mvc2g] %ld steps, %ld pictures shown, %ld steps not drawn, state %06x\n", step, shown, skipped,
            w2c_mvc2g_mvc2g_game_state(&m));
    if (shot) {
        FILE *f = fopen(shot, "wb");
        if (f) {
            fprintf(f, "P6\n640 480\n255\n");
            for (int k = 0; k < 640 * 480; k++) fwrite(px + 4 * k, 1, 3, f);
            fclose(f);
        }
    }
    if (dev) SDL_CloseAudioDevice(dev);
    SDL_Quit();
    return 0;
}
