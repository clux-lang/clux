#ifndef _H_ENGINE_VALUE_
#define _H_ENGINE_VALUE_
#include "core/allocator.h"
#include "core/safe_alloc.h"
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_value_t *
create_clux_value(allocator_t *allocator, clux_type_t *type, ptr_handle_t data);
ptr_handle_t clux_value_get_data(clux_value_t *self);
void *clux_value_get_raw(clux_value_t *self, size_t offset);
clux_type_t *clux_value_get_type(clux_value_t *self);
bool clux_value_is_shadow(clux_value_t *self);
/* TDZ（temporal dead zone）：声明后未完成首次赋值的标记。
   消费前必须校验（报 use of a value before initialization），
   首次赋值清除；TDZ 期间绕过 mutable 检查（const 支持首次赋值）。 */
bool clux_value_is_tdz(clux_value_t *self);
void clux_value_set_tdz(clux_value_t *self, bool tdz);
void delete_clux_value(clux_value_t **self);
#ifdef __cplusplus
}
#endif
#endif
