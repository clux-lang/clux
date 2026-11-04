#include "parser/expression.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/literal.h"
#include "parser/node.h"
#include "parser/parser.h"

node_t *parser_read_expression_assign(allocator_t *allocator,
                                      parser_t *parser,
                                      node_t *parent) {
  return NULL;
}
node_t *parser_read_expression(allocator_t *allocator,
                               parser_t *parser,
                               node_t *parent) {
  return parser_read_expression_assign(allocator, parser, parent);
}