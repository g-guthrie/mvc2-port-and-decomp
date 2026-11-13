/*
 * swgl: the GL calls the port's renderer makes, done on the CPU.
 *
 * r_gl.c and r_mesh.c are written against GL ES 3.  This file implements
 * that subset (PORT_GL_FUNCS in platform/gl.h) in portable C with no GPU, so
 * the same renderer code draws on any device: swgl_install() points the
 * p_gl* entries here instead of at a GL driver.
 *
 * GLSL is not interpreted.  The renderer has three programs, and each is
 * written out below in C (batch = k_vs/k_fs in r_gl.c, mesh = k_mvs/k_mfs in
 * r_mesh.c, present = k_pp_vs/k_pp_fs).  A program is recognised by the
 * attribute names bound to it.  A change to a shader needs the same change
 * here.
 *
 * Rasteriser: pixel centres, top-left fill rule, 8 sub-pixel bits, clipping
 * against -w <= z <= w, 24-bit depth, 8-bit RGBA colour, rows stored bottom
 * up as GL does.
 */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../platform/gl.h"
#include "swgl.h"

#ifndef GL_DIRECT

/* ---------------- objects ---------------- */
typedef struct { int w, h, fmt; uint8_t *px; int linear, clamp_s, clamp_t; } Tex; /* fmt: 4 RGBA8, 1 R8 */
typedef struct { uint8_t *data; size_t size; } Buf;
typedef struct { int on, size, type, norm, stride; size_t off; GLuint buf; } Attr;
typedef struct { Attr a[8]; GLuint ebo; } Vao;
typedef struct { char name[24]; float f[16]; int i; } Uni;
enum { PK_NONE, PK_BATCH, PK_MESH, PK_PRESENT };
typedef struct { int kind; Uni u[40]; int nu; char attr[8][12]; } Prog;
typedef struct { int w, h; uint32_t *z; } Rb;
typedef struct { GLuint color, depth; } Fbo;

#define MAXOBJ 4096
static Tex *texs[MAXOBJ];
static Buf *bufs[MAXOBJ];
static Vao *vaos[MAXOBJ];
static Prog *progs[64];
static Rb *rbs[64];
static Fbo *fbos[64];

static struct {
    GLuint tex_bound[2], active, array_buf, vao, prog, fbo, rb;
    int vp[4], sc[4];
    int scissor, blend, depth;
    GLenum bl_eq, bl_s, bl_d, zfunc;
    int zmask, cmask[4];
    float clear[4], clear_z;
    /* framebuffer 0: the window */
    int win_w, win_h;
    uint8_t *win;
} G;

static Vao vao0;

static GLuint new_id(void **tab, int max, size_t sz)
{
    for (int i = 1; i < max; i++)
        if (!tab[i]) { tab[i] = calloc(1, sz); return (GLuint)i; }
    return 0;
}

static Vao *cur_vao(void) { return G.vao && G.vao < MAXOBJ && vaos[G.vao] ? vaos[G.vao] : &vao0; }

/* ---------------- render target ---------------- */
typedef struct { int w, h; uint8_t *c; uint32_t *z; } Target;

static Target target(void)
{
    Target t = { 0, 0, 0, 0 };
    if (G.fbo && fbos[G.fbo]) {
        Tex *c = fbos[G.fbo]->color < MAXOBJ ? texs[fbos[G.fbo]->color] : 0;
        Rb *d = fbos[G.fbo]->depth < 64 ? rbs[fbos[G.fbo]->depth] : 0;
        if (c && c->fmt == 4) { t.w = c->w; t.h = c->h; t.c = c->px; }
        if (d && d->w == t.w && d->h == t.h) t.z = d->z;
    } else {
        t.w = G.win_w; t.h = G.win_h; t.c = G.win;
    }
    return t;
}

void swgl_window(int w, int h)
{
    if (w == G.win_w && h == G.win_h) return;
    free(G.win);
    G.win_w = w; G.win_h = h;
    G.win = (uint8_t *)calloc((size_t)w * h, 4);
}

const uint8_t *swgl_window_pixels(int *w, int *h)
{
    *w = G.win_w; *h = G.win_h;
    return G.win;
}

/* ---------------- simple state ---------------- */
static const GLubyte *GL_APIENTRY sw_glGetString(GLenum n)
{
    return (const GLubyte *)(n == GL_VERSION ? "OpenGL ES 3.0 swgl" : n == GL_RENDERER ? "swgl (software)" : "swgl");
}
static void GL_APIENTRY sw_glGetIntegerv(GLenum p, GLint *v)
{
    switch (p) {
    case GL_SCISSOR_BOX: memcpy(v, G.sc, sizeof G.sc); break;
    case GL_VIEWPORT: memcpy(v, G.vp, sizeof G.vp); break;
    case GL_BLEND_SRC_RGB: *v = (GLint)G.bl_s; break;
    case GL_BLEND_DST_RGB: *v = (GLint)G.bl_d; break;
    case GL_BLEND_EQUATION_RGB: *v = (GLint)G.bl_eq; break;
    case GL_DEPTH_FUNC: *v = (GLint)G.zfunc; break;
    case GL_BLEND: *v = G.blend; break;
    case GL_DEPTH_TEST: *v = G.depth; break;
    case GL_MAX_TEXTURE_SIZE: *v = 4096; break;
    default: *v = 0;
    }
}
static GLenum GL_APIENTRY sw_glGetError(void) { return GL_NO_ERROR; }
static void GL_APIENTRY sw_glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    G.vp[0] = x; G.vp[1] = y; G.vp[2] = w; G.vp[3] = h;
}
static void GL_APIENTRY sw_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    G.clear[0] = r; G.clear[1] = g; G.clear[2] = b; G.clear[3] = a;
}
void swgl_clear_depth(float d) { G.clear_z = d; }
static void set_cap(GLenum c, int on)
{
    if (c == GL_SCISSOR_TEST) G.scissor = on;
    else if (c == GL_BLEND) G.blend = on;
    else if (c == GL_DEPTH_TEST) G.depth = on;
}
static void GL_APIENTRY sw_glEnable(GLenum c) { set_cap(c, 1); }
static void GL_APIENTRY sw_glDisable(GLenum c) { set_cap(c, 0); }
static void GL_APIENTRY sw_glBlendFunc(GLenum s, GLenum d) { G.bl_s = s; G.bl_d = d; }
static void GL_APIENTRY sw_glBlendEquation(GLenum e) { G.bl_eq = e; }
static void GL_APIENTRY sw_glDepthFunc(GLenum f) { G.zfunc = f; }
static void GL_APIENTRY sw_glDepthMask(GLboolean m) { G.zmask = m != 0; }
static void GL_APIENTRY sw_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
    G.cmask[0] = r != 0; G.cmask[1] = g != 0; G.cmask[2] = b != 0; G.cmask[3] = a != 0;
}
static void GL_APIENTRY sw_glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    G.sc[0] = x; G.sc[1] = y; G.sc[2] = w; G.sc[3] = h;
}

/* The pixel rectangle draws and clears may touch. */
static void bounds(const Target *t, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = 0; *y0 = 0; *x1 = t->w; *y1 = t->h;
    if (G.scissor) {
        if (G.sc[0] > *x0) *x0 = G.sc[0];
        if (G.sc[1] > *y0) *y0 = G.sc[1];
        if (G.sc[0] + G.sc[2] < *x1) *x1 = G.sc[0] + G.sc[2];
        if (G.sc[1] + G.sc[3] < *y1) *y1 = G.sc[1] + G.sc[3];
    }
}

static uint8_t unorm8(float f) { return f <= 0.f ? 0 : f >= 1.f ? 255 : (uint8_t)(f * 255.f + 0.5f); }
static uint32_t depth24(float z) { return z <= 0.f ? 0 : z >= 1.f ? 0xFFFFFFu : (uint32_t)((double)z * 16777215.0 + 0.5); }

/* MVC2_SWGL_STATS=N (testing): draw nothing before canvas frame N, then time
 * the draw calls and print the mean per frame at exit. */
static struct { long frame, from, frames, stop; double sec, vsec; int on; long long visited, fb, fm, tris, verts; } St;
static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}
static void stats_report(void)
{
    if (St.frames > 0)
        fprintf(stderr, "[swgl] %ld frames, %.2f ms of drawing per frame (%.2f ms vertex stage); per frame: %lld tris, %lld verts, "
                "%lld pixels visited, %lld batch fragments, %lld mesh fragments\n", St.frames,
                St.sec * 1000.0 / (double)St.frames, St.vsec * 1000.0 / (double)St.frames, St.tris / St.frames,
                St.verts / St.frames, St.visited / St.frames, St.fb / St.frames, St.fm / St.frames);
}

static void GL_APIENTRY sw_glClear(GLbitfield mask)
{
    Target t = target();
    int x0, y0, x1, y1;
    if (G.fbo && (mask & GL_COLOR_BUFFER_BIT)) {
        St.frame++;
        if (St.on && St.frame > St.from) St.frames++;
        if (St.on && St.stop && St.frames > St.stop) { St.frames = St.stop; exit(0); } /* MVC2_SWGL_STOP */
    }
    if (!t.c) return;
    bounds(&t, &x0, &y0, &x1, &y1);
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint8_t c[4] = { unorm8(G.clear[0]), unorm8(G.clear[1]), unorm8(G.clear[2]), unorm8(G.clear[3]) };
        const int all = G.cmask[0] & G.cmask[1] & G.cmask[2] & G.cmask[3];
        for (int y = y0; y < y1 && x0 < x1; y++) {
            uint8_t *p = t.c + ((size_t)y * t.w + x0) * 4;
            if (all && y > y0) { /* every row is the same: copy the first one */
                memcpy(p, t.c + ((size_t)y0 * t.w + x0) * 4, (size_t)(x1 - x0) * 4);
                continue;
            }
            for (int x = x0; x < x1; x++, p += 4)
                for (int k = 0; k < 4; k++)
                    if (G.cmask[k]) p[k] = c[k];
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && t.z && G.zmask) {
        uint32_t z = depth24(G.clear_z);
        for (int y = y0; y < y1 && x0 < x1; y++) {
            uint32_t *zr = t.z + (size_t)y * t.w + x0;
            if (y > y0) { memcpy(zr, t.z + (size_t)y0 * t.w + x0, (size_t)(x1 - x0) * sizeof *zr); continue; }
            for (int x = x0; x < x1; x++) zr[x - x0] = z;
        }
    }
}

