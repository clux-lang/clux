#include "engine/module.h"
#include "core/allocator.h"
#include "core/vec.h"
#include "engine/interface.h"
#include "parser/node.h"
#include <string.h>

struct _clux_module_t {
  char *name;
  vec_t *globals;      /* char*，module 分配的名字副本（所有权） */
  vec_t *exports;      /* char*，与 globals 同一批指针（别名，不重复释放） */
  clux_scope_t *scope; /* 借用：生命周期归 scope 树 */
  node_t *node;        /* 所有权：模块 AST 载荷，可为 NULL */
  allocator_t *allocator;
};

clux_module_t *create_clux_module(allocator_t *allocator,
                                  const char *name,
                                  node_t *node,
                                  clux_scope_t *scope) {
  clux_module_t *module =
      allocator_alloc(allocator, sizeof(struct _clux_module_t));
  size_t name_len = strlen(name);
  module->name = allocator_alloc(allocator, name_len + 1);
  memcpy(module->name, name, name_len + 1);
  module->globals = create_vec(allocator);
  module->exports = create_vec(allocator);
  module->scope = scope;
  module->node = node;
  module->allocator = allocator;
  return module;
}

void delete_clux_module(clux_module_t **module) {
  if (!module || !*module) {
    return;
  }
  clux_module_t *self = *module;
  *module = NULL;
  allocator_t *allocator = self->allocator;
  for (size_t idx = 0; idx < vec_get_size(self->globals); idx++) {
    char *name = vec_get(self->globals, idx);
    allocator_free(allocator, (void **)&name);
  }
  delete_vec(&self->globals);
  /* exports 与 globals 共享同一批指针，本体已随上面释放，此处只删向量。 */
  delete_vec(&self->exports);
  delete_node(&self->node);
  allocator_free(allocator, (void **)&self->name);
  allocator_free(allocator, (void **)&self);
}

bool clux_module_declare(clux_module_t *self, const char *name, bool exported) {
  for (size_t idx = 0; idx < vec_get_size(self->globals); idx++) {
    char *name_ = vec_get(self->globals, idx);
    if (strcmp(name_, name) == 0) {
      return false;
    }
  }
  size_t len = strlen(name);
  char *name_ = allocator_alloc(self->allocator, len + 1);
  memcpy(name_, name, len + 1);
  vec_push(self->globals, name_);
  if (exported) {
    vec_push(self->exports, name_);
  }
  return true;
}

const char *clux_module_get_name(clux_module_t *self) {
  return self ? self->name : NULL;
}

vec_t *clux_module_get_globals(clux_module_t *self) {
  return self ? self->globals : NULL;
}

vec_t *clux_module_get_exports(clux_module_t *self) {
  return self ? self->exports : NULL;
}

clux_scope_t *clux_module_get_scope(clux_module_t *self) {
  return self ? self->scope : NULL;
}
