#ifndef _H_ENGINE_CONTEXT_
#define _H_ENGINE_CONTEXT_
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/safe_alloc.h"
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_context_t *create_clux_context(allocator_t *allocator);
void delete_clux_context(clux_context_t **self);
safe_allocator_t *clux_context_get_safe_allocator(clux_context_t *self);
clux_value_t *clux_context_create_value(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t data,
                                        const char *name,
                                        location_t *loc);
void clux_context_push_scope(clux_context_t *self);
void clux_context_pop_scope(clux_context_t *self);
clux_value_t *clux_context_create_error(clux_context_t *self,
                                        clux_error_t *error);
clux_value_t *clux_context_create_shadow_value(clux_context_t *self,
                                               clux_type_t *type);
clux_value_t *clux_context_create_i8(clux_context_t *self,
                                     int8_t value,
                                     const char *name,
                                     location_t *loc);
clux_value_t *clux_context_create_i16(clux_context_t *self,
                                      int16_t value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_i32(clux_context_t *self,
                                      int32_t value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_i64(clux_context_t *self,
                                      int64_t value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_u8(clux_context_t *self,
                                     uint8_t value,
                                     const char *name,
                                     location_t *loc);
clux_value_t *clux_context_create_u16(clux_context_t *self,
                                      uint16_t value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_u32(clux_context_t *self,
                                      uint32_t value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_u64(clux_context_t *self,
                                      uint64_t value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_isize(clux_context_t *self,
                                        intptr_t value,
                                        const char *name,
                                        location_t *loc);
clux_value_t *clux_context_create_usize(clux_context_t *self,
                                        size_t value,
                                        const char *name,
                                        location_t *loc);
clux_value_t *clux_context_create_f32(clux_context_t *self,
                                      float value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_f64(clux_context_t *self,
                                      double value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_bool(clux_context_t *self,
                                       bool value,
                                       const char *name,
                                       location_t *loc);
char *clux_context_alloc_str(clux_context_t *self, size_t len);
clux_value_t *clux_context_create_str(clux_context_t *self,
                                      const char *value,
                                      const char *name,
                                      location_t *loc);
clux_value_t *clux_context_create_ptr(clux_context_t *self,
                                      clux_type_t *type,
                                      ptr_handle_t value,
                                      const char *name,
                                      location_t *loc);
/* 取指向 value 的指针：error 自传播，shadow 产出 "*<base>" 类型的 shadow。
   任意类型可用，不经过 vtable。 */
clux_value_t *
clux_context_ref(clux_context_t *self, clux_value_t *value, location_t *loc);
/* 按 base 取 "*<base>" 指针类型：children 表 find-or-create，同一 base 恒等
   返回同一实例。 */
clux_type_t *clux_context_get_ptr_type(clux_context_t *self, clux_type_t *base);
/* 按 base 取 "[]<base>" 切片类型：children 表 find-or-create，同一 base
   恒等返回同一实例。 */
clux_type_t *clux_context_get_slice_type(clux_context_t *self,
                                         clux_type_t *base);
/* 创建胖指针切片值：data 布局为 { ptr, len }，借用 ptr 指向的内存。
   type 须是 CLUX_TYPE_SLICE（由调用方保证）。 */
clux_value_t *clux_context_create_slice(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t ptr,
                                        size_t len,
                                        const char *name,
                                        location_t *loc);
/* 读取切片长度：产出 usize 值；非切片类型返回 error，shadow 切片产出
   shadow usize。 */
clux_value_t *clux_context_slice_len(clux_context_t *self,
                                     clux_value_t *slice,
                                     location_t *loc);
/* 按 (base, len) 取 "[len]<base>" 定长数组类型：children 表 find-or-create，
   同一 (base, len) 恒等返回同一实例。 */
clux_type_t *clux_context_get_array_type(clux_context_t *self,
                                         clux_type_t *base,
                                         size_t len);
/* 创建数组值：N 个元素的连续内联块零初始化。type 须是
   CLUX_TYPE_ARRAY（由调用方保证）。 */
clux_value_t *clux_context_create_array(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        location_t *loc);
/* 指针解引用（*p）：返回 base 类型的 view（内存共享、携带引用属性），
   指针无效报 error，shadow 指针产出 base 类型的 shadow。
   写入由通用 assign 写穿 view 完成。 */
clux_value_t *
clux_context_deref(clux_context_t *self, clux_value_t *ptr, location_t *loc);
/* 数组/切片下标取项：返回 base 类型的 view（内存共享、携带引用属性）；
   越界/非整数下标报 error，任一操作数为 shadow 产出 base 类型的 shadow。
   写入由通用 assign 写穿 view 完成（无独立下标写原语）。 */
clux_value_t *clux_context_index(clux_context_t *self,
                                 clux_value_t *aggregate,
                                 clux_value_t *index,
                                 location_t *loc);
/* 下标值归一化为 int64：仅做整数类校验（负数与越界由调用方判定）。
   成功返回 NULL 并写入 *out，否则返回 error 值。 */
clux_value_t *clux_context_index_to_i64(clux_context_t *self,
                                        clux_value_t *index,
                                        int64_t *out);
clux_value_t *clux_context_create_str_value(clux_context_t *self,
                                            char *str,
                                            const char *name,
                                            location_t *loc);
clux_value_t *clux_context_create_error_message(clux_context_t *self,
                                                location_t *loc,
                                                const char *fmt,
                                                ...);
clux_value_t *clux_context_safe_cast(clux_context_t *self,
                                     clux_value_t *value,
                                     clux_type_t *target,
                                     location_t *loc);
clux_value_t *clux_context_cast(clux_context_t *self,
                                clux_value_t *value,
                                clux_type_t *target,
                                location_t *loc);
clux_value_t *clux_context_binary(clux_context_t *self,
                                  clux_binary_op_t op,
                                  clux_value_t *left,
                                  clux_value_t *right,
                                  location_t *loc);
clux_value_t *clux_context_prefix(clux_context_t *self,
                                  clux_prefix_op_t op,
                                  clux_value_t *value,
                                  location_t *loc);
clux_value_t *clux_context_add(clux_context_t *self,
                               clux_value_t *left,
                               clux_value_t *right,
                               location_t *loc);
/* 通用赋值（不经 vtable）：右值 safe_cast 成左值类型后 memcpy 穿透写入左值。
   返回左值本身；cast 失败返回 error，任一操作数为 error 自传播。 */
clux_value_t *clux_context_assign(clux_context_t *self,
                                  clux_value_t *target,
                                  clux_value_t *value,
                                  location_t *loc);
/* 通用克隆（不经 vtable）：data 是平凡位图，按类型尺寸新分配块整体拷贝。
   shadow 克隆为同类型 shadow，error 自传播。 */
clux_value_t *
clux_context_clone(clux_context_t *self, clux_value_t *value, location_t *loc);
clux_type_t *clux_context_get_i8_type(clux_context_t *ctx);
clux_type_t *clux_context_get_i16_type(clux_context_t *ctx);
clux_type_t *clux_context_get_i32_type(clux_context_t *ctx);
clux_type_t *clux_context_get_i64_type(clux_context_t *ctx);
clux_type_t *clux_context_get_u8_type(clux_context_t *ctx);
clux_type_t *clux_context_get_u16_type(clux_context_t *ctx);
clux_type_t *clux_context_get_u32_type(clux_context_t *ctx);
clux_type_t *clux_context_get_u64_type(clux_context_t *ctx);
clux_type_t *clux_context_get_isize_type(clux_context_t *ctx);
clux_type_t *clux_context_get_usize_type(clux_context_t *ctx);
clux_type_t *clux_context_get_f32_type(clux_context_t *ctx);
clux_type_t *clux_context_get_f64_type(clux_context_t *ctx);
clux_type_t *clux_context_get_bool_type(clux_context_t *ctx);
clux_type_t *clux_context_get_str_type(clux_context_t *ctx);
clux_type_t *clux_context_get_error_type(clux_context_t *ctx);
#ifdef __cplusplus
}
#endif
#endif
