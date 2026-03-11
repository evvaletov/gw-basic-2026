#include "graphics.h"
#include "hal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

static uint8_t *framebuf;
static int fb_width, fb_height;
static int screen_mode;
static int current_color = 1;
static int last_x, last_y;

/* CGA default palette (RGBI) */
static const uint32_t default_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* Active palette: palette_map[attr] gives the physical color index */
static int palette_map[16];

/* Viewport state (VIEW) */
static bool vp_active;
static bool vp_screen;  /* VIEW SCREEN flag */
static int vp_x1, vp_y1, vp_x2, vp_y2;

/* Window state (WINDOW) */
static bool win_active;
static bool win_screen;  /* WINDOW SCREEN flag */
static double win_x1, win_y1, win_x2, win_y2;

static void reset_palette(void)
{
    for (int i = 0; i < 16; i++)
        palette_map[i] = i;
}

bool gfx_active(void) { return framebuf != NULL; }
int  gfx_get_mode(void) { return screen_mode; }
void gfx_set_color(int c) { current_color = c; }
int  gfx_get_color(void) { return current_color; }
void gfx_get_last(int *x, int *y) { *x = last_x; *y = last_y; }
void gfx_set_last(int x, int y) { last_x = x; last_y = y; }
int  gfx_get_width(void) { return fb_width; }
int  gfx_get_height(void) { return fb_height; }

void gfx_init(int mode)
{
    gfx_shutdown();
    screen_mode = mode;
    switch (mode) {
    case 1: fb_width = 320; fb_height = 200; break;
    case 2: fb_width = 640; fb_height = 200; break;
    default: return;  /* text mode, no framebuffer */
    }
    framebuf = calloc(fb_width * fb_height, 1);
    current_color = (mode == 2) ? 1 : 3;
    last_x = 0;
    last_y = 0;
    vp_active = false;
    vp_x1 = 0; vp_y1 = 0;
    vp_x2 = fb_width - 1; vp_y2 = fb_height - 1;
    win_active = false;
    reset_palette();
}

void gfx_shutdown(void)
{
    free(framebuf);
    framebuf = NULL;
    fb_width = 0;
    fb_height = 0;
    screen_mode = 0;
    vp_active = false;
    win_active = false;
    reset_palette();
}

void gfx_cls(void)
{
    if (!framebuf) return;
    if (vp_active) {
        for (int y = vp_y1; y <= vp_y2; y++)
            for (int x = vp_x1; x <= vp_x2; x++)
                framebuf[y * fb_width + x] = 0;
    } else {
        memset(framebuf, 0, fb_width * fb_height);
    }
}

/* Clip bounds: viewport when active, otherwise full framebuffer */
static inline void clip_bounds(int *cx1, int *cy1, int *cx2, int *cy2)
{
    if (vp_active) {
        *cx1 = vp_x1; *cy1 = vp_y1;
        *cx2 = vp_x2; *cy2 = vp_y2;
    } else {
        *cx1 = 0; *cy1 = 0;
        *cx2 = fb_width - 1; *cy2 = fb_height - 1;
    }
}

static inline void set_pixel(int x, int y, int color)
{
    int cx1, cy1, cx2, cy2;
    clip_bounds(&cx1, &cy1, &cx2, &cy2);
    if (x >= cx1 && x <= cx2 && y >= cy1 && y <= cy2)
        framebuf[y * fb_width + x] = color;
}

static inline int get_pixel(int x, int y)
{
    if (x >= 0 && x < fb_width && y >= 0 && y < fb_height)
        return framebuf[y * fb_width + x];
    return 0;
}

void gfx_pset(int x, int y, int color)
{
    set_pixel(x, y, color);
    last_x = x;
    last_y = y;
}

int gfx_point(int x, int y)
{
    return get_pixel(x, y);
}