/* ---------------- textures ---------------- */
static void GL_APIENTRY sw_glGenTextures(GLsizei n, GLuint *o)
{
    for (int i = 0; i < n; i++) o[i] = new_id((void **)texs, MAXOBJ, sizeof(Tex));
}
static void GL_APIENTRY sw_glDeleteTextures(GLsizei n, const GLuint *o)
{
    for (int i = 0; i < n; i++)
        if (o[i] && o[i] < MAXOBJ && texs[o[i]]) { free(texs[o[i]]->px); free(texs[o[i]]); texs[o[i]] = 0; }
}
static void GL_APIENTRY sw_glActiveTexture(GLenum u) { G.active = u == GL_TEXTURE1 ? 1 : 0; }
static void GL_APIENTRY sw_glBindTexture(GLenum t, GLuint id) { (void)t; G.tex_bound[G.active] = id; }
static Tex *bound_tex(int unit)
{
    GLuint id = G.tex_bound[unit];
    return id && id < MAXOBJ ? texs[id] : 0;
}
static void GL_APIENTRY sw_glTexImage2D(GLenum tg, GLint lv, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum fmt,
                                        GLenum ty, const void *px)
{
    Tex *t = bound_tex(G.active);
    (void)tg; (void)lv; (void)b; (void)fmt; (void)ty;
    if (!t) return;
    free(t->px);
    t->w = w; t->h = h; t->fmt = ifmt == GL_R8 ? 1 : 4;
    t->px = (uint8_t *)calloc((size_t)w * h, (size_t)t->fmt);
    if (px) memcpy(t->px, px, (size_t)w * h * t->fmt);
}
static void GL_APIENTRY sw_glTexSubImage2D(GLenum tg, GLint lv, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt,
                                           GLenum ty, const void *px)
{
    Tex *t = bound_tex(G.active);
    (void)tg; (void)lv; (void)fmt; (void)ty;
    if (!t || !t->px || !px || x < 0 || y < 0 || x + w > t->w || y + h > t->h) return;
    for (int j = 0; j < h; j++)
        memcpy(t->px + ((size_t)(y + j) * t->w + x) * t->fmt, (const uint8_t *)px + (size_t)j * w * t->fmt,
               (size_t)w * t->fmt);
}
static void GL_APIENTRY sw_glTexParameteri(GLenum tg, GLenum p, GLint v)
{
    Tex *t = bound_tex(G.active);
    (void)tg;
    if (!t) return;
    if (p == GL_TEXTURE_MAG_FILTER) t->linear = v == GL_LINEAR;
    else if (p == GL_TEXTURE_WRAP_S) t->clamp_s = v == GL_CLAMP_TO_EDGE;
    else if (p == GL_TEXTURE_WRAP_T) t->clamp_t = v == GL_CLAMP_TO_EDGE;
}
static void GL_APIENTRY sw_glPixelStorei(GLenum p, GLint v) { (void)p; (void)v; }

/* ---------------- shaders and programs ---------------- */
static GLuint GL_APIENTRY sw_glCreateShader(GLenum t) { (void)t; return 1; }
static void GL_APIENTRY sw_glDeleteShader(GLuint s) { (void)s; }
static void GL_APIENTRY sw_glShaderSource(GLuint s, GLsizei n, const GLchar *const *src, const GLint *len)
{
    (void)s; (void)n; (void)src; (void)len;
}
static void GL_APIENTRY sw_glCompileShader(GLuint s) { (void)s; }
static void GL_APIENTRY sw_glGetShaderiv(GLuint s, GLenum p, GLint *v) { (void)s; (void)p; *v = 1; }
static void GL_APIENTRY sw_glGetShaderInfoLog(GLuint s, GLsizei n, GLsizei *len, GLchar *log)
{
    (void)s;
    if (n > 0) log[0] = 0;
    if (len) *len = 0;
}
static GLuint GL_APIENTRY sw_glCreateProgram(void) { return new_id((void **)progs, 64, sizeof(Prog)); }
static void GL_APIENTRY sw_glDeleteProgram(GLuint p)
{
    if (p && p < 64) { free(progs[p]); progs[p] = 0; }
}
static void GL_APIENTRY sw_glAttachShader(GLuint p, GLuint s) { (void)p; (void)s; }
static void GL_APIENTRY sw_glBindAttribLocation(GLuint p, GLuint i, const GLchar *name)
{
    if (p < 64 && progs[p] && i < 8) strncpy(progs[p]->attr[i], name, sizeof progs[p]->attr[i] - 1);
}
static void GL_APIENTRY sw_glLinkProgram(GLuint p)
{
    Prog *g = p < 64 ? progs[p] : 0;
    if (!g) return;
    if (!strcmp(g->attr[3], "a_pal")) g->kind = PK_BATCH;
    else if (!strcmp(g->attr[1], "a_nrm")) g->kind = PK_MESH;
    else g->kind = PK_PRESENT;
}
static void GL_APIENTRY sw_glGetProgramiv(GLuint p, GLenum n, GLint *v) { (void)p; (void)n; *v = 1; }
static void GL_APIENTRY sw_glGetProgramInfoLog(GLuint p, GLsizei n, GLsizei *len, GLchar *log)
{
    (void)p;
    if (n > 0) log[0] = 0;
    if (len) *len = 0;
}
static void GL_APIENTRY sw_glUseProgram(GLuint p) { G.prog = p; }
/* A location is program << 8 | slot, so glUniform* needs no current program. */
static GLint GL_APIENTRY sw_glGetUniformLocation(GLuint p, const GLchar *name)
{
    Prog *g = p < 64 ? progs[p] : 0;
    if (!g) return -1;
    for (int i = 0; i < g->nu; i++)
        if (!strcmp(g->u[i].name, name)) return (GLint)(p << 8 | (GLuint)i);
    if (g->nu == 40) return -1;
    strncpy(g->u[g->nu].name, name, sizeof g->u[0].name - 1);
    return (GLint)(p << 8 | (GLuint)g->nu++);
}
static Uni *uni_at(GLint loc)
{
    Prog *g;
    if (loc < 0) return 0;
    g = progs[(loc >> 8) & 63];
    return g && (loc & 255) < g->nu ? &g->u[loc & 255] : 0;
}
static void GL_APIENTRY sw_glUniform1i(GLint l, GLint v) { Uni *u = uni_at(l); if (u) u->i = v; }
static void GL_APIENTRY sw_glUniform1f(GLint l, GLfloat v) { Uni *u = uni_at(l); if (u) u->f[0] = v; }
static void GL_APIENTRY sw_glUniform2f(GLint l, GLfloat a, GLfloat b) { Uni *u = uni_at(l); if (u) { u->f[0] = a; u->f[1] = b; } }
static void setfv(GLint l, int n, const GLfloat *v)
{
    Uni *u = uni_at(l);
    if (u && n <= 16) memcpy(u->f, v, sizeof(float) * (size_t)n);
}
static void GL_APIENTRY sw_glUniform2fv(GLint l, GLsizei n, const GLfloat *v) { setfv(l, 2 * n, v); }
static void GL_APIENTRY sw_glUniform3fv(GLint l, GLsizei n, const GLfloat *v) { setfv(l, 3 * n, v); }
static void GL_APIENTRY sw_glUniform4fv(GLint l, GLsizei n, const GLfloat *v) { setfv(l, 4 * n, v); }
static void GL_APIENTRY sw_glUniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat *v)
{
    (void)t;
    setfv(l, 16 * n, v);
}
static const Uni *uni(const Prog *g, const char *name)
{
    static const Uni zero;
    for (int i = 0; i < g->nu; i++)
        if (!strcmp(g->u[i].name, name)) return &g->u[i];
    return &zero;
}

