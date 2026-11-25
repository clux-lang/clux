#ifndef _H_ENGINE_TEMPLATE_OBJECT_
#define _H_ENGINE_TEMPLATE_OBJECT_
#include "core/allocator.h"
#include "engine/interface.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 模板对象（type_template / func_template 值的封装层）：结构对齐
   clux_function_t——模板声明 AST 之外还承载定义模块与模板名。所有权约定：
   - ast 借用——声明 AST 归 AST 树，template 不创建也不销毁；
   - module 借用——生命周期归 context 的 modules 注册表；
   - name 借用——由 context 经 strs 驻留（随 context 释放），匿名可 NULL；
   - template 本体归 context 的模板注册表（rbtree 按名分桶），每次创建
     模板值时新建。 */
typedef struct _clux_template_t {
  void *ast;             /* 借用：模板声明 AST */
  clux_module_t *module; /* 借用：定义模块，NULL 表示 global */
  const char *name;      /* 借用：strs 驻留名，匿名模板可 NULL */
  allocator_t *allocator;
} clux_template_t;

clux_template_t *create_clux_template(allocator_t *allocator,
                                      void *ast,
                                      clux_module_t *module,
                                      const char *name);
void delete_clux_template(clux_template_t **self);
void *clux_template_get_ast(clux_template_t *self);
clux_module_t *clux_template_get_module(clux_template_t *self);
const char *clux_template_get_name(clux_template_t *self);
#ifdef __cplusplus
}
#endif
#endif
