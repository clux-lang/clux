#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/node.h"
#include "parser/node_program.h"
#include "parser/parser.h"
#include "parser/program.h"
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

static node_t *parse_program(allocator_t *a, parser_t **p, const char *src) {
  *p = create_parser(a, "test.clux", src);
  return parser_read_program(a, *p);
}

TEST(ProgramTest, EmptyProgram) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_program(a, &p, "");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_PROGRAM);
  EXPECT_EQ(program_node_get_stmt_count(node), 0u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ProgramTest, ParsesTopLevelStatements) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node =
      parse_program(a, &p, "var x: i32 = 1; func f(): void { return; }");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(program_node_get_stmt_count(node), 2u);
  EXPECT_EQ(node_get_kind(program_node_get_stmt(node, 0)), CLUX_NODE_STMT_VAR);
  EXPECT_EQ(node_get_kind(program_node_get_stmt(node, 1)), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ProgramTest, SkipsTriviaAndComments) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_program(
      a, &p, "// leading\n var x = 1; /* middle */ func f(): void {}");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(program_node_get_stmt_count(node), 2u);
  EXPECT_EQ(vec_get_size(parser_get_error(p)), 0u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 区间从首个有效 token 起点到最后一个语句终点。 */
TEST(ProgramTest, Span) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  const char *src = "  var x = 1;  ";
  node_t *node = parse_program(a, &p, src);
  ASSERT_NE(node, nullptr);
  const location_t *loc = node_get_location(node);
  ASSERT_NE(loc, nullptr);
  EXPECT_EQ(loc->begin.offset, src + 2);
  EXPECT_EQ(loc->end.offset, src + 12);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 错误节点/无法认领的 token 都必须被消费，循环不能卡住。 */
TEST(ProgramTest, ErrorRecoveryAlwaysAdvances) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node = parse_program(a, &p, "export ) a; ) )");
  ASSERT_NE(node, nullptr);
  EXPECT_EQ(node_get_kind(node), CLUX_NODE_PROGRAM);
  /* export 错误节点 + a; 表达式语句。 */
  EXPECT_EQ(program_node_get_stmt_count(node), 2u);
  EXPECT_GE(vec_get_size(parser_get_error(p)), 3u);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

TEST(ProgramTest, MalformedInputsDoNotLeak) {
  const char *sources[] = {
      "",
      "   ",
      "// only comment",
      ")",
      ") ) )",
      "export",
      "export )",
      "[[trace]]",
      "[[trace]] )",
      "comptime",
      "var",
      "func",
      "func f(",
      "struct",
      "bitfield",
      "interface",
      "a; b; c;",
      nullptr,
  };
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p;
    node_t *node = parse_program(a, &p, sources[i]);
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}
