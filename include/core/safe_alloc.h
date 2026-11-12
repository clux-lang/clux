#ifndef _H_CORE_SAFE_ALLOC_
#define _H_CORE_SAFE_ALLOC_
#include "core/allocator.h"
#include "core/location.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* 基于句柄的内存安全分配器。
   句柄是 uintptr_t 大小的不透明值，低半部分为槽位下标，高半部分为代际；
   释放后代际递增，旧句柄立即失效，可用于 UAF 检查。
   所有接口为单线程设计。 */
typedef uintptr_t ptr_handle_t;
typedef struct _safe_allocator_t safe_allocator_t;

/* 活跃分配实体的快照，供泄露检查。 */
typedef struct _safe_alloc_info_t {
  ptr_handle_t handle;
  void *base;
  size_t size;
  bool has_location;
  location_t location;
} safe_alloc_info_t;

safe_allocator_t *create_safe_allocator(allocator_t *allocator);
/* 释放该分配器仍持有的全部实体并销毁自身。 */
void delete_safe_allocator(safe_allocator_t **self);

/* 分配 size 字节；location 可为 NULL，非 NULL 时记录分配点。
   size 为 0 或分配失败时返回 0。 */
ptr_handle_t
safe_alloc(safe_allocator_t *self, size_t size, const location_t *location);
/* 只接受根句柄；view 句柄、过期句柄、重复释放均返回 false。 */
bool safe_free(ptr_handle_t handle);

bool ptr_is_valid(ptr_handle_t handle);
/* 无效句柄返回 0；size 为 0 的分配不允许，因此 0 可视为无效。 */
size_t ptr_get_size(ptr_handle_t handle);
/* offset 必须小于 size，否则返回 NULL。 */
void *ptr_to_raw(ptr_handle_t handle, size_t offset);
/* 生成借用 view：base+offset，size = 原 size - offset。
   offset 越界或句柄无效时返回 0。view 在根释放时自动失效。 */
ptr_handle_t ptr_offset(ptr_handle_t handle, size_t offset);

/* 当前仍活跃的根分配数量。 */
size_t safe_allocator_active_count(const safe_allocator_t *self);
/* 按注册顺序取第 idx 个活跃根分配；越界返回 false。 */
bool safe_allocator_get_active(const safe_allocator_t *self,
                               size_t idx,
                               safe_alloc_info_t *out);

#ifdef __cplusplus
}
#endif
#endif
