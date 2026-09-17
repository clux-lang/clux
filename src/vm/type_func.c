#include "vm/type_func.h"
#include "vm/value.h"
#include "vm/function.h"
#include "vm/vm.h"
#include "vm/scope.h"
#include "vm/type_error.h"
#include "core/panic.h"
#include "core/allocator.h"
#include "core/string.h"

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdalign.h>

/* ---- dispose ---- */

static void func_dispose(vm_t *vm, value_t *v) {
    /* func_t 归 vm->functions 统一释放（clone 浅拷贝指针，value dispose 不能
       释放共享的 func_t，否则 double free）；value_dispose 已释放 data 块 */
    (void)vm;
    (void)v;
}

/* ---- clone ---- */

static value_t *func_clone(vm_t *vm, value_t *v) {
    /* 函数值不可变，浅拷贝指针即可 */
    const func_t *fn = *(const func_t **)value_data(v);
    void *data = value_alloc_data_copy(vm->alloc, value_type(v), &fn);
    return value_make(vm, value_type(v), data);
}

/* ---- assign ---- */

/* 函数赋值：源与目标签名一致（implicit_cast 仅同签名可过）→ clone/浅拷贝。
   shadow 只校验类型兼容性，不拷贝 data（对齐 int_assign 模式）。 */
static value_t *func_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src))
        return dst;
    memcpy(value_data(dst), value_data(src), value_type(dst)->size);
    return dst;
}

/* ---- call: 通过 vtable 分派的函数调用 ---- */

/* 类型名 → 缓冲区（诊断用） */
static void sig_type_name(const type_t *t, char *buf, size_t cap) {
    if (!t || !t->name.ptr) {
        snprintf(buf, cap, "<none>");
        return;
    }
    size_t n = t->name.len < cap - 1 ? t->name.len : cap - 1;
    memcpy(buf, t->name.ptr, n);
    buf[n] = '\0';
}

/* shadow 调用：纯类型检查，不执行 cfunc、不切换作用域。
   签名信息全部来自签名 type_t（value->type），因此 shadow value（data=NULL）
   也能完成校验。返回 return_type 的 shadow value，失败返回 error value。 */
static value_t *func_shadow_call(vm_t *vm, const type_t *ct,
                                 value_t **args, size_t argc) {
    /* 仅签名类型可校验：vm->type_func 是无签名 func 基类（sig 空） */
    if (!ct || ct->vtable != &VTABLE_FUNC || ct == vm->type_func) {
        return value_make_error(vm, "func call: function has no signature");
    }
    const func_sig_t *sig = &((const func_type_t *)ct)->sig;
    char msg[96];

    /* 参数数量校验（非 variadic 必须精确匹配；variadic 至少 param_count） */
    if (!sig->is_variadic && argc != sig->param_count) {
        snprintf(msg, sizeof msg, "expects %zu arguments, got %zu",
                 sig->param_count, argc);
        return value_make_error(vm, msg);
    }
    if (sig->is_variadic && argc < sig->param_count) {
        snprintf(msg, sizeof msg, "expects at least %zu arguments",
                 sig->param_count);
        return value_make_error(vm, msg);
    }

    /* 逐个参数 implicit_cast 校验（跳过 error/void 恢复产物，避免级联二次诊断） */
    for (size_t i = 0; i < argc; i++) {
        if (value_is_error(vm, args[i]) ||
            value_type(args[i]) == vm->type_void)
            continue;
        if (i < sig->param_count && sig->params[i] &&
            !type_eq(value_type(args[i]), sig->params[i])) {
            value_t *casted = value_implicit_cast(vm, args[i], sig->params[i]);
            if (value_is_error(vm, casted)) {
                char an[64], pn[64];
                sig_type_name(value_type(args[i]), an, sizeof an);
                sig_type_name(sig->params[i], pn, sizeof pn);
                snprintf(msg, sizeof msg, "argument %zu: cannot convert %s to %s",
                         i + 1, an, pn);
                return value_make_error(vm, msg);
            }
        }
        /* variadic 额外参数：求值但类型不限 */
    }

    return value_make_shadow(vm, sig->return_type ? sig->return_type
                                                  : vm->type_void);
}

