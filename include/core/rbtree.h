#ifndef _H_CORE_RBTREE_
#define _H_CORE_RBTREE_
#include "core/allocator.h"
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _rbtree_t rbtree_t;
rbtree_t *create_rbtree(allocator_t *allocator, bool own);
void delete_rbtree(rbtree_t **tree);
size_t rbtree_get_size(rbtree_t *self);
/* 插入键值对，key 由调用方提供（按 uintptr_t 比较）。
   若 key 已存在且 own 为真，旧值被释放，新值替换。
   返回插入后的元素数，参数错误返回 (size_t)-1。 */
size_t rbtree_insert(rbtree_t *self, uintptr_t key, void *value);
/* 查找 key 对应的值，未找到返回 NULL。 */
void *rbtree_find(rbtree_t *self, uintptr_t key);
/* 删除 key 对应的节点，own 为真时释放关联值。
   返回删除后的元素数，key 不存在返回 (size_t)-1。 */
size_t rbtree_erase(rbtree_t *self, uintptr_t key);
/* 遍历：按 key 升序对每个元素调用 callback。 */
typedef void (*rbtree_callback_t)(uintptr_t key, void *value, void *user_data);
void rbtree_foreach(rbtree_t *self,
                    rbtree_callback_t callback,
                    void *user_data);
#ifdef __cplusplus
}
#endif
#endif
