#include "ctfe/ctfe.h"

#include "core/string.h"
#include "core/strslice.h"
#include "parser/ast_assign.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_block.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_for.h"
#include "parser/ast_if.h"
#include "parser/ast_return.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"
#include "vm/type_error.h"
#include "vm/value.h"

/* ===========================================================================
 * 语句解释
 *
 * 逐条执行块内语句。遇 AST_RETURN / AST_BREAK / AST_CONTINUE 设置 ctx->ctrl
 * 并立即返回（块解释循环据此停止）。表达式节点作为语句传入时求值并丢弃
 * 结果。作用域 pop 前先上移 RETURN/error value（ctfe_hoist_return /
 * ctfe_hoist_error），防止 pop 销毁。
 * =========================================================================== */

static value_t *ctfe_assign(ctfe_ctx_t *ctx, ast_assign_t *n) {
    vm_t *vm = ctx->vm;

    /* 下标左值赋值：a[i] = v / a[i] op= v。与运行期语义一致（求值序
       object → index → value；= → value_set_index；复合赋值 →
       value_get_index 取旧值 → 运算 → value_set_index 写回）。indices
       由 sema 保证单索引（与运行期 compile_assign_index 同约定）。 */
    if (n->target->kind == AST_INDEX) {
        ast_index_t *ix = (ast_index_t *)n->target;
        value_t *self = ctfe_eval(ctx, ix->object);
        if (value_is_error(vm, self)) return self;
        value_t *index = ctfe_eval(ctx, ix->indices);
        if (value_is_error(vm, index)) return index;
        value_t *src = ctfe_eval(ctx, n->value);
        if (value_is_error(vm, src)) return src;
        if (token_is(n->op, "="))
            return value_set_index(vm, self, index, src);
        value_t *old = value_get_index(vm, self, index);
        if (value_is_error(vm, old)) return old;
        value_t *r = NULL;
        if (token_is(n->op, "+="))      r = value_add(vm, old, src);
        else if (token_is(n->op, "-=")) r = value_sub(vm, old, src);
        else if (token_is(n->op, "*=")) r = value_mul(vm, old, src);
        else if (token_is(n->op, "/=")) r = value_div(vm, old, src);
        else if (token_is(n->op, "%=")) r = value_mod(vm, old, src);
        else return ctfe_err(ctx, "ctfe: unsupported assignment operator");
        if (value_is_error(vm, r)) return r;
        return value_set_index(vm, self, index, r);
    }

    if (n->target->kind != AST_IDENT)
        return ctfe_err(ctx, "ctfe: invalid assignment target");
    value_t *dst = scope_lookup(vm->current_scope,
                                ((ast_ident_t *)n->target)->name);
    if (!dst) return ctfe_err(ctx, "ctfe: assignment to undefined variable");
    value_t *src = ctfe_eval(ctx, n->value);
    if (value_is_error(vm, src)) return src;
    if (token_is(n->op, "=")) return value_assign(vm, dst, src);
    /* 复合赋值：+= -= *= /= %= */
    value_t *r = NULL;
    if (token_is(n->op, "+="))      r = value_add(vm, dst, src);
    else if (token_is(n->op, "-=")) r = value_sub(vm, dst, src);
    else if (token_is(n->op, "*=")) r = value_mul(vm, dst, src);
    else if (token_is(n->op, "/=")) r = value_div(vm, dst, src);
    else if (token_is(n->op, "%=")) r = value_mod(vm, dst, src);
    else return ctfe_err(ctx, "ctfe: unsupported assignment operator");
    if (value_is_error(vm, r)) return r;
    return value_assign(vm, dst, r);
}

