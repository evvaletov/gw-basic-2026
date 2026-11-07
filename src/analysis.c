/*
 * Analysis pass for the GW-BASIC compiler.
 *
 * Walks the tokenized program to collect:
 *   - Line number table (which lines exist, which are jump targets)
 *   - Variable census (all variable names and types)
 *   - DATA literal pool (all DATA statement contents)
 *   - GOTO/GOSUB target set
 */

#include "analysis.h"
#include "gwbasic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Read an encoded integer from the token stream, advance *pp */
static uint16_t read_encoded_int(uint8_t **pp)
{
    uint8_t *p = *pp;
    uint16_t val = 0;
    if (*p >= 0x11 && *p <= 0x1A) {
        val = *p - 0x11;
        *pp = p + 1;
    } else if (*p == TOK_INT1) {
        val = p[1];
        *pp = p + 2;
    } else if (*p == TOK_INT2) {
        val = (uint16_t)(p[1] | (p[2] << 8));
        *pp = p + 3;
    }
    return val;
}

/* Skip an encoded constant, advance *pp */
static void skip_constant(uint8_t **pp)
{
    uint8_t *p = *pp;
    if (*p >= 0x11 && *p <= 0x1A) { *pp = p + 1; }
    else if (*p == TOK_INT1) { *pp = p + 2; }
    else if (*p == TOK_INT2) { *pp = p + 3; }
    else if (*p == TOK_CONST_SNG) { *pp = p + 5; }
    else if (*p == TOK_CONST_DBL) { *pp = p + 9; }
}

static bool is_constant(uint8_t tok)
{
    return (tok >= 0x11 && tok <= 0x1A) || tok == TOK_INT1 || tok == TOK_INT2
        || tok == TOK_CONST_SNG || tok == TOK_CONST_DBL;
}

