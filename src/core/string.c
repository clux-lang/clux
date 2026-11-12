#include "core/string.h"
#include "core/allocator.h"
#include <stdint.h>
#include <string.h>

#ifndef CLUX_STRING_MIN_CAPACITY
#define CLUX_STRING_MIN_CAPACITY 8
#endif

struct _string_t {
  size_t size;
  size_t capacity;
  char *data;
  allocator_t *allocator;
};

/* data 是否指向容器自身缓冲区。 */
static bool string_data_aliases(string_t *self, const char *data) {
  if (!data) {
    return false;
  }
  uintptr_t begin = (uintptr_t)self->data;
  uintptr_t end = begin + self->capacity;
  uintptr_t ptr = (uintptr_t)data;
  return ptr >= begin && ptr < end;
}

string_t *create_string(allocator_t *allocator) {
  string_t *self = allocator_alloc(allocator, sizeof(struct _string_t));
  self->allocator = allocator;
  self->size = 0;
  self->capacity = CLUX_STRING_MIN_CAPACITY;
  self->data = allocator_alloc(allocator, CLUX_STRING_MIN_CAPACITY);
  self->data[0] = '\0';
  return self;
}

void delete_string(string_t **string) {
  if (!string || !*string) {
    return;
  }
  string_t *self = *string;
  allocator_free(self->allocator, (void **)&self->data);
  allocator_free(self->allocator, (void **)string);
}

size_t string_get_size(string_t *self) { return self->size; }

size_t string_get_capacity(string_t *self) { return self->capacity; }

const char *string_get_data(string_t *self) { return self->data; }

char *string_get_data_mut(string_t *self) { return self->data; }

string_t *string_reserve(string_t *self, size_t capacity) {
  if (capacity < CLUX_STRING_MIN_CAPACITY) {
    capacity = CLUX_STRING_MIN_CAPACITY;
  }
  if (capacity <= self->capacity) {
    return self;
  }
  char *data = allocator_alloc(self->allocator, capacity);
  memcpy(data, self->data, self->size + 1);
  allocator_free(self->allocator, (void **)&self->data);
  self->data = data;
  self->capacity = capacity;
  return self;
}

string_t *string_resize(string_t *self, size_t size) {
  if (size == (size_t)-1) {
    return NULL;
  }
  if (size + 1 > self->capacity) {
    size_t new_cap = self->capacity;
    while (new_cap < size + 1) {
      new_cap *= 2;
    }
    string_reserve(self, new_cap);
  }
  if (size > self->size) {
    memset(self->data + self->size, 0, size - self->size);
  }
  self->size = size;
  self->data[self->size] = '\0';
  return self;
}

void string_clear(string_t *self) {
  self->size = 0;
  self->data[0] = '\0';
}

char string_get(string_t *self, size_t idx) {
  if (idx >= self->size) {
    return '\0';
  }
  return self->data[idx];
}

bool string_set(string_t *self, size_t idx, char c) {
  if (idx >= self->size) {
    return false;
  }
  self->data[idx] = c;
  return true;
}

size_t string_append(string_t *self, const char *data, size_t len) {
  if (!data && len != 0) {
    return (size_t)-1;
  }
  if (len >= (size_t)-1 - self->size) {
    return (size_t)-1;
  }
  bool alias = string_data_aliases(self, data);
  size_t offset = alias ? (size_t)(data - self->data) : 0;
  size_t needed = self->size + len + 1;
  if (needed > self->capacity) {
    /* 按几何增长扩容，避免逐字节追加退化为 O(n^2)。 */
    size_t new_capacity = self->capacity;
    while (new_capacity < needed) {
      if (new_capacity > (size_t)-1 / 2) {
        new_capacity = needed;
        break;
      }
      new_capacity *= 2;
    }
    string_reserve(self, new_capacity);
  }
  if (alias) {
    data = self->data + offset;
  }
  if (len != 0) {
    memcpy(self->data + self->size, data, len);
  }
  self->size += len;
  self->data[self->size] = '\0';
  return self->size;
}

size_t string_append_cstr(string_t *self, const char *cstr) {
  if (!cstr) {
    return (size_t)-1;
  }
  return string_append(self, cstr, strlen(cstr));
}

size_t string_append_char(string_t *self, char c) {
  return string_append(self, &c, 1);
}

size_t string_assign(string_t *self, const char *data, size_t len) {
  if ((!data && len != 0) || len == (size_t)-1) {
    return (size_t)-1;
  }
  bool alias = string_data_aliases(self, data);
  size_t offset = alias ? (size_t)(data - self->data) : 0;
  string_reserve(self, len + 1);
  if (alias) {
    data = self->data + offset;
  }
  if (len != 0) {
    memmove(self->data, data, len);
  }
  self->size = len;
  self->data[self->size] = '\0';
  return self->size;
}

size_t string_insert(string_t *self, size_t idx, const char *data, size_t len) {
  if (idx > self->size || (!data && len != 0)) {
    return (size_t)-1;
  }
  if (len == 0) {
    return self->size;
  }
  if (len >= (size_t)-1 - self->size) {
    return (size_t)-1;
  }
  /* 源区间可能与右移后的目标区间重叠，先复制到临时缓冲区。 */
  char *temp = NULL;
  if (string_data_aliases(self, data)) {
    temp = allocator_alloc(self->allocator, len);
    memcpy(temp, data, len);
    data = temp;
  }
  string_reserve(self, self->size + len + 1);
  memmove(self->data + idx + len, self->data + idx, self->size - idx);
  memmove(self->data + idx, data, len);
  self->size += len;
  self->data[self->size] = '\0';
  if (temp) {
    allocator_free(self->allocator, (void **)&temp);
  }
  return self->size;
}

size_t string_erase(string_t *self, size_t idx, size_t len) {
  if (idx > self->size || len > self->size - idx) {
    return (size_t)-1;
  }
  if (len == 0) {
    return self->size;
  }
  memmove(self->data + idx, self->data + idx + len, self->size - idx - len);
  self->size -= len;
  self->data[self->size] = '\0';
  return self->size;
}

size_t string_push(string_t *self, char c) {
  return string_append(self, &c, 1);
}

size_t string_pop(string_t *self) {
  if (self->size == 0) {
    return (size_t)-1;
  }
  self->size--;
  self->data[self->size] = '\0';
  return self->size;
}
