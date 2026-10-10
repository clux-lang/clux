#include "vm/module.h"
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

static class_t g_module_str_class = {
    .name = "vm.module.str",
    .size = sizeof(char),
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

    /* 拷贝规范路径（module 拥有副本） */
    size_t len = strlen(canonical) + 1;
    mod->canonical = (char *)allocator_new_ex(alloc, "vm.module.str",
                                               len, NULL, NULL, NULL, 1);
    if (!mod->canonical) {
        allocator_free(alloc, (void **)&mod);
        return NULL;
    }
    memcpy(mod->canonical, canonical, len);

    return mod;
}

void module_destroy(vm_t *vm, module_t **pmod) {
    if (!pmod || !*pmod) return;
    module_t *mod = *pmod;
    allocator_t *alloc = vm->alloc;

    /* 释放字节码 */
    if (mod->bc) bcode_destroy(&mod->bc);

    /* 释放全局作用域（模块所有函数/var 绑定在此） */
    if (mod->global_scope) scope_destroy(vm, &mod->global_scope);

    /* 释放导出表 map（value 借用 global_scope，不释放 value） */
    if (mod->exports) strmap_free(alloc, &mod->exports);

    /* 释放规范路径字符串 */
    if (mod->canonical) {
        allocator_free(alloc, (void **)&mod->canonical);
    }

    allocator_free(alloc, (void **)&mod);
}
