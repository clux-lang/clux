#include "ctfe/ctfe.h"
#include "sema/sema.h"

#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"
#include "vm/value.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 内部工具
 *
 * 共享工具（ctfe_err/ctfe_make_int/ctfe_hoist_* 等）供 ctfe_expr.c /
 * ctfe_stmt.c / ctfe_call.c 经 ctfe.h internal 段调用。求值状态
 * （budget/depth/ctrl/ret_value）存于 ctfe_ctx_t。
 * =========================================================================== */

value_t *ctfe_err(ctfe_ctx_t *ctx, const char *msg) {
    return value_make_error(ctx->vm, msg);
}

/* 格式化错误（带函数名等上下文的诊断） */
value_t *ctfe_errf(ctfe_ctx_t *ctx, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return value_make_error(ctx->vm, buf);
}

/* 兄弟链计数 */
size_t ctfe_count_siblings(const ast_node_t *node) {
    size_t n = 0;
    for (const ast_node_t *p = node; p; p = p->next) n++;
    return n;
}

/* strslice → NUL 结尾临时缓冲区（栈上，仅短名使用） */
const char *ctfe_slice_to_cstr(strslice_t s, char *buf, size_t cap) {
    if (s.len >= cap) s.len = cap - 1;
    memcpy(buf, s.ptr, s.len);
    buf[s.len] = '\0';
    return buf;
}

/* 按类型宽度构造整数值（i8..u64） */
value_t *ctfe_make_int(ctfe_ctx_t *ctx, const type_t *t, uint64_t v) {
    void *data = value_alloc_data(ctx->vm->alloc, t);
    if (t == ctx->vm->type_i8)  *(int8_t  *)data = (int8_t)v;
    else if (t == ctx->vm->type_i16) *(int16_t *)data = (int16_t)v;
    else if (t == ctx->vm->type_i32) *(int32_t *)data = (int32_t)v;
    else if (t == ctx->vm->type_i64) *(int64_t *)data = (int64_t)v;
    else if (t == ctx->vm->type_u8)  *(uint8_t  *)data = (uint8_t)v;
    else if (t == ctx->vm->type_u16) *(uint16_t *)data = (uint16_t)v;
    else if (t == ctx->vm->type_u32) *(uint32_t *)data = (uint32_t)v;
    else if (t == ctx->vm->type_u64) *(uint64_t *)data = (uint64_t)v;
    else {
        /* 非整数类型：data 已分配，按错误路径释放 */
        allocator_free(ctx->vm->alloc, (void **)&data);
        return ctfe_err(ctx, "ctfe: internal: make_int on non-integer type");
    }
    return value_make(ctx->vm, t, data);
}

/* AST_INT_LIT 类型后缀 → vm 整数类型；空后缀默认 i32 */
const type_t *ctfe_int_lit_type(ctfe_ctx_t *ctx, strslice_t suffix) {
    vm_t *vm = ctx->vm;
    if (strslice_is_empty(suffix)) return vm->type_i32;
    if (strslice_eq(suffix, STRSLICE_LIT("i8")))  return vm->type_i8;
    if (strslice_eq(suffix, STRSLICE_LIT("i16"))) return vm->type_i16;
    if (strslice_eq(suffix, STRSLICE_LIT("i32"))) return vm->type_i32;
    if (strslice_eq(suffix, STRSLICE_LIT("i64"))) return vm->type_i64;
    if (strslice_eq(suffix, STRSLICE_LIT("u8")))  return vm->type_u8;
    if (strslice_eq(suffix, STRSLICE_LIT("u16"))) return vm->type_u16;
    if (strslice_eq(suffix, STRSLICE_LIT("u32"))) return vm->type_u32;
    if (strslice_eq(suffix, STRSLICE_LIT("u64"))) return vm->type_u64;
    return NULL;
}

/* 读取 bool 值（调用方保证类型为 bool） */
bool ctfe_read_bool(vm_t *vm, value_t *v) {
    (void)vm;
    return *(bool *)value_data(v);
}

/* ===========================================================================
 * 作用域上移工具
 * =========================================================================== */

/* RETURN 控制流下把 ret_value 上移到父作用域。作用域 pop 前调用：value 归
   owned 向量管理，pop 会 dispose+free；上移保证 ret_value 存活到 callee
   函数作用域（嵌套块逐层上移），ctfe_call_ast_fn 在 pop callee 前 clone 到
   调用方。 */
void ctfe_hoist_return(ctfe_ctx_t *ctx) {
    vm_t *vm = ctx->vm;
    if (ctx->ctrl != CTFE_CTRL_RETURN || !ctx->ret_value) return;
    scope_t *scope = vm->current_scope;
    if (!scope || !scope->parent) return;
    scope_t *saved = vm->current_scope;
    vm->current_scope = scope->parent;
    value_t *hoisted = value_clone(vm, ctx->ret_value);
    vm->current_scope = saved;
    ctx->ret_value = hoisted;
}

/* error value 跨作用域传播：pop 前 clone 到父作用域（error 归创建时 scope，
   pop 会 dispose+free；上移保证 error 存活到调用方，与 ctfe_hoist_return 同理）。
   非 error / 无父作用域时原样返回。 */
value_t *ctfe_hoist_error(ctfe_ctx_t *ctx, value_t *e) {
    vm_t *vm = ctx->vm;
    if (!e || !value_is_error(vm, e)) return e;
    scope_t *scope = vm->current_scope;
    if (!scope || !scope->parent) return e;
    scope_t *saved = vm->current_scope;
    vm->current_scope = scope->parent;
    value_t *hoisted = value_clone(vm, e);
    vm->current_scope = saved;
    return hoisted;
}

/* ===========================================================================
 * 表达式求值入口
 * =========================================================================== */

value_t *ctfe_eval(ctfe_ctx_t *ctx, ast_node_t *node) {
    if (!ctx || !ctx->vm) return NULL;
    if (!node) return ctfe_err(ctx, "ctfe: null expression node");
    if (ctx->budget == 0)
        return ctfe_err(ctx, "ctfe: evaluation budget exceeded (possible loop)");
    ctx->budget--;
    if (ctx->depth >= ctx->max_depth)
        return ctfe_err(ctx, "ctfe: evaluation depth exceeded");
    ctx->depth++;
    value_t *r = ctfe_eval_inner(ctx, node);
    ctx->depth--;
    return r;
}
