#include "gwbasic.h"
#include "graphics.h"
#include "sound.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

/*
 * Execution loop and control flow - the heart of the interpreter.
 * Reimplements NEWSTT, GOTO, GOSUB, FOR/NEXT, IF/THEN/ELSE, etc.
 */

jmp_buf gw_run_jmp;

/* ================================================================
 * Program Storage
 * ================================================================ */

void gw_store_line(uint16_t num, uint8_t *tokens, int len)
{
    /* Delete existing line with this number first */
    gw_delete_line(num);

    if (len == 0)
        return;  /* just a deletion */

    program_line_t *line = malloc(sizeof(program_line_t));
    if (!line) gw_error(ERR_OM);
    line->num = num;
    line->len = len;
    line->tokens = malloc(len + 1);
    if (!line->tokens) { free(line); gw_error(ERR_OM); }
    memcpy(line->tokens, tokens, len);
    line->tokens[len] = 0;

    /* Insert in sorted order */
    program_line_t **pp = &gw.prog_head;
    while (*pp && (*pp)->num < num)
        pp = &(*pp)->next;
    line->next = *pp;
    *pp = line;
}

void gw_delete_line(uint16_t num)
{
    program_line_t **pp = &gw.prog_head;
    while (*pp) {
        if ((*pp)->num == num) {
            program_line_t *del = *pp;
            *pp = del->next;
            free(del->tokens);
            free(del);
            return;
        }
        pp = &(*pp)->next;
    }
}

program_line_t *gw_find_line(uint16_t num)
{
    program_line_t *p = gw.prog_head;
    while (p) {
        if (p->num == num)
            return p;
        if (p->num > num)
            break;
        p = p->next;
    }
    return NULL;
}

void gw_free_program(void)
{
    program_line_t *p = gw.prog_head;
    while (p) {
        program_line_t *next = p->next;
        free(p->tokens);
        free(p);
        p = next;
    }
    gw.prog_head = NULL;
}

/* ================================================================
 * LIST
 * ================================================================ */

static void stmt_list(void)
{
    gw_skip_spaces();
    uint16_t start = 0, end = 65535;

    if (gw_chrgot() != 0 && gw_chrgot() != ':') {
        /* Check for -end */
        if (gw_chrgot() == TOK_MINUS) {
            gw_chrget();
            end = gw_eval_uint16();
        } else {
            start = gw_eval_uint16();
            end = start;
            gw_skip_spaces();
            if (gw_chrgot() == TOK_MINUS) {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() != 0 && gw_chrgot() != ':')
                    end = gw_eval_uint16();
                else
                    end = 65535;
            }
        }
    }

    char listbuf[512];
    program_line_t *p = gw.prog_head;
    while (p) {
        if (p->num >= start && p->num <= end) {
            gw_list_line(p->tokens, p->len, listbuf, sizeof(listbuf));
            char line[560];
            snprintf(line, sizeof(line), "%u %s\n", p->num, listbuf);
            if (gw_hal) gw_hal->puts(line);
            else fputs(line, stdout);
        }
        if (p->num > end)
            break;
        p = p->next;
    }
}

/* ================================================================
 * DATA/READ/RESTORE
 * ================================================================ */

/* Skip a DATA statement's items (used during execution) */
static void skip_data(void)
{
    while (*gw.text_ptr && *gw.text_ptr != ':')
        gw.text_ptr++;
}

/* Find the next DATA statement in the program starting from current data pointer */
static bool advance_data_ptr(void)
{
    /* If no data pointer set, start from program beginning */
    if (!gw.data_line_ptr) {
        gw.data_line_ptr = gw.prog_head;
        if (!gw.data_line_ptr) return false;
        gw.data_ptr = gw.data_line_ptr->tokens;
    }

    for (;;) {
        /* Scan current line for DATA token */
        while (*gw.data_ptr) {
            if (*gw.data_ptr == TOK_DATA) {
                gw.data_ptr++;  /* skip DATA token */
                return true;
            }
            /* Skip string literals */
            if (*gw.data_ptr == '"') {
                gw.data_ptr++;
                while (*gw.data_ptr && *gw.data_ptr != '"')
                    gw.data_ptr++;
                if (*gw.data_ptr == '"')
                    gw.data_ptr++;
                continue;
            }
            /* Skip REM to end of line */
            if (*gw.data_ptr == TOK_REM || *gw.data_ptr == TOK_SQUOTE)
                break;
            gw.data_ptr++;
        }

        /* Move to next program line */
        gw.data_line_ptr = gw.data_line_ptr->next;
        if (!gw.data_line_ptr)
            return false;
        gw.data_ptr = gw.data_line_ptr->tokens;
    }
}

/* Read one data item as a string (raw text from DATA statement) */
static void read_data_item(char *buf, int bufsize)
{
    if (!gw.data_ptr || !*gw.data_ptr || *gw.data_ptr == ':') {
        if (!advance_data_ptr())
            gw_error(ERR_OD);
    }

    /* Skip spaces */
    while (*gw.data_ptr == ' ') gw.data_ptr++;

    int i = 0;
    if (*gw.data_ptr == '"') {
        /* Quoted string */
        gw.data_ptr++;
        while (*gw.data_ptr && *gw.data_ptr != '"' && i < bufsize - 1)
            buf[i++] = *gw.data_ptr++;
        if (*gw.data_ptr == '"') gw.data_ptr++;
    } else {
        /* Unquoted: read until comma, colon, or end */
        while (*gw.data_ptr && *gw.data_ptr != ',' && *gw.data_ptr != ':'
               && i < bufsize - 1) {
            buf[i++] = *gw.data_ptr++;
        }
        /* Trim trailing spaces */
        while (i > 0 && buf[i - 1] == ' ') i--;
    }
    buf[i] = '\0';

    /* Skip comma separator */
    while (*gw.data_ptr == ' ') gw.data_ptr++;
    if (*gw.data_ptr == ',')
        gw.data_ptr++;
}

