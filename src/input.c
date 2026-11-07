#include "gwbasic.h"
#include "tui.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

extern int print_col;

/*
 * INPUT and LINE INPUT statements.
 * INPUT: prompt, read comma-separated values into variables.
 * LINE INPUT: read entire line as string.
 */

static char *read_input_line(bool no_cr)
{
    /* Use TUI line editor when TUI is active */
    if (tui.active) {
        char *line = tui_read_input(no_cr);
        if (!line) {
            tui.break_flag = true;
            tui_check_break();
        }
        if (line) print_col = tui_get_cursor_col();
        return line;
    }

    static char buf[256];
    if (gw_hal && gw_hal->is_tty && gw_hal->read_input) {
        int result = gw_hal->read_input(buf, sizeof(buf), no_cr);
        if (result == -2) {
            gw_hal->puts("\nBreak\n");
            gw.running = false;
            longjmp(gw_error_jmp, -1);
        }
        if (result == -1) return NULL;
        if (result >= 0) {
            print_col = gw_hal->get_cursor_col();
            return buf;
        }
    }
    if (gw_hal) gw_hal->disable_raw();
    if (fgets(buf, sizeof(buf), stdin) == NULL) {
        if (gw_hal) gw_hal->enable_raw();
        return NULL;
    }
    if (gw_hal) gw_hal->enable_raw();
    int len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    if (!no_cr) print_col = 0;
    else if (gw_hal) print_col = gw_hal->get_cursor_col();
    return buf;
}

