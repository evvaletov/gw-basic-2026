#ifndef GW_MATH_H
#define GW_MATH_H

#include "types.h"

/* Integer arithmetic with overflow detection */
int16_t gw_int_add(int16_t a, int16_t b);
int16_t gw_int_sub(int16_t a, int16_t b);
int16_t gw_int_mul(int16_t a, int16_t b);
int16_t gw_int_div(int16_t a, int16_t b);
int16_t gw_int_mod(int16_t a, int16_t b);
int16_t gw_int_idiv(int16_t a, int16_t b);
int16_t gw_int_neg(int16_t a);

/* Float operations */
double gw_fadd(double a, double b);
double gw_fsub(double a, double b);
double gw_fmul(double a, double b);
double gw_fdiv(double a, double b);
double gw_fpow(double base, double exp);
double gw_fneg(double a);

/* Type promotion for binary ops */
void gw_promote(gw_value_t *a, gw_value_t *b);

/* Transcendental functions */
double gw_sin(double x);
double gw_cos(double x);
double gw_tan(double x);
double gw_atn(double x);
double gw_sqr(double x);
double gw_log(double x);
double gw_exp(double x);
double gw_abs(double x);
double gw_sgn(double x);
double gw_int_fn(double x);
double gw_fix(double x);
double gw_rnd(double x);
void gw_randomize(int16_t seed);

/* Conversion functions */
int16_t  gw_cint(double x);
float    gw_csng(double x);
double   gw_cdbl(gw_value_t *v);

/* MBF conversion (for file I/O compatibility) */
float  gw_mbf_to_ieee_single(mbf_single_t mbf);
double gw_mbf_to_ieee_double(mbf_double_t mbf);
mbf_single_t gw_ieee_to_mbf_single(float f);
mbf_double_t gw_ieee_to_mbf_double(double d);

/* Number formatting for PRINT */
void gw_format_number(gw_value_t *v, char *buf, int bufsize);

#endif
