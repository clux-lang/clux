#include "core/allocator.h"
#include "core/panic.h"
#include <malloc.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Internal: allocator_t definition ---- */

struct _allocator_t {
  alloc_fn_t *alloc_fn;
  free_fn_t *free_fn;
  struct _alloc_header_t *head; /* linked list of live allocations */
  size_t live_count;            /* number of live allocations (for leak checks) */
};

/* ---- Internal header prepended to every allocation ---- */

typedef struct _alloc_header_t {
  class_t *clazz;
  size_t count;
  bool owns_clazz; /* true if clazz was heap-allocated by allocator_new_ex */
  struct _alloc_header_t *prev; /* doubly-linked list: previous allocation */
  struct _alloc_header_t *next; /* doubly-linked list: next allocation */
} alloc_header_t;

/* ---- Helper: recover header from user data pointer ---- */

static inline alloc_header_t *header_of(void *data) {
  return (alloc_header_t *)((char *)data - sizeof(alloc_header_t));
}

/* ---- Internal: linked-list operations ---- */

static void list_insert(allocator_t *a, alloc_header_t *hdr) {
  hdr->prev = NULL;
  hdr->next = a->head;
  if (a->head) a->head->prev = hdr;
  a->head = hdr;
}

static void list_remove(allocator_t *a, alloc_header_t *hdr) {
  if (hdr->prev)
    hdr->prev->next = hdr->next;
  else
    a->head = hdr->next;
  if (hdr->next) hdr->next->prev = hdr->prev;
  hdr->prev = NULL;
  hdr->next = NULL;
}

/* ---- Allocator lifetime ---- */

allocator_t *create_allocator(alloc_fn_t alloc_fn, free_fn_t free_fn) {
  if (!alloc_fn || !free_fn) return NULL;
  allocator_t *a = (allocator_t *)malloc(sizeof(allocator_t));
  if (!a) panic("out of memory: failed to create allocator");
  a->alloc_fn = alloc_fn;
  a->free_fn = free_fn;
  a->head = NULL;
  a->live_count = 0;
  return a;
}

void delete_allocator(allocator_t **allocator) {
  if (!allocator || !*allocator) return;
  allocator_t *a = *allocator;

  if (a->head) {
    fprintf(stderr,
            "memory leak detected: allocator %p has live allocations:\n",
            (void *)a);
    alloc_header_t *hdr = a->head;
    while (hdr) {
      size_t user_size = hdr->count * hdr->clazz->size;
      void *user_ptr = (char *)hdr + sizeof(alloc_header_t);
      fprintf(stderr,
              "  leak: %zu bytes at %p (type='%s', count=%zu)\n",
              user_size,
              user_ptr,
              hdr->clazz->name,
              hdr->count);
      hdr = hdr->next;
    }
  }

  free(a);
  *allocator = NULL;
}

/* ---- Allocation / deallocation ---- */

void *allocator_new(allocator_t *allocator, class_t *clazz, size_t count) {
  if (!allocator || !clazz || clazz->size == 0 || count == 0) return NULL;

  /* Overflow check: count * clazz->size */
  if (count > SIZE_MAX / clazz->size) return NULL;
  size_t user_size = count * clazz->size;

  /* Overflow check: sizeof(alloc_header_t) + user_size */
  if (user_size > SIZE_MAX - sizeof(alloc_header_t)) return NULL;
  size_t total = sizeof(alloc_header_t) + user_size;

  void *raw = allocator->alloc_fn(total);
  if (!raw)
    panic("out of memory: failed to allocate %zu bytes for '%s'",
          total,
          clazz->name);

  alloc_header_t *header = (alloc_header_t *)raw;
  header->clazz = clazz;
  header->count = count;
  header->owns_clazz = false;
  list_insert(allocator, header);
  allocator->live_count++;

  void *user = (char *)raw + sizeof(alloc_header_t);
  memset(user, 0, user_size);
  return user;
}

void *allocator_new_ex(allocator_t *allocator,
                       const char *name,
                       size_t size,
                       move_fn_t move_fn,
                       clone_fn_t clone_fn,
                       dispose_fn_t dispose_fn,
                       size_t count) {
  if (!allocator || size == 0 || count == 0) return NULL;

  /* Allocate a class_t on the heap so it survives past this call */
  class_t *clazz = (class_t *)malloc(sizeof(class_t));
  if (!clazz) panic("out of memory: failed to allocate class_t for '%s'", name);
  clazz->name = name;
  clazz->size = size;
  clazz->move_fn = move_fn;
  clazz->clone_fn = clone_fn;
  clazz->dispose_fn = dispose_fn;

  void *data = allocator_new(allocator, clazz, count);
  /* allocator_new panics on OOM, so data is never NULL here */

  /* Mark that this header owns the class_t and must free it */
  header_of(data)->owns_clazz = true;
  return data;
}

void allocator_free(allocator_t *allocator, void **data) {
  if (!allocator || !data || !*data) return;

  alloc_header_t *header = header_of(*data);
  class_t *clazz = header->clazz;
  bool owns_clazz = header->owns_clazz;

  /* Remove from live-allocation list before freeing */
  list_remove(allocator, header);
  allocator->live_count--;

  /* Call dispose before freeing memory */
  if (clazz->dispose_fn) {
    clazz->dispose_fn(*data, allocator);
  }

  /* Free the raw allocation (header + user data) */
  allocator->free_fn(header);

  /* Free the dynamically-allocated class_t if we own it */
  if (owns_clazz) {
    free(clazz);
  }

  *data = NULL;
}

/* ---- Move / clone ---- */

void *allocator_move(allocator_t *allocator, void **object) {
  if (!allocator || !object || !*object) return NULL;

  alloc_header_t *header = header_of(*object);

  if (!header->clazz->move_fn)
    panic("type '%s' does not support move", header->clazz->name);

  void *new_data = allocator_new(allocator, header->clazz, header->count);

  header->clazz->move_fn(new_data, allocator, *object);

  allocator_free(allocator, object);
  return new_data;
}

void *allocator_clone(allocator_t *allocator, void **object) {
  if (!allocator || !object || !*object) return NULL;

  alloc_header_t *header = header_of(*object);

  if (!header->clazz->clone_fn)
    panic("type '%s' does not support clone", header->clazz->name);

  void *new_data = allocator_new(allocator, header->clazz, header->count);

  header->clazz->clone_fn(new_data, allocator, *object);

  return new_data;
}

/* ---- Default callbacks for basic (trivially copyable) types ---- */

void default_move(void *self, allocator_t *allocator, void *another) {
  (void)allocator;
  if (!self || !another) return;
  alloc_header_t *header = header_of(another);
  size_t size = header->count * header->clazz->size;
  memcpy(self, another, size);
  memset(another, 0, size);
}

void default_clone(void *self, allocator_t *allocator, void *another) {
  (void)allocator;
  if (!self || !another) return;
  alloc_header_t *header = header_of(another);
  size_t size = header->count * header->clazz->size;
  memcpy(self, another, size);
}

/* ---- Accessors ---- */

const class_t *allocator_get_class(void *data) {
  if (!data) return NULL;
  return header_of(data)->clazz;
}

size_t allocator_get_count(void *data) {
  if (!data) return 0;
  return header_of(data)->count;
}

size_t allocator_live_count(allocator_t *allocator) {
  if (!allocator) return 0;
  return allocator->live_count;
}
