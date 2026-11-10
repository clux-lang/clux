#include "parser/node_struct_member.h"
#include "core/vec.h"
#include "parser/node.h"

node_t *create_struct_members_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_STRUCT_MEMBERS);
}

void struct_members_node_add_member(node_t *self, node_t *member) {
  node_add_child(self, member);
}

size_t struct_members_node_get_member_count(node_t *self) {
  return vec_get_size(node_get_children(self));
}

node_t *struct_members_node_get_member(node_t *self, size_t idx) {
  return node_get_child(self, idx);
}

node_t *create_struct_embed_node(allocator_t *allocator) {
  node_t *self = create_node(allocator, CLUX_NODE_STRUCT_EMBED);
  node_resize_children(self, CLUX_STRUCT_EMBED_CHILDREN_SIZE);
  return self;
}

void struct_embed_node_set_type(node_t *self, node_t *type) {
  node_set_child(self, CLUX_STRUCT_EMBED_TYPE, type);
}

node_t *struct_embed_node_get_type(node_t *self) {
  return node_get_child(self, CLUX_STRUCT_EMBED_TYPE);
}
