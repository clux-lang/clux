#ifndef _H_PARSER_NODE_STMT_TEST_
#define _H_PARSER_NODE_STMT_TEST_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 测试块 test "Suite.Name" { body }（白皮书 §3.14、§11.1）。
   NAME 为字符串字面量；BODY 为块语句（容错时可为 NULL）。 */
#define CLUX_STMT_TEST_NAME 0
#define CLUX_STMT_TEST_BODY 1
#define CLUX_STMT_TEST_CHILDREN_SIZE 2

node_t *create_stmt_test_node(allocator_t *allocator);
void stmt_test_node_set_name(node_t *self, node_t *name);
node_t *stmt_test_node_get_name(node_t *self);
/* BODY 允许为 NULL（容错：缺测试块体）。 */
void stmt_test_node_set_body(node_t *self, node_t *body);
node_t *stmt_test_node_get_body(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
