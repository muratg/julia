// utility procedures used in code generation

// --- string constants ---
static std::map<const std::string, GlobalVariable*> stringConstants;

static GlobalVariable *stringConst(const std::string &txt)
{
    GlobalVariable *gv = stringConstants[txt];
    static int strno = 0;
    if (gv == NULL) {
        std::stringstream ssno;
        std::string vname;
        ssno << strno;
        vname += "_j_str";
        vname += ssno.str();
        gv = new GlobalVariable(*jl_Module,
                                ArrayType::get(T_int8, txt.length()+1),
                                true,
                                GlobalVariable::PrivateLinkage,
#ifndef LLVM_VERSION_MAJOR
                                ConstantArray::get(getGlobalContext(),
                                                       txt.c_str()),
#elif LLVM_VERSION_MAJOR == 3 && LLVM_VERSION_MINOR >= 1
                                ConstantDataArray::get(getGlobalContext(),
                                                       ArrayRef<unsigned char>(
                                                       (const unsigned char*)txt.c_str(),
                                                       txt.length()+1)),
#endif
        vname);
        stringConstants[txt] = gv;
        strno++;
    }
    return gv;

}

// --- emitting pointers directly into code ---

static Value *literal_static_pointer_val(void *p, Type *t)
{
    // this function will emit a static pointer into the generated code
    // the generated code will only be valid during the current session,
    // and thus, this should typically be avoided in new API's
#if defined(_P64)
    return ConstantExpr::getIntToPtr(ConstantInt::get(T_int64, (uint64_t)p), t);
#else
    return ConstantExpr::getIntToPtr(ConstantInt::get(T_int32, (uint32_t)p), t);
#endif
}

typedef struct {Value* gv; int32_t index;} jl_value_llvm; // uses 1-based indexing
static std::map<void*, jl_value_llvm> jl_value_to_llvm;
static std::vector<Constant*> jl_sysimg_gvars;

extern "C" int32_t jl_get_llvm_gv(jl_value_t *p)
{
    // map a jl_value_t memory location to a GlobalVariable
    std::map<void*, jl_value_llvm>::iterator it;
    it = jl_value_to_llvm.find(p);
    if (it == jl_value_to_llvm.end())
        return 0;
    return it->second.index;
}
static void jl_gen_llvm_gv_array()
{
    // emit the variable table into the code image (can only call this once)
    // used just before dumping bitcode
    ArrayType *atype = ArrayType::get(T_psize,jl_sysimg_gvars.size());
    new GlobalVariable(
            *jl_Module,
            atype,
            true,
            GlobalVariable::ExternalLinkage,
            ConstantArray::get(atype, ArrayRef<Constant*>(jl_sysimg_gvars)),
            "jl_sysimg_gvars");
}

static int32_t jl_assign_functionID(Function *functionObject)
{
    // give the function an index in the constant lookup table
    if (!imaging_mode)
        return 0;
    jl_sysimg_gvars.push_back(ConstantExpr::getBitCast(functionObject,T_psize));
    return jl_sysimg_gvars.size();
}

static Value *julia_gv(const char *cname, void *addr)
{
    // emit a GlobalVariable for a jl_value_t named "cname"
    std::map<void*, jl_value_llvm>::iterator it;
    // first see if there already is a GlobalVariable for this address
    it = jl_value_to_llvm.find(addr);
    if (it != jl_value_to_llvm.end())
        return builder.CreateLoad(it->second.gv);
    // no existing GlobalVariable, create one and store it
    GlobalValue *gv = new GlobalVariable(*jl_Module, jl_pvalue_llvmt,
                           false, GlobalVariable::InternalLinkage,
                           ConstantPointerNull::get((PointerType*)jl_pvalue_llvmt), cname);
    // make the pointer valid for this session
    void **p = (void**)jl_ExecutionEngine->getPointerToGlobal(gv);
    *p = addr;
    // make the pointer valid for future sessions
    jl_sysimg_gvars.push_back(ConstantExpr::getBitCast(gv, T_psize));
    jl_value_llvm gv_struct;
    gv_struct.gv = gv;
    gv_struct.index = jl_sysimg_gvars.size();
    jl_value_to_llvm[addr] = gv_struct;
    return builder.CreateLoad(gv);
}

static Value *julia_gv(const char *prefix, jl_sym_t *name, jl_module_t *mod, void *addr)
{
    // emit a GlobalVariable for a jl_value_t, using the prefix, name, and module to
    // to create a readable name of the form prefixModA.ModB.name
    size_t len = strlen(name->name)+strlen(prefix)+1;
    jl_module_t *parent = mod, *prev = NULL;
    while (parent != NULL && parent != prev) {
        len += strlen(parent->name->name)+1;
        prev = parent;
        parent = parent->parent;
    }
    char *fullname = (char*)alloca(len);
    strcpy(fullname, prefix);
    len -= strlen(name->name)+1;
    strcpy(fullname+len,name->name);
    parent = mod;
    prev = NULL;
    while (parent != NULL && parent != prev) {
        size_t part = strlen(parent->name->name)+1;
        strcpy(fullname+len-part,parent->name->name);
        fullname[len-1] = '.';
        len -= part;
        prev = parent;
        parent = parent->parent;
    }
    return julia_gv(fullname, addr);
}

static Value *literal_pointer_val(jl_value_t *p)
{
    // emit a pointer to any jl_value_t which will be valid across reloading code
    // also, try to give it a nice name for gdb, for easy identification
    if (p == NULL)
        return ConstantPointerNull::get((PointerType*)jl_pvalue_llvmt);
    if (!imaging_mode)
        return literal_static_pointer_val(p, jl_pvalue_llvmt);
    if (jl_is_datatype(p)) {
        jl_datatype_t* addr = (jl_datatype_t*)p;
        // DataTypes are prefixed with a +
        return julia_gv("+", addr->name->name, addr->name->module, p);
    }
    if (jl_is_func(p)) {
        jl_lambda_info_t* linfo = ((jl_function_t*)p)->linfo;
        // Functions are prefixed with a -
        if (linfo != NULL)
            return julia_gv("-", linfo->name, linfo->module, p);
        // Anonymous lambdas are prefixed with jl_method#
        return julia_gv("jl_method#", p);
    }
    if (jl_is_lambda_info(p)) {
        jl_lambda_info_t* linfo = (jl_lambda_info_t*)p;
        // Type-inferred functions are prefixed with a -
        return julia_gv("-", linfo->name, linfo->module, p);
    }
    if (jl_is_symbol(p)) {
        jl_sym_t* addr = (jl_sym_t*)p;
        // Symbols are prefixed with jl_sym#
        return julia_gv("jl_sym#", addr, NULL, p);
    }
    // something else gets just a generic name
    return julia_gv("jl_global#", p);
}

