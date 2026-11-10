#ifndef _H_PARSER_NODE_FUNC_CAPTURE_
#define _H_PARSER_NODE_FUNC_CAPTURE_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 单个捕获项：NAME 必有；TYPE 缺失时为 NULL（容错）。
   捕获列表 |name: type, ...|（空列表写作 ||）是 CLUX_NODE_FUNC_CAPTURES，
   children 为 CLUX_NODE_FUNC_CAPTURE。 */
#define CLUX_FUNC_CAPTURE_NAME 0
#define CLUX_FUNC_CAPTURE_TYPE 1
#define CLUX_FUNC_CAPTURE_CHILDREN_SIZE 2

node_t *create_func_capture_node(allocator_t *allocator);
void func_capture_node_set_name(node_t *self, node_t *name);
node_t *func_capture_node_get_name(node_t *self);
/* TYPE 允许为 NULL（容错）。 */
void func_capture_node_set_type(node_t *self, node_t *type);
node_t *func_capture_node_get_type(node_t *self);

/* 捕获列表，children 为 CLUX_NODE_FUNC_CAPTURE（函数类型）或
   一个 CLUX_NODE_FUNC_PARAMS（函数声明）。 */
node_t *create_func_captures_node(allocator_t *allocator);
void func_captures_node_add_capture(node_t *self, node_t *capture);
size_t func_captures_node_get_capture_count(node_t *self);
/* 越界返回 NULL。 */
node_t *func_captures_node_get_capture(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
