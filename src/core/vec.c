#include "core/vec.h"
#include "core/allocator.h"
#ifndef CLUX_VEC_MIN_CAPACITY
#define CLUX_VEC_MIN_CAPACITY 8
#endif
struct _vec_t {
  size_t size;
  size_t capacity;
  void **data;
  allocator_t *allocator;
  bool own;
};

vec_t *create_vec(allocator_t *allocator, bool own) {
  vec_t *self = allocator_alloc(allocator, sizeof(struct _vec_t));
  self->allocator = allocator;
  self->capacity = CLUX_VEC_MIN_CAPACITY;
  self->size = 0;
  self->data =
      allocator_alloc(allocator, sizeof(void *) * CLUX_VEC_MIN_CAPACITY);
  self->own = own;
  return self;
}
void delete_vec(vec_t **vec) {
  allocator_t *allocator = (*vec)->allocator;
  if ((*vec)->own) {
    for (size_t idx = 0; idx < (*vec)->size; idx++) {
      allocator_free(allocator, &(*vec)->data[idx]);
    }
  }
  allocator_free(allocator, (void **)&(*vec)->data);
  allocator_free(allocator, (void **)vec);
}
size_t vec_get_size(vec_t *self) { return self->size; }
size_t vec_get_capacity(vec_t *self) { return self->capacity; }
vec_t *vec_resize(vec_t *self, size_t size) {
  if (size < self->size) {
    if (self->own) {
      for (size_t idx = size; idx < self->size; idx++) {
        void *item = self->data[idx];
        allocator_free(self->allocator, &item);
      }
    }
  } else {
    if (size > self->capacity) {
      size_t new_cap = self->capacity;
      while (new_cap < size) {
        new_cap *= 2;
      }
      vec_reserve(self, new_cap);
    }
    for (size_t idx = self->size; idx < size; idx++) {
      self->data[idx] = NULL;
    }
  }
  self->size = size;
  return self;
}
vec_t *vec_reserve(vec_t *self, size_t capacity) {
  if (capacity < CLUX_VEC_MIN_CAPACITY) {
    capacity = CLUX_VEC_MIN_CAPACITY;
  }
  if (capacity <= self->capacity) {
    return self;
  }
  void **data = allocator_alloc(self->allocator, sizeof(void *) * capacity);
  for (size_t idx = 0; idx < self->size; idx++) {
    data[idx] = self->data[idx];
  }
  allocator_free(self->allocator, (void **)&self->data);
  self->data = data;
  self->capacity = capacity;
  return self;
}
void *vec_get(vec_t *self, size_t idx) {
  if (idx >= self->size) {
    return NULL;
  }
  return self->data[idx];
}
size_t vec_insert(vec_t *self, size_t idx, void *data) {
  if (idx > self->size) {
    return (size_t)-1;
  }
  vec_resize(self, self->size + 1);
  for (size_t i = self->size - 1; i > idx; i--) {
    self->data[i] = self->data[i - 1];
  }
  self->data[idx] = data;
  return self->size;
}
size_t vec_erase(vec_t *self, size_t idx) {
  if (idx >= self->size) {
    return (size_t)-1;
  }
  if (self->own) {
    allocator_free(self->allocator, (void **)&self->data[idx]);
  }
  for (size_t i = idx; i + 1 < self->size; i++) {
    self->data[i] = self->data[i + 1];
  }
  self->size--;
  return self->size;
}
size_t vec_set(vec_t *self, size_t idx, void *data) {
  if (idx >= self->size) {
    return (size_t)-1;
  }
  if (self->data[idx] == data) {
    return self->size;
  }
  if (self->own) {
    allocator_free(self->allocator, (void **)&self->data[idx]);
  }
  self->data[idx] = data;
  return self->size;
}
size_t vec_push(vec_t *self, void *data) {
  vec_resize(self, self->size + 1);
  self->data[self->size - 1] = data;
  return self->size;
}
size_t vec_pop(vec_t *self) {
  if (self->size == 0) {
    return (size_t)-1;
  }
  vec_resize(self, self->size - 1);
  return self->size;
}