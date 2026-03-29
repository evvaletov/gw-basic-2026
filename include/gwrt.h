#ifndef GWRT_H
#define GWRT_H

/*
 * Runtime library for compiled GW-BASIC programs.
 *
 * Provides initialization, DATA/READ support, GOSUB return-label stack,
 * and convenience wrappers. Compiled programs link against libgwrt.a
 * which contains all the existing interpreter modules except the
 * execution loop.
 */

#include "gwbasic.h"
#include "strpool.h"
#include "portio.h"
#include "sound.h"
#include <setjmp.h>

/* Initialization / shutdown */
void gwrt_init(void);
void gwrt_shutdown(void);

/* DATA / READ / RESTORE */
void gwrt_data_set(const char **pool, const int *line_map, int line_count);
void gwrt_data_restore(int index);
const char *gwrt_data_read(void);     /* returns next datum or errors ERR_OD */

/* GOSUB return-label stack */
#define GWRT_GOSUB_MAX 24
void gwrt_gosub_push(int label);
int  gwrt_gosub_pop(void);

/* FOR/NEXT stack (for NEXT without variable matching) */
#define GWRT_FOR_MAX 16

/* Event checking (ON TIMER, ON KEY, Ctrl+Break) + string pool GC */
void gwrt_check_line(uint16_t line_num);

/* Error handling */
extern jmp_buf gwrt_error_jmp;
extern int gwrt_error_target;    /* ON ERROR GOTO label, 0 = none */
extern int gwrt_resume_label;    /* label for RESUME NEXT */

/* Print helpers (wrappers around existing print.c) */
void gwrt_print_sng(float v);
void gwrt_print_dbl(double v);
void gwrt_print_int(int16_t v);
void gwrt_print_str(gw_string_t s);
void gwrt_print_cstr(const char *s);
void gwrt_print_newline(void);
void gwrt_print_tab(void);      /* comma zone */
void gwrt_print_spc(int n);

#endif
