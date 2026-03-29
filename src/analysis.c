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
    return idx;
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

/* Scan a token stream for variable references, GOTO targets, DATA */
static void scan_tokens(analysis_t *a, uint8_t *tokens, int len)
{
    uint8_t *p = tokens;
    uint8_t *end = tokens + len;

    while (p < end && *p) {
        uint8_t tok = *p;

        /* Skip spaces */
        if (tok == ' ') { p++; continue; }

        /* Skip constants */
        if (is_constant(tok)) { skip_constant(&p); continue; }

        /* Skip string literals */
        if (tok == '"') {
            p++;
            while (p < end && *p && *p != '"') p++;
            if (p < end && *p == '"') p++;
            continue;
        }

        /* OPEN — skip the mode/filename tokens to avoid misidentifying
         * OUTPUT/INPUT/APPEND/RANDOM as variable names */
        if (tok == TOK_OPEN) {
            while (p < end && *p && *p != ':') p++;
            continue;
        }

        /* GOTO/GOSUB/THEN/RESTORE/RESUME/RUN — mark targets */
        if (tok == TOK_GOTO || tok == TOK_GOSUB || tok == TOK_THEN ||
            tok == TOK_RESTORE || tok == TOK_RESUME || tok == TOK_RUN) {
            p++;
            while (p < end && *p == ' ') p++;
            /* Read line number(s) */
            while (p < end && is_constant(*p)) {
                uint8_t *save = p;
                uint16_t target = read_encoded_int(&p);
                if (p != save)
                    mark_target(a, target);
                while (p < end && *p == ' ') p++;
                if (p < end && *p == ',') {
                    p++;
                    while (p < end && *p == ' ') p++;
                } else {
                    break;
                }
            }
            continue;
        }

        /* ON ERROR GOTO — the GOTO is followed by a line number */
        if (tok == TOK_ON) {
            p++;
            /* ON ERROR GOTO, ON n GOTO/GOSUB handled by scanning for GOTO/GOSUB above */
            continue;
        }

        /* DATA — collect literals */
        if (tok == TOK_DATA) {
            p++;
            while (p < end && *p == ' ') p++;
            /* Read comma-separated DATA items as raw strings */
            while (p < end && *p && *p != ':') {
                while (p < end && *p == ' ') p++;
                char buf[256];
                int bi = 0;
                if (*p == '"') {
                    p++; /* skip opening quote */
                    while (p < end && *p && *p != '"' && bi < 255)
                        buf[bi++] = *p++;
                    if (p < end && *p == '"') p++;
                } else {
                    while (p < end && *p && *p != ',' && *p != ':' && bi < 255)
                        buf[bi++] = *p++;
                    /* Trim trailing spaces */
                    while (bi > 0 && buf[bi-1] == ' ') bi--;
                }
                buf[bi] = '\0';
                if (a->data_count < MAX_DATA)
                    a->data_pool[a->data_count++] = strdup(buf);
                while (p < end && *p == ' ') p++;
                if (p < end && *p == ',') { p++; continue; }
                break;
            }
            continue;
        }

        /* REM — skip to end */
        if (tok == TOK_REM || tok == TOK_SQUOTE) {
            while (p < end && *p) p++;
            continue;
        }

        /* Variable reference: letter followed by optional second letter, optional suffix */
        if (is_letter(tok)) {
            char name[2] = {(char)toupper(tok), 0};
            p++;
            if (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9'))) {
                name[1] = (char)toupper(*p);
                p++;
                /* Skip remaining chars of long name (GW-BASIC only uses first 2) */
                while (p < end && (is_letter(*p) || (*p >= '0' && *p <= '9')))
                    p++;
            }
            uint8_t suffix = (p < end) ? *p : 0;
            if (suffix == '$' || suffix == '%' || suffix == '!' || suffix == '#')
                p++;
            else
                suffix = 0;
            gw_valtype_t type = resolve_var_type(a, (uint8_t)name[0], suffix);
            analysis_add_var(a, name, type);
            continue;
        }

        /* Extended tokens (0xFD, 0xFE, 0xFF prefix) — skip the prefix byte */
        if (tok == TOK_PREFIX_FD || tok == TOK_PREFIX_FE || tok == TOK_PREFIX_FF) {
            p += 2;
            continue;
        }

        /* Everything else: single byte token */
        p++;
    }
}

void analysis_run(analysis_t *a)
{
    memset(a, 0, sizeof(*a));
    for (int i = 0; i < 26; i++)
        a->def_type[i] = VT_SNG;

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

        scan_tokens(a, line->tokens, line->len);

        /* Finalize data line mapping */
        if (a->lines[li].has_data && a->data_line_count < MAX_LINES) {
            a->data_line_map[a->data_line_count][1] = a->lines[li].data_start;
            a->data_line_count++;
        }
    }

    /* Mark the first line as a target (program entry point) */
    if (a->line_count > 0)
        a->lines[0].is_target = true;
}