/* Bresenham line */
static void draw_line(int x0, int y0, int x1, int y1, int color)
{
    int dx = abs(x1 - x0);
    int dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_line(int x1, int y1, int x2, int y2, int color, int style)
{
    if (style == GFX_BOXF) {
        /* Filled box */
        int xlo = x1 < x2 ? x1 : x2;
        int xhi = x1 > x2 ? x1 : x2;
        int ylo = y1 < y2 ? y1 : y2;
        int yhi = y1 > y2 ? y1 : y2;
        for (int y = ylo; y <= yhi; y++)
            for (int x = xlo; x <= xhi; x++)
                set_pixel(x, y, color);
    } else if (style == GFX_BOX) {
        /* Box outline */
        draw_line(x1, y1, x2, y1, color);
        draw_line(x2, y1, x2, y2, color);
        draw_line(x2, y2, x1, y2, color);
        draw_line(x1, y2, x1, y1, color);
    } else {
        draw_line(x1, y1, x2, y2, color);
    }
    last_x = x2;
    last_y = y2;
}

/* Midpoint circle with aspect ratio */
void gfx_circle(int cx, int cy, int r, int color,
                double start, double end, double aspect)
{
    if (r <= 0) return;

    if (start == 0.0 && end == 0.0) {
        /* Full circle */
        double ax = (aspect > 0) ? aspect : (5.0 / 6.0); /* CGA aspect */
        int x = 0, y = r;
        int d = 1 - r;
        while (x <= y) {
            int px, py;
            /* 8 octants with aspect adjustment */
            px = (int)(x * ax); py = y;
            set_pixel(cx + px, cy + py, color);
            set_pixel(cx - px, cy + py, color);
            set_pixel(cx + px, cy - py, color);
            set_pixel(cx - px, cy - py, color);
            px = (int)(y * ax); py = x;
            set_pixel(cx + px, cy + py, color);
            set_pixel(cx - px, cy + py, color);
            set_pixel(cx + px, cy - py, color);
            set_pixel(cx - px, cy - py, color);
            if (d < 0) {
                d += 2 * x + 3;
            } else {
                d += 2 * (x - y) + 5;
                y--;
            }
            x++;
        }
    } else {
        /* Arc: parametric with aspect */
        double ax = (aspect > 0) ? aspect : (5.0 / 6.0);
        double step = 1.0 / r;
        if (step > 0.05) step = 0.05;
        for (double a = start; a <= end; a += step) {
            int px = cx + (int)(r * cos(a) * ax);
            int py = cy - (int)(r * sin(a));
            set_pixel(px, py, color);
        }
    }
    last_x = cx;
    last_y = cy;
}

/* Flood fill (stack-based) */
void gfx_paint(int x, int y, int fill_color, int border_color)
{
    if (!framebuf) return;
    if (x < 0 || x >= fb_width || y < 0 || y >= fb_height) return;

    int start = get_pixel(x, y);
    if (start == fill_color || start == border_color) return;

    /* Simple stack-based flood fill */
    int stack_cap = 4096;
    int *stack = malloc(stack_cap * 2 * sizeof(int));
    if (!stack) return;
    int sp = 0;

    #define PUSH(px, py) do { \
        if (sp < stack_cap) { stack[sp*2] = (px); stack[sp*2+1] = (py); sp++; } \
    } while(0)
    #define POP(px, py) do { sp--; (px) = stack[sp*2]; (py) = stack[sp*2+1]; } while(0)

    PUSH(x, y);
    while (sp > 0) {
        int cx, cy;
        POP(cx, cy);
        if (cx < 0 || cx >= fb_width || cy < 0 || cy >= fb_height) continue;
        int c = get_pixel(cx, cy);
        if (c == fill_color || c == border_color) continue;
        set_pixel(cx, cy, fill_color);
        /* Grow stack if needed */
        if (sp + 4 >= stack_cap) {
            stack_cap *= 2;
            int *ns = realloc(stack, stack_cap * 2 * sizeof(int));
            if (!ns) break;
            stack = ns;
        }
        PUSH(cx + 1, cy);
        PUSH(cx - 1, cy);
        PUSH(cx, cy + 1);
        PUSH(cx, cy - 1);
    }

    #undef PUSH
    #undef POP
    free(stack);
}