static Value *literal_pointer_val(jl_binding_t *p)
{
    // emit a pointer to any jl_value_t which will be valid across reloading code
    if (p == NULL)
        return ConstantPointerNull::get((PointerType*)jl_pvalue_llvmt);
    if (!imaging_mode)
        return literal_static_pointer_val(p, jl_pvalue_llvmt);
    // bindings are prefixed with jl_bnd#
    return julia_gv("jl_bnd#", p->name, p->owner, p);
}

static Value *julia_binding_gv(jl_binding_t *b)
{
    // emit a literal_pointer_val to the value field of a jl_binding_t
    // binding->value are prefixed with *
    Value *bv = imaging_mode ?
        builder.CreateBitCast(julia_gv("*", b->name, b->owner, b), jl_ppvalue_llvmt) :
        literal_static_pointer_val(b,jl_ppvalue_llvmt);
    return builder.CreateGEP(bv,ConstantInt::get(T_size,
                offsetof(jl_binding_t,value)/sizeof(size_t)));
}

// --- mapping between julia and llvm types ---

static Type *julia_struct_to_llvm(jl_value_t *jt);

static bool jltupleisbits(jl_value_t *jt, bool allow_unsized = true);

static Type *julia_type_to_llvm(jl_value_t *jt)
{
    if (jt == (jl_value_t*)jl_bool_type) return T_int1;
    if (jt == (jl_value_t*)jl_bottom_type) return T_void;
    if (jl_is_tuple(jt)) {
        // Represent tuples as anonymous structs
        size_t ntypes = jl_tuple_len(jt);
        if (ntypes == 0)
            return T_void;
        bool purebits = true;
        bool isvector = true;
        Type *type = NULL;
        for (size_t i = 0; i < ntypes; ++i) {
            jl_value_t *elt = jl_tupleref(jt,i);
            purebits &= jltupleisbits(elt);
            Type *newtype = julia_struct_to_llvm(elt);
            if (type != NULL && type != newtype)
                isvector = false;
            type = newtype;
            if (!purebits && !isvector)
                break;
        }
        if (purebits) {
            // Can't be bool due to
            // http://llvm.org/bugs/show_bug.cgi?id=12618
            if (isvector && type != T_int1 && type != T_void) {
                Type *ret = NULL;
                if (type->isSingleValueType() && !type->isVectorTy())
                    ret = VectorType::get(type,ntypes);
                else
                    ret = ArrayType::get(type,ntypes);
                return ret;
            }
            else {
                Type *types[ntypes];
                size_t j = 0;
                for (size_t i = 0; i < ntypes; ++i) {
                    Type *ty = julia_struct_to_llvm(jl_tupleref(jt,i));
                    if (ty == T_void || ty->isEmptyTy())
                        continue;
                    types[j++] = ty;
                }
                return StructType::get(jl_LLVMContext,ArrayRef<Type*>(&types[0],j));
            }
        }
    }
    if (!jl_is_leaf_type(jt))
        return jl_pvalue_llvmt;
    if (jl_is_cpointer_type(jt)) {
        Type *lt = julia_type_to_llvm(jl_tparam0(jt));
        if (lt == NULL)
            return NULL;
        if (lt == T_void)
            lt = T_int8;
        return PointerType::get(lt, 0);
    }
    if (jl_is_bitstype(jt)) {
        int nb = jl_datatype_size(jt);
        if (jl_is_floattype(jt)) {
#ifndef DISABLE_FLOAT16
            if (nb == 2)
                return Type::getHalfTy(jl_LLVMContext);
            else
#endif
            if (nb == 4)
                return Type::getFloatTy(jl_LLVMContext);
            else if (nb == 8)
                return Type::getDoubleTy(jl_LLVMContext);
            else if (nb == 16)
                return Type::getFP128Ty(jl_LLVMContext);
        }
        return Type::getIntNTy(jl_LLVMContext, nb*8);
    }
    if (jl_isbits(jt)) {
        if (((jl_datatype_t*)jt)->size == 0) {
            // TODO: come up with a representation for a 0-size value,
            // and make this 0 size everywhere. as an argument, simply
            // skip passing it.
            return jl_pvalue_llvmt;
        }
        return julia_struct_to_llvm(jt);
    }
    return jl_pvalue_llvmt;
}

static Type *julia_struct_to_llvm(jl_value_t *jt)
{
    if (jl_is_structtype(jt) && !jl_is_array_type(jt)) {
        if (!jl_is_leaf_type(jt))
            return NULL;
        jl_datatype_t *jst = (jl_datatype_t*)jt;
        if (jst->struct_decl == NULL) {
            size_t ntypes = jl_tuple_len(jst->types);
            if (ntypes == 0)
                return T_void;
            StructType *structdecl = StructType::create(getGlobalContext(), jst->name->name->name);
            jst->struct_decl = structdecl;
            std::vector<Type *> latypes(0);
            size_t i;
            for(i = 0; i < ntypes; i++) {
                jl_value_t *ty = jl_tupleref(jst->types, i);
                Type *lty;
                if (jst->fields[i].isptr)
                    lty = jl_pvalue_llvmt;
                else
                    lty = ty==(jl_value_t*)jl_bool_type ? T_int8 : julia_type_to_llvm(ty);
                latypes.push_back(lty);
            }
            structdecl->setBody(latypes);
        }
        return (Type*)jst->struct_decl;
    }
    return julia_type_to_llvm(jt);
}

// NOTE: llvm cannot express all julia types (for example unsigned),
// so this is an approximation. it's only correct if the associated LLVM
// value is not tagged with our value name hack.
// boxed(v) below gets the correct type.
static jl_value_t *llvm_type_to_julia(Type *t, bool throw_error)
{
    if (t == T_int1)  return (jl_value_t*)jl_bool_type;
    if (t == T_int8)  return (jl_value_t*)jl_int8_type;
    if (t == T_int16) return (jl_value_t*)jl_int16_type;
    if (t == T_int32) return (jl_value_t*)jl_int32_type;
    if (t == T_int64) return (jl_value_t*)jl_int64_type;
    if (t == T_float32) return (jl_value_t*)jl_float32_type;
    if (t == T_float64) return (jl_value_t*)jl_float64_type;
    if (t == T_void) return (jl_value_t*)jl_bottom_type;
    if (t->isEmptyTy()) return (jl_value_t*)jl_nothing->type;
    if (t == jl_pvalue_llvmt)
        return (jl_value_t*)jl_any_type;
    if (t->isPointerTy()) {
        jl_value_t *elty = llvm_type_to_julia(t->getContainedType(0),
                                              throw_error);
        if (elty != NULL) {
            return (jl_value_t*)jl_apply_type((jl_value_t*)jl_pointer_type,
                                              jl_tuple1(elty));
        }
    }
    if (throw_error) {
        jl_error("cannot convert type to a julia type");
    }
    return NULL;
}

