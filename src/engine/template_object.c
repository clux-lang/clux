#include "engine/template_object.h"
#include <stdbool.h>
#include <stddef.h>

clux_template_t *create_clux_template(allocator_t *allocator,
                                      void *ast,
                                      clux_module_t *module,
                                      const char *name) {
  if (!allocator) {
    return NULL;
  }
  clux_template_t *self = allocator_alloc(allocator, sizeof(clux_template_t));
  if (!self) {
    return NULL;
  }
  self->ast = ast;
  self->module = module;
  self->name = name;
  self->allocator = allocator;
  return self;
}

void delete_clux_template(clux_template_t **self) {
  if (!self || !*self) {
    return;
  }
  /* 三个指针字段全部借用（AST 树 / modules 注册表 / strs 驻留表），
     这里只释放对象本体。 */
  allocator_free((*self)->allocator, (void **)self);
}

void *clux_template_get_ast(clux_template_t *self) {
  return self ? self->ast : NULL;
}

clux_module_t *clux_template_get_module(clux_template_t *self) {
  return self ? self->module : NULL;
}

const char *clux_template_get_name(clux_template_t *self) {
  return self ? self->name : NULL;
}
