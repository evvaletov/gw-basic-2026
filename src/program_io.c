#include "gwbasic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/*
 * GW-BASIC binary file format:
 *   Header:  0xFF (tokenized) | 0xFE (protected) | 0x00 (ASCII)
 *   Lines:   [2-byte next-ptr][2-byte linenum][tokens...][0x00]
 *   End:     [0x00 0x00] [0x1A]
 *
 * The next-ptr is an absolute offset from the start of file data.  For our
 * purposes we compute it sequentially — the original GW-BASIC used memory
 * addresses here, but only the line number and token data matter on load.
 *
 * Floating-point constants are stored in our native IEEE format, not MBF.
 * This means files saved here are compatible with this interpreter but not
 * directly with the original GWBASIC.EXE binary format.
 */

#define BIN_HEADER_TOKEN  0xFF
#define BIN_HEADER_PROTECT 0xFE
#define BIN_EOF_MARKER    0x1A

static void write16(FILE *fp, uint16_t val)
{
    fputc(val & 0xFF, fp);
    fputc((val >> 8) & 0xFF, fp);
}

static uint16_t read16(FILE *fp)
{
    int lo = fgetc(fp);
    int hi = fgetc(fp);
    if (lo == EOF || hi == EOF) return 0;
    return (uint16_t)(lo | (hi << 8));
}

/* Save program in tokenized binary format */
static void save_binary(FILE *fp)
{
    fputc(BIN_HEADER_TOKEN, fp);

    /* First pass: compute the offset base (1 byte for header) */
    uint16_t offset = 1;

    program_line_t *p = gw.prog_head;
    while (p) {
        /* next-ptr(2) + linenum(2) + tokens(len) + null(1) */
        uint16_t line_size = 2 + 2 + p->len + 1;
        offset += line_size;
        write16(fp, offset);      /* next-line pointer */
        write16(fp, p->num);      /* line number */
        fwrite(p->tokens, 1, p->len, fp);
        fputc(0x00, fp);          /* null terminator */
        p = p->next;
    }

    /* End-of-program marker */
    write16(fp, 0x0000);
    fputc(BIN_EOF_MARKER, fp);
}

/* Save program in ASCII format */
static void save_ascii(FILE *fp)
{
    char listbuf[512];
    program_line_t *p = gw.prog_head;
    while (p) {
        gw_list_line(p->tokens, p->len, listbuf, sizeof(listbuf));
        fprintf(fp, "%u %s\n", p->num, listbuf);
        p = p->next;
    }
}

/* SAVE "filename" [,A] [,P]
 * Default = binary tokenized.  ,A = ASCII.  ,P = protected (stub). */
void gw_stmt_save(void)
{
    gw_skip_spaces();
    gw_value_t fname_val = gw_eval_str();
    char *filename = gw_str_to_cstr(&fname_val.sval);
    gw_str_free(&fname_val.sval);

    char mode = 'B';  /* default: binary */
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        gw_chrget();
        gw_skip_spaces();
        char flag = toupper(gw_chrgot());
        if (flag == 'A') mode = 'A';
        else if (flag == 'P') mode = 'P';
        if (gw_is_letter(gw_chrgot()))
            gw_chrget();
    }

    FILE *fp = fopen(filename, mode == 'A' ? "w" : "wb");
    if (!fp) {
        free(filename);
        gw_error(ERR_IO);
    }
    free(filename);

    if (mode == 'A')
        save_ascii(fp);
    else
        save_binary(fp);  /* P treated same as B for now */

    fclose(fp);
}

/* Load binary tokenized file */
static void load_binary(FILE *fp)
{
    for (;;) {
        uint16_t next_ptr = read16(fp);
        if (next_ptr == 0) break;  /* end of program */

        uint16_t linenum = read16(fp);

        /* Read token data until null terminator */
        uint8_t tokbuf[300];
        int len = 0;
        int ch;
        while ((ch = fgetc(fp)) != EOF && ch != 0x00 && len < (int)sizeof(tokbuf) - 1)
            tokbuf[len++] = (uint8_t)ch;

        if (len > 0)
            gw_store_line(linenum, tokbuf, len);
    }
}

