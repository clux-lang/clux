#ifndef _H_CLUX_PARSER_PARSE_STMT_
#define _H_CLUX_PARSER_PARSE_STMT_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/parser.h"
#include "parser/ast_node.h"

/**
 * 语句分派器。根据首 token 分派到对应 parse 函数。
 * 最后回退到 parse_assign_or_expr_stmt。
 */
ast_node_t *parse_stmt(parser_t *p);

/** break; */
ast_node_t *parse_break(parser_t *p);

/** continue; */
ast_node_t *parse_continue(parser_t *p);

/** comptime 前缀语句：comptime var / comptime func。 */
ast_node_t *parse_comptime_stmt(parser_t *p);
/** 解析 `static_assert(cond, "msg");`。 */
ast_node_t *parse_static_assert(parser_t *p);

/** 统一式：表达式 + ; → 语句（赋值已是表达式的一种）。 */
ast_node_t *parse_assign_or_expr_stmt(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif
