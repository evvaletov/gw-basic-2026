#include "gwbasic.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * CRUNCH: convert text to tokenized form.
 * Reimplements GWMAIN.ASM's CRUNCH routine.
 *
 * Text like: PRINT 2+SIN(3.14)
 * Becomes:   0x90 [space] 0x0F 0x02 0x00 0xE7 0xFF 0x88 0x28 ...
 */

static int try_keyword(const char *text, int pos, const keyword_entry_t **match)
{
    int best_len = 0;
    const keyword_entry_t *best = NULL;

    for (int i = 0; gw_keywords[i].name; i++) {
        const char *kw = gw_keywords[i].name;
        int klen = strlen(kw);

        /* Skip single-char operators - handled separately */
        if (klen == 1 && !isalpha((unsigned char)kw[0]))
            continue;

        /* Case-insensitive match */
        int j;
        for (j = 0; j < klen; j++) {
            if (toupper((unsigned char)text[pos + j]) != kw[j])
                break;
        }

        if (j == klen && klen > best_len) {
            /* FN is immediately followed by its name (FNA). */
            bool prefix_word = gw_keywords[i].prefix == 0 &&
                gw_keywords[i].token == TOK_FN;
            if (!prefix_word && isalpha((unsigned char)kw[0]) && kw[klen - 1] != '(' && kw[klen - 1] != '$') {
                char next = text[pos + klen];
                if (isalnum((unsigned char)next) || next == '.')
                    continue;
            }
            best_len = klen;
            best = &gw_keywords[i];
        }
    }

    *match = best;
    return best_len;
}

