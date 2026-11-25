#include "core/program.h"
#include "parser/node.h"
#include <string.h>

struct _program_t {
  node_t *decls;         /* 所有权归 program，可为 NULL */
  string_t *module_name; /* 可为 NULL */
  string_t *source_path; /* 可为 NULL */
  vec_t *strings;        /* string_t*，intern 表 */
  allocator_t *allocator;
};

program_t *create_program(allocator_t *allocator) {
  if (!allocator) {
    return NULL;
  }
  program_t *program = allocator_alloc(allocator, sizeof(struct _program_t));
  program->decls = NULL;
  program->module_name = NULL;
  program->source_path = NULL;
  program->strings = create_vec(allocator);
  program->allocator = allocator;
  return program;
}

void delete_program(program_t **program) {
  if (!program || !*program) {
    return;
  }
  program_t *self = *program;
  if (self->decls) {
    delete_node(&self->decls);
  }
  if (self->module_name) {
    delete_string(&self->module_name);
  }
  if (self->source_path) {
    delete_string(&self->source_path);
  }
  while (vec_get_size(self->strings) > 0) {
    string_t *str = vec_erase(self->strings, 0);
    delete_string(&str);
  }
  delete_vec(&self->strings);
  allocator_free(self->allocator, (void **)program);
}

void program_set_decls(program_t *self, node_t *decls) {
  if (!self) {
    return;
  }
  if (self->decls) {
    delete_node(&self->decls);
  }
  self->decls = decls;
}

node_t *program_get_decls(program_t *self) { return self ? self->decls : NULL; }

void program_set_module_name(program_t *self, const char *name) {
  if (!self) {
    return;
  }
  if (self->module_name) {
    delete_string(&self->module_name);
  }
  self->module_name = NULL;
  if (name) {
    self->module_name = create_string(self->allocator);
    string_append_cstr(self->module_name, name);
  }
}

const char *program_get_module_name(program_t *self) {
  return (self && self->module_name) ? string_get_data(self->module_name)
                                     : NULL;
}

void program_set_source_path(program_t *self, const char *path) {
  if (!self) {
    return;
  }
  if (self->source_path) {
    delete_string(&self->source_path);
  }
  self->source_path = NULL;
  if (path) {
    self->source_path = create_string(self->allocator);
    string_append_cstr(self->source_path, path);
  }
}

const char *program_get_source_path(program_t *self) {
  return (self && self->source_path) ? string_get_data(self->source_path)
                                     : NULL;
}

const char *
program_intern_string(program_t *self, const char *data, size_t len) {
  if (!self || !data) {
    return NULL;
  }
  /* 线性查重：第一版规模可控，需要时再换 hash 索引。 */
  for (size_t i = 0; i < vec_get_size(self->strings); ++i) {
    string_t *str = vec_get(self->strings, i);
    if (string_get_size(str) == len &&
        memcmp(string_get_data(str), data, len) == 0) {
      return string_get_data(str);
    }
  }
  string_t *str = create_string(self->allocator);
  string_append(str, data, len);
  vec_push(self->strings, str);
  return string_get_data(str);
}

vec_t *program_get_strings(program_t *self) {
  return self ? self->strings : NULL;
}
