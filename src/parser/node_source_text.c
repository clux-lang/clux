#include "parser/node_source_text.h"
#include "parser/node.h"

node_t *create_source_text_node(allocator_t *allocator) {
  return create_node(allocator, CLUX_NODE_SOURCE_TEXT);
}

void source_text_node_to_json(json_writer_t *writer, node_t *self) {
  node_json_begin(writer, self, "SourceText");
  node_json_text(writer, self);
  node_json_end(writer);
}
