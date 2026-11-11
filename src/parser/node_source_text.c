#include "parser/node_source_text.h"
#include "parser/node.h"

node_t *create_source_text_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_SOURCE_TEXT);
}
