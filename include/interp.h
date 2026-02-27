#ifndef GW_INTERP_H
#define GW_INTERP_H

#include "types.h"
#include <stdbool.h>

/* Interpreter state - the global context */
typedef struct {
    /* Text pointer (TXTPTR equivalent) */
    uint8_t *text_ptr;

    /* Current accumulator (FAC equivalent) */
    gw_value_t fac;

    /* Program state */
    uint16_t cur_line_num;      /* CURLIN - 65535 = direct mode */
    program_line_t *prog_head;  /* first line of program */
    bool trace_on;              /* TRON/TROFF */

    /* Tokenizer buffers */
    uint8_t kbuf[300];          /* crunch buffer */
    char buf[256];              /* input line buffer */

    /* Default variable types for A-Z (DEFTBL) */
    gw_valtype_t def_type[26];

    /* Variable storage */
    var_entry_t vars[256];
    int var_count;
    array_entry_t arrays[64];
    int array_count;
    int option_base;

    /* Control flow stacks */
#define MAX_FOR_DEPTH   16
#define MAX_GOSUB_DEPTH 24
#define MAX_WHILE_DEPTH 16
    for_entry_t for_stack[MAX_FOR_DEPTH];
    int for_sp;
    gosub_entry_t gosub_stack[MAX_GOSUB_DEPTH];
    int gosub_sp;
    while_entry_t while_stack[MAX_WHILE_DEPTH];
    int while_sp;

    /* DEF FN */
    fn_def_t fn_defs[26];

    /* Run state */
    program_line_t *cur_line;
    bool running;

    /* CONT state */
    uint8_t *cont_text;
    program_line_t *cont_line;

    /* DATA pointer */
    uint8_t *data_ptr;
    program_line_t *data_line_ptr;
    uint16_t data_line;

    /* File I/O table (#1-#15, index 0 unused) */
    file_entry_t files[16];

    /* Error trapping */
    uint16_t on_error_line;
    bool in_error_handler;
    uint8_t *err_resume_text;
    program_line_t *err_resume_line;
    uint16_t err_line_num;

    /* AUTO mode */
    bool auto_mode;
    uint16_t auto_line;
    uint16_t auto_inc;

    /* COMMON variable list for CHAIN */
    int common_count;
    struct { char name[2]; gw_valtype_t type; } common_vars[64];

    /* Event trapping */
    timer_trap_t timer_trap;
    event_trap_t key_traps[10];  /* KEY(1)-KEY(10) */
} interp_state_t;

extern interp_state_t gw;

/* Direct mode line number sentinel */
#define LINE_DIRECT 65535

void gw_init(void);
void gw_exec_direct(const char *line);
void gw_exec_stmt(void);
void gw_run_loop(void);

/* Program storage */
void gw_store_line(uint16_t num, uint8_t *tokens, int len);
void gw_delete_line(uint16_t num);
program_line_t *gw_find_line(uint16_t num);
void gw_free_program(void);

/* CHRGET/CHRGOT - advance/peek the text pointer */
uint8_t gw_chrget(void);
uint8_t gw_chrgot(void);
void gw_skip_spaces(void);
bool gw_is_letter(uint8_t ch);
bool gw_is_digit(uint8_t ch);

/* Expect a specific token, else syntax error */
void gw_expect(uint8_t tok);

#endif
