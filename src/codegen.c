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
#include <math.h>

/* ---- Emit helpers ---- */

static FILE *out;
static analysis_t *ana;
static bool safe_mode;
static bool no_gc_check;
static bool fast_math;
static const char *main_name;
static uint16_t emit_line;  /* current BASIC line number being emitted */
static uint8_t *tp;  /* token pointer (mirrors gw.text_ptr) */
static int ret_label_counter;
static int for_label_counter;

/* FOR stack: maps variable to its for_label_counter */
#define FOR_STACK_MAX 64
static struct { char name[2]; gw_valtype_t type; int label; bool has_step; } for_stack[FOR_STACK_MAX];
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

/* C type at the FFI boundary: STRING crosses as a C string, not gw_string_t. */
static const char *c_ffi_type(gw_valtype_t t)
{
    return (t == VT_STR) ? "const char *" : c_type(t);
}

/* Peek the full identifier at the token cursor into buf (uppercased, type
 * suffix excluded) WITHOUT advancing tp.  Returns the cursor position just
 * past the identifier and any trailing type-suffix character — assign it to
 * tp to consume.  Used to recognise multi-char '$EXTERN function names that
 * parse_var() would otherwise truncate to two significant characters. */
static uint8_t *peek_full_ident(char *buf, int max)
{
    uint8_t *p = tp;
    int i = 0;
    if (is_letter(*p)) {
        while ((is_letter(*p) || (*p >= '0' && *p <= '9')) && i < max - 1)
            buf[i++] = (char)toupper(*p++);
        while (is_letter(*p) || (*p >= '0' && *p <= '9')) p++;
    }
    buf[i] = 0;
    if (*p == '$' || *p == '%' || *p == '!' || *p == '#') p++;
    return p;
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

/* Emit gwrt_array_elem or gwrt_array_elem_safe call prefix */
static void emit_array_elem_call(const char name[2], gw_valtype_t type, int ndims)
{
    if (safe_mode) {
        EMIT("gwrt_array_elem_safe("); emit_name_str(name);
        EMIT(", %d, %d, (int[]){", type, ndims);
    } else {
        EMIT("gwrt_array_elem("); emit_name_str(name);
        EMIT(", %d, %d, (int[]){", type, ndims);
    }
}

/* Emit the closing args for gwrt_array_elem[_safe] */
static void emit_array_elem_close(void)
{
    if (safe_mode)
        EMIT("}, %u)", emit_line);
    else
        EMIT("})");
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
static void emit_extern_call(const extern_func_t *ef);

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
        advance();
        if (safe_mode && peek_expr_type() == VT_INT) {
            EMIT("gw_int_neg((int16_t)(");
            emit_atom();
            EMIT("))");
        } else {
            EMIT("(-(");
            emit_atom();
            EMIT("))");
        }
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
        case FUNC_ABS: {
            /* ABS preserves argument type: int->int, sng->sng, dbl->dbl */
            gw_valtype_t arg_type = peek_expr_type();
            if (arg_type == VT_INT) {
                EMIT("({ int16_t _v = (int16_t)(");
                advance();
                emit_num_expr();
                if (cur() == ')') advance();
                if (safe_mode)
                    EMIT("); _v < 0 ? gw_int_neg(_v) : _v; })");
                else
                    EMIT("); _v < 0 ? -_v : _v; })");
            } else {
                EMIT("fabs(");
                advance();
                emit_num_expr();
                if (cur() == ')') advance();
                EMIT(")");
            }
            return;
        }
        case FUNC_INT:
        case FUNC_SQR:
        case FUNC_SIN:
        case FUNC_COS:
        case FUNC_TAN:
        case FUNC_ATN:
        case FUNC_LOG:
        case FUNC_EXP: {
            const char *cfn[] = {
                [FUNC_INT] = "floor", [FUNC_SQR] = "sqrt",
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
            emit_str_expr();
            if (cur() == ')') advance();
            EMIT("}).ival");
        } else if (xfunc == XFUNC_CVS) {
            EMIT("gw_fn_cvs(&(gw_value_t){.type=VT_STR,.sval=");
            if (cur() == '(') advance();
            emit_str_expr();
            if (cur() == ')') advance();
            EMIT("}).fval");
        } else if (xfunc == XFUNC_CVD) {
            EMIT("gw_fn_cvd(&(gw_value_t){.type=VT_STR,.sval=");
            if (cur() == '(') advance();
            emit_str_expr();
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
            /* Seconds since midnight, offset-aware. */
            EMIT("((float)(((time(NULL)+gw.time_offset_secs) %% 86400)))");
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

    /* Extern (FFI) function call — must be checked before parse_var(), which
     * would truncate the name to two significant characters. */
    if (is_letter(tok)) {
        char fname[EXTERN_NAME_MAX];
        peek_full_ident(fname, sizeof fname);
        const extern_func_t *ef = analysis_find_extern(ana, fname);
        if (ef) {
            if (ef->ret_type != VT_STR) {
                emit_extern_call(ef);
            } else {
                /* string-returning extern in a numeric context: consume the
                 * call so the stream stays in sync, emit a numeric zero */
                FILE *orig = out; char *junk = NULL; size_t js = 0;
                out = open_memstream(&junk, &js);
                emit_extern_call(ef);
                fclose(out); out = orig; free(junk);
                EMIT("0 /* string extern in num ctx */");
            }
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

            emit_array_elem_call(name, type, ndims);
            for (int d = 0; d < ndims; d++) {
                if (d > 0) EMIT(", ");
                EMIT("(int)(%s)", sub_bufs[d]);
                free(sub_bufs[d]);
            }
            emit_array_elem_close();
            EMIT("->");
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

    /* String literal in numeric context: emit a placeholder zero but
     * consume the entire literal body (up to the closing quote) so the
     * outer parser doesn't reparse the contents as random tokens.  The
     * VT_STR-aware caller (emit_num_prec's string-cmp path) re-reads tp
     * from left_start and routes the operand through emit_str_expr; the
     * placeholder is a fallback for non-cmp contexts where a string in
     * a numeric position would already be a type error. */
    if (tok == '"') {
        EMIT("0 /* str literal in num ctx */");
        tp++;
        while (*tp && *tp != '"') tp++;
        if (*tp == '"') tp++;
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
    gw_valtype_t left_type = peek_expr_type();
    uint8_t *left_start = tp;

    /* Fast path: for simple expressions with no MOD/IDIV/POW/string-cmp,
     * emit directly without buffering via open_memstream */
    skip_spaces();
    bool needs_buffer = false;
    /* Peek ahead: scan for MOD/IDIV/POW operators or string type */
    if (left_type == VT_STR) needs_buffer = true;
    /* Safe mode: force buffered path for integer expressions so we can
     * wrap arithmetic in gw_int_add/sub/mul overflow checks */
    if (safe_mode && left_type == VT_INT) needs_buffer = true;
    if (!needs_buffer) {
        uint8_t *peek = tp;
        /* Quick scan of the expression for special operators */
        int depth = 0;
        while (*peek) {
            if (*peek == '(' ) depth++;
            else if (*peek == ')') { if (depth-- <= 0) break; }
            else if (depth == 0 && (*peek == ':' || *peek == 0 || *peek == ','
                     || *peek == ';' || *peek == TOK_THEN || *peek == TOK_ELSE
                     || *peek == TOK_TO || *peek == TOK_STEP))
                break;
            if (*peek == TOK_MOD || *peek == TOK_IDIV || *peek == TOK_POW
                || *peek == TOK_IMP || *peek == TOK_EQV
                || *peek == TOK_GT || *peek == TOK_LT || *peek == TOK_EQ)
                { needs_buffer = true; break; }
            peek++;
        }
    }

    if (!needs_buffer) {
        /* Direct emission — no buffering needed */
        emit_atom();
        for (;;) {
            skip_spaces();
            int prec = op_prec(cur());
            if (prec < min_prec) break;
            uint8_t op = cur();
            advance();
            /* Combined relationals */
            const char *cop = NULL;
            if (op == TOK_LT && cur() == TOK_EQ) { advance(); cop = "<="; }
            else if (op == TOK_GT && cur() == TOK_EQ) { advance(); cop = ">="; }
            else if (op == TOK_LT && cur() == TOK_GT) { advance(); cop = "!="; }
            else if (op == TOK_GT && cur() == TOK_LT) { advance(); cop = "!="; }
            else if (op == TOK_EQ && cur() == TOK_LT) { advance(); cop = "<="; }
            else if (op == TOK_EQ && cur() == TOK_GT) { advance(); cop = ">="; }
            if (cop || op == TOK_GT || op == TOK_LT || op == TOK_EQ) {
                /* Relationals need ternary for GW-BASIC -1/0 result.
                 * Use the buffered path which wraps correctly. */
                const char *rop = cop ? cop : binop_c(op);
                EMIT(" %s ", rop);
                emit_num_prec(prec + 1);
            } else if (op == TOK_DIV) {
                if (fast_math) {
                    /* Force float division (GW-BASIC / always returns float).
                     * Without the cast, integer / integer would trap on
                     * divide-by-zero with SIGFPE on Linux x86. */
                    EMIT(" / (double)(");
                    emit_num_prec(prec + 1);
                    EMIT(")");
                } else {
                    /* Division with zero-check via GCC statement expression */
                    EMIT(" / ({double _d=");
                    emit_num_prec(prec + 1);
                    EMIT("; if(_d==0.0)gw_error(11); _d;})");
                }
            } else {
                EMIT(" %s ", binop_c(op));
                emit_num_prec(prec + 1);
            }
        }
        return;
    }

    /* Slow path: buffer for MOD/IDIV/POW/string comparison */
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
            /* Re-emit left as a full string expression (was emitted as numeric
             * atom); emit_str_expr stops at the comparison op since it only
             * consumes TOK_PLUS. */
            uint8_t *save_tp = tp;
            tp = left_start;
            char *left_str;
            { FILE *orig = out; char *buf = NULL; size_t sz = 0;
              out = open_memstream(&buf, &sz);
              emit_str_expr();
              fclose(out); out = orig; left_str = buf; }
            tp = save_tp;
            char *right_str;
            { FILE *orig = out; char *buf = NULL; size_t sz = 0;
              out = open_memstream(&buf, &sz);
              emit_str_expr();
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

        gw_valtype_t right_type = peek_expr_type();
        char *right = emit_to_buf(emit_prec_wrapper, prec + 1);

        /* Constant folding: if both sides are numeric literals, compute now */
        char *end_l, *end_r;
        double lv = strtod(left, &end_l);
        double rv = strtod(right, &end_r);
        bool const_fold = (*end_l == '\0' && *end_r == '\0' &&
                           !cop && op != TOK_GT && op != TOK_LT && op != TOK_EQ);
        if (const_fold) {
            double result = 0;
            bool folded = true;
            switch (op) {
            case TOK_PLUS:  result = lv + rv; break;
            case TOK_MINUS: result = lv - rv; break;
            case TOK_MUL:   result = lv * rv; break;
            case TOK_DIV:   result = rv != 0 ? lv / rv : 0; folded = (rv != 0); break;
            case TOK_POW:   result = pow(lv, rv); break;
            case TOK_MOD:   result = rv != 0 ? (int16_t)lv % (int16_t)rv : 0; folded = (rv != 0); break;
            case TOK_IDIV:  result = rv != 0 ? (int16_t)lv / (int16_t)rv : 0; folded = (rv != 0); break;
            case TOK_AND:   result = (int16_t)lv & (int16_t)rv; break;
            case TOK_OR:    result = (int16_t)lv | (int16_t)rv; break;
            case TOK_XOR:   result = (int16_t)lv ^ (int16_t)rv; break;
            default: folded = false; break;
            }
            if (folded) {
                free(left); free(right);
                char buf[32];
                if (result == (int)result && result >= -32768 && result <= 32767)
                    snprintf(buf, sizeof(buf), "%d", (int)result);
                else
                    snprintf(buf, sizeof(buf), "%.9g", result);
                left = strdup(buf);
                continue;
            }
        }

        char *combined = NULL;
        size_t csz = 0;
        FILE *cm = open_memstream(&combined, &csz);

        if (cop) {
            fprintf(cm, "((%s %s %s) ? -1 : 0)", left, cop, right);
        } else if (op == TOK_GT || op == TOK_LT || op == TOK_EQ) {
            fprintf(cm, "((%s %s %s) ? -1 : 0)", left, binop_c(op), right);
        } else if (op == TOK_MOD) {
            if (safe_mode)
                fprintf(cm, "gw_int_mod((int16_t)(%s), (int16_t)(%s))", left, right);
            else
                fprintf(cm, "((int16_t)(%s) %% (int16_t)(%s))", left, right);
        } else if (op == TOK_IDIV) {
            if (safe_mode)
                fprintf(cm, "gw_int_idiv((int16_t)(%s), (int16_t)(%s))", left, right);
            else
                fprintf(cm, "((int16_t)(%s) / (int16_t)(%s))", left, right);
        } else if (op == TOK_IMP) {
            /* A IMP B = NOT(A) OR B = ~a | b */
            fprintf(cm, "((int16_t)(~(int16_t)(%s) | (int16_t)(%s)))", left, right);
        } else if (op == TOK_EQV) {
            /* A EQV B = NOT(A XOR B) = ~(a ^ b) */
            fprintf(cm, "((int16_t)(~((int16_t)(%s) ^ (int16_t)(%s))))", left, right);
        } else if (op == TOK_POW) {
            fprintf(cm, "pow((double)(%s), (double)(%s))", left, right);
        } else if (op == TOK_DIV) {
            if (fast_math)
                fprintf(cm, "((double)(%s) / (double)(%s))", left, right);
            else
                /* GW-BASIC / always produces float; check for division by zero */
                fprintf(cm, "({double _dv=(double)(%s); if(_dv==0.0) gw_error(11); (double)(%s)/_dv;})",
                        right, left);
        } else if (safe_mode && left_type == VT_INT && right_type == VT_INT &&
                   (op == TOK_PLUS || op == TOK_MINUS || op == TOK_MUL)) {
            const char *fn = op == TOK_PLUS ? "gw_int_add"
                           : op == TOK_MINUS ? "gw_int_sub" : "gw_int_mul";
            fprintf(cm, "%s((int16_t)(%s), (int16_t)(%s))", fn, left, right);
        } else {
            fprintf(cm, "(%s %s %s)", left, binop_c(op), right);
        }
        fclose(cm);

        free(left);
        free(right);
        left = combined;

        /* Update left_type for subsequent ops.  Prevents wrapping float
         * intermediates in gw_int_add (e.g. I% * 2.5 + I%). */
        if (op == TOK_DIV || op == TOK_POW)
            left_type = VT_DBL;
        else if (cop || op == TOK_GT || op == TOK_LT || op == TOK_EQ)
            left_type = VT_INT;  /* comparisons return 0/-1 */
        else if (op == TOK_PLUS && left_type == VT_STR && right_type == VT_STR)
            left_type = VT_STR;  /* string concat stays string */
        else if (left_type != VT_INT || right_type != VT_INT)
            left_type = (left_type == VT_DBL || right_type == VT_DBL)
                      ? VT_DBL : VT_SNG;
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

    /* String literal.  Concatenation is handled by emit_str_expr (the atom
     * just emits the literal value); a previous attempt to handle '+' at
     * the atom level emitted references to an undeclared `_cat` variable. */
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
            EMIT("({time_t _t=time(NULL)+gw.time_offset_secs; struct tm *_tm=localtime(&_t);"
                 " char _db[16]; snprintf(_db,16,\"%%02d-%%02d-%%04d\","
                 "_tm->tm_mon+1,_tm->tm_mday,_tm->tm_year+1900);"
                 " gw_str_from_cstr(_db);})");
            return;
        }
        if (xtok == XSTMT_TIME) {
            EMIT("({time_t _t=time(NULL)+gw.time_offset_secs; struct tm *_tm=localtime(&_t);"
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

    /* Extern (FFI) function call returning a string. */
    if (is_letter(tok)) {
        char fname[EXTERN_NAME_MAX];
        peek_full_ident(fname, sizeof fname);
        const extern_func_t *ef = analysis_find_extern(ana, fname);
        if (ef) {
            if (ef->ret_type == VT_STR) {
                emit_extern_call(ef);
            } else {
                /* numeric extern in a string context: consume + empty string */
                FILE *orig = out; char *junk = NULL; size_t js = 0;
                out = open_memstream(&junk, &js);
                emit_extern_call(ef);
                fclose(out); out = orig; free(junk);
                EMIT("gw_str_from_cstr(\"\") /* numeric extern in str ctx */");
            }
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
            EMIT("gw_str_copy(&");
            emit_array_elem_call(name, type, ndims);
            for (int d = 0; d < ndims; d++) {
                if (d > 0) EMIT(", ");
                EMIT("(int)(%s)", sub_bufs[d]);
                free(sub_bufs[d]);
            }
            emit_array_elem_close();
            EMIT("->sval)");
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
/* Emit a call to a declared '$EXTERN C function.  tp is positioned at the
 * function name; this consumes the name and a parenthesised argument list,
 * emitting a GCC statement-expression that coerces each argument to its
 * declared C type, calls the function, frees any temporary C strings, and
 * yields the result.  String arguments cross as NUL-terminated char*; a
 * string return is copied into the BASIC string pool (the callee owns its
 * returned buffer — it is not freed here). */
static void emit_extern_call(const extern_func_t *ef)
{
    char namebuf[EXTERN_NAME_MAX];
    tp = peek_full_ident(namebuf, sizeof namebuf);  /* consume name + suffix */
    skip_spaces();

    char *argbuf[MAX_EXTERN_ARGS];
    gw_valtype_t argt[MAX_EXTERN_ARGS];
    int n = 0;       /* args actually passed (capped at MAX_EXTERN_ARGS) */
    int total = 0;   /* args seen in the source call */
    if (cur() == '(') {
        advance();
        if (cur() != ')') {
            do {
                if (total > 0 && cur() == ',') advance();
                gw_valtype_t at = (total < ef->argc) ? ef->arg_types[total] : VT_SNG;
                FILE *orig = out; char *b = NULL; size_t sz = 0;
                out = open_memstream(&b, &sz);
                if (at == VT_STR) emit_str_expr();
                else              emit_num_expr();
                fclose(out); out = orig;
                /* Always consume every argument so the token stream stays in
                 * sync; only the first MAX_EXTERN_ARGS are passed through. */
                if (n < MAX_EXTERN_ARGS) { argbuf[n] = b; argt[n] = at; n++; }
                else                       free(b);
                total++;
            } while (cur() == ',');
        }
        if (cur() == ')') advance();
    }
    if (total != ef->argc)
        fprintf(stderr, "warning: line %u: extern %s called with %d argument(s),"
                " declared with %d\n", emit_line, ef->name, total, ef->argc);

    EMIT("({ ");
    for (int i = 0; i < n; i++) {
        if (argt[i] == VT_STR)
            EMIT("gw_string_t _s%d = (%s); char *_a%d = gw_str_to_cstr(&_s%d);"
                 " gw_str_free(&_s%d); ", i, argbuf[i], i, i, i);
        else
            EMIT("%s _a%d = (%s)(%s); ",
                 c_ffi_type(argt[i]), i, c_ffi_type(argt[i]), argbuf[i]);
        free(argbuf[i]);
    }
    if (ef->ret_type == VT_STR)
        EMIT("const char *_r = %s(", ef->name);
    else
        EMIT("%s _r = %s(", c_ffi_type(ef->ret_type), ef->name);
    for (int i = 0; i < n; i++) { if (i) EMIT(", "); EMIT("_a%d", i); }
    EMIT("); ");
    if (ef->ret_type == VT_STR) {
        /* Copy the result into the string pool BEFORE freeing the C-string arg
         * temporaries — a callee may legitimately return (a pointer into) one
         * of its char* arguments (e.g. an in-place trim), so freeing first
         * would be a use-after-free. */
        EMIT("gw_string_t _ret = gw_str_from_cstr(_r ? _r : \"\"); ");
        for (int i = 0; i < n; i++)
            if (argt[i] == VT_STR) EMIT("free(_a%d); ", i);
        EMIT("_ret; })");
    } else {
        for (int i = 0; i < n; i++)
            if (argt[i] == VT_STR) EMIT("free(_a%d); ", i);
        EMIT("_r; })");
    }
}

static gw_valtype_t peek_expr_type(void)
{
    uint8_t *save = tp;
    skip_spaces();
    uint8_t tok = cur();

    /* Variable — check suffix (most important case) */
    if (is_letter(tok)) {
        char fname[EXTERN_NAME_MAX];
        peek_full_ident(fname, sizeof fname);
        const extern_func_t *ef = analysis_find_extern(ana, fname);
        if (ef) { tp = save; return ef->ret_type; }
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
    /* Parenthesized expression — peek inside */
    if (tok == '(') { advance(); gw_valtype_t t = peek_expr_type(); tp = save; return t; }
    /* Functions */
    if (tok == TOK_PREFIX_FF) {
        uint8_t func = tp[1];
        tp = save;
        switch (func) {
        case FUNC_LEN: case FUNC_ASC: case FUNC_CINT: case FUNC_FIX:
        case FUNC_POS: case FUNC_PEEK: case FUNC_INP:
        case FUNC_SGN:
            return VT_INT;
        case FUNC_ABS: {
            /* ABS preserves the type of its argument */
            tp += 2; /* skip FF ABS */
            skip_spaces();
            if (*tp == '(') tp++;
            gw_valtype_t arg_type = peek_expr_type();
            tp = save;
            return arg_type;
        }
        /* GW-BASIC's transcendentals are single-precision; only CDBL
         * explicitly forces double.  Promoting them to VT_DBL here makes
         * PRINT use 15-digit format instead of the 7-digit single form. */
        case FUNC_CDBL:
            return VT_DBL;
        case FUNC_VAL: case FUNC_ATN: case FUNC_LOG: case FUNC_EXP:
            return VT_SNG;
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
    /* Integer constants — treated as VT_SNG to avoid false integer-mode
     * emission when constants appear in mixed expressions (e.g. 4*S where S
     * is float). The interpreter treats int constants as VT_INT, so this is
     * a known divergence for safe-mode overflow checking. */
    if ((tok >= 0x11 && tok <= 0x1A) || tok == TOK_INT1 || tok == TOK_INT2)
        { tp = save; return VT_SNG; }
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
/* sync_all: if true, sync ALL variables (needed for CHAIN which passes
 * variables to another program). If false, use selective sync. */
static void emit_delegate_stmt_ex(uint8_t *stmt_start, bool read_back, bool sync_all);

static void emit_delegate_stmt(uint8_t *stmt_start, bool read_back)
{
    emit_delegate_stmt_ex(stmt_start, read_back, false);
}

static void emit_delegate_stmt_ex(uint8_t *stmt_start, bool read_back, bool sync_all)
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
    /* Variable sync: selective (scan tokens for referenced names) or full */
    for (int vi = 0; vi < ana->var_count; vi++) {
        var_info_t *v = &ana->vars[vi];
        if (!sync_all) {
            bool found = false;
            uint8_t *sc = stmt_start;
            while (sc < tp) {
                if (*sc == '"') { sc++; while (sc < tp && *sc != '"') sc++; if (sc < tp) sc++; continue; }
                if (*sc == TOK_CONST_SNG) { sc += 5; continue; }
                if (*sc == TOK_CONST_DBL) { sc += 9; continue; }
                if (*sc == (uint8_t)v->name[0]) {
                    if (!v->name[1] || (sc + 1 < tp && sc[1] == (uint8_t)v->name[1])) {
                        found = true; break;
                    }
                }
                sc++;
            }
            if (!found) continue;
        }
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
            char fname[EXTERN_NAME_MAX];
            peek_full_ident(fname, sizeof fname);
            const extern_func_t *ef = analysis_find_extern(ana, fname);
            if (ef) {
                is_str = (ef->ret_type == VT_STR);
            } else {
                uint8_t *save = tp;
                char name[2];
                gw_valtype_t type = parse_var(name);
                tp = save;
                is_str = (type == VT_STR);
            }
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
        if (safe_mode) {
            EMIT("    gw_value_t *_elem = gwrt_array_elem_safe(");
            emit_name_str(name);
            EMIT(", %d, %d, _subs, %u);\n", type, ndims, emit_line);
        } else {
            EMIT("    gw_value_t *_elem = gwrt_array_elem(");
            emit_name_str(name);
            EMIT(", %d, %d, _subs);\n", type, ndims);
        }

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
            EMIT(type == VT_INT ? "));\n" : ");\n");
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
        if (safe_mode)
            EMIT("  gwrt_gosub_push_safe(%d, %u); goto L_%u;\n", rl, emit_line, target);
        else
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
        if (type == VT_INT) {
            EMIT(" = gw_cint((double)(");
            emit_num_expr();
            EMIT("));\n");
        } else {
            EMIT(" = (%s)(", c_type(type));
            emit_num_expr();
            EMIT(");\n");
        }
        skip_spaces();
        if (cur() == TOK_TO) advance();
        if (safe_mode && type == VT_INT) {
            EMIT("  static int16_t _for_limit_%d; _for_limit_%d = gw_cint((double)(",
                 for_label_counter, for_label_counter);
            emit_num_expr();
            EMIT("));\n");
        } else {
            EMIT("  static %s _for_limit_%d; _for_limit_%d = (%s)(",
                 c_type(type), for_label_counter, for_label_counter, c_type(type));
            emit_num_expr();
            EMIT(");\n");
        }
        bool has_step = false;
        skip_spaces();
        if (cur() == TOK_STEP) {
            has_step = true;
            if (safe_mode && type == VT_INT) {
                EMIT("  static int16_t _for_step_%d; _for_step_%d = gw_cint((double)(",
                     for_label_counter, for_label_counter);
                advance();
                emit_num_expr();
                EMIT("));\n");
            } else {
                EMIT("  static %s _for_step_%d; _for_step_%d = (%s)(",
                     c_type(type), for_label_counter, for_label_counter, c_type(type));
                advance();
                emit_num_expr();
                EMIT(");\n");
            }
        }
        int fc = for_label_counter++;
        EMIT("    for_top_%d:\n", fc);
        if (has_step) {
            /* Variable step: need sign check */
            EMIT("    if (_for_step_%d >= 0 ? ", fc);
            emit_varname(name, type);
            EMIT(" > _for_limit_%d : ", fc);
            emit_varname(name, type);
            EMIT(" < _for_limit_%d) goto for_done_%d;\n", fc, fc);
        } else {
            /* Default step=1: simple comparison */
            EMIT("    if (");
            emit_varname(name, type);
            EMIT(" > _for_limit_%d) goto for_done_%d;\n", fc, fc);
        }
        /* Push onto FOR stack */
        if (for_stack_sp < FOR_STACK_MAX) {
            for_stack[for_stack_sp].name[0] = name[0];
            for_stack[for_stack_sp].name[1] = name[1];
            for_stack[for_stack_sp].type = type;
            for_stack[for_stack_sp].label = fc;
            for_stack[for_stack_sp].has_step = has_step;
            for_stack_sp++;
        }
        return;
    }

    /* NEXT [var[,var...]] */
    if (tok == TOK_NEXT) {
        advance();
        skip_spaces();
        /* Handle one or more variables (NEXT J,I = NEXT J : NEXT I) */
        do {
            skip_spaces();
            int fc = -1;
            if (is_letter(cur())) {
                char name[2];
                gw_valtype_t type = parse_var(name);
                /* Search stack from top for matching variable */
                bool step_custom = true;
                for (int i = for_stack_sp - 1; i >= 0; i--) {
                    if (for_stack[i].name[0] == name[0] &&
                        for_stack[i].name[1] == name[1] &&
                        for_stack[i].type == type) {
                        fc = for_stack[i].label;
                        step_custom = for_stack[i].has_step;
                        for_stack_sp = i;
                        break;
                    }
                }
                if (fc < 0) fc = for_label_counter > 0 ? for_label_counter - 1 : 0;
                EMIT("  ");
                if (safe_mode && type == VT_INT) {
                    emit_varname(name, type);
                    if (step_custom) {
                        EMIT(" = gw_int_add(");
                        emit_varname(name, type);
                        EMIT(", _for_step_%d);\n", fc);
                    } else {
                        EMIT(" = gw_int_add(");
                        emit_varname(name, type);
                        EMIT(", 1);\n");
                    }
                } else {
                    emit_varname(name, type);
                    if (step_custom)
                        EMIT(" += _for_step_%d;\n", fc);
                    else
                        EMIT("++;\n");
                }
            } else {
                /* NEXT without variable -- match most recent FOR */
                char bname[2] = {0, 0};
                gw_valtype_t btype = VT_SNG;
                bool bstep = false;
                if (for_stack_sp > 0) {
                    for_stack_sp--;
                    fc = for_stack[for_stack_sp].label;
                    bname[0] = for_stack[for_stack_sp].name[0];
                    bname[1] = for_stack[for_stack_sp].name[1];
                    btype = for_stack[for_stack_sp].type;
                    bstep = for_stack[for_stack_sp].has_step;
                } else {
                    fc = for_label_counter > 0 ? for_label_counter - 1 : 0;
                }
                if (bname[0]) {
                    EMIT("  ");
                    if (safe_mode && btype == VT_INT) {
                        emit_varname(bname, btype);
                        if (bstep) {
                            EMIT(" = gw_int_add(");
                            emit_varname(bname, btype);
                            EMIT(", _for_step_%d);\n", fc);
                        } else {
                            EMIT(" = gw_int_add(");
                            emit_varname(bname, btype);
                            EMIT(", 1);\n");
                        }
                    } else {
                        emit_varname(bname, btype);
                        if (bstep)
                            EMIT(" += _for_step_%d;\n", fc);
                        else
                            EMIT("++;\n");
                    }
                }
            }
            EMIT("  goto for_top_%d;\n", fc);
            EMIT("  for_done_%d: ;\n", fc);
            skip_spaces();
        } while (cur() == ',' && (advance(), 1));
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
                if (safe_mode)
                    EMIT("      case %d: gwrt_gosub_push_safe(%d, %u); goto L_%u;\n", n++, rl, emit_line, target);
                else
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
        char sw_names[2][2];
        gw_valtype_t sw_types[2];
        bool sw_is_scalar[2] = {false, false};
        for (int si = 0; si < 2; si++) {
            skip_spaces();
            if (si == 1 && cur() == ',') advance();
            skip_spaces();
            char name[2];
            gw_valtype_t type = parse_var(name);
            sw_names[si][0] = name[0];
            sw_names[si][1] = name[1];
            sw_types[si] = type;
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
                if (safe_mode) {
                    EMIT("    gw_value_t *_sw%d = gwrt_array_elem_safe(", si);
                    emit_name_str(name);
                    EMIT(", %d, %d, (int[]){", type, nd);
                    for (int d = 0; d < nd; d++) {
                        if (d > 0) EMIT(",");
                        EMIT("(int)(%s)", sbufs[d]);
                        free(sbufs[d]);
                    }
                    EMIT("}, %u);\n", emit_line);
                } else {
                    EMIT("    gw_value_t *_sw%d = gwrt_array_elem(", si);
                    emit_name_str(name);
                    EMIT(", %d, %d, (int[]){", type, nd);
                    for (int d = 0; d < nd; d++) {
                        if (d > 0) EMIT(",");
                        EMIT("(int)(%s)", sbufs[d]);
                        free(sbufs[d]);
                    }
                    EMIT("});\n");
                }
            } else {
                /* Scalar: wrap in a static gw_value_t so we have a pointer */
                sw_is_scalar[si] = true;
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
        /* Write back scalar operands from the wrapper to the C variable */
        for (int si = 0; si < 2; si++) {
            if (!sw_is_scalar[si]) continue;
            EMIT("    ");
            emit_varname(sw_names[si], sw_types[si]);
            EMIT(" = _sw%d->", si);
            switch (sw_types[si]) {
            case VT_INT: EMIT("ival"); break;
            case VT_SNG: EMIT("fval"); break;
            case VT_DBL: EMIT("dval"); break;
            case VT_STR: EMIT("sval"); break;
            }
            EMIT(";\n");
        }
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
            /* DRAW/PLAY use =variable; substitution in strings, so
             * need full sync (selective would miss string-embedded refs) */
            bool full = (xstmt == XSTMT_DRAW || xstmt == XSTMT_PLAY);
            emit_delegate_stmt_ex(fe_start, false, full);
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
             * Must sync ALL variables (chained program reads COMMON vars). */
            emit_delegate_stmt_ex(fe_start, false, true);
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
                    if (safe_mode) {
                        EMIT("  { gw_value_t *_re = gwrt_array_elem_safe(");
                        emit_name_str(name);
                        EMIT(", %d, %d, (int[]){", type, nd);
                        for (int d = 0; d < nd; d++) {
                            if (d > 0) EMIT(",");
                            EMIT("(int)(%s)", sbufs[d]);
                            free(sbufs[d]);
                        }
                        EMIT("}, %u);\n", emit_line);
                    } else {
                        EMIT("  { gw_value_t *_re = gwrt_array_elem(");
                        emit_name_str(name);
                        EMIT(", %d, %d, (int[]){", type, nd);
                        for (int d = 0; d < nd; d++) {
                            if (d > 0) EMIT(",");
                            EMIT("(int)(%s)", sbufs[d]);
                            free(sbufs[d]);
                        }
                        EMIT("});\n");
                    }
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

void codegen_emit(FILE *f, analysis_t *a, const codegen_opts_t *opts)
{
    out = f;
    ana = a;
    safe_mode = opts ? opts->safe_mode : false;
    no_gc_check = opts ? opts->no_gc_check : false;
    fast_math = opts ? opts->fast_math : false;
    main_name = (opts && opts->main_name) ? opts->main_name : "main";
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

    /* Foreign-function prototypes from '$EXTERN pragmas (Level 2
     * cross-language linking).  The host project supplies these symbols at
     * link time. */
    for (int i = 0; i < a->extern_count; i++) {
        extern_func_t *e = &a->externs[i];
        EMIT("extern %s %s(", c_ffi_type(e->ret_type), e->name);
        if (e->argc == 0) {
            EMIT("void");
        } else {
            for (int k = 0; k < e->argc; k++) {
                if (k) EMIT(", ");
                EMIT("%s", c_ffi_type(e->arg_types[k]));
            }
        }
        EMIT(");\n");
    }
    if (a->extern_count > 0)
        EMIT("\n");

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

    /* Entry point.  Default name is "main" for standalone executables;
     * --main-name renames it for link into a larger C / Fortran project. */
    EMIT("int %s(int argc, char **argv) {\n", main_name);
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
        emit_line = line->num;
        EMIT("L_%u:\n", line->num);

        /* Skip GC/break check for REM-only lines, and for the entire
         * program under --no-gc-check (string pool will only compact when
         * a heap-pressure threshold is hit, and Ctrl+Break is ignored). */
        bool is_rem = (line->tokens[0] == TOK_REM || line->tokens[0] == TOK_SQUOTE);
        if (!is_rem && !no_gc_check)
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
