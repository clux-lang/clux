#ifndef _H_EXEC_CALL_
#define _H_EXEC_CALL_
#include "engine/interface.h"
#include "parser/node.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 函数调用表达式 f(args)（白皮书 §2.7）：callee 与实参逐个求值后经
   clux_context_invoke 交验签名，真函数回调帧协议执行器（见 call.c）。*/
clux_value_t *exec_expr_call(clux_context_t *ctx, node_t *node);
/* 懒安装帧协议执行器：直接走 clux_context_invoke 的路径（var 装饰器包装
   等）invoke 前调用，保证 executor 已挂且 engine 不依赖 exec。 */
void exec_call_install_executor(clux_context_t *ctx);
#ifdef __cplusplus
}
#endif
#endif
