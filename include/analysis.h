#ifndef ANALYSIS_H
#define ANALYSIS_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_LINES     8192
#define MAX_VARS      1024
#define MAX_ARRAYS    256
#define MAX_GOTOS     1024
#define MAX_DATA      4096
#define MAX_GOSUB_RET 1024

#define MAX_EXTERNS      64
#define MAX_EXTERN_ARGS  8
#define EXTERN_NAME_MAX  32

typedef struct {
    uint16_t line_num;
    bool is_target;       /* referenced by GOTO/GOSUB/etc. */
    bool has_data;        /* contains DATA statement */
    int data_start;       /* index into data pool */
} line_info_t;

/* A C function declared via the '$EXTERN NAME(ARGS) AS RET [= c_symbol] pragma.
 * `name` is the BASIC call name (case-preserving; matched case-insensitively at
 * call sites).  `c_name` is the emitted C symbol: it defaults to `name`, or an
 * optional "= c_symbol" alias maps a BASIC-legal name to an arbitrary C symbol
 * (e.g. one containing underscores, like sqlite3_open). */
typedef struct {
    char name[EXTERN_NAME_MAX];
    char c_name[EXTERN_NAME_MAX];
    gw_valtype_t ret_type;
    gw_valtype_t arg_types[MAX_EXTERN_ARGS];
    int argc;
} extern_func_t;

typedef struct {
    char name[GW_NAME_SIZE];
    gw_valtype_t type;
    uint16_t first_assign_line;  /* 0 = never assigned */
    uint16_t first_use_line;     /* 0 = never used */
} var_info_t;

#define MAX_ARR_DIMS 8

/* Array census for --inline-arrays: an array is `inlinable` when it is DIMmed
 * exactly once with constant integer bounds and never disqualified (variable
 * DIM, redefinition, or ERASE). Codegen caches the runtime storage pointer
 * for direct indexing, with a lookup fallback after storage invalidation. */
typedef struct {
    char name[GW_NAME_SIZE];
    gw_valtype_t type;
    int ndims;
    int dims[MAX_ARR_DIMS];   /* upper bound per dimension, as written in DIM */
    bool dimmed;              /* an explicit DIM was seen */
    bool inlinable;           /* eligible for cached storage and direct indexing */
} array_info_t;

typedef struct {
    line_info_t lines[MAX_LINES];
    int line_count;

    var_info_t vars[MAX_VARS];
    int var_count;

    array_info_t arrays[MAX_ARRAYS];
    int array_count;
    int option_base;             /* 0 (default) or 1, from OPTION BASE */

    uint16_t goto_targets[MAX_GOTOS];
    int goto_count;

    char *data_pool[MAX_DATA];  /* collected DATA literals */
    int data_count;

    int data_line_map[MAX_LINES][2];  /* [line_num, data_start_index] */
    int data_line_count;

    gw_valtype_t def_type[26];  /* from DEFINT/DEFSNG/DEFDBL/DEFSTR */
    bool runtime_types;        /* defaults can change after executable code */
    bool has_resume;           /* statement continuations are needed */

    extern_func_t externs[MAX_EXTERNS];  /* '$EXTERN FFI declarations */
    int extern_count;
} analysis_t;

/* Run analysis pass over the loaded program */
void analysis_run(analysis_t *a);

/* Find a variable in the census, return index or -1 */
int analysis_find_var(analysis_t *a, const char *name, gw_valtype_t type);

/* Add a variable to the census if not already present */
int analysis_add_var(analysis_t *a, const char *name, gw_valtype_t type);

/* Check if a line number is a jump target */
bool analysis_is_target(analysis_t *a, uint16_t line_num);

/* Find a declared extern function by name (case-insensitive), or NULL */
const extern_func_t *analysis_find_extern(analysis_t *a, const char *name);

/* Find an inlinable array by name+type; returns NULL if none or not inlinable */
array_info_t *analysis_inlinable_array(analysis_t *a, const char *name, gw_valtype_t type);

/* Emit static analysis warnings to stderr */
void analysis_warnings(analysis_t *a);

#endif
