/* M6 FFI Phase 5（语言层接线）：把 clux 函数变成 **C 函数指针**
 *
 *     cfunc cmp(a: i32, b: i32): i32 { return a - b; }
 *     var p: opaque = cfunc_ptr(cmp);   // p 是可被 C 调用的函数指针
 *
 * 机制：内建 `cfunc_ptr(f)` 用 libffi 为 f 的 §2 签名**惰性创建闭包**：
 *
 *     C 侧 `p(a, b)` → libffi trampoline → cfunc_closure_handler
 *        ├─ 按签名把 C 实参编组成 clux 值（value_alloc_shell + 按类型宽度拷贝）
 *        ├─ value_call(vm, f, args, argc) —— 完整 func_vcall 流程（作用域切换、
 *        │  实参 clone/隐式转换、返回值 clone 回调用方作用域）
 *        └─ 按返回类型把 clux 返回值回写 C 的 ret 槽
 *
 * 为什么"惰性 + 挂在函数对象上"：libffi 的 trampoline 不在本项目 allocator
 * 记账内（见 docs/m6-design.md §14.14），必须显式销毁，否则静默泄漏可执行内存。
 * 函数对象（func_t）是 **VM 生命周期**对象（vm->functions 统一释放），与
 * `closure_scope` 同一模型，因此句柄存在 func_t 上、在 func_destroy 释放：
 * 生命周期清晰、重复请求复用同一 trampoline、`delete_allocator` 亦不会漏报。
 *
 * 与 docs/m6-design.md §3.2 的差异：设计稿用 `ffi_func_t` + 定义时立即建闭包；
 * 现实现把闭包**惰性**建在既有 func_t 上（不改函数对象布局/不改调用路径），
 * 且签名不变量由 §2 谓词（ffi_c_abi_reject_reason）+ 本文件的可编组类型表共同
 * 保证。设计稿里"cfunc 值自身即 C 函数指针、clux 调用经 ffi_call 走同一个指针"
 * 的形态未采用——clux 侧调用仍走解释器直调（少一次 ABI 往返）。
 */
#include "vm/vm.h"
#include "vm/function.h"
#include "vm/type_func.h"
#include "vm/value.h"
#include "vm/ffi_lib.h"
#include "core/allocator.h"

#ifdef CLUX_HAVE_LIBFFI
#include <ffi.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 与 FFI 调用层同一上限（src/vm/ffi_lib.c 的 FFI_MAX_ARGS） */
#define CFUNC_CLOSURE_MAX_ARGS 16u

#ifdef CLUX_HAVE_LIBFFI

/* 闭包 handler 的上下文：被包装的函数 + 签名。归调用它的 func_t 所有
   （func_destroy 释放），因此 handler 生命周期内始终有效。
   `owned` 保存本层新建的 libffi 描述符（struct/cunion 的树）——libffi 的 cif
   会持有描述符指针直到闭包销毁，所以必须活到那时，由 dispose 统一释放。 */
typedef struct {
    vm_t          *vm;
    func_t        *fn;    /* 被包装的 clux 函数（归 vm->functions） */
    const type_t  *sig;   /* 签名类型（参数/返回类型来源） */
    size_t         argc;
    void          *owned[CFUNC_CLOSURE_MAX_ARGS + 1];
    size_t         owned_count;
} cfunc_ctx_t;

static void cfunc_ctx_dispose(void *self, allocator_t *allocator) {
    (void)allocator;
    cfunc_ctx_t *ctx = (cfunc_ctx_t *)self;
    for (size_t i = 0; i < ctx->owned_count; i++)
        ffi_type_release(ctx->owned[i]);
    ctx->owned_count = 0;
}

static class_t g_cfunc_ctx_class = {
    .name       = "clux.ffi.cfunc_ctx",
    .size       = sizeof(cfunc_ctx_t),
    .clone_fn   = NULL,
    .move_fn    = NULL,
    .dispose_fn = cfunc_ctx_dispose,
};

/* ---- clux 类型 → libffi 类型描述符 ----
 *
 * 直接复用调用层同一张映射（`ffi_type_for`）：标量/str/enum/指针 → 静态描述符；
 * struct/cunion → 新建描述符树（`*owned` 置 true，由 ctx 接管并在销毁时释放）。
 * §2 之外的类型仍给明确原因（option/切片/元组/闭包…）。
 *
 * **C → clux 的编组无需分类**：clux 的 value->data 与 C 同构，handler 直接按
 * 参数类型的 size 拷贝 libffi 给的实参指针即可（结构体就是那块 C 布局字节）。 */
