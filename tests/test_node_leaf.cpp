#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/vec.h"
#include "parser/node.h"
#include "parser/node_literal.h"
#include "parser/node_program.h"
#include "parser/node_stmt_jump.h"
#include "parser/node_symbol.h"
#include <stdlib.h>

namespace {

class NodeLeafTest : public ::testing::Test {
protected:
  void SetUp() override { a = create_allocator(malloc, free); }
  void TearDown() override { delete_allocator(&a); }
  allocator_t *a = nullptr;
};

} // namespace

/* ---- 叶子字面量 ---- */

TEST_F(NodeLeafTest, LiteralFactories) {
  struct {
    node_t *(*create)(allocator_t *);
    node_kind_t kind;
  } cases[] = {
      {create_literal_undefined_node, CLUX_NODE_LITERAL_UNDEFINED},
      {create_literal_string_node, CLUX_NODE_LITERAL_STRING},
      {create_literal_character_node, CLUX_NODE_LITERAL_CHARACTER},
      {create_literal_numeric_node, CLUX_NODE_LITERAL_NUMERIC},
      {create_literal_boolean_node, CLUX_NODE_LITERAL_BOOLEAN},
      {create_literal_identifier_node, CLUX_NODE_LITERAL_IDENTIFIER},
      {create_literal_type_node, CLUX_NODE_LITERAL_TYPE},
  };
  for (auto &c : cases) {
    node_t *n = c.create(a);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(node_get_kind(n), c.kind);
    EXPECT_EQ(vec_get_size(node_get_children(n)), 0u);
    EXPECT_EQ(node_get_parent(n), nullptr);
    delete_node(&n);
  }
}

/* ---- 符号 ---- */

TEST_F(NodeLeafTest, SymbolFactory) {
  node_t *n = create_symbol_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_SYMBOL);
  EXPECT_EQ(vec_get_size(node_get_children(n)), 0u);
  delete_node(&n);
}

/* ---- 跳转语句 ---- */

TEST_F(NodeLeafTest, JumpFactories) {
  node_t *brk = create_stmt_break_node(a);
  EXPECT_EQ(node_get_kind(brk), CLUX_NODE_STMT_BREAK);
  EXPECT_EQ(vec_get_size(node_get_children(brk)), 0u);
  delete_node(&brk);

  node_t *cont = create_stmt_continue_node(a);
  EXPECT_EQ(node_get_kind(cont), CLUX_NODE_STMT_CONTINUE);
  EXPECT_EQ(vec_get_size(node_get_children(cont)), 0u);
  delete_node(&cont);
}

/* ---- 程序根节点 ---- */

TEST_F(NodeLeafTest, Program) {
  node_t *n = create_program_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_PROGRAM);
  EXPECT_EQ(program_node_get_stmt_count(n), 0u);
  EXPECT_EQ(program_node_get_stmt(n, 0), nullptr);

  node_t *s0 = create_literal_numeric_node(a);
  node_t *s1 = create_literal_identifier_node(a);
  program_node_add_stmt(n, s0);
  program_node_add_stmt(n, s1);
  EXPECT_EQ(program_node_get_stmt_count(n), 2u);
  EXPECT_EQ(program_node_get_stmt(n, 0), s0);
  EXPECT_EQ(program_node_get_stmt(n, 1), s1);
  EXPECT_EQ(program_node_get_stmt(n, 2), nullptr);
  EXPECT_EQ(node_get_parent(s0), n);
  delete_node(&n);
}
