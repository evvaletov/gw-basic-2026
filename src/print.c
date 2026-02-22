#include "gwbasic.h"
#include <string.h>
#include <stdio.h>

/* Screen print column tracking */
static int print_col = 0;

/* Printer (LPT1) state */
static FILE *lpt_fp;
static const char *lpt_path;
static int lpt_col = 0;

#define LPT_DEFAULT_FILE "LPT1.TXT"
#define LPT_WIDTH 80

void gw_lpt_set_path(const char *path)
{
    lpt_path = path;
}

static FILE *lpt_ensure_open(void)
{
    if (lpt_fp)
        return lpt_fp;
    const char *path = lpt_path ? lpt_path : LPT_DEFAULT_FILE;
    lpt_fp = fopen(path, "a");
    if (!lpt_fp)
        gw_error(ERR_DF);
    return lpt_fp;
}

void gw_lpt_close(void)
{
    if (lpt_fp) {
        fclose(lpt_fp);
        lpt_fp = NULL;
    }
}

/* ---- output helpers ---- */

static void print_char(int ch)
{
    if (gw_hal)
        gw_hal->putch(ch);
    else
        putchar(ch);
    if (ch == '\n')
        print_col = 0;
    else
        print_col++;
}

static void lpt_char(FILE *fp, int ch)
{
    fputc(ch, fp);
    if (ch == '\n')
        lpt_col = 0;
    else
        lpt_col++;
}

void gw_print_newline(void)
{
    print_char('\n');
}

void gw_print_value(gw_value_t *v)
{
    if (v->type == VT_STR) {
        for (int i = 0; i < v->sval.len; i++)
            print_char(v->sval.data[i]);
        gw_str_free(&v->sval);
    } else {
        char buf[64];
        gw_format_number(v, buf, sizeof(buf));
        for (const char *p = buf; *p; p++)
            print_char(*p);
        print_char(' ');
    }
}

static void lpt_value(FILE *fp, gw_value_t *v)
{
    if (v->type == VT_STR) {
        fwrite(v->sval.data, 1, v->sval.len, fp);
        lpt_col += v->sval.len;
        gw_str_free(&v->sval);
    } else {
        char buf[64];
        gw_format_number(v, buf, sizeof(buf));
        fputs(buf, fp);
        fputc(' ', fp);
        lpt_col += strlen(buf) + 1;
    }
}

/*
 * PRINT statement - reimplements the PRINT code from GW-BASIC.
 * Handles: PRINT expr, expr; expr, TAB(n), SPC(n)
 */
void gw_stmt_print(void)
{
    gw_skip_spaces();
    if (gw_chrgot() == TOK_USING) {
        gw_chrget();
        gw_print_using(NULL);
        return;
    }

    int need_newline = 1;
    int screen_width = 80;
    if (gw_hal)
        screen_width = gw_hal->screen_width;

    for (;;) {
        gw_skip_spaces();
        uint8_t tok = gw_chrgot();

        if (tok == 0 || tok == ':' || tok == TOK_ELSE)
            break;

        if (tok == ';') {
            gw_chrget();
            need_newline = 0;
            continue;
        }

        if (tok == ',') {
            gw_chrget();
            int zone_width = 14;
            int target = ((print_col / zone_width) + 1) * zone_width;
            if (target >= screen_width) {
                gw_print_newline();
            } else {
                while (print_col < target)
                    print_char(' ');
            }
            need_newline = 0;
            continue;
        }

        if (tok == TOK_TAB) {
            gw_chrget();
            int n = gw_eval_int();
            gw_skip_spaces();
            gw_expect_rparen();
            if (n < 1) n = 1;
            n--;
            if (n > print_col) {
                while (print_col < n)
                    print_char(' ');
            }
            need_newline = 0;
            continue;
        }

        if (tok == TOK_SPC) {
            gw_chrget();
            int n = gw_eval_int();
            gw_skip_spaces();
            gw_expect_rparen();
            if (n < 0) n = 0;
            for (int i = 0; i < n; i++)
                print_char(' ');
            need_newline = 0;
            continue;
        }

        gw_value_t v = gw_eval();
        gw_print_value(&v);
        need_newline = 1;
    }

    if (need_newline)
        gw_print_newline();
}

/*
 * LPRINT - identical format logic to PRINT but outputs to printer.
 * On real hardware this goes to LPT1; on modern systems, to LPT1.TXT
 * or a device specified with --lpt.
 */
void gw_stmt_lprint(void)
{
    FILE *fp = lpt_ensure_open();

    gw_skip_spaces();
    if (gw_chrgot() == TOK_USING) {
        gw_chrget();
        gw_print_using(fp);
        fflush(fp);
        return;
    }

    int need_newline = 1;

    for (;;) {
        gw_skip_spaces();
        uint8_t tok = gw_chrgot();

        if (tok == 0 || tok == ':' || tok == TOK_ELSE)
            break;

        if (tok == ';') {
            gw_chrget();
            need_newline = 0;
            continue;
        }

        if (tok == ',') {
            gw_chrget();
            int zone_width = 14;
            int target = ((lpt_col / zone_width) + 1) * zone_width;
            if (target >= LPT_WIDTH) {
                lpt_char(fp, '\n');
            } else {
                while (lpt_col < target)
                    lpt_char(fp, ' ');
            }
            need_newline = 0;
            continue;
        }

        if (tok == TOK_TAB) {
            gw_chrget();
            int n = gw_eval_int();
            gw_skip_spaces();
            gw_expect_rparen();
            if (n < 1) n = 1;
            n--;
            if (n > lpt_col) {
                while (lpt_col < n)
                    lpt_char(fp, ' ');
            }
            need_newline = 0;
            continue;
        }

        if (tok == TOK_SPC) {
            gw_chrget();
            int n = gw_eval_int();
            gw_skip_spaces();
            gw_expect_rparen();
            if (n < 0) n = 0;
            for (int i = 0; i < n; i++)
                lpt_char(fp, ' ');
            need_newline = 0;
            continue;
        }

        gw_value_t v = gw_eval();
        lpt_value(fp, &v);
        need_newline = 1;
    }

    if (need_newline)
        lpt_char(fp, '\n');
    fflush(fp);
}

/*
 * LLIST - list program to printer.
 * Same range syntax as LIST but output goes to the printer device/file.
 */
void gw_stmt_llist(void)
{
    FILE *fp = lpt_ensure_open();

    gw_skip_spaces();
    uint16_t start = 0, end = 65535;

    if (gw_chrgot() != 0 && gw_chrgot() != ':') {
        if (gw_chrgot() == TOK_MINUS) {
            gw_chrget();
            if (!read_linenum(&end)) gw_error(ERR_SN);
        } else {
            if (!read_linenum(&start)) gw_error(ERR_SN);
            end = start;
            gw_skip_spaces();
            if (gw_chrgot() == TOK_MINUS) {
                gw_chrget();
                gw_skip_spaces();
                if (gw_chrgot() != 0 && gw_chrgot() != ':') {
                    if (!read_linenum(&end)) gw_error(ERR_SN);
                } else {
                    end = 65535;
                }
            }
        }
    }

    char listbuf[512];
    program_line_t *p = gw.prog_head;
    while (p) {
        if (p->num >= start && p->num <= end) {
            gw_list_line(p->tokens, p->len, listbuf, sizeof(listbuf));
            fprintf(fp, "%u %s\n", p->num, listbuf);
        }
        if (p->num > end)
            break;
        p = p->next;
    }
    fflush(fp);
}
