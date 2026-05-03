#include "tui.h"
#include "hal.h"
#include "gwbasic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifndef __MSDOS__
#include <unistd.h>
#include <sys/ioctl.h>
#endif
#ifdef _M_I86
#include <malloc.h>  /* _fcalloc, _ffree */
#endif

tui_state_t tui;

/* Saved original HAL pointers for passthrough in non-TUI mode */
static void (*orig_putch)(int);
static void (*orig_puts)(const char *);
static void (*orig_cls)(void);
static void (*orig_locate)(int, int);
static int  (*orig_get_cursor_row)(void);
static int  (*orig_get_cursor_col)(void);

/* Default F-key definitions matching GW-BASIC */
static const char *default_fkeys[10] = {
    "LIST ",          /* F1 */
    "RUN\r",          /* F2 */
    "LOAD\"",         /* F3 */
    "SAVE\"",         /* F4 */
    "CONT\r",         /* F5 */
    ",\"LPT1:\"\r",   /* F6 */
    "TRON\r",         /* F7 */
    "TROFF\r",        /* F8 */
    "KEY ",           /* F9 */
    "SCREEN 0,0,0\r", /* F10 */
};

static void scroll_up(void)
{
    int bottom = tui.view_bottom;
#ifdef _M_I86
    _fmemmove(&TUI_CELL(0, 0), &TUI_CELL(1, 0),
              bottom * tui.cols * sizeof(tui_cell_t));
#else
    memmove(&TUI_CELL(0, 0), &TUI_CELL(1, 0),
            bottom * tui.cols * sizeof(tui_cell_t));
#endif
    for (int c = 0; c < tui.cols; c++) {
        TUI_CELL(bottom, c).ch = ' ';
        TUI_CELL(bottom, c).attr = tui.current_attr;
    }
}

static void advance_cursor(void)
{
    tui.cursor_col++;
    if (tui.cursor_col >= tui.cols) {
        tui.cursor_col = 0;
        tui.cursor_row++;
        if (tui.cursor_row > tui.view_bottom) {
            scroll_up();
            tui.cursor_row = tui.view_bottom;
        }
    }
}

void tui_putch(int ch)
{
    if (ch == '\n') {
        tui.cursor_col = 0;
        tui.cursor_row++;
        if (tui.cursor_row > tui.view_bottom) {
            scroll_up();
            tui.cursor_row = tui.view_bottom;
            tui_refresh();
        }
        return;
    }
    if (ch == '\r') {
        tui.cursor_col = 0;
        return;
    }
    if (ch == '\b') {
        if (tui.cursor_col > 0) {
            tui.cursor_col--;
            TUI_CELL(tui.cursor_row, tui.cursor_col).ch = ' ';
            TUI_CELL(tui.cursor_row, tui.cursor_col).attr = tui.current_attr;
        }
        return;
    }
    if (ch == '\a') return;
    if (ch == '\t') {
        int target = (tui.cursor_col + 8) & ~7;
        while (tui.cursor_col < target && tui.cursor_col < tui.cols)
            tui_putch(' ');
        return;
    }

    TUI_CELL(tui.cursor_row, tui.cursor_col).ch = (uint8_t)ch;
    TUI_CELL(tui.cursor_row, tui.cursor_col).attr = tui.current_attr;
    advance_cursor();
}

void tui_puts(const char *s)
{
    while (*s)
        tui_putch((unsigned char)*s++);
    tui_refresh();
    tui_update_cursor();
}

void tui_cls(void)
{
    for (int r = 0; r < tui.rows; r++)
        for (int c = 0; c < tui.cols; c++) {
            TUI_CELL(r, c).ch = ' ';
            TUI_CELL(r, c).attr = tui.current_attr;
        }
    tui.cursor_row = 0;
    tui.cursor_col = 0;
    tui_refresh();
    tui_update_cursor();
}

void tui_locate(int row, int col)
{
    tui.cursor_row = row - 1;
    tui.cursor_col = col - 1;
    if (tui.cursor_row < 0) tui.cursor_row = 0;
    if (tui.cursor_col < 0) tui.cursor_col = 0;
    if (tui.cursor_row >= tui.rows) tui.cursor_row = tui.rows - 1;
    if (tui.cursor_col >= tui.cols) tui.cursor_col = tui.cols - 1;
    tui_update_cursor();
}