static void stmt_read(void)
{
    for (;;) {
        gw_skip_spaces();

        char name[2];
        gw_valtype_t type = gw_parse_varname(name);

        gw_skip_spaces();
        gw_value_t *arr_elem = NULL;
        var_entry_t *var = NULL;
        if (gw_chrgot() == '(') {
            arr_elem = gw_array_element(name, type);
        } else {
            var = gw_var_find_or_create(name, type);
        }

        char item[256];
        read_data_item(item, sizeof(item));

        gw_value_t val;
        if (type == VT_STR) {
            val.type = VT_STR;
            val.sval = gw_str_from_cstr(item);
        } else {
            char *end;
            double d = strtod(item, &end);
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

        gw_skip_spaces();
        if (gw_chrgot() != ',')
            break;
        gw_chrget();
    }
}

static void stmt_restore(void)
{
    gw_skip_spaces();
    if (gw_chrgot() != 0 && gw_chrgot() != ':') {
        uint16_t num = gw_eval_uint16();
        gw.data_line_ptr = gw_find_line(num);
        if (!gw.data_line_ptr)
            gw_error(ERR_UL);
        gw.data_ptr = gw.data_line_ptr->tokens;
    } else {
        gw.data_line_ptr = NULL;
        gw.data_ptr = NULL;
    }
}

/* ================================================================
 * IF/THEN/ELSE - skip_to_else_or_eol
 * ================================================================ */

static void skip_to_else_or_eol(void)
{
    int depth = 0;
    for (;;) {
        uint8_t ch = *gw.text_ptr;
        if (ch == 0) return;

        if (ch == TOK_IF) {
            depth++;
            gw.text_ptr++;
            continue;
        }
        if (ch == TOK_ELSE) {
            if (depth == 0) {
                gw.text_ptr++;  /* consume ELSE */
                return;
            }
            depth--;
            gw.text_ptr++;
            continue;
        }

        /* Skip embedded constants */
        if (ch == TOK_INT2)      { gw.text_ptr += 3; continue; }
        if (ch == TOK_INT1)      { gw.text_ptr += 2; continue; }
        if (ch >= 0x11 && ch <= 0x1A) { gw.text_ptr++; continue; }
        if (ch == TOK_CONST_SNG) { gw.text_ptr += 5; continue; }
        if (ch == TOK_CONST_DBL) { gw.text_ptr += 9; continue; }

        /* Skip string literals */
        if (ch == '"') {
            gw.text_ptr++;
            while (*gw.text_ptr && *gw.text_ptr != '"')
                gw.text_ptr++;
            if (*gw.text_ptr == '"')
                gw.text_ptr++;
            continue;
        }

        /* Skip REM to end of line */
        if (ch == TOK_REM || ch == TOK_SQUOTE) {
            while (*gw.text_ptr) gw.text_ptr++;
            return;
        }

        gw.text_ptr++;
    }
}

/* ================================================================
 * DEF FN
 * ================================================================ */

static void stmt_def_fn(void)
{
    /* DEF FN<letter>[(<param>)] = <expr> */
    gw_skip_spaces();
    if (gw_chrgot() != TOK_FN)
        gw_error(ERR_SN);
    gw_chrget();

    gw_skip_spaces();
    if (!gw_is_letter(gw_chrgot()))
        gw_error(ERR_SN);
    int fn_idx = toupper(gw_chrgot()) - 'A';
    gw_chrget();
    /* Skip rest of function name */
    while (gw_is_letter(gw_chrgot()) || gw_is_digit(gw_chrgot()))
        gw_chrget();

    /* Check type suffix on function name */
    gw_valtype_t ret_type = gw.def_type[fn_idx];
    if (gw_chrgot() == '%') { ret_type = VT_INT; gw_chrget(); }
    else if (gw_chrgot() == '!') { ret_type = VT_SNG; gw_chrget(); }
    else if (gw_chrgot() == '#') { ret_type = VT_DBL; gw_chrget(); }
    else if (gw_chrgot() == '$') { ret_type = VT_STR; gw_chrget(); }

    fn_def_t *fn = &gw.fn_defs[fn_idx];
    fn->defined = true;
    fn->ret_type = ret_type;
    fn->param_name[0] = '\0';
    fn->param_name[1] = '\0';
    fn->param_type = VT_SNG;

    gw_skip_spaces();
    if (gw_chrgot() == '(') {
        gw_chrget();
        gw_skip_spaces();
        /* Parse parameter name */
        if (gw_is_letter(gw_chrgot())) {
            fn->param_name[0] = toupper(gw_chrgot());
            gw_chrget();
            if (gw_is_letter(gw_chrgot()) || gw_is_digit(gw_chrgot())) {
                fn->param_name[1] = toupper(gw_chrgot());
                gw_chrget();
                while (gw_is_letter(gw_chrgot()) || gw_is_digit(gw_chrgot()))
                    gw_chrget();
            }
            /* Param type suffix */
            if (gw_chrgot() == '%') { fn->param_type = VT_INT; gw_chrget(); }
            else if (gw_chrgot() == '!') { fn->param_type = VT_SNG; gw_chrget(); }
            else if (gw_chrgot() == '#') { fn->param_type = VT_DBL; gw_chrget(); }
            else if (gw_chrgot() == '$') { fn->param_type = VT_STR; gw_chrget(); }
            else { fn->param_type = gw.def_type[fn->param_name[0] - 'A']; }
        }
        gw_expect_rparen();
    }

    gw_skip_spaces();
    gw_expect(TOK_EQ);

    /* Save body location */
    fn->body_text = gw.text_ptr;
    fn->body_line = gw.cur_line;

    /* Skip past the expression (don't evaluate now) */
    /* We need to skip to end of statement */
    while (*gw.text_ptr && *gw.text_ptr != ':' && *gw.text_ptr != TOK_ELSE)
        gw.text_ptr++;
}

/* Evaluate FN call - called from eval.c */
gw_value_t gw_eval_fn_call(void)
{
    gw_skip_spaces();
    if (!gw_is_letter(gw_chrgot()))
        gw_error(ERR_SN);
    int fn_idx = toupper(gw_chrgot()) - 'A';
    gw_chrget();
    while (gw_is_letter(gw_chrgot()) || gw_is_digit(gw_chrgot()))
        gw_chrget();

    /* Skip type suffix on function name */
    if (gw_chrgot() == '%' || gw_chrgot() == '!' ||
        gw_chrgot() == '#' || gw_chrgot() == '$')
        gw_chrget();

    fn_def_t *fn = &gw.fn_defs[fn_idx];
    if (!fn->defined)
        gw_error(ERR_UF);

    /* Evaluate argument if present */
    gw_value_t arg_val = {0};
    bool has_arg = false;
    gw_skip_spaces();
    if (gw_chrgot() == '(') {
        gw_chrget();
        arg_val = gw_eval();
        gw_expect_rparen();
        has_arg = true;
    }

    /* Save/restore text pointer around body evaluation */
    uint8_t *save_text = gw.text_ptr;
    program_line_t *save_line = gw.cur_line;

    /* Set parameter if function has one */
    var_entry_t *param_var = NULL;
    gw_value_t saved_val = {0};
    if (fn->param_name[0] && has_arg) {
        param_var = gw_var_find_or_create(fn->param_name, fn->param_type);
        saved_val = param_var->val;
        if (fn->param_type == VT_STR && saved_val.type == VT_STR)
            saved_val.sval = gw_str_copy(&param_var->val.sval);
        gw_var_assign(param_var, &arg_val);
    }

    /* Evaluate the body expression */
    gw.text_ptr = fn->body_text;
    gw.cur_line = fn->body_line;
    gw_value_t result = gw_eval();

    /* Restore parameter */
    if (param_var) {
        if (fn->param_type == VT_STR)
            gw_str_free(&param_var->val.sval);
        param_var->val = saved_val;
    }

    /* Restore text pointer */
    gw.text_ptr = save_text;
    gw.cur_line = save_line;

    return result;
}

/* ================================================================
 * MID$ Assignment
 * ================================================================ */

void gw_stmt_mid_assign(void)
{
    /* Called after TOK_PREFIX_FF FUNC_MID consumed, at '(' */
    gw_chrget();  /* skip '(' */

    /* Parse the target variable */
    char name[2];
    gw_valtype_t type = gw_parse_varname(name);
    if (type != VT_STR)
        gw_error(ERR_TM);

    gw_skip_spaces();
    var_entry_t *var = NULL;
    gw_value_t *arr_elem = NULL;
    if (gw_chrgot() == '(') {
        arr_elem = gw_array_element(name, type);
    } else {
        var = gw_var_find_or_create(name, type);
    }

    gw_skip_spaces();
    gw_expect(',');
    int start = gw_eval_int();
    if (start < 1) gw_error(ERR_FC);

    int maxlen = -1;
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        gw_chrget();
        maxlen = gw_eval_int();
        if (maxlen < 0) gw_error(ERR_FC);
    }

    gw_expect_rparen();
    gw_skip_spaces();
    gw_expect(TOK_EQ);

    gw_value_t rhs = gw_eval_str();

    /* Get pointer to the target string */
    gw_string_t *target;
    if (arr_elem) {
        target = &arr_elem->sval;
    } else {
        target = &var->val.sval;
    }

    /* Perform in-place replacement */
    int pos = start - 1;  /* 0-based */
    if (pos >= target->len) {
        gw_str_free(&rhs.sval);
        return;  /* nothing to replace */
    }

    int replace_len = rhs.sval.len;
    int available = target->len - pos;
    if (maxlen >= 0 && replace_len > maxlen)
        replace_len = maxlen;
    if (replace_len > available)
        replace_len = available;

    memcpy(target->data + pos, rhs.sval.data, replace_len);
    gw_str_free(&rhs.sval);
}

