#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

static struct termios orig_termios;
static int raw_mode = 0;
static int termios_saved = 0;
static int cursor_row = 0;
static int cursor_col = 0;
static int pending_char = -1;

static void posix_putch(int ch)
{
    putchar(ch);
    if (ch == '\n') {
        cursor_row++;
        cursor_col = 0;
    } else if (ch == '\r') {
        cursor_col = 0;
    } else {
        cursor_col++;
    }
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

static void posix_write_raw(const char *data, int len)
{
    fflush(stdout);
    write(STDOUT_FILENO, data, len);
}

static void posix_init(void)
{
    setbuf(stdout, NULL);
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
    .screen_width = 80,
    .screen_height = 25,
    .is_tty = false,
    .init = posix_init,
    .shutdown = posix_shutdown,
};

hal_ops_t *hal_posix_create(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) posix_hal.screen_width = ws.ws_col;
        if (ws.ws_row > 0) posix_hal.screen_height = ws.ws_row;
    }
    posix_hal.is_tty = isatty(STDIN_FILENO);
    return &posix_hal;
}
