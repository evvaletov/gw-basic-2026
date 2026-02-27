#include "gwbasic.h"
#include "tui.h"
#include "graphics.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/*
 * Expression evaluator - reimplements GWEVAL.ASM's FRMEVL.
 * Uses recursive descent with precedence climbing.
 *
 * Operator precedence (from GWDATA.ASM):
 *   ^          127
 *   * /        124
 *   \          123
 *   MOD        122
 *   + -        121
 *   relational 64
 *   NOT        90
 *   AND        70
 *   OR         60
 *   XOR        50
 *   EQV        40
 *   IMP        40
 */

/* Forward declarations */
static gw_value_t eval_expr(int min_prec);
static gw_value_t eval_unary(void);
static gw_value_t eval_atom(void);
static gw_value_t eval_function(uint8_t prefix, uint8_t func_tok);
static gw_value_t eval_paren(void);
static gw_value_t eval_number(void);
static gw_value_t eval_string_literal(void);

/* Operator precedence lookup */
static int op_prec(uint8_t tok)
{
    switch (tok) {
    case TOK_IMP:   return 40;
    case TOK_EQV:   return 42;
    case TOK_XOR:   return 44;
    case TOK_OR:    return 46;
    case TOK_AND:   return 48;
    case TOK_GT:
    case TOK_EQ:
    case TOK_LT:    return 64;
    case TOK_PLUS:
    case TOK_MINUS:  return 121;
    case TOK_MOD:    return 122;
    case TOK_IDIV:   return 123;
    case TOK_MUL:
    case TOK_DIV:    return 124;
    case TOK_POW:    return 127;
    default:         return -1;
    }
}