void gw_stmt_input(void)
{
    gw_skip_spaces();

    /* Optional semicolon suppresses the newline after the answer. */
    /* INPUT [;]["prompt";] var, var, ... */
    bool no_cr = false;
    if (gw_chrgot() == ';') {
        no_cr = true;
        gw_chrget();
        gw_skip_spaces();
    }

    bool question_mark = true;
    /* Check for prompt string */
    if (gw_chrgot() == '"') {
        gw.text_ptr++;
        while (*gw.text_ptr && *gw.text_ptr != '"') {
            if (gw_hal) gw_hal->putch(*gw.text_ptr);
            else putchar(*gw.text_ptr);
            gw.text_ptr++;
        }
        if (*gw.text_ptr == '"')
            gw.text_ptr++;

        gw_skip_spaces();
        if (gw_chrgot() == ';') {
            gw_chrget();
        } else if (gw_chrgot() == ',') {
            question_mark = false;
            gw_chrget();
        }
    }

retry:;
    /* Print "? " */
    if (question_mark) {
        if (gw_hal) gw_hal->puts("? ");
        else fputs("? ", stdout);
    }
    fflush(stdout);

    char *line = read_input_line(no_cr);
    if (!line) return;

    const char *p = line;

    /* Parse variable list and assign values */
    int var_idx = 0;
    for (;;) {
        gw_skip_spaces();
        uint8_t ch = gw_chrgot();
        if (ch == 0 || ch == ':')
            break;

        char name[GW_NAME_SIZE];
        gw_valtype_t type = gw_parse_varname(name);

        gw_skip_spaces();
        var_entry_t *var;
        gw_value_t *arr_elem = NULL;
        if (gw_chrgot() == '(') {
            arr_elem = gw_array_element(name, type);
        } else {
            var = gw_var_find_or_create(name, type);
        }

        /* Skip whitespace in input */
        while (*p == ' ') p++;

        /* Parse value from input line */
        gw_value_t val;
        if (type == VT_STR) {
            /* Read string: until comma or end of line */
            const char *start = p;
            if (*p == '"') {
                /* Quoted string */
                p++;
                start = p;
                while (*p && *p != '"') p++;
                int len = p - start;
                val.type = VT_STR;
                val.sval = gw_str_alloc(len);
                if (len) memcpy(val.sval.data, start, len);
                if (*p == '"') p++;
            } else {
                while (*p && *p != ',') p++;
                int len = p - start;
                /* Trim trailing spaces */
                while (len > 0 && start[len - 1] == ' ') len--;
                val.type = VT_STR;
                val.sval = gw_str_alloc(len);
                if (len) memcpy(val.sval.data, start, len);
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
        var_idx++;

        gw_skip_spaces();
        if (gw_chrgot() == ',') {
            gw_chrget();
        } else {
            break;
        }
    }

    /* If we have more variables than input, redo */
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        if (gw_hal) gw_hal->puts("?Redo from start\n");
        else fputs("?Redo from start\n", stdout);
        /* Need to re-read, but for simplicity continue */
        goto retry;
    }
}

void gw_stmt_line_input(void)
{
    gw_skip_spaces();

    /* LINE INPUT [;]["prompt";] string-variable */
    bool no_cr = false;
    if (gw_chrgot() == ';') {
        no_cr = true;
        gw_chrget();
        gw_skip_spaces();
    }

    /* Check for prompt string */
    if (gw_chrgot() == '"') {
        gw.text_ptr++;
        while (*gw.text_ptr && *gw.text_ptr != '"') {
            if (gw_hal) gw_hal->putch(*gw.text_ptr);
            else putchar(*gw.text_ptr);
            gw.text_ptr++;
        }
        if (*gw.text_ptr == '"')
            gw.text_ptr++;
        gw_skip_spaces();
        if (gw_chrgot() == ';')
            gw_chrget();
    }

    char name[GW_NAME_SIZE];
    gw_valtype_t type = gw_parse_varname(name);
    if (type != VT_STR)
        gw_error(ERR_TM);

    fflush(stdout);
    char *line = read_input_line(no_cr);
    if (!line) line = "";

    gw_value_t val;
    val.type = VT_STR;
    val.sval = gw_str_from_cstr(line);

    gw_skip_spaces();
    var_entry_t *var;
    if (gw_chrgot() == '(') {
        gw_value_t *elem = gw_array_element(name, type);
        gw_str_free(&elem->sval);
        elem->sval = val.sval;
        elem->type = VT_STR;
    } else {
        var = gw_var_find_or_create(name, type);
        gw_var_assign(var, &val);
    }
}

/* INPUT$ converts its count before the file expression, but rejects count zero
 * only after validating the file and closing parenthesis. Keep those stages
 * separate so errors and argument side effects retain their original order. */
int gw_input_byte(double value)
{
    int n = gw_cint(value);
    if (n < 0 || n > 255) gw_error(ERR_FC);
    return n;
}

static void input_break(void)
{
    if (tui.active) {
        tui.break_flag = true;
        tui_check_break();
    }
    if (gw_hal) gw_hal->puts("\nBreak\n");
    gw.running = false;
    longjmp(gw_error_jmp, -1);
}

gw_string_t gw_input_chars(int count, int filenum)
{
    file_entry_t *file = NULL;
    if (filenum != -1) {
        if (filenum == 0) gw_error(ERR_BN);
        if (filenum < 0 || filenum > 255) gw_error(ERR_FC);
        if (filenum > 15 || !gw.files[filenum].fp) gw_error(ERR_BM);
        file = &gw.files[filenum];
        if (file->mode != 'I' && file->mode != 'R') gw_error(ERR_BM);
    }
    if (count < 1 || count > 255) gw_error(ERR_FC);
    char buf[255];
    int n = 0;
    if (!file && tui.active) {
        tui_refresh();
        tui_update_cursor();
    }
    if (!file && !tui.active && gw_hal && gw_hal->read_chars) {
        n = gw_hal->read_chars(buf, count);
        if (n == -2) input_break();
        if (n == -3) n = 0;
        else if (n != count) gw_error(ERR_EF);
    }
    while (n < count) {
        int ch;
        if (file) {
            if (file->eof_flag) ch = EOF;
            else if (file->mode == 'R') {
                if (file->input_pos >= file->record_len) gw_error(ERR_FO);
                ch = file->field_buf[file->input_pos++];
            } else ch = fgetc(file->fp);
            if (ch == EOF || ch == 26) {
                file->eof_flag = true;
                gw_error(ERR_EF);
            }
        } else {
            ch = tui_pop_key();
            if (ch < 0) {
                if (tui.active) ch = tui_read_key();
                else if (gw_hal) ch = gw_hal->getch();
                else ch = getchar();
            }
            if (ch < 0) gw_error(ERR_EF);
            if (ch == 3) input_break();
            if (ch > 255) continue;  /* INPUT$ ignores extended/function keys. */
        }
        buf[n++] = (char)ch;
    }
    gw_string_t result = gw_str_alloc(count);
    memcpy(result.data, buf, count);
    return result;
}

/* RANDOMIZE reads a numeric literal, not an expression. Tokenizing only the
 * literal reuses BASIC's decimal, D-exponent, hex/octal and type-suffix rules
 * without borrowing or changing the interpreter's statement/retry cursor. */
static bool randomize_literal(const char *line, double *value)
{
    char compact[258];
    int len = 0;
    for (; *line; line++)
        if (*line != ' ' && *line != '\t') compact[len++] = *line;
    /* FIN treats an omitted exponent as zero, including 1E and 1D+. */
    int exponent = len - 1;
    if (exponent >= 0 && (compact[exponent] == '+' || compact[exponent] == '-')) exponent--;
    if (exponent > 0 && (toupper((unsigned char)compact[exponent]) == 'E' ||
                         toupper((unsigned char)compact[exponent]) == 'D')) compact[len++] = '0';
    compact[len] = 0;
    uint8_t tokens[300];
    if (gw_crunch(compact, tokens, sizeof(tokens)) < 0) return false;
    uint8_t *p = tokens;
    while (*p == ' ') p++;
    int sign = 1;
    if (*p == TOK_PLUS || *p == TOK_MINUS) {
        if (*p == TOK_MINUS) sign = -1;
        p++;
        while (*p == ' ') p++;
    }
    double v = 0;
    if (*p >= 0x11 && *p <= 0x1a) v = *p++ - 0x11;
    else if (*p == TOK_INT1) { v = p[1]; p += 2; }
    else if (*p == TOK_INT2) { v = (int16_t)(p[1] | ((uint16_t)p[2] << 8)); p += 3; }
    else if (*p == TOK_CONST_SNG) { float f; memcpy(&f, p + 1, 4); v = f; p += 5; }
    else if (*p == TOK_CONST_DBL) { memcpy(&v, p + 1, 8); p += 9; }
    else if (*p) return false;
    while (*p == ' ') p++;
    if (*p) return false;
    *value = sign * v;
    return true;
}

void gw_randomize_prompt(void)
{
    for (;;) {
        const char *prompt = "Random number seed (-32768 to 32767)? ";
        if (gw_hal) gw_hal->puts(prompt); else fputs(prompt, stdout);
        fflush(stdout);
        char *line = read_input_line(false);
        if (!line) gw_error(ERR_EF);
        double value;
        if (!randomize_literal(line, &value)) continue;
        gw_randomize(gw_cint(value));
        return;
    }
}
