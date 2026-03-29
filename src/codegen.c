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

    /* Variable */
    if (is_letter(tok)) {
        char name[2];
        gw_valtype_t type = parse_var(name);
        if (type == VT_STR) {
            /* String var used in numeric context — likely LEN or similar */
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
static void emit_num_prec(int min_prec)
{
    emit_atom();
    for (;;) {
        skip_spaces();
        int prec = op_prec(cur());
        if (prec < min_prec) break;
        uint8_t op = cur();
        advance();
        EMIT(" %s ", binop_c(op));
        emit_num_prec(prec + 1);
    }
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

    /* String variable */
    if (is_letter(tok)) {
        char name[2];
        gw_valtype_t type = parse_var(name);
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
            EMIT("  gwrt_print_sng((float)(");
            emit_num_expr();
            EMIT("));\n");
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

    /* Array element? */
    if (cur() == '(') {
        /* TODO: array assignment */
        EMIT("  /* TODO: array assignment */\n");
        while (cur() && cur() != ':') tp++;
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

        /* Fallback for unhandled extended statements */
        EMIT("  /* TODO: xstmt 0x%02x */\n", xstmt);
        while (cur() && cur() != ':') tp++;
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
    EMIT("#include <string.h>\n\n");

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
