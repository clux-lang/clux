#ifndef _H_PARSER_LEXER_
#define _H_PARSER_LEXER_
#include "core/allocator.h"
#include "core/location.h"
#include "core/position.h"
#ifdef __cplusplus
extern "C" {
#endif
enum _token_kind_t {
  CLUX_TOKEN_ERROR,
  CLUX_TOKEN_CHARACTER,
  CLUX_TOKEN_STRING,
  CLUX_TOKEN_NUMERIC,
  CLUX_TOKEN_IDENTIFIER,
  CLUX_TOKEN_SYMBOL,
  CLUX_TOKEN_KEYWORD,
  /* 编译器内建 `#name`，location 覆盖包括 '#' 在内的完整文本。 */
  CLUX_TOKEN_BUILTIN,
  CLUX_TOKEN_WHITESPACE,
  CLUX_TOKEN_COMMENT,
  CLUX_TOKEN_MULTILINE_COMMENT,
};
typedef enum _token_kind_t token_kind_t;
typedef struct _token_t token_t;
token_t *
create_token(allocator_t *allocator, token_kind_t kind, location_t loc);
void delete_token(token_t **token);
token_kind_t token_get_kind(token_t *self);
const location_t *token_get_location(token_t *self);
bool token_is(token_t *self, token_kind_t kind, const char *src);
token_t *lexer_read_character(allocator_t *allocator,
                              position_t *position,
                              const char *filename);
token_t *lexer_read_string(allocator_t *allocator,
                           position_t *position,
                           const char *filename);
token_t *lexer_read_identifier(allocator_t *allocator,
                               position_t *position,
                               const char *filename);
/* 编译器内建 `#name`。'#' 后必须紧跟标识符起始字符，否则返回 NULL。 */
token_t *lexer_read_builtin(allocator_t *allocator,
                            position_t *position,
                            const char *filename);
token_t *lexer_read_whitespace(allocator_t *allocator,
                               position_t *position,
                               const char *filename);
token_t *lexer_read_comment(allocator_t *allocator,
                            position_t *position,
                            const char *filename);
token_t *lexer_read_multiline_comment(allocator_t *allocator,
                                      position_t *position,
                                      const char *filename);
token_t *lexer_read_numeric(allocator_t *allocator,
                            position_t *position,
                            const char *filename);
token_t *lexer_read_symbol(allocator_t *allocator,
                           position_t *position,
                           const char *filename);
#ifdef __cplusplus
}
#endif
#endif