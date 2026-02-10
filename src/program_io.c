#include "gwbasic.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* SAVE "filename" [,A] - save program as ASCII text */
void gw_stmt_save(void)
{
    gw_skip_spaces();
    gw_value_t fname_val = gw_eval_str();
    char *filename = gw_str_to_cstr(&fname_val.sval);
    gw_str_free(&fname_val.sval);

    /* We only support ASCII saves (,A is optional/default) */
    gw_skip_spaces();
    if (gw_chrgot() == ',') {
        gw_chrget();
        gw_skip_spaces();
        /* Skip A or P flag */
        if (gw_is_letter(gw_chrgot()))
            gw_chrget();
    }

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        free(filename);
        gw_error(ERR_IO);
    }
    free(filename);

    char listbuf[512];
    program_line_t *p = gw.prog_head;
    while (p) {
        gw_list_line(p->tokens, p->len, listbuf, sizeof(listbuf));
        fprintf(fp, "%u %s\n", p->num, listbuf);
        p = p->next;
    }
    fclose(fp);
}

/* Helper: load lines from a file into the program, optionally clearing first */
void gw_stmt_load_internal(const char *filename, bool clear);

void gw_stmt_load_internal(const char *filename, bool clear)
{
    FILE *fp = fopen(filename, "r");
    if (!fp)
        gw_error(ERR_FF);

    if (clear) {
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
            continue;  /* skip non-numbered lines */
        }

        int data_len = clen - skip;
        uint8_t *data = gw.kbuf + skip;
        while (*data == ' ' && data_len > 0) { data++; data_len--; }

        if (data_len > 0 && *data != 0)
            gw_store_line(num, data, data_len);
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
