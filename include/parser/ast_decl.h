#ifndef _H_CLUX_PARSER_AST_DECL_
#define _H_CLUX_PARSER_AST_DECL_

#include "parser/ast_node.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* M7：声明是否是**泛型定义**（带非空参数表）。
 * 泛型定义不是运行时实体——sema 只登记符号，compiler 的预扫描/名字绑定/
 * 函数体编译都必须跳过它们（实例是 mangling 名的普通声明）。 */
bool ast_decl_is_generic(const ast_node_t *node);

#ifdef __cplusplus
}
#endif
#endif
