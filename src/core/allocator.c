#include "core/allocator.h"
#include <stdio.h>
#include <stdlib.h>
struct _allocator_t {
  alloc_fn_t alloc;
  free_fn_t free;
};
allocator_t *create_allocator(alloc_fn_t alloc, free_fn_t free) {
  allocator_t *allocator = alloc(sizeof(struct _allocator_t));
  if (!allocator) {
    fprintf(stderr, "Out of memory");
    abort();
  }
  allocator->alloc = alloc;
  allocator->free = free;
  return allocator;
}
void delete_allocator(allocator_t **allocator) {
  if (!allocator || !*allocator) {
    return;
  }
  (*allocator)->free(*allocator);
  *allocator = NULL;
}
void *allocator_alloc(allocator_t *self, size_t size) {
  void *data = self->alloc(size);
  if (!data) {
    fprintf(stderr, "Out of memory");
    abort();
  }
  return data;
}
void allocator_free(allocator_t *self, void **ptr) {
  if (*ptr) {
    self->free(*ptr);
    *ptr = NULL;
  }
}