/* DRAW mini-language parser */
void gfx_draw(const char *cmd)
{
    if (!framebuf) return;
    int x = last_x, y = last_y;
    int draw_color = current_color;
    int scale = 4;  /* default scale factor (C4) */
    const char *p = cmd;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        int blind = 0, no_update = 0;
        /* Prefix modifiers */
        while (*p == 'B' || *p == 'b' || *p == 'N' || *p == 'n') {
            if (*p == 'B' || *p == 'b') blind = 1;
            if (*p == 'N' || *p == 'n') no_update = 1;
            p++;
        }

        char ch = toupper(*p);
        if (!ch) break;
        p++;

        /* Parse optional numeric argument */
        int has_arg = 0, arg = 0;
        if (*p == '-' || isdigit((unsigned char)*p)) {
            int neg = 0;
            if (*p == '-') { neg = 1; p++; }
            while (isdigit((unsigned char)*p)) {
                arg = arg * 10 + (*p - '0');
                p++;
            }
            if (neg) arg = -arg;
            has_arg = 1;
        }

        int dist = has_arg ? arg : scale;
        int nx = x, ny = y;

        switch (ch) {
        case 'U': ny = y - dist; break;
        case 'D': ny = y + dist; break;
        case 'L': nx = x - dist; break;
        case 'R': nx = x + dist; break;
        case 'E': nx = x + dist; ny = y - dist; break;
        case 'F': nx = x + dist; ny = y + dist; break;
        case 'G': nx = x - dist; ny = y + dist; break;
        case 'H': nx = x - dist; ny = y - dist; break;
        case 'M': {
            /* M x,y or M +/-x, +/-y */
            int relative = 0;
            while (*p == ' ') p++;
            if (*p == '+' || *p == '-') relative = 1;
            int mx = 0, my = 0;
            int mneg = 0;
            if (*p == '-') { mneg = 1; p++; }
            else if (*p == '+') p++;
            while (isdigit((unsigned char)*p)) { mx = mx * 10 + (*p - '0'); p++; }
            if (mneg) mx = -mx;
            while (*p == ' ' || *p == ',') p++;
            mneg = 0;
            if (*p == '-') { mneg = 1; p++; }
            else if (*p == '+') p++;
            while (isdigit((unsigned char)*p)) { my = my * 10 + (*p - '0'); p++; }
            if (mneg) my = -my;
            if (relative) { nx = x + mx; ny = y + my; }
            else { nx = mx; ny = my; }
            break;
        }
        case 'C':
            draw_color = has_arg ? arg : 1;
            continue;
        case 'S':
            scale = has_arg ? arg : 4;
            continue;
        case 'A':
            /* Angle: 0-3, we ignore rotation for simplicity */
            continue;
        case ';':
            continue;
        default:
            continue;
        }

        if (!blind)
            draw_line(x, y, nx, ny, draw_color);

        if (!no_update) {
            x = nx;
            y = ny;
        }

        while (*p == ' ' || *p == ';') p++;
    }

    last_x = x;
    last_y = y;
}

