#ifndef _H_PARSER_NODE_STMT_STATIC_ASSERT_
#define _H_PARSER_NODE_STMT_STATIC_ASSERT_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 编译期断言 static_assert(cond, "msg");（白皮书 §4.16）。
   CONDITION 为条件表达式；MESSAGE 为字符串字面量；
   SOURCE 为条件表达式源码片段（CLUX_NODE_SOURCE_TEXT，location 指向原文）。
   容错时各槽均可为 NULL。 */
#define CLUX_STMT_STATIC_ASSERT_CONDITION 0
#define CLUX_STMT_STATIC_ASSERT_MESSAGE 1
#define CLUX_STMT_STATIC_ASSERT_SOURCE 2
#define CLUX_STMT_STATIC_ASSERT_CHILDREN_SIZE 3

node_t *create_stmt_static_assert_node(allocator_t *allocator);
void stmt_static_assert_node_set_condition(node_t *self, node_t *condition);
node_t *stmt_static_assert_node_get_condition(node_t *self);
void stmt_static_assert_node_set_message(node_t *self, node_t *message);
node_t *stmt_static_assert_node_get_message(node_t *self);
void stmt_static_assert_node_set_source(node_t *self, node_t *source);
node_t *stmt_static_assert_node_get_source(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
