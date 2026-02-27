#ifndef GW_TYPES_H
#define GW_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* Value types matching GW-BASIC's VALTYP */
typedef enum {
    VT_INT    = 2,   /* 16-bit integer (%) */
    VT_SNG    = 4,   /* single precision (!) */
    VT_DBL    = 8,   /* double precision (#) */
    VT_STR    = 3,   /* string ($) */
} gw_valtype_t;

/* String descriptor - matches original's 3-byte layout concept */
typedef struct {
    uint8_t len;
    char *data;       /* malloc'd, NOT null-terminated */
} gw_string_t;

/* Unified value - the FAC (floating accumulator) equivalent */
typedef struct {
    gw_valtype_t type;
    union {
        int16_t ival;
        float fval;
        double dval;
        gw_string_t sval;
    };
} gw_value_t;

/* Program line stored in memory */
typedef struct program_line {
    struct program_line *next;
    uint16_t num;        /* line number 0-65529 */
    uint16_t len;        /* token data length */
    uint8_t *tokens;     /* tokenized line data */
} program_line_t;

/* Variable entry */
typedef struct {
    char name[2];        /* 2-char name (original GW-BASIC limit) */
    gw_valtype_t type;
    gw_value_t val;
} var_entry_t;

/* Array entry */
typedef struct {
    char name[2];
    gw_valtype_t type;
    int ndims;
    int dims[8];        /* max dimensions per DIM */
    int total_elements;
    gw_value_t *data;   /* malloc'd flat array */
} array_entry_t;

/* FOR stack entry */
typedef struct {
    var_entry_t *var;
    gw_value_t limit, step;
    uint8_t *loop_text;
    struct program_line *loop_line;
    uint16_t line_num;
} for_entry_t;

/* Event trap mode */
typedef enum { TRAP_OFF = 0, TRAP_ON = 1, TRAP_STOP = 2 } trap_mode_t;

/* Event trap state (shared by TIMER/KEY traps) */
typedef struct event_trap {
    trap_mode_t mode;
    uint16_t gosub_line;
    bool pending;
    bool in_handler;
} event_trap_t;

/* ON TIMER trap */
typedef struct {
    event_trap_t trap;
    float interval;        /* seconds */
    double last_fire;      /* monotonic timestamp */
} timer_trap_t;

/* GOSUB stack entry */
typedef struct {
    uint8_t *ret_text;
    struct program_line *ret_line;
    uint16_t line_num;
    struct event_trap *event_source;  /* non-NULL for event handler GOSUB */
} gosub_entry_t;

/* WHILE stack entry */
typedef struct {
    uint8_t *while_text;
    struct program_line *while_line;
    uint16_t line_num;
} while_entry_t;

/* DEF FN entry */
typedef struct {
    bool defined;
    char param_name[2];
    gw_valtype_t param_type, ret_type;
    uint8_t *body_text;
    struct program_line *body_line;
} fn_def_t;

/* File entry for OPEN/CLOSE file table */
typedef struct {
    FILE *fp;
    int mode;       /* 0=closed, 'I'=input, 'O'=output, 'A'=append, 'R'=random */
    int file_num;
    bool eof_flag;
    int record_len;         /* record length for random access (default 128) */
    uint8_t *field_buf;     /* FIELD buffer (malloc'd, record_len bytes) */
    int field_count;        /* number of FIELDed variables */
    struct field_var {
        char name[2];
        gw_valtype_t type;
        int offset;
        int width;
    } fields[32];
} file_entry_t;

/* MBF (Microsoft Binary Format) types for file compatibility */
typedef struct {
    uint8_t mantissa[3];
    uint8_t exponent;
} mbf_single_t;

typedef struct {
    uint8_t mantissa[7];
    uint8_t exponent;
} mbf_double_t;

#endif
