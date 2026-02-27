#ifndef GW_TUI_H
#define GW_TUI_H

#include <stdint.h>
#include <stdbool.h>

#define TUI_DEFAULT_ROWS 25
#define TUI_DEFAULT_COLS 80
#define TUI_MAX_ROWS 200
#define TUI_MAX_COLS 300
#define TUI_MAX_LINE 255

/* Screen cell: character + color attribute */
typedef struct {
    uint8_t ch;
    uint8_t attr;  /* GW-BASIC color attribute (fg | bg<<4) */
} tui_cell_t;

/* Special key codes returned by tui_read_key() */
#define TK_UP       0x100
#define TK_DOWN     0x101
#define TK_LEFT     0x102
#define TK_RIGHT    0x103
#define TK_HOME     0x104
#define TK_END      0x105
#define TK_DELETE   0x106
#define TK_INSERT   0x107
#define TK_PGUP     0x108
#define TK_PGDN     0x109
#define TK_F1       0x10A
#define TK_F2       0x10B
#define TK_F3       0x10C
#define TK_F4       0x10D
#define TK_F5       0x10E
#define TK_F6       0x10F
#define TK_F7       0x110
#define TK_F8       0x111
#define TK_F9       0x112
#define TK_F10      0x113
#define TK_CTRL_HOME 0x114
#define TK_BACKSPACE 0x08
#define TK_ENTER    0x0D
#define TK_ESCAPE   0x1B
#define TK_TAB      0x09
#define TK_CTRL_C   0x03
#define TK_CTRL_BREAK 0x200

/* Key buffer for event trapping (keys consumed from HAL but not yet processed) */
#define TUI_KEYBUF_SIZE 16

/* TUI state */
typedef struct {
    tui_cell_t *screen;             /* dynamically allocated [rows * cols] */
    int rows;                       /* screen height (default 25) */
    int cols;                       /* screen width (default 80) */
    int cursor_row;
    int cursor_col;
    bool insert_mode;
    bool key_bar_visible;           /* KEY ON */
    uint8_t current_attr;           /* current color attribute */
    char fkey_defs[10][16];         /* F1-F10 definitions */
    bool active;
    volatile bool break_flag;       /* set by SIGINT handler */
    int view_top;                   /* top of scrollable area */
    int view_bottom;                /* bottom row (rows-1 normally, rows-2 with key bar) */
    int keybuf[TUI_KEYBUF_SIZE];   /* ring buffer for pushed-back keys */
    int keybuf_head;
    int keybuf_tail;
} tui_state_t;

extern tui_state_t tui;

/* Lifecycle */
void tui_init(bool fullscreen);
void tui_shutdown(void);

/* Screen cell access macro */
#define TUI_CELL(r, c) tui.screen[(r) * tui.cols + (c)]

/* Screen buffer operations (these replace HAL ops when TUI is active) */
void tui_putch(int ch);
void tui_puts(const char *s);
void tui_cls(void);
void tui_locate(int row, int col);
int  tui_get_cursor_row(void);
int  tui_get_cursor_col(void);

/* Rendering */
void tui_refresh(void);
void tui_refresh_row(int row);
void tui_update_cursor(void);

/* Input */
int  tui_read_key(void);
char *tui_read_line(void);

/* Function key bar */
void tui_key_on(void);
void tui_key_off(void);
void tui_key_list(void);

/* EDIT statement */
void tui_edit_line(const char *prefill);

/* Key buffer for event trapping */
void tui_push_key(int key);
int  tui_pop_key(void);
bool tui_keybuf_empty(void);

/* Ctrl+Break */
void tui_check_break(void);
void tui_install_break_handler(void);

/* Cursor shape */
void tui_set_cursor_block(void);
void tui_set_cursor_line(void);

#endif
