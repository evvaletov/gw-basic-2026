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

#define GW_VERSION "0.9.0"
#define GW_BANNER "GW-BASIC " GW_VERSION

/* Tokenizer */
int  gw_crunch(const char *text, uint8_t *out, int outsize);
void gw_list_line(uint8_t *tokens, int len, char *out, int outsize);

/* PRINT statement */
void gw_stmt_print(void);
void gw_print_value(gw_value_t *v);
void gw_print_newline(void);

/* LPRINT / LLIST (print.c) */
void gw_stmt_lprint(void);
void gw_stmt_llist(void);
void gw_lpt_set_path(const char *path);
void gw_lpt_close(void);

/* Line number parsing helper (interp.c) */
bool read_linenum(uint16_t *out);

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

/* File I/O (fileio.c) */
file_entry_t *gw_file_get(int num);
void gw_file_close_all(void);
void gw_stmt_open(void);
void gw_stmt_close(void);
void gw_stmt_print_file(void);
void gw_stmt_write_file(void);
void gw_stmt_input_file(void);
void gw_stmt_line_input_file(void);
int  gw_file_eof(int num);

/* Program I/O (program_io.c) */
void gw_stmt_save(void);
void gw_stmt_load(void);
void gw_stmt_merge(void);
void gw_stmt_load_internal(const char *filename, bool clear);

/* PRINT USING (print_using.c) */
void gw_print_using(FILE *fp);

/* MID$ assignment (interp.c) */
void gw_stmt_mid_assign(void);

/* Random-access file I/O (fileio.c) */
void gw_stmt_field(void);
void gw_stmt_lset(void);
void gw_stmt_rset(void);
void gw_stmt_put(void);
void gw_stmt_get(void);

/* MBF conversion functions (fileio.c) */
gw_value_t gw_fn_cvi(gw_value_t *s);
gw_value_t gw_fn_cvs(gw_value_t *s);
gw_value_t gw_fn_cvd(gw_value_t *s);
gw_value_t gw_fn_mki(int16_t n);
gw_value_t gw_fn_mks(float f);
gw_value_t gw_fn_mkd(double d);

/* CHAIN (interp.c) */
void gw_stmt_chain(void);

/* Error recovery for run loop */
extern jmp_buf gw_run_jmp;

#endif
