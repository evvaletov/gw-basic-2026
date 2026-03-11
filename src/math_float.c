#include "gwbasic.h"
#include <math.h>
#include <float.h>
#include <string.h>
#include <stdio.h>

static void check_overflow(double r)
{
    if (isinf(r) || isnan(r))
        gw_error(ERR_OV);
}

double gw_fadd(double a, double b)
{
    double r = a + b;
    check_overflow(r);
    return r;
}

double gw_fsub(double a, double b)
{
    double r = a - b;
    check_overflow(r);
    return r;
}

double gw_fmul(double a, double b)
{
    double r = a * b;
    check_overflow(r);
    return r;
}

double gw_fdiv(double a, double b)
{
    if (b == 0.0)
        gw_error(ERR_DZ);
    double r = a / b;
    check_overflow(r);
    return r;
}

double gw_fpow(double base, double exp)
{
    if (base == 0.0 && exp < 0.0)
        gw_error(ERR_DZ);
    if (base < 0.0 && exp != floor(exp))
        gw_error(ERR_FC);
    double r = pow(base, exp);
    check_overflow(r);
    return r;
}

double gw_fneg(double a)
{
    return -a;
}

/* Type promotion: bring both values to the wider type */
void gw_promote(gw_value_t *a, gw_value_t *b)
{
    if (a->type == VT_STR || b->type == VT_STR)
        gw_error(ERR_TM);

    if (a->type == b->type)
        return;

    /* Promote to the wider type: INT < SNG < DBL */
    gw_valtype_t target = (a->type > b->type) ? a->type : b->type;

    if (a->type != target) {
        if (target == VT_SNG) {
            a->fval = gw_to_sng(a);
            a->type = VT_SNG;
        } else {
            a->dval = gw_to_dbl(a);
            a->type = VT_DBL;
        }
    }
    if (b->type != target) {
        if (target == VT_SNG) {
            b->fval = gw_to_sng(b);
            b->type = VT_SNG;
        } else {
            b->dval = gw_to_dbl(b);
            b->type = VT_DBL;
        }
    }
}

/* CINT: convert to integer with rounding */
int16_t gw_cint(double x)
{
    if (x > 32767.0 || x < -32768.0)
        gw_error(ERR_OV);
    /* Banker's rounding like the original */
    double r = rint(x);
    return (int16_t)r;
}

float gw_csng(double x)
{
    float f = (float)x;
    if ((isinf(f) || isnan(f)) && !isinf(x) && !isnan(x))
        gw_error(ERR_OV);
    return f;
}

double gw_cdbl(gw_value_t *v)
{
    return gw_to_dbl(v);
}

/* Number formatting for PRINT - matches GW-BASIC's FOUT */
void gw_format_number(gw_value_t *v, char *buf, int bufsize)
{
    switch (v->type) {
    case VT_INT:
        snprintf(buf, bufsize, "% d", v->ival);
        break;
    case VT_SNG: {
        /* GW-BASIC prints up to 7 significant digits for single */
        double d = v->fval;
        if (d == 0.0) {
            snprintf(buf, bufsize, " 0");
            break;
        }
        double ad = fabs(d);
        if (ad >= 0.01 && ad < 1e7) {
            /* Try fixed notation */
            char tmp[32];
            for (int prec = 7; prec >= 0; prec--) {
                snprintf(tmp, sizeof(tmp), "%.*f", prec, d);
                /* Count significant digits */
                int sig = 0;
                for (char *p = tmp; *p; p++) {
                    if (*p == '-' || *p == ' ') continue;
                    if (*p == '.') continue;
                    if (sig == 0 && *p == '0') continue;
                    sig++;
                }
                if (sig <= 7) {
                    /* Remove trailing zeros after decimal point */
                    char *dot = strchr(tmp, '.');
                    if (dot) {
                        char *end = tmp + strlen(tmp) - 1;
                        while (end > dot && *end == '0') *end-- = '\0';
                        if (end == dot) *end = '\0';
                    }
                    if (d >= 0)
                        snprintf(buf, bufsize, " %s", tmp);
                    else
                        snprintf(buf, bufsize, "%s", tmp);
                    return;
                }
            }
        }
        /* Scientific notation - GW-BASIC uses format like 1.234E+10 */
        {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%.6E", fabs(d));
            /* Strip trailing zeros before E: 1.000000E+10 -> 1E+10 */
            char *e = strchr(tmp, 'E');
            if (e) {
                char *p = e - 1;
                while (p > tmp && *p == '0') p--;
                if (*p == '.') p--;  /* remove dot if no fractional part */
                memmove(p + 1, e, strlen(e) + 1);
                e = strchr(tmp, 'E');
            }
            /* Convert 3-digit exponent to 2-digit: E+010 -> E+10 */
            if (e && strlen(e) >= 5 && e[2] == '0') {
                memmove(e + 2, e + 3, strlen(e + 3) + 1);
            }
            if (d < 0)
                snprintf(buf, bufsize, "-%s", tmp);
            else
                snprintf(buf, bufsize, " %s", tmp);
        }
        break;
    }
    case VT_DBL: {
        double d = v->dval;
        if (d == 0.0) {
            snprintf(buf, bufsize, " 0");
            break;
        }
        double ad = fabs(d);
        if (ad >= 0.01 && ad < 1e16) {
            char tmp[32];
            for (int prec = 16; prec >= 0; prec--) {
                snprintf(tmp, sizeof(tmp), "%.*f", prec, d);
                int sig = 0;
                for (char *p = tmp; *p; p++) {
                    if (*p == '-' || *p == ' ') continue;
                    if (*p == '.') continue;
                    if (sig == 0 && *p == '0') continue;
                    sig++;
                }
                if (sig <= 16) {
                    char *dot = strchr(tmp, '.');
                    if (dot) {
                        char *end = tmp + strlen(tmp) - 1;
                        while (end > dot && *end == '0') *end-- = '\0';
                        if (end == dot) *end = '\0';
                    }
                    if (d >= 0)
                        snprintf(buf, bufsize, " %s", tmp);
                    else
                        snprintf(buf, bufsize, "%s", tmp);
                    return;
                }
            }
        }
        /* Scientific with D exponent for double precision */
        {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "%.15E", fabs(d));
            /* Strip trailing zeros before E: 1.000000000000000E+10 -> 1E+10 */
            char *e = strchr(tmp, 'E');
            if (e) {
                char *p = e - 1;
                while (p > tmp && *p == '0') p--;
                if (*p == '.') p--;
                memmove(p + 1, e, strlen(e) + 1);
                e = strchr(tmp, 'E');
            }
            if (e) *e = 'D';  /* GW-BASIC uses D for double */
            /* Convert 3-digit exponent to 2-digit: D+010 -> D+10 */
            if (e && strlen(e) >= 5 && e[2] == '0') {
                memmove(e + 2, e + 3, strlen(e + 3) + 1);
            }
            if (d < 0)
                snprintf(buf, bufsize, "-%s", tmp);
            else
                snprintf(buf, bufsize, " %s", tmp);
        }
        break;
    }
    default:
        snprintf(buf, bufsize, "?");
    }
}

