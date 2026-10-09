#include "vm/vm.h"
#include "vm/value.h"
#include "vm/function.h"
#include "vm/type.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_slice.h"
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
    const type_t *st = value_type(sv);

    /* shadow：只算类型——len 恒返回 u64 */
    if (value_is_shadow(sv))
        return value_make_shadow(vm, vm->type_u64);

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
    case TYPE_KIND_SLICE_OWN:
    case TYPE_KIND_SLICE_REF:
    case TYPE_KIND_SLICE_FATAL: {
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

    void *data = value_alloc_data(vm->alloc, vm->type_u64);
    *(uint64_t *)data = len;
    return value_make(vm, vm->type_u64, data);
}

/* 注册 len 到 vm->global_scope（调用方在 vm_new 内调用）。
   签名：len(s) -> u64（泛型 s，variadic）。
   由于 s 是泛型参数，注册为 variadic（0 个固定参数 + variadic），参数类型
   不经签名校验（cfunc 内部自验可长性）。返回类型 u64。 */
void vm_register_len(vm_t *vm) {
    if (!vm || !vm->global_scope) return;

    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_u64,
                                      /*is_variadic=*/true);
    if (!sig) return;

    value_t *fnv = func_new(vm, builtin_len_cfunc, NULL, vm->root_scope,
                            sig, STRSLICE_LIT("len"));
    if (!fnv) return;

    value_t *stored = scope_define(vm, vm->global_scope, "len", fnv);
    if (!stored || value_is_error(vm, stored)) {
        value_dispose(vm, fnv);
        allocator_free(vm->alloc, (void **)&fnv);
        return;
    }
    value_dispose(vm, fnv);
    allocator_free(vm->alloc, (void **)&fnv);
}
