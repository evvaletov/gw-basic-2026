#ifndef STRPOOL_H
#define STRPOOL_H

#include <stddef.h>
#include <stdbool.h>

#define STRPOOL_DEFAULT_SIZE  (32 * 1024)  /* 32KB — generous vs original's ~3KB */
#define STRPOOL_GC_THRESHOLD  4096         /* GC when less than 4KB free */

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
