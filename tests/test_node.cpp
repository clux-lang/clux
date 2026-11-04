#include <gtest/gtest.h>
#include "parser/node.h"
#include "core/allocator.h"
#include "core/location.h"
#include "core/vec.h"
#include <stdlib.h>
#include <string.h>

/* ---- 计数分配器：检测泄漏 ---- */
static size_t g_alloc_count;
static size_t g_free_count;

static void *counting_alloc(size_t size) {
  g_alloc_count++;
  return malloc(size);
}

static void counting_free(void *ptr) {
  if (ptr) g_free_count++;
  free(ptr);
}

static void reset_counts(void) {
  g_alloc_count = 0;
  g_free_count = 0;
}

static location_t make_loc(const char *src, size_t begin_off, size_t end_off) {
  location_t loc;
  loc.filename = "test.clux";
  loc.begin.filename = "test.clux";
  loc.begin.offset = src + begin_off;
  loc.begin.line = 1;
  loc.begin.column = (uint32_t)(begin_off + 1);
  loc.end.filename = "test.clux";
  loc.end.offset = src + end_off;
  loc.end.line = 1;
  loc.end.column = (uint32_t)(end_off + 1);
  return loc;
}

/* ---- 基本生命周期 ---- */

TEST(NodeTest, CreateAndDelete) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "1";
  location_t loc = make_loc(src, 0, 1);
  node_t *n = create_node(a, NODE_LITERAL_NUMERIC, NULL, loc);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(node_get_kind(n), NODE_LITERAL_NUMERIC);
  EXPECT_EQ(node_get_parent(n), nullptr);
  delete_node(&n);
  EXPECT_EQ(n, nullptr);
  delete_allocator(&a);
}

TEST(NodeTest, DeleteNullIsNoop) {
  node_t *n = nullptr;
  delete_node(&n);
  EXPECT_EQ(n, nullptr);
  delete_node(nullptr);
}

/* ---- getter ---- */

TEST(NodeTest, GetKind) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  node_kind_t kinds[] = {
      NODE_PROGRAM,
      NODE_STATEMENT_EXPRESSION,
      NODE_LITERAL_NIL,
      NODE_LITERAL_UNDEFINED,
      NODE_LITERAL_STRING,
      NODE_LITERAL_CHARACTER,
      NODE_LITERAL_NUMERIC,
      NODE_LITERAL_BOOLEAN,
      NODE_LITERAL_IDENTIFIER,
  };
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    node_t *n = create_node(a, kinds[i], NULL, loc);
    EXPECT_EQ(node_get_kind(n), kinds[i]);
    delete_node(&n);
  }
  delete_allocator(&a);
}

TEST(NodeTest, GetLocation) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "let x = 42;";
  location_t loc = make_loc(src, 8, 10); /* "42" */
  node_t *n = create_node(a, NODE_LITERAL_NUMERIC, NULL, loc);
  const location_t *got = node_get_location(n);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->begin.offset, src + 8);
  EXPECT_EQ(got->end.offset, src + 10);
  EXPECT_EQ(got->begin.line, 1u);
  EXPECT_EQ(got->begin.column, 9u);
  EXPECT_STREQ(got->filename, "test.clux");
  delete_node(&n);
  delete_allocator(&a);
}

TEST(NodeTest, GetParent) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  node_t *parent = create_node(a, NODE_PROGRAM, NULL, loc);
  node_t *child = create_node(a, NODE_LITERAL_IDENTIFIER, parent, loc);
  EXPECT_EQ(node_get_parent(child), parent);
  EXPECT_EQ(node_get_parent(parent), nullptr);
  /* 先删子再删父，避免父的 children 持有已删子指针 */
  delete_node(&child);
  delete_node(&parent);
  delete_allocator(&a);
}

/* ---- children ---- */

TEST(NodeTest, ChildrenInitiallyEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  node_t *n = create_node(a, NODE_PROGRAM, NULL, loc);
  vec_t *children = node_get_children(n);
  ASSERT_NE(children, nullptr);
  EXPECT_EQ(vec_get_size(children), 0u);
  delete_node(&n);
  delete_allocator(&a);
}

TEST(NodeTest, AddChildViaChildrenVec) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  node_t *parent = create_node(a, NODE_PROGRAM, NULL, loc);
  node_t *child = create_node(a, NODE_LITERAL_IDENTIFIER, parent, loc);
  vec_push(node_get_children(parent), child);
  EXPECT_EQ(vec_get_size(node_get_children(parent)), 1u);
  EXPECT_EQ(vec_get(node_get_children(parent), 0), child);
  delete_node(&parent); /* 父拥有子，递归释放 */
  delete_allocator(&a);
}

TEST(NodeTest, MultipleChildren) {
  allocator_t *a = create_allocator(malloc, free);
  const char *src = "abc";
  location_t loc = make_loc(src, 0, 1);
  node_t *parent = create_node(a, NODE_PROGRAM, NULL, loc);
  node_t *c1 = create_node(a, NODE_LITERAL_NUMERIC, parent, loc);
  node_t *c2 = create_node(a, NODE_LITERAL_STRING, parent, loc);
  node_t *c3 = create_node(a, NODE_LITERAL_BOOLEAN, parent, loc);
  vec_push(node_get_children(parent), c1);
  vec_push(node_get_children(parent), c2);
  vec_push(node_get_children(parent), c3);
  EXPECT_EQ(vec_get_size(node_get_children(parent)), 3u);
  EXPECT_EQ(vec_get(node_get_children(parent), 0), c1);
  EXPECT_EQ(vec_get(node_get_children(parent), 1), c2);
  EXPECT_EQ(vec_get(node_get_children(parent), 2), c3);
  delete_node(&parent);
  delete_allocator(&a);
}

/* ---- 泄漏检测（计数分配器） ---- */

TEST(NodeTest, NoLeakSingleNode) {
  reset_counts();
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  node_t *n = create_node(a, NODE_LITERAL_NUMERIC, NULL, loc);
  delete_node(&n);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

TEST(NodeTest, NoLeakParentWithChild) {
  reset_counts();
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  node_t *parent = create_node(a, NODE_PROGRAM, NULL, loc);
  node_t *child = create_node(a, NODE_LITERAL_IDENTIFIER, parent, loc);
  vec_push(node_get_children(parent), child);
  delete_node(&parent);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

TEST(NodeTest, NoLeakDeepNested) {
  reset_counts();
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  /* root -> mid -> leaf */
  node_t *root = create_node(a, NODE_PROGRAM, NULL, loc);
  node_t *mid = create_node(a, NODE_STATEMENT_EXPRESSION, root, loc);
  node_t *leaf = create_node(a, NODE_LITERAL_NUMERIC, mid, loc);
  vec_push(node_get_children(root), mid);
  vec_push(node_get_children(mid), leaf);
  delete_node(&root);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}

TEST(NodeTest, NoLeakWideTree) {
  reset_counts();
  allocator_t *a = create_allocator(counting_alloc, counting_free);
  const char *src = "x";
  location_t loc = make_loc(src, 0, 1);
  node_t *root = create_node(a, NODE_PROGRAM, NULL, loc);
  for (int i = 0; i < 10; i++) {
    node_t *child = create_node(a, NODE_LITERAL_NUMERIC, root, loc);
    /* 每个子节点再挂一个孙节点 */
    node_t *grandchild = create_node(a, NODE_LITERAL_STRING, child, loc);
    vec_push(node_get_children(child), grandchild);
    vec_push(node_get_children(root), child);
  }
  delete_node(&root);
  delete_allocator(&a);
  EXPECT_EQ(g_alloc_count, g_free_count);
}
