#include "gwbasic.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/*
 * Variable storage - reimplements PTRGET from GWMAIN.ASM.
 * Variable names: first 2 chars significant, case-insensitive.
 * Type determined by suffix (%, !, #, $) or def_type table.
 * Variables with different type suffixes are distinct (A% != A!).
 */

gw_valtype_t gw_parse_varname(char name_out[2])
{
    gw_skip_spaces();
    uint8_t ch = gw_chrgot();
    if (!gw_is_letter(ch))
        gw_error(ERR_SN);

    name_out[0] = toupper(ch);
    name_out[1] = '\0';
    gw_chrget();

    ch = gw_chrgot();
    if (gw_is_letter(ch) || gw_is_digit(ch)) {
        name_out[1] = toupper(ch);
        gw_chrget();
        /* Skip remaining alphanumeric chars (not significant) */
        while (gw_is_letter(gw_chrgot()) || gw_is_digit(gw_chrgot()))
            gw_chrget();
    }

    /* Check for type suffix */
    ch = gw_chrgot();
    if (ch == '%') { gw_chrget(); return VT_INT; }
    if (ch == '!') { gw_chrget(); return VT_SNG; }
    if (ch == '#') { gw_chrget(); return VT_DBL; }
    if (ch == '$') { gw_chrget(); return VT_STR; }

    /* Use DEFTBL */
    int idx = name_out[0] - 'A';
    return gw.def_type[idx];
}

var_entry_t *gw_var_find_or_create(const char name[2], gw_valtype_t type)
{
    for (int i = 0; i < gw.var_count; i++) {
        if (gw.vars[i].name[0] == name[0] && gw.vars[i].name[1] == name[1]
            && gw.vars[i].type == type)
            return &gw.vars[i];
    }

    if (gw.var_count >= 256)
        gw_error(ERR_OM);

    var_entry_t *v = &gw.vars[gw.var_count++];
    v->name[0] = name[0];
    v->name[1] = name[1];
    v->type = type;
    memset(&v->val, 0, sizeof(v->val));
    v->val.type = type;
    if (type == VT_STR) {
        v->val.sval.len = 0;
        v->val.sval.data = NULL;
    }
    return v;
}

void gw_var_assign(var_entry_t *var, gw_value_t *val)
{
    if (var->type == VT_STR) {
        gw_str_free(&var->val.sval);
        if (val->type != VT_STR)
            gw_error(ERR_TM);
        var->val.sval = val->sval;  /* take ownership */
    } else {
        if (val->type == VT_STR)
            gw_error(ERR_TM);
        switch (var->type) {
        case VT_INT: var->val.ival = gw_to_int(val); break;
        case VT_SNG: var->val.fval = gw_to_sng(val); break;
        case VT_DBL: var->val.dval = gw_to_dbl(val); break;
        default: break;
        }
        var->val.type = var->type;
    }
}

void gw_vars_clear(void)
{
    for (int i = 0; i < gw.var_count; i++) {
        if (gw.vars[i].type == VT_STR)
            gw_str_free(&gw.vars[i].val.sval);
    }
    gw.var_count = 0;
}

void gw_stmt_deftype(gw_valtype_t type)
{
    for (;;) {
        gw_skip_spaces();
        uint8_t ch = gw_chrgot();
        if (!gw_is_letter(ch))
            gw_error(ERR_SN);
        int first = toupper(ch) - 'A';
        gw_chrget();

        int last = first;
        gw_skip_spaces();
        if (gw_chrgot() == TOK_MINUS) {
            gw_chrget();
            gw_skip_spaces();
            ch = gw_chrgot();
            if (!gw_is_letter(ch))
                gw_error(ERR_SN);
            last = toupper(ch) - 'A';
            gw_chrget();
        }

        for (int i = first; i <= last; i++)
            gw.def_type[i] = type;

        gw_skip_spaces();
        if (gw_chrgot() != ',')
            break;
        gw_chrget();
    }
}

void gw_stmt_swap(void)
{
    /* Parse first variable */
    char name1[2];
    gw_valtype_t type1 = gw_parse_varname(name1);

    /* Check if it's an array element */
    gw_skip_spaces();
    gw_value_t *arr1 = NULL;
    var_entry_t *var1 = NULL;
    if (gw_chrgot() == '(') {
        arr1 = gw_array_element(name1, type1);
    } else {
        var1 = gw_var_find_or_create(name1, type1);
    }

    gw_skip_spaces();
    gw_expect(',');

    /* Parse second variable */
    char name2[2];
    gw_valtype_t type2 = gw_parse_varname(name2);

    gw_skip_spaces();
    gw_value_t *arr2 = NULL;
    var_entry_t *var2 = NULL;
    if (gw_chrgot() == '(') {
        arr2 = gw_array_element(name2, type2);
    } else {
        var2 = gw_var_find_or_create(name2, type2);
    }

    if (type1 != type2)
        gw_error(ERR_TM);

    gw_value_t *p1 = arr1 ? arr1 : &var1->val;
    gw_value_t *p2 = arr2 ? arr2 : &var2->val;

    gw_value_t tmp = *p1;
    *p1 = *p2;
    *p2 = tmp;
}
