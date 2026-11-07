#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <poll.h>

static struct termios orig_termios;
static int raw_mode = 0;
static int termios_saved = 0;
static int cursor_row = 0;
static int cursor_col = 0;
static int pending_char = -1;
static int terminal_cols = 80, terminal_rows = 25;
static bool output_tty;

static void posix_putch(int ch)
{
    putchar(ch);
    if (ch == '\n') {
        cursor_row++;
        cursor_col = 0;
    } else if (ch == '\r') {
        cursor_col = 0;
    } else if (ch == '\b') {
        if (cursor_col) cursor_col--;
    } else if (ch == '\t') {
        cursor_col = (cursor_col / 8 + 1) * 8;
        if (output_tty && cursor_col >= terminal_cols) cursor_col = terminal_cols - 1;
    } else if (ch >= 32) {
        cursor_col++;
        if (output_tty && cursor_col >= terminal_cols) {
            /* Resolve ANSI's deferred wrap so following cursor moves and
             * BASIC POS/CSRLIN agree with the screen immediately. */
            fputs("\r\n", stdout);
            cursor_col = 0;
            cursor_row++;
        }
    }
    if (output_tty && cursor_row >= terminal_rows) cursor_row = terminal_rows - 1;
}

static void posix_puts(const char *s)
{
    while (*s)
        posix_putch(*s++);
}

static int posix_getch(void)
{
    fflush(stdout);
    if (pending_char >= 0) {
        int ch = pending_char;
        pending_char = -1;
        return ch;
    }
    if (raw_mode) {
        /* Set VMIN=1 to block until a byte arrives */
        struct termios t;
        tcgetattr(STDIN_FILENO, &t);
        t.c_cc[VMIN] = 1;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        unsigned char ch;
        read(STDIN_FILENO, &ch, 1);
        t.c_cc[VMIN] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        return ch;
    }
    return getchar();
}

static bool posix_kbhit(void)
{
    if (pending_char >= 0)
        return true;
    if (!raw_mode)
        return false;
    unsigned char ch;
    ssize_t n = read(STDIN_FILENO, &ch, 1);
    if (n == 1) {
        pending_char = ch;
        return true;
    }
    return false;
}

static void posix_locate(int row, int col)
{
    printf("\033[%d;%dH", row, col);
    fflush(stdout);
    cursor_row = row - 1;
    cursor_col = col - 1;
}

static int posix_get_cursor_row(void) { return cursor_row; }
static int posix_get_cursor_col(void) { return cursor_col; }

static void posix_cls(void)
{
    printf("\033[2J\033[H");
    fflush(stdout);
    cursor_row = 0;
    cursor_col = 0;
}

static void posix_set_width(int cols)
{
    (void)cols;
}

static void posix_enable_raw(void)
{
    if (raw_mode || !isatty(STDIN_FILENO))
        return;
    if (!termios_saved) {
        tcgetattr(STDIN_FILENO, &orig_termios);
        termios_saved = 1;
    }
    struct termios raw = orig_termios;
    cfmakeraw(&raw);
    raw.c_oflag |= OPOST;  /* keep output processing (newline translation) */
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode = 1;
}

static void posix_disable_raw(void)
{
    if (!raw_mode)
        return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    raw_mode = 0;
}

static int input_byte(int timeout)
{
    if (pending_char >= 0) {
        int ch = pending_char;
        pending_char = -1;
        return ch;
    }
    struct pollfd fd = {STDIN_FILENO, POLLIN, 0};
    int ready;
    do { ready = poll(&fd, 1, timeout); } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return -1;
    unsigned char ch;
    ssize_t count;
    do { count = read(STDIN_FILENO, &ch, 1); } while (count < 0 && errno == EINTR);
    return count == 1 ? ch : -1;
}

/* Decode terminal keys for both echoing line input and INPUT$. Ordinary
 * bytes remain unchanged; supported extended keys use IBM PC scan codes. */