int tui_get_cursor_row(void) { return tui.cursor_row; }
int tui_get_cursor_col(void) { return tui.cursor_col; }

/* Row-sized scratch buffers for HAL render_run.  Sized to TUI_MAX_COLS
 * so they fit in the near data segment on 16-bit DOS while the screen
 * cell buffer itself lives on the far heap. */
static uint8_t row_chars[TUI_MAX_COLS];
static uint8_t row_attrs[TUI_MAX_COLS];

void tui_refresh(void)
{
    int bottom = tui.key_bar_visible ? tui.rows : tui.view_bottom + 1;
    int width = tui.cols < TUI_MAX_COLS ? tui.cols : TUI_MAX_COLS;
    for (int r = 0; r < bottom; r++) {
        for (int c = 0; c < width; c++) {
            row_chars[c] = TUI_CELL(r, c).ch;
            row_attrs[c] = TUI_CELL(r, c).attr;
        }
        gw_hal->render_run(r, 0, row_chars, row_attrs, width);
    }

    if (tui.key_bar_visible)
        tui_refresh_row(tui.rows - 1);
}

void tui_refresh_row(int row)
{
    if (row < 0 || row >= tui.rows) return;
    int width = tui.cols < TUI_MAX_COLS ? tui.cols : TUI_MAX_COLS;
    for (int c = 0; c < width; c++) {
        row_chars[c] = TUI_CELL(row, c).ch;
        row_attrs[c] = TUI_CELL(row, c).attr;
    }
    gw_hal->render_run(row, 0, row_chars, row_attrs, width);
}

void tui_update_cursor(void)
{
    /* Use the platform's original locate (saved before tui_init swapped its
     * own in) — gw_hal->locate now points at tui_locate, which would recurse. */
    if (orig_locate)
        orig_locate(tui.cursor_row + 1, tui.cursor_col + 1);
}

int tui_read_key(void)
{
    /* Drain key buffer first (keys pushed back by event trapping) */
    int buffered = tui_pop_key();
    if (buffered >= 0)
        return buffered;

    gw_hal->enable_raw();

    int ch = gw_hal->getch();
    if (ch < 0) return -1;

    if (ch == 3) return TK_CTRL_C;

#ifdef __MSDOS__
    /* DOS HAL encodes extended keys as 0x100 | BIOS scan code (INT 16h, AH=00h
     * returns AL=0, AH=scancode).  Translate to internal TK_* codes. */
    if (ch & 0x100) {
        switch (ch & 0xFF) {
        case 0x3B: return TK_F1;
        case 0x3C: return TK_F2;
        case 0x3D: return TK_F3;
        case 0x3E: return TK_F4;
        case 0x3F: return TK_F5;
        case 0x40: return TK_F6;
        case 0x41: return TK_F7;
        case 0x42: return TK_F8;
        case 0x43: return TK_F9;
        case 0x44: return TK_F10;
        case 0x47: return TK_HOME;
        case 0x48: return TK_UP;
        case 0x49: return TK_PGUP;
        case 0x4B: return TK_LEFT;
        case 0x4D: return TK_RIGHT;
        case 0x4F: return TK_END;
        case 0x50: return TK_DOWN;
        case 0x51: return TK_PGDN;
        case 0x52: return TK_INSERT;
        case 0x53: return TK_DELETE;
        case 0x77: return TK_CTRL_HOME;
        default:   return ch;
        }
    }
#endif

    if (ch != 27) return ch;

    if (!gw_hal->kbhit())
        return TK_ESCAPE;

    int seq1 = gw_hal->getch();
    if (seq1 < 0) return TK_ESCAPE;

    if (seq1 == '[') {
        int seq2 = gw_hal->getch();
        if (seq2 < 0) return TK_ESCAPE;

        switch (seq2) {
        case 'A': return TK_UP;
        case 'B': return TK_DOWN;
        case 'C': return TK_RIGHT;
        case 'D': return TK_LEFT;
        case 'H': return TK_HOME;
        case 'F': return TK_END;
        default:
            if (seq2 >= '0' && seq2 <= '9') {
                int seq3 = gw_hal->getch();
                if (seq3 == '~') {
                    switch (seq2) {
                    case '2': return TK_INSERT;
                    case '3': return TK_DELETE;
                    case '5': return TK_PGUP;
                    case '6': return TK_PGDN;
                    }
                } else if (seq2 == '1' && seq3 >= '0' && seq3 <= '9') {
                    int seq4 = gw_hal->getch();
                    if (seq4 == '~') {
                        int code = (seq2 - '0') * 10 + (seq3 - '0');
                        switch (code) {
                        case 15: return TK_F5;
                        case 17: return TK_F6;
                        case 18: return TK_F7;
                        case 19: return TK_F8;
                        case 20: return TK_F9;
                        case 21: return TK_F10;
                        }
                    }
                } else if (seq2 == '2' && seq3 >= '0' && seq3 <= '9') {
                    int seq4 = gw_hal->getch();
                    (void)seq4;
                }
            }
            break;
        }
    } else if (seq1 == 'O') {
        int seq2 = gw_hal->getch();
        switch (seq2) {
        case 'P': return TK_F1;
        case 'Q': return TK_F2;
        case 'R': return TK_F3;
        case 'S': return TK_F4;
        case 'H': return TK_HOME;
        case 'F': return TK_END;
        }
    }

    return TK_ESCAPE;
}

