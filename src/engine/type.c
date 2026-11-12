#include "engine/type.h"
#include "core/allocator.h"
#include "engine/interface.h"
struct _clux_type_t {
  allocator_t *allocator;
  clux_type_kind_t kind;
  const char *name;
  size_t size;
  size_t align;
  clux_vtable_t *vtable;
  void *meta;
};
clux_type_t *create_clux_type(allocator_t *allocator,
                              clux_type_kind_t kind,
                              const char *name,
                              size_t size,
                              size_t align,
                              clux_vtable_t *vtable) {
  clux_type_t *type = allocator_alloc(allocator, sizeof(struct _clux_type_t));
  type->allocator = allocator;
  type->kind = kind;
  type->name = name;
  type->size = size;
  type->align = align;
  type->vtable = vtable;
  type->meta = NULL;
  return type;
}
void delete_clux_type(clux_type_t **self) {
  if (!self || !*self) {
    return;
  }
  clux_type_t *type = *self;
  allocator_free(type->allocator, &type->meta);
  allocator_free(type->allocator, (void **)self);
}
clux_type_kind_t clux_type_get_kind(clux_type_t *self) { return self->kind; }
size_t clux_type_get_size(clux_type_t *self) { return self->size; }
size_t clux_type_get_align(clux_type_t *self) { return self->align; }
const char *clux_type_get_name(clux_type_t *self) { return self->name; }
clux_vtable_t *clux_type_get_vtable(clux_type_t *self) { return self->vtable; }
void *clux_type_create_meta(clux_type_t *self, size_t meta_size) {
  if (self->meta) {
    allocator_free(self->allocator, &self->meta);
  }
  self->meta = allocator_alloc(self->allocator, meta_size);
  return self->meta;
}
void *clux_type_get_meta(clux_type_t *self) { return self->meta; }