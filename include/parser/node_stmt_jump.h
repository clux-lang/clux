#ifndef _H_PARSER_NODE_STMT_JUMP_
#define _H_PARSER_NODE_STMT_JUMP_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* break; / continue;（白皮书 §4.10）。两者都是叶子节点，无子节点。
   只作用于当前最近一层循环，该约束属于语义阶段。 */
node_t *create_stmt_break_node(allocator_t *allocator);
node_t *create_stmt_continue_node(allocator_t *allocator);
#ifdef __cplusplus
}
#endif
#endif
