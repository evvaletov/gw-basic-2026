#include "gwbasic.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/*
 * Array support - reimplements DIMCON/VARGET from GWMAIN.ASM.
 * Column-major subscript calculation (leftmost varies fastest).
 * Default bounds 0-10 (11 elements) if not DIMmed.
 */

static array_entry_t *find_array(const char name[2], gw_valtype_t type)
{
    for (int i = 0; i < gw.array_count; i++) {
        if (gw.arrays[i].name[0] == name[0] && gw.arrays[i].name[1] == name[1]
            && gw.arrays[i].type == type)
            return &gw.arrays[i];
    }
    return NULL;
}

static array_entry_t *create_array(const char name[2], gw_valtype_t type,
                                   int ndims, const int *dims)
{
    if (gw.array_count >= 64)
        gw_error(ERR_OM);

    array_entry_t *a = &gw.arrays[gw.array_count++];
    a->name[0] = name[0];
    a->name[1] = name[1];
    a->type = type;
    a->ndims = ndims;

    int total = 1;
    for (int i = 0; i < ndims; i++) {
        a->dims[i] = dims[i];
        total *= (dims[i] - gw.option_base + 1);
    }
    a->total_elements = total;
    a->data = calloc(total, sizeof(gw_value_t));
    if (!a->data)
        gw_error(ERR_OM);

    for (int i = 0; i < total; i++)
        a->data[i].type = type;

    return a;
}

/* Parse subscripts and return pointer to element */
gw_value_t *gw_array_element(const char name[2], gw_valtype_t type)
{
    gw_expect('(');

    int subs[8];
    int nsubs = 0;
    for (;;) {
        if (nsubs >= 8)
            gw_error(ERR_BS);
        subs[nsubs++] = gw_eval_int();
        gw_skip_spaces();
        if (gw_chrgot() != ',')
            break;
        gw_chrget();
    }
    gw_expect_rparen();

    array_entry_t *a = find_array(name, type);
    if (!a) {
        /* Auto-DIM with default bounds 0-10 */
        int dims[8];
        for (int i = 0; i < nsubs; i++)
            dims[i] = 10;
        a = create_array(name, type, nsubs, dims);
    }

    if (nsubs != a->ndims)
        gw_error(ERR_BS);

    /* Column-major index calculation */
    int index = 0;
    int multiplier = 1;
    for (int i = 0; i < nsubs; i++) {
        int s = subs[i] - gw.option_base;
        int dim_size = a->dims[i] - gw.option_base + 1;
        if (s < 0 || s >= dim_size)
            gw_error(ERR_BS);
        index += s * multiplier;
        multiplier *= dim_size;
    }

    return &a->data[index];
}

void gw_stmt_dim(void)
{
    for (;;) {
        char name[2];
        gw_valtype_t type = gw_parse_varname(name);

        gw_skip_spaces();
        gw_expect('(');

        int dims[8];
        int ndims = 0;
        for (;;) {
            if (ndims >= 8)
                gw_error(ERR_BS);
            dims[ndims++] = gw_eval_int();
            gw_skip_spaces();
            if (gw_chrgot() != ',')
                break;
            gw_chrget();
        }
        gw_expect_rparen();

        if (find_array(name, type))
            gw_error(ERR_DD);

        create_array(name, type, ndims, dims);

        gw_skip_spaces();
        if (gw_chrgot() != ',')
            break;
        gw_chrget();
    }
}

void gw_stmt_erase(void)
{
    for (;;) {
        char name[2];
        gw_valtype_t type = gw_parse_varname(name);

        array_entry_t *a = find_array(name, type);
        if (!a)
            gw_error(ERR_FC);

        if (a->type == VT_STR) {
            for (int i = 0; i < a->total_elements; i++)
                gw_str_free(&a->data[i].sval);
        }
        free(a->data);

        /* Remove from array by swapping with last */
        int idx = a - gw.arrays;
        gw.arrays[idx] = gw.arrays[--gw.array_count];

        gw_skip_spaces();
        if (gw_chrgot() != ',')
            break;
        gw_chrget();
    }
}

void gw_stmt_option(void)
{
    /* OPTION BASE 0|1 */
    gw_skip_spaces();
    /* Expect "BASE" - it tokenizes as a keyword or letters */
    uint8_t ch = gw_chrgot();
    /* Skip the word BASE (may be tokenized as letters) */
    if (gw_is_letter(ch)) {
        while (gw_is_letter(gw_chrgot()))
            gw_chrget();
    }
    gw_skip_spaces();
    int base = gw_eval_int();
    if (base != 0 && base != 1)
        gw_error(ERR_FC);
    if (gw.array_count > 0)
        gw_error(ERR_FC);  /* can't change after arrays exist */
    gw.option_base = base;
}

void gw_arrays_clear(void)
{
    for (int i = 0; i < gw.array_count; i++) {
        if (gw.arrays[i].type == VT_STR) {
            for (int j = 0; j < gw.arrays[i].total_elements; j++)
                gw_str_free(&gw.arrays[i].data[j].sval);
        }
        free(gw.arrays[i].data);
    }
    gw.array_count = 0;
}