// --- scheme for tagging llvm values with julia types using metadata ---

static std::map<int, jl_value_t*> typeIdToType;
static jl_array_t *typeToTypeId;
static int cur_type_id = 1;

static int jl_type_to_typeid(jl_value_t *t)
{
    jl_value_t *id = jl_eqtable_get(typeToTypeId, t, NULL);
    if (id == NULL) {
        int mine = cur_type_id++;
        if (mine > 65025)
            jl_error("internal compiler error: too many bits types");
        JL_GC_PUSH1(&id);
        id = jl_box_long(mine);
        jl_eqtable_put(typeToTypeId, t, id);
        typeIdToType[mine] = t;
        JL_GC_POP();
        return mine;
    }
    return jl_unbox_long(id);
}

static jl_value_t *jl_typeid_to_type(int i)
{
    std::map<int, jl_value_t*>::iterator it = typeIdToType.find(i);
    if (it == typeIdToType.end()) {
        jl_error("internal compiler error: invalid type id");
    }
    return (*it).second;
}

static bool has_julia_type(Value *v)
{
    Instruction *inst = (dyn_cast<Instruction>(v));
    return (inst != NULL) &&
            (inst->getMetadata("julia_type")!=NULL);
}

static jl_value_t *julia_type_of_without_metadata(Value *v, bool err=true)
{
    if (dyn_cast<AllocaInst>(v) != NULL ||
        dyn_cast<GetElementPtrInst>(v) != NULL) {
        // an alloca always has llvm type pointer
        return llvm_type_to_julia(v->getType()->getContainedType(0), err);
    }
    return llvm_type_to_julia(v->getType(), err);
}

static jl_value_t *julia_type_of(Value *v)
{
    MDNode *mdn;
    if (dyn_cast<Instruction>(v) == NULL ||
        (mdn = ((Instruction*)v)->getMetadata("julia_type")) == NULL) {
        return julia_type_of_without_metadata(v, true);
    }
    MDString *md = (MDString*)mdn->getOperand(0);
    const unsigned char *vts = (const unsigned char*)md->getString().data();
    int id = (vts[0]-1) + (vts[1]-1)*255;
    return jl_typeid_to_type(id);
}

static Value *NoOpInst(Value *v)
{
    v = SelectInst::Create(ConstantInt::get(T_int1,1), v, v);
    builder.Insert((Instruction*)v);
    return v;
}

static Value *mark_julia_type(Value *v, jl_value_t *jt)
{
    if (jt == (jl_value_t*)jl_any_type || v->getType()==jl_pvalue_llvmt)
        return v;
    if (has_julia_type(v)) {
        if (julia_type_of(v) == jt)
            return v;
    }
    else if (julia_type_of_without_metadata(v,false) == jt) {
        return v;
    }
    if (dyn_cast<Instruction>(v) == NULL)
        v = NoOpInst(v);
    assert(dyn_cast<Instruction>(v));
    char name[3];
    int id = jl_type_to_typeid(jt);
    // store id as base-255 to avoid NUL
    name[0] = (id%255)+1;
    name[1] = (id/255)+1;
    name[2] = '\0';
    MDString *md = MDString::get(jl_LLVMContext, name);
    MDNode *mdn = MDNode::get(jl_LLVMContext, ArrayRef<Value*>(md));
    ((Instruction*)v)->setMetadata("julia_type", mdn);
    return v;
}

// --- generating various error checks ---

static jl_value_t *llvm_type_to_julia(Type *t, bool err=true);

static Value *emit_typeof(Value *p)
{
    // given p, a jl_value_t*, compute its type tag
    if (p->getType() == jl_pvalue_llvmt) {
        Value *tt = builder.CreateBitCast(p, jl_ppvalue_llvmt);
        tt = builder.
            CreateLoad(builder.CreateGEP(tt,ConstantInt::get(T_size,0)),
                       false);
#ifdef OVERLAP_TUPLE_LEN
        tt = builder.
            CreateIntToPtr(builder.
                           CreateAnd(builder.CreatePtrToInt(tt, T_int64),
                                     ConstantInt::get(T_int64,0x000ffffffffffffe)),
                           jl_pvalue_llvmt);
#endif
        return tt;
    }
    return literal_pointer_val(julia_type_of(p));
}

static void just_emit_error(const std::string &txt, jl_codectx_t *ctx)
{
    Value *zeros[2] = { ConstantInt::get(T_int32, 0),
                        ConstantInt::get(T_int32, 0) };
    builder.CreateCall(jlerror_func,
                       builder.CreateGEP(stringConst(txt),
                                         ArrayRef<Value*>(zeros)));
}

static void emit_error(const std::string &txt, jl_codectx_t *ctx)
{
    just_emit_error(txt, ctx);
    builder.CreateUnreachable();
    BasicBlock *cont = BasicBlock::Create(getGlobalContext(),"after_error",ctx->f);
    builder.SetInsertPoint(cont);
}

static void error_unless(Value *cond, const std::string &msg, jl_codectx_t *ctx)
{
    BasicBlock *failBB = BasicBlock::Create(getGlobalContext(),"fail",ctx->f);
    BasicBlock *passBB = BasicBlock::Create(getGlobalContext(),"pass");
    builder.CreateCondBr(cond, passBB, failBB);
    builder.SetInsertPoint(failBB);
    just_emit_error(msg, ctx);
    builder.CreateUnreachable();
    ctx->f->getBasicBlockList().push_back(passBB);
    builder.SetInsertPoint(passBB);
}

static void raise_exception_unless(Value *cond, Value *exc, jl_codectx_t *ctx)
{
    BasicBlock *failBB = BasicBlock::Create(getGlobalContext(),"fail",ctx->f);
    BasicBlock *passBB = BasicBlock::Create(getGlobalContext(),"pass");
    builder.CreateCondBr(cond, passBB, failBB);
    builder.SetInsertPoint(failBB);
    builder.CreateCall2(jlthrow_line_func, exc,
                        ConstantInt::get(T_int32, ctx->lineno));
    builder.CreateUnreachable();
    ctx->f->getBasicBlockList().push_back(passBB);
    builder.SetInsertPoint(passBB);
}

static void raise_exception_unless(Value *cond, GlobalVariable *exc,
                                   jl_codectx_t *ctx)
{
    raise_exception_unless(cond, (Value*)builder.CreateLoad(exc, false), ctx);
}

static void raise_exception_if(Value *cond, Value *exc, jl_codectx_t *ctx)
{
    raise_exception_unless(builder.CreateXor(cond, ConstantInt::get(T_int1,-1)),
                           exc, ctx);
}

