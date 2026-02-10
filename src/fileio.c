#include "gwbasic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

file_entry_t *gw_file_get(int num)
{
    if (num < 1 || num > 15)
        gw_error(ERR_BN);
    file_entry_t *f = &gw.files[num];
    if (!f->fp)
        gw_error(ERR_BN);
    return f;
}

void gw_file_close_all(void)
{
    for (int i = 1; i <= 15; i++) {
        if (gw.files[i].fp) {
            fclose(gw.files[i].fp);
            gw.files[i].fp = NULL;
            gw.files[i].mode = 0;
            gw.files[i].eof_flag = false;
        }
        free(gw.files[i].field_buf);
        gw.files[i].field_buf = NULL;
        gw.files[i].field_count = 0;
        gw.files[i].record_len = 0;
    }
}

int gw_file_eof(int num)
{
    if (num < 1 || num > 15)
        gw_error(ERR_BN);
    file_entry_t *f = &gw.files[num];
    if (!f->fp)
        gw_error(ERR_BN);
    if (f->eof_flag || feof(f->fp))
        return -1;
    /* Peek ahead to detect EOF before next read */
    int ch = fgetc(f->fp);
    if (ch == EOF) {
        f->eof_flag = true;
        return -1;
    }
    ungetc(ch, f->fp);
    return 0;
}

/* OPEN - two syntaxes:
 *   Modern:  OPEN "file" FOR INPUT|OUTPUT|APPEND AS #n
 *   Compact: OPEN "I"|"O"|"A", #n, "file"
 */
void gw_stmt_open(void)
{
    gw_skip_spaces();
    gw_value_t first_str = gw_eval_str();
    char *first = gw_str_to_cstr(&first_str.sval);
    gw_str_free(&first_str.sval);

    gw_skip_spaces();
    int mode = 0;
    int file_num = 0;
    char *filename = NULL;

    if (gw_chrgot() == TOK_FOR) {
        /* Modern syntax: OPEN "file" FOR mode AS #n */
        gw_chrget();
        gw_skip_spaces();
        filename = first;

        /* Parse mode keyword.
         * The tokenizer may break words up: "OUTPUT" becomes O U T + TOK_FE XSTMT_PUT
         * because "PUT" is a keyword. We identify mode by first letter. */
        uint8_t tok = gw_chrgot();
        if (tok == TOK_INPUT) {
            mode = 'I';
            gw_chrget();
        } else if (gw_is_letter(tok)) {
            char first = toupper(tok);
            if (first == 'O') mode = 'O';
            else if (first == 'A') mode = 'A';
            else if (first == 'R') mode = 'R';
            else { free(filename); gw_error(ERR_BM); }
            /* Skip remaining letters and embedded tokens of the mode word */
            gw_chrget();
            while (gw_is_letter(gw_chrgot()) || gw_chrgot() == TOK_PREFIX_FE)  {
                if (gw_chrgot() == TOK_PREFIX_FE) {
                    gw_chrget();  /* skip prefix */
                    gw_chrget();  /* skip extended token */
                } else {
                    gw_chrget();
                }
            }
        } else {
            free(filename);
            gw_error(ERR_SN);
        }

        gw_skip_spaces();
        /* Skip "AS" - not a token, just letters */
        if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'A') {
            gw_chrget();
            if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'S')
                gw_chrget();
        }

        gw_skip_spaces();
        if (gw_chrgot() == '#')
            gw_chrget();
        file_num = gw_eval_int();
    } else if (gw_chrgot() == ',') {
        /* Compact syntax: OPEN "I", #n, "file" */
        gw_chrget();
        if (strlen(first) >= 1) {
            char mc = toupper(first[0]);
            if (mc == 'I') mode = 'I';
            else if (mc == 'O') mode = 'O';
            else if (mc == 'A') mode = 'A';
            else if (mc == 'R') mode = 'R';  /* random - treat as input for now */
            else { free(first); gw_error(ERR_BM); }
        }
        free(first);
        first = NULL;

        gw_skip_spaces();
        if (gw_chrgot() == '#')
            gw_chrget();
        file_num = gw_eval_int();

        gw_skip_spaces();
        gw_expect(',');
        gw_value_t fname_val = gw_eval_str();
        filename = gw_str_to_cstr(&fname_val.sval);
        gw_str_free(&fname_val.sval);
    } else {
        free(first);
        gw_error(ERR_SN);
    }

    if (file_num < 1 || file_num > 15) {
        free(filename);
        gw_error(ERR_BN);
    }
    if (gw.files[file_num].fp) {
        free(filename);
        gw_error(ERR_AO);
    }

    const char *fmode;
    switch (mode) {
    case 'I': fmode = "r"; break;
    case 'O': fmode = "w"; break;
    case 'A': fmode = "a"; break;
    case 'R': fmode = "r+"; break;
    default:
        free(filename);
        gw_error(ERR_BM);
        return;
    }

    FILE *fp = fopen(filename, fmode);
    if (!fp) {
        /* For random mode, try creating if doesn't exist */
        if (mode == 'R')
            fp = fopen(filename, "w+");
        if (!fp) {
            free(filename);
            gw_error(ERR_FF);
        }
    }
    free(filename);

    gw.files[file_num].fp = fp;
    gw.files[file_num].mode = mode;
    gw.files[file_num].file_num = file_num;
    gw.files[file_num].eof_flag = false;
    gw.files[file_num].field_buf = NULL;
    gw.files[file_num].field_count = 0;

    /* Parse optional record length */
    int reclen = 128;
    gw_skip_spaces();
    /* Modern syntax: LEN = n */
    if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'L') {
        /* Skip LEN */
        while (gw_is_letter(gw_chrgot())) gw_chrget();
        gw_skip_spaces();
        if (gw_chrgot() == TOK_EQ) {
            gw_chrget();
            reclen = gw_eval_int();
        }
    } else if (gw_chrgot() == ',') {
        /* Compact syntax: , reclen */
        gw_chrget();
        reclen = gw_eval_int();
    }

    gw.files[file_num].record_len = reclen;
    if (mode == 'R') {
        gw.files[file_num].field_buf = calloc(1, reclen);
        if (!gw.files[file_num].field_buf) gw_error(ERR_OM);
    }
}

