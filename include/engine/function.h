#ifndef _H_ENGINE_FUNCTION_
#define _H_ENGINE_FUNCTION_
#include "core/allocator.h"
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 函数对象（func 值头部的封装层）：函数体 AST 指针之外还承载定义模块
   与函数名，供调用期定位模块上下文。所有权约定：
   - ast 借用——函数体 AST 归 AST 树，function 不创建也不销毁；
   - module 借用——生命周期归 context 的 modules 注册表；
   - name 借用——由 context 经 strs 驻留（随 context 释放），可 NULL；
   - function 本体归 context（functions 向量），每次挂接函数体新建。 */
typedef struct _clux_function_t {
  void *ast;             /* 借用：函数体 AST */
  clux_module_t *module; /* 借用：定义模块，NULL 表示 global */
  const char *name;      /* 借用：strs 驻留名，闭包匿名可 NULL */
  allocator_t *allocator;
} clux_function_t;

clux_function_t *create_clux_function(allocator_t *allocator,
                                      void *ast,
                                      clux_module_t *module,
                                      const char *name);
void delete_clux_function(clux_function_t **function);
void *clux_function_get_ast(clux_function_t *self);
clux_module_t *clux_function_get_module(clux_function_t *self);
const char *clux_function_get_name(clux_function_t *self);
#ifdef __cplusplus
}
#endif
#endif
