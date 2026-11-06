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
node_t *parser_read_stmt(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
