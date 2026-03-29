#ifndef ANALYSIS_H
#define ANALYSIS_H

#include "types.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_LINES    4096
#define MAX_VARS     256
#define MAX_GOTOS    256
#define MAX_DATA     1024
#define MAX_GOSUB_RET 256

typedef struct {
    uint16_t line_num;
    bool is_target;       /* referenced by GOTO/GOSUB/etc. */
    bool has_data;        /* contains DATA statement */
    int data_start;       /* index into data pool */
} line_info_t;

typedef struct {
    char name[2];
    gw_valtype_t type;
} var_info_t;

typedef struct {
    line_info_t lines[MAX_LINES];
    int line_count;

    var_info_t vars[MAX_VARS];
    int var_count;

    uint16_t goto_targets[MAX_GOTOS];
    int goto_count;

    char *data_pool[MAX_DATA];  /* collected DATA literals */
    int data_count;

    int data_line_map[MAX_LINES][2];  /* [line_num, data_start_index] */
    int data_line_count;

    gw_valtype_t def_type[26];  /* from DEFINT/DEFSNG/DEFDBL/DEFSTR */
} analysis_t;

/* Run analysis pass over the loaded program */
void analysis_run(analysis_t *a);

/* Find a variable in the census, return index or -1 */
int analysis_find_var(analysis_t *a, const char name[2], gw_valtype_t type);

/* Add a variable to the census if not already present */
int analysis_add_var(analysis_t *a, const char name[2], gw_valtype_t type);

/* Check if a line number is a jump target */
bool analysis_is_target(analysis_t *a, uint16_t line_num);

#endif