static bool is_letter(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/* ---- '$EXTERN FFI pragma support ---- */

/* Case-insensitive match of keyword kw at the start of s. */
static bool ci_starts(const char *s, const char *kw)
{
    for (; *kw; s++, kw++)
        if (toupper((unsigned char)*s) != toupper((unsigned char)*kw))
            return false;
    return true;
}

/* Read an identifier-like word from *pp (letters only), uppercase into buf. */
static int read_word(const char **pp, char *buf, int max)
{
    const char *p = *pp;
    int i = 0;
    while (*p == ' ') p++;
    while (is_letter((uint8_t)*p) && i < max - 1)
        buf[i++] = (char)toupper((unsigned char)*p++);
    while (is_letter((uint8_t)*p)) p++;  /* drain overflow so *pp lands past the word */
    buf[i] = 0;
    *pp = p;
    return i;
}

/* Map a BASIC type keyword to a value type, or (gw_valtype_t)0 if unknown. */
static gw_valtype_t parse_type_word(const char *w)
{
    if (!strcmp(w, "INTEGER") || !strcmp(w, "INT"))    return VT_INT;
    if (!strcmp(w, "SINGLE"))                          return VT_SNG;
    if (!strcmp(w, "DOUBLE"))                          return VT_DBL;
    if (!strcmp(w, "STRING") || !strcmp(w, "STR"))     return VT_STR;
    return (gw_valtype_t)0;
}

/* Parse a '$EXTERN NAME(T1, T2, ...) AS RET pragma body (the raw comment
 * text, starting at "$EXTERN") and register the function. */
static void parse_extern_pragma(analysis_t *a, const char *text)
{
    if (a->extern_count >= MAX_EXTERNS)
        return;
    const char *p = text + 7;  /* skip "$EXTERN" */
    extern_func_t ef;
    memset(&ef, 0, sizeof(ef));
    ef.ret_type = VT_SNG;  /* default if no AS clause */

    /* Function name (case-preserving).  Restricted to BASIC-legal identifier
     * characters (letters and digits) because the call site is tokenized as
     * ordinary BASIC; a C symbol with other characters needs a thin wrapper. */
    while (*p == ' ') p++;
    int i = 0;
    while ((is_letter((uint8_t)*p) || (*p >= '0' && *p <= '9'))
           && i < EXTERN_NAME_MAX - 1)
        ef.name[i++] = *p++;
    while (is_letter((uint8_t)*p) || (*p >= '0' && *p <= '9'))
        p++;  /* drain overflow so the arg/return parse below stays in sync */
    ef.name[i] = 0;
    if (i == 0 || !is_letter((uint8_t)ef.name[0]))
        return;  /* BASIC identifiers must start with a letter */

    /* Optional argument list. */
    while (*p == ' ') p++;
    if (*p == '(') {
        p++;
        while (*p == ' ') p++;
        if (*p != ')') {
            do {
                if (*p == ',') p++;
                char w[16];
                read_word(&p, w, sizeof(w));
                gw_valtype_t t = parse_type_word(w);
                if (t && ef.argc < MAX_EXTERN_ARGS)
                    ef.arg_types[ef.argc++] = t;
                while (*p == ' ') p++;
            } while (*p == ',');
        }
        if (*p == ')') p++;
    }

    /* Optional "AS RET" clause. */
    while (*p == ' ') p++;
    if (ci_starts(p, "AS")) {
        p += 2;
        char w[16];
        read_word(&p, w, sizeof(w));
        gw_valtype_t t = parse_type_word(w);
        if (t) ef.ret_type = t;
    }

    /* Optional "= c_symbol" alias: the emitted C symbol may differ from the
     * BASIC-legal call name (e.g. it can contain underscores). */
    while (*p == ' ') p++;
    if (*p == '=') {
        p++;
        while (*p == ' ') p++;
        /* A C identifier starts with a letter or underscore; if the alias is
         * malformed (e.g. digit-first), leave c_name empty so it falls back to
         * the BASIC name below rather than emitting an invalid C symbol. */
        if (is_letter((uint8_t)*p) || *p == '_') {
            int j = 0;
            while ((is_letter((uint8_t)*p) || (*p >= '0' && *p <= '9') || *p == '_')
                   && j < EXTERN_NAME_MAX - 1)
                ef.c_name[j++] = *p++;
            ef.c_name[j] = 0;
        }
    }
    if (ef.c_name[0] == 0)
        strcpy(ef.c_name, ef.name);  /* no alias: C symbol == BASIC name */

    a->externs[a->extern_count++] = ef;
}

const extern_func_t *analysis_find_extern(analysis_t *a, const char *name)
{
    for (int i = 0; i < a->extern_count; i++) {
        const char *s = a->externs[i].name, *q = name;
        while (*s && *q &&
               toupper((unsigned char)*s) == toupper((unsigned char)*q)) {
            s++; q++;
        }
        if (*s == 0 && *q == 0)
            return &a->externs[i];
    }
    return NULL;
}

/* Add a GOTO/GOSUB target line number */
static void add_target(analysis_t *a, uint16_t line_num)
{
    for (int i = 0; i < a->goto_count; i++)
        if (a->goto_targets[i] == line_num) return;
    if (a->goto_count < MAX_GOTOS)
        a->goto_targets[a->goto_count++] = line_num;
}

/* Mark a line as a jump target */
static void mark_target(analysis_t *a, uint16_t line_num)
{
    add_target(a, line_num);
    for (int i = 0; i < a->line_count; i++) {
        if (a->lines[i].line_num == line_num) {
            a->lines[i].is_target = true;
            return;
        }
    }
}

int analysis_find_var(analysis_t *a, const char name[2], gw_valtype_t type)
{
    for (int i = 0; i < a->var_count; i++) {
        if (a->vars[i].name[0] == name[0] && a->vars[i].name[1] == name[1]
            && a->vars[i].type == type)
            return i;
    }
    return -1;
}

int analysis_add_var(analysis_t *a, const char name[2], gw_valtype_t type)
{
    int idx = analysis_find_var(a, name, type);
    if (idx >= 0) return idx;
    if (a->var_count >= MAX_VARS) return -1;
    idx = a->var_count++;
    a->vars[idx].name[0] = name[0];
    a->vars[idx].name[1] = name[1];
    a->vars[idx].type = type;
    a->vars[idx].first_assign_line = 0;
    a->vars[idx].first_use_line = 0;
    return idx;
}

static void mark_var_assign(analysis_t *a, const char name[2],
                             gw_valtype_t type, uint16_t line)
{
    int idx = analysis_add_var(a, name, type);
    if (idx >= 0 && a->vars[idx].first_assign_line == 0)
        a->vars[idx].first_assign_line = line;
}

static void mark_var_use(analysis_t *a, const char name[2],
                          gw_valtype_t type, uint16_t line)
{
    int idx = analysis_add_var(a, name, type);
    if (idx >= 0 && a->vars[idx].first_use_line == 0)
        a->vars[idx].first_use_line = line;
}

bool analysis_is_target(analysis_t *a, uint16_t line_num)
{
    for (int i = 0; i < a->goto_count; i++)
        if (a->goto_targets[i] == line_num) return true;
    return false;
}

/* Resolve variable type from name suffix and DEF table */
static gw_valtype_t resolve_var_type(analysis_t *a, uint8_t first_char, uint8_t suffix)
{
    if (suffix == '$') return VT_STR;
    if (suffix == '%') return VT_INT;
    if (suffix == '!') return VT_SNG;
    if (suffix == '#') return VT_DBL;
    int idx = toupper(first_char) - 'A';
    if (idx >= 0 && idx < 26)
        return a->def_type[idx];
    return VT_SNG;
}

/* Scan a token stream for variable references, GOTO targets, DATA.
 * When line_num != 0, also tracks assignment vs. use context. */
static array_info_t *find_array_info(analysis_t *a, const char name[2], gw_valtype_t type)
{
    for (int i = 0; i < a->array_count; i++)
        if (a->arrays[i].name[0] == name[0] && a->arrays[i].name[1] == name[1]
            && a->arrays[i].type == type)
            return &a->arrays[i];
    return NULL;
}

/* Mark an array as never-inlinable (variable DIM, redefinition, or ERASE). */
static void disqualify_array(analysis_t *a, const char name[2], gw_valtype_t type)
{
    array_info_t *ai = find_array_info(a, name, type);
    if (ai) { ai->inlinable = false; return; }
    if (a->array_count < MAX_ARRAYS) {
        array_info_t *n = &a->arrays[a->array_count++];
        n->name[0] = name[0]; n->name[1] = name[1]; n->type = type;
        n->inlinable = false; n->dimmed = false; n->ndims = 0;
    }
}

array_info_t *analysis_inlinable_array(analysis_t *a, const char name[2], gw_valtype_t type)
{
    array_info_t *ai = find_array_info(a, name, type);
    return (ai && ai->inlinable && ai->dimmed) ? ai : NULL;
}

static void scan_tokens(analysis_t *a, uint8_t *tokens, int len, uint16_t line_num)
{
    uint8_t *p = tokens;
    uint8_t *end = tokens + len;
    /* Tracks whether the next variable seen is being assigned to.
     * Set at statement start, after FOR, READ, INPUT, LET, SWAP.
     * multi_assign: stays true across commas (for READ A,B,C / INPUT A,B). */
    bool assign_ctx = true;  /* start of line = statement start */
    bool multi_assign = false;

    while (p < end && *p) {
        uint8_t tok = *p;

        /* Skip spaces */
        if (tok == ' ') { p++; continue; }

        /* Colon = statement separator; next variable is assignment target */
        if (tok == ':') { p++; assign_ctx = true; multi_assign = false; continue; }

        /* Skip constants */
        if (is_constant(tok)) { skip_constant(&p); assign_ctx = false; continue; }

        /* Skip string literals (preserve assign_ctx for INPUT "prompt", var) */
        if (tok == '"') {
            p++;
            while (p < end && *p && *p != '"') p++;
            if (p < end && *p == '"') p++;
            continue;
        }

        /* DEFINT/DEFSNG/DEFDBL/DEFSTR — skip to end of statement
         * (range letters like A-Z are not variable references) */
        if (tok == TOK_DEFINT || tok == TOK_DEFSNG ||
            tok == TOK_DEFDBL || tok == TOK_DEFSTR) {
            while (p < end && *p && *p != ':' && *p != TOK_ELSE) p++;
            assign_ctx = false;
            continue;
        }

        /* DIM — parse each declared array's constant bounds for --inline-arrays.
         * An array is inlinable only if every dimension is a bare integer
         * literal; anything else (a variable, an expression, a float) leaves it
         * on the runtime path. */
        if (tok == TOK_DIM) {
            p++;
            for (;;) {
                while (p < end && *p == ' ') p++;
                if (p >= end || *p == ':' || *p == 0 || !is_letter(*p)) break;
                char nm[2] = {(char)toupper(*p), 0};
                p++;
                if (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9'))) {
                    nm[1] = (char)toupper(*p); p++;
                    while (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9'))) p++;
                }
                uint8_t suf = (p < end) ? *p : 0;
                if (suf == '$' || suf == '%' || suf == '!' || suf == '#') p++;
                else suf = 0;
                gw_valtype_t type = resolve_var_type(a, (uint8_t)nm[0], suf);
                while (p < end && *p == ' ') p++;
                if (p >= end || *p != '(') { disqualify_array(a, nm, type); break; }
                p++;  /* ( */
                int dims[MAX_ARR_DIMS]; int nd = 0; bool ok = true;
                int depth = 1; bool expect_num = true;
                while (p < end && *p && depth > 0) {
                    uint8_t t = *p;
                    if (t == ' ') { p++; continue; }
                    if (t == '(') { depth++; ok = false; p++; continue; }
                    if (t == ')') { depth--; p++; continue; }
                    if (depth == 1 && t == ',') { expect_num = true; p++; continue; }
                    if (depth == 1 && expect_num
                        && ((t >= 0x11 && t <= 0x1A) || t == TOK_INT1 || t == TOK_INT2)) {
                        uint16_t v = read_encoded_int(&p);
                        if (nd < MAX_ARR_DIMS) dims[nd++] = v; else ok = false;
                        expect_num = false;
                        continue;
                    }
                    ok = false;
                    if (is_constant(t)) skip_constant(&p); else p++;
                }
                array_info_t *ai = find_array_info(a, nm, type);
                if (ai) {
                    ai->inlinable = false;  /* redefinition -> runtime DD error */
                } else if (a->array_count < MAX_ARRAYS) {
                    array_info_t *n = &a->arrays[a->array_count++];
                    n->name[0] = nm[0]; n->name[1] = nm[1]; n->type = type;
                    /* String arrays stay on the runtime path so their elements
                     * remain visible to the string-pool garbage collector. */
                    n->dimmed = true; n->ndims = nd;
                    n->inlinable = ok && depth == 0 && !expect_num && nd > 0 && type != VT_STR;
                    for (int i = 0; i < nd; i++) n->dims[i] = dims[i];
                }
                while (p < end && *p == ' ') p++;
                if (p < end && *p == ',') { p++; continue; }
                break;
            }
            assign_ctx = false;
            continue;
        }

        /* OPTION BASE 0|1 — affects inlined array size and index base */
        if (tok == TOK_OPTION) {
            p++;
            while (p < end && *p == ' ') p++;
            while (p < end && is_letter(*p)) p++;  /* skip "BASE" */
            while (p < end && *p == ' ') p++;
            if (p < end && ((*p >= 0x11 && *p <= 0x1A) || *p == TOK_INT1 || *p == TOK_INT2))
                a->option_base = read_encoded_int(&p);
            while (p < end && *p && *p != ':' && *p != TOK_ELSE) {
                if (is_constant(*p)) skip_constant(&p);
                else if (*p == TOK_PREFIX_FD || *p == TOK_PREFIX_FE || *p == TOK_PREFIX_FF)
                    p += 2;
                else p++;
            }
            assign_ctx = false;
            continue;
        }

        /* ERASE — the array leaves compile-time-known state; never inline it */
        if (tok == TOK_ERASE) {
            p++;
            while (p < end && *p && *p != ':' && *p != TOK_ELSE) {
                while (p < end && *p == ' ') p++;
                if (p >= end || !*p || *p == ':' || *p == TOK_ELSE) break;
                if (p < end && is_letter(*p)) {
                    char nm[2] = {(char)toupper(*p), 0}; p++;
                    if (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9'))) {
                        nm[1] = (char)toupper(*p); p++;
                        while (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9'))) p++;
                    }
                    uint8_t suf = (p < end) ? *p : 0;
                    if (suf == '$' || suf == '%' || suf == '!' || suf == '#') p++;
                    else suf = 0;
                    disqualify_array(a, nm, resolve_var_type(a, (uint8_t)nm[0], suf));
                } else p++;
            }
            assign_ctx = false;
            continue;
        }

        /* OPEN — skip the mode/filename tokens to avoid misidentifying
         * OUTPUT/INPUT/APPEND/RANDOM as variable names */
        if (tok == TOK_OPEN) {
            while (p < end && *p && *p != ':') p++;
            assign_ctx = false;
            continue;
        }

        /* FOR — next variable is the loop variable (assigned) */
        if (tok == TOK_FOR) {
            p++; assign_ctx = true;
            continue;
        }

        /* READ/INPUT — all variables in the list are assigned */
        if (tok == TOK_READ || tok == TOK_INPUT) {
            p++; assign_ctx = true; multi_assign = true;
            continue;
        }

        /* LINE (as in LINE INPUT) — next token is INPUT, assign context */
        if (tok == TOK_LINE) {
            p++; assign_ctx = true;
            continue;
        }

        /* LET — next variable is assigned */
        if (tok == TOK_LET) {
            p++; assign_ctx = true;
            continue;
        }

        /* SWAP — both variables are assigned */
        if (tok == TOK_SWAP) {
            p++; assign_ctx = true; multi_assign = true;
            continue;
        }

        /* GOTO/GOSUB/THEN/ELSE/RESTORE/RESUME/RUN — mark targets */
        if (tok == TOK_GOTO || tok == TOK_GOSUB || tok == TOK_THEN ||
            tok == TOK_ELSE ||
            tok == TOK_RESTORE || tok == TOK_RESUME || tok == TOK_RUN) {
            bool is_branch = (tok == TOK_THEN || tok == TOK_ELSE);
            p++;
            while (p < end && *p == ' ') p++;
            /* Read line number(s) */
            while (p < end && is_constant(*p)) {
                uint8_t *save = p;
                uint16_t target = read_encoded_int(&p);
                if (p != save && !(tok == TOK_RESUME && target == 0))
                    mark_target(a, target);
                while (p < end && *p == ' ') p++;
                if (p < end && *p == ',') {
                    p++;
                    while (p < end && *p == ' ') p++;
                } else {
                    break;
                }
            }
            /* THEN/ELSE can also introduce an assignment rather than a line. */
            assign_ctx = is_branch;
            multi_assign = false;
            continue;
        }

        /* ON ERROR GOTO — the GOTO is followed by a line number */
        if (tok == TOK_ON) {
            p++;
            assign_ctx = false;
            continue;
        }

        /* DATA — collect literals */
        if (tok == TOK_DATA) {
            p++;
            while (p < end && *p == ' ') p++;
            while (p < end && *p && *p != ':') {
                while (p < end && *p == ' ') p++;
                char buf[256];
                int bi = 0;
                if (*p == '"') {
                    p++;
                    while (p < end && *p && *p != '"' && bi < 255)
                        buf[bi++] = *p++;
                    if (p < end && *p == '"') p++;
                } else {
                    while (p < end && *p && *p != ',' && *p != ':' && bi < 255)
                        buf[bi++] = *p++;
                    while (bi > 0 && buf[bi-1] == ' ') bi--;
                }
                buf[bi] = '\0';
                if (a->data_count < MAX_DATA)
                    a->data_pool[a->data_count++] = strdup(buf);
                while (p < end && *p == ' ') p++;
                if (p < end && *p == ',') { p++; continue; }
                break;
            }
            assign_ctx = false;
            continue;
        }

        /* REM — skip to end */
        if (tok == TOK_REM || tok == TOK_SQUOTE) {
            while (p < end && *p) p++;
            continue;
        }

        /* Variable reference */
        if (is_letter(tok)) {
            char name[2] = {(char)toupper(tok), 0};
            char full[EXTERN_NAME_MAX];
            int fi = 0;
            full[fi++] = (char)toupper(tok);
            p++;
            if (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9'))) {
                name[1] = (char)toupper(*p);
                if (fi < EXTERN_NAME_MAX - 1) full[fi++] = (char)toupper(*p);
                p++;
                while (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9'))) {
                    if (fi < EXTERN_NAME_MAX - 1) full[fi++] = (char)toupper(*p);
                    p++;
                }
            }
            full[fi] = 0;
            /* A declared extern is a function call, not a variable — don't add
             * it to the census.  Its argument expressions are scanned normally
             * by the surrounding loop. */
            if (analysis_find_extern(a, full)) {
                if (p < end && (*p == '$' || *p == '%' || *p == '!' || *p == '#'))
                    p++;
                assign_ctx = false;
                continue;
            }
            uint8_t suffix = (p < end) ? *p : 0;
            if (suffix == '$' || suffix == '%' || suffix == '!' || suffix == '#')
                p++;
            else
                suffix = 0;
            gw_valtype_t type = resolve_var_type(a, (uint8_t)name[0], suffix);

            if (line_num) {
                if (assign_ctx)
                    mark_var_assign(a, name, type, line_num);
                else
                    mark_var_use(a, name, type, line_num);
            } else {
                analysis_add_var(a, name, type);
            }
            /* In multi-assign context (READ/INPUT), keep assign_ctx for next var */
            if (!multi_assign)
                assign_ctx = false;
            continue;
        }

        /* Extended tokens (0xFD, 0xFE, 0xFF prefix) — skip the prefix byte */
        if (tok == TOK_PREFIX_FD || tok == TOK_PREFIX_FE || tok == TOK_PREFIX_FF) {
            /* COMMON — variables receive values from CHAIN, treat as assigned */
            if (tok == TOK_PREFIX_FE && p[1] == XSTMT_COMMON) {
                p += 2;
                assign_ctx = true;
                multi_assign = true;
                continue;
            }
            p += 2;
            assign_ctx = false;
            continue;
        }

        /* Everything else: single byte token.
         * Commas preserve assign_ctx (they separate items in INPUT, READ, etc.) */
        p++;
        if (tok != ',')
            assign_ctx = false;
    }
}

void analysis_run(analysis_t *a)
{
    memset(a, 0, sizeof(*a));
    for (int i = 0; i < 26; i++)
        a->def_type[i] = VT_SNG;

    /* Only declarations in the straight-line entry prefix establish static
     * defaults. Later declarations can be skipped, repeated, or change storage
     * identity, so those programs need runtime type resolution. Token payloads,
     * strings, DATA and comments must not masquerade as declarations. This
     * lexical pass also finds RESUME inside branches that later census helpers
     * skip, such as ELSE after OPEN. */
    bool entry_prefix = true;
    for (program_line_t *line = gw.prog_head; line; line = line->next) {
        uint8_t *p = line->tokens;
        while (*p) {
            if (*p == ' ' || *p == ':') { p++; continue; }
            if (*p == TOK_REM || *p == TOK_SQUOTE) break;
            if (*p == TOK_DATA) {
                p++;
                while (*p && *p != ':') {
                    if (*p == '"') {
                        p++;
                        while (*p && *p != '"') p++;
                        if (*p) p++;
                    } else p++;
                }
                continue;
            }
            if (*p == TOK_RESUME) a->has_resume = true;
            if (*p == TOK_DEFINT || *p == TOK_DEFSNG ||
                *p == TOK_DEFDBL || *p == TOK_DEFSTR) {
                if (!entry_prefix) a->runtime_types = true;
                gw_valtype_t dt;
                switch (*p) {
                case TOK_DEFINT: dt = VT_INT; break;
                case TOK_DEFSNG: dt = VT_SNG; break;
                case TOK_DEFDBL: dt = VT_DBL; break;
                case TOK_DEFSTR: dt = VT_STR; break;
                default: dt = VT_SNG; break;
                }
                p++;
                while (*p == ' ') p++;
                /* Parse letter ranges: A-Z, X-Z, etc. */
                while (*p && *p != ':' && *p != 0) {
                    if (is_letter(*p)) {
                        int from = toupper(*p) - 'A';
                        int to = from;
                        p++;
                        while (*p == ' ') p++;
                        if (*p == TOK_MINUS || *p == '-') {
                            p++;
                            while (*p == ' ') p++;
                            if (is_letter(*p)) {
                                to = toupper(*p) - 'A';
                                p++;
                            }
                        }
                        for (int c = from; c <= to && c < 26; c++)
                            a->def_type[c] = dt;
                    }
                    while (*p == ' ' || *p == ',') p++;
                    if (!is_letter(*p)) break;
                }
                continue;
            }
            entry_prefix = false;
            if (is_constant(*p)) skip_constant(&p);
            else if (*p == '"') {
                p++;
                while (*p && *p != '"') p++;
                if (*p) p++;
            } else if (*p == TOK_PREFIX_FD || *p == TOK_PREFIX_FE || *p == TOK_PREFIX_FF)
                p += 2;
            else p++;
        }
    }

    /* Pass 0b: collect '$EXTERN FFI pragmas.  These are standalone
     * apostrophe/REM comment lines (`'$EXTERN NAME(ARGS) AS RET`), so the
     * interpreter ignores them while the compiler registers them.  Must run
     * before Pass 1 so use-sites aren't mistaken for array variables. */
    for (program_line_t *line = gw.prog_head; line; line = line->next) {
        uint8_t *p = line->tokens;
        if (line->len <= 0 || (p[0] != TOK_REM && p[0] != TOK_SQUOTE))
            continue;
        uint8_t *end = line->tokens + line->len;
        p++;
        while (p < end && *p == ' ') p++;
        if (p < end && *p == '$') {
            char buf[256];
            int bi = 0;
            while (p < end && *p && bi < (int)sizeof(buf) - 1)
                buf[bi++] = (char)*p++;
            buf[bi] = 0;
            if (ci_starts(buf, "$EXTERN"))
                parse_extern_pragma(a, buf);
        }
    }

    /* Pass 1: collect line numbers and scan tokens */
    for (program_line_t *line = gw.prog_head; line; line = line->next) {
        if (a->line_count >= MAX_LINES) break;

        int li = a->line_count++;
        a->lines[li].line_num = line->num;
        a->lines[li].is_target = false;
        a->lines[li].has_data = false;
        a->lines[li].data_start = a->data_count;

        /* Check for DATA in this line */
        uint8_t *p = line->tokens;
        while (*p) {
            if (*p == TOK_DATA) {
                a->lines[li].has_data = true;
                break;
            }
            p++;
        }

        /* Record data line mapping */
        if (a->lines[li].has_data && a->data_line_count < MAX_LINES) {
            a->data_line_map[a->data_line_count][0] = line->num;
            a->data_line_map[a->data_line_count][1] = a->data_count;
        }

        scan_tokens(a, line->tokens, line->len, line->num);

        /* Finalize data line mapping */
        if (a->lines[li].has_data && a->data_line_count < MAX_LINES) {
            a->data_line_map[a->data_line_count][1] = a->lines[li].data_start;
            a->data_line_count++;
        }
    }

    /* Mark the first line as a target (program entry point) */
    if (a->line_count > 0)
        a->lines[0].is_target = true;

    /* Resolve forward-reference jump targets: mark_target() during scanning
     * can only set is_target for lines already in the table. Now that all
     * lines are collected, do a second pass over goto_targets[]. */
    for (int g = 0; g < a->goto_count; g++) {
        for (int i = 0; i < a->line_count; i++) {
            if (a->lines[i].line_num == a->goto_targets[g]) {
                a->lines[i].is_target = true;
                break;
            }
        }
    }
}

static const char *var_suffix_str(gw_valtype_t t)
{
    switch (t) {
    case VT_INT: return "%";
    case VT_STR: return "$";
    case VT_DBL: return "#";
    default: return "";
    }
}

static bool line_exists(analysis_t *a, uint16_t num)
{
    for (int i = 0; i < a->line_count; i++)
        if (a->lines[i].line_num == num) return true;
    return false;
}

void analysis_warnings(analysis_t *a)
{
    /* Uninitialized variable detection */
    for (int i = 0; i < a->var_count; i++) {
        var_info_t *v = &a->vars[i];
        char vname[8];
        if (v->name[1])
            snprintf(vname, sizeof(vname), "%c%c%s", v->name[0], v->name[1],
                     var_suffix_str(v->type));
        else
            snprintf(vname, sizeof(vname), "%c%s", v->name[0],
                     var_suffix_str(v->type));
        if (v->first_use_line && !v->first_assign_line) {
            fprintf(stderr, "warning: variable %s used at line %u"
                    " but never assigned\n", vname, v->first_use_line);
        } else if (v->first_use_line && v->first_assign_line &&
                   v->first_use_line < v->first_assign_line) {
            fprintf(stderr, "warning: variable %s used at line %u"
                    " before first assignment at line %u\n",
                    vname, v->first_use_line, v->first_assign_line);
        }
    }

    /* GOTO/GOSUB to nonexistent line */
    for (int i = 0; i < a->goto_count; i++) {
        if (!line_exists(a, a->goto_targets[i]))
            fprintf(stderr, "warning: GOTO/GOSUB target line %u does not exist\n",
                    a->goto_targets[i]);
    }

    /* Unreachable code: line after unconditional transfer that isn't a target */
    for (int i = 0; i < a->line_count - 1; i++) {
        /* Check if this line ends with GOTO/END/STOP */
        uint16_t num = a->lines[i].line_num;
        program_line_t *pl = NULL;
        for (program_line_t *p = gw.prog_head; p; p = p->next) {
            if (p->num == num) { pl = p; break; }
        }
        if (!pl) continue;

        /* Find the last statement-starting token on this line.
         * Walk statement by statement (split on ':'). */
        uint8_t *p = pl->tokens;
        uint8_t last_stmt_start = 0;
        while (*p) {
            while (*p == ' ' || *p == ':') p++;
            if (!*p) break;
            last_stmt_start = *p;
            /* Skip to next ':' or end */
            while (*p && *p != ':') {
                if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
                else p++;
            }
        }
        bool is_transfer = (last_stmt_start == TOK_GOTO ||
                            last_stmt_start == TOK_END ||
                            last_stmt_start == TOK_STOP);
        if (is_transfer && !a->lines[i + 1].is_target)
            fprintf(stderr, "warning: line %u: unreachable code\n",
                    a->lines[i + 1].line_num);
    }
}