/* ================================================================
 * CHAIN "filename" [, linenum] [, ALL]
 * ================================================================ */

void gw_stmt_chain(void)
{
    gw_skip_spaces();

    /* Skip optional MERGE keyword */
    bool merge = false;
    if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'M') {
        /* Could be MERGE - skip the word */
        uint8_t *save = gw.text_ptr;
        while (gw_is_letter(gw_chrgot()))
            gw_chrget();
        gw_skip_spaces();
        merge = true;
        /* If no string follows, this wasn't MERGE */
        if (gw_chrgot() != '"') {
            gw.text_ptr = save;
            merge = false;
        }
    }

    gw_value_t fname_val = gw_eval_str();
    char *filename = gw_str_to_cstr(&fname_val.sval);
    gw_str_free(&fname_val.sval);

    uint16_t start_line = 0;
    bool has_start = false;
    bool keep_all = false;

    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        gw_chrget();
        gw_skip_spaces();
        /* Optional line number */
        uint8_t ch = gw_chrgot();
        if ((ch >= 0x11 && ch <= 0x1A) || ch == TOK_INT1 || ch == TOK_INT2
            || ch == TOK_CONST_SNG || ch == TOK_CONST_DBL) {
            start_line = gw_eval_uint16();
            has_start = true;
        }
        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
            gw_skip_spaces();
            /* ALL keyword - keep all variables */
            if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'A') {
                keep_all = true;
                while (gw_is_letter(gw_chrgot()))
                    gw_chrget();
            }
        }
    }

    /* Load the new program */
    gw_stmt_load_internal(filename, !merge);
    free(filename);

    if (!keep_all && !merge) {
        /* Clear variables unless ALL specified */
        gw_vars_clear();
        gw_arrays_clear();
    }

    /* Start execution */
    program_line_t *start = gw.prog_head;
    if (has_start) {
        start = gw_find_line(start_line);
        if (!start) gw_error(ERR_UL);
    }

    if (start) {
        gw.for_sp = 0;
        gw.gosub_sp = 0;
        gw.while_sp = 0;
        gw.data_ptr = NULL;
        gw.data_line_ptr = NULL;
        gw.cont_text = NULL;
        gw.cont_line = NULL;
        gw.on_error_line = 0;
        gw.in_error_handler = false;
        gw.cur_line = start;
        gw.text_ptr = start->tokens;
        gw.cur_line_num = start->num;
        gw.running = true;
        gw_run_loop();
    }
}

/* ================================================================
 * Statement Dispatcher
 * ================================================================ */

