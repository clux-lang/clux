#ifndef _H_ENGINE_SCOPE_
#define _H_ENGINE_SCOPE_
#include "core/allocator.h"
#include "core/vec.h"
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
clux_scope_t *create_clux_scope(allocator_t *allocator, clux_scope_t *parent);
void delete_clux_scope(clux_scope_t **self);
clux_value_t *
clux_scope_define(clux_scope_t *self, const char *name, clux_value_t *value);
clux_value_t *clux_scope_get(clux_scope_t *self, const char *name);
/* 本层判重（不走 parent 链，外层名字允许遮蔽）：本层已定义 name 返回
   true。sema 局部判重用——clux_scope_get 是遮蔽语义（沿链查），无法
   只回答"本层有没有"。 */
bool clux_scope_defined_here(clux_scope_t *self, const char *name);
clux_scope_t *clux_scope_get_parent(clux_scope_t *self);

/* 将本作用域持有的匿名值具名化：原地给 owning slot 填名并挂入名称
   映射，零拷贝（声明即 move：初值产出的临时所有权转入变量名下）。
   返回值区分失败原因：
   - CONFLICT：本层（不沿 parent 链，外层名字可遮罩）已存在同名值；
   - BORROWED：value 不是本作用域的匿名持有（已被具名或注册在别处），
     借用语义——调用方需要独立存储时应按值拷贝落地。 */
typedef enum {
  CLUX_SCOPE_NAME_OK = 0,
  CLUX_SCOPE_NAME_CONFLICT,
  CLUX_SCOPE_NAME_BORROWED,
} clux_scope_name_result_t;
clux_scope_name_result_t clux_scope_name_value(clux_scope_t *self,
                                               clux_value_t *value,
                                               const char *name);

/* defer 栈：登记离开作用域时 LIFO 调用的延期体（func value 的借用
   指针，值本体由本作用域 values 持有，栈本身只管顺序不管生命周期；
   using 语法糖与 defer 共用本序列）。 */
void clux_scope_add_defer(clux_scope_t *self, clux_value_t *defer);
vec_t *clux_scope_get_defers(clux_scope_t *self);
#ifdef __cplusplus
}
#endif
#endif
