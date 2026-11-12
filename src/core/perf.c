#include "core/perf.h"
#include "core/allocator.h"
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

typedef struct _perf_entry_t {
  const char *name;
  size_t max_count;
  size_t cursor;
  size_t count;
  uint64_t *starts;
  uint64_t *ends;
  bool *valid;
} perf_entry_t;

struct _perf_state_t {
  allocator_t *allocator;
  perf_entry_t *entries;
  size_t entry_count;
  size_t entry_capacity;
  uint32_t *slots;
  size_t slot_count;
  size_t slot_capacity;
};

static uint64_t perf_now_ns(void) {
#ifdef _WIN32
  static LARGE_INTEGER frequency;
  static bool initialized;
  if (!initialized) {
    QueryPerformanceFrequency(&frequency);
    initialized = true;
  }
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  uint64_t ticks = (uint64_t)counter.QuadPart;
  uint64_t freq = (uint64_t)frequency.QuadPart;
  uint64_t seconds = ticks / freq;
  uint64_t remainder = ticks % freq;
  return seconds * 1000000000ULL + remainder * 1000000000ULL / freq;
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

perf_state_t *create_perf_state(allocator_t *allocator) {
  perf_state_t *self = allocator_alloc(allocator, sizeof(perf_state_t));
  self->allocator = allocator;
  self->entries = NULL;
  self->entry_count = 0;
  self->entry_capacity = 0;
  self->slots = NULL;
  self->slot_count = 0;
  self->slot_capacity = 0;
  return self;
}

void delete_perf_state(perf_state_t **state) {
  if (!state || !*state) {
    return;
  }
  perf_state_t *self = *state;
  *state = NULL;
  allocator_t *allocator = self->allocator;
  for (size_t i = 0; i < self->entry_count; i++) {
    perf_entry_t *entry = &self->entries[i];
    allocator_free(allocator, (void **)&entry->starts);
    allocator_free(allocator, (void **)&entry->ends);
    allocator_free(allocator, (void **)&entry->valid);
  }
  allocator_free(allocator, (void **)&self->entries);
  allocator_free(allocator, (void **)&self->slots);
  allocator_free(allocator, (void **)&self);
}

static bool ensure_entry_capacity(perf_state_t *self, size_t needed) {
  if (needed <= self->entry_capacity) {
    return true;
  }
  size_t capacity = self->entry_capacity ? self->entry_capacity : 8;
  while (capacity < needed) {
    if (capacity > (size_t)-1 / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }
  perf_entry_t *entries =
      allocator_alloc(self->allocator, capacity * sizeof(perf_entry_t));
  if (self->entries) {
    memcpy(entries, self->entries, self->entry_count * sizeof(perf_entry_t));
    allocator_free(self->allocator, (void **)&self->entries);
  }
  self->entries = entries;
  self->entry_capacity = capacity;
  return true;
}

static bool ensure_slot_capacity(perf_state_t *self, size_t needed) {
  if (needed <= self->slot_capacity) {
    return true;
  }
  size_t capacity = self->slot_capacity ? self->slot_capacity : 16;
  while (capacity < needed) {
    if (capacity > (size_t)-1 / 2) {
      capacity = needed;
      break;
    }
    capacity *= 2;
  }
  uint32_t *slots =
      allocator_alloc(self->allocator, capacity * sizeof(uint32_t));
  if (self->slots) {
    memcpy(slots, self->slots, self->slot_count * sizeof(uint32_t));
    allocator_free(self->allocator, (void **)&self->slots);
  }
  self->slots = slots;
  self->slot_capacity = capacity;
  return true;
}

uint32_t perf_create(perf_state_t *state, const char *name, size_t max_count) {
  if (!state || !name || max_count == 0) {
    return PERF_INVALID_NAME;
  }
  if (max_count > (size_t)UINT32_MAX - state->slot_count) {
    return PERF_INVALID_NAME;
  }
  size_t entry_idx = state->entry_count;
  size_t base = state->slot_count;
  if (!ensure_entry_capacity(state, entry_idx + 1) ||
      !ensure_slot_capacity(state, base + max_count)) {
    return PERF_INVALID_NAME;
  }

  perf_entry_t *entry = &state->entries[entry_idx];
  entry->name = name;
  entry->max_count = max_count;
  entry->cursor = 0;
  entry->count = 0;
  entry->starts =
      allocator_alloc(state->allocator, max_count * sizeof(uint64_t));
  entry->ends = allocator_alloc(state->allocator, max_count * sizeof(uint64_t));
  entry->valid = allocator_alloc(state->allocator, max_count * sizeof(bool));
  memset(entry->valid, 0, max_count * sizeof(bool));

  for (size_t i = 0; i < max_count; i++) {
    state->slots[base + i] = (uint32_t)entry_idx;
  }
  state->entry_count++;
  state->slot_count += max_count;
  return (uint32_t)base;
}

static perf_entry_t *perf_lookup(perf_state_t *state, uint32_t name) {
  if (!state || name == PERF_INVALID_NAME || name >= state->slot_count) {
    return NULL;
  }
  return &state->entries[state->slots[name]];
}

void perf_start(perf_state_t *state, uint32_t name) {
  perf_entry_t *entry = perf_lookup(state, name);
  if (!entry) {
    return;
  }
  size_t slot = entry->cursor;
  entry->starts[slot] = perf_now_ns();
  entry->valid[slot] = false;
}

void perf_end(perf_state_t *state, uint32_t name) {
  perf_entry_t *entry = perf_lookup(state, name);
  if (!entry) {
    return;
  }
  size_t slot = entry->cursor;
  entry->ends[slot] = perf_now_ns();
  entry->valid[slot] = true;
  entry->cursor = (entry->cursor + 1) % entry->max_count;
  if (entry->count < entry->max_count) {
    entry->count++;
  }
}

size_t perf_get_sample_count(const perf_state_t *state, uint32_t name) {
  if (!state || name == PERF_INVALID_NAME || name >= state->slot_count) {
    return 0;
  }
  return state->entries[state->slots[name]].count;
}

bool perf_get_sample(const perf_state_t *state,
                     uint32_t name,
                     size_t idx,
                     uint64_t *start_ns,
                     uint64_t *end_ns) {
  if (!state || name == PERF_INVALID_NAME || name >= state->slot_count) {
    return false;
  }
  const perf_entry_t *entry = &state->entries[state->slots[name]];
  if (idx >= entry->count) {
    return false;
  }
  size_t slot = entry->count == entry->max_count
                    ? (entry->cursor + idx) % entry->max_count
                    : idx;
  if (!entry->valid[slot]) {
    return false;
  }
  if (start_ns) {
    *start_ns = entry->starts[slot];
  }
  if (end_ns) {
    *end_ns = entry->ends[slot];
  }
  return true;
}

void perf_report(const perf_state_t *state, FILE *out) {
  if (!state || !out) {
    return;
  }
  for (size_t i = 0; i < state->entry_count; i++) {
    const perf_entry_t *entry = &state->entries[i];
    size_t count = 0;
    uint64_t min_ns = 0;
    uint64_t max_ns = 0;
    uint64_t sum_ns = 0;
    for (size_t j = 0; j < entry->max_count; j++) {
      if (!entry->valid[j] || entry->ends[j] < entry->starts[j]) {
        continue;
      }
      uint64_t elapsed = entry->ends[j] - entry->starts[j];
      if (count == 0 || elapsed < min_ns) {
        min_ns = elapsed;
      }
      if (count == 0 || elapsed > max_ns) {
        max_ns = elapsed;
      }
      sum_ns += elapsed;
      count++;
    }
    double avg_us = count ? (double)sum_ns / (double)count / 1000.0 : 0.0;
    fprintf(out,
            "perf %-16s n=%zu min=%.3fus avg=%.3fus max=%.3fus\n",
            entry->name,
            count,
            (double)min_ns / 1000.0,
            avg_us,
            (double)max_ns / 1000.0);
  }
}
