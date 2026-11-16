#ifndef _H_EXEC_EXEC_
#define _H_EXEC_EXEC_
#include "core/allocator.h"
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif

/* 执行器：walkAST 递归向下的解释执行层（白皮书 §4/§5 运行语义）。
   与 engine 平级的独立模块——只消费 engine 的公开原语（context 的
   create/binary/assign/safe_cast/scope 栈）与 parser 的 AST，不进入
   engine 内部。当前视角是将 clux 当做脚本运行；未来嵌入 sema 阶段做
   编译期计算折叠（同一 walk，engine 原语天然产出并传播 shadow 值）。

   值生命周期：所有值由 context 的当前作用域托管（create 即注册），
   clux_exec_run 返回借用引用，本体在 context（及其 scope 栈）销毁前
   保持有效。error 值即硬错误：walk 沿途快速失败，不再继续求值。 */
typedef struct _clux_exec_t clux_exec_t;

/* source 是 parser 解析时使用的源码文本（借用，不复制）：AST 节点只
   记 location 区间，名字与字面量文本由执行器按区间从 source 解码。 */
clux_exec_t *create_clux_exec(allocator_t *allocator,
                              clux_context_t *ctx,
                              const char *source);
void delete_clux_exec(clux_exec_t **self);

/* 按源码顺序执行 program 的顶层语句，返回最后一条产生值的语句的值
   （借用，见上）；无可值语句返回 NULL。执行错误返回 error 值——
   消息经 clux_error_message 通道可用，error 值本体同样由 context 托管。 */
clux_value_t *clux_exec_run(clux_exec_t *self, node_t *program);
#ifdef __cplusplus
}
#endif
#endif
