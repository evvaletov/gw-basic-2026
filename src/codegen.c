/*
 * C code generator for the GW-BASIC compiler.
 *
 * Walks the tokenized program (program_line_t linked list) and emits C
 * source that calls into the gwrt runtime library. Expressions are
 * compiled inline using the same precedence-climbing structure as eval.c.
 * All control flow uses goto/labels (no C for/while) so GOTO into loops works.
 */

#include "codegen.h"
#include "gwbasic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* ---- Emit helpers ---- */

static FILE *out;
static analysis_t *ana;
static uint8_t *tp;  /* token pointer (mirrors gw.text_ptr) */
static int ret_label_counter;
static int for_label_counter;

/* FOR stack: maps variable to its for_label_counter */
#define FOR_STACK_MAX 16
static struct { char name[2]; gw_valtype_t type; int label; } for_stack[FOR_STACK_MAX];
static int for_stack_sp;

#define EMIT(...) fprintf(out, __VA_ARGS__)

static uint8_t cur(void) { return *tp; }
static uint8_t advance(void)
{
    tp++;
    while (*tp == ' ') tp++;
    return *tp;
}
static void skip_spaces(void) { while (*tp == ' ') tp++; }

/* ---- Token stream readers ---- */

static uint16_t read_int(void)
{
    uint8_t tok = *tp;
    if (tok >= 0x11 && tok <= 0x1A) { tp++; return tok - 0x11; }
    if (tok == TOK_INT1) { uint16_t v = tp[1]; tp += 2; return v; }
    if (tok == TOK_INT2) { uint16_t v = tp[1] | (tp[2] << 8); tp += 3; return v; }
    return 0;
}

static bool is_const(uint8_t tok)
{
    return (tok >= 0x11 && tok <= 0x1A) || tok == TOK_INT1 || tok == TOK_INT2
        || tok == TOK_CONST_SNG || tok == TOK_CONST_DBL;
}