/* CLOSE [#n [, #n ...]] */
void gw_stmt_close(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == 0 || gw_chrgot() == ':') {
        gw_file_close_all();
        return;
    }

    for (;;) {
        gw_skip_spaces();
        if (gw_chrgot() == '#')
            gw_chrget();
        int num = gw_eval_int();
        if (num < 1 || num > 15)
            gw_error(ERR_BN);
        if (gw.files[num].fp) {
            fclose(gw.files[num].fp);
            gw.files[num].fp = NULL;
            gw.files[num].mode = 0;
            gw.files[num].eof_flag = false;
            free(gw.files[num].field_buf);
            gw.files[num].field_buf = NULL;
            gw.files[num].field_count = 0;
            gw.files[num].record_len = 0;
        }
        gw_skip_spaces();
        if (gw_chrgot() != ',')
            break;
        gw_chrget();
    }
}

/* Helper: write a gw_value_t to a FILE* */
static void fprint_value(FILE *fp, gw_value_t *v)
{
    if (v->type == VT_STR) {
        fwrite(v->sval.data, 1, v->sval.len, fp);
        gw_str_free(&v->sval);
    } else {
        char buf[64];
        gw_format_number(v, buf, sizeof(buf));
        fputs(buf, fp);
        fputc(' ', fp);
    }
}

/* PRINT #n, ... */
void gw_stmt_print_file(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == '#')
        gw_chrget();
    int num = gw_eval_int();
    file_entry_t *f = gw_file_get(num);
    if (f->mode != 'O' && f->mode != 'A')
        gw_error(ERR_BM);

    gw_skip_spaces();
    if (gw_chrgot() == ',')
        gw_chrget();

    /* Check for USING */
    gw_skip_spaces();
    if (gw_chrgot() == TOK_USING) {
        gw_chrget();
        gw_print_using(f->fp);
        return;
    }

    int need_newline = 1;
    for (;;) {
        gw_skip_spaces();
        uint8_t tok = gw_chrgot();

        if (tok == 0 || tok == ':' || tok == TOK_ELSE)
            break;

        if (tok == ';') {
            gw_chrget();
            need_newline = 0;
            continue;
        }
        if (tok == ',') {
            gw_chrget();
            /* Tab to next 14-char zone */
            /* Tab to next zone: approximate with comma */
            fputc(',', f->fp);
            need_newline = 0;
            continue;
        }

        gw_value_t v = gw_eval();
        fprint_value(f->fp, &v);
        need_newline = 1;
    }

    if (need_newline)
        fputc('\n', f->fp);
    fflush(f->fp);
}

