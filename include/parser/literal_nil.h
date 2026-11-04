#ifndef _H_PARSER_LITERAL_NIL_
#define _H_PARSER_LITERAL_NIL_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif
node_t *parser_read_literal_nil(allocator_t *allocator,
                                parser_t *parser,
                                node_t *parent);
#ifdef __cplusplus
}
#endif
#endif