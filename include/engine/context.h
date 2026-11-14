#ifndef _H_ENGINE_CONTEXT_
#define _H_ENGINE_CONTEXT_
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/safe_alloc.h"
#include "engine/cunion.h"
#include "engine/func.h"
#include "engine/interface.h"
#include "engine/struct.h"
#include "engine/union.h"
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
/* 按载荷 base 取 "?<base>" 可选类型：children 表 find-or-create，
   同一 base 恒等返回同一实例。 */
clux_type_t *clux_context_get_optional_type(clux_context_t *self,
                                            clux_type_t *base);
/* #some(T, v) -> ?T{v}：T 是载荷类型，内部提升为 ?T。
   value 类型须与 T 语义相等，error 自传播，不满足返回语义化 error。
   适用于返回值/字段/参数/变量等任何初始化位置。 */
clux_value_t *clux_context_create_optional_some(clux_context_t *self,
                                                clux_type_t *base,
                                                clux_value_t *value,
                                                const char *name,
                                                location_t *loc);
/* #none(T) -> ?T{none}：T 是载荷类型，内部提升为 ?T（零值）。 */
clux_value_t *clux_context_create_optional_none(clux_context_t *self,
                                                clux_type_t *base,
                                                const char *name,
                                                location_t *loc);
/* 判空查询（内置函数 #is_none）：none 返回 true。仅 optional 类型
   支持（vtable is_none 槽位），其余类型返回 error。 */
clux_value_t *clux_context_is_none(clux_context_t *self,
                                   clux_value_t *value,
                                   location_t *loc);
/* 断言取值（内置函数 #take，`.!` 语法糖编译为此调用）：some 返回
   载荷 base 类型的借引用 view，none 是运行期 error。仅 optional 类型
   支持（vtable take 槽位），其余类型返回 error。 */
clux_value_t *
clux_context_take(clux_context_t *self, clux_value_t *value, location_t *loc);
/* 创建具名 C union 类型（白皮书 §3.8）：无 tag，所有成员共享偏移 0，
   size/align 取成员最大值（C ABI 布局）。成员表整体拷入 meta，成员名经
   strs 驻留。结构判等（成员名+类型按声明序一致即同一类型）：已存在
   结构相同的实例时恒等返回之，类型由 context 的具名类型表托管释放。
   入参非法（缺名/空成员表/成员缺名缺类型/重名）返回 NULL 并记录 error。 */
clux_type_t *
clux_context_create_cunion_type(clux_context_t *self,
                                const char *name,
                                const clux_cunion_member_t *members,
                                size_t count);
/* 创建 cunion 零值（memset 清零）：type 须是 CLUX_TYPE_CUNION。 */
clux_value_t *clux_context_create_cunion(clux_context_t *self,
                                         clux_type_t *type,
                                         const char *name,
                                         location_t *loc);
/* 成员读取（x.field）：返回成员类型的 view（内存共享、携带引用属性），
   未知成员报 error，shadow 值产出成员类型的 shadow。写入由通用 assign
   写穿 view 完成。仅绑定 field 槽位的类型支持（当前 cunion 与 tag
   union），其余类型返回 error。 */
clux_value_t *clux_context_field(clux_context_t *self,
                                 clux_value_t *value,
                                 const char *field,
                                 location_t *loc);
/* 创建具名 tag union 类型（白皮书 §3.7）：tag 为定宽 u32（偏移 0），
   payload 从类型对齐处开始，同一时刻只有一个 member 有效。成员表整体
   拷入 meta，成员名经 strs 驻留。结构判等（成员名+类型按声明序一致即
   同一类型）：已存在结构相同的实例时恒等返回之，类型由 context 的具名
   类型表托管释放。入参非法（缺名/空成员表/成员缺名缺类型/重名）返回
   NULL 并记录 error。 */
clux_type_t *clux_context_create_union_type(clux_context_t *self,
                                            const char *name,
                                            const clux_union_member_t *members,
                                            size_t count);
/* 创建 tag union 零值（memset 清零，tag=0 即第一个成员）：type 须是
   CLUX_TYPE_UNION。切换成员走 clux_context_union_set_member。 */
clux_value_t *clux_context_create_union(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        location_t *loc);
/* 激活成员：切换 tag 为该成员声明序号并清零 payload，返回原值；未知
   成员返回语义化 error，shadow 值原样返回（无数据可变）。仅 tag union
   支持，其余类型返回 error。 */
clux_value_t *clux_context_union_set_member(clux_context_t *self,
                                            clux_value_t *value,
                                            const char *member,
                                            location_t *loc);
/* is 判定（value is Member，白皮书 §5.7）：当前 tag 即该成员声明序号时
   为 true，结果为 bool。仅 tag union 支持（vtable is_member 槽位），
   其余类型返回 error。 */
clux_value_t *clux_context_is_member(clux_context_t *self,
                                     clux_value_t *value,
                                     const char *member,
                                     location_t *loc);
/* 创建 func 类型（白皮书 §2.7）：签名 = 捕获表 + 参数类型序列 + 返回
   类型。捕获列表是类型的一部分——func|a:i32|(i32)->i32 与
   func(i32)->i32 是不同类型；捕获项默认值属于函数体（AST），不参与
   类型判等。结构去重：捕获表+参数序列+返回类型一致时恒等返回，类型由
   context 的具名类型表托管释放。入参非法（缺名/缺返回类型/捕获缺名缺
   类型/重名/参数缺类型）返回 NULL 并记录 error。 */
clux_type_t *clux_context_create_func_type(clux_context_t *self,
                                           const char *name,
                                           const clux_func_capture_t *captures,
                                           size_t capture_count,
                                           clux_type_t **params,
                                           size_t param_count,
                                           clux_type_t *return_type);
/* 创建 func 零值（函数体 AST 指针为 NULL，捕获字段清零）：type 须是
   CLUX_TYPE_FUNC。函数体经 clux_context_func_set_ast 挂接。 */
clux_value_t *clux_context_create_func(clux_context_t *self,
                                       clux_type_t *type,
                                       const char *name,
                                       location_t *loc);
/* 挂接函数体 AST 指针（借用，不拥有）：返回原值；非 func 返回语义化
   error，shadow 值原样返回（无数据可变）。 */
clux_value_t *clux_context_func_set_ast(clux_context_t *self,
                                        clux_value_t *value,
                                        void *ast,
                                        location_t *loc);
/* 捕获读取（f.capture）：func 捕获走独立 API（不占 field 槽位语义），
   返回捕获类型的读写双向 view（内存共享、携带引用属性），写入由通用
   assign 写穿 view 完成；未知捕获返回语义化 error，shadow 值产出
   捕获类型的 shadow。仅 func 支持，其余类型返回 error。 */
clux_value_t *clux_context_func_capture(clux_context_t *self,
                                        clux_value_t *value,
                                        const char *capture,
                                        location_t *loc);
/* 调用（f(args)）：按签名校验实参个数与类型，error 自传播；shadow
   函数或任一 shadow 实参产出返回类型的 shadow；校验通过的真函数没有
   可执行环境（AST 体的执行由编译流水线后端接线）是运行期 error。
   仅 func 支持（vtable invoke 槽位），其余类型返回 error。 */
clux_value_t *clux_context_invoke(clux_context_t *self,
                                  clux_value_t *value,
                                  size_t argc,
                                  clux_value_t *argv[],
                                  location_t *loc);
/* 创建具名 struct 类型（白皮书 §3-declarations，第一阶段仅字段）：
   字段按声明序顺序平铺（C 布局），所有字段同时有效。字段表整体拷入
   meta，字段名经 strs 驻留。结构判等（字段名+类型按声明序一致即同一
   类型）：已存在结构相同的实例时恒等返回之，类型由 context 的具名
   类型表托管释放。入参非法（缺名/空字段表/字段缺名缺类型/重名）返回
   NULL 并记录 error。 */
clux_type_t *clux_context_create_struct_type(clux_context_t *self,
                                             const char *name,
                                             const clux_struct_field_t *fields,
                                             size_t count);
/* 创建 struct 零值（memset 清零，全部字段同时有效）：type 须是
   CLUX_TYPE_STRUCT。字段读写走通用 field（写穿 view）。 */
clux_value_t *clux_context_create_struct(clux_context_t *self,
                                         clux_type_t *type,
                                         const char *name,
                                         location_t *loc);
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
