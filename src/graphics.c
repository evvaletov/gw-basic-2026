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
static uint32_t palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

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
}

void gfx_shutdown(void)
{
    free(framebuf);
    framebuf = NULL;
    fb_width = 0;
    fb_height = 0;
    screen_mode = 0;
}

void gfx_cls(void)
{
    if (framebuf)
        memset(framebuf, 0, fb_width * fb_height);
}

static inline void set_pixel(int x, int y, int color)
{
    if (x >= 0 && x < fb_width && y >= 0 && y < fb_height)
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

    /* Define palette */
    for (int c = 0; c < 16; c++) {
        if (!color_used[c]) continue;
        uint32_t rgb = palette[c];
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
