#include "vm/vm.h"
#include "vm/value.h"
#include "vm/function.h"
#include "vm/type.h"
#include "vm/type_func.h"
#include "vm/type_ptr.h"
#include "vm/type_option.h"
#include "core/strslice.h"
#include "core/panic.h"

#include <string.h>

/* ===========================================================================
 * upgrade 内置函数（m3-design §12.4）
 *
 * 签名：upgrade(weak *T) -> ?share *T
 *
 * weak 自身不能直接访问对象——访问必须先升级为 share。检查强引用是否存活：
 *   存活 → 返回 some share（计数 +1）
 *   已归零 → 返回 none（对象已析构）
 *
 * 升级使用 CAS 原子操作（§12.5 线程安全）：检查 strong>0 且递增必须是
 * 同一个原子操作，防止销毁竞态。
 * =========================================================================== */

value_t *builtin_upgrade_cfunc(vm_t *vm, func_t *self, size_t argc,
                               value_t **args) {
    (void)self;
    if (!vm || argc < 1 || !args || !args[0])
        return value_make_error(vm, "upgrade: expects 1 argument");

    value_t *wv = args[0];
    const type_t *wt = value_type(wv);
    if (!wt || wt->kind != TYPE_KIND_PTR_WEAK)
        return value_make_error(vm, "upgrade: argument must be weak pointer");

    /* shadow：只算类型——返回 ?share *T 的 shadow */
    if (value_is_shadow(wv)) {
        const type_t *share_t = type_ptr_intern(vm, TYPE_KIND_PTR_SHARE,
                                                ptr_type_base(wt));
        if (!share_t) return value_make_error(vm, "upgrade: cannot make share type");
        const type_t *opt_t = type_option_intern(vm, share_t);
        if (!opt_t) return value_make_error(vm, "upgrade: cannot make option type");
        return value_make_shadow(vm, opt_t);
    }

    /* 构造返回类型 ?share *T */
    const type_t *share_t = type_ptr_intern(vm, TYPE_KIND_PTR_SHARE,
                                            ptr_type_base(wt));
    if (!share_t) return value_make_error(vm, "upgrade: cannot make share type");
    const type_t *opt_t = type_option_intern(vm, share_t);
    if (!opt_t) return value_make_error(vm, "upgrade: cannot make option type");

    const ptr_value_t *pv = (const ptr_value_t *)value_data(wv);
    if (!pv->ptr) {
        /* weak 指向 NULL：返回 none */
        void *data = value_alloc_data(vm->alloc, opt_t);
        *(bool *)data = false;  /* ok = false → none */
        return value_make(vm, opt_t, data);
    }

    rc_block_t *rc = (rc_block_t *)pv->ptr;
    void *payload = rc_block_upgrade(rc);
    if (!payload) {
        /* strong 已归零：对象已析构，返回 none */
        void *data = value_alloc_data(vm->alloc, opt_t);
        *(bool *)data = false;  /* ok = false → none */
        return value_make(vm, opt_t, data);
    }

    /* 升级成功：构造 ?share *T = some(share)。
       share value 的 data 是 ptr_value_t { void *ptr; }，ptr 指向 RC 块
      （与 weak 同一 RC 块，strong 已 +1）。直接写入 option 的 value 字段。 */
    void *data = value_alloc_data(vm->alloc, opt_t);
    *(bool *)data = true;   /* ok = true → some */
    ptr_value_t *slot = (ptr_value_t *)option_value(data, opt_t);
    slot->ptr = rc;         /* share 指向同一 RC 块 */
    return value_make(vm, opt_t, data);
}

/* 注册 upgrade 到 vm->global_scope（调用方在 vm_new 内调用）。
   签名：upgrade(weak *T) -> ?share *T（泛型 T）。
   由于 T 是泛型参数，注册为 variadic（0 个固定参数 + variadic），参数类型
   不经签名校验（cfunc 内部自验 weak *T）。返回类型用 ?share *opaque 占位；
   sema 层在调用点从实参 weak *T 推断真实返回类型 ?share *T。 */
void vm_register_upgrade(vm_t *vm) {
    if (!vm || !vm->global_scope) return;

    /* ?share *opaque 作为占位返回类型（sema 调用点特判替换为 ?share *T） */
    const type_t *share_opaque = type_ptr_intern(vm, TYPE_KIND_PTR_SHARE,
                                                 vm->type_opaque);
    if (!share_opaque) return;
    const type_t *opt_share = type_option_intern(vm, share_opaque);
    if (!opt_share) return;

    /* 0 个固定参数 + variadic：接受任意参数，cfunc 内部校验 */
    const type_t *sig = type_func_sig(vm, NULL, 0, opt_share,
                                      /*is_variadic=*/true);
    if (!sig) return;

    value_t *fnv = func_new(vm, builtin_upgrade_cfunc, NULL, vm->root_scope,
                            sig, STRSLICE_LIT("upgrade"));
    if (!fnv) return;

    value_t *stored = scope_define(vm, vm->global_scope, "upgrade", fnv);
    if (!stored || value_is_error(vm, stored)) {
        value_dispose(vm, fnv);
        allocator_free(vm->alloc, (void **)&fnv);
        return;
    }
    value_dispose(vm, fnv);
    allocator_free(vm->alloc, (void **)&fnv);
}
