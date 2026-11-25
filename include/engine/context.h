#ifndef _H_ENGINE_CONTEXT_
#define _H_ENGINE_CONTEXT_
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/safe_alloc.h"
#include "engine/bitfield.h"
#include "engine/cunion.h"
#include "engine/enum.h"
#include "engine/func.h"
#include "engine/interface.h"
#include "engine/interface_type.h"
#include "engine/module.h"
#include "engine/struct.h"
#include "engine/union.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_context_t *create_clux_context(allocator_t *allocator);
void delete_clux_context(clux_context_t **self);
allocator_t *clux_context_get_allocator(clux_context_t *self);
safe_allocator_t *clux_context_get_safe_allocator(clux_context_t *self);
clux_value_t *clux_context_create_value(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t data,
                                        const char *name,
                                        const location_t *loc);
void clux_context_push_scope(clux_context_t *self);
void clux_context_pop_scope(clux_context_t *self);
/* 当前作用域（current scope，借用）：所有带名/匿名值的注册点。
   供执行层做名称查找与作用域栈导航，只读使用。 */
clux_scope_t *clux_context_get_scope(clux_context_t *self);
/* 全局作用域（global scope，借用）：栈帧的词法挂链父——函数体自由名字
   只允许解析到形参、显式捕获与模块级名字，帧因此挂 global 而非调用方。 */
clux_scope_t *clux_context_get_global_scope(clux_context_t *self);
/* 作用域切换（栈帧协议核心）：将 current scope 换成 target 并返回原
   scope。调用边界据此完成「保存调用方作用域 → 进入帧 → 执行 → 恢复」，
   引擎不提供帧的建立/销毁语义（帧就是 scope，生命周期归执行层）。 */
clux_scope_t *clux_context_swap_scope(clux_context_t *self,
                                      clux_scope_t *target);
/* ---- 模块（白皮书 §8）----
   context 维护模块表（按名注册，hash 桶 + 精确匹配）与当前模块。
   运行时作用域链：current ... -> module->scope -> global——模块作用域
   由 create 时创建并挂 global 之下，生命周期归 scope 树，module 借用。
   按名注册，重名返回 NULL（先注册者保留）。node 为模块 AST 载荷
   （所有权归 module），可为 NULL。 */
clux_module_t *clux_context_create_module(clux_context_t *self,
                                          const char *name,
                                          /* 可为 NULL */ node_t *node);
/* 按名查找模块，未注册返回 NULL。 */
clux_module_t *clux_context_get_module(clux_context_t *self, const char *name);
/* 当前模块（借用）；无当前模块返回 NULL。 */
clux_module_t *clux_context_get_current_module(clux_context_t *self);
/* 进入/离开模块。进入非 NULL 模块时 current scope 切到模块作用域
   （模块级名字落于此，parent 链到 global，查找链自动成立）；
   传 NULL 离开模块，恢复 current scope 为 global。
   pop_scope 不会越过模块作用域（模块边界由本切换守护）。
   调用方须保证进入/离开按序平衡（借用约定）。 */
void clux_context_set_current_module(clux_context_t *self,
                                     clux_module_t *module);
/* 将匿名值具名化加入当前作用域（var 声明的落地通道）：
   值是当前作用域的匿名持有时原地填名（声明即 move，零拷贝）；
   值是借用（对既有变量的引用）时按值拷贝落地为独立存储；
   本层已有同名值时返回 duplicate declaration 错误值。 */
clux_value_t *clux_context_declare_value(clux_context_t *self,
                                         clux_value_t *value,
                                         const char *name,
                                         const location_t *loc);
clux_value_t *clux_context_lookup(clux_context_t *self,
                                  const char *name,
                                  const location_t *loc);
/* 消费校验：读取 value 前统一通道。TDZ（声明后未经首次赋值）报
   "use of a value before initialization"，其余原值返回。 */
clux_value_t *clux_context_consume_value(clux_context_t *self,
                                         clux_value_t *value,
                                         const location_t *loc);