/* ---------------- buffers and vertex arrays ---------------- */
static void GL_APIENTRY sw_glGenBuffers(GLsizei n, GLuint *o)
{
    for (int i = 0; i < n; i++) o[i] = new_id((void **)bufs, MAXOBJ, sizeof(Buf));
}
static void GL_APIENTRY sw_glDeleteBuffers(GLsizei n, const GLuint *o)
{
    for (int i = 0; i < n; i++)
        if (o[i] && o[i] < MAXOBJ && bufs[o[i]]) { free(bufs[o[i]]->data); free(bufs[o[i]]); bufs[o[i]] = 0; }
}
static void GL_APIENTRY sw_glBindBuffer(GLenum t, GLuint id)
{
    if (t == GL_ELEMENT_ARRAY_BUFFER) cur_vao()->ebo = id;
    else G.array_buf = id;
}
static Buf *target_buf(GLenum t)
{
    GLuint id = t == GL_ELEMENT_ARRAY_BUFFER ? cur_vao()->ebo : G.array_buf;
    return id && id < MAXOBJ ? bufs[id] : 0;
}
static void GL_APIENTRY sw_glBufferData(GLenum t, GLsizeiptr size, const void *data, GLenum usage)
{
    Buf *b = target_buf(t);
    (void)usage;
    if (!b) return;
    free(b->data);
    b->size = (size_t)size;
    b->data = (uint8_t *)calloc(1, b->size ? b->size : 1);
    if (data) memcpy(b->data, data, b->size);
}
static void GL_APIENTRY sw_glBufferSubData(GLenum t, GLintptr off, GLsizeiptr size, const void *data)
{
    Buf *b = target_buf(t);
    if (b && (size_t)off + (size_t)size <= b->size) memcpy(b->data + off, data, (size_t)size);
}
static void GL_APIENTRY sw_glGenVertexArrays(GLsizei n, GLuint *o)
{
    for (int i = 0; i < n; i++) o[i] = new_id((void **)vaos, MAXOBJ, sizeof(Vao));
}
static void GL_APIENTRY sw_glDeleteVertexArrays(GLsizei n, const GLuint *o)
{
    for (int i = 0; i < n; i++)
        if (o[i] && o[i] < MAXOBJ && vaos[o[i]]) { free(vaos[o[i]]); vaos[o[i]] = 0; }
}
static void GL_APIENTRY sw_glBindVertexArray(GLuint id) { G.vao = id; }
static void GL_APIENTRY sw_glVertexAttribPointer(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride,
                                                 const void *off)
{
    Attr *a;
    if (i >= 8) return;
    a = &cur_vao()->a[i];
    a->size = size; a->type = (int)type; a->norm = norm != 0; a->stride = stride;
    a->off = (size_t)(uintptr_t)off; a->buf = G.array_buf;
}
static void GL_APIENTRY sw_glEnableVertexAttribArray(GLuint i) { if (i < 8) cur_vao()->a[i].on = 1; }
static void GL_APIENTRY sw_glDisableVertexAttribArray(GLuint i) { if (i < 8) cur_vao()->a[i].on = 0; }
static void GL_APIENTRY sw_glVertexAttrib4f(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    (void)i; (void)x; (void)y; (void)z; (void)w;
}

static void fetch(const Vao *v, int ai, GLuint idx, float out[4])
{
    const Attr *a = &v->a[ai];
    const Buf *b = a->buf && a->buf < MAXOBJ ? bufs[a->buf] : 0;
    const uint8_t *p;
    out[0] = out[1] = out[2] = 0.f; out[3] = 1.f;
    if (!a->on || !b) return;
    p = b->data + a->off + (size_t)idx * (size_t)a->stride;
    if (p + (a->type == GL_FLOAT ? 4 : 1) * a->size > b->data + b->size) return;
    for (int k = 0; k < a->size; k++) {
        if (a->type == GL_FLOAT) memcpy(&out[k], p + 4 * k, 4);
        else out[k] = a->norm ? p[k] / 255.f : (float)p[k];
    }
}

/* ---------------- framebuffers ---------------- */
static void GL_APIENTRY sw_glGenFramebuffers(GLsizei n, GLuint *o)
{
    for (int i = 0; i < n; i++) o[i] = new_id((void **)fbos, 64, sizeof(Fbo));
}
static void GL_APIENTRY sw_glBindFramebuffer(GLenum t, GLuint id) { (void)t; G.fbo = id; }
static void GL_APIENTRY sw_glFramebufferTexture2D(GLenum t, GLenum at, GLenum tt, GLuint tex, GLint lv)
{
    (void)t; (void)at; (void)tt; (void)lv;
    if (G.fbo && fbos[G.fbo]) fbos[G.fbo]->color = tex;
}
static void GL_APIENTRY sw_glGenRenderbuffers(GLsizei n, GLuint *o)
{
    for (int i = 0; i < n; i++) o[i] = new_id((void **)rbs, 64, sizeof(Rb));
}
static void GL_APIENTRY sw_glBindRenderbuffer(GLenum t, GLuint id) { (void)t; G.rb = id; }
static void GL_APIENTRY sw_glRenderbufferStorage(GLenum t, GLenum f, GLsizei w, GLsizei h)
{
    Rb *r = G.rb < 64 ? rbs[G.rb] : 0;
    (void)t; (void)f;
    if (!r) return;
    free(r->z);
    r->w = w; r->h = h;
    r->z = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
}
static void GL_APIENTRY sw_glFramebufferRenderbuffer(GLenum t, GLenum at, GLenum rt, GLuint rb)
{
    (void)t; (void)at; (void)rt;
    if (G.fbo && fbos[G.fbo]) fbos[G.fbo]->depth = rb;
}
static GLenum GL_APIENTRY sw_glCheckFramebufferStatus(GLenum t) { (void)t; return GL_FRAMEBUFFER_COMPLETE; }
static void GL_APIENTRY sw_glBlitFramebuffer(GLint a, GLint b, GLint c, GLint d, GLint e, GLint f, GLint g, GLint h,
                                             GLbitfield m, GLenum fl)
{
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)m; (void)fl;
}
static void GL_APIENTRY sw_glReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum ty, void *out)
{
    Target t = target();
    (void)fmt; (void)ty;
    if (!t.c) return;
    if (x >= 0 && y >= 0 && w > 0 && x + w <= t.w && y + h <= t.h) { /* inside the target: whole rows */
        for (int j = 0; j < h; j++)
            memcpy((uint8_t *)out + (size_t)j * w * 4, t.c + ((size_t)(y + j) * t.w + x) * 4, (size_t)w * 4);
        return;
    }
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            uint8_t *o = (uint8_t *)out + ((size_t)j * w + i) * 4;
            if (sx < 0 || sy < 0 || sx >= t.w || sy >= t.h) memset(o, 0, 4);
            else memcpy(o, t.c + ((size_t)sy * t.w + sx) * 4, 4);
        }
}
static void GL_APIENTRY sw_glReadBuffer(GLenum b) { (void)b; }
static void GL_APIENTRY sw_glFinish(void) {}

/* ---------------- texture sampling ---------------- */
static int wrapi(int i, int n, int clamp)
{
    if (clamp) return i < 0 ? 0 : i >= n ? n - 1 : i;
    i %= n;
    return i < 0 ? i + n : i;
}

/* One texel as RGBA8: an RGBA texture (pal == NULL) or an index texture
 * through a palette atlas row (lut() of the shaders). */
/* floor() as an int, without a libm call */
static inline int ifloor(float x)
{
    int i = (int)x;
    return (float)i > x ? i - 1 : i;
}

static const uint8_t *texel8(const Tex *t, const Tex *pal, int row, int cs, int ct, int i, int j)
{
    static const uint8_t zero[4];
    if ((unsigned)i >= (unsigned)t->w) i = wrapi(i, t->w, cs);
    if ((unsigned)j >= (unsigned)t->h) j = wrapi(j, t->h, ct);
    if (!pal) return t->px + ((size_t)j * t->w + i) * 4;
    if ((unsigned)row >= (unsigned)pal->h) return zero;
    return pal->px + ((size_t)row * pal->w + (t->fmt == 1 ? t->px[(size_t)j * t->w + i] : t->px[((size_t)j * t->w + i) * 4])) * 4;
}

/* texture() / the shaders' hand-written index filter: nearest, or bilinear
 * with 8-bit weights. */
static void sample8(const Tex *t, const Tex *pal, int row, int linear, int cs, int ct, float u, float v, uint8_t o[4])
{
    float x = u * (float)t->w, y = v * (float)t->h;
    if (!linear) {
        memcpy(o, texel8(t, pal, row, cs, ct, ifloor(x), ifloor(y)), 4);
    } else {
        int i, j, a, b;
        const uint8_t *c00, *c10, *c01, *c11;
        x -= 0.5f; y -= 0.5f;
        i = ifloor(x); j = ifloor(y);
        a = (int)((x - (float)i) * 256.f); b = (int)((y - (float)j) * 256.f);
        if (i >= 0 && j >= 0 && i + 1 < t->w && j + 1 < t->h && (!pal || ((unsigned)row < (unsigned)pal->h && t->fmt == 1))) {
            /* the four texels are inside the texture: no wrapping */
            const uint8_t *r0 = t->px + ((size_t)j * t->w + i) * (pal ? 1 : 4), *r1 = r0 + (size_t)t->w * (pal ? 1 : 4);
            if (pal) {
                const uint8_t *pr = pal->px + (size_t)row * pal->w * 4;
                c00 = pr + 4 * r0[0]; c10 = pr + 4 * r0[1]; c01 = pr + 4 * r1[0]; c11 = pr + 4 * r1[1];
            } else {
                c00 = r0; c10 = r0 + 4; c01 = r1; c11 = r1 + 4;
            }
        } else {
            c00 = texel8(t, pal, row, cs, ct, i, j); c10 = texel8(t, pal, row, cs, ct, i + 1, j);
            c01 = texel8(t, pal, row, cs, ct, i, j + 1); c11 = texel8(t, pal, row, cs, ct, i + 1, j + 1);
        }
        {
            /* two channels per multiply: x and y lerps on the 0x00FF00FF halves */
            uint32_t p00, p10, p01, p11, t0, t1, b0, b1, lo, hi;
            memcpy(&p00, c00, 4); memcpy(&p10, c10, 4); memcpy(&p01, c01, 4); memcpy(&p11, c11, 4);
            t0 = (((p00 & 0xFF00FFu) * (uint32_t)(256 - a) + (p10 & 0xFF00FFu) * (uint32_t)a) >> 8) & 0xFF00FFu;
            t1 = ((((p00 >> 8) & 0xFF00FFu) * (uint32_t)(256 - a) + ((p10 >> 8) & 0xFF00FFu) * (uint32_t)a) >> 8) & 0xFF00FFu;
            b0 = (((p01 & 0xFF00FFu) * (uint32_t)(256 - a) + (p11 & 0xFF00FFu) * (uint32_t)a) >> 8) & 0xFF00FFu;
            b1 = ((((p01 >> 8) & 0xFF00FFu) * (uint32_t)(256 - a) + ((p11 >> 8) & 0xFF00FFu) * (uint32_t)a) >> 8) & 0xFF00FFu;
            lo = ((t0 * (uint32_t)(256 - b) + b0 * (uint32_t)b + 0x800080u) >> 8) & 0xFF00FFu;
            hi = (t1 * (uint32_t)(256 - b) + b1 * (uint32_t)b + 0x800080u) & 0xFF00FF00u;
            lo |= hi;
            memcpy(o, &lo, 4);
        }
    }
}