static void raise_exception_if(Value *cond, GlobalVariable *exc,
                               jl_codectx_t *ctx)
{
    raise_exception_if(cond, (Value*)builder.CreateLoad(exc, false), ctx);
}

static void null_pointer_check(Value *v, jl_codectx_t *ctx)
{
    raise_exception_unless(builder.CreateICmpNE(v,Constant::getNullValue(v->getType())),
                           jlundeferr_var, ctx);
}

static Value *boxed(Value *v, jl_codectx_t *ctx, jl_value_t *jt=NULL);

static void emit_type_error(Value *x, jl_value_t *type, const std::string &msg,
                            jl_codectx_t *ctx)
{
    Value *zeros[2] = { ConstantInt::get(T_int32, 0),
                        ConstantInt::get(T_int32, 0) };
    Value *fname_val = builder.CreateGEP(stringConst(ctx->funcName),
                                         ArrayRef<Value*>(zeros));
    Value *msg_val = builder.CreateGEP(stringConst(msg),
                                       ArrayRef<Value*>(zeros));
    builder.CreateCall4(jltypeerror_func,
                        fname_val, msg_val,
                        literal_pointer_val(type), boxed(x,ctx));
}

static void emit_typecheck(Value *x, jl_value_t *type, const std::string &msg,
                           jl_codectx_t *ctx)
{
    Value *istype =
        builder.CreateICmpEQ(emit_typeof(x), literal_pointer_val(type));
    BasicBlock *failBB = BasicBlock::Create(getGlobalContext(),"fail",ctx->f);
    BasicBlock *passBB = BasicBlock::Create(getGlobalContext(),"pass");
    builder.CreateCondBr(istype, passBB, failBB);
    builder.SetInsertPoint(failBB);

    emit_type_error(x, type, msg, ctx);

    builder.CreateBr(passBB);
    ctx->f->getBasicBlockList().push_back(passBB);
    builder.SetInsertPoint(passBB);
}

#define CHECK_BOUNDS 1

static Value *emit_bounds_check(Value *i, Value *len, jl_codectx_t *ctx)
{
    Value *im1 = builder.CreateSub(i, ConstantInt::get(T_size, 1));
#if CHECK_BOUNDS==1
    if (ctx->boundsCheck.empty() || ctx->boundsCheck.back()==true) {
        Value *ok = builder.CreateICmpULT(im1, len);
        raise_exception_unless(ok, jlboundserr_var, ctx);
    }
#endif
    return im1;
}

static void emit_func_check(Value *x, jl_codectx_t *ctx)
{
    Value *xty = emit_typeof(x);
    Value *isfunc =
        builder.
        CreateOr(builder.
                 CreateICmpEQ(xty,
                              literal_pointer_val((jl_value_t*)jl_function_type)),
                 builder.
                 CreateICmpEQ(xty,
                              literal_pointer_val((jl_value_t*)jl_datatype_type)));
    BasicBlock *elseBB1 = BasicBlock::Create(getGlobalContext(),"notf", ctx->f);
    BasicBlock *mergeBB1 = BasicBlock::Create(getGlobalContext(),"isf");
    builder.CreateCondBr(isfunc, mergeBB1, elseBB1);

    builder.SetInsertPoint(elseBB1);
    emit_type_error(x, (jl_value_t*)jl_function_type, "apply", ctx);

    builder.CreateBr(mergeBB1);
    ctx->f->getBasicBlockList().push_back(mergeBB1);
    builder.SetInsertPoint(mergeBB1);
}

// --- loading and storing ---

static Value *emit_nthptr_addr(Value *v, size_t n)
{
    return builder.CreateGEP(builder.CreateBitCast(v, jl_ppvalue_llvmt),
                             ConstantInt::get(T_size, n));
}

static Value *emit_nthptr_addr(Value *v, Value *idx)
{
    return builder.CreateGEP(builder.CreateBitCast(v, jl_ppvalue_llvmt), idx);
}

static Value *emit_nthptr(Value *v, size_t n)
{
    // p = (jl_value_t**)v; p[n]
    Value *vptr = emit_nthptr_addr(v, n);
    return builder.CreateLoad(vptr, false);
}

static Value *emit_nthptr(Value *v, Value *idx)
{
    // p = (jl_value_t**)v; p[n]
    Value *vptr = emit_nthptr_addr(v, idx);
    return builder.CreateLoad(vptr, false);
}

static Value *typed_load(Value *ptr, Value *idx_0based, jl_value_t *jltype,
                         jl_codectx_t *ctx)
{
    Type *elty = julia_type_to_llvm(jltype);
    assert(elty != NULL);
    bool isbool=false;
    if (elty==T_int1) { elty = T_int8; isbool=true; }
    Value *data = builder.CreateBitCast(ptr, PointerType::get(elty, 0));
    Value *elt = builder.CreateLoad(builder.CreateGEP(data, idx_0based), false);
    if (elty == jl_pvalue_llvmt) {
        null_pointer_check(elt, ctx);
    }
    if (isbool)
        return builder.CreateTrunc(elt, T_int1);
    return mark_julia_type(elt, jltype);
}

static Value *emit_unbox(Type *to, Value *x, jl_value_t *jt);

static Value *typed_store(Value *ptr, Value *idx_0based, Value *rhs,
                          jl_value_t *jltype, jl_codectx_t *ctx)
{
    Type *elty = julia_type_to_llvm(jltype);
    assert(elty != NULL);
    if (elty==T_int1) { elty = T_int8; }
    if (jl_isbits(jltype) && ((jl_datatype_t*)jltype)->size > 0)
        rhs = emit_unbox(elty, rhs, jltype);
    else
        rhs = boxed(rhs,ctx);
    Value *data = builder.CreateBitCast(ptr, PointerType::get(elty, 0));
    return builder.CreateStore(rhs, builder.CreateGEP(data, idx_0based));
}

// --- convert boolean value to julia ---

static Value *julia_bool(Value *cond)
{
    return builder.CreateSelect(cond,
                                literal_pointer_val(jl_true),
                                literal_pointer_val(jl_false));
}

// --- get the inferred type of an AST node ---

static jl_value_t *static_eval(jl_value_t *ex, jl_codectx_t *ctx, bool sparams=true,
                               bool allow_alloc=true);

static inline jl_module_t *topmod(jl_codectx_t *ctx)
{
    return jl_base_relative_to(ctx->module);
}

