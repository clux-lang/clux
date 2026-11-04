#ifndef _H_PARSER_PARSER_
#define _H_PARSER_PARSER_
#include "core/allocator.h"
#include "core/error.h"
#include "core/vec.h"
#include "parser/lexer.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _parser_t parser_t;
parser_t *
create_parser(allocator_t *allocator, const char *filename, const char *source);
void delete_parser(parser_t **parser);
vec_t *parser_get_tokens(parser_t *self);
vec_t *parser_get_error(parser_t *self);
token_t *parser_read_token(parser_t *self);
token_t *parser_peek_token(parser_t *self);
size_t parser_get_position(parser_t *self);
bool parser_set_position(parser_t *self, size_t size);
void parser_push_error(parser_t *self, clux_error_t *error);
const char *parser_get_filename(parser_t *self);
#ifdef __cplusplus
}
#endif
#endif