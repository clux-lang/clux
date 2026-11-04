#ifndef _H_PARSER_NODE_
#define _H_PARSER_NODE_
#include "core/allocator.h"
#include "core/location.h"
#include "core/vec.h"
#ifdef __cplusplus
extern "C" {
#endif
enum _node_kind_t {
  NODE_PROGRAM,
  NODE_STATEMENT_EXPRESSION,
  NODE_LITERAL_NIL,
  NODE_LITERAL_UNDEFINED,
  NODE_LITERAL_STRING,
  NODE_LITERAL_CHARACTER,
  NODE_LITERAL_NUMERIC,
  NODE_LITERAL_BOOLEAN,
  NODE_LITERAL_IDENTIFIER,
};
typedef enum _node_kind_t node_kind_t;
typedef struct _node_t node_t;
node_t *create_node(allocator_t *allocator,
                    node_kind_t kind,
                    node_t *parent,
                    location_t loc);
void delete_node(node_t **node);
node_kind_t node_get_kind(node_t *self);
vec_t *node_get_children(node_t *self);
const location_t *node_get_location(node_t *self);
node_t *node_get_parent(node_t *self);
#ifdef __cplusplus
}
#endif
#endif