void gw_exec_stmt(void)
{
    gw_skip_spaces();
    uint8_t tok = gw_chrgot();

    if (tok == 0 || tok == ':')
        return;

    /* Trace */
    if (gw.trace_on && gw.cur_line_num != LINE_DIRECT) {
        char tbuf[16];
        snprintf(tbuf, sizeof(tbuf), "[%u]", gw.cur_line_num);
        if (gw_hal) gw_hal->puts(tbuf);
        else fputs(tbuf, stdout);
    }

    /* PRINT / ? */
    if (tok == TOK_PRINT || tok == '?') {
        gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() == '#') {
            gw_stmt_print_file();
            return;
        }
        gw_stmt_print();
        return;
    }

    /* LPRINT */
    if (tok == TOK_LPRINT) {
        gw_chrget();
        gw_stmt_print();
        return;
    }

    /* REM / single-quote */
    if (tok == TOK_REM || tok == TOK_SQUOTE) {
        while (*gw.text_ptr) gw.text_ptr++;
        return;
    }

    /* CLS */
    if (tok == TOK_CLS) {
        gw_chrget();
        if (gw_hal) gw_hal->cls();
        if (gfx_active()) { gfx_cls(); gfx_flush(); }
        return;
    }

    /* Extended statements (0xFE prefix) */
    if (tok == TOK_PREFIX_FE) {
        uint8_t *save = gw.text_ptr;
        gw_chrget();
        uint8_t xstmt = gw_chrgot();
        if (xstmt == XSTMT_SYSTEM) {
            gw_file_close_all();
            if (gw_hal) gw_hal->shutdown();
            exit(0);
        }
        if (xstmt == XSTMT_CHAIN) {
            gw_chrget();
            gw_stmt_chain();
            return;
        }
        if (xstmt == XSTMT_COMMON) {
            /* COMMON var, var... - just skip, variables already in table */
            gw_chrget();
            while (gw_chrgot() && gw_chrgot() != ':' && gw_chrgot() != TOK_ELSE)
                gw.text_ptr++;
            return;
        }
        if (xstmt == XSTMT_FIELD) {
            gw_chrget();
            gw_stmt_field();
            return;
        }
        if (xstmt == XSTMT_LSET) {
            gw_chrget();
            gw_stmt_lset();
            return;
        }
        if (xstmt == XSTMT_RSET) {
            gw_chrget();
            gw_stmt_rset();
            return;
        }
        if (xstmt == XSTMT_PUT) {
            gw_chrget();
            gw_stmt_put();
            return;
        }
        if (xstmt == XSTMT_GET) {
            gw_chrget();
            gw_stmt_get();
            return;
        }
        if (xstmt == XSTMT_KILL) {
            gw_chrget();
            gw_value_t fname = gw_eval_str();
            char *path = gw_str_to_cstr(&fname.sval);
            gw_str_free(&fname.sval);
            if (remove(path) != 0) { free(path); gw_error(ERR_FF); }
            free(path);
            return;
        }
        if (xstmt == XSTMT_NAME) {
            gw_chrget();
            gw_value_t old = gw_eval_str();
            char *oldpath = gw_str_to_cstr(&old.sval);
            gw_str_free(&old.sval);
            gw_skip_spaces();
            /* Skip AS */
            if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'A') {
                gw_chrget();
                if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'S')
                    gw_chrget();
            }
            gw_value_t new_val = gw_eval_str();
            char *newpath = gw_str_to_cstr(&new_val.sval);
            gw_str_free(&new_val.sval);
            if (rename(oldpath, newpath) != 0) {
                free(oldpath); free(newpath); gw_error(ERR_FF);
            }
            free(oldpath); free(newpath);
            return;
        }
        /* CIRCLE (cx,cy),radius[,[color][,[start][,[end][,aspect]]]] */
        if (xstmt == XSTMT_CIRCLE) {
            gw_chrget();
            gw_skip_spaces();
            gw_expect('(');
            int cx = gw_eval_int();
            gw_expect(',');
            int cy = gw_eval_int();
            gw_expect_rparen();
            gw_expect(',');
            int radius = gw_eval_int();
            int color = gfx_get_color();
            double start_a = 0, end_a = 0, aspect = 0;
            gw_value_t tmp;
            gw_skip_spaces();
            if (gw_chrgot() == ',') {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':')
                    color = gw_eval_int();
                gw_skip_spaces();
                if (gw_chrgot() == ',') {
                    gw_chrget();
                    gw_skip_spaces();
                    if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':') {
                        tmp = gw_eval_num(); start_a = gw_to_dbl(&tmp);
                    }
                    gw_skip_spaces();
                    if (gw_chrgot() == ',') {
                        gw_chrget();
                        gw_skip_spaces();
                        if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':') {
                            tmp = gw_eval_num(); end_a = gw_to_dbl(&tmp);
                        }
                        gw_skip_spaces();
                        if (gw_chrgot() == ',') {
                            gw_chrget();
                            tmp = gw_eval_num(); aspect = gw_to_dbl(&tmp);
                        }
                    }
                }
            }
            gfx_circle(cx, cy, radius, color, start_a, end_a, aspect);
            gfx_flush();
            return;
        }
        /* DRAW string-expr */
        if (xstmt == XSTMT_DRAW) {
            gw_chrget();
            gw_value_t s = gw_eval_str();
            char *cmd = gw_str_to_cstr(&s.sval);
            gw_str_free(&s.sval);
            gfx_draw(cmd);
            free(cmd);
            gfx_flush();
            return;
        }
        /* PAINT (x,y)[,fill_color[,border_color]] */
        if (xstmt == XSTMT_PAINT) {
            gw_chrget();
            gw_skip_spaces();
            gw_expect('(');
            int px = gw_eval_int();
            gw_expect(',');
            int py = gw_eval_int();
            gw_expect_rparen();
            int fill_c = gfx_get_color(), border_c = 0;
            gw_skip_spaces();
            if (gw_chrgot() == ',') {
                gw_chrget();
                fill_c = gw_eval_int();
                gw_skip_spaces();
                if (gw_chrgot() == ',') {
                    gw_chrget();
                    border_c = gw_eval_int();
                }
            }
            gfx_paint(px, py, fill_c, border_c);
            gfx_flush();
            return;
        }
        /* PLAY mml-string */
        if (xstmt == XSTMT_PLAY) {
            gw_chrget();
            gw_value_t s = gw_eval_str();
            char *cmd = gw_str_to_cstr(&s.sval);
            gw_str_free(&s.sval);
            snd_play(cmd);
            free(cmd);
            return;
        }
        /* Stubs: VIEW, WINDOW, PALETTE */
        if (xstmt == XSTMT_VIEW ||
            xstmt == XSTMT_WINDOW || xstmt == XSTMT_PALETTE) {
            gw_chrget();
            while (gw_chrgot() && gw_chrgot() != ':' && gw_chrgot() != TOK_ELSE)
                gw.text_ptr++;
            return;
        }
        gw.text_ptr = save;
    }

    /* END */
    if (tok == TOK_END) {
        gw_chrget();
        gw.running = false;
        gw.cont_text = gw.text_ptr;
        gw.cont_line = gw.cur_line;
        return;
    }

    /* STOP */
    if (tok == TOK_STOP) {
        gw_chrget();
        gw.running = false;
        gw.cont_text = gw.text_ptr;
        gw.cont_line = gw.cur_line;
        if (gw.cur_line_num != LINE_DIRECT) {
            char buf[40];
            snprintf(buf, sizeof(buf), "Break in %u\n", gw.cur_line_num);
            if (gw_hal) gw_hal->puts(buf);
            else fputs(buf, stdout);
        }
        return;
    }

    /* NEW */
    if (tok == TOK_NEW) {
        gw_chrget();
        gfx_shutdown();
        gw_free_program();
        gw_vars_clear();
        gw_arrays_clear();
        gw_file_close_all();
        memset(gw.fn_defs, 0, sizeof(gw.fn_defs));
        gw.for_sp = 0;
        gw.gosub_sp = 0;
        gw.while_sp = 0;
        gw.data_ptr = NULL;
        gw.data_line_ptr = NULL;
        gw.cont_text = NULL;
        gw.cont_line = NULL;
        gw.on_error_line = 0;
        gw.in_error_handler = false;
        gw.running = false;
        for (int i = 0; i < 26; i++)
            gw.def_type[i] = VT_SNG;
        gw.option_base = 0;
        return;
    }

    /* CLEAR */
    if (tok == TOK_CLEAR) {
        gw_chrget();
        gw_vars_clear();
        gw_arrays_clear();
        gw_file_close_all();
        memset(gw.fn_defs, 0, sizeof(gw.fn_defs));
        gw.for_sp = 0;
        gw.gosub_sp = 0;
        gw.while_sp = 0;
        gw.data_ptr = NULL;
        gw.data_line_ptr = NULL;
        gw.on_error_line = 0;
        gw.in_error_handler = false;
        /* Skip optional args (memory size, stack size) */
        while (gw_chrgot() && gw_chrgot() != ':')
            gw.text_ptr++;
        return;
    }

    /* RUN */
    if (tok == TOK_RUN) {
        gw_chrget();
        gw_skip_spaces();

        /* RUN "filename" - load and run a file */
        if (gw_chrgot() == '"') {
            gw_value_t fname_val = gw_eval_str();
            char *filename = gw_str_to_cstr(&fname_val.sval);
            gw_str_free(&fname_val.sval);
            gw_stmt_load_internal(filename, true);
            free(filename);
            if (gw.prog_head) {
                gw.cur_line = gw.prog_head;
                gw.text_ptr = gw.prog_head->tokens;
                gw.cur_line_num = gw.prog_head->num;
                gw.running = true;
                gw_run_loop();
            }
            return;
        }

        /* RUN with line number */
        program_line_t *start = gw.prog_head;
        if (gw_chrgot() >= TOK_INT2 && gw_chrgot() <= TOK_CONST_DBL) {
            uint16_t num = gw_eval_uint16();
            start = gw_find_line(num);
            if (!start) gw_error(ERR_UL);
        } else if (gw_chrgot() >= 0x11 && gw_chrgot() <= 0x1A) {
            uint16_t num = gw_eval_uint16();
            start = gw_find_line(num);
            if (!start) gw_error(ERR_UL);
        }

        if (!start) return;

        gw_vars_clear();
        gw_arrays_clear();
        memset(gw.fn_defs, 0, sizeof(gw.fn_defs));
        gw.for_sp = 0;
        gw.gosub_sp = 0;
        gw.while_sp = 0;
        gw.data_ptr = NULL;
        gw.data_line_ptr = NULL;
        gw.cont_text = NULL;
        gw.cont_line = NULL;
        gw.on_error_line = 0;
        gw.in_error_handler = false;
        gw.option_base = 0;

        gw.cur_line = start;
        gw.text_ptr = start->tokens;
        gw.cur_line_num = start->num;
        gw.running = true;
        gw_run_loop();
        return;
    }

    /* CONT */
    if (tok == TOK_CONT) {
        gw_chrget();
        if (!gw.cont_text || !gw.cont_line)
            gw_error(ERR_CN);
        gw.text_ptr = gw.cont_text;
        gw.cur_line = gw.cont_line;
        gw.cur_line_num = gw.cont_line->num;
        gw.running = true;
        gw.cont_text = NULL;
        gw.cont_line = NULL;
        gw_run_loop();
        return;
    }

    /* LIST */
    if (tok == TOK_LIST) {
        gw_chrget();
        stmt_list();
        return;
    }

    /* GOTO */
    if (tok == TOK_GOTO) {
        gw_chrget();
        uint16_t num = gw_eval_uint16();
        program_line_t *target = gw_find_line(num);
        if (!target) gw_error(ERR_UL);
        gw.cur_line = target;
        gw.text_ptr = target->tokens;
        gw.cur_line_num = target->num;
        return;
    }

    /* GOSUB */
    if (tok == TOK_GOSUB) {
        gw_chrget();
        uint16_t num = gw_eval_uint16();
        program_line_t *target = gw_find_line(num);
        if (!target) gw_error(ERR_UL);

        if (gw.gosub_sp >= 24)
            gw_error(ERR_OM);
        gw.gosub_stack[gw.gosub_sp].ret_text = gw.text_ptr;
        gw.gosub_stack[gw.gosub_sp].ret_line = gw.cur_line;
        gw.gosub_stack[gw.gosub_sp].line_num = gw.cur_line_num;
        gw.gosub_sp++;

        gw.cur_line = target;
        gw.text_ptr = target->tokens;
        gw.cur_line_num = target->num;
        return;
    }

    /* RETURN */
    if (tok == TOK_RETURN) {
        gw_chrget();
        if (gw.gosub_sp <= 0)
            gw_error(ERR_RG);
        gw.gosub_sp--;
        gw.text_ptr = gw.gosub_stack[gw.gosub_sp].ret_text;
        gw.cur_line = gw.gosub_stack[gw.gosub_sp].ret_line;
        gw.cur_line_num = gw.gosub_stack[gw.gosub_sp].line_num;

        /* Optional line number: RETURN <linenum> */
        gw_skip_spaces();
        if (gw_chrgot() >= TOK_INT2 && gw_chrgot() <= TOK_CONST_DBL) {
            uint16_t num = gw_eval_uint16();
            program_line_t *target = gw_find_line(num);
            if (!target) gw_error(ERR_UL);
            gw.cur_line = target;
            gw.text_ptr = target->tokens;
            gw.cur_line_num = target->num;
        } else if (gw_chrgot() >= 0x11 && gw_chrgot() <= 0x1A) {
            uint16_t num = gw_eval_uint16();
            program_line_t *target = gw_find_line(num);
            if (!target) gw_error(ERR_UL);
            gw.cur_line = target;
            gw.text_ptr = target->tokens;
            gw.cur_line_num = target->num;
        }
        return;
    }

    /* FOR */
    if (tok == TOK_FOR) {
        gw_chrget();

        /* Parse variable */
        char name[2];
        gw_valtype_t type = gw_parse_varname(name);
        if (type == VT_STR) gw_error(ERR_TM);
        var_entry_t *var = gw_var_find_or_create(name, type);

        gw_skip_spaces();
        gw_expect(TOK_EQ);

        /* Initial value */
        gw_value_t init = gw_eval_num();
        gw_var_assign(var, &init);

        gw_skip_spaces();
        gw_expect(TOK_TO);

        /* Limit */
        gw_value_t limit = gw_eval_num();

        /* Step (default 1) */
        gw_value_t step;
        step.type = VT_INT;
        step.ival = 1;
        gw_skip_spaces();
        if (gw_chrgot() == TOK_STEP) {
            gw_chrget();
            step = gw_eval_num();
        }

        /* Check if this variable already on stack, replace */
        for (int i = gw.for_sp - 1; i >= 0; i--) {
            if (gw.for_stack[i].var == var) {
                gw.for_sp = i;
                break;
            }
        }

        if (gw.for_sp >= 16)
            gw_error(ERR_OM);

        for_entry_t *f = &gw.for_stack[gw.for_sp++];
        f->var = var;
        f->limit = limit;
        f->step = step;
        f->loop_text = gw.text_ptr;
        f->loop_line = gw.cur_line;
        f->line_num = gw.cur_line_num;
        return;
    }

    /* NEXT */
    if (tok == TOK_NEXT) {
        gw_chrget();
        gw_skip_spaces();

        /* Parse optional variable name(s) */
        for (;;) {
            var_entry_t *var = NULL;
            if (gw_is_letter(gw_chrgot())) {
                char name[2];
                gw_valtype_t type = gw_parse_varname(name);
                var = gw_var_find_or_create(name, type);
            }

            /* Find matching FOR */
            int found = -1;
            for (int i = gw.for_sp - 1; i >= 0; i--) {
                if (!var || gw.for_stack[i].var == var) {
                    found = i;
                    break;
                }
            }
            if (found < 0)
                gw_error(ERR_NF);

            /* Discard any nested FORs above */
            gw.for_sp = found + 1;
            for_entry_t *f = &gw.for_stack[found];

            /* Increment */
            double cur, lim, stp;
            switch (f->var->type) {
            case VT_INT:
                f->var->val.ival = gw_int_add(f->var->val.ival,
                    gw_to_int(&f->step));
                cur = f->var->val.ival;
                break;
            case VT_SNG:
                f->var->val.fval += gw_to_sng(&f->step);
                cur = f->var->val.fval;
                break;
            case VT_DBL:
                f->var->val.dval += gw_to_dbl(&f->step);
                cur = f->var->val.dval;
                break;
            default: cur = 0; break;
            }
            lim = gw_to_dbl(&f->limit);
            stp = gw_to_dbl(&f->step);

            /* Check termination */
            bool done;
            if (stp >= 0)
                done = cur > lim;
            else
                done = cur < lim;

            if (!done) {
                /* Loop back */
                gw.text_ptr = f->loop_text;
                gw.cur_line = f->loop_line;
                gw.cur_line_num = f->line_num;
                return;
            }

            /* Loop done, pop stack */
            gw.for_sp = found;

            /* Check for more variables after comma */
            gw_skip_spaces();
            if (gw_chrgot() != ',')
                break;
            gw_chrget();
        }
        return;
    }

    /* IF */
    if (tok == TOK_IF) {
        gw_chrget();
        gw_value_t cond = gw_eval_num();
        double cv = gw_to_dbl(&cond);

        gw_skip_spaces();
        /* Expect THEN or GOTO */
        if (gw_chrgot() == TOK_THEN)
            gw_chrget();
        else if (gw_chrgot() == TOK_GOTO)
            ;  /* GOTO will be handled below */
        else
            gw_error(ERR_SN);

        if (cv != 0.0) {
            gw_skip_spaces();
            /* Check for line number after THEN */
            uint8_t ch = gw_chrgot();
            if ((ch >= 0x11 && ch <= 0x1A) || ch == TOK_INT1 || ch == TOK_INT2) {
                uint16_t num = gw_eval_uint16();
                program_line_t *target = gw_find_line(num);
                if (!target) gw_error(ERR_UL);
                gw.cur_line = target;
                gw.text_ptr = target->tokens;
                gw.cur_line_num = target->num;
                return;
            }
            /* Execute statement(s) after THEN */
            gw_exec_stmt();
            return;
        }

        /* False: skip to ELSE or end of line */
        skip_to_else_or_eol();
        if (*gw.text_ptr == 0)
            return;

        /* We're at ELSE clause */
        gw_skip_spaces();
        uint8_t ch = gw_chrgot();
        if ((ch >= 0x11 && ch <= 0x1A) || ch == TOK_INT1 || ch == TOK_INT2) {
            uint16_t num = gw_eval_uint16();
            program_line_t *target = gw_find_line(num);
            if (!target) gw_error(ERR_UL);
            gw.cur_line = target;
            gw.text_ptr = target->tokens;
            gw.cur_line_num = target->num;
            return;
        }
        gw_exec_stmt();
        return;
    }

    /* WHILE */
    if (tok == TOK_WHILE) {
        /* Save position AT the WHILE token for WEND to jump back to */
        uint8_t *while_text = gw.text_ptr;
        program_line_t *while_line = gw.cur_line;

        gw_chrget();

        gw_value_t cond = gw_eval_num();
        double cv = gw_to_dbl(&cond);

        if (cv != 0.0) {
            /* Push WHILE onto stack (pointing to condition) */
            /* Check if we're already in this WHILE */
            bool found = false;
            for (int i = gw.while_sp - 1; i >= 0; i--) {
                if (gw.while_stack[i].while_text == while_text) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (gw.while_sp >= 16)
                    gw_error(ERR_OM);
                gw.while_stack[gw.while_sp].while_text = while_text;
                gw.while_stack[gw.while_sp].while_line = while_line;
                gw.while_stack[gw.while_sp].line_num = gw.cur_line_num;
                gw.while_sp++;
            }
            return;  /* continue executing statements after WHILE */
        }

        /* Condition false: find matching WEND and skip */
        int depth = 1;
        for (;;) {
            /* Advance to next statement/line */
            while (*gw.text_ptr && *gw.text_ptr != ':') {
                if (*gw.text_ptr == TOK_WHILE) depth++;
                if (*gw.text_ptr == TOK_WEND) {
                    depth--;
                    if (depth == 0) {
                        gw.text_ptr++;
                        /* Remove from WHILE stack if present */
                        for (int i = gw.while_sp - 1; i >= 0; i--) {
                            if (gw.while_stack[i].while_text == while_text) {
                                gw.while_sp = i;
                                break;
                            }
                        }
                        return;
                    }
                }
                /* Skip embedded constants */
                uint8_t ch2 = *gw.text_ptr;
                if (ch2 == TOK_INT2)      { gw.text_ptr += 3; continue; }
                if (ch2 == TOK_INT1)      { gw.text_ptr += 2; continue; }
                if (ch2 >= 0x11 && ch2 <= 0x1A) { gw.text_ptr++; continue; }
                if (ch2 == TOK_CONST_SNG) { gw.text_ptr += 5; continue; }
                if (ch2 == TOK_CONST_DBL) { gw.text_ptr += 9; continue; }
                if (ch2 == '"') {
                    gw.text_ptr++;
                    while (*gw.text_ptr && *gw.text_ptr != '"')
                        gw.text_ptr++;
                    if (*gw.text_ptr == '"') gw.text_ptr++;
                    continue;
                }
                gw.text_ptr++;
            }
            if (*gw.text_ptr == ':') {
                gw.text_ptr++;
                continue;
            }
            /* End of line, advance to next program line */
            if (!gw.cur_line || !gw.cur_line->next)
                gw_error(ERR_WH);
            gw.cur_line = gw.cur_line->next;
            gw.text_ptr = gw.cur_line->tokens;
            gw.cur_line_num = gw.cur_line->num;
        }
    }

    /* WEND */
    if (tok == TOK_WEND) {
        gw_chrget();
        if (gw.while_sp <= 0)
            gw_error(ERR_WE);
        /* Jump back to WHILE token to re-evaluate condition */
        gw.while_sp--;
        gw.text_ptr = gw.while_stack[gw.while_sp].while_text;
        gw.cur_line = gw.while_stack[gw.while_sp].while_line;
        gw.cur_line_num = gw.while_stack[gw.while_sp].line_num;
        return;
    }

    /* ON ... GOTO / GOSUB */
    if (tok == TOK_ON) {
        gw_chrget();
        gw_skip_spaces();

        /* ON ERROR GOTO */
        if (gw_chrgot() == TOK_ERROR) {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() != TOK_GOTO)
                gw_error(ERR_SN);
            gw_chrget();
            uint16_t num = gw_eval_uint16();
            gw.on_error_line = num;
            if (num == 0) {
                gw.on_error_line = 0;
                gw.in_error_handler = false;
            }
            return;
        }

        int idx = gw_eval_int();
        gw_skip_spaces();

        bool is_gosub = false;
        if (gw_chrgot() == TOK_GOTO) {
            gw_chrget();
        } else if (gw_chrgot() == TOK_GOSUB) {
            gw_chrget();
            is_gosub = true;
        } else {
            gw_error(ERR_SN);
        }

        /* Parse line number list */
        int count = 0;
        uint16_t target_num = 0;
        for (;;) {
            gw_skip_spaces();
            uint16_t num = gw_eval_uint16();
            count++;
            if (count == idx)
                target_num = num;
            gw_skip_spaces();
            if (gw_chrgot() != ',')
                break;
            gw_chrget();
        }

        if (idx < 1 || idx > count)
            return;  /* out of range = fall through */

        program_line_t *target = gw_find_line(target_num);
        if (!target) gw_error(ERR_UL);

        if (is_gosub) {
            if (gw.gosub_sp >= 24)
                gw_error(ERR_OM);
            gw.gosub_stack[gw.gosub_sp].ret_text = gw.text_ptr;
            gw.gosub_stack[gw.gosub_sp].ret_line = gw.cur_line;
            gw.gosub_stack[gw.gosub_sp].line_num = gw.cur_line_num;
            gw.gosub_sp++;
        }

        gw.cur_line = target;
        gw.text_ptr = target->tokens;
        gw.cur_line_num = target->num;
        return;
    }

    /* DIM */
    if (tok == TOK_DIM) {
        gw_chrget();
        gw_stmt_dim();
        return;
    }

    /* ERASE */
    if (tok == TOK_ERASE) {
        gw_chrget();
        gw_stmt_erase();
        return;
    }

    /* OPTION */
    if (tok == TOK_OPTION) {
        gw_chrget();
        gw_stmt_option();
        return;
    }

    /* INPUT */
    if (tok == TOK_INPUT) {
        gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() == '#') {
            gw_stmt_input_file();
            return;
        }
        gw_stmt_input();
        return;
    }

    /* LINE INPUT / LINE (graphics stub) */
    if (tok == TOK_LINE) {
        gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() == TOK_INPUT) {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() == '#') {
                gw_stmt_line_input_file();
                return;
            }
            gw_stmt_line_input();
            return;
        }
        /* LINE (x1,y1)-(x2,y2) [,[color][,B[F]]] */
        if (gw_chrgot() == '(' || gw_chrgot() == TOK_MINUS || gw_chrgot() == TOK_STEP) {
            int x1, y1, x2, y2;
            /* First point is optional (uses last point) */
            gw_skip_spaces();
            if (gw_chrgot() == '(') {
                gw_chrget();
                x1 = gw_eval_int();
                gw_expect(',');
                y1 = gw_eval_int();
                gw_expect_rparen();
            } else {
                gfx_get_last(&x1, &y1);
            }
            gw_skip_spaces();
            gw_expect(TOK_MINUS);
            gw_expect('(');
            x2 = gw_eval_int();
            gw_expect(',');
            y2 = gw_eval_int();
            gw_expect_rparen();
            int color = gfx_get_color();
            int style = GFX_LINE;
            gw_skip_spaces();
            if (gw_chrgot() == ',') {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':' &&
                    gw_chrgot() != 'B' && gw_chrgot() != 'b' &&
                    !gw_is_letter(gw_chrgot()))
                    color = gw_eval_int();
                gw_skip_spaces();
                if (gw_chrgot() == ',') {
                    gw_chrget();
                    gw_skip_spaces();
                    if (gw_chrgot() == 'B' || gw_chrgot() == 'b') {
                        gw_chrget();
                        gw_skip_spaces();
                        if (gw_chrgot() == 'F' || gw_chrgot() == 'f') {
                            style = GFX_BOXF;
                            gw_chrget();
                        } else {
                            style = GFX_BOX;
                        }
                    }
                }
            }
            gfx_line(x1, y1, x2, y2, color, style);
            gfx_flush();
            return;
        }
        gw_error(ERR_SN);
    }

    /* DATA - skip during execution */
    if (tok == TOK_DATA) {
        gw_chrget();
        skip_data();
        return;
    }

    /* READ */
    if (tok == TOK_READ) {
        gw_chrget();
        stmt_read();
        return;
    }

    /* RESTORE */
    if (tok == TOK_RESTORE) {
        gw_chrget();
        stmt_restore();
        return;
    }

    /* SWAP */
    if (tok == TOK_SWAP) {
        gw_chrget();
        gw_stmt_swap();
        return;
    }

    /* TRON */
    if (tok == TOK_TRON) {
        gw_chrget();
        gw.trace_on = true;
        return;
    }

    /* TROFF */
    if (tok == TOK_TROFF) {
        gw_chrget();
        gw.trace_on = false;
        return;
    }

    /* RANDOMIZE */
    if (tok == TOK_RANDOMIZE) {
        gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() != 0 && gw_chrgot() != ':') {
            gw_value_t v = gw_eval_num();
            /* Seed the RNG - use the value as seed */
            extern uint32_t gw_rnd_seed;
            gw_rnd_seed = (uint32_t)gw_to_dbl(&v);
        }
        /* Without argument: prompt in real GW-BASIC, we just seed from time */
        return;
    }

    /* DEF */
    if (tok == TOK_DEF) {
        gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() == TOK_FN) {
            stmt_def_fn();
            return;
        }
        gw_error(ERR_SN);
    }

    /* DEFINT / DEFSNG / DEFDBL / DEFSTR */
    if (tok == TOK_DEFINT) { gw_chrget(); gw_stmt_deftype(VT_INT); return; }
    if (tok == TOK_DEFSNG) { gw_chrget(); gw_stmt_deftype(VT_SNG); return; }
    if (tok == TOK_DEFDBL) { gw_chrget(); gw_stmt_deftype(VT_DBL); return; }
    if (tok == TOK_DEFSTR) { gw_chrget(); gw_stmt_deftype(VT_STR); return; }

    /* ERROR */
    if (tok == TOK_ERROR) {
        gw_chrget();
        int errnum = gw_eval_int();
        gw_error(errnum);
        return;
    }

    /* RESUME */
    if (tok == TOK_RESUME) {
        gw_chrget();
        if (!gw.in_error_handler)
            gw_error(ERR_RW);
        gw.in_error_handler = false;

        gw_skip_spaces();
        if (gw_chrgot() == TOK_NEXT) {
            /* RESUME NEXT: continue at statement after the error */
            gw_chrget();
            if (gw.err_resume_text && gw.err_resume_line) {
                gw.text_ptr = gw.err_resume_text;
                gw.cur_line = gw.err_resume_line;
                gw.cur_line_num = gw.err_resume_line->num;
                /* Advance past current statement */
                while (*gw.text_ptr && *gw.text_ptr != ':')
                    gw.text_ptr++;
            }
            return;
        }

        uint8_t ch2 = gw_chrgot();
        if (ch2 == 0 || ch2 == ':') {
            /* RESUME (no args): retry the statement that caused the error */
            if (gw.err_resume_text && gw.err_resume_line) {
                gw.text_ptr = gw.err_resume_text;
                gw.cur_line = gw.err_resume_line;
                gw.cur_line_num = gw.err_resume_line->num;
            }
            return;
        }

        /* RESUME <linenum> */
        uint16_t num = gw_eval_uint16();
        program_line_t *target = gw_find_line(num);
        if (!target) gw_error(ERR_UL);
        gw.cur_line = target;
        gw.text_ptr = target->tokens;
        gw.cur_line_num = target->num;
        return;
    }

    /* POKE - ignore for now */
    if (tok == TOK_POKE) {
        gw_chrget();
        gw_eval_int();  /* address */
        gw_skip_spaces();
        gw_expect(',');
        gw_eval_int();  /* value */
        return;
    }

    /* WIDTH */
    if (tok == TOK_WIDTH) {
        gw_chrget();
        int w = gw_eval_int();
        if (gw_hal) gw_hal->set_width(w);
        return;
    }

    /* LOCATE */
    if (tok == TOK_LOCATE) {
        gw_chrget();
        int row = gw_eval_int();
        gw_skip_spaces();
        int col = 1;
        if (gw_chrgot() == ',') {
            gw_chrget();
            col = gw_eval_int();
        }
        if (gw_hal) gw_hal->locate(row - 1, col - 1);
        /* Skip additional optional params */
        while (gw_chrgot() == ',') {
            gw_chrget();
            if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':')
                gw_eval();
        }
        return;
    }

    /* COLOR */
    if (tok == TOK_COLOR) {
        gw_chrget();
        int fg = gw_eval_int();
        int bg = -1;
        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':')
                bg = gw_eval_int();
            /* Skip optional border parameter */
            gw_skip_spaces();
            if (gw_chrgot() == ',') {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() != 0 && gw_chrgot() != ':')
                    gw_eval_int();
            }
        }
        if (gfx_active()) {
            gfx_set_color(fg);
        } else if (gw_hal) {
            /* Text mode: emit ANSI color codes */
            char ansi[32];
            /* Map GW-BASIC colors to ANSI (simplified) */
            static const int ansi_fg[] = {30,34,32,36,31,35,33,37,90,94,92,96,91,95,93,97};
            static const int ansi_bg[] = {40,44,42,46,41,45,43,47};
            if (fg >= 0 && fg < 16) {
                snprintf(ansi, sizeof(ansi), "\033[%dm", ansi_fg[fg]);
                gw_hal->puts(ansi);
            }
            if (bg >= 0 && bg < 8) {
                snprintf(ansi, sizeof(ansi), "\033[%dm", ansi_bg[bg]);
                gw_hal->puts(ansi);
            }
        }
        return;
    }

    /* SCREEN mode [,[colorswitch][,[apage][,vpage]]] */
    if (tok == TOK_SCREEN) {
        gw_chrget();
        int mode = gw_eval_int();
        /* Skip optional args */
        while (gw_chrgot() == ',') {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':')
                gw_eval_int();
        }
        if (mode == 0) {
            gfx_shutdown();
        } else {
            gfx_init(mode);
        }
        return;
    }

    /* PSET (x,y)[,color] / PRESET (x,y)[,color] */
    if (tok == TOK_PSET || tok == TOK_PRESET) {
        int is_preset = (tok == TOK_PRESET);
        gw_chrget();
        gw_skip_spaces();
        gw_expect('(');
        int px = gw_eval_int();
        gw_expect(',');
        int py = gw_eval_int();
        gw_expect_rparen();
        int color = is_preset ? 0 : gfx_get_color();
        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
            color = gw_eval_int();
        }
        gfx_pset(px, py, color);
        gfx_flush();
        return;
    }

    /* BEEP */
    if (tok == TOK_BEEP) {
        gw_chrget();
        snd_beep();
        return;
    }

    /* SOUND freq, duration */
    if (tok == TOK_SOUND) {
        gw_chrget();
        int freq = gw_eval_int();
        gw_skip_spaces();
        gw_expect(',');
        int dur = gw_eval_int();
        if (freq < 37 || freq > 32767)
            gw_error(ERR_FC);
        if (dur < 0 || dur > 65535)
            gw_error(ERR_FC);
        snd_tone_sync(freq, dur);
        return;
    }

    /* KEY - parse and ignore */
    if (tok == TOK_KEY) {
        gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() == TOK_ON || gw_chrgot() == TOK_OFF) {
            gw_chrget();
            return;
        }
        while (gw_chrgot() && gw_chrgot() != ':')
            gw.text_ptr++;
        return;
    }

    /* OPEN */
    if (tok == TOK_OPEN) {
        gw_chrget();
        gw_stmt_open();
        return;
    }

    /* CLOSE */
    if (tok == TOK_CLOSE) {
        gw_chrget();
        gw_stmt_close();
        return;
    }

    /* SAVE */
    if (tok == TOK_SAVE) {
        gw_chrget();
        gw_stmt_save();
        return;
    }

    /* LOAD */
    if (tok == TOK_LOAD) {
        gw_chrget();
        gw_stmt_load();
        return;
    }

    /* MERGE */
    if (tok == TOK_MERGE) {
        gw_chrget();
        gw_stmt_merge();
        return;
    }

    /* WRITE */
    if (tok == TOK_WRITE) {
        gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() == '#') {
            gw_stmt_write_file();
            return;
        }
        int first = 1;
        for (;;) {
            gw_skip_spaces();
            uint8_t ch2 = gw_chrgot();
            if (ch2 == 0 || ch2 == ':') break;
            if (!first) {
                if (gw_hal) gw_hal->putch(',');
                else putchar(',');
            }
            gw_value_t v = gw_eval();
            if (v.type == VT_STR) {
                if (gw_hal) gw_hal->putch('"');
                else putchar('"');
                gw_print_value(&v);
                if (gw_hal) gw_hal->putch('"');
                else putchar('"');
            } else {
                gw_print_value(&v);
            }
            first = 0;
            gw_skip_spaces();
            if (gw_chrgot() == ',') { gw_chrget(); continue; }
            if (gw_chrgot() == ';') { gw_chrget(); continue; }
            break;
        }
        gw_print_newline();
        return;
    }

    /* MID$ assignment: MID$(var$, start [,len]) = expr */
    if (tok == TOK_PREFIX_FF) {
        uint8_t *save = gw.text_ptr;
        gw_chrget();
        if (gw_chrgot() == FUNC_MID) {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() == '(') {
                gw_stmt_mid_assign();
                return;
            }
        }
        gw.text_ptr = save;
        tok = gw_chrgot();
    }

    /* LET (explicit) */
    if (tok == TOK_LET) {
        gw_chrget();
        tok = gw_chrgot();
    }

    /* Implicit LET: variable assignment */
    if (gw_is_letter(tok)) {
        char name[2];
        gw_valtype_t type = gw_parse_varname(name);

        gw_skip_spaces();

        /* Check for array element or MID$ assignment */
        if (gw_chrgot() == '(') {
            gw_value_t *elem = gw_array_element(name, type);
            gw_skip_spaces();
            gw_expect(TOK_EQ);
            gw_value_t val = gw_eval();

            if (type == VT_STR) {
                if (val.type != VT_STR) gw_error(ERR_TM);
                gw_str_free(&elem->sval);
                elem->sval = val.sval;
                elem->type = VT_STR;
            } else {
                if (val.type == VT_STR) gw_error(ERR_TM);
                switch (type) {
                case VT_INT: elem->ival = gw_to_int(&val); break;
                case VT_SNG: elem->fval = gw_to_sng(&val); break;
                case VT_DBL: elem->dval = gw_to_dbl(&val); break;
                default: break;
                }
                elem->type = type;
            }
            return;
        }

        /* Scalar assignment */
        var_entry_t *var = gw_var_find_or_create(name, type);
        gw_skip_spaces();
        gw_expect(TOK_EQ);
        gw_value_t val = gw_eval();
        gw_var_assign(var, &val);
        return;
    }

    gw_error(ERR_SN);
}

