#include "ctfe/ctfe.h"
#include <stdio.h>
#include "sema/reflect.h"
#include "sema/sema.h"

#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"
#include "parser/ast_array.h"
#include "parser/ast_alignof.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_member.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_volatile.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_func_type.h"
#include "parser/ast_fill.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_new.h"
#include "parser/ast_ptr.h"
#include "parser/ast_owned_type.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_typeof.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/ast_var_def.h"
#include "parser/lexer.h"
#include "parser/parse_utils.h"
#include "sema/symbol.h"
#include "vm/bcode_function.h"  /* func_instantiate_raw：常量闭包实例化 */
#include "vm/function.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_error.h"
#include "vm/type_cfunc.h"
#include "vm/type_func.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_ownership.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/value.h"

/* 类型表达式里的所有权修饰 token → ownership_t。 */
static ownership_t ctfe_token_ownership(const token_t *tok) {
    if (!tok) return OWN_NONE;
    strslice_t own = token_strslice(tok);
    if (own.len == 3 && own.ptr && memcmp(own.ptr, "own", 3) == 0)
        return OWN_OWN;
    if (own.len == 3 && own.ptr && memcmp(own.ptr, "ref", 3) == 0)
        return OWN_REF;
    if (own.len == 5 && own.ptr && memcmp(own.ptr, "fatal", 5) == 0)
        return OWN_FATAL;
    return OWN_NONE;
}

/* 在类型外包装所有权修饰（布局不变），返回 type value。 */
static value_t *ctfe_owned_type_value(ctfe_ctx_t *ctx, const type_t *t,
                                      ownership_t own) {
    const type_t *w = type_ownership_apply(ctx->vm, own, t);
    if (!w) return ctfe_err(ctx, "ctfe: failed to construct ownership type");
    return type_as_value(ctx->vm, w);
}


/* ===========================================================================
 * 表达式求值
 *
 * 每个表达式产生真实 value（track 到 vm->current_scope）。失败返回 error
 * value。短路 &&/|| 惰性求值（不走 vtable 二元分派，与运行期解释器一致）。
 * =========================================================================== */

/* 按 id 查 vm->functions 里的 func_t（线性扫描，函数数量少）。覆盖：
   - 内建函数（printf，func_new 注册，id < FUNC_ID_PROGRAM_BASE）
   - CTFE 求值函数字面量时 func_new_program_ref 构造的引用对象（同一 fid
     可能被多次构造，全部注册进 vm->functions 统一释放）
   程序函数（全局/局部）编译期不在 vm->functions——按 fid 查 sema->funcs。 */
static func_t *ctfe_func_by_id(vm_t *vm, uint32_t fid) {
    if (!vm || !vm->functions) return NULL;
    size_t n = vec_len(vm->functions);
    for (size_t i = 0; i < n; i++) {
        func_t *fn = (func_t *)vec_get(vm->functions, i);
        if (fn && fn->id == fid && fn->type) return fn;
    }
    return NULL;
}

/* 函数字面量求值（comptime body 内 AST_FUNC_DEF）：求值参数类型 + 返回
   类型 → type_func_sig 构造签名 → 登记（写回 fn->sig_id，compiler hoist
   LOAD_TYPE 用）→ 分配 fid（幂等：sema_check_func_literal 可能已分配）→
   构造引用 func_t → 包装 func value。具名字面量绑定到当前 vm 作用域
   （comptime body 内 `func inc(){}` 语句 + `return inc` 引用）。 */