/* Apply a binary operator */
static gw_value_t apply_binop(uint8_t op, gw_value_t left, gw_value_t right)
{
    gw_value_t result;

    /* String concatenation */
    if (op == TOK_PLUS && left.type == VT_STR && right.type == VT_STR)
        return gw_str_concat(&left, &right);

    /* Relational operators can compare strings */
    if ((op == TOK_GT || op == TOK_EQ || op == TOK_LT)
        && left.type == VT_STR && right.type == VT_STR) {
        char *ls = gw_str_to_cstr(&left.sval);
        char *rs = gw_str_to_cstr(&right.sval);
        int cmp = strcmp(ls, rs);
        free(ls); free(rs);
        gw_str_free(&left.sval);
        gw_str_free(&right.sval);
        result.type = VT_INT;
        switch (op) {
        case TOK_GT: result.ival = cmp > 0 ? -1 : 0; break;
        case TOK_EQ: result.ival = cmp == 0 ? -1 : 0; break;
        case TOK_LT: result.ival = cmp < 0 ? -1 : 0; break;
        }
        return result;
    }

    /* Type mismatch if mixed string/numeric */
    if (left.type == VT_STR || right.type == VT_STR)
        gw_error(ERR_TM);

    /* Logical/bitwise operators: force to integer */
    if (op == TOK_AND || op == TOK_OR || op == TOK_XOR
        || op == TOK_EQV || op == TOK_IMP) {
        int16_t a = gw_to_int(&left);
        int16_t b = gw_to_int(&right);
        result.type = VT_INT;
        switch (op) {
        case TOK_AND: result.ival = a & b; break;
        case TOK_OR:  result.ival = a | b; break;
        case TOK_XOR: result.ival = a ^ b; break;
        case TOK_EQV: result.ival = ~(a ^ b); break;
        case TOK_IMP: result.ival = (~a) | b; break;
        }
        return result;
    }

    /* Integer division and MOD: force to integer */
    if (op == TOK_IDIV || op == TOK_MOD) {
        int16_t a = gw_to_int(&left);
        int16_t b = gw_to_int(&right);
        if (b == 0) gw_error(ERR_DZ);
        result.type = VT_INT;
        if (op == TOK_IDIV)
            result.ival = a / b;
        else
            result.ival = a % b;
        return result;
    }

    /* Promote to common numeric type */
    gw_promote(&left, &right);

    /* Integer arithmetic */
    if (left.type == VT_INT) {
        result.type = VT_INT;
        switch (op) {
        case TOK_PLUS:  result.ival = gw_int_add(left.ival, right.ival); break;
        case TOK_MINUS: result.ival = gw_int_sub(left.ival, right.ival); break;
        case TOK_MUL:   result.ival = gw_int_mul(left.ival, right.ival); break;
        case TOK_DIV:
            /* Integer / integer -> single in GW-BASIC */
            if (right.ival == 0) gw_error(ERR_DZ);
            result.type = VT_SNG;
            result.fval = (float)left.ival / (float)right.ival;
            break;
        case TOK_POW:
            result.type = VT_SNG;
            result.fval = (float)gw_fpow(left.ival, right.ival);
            break;
        case TOK_GT: result.ival = left.ival > right.ival ? -1 : 0; break;
        case TOK_EQ: result.ival = left.ival == right.ival ? -1 : 0; break;
        case TOK_LT: result.ival = left.ival < right.ival ? -1 : 0; break;
        default: gw_error(ERR_SN);
        }
        return result;
    }

    /* Float/double arithmetic */
    double a = (left.type == VT_SNG) ? left.fval : left.dval;
    double b = (right.type == VT_SNG) ? right.fval : right.dval;
    double r;
    int is_relational = 0;

    switch (op) {
    case TOK_PLUS:  r = gw_fadd(a, b); break;
    case TOK_MINUS: r = gw_fsub(a, b); break;
    case TOK_MUL:   r = gw_fmul(a, b); break;
    case TOK_DIV:   r = gw_fdiv(a, b); break;
    case TOK_POW:   r = gw_fpow(a, b); break;
    case TOK_GT:    result.type = VT_INT; result.ival = a > b ? -1 : 0; is_relational = 1; break;
    case TOK_EQ:    result.type = VT_INT; result.ival = a == b ? -1 : 0; is_relational = 1; break;
    case TOK_LT:    result.type = VT_INT; result.ival = a < b ? -1 : 0; is_relational = 1; break;
    default: gw_error(ERR_SN); r = 0;
    }

    if (!is_relational) {
        if (left.type == VT_DBL || right.type == VT_DBL) {
            result.type = VT_DBL;
            result.dval = r;
        } else {
            result.type = VT_SNG;
            result.fval = (float)r;
        }
    }
    return result;
}

/* Combined relational: handle >=, <=, <> as combined tokens */
static int try_combined_relational(uint8_t first, gw_value_t left, gw_value_t *result)
{
    uint8_t *save = gw.text_ptr;
    gw_skip_spaces();
    uint8_t next = gw_chrgot();

    if (first == TOK_LT && next == TOK_GT) {
        /* <> (not equal) */
        gw.text_ptr++;  /* consume '>' */
        gw_value_t right = eval_expr(65);
        *result = apply_binop(TOK_EQ, left, right);
        result->ival = ~result->ival;
        return 1;
    }
    if (first == TOK_LT && next == TOK_EQ) {
        /* <= */
        gw.text_ptr++;
        gw_value_t right = eval_expr(65);
        gw_value_t gt_result = apply_binop(TOK_GT, left, right);
        result->type = VT_INT;
        result->ival = ~gt_result.ival;
        return 1;
    }
    if (first == TOK_GT && next == TOK_EQ) {
        /* >= */
        gw.text_ptr++;
        gw_value_t right = eval_expr(65);
        gw_value_t lt_result = apply_binop(TOK_LT, left, right);
        result->type = VT_INT;
        result->ival = ~lt_result.ival;
        return 1;
    }
    if (first == TOK_EQ && (next == TOK_LT || next == TOK_GT)) {
        /* =< or => (unusual but valid) */
        gw.text_ptr++;
        gw_value_t right = eval_expr(65);
        if (next == TOK_LT) {
            gw_value_t gt_result = apply_binop(TOK_GT, left, right);
            result->type = VT_INT;
            result->ival = ~gt_result.ival;
        } else {
            gw_value_t lt_result = apply_binop(TOK_LT, left, right);
            result->type = VT_INT;
            result->ival = ~lt_result.ival;
        }
        return 1;
    }

    /* Not a combined relational, put back */
    gw.text_ptr = save;
    return 0;
}

