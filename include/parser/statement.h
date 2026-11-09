#ifndef _H_PARSER_STMT_
#define _H_PARSER_STMT_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif
node_t *parser_read_stmt_block(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_expr(allocator_t *allocator, parser_t *parser);

/* return; / return expr;（白皮书 §4.11）。
   VALUE 槽为返回表达式，无返回值时为 NULL。 */
#define CLUX_STMT_RETURN_VALUE 0
#define CLUX_STMT_RETURN_CHILDREN_SIZE 1

/* break; / continue;（白皮书 §4.10）。无子节点。 */
node_t *parser_read_stmt_return(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_break(allocator_t *allocator, parser_t *parser);
node_t *parser_read_stmt_continue(allocator_t *allocator, parser_t *parser);

node_t *parser_read_stmt(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