/* Sixel output encoder */
void gfx_flush(void)
{
    if (!framebuf || !gw_hal) return;

    /* Determine which colors are used */
    int color_used[16] = {0};
    for (int i = 0; i < fb_width * fb_height; i++)
        color_used[framebuf[i] & 0x0F] = 1;

    /* Build Sixel output in a buffer */
    int buf_cap = 1024 * 64;
    char *buf = malloc(buf_cap);
    if (!buf) return;
    int pos = 0;

    #define EMIT(s) do { \
        int _l = strlen(s); \
        if (pos + _l >= buf_cap) { \
            buf_cap *= 2; \
            char *nb = realloc(buf, buf_cap); \
            if (!nb) { free(buf); return; } \
            buf = nb; \
        } \
        memcpy(buf + pos, s, _l); pos += _l; \
    } while(0)

    /* DCS q - enter Sixel mode */
    EMIT("\033Pq");

    /* Define palette (using palette mapping) */
    for (int c = 0; c < 16; c++) {
        if (!color_used[c]) continue;
        uint32_t rgb = default_palette[palette_map[c]];
        int r = ((rgb >> 16) & 0xFF) * 100 / 255;
        int g = ((rgb >> 8) & 0xFF) * 100 / 255;
        int b = (rgb & 0xFF) * 100 / 255;
        char pal[32];
        snprintf(pal, sizeof(pal), "#%d;2;%d;%d;%d", c, r, g, b);
        EMIT(pal);
    }

    /* Output Sixel bands (6 rows each) */
    for (int band = 0; band < fb_height; band += 6) {
        int band_h = (band + 6 <= fb_height) ? 6 : fb_height - band;

        for (int c = 0; c < 16; c++) {
            if (!color_used[c]) continue;

            /* Check if this color appears in this band */
            int has_color = 0;
            for (int y2 = band; y2 < band + band_h && !has_color; y2++)
                for (int x2 = 0; x2 < fb_width && !has_color; x2++)
                    if (framebuf[y2 * fb_width + x2] == c)
                        has_color = 1;
            if (!has_color) continue;

            /* Emit color selector */
            char cs[8];
            snprintf(cs, sizeof(cs), "#%d", c);
            EMIT(cs);

            /* Emit Sixel data for this color in this band */
            /* Use RLE for runs of same byte */
            int prev_sixel = -1;
            int run_len = 0;

            for (int x2 = 0; x2 < fb_width; x2++) {
                int sixel = 0;
                for (int bit = 0; bit < band_h; bit++) {
                    int y2 = band + bit;
                    if (framebuf[y2 * fb_width + x2] == c)
                        sixel |= (1 << bit);
                }

                if (sixel == prev_sixel) {
                    run_len++;
                } else {
                    /* Flush previous run */
                    if (prev_sixel >= 0) {
                        char sc[16];
                        if (run_len > 3) {
                            snprintf(sc, sizeof(sc), "!%d%c", run_len, prev_sixel + 63);
                        } else {
                            for (int r2 = 0; r2 < run_len; r2++) {
                                sc[0] = prev_sixel + 63;
                                sc[1] = '\0';
                                EMIT(sc);
                            }
                            sc[0] = '\0';
                        }
                        EMIT(sc);
                    }
                    prev_sixel = sixel;
                    run_len = 1;
                }
            }
            /* Flush last run */
            if (prev_sixel >= 0) {
                char sc[16];
                if (run_len > 3) {
                    snprintf(sc, sizeof(sc), "!%d%c", run_len, prev_sixel + 63);
                } else {
                    for (int r2 = 0; r2 < run_len; r2++) {
                        sc[0] = prev_sixel + 63;
                        sc[1] = '\0';
                        EMIT(sc);
                    }
                    sc[0] = '\0';
                }
                EMIT(sc);
            }

            /* GNL ($/- selectors): $ = CR (reuse row), - = newline */
            /* If more colors in this band, use $; else use - for next band */
            /* Check if there's another color to render in this band */
            int more = 0;
            for (int nc = c + 1; nc < 16; nc++) {
                if (!color_used[nc]) continue;
                for (int y2 = band; y2 < band + band_h && !more; y2++)
                    for (int x2 = 0; x2 < fb_width && !more; x2++)
                        if (framebuf[y2 * fb_width + x2] == nc)
                            more = 1;
                if (more) break;
            }
            if (more) {
                EMIT("$");  /* CR - overlay next color on same band */
            }
        }
        EMIT("-");  /* LF - advance to next band */
    }

    /* ST - string terminator */
    EMIT("\033\\");

    #undef EMIT

    /* Write out via HAL raw output */
    gw_hal->write_raw(buf, pos);
    free(buf);
}

