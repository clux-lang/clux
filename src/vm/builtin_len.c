#include "vm/vm.h"
#include "vm/value.h"
#include "vm/function.h"
#include "vm/type.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_slice.h"
#include "vm/type_ptr.h"
#include "vm/type_option.h"
#include "core/panic.h"

#include <string.h>

/* ===========================================================================
 * len 内置函数（m4-design §5/§11.3）
 *
 * 签名：len(s) -> u64
 *
 * 返回序列的元素数量：
 *   - 数组 [N]T：编译期常量 N（从类型读取）
 *   - 切片 own/ref/fatal []T：运行期读胖指针 len 字段
 *   - str：字节长度（intern 池字符串长度）
 *
 * 泛型参数：len 接受任意可长度的值，注册为 variadic（0 固定参数），
 * sema 调用点特判从实参类型校验可长性。
 * =========================================================================== */

value_t *builtin_len_cfunc(vm_t *vm, func_t *self, size_t argc,
                           value_t **args) {
    (void)self;
    if (!vm || argc < 1 || !args || !args[0])
        return value_make_error(vm, "len: expects 1 argument");

    value_t *sv = args[0];
    /* 槽位类型可能带所有权修饰（own []T 等）：长度判定看裸类型。 */
    const type_t *st = type_strip_quals(value_type(sv));

    /* shadow：只算类型——len 恒返回 usize（C 的 size_t，随架构变化） */
    if (value_is_shadow(sv))
        return value_make_shadow(vm, vm_type_usize(vm));

    uint64_t len = 0;
    if (!st) return value_make_error(vm, "len: argument has no type");

    switch (st->kind) {
    case TYPE_KIND_ARRAY: {
        /* 数组：编译期常量 N（从类型读取，SIZE_MAX = 不定长数组） */
        size_t alen = array_type_len(st);
        if (alen == SIZE_MAX)
            return value_make_error(vm, "len: unsized array has no length");
        len = (uint64_t)alen;
        break;
    }
    case TYPE_KIND_SLICE: {
        /* 切片：运行期读胖指针 len 字段 */
        const slice_value_t *sval = (const slice_value_t *)value_data(sv);
        len = sval->len;
        break;
    }
    case TYPE_KIND_STR: {
        /* str：字节长度（intern 池字符串，NUL 结尾，data 存 const char*） */
        const char *s = *(const char *const *)value_data(sv);
        len = s ? (uint64_t)strlen(s) : 0;
        break;
    }
    default:
        return value_make_error(vm, "len: argument is not a sequence "
                                     "(array/slice/str)");
    }

    void *data = value_alloc_data(vm->alloc, vm_type_usize(vm));
    if (vm_type_usize(vm)->size == 4) *(uint32_t *)data = (uint32_t)len;
    else                              *(uint64_t *)data = len;
    return value_make(vm, vm_type_usize(vm), data);
}

/* 注册 len 到 vm->global_scope（调用方在 vm_new 内调用）。
   签名：len(s) -> usize（泛型 s，variadic；usize = C 的 size_t，独立类型）。
   由于 s 是泛型参数，注册为 variadic（0 个固定参数 + variadic），参数类型
   不经签名校验（cfunc 内部自验可长性）。返回类型 usize。 */
void vm_register_len(vm_t *vm) {
    if (!vm || !vm->global_scope) return;

    const type_t *sig = type_func_sig(vm, NULL, 0, vm_type_usize(vm),
                                      /*is_variadic=*/true);
    if (!sig) return;

    value_t *fnv = func_new(vm, builtin_len_cfunc, NULL, vm->root_scope,
                            sig, STRSLICE_LIT("len"));
    if (!fnv) return;

    value_t *stored = scope_define(vm, vm->global_scope, "len", fnv);
    if (!stored || value_is_error(vm, stored)) {
        return;
    }
}

/* __deep_clone(v)：运行期**深拷贝**一个值（对象图克隆，与已删除的 CLONE
   指令同一实现：ptr_clone_block）。用途收窄为"静态类型表达不了克隆语义"的
   形态——闭包值（env 的捕获聚合类型只有运行期才知道）与 cunion 被指向值。
   普通复合类型/指针/切片的 clone 仍由 sema 生成的 clone 函数完成。 */
