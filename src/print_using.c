#include "gwbasic.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

/*
 * PRINT USING formatter - implements GW-BASIC's formatted output.
 *
 * Numeric format characters:
 *   #       Digit position
 *   .       Decimal point
 *   ,       Insert commas every 3 digits (before decimal)
 *   +       Print sign (at start or end)
 *   -       Trailing negative sign
 *   $$      Leading dollar sign
 *   **      Asterisk fill
 *   **$     Asterisk fill + dollar
 *   ^^^^    Scientific notation
 *
 * String format characters:
 *   !       First character only
 *   &       Entire string
 *   \ \     Fixed width (count chars between backslashes inclusive)
 *
 * _       Literal escape: next char printed as-is
 */

/* Output helpers */
static void pu_putch(FILE *fp, int ch)
{
    if (fp) {
        fputc(ch, fp);
    } else if (gw_hal) {
        gw_hal->putch(ch);
    } else {
        putchar(ch);
    }
}

static void pu_puts(FILE *fp, const char *s)
{
    while (*s)
        pu_putch(fp, *s++);
}

/* Format a numeric value according to the format spec */
static void format_number(FILE *fp, const char *fmt, int fmtlen, double val)
{
    /* Parse the format spec */
    bool leading_plus = false, trailing_plus = false;
    bool trailing_minus = false;
    bool dollar = false;
    bool asterisk_fill = false;
    bool use_commas = false;
    bool scientific = false;
    int total_digits = 0;  /* total # positions */
    int decimal_digits = -1;  /* -1 = no decimal point */
    bool has_decimal = false;

    int i = 0;

    /* Check leading + */
    if (i < fmtlen && fmt[i] == '+') {
        leading_plus = true;
        i++;
    }

    /* Check ** or **$ or $$ */
    if (i + 1 < fmtlen && fmt[i] == '*' && fmt[i + 1] == '*') {
        asterisk_fill = true;
        total_digits += 2;
        i += 2;
        if (i < fmtlen && fmt[i] == '$') {
            dollar = true;
            i++;
            total_digits++;
        }
    } else if (i + 1 < fmtlen && fmt[i] == '$' && fmt[i + 1] == '$') {
        dollar = true;
        total_digits += 2;
        i += 2;
    }

    /* Count # before decimal */
    while (i < fmtlen && fmt[i] == '#') {
        total_digits++;
        i++;
    }

    /* Check for comma */
    if (i < fmtlen && fmt[i] == ',') {
        use_commas = true;
        i++;
    }

    /* More # after comma but before decimal */
    while (i < fmtlen && fmt[i] == '#') {
        total_digits++;
        i++;
    }

    /* Decimal point */
    if (i < fmtlen && fmt[i] == '.') {
        has_decimal = true;
        decimal_digits = 0;
        i++;
        while (i < fmtlen && fmt[i] == '#') {
            decimal_digits++;
            i++;
        }
    }

    /* Scientific notation (^^^^) */
    int caret_count = 0;
    while (i < fmtlen && fmt[i] == '^') {
        caret_count++;
        i++;
    }
    if (caret_count >= 4)
        scientific = true;

    /* Trailing sign */
    if (i < fmtlen && fmt[i] == '+') {
        trailing_plus = true;
        i++;
    } else if (i < fmtlen && fmt[i] == '-') {
        trailing_minus = true;
        i++;
    }

    /* Format the number */
    bool negative = val < 0;
    double absval = fabs(val);

    if (scientific) {
        int dec = decimal_digits >= 0 ? decimal_digits : 0;
        char numbuf[64];
        snprintf(numbuf, sizeof(numbuf), "%+.*E", dec, val);

        /* Reformat to match GW-BASIC: sign, digits, E+nn */
        int field_width = total_digits + (has_decimal ? 1 : 0) + 5; /* E+nn + sign */
        if (leading_plus || trailing_plus || trailing_minus)
            field_width++;

        char outbuf[64];
        int oi = 0;
        if (leading_plus)
            outbuf[oi++] = negative ? '-' : '+';
        else if (!trailing_plus && !trailing_minus)
            outbuf[oi++] = negative ? '-' : ' ';

        /* Format mantissa */
        char mantissa[32];
        int exp_val;
        if (val == 0) {
            snprintf(mantissa, sizeof(mantissa), "%.*f", dec, 0.0);
            exp_val = 0;
        } else {
            exp_val = (int)floor(log10(absval));
            double mant = absval / pow(10, exp_val);
            snprintf(mantissa, sizeof(mantissa), "%.*f", dec, mant);
            /* Check for rounding/precision overflow (e.g., 9.95 -> 10.0,
               or log10 imprecision giving mant >= 10) */
            if (mant >= 9.9995 || (mantissa[0] == '1' && mantissa[1] == '0')) {
                exp_val++;
                mant = absval / pow(10, exp_val);
                snprintf(mantissa, sizeof(mantissa), "%.*f", dec, mant);
            }
        }

        for (int j = 0; mantissa[j] && oi < (int)sizeof(outbuf) - 8; j++)
            outbuf[oi++] = mantissa[j];

        snprintf(outbuf + oi, sizeof(outbuf) - oi, "E%+03d", exp_val);
        oi = strlen(outbuf);

        if (trailing_plus)
            outbuf[oi++] = negative ? '-' : '+';
        else if (trailing_minus)
            outbuf[oi++] = negative ? '-' : ' ';
        outbuf[oi] = '\0';

        pu_puts(fp, outbuf);
        return;
    }

    /* Fixed-point formatting */
    int dec = (decimal_digits >= 0) ? decimal_digits : 0;
    char numbuf[64];
    snprintf(numbuf, sizeof(numbuf), "%.*f", dec, absval);

    /* Split into integer and fractional parts */
    char *dot = strchr(numbuf, '.');
    char intpart[48], fracpart[48];
    if (dot) {
        int ilen = dot - numbuf;
        memcpy(intpart, numbuf, ilen);
        intpart[ilen] = '\0';
        strcpy(fracpart, dot + 1);
    } else {
        strcpy(intpart, numbuf);
        fracpart[0] = '\0';
    }

    /* Handle commas */
    char int_with_commas[80];
    if (use_commas) {
        int ilen = strlen(intpart);
        int oi = 0;
        for (int j = 0; j < ilen; j++) {
            if (j > 0 && (ilen - j) % 3 == 0)
                int_with_commas[oi++] = ',';
            int_with_commas[oi++] = intpart[j];
        }
        int_with_commas[oi] = '\0';
    } else {
        strcpy(int_with_commas, intpart);
    }

    /* Build the number string without sign */
    char numstr[160];
    if (has_decimal)
        snprintf(numstr, sizeof(numstr), "%s.%s", int_with_commas, fracpart);
    else
        snprintf(numstr, sizeof(numstr), "%s", int_with_commas);

    /* Calculate total field width for integer part */
    int int_field = total_digits;
    int total_field = int_field + (has_decimal ? 1 + dec : 0);
    /* Commas in the actual formatted number expand the field */
    int actual_commas = (int)strlen(int_with_commas) - (int)strlen(intpart);

    /* Pad the number to fill the field */
    char result[80];
    int numlen = strlen(numstr);
    int padding = total_field + actual_commas - numlen;
    if (padding < 0) padding = 0;

    int ri = 0;

    /* Sign handling */
    bool sign_placed = false;
    if (leading_plus) {
        result[ri++] = negative ? '-' : '+';
        sign_placed = true;
    }

    /* Fill padding */
    char fillch = asterisk_fill ? '*' : ' ';
    bool dollar_placed = false;

    if (numlen > total_field + actual_commas) {
        /* Overflow: print % followed by the number */
        pu_putch(fp, '%');
        if (!sign_placed && !trailing_plus && !trailing_minus) {
            if (negative) pu_putch(fp, '-');
        }
        pu_puts(fp, numstr);
        if (trailing_plus)
            pu_putch(fp, negative ? '-' : '+');
        else if (trailing_minus)
            pu_putch(fp, negative ? '-' : ' ');
        return;
    }

    for (int j = 0; j < padding; j++) {
        if (dollar && !dollar_placed && j == padding - 1) {
            result[ri++] = '$';
            dollar_placed = true;
        } else {
            result[ri++] = fillch;
        }
    }

    /* Place sign if not yet placed and no trailing sign */
    if (!sign_placed && !trailing_plus && !trailing_minus) {
        if (negative) {
            /* Find rightmost fill char and replace with - */
            bool placed = false;
            for (int j = ri - 1; j >= 0; j--) {
                if (result[j] == ' ' || (result[j] == '*' && !asterisk_fill)) {
                    result[j] = '-';
                    placed = true;
                    break;
                }
            }
            if (!placed && ri > 0) {
                /* Put sign before number */
                if (result[0] == ' ' || result[0] == '*')
                    result[0] = '-';
                else {
                    /* Shift everything right */
                    memmove(result + 1, result, ri);
                    result[0] = '-';
                    ri++;
                }
            }
        }
    }

    /* Dollar sign if not placed yet */
    if (dollar && !dollar_placed) {
        if (ri > 0 && (result[ri - 1] == ' ' || result[ri - 1] == '*'))
            result[ri - 1] = '$';
        else
            result[ri++] = '$';
    }

    /* Copy the number digits */
    for (int j = 0; j < numlen && ri < (int)sizeof(result) - 4; j++)
        result[ri++] = numstr[j];

    /* Trailing sign */
    if (trailing_plus)
        result[ri++] = negative ? '-' : '+';
    else if (trailing_minus)
        result[ri++] = negative ? '-' : ' ';
    result[ri] = '\0';

    pu_puts(fp, result);
}

