#ifndef _H_CLUX_PARSER_PARSE_FOREACH_
#define _H_CLUX_PARSER_PARSE_FOREACH_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/** 解析 `foreach ([var] name [: T] of expr) { body }`（`foreach` 已消费）。 */
ast_node_t *parse_foreach(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif