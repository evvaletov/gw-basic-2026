#ifndef GWRT_H
#define GWRT_H

/*
 * Runtime library for compiled GW-BASIC programs.
 *
 * Provides initialization, DATA/READ support, GOSUB return-label stack,
 * and convenience wrappers. Compiled programs link against libgwrt.a
 * which also contains shared handlers for complex BASIC statements.
 */

#include "gwbasic.h"
#include "strpool.h"
#include "portio.h"
#include "sound.h"
#include "graphics.h"
#include "virmem.h"
#include <setjmp.h>

/* Initialization / shutdown */
void gwrt_init(void);
void gwrt_shutdown(void);

/* DATA / READ / RESTORE */
void gwrt_data_set(const char **pool, const int *line_map, int line_count);
void gwrt_data_restore(int index);
const char *gwrt_data_read(void);     /* returns next datum or errors ERR_OD */

/* GOSUB return-label stack */
#define GWRT_GOSUB_MAX 24
void gwrt_gosub_push(int label);
void gwrt_gosub_push_safe(int label, uint16_t from_line);
int  gwrt_gosub_pop(void);

/* Active compiled FOR loops. NULL selects the innermost loop for bare NEXT. */
void gwrt_control_clear(void);
bool gwrt_for_push(void *var, gw_valtype_t type, double limit, double step, int label);
int gwrt_for_next(void *var);   /* continuation label, or -1 when finished */

/* WEND removes its active frame before reevaluating the WHILE condition. */
void gwrt_while_drop(int label, bool required);
void gwrt_while_push(int label);

/* Event checking (ON TIMER, ON KEY, Ctrl+Break) + string pool GC */
void gwrt_check_line(uint16_t line_num);

/* Error handling */
extern jmp_buf gwrt_error_jmp;
extern int gwrt_error_target;    /* ON ERROR GOTO label, 0 = none */
extern int gwrt_resume_label;    /* label for RESUME NEXT */

/* Array element access (wraps gw_array_element) */
gw_value_t *gwrt_array_elem(const char *name, int type, int ndims, int *subs);

/* Safe variant: enhanced diagnostics on bounds error */
gw_value_t *gwrt_array_elem_safe(const char *name, int type, int ndims,
                                  int *subs, uint16_t line_num);

/* DIM statement (wraps gw_stmt_dim via text_ptr) */
gw_value_t *gwrt_dim(const char *name, int type, int ndims, int *dims);

/* Tagged expressions and execution-order default types for native programs. */
gw_value_t *gwrt_var(const char *name, int type);
gw_value_t gwrt_copy(gw_value_t *value);
gw_value_t gwrt_cast(int type, gw_value_t value);
void gwrt_assign(gw_value_t *target, gw_value_t value);
double gwrt_number(gw_value_t value);
int gwrt_integer(gw_value_t value);
gw_string_t gwrt_string(gw_value_t value);
gw_value_t gwrt_unary(int op, gw_value_t value);
gw_value_t gwrt_arg(int code, int index, gw_value_t value, gw_value_t *args);
gw_value_t gwrt_function(int code, int argc, gw_value_t *args);
gw_value_t gwrt_fn_tokens(const uint8_t *tokens);
void gwrt_read_value(gw_value_t *target);
void gwrt_expr_begin(void);
void gwrt_expr_root(gw_value_t *value);
void gwrt_collect_roots(char *pool, char *scratch, size_t *offset);
void gwrt_expr_end(void);
void gwrt_expr_unwind(void);

/* Print helpers (wrappers around existing print.c) */
void gwrt_print_sng(float v);
void gwrt_print_dbl(double v);
void gwrt_print_int(int16_t v);
void gwrt_print_str(gw_string_t s);
void gwrt_print_cstr(const char *s);
void gwrt_print_newline(void);
void gwrt_print_tab(void);      /* comma zone */
void gwrt_print_spc(int n);

#endif
