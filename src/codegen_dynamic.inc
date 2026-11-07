/* Native tagged expressions for programs whose default types change at run
 * time. Included here to share token cursors, diagnostics and output limits
 * with the static emitter; control flow remains in codegen.c. */
static void emit_dynamic_prec(int min_prec);
static void emit_dynamic_atom(int unused);

static void dynamic_expect(uint8_t token)
{
    skip_spaces();
    if (cur() != token) codegen_error("unexpected token in dynamic expression");
    advance();
}

static void emit_dynamic_type(const char *name, int type)
{
    if (type) EMIT("%d", type);
    else EMIT("gw.def_type[%d]", name[0]-'A');
}

/* Resolve the type before evaluating subscripts; evaluate each subscript in
 * source order. Runtime-selected arrays cannot use a fixed-type cache. */
static void emit_dynamic_lvalue(bool dimension)
{
    char name[GW_NAME_SIZE];
    int type=parse_var(name);
    skip_spaces();
    if (cur() != '(') {
        if (dimension) codegen_error("expected dimensions");
        EMIT("gwrt_var(\"%s\",%d)",name,type);
        return;
    }
    advance();
    EMIT("({int _type="); emit_dynamic_type(name,type);
    EMIT("; int _subs[8];");
    int nd=0;
    do {
        if (nd) advance();
        if (nd == 8) codegen_error("too many dimensions");
        EMIT("_subs[%d]=gwrt_integer(",nd++); emit_dynamic_prec(0); EMIT(");");
        skip_spaces();
    } while (cur() == ',');
    dynamic_expect(')');
    const char *function=dimension ? "gwrt_dim" : safe_mode ? "gwrt_array_elem_safe" : "gwrt_array_elem";
    EMIT("%s(\"%s\",_type,%d,_subs",function,name,nd);
    if (!dimension && safe_mode) EMIT(",%u",emit_line);
    EMIT(");})");
}

static void emit_dynamic_foreign(const extern_func_t *ef)
{
    char name[GW_NAME_SIZE];
    tp=peek_full_ident(name,sizeof name);
    skip_spaces();
    EMIT("({gwrt_expr_begin();gw_value_t _args[%d];",ef->argc ? ef->argc : 1);
    int argc=0;
    if (cur() == '(') {
        advance(); skip_spaces();
        if (cur() != ')') do {
            if (argc) advance();
            if (argc >= ef->argc) codegen_error("too many foreign-function arguments");
            EMIT("_args[%d]=gwrt_cast(%d,",argc,ef->arg_types[argc]);
            emit_dynamic_prec(0); EMIT(");gwrt_expr_root(&_args[%d]);",argc); argc++;
            skip_spaces();
        } while (cur() == ',');
        dynamic_expect(')');
    }
    if (argc != ef->argc) codegen_error("missing foreign-function argument");
    EMIT("gw_value_t _result=_gw_foreign_%d(_args);gwrt_expr_end();_result;})",(int)(ef-ana->externs));
}

static void emit_dynamic_builtin(int code, int minimum, int maximum, bool optional)
{
    EMIT("({gwrt_expr_begin();gw_value_t _args[%d];",maximum ? maximum : 1);
    int argc=0;
    skip_spaces();
    if (maximum && cur() == '(') {
        advance(); skip_spaces();
        if (cur() == ')') codegen_error("missing function argument");
        if (cur() != ')') do {
            if (argc) advance();
            if (argc >= maximum) codegen_error("too many function arguments");
            if (code == TOK_INSTR && argc == 2)
                EMIT("if(_args[0].type==VT_STR)gw_error(ERR_SN);");
            if ((code == TOK_INPUT || code == ((TOK_PREFIX_FE<<8)|XSTMT_IOCTL)) && cur() == '#') advance();
            EMIT("_args[%d]=gwrt_arg(%d,%d,",argc,code,argc);
            emit_dynamic_prec(0); EMIT(",_args);gwrt_expr_root(&_args[%d]);",argc); argc++;
            skip_spaces();
        } while (cur() == ',');
        dynamic_expect(')');
    } else if (!optional && maximum) codegen_error("expected '(' after function");
    if (argc < minimum) codegen_error("missing function argument");
    EMIT("gw_value_t _result=gwrt_function(%d,%d,_args);gwrt_expr_end();_result;})",code,argc);
}

