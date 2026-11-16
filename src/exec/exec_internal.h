#ifndef _H_EXEC_INTERNAL_
#define _H_EXEC_INTERNAL_
/* exec 模块内部声明：跨 exec_*.c 共享的结构、值通道与工具。
   外部一律走 include/exec/exec.h，不包含本文件。 */
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "parser/node.h"
#include <stdbool.h>

struct _clux_exec_t {
  allocator_t *allocator;
  clux_context_t *ctx;
  const char *source;
};

/* ---------------------------------------------------------------------------
   值通道：exec 的"值"是 engine value 或控制流信号（return/break/
   continue 是 exec 层的特殊值，不进 engine——语义只属于执行器）。
   信号由吸收点拦截：return 止于函数帧，break/continue 止于循环，
   逃逸到顶层是错误。 */
typedef enum {
  EXEC_VALUE_TAG = 0, /* value 槽有效；error 值即硬错误 */
  EXEC_SIGNAL_RETURN, /* value 槽携带返回值，可为 NULL（return;） */
  EXEC_SIGNAL_BREAK,
  EXEC_SIGNAL_CONTINUE,
} exec_tag_t;

typedef struct exec_value {
  exec_tag_t tag;
  clux_value_t *value;
} exec_value_t;

typedef exec_value_t (*exec_visit_fn)(clux_exec_t *self, node_t *node);

/* node_kind_t 枚举无 sentinel，执行器分派表按最后一个 kind 静态断言
   对齐：parser 增删节点种类时这里会立即编译失败，提醒同步表。 */
#define EXEC_NODE_KIND_COUNT (CLUX_NODE_STMT_INTERFACE + 1)

/* —— exec.c：生命周期与共享工具 —— */
exec_value_t exec_ok(clux_value_t *value);  /* value 可为 NULL（无值） */
exec_value_t exec_err(clux_value_t *error); /* error 值包装为硬错误 */
exec_value_t exec_signal(exec_tag_t tag, clux_value_t *carrier);
bool exec_is_error(exec_value_t result);
bool exec_is_error_value(clux_value_t *value);
/* 语义化 error 构造（clux_context_create_error_message 通道）。 */
exec_value_t
exec_errorf(clux_exec_t *self, const location_t *loc, const char *fmt, ...);
/* 整区间文本比较（location_is 通道，零分配）：node 须为名字/符号类
   叶子节点。 */
bool exec_node_is(node_t *node, const char *text);
/* 文本分配副本（location_get 通道），调用方负责释放。 */
char *exec_node_text(clux_exec_t *self, node_t *node);
/* 节点 location 的可写借用视图：engine 侧 API 以 location_t* 收参
   （记录分配点/错误位置使用），AST 侧只读。 */
static inline location_t *exec_loc(node_t *node) {
  return (location_t *)node_get_location(node);
}

/* 类型工具：type value（CLUX_TYPE_TYPE）提取被描述类型指针；
   非 type value 返回 NULL。 */
clux_type_t *exec_type_of_value(clux_value_t *value);
/* 求类型表达式为 clux_type_t*（walk 后经 type value 提取）；
   失败返回 NULL 且 *error 携带硬错误（求值错误原样透传，
   非 type value 报语义化错误）。 */
clux_type_t *
exec_eval_type(clux_exec_t *self, node_t *node, exec_value_t *error);
/* 条件转 bool：非 bool 值返回 error（脚本的 if/while 条件显式要求
   bool，与白皮书 §4.4/§4.6 一致）。loc 是条件表达式的出错定位。 */
exec_value_t
exec_cond_bool(clux_exec_t *self, clux_value_t *value, const location_t *loc);

/* —— exec_dispatch.c：kind 分派表（map，替代 switch） —— */
exec_value_t exec_dispatch_expr(clux_exec_t *self, node_t *node);
exec_value_t exec_dispatch_stmt(clux_exec_t *self, node_t *node);

/* —— exec_literal.c —— */
exec_value_t exec_visit_numeric(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_string(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_character(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_boolean(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_identifier(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_type_literal(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_undefined(clux_exec_t *self, node_t *node);

/* —— exec_expr.c —— */
exec_value_t exec_visit_group(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_ternary(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_assign(clux_exec_t *self, node_t *node);
/* 二元/一元前缀/后缀共用同一节点形态，共用一个 handler。 */
exec_value_t exec_visit_binary(clux_exec_t *self, node_t *node);

/* —— exec_stmt.c —— */
exec_value_t exec_visit_block(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_stmt_expr(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_empty(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_return(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_break(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_continue(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_if(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_while(clux_exec_t *self, node_t *node);

/* —— exec_decl.c —— */
exec_value_t exec_visit_var(clux_exec_t *self, node_t *node);

/* —— exec_func.c —— */
/* 函数语句（stmt 包装层）：取 FUNC_DECL 透传到声明体执行。 */
exec_value_t exec_visit_func_stmt(clux_exec_t *self, node_t *node);
/* 函数声明体（表达式位置共用：具名声明/匿名闭包）：
   签名落地为 func value，闭包捕获按值拷贝，具名经 declare_value。 */
exec_value_t exec_visit_func_decl(clux_exec_t *self, node_t *node);

/* —— exec_type.c —— */
exec_value_t exec_visit_type_stmt(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_struct_stmt(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_union_stmt(clux_exec_t *self, node_t *node);
exec_value_t exec_visit_enum_stmt(clux_exec_t *self, node_t *node);
#endif
