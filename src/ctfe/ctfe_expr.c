#include "ctfe/ctfe.h"

#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_volatile.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/ast_var_def.h"
#include "parser/lexer.h"
#include "sema/symbol.h"
#include "vm/function.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_error.h"
#include "vm/type_func.h"
#include "vm/value.h"

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
                                      nparams, ret, /*is_variadic=*/false);
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
    void *data = value_alloc_data_copy(vm->alloc, sig, &fn);
    value_t *fv = value_make(vm, sig, data);

    /* 具名字面量绑定到当前 vm 作用域（comptime body 内函数定义语句 +
       后续名字引用）。scope_define clone 到作用域 owned，引用值保留。 */
    if (n->name.len > 0) {
        char nb[128];
        const char *name = ctfe_slice_to_cstr(n->name, nb, sizeof nb);
        value_t *stored = scope_define(vm, vm->current_scope, name, fv);
        if (value_is_error(vm, stored)) return stored;
    }
    return fv;
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
        string_t *str = string_from_bytes(vm->alloc, n->text.ptr, n->text.len);
        void *data = value_alloc_data_copy(vm->alloc, vm->type_str, &str);
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
                sym->type->kind == TYPE_KIND_FUNC) {
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
        const type_t *inner = value_as(sub, const type_t *);
        return type_as_value(vm, type_volatile_intern(vm, inner));
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
    case AST_CALL: {
        ast_call_t *n = (ast_call_t *)node;
        if (n->callee && n->callee->kind == AST_IDENT) {
            ast_ident_t *id = (ast_ident_t *)n->callee;
            /* 1. vm scope 已有函数值（内置 printf / 注册函数）→ value_call */
            value_t *fnv = scope_lookup(vm->current_scope, id->name);
            if (fnv && value_type(fnv) && value_type(fnv)->vtable &&
                value_type(fnv)->vtable->call) {
                return ctfe_call_value(ctx, fnv, n->args);
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
        if (!lt || lt->kind != TYPE_KIND_INT)
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
        const type_t *at = type_array_intern(vm, elem, (size_t)raw);
        return type_as_value(vm, at);
    }
    case AST_FUNC_TYPE: {
        /* 函数签名类型表达式 func(ps...)->ret（类型即表达式）：递归求值
           参数类型 + 返回类型，type_func_sig 一次性构造（内部去重 intern），
           返回 type value。 */
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
        const type_t *sig = type_func_sig(vm, np > 0 ? params_arr : NULL,
                                          np, ret, /*is_variadic=*/false);
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
           逐个求值字段值，按数组语义构造连续块（value_make_array 内含
           元素类型隐式转换与深拷贝）。当前仅支持数组类型。 */
        ast_construct_t *n = (ast_construct_t *)node;
        value_t *ty = ctfe_eval(ctx, n->type);
        if (value_is_error(vm, ty)) return ty;
        if (!value_is_type(ty, TYPE_KIND_TYPE))
            return ctfe_err(ctx, "ctfe: construct type must be a type");
        const type_t *t = value_as(ty, const type_t *);
        if (t->kind != TYPE_KIND_ARRAY)
            return ctfe_err(ctx, "ctfe: construct only supports array types");
        const type_t *et = array_type_elem(t);
        size_t len = array_type_len(t);

        size_t nf = ctfe_count_siblings(n->fields);
        if (len != SIZE_MAX && nf != len)
            return ctfe_errf(ctx, "ctfe: construct: expected %zu elements for "
                                  "[..]T, got %zu", len, nf);

        value_t *elems[nf > 0 ? nf : 1];
        size_t i = 0;
        for (ast_node_t *f = n->fields; f; f = f->next) {
            elems[i] = ctfe_eval(ctx, f);
            if (value_is_error(vm, elems[i])) return elems[i];
            i++;
        }
        return value_make_array(vm, et, elems, nf);
    }
    case AST_UNDEF:
        return ctfe_err(ctx, "ctfe: 'undefined' is not an expression");
    case AST_NIL:
        /* nil：内置类型唯一值（函数 0 初始化/未来空指针），
           CTFE 求值压入真实 nil value（data = NULL 指针）。 */
        return value_make_nil(vm);
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
            if (!st || !st->type || st->type->kind != TYPE_KIND_FUNC) {
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
        if (!fn || !fn->type || fn->type->kind != TYPE_KIND_FUNC) {
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
    default:
        return ctfe_err(ctx, "ctfe: unsupported expression node");
    }
}
