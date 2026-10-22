#ifndef _H_CLUX_PARSER_PARSE_EXPR_
#define _H_CLUX_PARSER_PARSE_EXPR_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/parser.h"
#include "parser/ast_node.h"

/**
 * Pratt parser 入口。解析完整表达式。
 * 不匹配返回 NULL，错误返回 AST_ERROR。
 */
ast_node_t *parse_expr(parser_t *p);

/**
 * Pratt 核心，绑定力驱动。
 * min_prec = 0 时等价于 parse_expr。
 */
ast_node_t *parse_expr_prec(parser_t *p, int min_prec);

/**
 * 解析原子表达式（字面量 / 标识符 / 分组）。
 * 不匹配返回 NULL，错误返回 AST_ERROR。
 */
ast_node_t *parse_primary(parser_t *p);

/**
 * 解析一元前缀表达式（! / ~ / -）。
 * 不匹配返回 NULL（非一元前缀），错误返回 AST_ERROR。
 */
ast_node_t *parse_unary(parser_t *p);

/**
 * 解析一个**类型操作数**：unary 类型 + 可选的 `[args]` 泛型实例下标，
 * **不消费** `&` / `|` 等二元运算（implements 子句用于 `&` 分隔接口）。
 */
ast_node_t *parse_type_with_index(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_PARSE_EXPR_ */