value_t *ctfe_eval_stmt(ctfe_ctx_t *ctx, ast_node_t *stmt) {
    vm_t *vm = ctx->vm;
    if (!ctx || !vm) return NULL;
    if (!stmt) return value_make_undefined(vm);
    if (ctx->budget == 0)
        return ctfe_err(ctx, "ctfe: evaluation budget exceeded (possible loop)");
    ctx->budget--;

    switch (stmt->kind) {
    case AST_BLOCK: {
        ast_block_t *b = (ast_block_t *)stmt;
        vm_push_scope(vm);
        for (ast_node_t *s = b->stmts; s; s = s->next) {
            value_t *r = ctfe_eval_stmt(ctx, s);
            if (value_is_error(vm, r)) {
                r = ctfe_hoist_error(ctx, r);
                vm_pop_scope(vm);
                return r;
            }
            if (ctx->ctrl != CTFE_CTRL_NONE) break;
        }
        /* 块 pop 前：RETURN 值上移，防止 pop 销毁（嵌套块逐层上移） */
        ctfe_hoist_return(ctx);
        vm_pop_scope(vm);
        return value_make_undefined(vm);
    }
    case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)stmt;
        value_t *init = NULL;
        if (vd->init && vd->init->kind == AST_UNDEF) {
            /* 声明占位：分配声明类型零值。类型表达式按普通表达式 ctfe
               求值 → type value（data=type_t*），遮蔽感知与变量同机制。 */
            value_t *tv = ctfe_eval(ctx, vd->type_expr);
            if (value_is_error(vm, tv)) return tv;
            if (!value_is_type(tv, TYPE_KIND_TYPE))
                return ctfe_err(ctx, "ctfe: undefined variable type");
            const type_t *t = value_as(tv, const type_t *);
            void *data = value_alloc_data(vm->alloc, t);
            init = value_make(vm, t, data);
        } else {
            init = ctfe_eval(ctx, vd->init);
            if (value_is_error(vm, init)) return init;
        }
        char nb[128];
        const char *name = ctfe_slice_to_cstr(vd->name, nb, sizeof nb);
        value_t *stored = scope_define(vm, vm->current_scope, name, init);
        if (value_is_error(vm, stored)) return stored;
        return value_make_undefined(vm);
    }
    case AST_ASSIGN:
        return ctfe_assign(ctx, (ast_assign_t *)stmt);
    case AST_IF: {
        ast_if_t *n = (ast_if_t *)stmt;
        value_t *cv = ctfe_eval(ctx, n->cond);
        if (value_is_error(vm, cv)) return cv;
        if (value_type(cv) != vm->type_bool)
            return ctfe_err(ctx, "ctfe: if condition must be bool");
        if (ctfe_read_bool(vm, cv)) {
            value_t *r = ctfe_eval_stmt(ctx, n->then_body);
            if (value_is_error(vm, r)) return r;
        } else if (n->else_body) {
            value_t *r = ctfe_eval_stmt(ctx, n->else_body);
            if (value_is_error(vm, r)) return r;
        }
        return value_make_undefined(vm);
    }
    case AST_WHILE: {
        ast_while_t *n = (ast_while_t *)stmt;
        for (;;) {
            if (ctx->budget == 0)
                return ctfe_err(ctx, "ctfe: evaluation budget exceeded (loop)");
            value_t *cv = ctfe_eval(ctx, n->cond);
            if (value_is_error(vm, cv)) return cv;
            if (value_type(cv) != vm->type_bool)
                return ctfe_err(ctx, "ctfe: while condition must be bool");
            if (!ctfe_read_bool(vm, cv)) break;
            value_t *r = ctfe_eval_stmt(ctx, n->body);
            if (value_is_error(vm, r)) return r;
            if (ctx->ctrl == CTFE_CTRL_BREAK) {
                ctx->ctrl = CTFE_CTRL_NONE;
                break;
            }
            if (ctx->ctrl == CTFE_CTRL_CONTINUE) ctx->ctrl = CTFE_CTRL_NONE;
            if (ctx->ctrl == CTFE_CTRL_RETURN) break;
        }
        return value_make_undefined(vm);
    }
    case AST_FOR: {
        ast_for_t *n = (ast_for_t *)stmt;
        vm_push_scope(vm); /* for 头变量（init 定义）与 body 同作用域 */
        if (n->init) {
            value_t *r = ctfe_eval_stmt(ctx, n->init);
            if (value_is_error(vm, r)) {
                r = ctfe_hoist_error(ctx, r);
                vm_pop_scope(vm);
                return r;
            }
        }
        for (;;) {
            if (ctx->budget == 0) {
                vm_pop_scope(vm);
                return ctfe_err(ctx, "ctfe: evaluation budget exceeded (loop)");
            }
            if (n->cond) {
                value_t *cv = ctfe_eval(ctx, n->cond);
                if (value_is_error(vm, cv)) {
                    cv = ctfe_hoist_error(ctx, cv);
                    vm_pop_scope(vm);
                    return cv;
                }
                if (value_type(cv) != vm->type_bool) {
                    vm_pop_scope(vm);
                    return ctfe_err(ctx, "ctfe: for condition must be bool");
                }
                if (!ctfe_read_bool(vm, cv)) break;
            }
            value_t *r = ctfe_eval_stmt(ctx, n->body);
            if (value_is_error(vm, r)) {
                r = ctfe_hoist_error(ctx, r);
                vm_pop_scope(vm);
                return r;
            }
            if (ctx->ctrl == CTFE_CTRL_BREAK) {
                ctx->ctrl = CTFE_CTRL_NONE;
                break;
            }
            if (ctx->ctrl == CTFE_CTRL_CONTINUE) ctx->ctrl = CTFE_CTRL_NONE;
            if (ctx->ctrl == CTFE_CTRL_RETURN) break;
            if (n->update) {
                value_t *u = ctfe_eval_stmt(ctx, n->update);
                if (value_is_error(vm, u)) {
                    u = ctfe_hoist_error(ctx, u);
                    vm_pop_scope(vm);
                    return u;
                }
            }
        }
        /* for 作用域 pop 前：RETURN 值上移，防止 pop 销毁 */
        ctfe_hoist_return(ctx);
        vm_pop_scope(vm);
        return value_make_undefined(vm);
    }
    case AST_RETURN: {
        ast_return_t *n = (ast_return_t *)stmt;
        ctx->ret_value = n->value ? ctfe_eval(ctx, n->value)
                                  : value_make_undefined(vm);
        if (value_is_error(vm, ctx->ret_value)) return ctx->ret_value;
        ctx->ctrl = CTFE_CTRL_RETURN;
        return value_make_undefined(vm);
    }
    case AST_BREAK:
        ctx->ctrl = CTFE_CTRL_BREAK;
        return value_make_undefined(vm);
    case AST_CONTINUE:
        ctx->ctrl = CTFE_CTRL_CONTINUE;
        return value_make_undefined(vm);
    case AST_EXPR_STMT: {
        ast_expr_stmt_t *n = (ast_expr_stmt_t *)stmt;
        return ctfe_eval(ctx, n->expr);
    }
    case AST_EMPTY_STMT:
        return value_make_undefined(vm); /* ; 空语句：无操作 */
    case AST_FUNC_DEF: {
        /* comptime body 内函数定义语句（func inc(){}）：求值函数值
           （ctfe_eval_func_def 已绑定名字到当前 vm 作用域，供后续
           `return inc` 引用）并丢弃——语句无结果。 */
        value_t *fv = ctfe_eval(ctx, stmt);
        if (value_is_error(vm, fv)) return fv;
        return value_make_undefined(vm);
    }
    default:
        /* 表达式直接作为语句（for 的 update 等）→ 求值并丢弃 */
        return ctfe_eval(ctx, stmt);
    }
}