/* ================================================================
 * VIEW / WINDOW / PALETTE
 * ================================================================ */

void gfx_view(bool screen_flag, int x1, int y1, int x2, int y2,
              int fill, bool has_fill, int border, bool has_border)
{
    if (!framebuf) return;

    /* Normalize */
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    /* Clamp to screen */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= fb_width) x2 = fb_width - 1;
    if (y2 >= fb_height) y2 = fb_height - 1;

    vp_active = true;
    vp_screen = screen_flag;
    vp_x1 = x1; vp_y1 = y1;
    vp_x2 = x2; vp_y2 = y2;

    /* Draw border (outside viewport, on full framebuffer) */
    if (has_border) {
        bool save = vp_active;
        vp_active = false;
        if (x1 > 0) draw_line(x1 - 1, y1 > 0 ? y1 - 1 : y1, x1 - 1, y2 < fb_height - 1 ? y2 + 1 : y2, border);
        if (x2 < fb_width - 1) draw_line(x2 + 1, y1 > 0 ? y1 - 1 : y1, x2 + 1, y2 < fb_height - 1 ? y2 + 1 : y2, border);
        if (y1 > 0) draw_line(x1 > 0 ? x1 - 1 : x1, y1 - 1, x2 < fb_width - 1 ? x2 + 1 : x2, y1 - 1, border);
        if (y2 < fb_height - 1) draw_line(x1 > 0 ? x1 - 1 : x1, y2 + 1, x2 < fb_width - 1 ? x2 + 1 : x2, y2 + 1, border);
        vp_active = save;
    }

    /* Fill viewport */
    if (has_fill) {
        for (int y = vp_y1; y <= vp_y2; y++)
            for (int x = vp_x1; x <= vp_x2; x++)
                framebuf[y * fb_width + x] = fill;
    }

    /* Reset last position to viewport origin */
    last_x = vp_x1;
    last_y = vp_y1;

    /* Reset WINDOW when VIEW changes */
    win_active = false;
}

void gfx_view_reset(void)
{
    vp_active = false;
    if (framebuf) {
        vp_x1 = 0; vp_y1 = 0;
        vp_x2 = fb_width - 1; vp_y2 = fb_height - 1;
    }
    win_active = false;
    last_x = 0;
    last_y = 0;
}

void gfx_window(bool screen_flag, double x1, double y1, double x2, double y2)
{
    win_active = true;
    win_screen = screen_flag;
    win_x1 = x1; win_y1 = y1;
    win_x2 = x2; win_y2 = y2;
}

void gfx_window_reset(void)
{
    win_active = false;
}

bool gfx_has_window(void)
{
    return win_active;
}

int gfx_map_x(double x)
{
    int left  = vp_active ? vp_x1 : 0;
    int right = vp_active ? vp_x2 : (fb_width - 1);

    if (win_active) {
        if (win_x2 == win_x1) return left;
        return left + (int)((x - win_x1) / (win_x2 - win_x1) * (right - left) + 0.5);
    }
    if (vp_active && !vp_screen)
        return left + (int)x;
    return (int)x;
}

int gfx_map_y(double y)
{
    int top    = vp_active ? vp_y1 : 0;
    int bottom = vp_active ? vp_y2 : (fb_height - 1);

    if (win_active) {
        if (win_y2 == win_y1) return top;
        if (win_screen) {
            /* WINDOW SCREEN: Y increases downward */
            return top + (int)((y - win_y1) / (win_y2 - win_y1) * (bottom - top) + 0.5);
        } else {
            /* WINDOW (Cartesian): Y increases upward */
            return bottom - (int)((y - win_y1) / (win_y2 - win_y1) * (bottom - top) + 0.5);
        }
    }
    if (vp_active && !vp_screen)
        return top + (int)y;
    return (int)y;
}