/* Format a string value */
static void format_string(FILE *fp, const char *fmt, int fmtlen, gw_string_t *s)
{
    if (fmtlen == 1 && fmt[0] == '!') {
        /* First character only */
        if (s->len > 0)
            pu_putch(fp, s->data[0]);
        else
            pu_putch(fp, ' ');
    } else if (fmtlen == 1 && fmt[0] == '&') {
        /* Entire string */
        for (int i = 0; i < s->len; i++)
            pu_putch(fp, s->data[i]);
    } else if (fmt[0] == '\\') {
        /* Fixed width: count chars between backslashes, inclusive */
        int width = fmtlen;  /* includes both backslashes */
        for (int i = 0; i < width; i++) {
            if (i < s->len)
                pu_putch(fp, s->data[i]);
            else
                pu_putch(fp, ' ');
        }
    }
}

/*
 * PRINT USING "format"; expr [, expr ...]
 * fp: output FILE* (NULL = screen via HAL)
 * Called after USING token has been consumed.
 */
void gw_print_using(FILE *fp)
{
    gw_skip_spaces();
    gw_value_t fmt_val = gw_eval_str();
    char *fmt = gw_str_to_cstr(&fmt_val.sval);
    int fmtlen = fmt_val.sval.len;
    gw_str_free(&fmt_val.sval);

    gw_skip_spaces();
    if (gw_chrgot() == ';')
        gw_chrget();

    int fi = 0;  /* format index */
    bool had_semicolon = false;

    for (;;) {
        gw_skip_spaces();
        uint8_t tok = gw_chrgot();
        if (tok == 0 || tok == ':' || tok == TOK_ELSE)
            break;

        /* Reset format position if we've gone past the end */
        if (fi >= fmtlen)
            fi = 0;

        /* Output literal characters until we hit a format spec */
        while (fi < fmtlen) {
            char ch = fmt[fi];

            /* Literal escape */
            if (ch == '_') {
                fi++;
                if (fi < fmtlen)
                    pu_putch(fp, fmt[fi++]);
                continue;
            }

            /* Numeric format start */
            if (ch == '#' || ch == '+' || ch == '.' ||
                (ch == '*' && fi + 1 < fmtlen && fmt[fi + 1] == '*') ||
                (ch == '$' && fi + 1 < fmtlen && fmt[fi + 1] == '$'))
                break;

            /* String format start */
            if (ch == '!' || ch == '&' || ch == '\\')
                break;

            /* Regular character - output as literal */
            pu_putch(fp, ch);
            fi++;
        }

        if (fi >= fmtlen) {
            /* Check if there are more values to format */
            gw_skip_spaces();
            tok = gw_chrgot();
            if (tok == 0 || tok == ':' || tok == TOK_ELSE)
                break;
            /* More values: reset format and re-enter the loop */
            fi = 0;
            continue;
        }

        /* Determine format spec type and length */
        char ch = fmt[fi];

        /* String formats */
        if (ch == '!') {
            gw_value_t v = gw_eval_str();
            format_string(fp, "!", 1, &v.sval);
            gw_str_free(&v.sval);
            fi++;
        } else if (ch == '&') {
            gw_value_t v = gw_eval_str();
            format_string(fp, "&", 1, &v.sval);
            gw_str_free(&v.sval);
            fi++;
        } else if (ch == '\\') {
            /* Count to closing backslash */
            int start = fi;
            fi++;
            while (fi < fmtlen && fmt[fi] != '\\')
                fi++;
            if (fi < fmtlen) fi++;  /* skip closing backslash */
            int width = fi - start;
            gw_value_t v = gw_eval_str();
            format_string(fp, fmt + start, width, &v.sval);
            gw_str_free(&v.sval);
        } else {
            /* Numeric format - collect the entire spec */
            int start = fi;

            /* Leading + */
            if (fi < fmtlen && fmt[fi] == '+') fi++;

            /* ** or **$ or $$ */
            if (fi + 1 < fmtlen && fmt[fi] == '*' && fmt[fi + 1] == '*') {
                fi += 2;
                if (fi < fmtlen && fmt[fi] == '$') fi++;
            } else if (fi + 1 < fmtlen && fmt[fi] == '$' && fmt[fi + 1] == '$') {
                fi += 2;
            }

            /* # digits and commas */
            while (fi < fmtlen && (fmt[fi] == '#' || fmt[fi] == ','))
                fi++;

            /* Decimal point and fraction digits */
            if (fi < fmtlen && fmt[fi] == '.') {
                fi++;
                while (fi < fmtlen && fmt[fi] == '#')
                    fi++;
            }

            /* Carets for scientific notation */
            while (fi < fmtlen && fmt[fi] == '^')
                fi++;

            /* Trailing sign */
            if (fi < fmtlen && (fmt[fi] == '+' || fmt[fi] == '-'))
                fi++;

            gw_value_t v = gw_eval_num();
            double dv = gw_to_dbl(&v);
            format_number(fp, fmt + start, fi - start, dv);
        }

        /* Check for separator */
        gw_skip_spaces();
        had_semicolon = false;
        if (gw_chrgot() == ';') {
            gw_chrget();
            had_semicolon = true;
        } else if (gw_chrgot() == ',') {
            gw_chrget();
            had_semicolon = true;
        }

        if (!had_semicolon)
            break;
    }

    /* Output remaining literal chars from format */
    while (fi < fmtlen) {
        if (fmt[fi] == '_') {
            fi++;
            if (fi < fmtlen)
                pu_putch(fp, fmt[fi++]);
        } else {
            pu_putch(fp, fmt[fi++]);
        }
    }

    if (!had_semicolon)
        pu_putch(fp, '\n');

    if (fp)
        fflush(fp);

    free(fmt);
}
