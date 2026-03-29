/*
 * Runtime library for compiled GW-BASIC programs.
 *
 * Provides the global interpreter state (gw, gw_hal) and core functions
 * (gw_chrget, gw_chrgot, etc.) that the existing modules reference.
 * When used in the interpreter, these are in main.c; in the runtime
 * library, they live here instead.
 */

#include "gwrt.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

/*
 * Global state + core functions that main.c provides in the interpreter.
 * In the runtime library, gwrt.c provides them instead.
 */
interp_state_t gw;
hal_ops_t *gw_hal = NULL;

uint8_t gw_chrget(void)
{
    gw.text_ptr++;
    while (*gw.text_ptr == ' ') gw.text_ptr++;
    return *gw.text_ptr;
}

uint8_t gw_chrgot(void) { return *gw.text_ptr; }

void gw_skip_spaces(void) { while (*gw.text_ptr == ' ') gw.text_ptr++; }

bool gw_is_digit(uint8_t ch) { return ch >= '0' && ch <= '9'; }

bool gw_is_letter(uint8_t ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

void gw_expect(uint8_t expected)
{
    gw_skip_spaces();
    if (gw_chrgot() != expected)
        gw_error(ERR_SN);
    gw_chrget();
}

/* gw_init — same as in main.c, needed by runtime modules */
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

/* Stub for gw_exec_direct — not used in compiled programs but referenced by tui.c */
void gw_exec_direct(const char *line) { (void)line; }

/* DATA pool */
static const char **data_pool;
static int data_index;
static const int *data_line_map;   /* line_num → data_index pairs */
static int data_line_count;

/* GOSUB return-label stack */
static int gosub_stack[GWRT_GOSUB_MAX];
static int gosub_sp;

/* Error handling */
jmp_buf gwrt_error_jmp;
int gwrt_error_target;
int gwrt_resume_label;

void gwrt_init(void)
{
    gw_hal = hal_posix_create();
    gw_hal->init();
    memset(&gw, 0, sizeof(gw));
    for (int i = 0; i < 26; i++)
        gw.def_type[i] = VT_SNG;
    strpool_init(STRPOOL_DEFAULT_SIZE);
    snd_init();
    portio_reset();
    gw.running = true;

    data_pool = NULL;
    data_index = 0;
    data_line_map = NULL;
    data_line_count = 0;
    gosub_sp = 0;
    gwrt_error_target = 0;
    gwrt_resume_label = 0;
}

void gwrt_shutdown(void)
{
    gw_lpt_close();
    snd_shutdown();
    strpool_shutdown();
    if (gw_hal) gw_hal->shutdown();
}

/* --- DATA / READ / RESTORE --- */

void gwrt_data_set(const char **pool, const int *line_map, int line_count)
{
    data_pool = pool;
    data_line_map = line_map;
    data_line_count = line_count;
    data_index = 0;
}

void gwrt_data_restore(int index)
{
    data_index = index;
}

const char *gwrt_data_read(void)
{
    if (!data_pool || !data_pool[data_index])
        gw_error(ERR_OD);
    return data_pool[data_index++];
}

/* --- GOSUB stack --- */

void gwrt_gosub_push(int label)
{
    if (gosub_sp >= GWRT_GOSUB_MAX)
        gw_error(ERR_OM);
    gosub_stack[gosub_sp++] = label;
}

int gwrt_gosub_pop(void)
{
    if (gosub_sp <= 0)
        gw_error(ERR_RG);
    return gosub_stack[--gosub_sp];
}

/* --- Array support --- */

/* Find or auto-create an array, then index into it */
gw_value_t *gwrt_array_elem(const char *name, int type, int ndims, int *subs)
{
    char n[2] = {name[0], name[1]};

    /* Find or auto-create the array */
    array_entry_t *arr = NULL;
    for (int i = 0; i < gw.array_count; i++) {
        if (gw.arrays[i].name[0] == n[0] && gw.arrays[i].name[1] == n[1]
            && (int)gw.arrays[i].type == type) {
            arr = &gw.arrays[i];
            break;
        }
    }
    if (!arr) {
        /* Auto-DIM with default size 10 per dimension */
        if (gw.array_count >= 64) gw_error(ERR_OM);
        arr = &gw.arrays[gw.array_count++];
        arr->name[0] = n[0];
        arr->name[1] = n[1];
        arr->type = type;
        arr->ndims = ndims;
        arr->total_elements = 1;
        for (int d = 0; d < ndims; d++) {
            arr->dims[d] = 10;
            arr->total_elements *= (11 - gw.option_base);
        }
        arr->data = calloc(arr->total_elements, sizeof(gw_value_t));
        if (!arr->data) gw_error(ERR_OM);
        for (int j = 0; j < arr->total_elements; j++)
            arr->data[j].type = type;
    }

    /* Compute flat index (column-major) */
    int index = 0;
    int stride = 1;
    for (int d = 0; d < ndims; d++) {
        int sub = subs[d] - gw.option_base;
        int dim_size = arr->dims[d] + 1 - gw.option_base;
        if (sub < 0 || sub >= dim_size) gw_error(ERR_FC);
        index += sub * stride;
        stride *= dim_size;
    }
    if (index < 0 || index >= arr->total_elements) gw_error(ERR_FC);
    return &arr->data[index];
}

void gwrt_dim(const char *name, int type, int ndims, int *dims)
{
    if (gw.array_count >= 64) gw_error(ERR_OM);
    array_entry_t *arr = &gw.arrays[gw.array_count++];
    arr->name[0] = name[0];
    arr->name[1] = name[1];
    arr->type = type;
    arr->ndims = ndims;
    arr->total_elements = 1;
    for (int d = 0; d < ndims; d++) {
        arr->dims[d] = dims[d];
        arr->total_elements *= (dims[d] + 1 - gw.option_base);
    }
    arr->data = calloc(arr->total_elements, sizeof(gw_value_t));
    if (!arr->data) gw_error(ERR_OM);
    for (int j = 0; j < arr->total_elements; j++)
        arr->data[j].type = type;
}

/* --- Event + GC check (called at each line boundary) --- */

void gwrt_check_line(uint16_t line_num)
{
    gw.cur_line_num = line_num;

    /* String pool GC */
    if (strpool_free() < STRPOOL_GC_THRESHOLD)
        strpool_gc();

    /* Ctrl+Break check */
    if (tui.active)
        tui_check_break();
}

/* --- Print helpers --- */

void gwrt_print_int(int16_t v)
{
    gw_value_t val = {.type = VT_INT, .ival = v};
    gw_print_value(&val);
}

void gwrt_print_sng(float v)
{
    gw_value_t val = {.type = VT_SNG, .fval = v};
    gw_print_value(&val);
}

void gwrt_print_dbl(double v)
{
    gw_value_t val = {.type = VT_DBL, .dval = v};
    gw_print_value(&val);
}

void gwrt_print_str(gw_string_t s)
{
    gw_value_t val = {.type = VT_STR, .sval = s};
    /* gw_print_value frees the string — pass a copy so caller keeps theirs */
    val.sval = gw_str_copy(&s);
    gw_print_value(&val);
}

void gwrt_print_cstr(const char *s)
{
    if (gw_hal) {
        gw_hal->puts(s);
    } else {
        fputs(s, stdout);
    }
}

void gwrt_print_newline(void)
{
    gw_print_newline();
}

void gwrt_print_tab(void)
{
    /* Advance to next 14-column zone (GW-BASIC comma zone) */
    extern int print_col;
    int target = ((print_col / 14) + 1) * 14;
    while (print_col < target) {
        if (gw_hal) gw_hal->putch(' ');
        else putchar(' ');
        print_col++;
    }
}

void gwrt_print_spc(int n)
{
    for (int i = 0; i < n; i++) {
        if (gw_hal) gw_hal->putch(' ');
        else putchar(' ');
    }
}
