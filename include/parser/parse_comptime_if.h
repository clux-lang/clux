#ifndef _H_CLUX_PARSER_PARSE_COMPTIME_IF_
#define _H_CLUX_PARSER_PARSE_COMPTIME_IF_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/ast_node.h"
#include "parser/parser.h"

/** 解析 `comptime if (cond) { ... } [else ...]`（`comptime` 已消费）。 */
ast_node_t *parse_comptime_if(parser_t *p);

#ifdef __cplusplus
}
#endif
#endif