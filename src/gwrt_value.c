/* Typed value operations for generated C. No token parsing is used by the
 * arithmetic, variable, array, builtin or foreign-function expression paths. */
#include "gwrt.h"
#include "tui.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Native C temporaries must move with a compacting collection. Frames record
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

void gwrt_collect_roots(char *pool, char *scratch, size_t *offset)
{
    for (size_t i = 0; i < root_count; i++) {
        gw_value_t *value = expression_roots[i];
        if (value->type == VT_STR && value->sval.len && value->sval.data && strpool_owns(value->sval.data)) {
            memcpy(scratch + *offset, value->sval.data, value->sval.len);
            value->sval.data = pool + *offset;
            *offset += value->sval.len;
        }
    }
}

gw_value_t *gwrt_var(const char *name, int type)
{
    if (!type) type = gw.def_type[name[0] - 'A'];
    return &gw_var_find_or_create(name, (gw_valtype_t)type)->val;
}

gw_value_t gwrt_copy(gw_value_t *value)
{
    gw_value_t result = *value;
    if (result.type == VT_STR) result.sval = gw_str_copy(&value->sval);
    return result;
}

gw_value_t gwrt_cast(int type, gw_value_t value)
{
    gw_value_t result;
    result.type = (gw_valtype_t)type;
    switch (type) {
    case VT_INT: result.ival = gw_to_int(&value); break;
    case VT_SNG: result.fval = gw_to_sng(&value); break;
    case VT_DBL: result.dval = gw_to_dbl(&value); break;
    case VT_STR:
        if (value.type != VT_STR) gw_error(ERR_TM);
        result.sval = value.sval;
        break;
    default: gw_error(ERR_TM);
    }
    return result;
}

void gwrt_assign(gw_value_t *target, gw_value_t value)
{
    value = gwrt_cast(target->type, value);
    if (target->type == VT_STR) gw_str_free(&target->sval);
    *target = value;
}

double gwrt_number(gw_value_t value) { return gw_to_dbl(&value); }
int gwrt_integer(gw_value_t value) { return gw_to_int(&value); }
gw_string_t gwrt_string(gw_value_t value)
{
    if (value.type != VT_STR) gw_error(ERR_TM);
    return value.sval;
}

gw_value_t gwrt_unary(int op, gw_value_t value)
{
    if (op == TOK_NOT) {
        int16_t integer = (int16_t)~gw_to_int(&value);
        value.type = VT_INT; value.ival = integer; return value;
    }
    if (value.type == VT_STR) gw_error(ERR_TM);
    if (op == TOK_MINUS) {
        if (value.type == VT_INT) value.ival = gw_int_neg(value.ival);
        else if (value.type == VT_SNG) value.fval = -value.fval;
        else value.dval = -value.dval;
    }
    return value;
}

#define FF(f) ((TOK_PREFIX_FF << 8) | (f))
#define FD(f) ((TOK_PREFIX_FD << 8) | (f))
#define FE(f) ((TOK_PREFIX_FE << 8) | (f))

/* Validate/convert each argument before evaluating the next one. In particular
 * INPUT$ validates count range before file evaluation, but count zero only
 * after validating the file. */
gw_value_t gwrt_arg(int code, int index, gw_value_t value, gw_value_t *args)
{
    (void)args;
    int type = 0;
    switch (code) {
    case TOK_INPUT: {
        int number = gw_input_byte(gwrt_number(value));
        value.type = VT_INT; value.ival = number; return value;
    }
    case TOK_POINT: {
        double number = gfx_has_window() ? gwrt_number(value) : gwrt_integer(value);
        value.type = VT_DBL;
        value.dval = index ? gfx_map_y(number) : gfx_map_x(number);
        return value;
    }
    case TOK_INSTR:
        if (!index) return value.type == VT_STR ? value : gwrt_cast(VT_INT, value);
        type = VT_STR; break;
    case TOK_STRINGS:
        if (index) return value;
        type = VT_INT; break;
    case FF(FUNC_LEFT): case FF(FUNC_RIGHT): case FF(FUNC_MID):
        type = index ? VT_INT : VT_STR; break;
    case FF(FUNC_LEN): case FF(FUNC_ASC): case FF(FUNC_VAL):
    case FD(XFUNC_CVI): case FD(XFUNC_CVS): case FD(XFUNC_CVD):
    case FE(XSTMT_ENVIRON): type = VT_STR; break;
    case FF(FUNC_CHR): case FF(FUNC_SPACE): case FF(FUNC_HEX): case FF(FUNC_OCT):
    case FF(FUNC_EOF): case FF(FUNC_LOC): case FF(FUNC_LOF): case FF(FUNC_STICK):
    case FD(XFUNC_MKI): case FE(XSTMT_IOCTL): type = VT_INT; break;
    case FE(XSTMT_PMAP): if (index) type = VT_INT; break;
    case FF(FUNC_FRE): case FF(FUNC_POS): case FF(FUNC_LPOS):
    case FF(FUNC_PEN): case FF(FUNC_STRIG): return value;
    default: break;
    }
    if (type) return gwrt_cast(type, value);
    if (value.type == VT_STR) gw_error(ERR_TM);
    return value;
}

