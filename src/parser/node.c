#include "parser/node.h"
#include "core/allocator.h"
#include "core/location.h"
#include "core/vec.h"
struct _node_t {
  allocator_t *allocator;
  node_kind_t kind;
  node_t *parent;
  vec_t *children;
  location_t loc;
};
node_t *create_node(allocator_t *allocator,
                    node_kind_t kind,
                    node_t *parent,
                    location_t loc) {
  node_t *self = allocator_alloc(allocator, sizeof(struct _node_t));
  self->allocator = allocator;
  self->kind = kind;
  self->loc = loc;
  self->parent = parent;
  self->children = create_vec(allocator, false);
  return self;
}
void delete_node(node_t **node) {
  if (!node || !*node) {
    return;
  }
  node_t *self = *node;
  *node = NULL;
  allocator_t *allocator = self->allocator;
  /* 递归删除子节点（children 为非 own，需手动释放） */
  vec_t *children = self->children;
  size_t count = vec_get_size(children);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *child = vec_get(children, idx);
    delete_node(&child);
  }
  delete_vec(&self->children);
  allocator_free(allocator, (void **)&self);
}
node_kind_t node_get_kind(node_t *self) { return self->kind; }
vec_t *node_get_children(node_t *self) { return self->children; }
const location_t *node_get_location(node_t *self) { return &self->loc; }
node_t *node_get_parent(node_t *self) { return self->parent; }