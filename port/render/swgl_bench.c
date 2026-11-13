/*
 * swgl_bench: the rasteriser's main case in isolation (testing).
 *
 *   cc -O2 -DPORT_SWGL -Iport/platform port/render/swgl.c port/render/swgl_bench.c -lm -o swgl_bench
 *   ./swgl_bench [FRAMES]                     prints a checksum of the canvas and ms per frame
 *   valgrind --tool=callgrind ./swgl_bench 4  instructions (exact, repeatable)
 *
 * It draws what a fight's stage is made of (SWGL.md): small triangles in
 * perspective with the mesh program, an index texture through a palette row,
 * bilinear, a varying vertex colour, depth GEQUAL, no blending; then a layer
 * of alpha-blended triangles over it.  About 600,000 pixels are written per
 * frame.  The checksum must not change when the rasteriser is only made
 * faster.
 */
#define _POSIX_C_SOURCE 200809L
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gl.h"
#include "../render/swgl.h"

#define W 640
#define H 480
#define GRID 40 /* GRID x GRID quads per layer */

typedef struct { float pos[3], nrm[3], col[4], uv[2]; } V;

static uint32_t rnd_state = 12345;
static uint32_t rnd(void) { rnd_state = rnd_state * 1664525u + 1013904223u; return rnd_state >> 8; }

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 20;
    GLuint prog, tex, pal, canvas, depth, fbo, vao, vbo, ibo;
    static uint8_t texels[256 * 256], palette[256 * 1024 * 4], out[W * H * 4];
    static V verts[(GRID + 1) * (GRID + 1)];
    static uint16_t idx[GRID * GRID * 6];
    /* column-major: clip = (x, y, 0.5 z, z), so w varies with z (perspective) and depth is flat */
    static const float mvp[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0.5f, 1, 0, 0, 0, 0 };
    static const float vp[4] = { 1, 0, 1, 0 }, one4[4] = { 1, 1, 1, 1 }, cscale[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    static const float uv0[4] = { 1, 0, 0, 0 }, uv1[4] = { 0, 1, 0, 0 }, zero4[4] = { 0, 0, 0, 0 };
    static const float fog[2] = { 1e7f, 0 }, fogcol[3] = { 0, 0, 0 };
    uint32_t sum = 2166136261u;
    double t0;
    int n = 0;

    swgl_install();
    swgl_window(W, H);
    for (int i = 0; i < 256 * 256; i++) texels[i] = (uint8_t)(rnd() ^ (i >> 3));
    for (int i = 0; i < 256 * 1024 * 4; i++) palette[i] = (uint8_t)rnd();
    for (int i = 3; i < 256 * 1024 * 4; i += 4) palette[i] = (uint8_t)(128 + (palette[i] >> 1)); /* mostly opaque */

    prog = glCreateProgram();
    glBindAttribLocation(prog, 0, "a_pos");
    glBindAttribLocation(prog, 1, "a_nrm");
    glBindAttribLocation(prog, 2, "a_col");
    glBindAttribLocation(prog, 3, "a_uv");
    glLinkProgram(prog);
    glUseProgram(prog);
#define U(name) glGetUniformLocation(prog, name)
    glUniformMatrix4fv(U("u_mvp"), 1, GL_FALSE, mvp);
    glUniformMatrix4fv(U("u_clipm"), 1, GL_FALSE, mvp);
    glUniform4fv(U("u_vp"), 1, vp);
    glUniform1f(U("u_vpz"), 1.f);
    glUniform1i(U("u_flags"), 32); /* MF_TEX */
    glUniform4fv(U("u_cscale"), 1, cscale);
    glUniform4fv(U("u_mod"), 1, one4);
    glUniform4fv(U("u_matcol"), 1, one4);
    glUniform4fv(U("u_uv0"), 1, uv0);
    glUniform4fv(U("u_uv1"), 1, uv1);
    glUniform4fv(U("u_uv3"), 1, zero4);
    glUniform2fv(U("u_fog"), 1, fog);
    glUniform3fv(U("u_fogcol"), 1, fogcol);
    glUniform1f(U("u_lclamp"), 128.f);
    glUniform1i(U("u_adc"), 0);
    glUniform1i(U("u_mode"), 2);
    glUniform1i(U("u_linear"), 1);
    glUniform1i(U("u_clamp"), 0);
    glUniform1f(U("u_palrow"), 7.f);

    glGenTextures(1, &pal);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, pal);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, palette);
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, 256, 256, 0, GL_RED, GL_UNSIGNED_BYTE, texels);

    glGenTextures(1, &canvas);
    glBindTexture(GL_TEXTURE_2D, canvas);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, canvas, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    glBindTexture(GL_TEXTURE_2D, tex);
    glViewport(0, 0, W, H);

    /* a grid in perspective: w from 1 (bottom) to 4 (top), like a floor */
    for (int j = 0; j <= GRID; j++)
        for (int i = 0; i <= GRID; i++) {
            V *v = &verts[j * (GRID + 1) + i];
            const float w = 1.f + 3.f * (float)j / GRID;
            const float x = -1.f + 2.f * (float)i / GRID + (i && i < GRID ? (float)(rnd() % 100) / 8000.f : 0.f);
            const float y = -1.f + 2.f * (float)j / GRID;
            v->pos[0] = x * w; v->pos[1] = y * w; v->pos[2] = w;
            v->nrm[0] = 0; v->nrm[1] = 0; v->nrm[2] = 1;
            for (int k = 0; k < 4; k++) v->col[k] = (float)(160 + rnd() % 96);
            v->uv[0] = 3.f * (float)i / GRID; v->uv[1] = 3.f * w;
        }
    for (int j = 0; j < GRID; j++)
        for (int i = 0; i < GRID; i++) {
            const uint16_t a = (uint16_t)(j * (GRID + 1) + i), b = a + 1, c = (uint16_t)(a + GRID + 1), d = c + 1;
            const uint16_t q[6] = { a, b, c, b, d, c };
            memcpy(&idx[n], q, sizeof q);
            n += 6;
        }
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, nrm));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, col));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(V), (void *)offsetof(V, uv));
    glGenBuffers(1, &ibo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof idx, idx, GL_STATIC_DRAW);

    t0 = now_sec();
    for (int f = 0; f < frames; f++) {
        glDisable(GL_SCISSOR_TEST);
        glDepthMask(GL_TRUE);
        glClearColor(0.1f, 0.2f, 0.3f, 1.f);
        gl_clear_depth(0.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_GEQUAL);
        /* opaque layer, drawn twice: the second pass is what the depth test sees again */
        glDisable(GL_BLEND);
        glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, (void *)0);
        glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, (void *)0);
        /* a blended layer over the lower half */
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glDrawElements(GL_TRIANGLES, n / 2, GL_UNSIGNED_SHORT, (void *)0);
    }
    t0 = now_sec() - t0;
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out);
    for (int i = 0; i < W * H * 4; i++) sum = (sum ^ out[i]) * 16777619u;
    printf("swgl_bench: %d frames, checksum %08x, %.2f ms per frame\n", frames, sum, frames ? t0 * 1000.0 / frames : 0.0);
    return 0;
}