double gfx_pmap(double coord, int func)
{
    int left   = vp_active ? vp_x1 : 0;
    int right  = vp_active ? vp_x2 : (fb_width - 1);
    int top    = vp_active ? vp_y1 : 0;
    int bottom = vp_active ? vp_y2 : (fb_height - 1);

    if (!win_active) return coord;

    switch (func) {
    case 0: /* logical X → physical X */
        if (win_x2 == win_x1) return left;
        return left + (coord - win_x1) / (win_x2 - win_x1) * (right - left);
    case 1: /* logical Y → physical Y */
        if (win_y2 == win_y1) return top;
        if (win_screen)
            return top + (coord - win_y1) / (win_y2 - win_y1) * (bottom - top);
        else
            return bottom - (coord - win_y1) / (win_y2 - win_y1) * (bottom - top);
    case 2: /* physical X → logical X */
        if (right == left) return win_x1;
        return win_x1 + (coord - left) / (double)(right - left) * (win_x2 - win_x1);
    case 3: /* physical Y → logical Y */
        if (bottom == top) return win_y1;
        if (win_screen)
            return win_y1 + (coord - top) / (double)(bottom - top) * (win_y2 - win_y1);
        else
            return win_y1 + (bottom - coord) / (double)(bottom - top) * (win_y2 - win_y1);
    default:
        return coord;
    }
}

void gfx_palette_set(int attr, int color)
{
    if (attr < 0 || attr > 15 || color < 0 || color > 15) return;
    palette_map[attr] = color;
}

void gfx_palette_reset(void)
{
    reset_palette();
}

/*
 * CGA framebuffer PEEK/POKE.
 *
 * CGA interlaced layout:
 *   Even rows: offset 0x0000, odd rows: offset 0x2000
 *   Each row = 80 bytes
 *   SCREEN 1: 2 bpp (4 pixels/byte, MSB first)
 *   SCREEN 2: 1 bpp (8 pixels/byte, MSB first)
 */

static void cga_offset_to_pixels(uint16_t offset, int *row, int *col_byte)
{
    int bank = (offset >= 0x2000) ? 1 : 0;
    uint16_t linear = offset - bank * 0x2000;
    *row = (linear / 80) * 2 + bank;
    *col_byte = linear % 80;
}

uint8_t gfx_cga_peek(uint16_t offset)
{
    if (!framebuf) return 0;

    int row, col_byte;
    cga_offset_to_pixels(offset, &row, &col_byte);
    if (row < 0 || row >= fb_height) return 0;

    int bpp = (screen_mode == 2) ? 1 : 2;
    int ppb = 8 / bpp;
    int base_x = col_byte * ppb;
    uint8_t val = 0;

    for (int i = 0; i < ppb; i++) {
        int x = base_x + i;
        if (x >= fb_width) break;
        int px = get_pixel(x, row) & ((1 << bpp) - 1);
        val |= px << (8 - bpp - i * bpp);
    }
    return val;
}

void gfx_cga_poke(uint16_t offset, uint8_t val)
{
    if (!framebuf) return;

    int row, col_byte;
    cga_offset_to_pixels(offset, &row, &col_byte);
    if (row < 0 || row >= fb_height) return;

    int bpp = (screen_mode == 2) ? 1 : 2;
    int ppb = 8 / bpp;
    int base_x = col_byte * ppb;
    int mask = (1 << bpp) - 1;

    for (int i = 0; i < ppb; i++) {
        int x = base_x + i;
        if (x >= fb_width) break;
        int px = (val >> (8 - bpp - i * bpp)) & mask;
        set_pixel(x, row, px);
    }
}

