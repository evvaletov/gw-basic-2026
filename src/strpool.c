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
 * point the roots are the variable table and array storage. The shared
 * evaluator and tagged native code register temporaries so FRE can move
 * them safely during evaluation.
 */

#include "strpool.h"
#include "gwbasic.h"
#include <stdlib.h>
#include <string.h>

/* Keep BASIC strings near, but put the collector's second buffer outside the
 * 64 KiB data segment on DOS16. LOAD and file buffers can leave too little near
 * heap to compact even an otherwise mostly empty 8 KiB string pool. */
#ifdef _M_I86
#include <malloc.h>
typedef char __far *gc_buffer_t;
#define gc_alloc _fmalloc
#define gc_copy _fmemcpy
#define gc_free _ffree
#else
typedef char *gc_buffer_t;
#define gc_alloc malloc
#define gc_copy memcpy
#define gc_free free
#endif

static char *pool;
static size_t pool_size;
static size_t pool_used;
static int pin_count;

/* Expression temporaries must move with a compacting collection. Frames record
 * the roots that were live on entry, so nested expressions can return values
 * without leaving pointers to expired C locals in the collector. */
static gw_value_t **expression_roots;
static size_t root_count, root_capacity;
static size_t *expression_frames;
static size_t frame_count, frame_capacity;

void gwrt_expr_begin(void)
{
    if (frame_count == frame_capacity) {
        size_t capacity = frame_capacity ? 2 * frame_capacity : 16;
        size_t *frames = realloc(expression_frames, capacity * sizeof(*frames));
        if (!frames) gw_error(ERR_OM);
        expression_frames = frames; frame_capacity = capacity;
    }
    expression_frames[frame_count++] = root_count;
}

void gwrt_expr_root(gw_value_t *value)
{
    if (root_count == root_capacity) {
        size_t capacity = root_capacity ? 2 * root_capacity : 32;
        gw_value_t **roots = realloc(expression_roots, capacity * sizeof(*roots));
        if (!roots) gw_error(ERR_OM);
        expression_roots = roots; root_capacity = capacity;
    }
    expression_roots[root_count++] = value;
}

void gwrt_expr_end(void)
{
    if (frame_count) root_count = expression_frames[--frame_count];
}

void gwrt_expr_unwind(void)
{
    free(expression_roots); free(expression_frames);
    expression_roots = NULL; expression_frames = NULL;
    root_count = root_capacity = frame_count = frame_capacity = 0;
}

static void collect_expression_roots(gc_buffer_t scratch, size_t *offset)
{
    for (size_t i = 0; i < root_count; i++) {
        gw_value_t *value = expression_roots[i];
        if (value->type == VT_STR && value->sval.len && value->sval.data && strpool_owns(value->sval.data)) {
            gc_copy(scratch + *offset, value->sval.data, value->sval.len);
            value->sval.data = pool + *offset;
            *offset += value->sval.len;
        }
    }
}

void strpool_init(size_t size)
{
#ifdef _M_I86
    /* Reserve DGROUP before far allocations can block its DOS memory block. */
    _nheapgrow();
#endif
    pool = malloc(size);
    if (!pool) { pool_size = 0; pool_used = 0; return; }
    pool_size = size;
    pool_used = 0;
}

void strpool_shutdown(void)
{
    gwrt_expr_unwind();
    free(pool);
    pool = NULL;
    pool_size = pool_used = 0;
}

void strpool_reset(size_t size)
{
    gwrt_expr_unwind();
    if (size == 0) size = pool_size;
    pool_used = 0;
    pin_count = 0;
    if (size != pool_size) {
        /* Keep the existing pool usable if CLEAR cannot resize it. */
        char *resized = realloc(pool, size);
        if (!resized) gw_error(ERR_OM);
        pool = resized;
        pool_size = size;
    }
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
 * Walk every live string descriptor (variables, arrays and expression roots). Copy live data
 * into a temporary buffer in order, then memcpy back and update pool_used.
 * String descriptors are updated in place to point at their new locations.
 */
void strpool_pin(void)   { pin_count++; }
void strpool_unpin(void) { if (pin_count > 0) pin_count--; }

void strpool_gc(void)
{
    if (pin_count > 0) return;
    gc_buffer_t tmp = gc_alloc(pool_size);
    if (!tmp) return;

    size_t offset = 0;

    for (int i = 0; i < gw.var_count; i++) {
        if (gw.vars[i].type == VT_STR) {
            gw_string_t *s = &gw.vars[i].val.sval;
            if (s->len > 0 && s->data && strpool_owns(s->data)) {
                gc_copy(tmp + offset, s->data, s->len);
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
                    gc_copy(tmp + offset, s->data, s->len);
                    s->data = pool + offset;
                    offset += s->len;
                }
            }
        }
    }

    /* Evaluators register the addresses of their live descriptors. */
    collect_expression_roots(tmp, &offset);
    gc_copy(pool, tmp, offset);
    pool_used = offset;
    gc_free(tmp);
}
