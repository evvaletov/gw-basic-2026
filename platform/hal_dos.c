/*
 * DOS HAL backend for GW-BASIC 2026.
 *
 * Uses BIOS/DOS interrupts for terminal I/O, keyboard input,
 * and screen control.  Replaces the POSIX HAL (hal_posix.c)
 * when targeting FreeDOS or other DOS-compatible systems.
 *
 * Build with OpenWatcom:  wcl386 -bt=dos -l=dos4g ...
 * Build with DJGPP:       gcc -o gwbasic.exe ...
 *
 * Linux compatibility is retained -- hal_posix.c is used on
 * POSIX systems, hal_dos.c on DOS.  The hal_ops_t vtable
 * provides the compile-time abstraction.
 */

#ifdef __MSDOS__

#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>     /* OpenWatcom int86() */

static int cursor_row = 0;
static int cursor_col = 0;
static int screen_cols = 80;
static int screen_rows = 25;

/* --- BIOS video services (INT 10h) --- */

static void bios_set_cursor(int row, int col)
{
    union REGS r;
    r.h.ah = 0x02;      /* Set cursor position */
    r.h.bh = 0;         /* Page 0 */
    r.h.dh = (uint8_t)row;
    r.h.dl = (uint8_t)col;
    int86(0x10, &r, &r);
    cursor_row = row;
    cursor_col = col;
}

static void bios_get_cursor(int *row, int *col)
{
    union REGS r;
    r.h.ah = 0x03;      /* Get cursor position */
    r.h.bh = 0;
    int86(0x10, &r, &r);
    *row = r.h.dh;
    *col = r.h.dl;
}

static void bios_scroll_up(int lines, int attr, int r1, int c1, int r2, int c2)
{
    union REGS r;
    r.h.ah = 0x06;      /* Scroll window up */
    r.h.al = (uint8_t)lines;
    r.h.bh = (uint8_t)attr;
    r.h.ch = (uint8_t)r1;
    r.h.cl = (uint8_t)c1;
    r.h.dh = (uint8_t)r2;
    r.h.dl = (uint8_t)c2;
    int86(0x10, &r, &r);
}

static void bios_write_char(int ch, int attr)
{
    union REGS r;
    r.h.ah = 0x09;      /* Write char + attribute */
    r.h.al = (uint8_t)ch;
    r.h.bh = 0;
    r.h.bl = (uint8_t)attr;
    r.x.cx = 1;
    int86(0x10, &r, &r);
}

/* --- Terminal I/O --- */

static void dos_putch(int ch)
{
    if (ch == '\n') {
        cursor_col = 0;
        cursor_row++;
        if (cursor_row >= screen_rows) {
            bios_scroll_up(1, 0x07, 0, 0, screen_rows - 1, screen_cols - 1);
            cursor_row = screen_rows - 1;
        }
        bios_set_cursor(cursor_row, cursor_col);
    } else if (ch == '\r') {
        cursor_col = 0;
        bios_set_cursor(cursor_row, cursor_col);
    } else if (ch == '\b') {
        if (cursor_col > 0) cursor_col--;
        bios_set_cursor(cursor_row, cursor_col);
    } else {
        bios_write_char(ch, 0x07);
        cursor_col++;
        if (cursor_col >= screen_cols) {
            cursor_col = 0;
            cursor_row++;
            if (cursor_row >= screen_rows) {
                bios_scroll_up(1, 0x07, 0, 0, screen_rows - 1, screen_cols - 1);
                cursor_row = screen_rows - 1;
            }
        }
        bios_set_cursor(cursor_row, cursor_col);
    }
}

static void dos_puts(const char *s)
{
    while (*s)
        dos_putch(*s++);
}

static int dos_getch(void)
{
    /* INT 16h AH=00: wait for key, return AL=ASCII, AH=scan */
    union REGS r;
    r.h.ah = 0x00;
    int86(0x16, &r, &r);
    return r.h.al ? r.h.al : (0x100 | r.h.ah); /* extended key */
}

static bool dos_kbhit(void)
{
    return kbhit() != 0;
}

static void dos_locate(int row, int col)
{
    bios_set_cursor(row, col);
}

static int dos_get_cursor_row(void) { return cursor_row; }
static int dos_get_cursor_col(void) { return cursor_col; }

static void dos_cls(void)
{
    bios_scroll_up(0, 0x07, 0, 0, screen_rows - 1, screen_cols - 1);
    bios_set_cursor(0, 0);
}

static void dos_set_width(int cols)
{
    (void)cols;
}

/* DOS doesn't have raw/cooked mode distinction for console */
static void dos_enable_raw(void)  { }
static void dos_disable_raw(void) { }

static void dos_write_raw(const char *data, int len)
{
    /* On DOS, write directly to screen buffer for graphics.
     * For text, just output character by character. */
    for (int i = 0; i < len; i++)
        dos_putch(data[i]);
}

/* --- Lifecycle --- */

static void dos_init(void)
{
    /* Get current video mode to determine screen size */
    union REGS r;
    r.h.ah = 0x0F;  /* Get video mode */
    int86(0x10, &r, &r);
    screen_cols = r.h.ah;

    /* Get screen rows from BIOS data area */
    /* 0040:0084 = rows - 1 */
    screen_rows = *(uint8_t far *)MK_FP(0x0040, 0x0084) + 1;
    if (screen_rows < 25) screen_rows = 25;

    bios_get_cursor(&cursor_row, &cursor_col);
}

static void dos_shutdown(void)
{
    /* Nothing to clean up on DOS */
}

/* --- HAL vtable --- */

static hal_ops_t dos_hal = {
    .putch       = dos_putch,
    .puts        = dos_puts,
    .getch       = dos_getch,
    .kbhit       = dos_kbhit,
    .locate      = dos_locate,
    .get_cursor_row = dos_get_cursor_row,
    .get_cursor_col = dos_get_cursor_col,
    .cls         = dos_cls,
    .set_width   = dos_set_width,
    .enable_raw  = dos_enable_raw,
    .disable_raw = dos_disable_raw,
    .write_raw   = dos_write_raw,
    .screen_width  = 80,
    .screen_height = 25,
    .is_tty      = true,  /* DOS console is always a TTY */
    .init        = dos_init,
    .shutdown    = dos_shutdown,
};

hal_ops_t *hal_dos_create(void)
{
    return &dos_hal;
}

#endif /* __MSDOS__ */
