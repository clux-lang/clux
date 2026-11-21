#ifndef _H_ENGINE_STRUCT_
#define _H_ENGINE_STRUCT_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* struct 字段：按声明序顺序布局（C 布局），所有字段同时有效
   （与 tag union 的单成员激活不同）。 */
typedef struct _clux_struct_field_t {
  const char *name; /* 借用调用方的驻留字符串，类型不拷贝不释放 */
  clux_type_t *type;
} clux_struct_field_t;
/* attribute（含 method）：类型命名空间成员（白皮书 §3.5 静态变量与
   §3.5.1 方法）。method 判定看声明形式——method 只是名单表里的名字，
   本体 func 值照常存 attribute 表（var fn = func(...){...} 是 attr 不是
   method）。value 借用调用方（context 路径由 scope 托管），类型不拷贝
   不释放。 */
typedef struct _clux_struct_attr_t {
  const char *name; /* 借用调用方的驻留字符串 */
  clux_value_t *value;
} clux_struct_attr_t;
/* subtype：类型的嵌套类型成员，本体是借用引用（不拷贝不释放）。 */
typedef struct _clux_struct_subtype_t {
  const char *name; /* 借用调用方的驻留字符串 */
  clux_type_t *type;
} clux_struct_subtype_t;
/* 声明体（创建时一次性传入，白皮书 §3.5）：fields 必填，attrs/methods/
   subtypes 可空（对应 count 为 0，指针可为 NULL）。method 名单里的名字
   须在 attrs 中有同名项且值为 func 类型（由 context 路径校验）。 */
typedef struct _clux_struct_decl_t {
  const clux_struct_field_t *fields;
  size_t field_count;
  const clux_struct_attr_t /* nullable */ *attrs;
  size_t attr_count;
  const char *const /* nullable */ *methods;
  size_t method_count;
  const clux_struct_subtype_t /* nullable */ *subtypes;
  size_t subtype_count;
} clux_struct_decl_t;
/* 创建具名 struct 类型：字段按声明序顺序平铺（各自按自身对齐取偏移，
   size 向类型对齐取整，align = max(字段对齐)，空字段表非法）。声明表
   整体拷入 meta（与 meta 连体单块分配），各名字由调用方保证生命周期
   （context 路径经 strs 驻留）。attribute/subtype 不参与 Data 布局
   （size 只由字段表决定）。 */
clux_type_t *create_clux_struct_type(allocator_t *allocator,
                                     const char *name,
                                     const clux_struct_decl_t *decl);
clux_vtable_t *get_clux_struct_vtable(void);
/* 两阶段构造的壳入口：只装字段表（三表留空待 install），字段名由调用方
   保证生命周期（context 路径经 strs 驻留）。字段表一次定局，布局即时
   seal——壳可安全作指针 base 或被方法签名引用，值位置不受影响（字段
   完整即完整）。字段表为空非法，由 context 路径校验拦截。 */
clux_type_t *create_clux_struct_type_shell(allocator_t *allocator,
                                           const char *name,
                                           const clux_struct_field_t *fields,
                                           size_t field_count);
/* 二次安装三表（attribute/method 名单/subtype）：两阶段构造的收尾步骤。
   字段表在 create 时一次定局（布局即时 seal），三表是类型命名空间成员
   不参与布局，允许在壳已按名 define 之后安装——适配方法签名引用 struct
   自身（func 形参类型急切求值时壳须已可见）。三表整体拷入独立单块
   （tables_block 持有，delete_meta 释放），各名字由调用方保证生命周期
   （context 路径经 strs 驻留）。只允许安装一次（全空也置位），已安装
   返回 false。 */
bool clux_struct_type_install(clux_type_t *self,
                              allocator_t *allocator,
                              const clux_struct_attr_t *attrs,
                              size_t attr_count,
                              const char *const *methods,
                              size_t method_count,
                              const clux_struct_subtype_t *subtypes,
                              size_t subtype_count);
/* 字段表查询：按声明序。 */
size_t clux_struct_field_count(clux_type_t *type);
const clux_struct_field_t *clux_struct_get_field(clux_type_t *type, size_t idx);
const clux_struct_field_t *clux_struct_find_field(clux_type_t *type,
                                                  const char *name);
/* 字段在 Data 内的偏移（偏移 0 起，按声明序平铺）。 */
size_t clux_struct_field_offset(clux_type_t *type, size_t idx);
/* attribute 表查询：按声明序，attr 与 method 本体共用此表。 */
size_t clux_struct_attr_count(clux_type_t *type);
const clux_struct_attr_t *clux_struct_get_attr(clux_type_t *type, size_t idx);
const clux_struct_attr_t *clux_struct_find_attr(clux_type_t *type,
                                                const char *name);
/* method 名单判定：名单表里的名字才是 method（声明形式判定），
   名单外同名 attr 只是普通 attribute。 */
bool clux_struct_is_method(clux_type_t *type, const char *name);
/* subtype 表查询。 */
size_t clux_struct_subtype_count(clux_type_t *type);
const clux_struct_subtype_t *clux_struct_get_subtype(clux_type_t *type,
                                                     size_t idx);
const clux_struct_subtype_t *clux_struct_find_subtype(clux_type_t *type,
                                                      const char *name);
/* 结构判等（四张表按声明序：字段名+类型、attribute 名+值类型、method
   名单、subtype 名+类型）：把已有 struct 类型与原始声明体对比，供
   context 创建路径做 find-or-dedup，无需先构造临时类型实例。 */
bool clux_struct_decls_equal(clux_type_t *self, const clux_struct_decl_t *decl);
#ifdef __cplusplus
}
#endif
#endif