static value_t *builtin_deep_clone_cfunc(vm_t *vm, func_t *self, size_t argc,
                                         value_t **args) {
    (void)self;
    if (argc != 1 || !args[0])
        return value_make_error(vm, "__deep_clone: expects exactly 1 argument");
    value_t *v = args[0];
    if (value_is_shadow(v)) return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) {
        void *data = t ? value_alloc_data_copy(vm->alloc, t, value_data(v)) : NULL;
        return t ? value_make(vm, t, data) : v;
    }
    void *nb = ptr_clone_block(vm, t, value_data(v));
    return value_make(vm, t, nb);
}

void vm_register_deep_clone(vm_t *vm) {
    if (!vm || !vm->global_scope) return;
    /* 变参签名：sema 在调用点按实参类型精化返回类型（与 len/data 同款）。 */
    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_opaque,
                                      /*is_variadic=*/true);
    if (!sig) return;
    value_t *fnv = func_new(vm, builtin_deep_clone_cfunc, NULL, vm->root_scope,
                            sig, STRSLICE_LIT("__deep_clone"));
    if (!fnv) return;
    (void)scope_define(vm, vm->global_scope, "__deep_clone", fnv);
}

/* ---- RC 系内置（m3-design §12，2026-10 重新引入）----
   share/weak 是**真实类型**（`share *T` / `weak *T`），不是所有权修饰：
   - `__make_share(p)`：fatal/own `*T` → `share *T`（RC 创建通道，§12.1）；
   - `__weak_get(w)`  ：`weak *T` → `?share *T`（原子升级，§12.4；死对象得 nil）。
   两者都是 sema 生成的调用（源码不写这些名字——`__` 前缀是保留空间），
   因此不需要新指令，也不污染全局命名。 */
static value_t *builtin_make_share_cfunc(vm_t *vm, func_t *self, size_t argc,
                                         value_t **args) {
    (void)self;
    if (argc != 1 || !args[0])
        return value_make_error(vm, "__make_share: expects exactly 1 argument");
    value_t *v = args[0];
    if (value_is_shadow(v)) {
        const type_t *t = value_type(v);
        const type_t *base = t ? ((const ptr_type_t *)t)->base_type : NULL;
        const type_t *st = base ? type_ptr_intern_rc(vm, base, 1) : NULL;
        return st ? value_make_shadow(vm, st) : v;
    }
    value_t *r = ptr_make_share_from_fatal(vm, v);
    return r ? r : value_make_error(vm, "__make_share: expects a pointer value");
}

static value_t *builtin_weak_get_cfunc(vm_t *vm, func_t *self, size_t argc,
                                       value_t **args) {
    (void)self;
    if (argc != 1 || !args[0])
        return value_make_error(vm, "__weak_get: expects exactly 1 argument");
    value_t *v = args[0];
    const type_t *t = value_type(v);
    if (!type_is_weak(t))
        return value_make_error(vm, "__weak_get: expects a weak pointer");
    const type_t *base = ptr_type_base(t);
    const type_t *st = base ? type_ptr_intern_rc(vm, base, 1) : NULL;
    if (!st) return value_make_error(vm, "__weak_get: bad weak pointer type");
    const type_t *opt = type_option_intern(vm, st);
    if (!opt) return value_make_error(vm, "__weak_get: cannot build option type");
    if (value_is_shadow(v)) return value_make_shadow(vm, opt);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
    /* 原子升级：strong>0 → strong+1（这份计数直接归 option 里的 share） */
    void *payload = pv->ptr ? rc_block_upgrade(rc_block_of(base, pv->ptr)) : NULL;
    void *data = value_alloc_data(vm->alloc, opt);
    if (!data) return value_make_error(vm, "__weak_get: out of memory");
    memset(data, 0, opt->size);
    if (payload) {
        *(bool *)data = true; /* some */
        memcpy((uint8_t *)data + option_value_offset(opt), &payload,
               sizeof(payload));
    }
    return value_make(vm, opt, data);
}

void vm_register_rc_builtins(vm_t *vm) {
    if (!vm || !vm->global_scope) return;
    const type_t *sig1 = type_func_sig(vm, NULL, 0, vm->type_opaque,
                                       /*is_variadic=*/true);
    if (sig1) {
        value_t *fnv = func_new(vm, builtin_make_share_cfunc, NULL,
                                vm->root_scope, sig1, STRSLICE_LIT("__make_share"));
        if (fnv) (void)scope_define(vm, vm->global_scope, "__make_share", fnv);
        value_t *gw = func_new(vm, builtin_weak_get_cfunc, NULL,
                               vm->root_scope, sig1, STRSLICE_LIT("__weak_get"));
        if (gw) (void)scope_define(vm, vm->global_scope, "__weak_get", gw);
    }
}
