#ifndef _H_ENGINE_TYPE_
#define _H_ENGINE_TYPE_
#include "core/allocator.h"
#include "engine/interface.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* 类型无运行期 delete：生命周期与 context 相同，delete_clux_type 仅作
   teardown 递归入口。name 由类型对象持有（create 拷贝副本）。 */
clux_type_t *create_clux_type(allocator_t *allocator,
                              clux_type_kind_t kind,
                              const char *name,
                              size_t size,
                              size_t align,
                              clux_vtable_t /* borrow */ *vtable);
void delete_clux_type(clux_type_t **self);
clux_type_kind_t clux_type_get_kind(clux_type_t *self);
size_t clux_type_get_size(clux_type_t *self);
size_t clux_type_get_align(clux_type_t *self);
const char *clux_type_get_name(clux_type_t *self);
clux_vtable_t *clux_type_get_vtable(clux_type_t *self);
allocator_t *clux_type_get_allocator(clux_type_t *self);
void *clux_type_create_meta(clux_type_t *self, size_t meta_size);
void *clux_type_get_meta(clux_type_t *self);
/* children：基于本类型构造的复合类型，rbtree 按 kind 分桶，桶内存放同
   kind 的多个类型（如未来不同长度的 array）；父类型拥有 child，递归销毁
   时统一释放。 */
typedef bool (*clux_type_child_pred_t)(clux_type_t *child, void *user_data);
clux_type_t *clux_type_find_child(clux_type_t *self,
                                  clux_type_kind_t kind,
                                  clux_type_child_pred_t /* nullable */ pred,
                                  void /* nullable */ *user_data);
void clux_type_add_child(clux_type_t *self,
                         clux_type_kind_t kind,
                         clux_type_t /* transfer */ *child);
#ifdef __cplusplus
}
#endif
#endif