/* Main expression evaluator entry point */
static gw_value_t eval_expr(int min_prec)
{
    gw_value_t left = eval_unary();

    for (;;) {
        gw_skip_spaces();
        uint8_t tok = gw_chrgot();

        int prec = op_prec(tok);
        if (prec < min_prec)
            break;

        gw_chrget();  /* consume operator */

        /* Handle combined relationals like >=, <=, <> */
        if (tok == TOK_GT || tok == TOK_LT || tok == TOK_EQ) {
            gw_value_t combined;
            if (try_combined_relational(tok, left, &combined)) {
                left = combined;
                continue;
            }
        }

        gw_value_t right = eval_expr(prec + 1);
        left = apply_binop(tok, left, right);
    }

    return left;
}

/* Unary operators: NOT, unary +, unary - */
static gw_value_t eval_unary(void)
{
    gw_skip_spaces();
    uint8_t tok = gw_chrgot();

    if (tok == TOK_MINUS) {
        gw_chrget();
        gw_value_t v = eval_unary();
        if (v.type == VT_STR) gw_error(ERR_TM);
        if (v.type == VT_INT) {
            v.ival = gw_int_neg(v.ival);
        } else if (v.type == VT_SNG) {
            v.fval = -v.fval;
        } else {
            v.dval = -v.dval;
        }
        return v;
    }

    if (tok == TOK_PLUS) {
        gw_chrget();
        gw_value_t v = eval_unary();
        if (v.type == VT_STR) gw_error(ERR_TM);
        return v;
    }

    if (tok == TOK_NOT) {
        gw_chrget();
        gw_value_t v = eval_expr(50);
        int16_t i = gw_to_int(&v);
        gw_value_t result;
        result.type = VT_INT;
        result.ival = ~i;
        return result;
    }

    return eval_atom();
}

/* Parse a parenthesized expression */
static gw_value_t eval_paren(void)
{
    gw_chrget();  /* skip '(' */
    gw_value_t v = eval_expr(0);
    gw_skip_spaces();
    if (gw_chrgot() != ')')
        gw_error(ERR_SN);
    gw_chrget();  /* skip ')' */
    return v;
}

/* Parse a numeric literal from token stream */
static gw_value_t eval_number(void)
{
    gw_value_t v;
    uint8_t tok = gw_chrgot();

    /* Single-byte integer constant 0-9 */
    if (tok >= 0x11 && tok <= 0x1A) {
        gw.text_ptr++;
        v.type = VT_INT;
        v.ival = tok - 0x11;
        return v;
    }

    /* One-byte integer constant (TOK_INT1 followed by 1 data byte) */
    if (tok == TOK_INT1) {
        gw.text_ptr++;
        v.type = VT_INT;
        v.ival = *gw.text_ptr++;
        return v;
    }

    /* Two-byte integer constant (TOK_INT2 followed by 2 data bytes) */
    if (tok == TOK_INT2) {
        gw.text_ptr++;
        uint8_t lo = *gw.text_ptr++;
        uint8_t hi = *gw.text_ptr++;
        v.type = VT_INT;
        v.ival = (int16_t)(lo | (hi << 8));
        return v;
    }

    /* Single precision constant (0x1C followed by 4 data bytes) */
    if (tok == TOK_CONST_SNG) {
        gw.text_ptr++;
        float fval;
        memcpy(&fval, gw.text_ptr, 4);
        gw.text_ptr += 4;
        v.type = VT_SNG;
        v.fval = fval;
        return v;
    }

    /* Double precision constant (0x1F followed by 8 data bytes) */
    if (tok == TOK_CONST_DBL) {
        gw.text_ptr++;
        double dval;
        memcpy(&dval, gw.text_ptr, 8);
        gw.text_ptr += 8;
        v.type = VT_DBL;
        v.dval = dval;
        return v;
    }

    gw_error(ERR_SN);
    v.type = VT_INT;
    v.ival = 0;
    return v;
}

