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
    void (*locate)(int row, int col);
    int  (*get_cursor_row)(void);
    int  (*get_cursor_col)(void);
    void (*cls)(void);
    void (*set_width)(int cols);

    /* Raw mode for INKEY$/INPUT$ */
    void (*enable_raw)(void);
    void (*disable_raw)(void);

    /* Write raw bytes bypassing cursor tracking (for Sixel, etc.) */
    void (*write_raw)(const char *data, int len);

    /* Terminal properties */
    int  screen_width;
    int  screen_height;
    bool is_tty;

    /* Lifecycle */
    void (*init)(void);
    void (*shutdown)(void);
} hal_ops_t;

extern hal_ops_t *gw_hal;

/* Platform implementations */
hal_ops_t *hal_posix_create(void);

#endif
