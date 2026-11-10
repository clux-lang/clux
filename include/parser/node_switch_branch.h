#ifndef _H_PARSER_NODE_SWITCH_BRANCH_
#define _H_PARSER_NODE_SWITCH_BRANCH_
#include "core/allocator.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 单个 switch 分支 (pattern, ...) -> { body } 或 default -> { body }
   （白皮书 §4.5）。PATTERNS 为模式列表（default 分支为 NULL）；
   BODY 为块语句；容错时均可为 NULL。 */
#define CLUX_SWITCH_BRANCH_PATTERNS 0
#define CLUX_SWITCH_BRANCH_BODY 1
#define CLUX_SWITCH_BRANCH_CHILDREN_SIZE 2

node_t *create_switch_branch_node(allocator_t *allocator);
/* PATTERNS 允许为 NULL（default 分支）。 */
void switch_branch_node_set_patterns(node_t *self, node_t *patterns);
node_t *switch_branch_node_get_patterns(node_t *self);
void switch_branch_node_set_body(node_t *self, node_t *body);
node_t *switch_branch_node_get_body(node_t *self);

/* switch 分支列表，children 为 CLUX_NODE_SWITCH_BRANCH。 */
node_t *create_switch_branches_node(allocator_t *allocator);
void switch_branches_node_add_branch(node_t *self, node_t *branch);
size_t switch_branches_node_get_branch_count(node_t *self);
/* 越界返回 NULL。 */
node_t *switch_branches_node_get_branch(node_t *self, size_t idx);

/* switch 模式列表 (pattern, ...)，children 为模式表达式。 */
node_t *create_switch_patterns_node(allocator_t *allocator);
void switch_patterns_node_add_pattern(node_t *self, node_t *pattern);
size_t switch_patterns_node_get_pattern_count(node_t *self);
/* 越界返回 NULL。 */
node_t *switch_patterns_node_get_pattern(node_t *self, size_t idx);
#ifdef __cplusplus
}
#endif
#endif
