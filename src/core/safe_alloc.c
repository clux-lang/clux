#include "core/safe_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if UINTPTR_MAX > 0xFFFFFFFFu
#define PTR_INDEX_BITS 32u
#else
#define PTR_INDEX_BITS 16u
#endif
#define PTR_INDEX_MASK ((((uintptr_t)1) << PTR_INDEX_BITS) - 1)
#define PTR_GENERATION_SHIFT PTR_INDEX_BITS
#define PTR_GENERATION_MASK                                                    \
  ((((uintptr_t)1) << (sizeof(uintptr_t) * 8 - PTR_INDEX_BITS)) - 1)

typedef struct _safe_slot_t {
  void *base;
  size_t size;
  size_t offset;
  uint32_t generation;
  uint32_t root;
  safe_allocator_t *owner;
  location_t location;
  bool has_location;
  bool alive;
  bool is_view;
} safe_slot_t;

struct _safe_allocator_t {
  allocator_t *allocator;
};

/* 句柄表是进程级全局表：句柄本身只含下标+代际，查询接口无需再传分配器。 */
static safe_slot_t *g_slots;
static size_t g_slot_count;
static size_t g_slot_capacity;
static uint32_t *g_free_slots;
static size_t g_free_count;
static size_t g_free_capacity;
static size_t g_allocator_count;

static void *xrealloc(void *ptr, size_t size) {
  void *result = realloc(ptr, size);
  if (!result) {
    fprintf(stderr, "Out of memory");
    abort();
  }
  return result;
}

static void ensure_slot_capacity(size_t needed) {
  if (needed <= g_slot_capacity) {
    return;
  }
  size_t capacity = g_slot_capacity ? g_slot_capacity : 16;
  while (capacity < needed) {
    capacity *= 2;
  }
  g_slots = xrealloc(g_slots, capacity * sizeof(safe_slot_t));
  g_slot_capacity = capacity;
}

static void ensure_free_capacity(size_t needed) {
  if (needed <= g_free_capacity) {
    return;
  }
  size_t capacity = g_free_capacity ? g_free_capacity : 16;
  while (capacity < needed) {
    capacity *= 2;
  }
  g_free_slots = xrealloc(g_free_slots, capacity * sizeof(uint32_t));
  g_free_capacity = capacity;
}

/* 返回 SIZE_MAX 表示句柄表已满。 */
static size_t registry_alloc_slot(void) {
  if (g_free_count > 0) {
    return g_free_slots[--g_free_count];
  }
  if (g_slot_count >= PTR_INDEX_MASK) {
    return SIZE_MAX;
  }
  ensure_slot_capacity(g_slot_count + 1);
  size_t index = g_slot_count++;
  memset(&g_slots[index], 0, sizeof(safe_slot_t));
  g_slots[index].generation = 1;
  return index;
}

static void registry_release_slot(size_t index) {
  safe_slot_t *slot = &g_slots[index];
  slot->alive = false;
  slot->generation = (uint32_t)(((slot->generation + 1) & PTR_GENERATION_MASK));
  if (slot->generation == 0) {
    slot->generation = 1;
  }
  ensure_free_capacity(g_free_count + 1);
  g_free_slots[g_free_count++] = (uint32_t)index;
}

static ptr_handle_t encode_handle(size_t index, uint32_t generation) {
  return ((uintptr_t)generation << PTR_GENERATION_SHIFT) |
         (uintptr_t)(index + 1);
}

static bool
decode_handle(ptr_handle_t handle, size_t *index, uint32_t *generation) {
  if (handle == 0) {
    return false;
  }
  uintptr_t encoded_index = handle & PTR_INDEX_MASK;
  if (encoded_index == 0) {
    return false;
  }
  size_t slot_index = (size_t)(encoded_index - 1);
  if (slot_index >= g_slot_count) {
    return false;
  }
  *index = slot_index;
  *generation =
      (uint32_t)((handle >> PTR_GENERATION_SHIFT) & PTR_GENERATION_MASK);
  return true;
}

static safe_slot_t *lookup_handle(ptr_handle_t handle, size_t *index) {
  size_t slot_index;
  uint32_t generation;
  if (!decode_handle(handle, &slot_index, &generation)) {
    return NULL;
  }
  safe_slot_t *slot = &g_slots[slot_index];
  if (!slot->alive || slot->generation != generation) {
    return NULL;
  }
  if (index) {
    *index = slot_index;
  }
  return slot;
}

safe_allocator_t *create_safe_allocator(allocator_t *allocator) {
  safe_allocator_t *self = allocator_alloc(allocator, sizeof(safe_allocator_t));
  self->allocator = allocator;
  g_allocator_count++;
  return self;
}

