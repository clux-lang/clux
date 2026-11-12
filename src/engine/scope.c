#include "engine/scope.h"
#include "core/allocator.h"
#include "core/rbtree.h"
#include "core/vec.h"
#include "engine/interface.h"
#include "engine/value.h"
#include <string.h>

typedef struct _clux_slot_t {
  char *name;
  clux_value_t *value;
} clux_slot_t;

struct _clux_scope_t {
  allocator_t *allocator;
  clux_scope_t *parent;
  vec_t *children;
  vec_t *values;          /* clux_slot_t*，按定义顺序持有所有值 */
  rbtree_t *named_values; /* hash -> vec_t*，桶内按名字精确匹配 */
};

static uint32_t hash(const char *str) {
  uint32_t hash = 0;
  while (*str) {
    hash = (*str++) + (hash << 6) + (hash << 16) - hash;
  }
  return hash;
}

static char *copy_name(allocator_t *allocator, const char *name) {
  size_t len = strlen(name);
  char *copy = allocator_alloc(allocator, len + 1);
  memcpy(copy, name, len + 1);
  return copy;
}

static void free_slot(allocator_t *allocator, clux_slot_t *slot) {
  allocator_free(allocator, (void **)&slot->name);
  allocator_free(allocator, (void **)&slot);
}

static void dispose_bucket(uintptr_t key, void *value, void *user_data) {
  (void)key;
  (void)user_data;
  vec_t *bucket = value;
  delete_vec(&bucket);
}

static void detach_from_parent(clux_scope_t *scope) {
  if (!scope->parent) {
    return;
  }
  vec_t *siblings = scope->parent->children;
  for (size_t idx = 0; idx < vec_get_size(siblings); idx++) {
    if (vec_get(siblings, idx) == scope) {
      vec_erase(siblings, idx);
      break;
    }
  }
}

clux_scope_t *create_clux_scope(allocator_t *allocator, clux_scope_t *parent) {
  clux_scope_t *scope =
      allocator_alloc(allocator, sizeof(struct _clux_scope_t));
  scope->allocator = allocator;
  scope->parent = parent;
  scope->children = create_vec(allocator);
  scope->values = create_vec(allocator);
  scope->named_values = create_rbtree(allocator);
  if (parent) {
    vec_push(parent->children, scope);
  }
  return scope;
}

void delete_clux_scope(clux_scope_t **self) {
  if (!self || !*self) {
    return;
  }
  clux_scope_t *scope = *self;
  *self = NULL;
  detach_from_parent(scope);

  for (size_t idx = 0; idx < vec_get_size(scope->children); idx++) {
    clux_scope_t *child = vec_get(scope->children, idx);
    child->parent = NULL;
    delete_clux_scope(&child);
  }
  for (size_t idx = 0; idx < vec_get_size(scope->values); idx++) {
    clux_slot_t *slot = vec_get(scope->values, idx);
    delete_clux_value(&slot->value);
    free_slot(scope->allocator, slot);
  }
  rbtree_foreach(scope->named_values, dispose_bucket, NULL);
  delete_rbtree(&scope->named_values);
  delete_vec(&scope->values);
  delete_vec(&scope->children);
  allocator_free(scope->allocator, (void **)&scope);
}

clux_value_t *
clux_scope_define(clux_scope_t *self, const char *name, clux_value_t *value) {
  if (!self || !value) {
    return NULL;
  }
  clux_slot_t *slot =
      allocator_alloc(self->allocator, sizeof(struct _clux_slot_t));
  slot->name = name ? copy_name(self->allocator, name) : NULL;
  slot->value = value;

  if (name) {
    uint32_t key = hash(name);
    vec_t *bucket = rbtree_find(self->named_values, key);
    if (!bucket) {
      bucket = create_vec(self->allocator);
      rbtree_insert(self->named_values, key, bucket);
    }
    for (size_t idx = 0; idx < vec_get_size(bucket); idx++) {
      clux_slot_t *existing = vec_get(bucket, idx);
      if (strcmp(existing->name, name) == 0) {
        free_slot(self->allocator, slot);
        return existing->value;
      }
    }
    vec_push(bucket, slot);
  }
  vec_push(self->values, slot);
  return NULL;
}

clux_value_t *clux_scope_get(clux_scope_t *self, const char *name) {
  if (!name) {
    return NULL;
  }
  for (clux_scope_t *scope = self; scope; scope = scope->parent) {
    vec_t *bucket = rbtree_find(scope->named_values, hash(name));
    if (!bucket) {
      continue;
    }
    for (size_t idx = 0; idx < vec_get_size(bucket); idx++) {
      clux_slot_t *slot = vec_get(bucket, idx);
      if (strcmp(slot->name, name) == 0) {
        return slot->value;
      }
    }
  }
  return NULL;
}

clux_scope_t *clux_scope_get_parent(clux_scope_t *self) {
  return self ? self->parent : NULL;
}
