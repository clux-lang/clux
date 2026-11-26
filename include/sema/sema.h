#ifndef _H_SEMA_SEMA_
#define _H_SEMA_SEMA_
#include "core/allocator.h"
#include "core/vec.h"
#include "engine/context.h"
#include "parser/node.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* sema：语义分析两阶段驱动（docs/sema-design.md §4、§7）。
   - 阶段一 sema_walk：只读遍历原 AST，把声明登记进 engine/module 体系
     （作用域树 + 符号表直接消费 engine/scope，不自建）；错误逐层抛出到
     stmt 级经 context 的 diag 收集器入列，从下一个 stmt 继续。
   - 阶段二 sema_emit：从 engine/module 体系渲染全新 program_t 集合
     （out_allocator 上，与 engine 无关）。emit 对 sema 状态只读、幂等。
   借用关系：ctx 与 out_allocator 均归调用方；sema 本体随 engine 侧
   allocator 生死。walk 有 ERROR 级 diag 时不 emit——错误即终止。 */
typedef struct _clux_sema_t clux_sema_t;

clux_sema_t *sema_create(clux_context_t *ctx, allocator_t *out_allocator);
void sema_destroy(clux_sema_t **sema);

/* 阶段一。program 必须是 CLUX_NODE_PROGRAM；walk 只允许执行一次，
   重复调用返回 -1 并记 diag。返回 0 表示结构走完（语义错误经
   sema_has_error 查询，不在此返回值上反映）。 */
int sema_walk(clux_sema_t *sema, node_t *program);

bool sema_has_error(const clux_sema_t *sema);

/* 阶段二：返回 program_t* 的有序集合（import 拓扑序，归调用方所有，
   vec 与元素均分配在 out_allocator 上）。前置：walk 完成且无 ERROR
   级 diag，否则返回 NULL。 */
vec_t *sema_emit(clux_sema_t *sema);

/* 类型结果表查询（§8.5 第一版）：check pass 记录的节点类型——STMT_VAR
   语句节点、EXPR_CONSTRUCT_FIELD 节点、foreach 迭代变量 name 节点。
   返回借用指针（类型归 engine context 托管）；未入表或类型不可得
   返回 NULL。emit 侧渲染回填在 P10 接入。 */
clux_type_t *sema_get_node_type(clux_sema_t *sema, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