/* ---------------- the programs ---------------- */
#define NVAR 9
typedef struct { float pos[4]; float v[NVAR]; float flat; } VOut;

typedef struct {
    const Prog *g;
    const Tex *tex, *pal;
    int kind, mode, linear, clamp, flags, palrow;
    uint8_t fog8[3];
} FragCtx;

/* batch varyings: 0-1 uv, 2-5 colour * w, 6 w.  mesh: 0 adc, 1-2 uv, 3-6 colour * w, 7 fog * w, 8 w */
static void vs_batch(const Prog *g, const Vao *va, GLuint idx, VOut *o)
{
    float p[4], uv[4], c[4], pal[4];
    const float *view = uni(g, "u_view")->f;
    float nx, ny;
    fetch(va, 0, idx, p); fetch(va, 1, idx, uv); fetch(va, 2, idx, c); fetch(va, 3, idx, pal);
    nx = p[0] / view[0] * 2.f - 1.f;
    ny = 1.f - p[1] / view[1] * 2.f;
    o->pos[0] = nx * p[3]; o->pos[1] = ny * p[3]; o->pos[2] = p[2] * p[3]; o->pos[3] = p[3];
    o->v[0] = uv[0]; o->v[1] = uv[1];
    for (int k = 0; k < 4; k++) o->v[2 + k] = c[k] * p[3];
    o->v[6] = p[3];
    o->flat = pal[0];
}

static void mat_mul_vec(const float *m, const float *v, float *o) /* column-major mat4 * vec4 */
{
    for (int r = 0; r < 4; r++) o[r] = m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] * v[3];
}

typedef struct {
    const float *mvp, *vp, *cscale, *mod, *matcol, *uv0, *uv1, *uv3, *fog, *ldir, *lcol, *lamb, *clipm;
    float vpz, lclamp;
    int flags, adc;
} MeshUni;

static void mesh_uniforms(const Prog *g, MeshUni *m)
{
    m->mvp = uni(g, "u_mvp")->f; m->vp = uni(g, "u_vp")->f; m->cscale = uni(g, "u_cscale")->f;
    m->mod = uni(g, "u_mod")->f; m->matcol = uni(g, "u_matcol")->f; m->uv0 = uni(g, "u_uv0")->f;
    m->uv1 = uni(g, "u_uv1")->f; m->uv3 = uni(g, "u_uv3")->f; m->fog = uni(g, "u_fog")->f;
    m->ldir = uni(g, "u_ldir")->f; m->lcol = uni(g, "u_lcol")->f; m->lamb = uni(g, "u_lamb")->f;
    m->clipm = uni(g, "u_clipm")->f; m->vpz = uni(g, "u_vpz")->f[0]; m->lclamp = uni(g, "u_lclamp")->f[0];
    m->flags = uni(g, "u_flags")->i; m->adc = uni(g, "u_adc")->i;
}

static void vs_mesh(const MeshUni *m, const Vao *va, GLuint idx, VOut *o)
{
    float p[4], n[4], ac[4], uv[4], c[4], k[4], col[4], fog;
    fetch(va, 0, idx, p); fetch(va, 1, idx, n); fetch(va, 2, idx, ac); fetch(va, 3, idx, uv);
    p[3] = 1.f;
    mat_mul_vec(m->mvp, p, c);
    mat_mul_vec(m->clipm, p, k);
    o->v[0] = (m->adc && (fabsf(k[0]) > fabsf(k[3]) || fabsf(k[1]) > fabsf(k[3]) || fabsf(k[2]) > fabsf(k[3])))
                  ? 1.f : 0.f;
    o->pos[0] = c[0] * m->vp[0] + c[3] * m->vp[1];
    o->pos[1] = c[1] * m->vp[2] + c[3] * m->vp[3];
    o->pos[2] = c[2] * m->vpz;
    o->pos[3] = c[3];
    if (m->flags & 4) {
        float d[3];
        for (int i = 0; i < 3; i++) {
            d[i] = n[0] * m->ldir[3 * i] + n[1] * m->ldir[3 * i + 1] + n[2] * m->ldir[3 * i + 2];
            if (d[i] < 0.f) d[i] = 0.f;
        }
        for (int ch = 0; ch < 3; ch++) {
            float v = m->lcol[ch] * d[0] + m->lcol[4 + ch] * d[1] + m->lcol[8 + ch] * d[2] + m->lamb[ch];
            col[ch] = (float)ifloor((v < m->lclamp ? v : m->lclamp) * m->mod[ch]);
        }
        col[3] = (float)ifloor(m->lamb[3] * m->mod[3]);
    } else if (m->flags & 2) {
        memcpy(col, m->matcol, sizeof col);
    } else {
        for (int ch = 0; ch < 4; ch++) col[ch] = (float)ifloor(ac[ch] * m->cscale[ch] * m->mod[ch]);
    }
    if (m->flags & 8) {
        o->v[1] = m->uv0[0] * uv[0] + m->uv1[0] * uv[1] + m->uv3[0];
        o->v[2] = m->uv0[1] * uv[0] + m->uv1[1] * uv[1] + m->uv3[1];
    } else {
        o->v[1] = uv[0]; o->v[2] = uv[1];
    }
    for (int ch = 0; ch < 4; ch++) o->v[3 + ch] = col[ch] * c[3];
    if (m->flags & 16) {
        fog = (m->fog[0] - c[3]) * m->fog[1] * 255.f;
        fog = fog < 0.f ? 0.f : fog > 255.f ? 255.f : fog;
    } else {
        fog = 255.f;
    }
    o->v[7] = fog * c[3];
    o->v[8] = c[3];
    o->flat = 0.f;
}

