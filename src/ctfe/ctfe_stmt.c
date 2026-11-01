#include "ctfe/ctfe.h"

#include <stdio.h>
#include "sema/sema.h"

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
#include "parser/ast_switch.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_static_assert.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
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

    /* 复合值左值（成员 / 解引用 / 解包）：按类型布局寻址后读写。
       `a[i] op= v` 同一条路径（数组 / 元组 / 切片 / 指针下标）。 */
    if (n->target->kind == AST_MEMBER || n->target->kind == AST_DEREF ||
        n->target->kind == AST_UNWRAP)
        return ctfe_assign_lvalue(ctx, n);
    if (n->target->kind == AST_INDEX) return ctfe_index_assign(ctx, n);

    if (n->target->kind != AST_IDENT)
        return ctfe_err(ctx, "ctfe: invalid assignment target");
    /* 丢弃标记 `_ = expr`：求值后丢弃（不是变量赋值）。 */
    if (((ast_ident_t *)n->target)->name.len == 1 &&
        ((ast_ident_t *)n->target)->name.ptr[0] == '_') {
        value_t *v = ctfe_eval(ctx, n->value);
        return value_is_error(vm, v) ? v : value_make_undefined(vm);
    }
    value_t *dst = scope_lookup(vm->current_scope,
                                ((ast_ident_t *)n->target)->name);
    if (!dst) return ctfe_err(ctx, "ctfe: assignment to undefined variable");
    /* 右值按目标类型求值：`o = nil` → none；T → ?T 提升；隐式转换。 */
    value_t *src = ctfe_eval_as(ctx, n->value, value_type(dst));
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

static value_t *ctfe_eval_stmt_core(ctfe_ctx_t *ctx, ast_node_t *stmt);

/* 语句入口：先 prelude、再语句主体（`sugar_noop` 跳过）、最后 postlude。
 * sema 把 `.?' / `.!' 之类的错误处理语法糖展开成挂在语句上的
 * prelude/postlude（临时变量 + if 分支）——与 C 后端 lower_stmt 同序消费。
 * 控制流（try 失败路径的 return）一旦发生：立即停止，不再执行主体/后置。 */
value_t *ctfe_eval_stmt(ctfe_ctx_t *ctx, ast_node_t *stmt) {
    vm_t *vm = ctx->vm;
    if (!ctx || !vm) return NULL;
    if (!stmt) return value_make_undefined(vm);

    if (stmt->prelude) {
        for (ast_node_t *p = stmt->prelude; p; p = p->next) {
            value_t *r = ctfe_eval_stmt(ctx, p);
            if (value_is_error(vm, r)) return r;
            if (ctx->ctrl != CTFE_CTRL_NONE) return value_make_undefined(vm);
        }
    }
    bool noop = stmt->kind == AST_ASSIGN && ((ast_assign_t *)stmt)->sugar_noop;
    if (!noop) {
        value_t *r = ctfe_eval_stmt_core(ctx, stmt);
        if (value_is_error(vm, r)) return r;
        if (ctx->ctrl != CTFE_CTRL_NONE) return value_make_undefined(vm);
    }
    if (stmt->postlude) {
        for (ast_node_t *p = stmt->postlude; p; p = p->next) {
            value_t *r = ctfe_eval_stmt(ctx, p);
            if (value_is_error(vm, r)) return r;
            if (ctx->ctrl != CTFE_CTRL_NONE) return value_make_undefined(vm);
        }
    }
    return value_make_undefined(vm);
}