static jl_value_t *expr_type(jl_value_t *e, jl_codectx_t *ctx)
{
    if (jl_is_expr(e))
        return ((jl_expr_t*)e)->etype;
    if (jl_is_symbolnode(e))
        return jl_symbolnode_type(e);
    if (jl_is_quotenode(e))
        return (jl_value_t*)jl_typeof(jl_fieldref(e,0));
    if (jl_is_lambda_info(e))
        return (jl_value_t*)jl_function_type;
    if (jl_is_getfieldnode(e)) {
        jl_value_t *v = static_eval(e, ctx);
        if (v == NULL)
            return jl_getfieldnode_type(e);
        e = v;
        goto type_of_constant;
    }
    if (jl_is_topnode(e)) {
        e = jl_fieldref(e,0);
        jl_binding_t *b = jl_get_binding(topmod(ctx), (jl_sym_t*)e);
        if (!b || !b->value)
            return jl_top_type;
        if (b->constp) {
            e = b->value;
            goto type_of_constant;
        }
        else {
            return (jl_value_t*)jl_any_type;
        }
    }
    if (jl_is_symbol(e)) {
        if (jl_is_symbol(e)) {
            if (is_global((jl_sym_t*)e, ctx)) {
                // look for static parameter
                for(size_t i=0; i < jl_tuple_len(ctx->sp); i+=2) {
                    assert(jl_is_symbol(jl_tupleref(ctx->sp, i)));
                    if (e == jl_tupleref(ctx->sp, i)) {
                        e = jl_tupleref(ctx->sp, i+1);
                        goto type_of_constant;
                    }
                }
            }
            else {
                return (jl_value_t*)jl_any_type;
            }
        }
        jl_binding_t *b = jl_get_binding(ctx->module, (jl_sym_t*)e);
        if (!b || !b->value)
            return jl_top_type;
        if (b->constp)
            e = b->value;
        else
            return (jl_value_t*)jl_any_type;
    }
type_of_constant:
    if (jl_is_datatype(e) || jl_is_uniontype(e) || jl_is_typector(e))
        return (jl_value_t*)jl_wrap_Type(e);
    return (jl_value_t*)jl_typeof(e);
}

// --- accessing the representations of built-in data types ---

static Value *emit_tuplelen(Value *t,jl_value_t *jt)
{
    if (t == NULL)
        return ConstantInt::get(T_size,0);
    Type *ty = t->getType();
    if (ty == jl_pvalue_llvmt) { //boxed
    #ifdef OVERLAP_TUPLE_LEN
        Value *lenbits = emit_nthptr(t, (size_t)0);
        return builder.CreateLShr(builder.CreatePtrToInt(lenbits, T_int64),
                                  ConstantInt::get(T_int32, 52));
    #else
        Value *lenbits = emit_nthptr(t, 1);
        return builder.CreatePtrToInt(lenbits, T_size);
    #endif
    }
    else { //unboxed
        return ConstantInt::get(T_size,jl_tuple_len(jt));
    }
}

static Value *emit_tupleset(Value *tuple, Value *i, Value *x, jl_value_t *jt, jl_codectx_t *ctx)
{
    if (tuple == NULL) {
        // A typecheck must have caught this one
        //builder.CreateUnreachable();
        return NULL;
    }
    Type *ty = tuple->getType();
    if (ty == jl_pvalue_llvmt) { //boxed
    #ifdef OVERLAP_TUPLE_LEN
        Value *slot = builder.CreateGEP(builder.CreateBitCast(tuple, jl_ppvalue_llvmt),
                                 i);
    #else
        Value *slot = builder.CreateGEP(builder.CreateBitCast(tuple, jl_ppvalue_llvmt),
                                 builder.CreateAdd(ConstantInt::get(T_size,1),i));
    #endif
        builder.CreateStore(x,slot);
        return tuple;
    }
    else {
        Value *ret = NULL;
        ConstantInt *idx = dyn_cast<ConstantInt>(i);
        assert(idx != NULL && "tuplesets must use constant indices");
        if (ty->isVectorTy()) {
            Type *ity = i->getType();
            assert(ity->isIntegerTy());
            IntegerType *iity = dyn_cast<IntegerType>(ity);
            // ExtractElement needs i32 *sigh*
            if (iity->getBitWidth() > 32)
                i = builder.CreateTrunc(i,T_int32);
            else if (iity->getBitWidth() < 32)
                i = builder.CreateZExt(i,T_int32);
            ret = builder.CreateInsertElement(tuple,x,builder.CreateSub(i,ConstantInt::get(T_int32,1)));
        }
        else {
            unsigned ci = (unsigned)idx->getZExtValue()-1;
            size_t n = jl_tuple_len(jt);
            for (size_t i=0,j = 0; i<n; ++i) {
                Type *ty = julia_struct_to_llvm(jl_tupleref(jt,i));
                if (ci == i) {
                    if (ty == T_void || ty->isEmptyTy())
                        return tuple;
                    else
                        ret = builder.CreateInsertValue(tuple,x,ArrayRef<unsigned>(j));
                }
                if (ty != T_void)
                    ++j;
            }
        }
        return mark_julia_type(ret,jt);
    }
}

static Value *allocate_box_dynamic(Value *jlty, int nb, Value *v);
static void jl_add_linfo_root(jl_lambda_info_t *li, jl_value_t *val);

