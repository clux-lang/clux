#ifndef _H_PARSER_NODE_STMT_EMPTY_
#define _H_PARSER_NODE_STMT_EMPTY_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 空语句 `;`（白皮书 §4.14）。不产生操作，只占一个语句位置。
   叶子节点，无子节点；区间就是 ';' 本身。 */
node_t *create_stmt_empty_node(allocator_t *allocator);
#ifdef __cplusplus
}
#endif
#endif