/* Load ASCII text file */
static void load_ascii(FILE *fp)
{
    char buf[256];
    while (fgets(buf, sizeof(buf), fp)) {
        int len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (buf[0] == '\0') continue;

        int clen = gw_crunch(buf, gw.kbuf, sizeof(gw.kbuf));

        /* Parse line number from tokenized form */
        uint8_t *tp = gw.kbuf;
        while (*tp == ' ') tp++;
        uint8_t tok = *tp;
        uint16_t num;
        int skip = 0;

        if (tok >= 0x11 && tok <= 0x1A) {
            num = tok - 0x11;
            skip = (tp - gw.kbuf) + 1;
        } else if (tok == 0x0F) {
            num = tp[1];
            skip = (tp - gw.kbuf) + 2;
        } else if (tok == 0x0E) {
            num = (uint16_t)(tp[1] | (tp[2] << 8));
            skip = (tp - gw.kbuf) + 3;
        } else {
            continue;
        }

        int data_len = clen - skip;
        uint8_t *data = gw.kbuf + skip;
        while (*data == ' ' && data_len > 0) { data++; data_len--; }

        if (data_len > 0 && *data != 0)
            gw_store_line(num, data, data_len);
    }
}

/* Clear interpreter state for a fresh LOAD */
static void clear_state(void)
{
    gw_free_program();
    gw_vars_clear();
    gw_arrays_clear();
    gw_file_close_all();
    memset(gw.fn_defs, 0, sizeof(gw.fn_defs));
    gw.for_sp = 0;
    gw.gosub_sp = 0;
    gw.while_sp = 0;
    gw.data_ptr = NULL;
    gw.data_line_ptr = NULL;
    gw.cont_text = NULL;
    gw.cont_line = NULL;
    gw.on_error_line = 0;
    gw.in_error_handler = false;
}

/* Auto-detect format and load. */
void gw_stmt_load_internal(const char *filename, bool clear)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp)
        gw_error(ERR_FF);

    if (clear)
        clear_state();

    /* Peek at first byte to detect format */
    int header = fgetc(fp);
    if (header == BIN_HEADER_TOKEN || header == BIN_HEADER_PROTECT) {
        load_binary(fp);
    } else {
        /* ASCII: rewind and read as text */
        fclose(fp);
        fp = fopen(filename, "r");
        if (!fp) gw_error(ERR_FF);
        load_ascii(fp);
    }

    fclose(fp);
}

/* LOAD "filename" [,R] */
void gw_stmt_load(void)
{
    gw_skip_spaces();
    gw_value_t fname_val = gw_eval_str();
    char *filename = gw_str_to_cstr(&fname_val.sval);
    gw_str_free(&fname_val.sval);

    bool run_after = false;
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        gw_chrget();
        gw_skip_spaces();
        if (gw_is_letter(gw_chrgot()) && toupper(gw_chrgot()) == 'R') {
            run_after = true;
            gw_chrget();
        }
    }

    gw_stmt_load_internal(filename, true);
    free(filename);

    if (run_after && gw.prog_head) {
        gw.cur_line = gw.prog_head;
        gw.text_ptr = gw.prog_head->tokens;
        gw.cur_line_num = gw.prog_head->num;
        gw.running = true;
        gw_run_loop();
    }
}

/* MERGE "filename" - load without clearing existing program */
void gw_stmt_merge(void)
{
    gw_skip_spaces();
    gw_value_t fname_val = gw_eval_str();
    char *filename = gw_str_to_cstr(&fname_val.sval);
    gw_str_free(&fname_val.sval);

    gw_stmt_load_internal(filename, false);
    free(filename);
}