// Julia semantics
static Value *emit_tupleref(Value *tuple, Value *ival, jl_value_t *jt, jl_codectx_t *ctx)
{
    if (tuple == NULL) {
        // A typecheck must have caught this one
        //builder.CreateUnreachable();
        return NULL;
    }
    Type *ty = tuple->getType();
    if (ty == jl_pvalue_llvmt) { //boxed
#ifdef OVERLAP_TUPLE_LEN
        Value *slot = builder.CreateGEP(builder.CreateBitCast(tuple, jl_ppvalue_llvmt),ival);
#else
        Value *slot = builder.CreateGEP(builder.CreateBitCast(tuple, jl_ppvalue_llvmt),
                                        builder.CreateAdd(ConstantInt::get(T_size,1),ival));
#endif
        return builder.CreateLoad(slot);
    }
    ConstantInt *idx = dyn_cast<ConstantInt>(ival);
    unsigned ci = idx ? (unsigned)idx->getZExtValue()-1 : (unsigned)-1;
    if (ty->isVectorTy()) {
        Type *ity = ival->getType();
        assert(ity->isIntegerTy());
        IntegerType *iity = dyn_cast<IntegerType>(ity);
        // ExtractElement needs i32 *sigh*
        if (iity->getBitWidth() > 32)
            ival = builder.CreateTrunc(ival,T_int32);
        else if (iity->getBitWidth() < 32)
            ival = builder.CreateZExt(ival,T_int32);
        Value *v = builder.CreateExtractElement(tuple,builder.CreateSub(ival,ConstantInt::get(T_int32,1)));
        if (idx) {
            v = mark_julia_type(v,jl_tupleref(jt,ci));
        }
        else {
            if (sizeof(void*) != 4)
                ival = builder.CreateZExt(ival,T_size);
            jl_add_linfo_root(ctx->linfo, jt);
            v = allocate_box_dynamic(emit_tupleref(literal_pointer_val(jt),
                                                   ival, jl_typeof(jt), ctx),
                                     ty->getScalarSizeInBits(), v);
        }
        return v;
    }
    if (idx) {
        size_t n = jl_tuple_len(jt);
        for (size_t i = 0,j = 0; i<n; ++i) {
            Type *ty = julia_struct_to_llvm(jl_tupleref(jt,i));
            if (ci == i) {
                if (ty == T_void || ty->isEmptyTy())
                    return mark_julia_type(UndefValue::get(NoopType), jl_tupleref(jt,i));
                else
                    return mark_julia_type(builder.CreateExtractValue(tuple,ArrayRef<unsigned>(j)), jl_tupleref(jt,i));
            }
            if (ty != T_void) ++j;
        }
        assert(0 && "emit_tupleref must be called with an in-bounds index");
        return NULL;
    }
    if (ty->isArrayTy()) {
        ArrayType *at = dyn_cast<ArrayType>(ty);
        Value *tempSpace = builder.CreateAlloca(at);
        builder.CreateStore(tuple,tempSpace);
        Value *idxs[2];
        idxs[0] = ConstantInt::get(T_size,0);
        idxs[1] = builder.CreateSub(ival,ConstantInt::get(T_size,1));
        Value *v = builder.CreateGEP(tempSpace,ArrayRef<Value*>(&idxs[0],2));
        if (idx) {
            v = mark_julia_type(builder.CreateLoad(v), jl_tupleref(jt,ci));
        }
        else {
            jl_add_linfo_root(ctx->linfo, jt);
            Value *lty = emit_tupleref(literal_pointer_val(jt), ival, jl_typeof(jt), ctx);
            size_t i, l = jl_tuple_len(jt);
            for (i = 0; i < l; jt++)
                if (!jl_isbits(jl_tupleref(jt,i)))
                    return builder.CreateCall2(jlnewbits_func, boxed(lty,ctx),
                                               builder.CreatePointerCast(v,T_pint8));
            unsigned nb = (ty->getSequentialElementType()->getPrimitiveSizeInBits()+7)/8;
            v = allocate_box_dynamic(lty, nb, builder.CreateLoad(v));
        }
        return v;
    }
    assert(ty->isStructTy());
    StructType *st = dyn_cast<StructType>(ty);
    size_t n = st->getNumElements();
    Value *ret = builder.CreateAlloca(jl_pvalue_llvmt);
    BasicBlock *after = BasicBlock::Create(getGlobalContext(),"after_switch",ctx->f);
    BasicBlock *deflt = BasicBlock::Create(getGlobalContext(),"default_case",ctx->f);
    // Create the switch
    SwitchInst *sw = builder.CreateSwitch(ival,deflt,n);
    // Anything else is a bounds error
    builder.SetInsertPoint(deflt);
    builder.CreateCall2(jlthrow_line_func, builder.CreateLoad(jlboundserr_var),
                        ConstantInt::get(T_int32, ctx->lineno));
    builder.CreateUnreachable();
    // Now for the cases
    for (size_t i = 0, j = 0; i < jl_tuple_len(jt); ++i) {
        BasicBlock *blk = BasicBlock::Create(getGlobalContext(),"case",ctx->f);
        sw->addCase(ConstantInt::get((IntegerType*)T_size,i+1),blk);
        builder.SetInsertPoint(blk);
        jl_value_t *jltype = jl_tupleref(jt,i);
        Type *ty = julia_struct_to_llvm(jltype);
        Value *val;
        if (ty != T_void) {
            val = boxed(builder.CreateExtractValue(tuple,ArrayRef<unsigned>(j)),ctx,jltype);
            j++;
        }
        else {
            val = boxed(NULL,ctx,jltype);
        }
        builder.CreateStore(val,ret);
        builder.CreateBr(after);
    }
    builder.SetInsertPoint(after);
    return builder.CreateLoad(ret);
}


// emit length of vararg tuple
static Value *emit_n_varargs(jl_codectx_t *ctx)
{
    int nreq = ctx->nReqArgs;
    Value *valen = builder.CreateSub((Value*)ctx->argCount,
                                     ConstantInt::get(T_int32, nreq));
#ifdef _P64
    return builder.CreateSExt(valen, T_int64);
#else
    return valen;
#endif
}

static Value *emit_arraysize(Value *t, Value *dim)
{
#ifdef STORE_ARRAY_LEN
#ifdef _P64
    int o = 3;
#else
    int o = 4;
#endif
#else
#ifdef _P64
    int o = 2;
#else
    int o = 3;
#endif
#endif
    Value *dbits =
        emit_nthptr(t, builder.CreateAdd(dim,
                                         ConstantInt::get(dim->getType(), o)));
    return builder.CreatePtrToInt(dbits, T_size);
}

static jl_arrayvar_t *arrayvar_for(jl_value_t *ex, jl_codectx_t *ctx)
{
    if (ex == NULL) return NULL;
    jl_sym_t *aname=NULL;
    if (jl_is_symbol(ex))
        aname = ((jl_sym_t*)ex);
    else if (jl_is_symbolnode(ex))
        aname = jl_symbolnode_sym(ex);
    if (aname && ctx->arrayvars->find(aname) != ctx->arrayvars->end()) {
        return &(*ctx->arrayvars)[aname];
    }
    return NULL;
}

static Value *emit_arraysize(Value *t, int dim)
{
    return emit_arraysize(t, ConstantInt::get(T_int32, dim));
}

static Value *emit_arraylen_prim(Value *t, jl_value_t *ty)
{
#ifdef STORE_ARRAY_LEN
    (void)ty;
    Value *lenbits = emit_nthptr(t, 2);
    return builder.CreatePtrToInt(lenbits, T_size);
#else
    jl_value_t *p1 = jl_tparam1(ty);
    if (jl_is_long(p1)) {
        size_t nd = jl_unbox_long(p1);
        Value *l = ConstantInt::get(T_size, 1);
        for(size_t i=0; i < nd; i++) {
            l = builder.CreateMul(l, emit_arraysize(t, (int)(i+1)));
        }
        return l;
    }
    else {
        std::vector<Type *> fargt(0);
        fargt.push_back(jl_pvalue_llvmt);
        FunctionType *ft = FunctionType::get(T_size, fargt, false);
        Value *alen = jl_Module->getOrInsertFunction("jl_array_len_", ft);
        return builder.CreateCall(alen, t);
    }
#endif
}

