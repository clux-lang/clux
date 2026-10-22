#include "sema/sema_cross.h"

#include "core/arena.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_interface_def.h"
#include "parser/ast_program.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_type_def.h"
#include "parser/ast_union_def.h"
#include "module/module_path.h"
#include "sema/symbol.h"
#include "parser/ast_reexport.h"
#include "vm/module.h"
#include "vm/vm.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * M7 P7：跨模块泛型定义查找
 *
 * 被导入模块的 AST 与 token pool 由 module_t 保留（src_ast/src_tokens）。
 * 导入方 sema 在当前全局作用域登记一个 qualified 符号 `mod::Name`，ast 指向
 * 定义模块的 AST 节点、generic_tokens 指向定义模块 token pool；实例化随后
 * 在导入方 sema 产生实例（AST 在导入方 arena，mangling 名全局唯一）。
 * ========================================================================= */

static strslice_t cross_decl_name(ast_node_t *n) {
    switch (n->kind) {
    case AST_FUNC_DEF:      return ((ast_func_def_t *)n)->name;
    case AST_STRUCT_DEF:    return ((ast_struct_def_t *)n)->name;
    case AST_ENUM_DEF:      return ((ast_enum_def_t *)n)->name;
    case AST_UNION_DEF:     return ((ast_union_def_t *)n)->name;
    case AST_TYPE_DEF:      return ((ast_type_def_t *)n)->name;
    case AST_INTERFACE_DEF: return ((ast_interface_def_t *)n)->name;
    default:                return STRSLICE_EMPTY;
    }
}

static bool cross_decl_is_generic(ast_node_t *n) {
    switch (n->kind) {
    case AST_FUNC_DEF:      return ((ast_func_def_t *)n)->generic.count > 0;
    case AST_STRUCT_DEF:    return ((ast_struct_def_t *)n)->generic.count > 0;
    case AST_ENUM_DEF:      return ((ast_enum_def_t *)n)->generic.count > 0;
    case AST_UNION_DEF:     return ((ast_union_def_t *)n)->generic.count > 0;
    case AST_TYPE_DEF:      return ((ast_type_def_t *)n)->generic.count > 0;
    case AST_INTERFACE_DEF: return true;
    default:                return false;
    }
}

static sema_symbol_kind_t cross_decl_sym_kind(ast_node_t *n) {
    return n->kind == AST_FUNC_DEF      ? SEMA_SYM_FUNC
           : n->kind == AST_INTERFACE_DEF ? SEMA_SYM_INTERFACE
                                          : SEMA_SYM_TYPE;
}

typedef struct {
    ast_node_t *decl;
    module_t   *owner;
} cross_found_t;

/* 在模块（含 `export * from` 代理链）中查找导出泛型定义。 */
static bool cross_find_in_module(vm_t *vm, allocator_t *alloc, module_t *mod,
                                 strslice_t name, unsigned depth,
                                 cross_found_t *found) {
    if (!mod || depth > 16) return false;
    if (mod->src_ast && mod->src_ast->kind == AST_PROGRAM) {
        ast_program_t *prog = (ast_program_t *)mod->src_ast;
        for (ast_node_t *n = prog->funcs; n; n = n->next) {
            if (!n->is_exported || !cross_decl_is_generic(n)) continue;
            strslice_t nm = cross_decl_name(n);
            if (!strslice_eq(nm, name)) continue;
            found->decl = n;
            found->owner = mod;
            return true;
        }
        for (ast_node_t *n = prog->funcs; n; n = n->next) {
            if (n->kind != AST_REEXPORT) continue;
            ast_reexport_t *re = (ast_reexport_t *)n;
            module_import_kind_t kind = MODULE_IMPORT_MODULE;
            char *dep =
                module_path_resolve(alloc, re->path, mod->canonical, &kind);
            if (!dep) continue;
            module_t *dm = (module_t *)vm_module_lookup(vm, dep);
            allocator_free(alloc, (void **)&dep);
            if (dm && cross_find_in_module(vm, alloc, dm, name, depth + 1,
                                           found))
                return true;
        }
    }
    return false;
}

