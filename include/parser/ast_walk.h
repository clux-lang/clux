#ifndef _H_CLUX_PARSER_AST_WALK_
#define _H_CLUX_PARSER_AST_WALK_

#include "parser/ast_node.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 通用 AST 子节点遍历（表达式 / 类型表达式层）
 *
 * "哪些字段是子节点"是 AST 的结构事实。各阶段过去各自手写递归 walker，
 * 同一事实被重复描述多遍——sema 内就曾有三套（type_rhs_uses_var /
 * type_has_ct_op / type_has_nonliteral_array_bound），而且**覆盖面互不相同**
 * （例如前者认 AST_BINARY/AST_CALL，后两者不认；后两者认 AST_OPTION，前者
 * 不认，且都没有覆盖 AST_PTR/AST_SLICE_TYPE/AST_TUPLE）。同一类型槽位在不同
 * 查询下得到不同结论，是隐蔽 bug 的来源（该推迟的不推迟 → 槽位提前解析失败
 * → 快速失败闸门挡掉 3b）。
 *
 * 这里把该事实收敛为一份实现：`ast_visit_children` 描述直接子节点，
 * `ast_walk_preorder` 在其上做前序遍历（支持提前终止），各阶段只写"谓词"。
 *
 * 范围：**表达式与类型表达式**。语句层遍历（块/if/while/…）不在此列——需要
 * 语句层的地方（compiler 的 fid 预扫描、sema 的 3b walk）有各自的控制流语义，
 * 不宜共享。
 * =========================================================================== */

typedef enum {
  AST_WALK_CONTINUE = 0,  /* 继续遍历（visit 当前节点后进入其子节点） */
  AST_WALK_SKIP_CHILDREN, /* **不下钻当前节点的子节点**，但继续兄弟/外层遍历
                             （用于"该节点的子表达式不是我要找的东西"：例如
                             typeof/sizeof/alignof 的操作数是**表达式**，其中的
                             标识符是变量名，不是类型名） */
  AST_WALK_STOP           /* 停止整棵遍历（提前终止） */
} ast_walk_action_t;

/** 访问回调：child 为当前节点（前序遍历下的每个节点，含遍历根）。 */
typedef ast_walk_action_t (*ast_visit_fn)(ast_node_t *child, void *user);

/**
 * 对 node 的每个**直接子节点**调用 visit；子节点若成兄弟链（参数表、元素表、
 * 字段表、实参表、下标表…），链上每个节点都算直接子节点。
 * node 自身的兄弟链**不属于**它的子树（调用方自行遍历链）。
 * 返回 false = 被 visit 提前终止。
 */
bool ast_visit_children(ast_node_t *node, ast_visit_fn visit, void *user);

/**
 * 前序遍历 node 子树（含 node 自身），每个节点先 visit 再进入其子节点。
 * 返回 false = 被 visit 提前终止。node 为 NULL 时返回 true（空树）。
 */
bool ast_walk_preorder(ast_node_t *node, ast_visit_fn visit, void *user);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_AST_WALK_ */