static Value *emit_arraylen(Value *t, jl_value_t *ex, jl_codectx_t *ctx)
{
    jl_arrayvar_t *av = arrayvar_for(ex, ctx);
    if (av!=NULL)
        return builder.CreateLoad(av->len);
    return emit_arraylen_prim(t, expr_type(ex,ctx));
}

static Value *emit_arrayptr(Value *t)
{
    return emit_nthptr(t, 1);
}

static Value *emit_arrayptr(Value *t, jl_value_t *ex, jl_codectx_t *ctx)
{
    jl_arrayvar_t *av = arrayvar_for(ex, ctx);
    if (av!=NULL)
        return builder.CreateLoad(av->dataptr);
    return emit_arrayptr(t);
}

static Value *emit_arraysize(Value *t, jl_value_t *ex, int dim, jl_codectx_t *ctx)
{
    jl_arrayvar_t *av = arrayvar_for(ex, ctx);
    if (av != NULL && dim <= (int)av->sizes.size())
        return builder.CreateLoad(av->sizes[dim-1]);
    return emit_arraysize(t, dim);
}

static void assign_arrayvar(jl_arrayvar_t &av, Value *ar)
{
    builder.CreateStore(builder.CreateBitCast(emit_arrayptr(ar),T_pint8),
                        av.dataptr);
    builder.CreateStore(emit_arraylen_prim(ar, av.ty), av.len);
    for(size_t i=0; i < av.sizes.size(); i++)
        builder.CreateStore(emit_arraysize(ar,i+1), av.sizes[i]);
}

static Value *data_pointer(Value *x)
{
    return builder.CreateGEP(builder.CreateBitCast(x, jl_ppvalue_llvmt),
                             ConstantInt::get(T_size, 1));
}

static Value *emit_array_nd_index(Value *a, jl_value_t *ex, size_t nd, jl_value_t **args,
                                  size_t nidxs, jl_codectx_t *ctx)
{
    Value *i = ConstantInt::get(T_size, 0);
    Value *stride = ConstantInt::get(T_size, 1);
    bool bc = ctx->boundsCheck.empty() || ctx->boundsCheck.back()==true;
#if CHECK_BOUNDS==1
    BasicBlock *failBB=NULL, *endBB=NULL;
    if (bc) {
        failBB = BasicBlock::Create(getGlobalContext(), "oob");
        endBB = BasicBlock::Create(getGlobalContext(), "idxend");
    }
#endif
    for(size_t k=0; k < nidxs; k++) {
        Value *ii = emit_unbox(T_size, emit_unboxed(args[k], ctx), NULL);
        ii = builder.CreateSub(ii, ConstantInt::get(T_size, 1));
        i = builder.CreateAdd(i, builder.CreateMul(ii, stride));
        if (k < nidxs-1) {
            Value *d =
                k >= nd ? ConstantInt::get(T_size, 1) : emit_arraysize(a, ex, k+1, ctx);
#if CHECK_BOUNDS==1
            if (bc) {
                BasicBlock *okBB = BasicBlock::Create(getGlobalContext(), "ib");
                // if !(i < d) goto error
                builder.CreateCondBr(builder.CreateICmpULT(ii, d), okBB, failBB);
                ctx->f->getBasicBlockList().push_back(okBB);
                builder.SetInsertPoint(okBB);
            }
#endif
            stride = builder.CreateMul(stride, d);
        }
    }
#if CHECK_BOUNDS==1
    if (bc) {
        Value *alen = emit_arraylen(a, ex, ctx);
        // if !(i < alen) goto error
        builder.CreateCondBr(builder.CreateICmpULT(i, alen), endBB, failBB);

        ctx->f->getBasicBlockList().push_back(failBB);
        builder.SetInsertPoint(failBB);
        builder.CreateCall2(jlthrow_line_func, builder.CreateLoad(jlboundserr_var),
                            ConstantInt::get(T_int32, ctx->lineno));
        builder.CreateUnreachable();

        ctx->f->getBasicBlockList().push_back(endBB);
        builder.SetInsertPoint(endBB);
    }
#endif

    return i;
}

// --- propagate julia type from value a to b. returns b. ---

static Value *tpropagate(Value *a, Value *b)
{
    if (has_julia_type(a))
        return mark_julia_type(b, julia_type_of(a));
    return b;
}

// --- boxing ---

static Value *init_bits_value(Value *newv, Value *jt, Type *t, Value *v)
{
    builder.CreateStore(jt, builder.CreateBitCast(newv, jl_ppvalue_llvmt));
    builder.CreateStore(v , builder.CreateBitCast(data_pointer(newv),
                                                  PointerType::get(t,0)));
    return newv;
}

// allocate a box where the type might not be known at compile time
static Value *allocate_box_dynamic(Value *jlty, int nb, Value *v)
{
    if (v->getType()->isPointerTy()) {
        v = builder.CreatePtrToInt(v, T_size);
    }
    size_t sz = sizeof(void*) + nb;
    Value *newv = builder.CreateCall(jlallocobj_func,
                                     ConstantInt::get(T_size, sz));
    // TODO: make sure this is rooted. I think it is.
    return init_bits_value(newv, jlty, v->getType(), v);
}

static jl_value_t *static_void_instance(jl_value_t *jt)
{
    if (jl_is_datatype(jt)) {
        jl_datatype_t *jb = (jl_datatype_t*)jt;
        if (jb->instance == NULL)
            jl_new_struct_uninit(jb);
        assert(jb->instance != NULL);
        return (jl_value_t*)jb->instance;
    }
    else if (jt == jl_typeof(jl_nothing) || jt == jl_bottom_type) {
        return (jl_value_t*)jl_nothing;
    }
    assert(jl_is_tuple(jt));
    if (jl_tuple_len(jt) == 0)
        return (jl_value_t*)jl_null;
    size_t nargs = jl_tuple_len(jt);
    jl_value_t *tpl = (jl_value_t*)jl_alloc_tuple_uninit(nargs);
    JL_GC_PUSH1(&tpl);
    for(size_t i=0; i < nargs; i++) {
        jl_tupleset(tpl, i, static_void_instance(jl_tupleref(jt,i)));
    }
    JL_GC_POP();
    return tpl;
}