static void emit_dynamic_atom(int unused)
{
    skip_spaces();
    uint8_t tok=cur();
    if (tok == TOK_PLUS || tok == TOK_MINUS || tok == TOK_NOT) {
        advance(); EMIT("gwrt_unary(%d,",tok);
        if (tok == TOK_NOT) emit_dynamic_prec(50);
        else emit_dynamic_atom(0);
        EMIT(")"); return;
    }
    if (tok == '(') {
        advance(); EMIT("("); emit_dynamic_prec(0); dynamic_expect(')'); EMIT(")"); return;
    }
    if (tok >= 0x11 && tok <= 0x1a) {
        EMIT("(gw_value_t){.type=VT_INT,.ival=%d}",tok-0x11); tp++; return;
    }
    if (tok == TOK_INT1 || tok == TOK_INT2) {
        int value=tok == TOK_INT1 ? tp[1] : (int16_t)(tp[1]|((uint16_t)tp[2]<<8));
        tp+=tok == TOK_INT1 ? 2 : 3;
        EMIT("(gw_value_t){.type=VT_INT,.ival=%d}",value); return;
    }
    if (tok == TOK_CONST_SNG) {
        float value; memcpy(&value,tp+1,4); tp+=5;
        EMIT("(gw_value_t){.type=VT_SNG,.fval=%a}",(double)value); return;
    }
    if (tok == TOK_CONST_DBL) {
        double value; memcpy(&value,tp+1,8); tp+=9;
        EMIT("(gw_value_t){.type=VT_DBL,.dval=%a}",value); return;
    }
    if (tok == '"') {
        tp++; EMIT("(gw_value_t){.type=VT_STR,.sval=gw_str_from_cstr(\"");
        while (*tp && *tp != '"') {
            if (*tp < 32 || *tp >= 127) EMIT("\\%03o",*tp);
            else { if (*tp == '\\') EMIT("\\"); EMIT("%c",*tp); }
            tp++;
        }
        if (*tp == '"') tp++;
        EMIT("\")}"); return;
    }
    if (is_letter(tok)) {
        char name[GW_NAME_SIZE]; peek_full_ident(name,sizeof name);
        const extern_func_t *ef=analysis_find_extern(ana,name);
        if (ef) emit_dynamic_foreign(ef);
        else { EMIT("gwrt_copy("); emit_dynamic_lvalue(false); EMIT(")"); }
        return;
    }
    if (tok == TOK_FN) {
        uint8_t *start=tp;
        advance();
        char name[GW_NAME_SIZE]; parse_var(name); skip_spaces();
        if (cur() == '(') {
            advance(); char *discard=emit_to_buf(emit_dynamic_prec,0); free(discard); dynamic_expect(')');
        }
        EMIT("({static const uint8_t _fn[]={");
        for (uint8_t *p=start;p<tp;p++) EMIT("%u,",*p);
        EMIT("0};gwrt_fn_tokens(_fn);})"); return;
    }
    if (tok == TOK_INPUT) {
        advance(); dynamic_expect('$'); emit_dynamic_builtin(tok,1,2,false); return;
    }
    if (tok == TOK_STRINGS || tok == TOK_INSTR || tok == TOK_POINT) {
        advance(); emit_dynamic_builtin(tok,2,tok == TOK_INSTR ? 3 : 2,false); return;
    }
    if (tok == TOK_ERR || tok == TOK_ERL || tok == TOK_CSRLIN || tok == TOK_INKEYS) {
        advance(); emit_dynamic_builtin(tok,0,0,true); return;
    }
    if (tok == TOK_PREFIX_FF || tok == TOK_PREFIX_FD || tok == TOK_PREFIX_FE) {
        int function=tp[1], code=(tok<<8)|function;
        tp+=2; skip_spaces();
        int minimum=1, maximum=1; bool optional=false;
        if (tok == TOK_PREFIX_FF) {
            if (function == FUNC_RND) { minimum=0; optional=true; }
            else if (function == FUNC_LEFT || function == FUNC_RIGHT) minimum=maximum=2;
            else if (function == FUNC_MID) { minimum=2; maximum=3; }
        } else if (tok == TOK_PREFIX_FE) {
            if (function == XSTMT_PMAP) minimum=maximum=2;
            else if (function == XSTMT_DATE || function == XSTMT_TIME || function == XSTMT_TIMER || function == XSTMT_ERDEV) {
                minimum=maximum=0; optional=true;
                if (function == XSTMT_ERDEV && cur() == '$') { advance(); code|=0x10000; }
            } else if (function == XSTMT_ENVIRON || function == XSTMT_IOCTL) dynamic_expect('$');
        }
        emit_dynamic_builtin(code,minimum,maximum,optional); return;
    }
    codegen_error("unsupported dynamic expression");
}

