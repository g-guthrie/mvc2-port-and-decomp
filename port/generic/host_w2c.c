/* Native host for the wasm2c translation of mvc2g: plain C, any CPU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(int argc, char **argv)
{
    static w2c_mvc2g m;
    struct w2c_env env = { fopen(argv[1], "rb"), &m };
    struct w2c_wasi__snapshot__preview1 wasi = { &m };
    long i, n = strtol(argv[2], 0, 0);
    if (!env.disc) return 1;
    fseeko(env.disc, 0, SEEK_END);
    wasm_rt_init();
    wasm2c_mvc2g_instantiate(&m, &env, &wasi);
    w2c_mvc2g_0x5Finitialize(&m);
    if (w2c_mvc2g_mvc2g_boot(&m, (f64)ftello(env.disc))) return 2;
    for (i = 0; i < n; i++) {
        w2c_mvc2g_mvc2g_frame(&m);
        printf("%ld %08x %06x\n", i, w2c_mvc2g_mvc2g_state_hash(&m), w2c_mvc2g_mvc2g_game_state(&m));
    }
    return 0;
}