/* WRITE #n, ... - CSV format with quotes around strings */
void gw_stmt_write_file(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == '#')
        gw_chrget();
    int num = gw_eval_int();
    file_entry_t *f = gw_file_get(num);
    if (f->mode != 'O' && f->mode != 'A')
        gw_error(ERR_BM);

    gw_skip_spaces();
    if (gw_chrgot() == ',')
        gw_chrget();

    int first = 1;
    for (;;) {
        gw_skip_spaces();
        uint8_t ch = gw_chrgot();
        if (ch == 0 || ch == ':') break;
        if (!first) fputc(',', f->fp);

        gw_value_t v = gw_eval();
        if (v.type == VT_STR) {
            fputc('"', f->fp);
            fwrite(v.sval.data, 1, v.sval.len, f->fp);
            fputc('"', f->fp);
            gw_str_free(&v.sval);
        } else {
            char buf[64];
            gw_format_number(&v, buf, sizeof(buf));
            /* Strip leading space from positive numbers */
            char *p = buf;
            while (*p == ' ') p++;
            fputs(p, f->fp);
        }
        first = 0;

        gw_skip_spaces();
        if (gw_chrgot() == ',') { gw_chrget(); continue; }
        if (gw_chrgot() == ';') { gw_chrget(); continue; }
        break;
    }
    fputc('\n', f->fp);
    fflush(f->fp);
}

/* INPUT #n, var, var... */
void gw_stmt_input_file(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == '#')
        gw_chrget();
    int num = gw_eval_int();
    file_entry_t *f = gw_file_get(num);
    if (f->mode != 'I' && f->mode != 'R')
        gw_error(ERR_BM);

    gw_skip_spaces();
    if (gw_chrgot() == ',')
        gw_chrget();

    /* Read a line from the file */
    char linebuf[256];
    if (!fgets(linebuf, sizeof(linebuf), f->fp)) {
        f->eof_flag = true;
        gw_error(ERR_EF);
    }
    int len = strlen(linebuf);
    while (len > 0 && (linebuf[len - 1] == '\n' || linebuf[len - 1] == '\r'))
        linebuf[--len] = '\0';

    const char *p = linebuf;

    for (;;) {
        gw_skip_spaces();
        uint8_t ch = gw_chrgot();
        if (ch == 0 || ch == ':') break;

        char name[2];
        gw_valtype_t type = gw_parse_varname(name);

        gw_skip_spaces();
        var_entry_t *var = NULL;
        gw_value_t *arr_elem = NULL;
        if (gw_chrgot() == '(') {
            arr_elem = gw_array_element(name, type);
        } else {
            var = gw_var_find_or_create(name, type);
        }

        while (*p == ' ') p++;

        gw_value_t val;
        if (type == VT_STR) {
            const char *start = p;
            if (*p == '"') {
                p++;
                start = p;
                while (*p && *p != '"') p++;
                int slen = p - start;
                val.type = VT_STR;
                val.sval = gw_str_alloc(slen);
                memcpy(val.sval.data, start, slen);
                if (*p == '"') p++;
            } else {
                while (*p && *p != ',') p++;
                int slen = p - start;
                while (slen > 0 && start[slen - 1] == ' ') slen--;
                val.type = VT_STR;
                val.sval = gw_str_alloc(slen);
                memcpy(val.sval.data, start, slen);
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

        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
        } else {
            break;
        }
    }

    if (feof(f->fp))
        f->eof_flag = true;
}

/* LINE INPUT #n, var$ */
void gw_stmt_line_input_file(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == '#')
        gw_chrget();
    int num = gw_eval_int();
    file_entry_t *f = gw_file_get(num);
    if (f->mode != 'I' && f->mode != 'R')
        gw_error(ERR_BM);

    gw_skip_spaces();
    if (gw_chrgot() == ',')
        gw_chrget();

    char name[2];
    gw_valtype_t type = gw_parse_varname(name);
    if (type != VT_STR)
        gw_error(ERR_TM);

    char linebuf[256];
    if (!fgets(linebuf, sizeof(linebuf), f->fp)) {
        f->eof_flag = true;
        gw_error(ERR_EF);
    }
    int len = strlen(linebuf);
    while (len > 0 && (linebuf[len - 1] == '\n' || linebuf[len - 1] == '\r'))
        linebuf[--len] = '\0';

    gw_value_t val;
    val.type = VT_STR;
    val.sval = gw_str_from_cstr(linebuf);

    var_entry_t *var = gw_var_find_or_create(name, type);
    gw_var_assign(var, &val);

    if (feof(f->fp))
        f->eof_flag = true;
}

