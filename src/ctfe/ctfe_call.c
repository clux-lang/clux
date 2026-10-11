#include "ctfe/ctfe.h"
#include "sema/sema.h"

#include "core/string.h"
#include "core/strslice.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"
#include "vm/value.h"

/* ===========================================================================
 * 函数调用
 *
 * ctfe_call_ast_fn：解释 AST_FUNC_DEF（clux 函数），绑定实参到临时局部
 * 作用域 → 解释函数体 → 返回 clone 到调用方 scope 的返回值。
 * ctfe_call_value：调用已求值的 func value（内置 C 函数 / 运行时注册
 * 函数），求值实参 → value_call。
 * =========================================================================== */

/* 解释 AST_FUNC_DEF：绑定实参到临时局部作用域 → 解释函数体 → 返回 clone 到
   调用方 scope 的返回值（无显式 return → void）。 */
value_t *ctfe_call_ast_fn(ctfe_ctx_t *ctx, ast_func_def_t *fn,
                          ast_node_t *args) {
    vm_t *vm = ctx->vm;
    scope_t *caller_scope = vm->current_scope;

    /* 1. 临时局部作用域（参数 + 函数体变量） */
    vm_push_scope(vm);
    scope_t *callee_scope = vm->current_scope;

    /* 2. 绑定参数：实参链与参数声明链逐对对应 */
    ast_node_t *a = args;
    for (ast_node_t *p = fn->params; p; p = p->next) {
        if (!a) {
            value_t *e = ctfe_err(ctx, "ctfe: function call: too few arguments");
            e = ctfe_hoist_error(ctx, e);
            vm_pop_scope(vm);
            return e;
        }
        value_t *av = ctfe_eval(ctx, a);
        if (value_is_error(vm, av)) {
            av = ctfe_hoist_error(ctx, av);
            vm_pop_scope(vm);
            return av;
        }
        ast_var_def_t *vd = (ast_var_def_t *)p;
        char nb[128];
        const char *name = ctfe_slice_to_cstr(vd->name, nb, sizeof nb);
        value_t *stored = scope_define(vm, vm->current_scope, name, av);
        if (value_is_error(vm, stored)) {
            stored = ctfe_hoist_error(ctx, stored);
            vm_pop_scope(vm);
            return stored;
        }
        a = a->next;
    }
    if (a) {
        value_t *e = ctfe_err(ctx, "ctfe: function call: too many arguments");
        e = ctfe_hoist_error(ctx, e);
        vm_pop_scope(vm);
        return e;
    }

    /* 3. 解释函数体（body 为 AST_BLOCK，内部自带块作用域） */
    ctx->ctrl = CTFE_CTRL_NONE;
    ctx->ret_value = NULL;
    value_t *stmt_r = ctfe_eval_stmt(ctx, fn->body);
    if (value_is_error(vm, stmt_r)) {
        stmt_r = ctfe_hoist_error(ctx, stmt_r);
        vm_pop_scope(vm);
        return stmt_r;
    }

    /* 4. 取返回值并 clone 到调用方 scope（必须在 pop 之前：ret_value 归
       callee scope，pop 会销毁） */
    value_t *ret = NULL;
    {
        vm->current_scope = caller_scope;
        if (ctx->ctrl == CTFE_CTRL_RETURN && ctx->ret_value) {
            ret = value_clone(vm, ctx->ret_value);
        } else {
            ret = value_make_undefined(vm);
        }
    }
    ctx->ctrl = CTFE_CTRL_NONE;
    ctx->ret_value = NULL;

    /* 5. pop 函数局部作用域：clone 块切走了 current_scope，恢复为 callee
       再 pop（vm_pop_scope 无参、pop 当前作用域，误 pop caller 会销毁
       调用方作用域） */
    vm->current_scope = callee_scope;
    vm_pop_scope(vm);
    vm->current_scope = caller_scope;

    return ret;
}

/* 调用已求值的 func value：
   - 内建 C 函数（printf/upgrade 等，cfunc != NULL）→ 求值实参 → value_call
   - **程序函数引用**（sema 把函数名折叠成 AST_FUNC_REF 后的产物，cfunc == NULL）
     → value_call 会在 func_vcall 里报 "func call: invalid function"，必须按
     AST 解释执行（m2-design §9 决策点 2：非 comptime 函数在常量槽位被尝试
     编译期求值）。 */
value_t *ctfe_call_value(ctfe_ctx_t *ctx, value_t *callee,
                         ast_node_t *args) {
    vm_t *vm = ctx->vm;

    if (!value_is_shadow(callee) && ctx->sema && value_type(callee)) {
        const type_t *ct = value_type(callee);
        if (ct->vtable && ct->vtable->call) {
            func_t *fn = *(func_t **)value_data(callee);
            if (fn && !fn->cfunc) {
                sema_func_t *sf = sema_func_by_id(ctx->sema, fn->id);
                if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
                    return ctfe_call_ast_fn(ctx, (ast_func_def_t *)sf->def, args);
                return ctfe_errf(ctx,
                    "ctfe: function id %u is not a clux function definition",
                    (unsigned)fn->id);
            }
        }
    }

    size_t argc = ctfe_count_siblings(args);
    value_t *av[argc > 0 ? argc : 1];
    size_t i = 0;
    for (ast_node_t *a = args; a; a = a->next) {
        av[i] = ctfe_eval(ctx, a);
        if (value_is_error(vm, av[i])) return av[i];
        i++;
    }
    return value_call(vm, callee, av, argc);
}
