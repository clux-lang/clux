#ifndef _H_CORE_ALLOCATOR_
#define _H_CORE_ALLOCATOR_
#include <stdlib.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct _allocator_t allocator_t;
typedef void *(*alloc_fn_t)(size_t size);
typedef void (*free_fn_t)(void *ptr);
allocator_t *create_allocator(alloc_fn_t alloc, free_fn_t free);
void delete_allocator(allocator_t **allocator);
void *allocator_alloc(allocator_t *self, size_t size);
void allocator_free(allocator_t *self, void **ptr);
#ifdef __cplusplus
}
#endif
#endif
