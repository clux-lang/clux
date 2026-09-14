#include "core/vec.h"
#include "core/panic.h"
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---- Internal: vec_t definition ---- */

struct _vec_t {
  void **data;       /* pointer array (allocator-managed) */
  size_t len;        /* number of elements */
  size_t cap;        /* allocated capacity */
  bool owns_element; /* true: free/clone elements; false: shallow */
};

/* ---- Internal: class for the void* array ---- */

static class_t ptr_class = {
    .name = "void*",
    .size = sizeof(void *),
    .move_fn = default_move,
    .clone_fn = default_clone,
    .dispose_fn = NULL,
};

/* ---- Internal: class for vec_t itself ---- */

static void vec_dispose(void *self, allocator_t *allocator);
static void vec_move_cb(void *self, allocator_t *allocator, void *another);
static void vec_clone_cb(void *self, allocator_t *allocator, void *another);

static class_t vec_class = {
    .name = "vec_t",
    .size = sizeof(vec_t),
    .move_fn = vec_move_cb,
    .clone_fn = vec_clone_cb,
    .dispose_fn = vec_dispose,
};

/* ---- Internal: growth policy ---- */

static size_t next_capacity(size_t current, size_t required) {
  if (current == 0) return required < 4 ? 4 : required;
  size_t grown = current * 2;
  if (grown < current) /* overflow */
    grown = SIZE_MAX;
  return grown < required ? required : grown;
}

/* ---- Internal: grow the data array ---- */

static void vec_grow(vec_t *vec, allocator_t *allocator, size_t min_cap) {
  if (min_cap <= vec->cap) return;
  size_t new_cap = next_capacity(vec->cap, min_cap);

  /* Overflow check: new_cap * sizeof(void*) */
  if (new_cap > SIZE_MAX / sizeof(void *))
    panic("out of memory: vec capacity overflow");

  void **new_data = (void **)allocator_new(allocator, &ptr_class, new_cap);
  if (vec->len > 0) memcpy(new_data, vec->data, vec->len * sizeof(void *));

  if (vec->data) {
    void *old = vec->data;
    allocator_free(allocator, &old);
  }
  vec->data = new_data;
  vec->cap = new_cap;
}

/* ---- Construction / destruction ---- */

vec_t *vec_new(allocator_t *allocator, bool owns_element) {
  return vec_with_capacity(allocator, owns_element, 0);
}

vec_t *
vec_with_capacity(allocator_t *allocator, bool owns_element, size_t capacity) {
  if (!allocator) return NULL;

  vec_t *vec = (vec_t *)allocator_new(allocator, &vec_class, 1);
  vec->data = NULL;
  vec->len = 0;
  vec->cap = 0;
  vec->owns_element = owns_element;

  if (capacity > 0) vec_grow(vec, allocator, capacity);

  return vec;
}

void vec_free(allocator_t *allocator, vec_t **vec) {
  if (!allocator || !vec || !*vec) return;
  allocator_free(allocator, (void **)vec);
}

/* ---- Element access ---- */

void *vec_get(const vec_t *vec, size_t index) {
  if (!vec || index >= vec->len) return NULL;
  return vec->data[index];
}

void *vec_set(vec_t *vec, size_t index, void *value) {
  if (!vec || index >= vec->len) return NULL;
  void *old = vec->data[index];
  vec->data[index] = value;
  return old;
}

void *vec_first(const vec_t *vec) {
  if (!vec || vec->len == 0) return NULL;
  return vec->data[0];
}

void *vec_last(const vec_t *vec) {
  if (!vec || vec->len == 0) return NULL;
  return vec->data[vec->len - 1];
}

/* ---- Insertion / removal ---- */

void vec_push(vec_t *vec, allocator_t *allocator, void *value) {
  if (!vec || !value) return;
  if (vec->len == vec->cap) vec_grow(vec, allocator, vec->cap + 1);
  vec->data[vec->len++] = value;
}

void *vec_pop(vec_t *vec) {
  if (!vec || vec->len == 0) return NULL;
  return vec->data[--vec->len];
}

void vec_insert(vec_t *vec, allocator_t *allocator, size_t index, void *value) {
  if (!vec || !value) return;
  if (index > vec->len)
    panic("vec_insert: index %zu out of bounds (len=%zu)", index, vec->len);
  if (vec->len == vec->cap) vec_grow(vec, allocator, vec->cap + 1);
  memmove(&vec->data[index + 1],
          &vec->data[index],
          (vec->len - index) * sizeof(void *));
  vec->data[index] = value;
  vec->len++;
}

