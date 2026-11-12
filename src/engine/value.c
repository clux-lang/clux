#include "engine/value.h"
#include "core/allocator.h"
#include "core/safe_alloc.h"
#include "engine/interface.h"
struct _clux_value_t {
  allocator_t *allocator;
  clux_type_t *type;
  ptr_handle_t data;
};

clux_value_t *create_clux_value(allocator_t *allocator,
                                clux_type_t *type,
                                ptr_handle_t data) {
  clux_value_t *value =
      allocator_alloc(allocator, sizeof(struct _clux_value_t));
  value->allocator = allocator;
  value->type = type;
  value->data = data;
  return value;
}
clux_value_t *create_clux_shadow_value(allocator_t *allocator,
                                       clux_type_t *type) {
  clux_value_t *value =
      allocator_alloc(allocator, sizeof(struct _clux_value_t));
  value->allocator = allocator;
  value->type = type;
  value->data = 0;
  return value;
}
ptr_handle_t clux_value_get_data(clux_value_t *self) { return self->data; }
clux_type_t *clux_value_get_type(clux_value_t *self) { return self->type; }
void delete_clux_value(clux_value_t **self) {
  if (!self || !*self) {
    return;
  }
  clux_value_t *value = *self;
  if (value->data) {
    safe_free(value->data);
    value->data = 0;
  }
  allocator_free(value->allocator, (void **)self);
}