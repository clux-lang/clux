#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/vec.h"
#include "parser/node.h"
#include "parser/node_binary.h"
#include <stdlib.h>

/* ---- 二元节点语义 API ---- */

TEST(NodeBinaryTest, CreateHasThreeEmptySlots) {
  allocator_t *a = create_allocator(malloc, free);
  node_t *n = create_binary_node(a);
  ASSERT_NE(n, nullptr);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_EXPR_BINARY_CHILDREN_SIZE);
  EXPECT_EQ(binary_node_get_left(n), nullptr);
  EXPECT_EQ(binary_node_get_symbol(n), nullptr);
  EXPECT_EQ(binary_node_get_right(n), nullptr);
  delete_node(&n);
  delete_allocator(&a);
}

TEST(NodeBinaryTest, UnaryLeavesLeftEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  node_t *n = create_unary_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(binary_node_get_left(n), nullptr);
  delete_node(&n);
  delete_allocator(&a);
}

TEST(NodeBinaryTest, PostfixLeavesRightEmpty) {
  allocator_t *a = create_allocator(malloc, free);
  node_t *n = create_postfix_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_BINARY);
  EXPECT_EQ(binary_node_get_right(n), nullptr);
  delete_node(&n);
  delete_allocator(&a);
}

TEST(NodeBinaryTest, SettersAndGetters) {
  allocator_t *a = create_allocator(malloc, free);
  node_t *n = create_binary_node(a);
  node_t *left = create_node(a, CLUX_NODE_LITERAL_NUMERIC);
  node_t *symbol = create_node(a, CLUX_NODE_SYMBOL);
  node_t *right = create_node(a, CLUX_NODE_LITERAL_STRING);

  binary_node_set_left(n, left);
  binary_node_set_symbol(n, symbol);
  binary_node_set_right(n, right);

  EXPECT_EQ(binary_node_get_left(n), left);
  EXPECT_EQ(binary_node_get_symbol(n), symbol);
  EXPECT_EQ(binary_node_get_right(n), right);
  /* 槽位写入同时建立父子关系。 */
  EXPECT_EQ(node_get_parent(left), n);
  EXPECT_EQ(node_get_parent(symbol), n);
  EXPECT_EQ(node_get_parent(right), n);

  delete_node(&n); /* 父拥有子，递归释放 */
  delete_allocator(&a);
}

TEST(NodeBinaryTest, SetLeftNullClearsSlot) {
  allocator_t *a = create_allocator(malloc, free);
  node_t *n = create_binary_node(a);
  node_t *left = create_node(a, CLUX_NODE_LITERAL_NUMERIC);
  binary_node_set_left(n, left);
  ASSERT_EQ(binary_node_get_left(n), left);
  binary_node_set_left(n, nullptr);
  EXPECT_EQ(binary_node_get_left(n), nullptr);
  delete_node(&left);
  delete_node(&n);
  delete_allocator(&a);
}
