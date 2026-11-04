#ifndef _H_CORE_VEC_
#define _H_CORE_VEC_
#include "core/allocator.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _vec_t vec_t;
vec_t *create_vec(allocator_t *allocator);
void delete_vec(vec_t **vec);
size_t vec_get_size(vec_t *self);
size_t vec_get_capacity(vec_t *self);
vec_t *vec_resize(vec_t *self, size_t size);
vec_t *vec_reserve(vec_t *self, size_t capacity);
void *vec_get(vec_t *self, size_t idx);
size_t vec_insert(vec_t *self, size_t idx, void *data);
/* 删除 idx 处元素并返回它，idx 越界返回 NULL。 */
void *vec_erase(vec_t *self, size_t idx);
/* 替换 idx 处元素并返回被替换的旧值，idx 越界返回 NULL。 */
void *vec_set(vec_t *self, size_t idx, void *data);
size_t vec_push(vec_t *self, void *data);
size_t vec_pop(vec_t *self);
#ifdef __cplusplus
}
#endif
#endif
