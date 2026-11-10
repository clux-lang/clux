#ifndef _H_PARSER_NODE_SYMBOL_
#define _H_PARSER_NODE_SYMBOL_
#include "core/allocator.h"
#include "parser/node.h"
#include "parser/parser.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 符号节点：运算符、分隔符、关键字等词法单元在 AST 中的载体。
   无子节点，语义完全由 location 指向的源码文本决定。 */
node_t *create_symbol_node(allocator_t *allocator);

/* 读取当前 token 为符号并产出 SYMBOL 节点；不是符号时返回 NULL 且游标不动。 */
node_t *parser_read_symbol(allocator_t *allocator, parser_t *parser);
#ifdef __cplusplus
}
#endif
#endif
