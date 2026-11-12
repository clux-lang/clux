#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/perf.h"
#include <stdlib.h>

static size_t g_alloc_count;
static size_t g_free_count;

static void *counting_alloc(size_t size) {
  g_alloc_count++;
  return malloc(size);
}

static void counting_free(void *ptr) {
  g_free_count++;
  free(ptr);
}

TEST(PerfTest, CreateReturnsStableNumericNames) {
  allocator_t *a = create_allocator(malloc, free);
  perf_state_t *state = create_perf_state(a);
  uint32_t first = perf_create(state, "first", 3);
  uint32_t second = perf_create(state, "second", 2);
  EXPECT_NE(first, PERF_INVALID_NAME);
  EXPECT_NE(second, PERF_INVALID_NAME);
  EXPECT_EQ(first, 0u);
  EXPECT_EQ(second, 3u);
  delete_perf_state(&state);
  delete_allocator(&a);
}

TEST(PerfTest, InvalidArguments) {
  allocator_t *a = create_allocator(malloc, free);
  perf_state_t *state = create_perf_state(a);
  EXPECT_EQ(perf_create(state, NULL, 1), PERF_INVALID_NAME);
  EXPECT_EQ(perf_create(state, "zero", 0), PERF_INVALID_NAME);
  EXPECT_EQ(perf_get_sample_count(state, PERF_INVALID_NAME), 0u);
  perf_start(state, PERF_INVALID_NAME);
  perf_end(state, PERF_INVALID_NAME);
  delete_perf_state(&state);
  delete_allocator(&a);
}

TEST(PerfTest, RingBufferWrapsAtMaxCount) {
  allocator_t *a = create_allocator(malloc, free);
  perf_state_t *state = create_perf_state(a);
  uint32_t name = perf_create(state, "loop", 3);
  ASSERT_NE(name, PERF_INVALID_NAME);

  for (size_t i = 0; i < 4; i++) {
    perf_start(state, name);
    perf_end(state, name);
  }

  EXPECT_EQ(perf_get_sample_count(state, name), 3u);
  for (size_t i = 0; i < 3; i++) {
    uint64_t start = 0;
    uint64_t end = 0;
    EXPECT_TRUE(perf_get_sample(state, name, i, &start, &end));
    EXPECT_LE(start, end);
  }
  uint64_t start = 0;
  uint64_t end = 0;
  EXPECT_FALSE(perf_get_sample(state, name, 3, &start, &end));
  delete_perf_state(&state);
  delete_allocator(&a);
}

TEST(PerfTest, NamesDoNotInterfere) {
  allocator_t *a = create_allocator(malloc, free);
  perf_state_t *state = create_perf_state(a);
  uint32_t first = perf_create(state, "first", 2);
  uint32_t second = perf_create(state, "second", 2);
  ASSERT_NE(first, PERF_INVALID_NAME);
  ASSERT_NE(second, PERF_INVALID_NAME);

  perf_start(state, first);
  perf_end(state, first);
  perf_start(state, second);
  perf_end(state, second);

  EXPECT_EQ(perf_get_sample_count(state, first), 1u);
  EXPECT_EQ(perf_get_sample_count(state, second), 1u);
  delete_perf_state(&state);
  delete_allocator(&a);
}

TEST(PerfTest, NoLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  perf_state_t *state = create_perf_state(a);
  uint32_t name = perf_create(state, "leak", 8);
  for (size_t i = 0; i < 16; i++) {
    perf_start(state, name);
    perf_end(state, name);
  }
  delete_perf_state(&state);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