static value_t *func_vcall(vm_t *vm, value_t *callee, value_t **args, size_t argc) {
    const type_t *ct = value_type(callee);

    /* shadow 路径必须最先判定：shadow 值 data=NULL，不能 value_data */
    if (value_is_shadow(callee)) {
        return func_shadow_call(vm, ct, args, argc);
    }
    for (size_t i = 0; i < argc; i++) {
        if (value_is_shadow(args[i])) {
            return func_shadow_call(vm, ct, args, argc);
        }
    }

    func_t *fn = *(func_t **)value_data(callee);
    if (!fn || !fn->cfunc) {
        return value_make_error(vm, "func call: invalid function");
    }
    /* 签名读取：value->type 即签名类型（func_new 以 sig_type 创建）；
       func 基类（vm->type_func，无签名）→ sig=NULL 跳过校验 */
    const type_t *st = (ct && ct->vtable == &VTABLE_FUNC && ct != vm->type_func)
                           ? ct : NULL;
    const func_sig_t *sig = st ? &((const func_type_t *)st)->sig : NULL;

    /* 参数数量检查：非 variadic 函数拒绝多余实参 */
    if (sig && !sig->is_variadic && argc > sig->param_count) {
        return value_make_error(vm, "func call: too many arguments");
    }

    /* 1. 保存现场 */
    scope_t *caller_root  = vm->root_scope;
    scope_t *caller_scope = vm->current_scope;

    /* 2. 切换到函数的模块作用域 */
    vm->root_scope = fn->root_scope;

    /* 3. 临时接线孤立 closure_scope → root_scope（函数体可查看到模块变量），
       进入闭包作用域；调用结束恢复 parent（见步骤 9） */
    scope_t *saved_closure_parent = NULL;
    if (fn->closure_scope) {
        saved_closure_parent = fn->closure_scope->parent;
        fn->closure_scope->parent = fn->root_scope;
    }
    vm->current_scope = fn->closure_scope;

    /* 4. push 匿名局部作用域 */
    vm_push_scope(vm);

    /* 5. safe_cast + clone 参数到当前作用域（value_clone 自动注册到 owned） */
    value_t *local_args[argc > 0 ? argc : 1];
    value_t *ret = NULL;
    bool is_error = false;

    for (size_t i = 0; i < argc; i++) {
        if (sig && sig->params && i < sig->param_count && sig->params[i]
            && value_type(args[i]) != sig->params[i]) {
            /* safe_cast: implicit_cast（auto-tracked），失败返回 error */
            value_t *casted = value_implicit_cast(vm, args[i], sig->params[i]);
            if (value_is_error(vm, casted)) {
                is_error = true;
                ret = casted;
                break;
            }
            local_args[i] = casted;
        } else {
            /* 类型匹配、无类型声明、或 variadic 额外参数：clone（auto-tracked） */
            local_args[i] = value_clone(vm, args[i]);
        }
    }

    /* 6. call cfunc（仅在参数处理成功时） */
    if (!is_error) {
        value_t *result = fn->cfunc(vm, fn, argc, local_args);
        if (result && value_type(result)) {
            if (value_is_error(vm, result)) {
                is_error = true;
                ret = result;  /* error 借用 callee scope */
            } else if (sig && sig->return_type &&
                       value_type(result) != sig->return_type) {
                /* safe_cast 返回值到声明的返回类型（auto-tracked 到 callee scope） */
                ret = value_implicit_cast(vm, result, sig->return_type);
                if (value_is_error(vm, ret)) {
                    is_error = true;
                }
            } else {
                /* 类型匹配或无返回类型声明，借用 */
                ret = result;
            }
        }
    }

    /* 7. clone 返回值/error 到调用方作用域 */
    /* 临时切换 current_scope 到 caller，clone 后自动 track 到 caller 的 owned */
    scope_t *callee_current = vm->current_scope;
    vm->current_scope = caller_scope;
    if (ret && value_type(ret)) {
        ret = value_clone(vm, ret);
    }

    /* 8. 平衡作用域栈：销毁 callee 作用域子树 */
    vm->current_scope = callee_current;
    if (is_error) {
        /* error 路径：砍掉子树，不走 pop_scope（避免触发 defer） */
        while (vm->current_scope != fn->closure_scope) {
            if (!vm->current_scope || vm->current_scope == vm->global_scope) break;
            scope_t *cur = vm->current_scope;
            scope_t *parent = scope_parent(cur);
            scope_destroy_subtree(vm, &cur);
            vm->current_scope = parent;
        }
    } else {
        /* 正常路径：逐层 pop_scope */
        while (vm->current_scope != fn->closure_scope) {
            if (!vm->current_scope || vm->current_scope == vm->global_scope) break;
            vm_pop_scope(vm);
        }
    }

    /* 9. 恢复现场（含孤立 closure_scope 的临时 parent 接线） */
    if (fn->closure_scope) {
        fn->closure_scope->parent = saved_closure_parent;
    }
    vm->root_scope    = caller_root;
    vm->current_scope = caller_scope;

    return ret;
}