/* Parse a string literal */
static gw_value_t eval_string_literal(void)
{
    /* Advance past the opening quote - don't skip spaces */
    gw.text_ptr++;
    uint8_t *start = gw.text_ptr;
    int len = 0;

    while (*gw.text_ptr && *gw.text_ptr != '"') {
        gw.text_ptr++;
        len++;
    }

    gw_value_t v;
    v.type = VT_STR;
    v.sval = gw_str_alloc(len);
    memcpy(v.sval.data, start, len);

    if (*gw.text_ptr == '"')
        gw.text_ptr++;  /* skip closing " - don't use chrget, next token reads normally */

    return v;
}

/* Evaluate built-in functions (0xFF prefix) */
static gw_value_t eval_function(uint8_t prefix, uint8_t func_tok)
{
    gw_value_t v, arg;
    int n;

    /* Most functions: FUNCNAME(expr) */
    switch (func_tok) {
    /* Numeric -> Numeric functions */
    case FUNC_SGN:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_INT;
        v.ival = (int16_t)gw_sgn(gw_to_dbl(&arg));
        return v;

    case FUNC_INT:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = (arg.type == VT_DBL) ? VT_DBL : VT_SNG;
        if (v.type == VT_DBL) v.dval = gw_int_fn(arg.dval);
        else v.fval = (float)gw_int_fn(gw_to_dbl(&arg));
        return v;

    case FUNC_ABS:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = arg.type;
        if (arg.type == VT_INT) v.ival = arg.ival < 0 ? gw_int_neg(arg.ival) : arg.ival;
        else if (arg.type == VT_SNG) v.fval = (float)gw_abs(arg.fval);
        else v.dval = gw_abs(arg.dval);
        return v;

    case FUNC_SQR:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_sqr(gw_to_dbl(&arg));
        if (arg.type != VT_DBL) { v.type = VT_SNG; v.fval = (float)v.dval; }
        return v;

    case FUNC_SIN:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_sin(gw_to_dbl(&arg));
        if (arg.type != VT_DBL) { v.type = VT_SNG; v.fval = (float)v.dval; }
        return v;

    case FUNC_COS:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_cos(gw_to_dbl(&arg));
        if (arg.type != VT_DBL) { v.type = VT_SNG; v.fval = (float)v.dval; }
        return v;

    case FUNC_TAN:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_tan(gw_to_dbl(&arg));
        if (arg.type != VT_DBL) { v.type = VT_SNG; v.fval = (float)v.dval; }
        return v;

    case FUNC_ATN:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_atn(gw_to_dbl(&arg));
        if (arg.type != VT_DBL) { v.type = VT_SNG; v.fval = (float)v.dval; }
        return v;

    case FUNC_LOG:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_log(gw_to_dbl(&arg));
        if (arg.type != VT_DBL) { v.type = VT_SNG; v.fval = (float)v.dval; }
        return v;

    case FUNC_EXP:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_exp(gw_to_dbl(&arg));
        if (arg.type != VT_DBL) { v.type = VT_SNG; v.fval = (float)v.dval; }
        return v;

    case FUNC_RND: {
        double rnd_arg = 1.0;
        gw_skip_spaces();
        if (gw_chrgot() == '(') {
            gw_chrget();
            arg = gw_eval_num();
            gw_expect_rparen();
            rnd_arg = gw_to_dbl(&arg);
        }
        v.type = VT_SNG;
        v.fval = (float)gw_rnd(rnd_arg);
        return v;
    }

    case FUNC_FIX:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = arg.type;
        if (arg.type == VT_INT) v.ival = arg.ival;
        else if (arg.type == VT_SNG) v.fval = (float)gw_fix(arg.fval);
        else v.dval = gw_fix(arg.dval);
        return v;

    case FUNC_CINT:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_INT;
        v.ival = gw_cint(gw_to_dbl(&arg));
        return v;

    case FUNC_CSNG:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_SNG;
        v.fval = gw_csng(gw_to_dbl(&arg));
        return v;

    case FUNC_CDBL:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        v.type = VT_DBL;
        v.dval = gw_cdbl(&arg);
        return v;

    /* String functions */
    case FUNC_LEN:
        gw_expect('(');
        arg = gw_eval_str();
        gw_expect_rparen();
        return gw_fn_len(&arg);

    case FUNC_ASC:
        gw_expect('(');
        arg = gw_eval_str();
        gw_expect_rparen();
        return gw_fn_asc(&arg);

    case FUNC_CHR:
        gw_expect('(');
        n = gw_eval_int();
        gw_expect_rparen();
        return gw_fn_chr(n);

    case FUNC_VAL:
        gw_expect('(');
        arg = gw_eval_str();
        gw_expect_rparen();
        return gw_fn_val(&arg);

    case FUNC_STR:
        gw_expect('(');
        arg = gw_eval_num();
        gw_expect_rparen();
        return gw_fn_str(&arg);

    case FUNC_SPACE:
        gw_expect('(');
        n = gw_eval_int();
        gw_expect_rparen();
        return gw_fn_space(n);

    case FUNC_LEFT:
    {
        gw_expect('(');
        gw_value_t s = gw_eval_str();
        gw_skip_spaces();
        gw_expect(',');
        n = gw_eval_int();
        gw_expect_rparen();
        return gw_fn_left(&s, n);
    }

    case FUNC_RIGHT:
    {
        gw_expect('(');
        gw_value_t s = gw_eval_str();
        gw_skip_spaces();
        gw_expect(',');
        n = gw_eval_int();
        gw_expect_rparen();
        return gw_fn_right(&s, n);
    }

    case FUNC_MID:
    {
        gw_expect('(');
        gw_value_t s = gw_eval_str();
        gw_skip_spaces();
        gw_expect(',');
        int start = gw_eval_int();
        int len = -1;
        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
            len = gw_eval_int();
        }
        gw_expect_rparen();
        if (len < 0)
            len = 255;  /* default: rest of string */
        return gw_fn_mid(&s, start, len);
    }

    case FUNC_HEX:
        gw_expect('(');
        n = gw_eval_int();
        gw_expect_rparen();
        return gw_fn_hex(n);

    case FUNC_OCT:
        gw_expect('(');
        n = gw_eval_int();
        gw_expect_rparen();
        return gw_fn_oct(n);

    case FUNC_FRE:
        gw_expect('(');
        arg = gw_eval();  /* can be string or numeric */
        gw_expect_rparen();
        if (arg.type == VT_STR) gw_str_free(&arg.sval);
        /* Return fake free memory value */
        v.type = VT_SNG;
        v.fval = 60000.0f;
        return v;

    case FUNC_POS:
        gw_expect('(');
        gw_eval();  /* dummy argument */
        gw_expect_rparen();
        v.type = VT_INT;
        v.ival = gw_hal ? gw_hal->get_cursor_col() + 1 : 1;
        return v;

    case FUNC_LPOS:
        gw_expect('(');
        gw_eval();
        gw_expect_rparen();
        v.type = VT_INT;
        v.ival = 1;
        return v;

    case FUNC_EOF:
    {
        gw_expect('(');
        int fnum = gw_eval_int();
        gw_expect_rparen();
        v.type = VT_INT;
        v.ival = gw_file_eof(fnum);
        return v;
    }

    case FUNC_LOC:
    case FUNC_LOF:
    {
        gw_expect('(');
        int fnum = gw_eval_int();
        gw_expect_rparen();
        /* LOC and LOF: return approximate values */
        file_entry_t *lf = gw_file_get(fnum);
        v.type = VT_SNG;
        if (func_tok == FUNC_LOF) {
            long cur = ftell(lf->fp);
            fseek(lf->fp, 0, SEEK_END);
            v.fval = (float)ftell(lf->fp);
            fseek(lf->fp, cur, SEEK_SET);
        } else {
            v.fval = (float)(ftell(lf->fp) / 128 + 1);
        }
        return v;
    }

    case FUNC_INP:
    case FUNC_PEEK:
    case FUNC_PEN:
    case FUNC_STICK:
    case FUNC_STRIG:
        gw_expect('(');
        gw_eval();
        gw_expect_rparen();
        v.type = VT_INT;
        v.ival = 0;
        return v;

    default:
        gw_error(ERR_SN);
        v.type = VT_INT;
        v.ival = 0;
        return v;
    }
}

