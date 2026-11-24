#ifndef _H_EXEC_GENERIC_
#define _H_EXEC_GENERIC_
#include "engine/context.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 参数包名（白皮书 §6.4）：rest 节点 ...Args 的 EXPR 槽文本。
   返回调用方 allocator_free 的堆字符串；无名容错返回 NULL。 */
char *exec_rest_pack_name(allocator_t *allocator, node_t *rest);
/* 泛型显式实例化（白皮书 §6.6）：下标形态 tmpl[arg, ...] 的展开通道。
   exec_expr_subscript 检测 BASE 求值为模板值（type_template /
   func_template）时转入此处；取回模板 AST 的参数表逐参绑定——
   实参在实例化点作用域求值，绑定值（含 §6.2 默认值）clone 进实例化
   scope 按参名 declare，约束（§6.5）在绑定该参数之后检查，模板 AST
   在绑定环境内重执行产出实例值，最后按块出口协议把实例值 clone 出
   实例化 scope。参数包（§6.4）须在参数表最后：多余实参全部归包，
   绑定为一个 type value（描述 TUPLE 类型，元素即各实参类型，空包
   绑 <>）。 */
clux_value_t *exec_generic_instantiate(clux_context_t *ctx,
                                       clux_value_t *template_value,
                                       node_t *subscript);
/* 调用点推导（白皮书 §6.7）：只有函数调用触发——func 模板 callee 未
   写类型实参时，从实参类型推断类型参数绑定（形参类型为裸标识符且
   等于类型参数名），推断不出/值参数走默认值回退。rest 形参
   `args: ...Args` 从该位置起吃掉剩余实参类型，推断包绑定为一个
   type value（描述 TUPLE 类型）。返回推断实例化出的 func 值（调用方
   随后按普通函数 invoke）。 */
clux_value_t *exec_generic_infer_instance(clux_context_t *ctx,
                                          clux_value_t *template_value,
                                          size_t argc,
                                          clux_value_t *argv[],
                                          const location_t *loc);
#ifdef __cplusplus
}
#endif
#endif