/* TDZ 变量声明（var x: T = undefined）：分配 T 的未初始化存储（清零）
   并标记 TDZ。首赋值经 clux_context_assign 清除标记（绕过 mutable
   检查），首赋值前消费报错。同名冲突返回 duplicate declaration 错误。 */
clux_value_t *clux_context_create_tdz_value(clux_context_t *self,
                                            clux_type_t *type,
                                            const char *name,
                                            const location_t *loc);
clux_value_t *clux_context_create_error(clux_context_t *self,
                                        clux_error_t *error);
clux_value_t *clux_context_create_shadow_value(clux_context_t *self,
                                               clux_type_t *type);
/* shadow 模式（块提升的类型推断专用）：开启后 create_value 一律产出
   无存储、无具名的 shadow 值（error 值除外——错误须真实传播），invoke
   拦截 func 分派、直接产出返回类型的 shadow（不执行函数体，杜绝副作用）。
   调用方负责保存旧值并在推断结束后恢复。 */
void clux_context_set_shadow_mode(clux_context_t *self, bool enabled);
bool clux_context_is_shadow_mode(clux_context_t *self);
clux_value_t *clux_context_create_i8(clux_context_t *self,
                                     int8_t value,
                                     const char *name,
                                     const location_t *loc);
clux_value_t *clux_context_create_i16(clux_context_t *self,
                                      int16_t value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_i32(clux_context_t *self,
                                      int32_t value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_i64(clux_context_t *self,
                                      int64_t value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_u8(clux_context_t *self,
                                     uint8_t value,
                                     const char *name,
                                     const location_t *loc);
clux_value_t *clux_context_create_u16(clux_context_t *self,
                                      uint16_t value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_u32(clux_context_t *self,
                                      uint32_t value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_u64(clux_context_t *self,
                                      uint64_t value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_isize(clux_context_t *self,
                                        intptr_t value,
                                        const char *name,
                                        const location_t *loc);
clux_value_t *clux_context_create_usize(clux_context_t *self,
                                        size_t value,
                                        const char *name,
                                        const location_t *loc);
clux_value_t *clux_context_create_f32(clux_context_t *self,
                                      float value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_f64(clux_context_t *self,
                                      double value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_bool(clux_context_t *self,
                                       bool value,
                                       const char *name,
                                       const location_t *loc);
/* type value（值描述类型）：data 存被描述类型的指针；type 须非空，
   否则返回 error。eq / value_extends 代理类型层（见 type_value.c）。 */
clux_value_t *clux_context_create_type_value(clux_context_t *self,
                                             clux_type_t *type,
                                             const char *name,
                                             const location_t *loc);
/* 模板值（type_template / func_template）：模板不是类型，是值——data 只存
   原始 AST 指针，参数表每次实例化时从 AST 重新计算。ast 须非空，否则
   返回 error。模板值经 scope define 进词法命名空间（与变量/类型共用，
   遮罩规则自然生效）。 */
clux_value_t *clux_context_create_type_template(clux_context_t *self,
                                                void *ast,
                                                const char *name,
                                                const location_t *loc);
clux_value_t *clux_context_create_func_template(clux_context_t *self,
                                                void *ast,
                                                const char *name,
                                                const location_t *loc);
/* 模板值取回原始 AST（两种模板通用）：非模板值返回 NULL。 */
void *clux_context_template_ast(clux_value_t *value);
char *clux_context_alloc_str(clux_context_t *self, size_t len);
clux_value_t *clux_context_create_str(clux_context_t *self,
                                      const char *value,
                                      const char *name,
                                      const location_t *loc);
clux_value_t *clux_context_create_ptr(clux_context_t *self,
                                      clux_type_t *type,
                                      ptr_handle_t value,
                                      const char *name,
                                      const location_t *loc);
/* 取指向 value 的指针：error 自传播，shadow 产出 "*<base>" 类型的 shadow。
   任意类型可用，不经过 vtable。 */
clux_value_t *clux_context_ref(clux_context_t *self,
                               clux_value_t *value,
                               const location_t *loc);
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
                                        const location_t *loc);
/* 读取切片长度：产出 usize 值；非切片类型返回 error，shadow 切片产出
   shadow usize。 */
clux_value_t *clux_context_slice_len(clux_context_t *self,
                                     clux_value_t *slice,
                                     const location_t *loc);
/* 按 (base, len) 取 "[len]<base>" 定长数组类型：children 表 find-or-create，
   同一 (base, len) 恒等返回同一实例。 */
clux_type_t *clux_context_get_array_type(clux_context_t *self,
                                         clux_type_t *base,
                                         size_t len);
/* 取元组类型（白皮书 §2.6）<>：元素类型序列结构去重——按声明序一致
   时恒等返回，类型由 context 的具名类型表托管释放。空元组合法；
   入参非法（count>0 而 elems 为 NULL / 元素缺类型）返回 NULL 并记录
   error。 */
clux_type_t *clux_context_get_tuple_type(clux_context_t *self,
                                         clux_type_t **elems,
                                         size_t count);
/* 创建元组值：元素类型序列结构去重取类型后作值类型，data 是全部元素
   按布局平铺的连续内联块（零初始化，逐元素经 index view + assign 写入）。
   type 须是 CLUX_TYPE_TUPLE。 */
clux_value_t *clux_context_create_tuple(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        const location_t *loc);
/* 创建数组值：N 个元素的连续内联块零初始化。type 须是
   CLUX_TYPE_ARRAY（由调用方保证）。 */
clux_value_t *clux_context_create_array(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        const location_t *loc);
/* 指针解引用（*p）：返回 base 类型的 view（内存共享、携带引用属性），
   指针无效报 error，shadow 指针产出 base 类型的 shadow。
   写入由通用 assign 写穿 view 完成。 */
clux_value_t *clux_context_deref(clux_context_t *self,
                                 clux_value_t *ptr,
                                 const location_t *loc);
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
                                                const location_t *loc);
/* #none(T) -> ?T{none}：T 是载荷类型，内部提升为 ?T（零值）。 */
clux_value_t *clux_context_create_optional_none(clux_context_t *self,
                                                clux_type_t *base,
                                                const char *name,
                                                const location_t *loc);
/* 判空查询（内置函数 #is_none）：none 返回 true。仅 optional 类型
   支持（vtable is_none 槽位），其余类型返回 error。 */
clux_value_t *clux_context_is_none(clux_context_t *self,
                                   clux_value_t *value,
                                   const location_t *loc);
/* 断言取值（内置函数 #take，`.!` 语法糖编译为此调用）：some 返回
   载荷 base 类型的借引用 view，none 是运行期 error。仅 optional 类型
   支持（vtable take 槽位），其余类型返回 error。 */
clux_value_t *clux_context_take(clux_context_t *self,
                                clux_value_t *value,
                                const location_t *loc);
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
                                         const location_t *loc);
/* 成员读取（x.field）：返回成员类型的 view（内存共享、携带引用属性），
   未知成员报 error，shadow 值产出成员类型的 shadow。写入由通用 assign
   写穿 view 完成。仅绑定 field 槽位的类型支持（当前 cunion 与 tag
   union），其余类型返回 error。 */
clux_value_t *clux_context_field(clux_context_t *self,
                                 clux_value_t *value,
                                 const char *field,
                                 const location_t *loc);
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
/* 块提升的两阶段空壳（union/cunion 同构，is_cunion 分派）：预建未 seal
   的壳并托管进 context，具名 define 与成员补全由提升调用方完成。 */
clux_type_t *clux_context_create_union_shell(clux_context_t *self,
                                             const char *name,
                                             bool is_cunion);
/* 创建 tag union 零值（memset 清零，tag=0 即第一个成员）：type 须是
   CLUX_TYPE_UNION。切换成员走 clux_context_union_set_member。 */
clux_value_t *clux_context_create_union(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        const location_t *loc);
/* 创建具名 enum 类型（白皮书 §3.6）：布局与底层类型（整数或 str）一致，
   variant 表整体拷入 meta，variant 名与 str 常量经 strs 驻留。结构判等
   （底层类型 + 成员名 + 成员值按声明序一致即同一类型，str 按内容）：
   已存在结构相同的实例时恒等返回之，类型由 context 的具名类型表托管
   释放。入参非法（缺名/缺底层类型/底层非整数且非 str/空成员表/成员
   缺名/重名/str 底层缺字符串常量）返回 NULL 并记录 error。 */
clux_type_t *clux_context_create_enum_type(clux_context_t *self,
                                           const char *name,
                                           clux_type_t *underlying,
                                           const clux_enum_variant_t *variants,
                                           size_t count);
/* 创建具名 bitfield 类型（白皮书 §10）：底层仅 u8/u16/u32/u64，字段
   按 LSB 起声明序连续排布，宽度合法范围 1..底层位宽，总位数可超底层
   字宽（跨字访问由语言定义）。布局按隐藏存储数组（ceil(total_bits/
   word_bits) 个底层字）。字段名经 strs 驻留。结构判等（底层类型 +
   字段名+宽度按声明序一致即同一类型）：已存在结构相同的实例时恒等
   返回之。入参非法（缺底层类型/底层非 u8/u16/u32/u64/空字段表/
   字段缺名/重名/宽度超界）返回 NULL 并记录 error。 */
clux_type_t *
clux_context_create_bitfield_type(clux_context_t *self,
                                  const char *name,
                                  clux_type_t *underlying,
                                  const clux_bitfield_field_def_t *fields,
                                  size_t count);
/* 创建 bitfield 零值（memset 清零——缺失字段补零语义的落点）：
   type 须是 CLUX_TYPE_BITFIELD。字段写入走
   clux_context_bitfield_set_field（构造通道）。 */
clux_value_t *clux_context_create_bitfield(clux_context_t *self,
                                           clux_type_t *type,
                                           const char *name,
                                           const location_t *loc);
/* 字段写入（构造通道，白皮书 §10）：value 须已是底层类型（调用方
   先 safe_cast），引擎按字段宽度截断掩码 RMW 写入存储。未知字段返回
   语义化 error，shadow 值原样返回。 */
clux_value_t *clux_context_bitfield_set_field(clux_context_t *self,
                                              clux_value_t *value,
                                              const char *field,
                                              clux_value_t *rhs,
                                              const location_t *loc);
/* 创建具名 interface 类型（白皮书 §3.10、§6.9）：纯编译期结构约束——
   成员是方法签名（名 + func 类型，无捕获），不产生运行期对象/虚表。
   永不 seal（与 void 同族天然不完整）：create_value 的 seal 门禁由此
   拒绝 interface 作值类型（变量创建/类型标注等值位置）。成员名经 strs
   驻留。结构判等（成员名+签名按声明序一致即同一类型）：已存在结构相同
   的实例时恒等返回之，类型由 context 的具名类型表托管释放。入参非法
   （缺名/空成员表/成员缺名缺签名/签名非 func/签名带捕获/重名）返回
   NULL 并记录 error。 */
clux_type_t *
clux_context_create_interface_type(clux_context_t *self,
                                   const char *name,
                                   const clux_interface_member_t *members,
                                   size_t count);
/* 激活成员：切换 tag 为该成员声明序号并清零 payload，返回原值；未知
   成员返回语义化 error，shadow 值原样返回（无数据可变）。仅 tag union
   支持，其余类型返回 error。 */
clux_value_t *clux_context_union_set_member(clux_context_t *self,
                                            clux_value_t *value,
                                            const char *member,
                                            const location_t *loc);
/* is 判定（value is Member，白皮书 §5.7）：当前 tag 即该成员声明序号时
   为 true，结果为 bool。仅 tag union 支持（vtable is_member 槽位），
   其余类型返回 error。 */
clux_value_t *clux_context_is_member(clux_context_t *self,
                                     clux_value_t *value,
                                     const char *member,
                                     const location_t *loc);
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
                                       const location_t *loc);
/* 挂接函数体 AST 指针（借用，不拥有）：返回原值；非 func 返回语义化
   error，shadow 值原样返回（无数据可变）。 */
clux_value_t *clux_context_func_set_ast(clux_context_t *self,
                                        clux_value_t *value,
                                        void *ast,
                                        const location_t *loc);
/* 捕获读取（f.capture）：func 捕获走独立 API（不占 field 槽位语义），
   返回捕获类型的读写双向 view（内存共享、携带引用属性），写入由通用
   assign 写穿 view 完成；未知捕获返回语义化 error，shadow 值产出
   捕获类型的 shadow。仅 func 支持，其余类型返回 error。 */
clux_value_t *clux_context_func_capture(clux_context_t *self,
                                        clux_value_t *value,
                                        const char *capture,
                                        const location_t *loc);
/* 调用（f(args)）：按签名校验实参个数与类型，error 自传播；shadow
   函数或任一 shadow 实参产出返回类型的 shadow；校验通过的真函数走
   执行器回调（见 clux_context_set_executor）；无可执行环境（未装
   执行器或函数体未挂 AST）是运行期 error。仅 func 支持（vtable
   invoke 槽位），其余类型返回 error。 */
clux_value_t *clux_context_invoke(clux_context_t *self,
                                  clux_value_t *value,
                                  size_t argc,
                                  clux_value_t *argv[],
                                  const location_t *loc);
/* AST 执行器（栈帧协议的入口）：clux_context_invoke 对校验通过的真函
   数回调此处——engine 只透传 AST 指针与函数值/实参，不解析 AST；帧的
   建立（挂 global 的临时 scope）、形参/捕获绑定、return signal 消费、
   跨帧 clone 全部由执行层实现。engine 不依赖 exec 的分层借此保住。 */
typedef clux_value_t *(*clux_ast_executor_t)(clux_context_t *ctx,
                                             void *ast,
                                             clux_value_t *func,
                                             size_t argc,
                                             clux_value_t *argv[],
                                             const location_t *loc);
void clux_context_set_executor(clux_context_t *self,
                               clux_ast_executor_t executor);
clux_ast_executor_t clux_context_get_executor(clux_context_t *self);
/* 调用深度护栏：tree-walking 每层函数调用嵌一层 C 栈，无限递归等于
   不可诊断的栈溢出——执行器进入/退出调用边界时 enter/leave 配对，超限
   的 enter 返回 false（执行层报 maximum call depth exceeded）。上限
   默认 1024，环境变量 CLUX_MAX_CALL_DEPTH 覆盖（与内存预算护栏正交）。 */
bool clux_context_call_depth_enter(clux_context_t *self);
void clux_context_call_depth_leave(clux_context_t *self);
size_t clux_context_get_call_depth(clux_context_t *self);
/* 泛型实例化深度护栏：实例化是对模板 AST 的递归重执行，无界递归实例化
   （如模板体内再实例化自身）等于栈溢出——执行器进入/退出实例化边界时
   enter/leave 配对，超限的 enter 返回 false（执行层报 maximum generic
   instantiation depth exceeded）。上限默认 256，环境变量
   CLUX_MAX_INSTANCE_DEPTH 覆盖（与调用深度护栏正交）。 */
bool clux_context_instance_depth_enter(clux_context_t *self);
void clux_context_instance_depth_leave(clux_context_t *self);
size_t clux_context_get_instance_depth(clux_context_t *self);
/* return signal（栈帧协议的展开通道）：return 语句产出信号值（载荷为
   返回值的借用），语句层原样上抛，唯一消费点是调用表达式的帧边界；
   逃逸到调用边界之外由执行层报 "return outside of a function"。 */
clux_value_t *clux_context_create_return_signal(clux_context_t *self,
                                                clux_value_t *payload);
bool clux_value_is_return_signal(clux_value_t *value);
/* 信号载荷读取（借用）：非 return signal 返回 NULL。 */
clux_value_t *clux_return_signal_get_payload(clux_value_t *signal);
/* break/continue signal（循环跳转的展开通道）：break/continue 语句产出
   信号值（无载荷），语句层原样上抛，唯一消费点是最近一层循环边界；
   逃逸到循环边界之外由执行层报 "break/continue outside of a loop"。
   无载荷即零尺寸值（data=0），与空 tuple 同族。 */
clux_value_t *clux_context_create_break_signal(clux_context_t *self);
bool clux_value_is_break_signal(clux_value_t *value);
clux_value_t *clux_context_create_continue_signal(clux_context_t *self);
bool clux_value_is_continue_signal(clux_value_t *value);
/* 创建具名 struct 类型（白皮书 §3.5）：声明体一次性传入——字段表必填，
   attribute（含 method 本体）/method 名单/subtype 三表可选。字段按声明
   序顺序平铺（C 布局），所有字段同时有效；attribute/method/subtype 是
   类型命名空间成员，不参与 Data 布局。声明表整体拷入 meta，各名字经
   strs 驻留。结构判等（四张表按声明序：字段名+类型、attribute 名+值类
   型、method 名单、subtype 名+类型）：已存在结构相同的实例时恒等返回
   之，类型由 context 的具名类型表托管释放。入参非法（缺名/空字段表/字
   段缺名缺类型/重名/attribute 缺名缺值/重名/method 缺名/重名/不在
   attribute 表/对应值非 func/subtype 缺名缺类型/重名）返回 NULL 并记录
   error。 */
clux_type_t *clux_context_create_struct_type(clux_context_t *self,
                                             const char *name,
                                             const clux_struct_decl_t *decl);
/* struct 两阶段构造（适配方法签名引用 struct 自身——方法形参类型急切求值
   时字段壳须已按名 define）：begin 建字段壳（布局即时 seal，三表留空，
   不做结构去重）并托管；install 是收尾（三表校验+驻留+安装，只允许
   一次）。字段校验与一次性通道同构，入参非法返回 NULL/记录 error。 */
clux_type_t *clux_context_begin_struct_type(clux_context_t *self,
                                            const char *name,
                                            const clux_struct_field_t *fields,
                                            size_t field_count);
bool clux_context_install_struct_tables(clux_context_t *self,
                                        clux_type_t *type,
                                        const clux_struct_attr_t *attrs,
                                        size_t attr_count,
                                        const char *const *methods,
                                        size_t method_count,
                                        const clux_struct_subtype_t *subtypes,
                                        size_t subtype_count);
/* 创建 struct 零值（memset 清零，全部字段同时有效）：type 须是
   CLUX_TYPE_STRUCT。字段读写走通用 field（写穿 view）。 */
clux_value_t *clux_context_create_struct(clux_context_t *self,
                                         clux_type_t *type,
                                         const char *name,
                                         const location_t *loc);
/* attribute/method 读取（T::a / a.b 的类型命名空间形态）：返回成员值本
   身（借用，不拷贝）——method 也是 func 值，拿到后经 clux_context_invoke
   调用并自行将 receiver 作首参（白皮书 §3.5.1：实例调用 a.b(args) 是
   T::b(a, args) 的语法糖）。非 struct 类型或未知成员返回语义化 error。 */
clux_value_t *clux_context_struct_attr(clux_context_t *self,
                                       clux_type_t *type,
                                       const char *name,
                                       const location_t *loc);
/* subtype 读取（T::S）：返回嵌套类型本体（借用）。非 struct 类型或未知
   成员返回 NULL 并记录 error。 */
clux_type_t *clux_context_struct_subtype(clux_context_t *self,
                                         clux_type_t *type,
                                         const char *name,
                                         const location_t *loc);
/* 类型命名空间成员访问（Type::member，白皮书 §3.5/§3.5.1/§3.6）：
   struct 按 attribute 表（含 method 本体 func 值，借用返回）→ subtype
   （封装 type value 返回）顺序解析；enum 按 variant 表产出该 enum 类型
   的值（整数底层按位型截断、str 底层取驻留常量）。qualifier 包装不
   遮蔽命名空间。其余类型或未知成员返回语义化 error。 */
clux_value_t *clux_context_get_member(clux_context_t *self,
                                      clux_type_t *type,
                                      const char *name,
                                      const location_t *loc);
/* 数组/切片下标取项：返回 base 类型的 view（内存共享、携带引用属性）；
   越界/非整数下标报 error，任一操作数为 shadow 产出 base 类型的 shadow。
   写入由通用 assign 写穿 view 完成（无独立下标写原语）。 */
clux_value_t *clux_context_index(clux_context_t *self,
                                 clux_value_t *aggregate,
                                 clux_value_t *index,
                                 const location_t *loc);
/* 切片取段 base[lo?:hi?]：lo/hi 为 NULL 表示开放端点（缺省 0 / 长度）。
   返回元素类型的切片值（[]T 胖指针，借用 base 内存）；负数、越界、
   lo > hi 报 error，任一操作数为 shadow 产出切片类型的 shadow。 */
clux_value_t *clux_context_slice(clux_context_t *self,
                                 clux_value_t *aggregate,
                                 clux_value_t *lo,
                                 clux_value_t *hi,
                                 const location_t *loc);
/* 下标值归一化为 int64：仅做整数类校验（负数与越界由调用方判定）。
   成功返回 NULL 并写入 *out，否则返回 error 值。 */
clux_value_t *clux_context_index_to_i64(clux_context_t *self,
                                        clux_value_t *index,
                                        int64_t *out);
clux_value_t *clux_context_create_str_value(clux_context_t *self,
                                            char *str,
                                            const char *name,
                                            const location_t *loc);
clux_value_t *clux_context_create_error_message(clux_context_t *self,
                                                const location_t *loc,
                                                const char *fmt,
                                                ...);
clux_value_t *clux_context_safe_cast(clux_context_t *self,
                                     clux_value_t *value,
                                     clux_type_t *target,
                                     const location_t *loc);
clux_value_t *clux_context_cast(clux_context_t *self,
                                clux_value_t *value,
                                clux_type_t *target,
                                const location_t *loc);
clux_value_t *clux_context_binary(clux_context_t *self,
                                  clux_binary_op_t op,
                                  clux_value_t *left,
                                  clux_value_t *right,
                                  const location_t *loc);
clux_value_t *clux_context_prefix(clux_context_t *self,
                                  clux_prefix_op_t op,
                                  clux_value_t *value,
                                  const location_t *loc);
clux_value_t *clux_context_add(clux_context_t *self,
                               clux_value_t *left,
                               clux_value_t *right,
                               const location_t *loc);
/* 通用赋值（不经 vtable）：右值 safe_cast 成左值类型后 memcpy 穿透写入左值。
   返回左值本身；cast 失败返回 error，任一操作数为 error 自传播。 */
clux_value_t *clux_context_assign(clux_context_t *self,
                                  clux_value_t *target,
                                  clux_value_t *value,
                                  const location_t *loc);
/* 通用克隆（不经 vtable）：data 是平凡位图，按类型尺寸新分配块整体拷贝。
   shadow 克隆为同类型 shadow，error 自传播。 */
clux_value_t *clux_context_clone(clux_context_t *self,
                                 clux_value_t *value,
                                 const location_t *loc);
/* 内建类型按 kind 取回：引擎内部与测试的通用通道（非逐类型接口）。
   script 层类型名与 identifier 合一——内建类型作为 type value 按名
   注册进 global scope，经 context_lookup 按名查找，与用户类型/变量
   共用同一查找通道（白皮书 §2.1）。注意：这只是底层机制上的统一，
   sema 阶段仍会拒绝对内建类型名的遮蔽。 */
clux_type_t *clux_context_builtin_type(clux_context_t *ctx,
                                       clux_type_kind_t kind);
/* "type" 内建类型：所有 type value 的类型（context 单例）。 */
clux_type_t *clux_context_get_type_type(clux_context_t *ctx);
/* "type_template" / "func_template" 内建类型：模板值的类型（context 单例）。
   vtable 完全留空——模板是"参数表 + 原始 AST"的具名载体，实例化由上层
   取回 AST 后展开，engine 不参与模板语义。 */
clux_type_t *clux_context_get_type_template_type(clux_context_t *ctx);
clux_type_t *clux_context_get_func_template_type(clux_context_t *ctx);
clux_type_t *clux_context_get_error_type(clux_context_t *ctx);
#ifdef __cplusplus
}
#endif
#endif
