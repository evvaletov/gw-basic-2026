#ifndef CODEGEN_H
#define CODEGEN_H

#include "analysis.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    bool safe_mode;     /* --safe: emit runtime safety checks */
    bool warn_mode;     /* --warn: static analysis warnings */
} codegen_opts_t;

/* Generate C source from the analyzed program */
void codegen_emit(FILE *out, analysis_t *a, const codegen_opts_t *opts);

#endif