/* ---------------- fragments ---------------- */
static inline uint32_t mul255(uint32_t a, uint32_t b) /* round(a * b / 255) */
{
    uint32_t t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

/* What one draw call samples, resolved once: the texture, how to filter and
 * wrap it, and the palette row base for an index texture. */
typedef struct {
    const Tex *t;
    const Tex *pal;      /* NULL: RGBA texture */
    int row, linear, cs, ct;
    int on;              /* 0: no texture (texel = white) */
    int dead;            /* 1: the texel is always transparent black */
} Samp;

/* The inner loop keeps everything in scalar locals (no arrays, no struct
 * fields): pixel stores are byte stores, which the compiler must assume can
 * change any object in memory, so anything read through a pointer in the loop
 * is read again after every pixel.  Texels are packed R | G << 8 | B << 16 |
 * A << 24. */
static inline uint32_t rd_rgba(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* One texel: an RGBA texture (prow == NULL) or an index texture through its
 * palette atlas row (lut() of the shaders). */
static inline uint32_t texel32(const uint8_t *px, int w, int h, int bpp, const uint8_t *prow, int cs, int ct, int i, int j)
{
    if ((unsigned)i >= (unsigned)w) i = wrapi(i, w, cs);
    if ((unsigned)j >= (unsigned)h) j = wrapi(j, h, ct);
    if (!prow) return rd_rgba(px + ((size_t)j * w + i) * 4);
    return rd_rgba(prow + 4 * (size_t)px[((size_t)j * w + i) * bpp]);
}

/* Texel at 16.16 fixed-point texel coordinates (bilinear: already -0.5). */
static inline uint32_t sample32(const uint8_t *px, int w, int h, int bpp, const uint8_t *prow, int lin, int cs, int ct,
                                int32_t uf, int32_t vf)
{
    const int i = uf >> 16, j = vf >> 16;
    uint32_t a, b, p00, p10, p01, p11, t0, t1, b0, b1;
    if (!lin) return texel32(px, w, h, bpp, prow, cs, ct, i, j);
    a = ((uint32_t)uf >> 8) & 255u; b = ((uint32_t)vf >> 8) & 255u;
    if (i >= 0 && j >= 0 && i + 1 < w && j + 1 < h) { /* the four texels are inside the texture: no wrapping */
        const size_t n = prow ? (size_t)bpp : 4;
        const uint8_t *r0 = px + ((size_t)j * w + i) * n, *r1 = r0 + (size_t)w * n;
        if (prow) {
            p00 = rd_rgba(prow + 4 * (size_t)r0[0]); p10 = rd_rgba(prow + 4 * (size_t)r0[n]);
            p01 = rd_rgba(prow + 4 * (size_t)r1[0]); p11 = rd_rgba(prow + 4 * (size_t)r1[n]);
        } else {
            p00 = rd_rgba(r0); p10 = rd_rgba(r0 + 4); p01 = rd_rgba(r1); p11 = rd_rgba(r1 + 4);
        }
    } else {
        p00 = texel32(px, w, h, bpp, prow, cs, ct, i, j); p10 = texel32(px, w, h, bpp, prow, cs, ct, i + 1, j);
        p01 = texel32(px, w, h, bpp, prow, cs, ct, i, j + 1); p11 = texel32(px, w, h, bpp, prow, cs, ct, i + 1, j + 1);
    }
    /* two channels per multiply: x and y lerps on the 0x00FF00FF halves */
    t0 = (((p00 & 0xFF00FFu) * (256u - a) + (p10 & 0xFF00FFu) * a + 0x800080u) >> 8) & 0xFF00FFu;
    t1 = ((((p00 >> 8) & 0xFF00FFu) * (256u - a) + ((p10 >> 8) & 0xFF00FFu) * a + 0x800080u) >> 8) & 0xFF00FFu;
    b0 = (((p01 & 0xFF00FFu) * (256u - a) + (p11 & 0xFF00FFu) * a + 0x800080u) >> 8) & 0xFF00FFu;
    b1 = ((((p01 >> 8) & 0xFF00FFu) * (256u - a) + ((p11 >> 8) & 0xFF00FFu) * a + 0x800080u) >> 8) & 0xFF00FFu;
    return (((t0 * (256u - b) + b0 * b + 0x800080u) >> 8) & 0xFF00FFu) |
           ((t1 * (256u - b) + b1 * b + 0x800080u) & 0xFF00FF00u);
}

static void samp_setup(const FragCtx *c, int row, Samp *sp)
{
    memset(sp, 0, sizeof *sp);
    if (c->kind == PK_BATCH) {
        if (c->mode == 1 && c->tex) {
            sp->on = 1; sp->t = c->tex; sp->linear = c->tex->linear; sp->cs = c->tex->clamp_s; sp->ct = c->tex->clamp_t;
        } else if (c->mode == 2) {
            if (!c->tex) return; /* white, as the shader's default t */
            sp->on = 1; sp->t = c->tex; sp->pal = c->pal; sp->row = row; sp->linear = c->linear;
            sp->cs = sp->ct = c->clamp;
            if (!c->pal || (unsigned)row >= (unsigned)c->pal->h) sp->dead = 1;
        }
    } else if (c->flags & 32) {
        sp->on = 1; sp->t = c->tex;
        if (!c->tex) { sp->dead = 1; return; }
        if (c->mode == 2) {
            sp->pal = c->pal; sp->row = c->palrow; sp->linear = c->linear; sp->cs = sp->ct = c->clamp;
            if (!c->pal || (unsigned)c->palrow >= (unsigned)c->pal->h) sp->dead = 1;
        } else {
            sp->linear = c->tex->linear; sp->cs = c->tex->clamp_s; sp->ct = c->tex->clamp_t;
        }
    }
    if (sp->t && (sp->t->w <= 0 || sp->t->h <= 0 || !sp->t->px)) sp->dead = 1;
}

/* ---------------- output merger ---------------- */
enum { BL_OFF, BL_ALPHA, BL_ADD, BL_GEN };

/* Colours reach the blend with 8 fractional bits (0..ONE16), as GL blends the
 * unrounded shader output; only the stored pixel is rounded to 8 bits. */
#define ONE16 65280u /* 255 << 8 */

static inline uint32_t fac16(GLenum f, uint32_t sa, uint32_t da)
{
    switch (f) {
    case GL_ONE: return ONE16;
    case GL_SRC_ALPHA: return sa;
    case GL_ONE_MINUS_SRC_ALPHA: return ONE16 - sa;
    case GL_DST_ALPHA: return da;
    case GL_ONE_MINUS_DST_ALPHA: return ONE16 - da;
    default: return 0;
    }
}

static inline uint8_t round16(int32_t v) /* 8.8 -> 8 bits, clamped */
{
    return v <= 0 ? 0 : v >= (int32_t)ONE16 ? 255 : (uint8_t)((v + 128) >> 8);
}

static inline uint8_t round16u(int32_t v) /* the same for a value known to be in 0..ONE16 */
{
    return (uint8_t)((uint32_t)(v + 128) >> 8);
}

/* 8.8 (0..ONE16) -> a 15-bit weight (0..32768), so weight * 8.8 fits in 32 bits */
static inline int32_t weight15(uint32_t v) { return (int32_t)((v * 32897u) >> 16); }

static inline void write_pixel(uint8_t *p, uint32_t s0, uint32_t s1, uint32_t s2, uint32_t s3, int bl, int allmask)
{
    if (allmask) {
        if (bl == BL_OFF) {
            p[0] = round16u((int32_t)s0); p[1] = round16u((int32_t)s1);
            p[2] = round16u((int32_t)s2); p[3] = round16u((int32_t)s3);
            return;
        }
        if (bl == BL_ALPHA) { /* s * sa + d * (1 - sa) = d + (s - d) * sa: between d and s */
            const int32_t a = weight15(s3);
            const int32_t d0 = (int32_t)p[0] << 8, d1 = (int32_t)p[1] << 8, d2 = (int32_t)p[2] << 8, d3 = (int32_t)p[3] << 8;
            p[0] = round16u(d0 + ((((int32_t)s0 - d0) * a + 16384) >> 15));
            p[1] = round16u(d1 + ((((int32_t)s1 - d1) * a + 16384) >> 15));
            p[2] = round16u(d2 + ((((int32_t)s2 - d2) * a + 16384) >> 15));
            p[3] = round16u(d3 + ((((int32_t)s3 - d3) * a + 16384) >> 15));
            return;
        }
        if (bl == BL_ADD) {
            const int32_t d0 = (int32_t)p[0] << 8, d1 = (int32_t)p[1] << 8, d2 = (int32_t)p[2] << 8, d3 = (int32_t)p[3] << 8;
            p[0] = round16((int32_t)s0 + d0); p[1] = round16((int32_t)s1 + d1);
            p[2] = round16((int32_t)s2 + d2); p[3] = round16((int32_t)s3 + d3);
            return;
        }
    }
    {
        const uint32_t s[4] = { s0, s1, s2, s3 }, da = (uint32_t)p[3] << 8;
        const int32_t sf = weight15(bl == BL_OFF ? ONE16 : fac16(G.bl_s, s3, da));
        const int32_t df = weight15(bl == BL_OFF ? 0 : fac16(G.bl_d, s3, da));
        for (int k = 0; k < 4; k++) {
            const int32_t x = ((int32_t)s[k] * sf + 16384) >> 15, y = (((int32_t)p[k] << 8) * df + 16384) >> 15;
            const int32_t r = G.bl_eq == GL_FUNC_SUBTRACT ? x - y : G.bl_eq == GL_FUNC_REVERSE_SUBTRACT ? y - x : x + y;
            if (G.cmask[k]) p[k] = round16(r);
        }
    }
}

static inline int ztest(GLenum f, uint32_t z, uint32_t old)
{
    switch (f) {
    case GL_NEVER: return 0;
    case GL_LESS: return z < old;
    case GL_EQUAL: return z == old;
    case GL_LEQUAL: return z <= old;
    case GL_GREATER: return z > old;
    case GL_NOTEQUAL: return z != old;
    case GL_GEQUAL: return z >= old;
    default: return 1;
    }
}

/* ---------------- triangles ---------------- */
typedef struct { double x, y; float z, iw; float v[NVAR]; } SV; /* v = varying / w */

static inline int64_t floor64(double d)
{
    if (d > -2147483000.0 && d < 2147483000.0) { /* the usual case, in 32 bits */
        const int32_t i = (int32_t)d;
        return (double)i > d ? i - 1 : i;
    } else {
        const int64_t i = (int64_t)d;
        return (double)i > d ? i - 1 : i;
    }
}

/* Texel coordinate -> 16.16.  A repeating coordinate is reduced into the
 * texture first and a clamped one limited to just outside it, so the fixed
 * point value cannot overflow; `shift` carries the same move to the other
 * end of the chunk. */
#define SPAN 16
/* A pixel centre exactly on a texel boundary takes the texel on the higher
 * side.  Scaled sprites (16 texels over 26 pixels) hit that case every 13
 * pixels, and float rounding alone would decide it differently per device. */
#define TEXEL_TIE 64 /* 1/1024 texel */

/* Varyings divided by w are linear in screen space.  colour * w and fog * w
 * divided by w are the colour and the fog themselves, so only the texture
 * coordinates need a divide; that is done at the ends of SPAN-pixel pieces
 * (once per row when w is the same at the three vertices, as it is for every
 * 2D draw) and stepped in fixed point between them. */
static void raster(const Target *t, const FragCtx *fc, const SV *a, const SV *b, const SV *c, float flat)
{
    /* 8 sub-pixel bits; edge functions in 64-bit integers */
    int64_t x0 = floor64(a->x * 256.0 + 0.5), y0 = floor64(a->y * 256.0 + 0.5);
    int64_t x1 = floor64(b->x * 256.0 + 0.5), y1 = floor64(b->y * 256.0 + 0.5);
    int64_t x2 = floor64(c->x * 256.0 + 0.5), y2 = floor64(c->y * 256.0 + 0.5);
    int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    int bx0, by0, bx1, by1, minx, miny, maxx, maxy;
    int64_t A[3], B[3], C[3];
    int tie[3];
    double inv;
    const SV *s0 = a, *s1 = b, *s2 = c;
    const int batch = fc->kind == PK_BATCH, nvar = batch ? 6 : 8;
    const int ui = batch ? 0 : 1, ci = batch ? 2 : 3;
    const int do_z = t->z && G.depth, zwrite = do_z && G.zmask;
    const GLenum zf = G.zfunc;
    const int zfast = zf == GL_GEQUAL || zf == GL_GREATER, zgt = zf == GL_GREATER;
    const int allmask = G.cmask[0] & G.cmask[1] & G.cmask[2] & G.cmask[3];
    const int bl = !G.blend ? BL_OFF
                 : G.bl_eq != GL_FUNC_ADD ? BL_GEN
                 : G.bl_s == GL_SRC_ALPHA && G.bl_d == GL_ONE_MINUS_SRC_ALPHA ? BL_ALPHA
                 : G.bl_s == GL_ONE && G.bl_d == GL_ONE ? BL_ADD
                 : G.bl_s == GL_ONE && G.bl_d == GL_ZERO ? BL_OFF : BL_GEN;
    const int fogged = !batch && (fc->flags & 16);
    float d1[NVAR], d2[NVAR], step[NVAR], dz1, dz2, zstep, diw1, diw2, iwstep, tw = 1.f, th = 1.f, half = 0.f;
    int affine, constcol, white = 0, flat_uv = 0, lin_uv;
    float base[NVAR];
    uint32_t cc16[4] = { ONE16, ONE16, ONE16, ONE16 }; /* vertex colour, 8.8 */
    uint32_t q0, q1, q2, q3;
    const int32_t fog0 = (int32_t)fc->fog8[0] << 8, fog1 = (int32_t)fc->fog8[1] << 8, fog2 = (int32_t)fc->fog8[2] << 8;
    const uint8_t *tpx = 0, *prow = 0;
    int tex_on, lin, tcs, tct, txw = 1, txh = 1, tbpp = 4;
    Samp sp;
    if (area == 0) return;
    if (do_z && zf == GL_NEVER) return;
    if (!batch && (a->v[0] > 0.f || b->v[0] > 0.f || c->v[0] > 0.f)) return; /* ADC: the GS skips this triangle */
    if (area < 0) { /* make it counter-clockwise in the y-up frame */
        int64_t tx = x1, ty = y1;
        x1 = x2; y1 = y2; x2 = tx; y2 = ty;
        s1 = c; s2 = b;
        area = -area;
    }
    bounds(t, &bx0, &by0, &bx1, &by1);
    {
        int64_t mnx = x0 < x1 ? x0 : x1, mxx = x0 > x1 ? x0 : x1, mny = y0 < y1 ? y0 : y1, mxy = y0 > y1 ? y0 : y1;
        if (x2 < mnx) mnx = x2;
        if (x2 > mxx) mxx = x2;
        if (y2 < mny) mny = y2;
        if (y2 > mxy) mxy = y2;
        if (mxx < (int64_t)bx0 * 256 || mxy < (int64_t)by0 * 256 || mnx > (int64_t)bx1 * 256 || mny > (int64_t)by1 * 256)
            return;
        minx = mnx < (int64_t)bx0 * 256 ? bx0 : (int)(mnx >> 8);
        miny = mny < (int64_t)by0 * 256 ? by0 : (int)(mny >> 8);
        maxx = mxx > (int64_t)bx1 * 256 ? bx1 - 1 : (int)(mxx >> 8);
        maxy = mxy > (int64_t)by1 * 256 ? by1 - 1 : (int)(mxy >> 8);
        if (maxx >= bx1) maxx = bx1 - 1;
        if (maxy >= by1) maxy = by1 - 1;
        if (minx > maxx || miny > maxy) return;
    }
    samp_setup(fc, (int)flat, &sp);
    if (sp.dead && batch) return;            /* alpha 0 everywhere: nothing passes the alpha test */
    if (sp.dead && !batch) return;
    if (sp.on) { tw = (float)sp.t->w; th = (float)sp.t->h; half = sp.linear ? 0.5f : 0.f; }
    /* edge i is opposite vertex i: E(x, y) = A x + B y + C, > 0 inside */
    A[0] = y1 - y2; B[0] = x2 - x1; C[0] = x1 * y2 - x2 * y1;
    A[1] = y2 - y0; B[1] = x0 - x2; C[1] = x2 * y0 - x0 * y2;
    A[2] = y0 - y1; B[2] = x1 - x0; C[2] = x0 * y1 - x1 * y0;
    /* top-left rule (y up): a zero edge counts when it is a left edge or a top edge */
    for (int k = 0; k < 3; k++) tie[k] = A[k] > 0 || (A[k] == 0 && B[k] < 0);
    inv = 1.0 / (double)area;
    /* X(px, py) = X0 + w1 (X1 - X0) + w2 (X2 - X0); one pixel to the right adds `step` */
#define GRAD(D1, D2) (float)(256.0 * inv * ((double)A[1] * (double)(D1) + (double)A[2] * (double)(D2)))
    for (int k = 0; k < nvar; k++) {
        d1[k] = s1->v[k] - s0->v[k];
        d2[k] = s2->v[k] - s0->v[k];
        step[k] = GRAD(d1[k], d2[k]);
    }
    dz1 = s1->z - s0->z; dz2 = s2->z - s0->z; zstep = GRAD(dz1, dz2);
    diw1 = s1->iw - s0->iw; diw2 = s2->iw - s0->iw; iwstep = GRAD(diw1, diw2);
#undef GRAD
    affine = diw1 == 0.f && diw2 == 0.f;
    for (int k = 0; k < nvar; k++) base[k] = s0->v[k];
    /* A small triangle far from the eye: if the exact texture coordinate at the
     * middle of each edge and at the centre is within 1/64 texel of the straight
     * line between the vertices, step u and v linearly over the whole triangle
     * (no divide per row).  Most stage triangles are like this. */
    if (!affine && sp.on) {
        const float w[3] = { 1.f / s0->iw, 1.f / s1->iw, 1.f / s2->iw };
        const SV *sv[3] = { s0, s1, s2 };
        float uv[3][2], worst = 0.f;
        for (int i = 0; i < 3; i++) { uv[i][0] = sv[i]->v[ui] * w[i]; uv[i][1] = sv[i]->v[ui + 1] * w[i]; }
        for (int c = 0; c < 2; c++) {
            const float scale = c ? th : tw;
            for (int i = 0; i < 3; i++) {
                const int j = (i + 1) % 3;
                const float exact = (sv[i]->v[ui + c] + sv[j]->v[ui + c]) / (sv[i]->iw + sv[j]->iw);
                const float e = (exact - (uv[i][c] + uv[j][c]) * 0.5f) * scale;
                if (e > worst) worst = e;
                if (-e > worst) worst = -e;
            }
            {
                const float exact = (s0->v[ui + c] + s1->v[ui + c] + s2->v[ui + c]) / (s0->iw + s1->iw + s2->iw);
                const float e = (exact - (uv[0][c] + uv[1][c] + uv[2][c]) * (1.f / 3.f)) * scale;
                if (e > worst) worst = e;
                if (-e > worst) worst = -e;
            }
        }
        if (worst < 1.f / 64.f) {
            flat_uv = 1;
            for (int c = 0; c < 2; c++) {
                base[ui + c] = uv[0][c];
                d1[ui + c] = uv[1][c] - uv[0][c];
                d2[ui + c] = uv[2][c] - uv[0][c];
                step[ui + c] = (float)(256.0 * inv * ((double)A[1] * (double)d1[ui + c] + (double)A[2] * (double)d2[ui + c]));
            }
        }
    }
    lin_uv = affine || flat_uv;
    constcol = batch && d1[2] == 0.f && d2[2] == 0.f && d1[3] == 0.f && d2[3] == 0.f && d1[4] == 0.f &&
               d2[4] == 0.f && d1[5] == 0.f && d2[5] == 0.f;
    if (constcol) {
        for (int k = 0; k < 4; k++) {
            const float v = s0->v[2 + k] * (float)ONE16 + 0.5f;
            cc16[k] = v <= 0.f ? 0 : v >= (float)ONE16 ? ONE16 : (uint32_t)v;
        }
        white = cc16[0] == ONE16 && cc16[1] == ONE16 && cc16[2] == ONE16 && cc16[3] == ONE16;
        if (cc16[3] < 128 / 255 + 1) return; /* alpha 0: nothing passes the alpha test */
    }

    q0 = cc16[0]; q1 = cc16[1]; q2 = cc16[2]; q3 = cc16[3];
    tex_on = sp.on; lin = sp.linear; tcs = sp.cs; tct = sp.ct;
    if (sp.on) {
        tpx = sp.t->px; txw = sp.t->w; txh = sp.t->h; tbpp = sp.t->fmt;
        prow = sp.pal ? sp.pal->px + (size_t)sp.row * sp.pal->w * 4 : 0;
    }

    for (int py = miny; py <= maxy; py++) {
        const int64_t sy = (int64_t)py * 256 + 128, sx = (int64_t)minx * 256 + 128;
        int xs = minx, xe = maxx, empty = 0;
        uint8_t *row;
        uint32_t *zrow;
        double w1, w2;
        float z, iw, var[NVAR];
        int64_t zfix, zfstep;
        int zsafe;
        int32_t c0, c1, c2, c3, c4, e0, e1, e2, e3, e4; /* colour and fog and their steps, 16.16 */
        int32_t cfix[5] = { 0, 0, 0, 0, 0 }, cstep[5] = { 0, 0, 0, 0, 0 }; /* colour (and fog), 16.16 */
        int px;
        /* the pixels of this row inside the three edges */
        for (int k = 0; k < 3; k++) {
            const int64_t e = A[k] * sx + B[k] * sy + C[k], st = A[k] * 256;
            if (st > 0) {           /* a left edge: inside from some x on */
                int64_t n = e >= 0 ? 0 : (-e + st - 1) / st;        /* first n with e + n st >= 0 */
                if (!tie[k] && e + n * st == 0) n++;
                if (n > maxx - minx) { empty = 1; break; }
                if (minx + (int)n > xs) xs = minx + (int)n;
            } else if (st < 0) {    /* a right edge: inside up to some x */
                int64_t n;
                if (e < 0) { empty = 1; break; }
                n = e / -st;                                         /* last n with e + n st >= 0 */
                if (!tie[k] && e + n * st == 0) n--;
                if (n < 0) { empty = 1; break; }
                if (minx + n < xe) xe = (int)(minx + n);
            } else if (e < 0 || (e == 0 && !tie[k])) {
                empty = 1; break;
            }
        }
        if (empty || xs > xe) continue;
        {
            const int64_t sxs = (int64_t)xs * 256 + 128;
            w1 = (double)(A[1] * sxs + B[1] * sy + C[1]) * inv;
            w2 = (double)(A[2] * sxs + B[2] * sy + C[2]) * inv;
        }
        z = s0->z + (float)(w1 * (double)dz1 + w2 * (double)dz2);
        iw = s0->iw + (float)(w1 * (double)diw1 + w2 * (double)diw2);
        for (int k = 0; k < nvar; k++) var[k] = base[k] + (float)(w1 * (double)d1[k] + w2 * (double)d2[k]);
        /* depth: 24.32 fixed point, so the first pixel is depth24(z) exactly */
        {
            double zq = (double)(z <= 0.f ? 0.f : z >= 1.f ? 1.f : z) * 16777215.0 + 0.5;
            const double zend = zq + (double)zstep * 16777215.0 * (double)(xe - xs);
            zfix = (int64_t)(zq * 4294967296.0);
            zfstep = (int64_t)((double)zstep * 16777215.0 * 4294967296.0);
            /* the whole row inside the depth range: no clamp per pixel */
            zsafe = zq >= 1.0 && zq < 16777215.0 && zend >= 1.0 && zend < 16777215.0;
        }
        if (batch) {
            if (!constcol)
                for (int k = 0; k < 4; k++) {
                    cfix[k] = (int32_t)(var[2 + k] * (255.f * 65536.f));
                    cstep[k] = (int32_t)(step[2 + k] * (255.f * 65536.f));
                }
        } else {
            for (int k = 0; k < 4; k++) {
                cfix[k] = (int32_t)(var[ci + k] * 65536.f);
                cstep[k] = (int32_t)(step[ci + k] * 65536.f);
            }
            cfix[4] = (int32_t)(var[7] * 65536.f);
            cstep[4] = (int32_t)(step[7] * 65536.f);
        }
        row = t->c + ((size_t)py * t->w + xs) * 4;
        zrow = do_z ? t->z + (size_t)py * t->w + xs : 0;
        px = xs;
        c0 = cfix[0]; c1 = cfix[1]; c2 = cfix[2]; c3 = cfix[3]; c4 = cfix[4];
        e0 = cstep[0]; e1 = cstep[1]; e2 = cstep[2]; e3 = cstep[3]; e4 = cstep[4];
        while (px <= xe) {
            /* one piece: texture coordinates exact at both ends, linear between */
            int len = lin_uv || !sp.on ? xe - px + 1 : (xe - px + 1 < SPAN ? xe - px + 1 : SPAN);
            int32_t uf = 0, vf = 0, duf = 0, dvf = 0;
            /* shorten the piece until stepping linearly stays within 1/32 texel of
             * the exact coordinate at its middle */
            while (sp.on && !lin_uv && len > 1) {
                const float h = (float)(len / 2), n = (float)len;
                const float w0 = 1.f / iw, wm = 1.f / (iw + iwstep * h), wn = 1.f / (iw + iwstep * n);
                const float u0 = var[ui] * w0, un = (var[ui] + step[ui] * n) * wn, um = (var[ui] + step[ui] * h) * wm;
                const float v0 = var[ui + 1] * w0, vn = (var[ui + 1] + step[ui + 1] * n) * wn,
                            vm = (var[ui + 1] + step[ui + 1] * h) * wm;
                const float eu = (u0 + (un - u0) * (h / n) - um) * tw, ev = (v0 + (vn - v0) * (h / n) - vm) * th;
                if (eu < 1.f / 32.f && eu > -1.f / 32.f && ev < 1.f / 32.f && ev > -1.f / 32.f) break;
                len /= 2;
            }
            if (sp.on) {
                const float wa = flat_uv ? 1.f : 1.f / iw, iwb = iw + iwstep * (float)len, wb = lin_uv ? wa : 1.f / iwb;
                double ua = (double)(var[ui] * wa * tw - half), va = (double)(var[ui + 1] * wa * th - half);
                double ub = (double)((var[ui] + step[ui] * (float)len) * wb * tw - half);
                double vb = (double)((var[ui + 1] + step[ui + 1] * (float)len) * wb * th - half);
                double su, sv;
                if (sp.cs) { ua = ua < -4.0 ? -4.0 : ua > tw + 4.0 ? tw + 4.0 : ua; ub = ub < -4.0 ? -4.0 : ub > tw + 4.0 ? tw + 4.0 : ub; }
                else { su = (double)floor64(ua / tw) * tw; ua -= su; ub -= su; }
                if (sp.ct) { va = va < -4.0 ? -4.0 : va > th + 4.0 ? th + 4.0 : va; vb = vb < -4.0 ? -4.0 : vb > th + 4.0 ? th + 4.0 : vb; }
                else { sv = (double)floor64(va / th) * th; va -= sv; vb -= sv; }
                if (ub - ua > 16000.0 || ua - ub > 16000.0) ub = ua;
                if (vb - va > 16000.0 || va - vb > 16000.0) vb = va;
                uf = (int32_t)floor64(ua * 65536.0) + TEXEL_TIE; vf = (int32_t)floor64(va * 65536.0) + TEXEL_TIE;
                duf = (int32_t)(floor64(ub * 65536.0) - uf) / len;
                dvf = (int32_t)(floor64(vb * 65536.0) - vf) / len;
            }
            for (int i = 0; i < len; i++, row += 4, uf += duf, vf += dvf, zfix += zfstep, c0 += e0, c1 += e1,
                     c2 += e2, c3 += e3, c4 += e4) {
                uint32_t zi = 0, tx, tr, tg, tb, ta, o0, o1, o2, o3;
                if (do_z) {
                    if (zsafe) {
                        zi = (uint32_t)((uint64_t)zfix >> 32);
                    } else {
                        zi = zfix <= 0 ? 0 : (uint32_t)(zfix >> 32);
                        if (zi > 0xFFFFFFu) zi = 0xFFFFFFu;
                    }
                    /* GEQUAL and GREATER (the game's two) as one signed compare */
                    if (zfast ? (int32_t)zi - zgt < (int32_t)zrow[i] : !ztest(zf, zi, zrow[i])) continue;
                }
                tx = tex_on ? sample32(tpx, txw, txh, tbpp, prow, lin, tcs, tct, uf, vf) : 0xFFFFFFFFu;
                tr = tx & 255u; tg = (tx >> 8) & 255u; tb = (tx >> 16) & 255u; ta = tx >> 24;
                if (batch) {
                    if (white) {
                        if (!ta) continue; /* GS TEST: alpha != 0 */
                        o0 = tr << 8; o1 = tg << 8; o2 = tb << 8; o3 = ta << 8;
                    } else {
                        if (!constcol) {
                            const int32_t v0 = c0 >> 8, v1 = c1 >> 8, v2 = c2 >> 8, v3 = c3 >> 8;
                            q0 = (uint32_t)(v0 < 0 ? 0 : v0 > (int32_t)ONE16 ? (int32_t)ONE16 : v0);
                            q1 = (uint32_t)(v1 < 0 ? 0 : v1 > (int32_t)ONE16 ? (int32_t)ONE16 : v1);
                            q2 = (uint32_t)(v2 < 0 ? 0 : v2 > (int32_t)ONE16 ? (int32_t)ONE16 : v2);
                            q3 = (uint32_t)(v3 < 0 ? 0 : v3 > (int32_t)ONE16 ? (int32_t)ONE16 : v3);
                        }
                        o3 = (ta * q3 + 127) / 255;
                        if (o3 < 128) continue; /* below 0.5 / 255 */
                        o0 = (tr * q0 + 127) / 255;
                        o1 = (tg * q1 + 127) / 255;
                        o2 = (tb * q2 + 127) / 255;
                    }
                } else {
                    /* k_mfs: colour in byte units, 128 = 1.0 with a texture */
                    int32_t m0, m1, m2, m3;
                    if (tex_on) {
                        const int32_t ca = c3 >> 12; /* colour * 16 */
                        if ((int32_t)ta * ca < 255 * 16) continue;
                        m3 = ((int32_t)ta * ca) >> 3;     /* t * colour / 128, 8.8 */
                        m0 = ((int32_t)tr * (c0 >> 12)) >> 3;
                        m1 = ((int32_t)tg * (c1 >> 12)) >> 3;
                        m2 = ((int32_t)tb * (c2 >> 12)) >> 3;
                    } else {
                        if (c3 < 65536) continue;
                        m0 = c0 >> 8; m1 = c1 >> 8; m2 = c2 >> 8;
                        m3 = (int32_t)(((int64_t)c3 * 255 / 128) >> 8);
                    }
                    m0 = m0 < 0 ? 0 : m0 > (int32_t)ONE16 ? (int32_t)ONE16 : m0;
                    m1 = m1 < 0 ? 0 : m1 > (int32_t)ONE16 ? (int32_t)ONE16 : m1;
                    m2 = m2 < 0 ? 0 : m2 > (int32_t)ONE16 ? (int32_t)ONE16 : m2;
                    m3 = m3 < 0 ? 0 : m3 > (int32_t)ONE16 ? (int32_t)ONE16 : m3;
                    if (fogged && c4 < (int32_t)(255u << 16)) { /* 255 = no fog at this pixel */
                        const int32_t f = weight15((uint32_t)(c4 < 0 ? 0 : c4 >> 8));
                        m0 = fog0 + (((m0 - fog0) * f + 16384) >> 15); /* fog + (colour - fog) * f */
                        m1 = fog1 + (((m1 - fog1) * f + 16384) >> 15);
                        m2 = fog2 + (((m2 - fog2) * f + 16384) >> 15);
                    }
                    o0 = (uint32_t)m0; o1 = (uint32_t)m1; o2 = (uint32_t)m2; o3 = (uint32_t)m3;
                }
                if (zwrite) zrow[i] = zi;
                write_pixel(row, o0, o1, o2, o3, bl, allmask);
            }
            if (zrow) zrow += len;
            px += len;
            iw += iwstep * (float)len;
            var[ui] += step[ui] * (float)len;
            var[ui + 1] += step[ui + 1] * (float)len;
        }
    }
}

static void lerp_vout(const VOut *a, const VOut *b, float t, VOut *o)
{
    for (int k = 0; k < 4; k++) o->pos[k] = a->pos[k] + (b->pos[k] - a->pos[k]) * t;
    for (int k = 0; k < NVAR; k++) o->v[k] = a->v[k] + (b->v[k] - a->v[k]) * t;
    o->flat = a->flat;
}

/* Clip a polygon against s * z + w >= 0 (s = 1: near, s = -1: far). */
static int clip_plane(const VOut *in, int n, float s, VOut *out)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const VOut *a = &in[i], *b = &in[(i + 1) % n];
        float da = s * a->pos[2] + a->pos[3], db = s * b->pos[2] + b->pos[3];
        if (da >= 0.f) out[m++] = *a;
        if ((da >= 0.f) != (db >= 0.f)) lerp_vout(a, b, da / (da - db), &out[m++]);
    }
    return m;
}

