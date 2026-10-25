#include "vm/module.h"
#include "parser/lexer.h"
#include "core/vec.h"
#include "vm/scope.h"
#include "vm/vm.h"
#include "sema/symbol.h"
#include "sema/sema.h"
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
    mod->ffi_libs = NULL;  /* 旧 FFI 库表字段（执行器已移除） */
    mod->import_path = NULL;
    mod->importer = NULL;
    mod->src_lexer = NULL;
    mod->src_tokens = NULL;
    mod->src_ast = NULL;
    mod->sema_global = NULL;
    mod->sema = NULL;
    mod->prepared = false;
    mod->finalized = false;
    mod->sema_exports = NULL;
    mod->test_cases = NULL;

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

    /* 字节码/FFI 库表已随 VM 执行器移除；相关字段保留兼容，始终为空。 */

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

    /* M7 跨模块泛型：释放保留的源码缓冲区 / token pool */
    if (mod->src_lexer) lexer_close(&mod->src_lexer);
    if (mod->src_tokens) vec_free(alloc, (vec_t **)&mod->src_tokens);
    if (mod->sema) sema_destroy(&mod->sema);
    if (mod->sema_exports) strmap_free(alloc, &mod->sema_exports);
    if (mod->sema_global) sema_scope_destroy(&mod->sema_global);
    if (mod->test_cases) {
        size_t n = vec_len(mod->test_cases);
        for (size_t i = 0; i < n; i++) {
            module_test_case_t *tc = (module_test_case_t *)vec_get(mod->test_cases, i);
            if (!tc) continue;
            if (tc->display_name) allocator_free(alloc, (void **)&tc->display_name);
            if (tc->func_name) allocator_free(alloc, (void **)&tc->func_name);
            allocator_free(alloc, (void **)&tc);
        }
        vec_free(alloc, &mod->test_cases);
    }

    allocator_free(alloc, (void **)&mod);
}