sema_symbol_t *sema_module_export_symbol(struct module_t *mod,
                                         strslice_t name) {
    if (!mod || !mod->sema_exports || !name.ptr) return NULL;
    char buf[256];
    if (name.len >= sizeof buf) return NULL;
    memcpy(buf, name.ptr, name.len);
    buf[name.len] = '\0';
    return (sema_symbol_t *)strmap_get(mod->sema_exports, buf);
}

sema_symbol_t *sema_cross_generic_symbol(sema_t *sema, ast_node_t *er) {
    if (!sema || !er || er->kind != AST_SCOPE_REF) return NULL;
    ast_scope_ref_t *sr = (ast_scope_ref_t *)er;
    if (!sr->type_expr || sr->type_expr->kind != AST_IDENT) return NULL;
    sema_symbol_t *modsym =
        sema_scope_find_local(sema->global_scope,
                              ((ast_ident_t *)sr->type_expr)->name);
    if (!modsym || modsym->kind != SEMA_SYM_MODULE || !modsym->module)
        return NULL;
    module_t *mod = modsym->module;
    if (!mod) return NULL;

    /* qualified 名：`mod::Name`（内部符号，用户不可直接写）。 */
    char qbuf[512];
    int qn = snprintf(qbuf, sizeof qbuf, "%.*s::%.*s",
                      (int)((ast_ident_t *)sr->type_expr)->name.len,
                      ((ast_ident_t *)sr->type_expr)->name.ptr,
                      (int)sr->variant.len, sr->variant.ptr);
    if (qn <= 0 || (size_t)qn >= sizeof qbuf) return NULL;
    strslice_t qname = strslice_from_cstr(qbuf);
    sema_symbol_t *existing = sema_scope_find_local(sema->global_scope, qname);

    cross_found_t found = {0};
    if (!cross_find_in_module(sema->vm, sema->vm->alloc, mod, sr->variant, 0,
                              &found))
        return NULL;
    ast_node_t *n = found.decl;
    module_t *owner = found.owner;
    if (existing && existing->is_generic) {
        /* pass0/先前解析可能已登记 qname，但没有记录定义模块；补齐归属。 */
        existing->module = owner;
        existing->generic_tokens = owner->src_tokens;
        return existing;
    }
    {
        strslice_t nm = cross_decl_name(n);
        sema_symbol_t init = {0};
        init.kind = cross_decl_sym_kind(n);
        init.ast = n;
        init.is_generic = true;
        init.generic_tokens = owner->src_tokens;
        init.is_active = true;
        init.module = owner; /* 实例归属：定义模块 */
        /* 跨模块 mangling 前缀：与**定义模块内**的实例同前缀
           （owner canonical 的 FNV 哈希），避免同一泛型定义在“导入方实例”
           与“定义模块 body 内引用”之间产生两个不同身份。 */
        char pbuf[512];
        uint32_t h = 2166136261u;
        if (owner->canonical) {
            for (const char *p = owner->canonical; *p; p++) {
                h ^= (uint8_t)*p;
                h *= 16777619u;
            }
        }
        int pn = snprintf(pbuf, sizeof pbuf, "m%08x$%.*s", h, (int)nm.len,
                          nm.ptr);
        if (pn > 0 && (size_t)pn < sizeof pbuf) {
            char *pc = (char *)arena_alloc(sema->arena, (size_t)pn,
                                           ALIGNOF(max_align_t));
            if (pc) {
                memcpy(pc, pbuf, (size_t)pn);
                init.generic_prefix = strslice_from_bytes(pc, (size_t)pn);
            }
        }
        sema_symbol_t *sym = sema_scope_define(sema->global_scope, qname, &init);
        /* 交叉模块实例的**方法签名**会写 `Box[T]`（定义模块内的简单名）：
           若当前全局没有同名符号，额外登记一个简单名别名指向同一泛型定义，
           使方法签名里的自引用能解析回该定义（有同名本地符号时不覆盖）。 */
        if (sym && !sema_scope_find_local(sema->global_scope, sr->variant)) {
            sema_symbol_t alias_init = init;
            sema_scope_define(sema->global_scope, sr->variant, &alias_init);
        }
        return sym;
    }
}
