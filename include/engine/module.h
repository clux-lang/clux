#ifndef _H_ENGINE_MODULE_
#define _H_ENGINE_MODULE_
#include "core/allocator.h"
#include "core/vec.h"
#include "engine/interface.h"
#include "parser/node.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 模块（白皮书 §8）：顶层命名边界，绑定模块名、模块级声明名表
   （globals：全部模块级名字；exports：其中被导出的子集，两个向量
   存的是同一批字符串指针，释放只走 globals 一次）与模块作用域。
   所有权约定：
   - node（模块 AST 载荷）归 module 所有，销毁时随之一并 delete；
   - scope 是借用——由 context 创建并挂 scope 树（parent 链到
     global），生命周期归 scope 树，module 不创建也不销毁它。 */
clux_module_t *create_clux_module(allocator_t *allocator,
                                  const char *name,
                                  /* 可为 NULL */ node_t *node,
                                  clux_scope_t *scope);
void delete_clux_module(clux_module_t **module);
/* 登记一个模块级名字；exported 决定是否同时进 exports。
   重复登记返回 false（首登不受影响），成功返回 true。 */
bool clux_module_declare(clux_module_t *self, const char *name, bool exported);
const char *clux_module_get_name(clux_module_t *self);
vec_t *clux_module_get_globals(clux_module_t *self);
vec_t *clux_module_get_exports(clux_module_t *self);
clux_scope_t *clux_module_get_scope(clux_module_t *self);
#ifdef __cplusplus
}
#endif
#endif