void delete_safe_allocator(safe_allocator_t **self) {
  if (!self || !*self) {
    return;
  }
  safe_allocator_t *allocator = *self;
  *self = NULL;
  for (size_t i = 0; i < g_slot_count; i++) {
    safe_slot_t *slot = &g_slots[i];
    if (!slot->alive || slot->owner != allocator) {
      continue;
    }
    if (!slot->is_view) {
      allocator_free(allocator->allocator, &slot->base);
    }
    registry_release_slot(i);
  }
  allocator_free(allocator->allocator, (void **)&allocator);
  g_allocator_count--;
  if (g_allocator_count == 0) {
    free(g_slots);
    g_slots = NULL;
    g_slot_count = 0;
    g_slot_capacity = 0;
    free(g_free_slots);
    g_free_slots = NULL;
    g_free_count = 0;
    g_free_capacity = 0;
  }
}

ptr_handle_t
safe_alloc(safe_allocator_t *self, size_t size, const location_t *location) {
  if (!self || size == 0) {
    return 0;
  }
  size_t index = registry_alloc_slot();
  if (index == SIZE_MAX) {
    return 0;
  }
  void *base = allocator_alloc(self->allocator, size);
  safe_slot_t *slot = &g_slots[index];
  slot->base = base;
  slot->size = size;
  slot->offset = 0;
  slot->root = (uint32_t)index;
  slot->owner = self;
  slot->alive = true;
  slot->is_view = false;
  slot->has_location = location != NULL;
  if (location) {
    slot->location = *location;
  }
  return encode_handle(index, slot->generation);
}

bool safe_free(ptr_handle_t handle) {
  size_t index;
  safe_slot_t *slot = lookup_handle(handle, &index);
  if (!slot || slot->is_view) {
    return false;
  }
  safe_allocator_t *owner = slot->owner;
  allocator_free(owner->allocator, &slot->base);
  for (size_t i = 0; i < g_slot_count; i++) {
    safe_slot_t *view = &g_slots[i];
    if (view->alive && view->is_view && view->root == (uint32_t)index) {
      registry_release_slot(i);
    }
  }
  registry_release_slot(index);
  return true;
}

bool ptr_is_valid(ptr_handle_t handle) {
  return lookup_handle(handle, NULL) != NULL;
}

size_t ptr_get_size(ptr_handle_t handle) {
  safe_slot_t *slot = lookup_handle(handle, NULL);
  return slot ? slot->size : 0;
}

void *ptr_to_raw(ptr_handle_t handle, size_t offset) {
  safe_slot_t *slot = lookup_handle(handle, NULL);
  if (!slot || offset >= slot->size) {
    return NULL;
  }
  if (slot->is_view) {
    safe_slot_t *root = &g_slots[slot->root];
    if (!root->alive) {
      return NULL;
    }
    return (char *)root->base + slot->offset + offset;
  }
  return (char *)slot->base + offset;
}

ptr_handle_t ptr_offset(ptr_handle_t handle, size_t offset) {
  size_t index;
  safe_slot_t *slot = lookup_handle(handle, &index);
  if (!slot || offset > slot->size) {
    return 0;
  }
  size_t root_index = slot->is_view ? slot->root : index;
  safe_slot_t *root = &g_slots[root_index];
  if (!root->alive) {
    return 0;
  }
  size_t new_offset = (slot->is_view ? slot->offset : 0) + offset;
  size_t new_size = slot->size - offset;
  safe_allocator_t *owner = slot->owner;
  bool has_location = slot->has_location;
  location_t location = slot->location;

  size_t view_index = registry_alloc_slot();
  if (view_index == SIZE_MAX) {
    return 0;
  }
  safe_slot_t *view = &g_slots[view_index];
  view->base = NULL;
  view->size = new_size;
  view->offset = new_offset;
  view->root = (uint32_t)root_index;
  view->owner = owner;
  view->alive = true;
  view->is_view = true;
  view->has_location = has_location;
  if (has_location) {
    view->location = location;
  }
  return encode_handle(view_index, view->generation);
}

size_t safe_allocator_active_count(const safe_allocator_t *self) {
  if (!self) {
    return 0;
  }
  size_t count = 0;
  for (size_t i = 0; i < g_slot_count; i++) {
    const safe_slot_t *slot = &g_slots[i];
    if (slot->alive && !slot->is_view && slot->owner == self) {
      count++;
    }
  }
  return count;
}

bool safe_allocator_get_active(const safe_allocator_t *self,
                               size_t idx,
                               safe_alloc_info_t *out) {
  if (!self || !out) {
    return false;
  }
  size_t seen = 0;
  for (size_t i = 0; i < g_slot_count; i++) {
    safe_slot_t *slot = &g_slots[i];
    if (!slot->alive || slot->is_view || slot->owner != self) {
      continue;
    }
    if (seen == idx) {
      out->handle = encode_handle(i, slot->generation);
      out->base = slot->base;
      out->size = slot->size;
      out->has_location = slot->has_location;
      if (slot->has_location) {
        out->location = slot->location;
      }
      return true;
    }
    seen++;
  }
  return false;
}