/* Atom: number, string, function, or parenthesized expression */
static gw_value_t eval_atom(void)
{
    gw_skip_spaces();
    uint8_t tok = gw_chrgot();

    /* Parenthesized expression */
    if (tok == '(')
        return eval_paren();

    /* String literal */
    if (tok == '"')
        return eval_string_literal();

    /* Numeric constant tokens: 0x0E-0x1F covers all embedded constants */
    if (tok >= TOK_INT2 && tok <= TOK_CONST_DBL)
        return eval_number();

    /* Built-in function (0xFF prefix) */
    if (tok == TOK_PREFIX_FF) {
        gw_chrget();  /* consume 0xFF */
        uint8_t func = gw_chrgot();
        gw_chrget();  /* consume function token */
        return eval_function(TOK_PREFIX_FF, func);
    }

    /* Extended function (0xFD prefix): CVI, CVS, CVD, MKI$, MKS$, MKD$ */
    if (tok == TOK_PREFIX_FD) {
        gw_chrget();
        uint8_t func = gw_chrgot();
        gw_chrget();

        switch (func) {
        case XFUNC_CVI:
        {
            gw_expect('(');
            gw_value_t arg = gw_eval_str();
            gw_expect_rparen();
            return gw_fn_cvi(&arg);
        }
        case XFUNC_CVS:
        {
            gw_expect('(');
            gw_value_t arg = gw_eval_str();
            gw_expect_rparen();
            return gw_fn_cvs(&arg);
        }
        case XFUNC_CVD:
        {
            gw_expect('(');
            gw_value_t arg = gw_eval_str();
            gw_expect_rparen();
            return gw_fn_cvd(&arg);
        }
        case XFUNC_MKI:
        {
            gw_expect('(');
            int16_t n = gw_eval_int();
            gw_expect_rparen();
            return gw_fn_mki(n);
        }
        case XFUNC_MKS:
        {
            gw_expect('(');
            gw_value_t arg = gw_eval_num();
            gw_expect_rparen();
            return gw_fn_mks(gw_to_sng(&arg));
        }
        case XFUNC_MKD:
        {
            gw_expect('(');
            gw_value_t arg = gw_eval_num();
            gw_expect_rparen();
            return gw_fn_mkd(gw_to_dbl(&arg));
        }
        default:
            gw_error(ERR_SN);
            break;
        }
    }

    /* STRING$ function (single-byte token but acts like function) */
    if (tok == TOK_STRINGS) {
        gw_chrget();
        gw_expect('(');
        int n = gw_eval_int();
        gw_skip_spaces();
        gw_expect(',');
        gw_skip_spaces();
        gw_value_t arg2 = gw_eval();
        gw_expect_rparen();
        int code;
        if (arg2.type == VT_STR) {
            if (arg2.sval.len == 0) gw_error(ERR_FC);
            code = (uint8_t)arg2.sval.data[0];
            gw_str_free(&arg2.sval);
        } else {
            code = gw_to_int(&arg2);
        }
        return gw_fn_strings(n, code);
    }

    /* INSTR function */
    if (tok == TOK_INSTR) {
        gw_chrget();
        gw_expect('(');
        /* First arg could be start position or string */
        gw_value_t first = gw_eval();
        int start = 1;
        gw_value_t haystack, needle;
        gw_skip_spaces();
        gw_expect(',');
        if (first.type != VT_STR) {
            start = gw_to_int(&first);
            haystack = gw_eval_str();
            gw_skip_spaces();
            gw_expect(',');
            needle = gw_eval_str();
        } else {
            haystack = first;
            needle = gw_eval_str();
        }
        gw_expect_rparen();
        return gw_fn_instr(start, &haystack, &needle);
    }

    /* ERL, ERR (error info pseudo-variables) */
    if (tok == TOK_ERL) {
        gw_chrget();
        gw_value_t v;
        v.type = VT_INT;
        v.ival = gw.err_line_num;
        return v;
    }
    if (tok == TOK_ERR) {
        gw_chrget();
        gw_value_t v;
        v.type = VT_INT;
        v.ival = gw_errno;
        return v;
    }

    /* POINT(x,y) function */
    if (tok == TOK_POINT) {
        gw_chrget();
        gw_expect('(');
        int px = gw_eval_int();
        gw_expect(',');
        int py = gw_eval_int();
        gw_expect_rparen();
        gw_value_t v;
        v.type = VT_INT;
        v.ival = gfx_point(px, py);
        return v;
    }

    /* CSRLIN pseudo-variable */
    if (tok == TOK_CSRLIN) {
        gw_chrget();
        gw_value_t v;
        v.type = VT_INT;
        v.ival = gw_hal ? gw_hal->get_cursor_row() + 1 : 1;
        return v;
    }

    /* INKEY$ pseudo-variable */
    if (tok == TOK_INKEYS) {
        gw_chrget();
        gw_value_t v;
        v.type = VT_STR;
        /* Check key buffer first (keys pushed back by event trapping) */
        if (!tui_keybuf_empty()) {
            int ch = tui_pop_key();
            v.sval = gw_str_alloc(1);
            v.sval.data[0] = (char)ch;
        } else if (gw_hal && gw_hal->kbhit()) {
            int ch = gw_hal->getch();
            v.sval = gw_str_alloc(1);
            v.sval.data[0] = ch;
        } else {
            v.sval = gw_str_alloc(0);
        }
        return v;
    }

    /* INPUT$ function: INPUT$(n [,#filenum]) */
    if (tok == TOK_INPUT) {
        uint8_t *save = gw.text_ptr;
        gw_chrget();
        if (gw_chrgot() == '$') {
            gw_chrget();
            gw_expect('(');
            int n = gw_eval_int();
            if (n < 1 || n > 255) gw_error(ERR_FC);
            int filenum = 0;
            gw_skip_spaces();
            if (gw_chrgot() == ',') {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() == '#') gw_chrget();
                filenum = gw_eval_int();
            }
            gw_expect_rparen();
            gw_value_t v;
            v.type = VT_STR;
            v.sval = gw_str_alloc(n);
            if (filenum > 0) {
                file_entry_t *fe = gw_file_get(filenum);
                for (int i = 0; i < n; i++) {
                    int ch = fgetc(fe->fp);
                    if (ch == EOF) { v.sval.len = i; break; }
                    v.sval.data[i] = ch;
                }
            } else {
                for (int i = 0; i < n; i++)
                    v.sval.data[i] = gw_hal ? gw_hal->getch() : getchar();
            }
            return v;
        }
        gw.text_ptr = save;
    }

    /* Extended statement tokens that work as functions (DATE$, TIME$, TIMER) */
    if (tok == TOK_PREFIX_FE) {
        uint8_t *save = gw.text_ptr;
        gw_chrget();
        uint8_t xtok = gw_chrgot();
        if (xtok == XSTMT_DATE || xtok == XSTMT_TIME) {
            gw_chrget();
            gw_value_t v;
            v.type = VT_STR;
            char tbuf[16];
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            if (xtok == XSTMT_DATE) {
                snprintf(tbuf, sizeof(tbuf), "%02d-%02d-%04d",
                         tm->tm_mon + 1, tm->tm_mday, tm->tm_year + 1900);
                v.sval = gw_str_from_cstr(tbuf);
            } else {
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d:%02d",
                         tm->tm_hour, tm->tm_min, tm->tm_sec);
                v.sval = gw_str_from_cstr(tbuf);
            }
            return v;
        }
        if (xtok == XSTMT_TIMER) {
            gw_chrget();
            time_t now = time(NULL);
            struct tm *tm = localtime(&now);
            gw_value_t v;
            v.type = VT_SNG;
            v.fval = (float)(tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
            return v;
        }
        gw.text_ptr = save;
    }

    /* FN call */
    if (tok == TOK_FN) {
        gw_chrget();
        return gw_eval_fn_call();
    }

    /* Variable or array element */
    if (gw_is_letter(tok)) {
        char name[2];
        gw_valtype_t type = gw_parse_varname(name);

        gw_skip_spaces();
        if (gw_chrgot() == '(') {
            /* Array element */
            gw_value_t *elem = gw_array_element(name, type);
            gw_value_t v = *elem;
            if (v.type == VT_STR && v.sval.data)
                v.sval = gw_str_copy(&elem->sval);
            return v;
        }

        /* Scalar variable */
        var_entry_t *var = gw_var_find_or_create(name, type);
        gw_value_t v = var->val;
        if (v.type == VT_STR && v.sval.data)
            v.sval = gw_str_copy(&var->val.sval);
        return v;
    }

    /* End of expression */
    if (tok == 0 || tok == ':' || tok == ')' || tok == ','
        || tok == ';' || tok == TOK_THEN || tok == TOK_ELSE
        || tok == TOK_TO || tok == TOK_STEP)
        gw_error(ERR_MO);

    gw_error(ERR_SN);
    gw_value_t dummy = { .type = VT_INT, .ival = 0 };
    return dummy;
}

