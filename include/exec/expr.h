#ifndef _H_EXEC_EXPR_
#define _H_EXEC_EXPR_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *exec_expr_group(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_binary(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_subscript(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_slice(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_func_type(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_func_decl(clux_context_t *ctx, node_t *node);
/* 匿名函数值求值：不按名 define，供函数装饰器包装路径使用。 */
clux_value_t *exec_func_decl_value(clux_context_t *ctx, node_t *node);
/* 块提升两阶段捕获：phase 1 仅定义（跳过捕获初始化），phase 2 对已
   定义函数值统一补做定义点捕获快照——前向捕获成立（相互递归被类型
   判等拒绝：func.c signature_equal 含捕获表，类型无限回归）。 */
clux_value_t *exec_func_decl_define(clux_context_t *ctx, node_t *node);
clux_value_t *exec_func_decl_bind_captures(clux_context_t *ctx,
                                           clux_value_t *func,
                                           node_t *node);
/* 捕获表构造/初始化公共体（defer 延期体与函数定义共用 FUNC_CAPTURES
   布局）：capture_entry 并入一项捕获（|f: T| 标注 / |f| 名字推断 /
   |h = expr| 默认值推断）；init_captures 按 captures_node 做定义点
   捕获快照（node 仅提供位置上下文）。 */
#include "engine/func.h"
bool exec_func_decl_capture_entry(clux_context_t *ctx,
                                  node_t *name_node,
                                  node_t *type_node,
                                  node_t *default_node,
                                  size_t idx,
                                  clux_func_capture_t *captures,
                                  size_t *filled,
                                  clux_value_t **out_fail);
clux_value_t *exec_func_decl_init_captures(clux_context_t *ctx,
                                           clux_value_t *func,
                                           node_t *captures_node,
                                           node_t *node);
clux_value_t *exec_expr_union(clux_context_t *ctx, node_t *node);
/* 匿名 union 值求值：不按名 define，供 union 装饰器包装路径使用。 */
clux_value_t *exec_expr_union_value(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_enum_decl(clux_context_t *ctx, node_t *node);
/* 匿名 enum 值求值：不按名 define，供 enum 装饰器包装路径使用。 */
clux_value_t *exec_expr_enum_decl_value(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_bitfield_decl(clux_context_t *ctx, node_t *node);
/* 匿名 bitfield 值求值：不按名 define，供 bitfield 装饰器包装路径
   使用。 */
clux_value_t *exec_expr_bitfield_decl_value(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_interface_decl(clux_context_t *ctx, node_t *node);
/* 匿名 interface 值求值：不按名 define，供 interface 装饰器包装路径
   使用。 */
clux_value_t *exec_expr_interface_decl_value(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_struct_decl(clux_context_t *ctx, node_t *node);
/* 泛型实例重执行（exec/generic.c 进入）：在已含泛型参数绑定的实例化
   scope 内重执行声明 AST——func 实例的绑定并入捕获表（尾随捕获），
   struct 实例不 define 留原名作 shell 名，type 别名实例只重求值
   VALUE 表达式。 */
clux_value_t *exec_func_decl_instance(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_struct_decl_instance(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_tuple(clux_context_t *ctx, node_t *node);
/* 构造表达式 .<type?>{ elem, ... }（白皮书 §5.10）：构造必穷尽，具名按
   类型分流，匿名产出匿名 struct/tuple（经 safe_cast 鸭子类型转换到接收方）。 */
clux_value_t *exec_expr_construct(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_assign(clux_context_t *ctx, node_t *node);
clux_value_t *exec_expr_ternary(clux_context_t *ctx, node_t *node);
#ifdef __cplusplus
}
#endif
#endif