static uint16_t uint16_value(gw_value_t value)
{
    if (value.type == VT_INT) return (uint16_t)value.ival;
    double number = gwrt_number(value);
    if (!(number >= -32768 && number <= 65535)) gw_error(ERR_FC);
    return (uint16_t)(int32_t)number;
}

/* Arguments have already been evaluated and checked in BASIC order. */
gw_value_t gwrt_function(int code, int argc, gw_value_t *args)
{
    gw_value_t v = {.type=VT_INT, .ival=0};
    gw_value_t a = argc ? args[0] : v;
    double number;
    int n;
    switch (code) {
    case TOK_INPUT:
        v.type=VT_STR; v.sval=gw_input_chars(a.ival, argc == 2 ? args[1].ival : -1); return v;
    case TOK_STRINGS:
        if (args[1].type == VT_STR) {
            if (!args[1].sval.len) gw_error(ERR_FC);
            n=(unsigned char)args[1].sval.data[0];
        } else n=gwrt_integer(args[1]);
        return gw_fn_strings(a.ival, n);
    case TOK_INSTR:
        if (a.type == VT_STR) {
            if (argc != 2) gw_error(ERR_SN);
            return gw_fn_instr(1, &args[0], &args[1]);
        }
        if (argc != 3) gw_error(ERR_SN);
        return gw_fn_instr(a.ival, &args[1], &args[2]);
    case TOK_POINT: v.ival=gfx_point((int)a.dval,(int)args[1].dval); return v;
    case TOK_ERR: v.ival=gw_errno; return v;
    case TOK_ERL: v.type=VT_SNG; v.fval=gw.err_line_num; return v;
    case TOK_CSRLIN: v.ival=gw_hal ? gw_hal->get_cursor_row()+1 : 1; return v;
    case TOK_INKEYS: {
        int key=tui_pop_key();
        if (key < 0 && gw_hal && gw_hal->kbhit()) key=gw_hal->getch();
        int scan=key < 0 ? -1 : tui_key_to_scancode(key);
        v.type=VT_STR; v.sval=gw_str_alloc(key < 0 ? 0 : scan < 0 ? 1 : 2);
        if (v.sval.len) v.sval.data[0]=scan < 0 ? (char)key : 0;
        if (v.sval.len == 2) v.sval.data[1]=(char)scan;
        return v;
    }
    case FF(FUNC_SGN): v.ival=gw_sgn(gwrt_number(a)); return v;
    case FF(FUNC_INT):
        v.type=a.type == VT_DBL ? VT_DBL : VT_SNG;
        number=gw_int_fn(gwrt_number(a)); break;
    case FF(FUNC_FIX):
        v=a;
        if (v.type == VT_SNG) v.fval=(float)gw_fix(v.fval);
        else if (v.type == VT_DBL) v.dval=gw_fix(v.dval);
        return v;
    case FF(FUNC_ABS):
        v=a;
        if (v.type == VT_INT) v.ival=v.ival < 0 ? gw_int_neg(v.ival) : v.ival;
        else if (v.type == VT_SNG) v.fval=(float)gw_abs(v.fval);
        else v.dval=gw_abs(v.dval);
        return v;
    case FF(FUNC_RND): v.type=VT_SNG; v.fval=(float)gw_rnd(argc ? gwrt_number(a) : 1); return v;
    case FF(FUNC_CINT): return gwrt_cast(VT_INT,a);
    case FF(FUNC_CSNG): v.type=VT_SNG; v.fval=gw_csng(gwrt_number(a)); return v;
    case FF(FUNC_CDBL): v.type=VT_DBL; v.dval=gw_cdbl(&a); return v;
    case FF(FUNC_SQR): number=gw_sqr(gwrt_number(a)); goto transcendental;
    case FF(FUNC_SIN): number=gw_sin(gwrt_number(a)); goto transcendental;
    case FF(FUNC_COS): number=gw_cos(gwrt_number(a)); goto transcendental;
    case FF(FUNC_TAN): number=gw_tan(gwrt_number(a)); goto transcendental;
    case FF(FUNC_ATN): number=gw_atn(gwrt_number(a)); goto transcendental;
    case FF(FUNC_LOG): number=gw_log(gwrt_number(a)); goto transcendental;
    case FF(FUNC_EXP): number=gw_exp(gwrt_number(a)); goto transcendental;
    case FF(FUNC_LEN): return gw_fn_len(&a);
    case FF(FUNC_ASC): return gw_fn_asc(&a);
    case FF(FUNC_VAL): return gw_fn_val(&a);
    case FF(FUNC_STR): return gw_fn_str(&a);
    case FF(FUNC_CHR): return gw_fn_chr(a.ival);
    case FF(FUNC_SPACE): return gw_fn_space(a.ival);
    case FF(FUNC_HEX): return gw_fn_hex(a.ival);
    case FF(FUNC_OCT): return gw_fn_oct(a.ival);
    case FF(FUNC_LEFT): return gw_fn_left(&a,args[1].ival);
    case FF(FUNC_RIGHT): return gw_fn_right(&a,args[1].ival);
    case FF(FUNC_MID): return gw_fn_mid(&a,args[1].ival,argc == 3 && args[2].ival >= 0 ? args[2].ival : 255);
    case FD(XFUNC_CVI): return gw_fn_cvi(&a);
    case FD(XFUNC_CVS): return gw_fn_cvs(&a);
    case FD(XFUNC_CVD): return gw_fn_cvd(&a);
    case FD(XFUNC_MKI): return gw_fn_mki(a.ival);
    case FD(XFUNC_MKS): return gw_fn_mks(gw_to_sng(&a));
    case FD(XFUNC_MKD): return gw_fn_mkd(gw_to_dbl(&a));
    case FF(FUNC_FRE):
        if (a.type == VT_STR) gw_str_free(&args[0].sval);
        strpool_gc(); v.type=VT_SNG; v.fval=(float)strpool_free(); return v;
    case FF(FUNC_POS): v.ival=gw_hal ? gw_hal->get_cursor_col()+1 : 1; return v;
    case FF(FUNC_LPOS): v.ival=1; return v;
    case FF(FUNC_PEN): case FF(FUNC_STRIG): return v;
    case FF(FUNC_STICK): if (a.ival < 0 || a.ival > 3) gw_error(ERR_FC); v.ival=128; return v;
    case FF(FUNC_EOF): v.ival=gw_file_eof(a.ival); return v;
    case FF(FUNC_LOC): case FF(FUNC_LOF): {
        file_entry_t *file=gw_file_get(a.ival);
        long pos=ftell(file->fp);
        v.type=VT_SNG;
        if (code == FF(FUNC_LOC)) v.fval=(float)(pos/128+1);
        else { fseek(file->fp,0,SEEK_END); v.fval=(float)ftell(file->fp); fseek(file->fp,pos,SEEK_SET); }
        return v;
    }
    case FF(FUNC_PEEK): v.ival=virmem_peek(gw.def_seg,uint16_value(a)); return v;
    case FF(FUNC_INP): v.ival=portio_inp(uint16_value(a)); return v;
    case FE(XSTMT_PMAP): v.type=VT_SNG; v.fval=(float)gfx_pmap(gwrt_number(a),args[1].ival); return v;
    case FE(XSTMT_DATE): case FE(XSTMT_TIME): case FE(XSTMT_TIMER): {
        time_t now=time(NULL)+gw.time_offset_secs; struct tm *tm=localtime(&now); char text[40];
        if (code == FE(XSTMT_TIMER)) {
            v.type=VT_SNG; v.fval=(float)(tm->tm_hour*3600+tm->tm_min*60+tm->tm_sec); return v;
        }
        if (code == FE(XSTMT_DATE)) snprintf(text,sizeof text,"%02d-%02d-%04d",tm->tm_mon+1,tm->tm_mday,tm->tm_year+1900);
        else snprintf(text,sizeof text,"%02d:%02d:%02d",tm->tm_hour,tm->tm_min,tm->tm_sec);
        v.type=VT_STR; v.sval=gw_str_from_cstr(text); return v;
    }
    case FE(XSTMT_ENVIRON): {
        char *name=gw_str_to_cstr(&a.sval); const char *value=getenv(name);
        v.type=VT_STR; v.sval=gw_str_from_cstr(value ? value : ""); free(name); return v;
    }
    case FE(XSTMT_ERDEV): return v;
    case 0x10000 | FE(XSTMT_ERDEV): case FE(XSTMT_IOCTL):
        v.type=VT_STR; v.sval=gw_str_alloc(0); return v;
    default: gw_error(ERR_SN); return v;
    }
    goto floating_result;
transcendental:
    v.type=a.type == VT_DBL ? VT_DBL : VT_SNG;
floating_result:
    if (v.type == VT_DBL) v.dval=number;
    else v.fval=(float)number;
    return v;
}

/* DEF FN retains the existing shared body evaluator. Its surrounding native
 * expressions and delegated body can both call the entry's registered FFI. */
gw_value_t gwrt_fn_tokens(const uint8_t *tokens)
{
    uint8_t *saved=gw.text_ptr;
    gw.text_ptr=(uint8_t *)tokens+1;
    gw_value_t result=gw_eval_fn_call();
    gw.text_ptr=saved;
    return result;
}

void gwrt_read_value(gw_value_t *target)
{
    const char *text=gwrt_data_read();
    gw_value_t value;
    if (target->type == VT_STR) { value.type=VT_STR; value.sval=gw_str_from_cstr(text); }
    else { value.type=VT_DBL; value.dval=strtod(text,NULL); }
    gwrt_assign(target,value);
}
