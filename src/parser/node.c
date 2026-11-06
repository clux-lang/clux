#include "parser/node.h"
#include "core/allocator.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include <string.h>
struct _node_t {
  allocator_t *allocator;
  node_kind_t kind;
  node_t *parent;
  vec_t *children;
  location_t loc;
};
node_t *create_node(allocator_t *allocator, node_kind_t kind) {
  node_t *self = allocator_alloc(allocator, sizeof(struct _node_t));
  memset(&self->loc, 0, sizeof(location_t));
  self->allocator = allocator;
  self->kind = kind;
  self->parent = NULL;
  self->children = create_vec(allocator);
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
void node_set_parent(node_t *self, node_t *parent) { self->parent = parent; }
void node_set_location(node_t *self, const location_t *loc) {
  self->loc = *loc;
}
void node_resize_children(node_t *self, size_t size) {
  vec_resize(self->children, size);
}
node_t *node_set_child(node_t *self, size_t idx, node_t *child) {
  return (node_t *)vec_set(self->children, idx, child);
}
node_t *node_get_child(node_t *self, size_t idx) {
  return (node_t *)vec_get(self->children, idx);
}
void node_add_child(node_t *self, node_t *child) {
  vec_push(self->children, child);
}
node_t *parser_read_symbol(allocator_t *allocator, parser_t *parser) {
  token_t *token = parser_peek_token(parser);
  if (!token || token_get_kind(token) != CLUX_TOKEN_SYMBOL) {
    return NULL;
  }
  parser_next(parser);
  node_t *node = create_node(allocator, CLUX_NODE_SYMBOL);
  node_set_location(node, token_get_location(token));
  return node;
}