void *vec_remove(vec_t *vec, size_t index) {
  if (!vec || index >= vec->len) return NULL;
  void *removed = vec->data[index];
  vec->len--;
  if (index < vec->len)
    memmove(&vec->data[index],
            &vec->data[index + 1],
            (vec->len - index) * sizeof(void *));
  return removed;
}

void *vec_swap_remove(vec_t *vec, size_t index) {
  if (!vec || index >= vec->len) return NULL;
  void *removed = vec->data[index];
  vec->data[index] = vec->data[vec->len - 1];
  vec->len--;
  return removed;
}

/* ---- Capacity ---- */

size_t vec_len(const vec_t *vec) {
  if (!vec) return 0;
  return vec->len;
}

size_t vec_cap(const vec_t *vec) {
  if (!vec) return 0;
  return vec->cap;
}

bool vec_is_empty(const vec_t *vec) {
  if (!vec) return true;
  return vec->len == 0;
}

void vec_reserve(vec_t *vec, allocator_t *allocator, size_t additional) {
  if (!vec || additional == 0) return;
  size_t needed = vec->len + additional;
  if (needed < vec->len) /* overflow */
    panic("vec_reserve: capacity overflow");
  if (needed > vec->cap) vec_grow(vec, allocator, needed);
}

void vec_shrink_to_fit(vec_t *vec, allocator_t *allocator) {
  if (!vec || !allocator || vec->len == vec->cap) return;

  if (vec->len == 0) {
    /* Free the data array entirely */
    if (vec->data) {
      void *old = vec->data;
      allocator_free(allocator, &old);
    }
    vec->data = NULL;
    vec->cap = 0;
    return;
  }

  void **new_data = (void **)allocator_new(allocator, &ptr_class, vec->len);
  memcpy(new_data, vec->data, vec->len * sizeof(void *));

  void *old = vec->data;
  allocator_free(allocator, &old);

  vec->data = new_data;
  vec->cap = vec->len;
}

void vec_resize(vec_t *vec, allocator_t *allocator, size_t new_len) {
  if (!vec || new_len <= vec->len) return;
  if (new_len > vec->cap) vec_grow(vec, allocator, new_len);
  while (vec->len < new_len) vec->data[vec->len++] = NULL;
}

/* ---- Ownership query ---- */

bool vec_owns_element(const vec_t *vec) {
  if (!vec) return false;
  return vec->owns_element;
}

/* ---- Callbacks for vec_class ---- */

static void vec_dispose(void *self, allocator_t *allocator) {
  vec_t *vec = (vec_t *)self;
  if (!vec) return;

  if (vec->owns_element) {
    for (size_t i = 0; i < vec->len; i++) {
      if (vec->data[i]) {
        void *elem = vec->data[i];
        allocator_free(allocator, &elem);
      }
    }
  }

  if (vec->data) {
    void *old = vec->data;
    allocator_free(allocator, &old);
    vec->data = NULL;
  }
  vec->len = 0;
  vec->cap = 0;
}

static void vec_move_cb(void *self, allocator_t *allocator, void *another) {
  (void)allocator;
  vec_t *dst = (vec_t *)self;
  vec_t *src = (vec_t *)another;
  if (!dst || !src) return;

  dst->data = src->data;
  dst->len = src->len;
  dst->cap = src->cap;
  dst->owns_element = src->owns_element;

  src->data = NULL;
  src->len = 0;
  src->cap = 0;
}

static void vec_clone_cb(void *self, allocator_t *allocator, void *another) {
  vec_t *dst = (vec_t *)self;
  vec_t *src = (vec_t *)another;
  if (!dst || !src) return;

  dst->owns_element = src->owns_element;
  dst->len = 0;
  dst->cap = 0;
  dst->data = NULL;

  if (src->len > 0) {
    void **new_data = (void **)allocator_new(allocator, &ptr_class, src->len);
    dst->data = new_data;
    dst->cap = src->len;

    for (size_t i = 0; i < src->len; i++) {
      if (src->owns_element) {
        void *elem = src->data[i];
        new_data[i] = allocator_clone(allocator, &elem);
      } else {
        new_data[i] = src->data[i];
      }
    }
    dst->len = src->len;
  }
}
