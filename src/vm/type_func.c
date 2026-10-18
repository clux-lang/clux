#include "vm/type_func.h"
#include "vm/value.h"
#include "vm/value_deepcopy.h" /* value_relabel_into（fatal 形参重贴标签） */
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
    /* 192：最长的诊断（"argument N: cannot copy a value holding owned resources
       into parameter of type <类型名> (use move() or clone(), or declare the
       parameter as 'ref *T' to borrow)"）在类型名较长时约 150 字符。 */
    char msg[192];

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
        /* R1（§7 唯一所有权不可隐式拷贝）：形参声明 own 资源时，实参必须是
           **将亡值（fatal）**——否则实参是普通左值/值，传进被调用方会后与
           调用方**共享**内嵌 own 堆块，被调用方作用域退出时释放 → 调用方拿到
           悬垂指针（实测 access violation）。调用点必须显式 move()/clone() 让
           实参成为 fatal 将亡值；只想借用就写成 `ref *T` 形参。
           **必须在 type_eq 之前判**：`func read(b: Box)` 用 `read(a)`（a 也是 Box）
           时两侧类型完全相同，早前"仅类型不等时才检查"的写法会静默放行 ——
           这正是本缺陷（复合类型跨函数边界）的入口之一。
           本函数是"实参 → 形参"转换的**唯一**判定点：运行期 func_vcall 与 sema
           的 shadow 调用都经过这里，因此规则只此一处。
           所有权纯编译期——形参 ownership 从 sig->param_ownerships[i] 读取，
           实参 fatal 从 value_ownership(args[i]) 读取。 */
        if (i < sig->param_count && sig->params[i] &&
            sig->param_ownerships && sig->param_ownerships[i] == OWN_OWN &&
            value_ownership(args[i]) != OWN_FATAL) {
            char pn[64];
            sig_type_name(sig->params[i], pn, sizeof pn);
            snprintf(msg, sizeof msg,
                     "argument %zu: cannot copy a value holding owned resources "
                     "into parameter of type %s (use move() or clone(), or declare "
                     "the parameter as 'ref *T' to borrow)", i + 1, pn);
            return value_make_error(vm, msg);
        }
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

    /* 4.5 闭包上下文绑定：`__ctx` = 当前被调用的 func 值（含 env 块）。
       函数体里的**捕获读取**已被 sema 改写为 `__ctx.env.<cap>`（捕获聚合结构
       体），运行时与结构体字段访问完全同构 —— 不再有"按名查 closure_scope"
       的隐式捕获读取。 */
    if (fn->capture_type) {
        value_t *ctxv = value_clone(vm, callee);
        if (ctxv && !value_is_error(vm, ctxv))
            scope_define(vm, vm->current_scope, "__ctx", ctxv);
    }

    /* 5. safe_cast + clone 参数到当前作用域（value_clone 自动注册到 owned） */
    value_t *local_args[argc > 0 ? argc : 1];
    value_t *ret = NULL;
    bool is_error = false;

    for (size_t i = 0; i < argc; i++) {
        /* fatal 形参（`fatal *T` / `fatal []T`）：
           所有权**转移**进被调用方，不是拷贝。判定用"**实参持有资源（own）**
           且 形参声明为 fatal 且形参类型是指针/切片"——不要求实参的运行期类型
           本身是 fatal：构造表达式/`move()` 的产物在**语义**上是将亡值，但运行期
           保持 base 类型（所有权纯编译期，运行期不为同一逻辑类型维护两个实例）。
           下方 else 分支对同类型走 value_clone —— 对含 own 字段的复合类型那是一次
           **多余的深拷贝**（白分配一整套对象图），而且调用方实参仍持有原件，
           "转移"就名不副实。这里显式走 value_move：接管资源 + 清空源。
           所有权纯编译期——形参 ownership 从 sig->param_ownerships[i] 读取，
           实参 ownership 从 value_ownership(args[i]) 读取。 */
        if (sig && sig->params && sig->param_ownerships &&
            i < sig->param_count && sig->params[i] &&
            sig->param_ownerships[i] == OWN_FATAL &&
            (type_is_ptr(sig->params[i]) || type_is_slice(sig->params[i])) &&
            value_ownership(args[i]) == OWN_OWN) {
            value_t *moved = value_move(vm, args[i]);
            if (value_is_error(vm, moved)) {
                is_error = true;
                ret = moved;
                break;
            }
            /* 形参声明为 fatal：转移产物必须**是**该 fatal 类型（否则下方
               "同类型"分支会把它当普通值，而 DEFINE 绑参时按声明的 fatal 类型做
               普通→fatal 的转换，那不是合法转换）。base 与 fatal 变体逐字节同
               布局，按形参类型**重贴标签**即可（资源已在 move 内完成转移）。
               不用 value_implicit_cast：那条路径按各族 vtable 分派，对
               array/union 等"同布局不同身份"的形态会落到拒绝分支。 */
            if (value_type(moved) != sig->params[i]) {
                value_t *relabeled = value_relabel_into(vm, moved, sig->params[i]);
                if (relabeled) moved = relabeled;
            }
            local_args[i] = moved;
            continue;
        }
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
            } else if (sig && sig->return_type && !sig->is_variadic &&
                       value_type(result) != sig->return_type &&
                       /* 声明返回类型是 **fatal 且带资源** 时，这里**不** cast——
                          交给下方第 7 步做"转移 + 重贴标签"。原因：base→fatal 不是
                          一个合法的隐式转换方向（各族的 implicit_cast 只认
                          fatal→base 的接管），在此 cast 会直接失败：
                          `fatal [2]I` 返回 `[2]I` 时实测报
                          "array: implicit cast only within layout-compatible
                          array/tuple"。第 7 步的 value_move + value_relabel_into
                          才是这条路径的正解（struct/指针族此前偶然靠 base→fatal
                          的布局兜底通过，array/union 则暴露了问题）。 */
                       /* 声明返回类型是 **fatal 且带资源** 时，这里**不** cast——
                          交给下方第 7 步做"转移 + 重贴标签"。原因：base→fatal 不是
                          一个合法的隐式转换方向（各族的 implicit_cast 只认
                          fatal→base 的接管），在此 cast 会直接失败。
                          所有权纯编译期——return ownership 从 sig->return_ownership 读取。 */
                       !(sig->return_ownership == OWN_FATAL &&
                         (type_is_ptr(sig->return_type) ||
                          type_is_slice(sig->return_type)))) {
                /* safe_cast 返回值到声明的返回类型（auto-tracked 到 callee scope）。
                   variadic 内建函数（如 upgrade）使用类型擦除签名，返回类型
                   由 cfunc 按实参动态构造——跳过 cast，直接借用 cfunc 结果。 */
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

    /* 7. 返回值/error 交回调用方作用域 */
    /* 临时切换 current_scope 到 caller，新值自动 track 到 caller 的 owned。
       fatal 返回类型（返回值携带所有权出函数）：**转移**而不是拷贝——被调用方
       局部（`return move(b)` 的 b）在下方第 8 步随作用域销毁，拷贝会白复制一整套
       对象图；转移把资源直接交给调用方，被调用方局部已 ptr=NULL，安全。
       其余类型照旧 clone（借用视图 materialize 成独立副本）。 */
    scope_t *callee_current = vm->current_scope;
    vm->current_scope = caller_scope;
    if (ret && value_type(ret)) {
        if (sig && sig->return_type &&
            sig->return_ownership == OWN_FATAL &&
            (type_is_ptr(sig->return_type) ||
             type_is_slice(sig->return_type)) &&
            value_ownership(ret) == OWN_OWN) {
            /* 声明返回值带资源：**转移**（与形参接收同款）。产物是 base 类型，
               按声明的 fatal 返回类型重贴标签后再交回调用方。 */
            ret = value_move(vm, ret);
            if (!value_is_error(vm, ret) && value_type(ret) != sig->return_type) {
                value_t *relabeled = value_relabel_into(vm, ret, sig->return_type);
                if (relabeled) ret = relabeled;
            }
        } else {
            ret = value_clone(vm, ret);
        }
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

/* ---- eq/ne：func == func（指针比较；func 无空值，nil 判定走 ?T 窄化） ---- */

/* bool 值构造（与其他 vtable 同模式） */
static value_t *func_bool_store(vm_t *vm, bool val) {
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    *(bool *)data = val;
    return value_make(vm, vm->type_bool, data);
}

static value_t *func_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_error(vm, a)) return a;
    if (value_is_error(vm, b)) return b;
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
            if (ft->sig.param_ownerships) allocator_free(vm->alloc, (void **)&ft->sig.param_ownerships);
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
                            const ownership_t *param_ownerships,
                            size_t param_count, const type_t *return_type,
                            ownership_t return_ownership, bool is_variadic) {
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

        /* 所有权纯编译期——参数 ownership 数组随签名存储 */
        ownership_t *ocopy = (ownership_t *)allocator_new_ex(
            vm->alloc, "ownership_t", sizeof(ownership_t), NULL, NULL, NULL,
            param_count);
        if (!ocopy) panic("vm: out of memory allocating function signature ownership");
        if (param_ownerships) {
            memcpy(ocopy, param_ownerships, param_count * sizeof(ownership_t));
        } else {
            memset(ocopy, 0, param_count * sizeof(ownership_t));
        }
        ft->sig.param_ownerships = ocopy;
    }
    ft->sig.return_type = return_type;
    ft->sig.return_ownership = return_ownership;
    ft->sig.is_variadic = is_variadic;
    return func_type_seal(vm, &ft->base);
}
