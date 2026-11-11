#ifndef _H_PARSER_NODE_STMT_ASSERT_
#define _H_PARSER_NODE_STMT_ASSERT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 测试断言 assert(expr);（白皮书 §11.3）。
   CONDITION 为断言表达式；SOURCE 为表达式源码片段
   （CLUX_NODE_SOURCE_TEXT，location 指向原文），失败诊断直接引用。
   容错时各槽均可为 NULL。 */
#define CLUX_STMT_ASSERT_CONDITION 0
#define CLUX_STMT_ASSERT_SOURCE 1
#define CLUX_STMT_ASSERT_CHILDREN_SIZE 2

node_t *create_stmt_assert_node(allocator_t *allocator);
void stmt_assert_node_set_condition(node_t *self, node_t *condition);
node_t *stmt_assert_node_get_condition(node_t *self);
void stmt_assert_node_set_source(node_t *self, node_t *source);
node_t *stmt_assert_node_get_source(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