static jl_value_t *static_constant_instance(Constant *constant, jl_value_t *jt)
{
    assert(constant != NULL);

    ConstantInt *cint = dyn_cast<ConstantInt>(constant);
    if (cint != NULL) {
        assert(jl_is_datatype(jt));
        return jl_new_bits(jt,
            const_cast<uint64_t *>(cint->getValue().getRawData()));
    }

    ConstantFP *cfp = dyn_cast<ConstantFP>(constant);
    if (cfp != NULL) {
        assert(jl_is_datatype(jt));
        return jl_new_bits(jt,
            const_cast<uint64_t *>(cfp->getValueAPF().bitcastToAPInt().getRawData()));
    }

    ConstantPointerNull *cpn = dyn_cast<ConstantPointerNull>(constant);
    if (cpn != NULL) {
        assert(jl_is_cpointer_type(jt));
        uint64_t val = 0;
        return jl_new_bits(jt,&val);
    }

    assert(jl_is_tuple(jt));

    size_t nargs = 0;
    ConstantArray *carr = NULL;
    ConstantStruct *cst = NULL;
    ConstantVector *cvec = NULL;
    if ((carr = dyn_cast<ConstantArray>(constant)) != NULL)
        nargs = carr->getType()->getNumElements();
    else if ((cst = dyn_cast<ConstantStruct>(constant)) != NULL)
        nargs = cst->getType()->getNumElements();
    else if ((cvec = dyn_cast<ConstantVector>(constant)) != NULL)
        nargs = cvec->getType()->getNumElements();
    else
        assert(false && "Cannot process this type of constant");

    jl_value_t *tpl = (jl_value_t*)jl_alloc_tuple_uninit(nargs);
    JL_GC_PUSH1(&tpl);
    for(size_t i=0; i < nargs; i++) {
        jl_tupleset(tpl, i, static_constant_instance(
            constant->getAggregateElement(i),jl_tupleref(jt,i)));
    }
    JL_GC_POP();
    return tpl;
}


// this is used to wrap values for generic contexts, where a
// dynamically-typed value is required (e.g. argument to unknown function).
// if it's already a pointer it's left alone.
static Value *boxed(Value *v,  jl_codectx_t *ctx, jl_value_t *jt)
{
    Type *t = (v == NULL) ? NULL : v->getType();
    if (v == NULL || dyn_cast<UndefValue>(v) != 0 || t == NoopType) {
        if (jt == NULL || jl_is_uniontype(jt) || jl_is_abstracttype(jt))
            jt = julia_type_of(v);
        jl_value_t *s = static_void_instance(jt);
        if (jl_is_tuple(jt) && jl_tuple_len(jt) > 0)
            jl_add_linfo_root(ctx->linfo, s);
        return literal_pointer_val(s);
    }
    if (t == jl_pvalue_llvmt)
        return v;
    if (t == T_int1) return julia_bool(v);
    if (jt == NULL || jl_is_uniontype(jt) || jl_is_abstracttype(jt))
        jt = julia_type_of(v);
    if (t == T_void || t->isEmptyTy()) {
        jl_value_t *s = static_void_instance(jt);
        if (jl_is_tuple(jt) && jl_tuple_len(jt) > 0)
            jl_add_linfo_root(ctx->linfo, s);
        return literal_pointer_val(s);
    }
    Constant *c = NULL;
    if ((c = dyn_cast<Constant>(v)) != NULL) {
        jl_value_t *s = static_constant_instance(c,jt);
        jl_add_linfo_root(ctx->linfo, s);
        return literal_pointer_val(s);
    }
    if (jl_is_tuple(jt)) {
        size_t n = jl_tuple_len(jt);
        Value *tpl = builder.CreateCall(jl_alloc_tuple_func,ConstantInt::get(T_size,n));
        int last_depth = ctx->argDepth;
        make_gcroot(tpl,ctx);
        for (size_t i = 0; i < n; ++i) {
            jl_value_t *jti = jl_tupleref(jt,i);
            Value *vi = emit_tupleref(v,ConstantInt::get(T_size,i+1),jt,ctx);
            emit_tupleset(tpl,ConstantInt::get(T_size,i+1),boxed(vi,ctx,jti),jt,ctx);
        }
        ctx->argDepth = last_depth;
        return tpl;
    }
    jl_datatype_t *jb = (jl_datatype_t*)jt;
    assert(jl_is_datatype(jb));
    if (jb == jl_int8_type)
        return builder.CreateCall(box_int8_func,
                                  builder.CreateSExt(v, T_int32));
    if (jb == jl_int16_type) return builder.CreateCall(box_int16_func, v);
    if (jb == jl_int32_type) return builder.CreateCall(box_int32_func, v);
    if (jb == jl_int64_type) return builder.CreateCall(box_int64_func, v);
    if (jb == jl_float32_type) return builder.CreateCall(box_float32_func, v);
    //if (jb == jl_float64_type) return builder.CreateCall(box_float64_func, v);
    if (jb == jl_float64_type) {
        // manually inline alloc & init of Float64 box. cheap, I know.
#ifdef _P64
        Value *newv = builder.CreateCall(jlalloc2w_func);
#else
        Value *newv = builder.CreateCall(jlalloc3w_func);
#endif
        return init_bits_value(newv, literal_pointer_val(jt), t, v);
    }
    if (jb == jl_uint8_type)
        return builder.CreateCall(box_uint8_func,
                                  builder.CreateZExt(v, T_int32));
    if (jb == jl_uint16_type) return builder.CreateCall(box_uint16_func, v);
    if (jb == jl_uint32_type) return builder.CreateCall(box_uint32_func, v);
    if (jb == jl_uint64_type) return builder.CreateCall(box_uint64_func, v);
    if (jb == jl_char_type)   return builder.CreateCall(box_char_func, v);
    if (!jl_isbits(jt)) {
        assert("Don't know how to box this type" && false);
        return NULL;
    }
    if (!jb->abstract && jb->size == 0) {
        if (jb->instance == NULL)
            jl_new_struct_uninit(jb);
        assert(jb->instance != NULL);
        return literal_pointer_val(jb->instance);
    }
    return allocate_box_dynamic(literal_pointer_val(jt),jl_datatype_size(jt),v);
}

static void emit_cpointercheck(Value *x, const std::string &msg,
                               jl_codectx_t *ctx)
{
    Value *t = emit_typeof(x);
    emit_typecheck(t, (jl_value_t*)jl_datatype_type, msg, ctx);

    Value *istype =
        builder.CreateICmpEQ(emit_nthptr(t, offsetof(jl_datatype_t,name)/sizeof(char*)),
                             literal_pointer_val((jl_value_t*)jl_pointer_type->name));
    BasicBlock *failBB = BasicBlock::Create(getGlobalContext(),"fail",ctx->f);
    BasicBlock *passBB = BasicBlock::Create(getGlobalContext(),"pass");
    builder.CreateCondBr(istype, passBB, failBB);
    builder.SetInsertPoint(failBB);

    emit_type_error(x, (jl_value_t*)jl_pointer_type, msg, ctx);

    builder.CreateBr(passBB);
    ctx->f->getBasicBlockList().push_back(passBB);
    builder.SetInsertPoint(passBB);
}
