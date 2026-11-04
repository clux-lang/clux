#ifndef _H_PARSER_EXPRESSION_
#define _H_PARSER_EXPRESSION_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif
node_t *parser_read_expression_assign(allocator_t *allocator,
                                      parser_t *parser,
                                      node_t *parent);
node_t *parser_read_expression(allocator_t *allocator,
                               parser_t *parser,
                               node_t *parent);
#ifdef __cplusplus
}
#endif
#endif