static int input_key(void)
{
    int key = input_byte(-1);
    if (key != 27) return key;
    int lead = input_byte(100);
    if (lead < 0) return 27;
    if (lead != '[' && lead != 'O') { pending_char = lead; return 27; }
    int number = 0, ch;
    bool first = true;
    do {
        ch = input_byte(100);
        if (ch == 3) return 3;
        if (first && ch >= '0' && ch <= '9') {
            if (number < 100) number = number * 10 + ch - '0';
        } else first = false;
    } while (ch >= 0 && (ch < 0x40 || ch > 0x7e));
    int scan = ch == 'A' ? 72 : ch == 'B' ? 80 : ch == 'C' ? 77 :
               ch == 'D' ? 75 : ch == 'H' ? 71 : ch == 'F' ? 79 :
               ch >= 'P' && ch <= 'S' ? 59 + ch - 'P' : 0;
    if (ch == '~') {
        static const int scans[] = {0,71,82,83,79,73,81,71,79,0,0,59,60,61,62,63,0,64,65,66,67,68};
        if (number < (int)(sizeof(scans) / sizeof(scans[0]))) scan = scans[number];
    }
    return scan ? 256 + scan : -4;
}

/* Move within the answer using relative rows, preserving the surrounding
 * terminal and its scrollback. No alternate screen or absolute origin. */
static void input_move(int from, int to, int origin_col)
{
    int rows = (origin_col + to) / terminal_cols - (origin_col + from) / terminal_cols;
    if (rows) printf("\033[%d%c", abs(rows), rows < 0 ? 'A' : 'B');
    cursor_row += rows;
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_row >= terminal_rows) cursor_row = terminal_rows - 1;
    cursor_col = (origin_col + to) % terminal_cols;
    putchar('\r');
    if (cursor_col) printf("\033[%dC", cursor_col);
}

static int posix_read_chars(char *buf, int count)
{
    struct termios saved, raw;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &saved) < 0) return -3;
    raw = saved;
    cfmakeraw(&raw);
    raw.c_oflag = saved.c_oflag;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) return -3;
    int n = 0;
    while (n < count) {
        int ch = input_key();
        if (ch == -4) continue;
        if (ch < 0) break;
        if (ch == 3) { n = -2; break; }
        if (ch > 255) continue;  /* Original INPUT$ filters extended keys. */
        buf[n++] = (char)ch;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return n;
}

static int posix_read_input(char *buf, int capacity, bool suppress_newline)
{
    struct termios saved, raw;
    if (!output_tty || capacity < 1 || tcgetattr(STDIN_FILENO, &saved) != 0) return -3;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col) terminal_cols = ws.ws_col;
        if (ws.ws_row) terminal_rows = ws.ws_row;
    }
    raw = saved;
    cfmakeraw(&raw);
    raw.c_oflag = saved.c_oflag;  /* retain the caller's newline processing */
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -3;
    int len = 0, pos = 0, drawn = 0, shown = 0, result = -1;
    int origin_col = cursor_col;
    bool insert = false;
    buf[0] = '\0';
    for (;;) {
        int key = input_key();
        if (key == -4) continue;
        if (key < 0 || (key == 4 && !len)) break;
        if (key == 3) { result = -2; break; }
        if (key == '\r' || key == '\n' || key == 4) {
            if (!suppress_newline) {
                input_move(shown, len, origin_col);
                posix_putch('\n');
            }
            result = len;
            break;
        }
        if (key == 27) key = 21;  /* Escape clears the answer. */
        else if (key > 255) {
            key = key == 331 ? 256 : key == 333 ? 257 :
                  key == 327 ? 258 : key == 335 ? 259 :
                  key == 338 ? 260 : key == 339 ? 261 : 0;
        }
        if (key == 256) { if (pos) pos--; }
        else if (key == 257) { if (pos < len) pos++; }
        else if (key == 258 || key == 1) pos = 0;
        else if (key == 259 || key == 5) pos = len;
        else if (key == 260) insert = !insert;
        else if (key == 21) { pos = len = 0; buf[0] = '\0'; }
        else if (key == 8 || key == 127) {
            if (pos) { memmove(buf + pos - 1, buf + pos, len - pos + 1); pos--; len--; }
        } else if (key == 261) {
            if (pos < len) { memmove(buf + pos, buf + pos + 1, len - pos); len--; }
        } else if (key >= 32 && key <= 255) {
            if (pos < len && !insert) buf[pos++] = (char)key;
            else if (len < capacity - 1) {
                memmove(buf + pos + 1, buf + pos, len - pos + 1);
                buf[pos++] = (char)key;
                len++;
            }
        }
        input_move(shown, 0, origin_col);
        int extent = len > drawn ? len : drawn;
        for (int i = 0; i < extent; i++) posix_putch(i < len ? (unsigned char)buf[i] : ' ');
        input_move(extent, pos, origin_col);
        drawn = len;
        shown = pos;
        fflush(stdout);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    return result;
}

