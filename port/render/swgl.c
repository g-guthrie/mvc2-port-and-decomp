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
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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

static void GL_APIENTRY sw_glClear(GLbitfield mask)
{
    Target t = target();
    int x0, y0, x1, y1;
    if (!t.c) return;
    bounds(&t, &x0, &y0, &x1, &y1);
    if (mask & GL_COLOR_BUFFER_BIT) {
        uint8_t c[4] = { unorm8(G.clear[0]), unorm8(G.clear[1]), unorm8(G.clear[2]), unorm8(G.clear[3]) };
        for (int y = y0; y < y1; y++) {
            uint8_t *p = t.c + ((size_t)y * t.w + x0) * 4;
            for (int x = x0; x < x1; x++, p += 4)
                for (int k = 0; k < 4; k++)
                    if (G.cmask[k]) p[k] = c[k];
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && t.z && G.zmask) {
        uint32_t z = depth24(G.clear_z);
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) t.z[(size_t)y * t.w + x] = z;
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

static void texel_rgba(const Tex *t, int i, int j, float o[4])
{
    const uint8_t *p = t->px + ((size_t)wrapi(j, t->h, t->clamp_t) * t->w + wrapi(i, t->w, t->clamp_s)) * 4;
    o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; o[3] = p[3] / 255.f;
}

/* texture(u_tex, uv) on an RGBA8 texture with no mip levels */
static void sample_rgba(const Tex *t, float u, float v, float o[4])
{
    if (!t->linear) {
        texel_rgba(t, (int)floorf(u * (float)t->w), (int)floorf(v * (float)t->h), o);
    } else {
        float x = u * (float)t->w - 0.5f, y = v * (float)t->h - 0.5f, fx = floorf(x), fy = floorf(y);
        float a = x - fx, b = y - fy, c00[4], c10[4], c01[4], c11[4];
        int i = (int)fx, j = (int)fy;
        texel_rgba(t, i, j, c00); texel_rgba(t, i + 1, j, c10);
        texel_rgba(t, i, j + 1, c01); texel_rgba(t, i + 1, j + 1, c11);
        for (int k = 0; k < 4; k++)
            o[k] = (c00[k] * (1.f - a) + c10[k] * a) * (1.f - b) + (c01[k] * (1.f - a) + c11[k] * a) * b;
    }
}

/* lut() of the shaders: index texel through a palette atlas row */
static void lut(const Tex *t, const Tex *pal, int row, int clamp, int i, int j, float o[4])
{
    const uint8_t *p;
    int idx;
    if (clamp) {
        i = i < 0 ? 0 : i >= t->w ? t->w - 1 : i;
        j = j < 0 ? 0 : j >= t->h ? t->h - 1 : j;
    } else {
        i = ((i % t->w) + t->w) % t->w;
        j = ((j % t->h) + t->h) % t->h;
    }
    idx = t->fmt == 1 ? t->px[(size_t)j * t->w + i] : t->px[((size_t)j * t->w + i) * 4];
    if (!pal || row < 0 || row >= pal->h) { o[0] = o[1] = o[2] = o[3] = 0.f; return; }
    p = pal->px + ((size_t)row * pal->w + idx) * 4;
    o[0] = p[0] / 255.f; o[1] = p[1] / 255.f; o[2] = p[2] / 255.f; o[3] = p[3] / 255.f;
}

static void sample_index(const Tex *t, const Tex *pal, int row, int linear, int clamp, float u, float v, float o[4])
{
    float x = u * (float)t->w, y = v * (float)t->h;
    if (!linear) {
        lut(t, pal, row, clamp, (int)floorf(x), (int)floorf(y), o);
    } else {
        float fx, fy, a, b, c00[4], c10[4], c01[4], c11[4];
        int i, j;
        x -= 0.5f; y -= 0.5f;
        fx = floorf(x); fy = floorf(y); a = x - fx; b = y - fy; i = (int)fx; j = (int)fy;
        lut(t, pal, row, clamp, i, j, c00); lut(t, pal, row, clamp, i + 1, j, c10);
        lut(t, pal, row, clamp, i, j + 1, c01); lut(t, pal, row, clamp, i + 1, j + 1, c11);
        for (int k = 0; k < 4; k++)
            o[k] = (c00[k] * (1.f - a) + c10[k] * a) * (1.f - b) + (c01[k] * (1.f - a) + c11[k] * a) * b;
    }
}

/* ---------------- the programs ---------------- */
#define NVAR 9
typedef struct { float pos[4]; float v[NVAR]; float flat; } VOut;

typedef struct {
    const Prog *g;
    const Tex *tex, *pal;
    int kind, mode, linear, clamp, flags, palrow;
    float fogcol[3];
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
            col[ch] = floorf((v < m->lclamp ? v : m->lclamp) * m->mod[ch]);
        }
        col[3] = floorf(m->lamb[3] * m->mod[3]);
    } else if (m->flags & 2) {
        memcpy(col, m->matcol, sizeof col);
    } else {
        for (int ch = 0; ch < 4; ch++) col[ch] = floorf(ac[ch] * m->cscale[ch] * m->mod[ch]);
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

/* Returns 0 for a discarded fragment. */
static int fs_run(const FragCtx *c, const float *v, float flat, float o[4])
{
    float t[4] = { 1.f, 1.f, 1.f, 1.f };
    if (c->kind == PK_BATCH) {
        float iw = 1.f / v[6];
        if (c->mode == 1 && c->tex) sample_rgba(c->tex, v[0], v[1], t);
        else if (c->mode == 2 && c->tex) sample_index(c->tex, c->pal, (int)flat, c->linear, c->clamp, v[0], v[1], t);
        for (int k = 0; k < 4; k++) o[k] = t[k] * v[2 + k] * iw;
        return o[3] >= 0.5f / 255.f;
    } else {
        float iw, col[4], f;
        if (v[0] > 0.f) return 0;
        iw = 1.f / v[8];
        for (int k = 0; k < 4; k++) col[k] = v[3 + k] * iw;
        if (c->flags & 32) {
            if (!c->tex) t[0] = t[1] = t[2] = t[3] = 0.f;
            else if (c->mode == 2) sample_index(c->tex, c->pal, c->palrow, c->linear, c->clamp, v[1], v[2], t);
            else sample_rgba(c->tex, v[1], v[2], t);
            for (int k = 0; k < 4; k++) o[k] = t[k] * col[k] / 128.f;
        } else {
            o[0] = col[0] / 255.f; o[1] = col[1] / 255.f; o[2] = col[2] / 255.f; o[3] = col[3] / 128.f;
        }
        for (int k = 0; k < 3; k++)
            if (o[k] > 1.f) o[k] = 1.f;
        if (o[3] > 255.f / 128.f) o[3] = 255.f / 128.f;
        if (o[3] * 128.f < 1.f) return 0;
        f = v[7] * iw / 255.f;
        for (int k = 0; k < 3; k++) o[k] = c->fogcol[k] * (1.f - f) + o[k] * f;
        return 1;
    }
}

/* ---------------- output merger ---------------- */
static float factor(GLenum f, float sa, float da)
{
    switch (f) {
    case GL_ONE: return 1.f;
    case GL_SRC_ALPHA: return sa;
    case GL_ONE_MINUS_SRC_ALPHA: return 1.f - sa;
    case GL_DST_ALPHA: return da;
    case GL_ONE_MINUS_DST_ALPHA: return 1.f - da;
    default: return 0.f;
    }
}

static void write_pixel(uint8_t *p, float s[4])
{
    for (int k = 0; k < 4; k++) s[k] = s[k] < 0.f ? 0.f : s[k] > 1.f ? 1.f : s[k];
    if (G.blend) {
        float d[4] = { p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f };
        float sf = factor(G.bl_s, s[3], d[3]), df = factor(G.bl_d, s[3], d[3]);
        for (int k = 0; k < 4; k++) {
            float a = s[k] * sf, b = d[k] * df;
            s[k] = G.bl_eq == GL_FUNC_SUBTRACT ? a - b : G.bl_eq == GL_FUNC_REVERSE_SUBTRACT ? b - a : a + b;
        }
    }
    for (int k = 0; k < 4; k++)
        if (G.cmask[k]) p[k] = unorm8(s[k]);
}

static int ztest(uint32_t z, uint32_t old)
{
    switch (G.zfunc) {
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

static void raster(const Target *t, const FragCtx *fc, const SV *a, const SV *b, const SV *c, float flat)
{
    /* 8 sub-pixel bits; edge functions in 64-bit integers */
    int64_t x0 = (int64_t)floor(a->x * 256.0 + 0.5), y0 = (int64_t)floor(a->y * 256.0 + 0.5);
    int64_t x1 = (int64_t)floor(b->x * 256.0 + 0.5), y1 = (int64_t)floor(b->y * 256.0 + 0.5);
    int64_t x2 = (int64_t)floor(c->x * 256.0 + 0.5), y2 = (int64_t)floor(c->y * 256.0 + 0.5);
    int64_t area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    int bx0, by0, bx1, by1, minx, miny, maxx, maxy;
    int64_t A0, B0, C0, A1, B1, C1, A2, B2, C2;
    double inv;
    const SV *s0 = a, *s1 = b, *s2 = c;
    if (area == 0) return;
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
    }
    /* edge i is opposite vertex i: E(x, y) = A x + B y + C, > 0 inside */
    A0 = y1 - y2; B0 = x2 - x1; C0 = x1 * y2 - x2 * y1;
    A1 = y2 - y0; B1 = x0 - x2; C1 = x2 * y0 - x0 * y2;
    A2 = y0 - y1; B2 = x1 - x0; C2 = x0 * y1 - x1 * y0;
    inv = 1.0 / (double)area;
    for (int py = miny; py <= maxy; py++) {
        int64_t sy = (int64_t)py * 256 + 128;
        uint8_t *row = t->c + (size_t)py * t->w * 4;
        uint32_t *zrow = t->z ? t->z + (size_t)py * t->w : 0;
        for (int px = minx; px <= maxx; px++) {
            int64_t sx = (int64_t)px * 256 + 128;
            int64_t e0 = A0 * sx + B0 * sy + C0, e1 = A1 * sx + B1 * sy + C1, e2 = A2 * sx + B2 * sy + C2;
            double w1, w2;
            float z, iw, var[NVAR], col[4];
            uint32_t zi = 0;
            /* top-left rule (y up): a zero edge counts when it is a left edge or a top edge */
            if (e0 < 0 || e1 < 0 || e2 < 0) continue;
            if (e0 == 0 && !(A0 > 0 || (A0 == 0 && B0 < 0))) continue;
            if (e1 == 0 && !(A1 > 0 || (A1 == 0 && B1 < 0))) continue;
            if (e2 == 0 && !(A2 > 0 || (A2 == 0 && B2 < 0))) continue;
            w1 = (double)e1 * inv; w2 = (double)e2 * inv;
            /* v0 + w1 (v1 - v0) + w2 (v2 - v0): equal inputs give exactly that value */
            z = s0->z + (float)(w1 * (double)(s1->z - s0->z) + w2 * (double)(s2->z - s0->z));
            if (zrow && G.depth) {
                zi = depth24(z);
                if (!ztest(zi, zrow[px])) continue;
            }
            iw = s0->iw + (float)(w1 * (double)(s1->iw - s0->iw) + w2 * (double)(s2->iw - s0->iw));
            for (int k = 0; k < NVAR; k++)
                var[k] = (s0->v[k] + (float)(w1 * (double)(s1->v[k] - s0->v[k]) + w2 * (double)(s2->v[k] - s0->v[k]))) / iw;
            if (!fs_run(fc, var, flat, col)) continue;
            if (zrow && G.depth && G.zmask) zrow[px] = zi;
            write_pixel(row + (size_t)px * 4, col);
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
            float u, v, c[4];
            uint8_t *o;
            if (x < 0 || x >= t->w) continue;
            u = (rect[0] + ((float)(x - G.vp[0]) + 0.5f) / (float)G.vp[2] * rect[2]) / tsz[0];
            v = (rect[1] + ((float)(y - G.vp[1]) + 0.5f) / (float)G.vp[3] * rect[3]) / tsz[1];
            if (smooth) {
                Tex tmp = *src;
                tmp.linear = 1; tmp.clamp_s = tmp.clamp_t = 1;
                sample_rgba(&tmp, u, v, c);
            } else {
                Tex tmp = *src;
                tmp.linear = 0; tmp.clamp_s = tmp.clamp_t = 1;
                sample_rgba(&tmp, u, v, c);
            }
            o = t->c + ((size_t)y * t->w + x) * 4;
            o[0] = unorm8(c[0]); o[1] = unorm8(c[1]); o[2] = unorm8(c[2]); o[3] = 255;
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
    if (!g || !t.c || count <= 0) return;
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
        memcpy(fc.fogcol, uni(g, "u_fogcol")->f, sizeof fc.fogcol);
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
    for (GLsizei i = 0; i + 2 < count; i += 3) {
        GLuint a = idx ? idx[i] : (GLuint)(first + i), b = idx ? idx[i + 1] : (GLuint)(first + i + 1),
               c = idx ? idx[i + 2] : (GLuint)(first + i + 2);
        triangle(&t, &fc, &vo[a - lo], &vo[b - lo], &vo[c - lo]);
    }
    free(vo);
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
}

#else /* GL_DIRECT */
int swgl_active(void) { return 0; }
void swgl_install(void) {}
void swgl_window(int w, int h) { (void)w; (void)h; }
void swgl_clear_depth(float d) { (void)d; }
const uint8_t *swgl_window_pixels(int *w, int *h) { *w = *h = 0; return 0; }
#endif
