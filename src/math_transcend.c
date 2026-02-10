#include "gwbasic.h"
#include <math.h>
#include <stdlib.h>

/* Random number state (compatible with GW-BASIC's LCG) */
uint32_t gw_rnd_seed = 0x50000;  /* default seed */
static int rnd_seeded = 0;

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
        /* Seed from argument */
        union { float f; uint32_t u; } conv;
        conv.f = (float)x;
        gw_rnd_seed = conv.u;
        rnd_seeded = 1;
    }

    if (x != 0.0 || !rnd_seeded) {
        /* Linear congruential generator matching GW-BASIC */
        gw_rnd_seed = gw_rnd_seed * 0x43FD43FD + 0xC39EC3;
        gw_rnd_seed &= 0xFFFFFF;
        rnd_seeded = 1;
    }

    return (double)gw_rnd_seed / 16777216.0;
}