/* ================================================================
 * Random-access file I/O: FIELD, LSET, RSET, PUT, GET
 * ================================================================ */

static void field_buf_to_vars(file_entry_t *f);
static void vars_to_field_buf(file_entry_t *f);

/* FIELD #n, width AS var$ [, width AS var$ ...] */
void gw_stmt_field(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == '#')
        gw_chrget();
    int num = gw_eval_int();
    file_entry_t *f = gw_file_get(num);
    if (f->mode != 'R')
        gw_error(ERR_BM);

    gw_skip_spaces();
    if (gw_chrgot() == ',')
        gw_chrget();

    f->field_count = 0;
    int offset = 0;

    for (;;) {
        gw_skip_spaces();
        if (gw_chrgot() == 0 || gw_chrgot() == ':')
            break;

        int width = gw_eval_int();
        if (width < 0) gw_error(ERR_FC);
        if (offset + width > f->record_len) gw_error(ERR_FO);

        gw_skip_spaces();
        /* Skip AS keyword */
        if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'A') {
            gw_chrget();
            if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'S')
                gw_chrget();
        }

        gw_skip_spaces();
        char name[2];
        gw_valtype_t type = gw_parse_varname(name);
        if (type != VT_STR) gw_error(ERR_TM);

        if (f->field_count >= 32) gw_error(ERR_FO);
        f->fields[f->field_count].name[0] = name[0];
        f->fields[f->field_count].name[1] = name[1];
        f->fields[f->field_count].type = type;
        f->fields[f->field_count].offset = offset;
        f->fields[f->field_count].width = width;
        f->field_count++;

        offset += width;

        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
            continue;
        }
        break;
    }

    /* Initialize FIELD variables with proper widths (space-filled) */
    field_buf_to_vars(f);
}

/* Copy field buffer data into the FIELD variables */
static void field_buf_to_vars(file_entry_t *f)
{
    for (int i = 0; i < f->field_count; i++) {
        var_entry_t *var = gw_var_find_or_create(f->fields[i].name,
                                                  f->fields[i].type);
        gw_str_free(&var->val.sval);
        var->val.type = VT_STR;
        var->val.sval = gw_str_alloc(f->fields[i].width);
        memcpy(var->val.sval.data,
               f->field_buf + f->fields[i].offset,
               f->fields[i].width);
    }
}

/* Copy FIELD variables into the field buffer */
static void vars_to_field_buf(file_entry_t *f)
{
    for (int i = 0; i < f->field_count; i++) {
        var_entry_t *var = gw_var_find_or_create(f->fields[i].name,
                                                  f->fields[i].type);
        int width = f->fields[i].width;
        int len = (var->val.type == VT_STR) ? var->val.sval.len : 0;
        int copy = (len < width) ? len : width;
        if (copy > 0)
            memcpy(f->field_buf + f->fields[i].offset,
                   var->val.sval.data, copy);
        if (copy < width)
            memset(f->field_buf + f->fields[i].offset + copy, ' ',
                   width - copy);
    }
}

/* LSET var$ = expr$ - left-justify into field variable */
void gw_stmt_lset(void)
{
    gw_skip_spaces();
    char name[2];
    gw_valtype_t type = gw_parse_varname(name);
    if (type != VT_STR) gw_error(ERR_TM);
    var_entry_t *var = gw_var_find_or_create(name, type);

    gw_skip_spaces();
    gw_expect(TOK_EQ);
    gw_value_t rhs = gw_eval_str();

    int target_len = var->val.sval.len;
    if (target_len == 0) {
        gw_str_free(&rhs.sval);
        return;
    }

    int copy = (rhs.sval.len < target_len) ? rhs.sval.len : target_len;
    memcpy(var->val.sval.data, rhs.sval.data, copy);
    if (copy < target_len)
        memset(var->val.sval.data + copy, ' ', target_len - copy);
    gw_str_free(&rhs.sval);
}