static bool is_letter(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/* ---- Variable naming ---- */

static const char *type_suffix(gw_valtype_t t)
{
    switch (t) {
    case VT_INT: return "int";
    case VT_SNG: return "sng";
    case VT_DBL: return "dbl";
    case VT_STR: return "str";
    default: return "sng";
    }
}

static const char *c_type(gw_valtype_t t)
{
    switch (t) {
    case VT_INT: return "int16_t";
    case VT_SNG: return "float";
    case VT_DBL: return "double";
    case VT_STR: return "gw_string_t";
    default: return "float";
    }
}

/* Emit a 2-char name as a C string literal: "AB" or "A" */
static void emit_name_str(const char name[2])
{
    if (name[1])
        EMIT("\"%c%c\"", name[0], name[1]);
    else
        EMIT("\"%c\"", name[0]);
}

/* Emit C variable name: var_AB_int */
static void emit_varname(const char name[2], gw_valtype_t type)
{
    if (name[1])
        EMIT("var_%c%c_%s", toupper(name[0]), toupper(name[1]), type_suffix(type));
    else
        EMIT("var_%c_%s", toupper(name[0]), type_suffix(type));
}

/* Parse a variable name from the token stream, return its type */
static gw_valtype_t parse_var(char name_out[2])
{
    name_out[0] = toupper(cur());
    name_out[1] = 0;
    advance();
    if (is_letter(cur()) || (cur() >= '0' && cur() <= '9')) {
        name_out[1] = toupper(cur());
        advance();
        while (is_letter(cur()) || (cur() >= '0' && cur() <= '9'))
            advance();
    }
    uint8_t suffix = cur();
    if (suffix == '$') { advance(); return VT_STR; }
    if (suffix == '%') { advance(); return VT_INT; }
    if (suffix == '!') { advance(); return VT_SNG; }
    if (suffix == '#') { advance(); return VT_DBL; }
    int idx = name_out[0] - 'A';
    return (idx >= 0 && idx < 26) ? ana->def_type[idx] : VT_SNG;
}

/* ---- Expression compilation ---- */

/* Forward declarations */
static void emit_str_expr(void);
static void emit_num_expr(void);

static int op_prec(uint8_t tok)
{
    switch (tok) {
    case TOK_IMP:   return 40;
    case TOK_EQV:   return 42;
    case TOK_XOR:   return 44;
    case TOK_OR:    return 46;
    case TOK_AND:   return 48;
    case TOK_NOT:   return 50;
    case TOK_GT: case TOK_EQ: case TOK_LT: return 64;
    case TOK_PLUS: case TOK_MINUS: return 121;
    case TOK_MOD:   return 122;
    case TOK_IDIV:  return 123;
    case TOK_MUL: case TOK_DIV: return 124;
    case TOK_POW:   return 127;
    default:        return -1;
    }
}

static const char *binop_c(uint8_t tok)
{
    switch (tok) {
    case TOK_PLUS:  return "+";
    case TOK_MINUS: return "-";
    case TOK_MUL:   return "*";
    case TOK_DIV:   return "/";
    case TOK_GT:    return ">";
    case TOK_LT:    return "<";
    case TOK_EQ:    return "==";
    case TOK_AND:   return "&";
    case TOK_OR:    return "|";
    case TOK_XOR:   return "^";
    case TOK_MOD:   return "%";
    case TOK_IDIV:  return "/";
    default:        return "?";
    }
}

/* Emit a numeric atom */
static void emit_atom(void)
{
    skip_spaces();
    uint8_t tok = cur();

    /* Numeric constant */
    if (tok >= 0x11 && tok <= 0x1A) {
        EMIT("%d", tok - 0x11);
        tp++;
        return;
    }
    if (tok == TOK_INT1) {
        EMIT("%d", tp[1]);
        tp += 2;
        return;
    }
    if (tok == TOK_INT2) {
        int16_t v = (int16_t)(tp[1] | (tp[2] << 8));
        EMIT("%d", v);
        tp += 3;
        return;
    }
    if (tok == TOK_CONST_SNG) {
        float v;
        memcpy(&v, tp + 1, 4);
        tp += 5;
        EMIT("%.9gf", v);
        return;
    }
    if (tok == TOK_CONST_DBL) {
        double v;
        memcpy(&v, tp + 1, 8);
        tp += 9;
        EMIT("%.17g", v);
        return;
    }

    /* Parenthesized expression */
    if (tok == '(') {
        EMIT("(");
        advance();
        emit_num_expr();
        if (cur() == ')') advance();
        EMIT(")");
        return;
    }

    /* Unary minus */
    if (tok == TOK_MINUS) {
        EMIT("(-");
        advance();
        emit_atom();
        EMIT(")");
        return;
    }

    /* NOT */
    if (tok == TOK_NOT) {
        EMIT("(~(int16_t)(");
        advance();
        emit_num_expr();
        EMIT("))");
        return;
    }

    /* Built-in functions (0xFF prefix) */
    if (tok == TOK_PREFIX_FF) {
        uint8_t func = tp[1];
        tp += 2;
        skip_spaces();
        switch (func) {
        case FUNC_ABS:
        case FUNC_INT:
        case FUNC_SQR:
        case FUNC_SIN:
        case FUNC_COS:
        case FUNC_TAN:
        case FUNC_ATN:
        case FUNC_LOG:
        case FUNC_EXP: {
            const char *cfn[] = {
                [FUNC_ABS] = "fabs", [FUNC_INT] = "floor", [FUNC_SQR] = "sqrt",
                [FUNC_SIN] = "sin", [FUNC_COS] = "cos", [FUNC_TAN] = "tan",
                [FUNC_ATN] = "atan", [FUNC_LOG] = "log", [FUNC_EXP] = "exp",
            };
            EMIT("%s(", cfn[func]);
            advance();
            emit_num_expr();
            if (cur() == ')') advance();
            EMIT(")");
            return;
        }
        case FUNC_SGN: {
            EMIT("({ double _v = (");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT("); (_v > 0) - (_v < 0); })");
            return;
        }
        case FUNC_RND: {
            /* RND may or may not have parens */
            if (cur() == '(') {
                advance(); emit_num_expr();
                if (cur() == ')') advance();
            }
            EMIT("((float)rand() / RAND_MAX)");
            return;
        }
        case FUNC_CINT: {
            EMIT("((int16_t)gw_round(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT("))");
            return;
        }
        case FUNC_CSNG: {
            EMIT("((float)(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT("))");
            return;
        }
        case FUNC_CDBL: {
            EMIT("((double)(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT("))");
            return;
        }
        case FUNC_FIX: {
            EMIT("((int16_t)(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT("))");
            return;
        }
        case FUNC_LEN: {
            EMIT("gw_fn_len(&(gw_value_t){.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ')') advance();
            EMIT("}).ival");
            return;
        }
        case FUNC_ASC: {
            EMIT("gw_fn_asc(&(gw_value_t){.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ')') advance();
            EMIT("}).ival");
            return;
        }
        case FUNC_VAL: {
            EMIT("gw_fn_val(&(gw_value_t){.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ')') advance();
            EMIT("}).dval");
            return;
        }
        case 0xD6 /* INSTR */: {
            /* INSTR([start,] haystack$, needle$) */
            EMIT("gw_fn_instr(1, ");
            advance(); /* ( */
            /* TODO: handle optional start parameter */
            EMIT("&(gw_value_t){.type=VT_STR,.sval=");
            emit_str_expr();
            EMIT("}, ");
            if (cur() == ',') advance();
            EMIT("&(gw_value_t){.type=VT_STR,.sval=");
            emit_str_expr();
            if (cur() == ')') advance();
            EMIT("}).ival");
            return;
        }
        case FUNC_POS:
            EMIT("(gw_hal ? gw_hal->get_cursor_col() + 1 : 1)");
            if (cur() == '(') { advance(); emit_num_expr(); if (cur() == ')') advance(); }
            return;
        case FUNC_FRE:
            EMIT("((float)strpool_free())");
            if (cur() == '(') {
                advance();
                /* consume arg but ignore */
                while (cur() && cur() != ')') tp++;
                if (cur() == ')') advance();
            }
            return;
        case FUNC_PEEK:
            EMIT("virmem_peek(gw.def_seg, ");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT(")");
            return;
        case FUNC_INP:
            EMIT("portio_inp(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT(")");
            return;
        default:
            /* Fallback: emit 0 for unhandled functions */
            EMIT("0 /* unhandled func 0x%02x */", func);
            if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
            return;
        }
    }

    /* Variable or array element */
    if (is_letter(tok)) {
        char name[2];
        gw_valtype_t type = parse_var(name);
        skip_spaces();
        if (cur() == '(') {
            /* Array element access — buffer subscripts to get ndims */
            advance(); /* skip ( */
            char *sub_bufs[8];
            int ndims = 0;
            do {
                if (ndims > 0 && cur() == ',') advance();
                FILE *orig = out;
                char *sbuf = NULL; size_t ssz = 0;
                out = open_memstream(&sbuf, &ssz);
                emit_num_expr();
                fclose(out);
                out = orig;
                sub_bufs[ndims++] = sbuf;
            } while (cur() == ',' && ndims < 8);
            if (cur() == ')') advance();

            EMIT("gwrt_array_elem("); emit_name_str(name);
            EMIT(", %d, %d, (int[]){", type, ndims);
            for (int d = 0; d < ndims; d++) {
                if (d > 0) EMIT(", ");
                EMIT("(int)(%s)", sub_bufs[d]);
                free(sub_bufs[d]);
            }
            EMIT("})->");
            switch (type) {
            case VT_INT: EMIT("ival"); break;
            case VT_SNG: EMIT("fval"); break;
            case VT_DBL: EMIT("dval"); break;
            default: EMIT("fval"); break;
            }
            return;
        }
        if (type == VT_STR) {
            EMIT("0 /* str var in num ctx */");
        } else {
            emit_varname(name, type);
        }
        return;
    }

    /* Fallback */
    EMIT("0 /* unknown tok 0x%02x */", tok);
    tp++;
}

