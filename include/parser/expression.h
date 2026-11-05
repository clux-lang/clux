#ifndef _H_PARSER_EXPRESSION_
#define _H_PARSER_EXPRESSION_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

#define CLUX_EXPR_BINARY_CHILDREN_SIZE 3
#define CLUX_EXPR_BINARY_LEFT 0
#define CLUX_EXPR_BINARY_SYMBOL 1
#define CLUX_EXPR_BINARY_RIGHT 2
node_t *parser_read_expression_lor(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_land(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_or(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_xor(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_and(allocator_t *allocator, parser_t *parser);
node_t *parser_read_expression_equality(allocator_t *allocator,
                                        parser_t *parser);
node_t *parser_read_expression_relational(allocator_t *allocator,
                                          parser_t *parser);

#define CLUX_EXPR_TERNARY_CHILDREN_SIZE 3
#define CLUX_EXPR_TERNARY_CONDITION 0
#define CLUX_EXPR_TERNARY_THEN 1
#define CLUX_EXPR_TERNARY_ELSE 2
node_t *parser_read_expression_ternary(allocator_t *allocator,
                                       parser_t *parser);
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
