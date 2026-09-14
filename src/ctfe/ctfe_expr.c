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
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/lexer.h"
#include "sema/symbol.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_error.h"
#include "vm/value.h"

/* ===========================================================================
 * 表达式求值
 *
 * 每个表达式产生真实 value（track 到 vm->current_scope）。失败返回 error
 * value。短路 &&/|| 惰性求值（不走 vtable 二元分派，与运行期解释器一致）。
 * =========================================================================== */

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
            /* 2. sema 符号表：AST_FUNC_DEF（clux 函数）→ 解释调用 */
            if (ctx->sema && ctx->sema->global_scope) {
                sema_symbol_t *sym =
                    sema_lookup(ctx->sema->global_scope, id->name);
                if (sym && sym->ast && sym->ast->kind == AST_FUNC_DEF) {
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
    case AST_TYPE_REF: {
        /* 具名类型引用（sema 登记的 "__type_N"）：查 types 队列拿类型单例，
           返回 type value。槽位已被 sema 替换为 AST_TYPE_REF（类型构造收敛
           到 hoist 提升区），ctfe 重复求值时命中此分支。eval 场景
           （ctx->sema == NULL）无 sema 阶段，不可能出现此节点。 */
        ast_type_ref_t *n = (ast_type_ref_t *)node;
        if (!ctx->sema)
            return ctfe_err(ctx, "ctfe: type reference requires sema context");
        const sema_type_t *st = sema_type_find_name(ctx->sema, n->name);
        if (!st) return ctfe_err(ctx, "ctfe: unknown type reference");
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
    default:
        return ctfe_err(ctx, "ctfe: unsupported expression node");
    }
}
