#include "engine/type.h"
#include "core/allocator.h"
#include "core/rbtree.h"
#include "core/vec.h"
#include "engine/interface.h"
#include <stddef.h>
#include <string.h>
struct _clux_type_t {
  allocator_t *allocator;
  clux_type_kind_t kind;
  const char *name;
  size_t size;
  size_t align;
  clux_vtable_t *vtable;
  void *meta;
  rbtree_t *children; /* key 为 kind，value 为拥有 child 的桶 vec_t */
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
  size_t name_len = strlen(name) + 1;
  char *name_copy = allocator_alloc(allocator, name_len);
  (void)memcpy(name_copy, name, name_len);
  type->name = name_copy;
  type->size = size;
  type->align = align;
  type->vtable = vtable;
  type->meta = NULL;
  type->children = create_rbtree(allocator);
  return type;
}
static void
delete_child_bucket(uintptr_t kind, vec_t *bucket, void *user_data) {
  (void)kind;
  (void)user_data;
  for (size_t idx = 0; idx < vec_get_size(bucket); idx++) {
    clux_type_t *child = vec_get(bucket, idx);
    delete_clux_type(&child);
  }
  delete_vec(&bucket);
}
void delete_clux_type(clux_type_t **self) {
  if (!self || !*self) {
    return;
  }
  clux_type_t *type = *self;
  rbtree_foreach(type->children, (rbtree_callback_t)delete_child_bucket, NULL);
  delete_rbtree(&type->children);
  allocator_free(type->allocator, &type->meta);
  allocator_free(type->allocator, (void **)&type->name);
  allocator_free(type->allocator, (void **)self);
}
clux_type_kind_t clux_type_get_kind(clux_type_t *self) { return self->kind; }
size_t clux_type_get_size(clux_type_t *self) { return self->size; }
size_t clux_type_get_align(clux_type_t *self) { return self->align; }
const char *clux_type_get_name(clux_type_t *self) { return self->name; }
clux_vtable_t *clux_type_get_vtable(clux_type_t *self) { return self->vtable; }
allocator_t *clux_type_get_allocator(clux_type_t *self) {
  return self->allocator;
}
void *clux_type_create_meta(clux_type_t *self, size_t meta_size) {
  if (self->meta) {
    allocator_free(self->allocator, &self->meta);
  }
  self->meta = allocator_alloc(self->allocator, meta_size);
  return self->meta;
}
void *clux_type_get_meta(clux_type_t *self) { return self->meta; }
clux_type_t *clux_type_find_child(clux_type_t *self,
                                  clux_type_kind_t kind,
                                  clux_type_child_pred_t pred,
                                  void *user_data) {
  vec_t *bucket = rbtree_find(self->children, (uintptr_t)kind);
  if (!bucket) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(bucket); idx++) {
    clux_type_t *child = vec_get(bucket, idx);
    if (!pred || pred(child, user_data)) {
      return child;
    }
  }
  return NULL;
}
void clux_type_add_child(clux_type_t *self,
                         clux_type_kind_t kind,
                         clux_type_t *child) {
  vec_t *bucket = rbtree_find(self->children, (uintptr_t)kind);
  if (!bucket) {
    bucket = create_vec(self->allocator);
    (void)rbtree_insert(self->children, (uintptr_t)kind, bucket);
  }
  (void)vec_push(bucket, child);
}