static int extract_screen_line(int row, char *buf, int bufsz)
{
    int len = 0;
    for (int c = 0; c < tui.cols && len < bufsz - 1; c++) {
        buf[len++] = TUI_CELL(row, c).ch ? TUI_CELL(row, c).ch : ' ';
    }
    while (len > 0 && buf[len - 1] == ' ')
        len--;
    buf[len] = '\0';
    return len;
}

char *tui_read_line(void)
{
    static char line_buf[TUI_MAX_LINE + 1];
    int enter_row = tui.cursor_row;

    tui_update_cursor();

    for (;;) {
        int key = tui_read_key();
        if (key < 0) return NULL;

        if (key == TK_CTRL_C || tui.break_flag) {
            tui.break_flag = false;
            line_buf[0] = '\0';
            return NULL;
        }

        if (key >= TK_F1 && key <= TK_F10) {
            int fk = key - TK_F1;
            const char *def = tui.fkey_defs[fk];
            for (const char *p = def; *p; p++) {
                if (*p == '\r') {
                    tui_putch('\n');
                    tui_refresh();
                    extract_screen_line(enter_row, line_buf, sizeof(line_buf));
                    tui_update_cursor();
                    return line_buf;
                }
                TUI_CELL(tui.cursor_row, tui.cursor_col).ch = (uint8_t)*p;
                TUI_CELL(tui.cursor_row, tui.cursor_col).attr = tui.current_attr;
                advance_cursor();
            }
            tui_refresh_row(tui.cursor_row);
            tui_update_cursor();
            continue;
        }

        switch (key) {
        case TK_ENTER:
            extract_screen_line(enter_row, line_buf, sizeof(line_buf));
            tui.cursor_col = 0;
            tui.cursor_row++;
            if (tui.cursor_row > tui.view_bottom) {
                scroll_up();
                tui.cursor_row = tui.view_bottom;
            }
            tui_refresh();
            tui_update_cursor();
            return line_buf;

        case TK_BACKSPACE:
        case 127:
            if (tui.cursor_col > 0) {
                tui.cursor_col--;
                for (int c = tui.cursor_col; c < tui.cols - 1; c++)
                    TUI_CELL(tui.cursor_row, c) = TUI_CELL(tui.cursor_row, c + 1);
                TUI_CELL(tui.cursor_row, tui.cols - 1).ch = ' ';
                TUI_CELL(tui.cursor_row, tui.cols - 1).attr = tui.current_attr;
                tui_refresh_row(tui.cursor_row);
            }
            tui_update_cursor();
            break;

        case TK_DELETE:
            for (int c = tui.cursor_col; c < tui.cols - 1; c++)
                TUI_CELL(tui.cursor_row, c) = TUI_CELL(tui.cursor_row, c + 1);
            TUI_CELL(tui.cursor_row, tui.cols - 1).ch = ' ';
            TUI_CELL(tui.cursor_row, tui.cols - 1).attr = tui.current_attr;
            tui_refresh_row(tui.cursor_row);
            tui_update_cursor();
            break;

        case TK_LEFT:
            if (tui.cursor_col > 0)
                tui.cursor_col--;
            else if (tui.cursor_row > 0) {
                tui.cursor_row--;
                tui.cursor_col = tui.cols - 1;
            }
            tui_update_cursor();
            break;

        case TK_RIGHT:
            if (tui.cursor_col < tui.cols - 1)
                tui.cursor_col++;
            else if (tui.cursor_row < tui.view_bottom) {
                tui.cursor_row++;
                tui.cursor_col = 0;
            }
            tui_update_cursor();
            break;

        case TK_UP:
            if (tui.cursor_row > 0)
                tui.cursor_row--;
            tui_update_cursor();
            break;

        case TK_DOWN:
            if (tui.cursor_row < tui.view_bottom)
                tui.cursor_row++;
            tui_update_cursor();
            break;

        case TK_HOME:
            tui.cursor_col = 0;
            tui_update_cursor();
            break;

        case TK_END: {
            int c = tui.cols - 1;
            while (c > 0 && TUI_CELL(tui.cursor_row, c).ch == ' ')
                c--;
            if (TUI_CELL(tui.cursor_row, c).ch != ' ')
                c++;
            if (c >= tui.cols) c = tui.cols - 1;
            tui.cursor_col = c;
            tui_update_cursor();
            break;
        }

        case TK_CTRL_HOME:
            tui_cls();
            enter_row = 0;
            break;

        case TK_INSERT:
            tui.insert_mode = !tui.insert_mode;
            if (tui.insert_mode)
                tui_set_cursor_block();
            else
                tui_set_cursor_line();
            break;

        case TK_ESCAPE:
            for (int c = tui.cursor_col; c < tui.cols; c++) {
                TUI_CELL(tui.cursor_row, c).ch = ' ';
                TUI_CELL(tui.cursor_row, c).attr = tui.current_attr;
            }
            tui_refresh_row(tui.cursor_row);
            tui_update_cursor();
            break;

        default:
            if (key >= 32 && key < 127) {
                if (tui.insert_mode && tui.cursor_col < tui.cols - 1) {
                    for (int c = tui.cols - 1; c > tui.cursor_col; c--)
                        TUI_CELL(tui.cursor_row, c) = TUI_CELL(tui.cursor_row, c - 1);
                }
                TUI_CELL(tui.cursor_row, tui.cursor_col).ch = (uint8_t)key;
                TUI_CELL(tui.cursor_row, tui.cursor_col).attr = tui.current_attr;
                if (tui.cursor_row != enter_row && tui.cursor_col == 0)
                    enter_row = tui.cursor_row;
                advance_cursor();
                tui_refresh_row(tui.cursor_row);
                tui_update_cursor();
            }
            break;
        }
    }
}