static void posix_write_raw(const char *data, int len)
{
    fflush(stdout);
    write(STDOUT_FILENO, data, len);
}

static const int ansi_fg[16] = {30,34,32,36,31,35,33,37,90,94,92,96,91,95,93,97};
static const int ansi_bg[8]  = {40,44,42,46,41,45,43,47};

static void posix_tui_enter(void)
{
    printf("\033[?1049h\033[2J\033[H");
    fflush(stdout);
}

static void posix_tui_leave(void)
{
    printf("\033[?1049l\033[0 q\033[?25h");
    fflush(stdout);
}

static void posix_render_run(int row, int col,
                             const uint8_t *chars, const uint8_t *attrs, int len)
{
    printf("\033[%d;%dH", row + 1, col + 1);
    int prev_attr = -1;
    for (int i = 0; i < len; i++) {
        int a = attrs[i];
        if (a != prev_attr) {
            printf("\033[%d;%dm", ansi_fg[a & 0x0F], ansi_bg[(a >> 4) & 0x07]);
            prev_attr = a;
        }
        unsigned char ch = chars[i];
        putchar(ch ? ch : ' ');
    }
    printf("\033[0m");
    fflush(stdout);
}

static void posix_set_cursor_shape(int shape)
{
    switch (shape) {
    case 0: printf("\033[?25l"); break;
    case 1: printf("\033[?25h\033[1 q"); break;
    case 2: printf("\033[?25h\033[5 q"); break;
    }
    fflush(stdout);
}

static void posix_init(void)
{
    setbuf(stdout, NULL);
    /* INPUT$ can use getchar before the terminal editor reads the descriptor.
     * Do not let stdio prefetch and hide queued terminal bytes from that path. */
    if (isatty(STDIN_FILENO)) setbuf(stdin, NULL);
}

static void posix_shutdown(void)
{
    if (raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode = 0;
    }
}

static hal_ops_t posix_hal = {
    .putch = posix_putch,
    .puts = posix_puts,
    .getch = posix_getch,
    .kbhit = posix_kbhit,
    .locate = posix_locate,
    .get_cursor_row = posix_get_cursor_row,
    .get_cursor_col = posix_get_cursor_col,
    .cls = posix_cls,
    .set_width = posix_set_width,
    .enable_raw = posix_enable_raw,
    .disable_raw = posix_disable_raw,
    .write_raw = posix_write_raw,
    .tui_enter = posix_tui_enter,
    .tui_leave = posix_tui_leave,
    .render_run = posix_render_run,
    .set_cursor_shape = posix_set_cursor_shape,
    .screen_width = 80,
    .screen_height = 25,
    .is_tty = false,
    .init = posix_init,
    .shutdown = posix_shutdown,
    .read_input = posix_read_input,
    .read_chars = posix_read_chars,
};

hal_ops_t *hal_posix_create(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) posix_hal.screen_width = ws.ws_col;
        if (ws.ws_row > 0) posix_hal.screen_height = ws.ws_row;
    }
    posix_hal.is_tty = isatty(STDIN_FILENO);
    output_tty = isatty(STDOUT_FILENO);
    terminal_cols = posix_hal.screen_width;
    terminal_rows = posix_hal.screen_height;
    return &posix_hal;
}
