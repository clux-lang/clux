#ifndef _H_PARSER_EXPR_INTERNAL_
#define _H_PARSER_EXPR_INTERNAL_
#include "core/allocator.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 表达式解析各文件共享的内部辅助。不对外暴露，仅供 src/parser 内部使用。 */

/* 用首尾子节点的位置拼出表达式自身的区间。 */
void node_set_location_span(node_t *node,
                            const location_t *begin,
                            const location_t *end);

/* 错误位置：优先指向当前未消费的 token；已到流末尾时退化为 fallback 的终点。 */
location_t expression_error_location(parser_t *parser,
                                     const location_t *fallback);

/* 取当前 token 之后第 offset 个 token（不移动游标）。 */
token_t *peek_token_at(parser_t *parser, size_t offset);

/* 类型前缀层：指针、类型修饰、切片、数组、元组、optional。 */
node_t *parser_read_expr_type(allocator_t *allocator, parser_t *parser);

/* 函数类型 func |captures|(params) -> ret。 */
node_t *parser_read_expr_func_type(allocator_t *allocator, parser_t *parser);

/* 函数声明 / 定义 / 闭包。 */
node_t *parser_read_expr_func_decl(allocator_t *allocator, parser_t *parser);

/* 泛型参数列表 [T, U, ...]，函数声明与 union 定义共用。 */
node_t *parser_read_func_decl_generics(allocator_t *allocator,
                                       parser_t *parser);

/* union 定义（tag union 与 cunion）。 */
node_t *parser_read_expr_union(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