/* Emit numeric expression with precedence climbing.
 *
 * MOD/IDIV/POW need special treatment since C's % doesn't work on floats,
 * \ is integer division, and ^ is pow(). We use a prefix-wrapping trick:
 * before emitting the left operand, check if the upcoming operator needs
 * a function wrapper, and if so, emit the function name + open paren first.
 */
static void emit_num_prec(int min_prec);


/* Buffer helper: emit to a memory buffer, return allocated string */
static char *emit_to_buf(void (*fn)(int), int arg)
{
    FILE *orig = out;
    char *buf = NULL;
    size_t sz = 0;
    out = open_memstream(&buf, &sz);
    fn(arg);
    fclose(out);
    out = orig;
    return buf;
}

static void emit_atom_wrapper(int unused) { (void)unused; emit_atom(); }
static void emit_prec_wrapper(int prec) { emit_num_prec(prec); }

static void emit_num_prec(int min_prec)
{
    /* Buffer left operand so we can wrap it for MOD/IDIV/POW */
    char *left = emit_to_buf(emit_atom_wrapper, 0);

    for (;;) {
        skip_spaces();
        int prec = op_prec(cur());
        if (prec < min_prec) break;
        uint8_t op = cur();
        advance();

        /* Handle combined relationals: <=, >=, <> */
        const char *cop = NULL;
        if (op == TOK_LT && cur() == TOK_EQ) { advance(); cop = "<="; }
        else if (op == TOK_GT && cur() == TOK_EQ) { advance(); cop = ">="; }
        else if (op == TOK_LT && cur() == TOK_GT) { advance(); cop = "!="; }
        else if (op == TOK_GT && cur() == TOK_LT) { advance(); cop = "!="; }
        else if (op == TOK_EQ && cur() == TOK_LT) { advance(); cop = "<="; }
        else if (op == TOK_EQ && cur() == TOK_GT) { advance(); cop = ">="; }

        char *right = emit_to_buf(emit_prec_wrapper, prec + 1);

        char *combined = NULL;
        size_t csz = 0;
        FILE *cm = open_memstream(&combined, &csz);

        if (cop) {
            fprintf(cm, "((%s %s %s) ? -1 : 0)", left, cop, right);
        } else if (op == TOK_GT || op == TOK_LT || op == TOK_EQ) {
            fprintf(cm, "((%s %s %s) ? -1 : 0)", left, binop_c(op), right);
        } else if (op == TOK_MOD) {
            fprintf(cm, "((int16_t)(%s) %% (int16_t)(%s))", left, right);
        } else if (op == TOK_IDIV) {
            fprintf(cm, "((int16_t)(%s) / (int16_t)(%s))", left, right);
        } else if (op == TOK_POW) {
            fprintf(cm, "pow((double)(%s), (double)(%s))", left, right);
        } else {
            fprintf(cm, "(%s %s %s)", left, binop_c(op), right);
        }
        fclose(cm);

        free(left);
        free(right);
        left = combined;
    }

    EMIT("%s", left);
    free(left);
}


static void emit_num_expr(void)
{
    emit_num_prec(0);
}

