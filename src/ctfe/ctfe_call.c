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
   调用方 scope 的返回值（无显式 return → void）。
   带捕获的函数（闭包/defer 改写产物 __defer_N）：在 caller 与 callee 之间
   插入独立的 closure scope（parent = global_scope，与运行期 func_vcall 的
   closure_scope->parent = root_scope 同构），捕获值在调用点 clone 绑定——
   函数体通过 scope_lookup 查到的是捕获副本而非外层原始变量。 */
value_t *ctfe_call_ast_fn(ctfe_ctx_t *ctx, ast_func_def_t *fn,
                          ast_node_t *args) {
    vm_t *vm = ctx->vm;
    scope_t *caller_scope = vm->current_scope;

    /* 0. 闭包捕获：有 captures 时建独立 closure scope（parent=global_scope，
       隔离 caller 局部变量），在**调用点** clone 绑定捕获值。
       纯 id 捕获：clone caller scope 中同名变量的当前值（值捕获语义）。
       括号捕获：在 caller scope 求值 init 表达式 → clone。
       运行期等价：SET_CLOSURE 在定义点把 clone 值填进 func_t->closure_scope，
       func_vcall 切到 closure_scope 执行。CTFE 无 func_t 闭包对象，在调用点
       即时重建——语义等价（捕获是值副本，定义后外层修改不影响已捕获的值）。 */
    scope_t *closure_scope = NULL;
    if (fn->captures) {
        closure_scope = scope_new(vm->alloc, vm->global_scope);
        if (!closure_scope)
            return ctfe_hoist_error(ctx,
                ctfe_err(ctx, "ctfe: out of memory creating closure scope"));
        vm->current_scope = closure_scope;
        for (ast_node_t *c = fn->captures; c; c = c->next) {
            ast_var_def_t *cv = (ast_var_def_t *)c;
            value_t *cap_val;
            if (cv->init) {
                /* 括号捕获：在 caller scope 求值 init */
                vm->current_scope = caller_scope;
                cap_val = ctfe_eval(ctx, cv->init);
                if (value_is_error(vm, cap_val)) {
                    cap_val = ctfe_hoist_error(ctx, cap_val);
                    vm->current_scope = caller_scope;
                    scope_destroy(vm, &closure_scope);
                    return cap_val;
                }
            } else {
                /* 纯 id 捕获：clone caller scope 中同名变量的值 */
                value_t *outer = scope_lookup(caller_scope, cv->name);
                if (!outer) {
                    value_t *e = ctfe_errf(ctx,
                        "ctfe: cannot capture undefined variable '%.*s'",
                        (int)cv->name.len, cv->name.ptr);
                    e = ctfe_hoist_error(ctx, e);
                    vm->current_scope = caller_scope;
                    scope_destroy(vm, &closure_scope);
                    return e;
                }
                if (value_is_shadow(outer)) {
                    value_t *e = ctfe_errf(ctx,
                        "ctfe: cannot capture runtime variable '%.*s'",
                        (int)cv->name.len, cv->name.ptr);
                    e = ctfe_hoist_error(ctx, e);
                    vm->current_scope = caller_scope;
                    scope_destroy(vm, &closure_scope);
                    return e;
                }
                cap_val = outer; /* scope_define 会 clone */
            }
            vm->current_scope = closure_scope;
            char nb[128];
            const char *name = ctfe_slice_to_cstr(cv->name, nb, sizeof nb);
            value_t *stored = scope_define(vm, closure_scope, name, cap_val);
            if (value_is_error(vm, stored)) {
                stored = ctfe_hoist_error(ctx, stored);
                vm->current_scope = caller_scope;
                scope_destroy(vm, &closure_scope);
                return stored;
            }
        }
        vm->current_scope = caller_scope;
    }

    /* 1. 临时局部作用域（参数 + 函数体变量）。
       有捕获时 parent = closure_scope（隔离 caller 局部，仅可见捕获+全局）；
       无捕获时 parent = caller_scope（保持原有行为）。 */
    if (closure_scope) {
        scope_t *callee = scope_new(vm->alloc, closure_scope);
        if (!callee) {
            scope_destroy(vm, &closure_scope);
            vm->current_scope = caller_scope;
            return ctfe_hoist_error(ctx,
                ctfe_err(ctx, "ctfe: out of memory creating callee scope"));
        }
        vm->current_scope = callee;
    } else {
        vm_push_scope(vm);
    }
    scope_t *callee_scope = vm->current_scope;

    /* 2. 绑定参数：实参链与参数声明链逐对对应。
       错误路径统一走 cleanup 标签（同时销毁 callee + closure scope）。
       有 closure scope 时 ctfe_hoist_error 只上移到 closure scope（pop 时
       销毁），因此错误路径须直接 clone 到 caller_scope——用 hoist_to_caller
       绕过中间 scope。 */
    value_t *ret = NULL;
    ast_node_t *a = args;
    for (ast_node_t *p = fn->params; p; p = p->next) {
        if (!a) {
            ret = ctfe_err(ctx, "ctfe: function call: too few arguments");
            goto cleanup;
        }
        value_t *av = ctfe_eval(ctx, a);
        if (value_is_error(vm, av)) {
            ret = av;
            goto cleanup;
        }
        ast_var_def_t *vd = (ast_var_def_t *)p;
        char nb[128];
        const char *name = ctfe_slice_to_cstr(vd->name, nb, sizeof nb);
        value_t *stored = scope_define(vm, vm->current_scope, name, av);
        if (value_is_error(vm, stored)) {
            ret = stored;
            goto cleanup;
        }
        a = a->next;
    }
    if (a) {
        ret = ctfe_err(ctx, "ctfe: function call: too many arguments");
        goto cleanup;
    }

    /* 3. 解释函数体（body 为 AST_BLOCK，内部自带块作用域） */
    ctx->ctrl = CTFE_CTRL_NONE;
    ctx->ret_value = NULL;
    value_t *stmt_r = ctfe_eval_stmt(ctx, fn->body);
    if (value_is_error(vm, stmt_r)) {
        ret = stmt_r;
        goto cleanup;
    }

    /* 4. 取返回值并 clone 到调用方 scope（必须在 pop 之前：ret_value 归
       callee scope，pop 会销毁） */
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

cleanup:
    /* 5. 错误路径：ret 在 callee/closure scope，须直接 clone 到 caller scope
       （跳过中间 closure scope——ctfe_hoist_error 只上移一层到 closure scope，
       pop 时会销毁）。成功路径 ret 已在 caller scope（step 4），clone 是幂等
       的额外开销但 value_clone 对 error 以外的值在 caller scope 仍安全。 */
    if (ret && value_is_error(vm, ret)) {
        vm->current_scope = caller_scope;
        ret = value_clone(vm, ret);
    }
    /* pop 函数局部作用域（vm_pop_scope 无参、pop 当前作用域，误 pop caller
       会销毁调用方作用域）。有 closure scope 时再销毁 closure scope。 */
    vm->current_scope = callee_scope;
    vm_pop_scope(vm);
    if (closure_scope) {
        vm->current_scope = closure_scope;
        vm_pop_scope(vm);
    }
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
