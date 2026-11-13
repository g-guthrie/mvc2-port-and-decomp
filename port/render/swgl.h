/* swgl: a software implementation of the GL subset the renderer uses (swgl.c). */
#ifndef PORT_SWGL_H
#define PORT_SWGL_H
#include <stdint.h>

/* Point every p_gl* entry at the software implementation.  Call it in place
 * of gl_load(), before r_init(). */
void swgl_install(void);
int swgl_active(void);
/* Size of framebuffer 0 (the window the present pass draws into). */
void swgl_window(int w, int h);
/* Framebuffer 0: RGBA8, rows bottom up. */
const uint8_t *swgl_window_pixels(int *w, int *h);
void swgl_clear_depth(float d);
#endif
