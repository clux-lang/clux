#ifndef _H_CLUX_SEMA_SEMA_CROSS_
#define _H_CLUX_SEMA_SEMA_CROSS_

#include "parser/ast_node.h"
#include "sema/sema.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * M7 P7：解析 `mod::Name` 指向的被导入模块**导出泛型定义**，在当前 sema
 * 注册/复用 qualified 符号（`mod::Name`，is_generic=true，
 * generic_tokens=定义模块 token pool）。找不到（非模块/非导出/非泛型）返回
 * NULL 且不诊断——调用方按普通跨模块路径回退或自行报错。
 *
 * er 必须是 AST_SCOPE_REF（type_expr = 模块别名 IDENT，variant = 名字）。
 */
sema_symbol_t *sema_cross_generic_symbol(sema_t *sema, ast_node_t *er);

/** 模块阶段 1 导出符号（sema_exports）：name → 符号（借用）。
 * 模块尚未 finalize 时，跨模块 sema 解析用它代替 runtime exports。 */
sema_symbol_t *sema_module_export_symbol(struct module_t *mod,
                                         strslice_t name);

#ifdef __cplusplus
}
#endif
#endif
