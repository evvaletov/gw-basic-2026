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
static int compiled_var_count;
static bool safe_mode;
static bool no_gc_check;
static bool fast_math;
static bool inline_arrays;      /* --inline-arrays: direct C-array indexing */
static int  cg_opt_level = 2;   /* -O tier; see codegen_opts_t.opt_level */
static const char *main_name;
static uint16_t emit_line;  /* current BASIC line number being emitted */
static uint8_t *tp;  /* token pointer (mirrors gw.text_ptr) */
static int ret_label_counter;
static int for_label_counter;
typedef struct {
    program_line_t *line;
    uint8_t *text;
} resume_point_t;
static resume_point_t *resume_points;
static int resume_count, resume_capacity, active_statement;
static program_line_t *emit_source_line;

/* WHILE/WEND have lexical pairing; FOR/NEXT are matched at runtime. */
#define WHILE_STACK_MAX 64
static int while_stack[WHILE_STACK_MAX];
static int while_stack_sp;
static int while_label_counter;

/* Output-size guard.  A codegen path that emits without advancing the token
 * pointer produces unbounded C and can fill the disk (this happened once,
 * writing a 384 GB paren.c).  Bytes written to the real output file are
 * counted -- the open_memstream scratch buffers used for sub-expressions are
 * not -- and codegen aborts cleanly once the limit is exceeded. */
static FILE       *g_real_out;   /* the actual -o file (not a scratch memstream) */
static const char *g_out_path;   /* its path, for removing the partial file      */
static size_t      g_emit_bytes; /* bytes emitted to g_real_out so far            */
static size_t      g_emit_cap;   /* abort once g_emit_bytes exceeds this (0=off)  */

static void codegen_error(const char *message)
{
    fprintf(stderr, "gwbasic-compile: %s at BASIC line %u (token 0x%02x)\n",
            message, emit_line, tp ? *tp : 0);
    if (g_real_out && g_real_out != stdout) {
        fclose(g_real_out);
        if (g_out_path) remove(g_out_path);
    }
    exit(2);
}

static void codegen_overflow(void)
{
    if (g_real_out) {
        fflush(g_real_out);
        if (g_real_out != stdout) {
            fclose(g_real_out);
            if (g_out_path) remove(g_out_path);   /* reclaim the partial file */
        }
        g_real_out = NULL;
    }
    fprintf(stderr,
        "gwbasic-compile: error: generated C exceeded the %zu MB output limit; "
        "aborting (probable compiler codegen loop). "
        "Raise or disable it with --max-output-size <MB> (0 = unlimited).\n",
        g_emit_cap / (1024 * 1024));
    exit(2);
}

#define EMIT(...)                                            \
    do {                                                     \
        int _emit_n = fprintf(out, __VA_ARGS__);             \
        if (out != g_real_out && g_emit_cap                 \
            && ftell(out) > (long)g_emit_cap)               \
            codegen_overflow();                            \
        if (out == g_real_out && _emit_n > 0) {              \
            g_emit_bytes += (size_t)_emit_n;                 \
            if (g_emit_cap && g_emit_bytes > g_emit_cap)     \
                codegen_overflow();                          \
        }                                                    \
    } while (0)

static uint8_t cur(void) { return *tp; }
static uint8_t advance(void)
{
    tp++;
    while (*tp == ' ') tp++;
    return *tp;
}
static void skip_spaces(void) { while (*tp == ' ') tp++; }

static bool line_exists(uint16_t num)
{
    for (int i = 0; i < ana->line_count; i++)
        if (ana->lines[i].line_num == num) return true;
    return false;
}

/* ---- Token stream readers ---- */