static void triangle(const Target *t, const FragCtx *fc, const VOut *v0, const VOut *v1, const VOut *v2)
{
    VOut p[8], q[8];
    SV s[8];
    int n = 3;
    float flat = v2->flat; /* the provoking vertex is the last one */
    p[0] = *v0; p[1] = *v1; p[2] = *v2;
    n = clip_plane(p, n, 1.f, q);
    if (n < 3) return;
    n = clip_plane(q, n, -1.f, p);
    if (n < 3) return;
    for (int i = 0; i < n; i++) {
        float w = p[i].pos[3], iw;
        if (!(w > 0.f)) return;
        iw = 1.f / w;
        s[i].x = ((double)(p[i].pos[0] * iw) * 0.5 + 0.5) * G.vp[2] + G.vp[0];
        s[i].y = ((double)(p[i].pos[1] * iw) * 0.5 + 0.5) * G.vp[3] + G.vp[1];
        s[i].z = p[i].pos[2] * iw * 0.5f + 0.5f;
        s[i].iw = iw;
        for (int k = 0; k < NVAR; k++) s[i].v[k] = p[i].v[k] * iw;
        if (!(fabs(s[i].x) < 1e7 && fabs(s[i].y) < 1e7)) return;
    }
    for (int i = 1; i + 1 < n; i++) raster(t, fc, &s[0], &s[i], &s[i + 1], flat);
}

