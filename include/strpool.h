#ifndef STRPOOL_H
#define STRPOOL_H

#include <stddef.h>
#include <stdbool.h>

#ifdef _M_I86
/* Program lines, variables, and the GC scratch buffer share 64KB of near data. */
#define STRPOOL_DEFAULT_SIZE  (8 * 1024)
#define STRPOOL_GC_THRESHOLD  1024
#else
#define STRPOOL_DEFAULT_SIZE  (32 * 1024)
#define STRPOOL_GC_THRESHOLD  4096
#endif

void strpool_init(size_t size);
void strpool_shutdown(void);
void strpool_reset(size_t size);    /* resize and clear (0 = keep current size) */
char *strpool_alloc(int len);       /* bump-allocate from pool */
void strpool_gc(void);              /* compact: walk vars+arrays, squeeze out dead space */
size_t strpool_free(void);          /* bytes available */
bool strpool_owns(const char *p);   /* true if p points into the pool */
void strpool_pin(void);             /* prevent GC from running (nestable) */
void strpool_unpin(void);           /* allow GC again */

#endif