static void render_key_bar(void)
{
    int row = tui.rows - 1;
    for (int c = 0; c < tui.cols; c++) {
        TUI_CELL(row, c).ch = ' ';
        TUI_CELL(row, c).attr = 0x70;
    }

    int col = 0;
    for (int i = 0; i < 10 && col < tui.cols; i++) {
        char label[4];
        snprintf(label, sizeof(label), "%d", i + 1);
        for (const char *p = label; *p && col < tui.cols; p++) {
            TUI_CELL(row, col).ch = *p;
            TUI_CELL(row, col).attr = 0x07;
            col++;
        }
        const char *def = tui.fkey_defs[i];
        int maxw = 6;
        for (int j = 0; j < maxw && def[j] && def[j] != '\r' && col < tui.cols; j++) {
            TUI_CELL(row, col).ch = (uint8_t)def[j];
            TUI_CELL(row, col).attr = 0x70;
            col++;
        }
        if (col < tui.cols) {
            TUI_CELL(row, col).ch = ' ';
            TUI_CELL(row, col).attr = 0x07;
            col++;
        }
    }

    tui_refresh_row(row);
}

void tui_key_on(void)
{
    if (!tui.screen) return;
    tui.key_bar_visible = true;
    tui.view_bottom = tui.rows - 2;
    render_key_bar();
    tui_update_cursor();
}