static const void *cfunc_ffi_type(vm_t *vm, const type_t *t, bool *owned,
                                  const char **why) {
    const char *rej = ffi_c_abi_reject_reason(t);
    if (rej) {
        *why = rej;
        return NULL;
    }
    char err[256] = {0};
    const void *ft = ffi_type_for(t, vm, owned, err, sizeof(err));
    if (!ft) *why = err[0] ? "type cannot be marshalled across a closure" : "";
    return ft;
}

/* ---- C → clux：把 C 实参编组成 clux 值 ----
 *
 * libffi 的 args[i] 指向该实参的值，宽度与上面选出的 ffi_type 一致（与 clux
 * 类型同宽）。未 track（value_alloc_shell）：调用返回后由本函数释放，value_call
 * 已把实参 clone 进被调函数作用域。 */
static void cfunc_closure_handler(void *ret, void **args, void *user) {
    cfunc_ctx_t *ctx = (cfunc_ctx_t *)user;
    if (!ctx || !ctx->vm || !ctx->fn || !ctx->sig) return;
    vm_t *vm = ctx->vm;

    value_t *cargs[CFUNC_CLOSURE_MAX_ARGS];
    size_t n = ctx->argc;
    if (n > CFUNC_CLOSURE_MAX_ARGS) n = CFUNC_CLOSURE_MAX_ARGS;
    for (size_t i = 0; i < n; i++) {
        const type_t *pt = func_type_param(ctx->sig, i);
        void *data = value_alloc_data_copy(vm->alloc, pt, args[i]);
        cargs[i] = value_alloc_shell(vm->alloc, pt, data);
    }

    /* 函数值现场构造（data = func_t*，type = 签名；与 func_new/LOAD_FUNCTION 同一
       约定）。value_make track 到当前作用域——与 value_call 的返回值同一归属，
       随该作用域销毁，不在此处释放。 */
    void *fdata = value_alloc_data_copy(vm->alloc, ctx->sig, &ctx->fn);
    value_t *fval = value_make(vm, ctx->sig, fdata);
    value_t *r = value_call(vm, fval, cargs, n);

    for (size_t i = 0; i < n; i++) {
        value_dispose(vm, cargs[i]);
        allocator_free(vm->alloc, (void **)&cargs[i]);
    }

    /* clux → C：按返回类型宽度回写。void 无返回槽；错误/缺返回写零值而不是
       让 C 侧拿到未初始化内存（C ABI 无法表达 clux 错误，调用方约定见设计文档）。 */
    const type_t *rt = func_type_return(ctx->sig);
    if (!ret || !rt || rt == vm->type_void) return;
    if (!r || value_is_error(vm, r) || !value_data(r)) {
        memset(ret, 0, rt->size);
        return;
    }
    memcpy(ret, value_data(r), rt->size);
}

#endif /* CLUX_HAVE_LIBFFI */

/* ---- 内建函数 `cfunc_ptr(f)` ----
 * 签名（sema 侧同）：variadic + 返回 opaque —— 参数类型在 cfunc 内部自验
 * （与 len/upgrade 同一套路）。返回的 opaque 值 data 即 trampoline 地址。 */
