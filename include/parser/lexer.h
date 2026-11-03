#ifndef _H_PARSER_LEXER_
#define _H_PARSER_LEXER_
#include "core/location.h"
#ifdef __cplusplus
extern "C" {
#endif
enum _token_kind_t {
  CLUX_TOKEN_CHARACTER,
  CLUX_TOKEN_STRING,
  CLUX_TOKEN_NUMERIC,
  CLUX_TOKEN_IDENTIFIER,
  CLUX_TOKEN_SYMBOL,
  CLUX_TOKEN_KEYWORD,
};
typedef enum _token_kind_t token_kind_t;
struct _token_t {
  token_kind_t kind;
  location_t loc;
};
typedef struct _token_t token_t;
#ifdef __cplusplus
}
#endif
#endif