int gw_crunch(const char *text, uint8_t *out, int outsize)
{
    int ip = 0;   /* input position */
    int op = 0;   /* output position */
    int in_rem = 0;
    int in_data = 0;
    int in_string = 0;

    if (outsize <= 0) return -1;

    /* Every token, including its payload, must leave room for the terminator. */
#define NEED_BYTES(n) do { if ((n) > outsize - op - 1) goto overflow; } while (0)
    while (text[ip]) {
        NEED_BYTES(1);
        char ch = text[ip];

        /* Inside a string literal */
        if (in_string) {
            out[op++] = ch;
            if (ch == '"')
                in_string = 0;
            ip++;
            continue;
        }

        /* Inside REM or DATA - copy literally (DATA stops at colon) */
        if (in_rem) {
            out[op++] = ch;
            ip++;
            continue;
        }
        if (in_data) {
            if (ch == ':') {
                in_data = 0;
                /* fall through to normal processing */
            } else {
                out[op++] = ch;
                ip++;
                continue;
            }
        }

        /* Start of string */
        if (ch == '"') {
            in_string = 1;
            out[op++] = ch;
            ip++;
            continue;
        }

        /* Single quote = REM shorthand */
        if (ch == '\'') {
            out[op++] = TOK_SQUOTE;
            ip++;
            in_rem = 1;
            continue;
        }

        /* Colon - statement separator */
        if (ch == ':') {
            out[op++] = ':';
            ip++;
            continue;
        }

        /* Special single-char operators */
        if (ch == '+') { out[op++] = TOK_PLUS;  ip++; continue; }
        if (ch == '-') { out[op++] = TOK_MINUS; ip++; continue; }
        if (ch == '*') { out[op++] = TOK_MUL;   ip++; continue; }
        if (ch == '/') { out[op++] = TOK_DIV;   ip++; continue; }
        if (ch == '^') { out[op++] = TOK_POW;   ip++; continue; }
        if (ch == '\\') { out[op++] = TOK_IDIV; ip++; continue; }
        if (ch == '>') { out[op++] = TOK_GT;    ip++; continue; }
        if (ch == '=') { out[op++] = TOK_EQ;    ip++; continue; }
        if (ch == '<') { out[op++] = TOK_LT;    ip++; continue; }

        /* Try keyword match (alphabetic) */
        if (isalpha((unsigned char)ch)) {
            const keyword_entry_t *kw;
            int klen = try_keyword(text, ip, &kw);
            if (kw) {
                NEED_BYTES(kw->prefix ? 2 : 1);
                if (kw->prefix) {
                    out[op++] = kw->prefix;
                }
                out[op++] = kw->token;
                ip += klen;

                /* Set REM/DATA modes */
                if (kw->token == TOK_REM && kw->prefix == 0)
                    in_rem = 1;
                if (kw->token == TOK_DATA && kw->prefix == 0)
                    in_data = 1;
                continue;
            }

            /* Not a keyword: copy entire variable name (letters + digits) */
            while (isalnum((unsigned char)text[ip]) || text[ip] == '.') {
                NEED_BYTES(1);
                out[op++] = text[ip++];
            }
            /* Copy type suffix if present */
            if (text[ip] == '%' || text[ip] == '!' ||
                text[ip] == '#' || text[ip] == '$') {
                NEED_BYTES(1);
                out[op++] = text[ip++];
            }
            continue;
        }

        /* &H hex, &O octal, & octal */
        if (ch == '&') {
            char next = toupper((unsigned char)text[ip + 1]);
            if (next == 'H') {
                NEED_BYTES(3);
                ip += 2;
                int16_t val = (int16_t)strtol(&text[ip], NULL, 16);
                while (isxdigit((unsigned char)text[ip])) ip++;
                out[op++] = TOK_INT2;
                out[op++] = val & 0xFF;
                out[op++] = (val >> 8) & 0xFF;
                continue;
            }
            if (next == 'O' || isdigit((unsigned char)next)) {
                NEED_BYTES(3);
                if (next == 'O') ip += 2; else ip += 1;
                int16_t val = (int16_t)strtol(&text[ip], NULL, 8);
                while (text[ip] >= '0' && text[ip] <= '7') ip++;
                out[op++] = TOK_INT2;
                out[op++] = val & 0xFF;
                out[op++] = (val >> 8) & 0xFF;
                continue;
            }
            /* Just & by itself */
            out[op++] = ch;
            ip++;
            continue;
        }

        /* Numeric constant */
        if (isdigit((unsigned char)ch) || ch == '.') {
            const char *start = &text[ip];
            char *end;

            /* GW-BASIC uses D for double exponent (1D10 = 1E10 as double).
               C's strtod doesn't handle D, so scan ahead and substitute. */
            char nbuf[32];
            int is_double = 0;
            int is_float = 0;
            int scan = 0;
            while (scan < (int)sizeof(nbuf) - 1) {
                char c = start[scan];
                if (!isdigit((unsigned char)c) && c != '.' && c != '+' && c != '-'
                    && toupper((unsigned char)c) != 'E' && toupper((unsigned char)c) != 'D')
                    break;
                if (toupper((unsigned char)c) == 'D') {
                    is_double = 1;
                    nbuf[scan] = 'E';
                } else {
                    nbuf[scan] = c;
                }
                /* Don't allow +/- except after E/D */
                if ((c == '+' || c == '-') && scan > 0
                    && toupper((unsigned char)start[scan - 1]) != 'E'
                    && toupper((unsigned char)start[scan - 1]) != 'D')
                    break;
                scan++;
            }
            nbuf[scan] = '\0';

            double val = strtod(nbuf, &end);
            int nlen = end - nbuf;
            if (nlen == 0) {
                out[op++] = ch;
                ip++;
                continue;
            }

            for (int k = 0; k < nlen; k++) {
                if (nbuf[k] == '.')
                    is_float = 1;
                else if (toupper((unsigned char)nbuf[k]) == 'E' && !is_double)
                    is_float = 1;
            }

            /* Check for explicit type suffix after number */
            char suffix = toupper((unsigned char)text[ip + nlen]);
            int consumed = nlen;
            if (suffix == '!' || suffix == '#' || suffix == '%')
                consumed++;

            if (!is_float && !is_double && suffix != '!' && suffix != '#') {
                /* Try integer */
                if (val >= -32768 && val <= 32767) {
                    int16_t ival = (int16_t)val;
                    if (ival >= 0 && ival <= 9) {
                        out[op++] = 0x11 + ival;
                    } else if (ival >= 10 && ival <= 255) {
                        NEED_BYTES(2);
                        out[op++] = TOK_INT1;
                        out[op++] = (uint8_t)ival;
                    } else {
                        NEED_BYTES(3);
                        out[op++] = TOK_INT2;
                        out[op++] = ival & 0xFF;
                        out[op++] = (ival >> 8) & 0xFF;
                    }
                    ip += consumed;
                    continue;
                }
            }

            /* Double: D exponent or # suffix */
            if (is_double || suffix == '#') {
                NEED_BYTES(9);
                out[op++] = TOK_CONST_DBL;
                memcpy(&out[op], &val, 8);
                op += 8;
            } else {
                NEED_BYTES(5);
                /* Single: default for decimal/E numbers */
                float fval = (float)val;
                out[op++] = TOK_CONST_SNG;
                memcpy(&out[op], &fval, 4);
                op += 4;
            }
            ip += consumed;
            continue;
        }

        /* Spaces - preserve in token stream (GW-BASIC preserves spaces) */
        if (ch == ' ') {
            out[op++] = ' ';
            ip++;
            continue;
        }

        /* Anything else (parens, commas, semicolons, variable names) */
        out[op++] = ch;
        ip++;
    }

    out[op] = 0;  /* null terminate */
    return op;
overflow:
    out[0] = 0;  /* Never let callers execute a truncated line. */
    return -1;
#undef NEED_BYTES
}

/*
 * LIST: convert tokens back to text.
 * Reimplements the LIST subroutine from IBMRES.ASM.
 */
