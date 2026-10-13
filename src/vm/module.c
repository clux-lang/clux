#include "vm/module.h"
#include "vm/ffi_lib.h"
#include "vm/bcode.h"
#include "vm/scope.h"
#include "vm/vm.h"
#include "core/string.h"
#include <stdlib.h>

static class_t g_module_class = {
    .name = "vm.module",
    .size = sizeof(module_t),
    .move_fn = NULL,
    .clone_fn = NULL,
    .dispose_fn = NULL,
};

module_t *module_new(allocator_t *alloc, bytecode_t *bc,
                     scope_t *global_scope, strmap_t *exports,
                     const char *canonical) {
    if (!alloc || !canonical) return NULL;

    module_t *mod = (module_t *)allocator_new(alloc, &g_module_class, 1);
    if (!mod) return NULL;

    mod->bc = bc;
    mod->global_scope = global_scope;
    mod->exports = exports;
    mod->ffi_libs = NULL;  /* M6 FFI：库表懒创建 */
    mod->import_path = NULL;
    mod->importer = NULL;

    /* 拷贝规范路径（module 拥有副本） */
    size_t len = strlen(canonical) + 1;
    mod->canonical = (char *)allocator_new_ex(alloc, "vm.module.str",
                                               len, NULL, NULL, NULL, 1);
    if (!mod->canonical) {
        allocator_free(alloc, (void **)&mod);
        return NULL;
    }
    memcpy(mod->canonical, canonical, len);

    /* 字节码记录所属模块的规范路径：运行期 `IMPORT <源码路径>` 以它为基准
       重新规范化（借用 module 的 canonical，生命周期同 module）。 */
    if (mod->bc) mod->bc->module_path = mod->canonical;

    return mod;
}

void module_destroy(vm_t *vm, module_t **pmod) {
    if (!pmod || !*pmod) return;
    module_t *mod = *pmod;
    allocator_t *alloc = vm->alloc;
    /* M6 FFI：关闭所有动态库句柄（dlopen/LoadLibrary 资源） */
    if (mod->ffi_libs) {
        const vec_t *fkeys = strmap_keys(mod->ffi_libs);
        size_t fn = fkeys ? vec_len(fkeys) : 0;
        for (size_t fi = 0; fi < fn; fi++) {
            const char *fk = (const char *)vec_get(fkeys, fi);
            ffi_lib_t *fl = (ffi_lib_t *)strmap_get(mod->ffi_libs, fk);
            if (fl) ffi_lib_close(fl);
        }
        strmap_free(alloc, &mod->ffi_libs);
    }

    /* 释放字节码 */
    if (mod->bc) bcode_destroy(&mod->bc);

    /* 释放全局作用域（模块所有函数/var 绑定在此）+ 其子作用域。
       struct member_scope 挂在 global_scope 下（STORE_ATTR 首次写入时创建），
       需 scope_destroy_subtree 递归销毁，否则 member_scope 及其 owned 值泄漏。 */
    if (mod->global_scope) scope_destroy_subtree(vm, &mod->global_scope);

    /* 释放导出表 map（value 借用 global_scope，不释放 value） */
    if (mod->exports) strmap_free(alloc, &mod->exports);

    /* 释放规范路径字符串 */
    if (mod->canonical) {
        allocator_free(alloc, (void **)&mod->canonical);
    }

    /* 释放"源码路径 / 导入者"副本（M5 字节码容器记录用） */
    if (mod->import_path) allocator_free(alloc, (void **)&mod->import_path);
    if (mod->importer) allocator_free(alloc, (void **)&mod->importer);

    allocator_free(alloc, (void **)&mod);
}