/* Public API */
gw_value_t gw_eval(void)
{
    return eval_expr(0);
}

gw_value_t gw_eval_num(void)
{
    gw_value_t v = eval_expr(0);
    if (v.type == VT_STR)
        gw_error(ERR_TM);
    return v;
}

gw_value_t gw_eval_str(void)
{
    gw_value_t v = eval_expr(0);
    if (v.type != VT_STR)
        gw_error(ERR_TM);
    return v;
}

int16_t gw_eval_int(void)
{
    gw_value_t v = gw_eval_num();
    return gw_to_int(&v);
}

uint16_t gw_eval_uint16(void)
{
    gw_value_t v = gw_eval_num();
    double d = gw_to_dbl(&v);
    if (d < 0 || d > 65535)
        gw_error(ERR_FC);
    return (uint16_t)d;
}

void gw_expect_rparen(void)
{
    gw_skip_spaces();
    if (gw_chrgot() != ')')
        gw_error(ERR_SN);
    gw_chrget();
}

/* Type conversion helpers */
int16_t gw_to_int(gw_value_t *v)
{
    switch (v->type) {
    case VT_INT: return v->ival;
    case VT_SNG: return gw_cint(v->fval);
    case VT_DBL: return gw_cint(v->dval);
    default: gw_error(ERR_TM); return 0;
    }
}

float gw_to_sng(gw_value_t *v)
{
    switch (v->type) {
    case VT_INT: return (float)v->ival;
    case VT_SNG: return v->fval;
    case VT_DBL: return (float)v->dval;
    default: gw_error(ERR_TM); return 0;
    }
}

double gw_to_dbl(gw_value_t *v)
{
    switch (v->type) {
    case VT_INT: return (double)v->ival;
    case VT_SNG: return (double)v->fval;
    case VT_DBL: return v->dval;
    default: gw_error(ERR_TM); return 0;
    }
}