/* MBF conversion routines */
float gw_mbf_to_ieee_single(mbf_single_t mbf)
{
    if (mbf.exponent == 0) return 0.0f;

    uint32_t ieee;
    uint8_t sign = mbf.mantissa[2] & 0x80;
    uint32_t mantissa = ((uint32_t)(mbf.mantissa[2] | 0x80) << 16)
                       | ((uint32_t)mbf.mantissa[1] << 8)
                       | mbf.mantissa[0];

    int exp = (int)mbf.exponent - 128 - 1 + 127;  /* MBF bias 128, IEEE bias 127 */
    if (exp < 0 || exp > 254) return 0.0f;

    /* Hidden 1 at bit 23 of mantissa, IEEE stores bits 0-22 (fraction only) */
    ieee = ((uint32_t)sign << 24) | ((uint32_t)exp << 23) | (mantissa & 0x7FFFFF);
    float result;
    memcpy(&result, &ieee, 4);
    return result;
}

double gw_mbf_to_ieee_double(mbf_double_t mbf)
{
    if (mbf.exponent == 0) return 0.0;

    uint64_t ieee;
    uint8_t sign = mbf.mantissa[6] & 0x80;
    uint64_t mantissa = 0;
    for (int i = 6; i >= 0; i--)
        mantissa = (mantissa << 8) | (i == 6 ? (mbf.mantissa[i] | 0x80) : mbf.mantissa[i]);

    int exp = (int)mbf.exponent - 128 - 1 + 1023;
    if (exp < 0 || exp > 2046) return 0.0;

    /* Hidden 1 at bit 55 of mantissa, IEEE stores bits 0-51 (fraction only).
     * MBF has 55 fraction bits (3 more than IEEE), shift right 3 to align. */
    ieee = ((uint64_t)sign << 56) | ((uint64_t)exp << 52) | ((mantissa >> 3) & 0x000FFFFFFFFFFFFF);
    double result;
    memcpy(&result, &ieee, 8);
    return result;
}

mbf_single_t gw_ieee_to_mbf_single(float f)
{
    mbf_single_t mbf = {{0}, 0};
    if (f == 0.0f) return mbf;

    uint32_t ieee;
    memcpy(&ieee, &f, 4);

    uint8_t sign = (ieee >> 31) & 1;
    int exp = ((ieee >> 23) & 0xFF) - 127 + 128 + 1;
    uint32_t mantissa = (ieee & 0x7FFFFF) | 0x800000;

    if (exp < 0) { mbf.exponent = 0; return mbf; }
    if (exp > 255) exp = 255;

    mbf.exponent = exp;
    /* Hidden 1 at bit 23, sign replaces it in byte 2 bit 7 */
    mbf.mantissa[0] = mantissa & 0xFF;
    mbf.mantissa[1] = (mantissa >> 8) & 0xFF;
    mbf.mantissa[2] = ((mantissa >> 16) & 0x7F) | (sign << 7);

    return mbf;
}

mbf_double_t gw_ieee_to_mbf_double(double d)
{
    mbf_double_t mbf = {{0}, 0};
    if (d == 0.0) return mbf;

    uint64_t ieee;
    memcpy(&ieee, &d, 8);

    uint8_t sign = (ieee >> 63) & 1;
    int exp = ((ieee >> 52) & 0x7FF) - 1023 + 128 + 1;
    uint64_t mantissa = (ieee & 0x000FFFFFFFFFFFFF) | 0x0010000000000000;

    if (exp < 0) { mbf.exponent = 0; return mbf; }
    if (exp > 255) exp = 255;

    mbf.exponent = exp;
    /* Hidden 1 at bit 52, MBF stores 55 fraction bits + sign.
     * Shift left 3 to place hidden 1 at bit 55 (byte 6 bit 7 = sign). */
    mantissa <<= 3;
    for (int i = 0; i < 7; i++) {
        if (i == 6)
            mbf.mantissa[i] = ((mantissa >> (i * 8)) & 0x7F) | (sign << 7);
        else
            mbf.mantissa[i] = (mantissa >> (i * 8)) & 0xFF;
    }

    return mbf;
}
