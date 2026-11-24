#ifndef _H_EXEC_GENERIC_
#define _H_EXEC_GENERIC_
#include "engine/context.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 泛型显式实例化（白皮书 §6.6）：下标形态 tmpl[arg, ...] 的展开通道。
   exec_expr_subscript 检测 BASE 求值为模板值（type_template /
   func_template）时转入此处；取回模板 AST 的参数表逐参绑定——
   实参在实例化点作用域求值，绑定值（含 §6.2 默认值）clone 进实例化
   scope 按参名 declare，模板 AST 在绑定环境内重执行产出实例值，最后
   按块出口协议把实例值 clone 出实例化 scope。调用点推导（§6.7）与
   约束检查（§6.5）、参数包（§6.4）本批不支持。 */
clux_value_t *exec_generic_instantiate(clux_context_t *ctx,
                                       clux_value_t *template_value,
                                       node_t *subscript);
#ifdef __cplusplus
}
#endif
#endif