static value_t *ctfe_eval_stmt_core(ctfe_ctx_t *ctx, ast_node_t *stmt) {
    vm_t *vm = ctx->vm;
    if (ctx->budget == 0)
        return ctfe_err(ctx, "ctfe: evaluation budget exceeded (possible loop)");
    ctx->budget--;

    switch (stmt->kind) {
    case AST_BLOCK: {
        ast_block_t *b = (ast_block_t *)stmt;
        vm_push_scope(vm);
        for (ast_node_t *s = b->stmts; s; s = s->next) {
            value_t *r = ctfe_eval_stmt(ctx, s);
            /* 控制流优先于错误：`.?' 的失败路径在表达式内部执行了 `return`
               （tail_stmt），其后同一语句里的表达式求值可能产生哨兵错误——
               此时以 ctrl 为准，错误是"已跳转"的副产物。 */
            if (ctx->ctrl != CTFE_CTRL_NONE) {
                /* 退出点节点上挂载的 defer calls（desugar_defers 设到
                   defers 字段）仍需执行——先跑 defer 再退出块。
                   保存/恢复 ctrl + ret_value：defer call 是无参 void 函数，
                   ctfe_call_ast_fn 会重置 ctrl/ret_value，不能让 defer
                   覆盖外层 return/break/continue 的控制流状态。 */
                ctfe_ctrl_t saved_ctrl = ctx->ctrl;
                value_t *saved_ret = ctx->ret_value;
                for (ast_node_t *d = s->defers; d; d = d->next) {
                    ctx->ctrl = CTFE_CTRL_NONE;
                    ctx->ret_value = NULL;
                    value_t *dr = ctfe_eval_stmt(ctx, d);
                    if (value_is_error(vm, dr)) {
                        dr = ctfe_hoist_error(ctx, dr);
                        vm_pop_scope(vm);
                        return dr;
                    }
                }
                ctx->ctrl = saved_ctrl;
                ctx->ret_value = saved_ret;
                break;
            }
            if (value_is_error(vm, r)) {
                r = ctfe_hoist_error(ctx, r);
                vm_pop_scope(vm);
                return r;
            }
        }
        /* 块 pop 前：RETURN 值上移，防止 pop 销毁（嵌套块逐层上移） */
        ctfe_hoist_return(ctx);
        vm_pop_scope(vm);
        return value_make_undefined(vm);
    }
    case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)stmt;
        /* 声明类型（带类型上下文求值：`var o: ?i32 = nil`）。解析失败
           （泛型参数等）时退回无上下文求值。 */
        const type_t *dt = NULL;
        if (vd->type_expr) {
            value_t *tv = ctfe_eval(ctx, vd->type_expr);
            if (!value_is_error(vm, tv) && value_is_type(tv, TYPE_KIND_TYPE))
                dt = value_as(tv, const type_t *);
        }
        value_t *init = NULL;
        if (vd->zero_init) {
            /* 编译器生成的零初始化声明（`__init__` 构造的临时对象）。 */
            if (!dt) return ctfe_err(ctx, "ctfe: zero-init variable has no type");
            void *data = value_alloc_data(vm->alloc, dt);
            init = value_make(vm, dt, data);
        } else if (vd->init && vd->init->kind == AST_UNDEF) {
            /* 声明占位：分配声明类型零值。 */
            if (!dt) return ctfe_err(ctx, "ctfe: undefined variable type");
            void *data = value_alloc_data(vm->alloc, dt);
            init = value_make(vm, dt, data);
        } else {
            init = ctfe_eval_as(ctx, vd->init, dt);
            if (value_is_error(vm, init)) return init;
        }
        char nb[128];
        const char *name = ctfe_slice_to_cstr(vd->name, nb, sizeof nb);
        value_t *stored = ctfe_define_value(ctx, vm->current_scope, name, init);
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
    case AST_DOWHILE: {
        /* do-while：先执行体，再判条件（体至少执行一次） */
        ast_dowhile_t *n = (ast_dowhile_t *)stmt;
        for (;;) {
            if (ctx->budget == 0)
                return ctfe_err(ctx, "ctfe: evaluation budget exceeded (loop)");
            value_t *r = ctfe_eval_stmt(ctx, n->body);
            if (value_is_error(vm, r)) return r;
            if (ctx->ctrl == CTFE_CTRL_BREAK) {
                ctx->ctrl = CTFE_CTRL_NONE;
                break;
            }
            if (ctx->ctrl == CTFE_CTRL_CONTINUE) ctx->ctrl = CTFE_CTRL_NONE;
            if (ctx->ctrl == CTFE_CTRL_RETURN) break;
            value_t *cv = ctfe_eval(ctx, n->cond);
            if (value_is_error(vm, cv)) return cv;
            if (value_type(cv) != vm->type_bool)
                return ctfe_err(ctx, "ctfe: do-while condition must be bool");
            if (!ctfe_read_bool(vm, cv)) break;
        }
        return value_make_undefined(vm);
    }
    case AST_SWITCH: {
        /* switch：cond 求值一次 → 逐模式 value_eq 短接（模式间 = ||
           链，惰性）→ 命中分支体执行 → 未命中走 default。无 fallthrough。 */
        ast_switch_t *n = (ast_switch_t *)stmt;
        value_t *cv = ctfe_eval(ctx, n->cond);
        if (value_is_error(vm, cv)) return cv;
        bool matched = false;
        for (ast_node_t *cs = n->cases; cs && !matched; cs = cs->next) {
            ast_switch_case_t *sc = (ast_switch_case_t *)cs;
            for (ast_node_t *pt = sc->patterns; pt && !matched; pt = pt->next) {
                value_t *pv = ctfe_eval(ctx, pt);
                if (value_is_error(vm, pv)) return pv;
                value_t *eq = value_eq(vm, cv, pv);
                if (value_is_error(vm, eq)) return eq;
                if (ctfe_read_bool(vm, eq)) {
                    matched = true;
                    value_t *r = ctfe_eval_stmt(ctx, sc->body);
                    if (value_is_error(vm, r)) return r;
                }
            }
        }
        if (!matched && n->default_body) {
            value_t *r = ctfe_eval_stmt(ctx, n->default_body);
            if (value_is_error(vm, r)) return r;
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
        /* return 值按**声明返回类型**求值：`return nil` / T → ?T 等需要类型
           上下文（ctx->ret_type 由 ctfe_call_ast_fn_vals 设置）。 */
        ctx->ret_value = n->value ? ctfe_eval_as(ctx, n->value, ctx->ret_type)
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
    case AST_STATIC_ASSERT: {
        ast_static_assert_t *sa = (ast_static_assert_t *)stmt;
        value_t *c = ctfe_eval(ctx, sa->cond);
        if (value_is_error(vm, c)) return c;
        if (value_type(c) != vm->type_bool)
            return value_make_error(vm, "static_assert condition must be bool");
        if (!*(const bool *)value_data(c)) {
            char buf[256];
            const char *msg = NULL;
            if (sa->msg && sa->msg->kind == AST_STRING_LIT) {
                strslice_t s = ((ast_string_lit_t *)sa->msg)->text;
                if (s.ptr && s.len > 0) { msg = s.ptr; }
                if (msg) {
                    snprintf(buf, sizeof buf, "static_assert failed: \"%.*s\"",
                             (int)s.len, s.ptr);
                    return value_make_error(vm, buf);
                }
            }
            return value_make_error(vm, "static_assert failed");
        }
        return value_make_undefined(vm);
    }
    case AST_EMPTY_STMT:
        return value_make_undefined(vm); /* ; 空语句：无操作 */
    /* AST_DEFER 已在 sema Pass 3a desugar_defers 中改写为 AST_FUNC_DEF +
       退出点 call（AST_EXPR_STMT），到达 CTFE 时已不存在 AST_DEFER。
       defer 体内的函数定义和 call 语句走正常的 AST_FUNC_DEF / AST_EXPR_STMT
       路径，CTFE 自然执行，不再需要拒绝。 */
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