static void emit_dynamic_prec(int min_prec)
{
    char *left=emit_to_buf(emit_dynamic_atom,0);
    for (;;) {
        skip_spaces(); int prec=op_prec(cur());
        if (prec < min_prec) break;
        uint8_t op=cur(); advance(); skip_spaces(); bool invert=false;
        if (op == TOK_LT && cur() == TOK_GT) { advance(); op=TOK_EQ; invert=true; }
        else if (op == TOK_LT && cur() == TOK_EQ) { advance(); op=TOK_GT; invert=true; }
        else if (op == TOK_GT && cur() == TOK_EQ) { advance(); op=TOK_LT; invert=true; }
        else if (op == TOK_EQ && (cur() == TOK_LT || cur() == TOK_GT)) {
            op=cur() == TOK_LT ? TOK_GT : TOK_LT; advance(); invert=true;
        }
        char *right=emit_to_buf(emit_dynamic_prec,prec+1), *combined=NULL; size_t size=0;
        FILE *saved=out; out=open_memstream(&combined,&size);
        if (!out) codegen_error("cannot allocate dynamic expression");
        EMIT("({gwrt_expr_begin();gw_value_t _left=%s;gwrt_expr_root(&_left);"
             "gw_value_t _right=%s;gwrt_expr_root(&_right);gw_value_t _result=",left,right);
        if (invert) EMIT("gwrt_unary(%d,",TOK_NOT);
        EMIT("gw_value_binary(%d,_left,_right)",op);
        if (invert) EMIT(")");
        EMIT(";gwrt_expr_end();_result;})"); fclose(out); out=saved;
        free(left); free(right); left=combined;
    }
    EMIT("%s",left); free(left);
}

static void emit_dynamic_expr(void)
{
    EMIT("({gwrt_expr_begin();gw_value_t _value="); emit_dynamic_prec(0);
    EMIT(";gwrt_expr_end();_value;})");
}

static void emit_dynamic_assignment(void)
{
    EMIT("  {gwrt_expr_begin();gw_value_t *_target="); emit_dynamic_lvalue(false);
    skip_spaces();
    if (cur() != TOK_EQ) {
        EMIT(";gw_error(ERR_SN);}\n"); skip_statement_tail(); return;
    }
    advance(); EMIT(";gw_value_t _value="); emit_dynamic_prec(0);
    EMIT(";gwrt_assign(_target,_value);gwrt_expr_end();}\n");
}

static void emit_dynamic_for(void)
{
    advance(); char name[GW_NAME_SIZE]; int type=parse_var(name);
    if (cur() != TOK_EQ) { EMIT("  gw_error(ERR_SN);\n"); skip_statement_tail(); return; }
    program_line_t *target=NULL; uint8_t *after=NULL,*next_var;
    bool matched=gw_for_exit(emit_source_line,tp,&target,&after,&next_var);
    EMIT("  {int _entry_type="); emit_dynamic_type(name,type);
    EMIT(";if(_entry_type==VT_STR)gw_error(ERR_TM);gw_value_t *_var=gwrt_var(\"%s\",_entry_type);\n",name);
    advance(); EMIT("gw_value_t _initial=gwrt_cast(_entry_type,"); emit_dynamic_expr(); EMIT(");\n");
    dynamic_expect(TOK_TO);
    EMIT("gw_value_t _limit=gwrt_cast(_entry_type,"); emit_dynamic_expr(); EMIT(");\n");
    EMIT("gw_value_t _step=gwrt_cast(_entry_type,");
    if (cur() == TOK_STEP) { advance(); emit_dynamic_expr(); }
    else EMIT("(gw_value_t){.type=VT_INT,.ival=1}");
    EMIT(");\n");
    if (!matched) EMIT("gw_error(ERR_FW);\n");
    else {
        uint8_t *saved=tp; tp=next_var;
        if (is_letter(cur())) {
            char next_name[GW_NAME_SIZE]; int next_type=parse_var(next_name);
            EMIT("if(%d || ",strcmp(name,next_name) != 0 || *after == '(');
            emit_dynamic_type(next_name,next_type);
            EMIT(" != _entry_type){gw.cur_line_num=%u;gw_error(ERR_NF);}\n",target->num);
        }
        tp=saved;
    }
    int fc=for_label_counter++;
    EMIT("gwrt_assign(_var,_initial);\nif(!gwrt_for_push(&_var->ival,_entry_type,gwrt_number(_limit),gwrt_number(_step),%d))",fc);
    if (matched) EMIT("{gw.cur_line_num=%u;goto for_exit_%u_%d;}",target->num,target->num,(int)(after-target->tokens));
    else EMIT("gw_error(ERR_FW);");
    EMIT("}\nfor_body_%d: ;\n",fc);
}