/* ---------------- present (k_pp_fs): the canvas onto the window ---------------- */
static void present(const Prog *g, const Target *t)
{
    const Tex *src = bound_tex(0);
    const float *rect = uni(g, "u_src_rect")->f, *tsz = uni(g, "u_tex_size")->f;
    int smooth = uni(g, "u_filter")->i == 2;
    if (!src || src->fmt != 4 || !t->c) return;
    for (int y = G.vp[1]; y < G.vp[1] + G.vp[3]; y++) {
        if (y < 0 || y >= t->h) continue;
        for (int x = G.vp[0]; x < G.vp[0] + G.vp[2]; x++) {
            float u, v;
            uint8_t *o, c[4];
            if (x < 0 || x >= t->w) continue;
            u = (rect[0] + ((float)(x - G.vp[0]) + 0.5f) / (float)G.vp[2] * rect[2]) / tsz[0];
            v = (rect[1] + ((float)(y - G.vp[1]) + 0.5f) / (float)G.vp[3] * rect[3]) / tsz[1];
            sample8(src, 0, 0, smooth, 1, 1, u, v, c);
            o = t->c + ((size_t)y * t->w + x) * 4;
            o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = 255;
        }
    }
}

/* ---------------- draw calls ---------------- */
static void draw(GLenum mode, GLsizei count, const GLuint *idx, GLint first)
{
    const Prog *g = G.prog < 64 ? progs[G.prog] : 0;
    const Vao *va = cur_vao();
    Target t = target();
    FragCtx fc;
    MeshUni mu;
    VOut *vo;
    GLuint lo = 0xFFFFFFFFu, hi = 0;
    double t0 = 0.0;
    if (!g || !t.c || count <= 0) return;
    if (St.on) {
        if (St.frame <= St.from) return;
        t0 = now_sec();
    }
    if (g->kind == PK_PRESENT) { present(g, &t); return; }
    if (mode != GL_TRIANGLES) return;
    memset(&fc, 0, sizeof fc);
    fc.g = g; fc.kind = g->kind;
    fc.tex = bound_tex(0); fc.pal = bound_tex(1);
    fc.mode = uni(g, "u_mode")->i; fc.linear = uni(g, "u_linear")->i; fc.clamp = uni(g, "u_clamp")->i;
    if (g->kind == PK_MESH) {
        mesh_uniforms(g, &mu);
        fc.flags = mu.flags;
        fc.palrow = (int)uni(g, "u_palrow")->f[0];
        for (int k = 0; k < 3; k++) fc.fog8[k] = unorm8(uni(g, "u_fogcol")->f[k]);
    }
    /* shade each vertex of the index range once */
    for (GLsizei i = 0; i < count; i++) {
        GLuint k = idx ? idx[i] : (GLuint)(first + i);
        if (k < lo) lo = k;
        if (k > hi) hi = k;
    }
    vo = (VOut *)malloc(sizeof(VOut) * (size_t)(hi - lo + 1));
    if (!vo) return;
    for (GLuint k = lo; k <= hi; k++) {
        if (g->kind == PK_BATCH) vs_batch(g, va, k, &vo[k - lo]);
        else vs_mesh(&mu, va, k, &vo[k - lo]);
    }
    if (St.on) { St.vsec += now_sec() - t0; St.tris += count / 3; St.verts += hi - lo + 1; }
    for (GLsizei i = 0; i + 2 < count; i += 3) {
        GLuint a = idx ? idx[i] : (GLuint)(first + i), b = idx ? idx[i + 1] : (GLuint)(first + i + 1),
               c = idx ? idx[i + 2] : (GLuint)(first + i + 2);
        triangle(&t, &fc, &vo[a - lo], &vo[b - lo], &vo[c - lo]);
    }
    free(vo);
    if (St.on) St.sec += now_sec() - t0;
}