void gw_list_line(uint8_t *tokens, int len, char *out, int outsize)
{
    int ip = 0;
    int op = 0;

    while (ip < len && op < outsize - 10) {
        uint8_t ch = tokens[ip++];

        /* Multi-byte token prefixes */
        if (ch == TOK_PREFIX_FF || ch == TOK_PREFIX_FE || ch == TOK_PREFIX_FD) {
            if (ip < len) {
                uint8_t tok = tokens[ip++];
                const char *name = gw_token_name(ch, tok);
                if (name) {
                    int nlen = strlen(name);
                    if (op + nlen + 2 < outsize) {
                        /* Space before keyword if needed */
                        if (op > 0 && isalnum((unsigned char)out[op - 1]))
                            out[op++] = ' ';
                        memcpy(&out[op], name, nlen);
                        op += nlen;
                    }
                }
            }
            continue;
        }

        /* Embedded integer constant */
        if (ch == TOK_INT2 && ip + 1 < len) {
            int16_t val = (int16_t)(tokens[ip] | (tokens[ip + 1] << 8));
            ip += 2;
            op += snprintf(&out[op], outsize - op, "%d", val);
            continue;
        }

        /* Single-byte integer 0-9 */
        if (ch >= 0x11 && ch <= 0x1A) {
            out[op++] = '0' + (ch - 0x11);
            continue;
        }

        /* One-byte int constant */
        if (ch == TOK_INT1 && ip < len) {
            op += snprintf(&out[op], outsize - op, "%d", tokens[ip++]);
            continue;
        }

        /* Embedded single constant */
        if (ch == TOK_CONST_SNG && ip + 3 < len) {
            float fval;
            memcpy(&fval, &tokens[ip], 4);
            ip += 4;
            op += snprintf(&out[op], outsize - op, "%g", (double)fval);
            continue;
        }

        /* Embedded double constant */
        if (ch == TOK_CONST_DBL && ip + 7 < len) {
            double dval;
            memcpy(&dval, &tokens[ip], 8);
            ip += 8;
            op += snprintf(&out[op], outsize - op, "%g", dval);
            continue;
        }

        /* Single-byte token */
        if (ch >= 0x80) {
            const char *name = gw_token_name(0, ch);
            if (name) {
                int nlen = strlen(name);
                if (op + nlen + 2 < outsize) {
                    if (op > 0 && isalnum((unsigned char)out[op - 1])
                        && isalpha((unsigned char)name[0]))
                        out[op++] = ' ';
                    memcpy(&out[op], name, nlen);
                    op += nlen;
                    /* Space after alphabetic keyword */
                    if (isalpha((unsigned char)name[nlen - 1])
                        && name[nlen - 1] != '(')
                        out[op++] = ' ';
                }
            }
            continue;
        }

        /* Regular character */
        out[op++] = ch;
    }

    out[op] = '\0';
}

/* Locate the end of an initially empty FOR without evaluating skipped code.
 * NEXT J,I closes two lexical levels; return just after the matching component
 * so the caller can still execute the remaining NEXT variables. */
bool gw_for_exit(program_line_t *line, uint8_t *p,
                 program_line_t **exit_line, uint8_t **exit_text, uint8_t **next_var)
{
    int depth = 0;
    bool at_start = false;
    for (; line; line = line->next, p = line ? line->tokens : NULL) {
        uint8_t *end = line->len ? line->tokens + line->len : NULL;
        while ((!end || p < end) && *p) {
            uint8_t tok = *p++;
            if (tok == ' ') continue;
            if (tok == ':' || tok == TOK_THEN || tok == TOK_ELSE) {
                at_start = true;
                continue;
            }
            bool statement = at_start;
            at_start = false;
            if (tok == TOK_REM || tok == TOK_SQUOTE) break;
            if (tok == '"') {
                while ((!end || p < end) && *p && *p != '"') p++;
                if ((!end || p < end) && *p) p++;
            } else if (tok == TOK_INT1) p++;
            else if (tok == TOK_INT2) p += 2;
            else if (tok == TOK_CONST_SNG) p += 4;
            else if (tok == TOK_CONST_DBL) p += 8;
            else if (tok == TOK_PREFIX_FD || tok == TOK_PREFIX_FE || tok == TOK_PREFIX_FF) p++;
            else if (statement && tok == TOK_FOR) depth++;
            else if (statement && tok == TOK_NEXT) {
                for (;;) {
                    while ((!end || p < end) && *p == ' ') p++;
                    uint8_t *component = p;
                    while ((!end || p < end) && (isalnum((unsigned char)*p) || *p == '.')) p++;
                    if ((!end || p < end) && (*p == '%' || *p == '!' || *p == '#' || *p == '$')) p++;
                    while ((!end || p < end) && *p == ' ') p++;
                    if (depth-- == 0) {
                        *exit_line = line;
                        *exit_text = p;
                        *next_var = component;
                        return true;
                    }
                    if ((end && p >= end) || *p != ',') break;
                    p++;
                }
            }
        }
        at_start = true;
    }
    return false;
}