static value_t *ctfe_eval_func_def(ctfe_ctx_t *ctx, ast_func_def_t *n) {
    vm_t *vm = ctx->vm;
    size_t nparams = ctfe_count_siblings(n->params);
    const type_t *params_arr[nparams > 0 ? nparams : 1];
    size_t i = 0;
    for (ast_node_t *p = n->params; p; p = p->next, i++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        value_t *pt = ctfe_eval(ctx, vd->type_expr);
        if (value_is_error(vm, pt)) return pt;
        if (!value_is_type(pt, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: func literal parameter type must be a type");
        params_arr[i] = value_as(pt, const type_t *);
    }
    const type_t *ret = NULL;
    if (n->return_expr) {
        value_t *rt = ctfe_eval(ctx, n->return_expr);
        if (value_is_error(vm, rt)) return rt;
        if (!value_is_type(rt, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: func literal return type must be a type");
        ret = value_as(rt, const type_t *);
    }
    const type_t *sig = type_func_sig(vm, nparams > 0 ? params_arr : NULL,
                                      nparams, ret,
                                      /*is_variadic=*/false);
    if (!sig)
        return ctfe_err(ctx, "ctfe: failed to construct function literal signature");

    /* 登记签名类型（按指针去重幂等）并写回 fn->sig_id——compiler hoist
       函数注册区发 LOAD_TYPE <sig_id> 需要。sema walk（sema_check_func_literal）
       可能晚于此（pass_globals 早于 Pass 3b），两者写回同一 id。 */
    if (ctx->sema) {
        const sema_type_t *st = sema_type_register(ctx->sema, sig);
        if (st) n->sig_id = st->id;
    }

    /* fid 单一来源在 sema：幂等分配（sema_check_func_literal 已分配则复用）。
       eval 场景（ctx->sema == NULL）无 fid，引用对象 id=0（仅求值显示用）。 */
    uint32_t fid = ctx->sema ? sema_func_id_alloc(ctx->sema, n) : 0;

    func_t *fn = func_new_program_ref(vm, sig, n->name, fid);
    if (!fn)
        return ctfe_err(ctx, "ctfe: out of memory creating function literal");
    fn->ct_literal = n; /* 常量闭包折叠需要字面量 AST（见 function.h） */

    /* 带捕获的字面量 = **编译期常量闭包**：把捕获值物化进实例 env。
       —— 与运行期 `MAKE_FUNCTION <fid>` + `SET_CLOSURE <name>` 完全同构：
          实例持有 capture_type（捕获聚合结构体）+ env 块，捕获值按槽位写入。
       —— 为什么必须现在做：comptime 函数返回闭包时，返回的是这个 func 值；
          没有 env 的话编码出来的常量只有 func_id，运行期调用会报
          "closure has no captures"。
       —— 捕获类型此时由**捕获值的类型**就地合成（sema 的 Pass 3b 尚未跑到字面量，
          n->capture_type_id 还是 0）；稍后 sema 按同样字段（名/类型/OWN_NONE）
          重算会**去重**到同一个 struct 类型。捕获名两端一致，SET_CLOSURE 的
          字段偏移因此不会错位。
       —— 捕获值必须是编译期常量（编码阶段 sema_ct_encode 递归校验）；
          引用运行期变量在这里就报错，而不是留到折叠失败。 */
    if (n->captures) {
        size_t nc = 0;
        for (ast_node_t *c = n->captures; c; c = c->next) nc++;
        value_t **capv = (value_t **)allocator_new_ex(
            vm->alloc, "ctfe_captures", sizeof(value_t *), NULL, NULL, NULL,
            nc);
        struct_field_t *fields = (struct_field_t *)allocator_new_ex(
            vm->alloc, "ctfe_capture_fields", sizeof(struct_field_t), NULL, NULL,
            NULL, nc);
        if (!capv || !fields) {
            if (capv) allocator_free(vm->alloc, (void **)&capv);
            if (fields) allocator_free(vm->alloc, (void **)&fields);
            return ctfe_err(ctx, "ctfe: out of memory capturing closure");
        }
        size_t k = 0;
        value_t *bad = NULL;
        for (ast_node_t *c = n->captures; c; c = c->next, k++) {
            ast_var_def_t *cv = (ast_var_def_t *)c;
            value_t *cap = NULL;
            if (cv->init) {
                cap = ctfe_eval(ctx, cv->init);
                if (value_is_error(vm, cap)) {
                    bad = cap;
                    break;
                }
            } else {
                cap = scope_lookup(vm->current_scope, cv->name);
                if (!cap) {
                    bad = ctfe_errf(
                        ctx, "ctfe: cannot capture undefined variable '%.*s'",
                        (int)cv->name.len, cv->name.ptr);
                    break;
                }
                if (value_is_shadow(cap)) {
                    bad = ctfe_errf(
                        ctx,
                        "ctfe: cannot capture runtime variable '%.*s' "
                        "(closures returned from comptime functions may only "
                        "capture compile-time constants)",
                        (int)cv->name.len, cv->name.ptr);
                    break;
                }
            }
            capv[k] = cap;
            memset(&fields[k], 0, sizeof(fields[k]));
            fields[k].name = cv->name;
            fields[k].type = value_type(cap);
            fields[k].ownership = OWN_NONE; /* 编译期常量：无堆所有权 */
        }
        if (!bad) {
            const type_t *ct = type_struct_intern(vm, fields, nc);
            if (!ct) bad = ctfe_err(ctx, "ctfe: cannot intern capture type");
            /* 写回 capture_type_id：编译期要能为该字面量生成 SET_FUNC_CAPTURES
               （否则运行期实例没有 env，SET_CLOSURE 报 "function has no capture
               env"）。 */
            if (ct && ctx->sema) {
                const sema_type_t *st = sema_type_register(ctx->sema, ct);
                if (st) n->capture_type_id = st->id;
            }
            if (!bad) {
                fn->capture_type = (const struct_type_t *)ct;
                fn->env_dispose_fid = n->env_dispose_fid;
                fn->env_clone_fid = n->env_clone_fid;
                func_t *inst = func_instantiate_raw(vm, fn);
                if (!inst) bad = ctfe_err(ctx, "ctfe: out of memory creating closure instance");
                for (size_t i = 0; !bad && i < nc; i++) {
                    ast_var_def_t *cv = (ast_var_def_t *)n->captures;
                    for (size_t j = 0; j < i && cv; j++) cv = (ast_var_def_t *)cv->base.next;
                    const char *cerr =
                        cv ? func_closure_store(vm, inst, cv->name, capv[i]) : "capture name missing";
                    if (cerr) bad = ctfe_errf(ctx, "ctfe: %s", cerr);
                }
                if (!bad) fn = inst; /* 值里放**实例**（实例归 vm->functions 所有） */
            }
        }
        allocator_free(vm->alloc, (void **)&capv);
        allocator_free(vm->alloc, (void **)&fields);
        if (bad) return bad;
    }

    void *data = value_alloc_data_copy(vm->alloc, sig, &fn);
    value_t *fv = value_make(vm, sig, data);

    /* 具名字面量绑定到当前 vm 作用域（comptime body 内函数定义语句 +
       后续名字引用）。scope_define clone 到作用域 owned，引用值保留。 */
    if (n->name.len > 0) {
        char nb[128];
        const char *name = ctfe_slice_to_cstr(n->name, nb, sizeof nb);
        /* 幂等：同一个具名字面量可能被**多次**求值（sema 试跑轮 + 正式轮都会
           折叠同一个全局初始化器），第二次 scope_define 会报
           "duplicate variable definition"。已存在则覆盖。 */
        value_t *stored = scope_lookup(vm->current_scope, n->name)
                              ? scope_set(vm, vm->current_scope, name, fv)
                              : scope_define(vm, vm->current_scope, name, fv);
        if (value_is_error(vm, stored)) return stored;
    }
    return fv;
}

/* 编译期运算符（sizeof/alignof/typeof，m2-design §8）的操作数类型提取：
 * "操作数按类型（shadow 语义）求值，结果产生真实常量"——操作数只取类型，
 * 不要求真实执行。三路解析：
 *   1. 常规 ctfe 求值成功：
 *      - type value → value_as 取类型
 *      - 普通值 → value_type
 *   2. ctfe 求值失败（操作数是运行期 shadow 值，如 `[sizeof(a)]i32` 的
 *      `a`）：scope_lookup 在当前 VM scope 链查到 shadow value → 其类型即
 *      操作数类型（shadow 语义：只取类型不执行）
 *   3. AST_IDENT + sema 上下文兜底：sema 符号表查类型（type 别名/未进 VM
 *      scope 的符号）。
 * 返回 NULL = 无法确定类型（调用方报错）。 */
/* 编译期构造 struct/tuple 值：按类型表布局把字段值数据 blit 到目标块
   （与运行期 op_construct 的 struct/tuple 分支同语义——字段序 = 类型表序，
   未提供/零字段留零）。用于 `comptime var S = .P{...}` 这类聚合常量：
   CTFE 必须能构造真实聚合值，编码（sema_ct_encode）才能按字段递归折叠。
   局限：字段为内嵌聚合时按字节 blit（浅拷贝）；含 own/RC 指针字段的编译期
   聚合常量会共享内部块（编译期常量本就不应携带堆所有权，m3 全局 own 亦禁止）。 */
static value_t *ctfe_make_aggregate(vm_t *vm, const type_t *t,
                                    value_t **fields, size_t n) {
    void *data = value_alloc_data(vm->alloc, t);
    if (!data) return NULL;
    memset(data, 0, t->size);
    for (size_t i = 0; i < n; i++) {
        if (!fields[i]) continue;
        const type_t *ft = NULL;
        size_t off = 0;
        if (type_kind_of(t) == TYPE_KIND_STRUCT) {
            const struct_field_t *f = struct_type_field(t, i);
            if (f) { ft = f->type; off = f->offset; }
        } else {
            const tuple_elem_t *e = tuple_type_elem(t, i);
            if (e) { ft = e->type; off = e->offset; }
        }
        if (!ft) continue;
        const void *src = value_data(fields[i]);
        if (src && ft->size) memcpy((char *)data + off, src, ft->size);
    }
    return value_make(vm, t, data);
}

/* 沿 sema 作用域链查找**符号本身**，不看激活状态（is_active）。
 *
 * 为什么需要：`type T = typeof(a)` 的 RHS 在**块入口**求值（局部 type 提升），
 * 此时同块里 `var a` 还没走到定义点，符号未激活；而 sema_lookup 会跳过未激活
 * 符号（那是为了 TDZ 语义）。typeof/sizeof/alignof 只要**静态类型**，与"值是否
 * 已初始化"无关，因此这里退一步按声明查找。 */
static sema_symbol_t *ctfe_lookup_symbol_any(const sema_scope_t *scope,
                                             strslice_t name) {
    for (const sema_scope_t *s = scope; s; s = s->parent) {
        sema_symbol_t *sym = sema_scope_find_local(s, name);
        if (sym) return sym;
    }
    return NULL;
}

static const type_t *ctfe_operand_type(ctfe_ctx_t *ctx, ast_node_t *opnd) {
    vm_t *vm = ctx->vm;
    value_t *v = ctfe_eval(ctx, opnd);
    if (!value_is_error(vm, v)) {
        if (value_is_type(v, TYPE_KIND_TYPE))
            return value_as(v, const type_t *);
        return value_type(v);
    }
    /* 操作数为运行期值（shadow）：scope_lookup 查到 shadow value 即类型 */
    if (opnd && opnd->kind == AST_IDENT) {
        ast_ident_t *id = (ast_ident_t *)opnd;
        value_t *sv = scope_lookup(vm->current_scope, id->name);
        if (sv) return value_type(sv);
        if (ctx->sema && ctx->sema->global_scope) {
            sema_symbol_t *sym = ctx->sema_scope
                                     ? sema_lookup(ctx->sema_scope, id->name)
                                     : NULL;
            if (!sym)
                sym = sema_lookup(ctx->sema->global_scope, id->name);
            /* 提升期（type def 在块入口求值）局部 var 尚未激活 → 按声明查找 */
            if (!sym && ctx->sema_scope)
                sym = ctfe_lookup_symbol_any(ctx->sema_scope, id->name);
            if (sym && sym->type) return sym->type;
        }
    }
    return NULL;
}

value_t *ctfe_eval_inner(ctfe_ctx_t *ctx, ast_node_t *node) {
    vm_t *vm = ctx->vm;
    if (!node) return ctfe_err(ctx, "ctfe: null expression node");

    switch (node->kind) {
    case AST_INT_LIT: {
        ast_int_lit_t *n = (ast_int_lit_t *)node;
        const type_t *t = ctfe_int_lit_type(ctx, n->type);
        if (!t) return ctfe_err(ctx, "ctfe: unsupported integer literal type");
        return ctfe_make_int(ctx, t, n->value);
    }
    case AST_FLOAT_LIT: {
        ast_float_lit_t *n = (ast_float_lit_t *)node;
        if (strslice_eq(n->type, STRSLICE_LIT("f32"))) {
            float f = (float)n->value;
            void *data = value_alloc_data_copy(vm->alloc, vm->type_f32, &f);
            return value_make(vm, vm->type_f32, data);
        }
        void *data = value_alloc_data_copy(vm->alloc, vm->type_f64, &n->value);
        return value_make(vm, vm->type_f64, data);
    }
    case AST_BOOL_LIT: {
        ast_bool_lit_t *n = (ast_bool_lit_t *)node;
        void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &n->value);
        return value_make(vm, vm->type_bool, data);
    }
    case AST_CHAR_LIT: {
        ast_char_lit_t *n = (ast_char_lit_t *)node;
        return ctfe_make_int(ctx, vm->type_u8, n->value);
    }
    case AST_STRING_LIT: {
        ast_string_lit_t *n = (ast_string_lit_t *)node;
        /* 字符串走 vm 字符串池 intern（data 全平凡：直接存池内指针） */
        const char *s = vm_str_intern_len(vm, n->text.ptr, n->text.len);
        void *data = value_alloc_data_copy(vm->alloc, vm->type_str, &s);
        return value_make(vm, vm->type_str, data);
    }
    case AST_IDENT: {
        ast_ident_t *n = (ast_ident_t *)node;
        value_t *v = scope_lookup(vm->current_scope, n->name);
        if (!v && ctx->sema && ctx->sema->global_scope) {
            /* 函数引用（函数值，编译期/运行期二元）：VM scope 查不到（程序
               函数名不在运行时 scope）→ 回退查 sema 符号表，命中用户函数
               （AST_FUNC_DEF，非 comptime）→ 构造引用 func_t（cfunc=NULL，
               不可调用，仅携带签名+名字）→ 包装为 func value。折叠终点是
               AST_FUNC_REF（sema_ct_lit 按名字产出），运行期 LOAD_FUNCTION
               加载真实函数。内建函数（printf）已在 VM scope，直接命中上方。 */
            sema_symbol_t *sym =
                sema_lookup(ctx->sema->global_scope, n->name);
            if (sym && sym->kind == SEMA_SYM_FUNC && sym->type &&
                type_kind_of(sym->type) == TYPE_KIND_FUNC) {
                if (sym->is_comptime) {
                    return ctfe_errf(ctx,
                                "ctfe: comptime function '%.*s' cannot be used as a value",
                                (int)n->name.len, n->name.ptr);
                }
                ast_func_def_t *fd = (ast_func_def_t *)sym->ast;
                /* 引用对象携带 sema 分配的 fid（折叠编码 LOAD_FUNCTION
                   <fid> 的依据，与函数是否有 name 无关） */
                func_t *fn = func_new_program_ref(vm, sym->type, fd->name,
                                                  fd->fid);
                if (!fn)
                    return ctfe_err(ctx, "ctfe: out of memory creating function reference");
                void *data = value_alloc_data_copy(vm->alloc, sym->type, &fn);
                return value_make(vm, sym->type, data);
            }
        }
        if (!v) return ctfe_err(ctx, "ctfe: undefined variable (not compile-time)");
        /* shadow value（data=NULL）是 sema 阶段运行期变量的占位：无真实
           数据可读，不是编译期常量 */
        if (value_is_shadow(v))
            return ctfe_err(ctx, "ctfe: variable is not a compile-time constant");
        return v; /* 借用引用，归 scope */
    }
    case AST_CONST: {
        ast_const_t *n = (ast_const_t *)node;
        value_t *sub = ctfe_eval(ctx, n->sub);
        if (value_is_error(vm, sub)) return sub;
        if (!value_is_type(sub, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: 'const' requires a type operand");
        const type_t *inner = value_as(sub, const type_t *);
        return type_as_value(vm, type_const_intern(vm, inner));
    }
    case AST_VOLATILE: {
        ast_volatile_t *n = (ast_volatile_t *)node;
        value_t *sub = ctfe_eval(ctx, n->sub);
        if (value_is_error(vm, sub)) return sub;
        if (!value_is_type(sub, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: 'volatile' requires a type operand");
        /* `volatile T` 在脚本态无意义 → 折叠为 T（与 sema 的类型解析、compiler 的
           类型构造一致）。类型别名 `type X = [2]volatile i32;` 走的就是这条 CTFE
           路径，漏掉它会让声明侧留下 volatile 类型而初始化值没有。 */
        return sub;
    }
    case AST_SIZEOF:
    case AST_ALIGNOF:
    case AST_TYPEOF: {
        /* 编译期运算符（m2-design §8）：操作数按类型（shadow 语义）求值，
           结果产生真实常量——sizeof/alignof → u64 立即值（ctfe_make_int），
           typeof → type value。数组边界 [sizeof(a)]i32 经 ctfe_operand_type
           从运行期 shadow 值取类型，产出真实常量喂给边界槽位。 */
        ast_node_t *opnd = node->kind == AST_TYPEOF
                               ? ((ast_typeof_t *)node)->operand
                               : (node->kind == AST_SIZEOF
                                      ? ((ast_sizeof_t *)node)->operand
                                      : ((ast_alignof_t *)node)->operand);
        const type_t *t = ctfe_operand_type(ctx, opnd);
        if (!t) return ctfe_err(ctx, "ctfe: cannot determine operand type");
        if (node->kind == AST_TYPEOF) return type_as_value(vm, t);
        uint64_t n = node->kind == AST_SIZEOF ? (uint64_t)t->size
                                              : (uint64_t)t->align;
        /* 宽度/对齐结果 = usize（C 的 size_t） */
        return ctfe_make_int(ctx, vm_type_usize(vm), n);
    }
    case AST_BINARY: {
        ast_binary_t *n = (ast_binary_t *)node;
        /* 短路 && / ||：惰性求值，不走 vtable 二元分派 */
        if (token_is(n->op, "&&") || token_is(n->op, "||")) {
            bool is_and = token_is(n->op, "&&");
            value_t *lhs = ctfe_eval(ctx, n->lhs);
            if (value_is_error(vm, lhs)) return lhs;
            if (value_type(lhs) != vm->type_bool)
                return ctfe_err(ctx, "ctfe: '&&'/'||' requires bool operands");
            bool lv = ctfe_read_bool(vm, lhs);
            if (is_and && !lv) {
                bool f = false;
                void *d = value_alloc_data_copy(vm->alloc, vm->type_bool, &f);
                return value_make(vm, vm->type_bool, d);
            }
            if (!is_and && lv) {
                bool t = true;
                void *d = value_alloc_data_copy(vm->alloc, vm->type_bool, &t);
                return value_make(vm, vm->type_bool, d);
            }
            value_t *rhs = ctfe_eval(ctx, n->rhs);
            if (value_is_error(vm, rhs)) return rhs;
            if (value_type(rhs) != vm->type_bool)
                return ctfe_err(ctx, "ctfe: '&&'/'||' requires bool operands");
            return rhs;
        }
        /* as：显式类型转换。lhs/rhs 都按普通表达式求值——rhs 是类型
           表达式，求值结果应为 type value（data=type_t*）。遮蔽感知：
           类型名走 scope_lookup 与变量同机制。 */
        if (token_is(n->op, "as")) {
            value_t *lhs = ctfe_eval(ctx, n->lhs);
            if (value_is_error(vm, lhs)) return lhs;
            value_t *ty = ctfe_eval(ctx, n->rhs);
            if (value_is_error(vm, ty)) return ty;
            if (!value_is_type(ty, TYPE_KIND_TYPE))
                return ctfe_err(ctx, "ctfe: cast target must be a type");
            const type_t *target = value_as(ty, const type_t *);
            return value_explicit_cast(vm, lhs, target);
        }
        /* extends：类型兼容判断（编译期类型计算）。两侧按普通表达式求值
           ——都是类型表达式，结果应为 type value（data=type_t*）。分派
           value_extends → vtable->extends（type value 的 VTABLE_TYPE.extends
           → value_type_extends：非 type value 报 "extends: type value required"）。 */
        if (token_is(n->op, "extends")) {
            value_t *lhs = ctfe_eval(ctx, n->lhs);
            if (value_is_error(vm, lhs)) return lhs;
            value_t *rhs = ctfe_eval(ctx, n->rhs);
            if (value_is_error(vm, rhs)) return rhs;
            return value_extends(vm, lhs, rhs);
        }
        value_t *lhs = ctfe_eval(ctx, n->lhs);
        if (value_is_error(vm, lhs)) return lhs;
        value_t *rhs = ctfe_eval(ctx, n->rhs);
        if (value_is_error(vm, rhs)) return rhs;
        if (token_is(n->op, "+"))  return value_add(vm, lhs, rhs);
        if (token_is(n->op, "-"))  return value_sub(vm, lhs, rhs);
        if (token_is(n->op, "*"))  return value_mul(vm, lhs, rhs);
        if (token_is(n->op, "/"))  return value_div(vm, lhs, rhs);
        if (token_is(n->op, "%"))  return value_mod(vm, lhs, rhs);
        if (token_is(n->op, "==")) return value_eq(vm, lhs, rhs);
        if (token_is(n->op, "!=")) return value_ne(vm, lhs, rhs);
        if (token_is(n->op, "<"))  return value_lt(vm, lhs, rhs);
        if (token_is(n->op, "<=")) return value_le(vm, lhs, rhs);
        if (token_is(n->op, ">"))  return value_gt(vm, lhs, rhs);
        if (token_is(n->op, ">=")) return value_ge(vm, lhs, rhs);
        if (token_is(n->op, "&"))  return value_band(vm, lhs, rhs);
        if (token_is(n->op, "|"))  return value_bor(vm, lhs, rhs);
        if (token_is(n->op, "^"))  return value_bxor(vm, lhs, rhs);
        if (token_is(n->op, "<<")) return value_shl(vm, lhs, rhs);
        if (token_is(n->op, ">>")) return value_shr(vm, lhs, rhs);
        return ctfe_err(ctx, "ctfe: unsupported binary operator");
    }
    case AST_UNARY: {
        ast_unary_t *n = (ast_unary_t *)node;
        value_t *v = ctfe_eval(ctx, n->operand);
        if (value_is_error(vm, v)) return v;
        if (token_is(n->op, "-"))  return value_neg(vm, v);
        if (token_is(n->op, "!"))  return value_lnot(vm, v);
        if (token_is(n->op, "~"))  return value_bnot(vm, v);
        return ctfe_err(ctx, "ctfe: unsupported unary operator");
    }
    /* 反射注入（`T.key` / `T.key(i)`）：CTFE 里同样按类型元数据折叠。
       `type Alias = T.field_type(0);` 这类类型定义 RHS 由 CTFE 求值，所以
       反射必须在这里也能用（与 sema 的折叠共用 sema_reflect_lookup）。 */
    case AST_MEMBER: {
        ast_member_t *m = (ast_member_t *)node;
        /* 非反射成员：CTFE 不支持普通成员访问，报同款错误（原 default 文案）。 */
        if (!sema_reflect_is_key(m->field))
            return ctfe_err(ctx, "ctfe: unsupported expression node");
        value_t *tv = ctfe_eval(ctx, m->object);
        if (value_is_error(vm, tv)) return tv;
        if (!value_is_type(tv, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: reflection receiver is not a type");
        const type_t *T = value_as(tv, const type_t *);
        sema_rv_t rv;
        char rerr[512] = {0};
        if (!sema_reflect_eval(ctx->sema, T, m->field, NULL, &rv, rerr,
                               sizeof rerr))
            return ctfe_err(ctx, rerr);
        value_t *out = sema_reflect_value(vm, &rv);
        return out ? out : ctfe_err(ctx, "ctfe: reflection produced no value");
    }

    case AST_CALL: {
        /* 反射调用 `T.key(i)`：callee 是成员访问、接收者是类型、key 带索引。 */
        {
            ast_call_t *rc = (ast_call_t *)node;
            if (rc->callee && rc->callee->kind == AST_MEMBER) {
                ast_member_t *rm = (ast_member_t *)rc->callee;
                if (sema_reflect_is_key(rm->field)) {
                    value_t *tv = ctfe_eval(ctx, rm->object);
                    if (value_is_error(vm, tv)) return tv;
                    if (value_is_type(tv, TYPE_KIND_TYPE)) {
                        const type_t *T = value_as(tv, const type_t *);
                        sema_reflect_arg_t rarg;
                        memset(&rarg, 0, sizeof rarg);
                        rarg.kind = sema_reflect_arg_kind(T, rm->field);
                        if (rarg.kind == SEMA_RARG_INDEX) {
                            if (!rc->args || rc->args->next)
                                return ctfe_err(
                                    ctx, "ctfe: reflection expects 1 index");
                            value_t *iv = ctfe_eval(ctx, rc->args);
                            if (value_is_error(vm, iv)) return iv;
                            const type_t *it = value_type(iv);
                            if (!it || type_kind_of(it) != TYPE_KIND_INT || !value_data(iv))
                                return ctfe_err(
                                    ctx, "ctfe: reflection index must be an integer");
                            int64_t s = 0;
                            switch (it->size) {
                            case 4: s = *(const int32_t *)value_data(iv); break;
                            case 8: s = *(const int64_t *)value_data(iv); break;
                            default:
                                return ctfe_err(
                                    ctx, "ctfe: unsupported reflection index width");
                            }
                            if (s < 0)
                                return ctfe_err(ctx,
                                                "ctfe: reflection index must be "
                                                "non-negative");
                            rarg.index = (uint64_t)s;
                        }
                        sema_rv_t rv;
                        char rerr[512] = {0};
                        if (!sema_reflect_eval(ctx->sema, T, rm->field, &rarg,
                                               &rv, rerr, sizeof rerr))
                            return ctfe_err(ctx, rerr);
                        value_t *out = sema_reflect_value(vm, &rv);
                        return out ? out
                                   : ctfe_err(ctx,
                                              "ctfe: reflection produced no value");
                    }
                }
            }
        }
        ast_call_t *n = (ast_call_t *)node;
        if (n->callee && n->callee->kind == AST_IDENT) {
            ast_ident_t *id = (ast_ident_t *)n->callee;
            /* 1. vm scope 已有函数值（内置 printf / 已注册函数）→ value_call。
               但**只有带 cfunc 的内建函数**能直接调用：程序函数的引用对象
               （func_new_program_ref，cfunc=NULL）走 value_call 会在
               func_vcall 里报 "func call: invalid function"——它必须回落到
               下方 sema 符号表按 AST 解释执行（这正是"常量槽位里的普通函数
               调用"路径）。 */
            value_t *fnv = scope_lookup(vm->current_scope, id->name);
            if (fnv && !value_is_shadow(fnv) && value_type(fnv) &&
                value_type(fnv)->vtable && value_type(fnv)->vtable->call) {
                func_t *bf = *(func_t **)value_data(fnv);
                if (bf && bf->cfunc) return ctfe_call_value(ctx, fnv, n->args);
            }
            /* 2. sema 符号表：SYM_FUNC（clux 函数）→ 解释调用。
               先查当前词法作用域（局部 comptime 函数/变量的符号所在块，
               沿 parent 链可上溯到 global_scope），再回退 global_scope
               （顶层 comptime var / 数组边界等无 sema_scope 的求值场景）。 */
            if (ctx->sema && ctx->sema->global_scope) {
                sema_symbol_t *sym =
                    ctx->sema_scope
                        ? sema_lookup(ctx->sema_scope, id->name)
                        : NULL;
                if (!sym)
                    sym = sema_lookup(ctx->sema->global_scope, id->name);
                if (sym && sym->kind == SEMA_SYM_FUNC) {
                    /* 防御：pass1b_types（type/enum 定义求值）先于
                       pass2_types（函数签名解析 + sym->ast 绑定）——此阶段
                       函数 AST 未绑定，解释调用会解引用 NULL 崩溃。函数
                       调用非编译期常量（本阶段不可求值）→ 干净报错。 */
                    if (!sym->ast)
                        return ctfe_errf(ctx,
                            "ctfe: function '%.*s' is not a compile-time constant",
                            (int)id->name.len, id->name.ptr);
                    return ctfe_call_ast_fn(ctx, (ast_func_def_t *)sym->ast,
                                            n->args);
                }
            }
            return ctfe_errf(ctx, "ctfe: undefined function '%.*s' (not compile-time)",
                        (int)id->name.len, id->name.ptr);
        }
        /* 3. 一般 callee：求值 → value_call */
        value_t *callee = ctfe_eval(ctx, n->callee);
        if (value_is_error(vm, callee)) return callee;
        return ctfe_call_value(ctx, callee, n->args);
    }
    case AST_ARRAY: {
        /* 数组类型表达式 [N]T（类型即表达式）：递归求值元素类型 + 编译期
           边界 N，intern 出数组类型，返回 type value。base_type 可为
           AST_IDENT（类型名，走 scope_lookup 同变量机制）/ AST_CONST /
           嵌套 AST_ARRAY；length 为编译期常量表达式。 */
        ast_array_t *n = (ast_array_t *)node;
        value_t *bt = ctfe_eval(ctx, n->base_type);
        if (value_is_error(vm, bt)) return bt;
        if (!value_is_type(bt, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: array element must be a type");
        const type_t *elem = value_as(bt, const type_t *);

        value_t *lv = ctfe_eval(ctx, n->length);
        if (value_is_error(vm, lv)) return lv;
        const type_t *lt = value_type(lv);
        if (!lt || type_kind_of(lt) != TYPE_KIND_INT)
            return ctfe_err(ctx, "ctfe: array length must be an integer constant");
        uint64_t raw = 0;
        switch (lt->size) {
            case 1: raw = *(const uint8_t  *)value_data(lv); break;
            case 2: raw = *(const uint16_t *)value_data(lv); break;
            case 4: raw = *(const uint32_t *)value_data(lv); break;
            default: raw = *(const uint64_t *)value_data(lv); break;
        }
        if (raw > (uint64_t)SIZE_MAX)
            return ctfe_err(ctx, "ctfe: array length too large");
        const type_t *at = type_array_intern_own(vm, elem, (size_t)raw,
                                                 type_ownership_of(elem));
        return type_as_value(vm, at);
    }
    case AST_OWNED_TYPE: {
        /* `fatal T` / `own T` / `ref T`：所有权是类型修饰，求值为包装类型。 */
        ast_owned_type_t *ot = (ast_owned_type_t *)node;
        value_t *bv = ctfe_eval(ctx, ot->base_type);
        if (value_is_error(vm, bv)) return bv;
        if (!value_is_type(bv, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: owned type base must be a type");
        return ctfe_owned_type_value(ctx, value_as(bv, const type_t *),
                                     ctfe_token_ownership(ot->ownership));
    }
    case AST_PTR: {
        /* 指针类型表达式 *T（所有权纯编译期——类型不区分 own/ref/fatal，
           只有一种指针类型 *T）：递归求值被指向类型 T → type_ptr_intern
           → 返回 type value。与 AST_ARRAY 同构。 */
        ast_ptr_t *n = (ast_ptr_t *)node;
        value_t *bt = ctfe_eval(ctx, n->base_type);
        if (value_is_error(vm, bt)) return bt;
        if (!value_is_type(bt, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: pointer base must be a type");
        const type_t *base = value_as(bt, const type_t *);
        ownership_t oq = ctfe_token_ownership(n->ownership);
        if (oq == OWN_NONE)
            return ctfe_err(ctx, "ctfe: unsupported pointer ownership");
        const type_t *pt = type_ptr_intern(vm, base);
        if (!pt) return ctfe_err(ctx, "ctfe: failed to construct pointer type");
        return ctfe_owned_type_value(ctx, pt, oq);
    }
    case AST_SLICE_TYPE: {
        /* []T 切片类型表达式：所有权是类型修饰。 */
        ast_slice_type_t *n = (ast_slice_type_t *)node;
        value_t *ev = ctfe_eval(ctx, n->elem_type);
        if (value_is_error(vm, ev)) return ev;
        if (!value_is_type(ev, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: slice element must be a type");
        const type_t *elem = value_as(ev, const type_t *);
        ownership_t oq = ctfe_token_ownership(n->ownership);
        if (oq == OWN_NONE)
            return ctfe_err(ctx, "ctfe: unsupported slice ownership");
        const type_t *st = type_slice_intern_own(vm, elem,
                                                 type_ownership_of(elem));
        if (!st) return ctfe_err(ctx, "ctfe: failed to construct slice type");
        return ctfe_owned_type_value(ctx, st, oq);
    }
    case AST_FUNC_TYPE: {
        ast_func_type_t *n = (ast_func_type_t *)node;
        size_t np = ctfe_count_siblings(n->params);
        const type_t *params_arr[np > 0 ? np : 1];
        size_t i = 0;
        for (ast_node_t *pr = n->params; pr; pr = pr->next, i++) {
            value_t *pt = ctfe_eval(ctx, pr);
            if (value_is_error(vm, pt)) return pt;
            if (!value_is_type(pt, TYPE_KIND_TYPE))
                return ctfe_err(ctx, "ctfe: func param must be a type");
            params_arr[i] = value_as(pt, const type_t *);
        }
        const type_t *ret = NULL;
        if (n->return_type) {
            value_t *rt = ctfe_eval(ctx, n->return_type);
            if (value_is_error(vm, rt)) return rt;
            if (!value_is_type(rt, TYPE_KIND_TYPE))
                return ctfe_err(ctx, "ctfe: func return type must be a type");
            ret = value_as(rt, const type_t *);
        }
        /* `cfunc(...)` = C 函数指针类型（无 env）；与 func(...) 互不兼容。 */
        const type_t *sig =
            n->is_c_abi
                ? type_cfunc_sig(vm, np > 0 ? params_arr : NULL, np, ret,
                                 /*is_variadic=*/false)
                : type_func_sig(vm, np > 0 ? params_arr : NULL, np, ret,
                                /*is_variadic=*/false);
        if (!sig)
            return ctfe_err(ctx, "ctfe: failed to construct function signature type");
        return type_as_value(vm, sig);
    }
    case AST_TYPE_REF: {
        /* 具名类型引用（sema 登记的 "__type_N"）：查 types 队列拿类型单例，           返回 type value。槽位已被 sema 替换为 AST_TYPE_REF（类型构造收敛
           到 hoist 提升区），ctfe 重复求值时命中此分支。eval 场景
           （ctx->sema == NULL）无 sema 阶段，不可能出现此节点。 */
        ast_type_ref_t *n = (ast_type_ref_t *)node;
        if (!ctx->sema)
            return ctfe_err(ctx, "ctfe: type reference requires sema context");
        const sema_type_t *st = sema_type_find_name(ctx->sema, n->name);
        /* 内建类型引用（名字 = 规范名 "i32"）不登记 sema->types → 按内建
           id 直接加载（与 compiler AST_TYPE_REF 分支同构的 type_lookup 兜底） */
        if (!st) {
            const type_t *bt = type_lookup(vm, n->name);
            if (!bt) return ctfe_err(ctx, "ctfe: unknown type reference");
            return type_as_value(vm, bt);
        }
        return type_as_value(vm, st->type);
    }
    case AST_CONSTRUCT: {
        /* 类型字面量构造 .<type>{ fields }：求值类型位为 type value，
           逐个求值字段值（fill 值包 <v,N> 展开为 N 份 v），按数组语义构造
           连续块（value_make_array 内含元素类型隐式转换与深拷贝）。
           当前仅支持数组类型（optional 构造器走 op_construct，CTFE 不涉及）。 */
        ast_construct_t *n = (ast_construct_t *)node;
        value_t *ty = ctfe_eval(ctx, n->type);
        if (value_is_error(vm, ty)) return ty;
        if (!value_is_type(ty, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: construct type must be a type");
        const type_t *t = value_as(ty, const type_t *);
        if (type_kind_of(t) == TYPE_KIND_STRUCT || type_kind_of(t) == TYPE_KIND_TUPLE) {
            /* struct / tuple 构造（comptime var 支持所有类型）：字段按类型表序
               求值 → 编译期构造聚合值。具名字段链已由 sema 重排为表序
               （sema_expr 的 struct 分支"值节点直链"），此处按链序位置化即可。 */
            size_t nf = ctfe_count_siblings(n->fields);
            size_t expect = (type_kind_of(t) == TYPE_KIND_STRUCT)
                                ? struct_type_field_count(t)
                                : tuple_type_elem_count(t);
            if (nf != expect)
                return ctfe_errf(ctx, "ctfe: construct: expected %zu fields, got %zu",
                                 expect, nf);
            value_t *fields[nf > 0 ? nf : 1];
            size_t i = 0;
            for (ast_node_t *f = n->fields; f; f = f->next, i++) {
                fields[i] = ctfe_eval(ctx, f);
                if (value_is_error(vm, fields[i])) return fields[i];
            }
            value_t *agg = ctfe_make_aggregate(vm, t, fields, nf);
            if (!agg) return ctfe_err(ctx, "ctfe: out of memory constructing aggregate");
            return agg;
        }
        /* 基本类型构造 `.i32{123}`（2026-10）：单值 + 隐式（失败则显式）转换。 */
        if (type_kind_of(t) == TYPE_KIND_INT || type_kind_of(t) == TYPE_KIND_FLOAT ||
            type_kind_of(t) == TYPE_KIND_BOOL || type_kind_of(t) == TYPE_KIND_STR) {
            if (!n->fields || n->fields->next)
                return ctfe_err(ctx, "ctfe: basic type construct needs exactly one value");
            value_t *fv = ctfe_eval(ctx, n->fields);
            if (value_is_error(vm, fv)) return fv;
            if (value_type(fv) == t) return value_clone(vm, fv);
            value_t *casted = value_implicit_cast(vm, fv, t);
            if (value_is_error(vm, casted))
                casted = value_explicit_cast(vm, fv, t);
            return casted;
        }
        if (type_kind_of(t) != TYPE_KIND_ARRAY)
            return ctfe_err(ctx, "ctfe: construct only supports basic/array/struct/tuple types");
        const type_t *et = array_type_elem(t);
        size_t len = array_type_len(t);

        /* 第一遍：统计展开后总元素数（fill count 已由 sema 折叠为
           AST_INT_LIT 立即数），超出声明长度报错 */
        size_t total = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
            if (f->kind == AST_FILL) {
                ast_fill_t *fl = (ast_fill_t *)f;
                total += (size_t)((ast_int_lit_t *)fl->count)->value;
            } else {
                total += 1;
            }
        }
        if (len != SIZE_MAX && total > len)
            return ctfe_errf(ctx, "ctfe: construct: expected %zu elements for "
                                  "[..]T, got %zu", len, total);

        value_t *elems[total > 0 ? total : 1];
        size_t i = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
            if (f->kind == AST_FILL) {
                /* 值包展开：v 求值一次，count 份同类型值 */
                ast_fill_t *fl = (ast_fill_t *)f;
                value_t *fv = ctfe_eval(ctx, fl->value);
                if (value_is_error(vm, fv)) return fv;
                size_t cnt = (size_t)((ast_int_lit_t *)fl->count)->value;
                for (size_t k = 0; k < cnt; k++) {
                    value_t *copy = value_clone(vm, fv);
                    if (value_is_error(vm, copy)) return copy;
                    elems[i++] = copy;
                }
            } else {
                elems[i] = ctfe_eval(ctx, f);
                if (value_is_error(vm, elems[i])) return elems[i];
                i++;
            }
        }
        return value_make_array(vm, et, elems, total);
    }
    case AST_UNDEF:
        return ctfe_err(ctx, "ctfe: 'undefined' is not an expression");
    case AST_NIL:
        /* nil 不是 value：CTFE 表达式位置不合法（sema 已拦截，防御性报错） */
        return ctfe_err(ctx, "ctfe: 'nil' is not an expression");
    case AST_INDEX: {
        /* 右值下标 a[i]：object → index → value_get_index（与运行期
           INDEX_GET 语义一致）。indices 由 sema 保证单索引。 */
        ast_index_t *n = (ast_index_t *)node;
        value_t *self = ctfe_eval(ctx, n->object);
        if (value_is_error(vm, self)) return self;
        value_t *index = ctfe_eval(ctx, n->indices);
        if (value_is_error(vm, index)) return index;
        return value_get_index(vm, self, index);
    }
    case AST_TERNARY: {
        /* 三元条件表达式：求值 cond → bool，惰性只求值选中分支（与
           &&/|| 短路一致，未选中分支不求值）。分支值归 scope 管理
           （value_make 自动 track），直接返回引用即可。 */
        ast_ternary_t *n = (ast_ternary_t *)node;
        value_t *cond = ctfe_eval(ctx, n->cond);
        if (value_is_error(vm, cond)) return cond;
        if (value_type(cond) != vm->type_bool)
            return ctfe_err(ctx, "ctfe: ternary condition must be bool");
        bool c = ctfe_read_bool(vm, cond);
        return c ? ctfe_eval(ctx, n->then_branch)
                 : ctfe_eval(ctx, n->else_branch);
    }
    case AST_FUNC_REF: {
        /* 函数引用节点（sema 折叠产物：AST_IDENT 确认函数符号时改写，或
           comptime 折叠 AST_FUNC_REF）：纯 fid 标识——compiler 运行期
           LOAD_FUNCTION <fid> 加载真实函数值，CTFE 此处按 fid 构造引用
           func_t 包装 func value（与函数是否有 name 无关：匿名字面量
           无名字也可折叠）。
           程序函数（全局/局部）AST 托管在 sema->funcs：按 fid 查 →
           sig_id 反查签名类型 → 构造引用对象。内建函数 / CTFE 已构造的
           字面量引用（注册进 vm->functions）按 fid 直接复用现有对象。 */
        ast_func_ref_t *n = (ast_func_ref_t *)node;
        if (!ctx->sema || !ctx->sema->global_scope)
            return ctfe_err(ctx, "ctfe: function reference requires sema context");
        sema_func_t *sf = sema_func_by_id(ctx->sema, n->fid);
        if (sf) {
            ast_func_def_t *fd = (ast_func_def_t *)sf->def;
            if (fd->is_comptime) {
                return ctfe_errf(ctx,
                            "ctfe: comptime function '%.*s' cannot be used as a value",
                            (int)fd->name.len, fd->name.ptr);
            }
            const sema_type_t *st = sema_type_by_id(ctx->sema, fd->sig_id);
            if (!st || !st->type || type_kind_of(st->type) != TYPE_KIND_FUNC) {
                return ctfe_errf(ctx, "ctfe: function id %u missing signature",
                                 n->fid);
            }
            func_t *fn = func_new_program_ref(vm, st->type, fd->name, n->fid);
            if (!fn)
                return ctfe_err(ctx, "ctfe: out of memory creating function reference");
            void *data = value_alloc_data_copy(vm->alloc, st->type, &fn);
            return value_make(vm, st->type, data);
        }
        /* 内建函数 / CTFE 字面量引用：vm->functions 按 id 复用现有对象 */
        func_t *fn = ctfe_func_by_id(vm, n->fid);
        if (!fn || !fn->type || type_kind_of(fn->type) != TYPE_KIND_FUNC) {
            return ctfe_errf(ctx, "ctfe: unknown function id %u", n->fid);
        }
        void *data = value_alloc_data_copy(vm->alloc, fn->type, &fn);
        return value_make(vm, fn->type, data);
    }
    case AST_FUNC_DEF: {
        /* 函数字面量（comptime body 内表达式/语句级）：求值签名 + 构造
           func value（ctfe_eval_func_def：登记签名 + 分配 fid + 绑定名字）。
           函数体不在此解释——运行期由编译产物执行（折叠产物 LOAD_FUNCTION
           <fid> 加载真实函数对象）。 */
        return ctfe_eval_func_def(ctx, (ast_func_def_t *)node);
    }
    case AST_SCOPE_REF: {
        /* 枚举 variant 引用 Color::Red（sema 折叠产物）：type_expr 已折叠
           为 AST_TYPE_REF，value 字段已填底层整数值——查 enum 类型 →
           构造 enum 类型 value（data = 底层宽度的整数值块，MAKE_ENUM
           运行期同构）。全局 enum 变量 init / comptime 折叠消费。 */
        ast_scope_ref_t *n = (ast_scope_ref_t *)node;
        value_t *ty = ctfe_eval(ctx, n->type_expr);
        if (value_is_error(vm, ty)) return ty;
        if (!value_is_type(ty, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: enum reference type must be a type");
        const type_t *t = value_as(ty, const type_t *);
        if (type_kind_of(t) != TYPE_KIND_ENUM)
            return ctfe_err(ctx, "ctfe: enum reference type is not an enum");
        const type_t *u = enum_type_underlying(t);
        if (!u) return ctfe_err(ctx, "ctfe: enum type missing underlying");
        /* 按底层宽度截断存储（与 MAKE_ENUM / enum_store_value 同语义） */
        void *data = value_alloc_data(vm->alloc, t);
        switch (u->size) {
        case 1: *(int8_t  *)data = (int8_t)n->value; break;
        case 2: *(int16_t *)data = (int16_t)n->value; break;
        case 4: *(int32_t *)data = (int32_t)n->value; break;
        default: *(int64_t *)data = n->value; break;
        }
        return value_make(vm, t, data);
    }
    default:
        return ctfe_err(ctx, "ctfe: unsupported expression node");
    }
}