static bool emit_dynamic_statement(uint8_t tok)
{
    if (tok == TOK_FOR) { emit_dynamic_for(); return true; }
    if (tok == TOK_DIM) {
        advance();
        do {
            EMIT("  {gwrt_expr_begin();(void)"); emit_dynamic_lvalue(true); EMIT(";gwrt_expr_end();}\n");
            skip_spaces();
            if (cur() != ',') break;
            advance();
        } while (1);
        return true;
    }
    if (tok == TOK_READ) {
        advance();
        do {
            EMIT("  {gwrt_expr_begin();gw_value_t *_target="); emit_dynamic_lvalue(false);
            EMIT(";gwrt_read_value(_target);gwrt_expr_end();}\n");
            skip_spaces();
            if (cur() != ',') break;
            advance();
        } while (1);
        return true;
    }
    if (tok == TOK_SWAP || tok == TOK_WRITE) { emit_delegate_stmt(tp,true); return true; }
    return false;
}

static void emit_dynamic_ffi_wrappers(void)
{
    for (int i=0;i<ana->extern_count;i++) {
        const extern_func_t *ef=&ana->externs[i];
        EMIT("static gw_value_t _gw_foreign_%d(gw_value_t *args){\n",i);
        for (int n=0;n<ef->argc;n++) {
            int type=ef->arg_types[n];
            if (type == VT_STR) {
                EMIT("char _a%d[256];if(args[%d].sval.len)memcpy(_a%d,args[%d].sval.data,args[%d].sval.len);"
                     "_a%d[args[%d].sval.len]=0;\n",n,n,n,n,n,n,n);
            } else EMIT("%s _a%d=args[%d].%s;\n",c_ffi_type(type),n,n,type == VT_INT ? "ival" : type == VT_DBL ? "dval" : "fval");
        }
        EMIT("%s _result=%s(",c_ffi_type(ef->ret_type),ef->c_name);
        for (int n=0;n<ef->argc;n++) EMIT("%s_a%d",n ? "," : "",n);
        EMIT(");return (gw_value_t){.type=%d,",ef->ret_type);
        if (ef->ret_type == VT_STR) EMIT(".sval=gw_str_from_cstr(_result?_result:\"\")");
        else EMIT(".%s=_result",ef->ret_type == VT_INT ? "ival" : ef->ret_type == VT_DBL ? "dval" : "fval");
        EMIT("};}\n");
    }
    if (!ana->extern_count) return;
    EMIT("static bool _gw_foreign_eval(gw_value_t *result){\n"
         "uint8_t *end=gw.text_ptr;char name[GW_NAME_SIZE];if(!gw_scan_name(&end,name))return false;\n");
    for (int i=0;i<ana->extern_count;i++) {
        const extern_func_t *ef=&ana->externs[i];
        char name[EXTERN_NAME_MAX];
        for (int n=0;n<EXTERN_NAME_MAX;n++) { name[n]=(char)toupper((unsigned char)ef->name[n]); if(!name[n]) break; }
        EMIT("if(strcmp(name,\"%s\")==0){\ngw.text_ptr=end;"
             "if(*gw.text_ptr=='$'||*gw.text_ptr=='%%'||*gw.text_ptr=='!'||*gw.text_ptr=='#')gw.text_ptr++;"
             "gw_skip_spaces();bool paren=*gw.text_ptr=='(';if(paren)gw_chrget();\n",name);
        if (ef->argc) EMIT("if(!paren)gw_error(ERR_SN);\n");
        EMIT("gwrt_expr_begin();gw_value_t args[%d];\n",ef->argc ? ef->argc : 1);
        for (int n=0;n<ef->argc;n++) {
            if(n) EMIT("gw_expect(',');\n");
            EMIT("args[%d]=gwrt_cast(%d,gw_eval());gwrt_expr_root(&args[%d]);\n",n,ef->arg_types[n],n);
        }
        EMIT("if(paren)gw_expect_rparen();*result=_gw_foreign_%d(args);gwrt_expr_end();return true;}\n",i);
    }
    EMIT("return false;}\n");
}
