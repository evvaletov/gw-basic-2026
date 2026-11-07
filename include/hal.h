#ifndef GW_HAL_H
#define GW_HAL_H

#include <stdint.h>
#include <stdbool.h>

/* Hardware Abstraction Layer vtable */
typedef struct hal_ops {
    /* Terminal I/O */
    void (*putch)(int ch);
    void (*puts)(const char *s);
    int  (*getch)(void);           /* blocking read */
    bool (*kbhit)(void);           /* key available? */
    void (*locate)(int row, int col); /* one-based coordinates */
    int  (*get_cursor_row)(void);
    int  (*get_cursor_col)(void);
    void (*cls)(void);
    void (*set_width)(int cols);

    /* Raw mode for INKEY$/INPUT$ */
    void (*enable_raw)(void);
    void (*disable_raw)(void);

    /* Write raw bytes bypassing cursor tracking (for Sixel, etc.) */
    void (*write_raw)(const char *data, int len);

    /* TUI rendering (used by src/tui.c).  POSIX backend emits ANSI escape
     * sequences; DOS backend drives the BIOS so the editor works on bare
     * FreeDOS without ANSI.SYS. */
    void (*tui_enter)(void);
    void (*tui_leave)(void);
    void (*render_run)(int row, int col,
                       const uint8_t *chars, const uint8_t *attrs, int len);
    void (*set_cursor_shape)(int shape);  /* 0=hide, 1=block, 2=line */

    /* Terminal properties */
    int  screen_width;
    int  screen_height;
    bool is_tty;

    /* Lifecycle */
    void (*init)(void);
    void (*shutdown)(void);

    /* Optional asynchronous keyboard break poll.  Consume only break keys;
     * preserve other queued input for INKEY$, INPUT and ON KEY. */
    bool (*check_break)(void);

    /* Optional console line editor outside the TUI. Returns length, -1 EOF,
     * -2 keyboard break, or -3 to use the stdio fallback. Restores terminal
     * settings before returning. The caller owns the buffer. */
    int (*read_input)(char *buf, int capacity, bool suppress_newline);

    /* Optional un-echoed byte input: count read, -2 break, -3 stdio fallback.
     * Restores the entry terminal state before returning, including on EOF. */
    int (*read_chars)(char *buf, int count);
} hal_ops_t;

extern hal_ops_t *gw_hal;

/* Platform implementations */
hal_ops_t *hal_posix_create(void);
#ifdef __MSDOS__
hal_ops_t *hal_dos_create(void);
#endif

#endif
