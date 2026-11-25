#include "engine/function.h"
#include <stdbool.h>
#include <stddef.h>

clux_function_t *create_clux_function(allocator_t *allocator,
                                      void *ast,
                                      clux_module_t *module,
                                      const char *name) {
  if (!allocator) {
    return NULL;
  }
  clux_function_t *self = allocator_alloc(allocator, sizeof(clux_function_t));
  if (!self) {
    return NULL;
  }
  self->ast = ast;
  self->module = module;
  self->name = name;
  self->allocator = allocator;
  return self;
}

void delete_clux_function(clux_function_t **function) {
  if (!function || !*function) {
    return;
  }
  /* 三个指针字段全部借用（AST 树 / modules 注册表 / strs 驻留表），
     这里只释放对象本体。 */
  allocator_free((*function)->allocator, (void **)function);
}

void *clux_function_get_ast(clux_function_t *self) {
  return self ? self->ast : NULL;
}

clux_module_t *clux_function_get_module(clux_function_t *self) {
  return self ? self->module : NULL;
}

const char *clux_function_get_name(clux_function_t *self) {
  return self ? self->name : NULL;
}
