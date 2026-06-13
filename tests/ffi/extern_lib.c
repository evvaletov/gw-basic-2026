/* C functions called from extern_demo.bas via '$EXTERN pragmas. */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>

int16_t Cadd(int16_t a, int16_t b)   { return (int16_t)(a + b); }
double  Chypot(double a, double b)   { return hypot(a, b); }
int16_t Getn(void)                   { return 42; }

const char *Greet(const char *who)
{
    static char buf[128];
    snprintf(buf, sizeof buf, "Hello, %s!", who);
    return buf;
}

/* Returns its own argument pointer after modifying it in place.  Exercises the
 * string-return path where the result aliases a C-string arg temporary — the
 * codegen must copy the result before freeing that temporary. */
const char *Upcase(const char *s)
{
    for (char *p = (char *)s; *p; p++)
        *p = (char)toupper((unsigned char)*p);
    return s;
}

/* Aliased C symbol: BASIC calls it `Rev`, the '$EXTERN pragma maps that to the
 * underscore-containing C name `c_rev`. Reverses its argument in place. */
const char *c_rev(const char *s)
{
    static char buf[128];
    size_t n = strlen(s), i;
    for (i = 0; i < n && i < sizeof buf - 1; i++)
        buf[i] = s[n - 1 - i];
    buf[i] = 0;
    return buf;
}
