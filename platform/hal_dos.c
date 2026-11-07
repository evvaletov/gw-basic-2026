/*
 * DOS HAL backend for GW-BASIC 2026.
 *
 * Uses BIOS/DOS interrupts for terminal I/O, keyboard input,
 * and screen control.  Selected at compile time via __MSDOS__.
 * Linux HAL (hal_posix.c) is unchanged -- full backward compatibility.
 *
 * Build: wcc386 -bt=dos -mf -ox -za99 -D__MSDOS__ -Iinclude  (32-bit)
 *        wcc   -bt=dos -mm -ox -za99 -D__MSDOS__ -Iinclude  (16-bit)
 */

#ifdef __MSDOS__

#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <i86.h>
#include <dos.h>

/* int86 (16-bit real mode) vs int386 (32-bit protected mode) */
#ifdef _M_I86
#define INTX(n, r_in, r_out) int86(n, r_in, r_out)
#else
#define INTX(n, r_in, r_out) int386(n, r_in, r_out)
#endif

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
    INTX(0x10, &r, &r);
    cursor_row = row;
    cursor_col = col;
}

static void bios_get_cursor(int *row, int *col)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x03;
    r.h.bh = 0;
    INTX(0x10, &r, &r);
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
    INTX(0x10, &r, &r);
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
    INTX(0x10, &r, &r);
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
    INTX(0x16, &r, &r);
    return r.h.al ? r.h.al : (0x100 | r.h.ah);
}

static bool dos_kbhit(void)
{
    return kbhit() != 0;
}

/* DOS raises SIGINT during selected DOS calls, so a compute-only BASIC loop
 * needs its own keyboard poll.  Inspect the BIOS ring without reading ordinary
 * keys: even a queued letter or function key must not hide a later Ctrl-C. */
static unsigned bios_lock_keyboard(void);
static void bios_unlock_keyboard(unsigned flags);
#ifdef _M_I86
#pragma aux bios_lock_keyboard = "pushf" "pop ax" "cli" \
    __value [ax] __modify __exact [ax];
#pragma aux bios_unlock_keyboard = "push ax" "popf" \
    __parm [ax] __modify __exact [];
typedef volatile uint16_t __far *bios_words_t;
#else
#pragma aux bios_lock_keyboard = "mov ax, 0900h" "int 31h" "movzx eax, ax" \
    __value [eax] __modify __exact [eax];
/* DPMI virtual interrupt state is independent of PUSHFD/POPFD.  Function
 * 0900h returns AX=0900h or 0901h, which restores the old state when called. */
#pragma aux bios_unlock_keyboard = "int 31h" \
    __parm [eax] __modify __exact [eax];
typedef volatile uint16_t *bios_words_t;
#endif

static bool dos_check_break(void)
{
#ifdef _M_I86
    bios_words_t bda = (bios_words_t)MK_FP(0x40, 0);
#else
    /* DOS/4GW maps the first megabyte at its physical linear addresses. */
    bios_words_t bda = (bios_words_t)0x400;
#endif
    unsigned flags;
    uint16_t first, end, head, tail, read_pos, write_pos;
    bool ctrl_break, found;

    if (bda[0x1a / 2] == bda[0x1c / 2] && !(bda[0x70 / 2] & 0x8000))
        return false;

    flags = bios_lock_keyboard();
    first = bda[0x80 / 2];
    end = bda[0x82 / 2];
    /* Older BIOSes have the fixed 16-word keyboard ring. */
    if (first < 0x1e || first >= end || (first & 1) || (end & 1)) {
        first = 0x1e;
        end = 0x3e;
    }
    head = bda[0x1a / 2];
    tail = bda[0x1c / 2];
    ctrl_break = (bda[0x70 / 2] & 0x8000) != 0;
    found = ctrl_break;
    bda[0x70 / 2] &= (uint16_t)~0x8000;

    if (head >= first && head < end && tail >= first && tail < end &&
        !(head & 1) && !(tail & 1)) {
        write_pos = head;
        for (read_pos = head; read_pos != tail;) {
            uint16_t key = bda[read_pos / 2];
            if ((key & 0xff) == 3 || (ctrl_break && key == 0)) {
                found = true;
            } else {
                if (write_pos != read_pos)
                    bda[write_pos / 2] = key;
                write_pos += 2;
                if (write_pos == end) write_pos = first;
            }
            read_pos += 2;
            if (read_pos == end) read_pos = first;
        }
        bda[0x1c / 2] = write_pos;
    }
    bios_unlock_keyboard(flags);
    return found;
}

