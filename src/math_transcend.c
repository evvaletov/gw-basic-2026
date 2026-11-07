#include "gwbasic.h"
#include <math.h>
#include <stdlib.h>

/* Random number state (compatible with GW-BASIC's LCG) */
uint32_t gw_rnd_seed = 0x4FC752;  /* initial 24-bit state from GW-BASIC */

void gw_randomize(int16_t seed)
{
    /* GWMAIN.ASM STRNDS replaces the upper 16 bits, retains the low byte,
     * and advances once before returning to the program. */
    gw_rnd_seed = (gw_rnd_seed & 0xFF) | ((uint32_t)(uint16_t)seed << 8);
    (void)gw_rnd(1.0);
}

double gw_sin(double x) { return sin(x); }
double gw_cos(double x) { return cos(x); }

double gw_tan(double x)
{
    double c = cos(x);
    if (c == 0.0)
        gw_error(ERR_OV);
    return sin(x) / c;
}

double gw_atn(double x) { return atan(x); }

double gw_sqr(double x)
{
    if (x < 0.0)
        gw_error(ERR_FC);
    return sqrt(x);
}

double gw_log(double x)
{
    if (x <= 0.0)
        gw_error(ERR_FC);
    return log(x);
}

double gw_exp(double x)
{
    double r = exp(x);
    if (isinf(r) || isnan(r))
        gw_error(ERR_OV);
    return r;
}

double gw_abs(double x) { return fabs(x); }

double gw_sgn(double x)
{
    if (x > 0.0) return 1.0;
    if (x < 0.0) return -1.0;
    return 0.0;
}

double gw_int_fn(double x)
{
    return floor(x);
}

double gw_fix(double x)
{
    return (x >= 0.0) ? floor(x) : ceil(x);
}

/*
 * RND function - compatible with GW-BASIC:
 *   RND(0)  = repeat last
 *   RND(>0) = next random
 *   RND(<0) = seed with argument, return first
 */
double gw_rnd(double x)
{
    if (x < 0.0) {
        /* The original uses the normalized single-precision mantissa,
         * including its implicit leading bit, without sign or exponent. */
        union { float f; uint32_t u; } conv;
        int exponent;
        conv.f = (float)fabs(x);
        if (!isfinite(conv.f)) gw_error(ERR_OV);
        if (conv.f == 0.0f) return (double)gw_rnd_seed / 16777216.0;
        conv.f = (float)frexp(conv.f, &exponent);
        gw_rnd_seed = (conv.u & 0x7FFFFF) | 0x800000;
    }

    if (x != 0.0) {
        /* MATH1.ASM $RNDA/$RNDC and MATH2.ASM RND10, modulo 2^24. */
        gw_rnd_seed = gw_rnd_seed * 214013UL + 2531011UL;
        gw_rnd_seed &= 0xFFFFFF;
    }

    return (double)gw_rnd_seed / 16777216.0;
}
