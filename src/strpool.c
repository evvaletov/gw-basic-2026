/*
 * String space pool with compacting garbage collector.
 *
 * Original GW-BASIC stored all string data in a contiguous "string space"
 * (GETSPA/GARBAG in GWEVAL.ASM).  Allocation was a bump pointer; when the
 * space filled, GARBAG compacted it by scanning every live string descriptor
 * (variables, arrays, temporaries) and sliding live data toward the bottom.
 *
 * This reimplementation follows the same model.  The pool is a flat char
 * buffer.  gw_str_alloc() bumps a pointer; gw_str_free() is a no-op (just
 * nulls the descriptor).  Compaction runs at statement boundaries — at that
 * point no temporaries exist on the C stack, so the only roots are the
 * variable table and array storage.
 */

#include "strpool.h"
#include "gwbasic.h"
#include <stdlib.h>
#include <string.h>

static char *pool;
static size_t pool_size;
static size_t pool_used;

void strpool_init(size_t size)
{
    pool = malloc(size);
    if (!pool) { pool_size = 0; pool_used = 0; return; }
    pool_size = size;
    pool_used = 0;
}

void strpool_shutdown(void)
{
    free(pool);
    pool = NULL;
    pool_size = pool_used = 0;
}

void strpool_reset(size_t size)
{
    if (size == 0) size = pool_size;
    if (size != pool_size) {
        free(pool);
        pool = malloc(size);
        pool_size = pool ? size : 0;
    }
    pool_used = 0;
}

bool strpool_owns(const char *p)
{
    return p >= pool && p < pool + pool_size;
}

char *strpool_alloc(int len)
{
    if ((size_t)len > pool_size - pool_used)
        gw_error(ERR_OS);
    char *p = pool + pool_used;
    pool_used += len;
    return p;
}

size_t strpool_free(void)
{
    return pool_size - pool_used;
}

/*
 * Compacting garbage collector.
 *
 * Walk every live string descriptor (variables + arrays).  Copy live data
 * into a temporary buffer in order, then memcpy back and update pool_used.
 * String descriptors are updated in place to point at their new locations.
 */
void strpool_gc(void)
{
    char *tmp = malloc(pool_size);
    if (!tmp) return;

    size_t offset = 0;

    for (int i = 0; i < gw.var_count; i++) {
        if (gw.vars[i].type == VT_STR) {
            gw_string_t *s = &gw.vars[i].val.sval;
            if (s->len > 0 && s->data && strpool_owns(s->data)) {
                memcpy(tmp + offset, s->data, s->len);
                s->data = pool + offset;
                offset += s->len;
            }
        }
    }

    for (int i = 0; i < gw.array_count; i++) {
        if (gw.arrays[i].type == VT_STR) {
            for (int j = 0; j < gw.arrays[i].total_elements; j++) {
                gw_string_t *s = &gw.arrays[i].data[j].sval;
                if (s->len > 0 && s->data && strpool_owns(s->data)) {
                    memcpy(tmp + offset, s->data, s->len);
                    s->data = pool + offset;
                    offset += s->len;
                }
            }
        }
    }

    memcpy(pool, tmp, offset);
    pool_used = offset;
    free(tmp);
}
