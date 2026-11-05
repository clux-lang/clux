#ifndef _H_PARSER_EXPRESSION_
#define _H_PARSER_EXPRESSION_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CLUX_EXPR_ASSIGN_CHILDREN_SIZE 3
#define CLUX_EXPR_ASSIGN_ID 0
#define CLUX_EXPR_ASSIGN_SYMBOL 1
#define CLUX_EXPR_ASSIGN_VALUE 2
node_t *parser_read_expression_assign(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
