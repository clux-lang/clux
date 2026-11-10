#ifndef _H_PARSER_NODE_STRUCT_MEMBER_
#define _H_PARSER_NODE_STRUCT_MEMBER_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* struct 成员列表 { member ... }（白皮书 §3.5）。
   children 按源码顺序为 CLUX_NODE_FIELD、CLUX_NODE_STMT_VAR、
   CLUX_NODE_STMT_FUNC 或 CLUX_NODE_STRUCT_EMBED。 */
node_t *create_struct_members_node(allocator_t *allocator);
void struct_members_node_add_member(node_t *self, node_t *member);
size_t struct_members_node_get_member_count(node_t *self);
/* 越界返回 NULL。 */
node_t *struct_members_node_get_member(node_t *self, size_t idx);

/* 嵌入成员 ...Base;。TYPE 为被嵌入的类型；缺失时为 NULL（容错）。 */
#define CLUX_STRUCT_EMBED_TYPE 0
#define CLUX_STRUCT_EMBED_CHILDREN_SIZE 1

node_t *create_struct_embed_node(allocator_t *allocator);
void struct_embed_node_set_type(node_t *self, node_t *type);
node_t *struct_embed_node_get_type(node_t *self);
#ifdef __cplusplus
}
#endif
#endif
