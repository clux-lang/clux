/* void 类型：只作为"无返回值"的返回类型占位（白皮书 §2），无值、
   无运算——vtable 只提供按 kind 的类型判等，其余槽位留空由 context
   兜底报不支持。 */
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/void.h"

static bool void_type_equal(clux_type_t *self, clux_type_t *other) {
  return clux_type_get_kind(self) == clux_type_get_kind(other);
}

static bool void_type_extends(clux_type_t *self, clux_type_t *base) {
  return clux_type_get_kind(self) == clux_type_get_kind(base);
}

static clux_vtable_t g_void_vtable = {
    .type_equal = void_type_equal,
    .type_extends = void_type_extends,
};

clux_vtable_t *get_clux_void_vtable(void) { return &g_void_vtable; }
