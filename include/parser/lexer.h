#ifndef _H_PARSER_LEXER_
#define _H_PARSER_LEXER_
#include "core/allocator.h"
#include "core/location.h"
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
  CLUX_TOKEN_WHITESPACE,
  CLUX_TOKEN_COMMENT,
  CLUX_TOKEN_MULTILINE_COMMENT,
};
typedef enum _token_kind_t token_kind_t;
struct _token_t {
  token_kind_t kind;
  location_t loc;
};
typedef struct _token_t token_t;
token_t *lexer_read_character(allocator_t *allocator,
                              position_t *position,
                              const char *filename);
#ifdef __cplusplus
}
#endif
#endif