/* RSET var$ = expr$ - right-justify into field variable */
void gw_stmt_rset(void)
{
    gw_skip_spaces();
    char name[2];
    gw_valtype_t type = gw_parse_varname(name);
    if (type != VT_STR) gw_error(ERR_TM);
    var_entry_t *var = gw_var_find_or_create(name, type);

    gw_skip_spaces();
    gw_expect(TOK_EQ);
    gw_value_t rhs = gw_eval_str();

    int target_len = var->val.sval.len;
    if (target_len == 0) {
        gw_str_free(&rhs.sval);
        return;
    }

    int copy = (rhs.sval.len < target_len) ? rhs.sval.len : target_len;
    int pad = target_len - copy;
    if (pad > 0)
        memset(var->val.sval.data, ' ', pad);
    memcpy(var->val.sval.data + pad, rhs.sval.data, copy);
    gw_str_free(&rhs.sval);
}

/* PUT #n [, record] - write field buffer to file */
void gw_stmt_put(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == '#')
        gw_chrget();
    int num = gw_eval_int();
    file_entry_t *f = gw_file_get(num);
    if (f->mode != 'R')
        gw_error(ERR_BM);

    long record = -1;
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        gw_chrget();
        record = gw_eval_int();
        if (record < 1) gw_error(ERR_RN);
    }

    vars_to_field_buf(f);

    if (record > 0)
        fseek(f->fp, (long)(record - 1) * f->record_len, SEEK_SET);

    fwrite(f->field_buf, 1, f->record_len, f->fp);
    fflush(f->fp);
}

/* GET #n [, record] - read record from file into field buffer */
void gw_stmt_get(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == '#')
        gw_chrget();
    int num = gw_eval_int();
    file_entry_t *f = gw_file_get(num);
    if (f->mode != 'R')
        gw_error(ERR_BM);

    long record = -1;
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        gw_chrget();
        record = gw_eval_int();
        if (record < 1) gw_error(ERR_RN);
    }

    if (record > 0)
        fseek(f->fp, (long)(record - 1) * f->record_len, SEEK_SET);

    memset(f->field_buf, 0, f->record_len);
    size_t got = fread(f->field_buf, 1, f->record_len, f->fp);
    if (got == 0)
        f->eof_flag = true;

    field_buf_to_vars(f);
}

/* ================================================================
 * MBF Conversion Functions: CVI, CVS, CVD, MKI$, MKS$, MKD$
 * ================================================================ */

gw_value_t gw_fn_cvi(gw_value_t *s)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    if (s->sval.len < 2) gw_error(ERR_FC);
    gw_value_t v;
    v.type = VT_INT;
    v.ival = (int16_t)((uint8_t)s->sval.data[0] |
                       ((uint8_t)s->sval.data[1] << 8));
    gw_str_free(&s->sval);
    return v;
}

gw_value_t gw_fn_cvs(gw_value_t *s)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    if (s->sval.len < 4) gw_error(ERR_FC);
    gw_value_t v;
    v.type = VT_SNG;
    memcpy(&v.fval, s->sval.data, 4);
    gw_str_free(&s->sval);
    return v;
}

gw_value_t gw_fn_cvd(gw_value_t *s)
{
    if (s->type != VT_STR) gw_error(ERR_TM);
    if (s->sval.len < 8) gw_error(ERR_FC);
    gw_value_t v;
    v.type = VT_DBL;
    memcpy(&v.dval, s->sval.data, 8);
    gw_str_free(&s->sval);
    return v;
}

gw_value_t gw_fn_mki(int16_t n)
{
    gw_value_t v;
    v.type = VT_STR;
    v.sval = gw_str_alloc(2);
    v.sval.data[0] = (char)(n & 0xFF);
    v.sval.data[1] = (char)((n >> 8) & 0xFF);
    return v;
}

gw_value_t gw_fn_mks(float f)
{
    gw_value_t v;
    v.type = VT_STR;
    v.sval = gw_str_alloc(4);
    memcpy(v.sval.data, &f, 4);
    return v;
}

gw_value_t gw_fn_mkd(double d)
{
    gw_value_t v;
    v.type = VT_STR;
    v.sval = gw_str_alloc(8);
    memcpy(v.sval.data, &d, 8);
    return v;
}
