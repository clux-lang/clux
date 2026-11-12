#include "engine/integer.h"
#include "core/safe_alloc.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stddef.h>
#include <stdint.h>

static int32_t i32_read(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(int32_t *)raw : 0;
}

static clux_value_t *i32_add(clux_context_t *ctx,
                             clux_value_t *left,
                             clux_binary_op_t op,
                             clux_value_t *right) {
  (void)op;
  int32_t result = i32_read(left) + i32_read(right);
  return clux_context_create_i32(ctx, result, NULL, NULL);
}

static clux_vtable_t g_i32_vtable = {
    .add = i32_add,
};

clux_vtable_t *get_clux_i32_vtable(void) { return &g_i32_vtable; }
