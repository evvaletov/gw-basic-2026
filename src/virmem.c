#include "virmem.h"
#include "tui.h"
#include "graphics.h"
#include <time.h>

/*
 * BIOS data area emulation (segment 0x0040).
 *
 * Real addresses we care about:
 *   0040:0017  Keyboard shift flags byte 1
 *   0040:0018  Keyboard shift flags byte 2
 *   0040:0049  Current video mode
 *   0040:004A  Number of screen columns (word)
 *   0040:0050  Cursor position page 0 (col, row)
 *   0040:006C  Timer tick count (dword, ~18.2 Hz)
 *   0040:0084  Rows on screen minus 1
 */
static uint8_t bios_peek(uint16_t offset)
{
    switch (offset) {
    case 0x17:
        /* Bit 7 = insert mode; other modifier bits not detectable on POSIX */
        return tui.insert_mode ? 0x80 : 0;
    case 0x18:
        return 0;

    case 0x49:
        /* Current video mode: 3 = 80x25 color text, 1 = 320x200, 2 = 640x200 */
        return gfx_active() ? (uint8_t)gfx_get_mode() : 3;

    case 0x4A:
        return tui.active ? (uint8_t)tui.cols : 80;
    case 0x4B:
        return 0;  /* high byte of column count */

    case 0x50:
        return tui.active ? (uint8_t)tui.cursor_col : 0;
    case 0x51:
        return tui.active ? (uint8_t)tui.cursor_row : 0;

    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        /* Timer ticks since midnight (~18.2065 Hz) */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        struct tm tm;
        localtime_r(&ts.tv_sec, &tm);
        long secs = tm.tm_hour * 3600L + tm.tm_min * 60L + tm.tm_sec;
        uint32_t ticks = (uint32_t)(secs * 18.2065 + ts.tv_nsec / 54925400.0);
        return (ticks >> ((offset - 0x6C) * 8)) & 0xFF;
    }

    case 0x84:
        return tui.active ? (uint8_t)(tui.rows - 1) : 24;

    default:
        return 0;
    }
}

static void bios_poke(uint16_t offset, uint8_t value)
{
    (void)offset;
    (void)value;
    /* BIOS data area writes are silently ignored */
}

/*
 * CGA text video buffer emulation (segment 0xB800).
 *
 * Layout: 80x25 = 4000 bytes, alternating character and attribute bytes.
 *   offset = (row * 80 + col) * 2      → character
 *   offset = (row * 80 + col) * 2 + 1  → attribute (fg | bg<<4)
 */
static uint8_t cga_text_peek(uint16_t offset)
{
    if (!tui.active || !tui.screen) return 0;

    int cell = offset / 2;
    int cols = tui.cols;
    int row = cell / cols;
    int col = cell % cols;

    if (row >= tui.rows) return 0;

    if (offset & 1)
        return TUI_CELL(row, col).attr;
    else
        return TUI_CELL(row, col).ch;
}

static void cga_text_poke(uint16_t offset, uint8_t value)
{
    if (!tui.active || !tui.screen) return;

    int cell = offset / 2;
    int cols = tui.cols;
    int row = cell / cols;
    int col = cell % cols;

    if (row >= tui.rows) return;

    if (offset & 1)
        TUI_CELL(row, col).attr = value;
    else
        TUI_CELL(row, col).ch = value;
}

uint8_t virmem_peek(uint16_t segment, uint16_t offset)
{
    if (segment == 0x0040)
        return bios_peek(offset);

    if (segment == 0xB800) {
        if (gfx_active())
            return gfx_cga_peek(offset);
        return cga_text_peek(offset);
    }

    /* Unhandled regions return 0 */
    return 0;
}

void virmem_poke(uint16_t segment, uint16_t offset, uint8_t value)
{
    if (segment == 0x0040) {
        bios_poke(offset, value);
        return;
    }

    if (segment == 0xB800) {
        if (gfx_active())
            gfx_cga_poke(offset, value);
        else
            cga_text_poke(offset, value);
        return;
    }

    /* Unhandled regions: silently discard */
}
