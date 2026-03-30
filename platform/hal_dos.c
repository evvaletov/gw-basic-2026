/*
 * DOS HAL backend for GW-BASIC 2026.
 *
 * Uses BIOS/DOS interrupts for terminal I/O, keyboard input,
 * and screen control.  Selected at compile time via __MSDOS__.
 * Linux HAL (hal_posix.c) is unchanged -- full backward compatibility.
 *
 * Build: wcc386 -bt=dos -mf -ox -za99 -D__MSDOS__ -Iinclude
 */

#ifdef __MSDOS__

#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <i86.h>
#include <dos.h>

static int cursor_row = 0;
static int cursor_col = 0;
static int screen_cols = 80;
static int screen_rows = 25;

/* --- BIOS video services (INT 10h) --- */

static void bios_set_cursor(int row, int col)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x02;
    r.h.bh = 0;
    r.h.dh = (unsigned char)row;
    r.h.dl = (unsigned char)col;
    int386(0x10, &r, &r);
    cursor_row = row;
    cursor_col = col;
}

static void bios_get_cursor(int *row, int *col)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x03;
    r.h.bh = 0;
    int386(0x10, &r, &r);
    *row = r.h.dh;
    *col = r.h.dl;
}

static void bios_scroll_up(int lines, int attr, int r1, int c1, int r2, int c2)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x06;
    r.h.al = (unsigned char)lines;
    r.h.bh = (unsigned char)attr;
    r.h.ch = (unsigned char)r1;
    r.h.cl = (unsigned char)c1;
    r.h.dh = (unsigned char)r2;
    r.h.dl = (unsigned char)c2;
    int386(0x10, &r, &r);
}

static void bios_write_char(int ch, int attr)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x09;
    r.h.al = (unsigned char)ch;
    r.h.bh = 0;
    r.h.bl = (unsigned char)attr;
    r.w.cx = 1;
    int386(0x10, &r, &r);
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
    while (*s) dos_putch(*s++);
}

static int dos_getch(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x00;
    int386(0x16, &r, &r);
    return r.h.al ? r.h.al : (0x100 | r.h.ah);
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

static void dos_set_width(int cols) { (void)cols; }
static void dos_enable_raw(void)  { }
static void dos_disable_raw(void) { }

static void dos_write_raw(const char *data, int len)
{
    for (int i = 0; i < len; i++)
        dos_putch(data[i]);
}

static void dos_init(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x0F;
    int386(0x10, &r, &r);
    screen_cols = r.h.ah;
    screen_rows = 25;  /* safe default; BIOS data area read needs far ptr */
    bios_get_cursor(&cursor_row, &cursor_col);
}

static void dos_shutdown(void) { }

static hal_ops_t dos_hal = {
    dos_putch, dos_puts, dos_getch, dos_kbhit,
    dos_locate, dos_get_cursor_row, dos_get_cursor_col,
    dos_cls, dos_set_width, dos_enable_raw, dos_disable_raw,
    dos_write_raw, 80, 25, 1 /* is_tty */,
    dos_init, dos_shutdown
};

hal_ops_t *hal_dos_create(void) { return &dos_hal; }

#endif /* __MSDOS__ */
