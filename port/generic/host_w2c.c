#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
/* Native host for the wasm2c translation of mvc2g: plain C, any CPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mvc2g_w2c.h"

struct w2c_env { FILE *disc; w2c_mvc2g *m; };
struct w2c_wasi__snapshot__preview1 { w2c_mvc2g *m; };
static u8 *mem(w2c_mvc2g *m) { return w2c_mvc2g_memory(m)->data; }
static u32 rd32(u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void wr32(u8 *p, u32 v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

u32 w2c_env_0x5F_syscall_getdents64(struct w2c_env *e, u32 a, u32 b, u32 c) { return (u32)-1; }
u32 w2c_env_0x5F_syscall_rmdir(struct w2c_env *e, u32 a) { return (u32)-1; }
u32 w2c_env_0x5F_syscall_unlinkat(struct w2c_env *e, u32 a, u32 b, u32 c) { return (u32)-1; }
u32 w2c_env_emscripten_asm_const_int(struct w2c_env *e, u32 a, u32 b, u32 c) { return 0; }
u32 w2c_env_page_disc_read(struct w2c_env *e, f64 off, u32 n, u32 dst)
{
    fseeko(e->disc, (off_t)off, SEEK_SET);
    return (u32)fread(mem(e->m) + dst, 1, n, e->disc);
}
u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1 *w, u32 id, u64 prec, u32 out) { memset(mem(w->m) + out, 0, 8); return 0; }
u32 w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1 *w, u32 a, u32 b) { return 0; }
u32 w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1 *w, u32 a, u32 b) { wr32(mem(w->m) + a, 0); wr32(mem(w->m) + b, 0); return 0; }
u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1 *w, u32 a) { return 0; }
u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1 *w, u32 a, u32 b, u32 c, u32 d) { return 8; }
u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1 *w, u32 a, u64 b, u32 c, u32 d) { return 8; }
u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1 *w, u32 fd, u32 iov, u32 n, u32 pn)
{
    u32 t = 0, i;
    for (i = 0; i < n; i++) {
        u32 p = rd32(mem(w->m) + iov + 8 * i), l = rd32(mem(w->m) + iov + 8 * i + 4);
        fwrite(mem(w->m) + p, 1, l, stderr);
        t += l;
    }
    wr32(mem(w->m) + pn, t);
    return 0;
}
void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1 *w, u32 c) { exit((int)c); }

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* mvc2g_native DISC FRAMES [--hash] [--mash] [--shot FRAME:FILE.ppm]...
 * --hash prints the state hash of every frame (the wasm comparison);
 * --mash presses START and CROSS in turn, which gets from the title into a fight. */
int main(int argc, char **argv)
{
    static w2c_mvc2g m;
    struct w2c_env env = { argc > 1 ? fopen(argv[1], "rb") : 0, &m };
    struct w2c_wasi__snapshot__preview1 wasi = { &m };
    long i, n = argc > 2 ? strtol(argv[2], 0, 0) : 0, shot_at[16];
    const char *shot_file[16];
    int hash = 0, mash = 0, nshot = 0, a;
    double t0, step = 0.0;
    if (!env.disc) { fprintf(stderr, "usage: %s DISC FRAMES [--hash] [--mash] [--shot FRAME:FILE.ppm]\n", argv[0]); return 1; }
    for (a = 3; a < argc; a++) {
        if (!strcmp(argv[a], "--hash")) hash = 1;
        else if (!strcmp(argv[a], "--mash")) mash = 1;
        else if (!strcmp(argv[a], "--shot") && a + 1 < argc && nshot < 16) {
            char *c = strchr(argv[++a], ':');
            if (c) { shot_at[nshot] = strtol(argv[a], 0, 0); shot_file[nshot++] = c + 1; }
        }
    }
    fseeko(env.disc, 0, SEEK_END);
    wasm_rt_init();
    wasm2c_mvc2g_instantiate(&m, &env, &wasi);
    w2c_mvc2g_0x5Finitialize(&m);
    if (w2c_mvc2g_mvc2g_boot(&m, (f64)ftello(env.disc))) return 2;
    for (i = 0; i < n; i++) {
        if (mash) {
            int ph = (int)(i % 40);
            w2c_mvc2g_mvc2g_pad(&m, 0, ph < 4 ? 0x0008 : ph >= 20 && ph < 24 ? 0x4000 : 0);
        }
        t0 = now_sec();
        w2c_mvc2g_mvc2g_frame(&m);
        step += now_sec() - t0;
        if (hash)
            printf("%ld %08x %06x\n", i, w2c_mvc2g_mvc2g_state_hash(&m), w2c_mvc2g_mvc2g_game_state(&m));
        for (a = 0; a < nshot; a++)
            if (shot_at[a] == i) {
                const u8 *px = mem(&m) + w2c_mvc2g_mvc2g_canvas(&m);
                FILE *f = fopen(shot_file[a], "wb");
                int k;
                if (!f) continue;
                fprintf(f, "P6\n640 480\n255\n");
                for (k = 0; k < 640 * 480; k++) fwrite(px + 4 * k, 1, 3, f);
                fclose(f);
            }
    }
    fprintf(stderr, "[mvc2g] %ld frames, %.2f ms per frame (game step + drawing), state %06x\n", n,
            n ? step * 1000.0 / (double)n : 0.0, w2c_mvc2g_mvc2g_game_state(&m));
    return 0;
}
