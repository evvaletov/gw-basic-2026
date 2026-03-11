#ifndef GW_GRAPHICS_H
#define GW_GRAPHICS_H

#include <stdbool.h>

void gfx_init(int mode);
void gfx_shutdown(void);
void gfx_cls(void);
bool gfx_active(void);
int  gfx_get_mode(void);

void gfx_pset(int x, int y, int color);
int  gfx_point(int x, int y);
void gfx_line(int x1, int y1, int x2, int y2, int color, int style);
void gfx_circle(int cx, int cy, int r, int color, double start, double end, double aspect);
void gfx_paint(int x, int y, int fill_color, int border_color);
void gfx_draw(const char *cmd);

void gfx_flush(void);

/* Current drawing state */
void gfx_set_color(int c);
int  gfx_get_color(void);
void gfx_get_last(int *x, int *y);
void gfx_set_last(int x, int y);
int  gfx_get_width(void);
int  gfx_get_height(void);

/* LINE style constants */
#define GFX_LINE  0
#define GFX_BOX   'B'
#define GFX_BOXF  'F'

/* GET/PUT sprite support */
#include <stdint.h>

/* PUT action modes */
#define GFX_ACTION_XOR    0
#define GFX_ACTION_PSET   1
#define GFX_ACTION_PRESET 2
#define GFX_ACTION_AND    3
#define GFX_ACTION_OR     4

/* Capture screen rectangle into int16 array. Returns required array size. */
int gfx_sprite_get(int x1, int y1, int x2, int y2, int16_t *buf, int bufsize);

/* Blit sprite from int16 array to screen. */
void gfx_sprite_put(int x, int y, const int16_t *buf, int bufsize, int action);

/* Calculate required array element count for a sprite rectangle. */
int gfx_sprite_size(int x1, int y1, int x2, int y2);

/* VIEW / WINDOW / PALETTE */
void gfx_view(bool screen_flag, int x1, int y1, int x2, int y2,
              int fill, bool has_fill, int border, bool has_border);
void gfx_view_reset(void);
void gfx_window(bool screen_flag, double x1, double y1, double x2, double y2);
void gfx_window_reset(void);
void gfx_palette_set(int attr, int color);
void gfx_palette_reset(void);

/* Coordinate mapping (logical ↔ physical) */
int  gfx_map_x(double x);
int  gfx_map_y(double y);
double gfx_pmap(double coord, int func);
bool gfx_has_window(void);

/* CGA framebuffer PEEK/POKE (segment 0xB800 in graphics mode) */
uint8_t gfx_cga_peek(uint16_t offset);
void    gfx_cga_poke(uint16_t offset, uint8_t val);

#endif