/* Emit a string expression */
static void emit_str_expr(void)
{
    skip_spaces();
    uint8_t tok = cur();

    /* String literal */
    if (tok == '"') {
        EMIT("gw_str_from_cstr(\"");
        tp++;
        while (*tp && *tp != '"') {
            if (*tp == '\\' || *tp == '"') EMIT("\\");
            EMIT("%c", *tp);
            tp++;
        }
        if (*tp == '"') tp++;
        EMIT("\")");
        /* Handle concatenation */
        skip_spaces();
        while (cur() == TOK_PLUS) {
            advance();
            EMIT("; _cat = gw_str_concat(&(gw_value_t){.type=VT_STR,.sval=_cat.sval}, &(gw_value_t){.type=VT_STR,.sval=");
            emit_str_expr();
            EMIT("})");
        }
        return;
    }

    /* String functions (0xFF prefix) */
    if (tok == TOK_PREFIX_FF) {
        uint8_t func = tp[1];
        tp += 2;
        skip_spaces();
        switch (func) {
        case FUNC_CHR:
            EMIT("gw_fn_chr(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        case FUNC_STR:
            EMIT("gw_fn_str(&(gw_value_t){.type=VT_DBL,.dval=(double)(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT(")}).sval");
            return;
        case FUNC_LEFT: {
            EMIT("gw_fn_left(&(gw_value_t){.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ',') advance();
            EMIT("}, ");
            emit_num_expr();
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        }
        case FUNC_RIGHT: {
            EMIT("gw_fn_right(&(gw_value_t){.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ',') advance();
            EMIT("}, ");
            emit_num_expr();
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        }
        case FUNC_MID: {
            EMIT("gw_fn_mid(&(gw_value_t){.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ',') advance();
            EMIT("}, ");
            emit_num_expr();
            EMIT(", 255");
            if (cur() == ',') { advance(); EMIT("); /* mid with len */ "); }
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        }
        case FUNC_SPACE:
            EMIT("gw_fn_space(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        case 0xD4 /* STRING$ */:
            EMIT("gw_fn_strings(");
            advance(); emit_num_expr();
            if (cur() == ',') advance();
            EMIT(", ");
            emit_num_expr();
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        case FUNC_HEX:
            EMIT("gw_fn_hex(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        case FUNC_OCT:
            EMIT("gw_fn_oct(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT(").sval");
            return;
        default:
            EMIT("gw_str_from_cstr(\"\") /* unhandled str func */");
            return;
        }
    }

    /* String variable or array element */
    if (is_letter(tok)) {
        char name[2];
        gw_valtype_t type = parse_var(name);
        skip_spaces();
        if (cur() == '(' && type == VT_STR) {
            /* String array element */
            advance();
            char *sub_bufs[8]; int ndims = 0;
            do {
                if (ndims > 0 && cur() == ',') advance();
                FILE *orig = out; char *sb = NULL; size_t ss = 0;
                out = open_memstream(&sb, &ss);
                emit_num_expr();
                fclose(out); out = orig;
                sub_bufs[ndims++] = sb;
            } while (cur() == ',' && ndims < 8);
            if (cur() == ')') advance();
            EMIT("gw_str_copy(&gwrt_array_elem("); emit_name_str(name);
            EMIT(", %d, %d, (int[]){", type, ndims);
            for (int d = 0; d < ndims; d++) {
                if (d > 0) EMIT(", ");
                EMIT("(int)(%s)", sub_bufs[d]);
                free(sub_bufs[d]);
            }
            EMIT("})->sval)");
            return;
        }
        if (type == VT_STR) {
            EMIT("gw_str_copy(&");
            emit_varname(name, type);
            EMIT(")");
        } else {
            EMIT("gw_str_from_cstr(\"\") /* num var in str ctx */");
        }
        return;
    }

    EMIT("gw_str_from_cstr(\"\") /* unknown str expr */");
}


/* ---- Statement compilation ---- */

static void emit_stmt(void);

/* Peek at the next expression to guess its result type.
 * Only returns VT_INT for pure integer atoms (no operators).
 * For anything involving operators, returns the variable/constant type. */
static gw_valtype_t peek_expr_type(void)
{
    uint8_t *save = tp;
    skip_spaces();
    uint8_t tok = cur();

    /* Variable — check suffix (most important case) */
    if (is_letter(tok)) {
        char name[2];
        gw_valtype_t type = parse_var(name);
        tp = save;
        return type;
    }
    /* Double constant */
    if (tok == TOK_CONST_DBL) { tp = save; return VT_DBL; }
    /* Single constant */
    if (tok == TOK_CONST_SNG) { tp = save; return VT_SNG; }
    /* Unary minus — peek past it */
    if (tok == TOK_MINUS) { advance(); gw_valtype_t t = peek_expr_type(); tp = save; return t; }
    /* Functions */
    if (tok == TOK_PREFIX_FF) {
        uint8_t func = tp[1];
        tp = save;
        switch (func) {
        case FUNC_LEN: case FUNC_ASC: case FUNC_CINT: case FUNC_FIX:
        case FUNC_POS: case FUNC_PEEK: case FUNC_INP:
            return VT_INT;
        case FUNC_VAL: case FUNC_ATN: case FUNC_LOG: case FUNC_EXP:
        case FUNC_CDBL:
            return VT_DBL;
        default:
            return VT_SNG;
        }
    }
    /* Integer constants — only if truly standalone (PRINT 42 vs PRINT 4*X) */
    if ((tok >= 0x11 && tok <= 0x1A) || tok == TOK_INT1 || tok == TOK_INT2)
        { tp = save; return VT_SNG; }  /* default to SNG for safety */
    tp = save;
    return VT_SNG;
}

static void emit_print(void)
{
    skip_spaces();
    bool need_newline = true;

    while (cur() && cur() != ':' && cur() != TOK_ELSE) {
        skip_spaces();

        if (cur() == ';') {
            need_newline = false;
            advance();
            continue;
        }
        if (cur() == ',') {
            EMIT("  gwrt_print_tab();\n");
            need_newline = false;
            advance();
            continue;
        }

        /* Detect string vs numeric expression */
        /* String: starts with " or a string variable or string function */
        uint8_t tok = cur();
        bool is_str = (tok == '"');
        if (is_letter(tok)) {
            /* Peek ahead for $ suffix */
            uint8_t *save = tp;
            char name[2];
            gw_valtype_t type = parse_var(name);
            tp = save;  /* restore */
            is_str = (type == VT_STR);
        }
        if (tok == TOK_PREFIX_FF) {
            uint8_t func = tp[1];
            is_str = (func == FUNC_CHR || func == FUNC_STR || func == FUNC_LEFT
                   || func == FUNC_RIGHT || func == FUNC_MID || func == FUNC_SPACE
                   || func == 0xD4 /* STRING$ */ || func == FUNC_HEX || func == FUNC_OCT);
        }

        if (is_str) {
            EMIT("  { gw_string_t _s = ");
            emit_str_expr();
            EMIT("; gwrt_print_str(_s); gw_str_free(&_s); }\n");
        } else {
            gw_valtype_t etype = peek_expr_type();
            switch (etype) {
            case VT_INT:
                EMIT("  { gw_value_t _pv = {.type = VT_INT, .ival = (int16_t)(");
                emit_num_expr();
                EMIT(")}; gw_print_value(&_pv); }\n");
                break;
            case VT_DBL:
                EMIT("  { gw_value_t _pv = {.type = VT_DBL, .dval = (double)(");
                emit_num_expr();
                EMIT(")}; gw_print_value(&_pv); }\n");
                break;
            default:
                EMIT("  { gw_value_t _pv = {.type = VT_SNG, .fval = (float)(");
                emit_num_expr();
                EMIT(")}; gw_print_value(&_pv); }\n");
                break;
            }
        }
        need_newline = true;
    }

    if (need_newline)
        EMIT("  gwrt_print_newline();\n");
}

static void emit_assignment(void)
{
    char name[2];
    gw_valtype_t type = parse_var(name);
    skip_spaces();

    /* Array element assignment: A(i) = expr */
    if (cur() == '(') {
        advance(); /* skip ( */
        EMIT("  { int _subs[] = {(int)(");
        emit_num_expr();
        int ndims = 1;
        while (cur() == ',') {
            advance();
            EMIT("), (int)(");
            emit_num_expr();
            ndims++;
        }
        EMIT(")};\n");
        if (cur() == ')') advance();
        skip_spaces();
        if (cur() == TOK_EQ) advance();

        /* Call runtime to get element pointer */
        EMIT("    gw_value_t *_elem = gwrt_array_elem(");
        emit_name_str(name);
        EMIT(", %d, %d, _subs);\n", type, ndims);

        if (type == VT_STR) {
            EMIT("    gw_str_free(&_elem->sval);\n");
            EMIT("    _elem->sval = ");
            emit_str_expr();
            EMIT(";\n    _elem->type = VT_STR;\n");
        } else {
            EMIT("    *_elem = (gw_value_t){.type=%d};\n", type);
            switch (type) {
            case VT_INT: EMIT("    _elem->ival = (int16_t)("); break;
            case VT_SNG: EMIT("    _elem->fval = (float)("); break;
            case VT_DBL: EMIT("    _elem->dval = (double)("); break;
            default: EMIT("    _elem->fval = (float)("); break;
            }
            emit_num_expr();
            EMIT(");\n");
        }
        EMIT("  }\n");
        return;
    }

    if (cur() != TOK_EQ) {
        EMIT("  /* expected = */\n");
        return;
    }
    advance();

    if (type == VT_STR) {
        EMIT("  gw_str_free(&");
        emit_varname(name, type);
        EMIT(");\n");
        EMIT("  ");
        emit_varname(name, type);
        EMIT(" = ");
        emit_str_expr();
        EMIT(";\n");
    } else {
        EMIT("  ");
        emit_varname(name, type);
        EMIT(" = (%s)(", c_type(type));
        emit_num_expr();
        EMIT(");\n");
    }
}

/* Compile one statement */
static void emit_stmt(void)
{
    skip_spaces();
    uint8_t tok = cur();

    if (tok == 0 || tok == ':') return;

    /* REM */
    if (tok == TOK_REM || tok == TOK_SQUOTE) {
        while (*tp) tp++;
        return;
    }

    /* PRINT */
    if (tok == TOK_PRINT || tok == '?') {
        advance();
        emit_print();
        return;
    }

    /* GOTO */
    if (tok == TOK_GOTO) {
        advance();
        skip_spaces();
        uint16_t target = read_int();
        EMIT("  goto L_%u;\n", target);
        return;
    }

    /* GOSUB */
    if (tok == TOK_GOSUB) {
        advance();
        skip_spaces();
        uint16_t target = read_int();
        int rl = ret_label_counter++;
        EMIT("  gwrt_gosub_push(%d); goto L_%u;\n", rl, target);
        EMIT("ret_%d: ;\n", rl);
        return;
    }

    /* RETURN */
    if (tok == TOK_RETURN) {
        advance();
        EMIT("  switch(gwrt_gosub_pop()) {\n");
        for (int i = 0; i < ret_label_counter; i++)
            EMIT("    case %d: goto ret_%d;\n", i, i);
        EMIT("  }\n");
        return;
    }

    /* FOR */
    if (tok == TOK_FOR) {
        advance();
        char name[2];
        gw_valtype_t type = parse_var(name);
        skip_spaces();
        if (cur() == TOK_EQ) advance();
        EMIT("  ");
        emit_varname(name, type);
        EMIT(" = (%s)(", c_type(type));
        emit_num_expr();
        EMIT(");\n");
        skip_spaces();
        if (cur() == TOK_TO) advance();
        /* Store limit in a temp */
        EMIT("  { %s _for_limit_%d = (%s)(", c_type(type), for_label_counter, c_type(type));
        emit_num_expr();
        EMIT(");\n");
        /* Step (default 1) */
        EMIT("    %s _for_step_%d = 1;\n", c_type(type), for_label_counter);
        skip_spaces();
        if (cur() == TOK_STEP) {
            advance();
            EMIT("    _for_step_%d = (%s)(", for_label_counter, c_type(type));
            emit_num_expr();
            EMIT(");\n");
        }
        int fc = for_label_counter++;
        EMIT("    for_top_%d:\n", fc);
        EMIT("    if (_for_step_%d >= 0 ? ", fc);
        emit_varname(name, type);
        EMIT(" > _for_limit_%d : ", fc);
        emit_varname(name, type);
        EMIT(" < _for_limit_%d) goto for_done_%d;\n", fc, fc);
        /* Push onto FOR stack */
        if (for_stack_sp < FOR_STACK_MAX) {
            for_stack[for_stack_sp].name[0] = name[0];
            for_stack[for_stack_sp].name[1] = name[1];
            for_stack[for_stack_sp].type = type;
            for_stack[for_stack_sp].label = fc;
            for_stack_sp++;
        }
        return;
    }

    /* NEXT [var] */
    if (tok == TOK_NEXT) {
        advance();
        skip_spaces();
        /* Find matching FOR on the stack */
        int fc = -1;
        if (is_letter(cur())) {
            char name[2];
            gw_valtype_t type = parse_var(name);
            /* Search stack from top for matching variable */
            for (int i = for_stack_sp - 1; i >= 0; i--) {
                if (for_stack[i].name[0] == name[0] &&
                    for_stack[i].name[1] == name[1] &&
                    for_stack[i].type == type) {
                    fc = for_stack[i].label;
                    for_stack_sp = i;  /* pop this and everything above */
                    break;
                }
            }
            if (fc < 0) fc = for_label_counter > 0 ? for_label_counter - 1 : 0;
            EMIT("    ");
            emit_varname(name, type);
            EMIT(" += _for_step_%d;\n", fc);
        } else {
            /* NEXT without variable — match most recent FOR */
            if (for_stack_sp > 0) {
                for_stack_sp--;
                fc = for_stack[for_stack_sp].label;
            } else {
                fc = for_label_counter > 0 ? for_label_counter - 1 : 0;
            }
        }
        EMIT("    goto for_top_%d;\n", fc);
        EMIT("    for_done_%d: ;\n", fc);
        EMIT("  }\n");
        return;
    }

    /* WHILE */
    if (tok == TOK_WHILE) {
        int wl = for_label_counter++;
        EMIT("  while_top_%d:\n", wl);
        advance();
        EMIT("  if (!(");
        emit_num_expr();
        EMIT(")) goto while_done_%d;\n", wl);
        /* Push on FOR stack (reuse for WEND matching) */
        if (for_stack_sp < FOR_STACK_MAX) {
            for_stack[for_stack_sp].name[0] = 'W';
            for_stack[for_stack_sp].name[1] = 0;
            for_stack[for_stack_sp].type = VT_INT;
            for_stack[for_stack_sp].label = wl;
            for_stack_sp++;
        }
        return;
    }

    /* WEND */
    if (tok == TOK_WEND) {
        advance();
        int wl = 0;
        if (for_stack_sp > 0) {
            for_stack_sp--;
            wl = for_stack[for_stack_sp].label;
        }
        EMIT("  goto while_top_%d;\n", wl);
        EMIT("  while_done_%d: ;\n", wl);
        return;
    }

    /* ON n GOTO / ON n GOSUB */
    if (tok == TOK_ON) {
        advance();
        skip_spaces();
        /* Check for ON ERROR GOTO */
        if (cur() == TOK_ERROR) {
            advance(); skip_spaces();
            if (cur() == TOK_GOTO) {
                advance(); skip_spaces();
                uint16_t target = read_int();
                EMIT("  gwrt_error_target = %u; /* ON ERROR GOTO */\n", target);
            }
            return;
        }
        EMIT("  { int _on_val = (int)(");
        emit_num_expr();
        EMIT(");\n");
        skip_spaces();
        if (cur() == TOK_GOTO) {
            advance(); skip_spaces();
            int n = 1;
            EMIT("    switch(_on_val) {\n");
            while (is_const(cur())) {
                uint16_t target = read_int();
                EMIT("      case %d: goto L_%u;\n", n++, target);
                skip_spaces();
                if (cur() == ',') { advance(); skip_spaces(); }
                else break;
            }
            EMIT("    }\n  }\n");
        } else if (cur() == TOK_GOSUB) {
            advance(); skip_spaces();
            int n = 1;
            int rl = ret_label_counter;
            EMIT("    switch(_on_val) {\n");
            while (is_const(cur())) {
                uint16_t target = read_int();
                EMIT("      case %d: gwrt_gosub_push(%d); goto L_%u;\n", n++, rl, target);
                skip_spaces();
                if (cur() == ',') { advance(); skip_spaces(); }
                else break;
            }
            EMIT("    }\n  }\n");
            EMIT("  ret_%d: ;\n", rl);
            ret_label_counter++;
        }
        return;
    }

    /* DIM */
    if (tok == TOK_DIM) {
        advance();
        while (cur() && cur() != ':' && cur() != 0) {
            skip_spaces();
            if (!is_letter(cur())) break;
            char name[2];
            gw_valtype_t type = parse_var(name);
            skip_spaces();
            if (cur() != '(') break;
            advance();
            /* Buffer dimensions */
            char *dim_bufs[8]; int ndims = 0;
            do {
                if (ndims > 0 && cur() == ',') advance();
                FILE *orig = out; char *db = NULL; size_t ds = 0;
                out = open_memstream(&db, &ds);
                emit_num_expr();
                fclose(out); out = orig;
                dim_bufs[ndims++] = db;
            } while (cur() == ',' && ndims < 8);
            if (cur() == ')') advance();
            EMIT("  gwrt_dim("); emit_name_str(name);
            EMIT(", %d, %d, (int[]){", type, ndims);
            for (int d = 0; d < ndims; d++) {
                if (d > 0) EMIT(", ");
                EMIT("(int)(%s)", dim_bufs[d]);
                free(dim_bufs[d]);
            }
            EMIT("});\n");
            skip_spaces();
            if (cur() == ',') advance();
            else break;
        }
        return;
    }

    /* SWAP */
    if (tok == TOK_SWAP) {
        advance();
        char n1[2]; gw_valtype_t t1 = parse_var(n1);
        skip_spaces();
        if (cur() == ',') advance();
        char n2[2]; gw_valtype_t t2 = parse_var(n2);
        EMIT("  { %s _swap = ", c_type(t1));
        emit_varname(n1, t1);
        EMIT("; ");
        emit_varname(n1, t1);
        EMIT(" = ");
        emit_varname(n2, t2);
        EMIT("; ");
        emit_varname(n2, t2);
        EMIT(" = _swap; }\n");
        return;
    }

    /* POKE */
    if (tok == TOK_POKE) {
        advance();
        EMIT("  virmem_poke(gw.def_seg, (uint16_t)(");
        emit_num_expr();
        EMIT("), (uint8_t)(");
        skip_spaces();
        if (cur() == ',') advance();
        emit_num_expr();
        EMIT("));\n");
        return;
    }

    /* DEF SEG / DEF FN */
    if (tok == TOK_DEF) {
        advance();
        skip_spaces();
        /* DEF SEG = expr  or  DEF FN */
        if (toupper(cur()) == 'S') {
            /* Skip "SEG" */
            while (is_letter(cur())) advance();
            skip_spaces();
            if (cur() == TOK_EQ) {
                advance();
                EMIT("  gw.def_seg = (uint16_t)(");
                emit_num_expr();
                EMIT(");\n");
            } else {
                EMIT("  gw.def_seg = 0;\n");
            }
        } else {
            /* DEF FN — skip for now, handled by analysis */
            while (cur() && cur() != ':' && cur() != 0) tp++;
        }
        return;
    }

    /* RANDOMIZE */
    if (tok == TOK_RANDOMIZE) {
        advance();
        skip_spaces();
        if (cur() && cur() != ':' && cur() != 0) {
            EMIT("  srand((unsigned)(");
            emit_num_expr();
            EMIT("));\n");
        } else {
            EMIT("  srand((unsigned)time(NULL));\n");
        }
        return;
    }

    /* COLOR */
    if (tok == TOK_COLOR) {
        advance();
        EMIT("  /* COLOR */\n");
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }

    /* LOCATE */
    if (tok == TOK_LOCATE) {
        advance();
        EMIT("  { int _row = (int)(");
        emit_num_expr();
        EMIT("); int _col = 1;\n");
        skip_spaces();
        if (cur() == ',') {
            advance();
            EMIT("  _col = (int)(");
            emit_num_expr();
            EMIT(");\n");
        }
        EMIT("  if (gw_hal) gw_hal->locate(_row - 1, _col - 1); }\n");
        return;
    }

    /* WIDTH */
    if (tok == TOK_WIDTH) {
        advance();
        EMIT("  /* WIDTH */\n");
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }

    /* SCREEN */
    if (tok == TOK_SCREEN) {
        advance();
        EMIT("  { int _mode = (int)(");
        emit_num_expr();
        EMIT("); if (_mode) gfx_init(_mode); else gfx_shutdown(); }\n");
        while (cur() == ',') { advance(); emit_num_expr(); }
        return;
    }

    /* OPEN / CLOSE — file I/O (Phase 3: needs inline argument parsing) */
    if (tok == TOK_OPEN || tok == TOK_CLOSE) {
        EMIT("  /* %s — file I/O not yet compiled */\n", tok == TOK_OPEN ? "OPEN" : "CLOSE");
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }
    /* INPUT */
    if (tok == TOK_INPUT) {
        advance();
        EMIT("  /* INPUT — not yet compiled */\n");
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }

    /* TRON/TROFF */
    if (tok == TOK_TRON) { advance(); return; }
    if (tok == TOK_TROFF) { advance(); return; }

    /* OPTION BASE */
    if (tok == TOK_OPTION) {
        advance(); /* skip OPTION */
        skip_spaces();
        /* expect BASE */
        advance(); /* skip BASE */
        skip_spaces();
        EMIT("  gw.option_base = (int)(");
        emit_num_expr();
        EMIT(");\n");
        return;
    }

    /* DEFINT/DEFSNG/DEFDBL/DEFSTR — compile-time only, skip */
    if (tok == TOK_DEFINT || tok == TOK_DEFSNG ||
        tok == TOK_DEFDBL || tok == TOK_DEFSTR) {
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }

    /* KEY */
    if (tok == TOK_KEY) {
        advance();
        EMIT("  /* KEY */\n");
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }

    /* WRITE (output) */
    if (tok == TOK_WRITE) {
        advance();
        skip_spaces();
        /* Simple WRITE: comma-separated values with quotes around strings */
        bool first = true;
        while (cur() && cur() != ':' && cur() != 0) {
            if (!first) EMIT("  gwrt_print_cstr(\",\");\n");
            first = false;
            skip_spaces();
            if (cur() == '"' || (is_letter(cur()) && ({
                uint8_t *s = tp; char n[2]; gw_valtype_t t = parse_var(n);
                tp = s; t == VT_STR; }))) {
                EMIT("  gwrt_print_cstr(\"\\\"\");\n");
                EMIT("  { gw_string_t _s = ");
                emit_str_expr();
                EMIT("; gwrt_print_str(_s); gw_str_free(&_s); }\n");
                EMIT("  gwrt_print_cstr(\"\\\"\");\n");
            } else {
                EMIT("  { gw_value_t _pv = {.type = VT_SNG, .fval = (float)(");
                emit_num_expr();
                EMIT(")}; gw_print_value(&_pv); }\n");
            }
            skip_spaces();
            if (cur() == ',') advance();
            else break;
        }
        EMIT("  gwrt_print_newline();\n");
        return;
    }

    /* IF / THEN / ELSE */
    if (tok == TOK_IF) {
        advance();
        EMIT("  if ((");
        emit_num_expr();
        EMIT(") != 0) {\n");
        skip_spaces();
        if (cur() == TOK_THEN) advance();
        skip_spaces();
        /* THEN followed by line number? */
        if (is_const(cur())) {
            uint16_t target = read_int();
            EMIT("    goto L_%u;\n", target);
        } else {
            /* THEN followed by statements */
            while (cur() && cur() != TOK_ELSE && cur() != 0) {
                if (cur() == ':') { advance(); continue; }
                emit_stmt();
            }
        }
        if (cur() == TOK_ELSE) {
            advance();
            EMIT("  } else {\n");
            skip_spaces();
            if (is_const(cur())) {
                uint16_t target = read_int();
                EMIT("    goto L_%u;\n", target);
            } else {
                while (cur() && cur() != 0) {
                    if (cur() == ':') { advance(); continue; }
                    emit_stmt();
                }
            }
        }
        EMIT("  }\n");
        return;
    }

    /* END */
    if (tok == TOK_END) {
        advance();
        EMIT("  gwrt_shutdown(); return 0;\n");
        return;
    }

    /* STOP */
    if (tok == TOK_STOP) {
        advance();
        EMIT("  gwrt_shutdown(); return 0;\n");
        return;
    }

    /* Extended statements (0xFE prefix) */
    if (tok == TOK_PREFIX_FE) {
        uint8_t xstmt = tp[1];
        tp += 2;
        skip_spaces();

        if (xstmt == XSTMT_SYSTEM) {
            EMIT("  gwrt_shutdown(); exit(0);\n");
            return;
        }
        if (xstmt == XSTMT_FILES || xstmt == XSTMT_SHELL ||
            xstmt == XSTMT_CHDIR || xstmt == XSTMT_MKDIR ||
            xstmt == XSTMT_RMDIR || xstmt == XSTMT_KILL ||
            xstmt == XSTMT_NAME) {
            /* These need a string argument — emit runtime call */
            EMIT("  /* xstmt 0x%02x — runtime call needed */\n", xstmt);
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }
        if (xstmt == XSTMT_TIMER) {
            /* TIMER ON/OFF/STOP */
            EMIT("  /* TIMER trap */\n");
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }
        if (xstmt == XSTMT_CIRCLE || xstmt == XSTMT_DRAW ||
            xstmt == XSTMT_PAINT || xstmt == XSTMT_PLAY) {
            /* Graphics/sound — skip args for now */
            EMIT("  /* graphics/sound xstmt 0x%02x */\n", xstmt);
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }
        if (xstmt == XSTMT_VIEW || xstmt == XSTMT_WINDOW ||
            xstmt == XSTMT_PALETTE) {
            EMIT("  /* view/window/palette */\n");
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }
        if (xstmt == XSTMT_FIELD || xstmt == XSTMT_LSET ||
            xstmt == XSTMT_RSET || xstmt == XSTMT_PUT ||
            xstmt == XSTMT_GET) {
            EMIT("  /* file I/O xstmt */\n");
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }
        if (xstmt == XSTMT_COMMON || xstmt == XSTMT_CHAIN) {
            EMIT("  /* CHAIN/COMMON — not supported in compiled mode */\n");
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }
        if (xstmt == XSTMT_ENVIRON || xstmt == XSTMT_ERDEV ||
            xstmt == XSTMT_IOCTL || xstmt == XSTMT_LCOPY ||
            xstmt == XSTMT_CALLS || xstmt == XSTMT_COM ||
            xstmt == XSTMT_RESET || xstmt == XSTMT_DATE ||
            xstmt == XSTMT_TIME || xstmt == XSTMT_PMAP) {
            /* Minor statements — skip args */
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }

        /* Fallback */
        EMIT("  /* unhandled xstmt 0x%02x */\n", xstmt);
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }

    /* DATA — skip (collected at analysis time) */
    if (tok == TOK_DATA) {
        while (*tp && *tp != ':') tp++;
        return;
    }

    /* READ */
    if (tok == TOK_READ) {
        advance();
        while (cur() && cur() != ':' && cur() != 0) {
            skip_spaces();
            if (is_letter(cur())) {
                char name[2];
                gw_valtype_t type = parse_var(name);
                if (type == VT_STR) {
                    EMIT("  gw_str_free(&");
                    emit_varname(name, type);
                    EMIT(");\n");
                    EMIT("  ");
                    emit_varname(name, type);
                    EMIT(" = gw_str_from_cstr(gwrt_data_read());\n");
                } else {
                    EMIT("  ");
                    emit_varname(name, type);
                    EMIT(" = (%s)atof(gwrt_data_read());\n", c_type(type));
                }
            }
            skip_spaces();
            if (cur() == ',') advance();
            else break;
        }
        return;
    }

    /* RESTORE */
    if (tok == TOK_RESTORE) {
        advance();
        skip_spaces();
        if (is_const(cur())) {
            uint16_t line = read_int();
            /* Find data index for this line */
            EMIT("  gwrt_data_restore(data_line_%u);\n", line);
        } else {
            EMIT("  gwrt_data_restore(0);\n");
        }
        return;
    }

    /* LET (explicit) */
    if (tok == TOK_LET) {
        advance();
        emit_assignment();
        return;
    }

    /* CLS */
    if (tok == TOK_CLS) {
        advance();
        EMIT("  if (gw_hal) gw_hal->cls();\n");
        return;
    }

    /* Implicit assignment (variable at start of statement) */
    if (is_letter(tok)) {
        emit_assignment();
        return;
    }

    /* Skip unknown tokens */
    EMIT("  /* skip tok 0x%02x */\n", tok);
    tp++;
}

/* ---- Main code generation ---- */

void codegen_emit(FILE *f, analysis_t *a)
{
    out = f;
    ana = a;
    ret_label_counter = 0;
    for_label_counter = 0;
    for_stack_sp = 0;

    /* Header */
    EMIT("/* Generated by gwbasic-compile */\n");
    EMIT("#include \"gwrt.h\"\n");
    EMIT("#include <math.h>\n");
    EMIT("#include <stdlib.h>\n");
    EMIT("#include <string.h>\n");
    EMIT("#include <time.h>\n\n");

    /* Variable declarations */
    for (int i = 0; i < a->var_count; i++) {
        EMIT("static %s ", c_type(a->vars[i].type));
        emit_varname(a->vars[i].name, a->vars[i].type);
        if (a->vars[i].type == VT_STR)
            EMIT(" = {0, NULL}");
        else
            EMIT(" = 0");
        EMIT(";\n");
    }
    EMIT("\n");

    /* DATA pool */
    if (a->data_count > 0) {
        EMIT("static const char *_data_pool[] = {\n");
        for (int i = 0; i < a->data_count; i++) {
            EMIT("  \"");
            for (const char *p = a->data_pool[i]; *p; p++) {
                if (*p == '"' || *p == '\\') EMIT("\\");
                EMIT("%c", *p);
            }
            EMIT("\",\n");
        }
        EMIT("  NULL\n};\n");

        /* Data line map (for RESTORE n) */
        for (int i = 0; i < a->data_line_count; i++)
            EMIT("static const int data_line_%d = %d;\n",
                 a->data_line_map[i][0], a->data_line_map[i][1]);
        EMIT("\n");
    }

    /* Main function */
    EMIT("int main(int argc, char **argv) {\n");
    EMIT("  (void)argc; (void)argv;\n");
    EMIT("  gwrt_init();\n");
    if (a->data_count > 0)
        EMIT("  gwrt_data_set(_data_pool, NULL, 0);\n");
    EMIT("\n");

    /* Emit code for each program line */
    for (program_line_t *line = gw.prog_head; line; line = line->next) {
        /* Label (always emit for targets, comment for others) */
        if (analysis_is_target(a, line->num))
            EMIT("L_%u:\n", line->num);
        else
            EMIT("/* %u */ ", line->num);

        EMIT("  gwrt_check_line(%u);\n", line->num);

        /* Walk statements on this line */
        tp = line->tokens;
        while (*tp) {
            skip_spaces();
            if (*tp == ':') { tp++; continue; }
            if (*tp == 0) break;
            emit_stmt();
        }
    }

    /* Implicit END at bottom */
    EMIT("  gwrt_shutdown();\n");
    EMIT("  return 0;\n");
    EMIT("}\n");
}
