#include "gwbasic.h"
#include "tui.h"
#include "graphics.h"
#include "sound.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/*
 * Execution loop and control flow - the heart of the interpreter.
 * Reimplements NEWSTT, GOTO, GOSUB, FOR/NEXT, IF/THEN/ELSE, etc.
 */

jmp_buf gw_run_jmp;

static int ci_strcmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int d = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (d != 0 || !*a) return d;
    }
}

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

/* Read a tokenized line number literal without expression evaluation.
 * Returns true if a number was read, advances gw.text_ptr. */
bool read_linenum(uint16_t *out)
{
    gw_skip_spaces();
    uint8_t tok = gw_chrgot();
    if (tok >= 0x11 && tok <= 0x1A) {
        *out = tok - 0x11;
        gw_chrget();
        return true;
    }
    if (tok == TOK_INT1) {
        *out = gw.text_ptr[1];
        gw.text_ptr += 2;
        return true;
    }
    if (tok == TOK_INT2) {
        *out = (uint16_t)(gw.text_ptr[1] | (gw.text_ptr[2] << 8));
        gw.text_ptr += 3;
        return true;
    }
    return false;
}

/* ================================================================
 * LIST
 * ================================================================ */

static void stmt_list(void)
{
    gw_skip_spaces();
    uint16_t start = 0, end = 65535;

    if (gw_chrgot() != 0 && gw_chrgot() != ':') {
        if (gw_chrgot() == TOK_MINUS) {
            gw_chrget();
            if (!read_linenum(&end)) gw_error(ERR_SN);
        } else {
            if (!read_linenum(&start)) gw_error(ERR_SN);
            end = start;
            gw_skip_spaces();
            if (gw_chrgot() == TOK_MINUS) {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() != 0 && gw_chrgot() != ':') {
                    if (!read_linenum(&end)) gw_error(ERR_SN);
                } else {
                    end = 65535;
                }
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
        /* Scan to the DATA token and skip past it so read_data_item
           finds values, not the keyword.  If the target line has no
           DATA, data_ptr ends at '\0' and the next READ will call
           advance_data_ptr to move to subsequent lines. */
        while (*gw.data_ptr && *gw.data_ptr != TOK_DATA) {
            if (*gw.data_ptr == '"') {
                gw.data_ptr++;
                while (*gw.data_ptr && *gw.data_ptr != '"')
                    gw.data_ptr++;
                if (*gw.data_ptr == '"') gw.data_ptr++;
                continue;
            }
            gw.data_ptr++;
        }
        if (*gw.data_ptr == TOK_DATA)
            gw.data_ptr++;
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

    /* Save COMMON state before load clears things */
    int saved_common_count = gw.common_count;

    if (!merge) {
        /* Clear program but not variables — CHAIN manages variables below */
        gw_free_program();
        gw_file_close_all();
        memset(gw.fn_defs, 0, sizeof(gw.fn_defs));
    }

    /* Load the new program without clearing */
    gw_stmt_load_internal(filename, false);
    free(filename);

    if (!keep_all && !merge) {
        if (saved_common_count > 0) {
            /* Preserve only COMMON variables, clear the rest */
            for (int i = gw.var_count - 1; i >= 0; i--) {
                bool keep = false;
                for (int j = 0; j < saved_common_count; j++) {
                    if (gw.vars[i].name[0] == gw.common_vars[j].name[0] &&
                        gw.vars[i].name[1] == gw.common_vars[j].name[1] &&
                        gw.vars[i].type == gw.common_vars[j].type) {
                        keep = true;
                        break;
                    }
                }
                if (!keep) {
                    if (gw.vars[i].type == VT_STR)
                        gw_str_free(&gw.vars[i].val.sval);
                    if (i < gw.var_count - 1)
                        gw.vars[i] = gw.vars[gw.var_count - 1];
                    gw.var_count--;
                }
            }
            gw_arrays_clear();
        } else {
            gw_vars_clear();
            gw_arrays_clear();
        }
    }
    gw.common_count = 0;

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
        gw_stmt_lprint();
        return;
    }

    /* LLIST */
    if (tok == TOK_LLIST) {
        gw_chrget();
        gw_stmt_llist();
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
            gw_chrget();
            for (;;) {
                gw_skip_spaces();
                if (!gw_chrgot() || gw_chrgot() == ':' || gw_chrgot() == TOK_ELSE)
                    break;
                char name[2];
                gw_valtype_t type = gw_parse_varname(name);
                /* Skip array parens if present: COMMON A() */
                gw_skip_spaces();
                if (gw_chrgot() == '(') {
                    gw_chrget();
                    gw_skip_spaces();
                    if (gw_chrgot() == ')') gw_chrget();
                }
                if (gw.common_count < 64) {
                    gw.common_vars[gw.common_count].name[0] = name[0];
                    gw.common_vars[gw.common_count].name[1] = name[1];
                    gw.common_vars[gw.common_count].type = type;
                    gw.common_count++;
                }
                gw_skip_spaces();
                if (gw_chrgot() == ',') { gw_chrget(); continue; }
                break;
            }
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
        /* FILES [filespec$] */
        if (xstmt == XSTMT_FILES) {
            gw_chrget();
            gw_skip_spaces();
            char *pattern = NULL;
            if (gw_chrgot() && gw_chrgot() != ':' && gw_chrgot() != TOK_ELSE) {
                gw_value_t v = gw_eval_str();
                pattern = gw_str_to_cstr(&v.sval);
                gw_str_free(&v.sval);
            }
            const char *dir = ".";
            char dirpath[256] = ".";
            const char *wild = NULL;
            if (pattern) {
                /* Split "dir/pattern" into directory and wildcard */
                char *sep = strrchr(pattern, '/');
                if (sep) {
                    *sep = '\0';
                    snprintf(dirpath, sizeof(dirpath), "%s", pattern);
                    dir = dirpath;
                    wild = sep + 1;
                } else {
                    wild = pattern;
                }
                if (wild && !*wild) wild = NULL;
            }
            DIR *dp = opendir(dir);
            if (!dp) { free(pattern); gw_error(ERR_PE); }
            struct dirent *ent;
            int col = 0;
            while ((ent = readdir(dp)) != NULL) {
                if (ent->d_name[0] == '.') continue;
                if (wild) {
                    /* Simple *.ext matching: if wild starts with *. check extension */
                    if (wild[0] == '*' && wild[1] == '.') {
                        const char *ext = strrchr(ent->d_name, '.');
                        if (!ext || ci_strcmp(ext + 1, wild + 2) != 0) continue;
                    } else if (strcmp(wild, "*.*") != 0 && strcmp(wild, "*") != 0) {
                        /* Exact match */
                        if (ci_strcmp(ent->d_name, wild) != 0) continue;
                    }
                }
                char entry[270];
                snprintf(entry, sizeof(entry), "%-14s", ent->d_name);
                if (gw_hal) gw_hal->puts(entry);
                else fputs(entry, stdout);
                col += 14;
                if (col >= 70) {
                    if (gw_hal) gw_hal->puts("\n");
                    else fputs("\n", stdout);
                    col = 0;
                }
            }
            if (col > 0) {
                if (gw_hal) gw_hal->puts("\n");
                else fputs("\n", stdout);
            }
            closedir(dp);
            free(pattern);
            return;
        }
        /* SHELL [command$] */
        if (xstmt == XSTMT_SHELL) {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() && gw_chrgot() != ':' && gw_chrgot() != TOK_ELSE) {
                gw_value_t v = gw_eval_str();
                char *cmd = gw_str_to_cstr(&v.sval);
                gw_str_free(&v.sval);
                int rc = system(cmd);
                free(cmd);
                (void)rc;
            } else {
                const char *sh = getenv("SHELL");
                if (!sh) sh = "/bin/sh";
                int rc = system(sh);
                (void)rc;
            }
            return;
        }
        /* CHDIR path$ */
        if (xstmt == XSTMT_CHDIR) {
            gw_chrget();
            gw_value_t v = gw_eval_str();
            char *path = gw_str_to_cstr(&v.sval);
            gw_str_free(&v.sval);
            if (chdir(path) != 0) { free(path); gw_error(ERR_PE); }
            free(path);
            return;
        }
        /* MKDIR path$ */
        if (xstmt == XSTMT_MKDIR) {
            gw_chrget();
            gw_value_t v = gw_eval_str();
            char *path = gw_str_to_cstr(&v.sval);
            gw_str_free(&v.sval);
            if (mkdir(path, 0755) != 0) {
                int e = errno;
                free(path);
                gw_error(e == EEXIST ? ERR_FE : ERR_PE);
            }
            free(path);
            return;
        }
        /* RMDIR path$ */
        if (xstmt == XSTMT_RMDIR) {
            gw_chrget();
            gw_value_t v = gw_eval_str();
            char *path = gw_str_to_cstr(&v.sval);
            gw_str_free(&v.sval);
            if (rmdir(path) != 0) {
                int e = errno;
                free(path);
                gw_error(e == ENOENT ? ERR_PE : ERR_FF);
            }
            free(path);
            return;
        }
        /* TIMER ON/OFF/STOP */
        if (xstmt == XSTMT_TIMER) {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() == TOK_ON) {
                gw_chrget();
                gw.timer_trap.trap.mode = TRAP_ON;
                return;
            }
            if (gw_chrgot() == TOK_OFF) {
                gw_chrget();
                gw.timer_trap.trap.mode = TRAP_OFF;
                gw.timer_trap.trap.pending = false;
                return;
            }
            if (gw_chrgot() == TOK_STOP) {
                gw_chrget();
                gw.timer_trap.trap.mode = TRAP_STOP;
                return;
            }
            gw_error(ERR_SN);
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
        memset(&gw.timer_trap, 0, sizeof(gw.timer_trap));
        memset(gw.key_traps, 0, sizeof(gw.key_traps));

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

    /* DELETE [start]-[end] */
    if (tok == TOK_DELETE) {
        gw_chrget();
        gw_skip_spaces();
        uint16_t start = 0, end = 65535;
        if (gw_chrgot() == TOK_MINUS) {
            gw_chrget();
            if (!read_linenum(&end)) gw_error(ERR_SN);
        } else {
            if (!read_linenum(&start)) gw_error(ERR_SN);
            end = start;
            gw_skip_spaces();
            if (gw_chrgot() == TOK_MINUS) {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() != 0 && gw_chrgot() != ':') {
                    if (!read_linenum(&end)) gw_error(ERR_SN);
                } else {
                    end = 65535;
                }
            }
        }
        if (!gw_find_line(start) && start == end)
            gw_error(ERR_FC);
        program_line_t **pp = &gw.prog_head;
        while (*pp) {
            if ((*pp)->num >= start && (*pp)->num <= end) {
                program_line_t *del = *pp;
                *pp = del->next;
                free(del->tokens);
                free(del);
            } else {
                pp = &(*pp)->next;
            }
        }
        gw.cont_text = NULL;
        gw.cont_line = NULL;
        return;
    }

    /* EDIT [linenum] */
    if (tok == TOK_EDIT) {
        gw_chrget();
        gw_skip_spaces();
        uint16_t num;
        if (gw_chrgot() && gw_chrgot() != ':' && gw_chrgot() != TOK_ELSE) {
            num = gw_eval_uint16();
        } else if (gw.cont_line) {
            num = gw.cont_line->num;
        } else if (gw.err_line_num) {
            num = gw.err_line_num;
        } else {
            gw_error(ERR_SN);
        }
        program_line_t *line = gw_find_line(num);
        if (!line) gw_error(ERR_UL);
        char listbuf[512];
        gw_list_line(line->tokens, line->len, listbuf, sizeof(listbuf));
        char formatted[560];
        snprintf(formatted, sizeof(formatted), "%u %s", line->num, listbuf);
        if (tui.active) {
            tui_edit_line(formatted);
        } else {
            /* Non-interactive: just display the line */
            if (gw_hal) {
                gw_hal->puts(formatted);
                gw_hal->puts("\n");
            } else {
                printf("%s\n", formatted);
            }
        }
        return;
    }

    /* AUTO [start[,increment]] */
    if (tok == TOK_AUTO) {
        gw_chrget();
        gw_skip_spaces();
        uint16_t start = 10, inc = 10;
        if (gw_chrgot() && gw_chrgot() != ':' && gw_chrgot() != ',' && gw_chrgot() != TOK_ELSE) {
            if (!read_linenum(&start)) gw_error(ERR_SN);
        }
        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
            if (!read_linenum(&inc)) gw_error(ERR_SN);
        }
        if (inc == 0) gw_error(ERR_FC);
        gw.auto_mode = true;
        gw.auto_line = start;
        gw.auto_inc = inc;
        return;
    }

    /* RENUM [new[,old[,increment]]] */
    if (tok == TOK_RENUM) {
        gw_chrget();
        gw_skip_spaces();
        uint16_t new_start = 10, old_start = 0, inc = 10;
        if (gw_chrgot() && gw_chrgot() != ':' && gw_chrgot() != TOK_ELSE &&
            gw_chrgot() != ',') {
            if (!read_linenum(&new_start)) gw_error(ERR_SN);
        }
        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
            gw_skip_spaces();
            if (gw_chrgot() != ',' && gw_chrgot() != 0 && gw_chrgot() != ':')
                if (!read_linenum(&old_start)) gw_error(ERR_SN);
            gw_skip_spaces();
            if (gw_chrgot() == ',') {
                gw_chrget();
                if (!read_linenum(&inc)) gw_error(ERR_SN);
            }
        }
        if (inc == 0) gw_error(ERR_FC);

        /* Count lines to renumber and build mapping table */
        int count = 0;
        for (program_line_t *p = gw.prog_head; p; p = p->next)
            if (p->num >= old_start) count++;
        if (count == 0) return;

        /* Check if new numbers would overflow */
        uint32_t last = (uint32_t)new_start + (uint32_t)(count - 1) * inc;
        if (last > 65529) gw_error(ERR_FC);

        /* Build old->new mapping */
        uint16_t *old_nums = malloc(count * sizeof(uint16_t));
        uint16_t *new_nums = malloc(count * sizeof(uint16_t));
        if (!old_nums || !new_nums) { free(old_nums); free(new_nums); gw_error(ERR_OM); }

        int idx = 0;
        uint16_t nn = new_start;
        for (program_line_t *p = gw.prog_head; p; p = p->next) {
            if (p->num >= old_start) {
                old_nums[idx] = p->num;
                new_nums[idx] = nn;
                nn += inc;
                idx++;
            }
        }

        /* Patch line number references in all program lines */
        for (program_line_t *p = gw.prog_head; p; p = p->next) {
            uint8_t *t = p->tokens;
            while (*t) {
                uint8_t tok_b = *t;
                /* Tokens that take a line number argument */
                if (tok_b == TOK_GOTO || tok_b == TOK_GOSUB ||
                    tok_b == TOK_THEN || tok_b == TOK_RESTORE ||
                    tok_b == TOK_RUN || tok_b == TOK_RESUME) {
                    t++;
                    while (*t == ' ') t++;
                    /* Read the encoded line number */
                    uint16_t ref = 0;
                    uint8_t *numpos = t;
                    int numlen = 0;
                    if (*t >= 0x11 && *t <= 0x1A) {
                        ref = *t - 0x11;
                        numlen = 1;
                    } else if (*t == TOK_INT1) {
                        ref = t[1];
                        numlen = 2;
                    } else if (*t == TOK_INT2) {
                        ref = (uint16_t)(t[1] | (t[2] << 8));
                        numlen = 3;
                    } else {
                        continue;
                    }
                    /* Look up in mapping */
                    for (int i = 0; i < count; i++) {
                        if (old_nums[i] == ref) {
                            /* Rewrite the number in the token stream */
                            uint16_t nv = new_nums[i];
                            int new_numlen;
                            uint8_t nbuf[3];
                            if (nv <= 9) {
                                nbuf[0] = 0x11 + nv;
                                new_numlen = 1;
                            } else if (nv <= 255) {
                                nbuf[0] = TOK_INT1;
                                nbuf[1] = nv;
                                new_numlen = 2;
                            } else {
                                nbuf[0] = TOK_INT2;
                                nbuf[1] = nv & 0xFF;
                                nbuf[2] = (nv >> 8) & 0xFF;
                                new_numlen = 3;
                            }
                            if (new_numlen == numlen) {
                                memcpy(numpos, nbuf, new_numlen);
                            } else {
                                /* Need to resize the token buffer */
                                int old_len = p->len;
                                int diff = new_numlen - numlen;
                                int offset = numpos - p->tokens;
                                uint8_t *newbuf = malloc(old_len + diff + 1);
                                if (!newbuf) { free(old_nums); free(new_nums); gw_error(ERR_OM); }
                                memcpy(newbuf, p->tokens, offset);
                                memcpy(newbuf + offset, nbuf, new_numlen);
                                memcpy(newbuf + offset + new_numlen,
                                       p->tokens + offset + numlen,
                                       old_len - offset - numlen + 1);
                                free(p->tokens);
                                p->tokens = newbuf;
                                p->len = old_len + diff;
                                t = p->tokens + offset + new_numlen;
                                continue;
                            }
                            break;
                        }
                    }
                    t = numpos + numlen;
                    /* ON x GOTO/GOSUB can have comma-separated line numbers */
                    while (*t == ' ') t++;
                    while (*t == ',') {
                        t++;
                        while (*t == ' ') t++;
                        numpos = t;
                        numlen = 0;
                        ref = 0;
                        if (*t >= 0x11 && *t <= 0x1A) {
                            ref = *t - 0x11; numlen = 1;
                        } else if (*t == TOK_INT1) {
                            ref = t[1]; numlen = 2;
                        } else if (*t == TOK_INT2) {
                            ref = (uint16_t)(t[1] | (t[2] << 8)); numlen = 3;
                        } else break;
                        for (int i = 0; i < count; i++) {
                            if (old_nums[i] == ref) {
                                uint16_t nv = new_nums[i];
                                int new_numlen;
                                uint8_t nbuf[3];
                                if (nv <= 9) { nbuf[0] = 0x11 + nv; new_numlen = 1; }
                                else if (nv <= 255) { nbuf[0] = TOK_INT1; nbuf[1] = nv; new_numlen = 2; }
                                else { nbuf[0] = TOK_INT2; nbuf[1] = nv & 0xFF; nbuf[2] = (nv >> 8) & 0xFF; new_numlen = 3; }
                                if (new_numlen == numlen) {
                                    memcpy(numpos, nbuf, new_numlen);
                                } else {
                                    int old_len = p->len;
                                    int diff = new_numlen - numlen;
                                    int offset = numpos - p->tokens;
                                    uint8_t *newbuf = malloc(old_len + diff + 1);
                                    if (!newbuf) { free(old_nums); free(new_nums); gw_error(ERR_OM); }
                                    memcpy(newbuf, p->tokens, offset);
                                    memcpy(newbuf + offset, nbuf, new_numlen);
                                    memcpy(newbuf + offset + new_numlen,
                                           p->tokens + offset + numlen,
                                           old_len - offset - numlen + 1);
                                    free(p->tokens);
                                    p->tokens = newbuf;
                                    p->len = old_len + diff;
                                    t = p->tokens + offset + new_numlen;
                                    numpos = t - new_numlen;
                                    numlen = new_numlen;
                                }
                                break;
                            }
                        }
                        t = numpos + numlen;
                        while (*t == ' ') t++;
                    }
                    continue;
                }
                t++;
            }
        }

        /* Assign new line numbers */
        idx = 0;
        for (program_line_t *p = gw.prog_head; p; p = p->next) {
            if (p->num >= old_start) {
                p->num = new_nums[idx++];
            }
        }

        free(old_nums);
        free(new_nums);
        gw.cont_text = NULL;
        gw.cont_line = NULL;
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

        if (gw.gosub_sp >= MAX_GOSUB_DEPTH)
            gw_error(ERR_OM);
        gw.gosub_stack[gw.gosub_sp].ret_text = gw.text_ptr;
        gw.gosub_stack[gw.gosub_sp].ret_line = gw.cur_line;
        gw.gosub_stack[gw.gosub_sp].line_num = gw.cur_line_num;
        gw.gosub_stack[gw.gosub_sp].event_source = NULL;
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

        /* Clear event handler flag if returning from event trap */
        if (gw.gosub_stack[gw.gosub_sp].event_source)
            gw.gosub_stack[gw.gosub_sp].event_source->in_handler = false;

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

        if (gw.for_sp >= MAX_FOR_DEPTH)
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
                if (gw.while_sp >= MAX_WHILE_DEPTH)
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

        /* ON TIMER(n) GOSUB line */
        if (gw_chrgot() == TOK_PREFIX_FE && gw.text_ptr[1] == XSTMT_TIMER) {
            gw.text_ptr += 2;
            gw_expect('(');
            gw_value_t v = gw_eval_num();
            float interval;
            if (v.type == VT_INT) interval = (float)v.ival;
            else if (v.type == VT_SNG) interval = v.fval;
            else interval = (float)v.dval;
            if (interval <= 0) gw_error(ERR_FC);
            gw_expect(')');
            gw_skip_spaces();
            if (gw_chrgot() != TOK_GOSUB) gw_error(ERR_SN);
            gw_chrget();
            uint16_t line = gw_eval_uint16();
            gw.timer_trap.interval = interval;
            gw.timer_trap.trap.gosub_line = line;
            gw.timer_trap.trap.pending = false;
            gw.timer_trap.trap.in_handler = false;
            /* Reset the clock */
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            gw.timer_trap.last_fire = ts.tv_sec + ts.tv_nsec / 1e9;
            return;
        }

        /* ON KEY(n) GOSUB line */
        if (gw_chrgot() == TOK_KEY) {
            gw_chrget();
            gw_expect('(');
            int n = gw_eval_int();
            if (n < 1 || n > 10) gw_error(ERR_FC);
            gw_expect(')');
            gw_skip_spaces();
            if (gw_chrgot() != TOK_GOSUB) gw_error(ERR_SN);
            gw_chrget();
            uint16_t line = gw_eval_uint16();
            gw.key_traps[n - 1].gosub_line = line;
            gw.key_traps[n - 1].pending = false;
            gw.key_traps[n - 1].in_handler = false;
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
            if (gw.gosub_sp >= MAX_GOSUB_DEPTH)
                gw_error(ERR_OM);
            gw.gosub_stack[gw.gosub_sp].ret_text = gw.text_ptr;
            gw.gosub_stack[gw.gosub_sp].ret_line = gw.cur_line;
            gw.gosub_stack[gw.gosub_sp].line_num = gw.cur_line_num;
            gw.gosub_stack[gw.gosub_sp].event_source = NULL;
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

    /* KEY statement */
    if (tok == TOK_KEY) {
        gw_chrget();
        gw_skip_spaces();
        /* KEY(n) ON/OFF/STOP — event trapping */
        if (gw_chrgot() == '(') {
            gw_chrget();
            int n = gw_eval_int();
            if (n < 1 || n > 10) gw_error(ERR_FC);
            gw_expect(')');
            gw_skip_spaces();
            if (gw_chrgot() == TOK_ON) {
                gw_chrget();
                gw.key_traps[n - 1].mode = TRAP_ON;
                return;
            }
            if (gw_chrgot() == TOK_OFF) {
                gw_chrget();
                gw.key_traps[n - 1].mode = TRAP_OFF;
                gw.key_traps[n - 1].pending = false;
                return;
            }
            if (gw_chrgot() == TOK_STOP) {
                gw_chrget();
                gw.key_traps[n - 1].mode = TRAP_STOP;
                return;
            }
            gw_error(ERR_SN);
        }
        if (gw_chrgot() == TOK_ON) {
            gw_chrget();
            tui_key_on();
            return;
        }
        if (gw_chrgot() == TOK_OFF) {
            gw_chrget();
            tui_key_off();
            return;
        }
        if (gw_chrgot() == TOK_LIST) {
            gw_chrget();
            tui_key_list();
            return;
        }
        /* KEY n, "string" */
        {
            gw_value_t v = gw_eval();
            int n = gw_to_int(&v);
            if (n < 1 || n > 10) gw_error(ERR_FC);
            gw_skip_spaces();
            gw_expect(',');
            gw_value_t s = gw_eval();
            if (s.type != VT_STR) gw_error(ERR_TM);
            int len = s.sval.len;
            if (len > 15) len = 15;
            memcpy(tui.fkey_defs[n - 1], s.sval.data, len);
            tui.fkey_defs[n - 1][len] = '\0';
            gw_str_free(&s.sval);
            if (tui.key_bar_visible)
                tui_key_on();  /* refresh bar */
        }
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
 * Event Trapping
 * ================================================================ */

static void fire_event_trap(event_trap_t *trap)
{
    trap->in_handler = true;
    trap->pending = false;

    program_line_t *target = gw_find_line(trap->gosub_line);
    if (!target) return;

    if (gw.gosub_sp >= MAX_GOSUB_DEPTH)
        gw_error(ERR_OM);
    gw.gosub_stack[gw.gosub_sp].ret_text = gw.text_ptr;
    gw.gosub_stack[gw.gosub_sp].ret_line = gw.cur_line;
    gw.gosub_stack[gw.gosub_sp].line_num = gw.cur_line_num;
    gw.gosub_stack[gw.gosub_sp].event_source = trap;
    gw.gosub_sp++;

    gw.cur_line = target;
    gw.text_ptr = target->tokens;
    gw.cur_line_num = target->num;
}

static void gw_check_events(void)
{
    /* Timer trap */
    if (gw.timer_trap.trap.gosub_line && !gw.timer_trap.trap.in_handler) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        double now = ts.tv_sec + ts.tv_nsec / 1e9;
        double elapsed = now - gw.timer_trap.last_fire;

        if (elapsed >= gw.timer_trap.interval) {
            if (gw.timer_trap.trap.mode == TRAP_ON) {
                gw.timer_trap.last_fire = now;
                fire_event_trap(&gw.timer_trap.trap);
                return;
            } else if (gw.timer_trap.trap.mode == TRAP_STOP) {
                gw.timer_trap.trap.pending = true;
            }
        }
    }

    /* Check for pending timer event after TIMER ON */
    if (gw.timer_trap.trap.pending && gw.timer_trap.trap.mode == TRAP_ON &&
        !gw.timer_trap.trap.in_handler) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        gw.timer_trap.last_fire = ts.tv_sec + ts.tv_nsec / 1e9;
        fire_event_trap(&gw.timer_trap.trap);
        return;
    }

    /* Key traps: only consume keystrokes when at least one trap is configured */
    bool any_key_trap = false;
    for (int i = 0; i < 10; i++) {
        if (gw.key_traps[i].gosub_line) { any_key_trap = true; break; }
    }

    if (any_key_trap && gw_hal && gw_hal->kbhit()) {
        int ch = gw_hal->getch();
        int fkey = -1;
        if (ch == 27 && gw_hal->kbhit()) {
            int seq1 = gw_hal->getch();
            if (seq1 == 'O') {
                int seq2 = gw_hal->getch();
                switch (seq2) {
                case 'P': fkey = 0; break;  /* F1 */
                case 'Q': fkey = 1; break;  /* F2 */
                case 'R': fkey = 2; break;  /* F3 */
                case 'S': fkey = 3; break;  /* F4 */
                default:
                    tui_push_key(27);
                    tui_push_key('O');
                    tui_push_key(seq2);
                    return;
                }
            } else if (seq1 == '[') {
                int seq2 = gw_hal->getch();
                if ((seq2 == '1' || seq2 == '2') && gw_hal->kbhit()) {
                    int seq3 = gw_hal->getch();
                    if (gw_hal->kbhit()) {
                        int seq4 = gw_hal->getch();
                        if (seq4 == '~') {
                            int code = (seq2 - '0') * 10 + (seq3 - '0');
                            switch (code) {
                            case 15: fkey = 4; break;   /* F5 */
                            case 17: fkey = 5; break;   /* F6 */
                            case 18: fkey = 6; break;   /* F7 */
                            case 19: fkey = 7; break;   /* F8 */
                            case 20: fkey = 8; break;   /* F9 */
                            case 21: fkey = 9; break;   /* F10 */
                            }
                        }
                        if (fkey < 0) {
                            tui_push_key(27);
                            tui_push_key('[');
                            tui_push_key(seq2);
                            tui_push_key(seq3);
                            tui_push_key(seq4);
                            return;
                        }
                    } else {
                        tui_push_key(27);
                        tui_push_key('[');
                        tui_push_key(seq2);
                        tui_push_key(seq3);
                        return;
                    }
                } else {
                    tui_push_key(27);
                    tui_push_key('[');
                    tui_push_key(seq2);
                    return;
                }
            } else {
                tui_push_key(27);
                tui_push_key(seq1);
                return;
            }
        }

        if (fkey >= 0 && fkey < 10) {
            event_trap_t *kt = &gw.key_traps[fkey];
            if (kt->gosub_line && kt->mode == TRAP_ON && !kt->in_handler) {
                fire_event_trap(kt);
                return;
            } else if (kt->gosub_line && kt->mode == TRAP_STOP) {
                kt->pending = true;
                return;
            }
            return;
        }

        /* Not an F-key: push into key buffer for INKEY$/input */
        tui_push_key(ch);
    }

    /* Check for pending key traps after KEY(n) ON */
    for (int i = 0; i < 10; i++) {
        event_trap_t *kt = &gw.key_traps[i];
        if (kt->pending && kt->mode == TRAP_ON && !kt->in_handler) {
            fire_event_trap(kt);
            return;
        }
    }
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
        /* Check for Ctrl+Break */
        if (tui.active)
            tui_check_break();

        /* Check event traps (ON TIMER, ON KEY) */
        gw_check_events();

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
