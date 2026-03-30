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

/* Forward declarations */
static char *emit_to_buf(void (*fn)(int), int arg);
static void emit_num_prec(int min_prec);
static void emit_prec_wrapper(int prec);
static void emit_str_atom(void);
static gw_valtype_t peek_expr_type(void);

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
        { char nbuf[32]; snprintf(nbuf, sizeof(nbuf), "%.9g", v);
          if (!strchr(nbuf, '.') && !strchr(nbuf, 'e'))
              EMIT("%s.0f", nbuf);  /* 0 → 0.0f */
          else
              EMIT("%sf", nbuf);    /* 3.14 → 3.14f */
        }
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
            /* RND([n]) — argument is ignored (controls seed behavior) */
            if (cur() == '(') {
                advance();
                /* Consume but discard the argument */
                char *discard = emit_to_buf(emit_prec_wrapper, 0);
                free(discard);
                if (cur() == ')') advance();
            }
            EMIT("((float)gw_rnd(1.0))");
            return;
        }
        case FUNC_CINT: {
            EMIT("((int16_t)gw_cint(");
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
            /* In compiled code, GC can't find C-static variables.
             * Sync vars to interpreter table before GC for FRE(""). */
            if (cur() == '(' && tp[1] == '"') {
                EMIT("({");
                for (int vi = 0; vi < ana->var_count; vi++) {
                    var_info_t *v = &ana->vars[vi];
                    if (v->type != VT_STR) continue;
                    if (v->name[1])
                        EMIT("gw_var_find_or_create(\"%c%c\",%d)->val.sval=", v->name[0], v->name[1], v->type);
                    else
                        EMIT("gw_var_find_or_create(\"%c\",%d)->val.sval=", v->name[0], v->type);
                    emit_varname(v->name, v->type);
                    EMIT(";");
                }
                EMIT(" strpool_gc(); (float)strpool_free();})");
            } else {
                EMIT("((float)strpool_free())");
            }
            if (cur() == '(') {
                advance();
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
        case FUNC_STICK:
            /* Joystick position — return 128 (center) */
            EMIT("128");
            if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
            return;
        case FUNC_STRIG:
            EMIT("0"); /* No joystick button */
            if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
            return;
        case FUNC_PEN:
        case FUNC_LPOS:
            EMIT("0");
            if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
            return;
        case FUNC_EOF: {
            EMIT("gw_file_eof((int)(");
            advance(); emit_num_expr();
            if (cur() == ')') advance();
            EMIT("))");
            return;
        }
        case FUNC_LOC:
        case FUNC_LOF:
            EMIT("0 /* LOC/LOF */");
            if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
            return;
        default:
            EMIT("0 /* unhandled func 0x%02x */", func);
            if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
            return;
        }
    }

    /* Extended functions (0xFD prefix): CVI, CVS, CVD */
    if (tok == TOK_PREFIX_FD) {
        uint8_t xfunc = tp[1];
        tp += 2;
        skip_spaces();
        if (xfunc == XFUNC_CVI) {
            EMIT("gw_fn_cvi(&(gw_value_t){.type=VT_STR,.sval=");
            if (cur() == '(') advance();
            emit_str_atom();
            if (cur() == ')') advance();
            EMIT("}).ival");
        } else if (xfunc == XFUNC_CVS) {
            EMIT("gw_fn_cvs(&(gw_value_t){.type=VT_STR,.sval=");
            if (cur() == '(') advance();
            emit_str_atom();
            if (cur() == ')') advance();
            EMIT("}).fval");
        } else if (xfunc == XFUNC_CVD) {
            EMIT("gw_fn_cvd(&(gw_value_t){.type=VT_STR,.sval=");
            if (cur() == '(') advance();
            emit_str_atom();
            if (cur() == ')') advance();
            EMIT("}).dval");
        } else {
            EMIT("0 /* unhandled xfunc 0x%02x */", xfunc);
            if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
        }
        return;
    }

    /* FN call: embed tokens and use interpreter's gw_eval_fn_call */
    if (tok == TOK_FN) {
        uint8_t *start = tp;
        tp++;  /* skip TOK_FN */
        skip_spaces();
        /* Skip function name + arguments */
        while (is_letter(cur()) || (cur() >= '0' && cur() <= '9')) advance();
        if (cur() == '(') {
            int depth = 1; advance();
            while (*tp && depth > 0) {
                if (cur() == '(') depth++;
                else if (cur() == ')') depth--;
                advance();
            }
        }
        int len = (int)(tp - start);
        /* Sync variables, embed tokens, call evaluator */
        EMIT("({");
        for (int vi = 0; vi < ana->var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            if (v->name[1])
                EMIT(" gw_var_find_or_create(\"%c%c\",%d)->val=(gw_value_t){.type=%d,",
                     v->name[0], v->name[1], v->type, v->type);
            else
                EMIT(" gw_var_find_or_create(\"%c\",%d)->val=(gw_value_t){.type=%d,",
                     v->name[0], v->type, v->type);
            switch (v->type) {
            case VT_INT: EMIT(".ival="); break;
            case VT_SNG: EMIT(".fval="); break;
            case VT_DBL: EMIT(".dval="); break;
            case VT_STR: EMIT(".sval="); break;
            }
            emit_varname(v->name, v->type);
            EMIT("};");
        }
        EMIT(" static const uint8_t _fn[]={");
        for (int i = 0; i < len; i++)
            EMIT("%s%u", i ? "," : "", start[i]);
        EMIT(",0};");
        EMIT(" gw.text_ptr=(uint8_t*)_fn+1;");  /* skip TOK_FN */
        EMIT(" while(*gw.text_ptr==' ')gw.text_ptr++;");  /* skip spaces */
        EMIT(" gw_value_t _fr=gw_eval_fn_call();");
        EMIT(" _fr.type==VT_INT?_fr.ival:_fr.type==VT_DBL?_fr.dval:(double)_fr.fval; })");
        return;
    }

    /* Extended expressions (0xFE prefix) — DATE$, TIME$, TIMER, etc. */
    if (tok == TOK_PREFIX_FE) {
        uint8_t xtok = tp[1];
        tp += 2;
        skip_spaces();
        if (xtok == XSTMT_TIMER) {
            EMIT("((float)(time(NULL) %% 86400))");  /* seconds since midnight */
            return;
        }
        /* PMAP(coord, func) */
        if (xtok == XSTMT_PMAP) {
            if (cur() == '(') advance();
            char *arg1 = emit_to_buf(emit_prec_wrapper, 0);
            if (cur() == ',') advance();
            char *arg2 = emit_to_buf(emit_prec_wrapper, 0);
            if (cur() == ')') advance();
            EMIT("(float)gfx_pmap((double)(%s), (int)(%s))", arg1, arg2);
            free(arg1); free(arg2);
            return;
        }
        /* Other FE tokens used as numeric expressions — emit 0 */
        EMIT("0 /* xfunc 0x%02x */", xtok);
        return;
    }

    /* Single-byte function tokens (not 0xFF-prefixed) */
    if (tok == TOK_INSTR) {
        /* INSTR([start,] haystack$, needle$) */
        tp++;
        EMIT("({");
        if (cur() == '(') advance();
        /* Check if first arg is numeric (start position) */
        uint8_t *save_p = tp;
        bool has_start = false;
        /* Heuristic: if first token after ( is a number, it's the start arg */
        skip_spaces();
        if (is_const(cur()) || (is_letter(cur()) && ({
            uint8_t *sv = tp; char n[2]; gw_valtype_t t = parse_var(n); tp = sv; t != VT_STR; }))) {
            has_start = true;
        }
        tp = save_p;
        skip_spaces();
        if (has_start) {
            char *sbuf = emit_to_buf(emit_prec_wrapper, 0);
            EMIT(" int _start = (int)(%s);", sbuf);
            free(sbuf);
            if (cur() == ',') advance();
        }
        EMIT(" gw_value_t _h = {.type=VT_STR,.sval=");
        emit_str_expr();
        EMIT("}; ");
        if (cur() == ',') advance();
        EMIT("gw_value_t _n = {.type=VT_STR,.sval=");
        emit_str_expr();
        EMIT("}; ");
        if (cur() == ')') advance();
        if (has_start)
            EMIT("gw_fn_instr(_start, &_h, &_n).ival; })");
        else
            EMIT("gw_fn_instr(1, &_h, &_n).ival; })");
        return;
    }
    if (tok == TOK_CSRLIN) {
        tp++;
        EMIT("(gw_hal ? gw_hal->get_cursor_row() + 1 : 1)");
        return;
    }
    if (tok == TOK_ERR) {
        tp++;
        EMIT("gw_errno");
        return;
    }
    if (tok == TOK_ERL) {
        tp++;
        EMIT("gw.err_line_num");
        return;
    }
    if (tok == TOK_POINT) {
        tp++;
        if (cur() == '(') advance();
        char *x = emit_to_buf(emit_prec_wrapper, 0);
        if (cur() == ',') {
            advance();
            char *y = emit_to_buf(emit_prec_wrapper, 0);
            EMIT("gfx_point((int)(%s), (int)(%s))", x, y);
            free(y);
        } else {
            /* POINT(n) — get current drawing position */
            EMIT("0 /* POINT(n) */");
        }
        free(x);
        if (cur() == ')') advance();
        return;
    }
    if (tok == TOK_VARPTR) {
        tp++;
        EMIT("0 /* VARPTR */");
        if (cur() == '(') { advance(); while (cur() && cur() != ')') tp++; if (cur() == ')') advance(); }
        return;
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
    /* Peek at atom type (for string comparison detection) */
    gw_valtype_t left_type = peek_expr_type();
    uint8_t *left_start = tp;  /* save position for string re-emit */
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

        /* For string comparisons, emit strcmp-based code */
        bool is_relational = cop || op == TOK_GT || op == TOK_LT || op == TOK_EQ;
        if (is_relational && left_type == VT_STR) {
            /* Re-emit left as string (it was emitted as numeric atom) */
            uint8_t *save_tp = tp;
            tp = left_start;
            char *left_str;
            { FILE *orig = out; char *buf = NULL; size_t sz = 0;
              out = open_memstream(&buf, &sz);
              emit_str_atom();
              fclose(out); out = orig; left_str = buf; }
            tp = save_tp;
            /* Buffer right as string expression */
            char *right_str;
            { FILE *orig = out; char *buf = NULL; size_t sz = 0;
              out = open_memstream(&buf, &sz);
              emit_str_atom();
              fclose(out); out = orig; right_str = buf; }
            const char *cmpop = cop ? cop : binop_c(op);
            char *combined = NULL; size_t csz = 0;
            FILE *cm = open_memstream(&combined, &csz);
            fprintf(cm, "({gw_string_t _sl=%s; gw_string_t _sr=%s;"
                    " char *_cl=gw_str_to_cstr(&_sl); char *_cr=gw_str_to_cstr(&_sr);"
                    " int _cmp=strcmp(_cl,_cr); free(_cl); free(_cr);"
                    " gw_str_free(&_sl); gw_str_free(&_sr);"
                    " (_cmp %s 0) ? -1 : 0;})",
                    left_str, right_str, cmpop);
            fclose(cm);
            free(left); free(left_str); free(right_str);
            left = combined;
            left_type = VT_INT;
            continue;
        }

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
        } else if (op == TOK_DIV) {
            /* GW-BASIC / always produces float; check for division by zero */
            fprintf(cm, "({double _dv=(double)(%s); if(_dv==0.0) gw_error(11); (double)(%s)/_dv;})",
                    right, left);
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

/* Emit a string atom (no concatenation) */
static void emit_str_atom(void)
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
            EMIT("}, (int)(");
            emit_num_expr();
            EMIT("), ");
            if (cur() == ',') {
                advance();
                EMIT("(int)(");
                emit_num_expr();
                EMIT(")");
            } else {
                EMIT("255");
            }
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

    /* Extended string expressions (0xFE prefix) — DATE$, TIME$, ENVIRON$ */
    if (tok == TOK_PREFIX_FE) {
        uint8_t xtok = tp[1];
        tp += 2;
        skip_spaces();
        if (xtok == XSTMT_DATE) {
            EMIT("({time_t _t=time(NULL); struct tm *_tm=localtime(&_t);"
                 " char _db[16]; snprintf(_db,16,\"%%02d-%%02d-%%04d\","
                 "_tm->tm_mon+1,_tm->tm_mday,_tm->tm_year+1900);"
                 " gw_str_from_cstr(_db);})");
            return;
        }
        if (xtok == XSTMT_TIME) {
            EMIT("({time_t _t=time(NULL); struct tm *_tm=localtime(&_t);"
                 " char _tb[16]; snprintf(_tb,16,\"%%02d:%%02d:%%02d\","
                 "_tm->tm_hour,_tm->tm_min,_tm->tm_sec);"
                 " gw_str_from_cstr(_tb);})");
            return;
        }
        if (xtok == XSTMT_ENVIRON) {
            /* ENVIRON$("name") */
            if (cur() == '$') advance();
            if (cur() == '(') advance();
            EMIT("({gw_string_t _a=");
            emit_str_expr();
            if (cur() == ')') advance();
            EMIT("; char *_n=gw_str_to_cstr(&_a); gw_str_free(&_a);"
                 " const char *_v=getenv(_n);"
                 " gw_string_t _r=gw_str_from_cstr(_v?_v:\"\");"
                 " free(_n); _r;})");
            return;
        }
        if (xtok == XSTMT_ERDEV) {
            if (cur() == '$') advance();
            EMIT("gw_str_from_cstr(\"\")");
            return;
        }
        if (xtok == XSTMT_IOCTL) {
            /* IOCTL$(#filenum) — always returns "" */
            if (cur() == '$') advance();
            if (cur() == '(') {
                advance();
                if (cur() == '#') advance();
                while (cur() && cur() != ')') tp++;
                if (cur() == ')') advance();
            }
            EMIT("gw_str_from_cstr(\"\")");
            return;
        }
        EMIT("gw_str_from_cstr(\"\") /* unhandled xstr 0x%02x */", xtok);
        return;
    }

    /* Extended string functions (0xFD prefix): MKI$, MKS$, MKD$ */
    if (tok == TOK_PREFIX_FD) {
        uint8_t xfunc = tp[1];
        tp += 2;
        skip_spaces();
        if (xfunc == XFUNC_MKI) {
            EMIT("gw_fn_mki((int16_t)(");
            if (cur() == '(') advance();
            emit_num_expr();
            if (cur() == ')') advance();
            EMIT(")).sval");
        } else if (xfunc == XFUNC_MKS) {
            EMIT("gw_fn_mks((float)(");
            if (cur() == '(') advance();
            emit_num_expr();
            if (cur() == ')') advance();
            EMIT(")).sval");
        } else if (xfunc == XFUNC_MKD) {
            EMIT("gw_fn_mkd((double)(");
            if (cur() == '(') advance();
            emit_num_expr();
            if (cur() == ')') advance();
            EMIT(")).sval");
        } else {
            EMIT("gw_str_from_cstr(\"\") /* unhandled xstr func 0x%02x */", xfunc);
        }
        return;
    }

    /* STRING$ (single-byte token 0xD4) */
    if (tok == TOK_STRINGS) {
        tp++;
        if (cur() == '(') advance();
        char *arg1 = emit_to_buf(emit_prec_wrapper, 0);
        if (cur() == ',') advance();
        skip_spaces();
        if (cur() == '"') {
            /* STRING$(n, "c") — use ASCII value of first char */
            tp++;  /* skip opening " */
            int ch = *tp ? *tp : 32;
            while (*tp && *tp != '"') tp++;
            if (*tp == '"') tp++;
            EMIT("gw_fn_strings((int)(%s), %d).sval", arg1, ch);
        } else {
            char *arg2 = emit_to_buf(emit_prec_wrapper, 0);
            EMIT("gw_fn_strings((int)(%s), (int)(%s)).sval", arg1, arg2);
            free(arg2);
        }
        free(arg1);
        if (cur() == ')') advance();
        return;
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

/* Emit a string expression with concatenation (A$ + B$ + C$) */
static void emit_str_expr(void)
{
    char *left;
    { FILE *orig = out; char *buf = NULL; size_t sz = 0;
      out = open_memstream(&buf, &sz);
      emit_str_atom();
      fclose(out); out = orig; left = buf; }

    while (1) {
        skip_spaces();
        if (cur() != TOK_PLUS) break;
        advance();
        char *right;
        { FILE *orig = out; char *buf = NULL; size_t sz = 0;
          out = open_memstream(&buf, &sz);
          emit_str_atom();
          fclose(out); out = orig; right = buf; }
        /* Combine: gw_str_concat returns gw_value_t, we want .sval */
        char *combined = NULL;
        size_t csz = 0;
        FILE *cm = open_memstream(&combined, &csz);
        fprintf(cm, "gw_str_concat("
                "&(gw_value_t){.type=VT_STR,.sval=%s},"
                "&(gw_value_t){.type=VT_STR,.sval=%s}).sval",
                left, right);
        fclose(cm);
        free(left);
        free(right);
        left = combined;
    }

    EMIT("%s", left);
    free(left);
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
    /* Extended functions (FD prefix) */
    if (tok == TOK_PREFIX_FD) {
        uint8_t xf = tp[1];
        tp = save;
        if (xf == XFUNC_CVI) return VT_INT;
        if (xf == XFUNC_CVS) return VT_SNG;
        if (xf == XFUNC_CVD) return VT_DBL;
        return VT_SNG;
    }
    /* Integer constants — only if truly standalone (PRINT 42 vs PRINT 4*X) */
    if ((tok >= 0x11 && tok <= 0x1A) || tok == TOK_INT1 || tok == TOK_INT2)
        { tp = save; return VT_SNG; }  /* default to SNG for safety */
    tp = save;
    return VT_SNG;
}

/*
 * Delegate a statement to the runtime interpreter via token embedding.
 * Syncs all compiled variables to the interpreter table, embeds the
 * raw token bytes, calls gw_exec_stmt(), then reads back variables
 * that may have been modified (strings, and optionally all).
 *
 * stmt_start: pointer to first token byte (before advancing)
 * read_back: if true, read all variables back from interpreter table
 */
static void emit_delegate_stmt(uint8_t *stmt_start, bool read_back)
{
    /* Find end of statement, respecting strings and constants */
    program_line_t *cur_pl = NULL;
    for (program_line_t *pl = gw.prog_head; pl; pl = pl->next) {
        if (tp >= pl->tokens && tp <= pl->tokens + pl->len) {
            cur_pl = pl; break;
        }
    }
    uint8_t *line_end = cur_pl ? cur_pl->tokens + cur_pl->len : tp + 256;
    while (tp < line_end) {
        if (*tp == '"') { tp++; while (tp < line_end && *tp != '"') tp++; if (tp < line_end) tp++; continue; }
        if (*tp == TOK_CONST_SNG) { tp += 5; continue; }
        if (*tp == TOK_CONST_DBL) { tp += 9; continue; }
        if (*tp == TOK_INT2) { tp += 3; continue; }
        if (*tp == TOK_INT1) { tp += 2; continue; }
        if (*tp == ':' || *tp == 0) break;
        tp++;
    }
    int slen = (int)(tp - stmt_start);

    EMIT("  {\n");
    /* Sync all variables to interpreter table */
    for (int vi = 0; vi < ana->var_count; vi++) {
        var_info_t *v = &ana->vars[vi];
        if (v->name[1])
            EMIT("    gw_var_find_or_create(\"%c%c\", %d)->val = ", v->name[0], v->name[1], v->type);
        else
            EMIT("    gw_var_find_or_create(\"%c\", %d)->val = ", v->name[0], v->type);
        EMIT("(gw_value_t){.type=%d,", v->type);
        switch (v->type) {
        case VT_INT: EMIT(".ival="); break;
        case VT_SNG: EMIT(".fval="); break;
        case VT_DBL: EMIT(".dval="); break;
        case VT_STR: EMIT(".sval="); break;
        }
        emit_varname(v->name, v->type);
        EMIT("};\n");
    }
    /* Embed token bytes */
    EMIT("    static const uint8_t _ds[] = {");
    for (int i = 0; i < slen; i++)
        EMIT("%s%u", i ? "," : "", stmt_start[i]);
    EMIT(",0};\n");
    EMIT("    gw.text_ptr = (uint8_t *)_ds;\n");
    EMIT("    gw_exec_stmt();\n");

    /* Read back variables from interpreter table */
    if (read_back) {
        for (int vi = 0; vi < ana->var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            EMIT("    ");
            emit_varname(v->name, v->type);
            if (v->name[1])
                EMIT(" = gw_var_find_or_create(\"%c%c\", %d)->val.", v->name[0], v->name[1], v->type);
            else
                EMIT(" = gw_var_find_or_create(\"%c\", %d)->val.", v->name[0], v->type);
            switch (v->type) {
            case VT_INT: EMIT("ival;\n"); break;
            case VT_SNG: EMIT("fval;\n"); break;
            case VT_DBL: EMIT("dval;\n"); break;
            case VT_STR: EMIT("sval;\n"); break;
            }
        }
    }
    EMIT("  }\n");
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
        bool is_str = (tok == '"' || tok == TOK_STRINGS);
        if (is_letter(tok)) {
            uint8_t *save = tp;
            char name[2];
            gw_valtype_t type = parse_var(name);
            tp = save;
            is_str = (type == VT_STR);
        }
        if (tok == TOK_PREFIX_FF) {
            uint8_t func = tp[1];
            is_str = (func == FUNC_CHR || func == FUNC_STR || func == FUNC_LEFT
                   || func == FUNC_RIGHT || func == FUNC_MID || func == FUNC_SPACE
                   || func == FUNC_HEX || func == FUNC_OCT);
        }
        /* FE-prefix string functions: ENVIRON$, DATE$, TIME$, ERDEV$ */
        if (tok == TOK_PREFIX_FE) {
            uint8_t xtok = tp[1];
            /* Check if followed by $ (string form) — peek at byte after FE+xstmt */
            /* Peek past FE+xstmt (and any spaces) for $ suffix */
            uint8_t *pk = tp + 2;
            while (*pk == ' ') pk++;
            if ((xtok == XSTMT_ENVIRON || xtok == XSTMT_ERDEV || xtok == XSTMT_IOCTL) && *pk == '$')
                is_str = true;
            if (xtok == XSTMT_DATE || xtok == XSTMT_TIME)
                is_str = true;
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
            EMIT("    _elem->type = %d;\n", type);
            switch (type) {
            case VT_INT: EMIT("    _elem->ival = gw_cint((double)("); break;
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
        /* Evaluate RHS to temp first (RHS may reference the same var) */
        EMIT("  { gw_string_t _rhs = ");
        emit_str_expr();
        EMIT("; gw_str_free(&");
        emit_varname(name, type);
        EMIT("); ");
        emit_varname(name, type);
        EMIT(" = _rhs; }\n");
    } else {
        EMIT("  ");
        emit_varname(name, type);
        if (type == VT_INT) {
            EMIT(" = gw_cint((double)(");
            emit_num_expr();
            EMIT("));\n");
        } else {
            EMIT(" = (%s)(", c_type(type));
            emit_num_expr();
            EMIT(");\n");
        }
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
        uint8_t *print_tok = tp;  /* save position of PRINT token */
        advance();
        skip_spaces();
        /* PRINT USING — embed token bytes and call runtime */
        if (cur() == TOK_USING) {
            /* Skip past USING token, then embed remaining bytes */
            advance(); /* skip TOK_USING */
            uint8_t *start = tp;
            /* Scan to end of statement, skipping strings and constants
             * (float constants may contain 0x00 bytes) */
            {
                /* Find the token line end from the program_line_t */
                program_line_t *cur_line = NULL;
                for (program_line_t *pl = gw.prog_head; pl; pl = pl->next) {
                    if (tp >= pl->tokens && tp < pl->tokens + pl->len + 1) {
                        cur_line = pl; break;
                    }
                }
                uint8_t *line_end = cur_line ? cur_line->tokens + cur_line->len : tp + 256;
                while (tp < line_end) {
                    if (*tp == '"') { tp++; while (tp < line_end && *tp != '"') tp++; if (tp < line_end) tp++; continue; }
                    if (*tp == TOK_CONST_SNG) { tp += 5; continue; }
                    if (*tp == TOK_CONST_DBL) { tp += 9; continue; }
                    if (*tp == TOK_INT2) { tp += 3; continue; }
                    if (*tp == TOK_INT1) { tp += 2; continue; }
                    if (*tp == ':' || *tp == 0) break;
                    tp++;
                }
            }
            int len = (int)(tp - start);
            /* Sync compiled variables to interpreter table so gw_eval works */
            EMIT("  {\n");
            for (int vi = 0; vi < ana->var_count; vi++) {
                var_info_t *v = &ana->vars[vi];
                char n0 = v->name[0], n1 = v->name[1];
                if (n1)
                    EMIT("    gw_var_find_or_create(\"%c%c\", %d)->val = ", n0, n1, v->type);
                else
                    EMIT("    gw_var_find_or_create(\"%c\", %d)->val = ", n0, v->type);
                EMIT("(gw_value_t){.type=%d,", v->type);
                switch (v->type) {
                case VT_INT: EMIT(".ival="); break;
                case VT_SNG: EMIT(".fval="); break;
                case VT_DBL: EMIT(".dval="); break;
                case VT_STR: EMIT(".sval="); break;
                }
                emit_varname(v->name, v->type);
                EMIT("};\n");
            }
            EMIT("    static const uint8_t _pu[] = {");
            for (int i = 0; i < len; i++)
                EMIT("%s%u", i ? "," : "", start[i]);
            EMIT(",0};\n");
            EMIT("    gw.text_ptr = (uint8_t *)_pu;\n");
            EMIT("    gw_print_using(NULL);\n  }\n");
            return;
        }
        /* PRINT # (file) — delegate to runtime */
        if (cur() == '#') {
            emit_delegate_stmt(print_tok, false);
            return;
        }
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
        /* Store limit and step at function scope (no {} block — FOR may span IF THEN) */
        EMIT("  static %s _for_limit_%d; _for_limit_%d = (%s)(",
             c_type(type), for_label_counter, for_label_counter, c_type(type));
        emit_num_expr();
        EMIT(");\n");
        EMIT("  static %s _for_step_%d; _for_step_%d = 1;\n",
             c_type(type), for_label_counter, for_label_counter);
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
        EMIT("  goto for_top_%d;\n", fc);
        EMIT("  for_done_%d: ;\n", fc);
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
                EMIT("  gwrt_error_target = %u; gw.on_error_line = %u;\n", target, target);
                EMIT("  { static program_line_t _el = {.num=%u,.tokens=(uint8_t*)\"\\0\"};\n", target);
                EMIT("    if (!gw_find_line(%u)) { _el.next = gw.prog_head; gw.prog_head = &_el; } }\n", target);
            }
            return;
        }
        /* ON TIMER / ON KEY / ON COM — event trapping, skip */
        if (cur() == TOK_PREFIX_FE || cur() == TOK_KEY) {
            EMIT("  /* ON event trap — not compiled */\n");
            while (cur() && cur() != ':' && cur() != 0) tp++;
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
        /* Parse both operands, emit as pointer swaps */
        EMIT("  {\n");
        for (int si = 0; si < 2; si++) {
            skip_spaces();
            if (si == 1 && cur() == ',') advance();
            skip_spaces();
            char name[2];
            gw_valtype_t type = parse_var(name);
            skip_spaces();
            if (cur() == '(') {
                /* Array element */
                advance();
                char *sbufs[8]; int nd = 0;
                do {
                    if (nd > 0 && cur() == ',') advance();
                    sbufs[nd++] = emit_to_buf(emit_prec_wrapper, 0);
                } while (cur() == ',' && nd < 8);
                if (cur() == ')') advance();
                EMIT("    gw_value_t *_sw%d = gwrt_array_elem(", si);
                emit_name_str(name);
                EMIT(", %d, %d, (int[]){", type, nd);
                for (int d = 0; d < nd; d++) {
                    if (d > 0) EMIT(",");
                    EMIT("(int)(%s)", sbufs[d]);
                    free(sbufs[d]);
                }
                EMIT("});\n");
            } else {
                /* Scalar: wrap in a static gw_value_t so we have a pointer */
                EMIT("    static gw_value_t _swv%d; _swv%d.type = %d; ",
                     si, si, type);
                switch (type) {
                case VT_INT: EMIT("_swv%d.ival = ", si); break;
                case VT_SNG: EMIT("_swv%d.fval = ", si); break;
                case VT_DBL: EMIT("_swv%d.dval = ", si); break;
                case VT_STR: EMIT("_swv%d.sval = ", si); break;
                }
                emit_varname(name, type);
                EMIT(";\n    gw_value_t *_sw%d = &_swv%d;\n", si, si);
            }
        }
        EMIT("    gw_value_t _tmp = *_sw0; *_sw0 = *_sw1; *_sw1 = _tmp;\n");
        /* For scalar operands, write back from the wrapper */
        EMIT("  }\n");
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
        } else if (cur() == TOK_FN) {
            /* DEF FN — embed tokens for runtime interpreter */
            uint8_t *start = tp - 2;  /* back to TOK_DEF */
            while (*tp && *tp != ':') tp++;
            int len = (int)(tp - start);
            EMIT("  { static const uint8_t _df[] = {");
            for (int i = 0; i < len; i++)
                EMIT("%s%u", i ? "," : "", start[i]);
            EMIT(",0};\n");
            EMIT("    uint8_t *_save = gw.text_ptr;\n");
            EMIT("    gw.text_ptr = (uint8_t *)_df;\n");
            EMIT("    gw_exec_stmt();\n");
            EMIT("    gw.text_ptr = _save; }\n");
        } else {
            while (cur() && cur() != ':' && cur() != 0) tp++;
        }
        return;
    }

    /* RANDOMIZE */
    if (tok == TOK_RANDOMIZE) {
        advance();
        skip_spaces();
        if (cur() && cur() != ':' && cur() != 0) {
            EMIT("  { extern uint32_t gw_rnd_seed; gw_rnd_seed = (uint32_t)(");
            emit_num_expr();
            EMIT("); }\n");
        } else {
            EMIT("  { extern uint32_t gw_rnd_seed; gw_rnd_seed = (uint32_t)time(NULL); }\n");
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
    /* RUN ["file"] — if with string arg, delegate to runtime interpreter */
    if (tok == TOK_RUN) {
        uint8_t *start = tp;
        advance();
        skip_spaces();
        if (cur() == '"') {
            /* RUN "file" — load and run via interpreter. Doesn't return. */
            emit_delegate_stmt(start, false);
            EMIT("  gwrt_shutdown(); exit(0);\n");
        }
        /* RUN (without file) — restart compiled program. Just goto first line. */
        return;
    }

    /* SAVE / LOAD / MERGE / BSAVE / BLOAD — delegate to runtime */
    if (tok == TOK_SAVE || tok == TOK_LOAD) {
        uint8_t *start = tp;
        advance();
        emit_delegate_stmt(start, false);
        return;
    }
    /* BSAVE / BLOAD — delegate to runtime */
    if (tok == TOK_BSAVE || tok == TOK_BLOAD) {
        uint8_t *start = tp;
        advance();
        emit_delegate_stmt(start, true);
        return;
    }

    /* PSET / PRESET — delegate to runtime */
    if (tok == TOK_PSET || tok == TOK_PRESET) {
        uint8_t *start = tp;
        advance();
        emit_delegate_stmt(start, false);
        return;
    }

    /* LPRINT / LLIST — delegate to runtime */
    if (tok == TOK_LPRINT || tok == TOK_LLIST) {
        uint8_t *start = tp;
        advance();
        emit_delegate_stmt(start, false);
        return;
    }

    /* LINE — could be LINE INPUT or LINE (graphics). Delegate both. */
    if (tok == TOK_LINE) {
        uint8_t *start = tp;
        advance();
        emit_delegate_stmt(start, true);
        return;
    }

    /* OPEN / CLOSE — delegate to runtime */
    if (tok == TOK_OPEN || tok == TOK_CLOSE) {
        uint8_t *start = tp;
        advance();  /* move past the token to begin scanning */
        emit_delegate_stmt(start, tok == TOK_CLOSE);
        return;
    }
    /* INPUT / LINE INPUT — delegate to runtime (reads variables) */
    if (tok == TOK_INPUT) {
        uint8_t *start = tp;
        advance();
        skip_spaces();
        /* INPUT# (file) or regular INPUT — both delegate */
        emit_delegate_stmt(start, true);  /* read_back = true */
        return;
    }

    /* RESUME / RESUME NEXT / RESUME n */
    if (tok == TOK_RESUME) {
        advance();
        skip_spaces();
        if (cur() == TOK_NEXT) {
            advance();
            EMIT("  gw.in_error_handler = false;\n");
            EMIT("  { uint16_t _el = gw.err_line_num;\n");
            for (int i = 0; i < ana->line_count - 1; i++)
                EMIT("    if (_el == %u) goto L_%u;\n",
                     ana->lines[i].line_num, ana->lines[i+1].line_num);
            EMIT("  }\n");
        } else if (is_const(cur())) {
            uint16_t target = read_int();
            EMIT("  goto L_%u;\n", target);
        } else {
            /* RESUME (same line) */
            EMIT("  { uint16_t _el = gw.err_line_num;\n");
            for (int i = 0; i < ana->line_count; i++)
                EMIT("    if (_el == %u) goto L_%u;\n",
                     ana->lines[i].line_num, ana->lines[i].line_num);
            EMIT("  }\n");
        }
        return;
    }

    /* ERROR n */
    if (tok == TOK_ERROR) {
        advance();
        EMIT("  gw_error((int)(");
        emit_num_expr();
        EMIT("));\n");
        return;
    }

    /* ERASE */
    if (tok == TOK_ERASE) {
        advance();
        EMIT("  /* ERASE — not yet compiled */\n");
        while (cur() && cur() != ':' && cur() != 0) tp++;
        return;
    }

    /* CLEAR [stringspace] */
    if (tok == TOK_CLEAR) {
        advance();
        skip_spaces();
        if (cur() && cur() != ':' && cur() != 0 && !is_const(cur()) && cur() == ',') {
            advance(); skip_spaces();
        }
        if (is_const(cur())) {
            EMIT("  strpool_reset((size_t)(");
            emit_num_expr();
            EMIT("));\n");
        } else {
            EMIT("  strpool_reset(0);\n");
        }
        /* Skip remaining args */
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
        uint8_t *write_start = tp;
        advance();
        skip_spaces();
        /* WRITE # (file) — delegate to runtime */
        if (cur() == '#') {
            emit_delegate_stmt(write_start, false);
            return;
        }
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
        uint8_t *fe_start = tp;  /* save position of 0xFE prefix */
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
            /* Delegate to runtime */
            emit_delegate_stmt(fe_start, false);
            return;
        }
        if (xstmt == XSTMT_TIMER) {
            /* TIMER ON/OFF/STOP */
            EMIT("  /* TIMER trap */\n");
            while (cur() && cur() != ':' && cur() != 0) tp++;
            return;
        }
        /* Graphics, sound, view/window/palette: delegate without read-back */
        if (xstmt == XSTMT_CIRCLE || xstmt == XSTMT_DRAW ||
            xstmt == XSTMT_PAINT || xstmt == XSTMT_PLAY ||
            xstmt == XSTMT_VIEW || xstmt == XSTMT_WINDOW ||
            xstmt == XSTMT_PALETTE) {
            emit_delegate_stmt(fe_start, false);
            return;
        }
        /* File I/O extended stmts: delegate WITH read-back (FIELD/GET modify vars) */
        if (xstmt == XSTMT_FIELD || xstmt == XSTMT_LSET ||
            xstmt == XSTMT_RSET || xstmt == XSTMT_PUT ||
            xstmt == XSTMT_GET) {
            emit_delegate_stmt(fe_start, true);
            return;
        }
        if (xstmt == XSTMT_COMMON) {
            /* COMMON marks variables for CHAIN preservation — delegate */
            emit_delegate_stmt(fe_start, false);
            return;
        }
        if (xstmt == XSTMT_CHAIN) {
            /* CHAIN loads + runs another .bas file via the runtime interpreter.
             * Sync all variables, then delegate. CHAIN doesn't return. */
            emit_delegate_stmt(fe_start, false);
            EMIT("  gwrt_shutdown(); exit(0); /* CHAIN doesn't return */\n");
            return;
        }
        if (xstmt == XSTMT_ENVIRON || xstmt == XSTMT_DATE ||
            xstmt == XSTMT_TIME || xstmt == XSTMT_RESET) {
            /* Delegate to runtime (ENVIRON sets env vars, DATE$/TIME$ sets clock) */
            emit_delegate_stmt(fe_start, false);
            return;
        }
        if (xstmt == XSTMT_ERDEV || xstmt == XSTMT_IOCTL ||
            xstmt == XSTMT_LCOPY || xstmt == XSTMT_CALLS ||
            xstmt == XSTMT_COM || xstmt == XSTMT_PMAP) {
            /* Minor stubs — skip args */
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
                skip_spaces();
                if (cur() == '(') {
                    /* READ into array element */
                    advance();
                    char *sbufs[8]; int nd = 0;
                    do {
                        if (nd > 0 && cur() == ',') advance();
                        sbufs[nd++] = emit_to_buf(emit_prec_wrapper, 0);
                    } while (cur() == ',' && nd < 8);
                    if (cur() == ')') advance();
                    EMIT("  { gw_value_t *_re = gwrt_array_elem(");
                    emit_name_str(name);
                    EMIT(", %d, %d, (int[]){", type, nd);
                    for (int d = 0; d < nd; d++) {
                        if (d > 0) EMIT(",");
                        EMIT("(int)(%s)", sbufs[d]);
                        free(sbufs[d]);
                    }
                    EMIT("});\n");
                    if (type == VT_STR) {
                        EMIT("    gw_str_free(&_re->sval);\n");
                        EMIT("    _re->sval = gw_str_from_cstr(gwrt_data_read());\n");
                        EMIT("    _re->type = VT_STR; }\n");
                    } else {
                        EMIT("    _re->type = %d;\n", type);
                        switch (type) {
                        case VT_INT: EMIT("    _re->ival = (int16_t)atof(gwrt_data_read()); }\n"); break;
                        case VT_DBL: EMIT("    _re->dval = atof(gwrt_data_read()); }\n"); break;
                        default: EMIT("    _re->fval = (float)atof(gwrt_data_read()); }\n"); break;
                        }
                    }
                } else if (type == VT_STR) {
                    EMIT("  gw_str_free(&");
                    emit_varname(name, type);
                    EMIT(");\n");
                    EMIT("  ");
                    emit_varname(name, type);
                    EMIT(" = gw_str_from_cstr(gwrt_data_read());\n");
                } else {
                    EMIT("  ");
                    emit_varname(name, type);
                    if (type == VT_INT)
                        EMIT(" = (int16_t)atof(gwrt_data_read());\n");
                    else
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
        EMIT("  if (gfx_active()) { gfx_cls(); gfx_flush(); }\n");
        return;
    }

    /* MID$ assignment: MID$(var$, start [,len]) = expr */
    if (tok == TOK_PREFIX_FF && tp[1] == FUNC_MID) {
        /* MID$ assignment: embed tokens and delegate to runtime */
        uint8_t *mid_start = tp;
        /* Find end of statement, skipping strings and constants */
        program_line_t *cur_pl = NULL;
        for (program_line_t *pl = gw.prog_head; pl; pl = pl->next) {
            if (tp >= pl->tokens && tp <= pl->tokens + pl->len) {
                cur_pl = pl; break;
            }
        }
        uint8_t *line_end = cur_pl ? cur_pl->tokens + cur_pl->len : tp + 256;
        while (tp < line_end) {
            if (*tp == '"') { tp++; while (tp < line_end && *tp != '"') tp++; if (tp < line_end) tp++; continue; }
            if (*tp == TOK_CONST_SNG) { tp += 5; continue; }
            if (*tp == TOK_CONST_DBL) { tp += 9; continue; }
            if (*tp == ':' || *tp == 0) break;
            tp++;
        }
        int slen = (int)(tp - mid_start);
        EMIT("  {\n");
        /* Sync string variables to interpreter table */
        for (int vi = 0; vi < ana->var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            if (v->name[1])
                EMIT("    gw_var_find_or_create(\"%c%c\", %d)->val = ", v->name[0], v->name[1], v->type);
            else
                EMIT("    gw_var_find_or_create(\"%c\", %d)->val = ", v->name[0], v->type);
            EMIT("(gw_value_t){.type=%d,", v->type);
            switch (v->type) {
            case VT_INT: EMIT(".ival="); break;
            case VT_SNG: EMIT(".fval="); break;
            case VT_DBL: EMIT(".dval="); break;
            case VT_STR: EMIT(".sval="); break;
            }
            emit_varname(v->name, v->type);
            EMIT("};\n");
        }
        EMIT("    static const uint8_t _ms[] = {");
        for (int i = 0; i < slen; i++)
            EMIT("%s%u", i ? "," : "", mid_start[i]);
        EMIT(",0};\n");
        EMIT("    gw.text_ptr = (uint8_t *)_ms + 2;\n");  /* skip 0xFF,FUNC_MID */
        EMIT("    while (*gw.text_ptr == ' ') gw.text_ptr++;\n");
        /* gw_stmt_mid_assign expects text_ptr at '(' — it does gw_chrget to skip it */
        EMIT("    gw_stmt_mid_assign();\n");
        /* Read back string variables that may have been modified */
        for (int vi = 0; vi < ana->var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            if (v->type != VT_STR) continue;
            EMIT("    ");
            emit_varname(v->name, v->type);
            if (v->name[1])
                EMIT(" = gw_var_find_or_create(\"%c%c\", %d)->val.sval;\n", v->name[0], v->name[1], v->type);
            else
                EMIT(" = gw_var_find_or_create(\"%c\", %d)->val.sval;\n", v->name[0], v->type);
        }
        EMIT("  }\n");
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

    /* Error handling: ALL errors longjmp to gw_error_jmp.
     * gw_error() sets gw.on_error_line = 0 and prints if no handler.
     * For compiled programs, we intercept the longjmp and check
     * gw.on_error_line BEFORE gw_error() clears it.
     * Trick: store a dummy program_line_t so gw_find_line succeeds
     * and gw_error() takes the ON ERROR path → longjmp(gw_run_jmp). */
    EMIT("  static program_line_t _dummy_line = {0};\n");
    EMIT("  _err_entry:\n");
    EMIT("  if (setjmp(gw_run_jmp)) {\n");
    EMIT("    if (gw.on_error_line) {\n");
    EMIT("      int _tgt = gw.on_error_line;\n");
    EMIT("      gw.in_error_handler = true;\n");
    for (int i = 0; i < a->line_count; i++)
        EMIT("      if (_tgt == %u) goto L_%u;\n",
             a->lines[i].line_num, a->lines[i].line_num);
    EMIT("    }\n");
    EMIT("    gwrt_shutdown(); return 1;\n");
    EMIT("  }\n");
    EMIT("  if (setjmp(gw_error_jmp)) { gwrt_shutdown(); return 1; }\n\n");

    /* Emit code for each program line */
    for (program_line_t *line = gw.prog_head; line; line = line->next) {
        EMIT("L_%u:\n", line->num);

        /* Skip GC/break check for REM-only lines */
        bool is_rem = (line->tokens[0] == TOK_REM || line->tokens[0] == TOK_SQUOTE);
        if (!is_rem)
            EMIT("  gwrt_check_line(%u);\n", line->num);

        /* Walk statements on this line */
        tp = line->tokens;
        while (*tp) {
            skip_spaces();
            if (*tp == ':') { tp++; continue; }
            if (*tp == 0) break;

            /* Check for dead code after unconditional transfers */
            uint8_t first = *tp;
            emit_stmt();

            /* Dead code elimination: skip remaining after GOTO/END/STOP/SYSTEM */
            if (first == TOK_GOTO || first == TOK_END || first == TOK_STOP)
                break;
            if (first == TOK_PREFIX_FE && tp > line->tokens) {
                /* Check if it was SYSTEM (xstmt after FE prefix) */
                /* Already emitted and consumed — just continue */
            }
        }
    }

    /* Implicit END at bottom */
    EMIT("  gwrt_shutdown();\n");
    EMIT("  return 0;\n");
    EMIT("}\n");
}