/*
 * GET/PUT sprite support.
 *
 * Sprites are stored in integer arrays using the CGA packed format:
 *   Word 0: width in bits (pixel_width * bits_per_pixel)
 *   Word 1: height in scan lines
 *   Remaining: packed pixel data, row by row, padded to word boundary
 *
 * SCREEN 1: 2 bits/pixel (4 pixels/byte)
 * SCREEN 2: 1 bit/pixel  (8 pixels/byte)
 */

static int bits_per_pixel(void)
{
    return (screen_mode == 2) ? 1 : 2;
}

int gfx_sprite_size(int x1, int y1, int x2, int y2)
{
    if (!framebuf) return 0;
    int w = abs(x2 - x1) + 1;
    int h = abs(y2 - y1) + 1;
    int bpp = bits_per_pixel();
    int bytes_per_row = (w * bpp + 7) / 8;
    if (bytes_per_row & 1) bytes_per_row++;  /* pad to word boundary */
    int data_bytes = bytes_per_row * h;
    return 2 + (data_bytes + 1) / 2;  /* 2 header words + data words */
}

int gfx_sprite_get(int x1, int y1, int x2, int y2, int16_t *buf, int bufsize)
{
    if (!framebuf) return 0;

    /* Normalize coordinates */
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int bpp = bits_per_pixel();
    int width_bits = w * bpp;
    int bytes_per_row = (width_bits + 7) / 8;
    if (bytes_per_row & 1) bytes_per_row++;  /* word-align */
    int data_bytes = bytes_per_row * h;
    int required = 2 + (data_bytes + 1) / 2;

    if (bufsize < required) return required;

    buf[0] = (int16_t)width_bits;
    buf[1] = (int16_t)h;

    /* Pack pixels into CGA format */
    uint8_t *out = (uint8_t *)&buf[2];
    memset(out, 0, data_bytes);

    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            int px = get_pixel(x1 + col, y1 + row);
            int bit_offset = col * bpp;
            int byte_idx = bit_offset / 8;
            int bit_pos = 8 - bpp - (bit_offset % 8);  /* MSB first */
            out[row * bytes_per_row + byte_idx] |=
                (px & ((1 << bpp) - 1)) << bit_pos;
        }
    }

    return required;
}

void gfx_sprite_put(int x, int y, const int16_t *buf, int bufsize, int action)
{
    if (!framebuf || bufsize < 2) return;

    int bpp = bits_per_pixel();
    int width_bits = (uint16_t)buf[0];
    int h = (uint16_t)buf[1];
    int w = width_bits / bpp;
    if (w <= 0 || h <= 0) return;

    int bytes_per_row = (width_bits + 7) / 8;
    if (bytes_per_row & 1) bytes_per_row++;

    const uint8_t *src = (const uint8_t *)&buf[2];
    int mask = (1 << bpp) - 1;

    for (int row = 0; row < h; row++) {
        int sy = y + row;
        if (sy < 0 || sy >= fb_height) continue;
        for (int col = 0; col < w; col++) {
            int sx = x + col;
            if (sx < 0 || sx >= fb_width) continue;

            int bit_offset = col * bpp;
            int byte_idx = bit_offset / 8;
            int bit_pos = 8 - bpp - (bit_offset % 8);
            int sprite_px = (src[row * bytes_per_row + byte_idx] >> bit_pos) & mask;

            int old_px = get_pixel(sx, sy);
            int new_px;
            switch (action) {
            case GFX_ACTION_PSET:   new_px = sprite_px; break;
            case GFX_ACTION_PRESET: new_px = (~sprite_px) & mask; break;
            case GFX_ACTION_AND:    new_px = old_px & sprite_px; break;
            case GFX_ACTION_OR:     new_px = old_px | sprite_px; break;
            default:                new_px = old_px ^ sprite_px; break;  /* XOR */
            }
            set_pixel(sx, sy, new_px);
        }
    }
}
