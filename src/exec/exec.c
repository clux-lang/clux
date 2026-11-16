/* 执行器生命周期与共享工具：值通道包装、错误构造、location 文本解码。 */
#include "exec_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

clux_exec_t *create_clux_exec(allocator_t *allocator,
                              clux_context_t *ctx,
                              const char *source) {
  clux_exec_t *self = allocator_alloc(allocator, sizeof(struct _clux_exec_t));
  self->allocator = allocator;
  self->ctx = ctx;
  self->source = source;
  return self;
}

void delete_clux_exec(clux_exec_t **self) {
  if (!self || !*self) {
    return;
  }
  allocator_free((*self)->allocator, (void **)self);
}

exec_value_t exec_ok(clux_value_t *value) {
  exec_value_t result = {EXEC_VALUE_TAG, value};
  return result;
}

exec_value_t exec_err(clux_value_t *error) { return exec_ok(error); }

exec_value_t exec_signal(exec_tag_t tag, clux_value_t *carrier) {
  exec_value_t result = {tag, carrier};
  return result;
}

bool exec_is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

bool exec_is_error(exec_value_t result) {
  return result.tag == EXEC_VALUE_TAG && result.value &&
         exec_is_error_value(result.value);
}

exec_value_t
exec_errorf(clux_exec_t *self, const location_t *loc, const char *fmt, ...) {
  char message[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  return exec_err(clux_context_create_error_message(
      self->ctx, (location_t *)loc, "%s", message));
}

bool exec_node_is(node_t *node, const char *text) {
  return node ? location_is(node_get_location(node), text) : false;
}

char *exec_node_text(clux_exec_t *self, node_t *node) {
  return location_get(node_get_location(node), self->allocator);
}

clux_type_t *exec_type_of_value(clux_value_t *value) {
  if (clux_type_get_kind(clux_value_get_type(value)) != CLUX_TYPE_TYPE) {
    return NULL;
  }
  ptr_handle_t data = clux_value_get_data(value);
  if (!data) {
    return NULL;
  }
  void *raw = ptr_to_raw(data, 0);
  return raw ? *(clux_type_t **)raw : NULL;
}

clux_type_t *exec_eval_type(clux_exec_t *self,
                            node_t *node,
                            exec_value_t *error) {
  if (!node) {
    *error = exec_errorf(self, NULL, "missing type expression");
    return NULL;
  }
  exec_value_t value = exec_dispatch_expr(self, node);
  if (exec_is_error(value)) {
    *error = value;
    return NULL;
  }
  clux_type_t *type = exec_type_of_value(value.value);
  if (!type) {
    *error = exec_errorf(self, exec_loc(node), "expected a type expression");
    return NULL;
  }
  return type;
}

exec_value_t
exec_cond_bool(clux_exec_t *self, clux_value_t *value, const location_t *loc) {
  clux_type_t *type = clux_value_get_type(value);
  if (clux_type_get_kind(type) != CLUX_TYPE_BOOL) {
    return exec_errorf(self,
                       loc,
                       "condition requires 'bool', got '%s'",
                       clux_type_get_name(type));
  }
  bool cond = *(bool *)ptr_to_raw(clux_value_get_data(value), 0);
  return exec_ok(clux_context_create_bool(self->ctx, cond, NULL, NULL));
}

clux_value_t *clux_exec_run(clux_exec_t *self, node_t *program) {
  if (!self) {
    return NULL;
  }
  if (!program || node_get_kind(program) != CLUX_NODE_PROGRAM) {
    return clux_context_create_error_message(
        self->ctx, NULL, "exec requires a program node");
  }
  clux_value_t *last = NULL;
  vec_t *children = node_get_children(program);
  size_t count = vec_get_size(children);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = vec_get(children, idx);
    exec_value_t result = exec_dispatch_stmt(self, stmt);
    if (exec_is_error(result)) {
      return result.value;
    }
    if (result.tag != EXEC_VALUE_TAG) {
      static const char *outside[] = {
          "return",
          "break",
          "continue",
      };
      return clux_context_create_error_message(
          self->ctx,
          exec_loc(stmt),
          "'%s' outside of its consuming construct",
          outside[result.tag - 1]);
    }
    if (result.value) {
      last = result.value;
    }
  }
  return last;
}