static uint16_t read_int(void)
{
    uint16_t value;
    if (!gw_read_line_number(&tp, &value)) codegen_error("invalid line number");
    return value;
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
 * parse_var() applies BASIC name-length and type rules. */
static uint8_t *peek_full_ident(char *buf, int max)
{
    uint8_t *p = tp;
    int i = 0;
    if (is_letter(*p)) {
        while (gw_name_char(*p) && i < max - 1)
            buf[i++] = (char)toupper(*p++);
        while (gw_name_char(*p)) p++;
    }
    buf[i] = 0;
    if (*p == '$' || *p == '%' || *p == '!' || *p == '#') p++;
    return p;
}

/* BASIC names are quoted literally for runtime lookup; dots need a distinct
 * spelling in C symbols. Underscores cannot occur in a BASIC identifier. */
static void emit_name_str(const char *name)
{
    EMIT("\"%s\"", name);
}

static void emit_ident(const char *name)
{
    for (; *name; name++) {
        if (*name == '.') EMIT("_dot_");
        else EMIT("%c", *name);
    }
}

static void emit_varname(const char *name, gw_valtype_t type)
{
    EMIT("var_"); emit_ident(name); EMIT("_%s", type_suffix(type));
}

static void emit_arr_sym(const char *name, gw_valtype_t type)
{
    EMIT("_gwa_"); emit_ident(name);
    if (!name[1]) EMIT("_");
    EMIT("_%d", (int)type);
}

/* Keep optimized accesses on the runtime allocation. Delegated statements
 * therefore see the same elements. A generation check prevents cached pointers
 * surviving CLEAR/ERASE/CHAIN; access before DIM takes the ordinary path. */
static void emit_elem_ptr(const char *name, gw_valtype_t type, int ndims,
                          char *const sub_bufs[])
{
    array_info_t *ai = inline_arrays ? analysis_inlinable_array(ana, name, type) : NULL;
    bool direct = ai && ai->ndims == ndims && !safe_mode;
    if (direct) {
        EMIT("({ int _subs[] = {");
        for (int d = 0; d < ndims; d++)
            EMIT("%sgw_cint((double)(%s))", d ? ", " : "", sub_bufs[d]);
        EMIT("}; gw_value_t *_ap; if ("); emit_arr_sym(name, type);
        EMIT(" && "); emit_arr_sym(name, type);
        EMIT("_generation == gw_array_generation) { int _idx=0, _stride=1, _base=");
        emit_arr_sym(name, type); EMIT("_base;");
        for (int d = 0; d < ndims; d++) {
            EMIT(" { int _s=_subs[%d]-_base, _size=%d+1-_base;"
                 " if (_s<0 || _s>=_size) gw_error(9);"
                 " _idx+=_s*_stride; _stride*=_size; }", d, ai->dims[d]);
        }
        EMIT(" _ap=&"); emit_arr_sym(name, type);
        EMIT("[_idx]; } else { _ap=gwrt_array_elem(");
        emit_name_str(name);
        EMIT(", %d, %d, _subs); } _ap; })", type, ndims);
        return;
    }
    if (safe_mode) { EMIT("gwrt_array_elem_safe("); emit_name_str(name); }
    else           { EMIT("gwrt_array_elem("); emit_name_str(name); }
    EMIT(", %d, %d, (int[]){", type, ndims);
    for (int d = 0; d < ndims; d++) {
        if (d) EMIT(", ");
        EMIT("gw_cint((double)(%s))", sub_bufs[d]);
    }
    if (safe_mode) EMIT("}, %u)", emit_line);
    else           EMIT("})");
}

/* Parse a variable name from the token stream, return its type */
static gw_valtype_t parse_var(char name_out[GW_NAME_SIZE])
{
    if (!gw_scan_name(&tp, name_out)) codegen_error("invalid variable name");
    uint8_t suffix = cur();
    if (suffix == '$') { advance(); return VT_STR; }
    if (suffix == '%') { advance(); return VT_INT; }
    if (suffix == '!') { advance(); return VT_SNG; }
    if (suffix == '#') { advance(); return VT_DBL; }
    skip_spaces();
    if (ana->runtime_types) return 0;
    int idx = name_out[0] - 'A';
    return (idx >= 0 && idx < 26) ? ana->def_type[idx] : VT_SNG;
}

/* The last consumed nonspace byte records an explicit type suffix. */
static bool parsed_explicit_type(const uint8_t *start)
{
    const uint8_t *end = tp;
    while (end > start && end[-1] == ' ') end--;
    return end > start && (end[-1] == '%' || end[-1] == '!' ||
                           end[-1] == '#' || end[-1] == '$');
}

/* ---- Expression compilation ---- */

/* Forward declarations */
static void emit_str_expr(void);
static void emit_num_expr(void);
static void emit_dynamic_expr(void);
static void emit_dynamic_assignment(void);
static bool emit_dynamic_statement(uint8_t tok);
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
static bool peek_is_string_expr(void);
static void emit_stmt_guarded(bool new_statement);

/* Emit a numeric atom */
static void emit_atom(void)
{
    skip_spaces();
    uint8_t tok = cur();
    if (!tok || tok == ')' || tok == ',' || tok == ':' || tok == ';'
        || tok == TOK_THEN || tok == TOK_ELSE)
        codegen_error("expected an expression");

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
            EMIT("((float)gw_rnd(");
            if (cur() == '(') {
                advance();
                emit_num_expr();
                if (cur() != ')') codegen_error("expected ')' after RND argument");
                advance();
            } else EMIT("1.0");
            EMIT("))");
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
            if (cur() != '(') codegen_error("expected '(' after POS");
            advance();
            EMIT("((void)(");
            if (peek_is_string_expr()) emit_str_expr(); else emit_num_expr();
            if (cur() != ')') codegen_error("expected ')' after POS argument");
            advance();
            EMIT("), (gw_hal ? gw_hal->get_cursor_col() + 1 : 1))");
            return;
        case FUNC_FRE:
            /* In compiled code, GC can't find C-static variables.
             * Sync vars to interpreter table before GC for FRE(""). */
            if (cur() == '(' && tp[1] == '"') {
                EMIT("({");
                for (int vi = 0; vi < compiled_var_count; vi++) {
                    var_info_t *v = &ana->vars[vi];
                    if (v->type != VT_STR) continue;
                    EMIT("gw_var_find_or_create(\"%s\",%d)->val.sval=", v->name, v->type);
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
        while (gw_name_char(cur())) advance();
        if (cur() == '%' || cur() == '!' || cur() == '#' || cur() == '$') advance();
        skip_spaces();
        if (cur() == '(') {
            int depth = 1; advance();
            while (*tp && depth > 0) {
                if (cur() == TOK_INT1) { tp += 2; continue; }
                if (cur() == TOK_INT2) { tp += 3; continue; }
                if (cur() == TOK_CONST_SNG) { tp += 5; continue; }
                if (cur() == TOK_CONST_DBL) { tp += 9; continue; }
                if (cur() == TOK_PREFIX_FD || cur() == TOK_PREFIX_FE || cur() == TOK_PREFIX_FF) {
                    tp += 2; continue;
                }
                if (cur() == '"') {
                    advance();
                    while (cur() && cur() != '"') advance();
                    if (cur()) advance();
                    continue;
                }
                if (cur() == ':' || cur() == TOK_ELSE) break;
                if (cur() == '(') depth++;
                else if (cur() == ')') depth--;
                advance();
            }
            if (depth != 0) codegen_error("expected ')' in FN call");
        }
        int len = (int)(tp - start);
        /* Sync variables, embed tokens, call evaluator */
        EMIT("({");
        for (int vi = 0; vi < compiled_var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            EMIT(" gw_var_find_or_create(\"%s\",%d)->val=(gw_value_t){.type=%d,",
                     v->name, v->type, v->type);
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
            EMIT("({double _coord=(double)(%s); int _func=(int)(%s);"
                 " (float)gfx_pmap(_coord,_func);})", arg1, arg2);
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
        /* The first arg is the start position iff it is numeric; a string
         * first arg (literal, var, string function, or string-returning extern)
         * means there is no start and it is the haystack. */
        skip_spaces();
        bool has_start = !peek_is_string_expr();
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
            EMIT("({int _x=(int)(%s); int _y=(int)(%s); gfx_point(_x,_y);})", x, y);
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

    /* Extern (FFI) function call — must be checked before parse_var(), before applying BASIC variable type rules. */
    if (is_letter(tok)) {
        char fname[GW_NAME_SIZE];
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
        char name[GW_NAME_SIZE];
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

            emit_elem_ptr(name, type, ndims, sub_bufs);
            for (int d = 0; d < ndims; d++) free(sub_bufs[d]);
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
    if (!out) codegen_error("cannot allocate expression buffer");
    uint8_t *start = tp;
    fn(arg);
    if (tp <= start) codegen_error("expression made no progress");
    fclose(out);
    out = orig;
    return buf;
}

static void emit_atom_wrapper(int unused) { (void)unused; emit_atom(); }
static void emit_string_wrapper(int unused) { (void)unused; emit_str_expr(); }
static void emit_prec_wrapper(int prec) { emit_num_prec(prec); }

static void emit_num_prec(int min_prec)
{
    gw_valtype_t left_type = peek_expr_type();

    /* Fast path: for simple expressions with no MOD/IDIV/POW/string-cmp,
     * emit directly without buffering via open_memstream */
    skip_spaces();
    bool needs_buffer = false;
    /* Peek ahead: scan for MOD/IDIV/POW operators or string type */
    if (left_type == VT_STR) needs_buffer = true;
    /* Safe mode: force buffered path for integer expressions so we can
     * wrap arithmetic in gw_int_add/sub/mul overflow checks */
    if (safe_mode && left_type == VT_INT) needs_buffer = true;
    /* -O0/-O1: skip the direct fast path so all expressions take the uniform
     * buffered route (semantically identical, easier to correlate with source) */
    if (cg_opt_level < 2) needs_buffer = true;
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
            if (*peek == TOK_INPUT || *peek == TOK_MOD || *peek == TOK_IDIV || *peek == TOK_POW
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
    char *left = emit_to_buf(left_type == VT_STR ? emit_string_wrapper : emit_atom_wrapper, 0);

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
            /* The complete left string (including concatenation and grouping)
             * has already been consumed. Emit each operand exactly once. */
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
                    left, right_str, cmpop);
            fclose(cm);
            free(left); free(right_str);
            left = combined;
            left_type = VT_INT;
            continue;
        }

        gw_valtype_t right_type = peek_expr_type();
        char *right = emit_to_buf(emit_prec_wrapper, prec + 1);

        /* Fold exact, bounded integer literals only. Floating-point rounding,
         * overflow and division errors must not depend on the codegen tier. */
        char *end_l, *end_r;
        double lv = strtod(left, &end_l);
        double rv = strtod(right, &end_r);
        bool const_fold = (cg_opt_level >= 1 && *end_l == '\0' && *end_r == '\0' &&
                           end_l != left && end_r != right &&
                           isfinite(lv) && isfinite(rv) &&
                           lv >= -32768 && lv <= 32767 && trunc(lv) == lv &&
                           rv >= -32768 && rv <= 32767 && trunc(rv) == rv &&
                           !cop && op != TOK_GT && op != TOK_LT && op != TOK_EQ);
        if (const_fold) {
            double result = 0;
            bool folded = true;
            switch (op) {
            case TOK_PLUS:  result = lv + rv; break;
            case TOK_MINUS: result = lv - rv; break;
            case TOK_MUL:   result = lv * rv; break;
            case TOK_MOD:   result = rv != 0 ? (int16_t)lv % (int16_t)rv : 0; folded = (rv != 0); break;
            case TOK_IDIV:  result = rv != 0 ? (int16_t)lv / (int16_t)rv : 0; folded = (rv != 0); break;
            case TOK_AND:   result = (int16_t)lv & (int16_t)rv; break;
            case TOK_OR:    result = (int16_t)lv | (int16_t)rv; break;
            case TOK_XOR:   result = (int16_t)lv ^ (int16_t)rv; break;
            default: folded = false; break;
            }
            if (folded && result >= -32768 && result <= 32767) {
                free(left); free(right);
                char buf[32];
                snprintf(buf, sizeof(buf), "%d", (int)result);
                left = strdup(buf);
                continue;
            }
        }

        char *combined = NULL;
        size_t csz = 0;
        FILE *cm = open_memstream(&combined, &csz);

        /* Stream reads are observable: C does not specify operand order. */
        bool reads_input = strstr(left, "gw_input_chars(") || strstr(right, "gw_input_chars(");
        char *saved_left = left, *saved_right = right;
        if (reads_input) {
            fprintf(cm, "({__auto_type _lhs=(%s); __auto_type _rhs=(%s); ", left, right);
            left = "_lhs"; right = "_rhs";
        }
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
                fprintf(cm, "({double _nv=(double)(%s); double _dv=(double)(%s); if(_dv==0.0) gw_error(11); _nv/_dv;})",
                        left, right);
        } else if (safe_mode && left_type == VT_INT && right_type == VT_INT &&
                   (op == TOK_PLUS || op == TOK_MINUS || op == TOK_MUL)) {
            const char *fn = op == TOK_PLUS ? "gw_int_add"
                           : op == TOK_MINUS ? "gw_int_sub" : "gw_int_mul";
            fprintf(cm, "%s((int16_t)(%s), (int16_t)(%s))", fn, left, right);
        } else {
            fprintf(cm, "(%s %s %s)", left, binop_c(op), right);
        }
        if (reads_input) fprintf(cm, ";})");
        left = saved_left; right = saved_right;
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
    if (ana->runtime_types) {
        EMIT("gwrt_number("); emit_dynamic_expr(); EMIT(")"); return;
    }
    emit_num_prec(0);
}

static void emit_input_byte(void)
{
    if (peek_is_string_expr()) {
        EMIT("({gw_string_t _bad="); emit_str_expr();
        EMIT("; gw_str_free(&_bad); gw_error(ERR_TM); 0;})");
    } else {
        EMIT("gw_input_byte((double)("); emit_num_expr(); EMIT("))");
    }
}

/* Emit a string atom (no concatenation) */
static void emit_str_atom(void)
{
    skip_spaces();
    uint8_t tok = cur();

    if (tok == '(') {
        advance();
        EMIT("(");
        emit_str_expr();
        skip_spaces();
        if (cur() != ')') codegen_error("missing ')' in string expression");
        advance();
        EMIT(")");
        return;
    }

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

    if (tok == TOK_INPUT) {
        advance();
        if (cur() != '$') codegen_error("expected '$' after INPUT");
        advance();
        if (cur() != '(') codegen_error("expected '(' after INPUT$");
        advance();
        EMIT("({int _n="); emit_input_byte(); EMIT("; int _f=-1;");
        if (cur() == ',') {
            advance();
            if (cur() == '#') advance();
            EMIT("_f="); emit_input_byte(); EMIT(";");
        }
        if (cur() != ')') codegen_error("expected ')' after INPUT$");
        advance();
        EMIT("gw_input_chars(_n,_f);})");
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
        case FUNC_LEFT:
        case FUNC_RIGHT: {
            EMIT("({gw_value_t _s={.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ',') advance();
            EMIT("}; int _n="); emit_num_expr();
            if (cur() == ')') advance();
            EMIT("; %s(&_s,_n).sval;})", func == FUNC_LEFT ? "gw_fn_left" : "gw_fn_right");
            return;
        }
        case FUNC_MID: {
            EMIT("({gw_value_t _s={.type=VT_STR,.sval=");
            advance(); emit_str_expr();
            if (cur() == ',') advance();
            EMIT("}; int _start="); emit_num_expr();
            EMIT("; int _len=");
            if (cur() == ',') { advance(); emit_num_expr(); }
            else EMIT("255");
            if (cur() == ')') advance();
            EMIT("; gw_fn_mid(&_s,_start,_len).sval;})");
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
        EMIT("({int _n=gw_cint((double)(%s)); ", arg1);
        if (peek_is_string_expr()) {
            EMIT("gw_string_t _ch="); emit_str_expr();
            EMIT("; if (!_ch.len) gw_error(ERR_FC); int _code=(unsigned char)_ch.data[0];"
                 " gw_str_free(&_ch); gw_fn_strings(_n,_code).sval;})");
        } else {
            EMIT("int _code=gw_cint((double)("); emit_num_expr();
            EMIT(")); gw_fn_strings(_n,_code).sval;})");
        }
        free(arg1);
        if (cur() == ')') advance();
        return;
    }

    /* Extern (FFI) function call returning a string. */
    if (is_letter(tok)) {
        char fname[GW_NAME_SIZE];
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
        char name[GW_NAME_SIZE];
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
            emit_elem_ptr(name, type, ndims, sub_bufs);
            for (int d = 0; d < ndims; d++) free(sub_bufs[d]);
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

    EMIT("gw_str_from_cstr(\"\") /* unknown str expr 0x%02x */", cur());
    if (*tp) tp++;  /* consume the unrecognized token so a looping caller advances */
}

/* Emit a string expression with concatenation (A$ + B$ + C$) */
static void emit_str_expr(void)
{
    if (ana->runtime_types) {
        EMIT("gwrt_string("); emit_dynamic_expr(); EMIT(")"); return;
    }
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
        fprintf(cm, "({gw_value_t _left={.type=VT_STR,.sval=%s};"
                "gw_value_t _right={.type=VT_STR,.sval=%s};"
                "gw_str_concat(&_left,&_right).sval;})",
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
        EMIT("const char *_r = %s(", ef->c_name);
    else
        EMIT("%s _r = %s(", c_ffi_type(ef->ret_type), ef->c_name);
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

/* Classify the result, not just the first atom: (A$) is a string, whereas
 * (A$ = B$) and (A$) = B$ are numeric. Skip token payloads when looking for
 * comparisons; zero bytes and operator bytes inside constants are data. */
static bool peek_is_string_expr(void)
{
    uint8_t *save0 = tp;
    skip_spaces();
    uint8_t tok = cur();
    bool result = false;
    if (tok == '(') {
        advance();
        result = peek_is_string_expr();
    } else if (tok == TOK_INPUT) {
        uint8_t *pk = tp + 1;
        while (*pk == ' ') pk++;
        result = *pk == '$';
    } else if (tok == '"' || tok == TOK_STRINGS) {
        result = true;
    } else if (is_letter(tok)) {
        char fname[GW_NAME_SIZE];
        peek_full_ident(fname, sizeof fname);
        const extern_func_t *ef = analysis_find_extern(ana, fname);
        if (ef) {
            result = (ef->ret_type == VT_STR);
        } else {
            char name[GW_NAME_SIZE];
            result = (parse_var(name) == VT_STR);
        }
    } else if (tok == TOK_PREFIX_FF) {
        uint8_t func = tp[1];
        result = func == FUNC_CHR || func == FUNC_STR || func == FUNC_LEFT
              || func == FUNC_RIGHT || func == FUNC_MID || func == FUNC_SPACE
              || func == FUNC_HEX || func == FUNC_OCT;
    } else if (tok == TOK_PREFIX_FE) {
        uint8_t xtok = tp[1];
        uint8_t *pk = tp + 2;
        while (*pk == ' ') pk++;
        if ((xtok == XSTMT_ENVIRON || xtok == XSTMT_ERDEV || xtok == XSTMT_IOCTL)
            && *pk == '$')
            result = true;
        else if (xtok == XSTMT_DATE || xtok == XSTMT_TIME)
            result = true;
    } else if (tok == TOK_PREFIX_FD) {
        result = tp[1] == XFUNC_MKI || tp[1] == XFUNC_MKS || tp[1] == XFUNC_MKD;
    }
    tp = save0;
    int depth = 0;
    while (result && *tp) {
        uint8_t ch = *tp;
        if (ch == '"') {
            tp++;
            while (*tp && *tp != '"') tp++;
            if (*tp) tp++;
            continue;
        }
        if (ch == TOK_INT1) { tp += 2; continue; }
        if (ch == TOK_INT2) { tp += 3; continue; }
        if (ch == TOK_CONST_SNG) { tp += 5; continue; }
        if (ch == TOK_CONST_DBL) { tp += 9; continue; }
        if (ch == TOK_PREFIX_FD || ch == TOK_PREFIX_FE || ch == TOK_PREFIX_FF) {
            tp += 2; continue;
        }
        if (ch == '(') depth++;
        else if (ch == ')') { if (depth == 0) break; depth--; }
        else if (depth == 0) {
            if (ch == ':' || ch == ',' || ch == ';' || ch == TOK_THEN
                || ch == TOK_ELSE || ch == TOK_TO || ch == TOK_STEP) break;
            if (ch == TOK_LT || ch == TOK_EQ || ch == TOK_GT) result = false;
        }
        tp++;
    }
    tp = save0;
    return result;
}

static gw_valtype_t peek_expr_type(void)
{
    uint8_t *save = tp;
    skip_spaces();
    uint8_t tok = cur();

    if (tok == '"' || tok == TOK_STRINGS || tok == TOK_INPUT) { tp = save; return VT_STR; }

    /* Variable — check suffix (most important case) */
    if (is_letter(tok)) {
        char fname[GW_NAME_SIZE];
        peek_full_ident(fname, sizeof fname);
        const extern_func_t *ef = analysis_find_extern(ana, fname);
        if (ef) { tp = save; return ef->ret_type; }
        char name[GW_NAME_SIZE];
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
    if (tok == '(') {
        advance();
        gw_valtype_t t = peek_expr_type();
        if (t == VT_STR && !peek_is_string_expr()) t = VT_INT;
        tp = save;
        return t;
    }
    /* Functions */
    if (tok == TOK_PREFIX_FF) {
        uint8_t func = tp[1];
        tp = save;
        switch (func) {
        case FUNC_CHR: case FUNC_STR: case FUNC_LEFT: case FUNC_RIGHT:
        case FUNC_MID: case FUNC_SPACE: case FUNC_HEX: case FUNC_OCT:
            return VT_STR;
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
        if (xf == XFUNC_MKI || xf == XFUNC_MKS || xf == XFUNC_MKD) return VT_STR;
        return VT_SNG;
    }
    if (tok == TOK_PREFIX_FE && peek_is_string_expr()) { tp = save; return VT_STR; }
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

static void skip_statement_tail(void)
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
        if (*tp == TOK_PREFIX_FD || *tp == TOK_PREFIX_FE || *tp == TOK_PREFIX_FF) {
            tp += 2; continue;
        }
        if (*tp == ':' || *tp == TOK_ELSE || *tp == 0) break;
        tp++;
    }
}

static void emit_delegate_stmt_ex(uint8_t *stmt_start, bool read_back, bool sync_all)
{
    skip_statement_tail();
    int slen = (int)(tp - stmt_start);

    EMIT("  {\n");
    /* Variable sync: selective (scan tokens for referenced names) or full */
    for (int vi = 0; vi < compiled_var_count; vi++) {
        var_info_t *v = &ana->vars[vi];
        /* A delegate that reads every variable back must first receive every
         * current value, including variables absent from the statement. */
        if (!sync_all && !read_back) {
            bool found = false;
            uint8_t *sc = stmt_start;
            while (sc < tp) {
                if (*sc == '"') { sc++; while (sc < tp && *sc != '"') sc++; if (sc < tp) sc++; continue; }
                if (*sc == TOK_CONST_SNG) { sc += 5; continue; }
                if (*sc == TOK_CONST_DBL) { sc += 9; continue; }
                if (is_letter(*sc)) {
                    char candidate[GW_NAME_SIZE];
                    bool valid = gw_scan_name(&sc, candidate);
                    if (valid && strcmp(candidate, v->name) == 0) {
                        found = true; break;
                    }
                    continue;
                }
                if (*sc == TOK_INT1) sc += 2;
                else if (*sc == TOK_INT2) sc += 3;
                else if (*sc == TOK_PREFIX_FD || *sc == TOK_PREFIX_FE || *sc == TOK_PREFIX_FF) sc += 2;
                else sc++;

            }
            if (!found) continue;
        }
        EMIT("    gw_var_find_or_create(\"%s\", %d)->val = ", v->name, v->type);
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
        for (int vi = 0; vi < compiled_var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            EMIT("    ");
            emit_varname(v->name, v->type);
            EMIT(" = gw_var_find_or_create(\"%s\", %d)->val.", v->name, v->type);
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
        if (!cur() || cur() == ':' || cur() == TOK_ELSE) break;

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

        /* Detect string vs numeric expression (string literal/var/function or
         * a string-returning '$EXTERN call). */
        bool is_str = peek_is_string_expr();

        if (ana->runtime_types) {
            EMIT("  {gw_value_t _pv="); emit_dynamic_expr();
            EMIT(";gw_print_value(&_pv); if(_pv.type==VT_STR)gw_str_free(&_pv.sval);}\n");
        } else if (is_str) {
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
    if (ana->runtime_types) { emit_dynamic_assignment(); return; }
    char name[GW_NAME_SIZE];
    gw_valtype_t type = parse_var(name);
    skip_spaces();

    /* Array element assignment: A(i) = expr */
    if (cur() == '(') {
        advance(); /* skip ( */
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
        skip_spaces();
        if (cur() == TOK_EQ) advance();

        EMIT("  { gw_value_t *_elem = ");
        emit_elem_ptr(name, type, ndims, sub_bufs);
        EMIT(";\n");
        for (int d = 0; d < ndims; d++) free(sub_bufs[d]);

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
        EMIT("  gw_error(ERR_SN);\n");
        skip_statement_tail();
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
    if (ana->runtime_types && emit_dynamic_statement(tok)) return;

    /* REM */
    if (tok == TOK_REM || tok == TOK_SQUOTE) {
        while (*tp) tp++;
        return;
    }

    /* PRINT */
    if (tok == TOK_PRINT || tok == '?') {
        /* TAB(/SPC( include their opening parenthesis in the token. Use the
         * runtime printer for these position-sensitive statements. */
        uint8_t *scan = tp + 1;
        while (*scan && *scan != ':' && *scan != TOK_ELSE) {
            if (*scan == '"') {
                scan++;
                while (*scan && *scan != '"') scan++;
                if (*scan) scan++;
            } else if (*scan == TOK_INT1) scan += 2;
            else if (*scan == TOK_INT2) scan += 3;
            else if (*scan == TOK_CONST_SNG) scan += 5;
            else if (*scan == TOK_CONST_DBL) scan += 9;
            else if (*scan == TOK_PREFIX_FD || *scan == TOK_PREFIX_FE || *scan == TOK_PREFIX_FF) scan += 2;
            else if (*scan == TOK_TAB || *scan == TOK_SPC) {
                emit_delegate_stmt(tp, false);
                return;
            } else scan++;
        }
        uint8_t *print_tok = tp;  /* save position of PRINT token */
        advance();
        skip_spaces();
        /* PRINT USING — embed token bytes and call runtime */
        if (cur() == TOK_USING) {
            if (ana->runtime_types) { emit_delegate_stmt(print_tok,false); return; }
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
                    if (*tp == TOK_PREFIX_FD || *tp == TOK_PREFIX_FE || *tp == TOK_PREFIX_FF) {
                        tp += 2; continue;
                    }
                    if (*tp == ':' || *tp == TOK_ELSE || *tp == 0) break;
                    tp++;
                }
            }
            int len = (int)(tp - start);
            /* Sync compiled variables to interpreter table so gw_eval works */
            EMIT("  {\n");
            for (int vi = 0; vi < compiled_var_count; vi++) {
                var_info_t *v = &ana->vars[vi];
                EMIT("    gw_var_find_or_create(\"%s\", %d)->val = ", v->name, v->type);
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
        if (!line_exists(target))
            EMIT("  gw_error(ERR_UL);\n");
        else if (safe_mode)
            EMIT("  gwrt_gosub_push_safe(%d, %u); goto L_%u;\n", rl, emit_line, target);
        else
            EMIT("  gwrt_gosub_push(%d); goto L_%u;\n", rl, target);
        EMIT("ret_%d: ;\n", rl);
        return;
    }

    /* RETURN targets can be defined by GOSUB sites later in the source. */
    if (tok == TOK_RETURN) {
        advance();
        EMIT("  _gw_return = gwrt_gosub_pop(); goto _gw_return_dispatch;\n");
        return;
    }

    /* FOR records the executed loop, including its continuation label. */
    if (tok == TOK_FOR) {
        advance();
        char name[GW_NAME_SIZE];
        uint8_t *for_name = tp;
        gw_valtype_t type = parse_var(name);
        bool for_explicit_type = parsed_explicit_type(for_name);
        if (cur() != TOK_EQ) {
            EMIT("  gw_error(ERR_SN);\n");
            skip_statement_tail();
            return;
        }
        if (type == VT_STR) codegen_error("FOR requires a numeric variable");
        program_line_t *line = gw.prog_head, *target = NULL;
        uint8_t *after = NULL, *next_var;
        while (line && line->num != emit_line) line = line->next;
        bool matched = gw_for_exit(line, tp, &target, &after, &next_var);
        EMIT("  { int _entry_type = ");
        if (for_explicit_type) EMIT("%d", type);
        else EMIT("gw.def_type[%d]", name[0] - 'A');
        EMIT(";\n");
        skip_spaces();
        if (cur() != TOK_EQ) codegen_error("expected '=' in FOR");
        advance();
        EMIT("    %s _initial = %s(", c_type(type), type == VT_INT ? "gw_cint" : type == VT_SNG ? "(float)" : "(double)");
        emit_num_expr();
        EMIT(");\n");
        skip_spaces();
        if (cur() != TOK_TO) codegen_error("expected TO in FOR");
        advance();
        EMIT("    %s _limit = %s(", c_type(type), type == VT_INT ? "gw_cint" : type == VT_SNG ? "(float)" : "(double)");
        emit_num_expr();
        EMIT(");\n    %s _step = ", c_type(type));
        skip_spaces();
        if (cur() == TOK_STEP) {
            advance();
            EMIT("%s(", type == VT_INT ? "gw_cint" : type == VT_SNG ? "(float)" : "(double)");
            emit_num_expr();
            EMIT(")");
        } else {
            EMIT("1");
        }
        EMIT(";\n");
        if (!matched) {
            EMIT("    gw_error(ERR_FW);\n");
        } else {
            uint8_t *save = tp;
            tp = next_var;
            if (is_letter(cur())) {
                char next_name[GW_NAME_SIZE];
                uint8_t *next_start = tp;
                gw_valtype_t next_type = parse_var(next_name);
                bool explicit_type = parsed_explicit_type(next_start);
                EMIT("    if (%d || ", strcmp(name, next_name) != 0 || *after == '(');
                if (explicit_type) EMIT("%d", next_type);
                else EMIT("gw.def_type[%d]", next_name[0] - 'A');
                EMIT(" != _entry_type) { gw.cur_line_num = %u; gw_error(ERR_NF); }\n", target->num);
            }
            tp = save;
        }
        EMIT("    ");
        emit_varname(name, type);
        EMIT(" = _initial;\n");
        int fc = for_label_counter++;
        EMIT("    if (!gwrt_for_push(&");
        emit_varname(name, type);
        EMIT(", %d, _limit, _step, %d)) ", type, fc);
        if (matched) {
            EMIT("{ gw.cur_line_num = %u; goto for_exit_%u_%d; }",
                 target->num, target->num, (int)(after - target->tokens));
        } else {
            EMIT("gw_error(ERR_FW);");
        }
        EMIT(" }\n");
        EMIT("for_body_%d: ;\n", fc);
        return;
    }

    /* NEXT J,I executes NEXT I only when J has finished. */
    if (tok == TOK_NEXT) {
        advance();
        do {
            skip_spaces();
            EMIT("  _gw_loop = gwrt_for_next(");
            if (is_letter(cur())) {
                char name[GW_NAME_SIZE];
                gw_valtype_t type = parse_var(name);
                if (ana->runtime_types) EMIT("&gwrt_var(\"%s\",%d)->ival",name,type);
                else { EMIT("&"); emit_varname(name, type); }
            } else {
                EMIT("NULL");
            }
            EMIT(");\n  if (_gw_loop >= 0) goto _gw_for_dispatch;\n");
            skip_spaces();
            program_line_t *line = gw.prog_head;
            while (line && line->num != emit_line) line = line->next;
            EMIT("for_exit_%u_%d: ;\n", emit_line, (int)(tp - line->tokens));
        } while (cur() == ',' && (advance(), 1));
        if (cur() && cur() != ':' && cur() != TOK_ELSE) {
            EMIT("  gw_error(ERR_SN);\n");
            skip_statement_tail();
        }
        return;
    }

    /* WHILE/WEND use a separate stack so FORs cannot disturb their pairing. */
    if (tok == TOK_WHILE) {
        if (while_stack_sp >= WHILE_STACK_MAX) codegen_error("WHILE nesting too deep");
        int wl = while_label_counter++;
        while_stack[while_stack_sp++] = wl;
        EMIT("while_top_%d: ;\n", wl);
        if (ana->has_resume) EMIT("  _gw_stmt = %d;\n", active_statement);
        EMIT("  gwrt_while_drop(%d, false);\nwhile_test_%d: ;\n", wl, wl);
        EMIT("  gw.cur_line_num = %u;\n", emit_line);
        advance();
        EMIT("  if (!(");
        emit_num_expr();
        EMIT(")) goto while_done_%d;\n", wl);
        EMIT("  gwrt_while_push(%d);\n", wl);
        return;
    }
    if (tok == TOK_WEND) {
        advance();
        skip_spaces();
        bool bad_tail = cur() && cur() != ':' && cur() != TOK_ELSE && cur() != TOK_SQUOTE;
        if (bad_tail) {
            EMIT("  gw_error(ERR_SN);\n");
            skip_statement_tail();
        }
        if (!while_stack_sp) codegen_error("WEND without WHILE");
        int wl = while_stack[--while_stack_sp];
        EMIT("  gwrt_while_drop(%d, true);\n", wl);
        EMIT("  goto while_test_%d;\nwhile_done_%d: ;\n", wl, wl);
        if (bad_tail) {
            /* An initially false WHILE also reaches the text after WEND. */
            EMIT("  gw.cur_line_num = %u;\n", emit_line);
            if (ana->has_resume) EMIT("  _gw_stmt = %d;\n", active_statement);
            EMIT("  gw_error(ERR_SN);\n");
        }
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
                if (!line_exists(target))
                    EMIT("      case %d: gw_error(ERR_UL); break;\n", n++);
                else if (safe_mode)
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
            char name[GW_NAME_SIZE];
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
            array_info_t *ai = inline_arrays ? analysis_inlinable_array(ana, name, type) : NULL;
            EMIT("  ");
            if (ai && !safe_mode) { emit_arr_sym(name, type); EMIT(" = "); }
            EMIT("gwrt_dim("); emit_name_str(name);
            EMIT(", %d, %d, (int[]){", type, ndims);
            for (int d = 0; d < ndims; d++) {
                if (d > 0) EMIT(", ");
                EMIT("gw_cint((double)(%s))", dim_bufs[d]);
                free(dim_bufs[d]);
            }
            EMIT("});\n");
            if (ai && !safe_mode) {
                EMIT("  "); emit_arr_sym(name, type); EMIT("_generation = gw_array_generation;\n");
                EMIT("  "); emit_arr_sym(name, type); EMIT("_base = gw.option_base;\n");
            }
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
        char sw_names[2][GW_NAME_SIZE];
        gw_valtype_t sw_types[2];
        bool sw_is_scalar[2] = {false, false};
        for (int si = 0; si < 2; si++) {
            skip_spaces();
            if (si == 1 && cur() == ',') advance();
            skip_spaces();
            char name[GW_NAME_SIZE];
            gw_valtype_t type = parse_var(name);
            strcpy(sw_names[si], name);
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
                EMIT("    gw_value_t *_sw%d = ", si);
                emit_elem_ptr(name, type, nd, sbufs);
                EMIT(";\n");
                for (int d = 0; d < nd; d++) free(sbufs[d]);
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
        uint8_t *def_start = tp;
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
            /* Use the token-aware boundary scanner: constants may contain
             * zero bytes, and DEF/FN need not have exactly one space. */
            emit_delegate_stmt(def_start, false);
        } else {
            while (cur() && cur() != ':' && cur() != 0) tp++;
        }
        return;
    }

    /* RANDOMIZE */
    if (tok == TOK_RANDOMIZE) {
        advance();
        skip_spaces();
        if (cur() && cur() != ':' && cur() != TOK_ELSE && cur() != TOK_SQUOTE && cur() != 0) {
            EMIT("  gw_randomize(gw_cint(");
            emit_num_expr();
            EMIT("));\n");
        } else {
            EMIT("  gw_randomize_prompt();\n");
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
        EMIT("  if (gw_hal) gw_hal->locate(_row, _col); }\n");
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

    /* Port statements must consume whole expressions, including encoded
     * constants, and share the interpreter's simulated device state. */
    if (tok == TOK_OUT || tok == TOK_WAIT || tok == TOK_MOTOR) {
        uint8_t *start = tp;
        advance();
        emit_delegate_stmt(start, false);
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
        EMIT("  if (!gw.in_error_handler) gw_error(ERR_RW);\n");
        EMIT("  gw_errno = 0;\n");
        bool next = cur() == TOK_NEXT;
        uint16_t target = 0;
        if (next) {
            advance();
        } else if (is_const(cur())) {
            target = read_int();
        }
        skip_spaces();
        if (cur() && cur() != ':' && cur() != TOK_ELSE) {
            EMIT("  gw_error(ERR_SN);\n");
            skip_statement_tail();
            return;
        }
        if (next) {
            EMIT("  gw.in_error_handler = false;\n");
            EMIT("  goto _gw_resume_next;\n");
        } else {
            if (target && !line_exists(target)) {
                /* Keep the handler active until the target is validated. */
                EMIT("  gw_error(ERR_UL);\n");
            } else {
                EMIT("  gw.in_error_handler = false;\n");
                if (target) EMIT("  goto L_%u;\n", target);
                else EMIT("  goto _gw_resume_retry;\n");
            }
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

    /* Array lifetime and variable reset must use the shared runtime state. */
    if (tok == TOK_ERASE || tok == TOK_CLEAR) {
        emit_delegate_stmt(tp, tok == TOK_CLEAR);
        if (tok == TOK_CLEAR) EMIT("  gwrt_control_clear();\n");
        return;
    }

    /* TRON/TROFF */
    if (tok == TOK_TRON) { advance(); return; }
    if (tok == TOK_TROFF) { advance(); return; }

    /* OPTION BASE must retain validation and the no-existing-arrays rule. */
    if (tok == TOK_OPTION) {
        emit_delegate_stmt(tp, false);
        return;
    }

    /* Retain executed default types for delegated code and empty-loop checks. */
    if (tok == TOK_DEFINT || tok == TOK_DEFSNG ||
        tok == TOK_DEFDBL || tok == TOK_DEFSTR) {
        emit_delegate_stmt(tp, false);
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
            if (peek_is_string_expr()) {
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
            skip_spaces();
        } else {
            /* THEN followed by statements */
            bool new_statement = false;
            for (;;) {
                skip_spaces();
                if (!cur() || cur() == TOK_ELSE) break;
                if (cur() == ':') { advance(); new_statement = true; continue; }
                emit_stmt_guarded(new_statement);
            }
        }
        if (cur() == TOK_ELSE) {
            advance();
            EMIT("  } else {\n");
            skip_spaces();
            if (is_const(cur())) {
                uint16_t target = read_int();
                EMIT("    goto L_%u;\n", target);
                skip_spaces();
            } else {
                /* A further ELSE belongs to an enclosing IF. */
                bool new_statement = false;
                for (;;) {
                    skip_spaces();
                    if (!cur() || cur() == TOK_ELSE) break;
                    if (cur() == ':') { advance(); new_statement = true; continue; }
                    emit_stmt_guarded(new_statement);
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
                char name[GW_NAME_SIZE];
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
                    EMIT("  { gw_value_t *_re = ");
                    emit_elem_ptr(name, type, nd, sbufs);
                    EMIT(";\n");
                    for (int d = 0; d < nd; d++) free(sbufs[d]);
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
        if (ana->runtime_types) { emit_delegate_stmt(tp,true); return; }
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
        for (int vi = 0; vi < compiled_var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            EMIT("    gw_var_find_or_create(\"%s\", %d)->val = ", v->name, v->type);
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
        for (int vi = 0; vi < compiled_var_count; vi++) {
            var_info_t *v = &ana->vars[vi];
            if (v->type != VT_STR) continue;
            EMIT("    ");
            emit_varname(v->name, v->type);
            EMIT(" = gw_var_find_or_create(\"%s\", %d)->val.sval;\n", v->name, v->type);
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

/* Fail on a stalled statement. Silently dropping a token would produce a
 * successful compilation with changed program behavior. Inner emitters are
 * additionally bounded by the output and scratch-buffer size limits. */
static void emit_stmt_guarded(bool new_statement)
{
    skip_spaces();
    uint8_t *start = tp;
    int parent_statement = active_statement;
    if (new_statement && ana->has_resume) {
        if (resume_count == resume_capacity) {
            int capacity = resume_capacity ? resume_capacity * 2 : 64;
            resume_point_t *points = realloc(resume_points, capacity * sizeof(*points));
            if (!points) codegen_error("cannot allocate statement continuations");
            resume_points = points;
            resume_capacity = capacity;
        }
        active_statement = resume_count++;
        resume_points[active_statement] = (resume_point_t){emit_source_line, start};
        EMIT("stmt_%d: ;\n  _gw_stmt = %d; gw.cur_line_num = %u;\n",
             active_statement, active_statement, emit_line);
    }
    emit_stmt();
    active_statement = parent_statement;
    if (tp <= start && *tp) codegen_error("statement made no progress");
}

/* ---- Main code generation ---- */

#include "codegen_dynamic.inc"

void codegen_emit(FILE *f, analysis_t *a, const codegen_opts_t *opts)
{
    out = f;
    g_real_out = f;
    g_out_path = opts ? opts->out_path : NULL;
    g_emit_cap = opts ? opts->max_output_bytes : 0;
    g_emit_bytes = 0;
    ana = a;
    compiled_var_count = a->runtime_types ? 0 : a->var_count;
    safe_mode = opts ? opts->safe_mode : false;
    no_gc_check = opts ? opts->no_gc_check : false;
    inline_arrays = opts && !a->runtime_types ? opts->inline_arrays : false;
    cg_opt_level = opts ? opts->opt_level : 2;
    fast_math = opts ? opts->fast_math : false;
    main_name = (opts && opts->main_name) ? opts->main_name : "main";
    ret_label_counter = 0;
    for_label_counter = 0;
    while_stack_sp = 0;
    while_label_counter = 0;
    free(resume_points);
    resume_points = NULL;
    resume_count = resume_capacity = 0;
    active_statement = -1;

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
        EMIT("extern %s %s(", c_ffi_type(e->ret_type), e->c_name);
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

    if (a->runtime_types) emit_dynamic_ffi_wrappers();

    /* Variable declarations */
    for (int i = 0; i < compiled_var_count; i++) {
        EMIT("static %s ", c_type(a->vars[i].type));
        emit_varname(a->vars[i].name, a->vars[i].type);
        if (a->vars[i].type == VT_STR)
            EMIT(" = {0, NULL}");
        else
            EMIT(" = 0");
        EMIT(";\n");
    }

    /* Cache only the shared allocation; no private copy of array values. */
    if (inline_arrays && !safe_mode) {
        for (int i = 0; i < a->array_count; i++) {
            array_info_t *ai = &a->arrays[i];
            if (!ai->inlinable || !ai->dimmed) continue;
            EMIT("static gw_value_t *"); emit_arr_sym(ai->name, ai->type); EMIT(";\n");
            EMIT("static unsigned long "); emit_arr_sym(ai->name, ai->type); EMIT("_generation;\n");
            EMIT("static int "); emit_arr_sym(ai->name, ai->type); EMIT("_base;\n");
        }
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
    EMIT("  int _gw_loop, _gw_return;\n");
    /* These locals change between setjmp and longjmp. */
    if (a->has_resume)
        EMIT("  volatile int _gw_stmt = -1, _gw_error_stmt = -1;\n");
    EMIT("  gwrt_init();\n");
    if (a->runtime_types) {
        EMIT("  if(gw_hal)gw_hal->enable_raw();\n");
        if(a->extern_count) EMIT("  gw.extern_eval=_gw_foreign_eval;\n");
    }
    if (inline_arrays && !safe_mode) {
        for (int i = 0; i < a->array_count; i++) {
            array_info_t *ai = &a->arrays[i];
            if (!ai->inlinable || !ai->dimmed) continue;
            EMIT("  "); emit_arr_sym(ai->name, ai->type); EMIT(" = NULL;\n");
        }
    }
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
    if(a->runtime_types) EMIT("    gwrt_expr_unwind();\n");
    if (a->has_resume) EMIT("    _gw_error_stmt = _gw_stmt;\n");
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
        emit_source_line = line;
        EMIT("L_%u:\n", line->num);

        /* Skip GC/break check for REM-only lines, and for the entire
         * program under --no-gc-check (string pool will only compact when
         * a heap-pressure threshold is hit, and Ctrl+Break is ignored). */
        bool is_rem = (line->tokens[0] == TOK_REM || line->tokens[0] == TOK_SQUOTE);
        if (!is_rem && !no_gc_check)
            EMIT("  gwrt_check_line(%u);\n", line->num);
        else
            EMIT("  gw.cur_line_num = %u;\n", line->num);

        /* Walk statements on this line */
        tp = line->tokens;
        while (*tp) {
            skip_spaces();
            if (*tp == ':') { tp++; continue; }
            if (*tp == 0) break;

            uint8_t first = *tp;
            emit_stmt_guarded(true);
            /* A failing transfer can RESUME NEXT into the remaining text. */
            if (!a->has_resume && cg_opt_level >= 1 && for_label_counter == 0 &&
                (first == TOK_GOTO || first == TOK_END || first == TOK_STOP))
                break;
        }
        if (a->has_resume) EMIT("line_end_%u: ;\n", line->num);
    }

    /* Implicit END at bottom */
    EMIT("  gwrt_shutdown();\n");
    EMIT("  return 0;\n");

    if (a->has_resume) {
        EMIT("_gw_resume_retry:\n  switch (_gw_error_stmt) {\n");
        for (int i = 0; i < resume_count; i++)
            EMIT("    case %d: goto stmt_%d;\n", i, i);
        EMIT("  }\n  gw_error(ERR_RW); return 1;\n");
        EMIT("_gw_resume_next:\n  switch (_gw_error_stmt) {\n");
        for (int i = 0; i < resume_count; i++) {
            resume_point_t point = resume_points[i];
            uint8_t *next = gw_statement_end(point.line, point.text);
            if (!next) codegen_error("truncated token in statement continuation");
            while (*next == ':' || *next == ' ') next++;
            if (!*next || *next == TOK_ELSE) {
                EMIT("    case %d: goto line_end_%u;\n", i, point.line->num);
            } else {
                int target;
                for (target = i + 1; target < resume_count; target++)
                    if (resume_points[target].line == point.line && resume_points[target].text == next)
                        break;
                if (target == resume_count) codegen_error("missing statement continuation");
                EMIT("    case %d: goto stmt_%d;\n", i, target);
            }
        }
        EMIT("  }\n  gw_error(ERR_RW); return 1;\n");
    }
    free(resume_points);
    resume_points = NULL;
    resume_count = resume_capacity = 0;

    /* The complete target sets are known only after every line was emitted. */
    EMIT("_gw_for_dispatch:\n  switch (_gw_loop) {\n");
    for (int i = 0; i < for_label_counter; i++)
        EMIT("    case %d: goto for_body_%d;\n", i, i);
    EMIT("  }\n  gw_error(ERR_NF); return 1;\n");
    EMIT("_gw_return_dispatch:\n  switch (_gw_return) {\n");
    for (int i = 0; i < ret_label_counter; i++)
        EMIT("    case %d: goto ret_%d;\n", i, i);
    EMIT("  }\n  gw_error(ERR_RG); return 1;\n");
    if (while_stack_sp) codegen_error("WHILE without WEND");

    /* Undefined-line-number targets: a GOTO/GOSUB/THEN/etc. aimed at a line that
     * does not exist would otherwise leave a dangling `goto L_n` with no label
     * (a C compile error).  Real GW-BASIC raises "Undefined line number"
     * (error 8) at run time; emit a stub label that does the same.  Placed past
     * the implicit `return 0` so they are reached only by an explicit branch,
     * never by fall-through.  gw_error() longjmps, so the trailing return is
     * unreachable — it only keeps the C well-formed. */
    {
        int undefined = 0;
        for (int gi = 0; gi < a->goto_count; gi++) {
            uint16_t tgt = a->goto_targets[gi];
            bool defined = false;
            for (int li = 0; li < a->line_count; li++)
                if (a->lines[li].line_num == tgt) { defined = true; break; }
            if (!defined) {
                EMIT("  L_%u: gw_error(8); /* undefined line number */\n", tgt);
                undefined++;
            }
        }
        if (undefined) EMIT("  return 1;\n");
    }
    EMIT("}\n");
}