static void dos_locate(int row, int col)
{
    bios_set_cursor(row - 1, col - 1);
}

static int dos_get_cursor_row(void) { return cursor_row; }
static int dos_get_cursor_col(void) { return cursor_col; }

static void dos_cls(void)
{
    bios_scroll_up(0, 0x07, 0, 0, screen_rows - 1, screen_cols - 1);
    bios_set_cursor(0, 0);
}

static void dos_set_width(int cols) { (void)cols; }
static void dos_enable_raw(void)
{
    /* Console stdio input updates the BIOS cursor outside the HAL. */
    bios_get_cursor(&cursor_row, &cursor_col);
}
static void dos_disable_raw(void) { }

static void dos_write_raw(const char *data, int len)
{
    for (int i = 0; i < len; i++)
        dos_putch(data[i]);
}

static void dos_tui_enter(void)
{
    bios_scroll_up(0, 0x07, 0, 0, screen_rows - 1, screen_cols - 1);
    bios_set_cursor(0, 0);
}

static void dos_tui_leave(void)
{
    bios_scroll_up(0, 0x07, 0, 0, screen_rows - 1, screen_cols - 1);
    bios_set_cursor(0, 0);
}

static void dos_render_run(int row, int col,
                           const uint8_t *chars, const uint8_t *attrs, int len)
{
    /* INT 10h AH=09h writes char+attr at the cursor without advancing it. */
    for (int i = 0; i < len; i++) {
        bios_set_cursor(row, col + i);
        unsigned char ch = chars[i];
        bios_write_char(ch ? ch : ' ', attrs[i]);
    }
}

static void dos_set_cursor_shape(int shape)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x01;
    switch (shape) {
    case 0: r.h.ch = 0x20; r.h.cl = 0; break;  /* hide (high bit set) */
    case 1: r.h.ch = 0;    r.h.cl = 7; break;  /* block */
    case 2: r.h.ch = 6;    r.h.cl = 7; break;  /* underline */
    default: return;
    }
    INTX(0x10, &r, &r);
}

/* Forward-declared so dos_init can write back screen size before the struct
 * definition's positional initializers populate the rest. */
static hal_ops_t dos_hal;

static void dos_init(void)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.h.ah = 0x0F;
    INTX(0x10, &r, &r);
    screen_cols = r.h.ah;
    screen_rows = 25;  /* safe default; BIOS data area read needs far ptr */
    dos_hal.screen_width = screen_cols;
    dos_hal.screen_height = screen_rows;
    bios_get_cursor(&cursor_row, &cursor_col);
}

static void dos_shutdown(void) { }

static hal_ops_t dos_hal = {
    dos_putch, dos_puts, dos_getch, dos_kbhit,
    dos_locate, dos_get_cursor_row, dos_get_cursor_col,
    dos_cls, dos_set_width, dos_enable_raw, dos_disable_raw,
    dos_write_raw,
    dos_tui_enter, dos_tui_leave, dos_render_run, dos_set_cursor_shape,
    80, 25, 1 /* is_tty */,
    dos_init, dos_shutdown, dos_check_break
};

hal_ops_t *hal_dos_create(void) { return &dos_hal; }

#endif /* __MSDOS__ */
