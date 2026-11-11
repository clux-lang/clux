#include "parser/program.h"
#include "core/error.h"
#include "core/location.h"
#include "parser/node_program.h"
#include "parser/statement.h"

/* 错误位置：指向当前未消费的 token。 */
static location_t program_error_location(parser_t *parser, token_t *token) {
  location_t loc = *token_get_location(token);
  loc.filename = parser_get_filename(parser);
  return loc;
}

node_t *parser_read_program(allocator_t *allocator, parser_t *parser) {
  node_t *node = create_program_node(allocator);

  parser_skip_trivia(parser);
  token_t *first = parser_peek_token(parser);
  location_t begin_loc;
  if (first) {
    begin_loc = *token_get_location(first);
  } else {
    begin_loc = (location_t){.filename = parser_get_filename(parser)};
  }
  const location_t *end_loc = &begin_loc;
  node_set_location(node, &begin_loc);

  for (;;) {
    parser_skip_trivia(parser);
    token_t *token = parser_peek_token(parser);
    if (!token) {
      break;
    }
    size_t before = parser_get_position(parser);
    node_t *stmt = parser_read_stmt(allocator, parser);
    if (!stmt) {
      location_t loc = program_error_location(parser, token);
      parser_push_error(
          parser,
          create_error(allocator, &loc, "expected statement in program"));
      parser_next(parser);
      continue;
    }
    program_node_add_stmt(node, stmt);
    end_loc = node_get_location(stmt);
    if (parser_get_position(parser) == before) {
      /* 防御性保证：错误节点可能不消费 token，主动步进避免死循环。 */
      location_t loc = program_error_location(parser, token);
      parser_push_error(
          parser,
          create_error(allocator,
                       &loc,
                       "parser made no progress while reading program"));
      parser_next(parser);
    }
  }

  location_t loc = begin_loc;
  loc.end = end_loc->end;
  loc.filename = parser_get_filename(parser);
  node_set_location(node, &loc);
  return node;
}
