#ifndef GWBASIC_H
#define GWBASIC_H

#include "types.h"
#include "tokens.h"
#include "error.h"
#include "hal.h"
#include "interp.h"
#include "eval.h"
#include "gw_math.h"
#include "strings.h"

#define GW_VERSION "0.2.0"
#define GW_BANNER "GW-BASIC " GW_VERSION

/* Tokenizer */
int  gw_crunch(const char *text, uint8_t *out, int outsize);
void gw_list_line(uint8_t *tokens, int len, char *out, int outsize);

/* PRINT statement */
void gw_stmt_print(void);
void gw_print_value(gw_value_t *v);
void gw_print_newline(void);

/* Variables (vars.c) */
gw_valtype_t gw_parse_varname(char name_out[2]);
var_entry_t *gw_var_find_or_create(const char name[2], gw_valtype_t type);
void gw_var_assign(var_entry_t *var, gw_value_t *val);
void gw_vars_clear(void);
void gw_stmt_deftype(gw_valtype_t type);
void gw_stmt_swap(void);

/* Arrays (arrays.c) */
void gw_stmt_dim(void);
gw_value_t *gw_array_element(const char name[2], gw_valtype_t type);
void gw_stmt_erase(void);
void gw_stmt_option(void);
void gw_arrays_clear(void);

/* Input (input.c) */
void gw_stmt_input(void);
void gw_stmt_line_input(void);

/* DEF FN evaluation (interp.c) */
gw_value_t gw_eval_fn_call(void);

/* Error recovery for run loop */
extern jmp_buf gw_run_jmp;

#endif