static value_t *builtin_cfunc_ptr_cfunc(vm_t *vm, func_t *self, size_t argc,
                                        value_t **args) {
    (void)self;
#if !defined(CLUX_HAVE_LIBFFI)
    (void)argc;
    (void)args;
    return value_make_error(vm, "cfunc_ptr: closures require the libffi path");
#else
    if (argc != 1)
        return value_make_error(
            vm, "cfunc_ptr: expects exactly one argument (a function)");
    value_t *fv = args[0];
    if (!fv || value_is_shadow(fv) || !value_data(fv))
        return value_make_error(vm, "cfunc_ptr: argument must be a function value");
    const type_t *sig = value_type(fv);
    if (!sig || sig->kind != TYPE_KIND_FUNC)
        return value_make_error(
            vm, "cfunc_ptr: argument must be a named function (cfunc definition)");
    func_t *fn = *(func_t **)value_data(fv);
    if (!fn || !fn->cfunc)
        return value_make_error(vm, "cfunc_ptr: function is not callable");

    /* 已创建：复用同一 trampoline（C 侧可比较指针相等，且不重复占可执行内存） */
    if (fn->ffi_closure_code) {
        int64_t raw = (int64_t)(intptr_t)fn->ffi_closure_code;
        return value_make(vm, vm->type_opaque,
                          value_alloc_data_copy(vm->alloc, vm->type_opaque, &raw));
    }

    if (func_type_is_variadic(sig))
        return value_make_error(
            vm, "cfunc_ptr: variadic functions cannot be C callbacks");
    size_t n = func_type_param_count(sig);
    if (n > CFUNC_CLOSURE_MAX_ARGS)
        return value_make_error(vm, "cfunc_ptr: too many parameters for a closure");

    char msg[512];
    const char *why = NULL;
    void *argtypes[CFUNC_CLOSURE_MAX_ARGS];
    /* 先攒 ctx（新建描述符的所有权立刻转进去：中途失败也能靠 allocator_free
       触发 dispose 释放，不会泄漏） */
    cfunc_ctx_t *ctx = (cfunc_ctx_t *)allocator_new(
        vm->alloc, &g_cfunc_ctx_class, 1);
    if (!ctx) return value_make_error(vm, "cfunc_ptr: out of memory");
    ctx->vm = vm;
    ctx->fn = fn;
    ctx->sig = sig;
    ctx->argc = n;
    ctx->owned_count = 0;

    for (size_t i = 0; i < n; i++) {
        const type_t *pt = func_type_param(sig, i);
        if (pt == vm->type_void) {
            allocator_free(vm->alloc, (void **)&ctx);
            snprintf(msg, sizeof(msg),
                     "cfunc_ptr: parameter %zu is void", i + 1);
            return value_make_error(vm, msg);
        }
        bool owned = false;
        const void *ft = cfunc_ffi_type(vm, pt, &owned, &why);
        if (!ft) {
            allocator_free(vm->alloc, (void **)&ctx);
            snprintf(msg, sizeof(msg), "cfunc_ptr: parameter %zu: %s", i + 1,
                     why && *why ? why : "type cannot cross the C ABI");
            return value_make_error(vm, msg);
        }
        if (owned && ctx->owned_count <= CFUNC_CLOSURE_MAX_ARGS)
            ctx->owned[ctx->owned_count++] = (void *)ft;
        argtypes[i] = (void *)ft;
    }
    const type_t *rt = func_type_return(sig);
    void *rtype = (void *)&ffi_type_void;
    if (rt && rt != vm->type_void) {
        bool owned = false;
        const void *ft = cfunc_ffi_type(vm, rt, &owned, &why);
        if (!ft) {
            allocator_free(vm->alloc, (void **)&ctx);
            snprintf(msg, sizeof(msg), "cfunc_ptr: return type: %s",
                     why && *why ? why : "type cannot cross the C ABI");
            return value_make_error(vm, msg);
        }
        if (owned && ctx->owned_count <= CFUNC_CLOSURE_MAX_ARGS)
            ctx->owned[ctx->owned_count++] = (void *)ft;
        rtype = (void *)ft;
    }

    char err[512] = {0};
    void *handle = NULL, *code = NULL;
    if (!clux_ffi_closure_create(vm->alloc, n, argtypes, rtype,
                                 cfunc_closure_handler, ctx, &handle, &code, err,
                                 sizeof(err))) {
        allocator_free(vm->alloc, (void **)&ctx);
        snprintf(msg, sizeof(msg), "cfunc_ptr: %s",
                 err[0] ? err : "closure creation failed");
        return value_make_error(vm, msg);
    }
    /* 所有权转移给函数对象：随 vm->functions 释放（见 func_destroy） */
    fn->ffi_closure = handle;
    fn->ffi_closure_ctx = ctx;
    fn->ffi_closure_code = code;

    int64_t raw = (int64_t)(intptr_t)code;
    return value_make(vm, vm->type_opaque,
                      value_alloc_data_copy(vm->alloc, vm->type_opaque, &raw));
#endif /* CLUX_HAVE_LIBFFI */
}

/* 注册 cfunc_ptr 到 vm->global_scope（调用方在 vm_new 内调用）。 */
void vm_register_cfunc_ptr(vm_t *vm) {
    if (!vm || !vm->global_scope) return;
    const type_t *sig = type_func_sig(vm, NULL, NULL, 0, vm->type_opaque,
                                      OWN_NONE, /*is_variadic=*/true);
    if (!sig) return;
    value_t *fnv = func_new(vm, builtin_cfunc_ptr_cfunc, NULL, vm->root_scope,
                            sig, STRSLICE_LIT("cfunc_ptr"));
    if (!fnv) return;
    value_t *stored = scope_define(vm, vm->global_scope, "cfunc_ptr", fnv);
    if (!stored || value_is_error(vm, stored)) return;
}