/* ---- eq/ne：func == nil（0 初始化检测）与 func == func（指针比较） ---- */

/* bool 值构造（与其他 vtable 同模式） */
static value_t *func_bool_store(vm_t *vm, bool val) {
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = val;
    return value_make(vm, vm->type_bool, data);
}

static value_t *func_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
    bool a_nil = (value_type(a) == vm->type_nil);
    bool b_nil = (value_type(b) == vm->type_nil);
    if (a_nil || b_nil) {
        /* func == nil：函数指针是否为 NULL（nil 的 0 初始化语义） */
        if (value_is_shadow(a) || value_is_shadow(b))
            return value_make_shadow(vm, vm->type_bool);
        const func_t *fn = *(const func_t **)value_data(a_nil ? b : a);
        return func_bool_store(vm, fn == NULL);
    }
    /* func == func：签名类型必须一致，指针比较 */
    if (value_type(a) != value_type(b))
        return value_make_error(vm, "==: function type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const func_t *fa = *(const func_t **)value_data(a);
    const func_t *fb = *(const func_t **)value_data(b);
    return func_bool_store(vm, fa == fb);
}

static value_t *func_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
    bool a_nil = (value_type(a) == vm->type_nil);
    bool b_nil = (value_type(b) == vm->type_nil);
    if (a_nil || b_nil) {
        if (value_is_shadow(a) || value_is_shadow(b))
            return value_make_shadow(vm, vm->type_bool);
        const func_t *fn = *(const func_t **)value_data(a_nil ? b : a);
        return func_bool_store(vm, fn != NULL);
    }
    if (value_type(a) != value_type(b))
        return value_make_error(vm, "!=: function type mismatch");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const func_t *fa = *(const func_t **)value_data(a);
    const func_t *fb = *(const func_t **)value_data(b);
    return func_bool_store(vm, fa != fb);
}

const vtable_t VTABLE_FUNC = {
    .dispose   = func_dispose,
    .clone     = func_clone,
    .assign    = func_assign,
    .call      = func_vcall,
    .type_seal = func_type_seal,
    .eq        = func_eq,
    .ne        = func_ne,
};

/* ================================================================ */
/* func type 构造（与 array type 统一：push 入池 + 压栈 + 分步 set + seal） */
/* ================================================================ */

/* 比较两签名是否等价（按 params/return_type/is_variadic 指针比较去重） */
static bool func_sig_eq(const func_sig_t *a, const func_sig_t *b) {
    if (!a || !b) return false;
    if (a->param_count != b->param_count) return false;
    if (a->is_variadic != b->is_variadic) return false;
    if (a->return_type != b->return_type) return false;
    if (a->param_count > 0 &&
        memcmp(a->params, b->params, a->param_count * sizeof(type_t *)) != 0)
        return false;
    return true;
}

/* 构造规范类型名 "func(i32, i32): i32" / "func(...): void" */
static char *func_sig_name(allocator_t *alloc, const type_t *const *params,
                           size_t param_count, const type_t *return_type,
                           bool is_variadic) {
    string_t *s = string_new(alloc);
    if (!s) panic("vm: out of memory building func type name");
    string_append_cstr(s, "func(");
    if (is_variadic && param_count == 0) {
        string_append_cstr(s, "...");
    } else {
        for (size_t i = 0; i < param_count; i++) {
            if (i > 0) string_append_cstr(s, ", ");
            if (params && params[i] && params[i]->name.ptr)
                string_append_bytes(s, params[i]->name.ptr, params[i]->name.len);
            else
                string_append_cstr(s, "?");
        }
        if (is_variadic) string_append_cstr(s, ", ...");
    }
    string_append_cstr(s, "): ");
    if (return_type && return_type->name.ptr)
        string_append_bytes(s, return_type->name.ptr, return_type->name.len);
    else
        string_append_cstr(s, "void");
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 string_len(s) + 1);
    if (buf) {
        memcpy(buf, string_data(s), string_len(s));
        buf[string_len(s)] = '\0';
    }
    string_free(&s);
    return buf;
}

/* 内部：分配开放 func_type（不压栈、不入池），返回非 const 指针。
 * 入池动作已移至 func_type_seal（去重 intern 时完成）。 */
static func_type_t *func_type_create_open(vm_t *vm) {
    func_type_t *ft = (func_type_t *)allocator_new_ex(
        vm->alloc, "func_type_t", sizeof(func_type_t), NULL, NULL, NULL, 1);
    if (!ft) panic("vm: out of memory allocating function signature type");
    memset(ft, 0, sizeof(func_type_t));
    ft->base.vtable = &VTABLE_FUNC;
    ft->base.name   = (strslice_t){ NULL, 0 };
    ft->base.size   = sizeof(func_t *);
    ft->base.align  = alignof(func_t *);
    ft->base.kind   = TYPE_KIND_FUNC;
    /* sealed 由 memset 置 0；密封在 func_type_seal 中置位 */
    return ft;
}

