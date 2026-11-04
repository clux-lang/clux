#include "parser/parser.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/position.h"
#include "core/vec.h"
#include "parser/lexer.h"
struct _parser_t {
  allocator_t *allocator;
  vec_t *tokens;
  vec_t *errors;
  size_t position;
  const char *filename;
};
parser_t *create_parser(allocator_t *allocator,
                        const char *filename,
                        const char *source) {
  vec_t *tokens = create_vec(allocator);
  vec_t *errors = create_vec(allocator);
  parser_t *self = allocator_alloc(allocator, sizeof(struct _parser_t));
  self->allocator = allocator;
  self->position = 0;
  self->tokens = tokens;
  self->errors = errors;
  self->filename = filename;
  position_t position = {
      .column = 0,
      .line = 0,
      .offset = source,
  };
  for (;;) {
    token_t *token = lexer_read_string(allocator, &position, filename);
    if (!token) {
      token = lexer_read_character(allocator, &position, filename);
    }
    if (!token) {
      token = lexer_read_numeric(allocator, &position, filename);
    }
    if (!token) {
      token = lexer_read_comment(allocator, &position, filename);
    }
    if (!token) {
      token = lexer_read_multiline_comment(allocator, &position, filename);
    }
    if (!token) {
      token = lexer_read_whitespace(allocator, &position, filename);
    }
    if (!token) {
      token = lexer_read_identifier(allocator, &position, filename);
    }
    if (!token) {
      token = lexer_read_symbol(allocator, &position, filename);
    }
    if (!token) {
      break;
    }
    if (token_get_kind(token) == CLUX_TOKEN_ERROR) {
      /* lexer 只在两种情况下产出 ERROR：非法 UTF-8、缺失闭合引号。
         根据起始字符区分，报出更具体的消息。 */
      const location_t *loc = token_get_location(token);
      char first = *loc->begin.offset;
      const char *message = (first == '"' || first == '\'')
                                ? "unterminated string or character literal"
                                : "invalid UTF-8 sequence";
      vec_push(errors, create_error(allocator, loc, "%s", message));
      delete_token(&token);
      break;
    }
    vec_push(tokens, token);
  }
  if (*position.offset != '\0') {
    location_t loc = {
        .filename = filename,
        .begin = position,
        .end = position,
    };
    clux_error_t *error = create_error(
        allocator, &loc, "unexpected character '%c'", *position.offset);
    vec_push(errors, error);
  }
  return self;
}
void delete_parser(parser_t **parser) {
  if (!parser || !*parser) {
    return;
  }
  parser_t *self = *parser;
  allocator_t *allocator = self->allocator;
  for (size_t idx = 0; idx < vec_get_size(self->errors); idx++) {
    clux_error_t *err = vec_get(self->errors, idx);
    delete_error(&err);
  }
  delete_vec(&self->errors);
  for (size_t idx = 0; idx < vec_get_size(self->tokens); idx++) {
    token_t *token = vec_get(self->tokens, idx);
    delete_token(&token);
  }
  delete_vec(&self->tokens);
  allocator_free(allocator, (void **)parser);
}
vec_t *parser_get_tokens(parser_t *self) { return self->tokens; }
vec_t *parser_get_error(parser_t *self) { return self->errors; }
token_t *parser_read_token(parser_t *self) {
  if (self->position >= vec_get_size(self->tokens)) {
    return NULL;
  }
  return vec_get(self->tokens, self->position++);
}
token_t *parser_peek_token(parser_t *self) {
  if (self->position >= vec_get_size(self->tokens)) {
    return NULL;
  }
  return vec_get(self->tokens, self->position);
}
size_t parser_get_position(parser_t *self) { return self->position; }
bool parser_set_position(parser_t *self, size_t size) {
  if (size > vec_get_size(self->tokens)) {
    return false;
  }
  self->position = size;
  return true;
}
void parser_push_error(parser_t *self, clux_error_t *error) {
  vec_push(self->errors, error);
}
const char *parser_get_filename(parser_t *self) { return self->filename; }