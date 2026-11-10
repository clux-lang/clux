#ifndef _H_PARSER_NODE_FUNC_DECL_
#define _H_PARSER_NODE_FUNC_DECL_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 函数声明 / 函数定义 / 闭包 func |captures| name[generics](params): ret {body}
   （白皮书 §3.3）。是 func_type 的后退方案：首 token 为 func/cfunc 且
   func_type 不匹配时，必然是声明，任何不匹配都是硬错误。
   KEYWORD 区分 func 与 cfunc；NAME 为函数名（匿名函数与 cfunc 为 NULL）；
   GENERICS 为泛型参数列表（无泛型参数时为 NULL）；
   CAPTURES 为捕获列表（无捕获时为 NULL）；PARAMS 为形参列表；
   RETURN 为返回类型；BODY 为函数体块（cfunc 与匿名函数类型除外）。 */
#define CLUX_EXPR_FUNC_DECL_KEYWORD 0
#define CLUX_EXPR_FUNC_DECL_NAME 1
#define CLUX_EXPR_FUNC_DECL_GENERICS 2
#define CLUX_EXPR_FUNC_DECL_CAPTURES 3
#define CLUX_EXPR_FUNC_DECL_PARAMS 4
#define CLUX_EXPR_FUNC_DECL_RETURN 5
#define CLUX_EXPR_FUNC_DECL_BODY 6
#define CLUX_EXPR_FUNC_DECL_CHILDREN_SIZE 7

node_t *create_func_decl_node(allocator_t *allocator);
void func_decl_node_set_keyword(node_t *self, node_t *keyword);
node_t *func_decl_node_get_keyword(node_t *self);
/* NAME 允许为 NULL（匿名函数 / cfunc）。 */
void func_decl_node_set_name(node_t *self, node_t *name);
node_t *func_decl_node_get_name(node_t *self);
/* GENERICS 允许为 NULL（无泛型参数）。 */
void func_decl_node_set_generics(node_t *self, node_t *generics);
node_t *func_decl_node_get_generics(node_t *self);
/* CAPTURES 允许为 NULL（无捕获）。 */
void func_decl_node_set_captures(node_t *self, node_t *captures);
node_t *func_decl_node_get_captures(node_t *self);
void func_decl_node_set_params(node_t *self, node_t *params);
node_t *func_decl_node_get_params(node_t *self);
/* RETURN / BODY 允许为 NULL（容错：缺返回类型或函数体）。 */
void func_decl_node_set_return(node_t *self, node_t *ret);
node_t *func_decl_node_get_return(node_t *self);
void func_decl_node_set_body(node_t *self, node_t *body);
node_t *func_decl_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
