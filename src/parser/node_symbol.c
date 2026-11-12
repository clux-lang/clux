#include "parser/node_symbol.h"
#include "parser/lexer.h"
#include "parser/node.h"

node_t *create_symbol_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_SYMBOL);
}

node_t *parser_read_symbol(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_SYMBOL) {
    return NULL;
  }
  parser_next(parser);
  node_t *node = create_symbol_node(allocator);
  node_set_location(node, token_get_location(token));
  return node;
}

void symbol_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "Symbol");
  node_json_text(writer, self);
  node_json_end(writer);
}
