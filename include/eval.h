#ifndef GW_EVAL_H
#define GW_EVAL_H

#include "types.h"

/* Expression evaluator - recursive descent with precedence climbing */
gw_value_t gw_eval(void);       /* evaluate any expression */
gw_value_t gw_eval_num(void);   /* evaluate, require numeric */
gw_value_t gw_eval_str(void);   /* evaluate, require string */
int16_t    gw_eval_int(void);   /* evaluate, require integer result */
uint16_t   gw_eval_uint16(void);

/* Shared typed operation used by interpreted and generated expressions. */
gw_value_t gw_value_binary(uint8_t op, gw_value_t left, gw_value_t right);

/* Shared handlers in libgwrt can evaluate expressions containing native FFI.
 * Their live temporaries join the native collector; standalone builds omit it. */
#ifdef GW_NATIVE_RUNTIME
void gwrt_expr_begin(void);
void gwrt_expr_root(gw_value_t *value);
void gwrt_expr_end(void);
void gwrt_expr_unwind(void);
#define GW_TEMP_BEGIN() gwrt_expr_begin()
#define GW_TEMP_ROOT(value) gwrt_expr_root(value)
#define GW_TEMP_END() gwrt_expr_end()
#else
#define GW_TEMP_BEGIN() ((void)0)
#define GW_TEMP_ROOT(value) ((void)0)
#define GW_TEMP_END() ((void)0)
#endif

/* Force type conversions */
int16_t  gw_to_int(gw_value_t *v);
float    gw_to_sng(gw_value_t *v);
double   gw_to_dbl(gw_value_t *v);

/* Check for closing paren */
void gw_expect_rparen(void);

#endif
