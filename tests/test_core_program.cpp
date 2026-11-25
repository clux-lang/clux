#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/program.h"
#include "parser/node.h"
#include <stdlib.h>
#include <string.h>

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

TEST(CoreProgramTest, CreateDeleteLifecycle) {
  allocator_t *a = create_allocator(malloc, free);
  program_t *program = create_program(a);
  ASSERT_NE(program, nullptr);
  EXPECT_EQ(program_get_decls(program), nullptr);
  EXPECT_EQ(program_get_module_name(program), nullptr);
  EXPECT_EQ(program_get_source_path(program), nullptr);
  EXPECT_EQ(vec_get_size(program_get_strings(program)), 0u);
  delete_program(&program);
  EXPECT_EQ(program, nullptr);
  delete_allocator(&a);
}

TEST(CoreProgramTest, NullAllocatorAndNullSelfAreSafe) {
  EXPECT_EQ(create_program(nullptr), nullptr);
  program_set_decls(nullptr, nullptr);
  EXPECT_EQ(program_get_decls(nullptr), nullptr);
  program_set_module_name(nullptr, "m");
  EXPECT_EQ(program_get_module_name(nullptr), nullptr);
  program_set_source_path(nullptr, "p");
  EXPECT_EQ(program_get_source_path(nullptr), nullptr);
  EXPECT_EQ(program_intern_string(nullptr, "x", 1), nullptr);
  EXPECT_EQ(program_get_strings(nullptr), nullptr);
  delete_program(nullptr);
  delete_program((program_t **)nullptr);
}

TEST(CoreProgramTest, DeclsOwnershipTransfersAndOldIsFreed) {
  allocator_t *a = create_allocator(malloc, free);
  program_t *program = create_program(a);
  node_t *first = create_node(a, CLUX_NODE_PROGRAM);
  program_set_decls(program, first);
  EXPECT_EQ(program_get_decls(program), first);
  /* 二次 set：旧的 first 归 program 释放，调用方不得再触碰。 */
  node_t *second = create_node(a, CLUX_NODE_PROGRAM);
  program_set_decls(program, second);
  EXPECT_EQ(program_get_decls(program), second);
  EXPECT_EQ(node_get_kind(program_get_decls(program)), CLUX_NODE_PROGRAM);
  delete_program(&program);
  delete_allocator(&a);
}

TEST(CoreProgramTest, NamesAreCopiedNotBorrowed) {
  allocator_t *a = create_allocator(malloc, free);
  program_t *program = create_program(a);
  char buffer[8];
  strcpy(buffer, "main");
  program_set_module_name(program, buffer);
  program_set_source_path(program, buffer);
  /* 修改原缓冲不影响 program 内的拷贝。 */
  strcpy(buffer, "xxxx");
  EXPECT_STREQ(program_get_module_name(program), "main");
  EXPECT_STREQ(program_get_source_path(program), "main");
  /* NULL 清空。 */
  program_set_module_name(program, nullptr);
  EXPECT_EQ(program_get_module_name(program), nullptr);
  EXPECT_STREQ(program_get_source_path(program), "main");
  delete_program(&program);
  delete_allocator(&a);
}

TEST(CoreProgramTest, InternDedupesSameContent) {
  allocator_t *a = create_allocator(malloc, free);
  program_t *program = create_program(a);
  const char *s1 = program_intern_string(program, "hello", 5);
  const char *s2 = program_intern_string(program, "hello", 5);
  const char *s3 = program_intern_string(program, "hell", 4);
  const char *s4 = program_intern_string(program, "hellox", 6);
  ASSERT_NE(s1, nullptr);
  EXPECT_EQ(s1, s2) << "same content must intern to same pointer";
  EXPECT_NE(s1, s3);
  EXPECT_NE(s1, s4);
  EXPECT_STREQ(s1, "hello");
  EXPECT_STREQ(s3, "hell");
  EXPECT_EQ(vec_get_size(program_get_strings(program)), 3u);
  /* 空串与带内嵌 NUL 的内容按字节区间判等。 */
  const char *empty = program_intern_string(program, "", 0);
  EXPECT_EQ(vec_get_size(program_get_strings(program)), 4u);
  EXPECT_EQ(program_intern_string(program, "", 0), empty);
  EXPECT_EQ(program_intern_string(program, nullptr, 0), nullptr);
  delete_program(&program);
  delete_allocator(&a);
}

/* 生命周期内 allocations 与 frees 严格配对（不覆盖 allocator 自身）。 */
TEST(CoreProgramTest, FullLifecycleDoesNotLeak) {
  g_alloc_count = 0;
  g_free_count = 0;
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  program_t *program = create_program(a);
  program_set_module_name(program, "app");
  program_set_module_name(program, "other"); /* 旧 string 释放 */
  program_set_source_path(program, "src/app.clux");
  program_set_decls(program, create_node(a, CLUX_NODE_PROGRAM));
  program_set_decls(program, create_node(a, CLUX_NODE_PROGRAM));
  program_intern_string(program, "a", 1);
  program_intern_string(program, "b", 1);
  program_intern_string(program, "a", 1); /* 去重，不新增 */
  delete_program(&program);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
