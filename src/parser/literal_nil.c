#include "parser/literal_nil.h"
#include "core/location.h"
#include "parser/lexer.h"
#include "parser/node.h"
#include "parser/parser.h"

node_t *parser_read_literal_nil(allocator_t *allocator,
                                parser_t *parser,
                                node_t *parent) {
  token_t *token = parser_peek_token(parser);
  if (!token) {
    return NULL;
  }
  if (token_is(token, CLUX_TOKEN_KEYWORD, "nil")) {
    location_t loc = *token_get_location(token);
    parser_next(parser); /* 消费该 token，避免调用方重复匹配 */
    return create_node(allocator, CLUX_NODE_LITERAL_NIL, parent, loc);
  }
  return NULL;
}