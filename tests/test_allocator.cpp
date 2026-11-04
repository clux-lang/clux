#include <gtest/gtest.h>
#include "core/allocator.h"
#include <stdlib.h>
#include <string.h>

TEST(AllocatorTest, CreateAndDelete) {
  allocator_t *a = create_allocator(malloc, free);
  EXPECT_NE(a, nullptr);
  delete_allocator(&a);
  EXPECT_EQ(a, nullptr);
}

TEST(AllocatorTest, AllocReturnsWritableMemory) {
  allocator_t *a = create_allocator(malloc, free);
  void *p = allocator_alloc(a, 128);
  EXPECT_NE(p, nullptr);
  memset(p, 0xAB, 128);
  allocator_free(a, &p);
  EXPECT_EQ(p, nullptr);
  delete_allocator(&a);
}

TEST(AllocatorTest, FreeNullIsNoop) {
  allocator_t *a = create_allocator(malloc, free);
  void *p = nullptr;
  allocator_free(a, &p);
  EXPECT_EQ(p, nullptr);
  delete_allocator(&a);
}

TEST(AllocatorTest, AllocZeroSize) {
  allocator_t *a = create_allocator(malloc, free);
  void *p = allocator_alloc(a, 0);
  // malloc(0) 实现定义：返回 NULL 或唯一指针，二者均合法，不应 abort
  if (p) allocator_free(a, &p);
  delete_allocator(&a);
}

TEST(AllocatorTest, DeleteNullIsNoop) {
  allocator_t *a = nullptr;
  delete_allocator(&a);
  EXPECT_EQ(a, nullptr);
  delete_allocator(nullptr);
}

/* ---- OOM 路径：分配器返回 NULL 时应 abort ---- */

static void *always_fail_alloc(size_t size) {
  (void)size;
  return NULL;
}

TEST(AllocatorDeathTest, CreateAllocatorOomAborts) {
  EXPECT_DEATH(create_allocator(always_fail_alloc, free), "Out of memory");
}

/* 前 N 次成功，之后失败——用于让 create_allocator 成功、allocator_alloc 失败 */
static int g_fail_after;
static void *fail_after_n_alloc(size_t size) {
  if (g_fail_after <= 0) return NULL;
  g_fail_after--;
  return malloc(size);
}

/* death test 的 fork 子进程在 abort 前不会释放父进程遗留的 allocator，
   LeakSanitizer 会把它误报为泄漏。子进程内禁用 LSan 检查。 */
#ifdef __SANITIZE_ADDRESS__
extern void __lsan_disable(void);
#define CLUX_LSAN_DISABLE() __lsan_disable()
#else
#define CLUX_LSAN_DISABLE() ((void)0)
#endif

TEST(AllocatorDeathTest, AllocatorAllocOomAborts) {
  g_fail_after = 1; /* create_allocator 内部的那次分配成功 */
  allocator_t *a = create_allocator(fail_after_n_alloc, free);
  ASSERT_NE(a, nullptr);
  /* 下一次 allocator_alloc 返回 NULL → abort */
  EXPECT_DEATH(
      {
        CLUX_LSAN_DISABLE();
        allocator_alloc(a, 64);
      },
      "Out of memory");
  delete_allocator(&a);
}
