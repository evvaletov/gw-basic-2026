#include "gwbasic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>

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
 * Floating-point constants are stored in Microsoft Binary Format (MBF) on disk
 * and IEEE 754 in memory, matching the original GWBASIC.EXE binary format.
 * Conversion happens at the load_binary()/save_binary() boundary.
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

/*
 * Walk a token buffer and convert float constants between IEEE and MBF.
 * direction: 0 = IEEE→MBF (for saving), 1 = MBF→IEEE (for loading).
 */
static void convert_floats(uint8_t *tok, int len, int direction)
{
    int i = 0;
    while (i < len) {
        uint8_t ch = tok[i];

        /* REM or ' — rest of line is literal text, stop scanning */
        if (ch == TOK_REM || ch == TOK_SQUOTE)
            return;

        /* String literal — skip to closing quote */
        if (ch == '"') {
            i++;
            while (i < len && tok[i] != '"')
                i++;
            if (i < len) i++;  /* skip closing quote */
            continue;
        }

        /* Multi-byte token prefixes: skip the prefix + 1 sub-token byte */
        if (ch == TOK_PREFIX_FD || ch == TOK_PREFIX_FE || ch == TOK_PREFIX_FF) {
            i += 2;
            continue;
        }

        /* 2-byte integer constant */
        if (ch == TOK_INT2) {
            i += 3;  /* prefix + 2 data bytes */
            continue;
        }

        /* 1-byte integer constant */
        if (ch == TOK_INT1) {
            i += 2;  /* prefix + 1 data byte */
            continue;
        }

        /* Single-precision float constant — convert 4 bytes */
        if (ch == TOK_CONST_SNG && i + 4 < len) {
            if (direction == 0) {
                /* IEEE → MBF */
                float f;
                memcpy(&f, &tok[i + 1], 4);
                mbf_single_t mbf = gw_ieee_to_mbf_single(f);
                memcpy(&tok[i + 1], &mbf, 4);
            } else {
                /* MBF → IEEE */
                mbf_single_t mbf;
                memcpy(&mbf, &tok[i + 1], 4);
                float f = gw_mbf_to_ieee_single(mbf);
                memcpy(&tok[i + 1], &f, 4);
            }
            i += 5;
            continue;
        }

        /* Double-precision float constant — convert 8 bytes */
        if (ch == TOK_CONST_DBL && i + 8 < len) {
            if (direction == 0) {
                double d;
                memcpy(&d, &tok[i + 1], 8);
                mbf_double_t mbf = gw_ieee_to_mbf_double(d);
                memcpy(&tok[i + 1], &mbf, 8);
            } else {
                mbf_double_t mbf;
                memcpy(&mbf, &tok[i + 1], 8);
                double d = gw_mbf_to_ieee_double(mbf);
                memcpy(&tok[i + 1], &d, 8);
            }
            i += 9;
            continue;
        }

        i++;
    }
}

/* Save program in tokenized binary format */
static void save_binary(FILE *fp)
{
    fputc(BIN_HEADER_TOKEN, fp);

    /* First pass: compute the offset base (1 byte for header) */
    uint16_t offset = 1;

    program_line_t *p = gw.prog_head;
    while (p) {
        if ((uint32_t)offset + p->len + 5 > 65535UL) {
            fclose(fp);
            gw_error(ERR_OM);
        }
        uint8_t *cvtbuf = malloc(p->len ? p->len : 1);
        if (!cvtbuf) { fclose(fp); gw_error(ERR_OM); }
        /* next-ptr(2) + linenum(2) + tokens(len) + null(1) */
        uint16_t line_size = 2 + 2 + p->len + 1;
        offset += line_size;
        write16(fp, offset);      /* next-line pointer */
        write16(fp, p->num);      /* line number */
        /* Convert IEEE→MBF on a copy so in-memory tokens stay IEEE */
        memcpy(cvtbuf, p->tokens, p->len);
        convert_floats(cvtbuf, p->len, 0);
        fwrite(cvtbuf, 1, p->len, fp);
        free(cvtbuf);
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

/* Load binary tokenized file.
 * Uses the next-line pointer to determine token length rather than scanning
 * for 0x00, since token data (especially MBF floats) can contain null bytes. */
static void load_binary(FILE *fp)
{
    uint16_t offset = 1;  /* file position: 1 byte header already consumed */

    for (;;) {
        uint16_t next_ptr = read16(fp);
        if (next_ptr == 0) break;

        uint16_t linenum = read16(fp);

        /* next-ptr(2) + linenum(2) + tokens(?) + null(1) = next_ptr - offset */
        long tok_len = (long)next_ptr - (long)offset - 5;
        if (tok_len < 0 || tok_len >= INT_MAX || linenum > GW_MAX_LINE) {
            fclose(fp);
            gw_error(ERR_SN);
        }
        uint8_t *tokbuf = malloc((size_t)tok_len + 1);
        if (!tokbuf) { fclose(fp); gw_error(ERR_OM); }
        if (fread(tokbuf, 1, (size_t)tok_len, fp) != (size_t)tok_len || fgetc(fp) != 0) {
            free(tokbuf);
            fclose(fp);
            gw_error(ERR_SN);
        }

        offset = next_ptr;

        if (tok_len > 0) {
            convert_floats(tokbuf, tok_len, 1);  /* MBF → IEEE */
            gw_store_line(linenum, tokbuf, tok_len);
        }
        free(tokbuf);
    }
}

/* Load ASCII text file */
static void load_ascii(FILE *fp)
{
    char buf[256];
    uint8_t tokbuf[300];  /* Keep the active direct command in gw.kbuf intact. */
    while (fgets(buf, sizeof(buf), fp)) {
        int len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        if (buf[0] == '\0') continue;

        uint16_t num;
        int skip = gw_parse_line_number(buf, &num);
        if (skip < 0) {
            fclose(fp);
            gw_error(ERR_SN);
        }
        if (!skip) continue;
        int clen = gw_crunch(buf + skip, tokbuf, sizeof(tokbuf));
        if (clen < 0) {
            fclose(fp);
            gw_error(ERR_BO);
        }

        int data_len = clen;
        uint8_t *data = tokbuf;
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
    /* Keep the name alive across CLEAR without leaking it on load errors. */
    char filename[256];
    if (fname_val.sval.len) memcpy(filename, fname_val.sval.data, fname_val.sval.len);
    filename[fname_val.sval.len] = '\0';
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
    char filename[256];
    if (fname_val.sval.len) memcpy(filename, fname_val.sval.data, fname_val.sval.len);
    filename[fname_val.sval.len] = '\0';
    gw_str_free(&fname_val.sval);

    gw_stmt_load_internal(filename, false);
}
