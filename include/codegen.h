#ifndef CODEGEN_H
#define CODEGEN_H

#include "analysis.h"
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    bool safe_mode;     /* --safe: emit runtime safety checks */
    bool warn_mode;     /* --warn: static analysis warnings */
    bool no_gc_check;   /* --no-gc-check: skip gwrt_check_line per line
                         * (no string-pool GC, no Ctrl+Break check) */
    bool fast_math;     /* --fast-math: skip / by-zero checks */
    const char *main_name;  /* --main-name: rename entry point from
                             * "main" to NAME (for cross-language link;
                             * NULL keeps "main") */
    size_t max_output_bytes; /* --max-output-size: abort codegen once this many
                              * bytes have been written to the output file (a
                              * codegen loop can otherwise emit unbounded C and
                              * fill the disk); 0 = unlimited */
    const char *out_path;   /* path of the output file, removed if the limit is
                             * hit so the partial file is not left behind
                             * (NULL when emitting to stdout) */
} codegen_opts_t;

/* Generate C source from the analyzed program */
void codegen_emit(FILE *out, analysis_t *a, const codegen_opts_t *opts);

#endif
