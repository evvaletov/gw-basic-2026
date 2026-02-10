#include "gwbasic.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * INPUT and LINE INPUT statements.
 * INPUT: prompt, read comma-separated values into variables.
 * LINE INPUT: read entire line as string.
 */

static char *read_input_line(void)
{
    static char buf[256];
    if (gw_hal) gw_hal->disable_raw();
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        if (gw_hal) gw_hal->enable_raw();
        return NULL;
    }
    if (gw_hal) gw_hal->enable_raw();
    int len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return buf;
}

void gw_stmt_input(void)
{
    gw_skip_spaces();

    /* Optional semicolon after INPUT (suppresses CR on prompt) */
    /* INPUT [;]["prompt";] var, var, ... */
    bool no_cr = false;
    if (gw_chrgot() == ';') {
        no_cr = true;
        gw_chrget();
        gw_skip_spaces();
    }
    (void)no_cr;

    /* Check for prompt string */
    if (gw_chrgot() == '"') {
        gw.text_ptr++;
        while (*gw.text_ptr && *gw.text_ptr != '"') {
            if (gw_hal) gw_hal->putch(*gw.text_ptr);
            else putchar(*gw.text_ptr);
            gw.text_ptr++;
        }
        if (*gw.text_ptr == '"')
            gw.text_ptr++;

        gw_skip_spaces();
        if (gw_chrgot() == ';') {
            gw_chrget();
        } else if (gw_chrgot() == ',') {
            gw_chrget();
        }
    }

retry:;
    /* Print "? " */
    if (gw_hal) gw_hal->puts("? ");
    else fputs("? ", stdout);
    fflush(stdout);

    char *line = read_input_line();
    if (!line) return;

    const char *p = line;

    /* Parse variable list and assign values */
    int var_idx = 0;
    for (;;) {
        gw_skip_spaces();
        uint8_t ch = gw_chrgot();
        if (ch == 0 || ch == ':')
            break;

        char name[2];
        gw_valtype_t type = gw_parse_varname(name);

        gw_skip_spaces();
        var_entry_t *var;
        gw_value_t *arr_elem = NULL;
        if (gw_chrgot() == '(') {
            arr_elem = gw_array_element(name, type);
        } else {
            var = gw_var_find_or_create(name, type);
        }

        /* Skip whitespace in input */
        while (*p == ' ') p++;

        /* Parse value from input line */
        gw_value_t val;
        if (type == VT_STR) {
            /* Read string: until comma or end of line */
            const char *start = p;
            if (*p == '"') {
                /* Quoted string */
                p++;
                start = p;
                while (*p && *p != '"') p++;
                int len = p - start;
                val.type = VT_STR;
                val.sval = gw_str_alloc(len);
                memcpy(val.sval.data, start, len);
                if (*p == '"') p++;
            } else {
                while (*p && *p != ',') p++;
                int len = p - start;
                /* Trim trailing spaces */
                while (len > 0 && start[len - 1] == ' ') len--;
                val.type = VT_STR;
                val.sval = gw_str_alloc(len);
                memcpy(val.sval.data, start, len);
            }
        } else {
            char *end;
            double d = strtod(p, &end);
            if (end == p) d = 0;
            p = end;
            val.type = VT_DBL;
            val.dval = d;
        }

        if (arr_elem) {
            if (type == VT_STR) {
                gw_str_free(&arr_elem->sval);
                arr_elem->sval = val.sval;
                arr_elem->type = VT_STR;
            } else {
                switch (type) {
                case VT_INT: arr_elem->ival = gw_to_int(&val); break;
                case VT_SNG: arr_elem->fval = gw_to_sng(&val); break;
                case VT_DBL: arr_elem->dval = gw_to_dbl(&val); break;
                default: break;
                }
                arr_elem->type = type;
            }
        } else {
            gw_var_assign(var, &val);
        }

        if (*p == ',') p++;
        var_idx++;

        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
        } else {
            break;
        }
    }

    /* If we have more variables than input, redo */
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        if (gw_hal) gw_hal->puts("?Redo from start\n");
        else fputs("?Redo from start\n", stdout);
        /* Need to re-read, but for simplicity continue */
        goto retry;
    }
}

void gw_stmt_line_input(void)
{
    gw_skip_spaces();

    /* LINE INPUT [;]["prompt";] string-variable */
    if (gw_chrgot() == ';') {
        gw_chrget();
        gw_skip_spaces();
    }

    /* Check for prompt string */
    if (gw_chrgot() == '"') {
        gw.text_ptr++;
        while (*gw.text_ptr && *gw.text_ptr != '"') {
            if (gw_hal) gw_hal->putch(*gw.text_ptr);
            else putchar(*gw.text_ptr);
            gw.text_ptr++;
        }
        if (*gw.text_ptr == '"')
            gw.text_ptr++;
        gw_skip_spaces();
        if (gw_chrgot() == ';')
            gw_chrget();
    }

    char name[2];
    gw_valtype_t type = gw_parse_varname(name);
    if (type != VT_STR)
        gw_error(ERR_TM);

    fflush(stdout);
    char *line = read_input_line();
    if (!line) line = "";

    gw_value_t val;
    val.type = VT_STR;
    val.sval = gw_str_from_cstr(line);

    gw_skip_spaces();
    var_entry_t *var;
    if (gw_chrgot() == '(') {
        gw_value_t *elem = gw_array_element(name, type);
        gw_str_free(&elem->sval);
        elem->sval = val.sval;
        elem->type = VT_STR;
    } else {
        var = gw_var_find_or_create(name, type);
        gw_var_assign(var, &val);
    }
}