void tui_key_off(void)
{
    if (!tui.screen) return;
    tui.key_bar_visible = false;
    tui.view_bottom = tui.rows - 1;
    for (int c = 0; c < tui.cols; c++) {
        TUI_CELL(tui.rows - 1, c).ch = ' ';
        TUI_CELL(tui.rows - 1, c).attr = tui.current_attr;
    }
    tui_refresh_row(tui.rows - 1);
    tui_update_cursor();
}

void tui_key_list(void)
{
    if (!tui.screen) return;
    for (int i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "F%d ", i + 1);
        tui_puts(buf);
        for (const char *p = tui.fkey_defs[i]; *p; p++) {
            if (*p == '\r')
                tui_putch(0x0D);
            else
                tui_putch(*p);
        }
        tui_putch('\n');
    }
    tui_refresh();
    tui_update_cursor();
}

/* Key buffer ring buffer operations */
void tui_push_key(int key)
{
    int next = (tui.keybuf_head + 1) % TUI_KEYBUF_SIZE;
    if (next == tui.keybuf_tail)
        return;  /* buffer full, drop key */
    tui.keybuf[tui.keybuf_head] = key;
    tui.keybuf_head = next;
}

int tui_pop_key(void)
{
    if (tui.keybuf_head == tui.keybuf_tail)
        return -1;
    int key = tui.keybuf[tui.keybuf_tail];
    tui.keybuf_tail = (tui.keybuf_tail + 1) % TUI_KEYBUF_SIZE;
    return key;
}

bool tui_keybuf_empty(void)
{
    return tui.keybuf_head == tui.keybuf_tail;
}

int tui_key_to_scancode(int key)
{
    /* Map internal TK_* key codes to IBM PC scan codes for INKEY$.
       Returns -1 for regular ASCII keys (no scan code). */
    switch (key) {
    case TK_F1:     return 59;
    case TK_F2:     return 60;
    case TK_F3:     return 61;
    case TK_F4:     return 62;
    case TK_F5:     return 63;
    case TK_F6:     return 64;
    case TK_F7:     return 65;
    case TK_F8:     return 66;
    case TK_F9:     return 67;
    case TK_F10:    return 68;
    case TK_HOME:   return 71;
    case TK_UP:     return 72;
    case TK_PGUP:   return 73;
    case TK_LEFT:   return 75;
    case TK_RIGHT:  return 77;
    case TK_END:    return 79;
    case TK_DOWN:   return 80;
    case TK_PGDN:   return 81;
    case TK_INSERT: return 82;
    case TK_DELETE: return 83;
    default:        return -1;
    }
}

void tui_edit_line(const char *prefill)
{
    /* Advance to new line */
    tui.cursor_col = 0;
    tui.cursor_row++;
    if (tui.cursor_row > tui.view_bottom) {
        scroll_up();
        tui.cursor_row = tui.view_bottom;
    }

    /* Write prefill text into screen buffer */
    for (int i = 0; prefill[i] && i < tui.cols; i++) {
        TUI_CELL(tui.cursor_row, i).ch = (uint8_t)prefill[i];
        TUI_CELL(tui.cursor_row, i).attr = tui.current_attr;
    }

    tui_refresh_row(tui.cursor_row);
    tui_update_cursor();

    char *result = tui_read_line();
    if (result && result[0])
        gw_exec_direct(result);
}

void tui_set_cursor_block(void)
{
    if (gw_hal->set_cursor_shape)
        gw_hal->set_cursor_shape(1);
}

void tui_set_cursor_line(void)
{
    if (gw_hal->set_cursor_shape)
        gw_hal->set_cursor_shape(2);
}

static void sigint_handler(int sig)
{
    (void)sig;
    tui.break_flag = true;
}

