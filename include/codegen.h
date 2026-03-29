#ifndef CODEGEN_H
#define CODEGEN_H

#include "analysis.h"
#include <stdio.h>

/* Generate C source from the analyzed program */
void codegen_emit(FILE *out, analysis_t *a);

#endif