const type_t *func_type_push(vm_t *vm) {
    if (!vm) return NULL;
    /* 分配空 func type 入池 + 压其 type value（对应字节码 PUSH_FUNC_TYPE） */
    func_type_t *ft = func_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &ft->base));
    return &ft->base;  /* 外部只持有 type_t*，不感知 func_type_t 子类 */
}

void func_type_add_param(vm_t *vm, const type_t *t, const type_t *param) {
    if (!vm || !t || t->kind != TYPE_KIND_FUNC) return;
    func_type_t *ft = (func_type_t *)t;
    if (type_is_sealed(t)) return;  /* 密封后不可再修改 */
    const type_t **pcopy = (const type_t **)allocator_new_ex(
        vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL,
        ft->sig.param_count + 1);
    if (!pcopy) panic("vm: out of memory appending func param type");
    if (ft->sig.param_count > 0)
        memcpy(pcopy, ft->sig.params,
               ft->sig.param_count * sizeof(type_t *));
    pcopy[ft->sig.param_count] = param;
    if (ft->sig.params) allocator_free(vm->alloc, (void **)&ft->sig.params);
    ft->sig.params = pcopy;
    ft->sig.param_count++;
}

void func_type_set_return(vm_t *vm, const type_t *t, const type_t *ret) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_FUNC) return;
    func_type_t *ft = (func_type_t *)t;
    if (type_is_sealed(t)) return;
    ft->sig.return_type = ret;
}

void func_type_set_variadic(vm_t *vm, const type_t *t, bool variadic) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_FUNC) return;
    func_type_t *ft = (func_type_t *)t;
    if (type_is_sealed(t)) return;
    ft->sig.is_variadic = variadic;
}

const type_t *func_type_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_FUNC) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    func_type_t *ft = (func_type_t *)t;

    if (!vm->sig_types) vm->sig_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按签名）：命中已有 sealed 类型则复用并手工回收本开放类型 */
    size_t n = vec_len(vm->sig_types);
    for (size_t i = 0; i < n; i++) {
        const func_type_t *other = (const func_type_t *)vec_get(vm->sig_types, i);
        if (other && other != ft && type_is_sealed(&other->base) &&
            func_sig_eq(&other->sig, &ft->sig)) {
            /* 本开放类型与已有 sealed 类型重复：复用 other。
             * 操作数栈中引用本开放类型 ft 的 type value 由 value_seal 负责
             * 重定向到 other（避免悬空）；此处仅手工回收 ft。 */
            if (ft->base.name.ptr) {
                char *np = (char *)ft->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            if (ft->sig.params) allocator_free(vm->alloc, (void **)&ft->sig.params);
            allocator_free(vm->alloc, (void **)&ft);
            return &other->base;
        }
    }

    /* 计算规范名并标记 sealed（base.size/align 恒为 func_t* 大小，无需布局计算） */
    char *name = func_sig_name(vm->alloc, ft->sig.params, ft->sig.param_count,
                               ft->sig.return_type, ft->sig.is_variadic);
    if (!name) panic("vm: out of memory allocating function signature name");
    ft->base.name = (strslice_t){ name, strlen(name) };
    ft->base.sealed = true;
    vec_push(vm->sig_types, vm->alloc, ft);  /* 密封后入池（去重 intern） */
    return &ft->base;
}

const type_t *type_func_sig(vm_t *vm, const type_t *const *params,
                            size_t param_count, const type_t *return_type,
                            bool is_variadic) {
    /* 一次性快捷（func_type_push + add_param* + set_return + set_variadic +
     * seal）；内部走 create_open（不向 vm 栈压入 type value，供 C 侧直接用）。
     * 入池由 func_type_seal 完成。 */
    if (!vm) return NULL;
    func_type_t *ft = func_type_create_open(vm);
    if (param_count > 0 && params) {
        const type_t **pcopy = (const type_t **)allocator_new_ex(
            vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL,
            param_count);
        if (!pcopy) panic("vm: out of memory allocating function signature");
        memcpy(pcopy, params, param_count * sizeof(type_t *));
        ft->sig.params = pcopy;
        ft->sig.param_count = param_count;
    }
    ft->sig.return_type = return_type;
    ft->sig.is_variadic = is_variadic;
    return func_type_seal(vm, &ft->base);
}
