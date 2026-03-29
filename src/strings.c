#include "gwbasic.h"
#include "strpool.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

gw_string_t gw_str_alloc(int len)
{
    gw_string_t s;
    if (len < 0 || len > 255)
        gw_error(ERR_LS);
    s.len = len;
    s.data = len > 0 ? strpool_alloc(len) : NULL;
    return s;
}

gw_string_t gw_str_from_cstr(const char *cs)
{
    int len = strlen(cs);
    if (len > 255) len = 255;
    gw_string_t s = gw_str_alloc(len);
    memcpy(s.data, cs, len);
    return s;
}

gw_string_t gw_str_copy(gw_string_t *s)
{
    gw_string_t n = gw_str_alloc(s->len);
    if (s->len > 0)
        memcpy(n.data, s->data, s->len);
    return n;
}

void gw_str_free(gw_string_t *s)
{
    /* Pool strings are reclaimed by GC, not individually freed */
    s->data = NULL;
    s->len = 0;
}

char *gw_str_to_cstr(gw_string_t *s)
{
    char *cs = malloc(s->len + 1);
    if (!cs) gw_error(ERR_OS);
    memcpy(cs, s->data, s->len);
    cs[s->len] = '\0';
    return cs;
}

gw_value_t gw_fn_len(gw_value_t *s)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    gw_value_t r;
    r.type = VT_INT;
    r.ival = s->sval.len;
    gw_str_free(&s->sval);
    return r;
}

gw_value_t gw_fn_left(gw_value_t *s, int n)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    if (n < 0) gw_error(ERR_FC);
    if (n > s->sval.len) n = s->sval.len;

    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_alloc(n);
    memcpy(r.sval.data, s->sval.data, n);
    gw_str_free(&s->sval);
    return r;
}

gw_value_t gw_fn_right(gw_value_t *s, int n)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    if (n < 0) gw_error(ERR_FC);
    if (n > s->sval.len) n = s->sval.len;

    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_alloc(n);
    memcpy(r.sval.data, s->sval.data + s->sval.len - n, n);
    gw_str_free(&s->sval);
    return r;
}

gw_value_t gw_fn_mid(gw_value_t *s, int start, int len)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    if (start < 1) gw_error(ERR_FC);
    start--;  /* convert to 0-based */

    int avail = s->sval.len - start;
    if (avail < 0) avail = 0;
    if (len > avail) len = avail;

    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_alloc(len);
    if (len > 0)
        memcpy(r.sval.data, s->sval.data + start, len);
    gw_str_free(&s->sval);
    return r;
}

gw_value_t gw_fn_chr(int code)
{
    if (code < 0 || code > 255) gw_error(ERR_FC);
    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_alloc(1);
    r.sval.data[0] = (char)code;
    return r;
}

gw_value_t gw_fn_asc(gw_value_t *s)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    if (s->sval.len == 0) gw_error(ERR_FC);
    gw_value_t r;
    r.type = VT_INT;
    r.ival = (uint8_t)s->sval.data[0];
    gw_str_free(&s->sval);
    return r;
}

gw_value_t gw_fn_val(gw_value_t *s)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    char *cs = gw_str_to_cstr(&s->sval);
    gw_str_free(&s->sval);

    /* Skip leading spaces */
    char *p = cs;
    while (*p == ' ') p++;

    char *end;
    double d = strtod(p, &end);
    free(cs);

    /* Check for &H hex and &O octal prefix */
    gw_value_t r;
    r.type = VT_DBL;
    r.dval = d;
    return r;
}

gw_value_t gw_fn_str(gw_value_t *v)
{
    if (v->type == VT_STR) gw_error(ERR_TM);
    char buf[32];
    gw_format_number(v, buf, sizeof(buf));
    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_from_cstr(buf);
    return r;
}

gw_value_t gw_fn_space(int n)
{
    if (n < 0 || n > 255) gw_error(ERR_FC);
    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_alloc(n);
    memset(r.sval.data, ' ', n);
    return r;
}

gw_value_t gw_fn_strings(int n, int code)
{
    if (n < 0 || n > 255) gw_error(ERR_FC);
    if (code < 0 || code > 255) gw_error(ERR_FC);
    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_alloc(n);
    memset(r.sval.data, (char)code, n);
    return r;
}

gw_value_t gw_fn_instr(int start, gw_value_t *haystack, gw_value_t *needle)
{
    if (haystack->type != VT_STR || needle->type != VT_STR)
        gw_error(ERR_TM);
    if (start < 1) gw_error(ERR_FC);

    gw_value_t r;
    r.type = VT_INT;
    r.ival = 0;

    if (start > haystack->sval.len || needle->sval.len == 0) {
        if (needle->sval.len == 0 && start <= haystack->sval.len + 1)
            r.ival = start;
        gw_str_free(&haystack->sval);
        gw_str_free(&needle->sval);
        return r;
    }

    for (int i = start - 1; i <= haystack->sval.len - needle->sval.len; i++) {
        if (memcmp(haystack->sval.data + i, needle->sval.data, needle->sval.len) == 0) {
            r.ival = i + 1;
            break;
        }
    }

    gw_str_free(&haystack->sval);
    gw_str_free(&needle->sval);
    return r;
}

gw_value_t gw_fn_hex(int16_t n)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%X", (uint16_t)n);
    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_from_cstr(buf);
    return r;
}

gw_value_t gw_fn_oct(int16_t n)
{
    char buf[8];
    snprintf(buf, sizeof(buf), "%o", (uint16_t)n);
    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_from_cstr(buf);
    return r;
}

gw_value_t gw_str_concat(gw_value_t *a, gw_value_t *b)
{
    if (a->type != VT_STR || b->type != VT_STR)
        gw_error(ERR_TM);

    int newlen = a->sval.len + b->sval.len;
    if (newlen > 255)
        gw_error(ERR_LS);

    gw_value_t r;
    r.type = VT_STR;
    r.sval = gw_str_alloc(newlen);
    memcpy(r.sval.data, a->sval.data, a->sval.len);
    memcpy(r.sval.data + a->sval.len, b->sval.data, b->sval.len);
    gw_str_free(&a->sval);
    gw_str_free(&b->sval);
    return r;
}