/* ================================================================
 * NEWSTT Loop (Run Loop)
 * ================================================================ */

void gw_run_loop(void)
{
    if (gw_hal) gw_hal->enable_raw();

    if (setjmp(gw_run_jmp) != 0) {
        /* Error handler redirected here via ON ERROR GOTO */
        if (!gw.running) {
            if (gw_hal) gw_hal->disable_raw();
            return;
        }
    }

    while (gw.running) {
        gw_skip_spaces();
        uint8_t ch = gw_chrgot();

        if (ch == 0) {
            /* End of current line, advance to next */
            if (!gw.cur_line || !gw.cur_line->next) {
                gw.running = false;
                break;
            }
            gw.cur_line = gw.cur_line->next;
            gw.text_ptr = gw.cur_line->tokens;
            gw.cur_line_num = gw.cur_line->num;
            continue;
        }

        if (ch == ':') {
            gw.text_ptr++;
            continue;
        }

        /* Skip ELSE during normal flow (only reached via IF true path continuing) */
        if (ch == TOK_ELSE) {
            /* Skip to end of line */
            while (*gw.text_ptr) gw.text_ptr++;
            continue;
        }

        gw_exec_stmt();

        if (!gw.running) break;
    }

    if (gw_hal) gw_hal->disable_raw();
}