static void GL_APIENTRY sw_glDrawArrays(GLenum mode, GLint first, GLsizei count) { draw(mode, count, 0, first); }
static void GL_APIENTRY sw_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *off)
{
    const Vao *va = cur_vao();
    const Buf *e = va->ebo && va->ebo < MAXOBJ ? bufs[va->ebo] : 0;
    size_t o = (size_t)(uintptr_t)off, isz = type == GL_UNSIGNED_INT ? 4 : 2;
    GLuint *idx;
    if (!e || count <= 0 || o + (size_t)count * isz > e->size) return;
    idx = (GLuint *)malloc(sizeof(GLuint) * (size_t)count);
    if (!idx) return;
    for (GLsizei i = 0; i < count; i++) {
        if (isz == 4) memcpy(&idx[i], e->data + o + 4 * (size_t)i, 4);
        else { uint16_t s; memcpy(&s, e->data + o + 2 * (size_t)i, 2); idx[i] = s; }
    }
    draw(mode, count, idx, 0);
    free(idx);
}

/* ---------------- install ---------------- */
static int installed;
int swgl_active(void) { return installed; }

void swgl_install(void)
{
    memset(&G, 0, sizeof G);
    G.bl_eq = GL_FUNC_ADD; G.bl_s = GL_ONE; G.bl_d = GL_ZERO; G.zfunc = GL_LESS; G.zmask = 1;
    G.cmask[0] = G.cmask[1] = G.cmask[2] = G.cmask[3] = 1;
    G.clear_z = 1.f;
#define SW_SET(type, name) p_##name = sw_##name;
    PORT_GL_FUNCS(SW_SET)
#undef SW_SET
    installed = 1;
    if (getenv("MVC2_SWGL_STATS")) {
        St.on = 1;
        St.from = strtol(getenv("MVC2_SWGL_STATS"), 0, 0);
        if (getenv("MVC2_SWGL_STOP")) St.stop = strtol(getenv("MVC2_SWGL_STOP"), 0, 0); /* quit after that many timed frames */
        atexit(stats_report);
    }
}

#ifdef PORT_SWGL
/* No platform/gl_load.c in this build: the entry points live here. */
#define PORT_GL_DEF(type, name) type p_##name;
PORT_GL_FUNCS(PORT_GL_DEF)
#undef PORT_GL_DEF
void gl_clear_depth(float d) { swgl_clear_depth(d); }
int gl_load(int is_es) { (void)is_es; swgl_install(); return 0; }
int gl_is_es(void) { return 1; }
const char *gl_shader_header(void) { return ""; }
#endif

#else /* GL_DIRECT */
int swgl_active(void) { return 0; }
void swgl_install(void) {}
void swgl_window(int w, int h) { (void)w; (void)h; }
void swgl_clear_depth(float d) { (void)d; }
const uint8_t *swgl_window_pixels(int *w, int *h) { *w = *h = 0; return 0; }
#endif
