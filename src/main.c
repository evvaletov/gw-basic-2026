#include "gwbasic.h"
#include "tui.h"
#include "sound.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

/* Global interpreter state */
interp_state_t gw;
hal_ops_t *gw_hal = NULL;

void gw_init(void)
{
    gw_free_program();
    gw_vars_clear();
    gw_arrays_clear();
    memset(&gw, 0, sizeof(gw));
    gw.cur_line_num = LINE_DIRECT;

    for (int i = 0; i < 26; i++)
        gw.def_type[i] = VT_SNG;
}

/* CHRGET: advance text pointer, skip spaces, return current byte */
uint8_t gw_chrget(void)
{
    gw.text_ptr++;
    while (*gw.text_ptr == ' ')
        gw.text_ptr++;
    return *gw.text_ptr;
}

/* CHRGOT: return current byte without advancing */
uint8_t gw_chrgot(void)
{
    return *gw.text_ptr;
}

void gw_skip_spaces(void)
{
    while (*gw.text_ptr == ' ')
        gw.text_ptr++;
}

bool gw_is_letter(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

bool gw_is_digit(uint8_t ch)
{
    return ch >= '0' && ch <= '9';
}

void gw_expect(uint8_t tok)
{
    gw_skip_spaces();
    if (gw_chrgot() != tok)
        gw_error(ERR_SN);
    gw_chrget();
}

/* Check if tokenized data starts with a line number */
static int parse_line_number(uint8_t *tokens, uint16_t *num)
{
    uint8_t *p = tokens;
    while (*p == ' ') p++;

    uint8_t tok = *p;

    /* Literal 0-9 */
    if (tok >= 0x11 && tok <= 0x1A) {
        *num = tok - 0x11;
        return (p - tokens) + 1;
    }
    /* One-byte int */
    if (tok == TOK_INT1) {
        *num = p[1];
        return (p - tokens) + 2;
    }
    /* Two-byte int */
    if (tok == TOK_INT2) {
        *num = (uint16_t)(p[1] | (p[2] << 8));
        return (p - tokens) + 3;
    }

    return 0;
}

/* Execute a direct mode line: detect line numbers -> store; otherwise execute */
void gw_exec_direct(const char *line)
{
    gw.cur_line_num = LINE_DIRECT;
    gw.cur_line = NULL;
    int len = gw_crunch(line, gw.kbuf, sizeof(gw.kbuf));

    /* Check for line number -> program storage */
    uint16_t num;
    int skip = parse_line_number(gw.kbuf, &num);
    if (skip > 0) {
        int data_len = len - skip;
        /* Skip spaces after line number */
        uint8_t *data = gw.kbuf + skip;
        while (*data == ' ' && data_len > 0) { data++; data_len--; }

        /* Invalidate CONT when editing program */
        gw.cont_text = NULL;
        gw.cont_line = NULL;

        if (data_len <= 0 || *data == 0) {
            gw_delete_line(num);
        } else {
            gw_store_line(num, data, data_len);
        }
        return;
    }

    /* Direct mode execution with multi-statement support */
    gw.text_ptr = gw.kbuf;
    for (;;) {
        gw_skip_spaces();
        if (*gw.text_ptr == 0)
            break;
        if (*gw.text_ptr == ':') {
            gw.text_ptr++;
            continue;
        }
        /* Skip ELSE during normal flow (IF true path continuing) */
        if (*gw.text_ptr == TOK_ELSE) {
            while (*gw.text_ptr) gw.text_ptr++;
            break;
        }
        gw_exec_stmt();
        if (gw.running)
            break;  /* RUN/CONT started, loop handles it */
    }
}

static char *read_line(void)
{
    static char buf[256];
    if (fgets(buf, sizeof(buf), stdin) == NULL)
        return NULL;
    int len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return buf;
}

static void print_banner(int interactive)
{
    if (!interactive)
        return;
    const char *banner =
        "GW-BASIC 2026 " GW_VERSION "\n"
        "(C) Eremey Valetov 2026. MIT License.\n"
        "Based on Microsoft GW-BASIC assembly source.\n";
    if (gw_hal)
        gw_hal->puts(banner);
    else
        fputs(banner, stdout);
}

int main(int argc, char **argv)
{
    gw_hal = hal_posix_create();
    gw_hal->init();
    gw_init();
    snd_init();

    int interactive = isatty(fileno(stdin));

    const char *filename = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: gwbasic [options] [file.bas]\n"
                   "Options:\n"
                   "  -h, --help     Show this help\n"
                   "  -v, --version  Show version\n");
            return 0;
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-v") == 0) {
            printf("GW-BASIC %s\n", GW_VERSION);
            return 0;
        }
        if (argv[i][0] != '-') {
            filename = argv[i];
        }
    }

    /* Load file: read lines, store numbered lines, then RUN */
    if (filename) {
        FILE *f = fopen(filename, "r");
        if (!f) {
            fprintf(stderr, "File not found: %s\n", filename);
            return 1;
        }
        char buf[256];
        while (fgets(buf, sizeof(buf), f)) {
            int len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = '\0';
            if (buf[0] == '\0') continue;

            if (setjmp(gw_error_jmp) != 0)
                continue;
            gw_exec_direct(buf);
        }
        fclose(f);

        /* If program was loaded but RUN wasn't in the file, auto-run */
        if (gw.prog_head && !gw.running) {
            if (setjmp(gw_error_jmp) != 0) {
                /* error during auto-run */
            } else {
                gw_exec_direct("RUN");
            }
        }

        if (!interactive) {
            snd_shutdown();
            if (gw_hal) gw_hal->shutdown();
            return 0;
        }
    }

    /* Initialize TUI for interactive sessions */
    if (interactive)
        tui_init();

    print_banner(interactive);

    for (;;) {
        if (interactive && !gw.auto_mode) {
            gw_hal->puts("Ok\n");
        }

        if (setjmp(gw_error_jmp) != 0) {
            gw.auto_mode = false;
            continue;
        }

        /* AUTO mode: display line number prompt and prepend it to input */
        if (gw.auto_mode && interactive) {
            char prompt[16];
            bool exists = gw_find_line(gw.auto_line) != NULL;
            snprintf(prompt, sizeof(prompt), "%u%s", gw.auto_line,
                     exists ? "*" : " ");
            gw_hal->puts(prompt);

            char *line = tui_read_line();
            if (!line || tui.break_flag) {
                gw.auto_mode = false;
                tui.break_flag = false;
                continue;
            }
            /* Empty input exits AUTO mode */
            if (line[0] == '\0') {
                gw.auto_mode = false;
                continue;
            }

            char full[300];
            snprintf(full, sizeof(full), "%u %s", gw.auto_line, line);
            gw_exec_direct(full);

            uint32_t next = (uint32_t)gw.auto_line + gw.auto_inc;
            if (next > 65529) {
                gw.auto_mode = false;
            } else {
                gw.auto_line = (uint16_t)next;
            }
            continue;
        }

        char *line = interactive ? tui_read_line() : read_line();
        if (!line)
            break;

        if (line[0] == '\0')
            continue;

        gw_exec_direct(line);
    }

    if (interactive)
        tui_shutdown();
    snd_shutdown();
    if (gw_hal)
        gw_hal->shutdown();
    return 0;
}