void tui_install_break_handler(void)
{
#ifdef __MSDOS__
    signal(SIGINT, sigint_handler);
#else
    struct sigaction sa;
    sa.sa_handler = sigint_handler;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
#endif
}

void tui_check_break(void)
{
    if (tui.break_flag) {
        tui.break_flag = false;
        tui_puts("\nBreak\n");
        gw.running = false;
        longjmp(gw_error_jmp, -1);
    }
}

void tui_init(bool fullscreen)
{
    memset(&tui, 0, sizeof(tui));
    tui.current_attr = 0x07;
    tui.insert_mode = false;
    tui.active = true;

    /* Determine screen dimensions */
    tui.rows = TUI_DEFAULT_ROWS;
    tui.cols = TUI_DEFAULT_COLS;
    if (fullscreen) {
#ifndef __MSDOS__
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
            tui.rows = ws.ws_row;
            tui.cols = ws.ws_col;
            if (tui.rows > TUI_MAX_ROWS) tui.rows = TUI_MAX_ROWS;
            if (tui.cols > TUI_MAX_COLS) tui.cols = TUI_MAX_COLS;
        }
#else
        tui.rows = gw_hal ? gw_hal->screen_height : TUI_DEFAULT_ROWS;
        tui.cols = gw_hal ? gw_hal->screen_width : TUI_DEFAULT_COLS;
#endif
    }

    tui.view_bottom = tui.rows - 1;

    /* Allocate screen buffer */
    /* On 16-bit DOS, use far heap to avoid exhausting the 64KB data segment */
#ifdef _M_I86
    tui.screen = _fcalloc(tui.rows * tui.cols, sizeof(tui_cell_t));
#else
    tui.screen = calloc(tui.rows * tui.cols, sizeof(tui_cell_t));
#endif
    if (!tui.screen) {
        tui.active = false;
        return;
    }

    /* Set default F-key definitions */
    for (int i = 0; i < 10; i++)
        strncpy(tui.fkey_defs[i], default_fkeys[i], sizeof(tui.fkey_defs[i]) - 1);

    /* Clear screen buffer */
    for (int r = 0; r < tui.rows; r++)
        for (int c = 0; c < tui.cols; c++) {
            TUI_CELL(r, c).ch = ' ';
            TUI_CELL(r, c).attr = tui.current_attr;
        }

    /* Save original HAL pointers */
    orig_putch = gw_hal->putch;
    orig_puts = gw_hal->puts;
    orig_cls = gw_hal->cls;
    orig_locate = gw_hal->locate;
    orig_get_cursor_row = gw_hal->get_cursor_row;
    orig_get_cursor_col = gw_hal->get_cursor_col;

    /* Swap in TUI handlers */
    gw_hal->putch = tui_putch;
    gw_hal->puts = tui_puts;
    gw_hal->cls = tui_cls;
    gw_hal->locate = tui_locate;
    gw_hal->get_cursor_row = tui_get_cursor_row;
    gw_hal->get_cursor_col = tui_get_cursor_col;

    /* Install break handler */
    tui_install_break_handler();

    /* Enter TUI mode (alt screen on POSIX, clear on DOS) */
    if (gw_hal->tui_enter)
        gw_hal->tui_enter();
    tui_set_cursor_line();

    /* Show KEY bar by default (matches real GW-BASIC) */
    tui_key_on();
}

void tui_shutdown(void)
{
    if (!tui.active) return;
    tui.active = false;

    /* Restore original HAL pointers */
    gw_hal->putch = orig_putch;
    gw_hal->puts = orig_puts;
    gw_hal->cls = orig_cls;
    gw_hal->locate = orig_locate;
    gw_hal->get_cursor_row = orig_get_cursor_row;
    gw_hal->get_cursor_col = orig_get_cursor_col;

    /* Leave TUI mode (alt screen restore on POSIX, clear on DOS) */
    if (gw_hal->tui_leave)
        gw_hal->tui_leave();

#ifdef _M_I86
    _ffree(tui.screen);
#else
    free(tui.screen);
#endif
    tui.screen = NULL;

    signal(SIGINT, SIG_DFL);
}
