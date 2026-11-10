#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/vec.h"
#include "parser/node.h"
#include "parser/node_assign.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_decorator.h"
#include "parser/node_enum_decl.h"
#include "parser/node_enum_variant.h"
#include "parser/node_field.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_func_type.h"
#include "parser/node_generic_param.h"
#include "parser/node_group.h"
#include "parser/node_implements.h"
#include "parser/node_rest.h"
#include "parser/node_slice.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/node_stmt_do_while.h"
#include "parser/node_stmt_empty.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_switch.h"
#include "parser/node_stmt_type.h"
#include "parser/node_switch_branch.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_while.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_subscript.h"
#include "parser/node_ternary.h"
#include "parser/node_tuple.h"
#include "parser/node_union.h"
#include <stdlib.h>

namespace {

node_t *make_leaf(allocator_t *a) {
  return create_node(a, CLUX_NODE_LITERAL_IDENTIFIER);
}

/* 创建节点、写入若干子节点后整体释放，验证槽位读写与父子关系。 */
class NodeSemanticTest : public ::testing::Test {
protected:
  void SetUp() override { a = create_allocator(malloc, free); }
  void TearDown() override { delete_allocator(&a); }
  allocator_t *a = nullptr;
};

} // namespace

/* ---- group ---- */

TEST_F(NodeSemanticTest, Group) {
  node_t *n = create_group_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_GROUP);
  EXPECT_EQ(vec_get_size(node_get_children(n)), 1u);
  EXPECT_EQ(group_node_get_expr(n), nullptr);
  node_t *inner = make_leaf(a);
  group_node_set_expr(n, inner);
  EXPECT_EQ(group_node_get_expr(n), inner);
  EXPECT_EQ(node_get_parent(inner), n);
  delete_node(&n);
}

/* ---- rest ---- */

TEST_F(NodeSemanticTest, RestWithoutExpr) {
  node_t *n = create_rest_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_REST);
  EXPECT_EQ(vec_get_size(node_get_children(n)), 0u);
  EXPECT_EQ(rest_node_get_expr(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, RestWithExpr) {
  node_t *n = create_rest_node(a);
  node_t *operand = make_leaf(a);
  rest_node_set_expr(n, operand);
  EXPECT_EQ(vec_get_size(node_get_children(n)), 1u);
  EXPECT_EQ(rest_node_get_expr(n), operand);
  EXPECT_EQ(node_get_parent(operand), n);
  delete_node(&n);
}

/* 已有操作数时再次设置：children 已扩到 1 槽，不应重复扩容。 */
TEST_F(NodeSemanticTest, RestReplaceExpr) {
  node_t *n = create_rest_node(a);
  node_t *first = make_leaf(a);
  node_t *second = make_leaf(a);
  rest_node_set_expr(n, first);
  rest_node_set_expr(n, second);
  EXPECT_EQ(vec_get_size(node_get_children(n)), 1u);
  EXPECT_EQ(rest_node_get_expr(n), second);
  delete_node(&first);
  delete_node(&n);
}

/* ---- slice ---- */

TEST_F(NodeSemanticTest, Slice) {
  node_t *n = create_slice_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_SLICE);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_EXPR_SLICE_CHILDREN_SIZE);
  node_t *base = make_leaf(a);
  node_t *lo = make_leaf(a);
  node_t *hi = make_leaf(a);
  slice_node_set_base(n, base);
  slice_node_set_lo(n, lo);
  slice_node_set_hi(n, hi);
  EXPECT_EQ(slice_node_get_base(n), base);
  EXPECT_EQ(slice_node_get_lo(n), lo);
  EXPECT_EQ(slice_node_get_hi(n), hi);
  EXPECT_EQ(node_get_parent(hi), n);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, SliceOmitsEnds) {
  node_t *n = create_slice_node(a);
  node_t *base = make_leaf(a);
  slice_node_set_base(n, base);
  slice_node_set_lo(n, nullptr);
  slice_node_set_hi(n, nullptr);
  EXPECT_EQ(slice_node_get_lo(n), nullptr);
  EXPECT_EQ(slice_node_get_hi(n), nullptr);
  delete_node(&n);
}

/* ---- subscript ---- */

TEST_F(NodeSemanticTest, Subscript) {
  node_t *n = create_subscript_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_SUBSCRIPT);
  EXPECT_EQ(subscript_node_get_arg_count(n), 0u);
  node_t *base = make_leaf(a);
  subscript_node_set_base(n, base);
  EXPECT_EQ(subscript_node_get_base(n), base);
  node_t *arg0 = make_leaf(a);
  node_t *arg1 = make_leaf(a);
  subscript_node_add_arg(n, arg0);
  subscript_node_add_arg(n, arg1);
  EXPECT_EQ(subscript_node_get_arg_count(n), 2u);
  EXPECT_EQ(subscript_node_get_arg(n, 0), arg0);
  EXPECT_EQ(subscript_node_get_arg(n, 1), arg1);
  EXPECT_EQ(subscript_node_get_arg(n, 2), nullptr);
  delete_node(&n);
}

/* ---- call ---- */

TEST_F(NodeSemanticTest, Call) {
  node_t *n = create_call_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_CALL);
  EXPECT_EQ(call_node_get_arg_count(n), 0u);
  node_t *callee = make_leaf(a);
  call_node_set_callee(n, callee);
  EXPECT_EQ(call_node_get_callee(n), callee);
  node_t *arg = make_leaf(a);
  call_node_add_arg(n, arg);
  EXPECT_EQ(call_node_get_arg_count(n), 1u);
  EXPECT_EQ(call_node_get_arg(n, 0), arg);
  EXPECT_EQ(call_node_get_arg(n, 1), nullptr);
  delete_node(&n);
}

/* ---- construct ---- */

TEST_F(NodeSemanticTest, Construct) {
  node_t *n = create_construct_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_CONSTRUCT);
  EXPECT_EQ(construct_node_get_elem_count(n), 0u);
  node_t *type = make_leaf(a);
  construct_node_set_type(n, type);
  EXPECT_EQ(construct_node_get_type(n), type);
  node_t *elem = make_leaf(a);
  construct_node_add_elem(n, elem);
  EXPECT_EQ(construct_node_get_elem_count(n), 1u);
  EXPECT_EQ(construct_node_get_elem(n, 0), elem);
  EXPECT_EQ(construct_node_get_elem(n, 1), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, ConstructAnonymous) {
  node_t *n = create_construct_node(a);
  construct_node_set_type(n, nullptr);
  EXPECT_EQ(construct_node_get_type(n), nullptr);
  delete_node(&n);
}

/* ---- ternary ---- */

TEST_F(NodeSemanticTest, Ternary) {
  node_t *n = create_ternary_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_TERNARY);
  node_t *cond = make_leaf(a);
  node_t *then = make_leaf(a);
  node_t *else_ = make_leaf(a);
  ternary_node_set_condition(n, cond);
  ternary_node_set_then(n, then);
  ternary_node_set_else(n, else_);
  EXPECT_EQ(ternary_node_get_condition(n), cond);
  EXPECT_EQ(ternary_node_get_then(n), then);
  EXPECT_EQ(ternary_node_get_else(n), else_);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, TernaryMissingBranches) {
  node_t *n = create_ternary_node(a);
  node_t *cond = make_leaf(a);
  ternary_node_set_condition(n, cond);
  ternary_node_set_then(n, nullptr);
  ternary_node_set_else(n, nullptr);
  EXPECT_EQ(ternary_node_get_then(n), nullptr);
  EXPECT_EQ(ternary_node_get_else(n), nullptr);
  delete_node(&n);
}

/* ---- assign ---- */

TEST_F(NodeSemanticTest, Assign) {
  node_t *n = create_assign_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_ASSIGN);
  node_t *id = make_leaf(a);
  node_t *symbol = make_leaf(a);
  node_t *value = make_leaf(a);
  assign_node_set_id(n, id);
  assign_node_set_symbol(n, symbol);
  assign_node_set_value(n, value);
  EXPECT_EQ(assign_node_get_id(n), id);
  EXPECT_EQ(assign_node_get_symbol(n), symbol);
  EXPECT_EQ(assign_node_get_value(n), value);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, AssignMissingValue) {
  node_t *n = create_assign_node(a);
  node_t *id = make_leaf(a);
  node_t *symbol = make_leaf(a);
  assign_node_set_id(n, id);
  assign_node_set_symbol(n, symbol);
  assign_node_set_value(n, nullptr);
  EXPECT_EQ(assign_node_get_value(n), nullptr);
  delete_node(&n);
}

/* ---- tuple ---- */

TEST_F(NodeSemanticTest, Tuple) {
  node_t *n = create_tuple_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_TUPLE);
  EXPECT_EQ(tuple_node_get_elem_count(n), 0u);
  node_t *e0 = make_leaf(a);
  node_t *e1 = make_leaf(a);
  tuple_node_add_elem(n, e0);
  tuple_node_add_elem(n, e1);
  EXPECT_EQ(tuple_node_get_elem_count(n), 2u);
  EXPECT_EQ(tuple_node_get_elem(n, 0), e0);
  EXPECT_EQ(tuple_node_get_elem(n, 1), e1);
  EXPECT_EQ(tuple_node_get_elem(n, 2), nullptr);
  delete_node(&n);
}

/* ---- func_type ---- */

TEST_F(NodeSemanticTest, FuncType) {
  node_t *n = create_func_type_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_FUNC_TYPE);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_EXPR_FUNC_TYPE_CHILDREN_SIZE);
  node_t *captures = make_leaf(a);
  node_t *params = make_leaf(a);
  node_t *ret = make_leaf(a);
  func_type_node_set_captures(n, captures);
  func_type_node_set_params(n, params);
  func_type_node_set_return(n, ret);
  EXPECT_EQ(func_type_node_get_captures(n), captures);
  EXPECT_EQ(func_type_node_get_params(n), params);
  EXPECT_EQ(func_type_node_get_return(n), ret);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FuncTypeWithoutCaptures) {
  node_t *n = create_func_type_node(a);
  func_type_node_set_captures(n, nullptr);
  EXPECT_EQ(func_type_node_get_captures(n), nullptr);
  delete_node(&n);
}

/* ---- func_decl ---- */

TEST_F(NodeSemanticTest, FuncDecl) {
  node_t *n = create_func_decl_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_FUNC_DECL);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_EXPR_FUNC_DECL_CHILDREN_SIZE);
  node_t *keyword = make_leaf(a);
  node_t *name = make_leaf(a);
  node_t *generics = make_leaf(a);
  node_t *captures = make_leaf(a);
  node_t *params = make_leaf(a);
  node_t *ret = make_leaf(a);
  node_t *body = make_leaf(a);
  func_decl_node_set_keyword(n, keyword);
  func_decl_node_set_name(n, name);
  func_decl_node_set_generics(n, generics);
  func_decl_node_set_captures(n, captures);
  func_decl_node_set_params(n, params);
  func_decl_node_set_return(n, ret);
  func_decl_node_set_body(n, body);
  EXPECT_EQ(func_decl_node_get_keyword(n), keyword);
  EXPECT_EQ(func_decl_node_get_name(n), name);
  EXPECT_EQ(func_decl_node_get_generics(n), generics);
  EXPECT_EQ(func_decl_node_get_captures(n), captures);
  EXPECT_EQ(func_decl_node_get_params(n), params);
  EXPECT_EQ(func_decl_node_get_return(n), ret);
  EXPECT_EQ(func_decl_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FuncDeclAnonymous) {
  node_t *n = create_func_decl_node(a);
  func_decl_node_set_name(n, nullptr);
  func_decl_node_set_generics(n, nullptr);
  func_decl_node_set_captures(n, nullptr);
  func_decl_node_set_return(n, nullptr);
  func_decl_node_set_body(n, nullptr);
  EXPECT_EQ(func_decl_node_get_name(n), nullptr);
  EXPECT_EQ(func_decl_node_get_generics(n), nullptr);
  EXPECT_EQ(func_decl_node_get_captures(n), nullptr);
  EXPECT_EQ(func_decl_node_get_return(n), nullptr);
  EXPECT_EQ(func_decl_node_get_body(n), nullptr);
  delete_node(&n);
}

/* ---- union / field / fields ---- */

TEST_F(NodeSemanticTest, Union) {
  node_t *n = create_union_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_UNION);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_EXPR_UNION_CHILDREN_SIZE);
  node_t *keyword = make_leaf(a);
  node_t *name = make_leaf(a);
  node_t *generics = make_leaf(a);
  node_t *body = make_leaf(a);
  union_node_set_keyword(n, keyword);
  union_node_set_name(n, name);
  union_node_set_generics(n, generics);
  union_node_set_body(n, body);
  EXPECT_EQ(union_node_get_keyword(n), keyword);
  EXPECT_EQ(union_node_get_name(n), name);
  EXPECT_EQ(union_node_get_generics(n), generics);
  EXPECT_EQ(union_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, UnionWithoutGenerics) {
  node_t *n = create_union_node(a);
  union_node_set_generics(n, nullptr);
  EXPECT_EQ(union_node_get_generics(n), nullptr);
  delete_node(&n);
}

/* ---- enum ---- */

TEST_F(NodeSemanticTest, EnumDecl) {
  node_t *n = create_enum_decl_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_ENUM_DECL);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_EXPR_ENUM_DECL_CHILDREN_SIZE);
  node_t *keyword = make_leaf(a);
  node_t *name = make_leaf(a);
  node_t *underlying = make_leaf(a);
  node_t *body = make_leaf(a);
  enum_decl_node_set_keyword(n, keyword);
  enum_decl_node_set_name(n, name);
  enum_decl_node_set_underlying(n, underlying);
  enum_decl_node_set_body(n, body);
  EXPECT_EQ(enum_decl_node_get_keyword(n), keyword);
  EXPECT_EQ(enum_decl_node_get_name(n), name);
  EXPECT_EQ(enum_decl_node_get_underlying(n), underlying);
  EXPECT_EQ(enum_decl_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, EnumVariant) {
  node_t *n = create_enum_variant_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_ENUM_VARIANT);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_ENUM_VARIANT_CHILDREN_SIZE);
  node_t *name = make_leaf(a);
  node_t *value = make_leaf(a);
  enum_variant_node_set_name(n, name);
  enum_variant_node_set_value(n, value);
  EXPECT_EQ(enum_variant_node_get_name(n), name);
  EXPECT_EQ(enum_variant_node_get_value(n), value);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, EnumVariantWithoutValue) {
  node_t *n = create_enum_variant_node(a);
  enum_variant_node_set_value(n, nullptr);
  EXPECT_EQ(enum_variant_node_get_value(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, EnumVariants) {
  node_t *n = create_enum_variants_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_ENUM_VARIANTS);
  EXPECT_EQ(enum_variants_node_get_variant_count(n), 0u);
  node_t *first = make_leaf(a);
  node_t *second = make_leaf(a);
  enum_variants_node_add_variant(n, first);
  enum_variants_node_add_variant(n, second);
  EXPECT_EQ(enum_variants_node_get_variant_count(n), 2u);
  EXPECT_EQ(enum_variants_node_get_variant(n, 0), first);
  EXPECT_EQ(enum_variants_node_get_variant(n, 1), second);
  EXPECT_EQ(enum_variants_node_get_variant(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, Field) {
  node_t *n = create_field_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_FIELD);
  node_t *name = make_leaf(a);
  node_t *type = make_leaf(a);
  field_node_set_name(n, name);
  field_node_set_type(n, type);
  EXPECT_EQ(field_node_get_name(n), name);
  EXPECT_EQ(field_node_get_type(n), type);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FieldWithoutType) {
  node_t *n = create_field_node(a);
  node_t *name = make_leaf(a);
  field_node_set_name(n, name);
  field_node_set_type(n, nullptr);
  EXPECT_EQ(field_node_get_type(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, Fields) {
  node_t *n = create_fields_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_FIELDS);
  EXPECT_EQ(fields_node_get_field_count(n), 0u);
  node_t *f0 = create_field_node(a);
  node_t *f1 = create_field_node(a);
  fields_node_add_field(n, f0);
  fields_node_add_field(n, f1);
  EXPECT_EQ(fields_node_get_field_count(n), 2u);
  EXPECT_EQ(fields_node_get_field(n, 0), f0);
  EXPECT_EQ(fields_node_get_field(n, 1), f1);
  EXPECT_EQ(fields_node_get_field(n, 2), nullptr);
  delete_node(&n);
}

/* ---- func_param / func_params ---- */

TEST_F(NodeSemanticTest, FuncParam) {
  node_t *n = create_func_param_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_FUNC_PARAM);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_FUNC_PARAM_CHILDREN_SIZE);
  node_t *name = make_leaf(a);
  node_t *type = make_leaf(a);
  node_t *def = make_leaf(a);
  func_param_node_set_name(n, name);
  func_param_node_set_type(n, type);
  func_param_node_set_default(n, def);
  EXPECT_EQ(func_param_node_get_name(n), name);
  EXPECT_EQ(func_param_node_get_type(n), type);
  EXPECT_EQ(func_param_node_get_default(n), def);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FuncParamAllOptional) {
  node_t *n = create_func_param_node(a);
  func_param_node_set_name(n, nullptr);
  func_param_node_set_type(n, nullptr);
  func_param_node_set_default(n, nullptr);
  EXPECT_EQ(func_param_node_get_name(n), nullptr);
  EXPECT_EQ(func_param_node_get_type(n), nullptr);
  EXPECT_EQ(func_param_node_get_default(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FuncParams) {
  node_t *n = create_func_params_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_FUNC_PARAMS);
  EXPECT_EQ(func_params_node_get_param_count(n), 0u);
  node_t *p0 = create_func_param_node(a);
  node_t *p1 = create_func_param_node(a);
  func_params_node_add_param(n, p0);
  func_params_node_add_param(n, p1);
  EXPECT_EQ(func_params_node_get_param_count(n), 2u);
  EXPECT_EQ(func_params_node_get_param(n, 0), p0);
  EXPECT_EQ(func_params_node_get_param(n, 1), p1);
  EXPECT_EQ(func_params_node_get_param(n, 2), nullptr);
  delete_node(&n);
}

/* ---- generic_param / generic_params ---- */

TEST_F(NodeSemanticTest, GenericParam) {
  node_t *n = create_generic_param_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_GENERIC_PARAM);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_GENERIC_PARAM_CHILDREN_SIZE);
  node_t *name = make_leaf(a);
  node_t *type = make_leaf(a);
  node_t *constraint = make_leaf(a);
  node_t *def = make_leaf(a);
  generic_param_node_set_name(n, name);
  generic_param_node_set_type(n, type);
  generic_param_node_set_constraint(n, constraint);
  generic_param_node_set_default(n, def);
  EXPECT_EQ(generic_param_node_get_name(n), name);
  EXPECT_EQ(generic_param_node_get_type(n), type);
  EXPECT_EQ(generic_param_node_get_constraint(n), constraint);
  EXPECT_EQ(generic_param_node_get_default(n), def);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, GenericParamAllOptional) {
  node_t *n = create_generic_param_node(a);
  generic_param_node_set_name(n, nullptr);
  generic_param_node_set_type(n, nullptr);
  generic_param_node_set_constraint(n, nullptr);
  generic_param_node_set_default(n, nullptr);
  EXPECT_EQ(generic_param_node_get_name(n), nullptr);
  EXPECT_EQ(generic_param_node_get_type(n), nullptr);
  EXPECT_EQ(generic_param_node_get_constraint(n), nullptr);
  EXPECT_EQ(generic_param_node_get_default(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, GenericParams) {
  node_t *n = create_generic_params_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_GENERIC_PARAMS);
  EXPECT_EQ(generic_params_node_get_param_count(n), 0u);
  node_t *p0 = create_generic_param_node(a);
  node_t *p1 = create_generic_param_node(a);
  generic_params_node_add_param(n, p0);
  generic_params_node_add_param(n, p1);
  EXPECT_EQ(generic_params_node_get_param_count(n), 2u);
  EXPECT_EQ(generic_params_node_get_param(n, 0), p0);
  EXPECT_EQ(generic_params_node_get_param(n, 1), p1);
  EXPECT_EQ(generic_params_node_get_param(n, 2), nullptr);
  delete_node(&n);
}

/* ---- func_capture / func_captures ---- */

TEST_F(NodeSemanticTest, FuncCapture) {
  node_t *n = create_func_capture_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_FUNC_CAPTURE);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_FUNC_CAPTURE_CHILDREN_SIZE);
  node_t *name = make_leaf(a);
  node_t *type = make_leaf(a);
  func_capture_node_set_name(n, name);
  func_capture_node_set_type(n, type);
  EXPECT_EQ(func_capture_node_get_name(n), name);
  EXPECT_EQ(func_capture_node_get_type(n), type);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FuncCaptureWithoutType) {
  node_t *n = create_func_capture_node(a);
  node_t *name = make_leaf(a);
  func_capture_node_set_name(n, name);
  func_capture_node_set_type(n, nullptr);
  EXPECT_EQ(func_capture_node_get_type(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FuncCaptures) {
  node_t *n = create_func_captures_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_FUNC_CAPTURES);
  EXPECT_EQ(func_captures_node_get_capture_count(n), 0u);
  node_t *c0 = create_func_capture_node(a);
  node_t *c1 = create_func_capture_node(a);
  func_captures_node_add_capture(n, c0);
  func_captures_node_add_capture(n, c1);
  EXPECT_EQ(func_captures_node_get_capture_count(n), 2u);
  EXPECT_EQ(func_captures_node_get_capture(n, 0), c0);
  EXPECT_EQ(func_captures_node_get_capture(n, 1), c1);
  EXPECT_EQ(func_captures_node_get_capture(n, 2), nullptr);
  delete_node(&n);
}

/* ---- 语句节点 ---- */

TEST_F(NodeSemanticTest, StmtBlock) {
  node_t *n = create_stmt_block_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_BLOCK);
  EXPECT_EQ(stmt_block_node_get_stmt_count(n), 0u);
  node_t *s0 = make_leaf(a);
  node_t *s1 = make_leaf(a);
  stmt_block_node_add_stmt(n, s0);
  stmt_block_node_add_stmt(n, s1);
  EXPECT_EQ(stmt_block_node_get_stmt_count(n), 2u);
  EXPECT_EQ(stmt_block_node_get_stmt(n, 0), s0);
  EXPECT_EQ(stmt_block_node_get_stmt(n, 1), s1);
  EXPECT_EQ(stmt_block_node_get_stmt(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtExpr) {
  node_t *n = create_stmt_expr_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_EXPR);
  node_t *expr = make_leaf(a);
  stmt_expr_node_set_expr(n, expr);
  EXPECT_EQ(stmt_expr_node_get_expr(n), expr);
  EXPECT_EQ(node_get_parent(expr), n);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtReturn) {
  node_t *n = create_stmt_return_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_RETURN);
  EXPECT_EQ(stmt_return_node_get_value(n), nullptr);
  node_t *value = make_leaf(a);
  stmt_return_node_set_value(n, value);
  EXPECT_EQ(stmt_return_node_get_value(n), value);
  stmt_return_node_set_value(n, nullptr);
  EXPECT_EQ(stmt_return_node_get_value(n), nullptr);
  delete_node(&value);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtFunc) {
  node_t *n = create_stmt_func_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_FUNC);
  EXPECT_EQ(stmt_func_node_get_decorator_count(n), 0u);
  node_t *decl = create_func_decl_node(a);
  stmt_func_node_set_func_decl(n, decl);
  EXPECT_EQ(stmt_func_node_get_func_decl(n), decl);
  node_t *d0 = create_decorator_node(a);
  node_t *d1 = create_decorator_node(a);
  stmt_func_node_add_decorator(n, d0);
  stmt_func_node_add_decorator(n, d1);
  EXPECT_EQ(stmt_func_node_get_decorator_count(n), 2u);
  EXPECT_EQ(stmt_func_node_get_decorator(n, 0), d0);
  EXPECT_EQ(stmt_func_node_get_decorator(n, 1), d1);
  EXPECT_EQ(stmt_func_node_get_decorator(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtVar) {
  node_t *n = create_stmt_var_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_VAR);
  EXPECT_EQ(stmt_var_node_get_decorator_count(n), 0u);
  node_t *keyword = make_leaf(a);
  node_t *name = make_leaf(a);
  node_t *type = make_leaf(a);
  node_t *value = make_leaf(a);
  node_t *comptime = make_leaf(a);
  stmt_var_node_set_keyword(n, keyword);
  stmt_var_node_set_name(n, name);
  stmt_var_node_set_type(n, type);
  stmt_var_node_set_value(n, value);
  stmt_var_node_set_comptime(n, comptime);
  EXPECT_EQ(stmt_var_node_get_keyword(n), keyword);
  EXPECT_EQ(stmt_var_node_get_name(n), name);
  EXPECT_EQ(stmt_var_node_get_type(n), type);
  EXPECT_EQ(stmt_var_node_get_value(n), value);
  EXPECT_EQ(stmt_var_node_get_comptime(n), comptime);
  node_t *d0 = create_decorator_node(a);
  stmt_var_node_add_decorator(n, d0);
  EXPECT_EQ(stmt_var_node_get_decorator_count(n), 1u);
  EXPECT_EQ(stmt_var_node_get_decorator(n, 0), d0);
  EXPECT_EQ(stmt_var_node_get_decorator(n, 1), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtVarWithoutType) {
  node_t *n = create_stmt_var_node(a);
  stmt_var_node_set_type(n, nullptr);
  stmt_var_node_set_comptime(n, nullptr);
  EXPECT_EQ(stmt_var_node_get_type(n), nullptr);
  EXPECT_EQ(stmt_var_node_get_comptime(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtType) {
  node_t *n = create_stmt_type_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_TYPE);
  EXPECT_EQ(stmt_type_node_get_decorator_count(n), 0u);
  node_t *name = make_leaf(a);
  node_t *generics = make_leaf(a);
  node_t *value = make_leaf(a);
  stmt_type_node_set_name(n, name);
  stmt_type_node_set_generics(n, generics);
  stmt_type_node_set_value(n, value);
  EXPECT_EQ(stmt_type_node_get_name(n), name);
  EXPECT_EQ(stmt_type_node_get_generics(n), generics);
  EXPECT_EQ(stmt_type_node_get_value(n), value);
  node_t *d0 = create_decorator_node(a);
  stmt_type_node_add_decorator(n, d0);
  EXPECT_EQ(stmt_type_node_get_decorator_count(n), 1u);
  EXPECT_EQ(stmt_type_node_get_decorator(n, 0), d0);
  EXPECT_EQ(stmt_type_node_get_decorator(n, 1), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtTypeWithoutGenerics) {
  node_t *n = create_stmt_type_node(a);
  stmt_type_node_set_generics(n, nullptr);
  EXPECT_EQ(stmt_type_node_get_generics(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtEmpty) {
  node_t *n = create_stmt_empty_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_EMPTY);
  EXPECT_EQ(vec_get_size(node_get_children(n)), 0u);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtSwitch) {
  node_t *n = create_stmt_switch_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_SWITCH);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_SWITCH_CHILDREN_SIZE);
  node_t *value = make_leaf(a);
  node_t *body = make_leaf(a);
  stmt_switch_node_set_value(n, value);
  stmt_switch_node_set_body(n, body);
  EXPECT_EQ(stmt_switch_node_get_value(n), value);
  EXPECT_EQ(stmt_switch_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, SwitchBranch) {
  node_t *n = create_switch_branch_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_SWITCH_BRANCH);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_SWITCH_BRANCH_CHILDREN_SIZE);
  node_t *patterns = make_leaf(a);
  node_t *body = make_leaf(a);
  switch_branch_node_set_patterns(n, patterns);
  switch_branch_node_set_body(n, body);
  EXPECT_EQ(switch_branch_node_get_patterns(n), patterns);
  EXPECT_EQ(switch_branch_node_get_body(n), body);
  switch_branch_node_set_patterns(n, nullptr);
  EXPECT_EQ(switch_branch_node_get_patterns(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, SwitchBranches) {
  node_t *n = create_switch_branches_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_SWITCH_BRANCHES);
  EXPECT_EQ(switch_branches_node_get_branch_count(n), 0u);
  node_t *first = make_leaf(a);
  node_t *second = make_leaf(a);
  switch_branches_node_add_branch(n, first);
  switch_branches_node_add_branch(n, second);
  EXPECT_EQ(switch_branches_node_get_branch_count(n), 2u);
  EXPECT_EQ(switch_branches_node_get_branch(n, 0), first);
  EXPECT_EQ(switch_branches_node_get_branch(n, 1), second);
  EXPECT_EQ(switch_branches_node_get_branch(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, SwitchPatterns) {
  node_t *n = create_switch_patterns_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_SWITCH_PATTERNS);
  EXPECT_EQ(switch_patterns_node_get_pattern_count(n), 0u);
  node_t *first = make_leaf(a);
  node_t *second = make_leaf(a);
  switch_patterns_node_add_pattern(n, first);
  switch_patterns_node_add_pattern(n, second);
  EXPECT_EQ(switch_patterns_node_get_pattern_count(n), 2u);
  EXPECT_EQ(switch_patterns_node_get_pattern(n, 0), first);
  EXPECT_EQ(switch_patterns_node_get_pattern(n, 1), second);
  EXPECT_EQ(switch_patterns_node_get_pattern(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtDoWhile) {
  node_t *n = create_stmt_do_while_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_DO_WHILE);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_DO_WHILE_CHILDREN_SIZE);
  node_t *body = make_leaf(a);
  node_t *condition = make_leaf(a);
  stmt_do_while_node_set_body(n, body);
  stmt_do_while_node_set_condition(n, condition);
  EXPECT_EQ(stmt_do_while_node_get_body(n), body);
  EXPECT_EQ(stmt_do_while_node_get_condition(n), condition);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtForeach) {
  node_t *n = create_stmt_foreach_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_FOREACH);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_FOREACH_CHILDREN_SIZE);
  node_t *keyword = make_leaf(a);
  node_t *name = make_leaf(a);
  node_t *type = make_leaf(a);
  node_t *iterable = make_leaf(a);
  node_t *body = make_leaf(a);
  stmt_foreach_node_set_keyword(n, keyword);
  stmt_foreach_node_set_name(n, name);
  stmt_foreach_node_set_type(n, type);
  stmt_foreach_node_set_iterable(n, iterable);
  stmt_foreach_node_set_body(n, body);
  EXPECT_EQ(stmt_foreach_node_get_keyword(n), keyword);
  EXPECT_EQ(stmt_foreach_node_get_name(n), name);
  EXPECT_EQ(stmt_foreach_node_get_type(n), type);
  EXPECT_EQ(stmt_foreach_node_get_iterable(n), iterable);
  EXPECT_EQ(stmt_foreach_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtForeachOptionalSlots) {
  node_t *n = create_stmt_foreach_node(a);
  stmt_foreach_node_set_keyword(n, nullptr);
  stmt_foreach_node_set_type(n, nullptr);
  EXPECT_EQ(stmt_foreach_node_get_keyword(n), nullptr);
  EXPECT_EQ(stmt_foreach_node_get_type(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtFor) {
  node_t *n = create_stmt_for_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_FOR);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_FOR_CHILDREN_SIZE);
  node_t *init = make_leaf(a);
  node_t *condition = make_leaf(a);
  node_t *update = make_leaf(a);
  node_t *body = make_leaf(a);
  stmt_for_node_set_init(n, init);
  stmt_for_node_set_condition(n, condition);
  stmt_for_node_set_update(n, update);
  stmt_for_node_set_body(n, body);
  EXPECT_EQ(stmt_for_node_get_init(n), init);
  EXPECT_EQ(stmt_for_node_get_condition(n), condition);
  EXPECT_EQ(stmt_for_node_get_update(n), update);
  EXPECT_EQ(stmt_for_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtForOptionalSlots) {
  node_t *n = create_stmt_for_node(a);
  stmt_for_node_set_init(n, nullptr);
  stmt_for_node_set_condition(n, nullptr);
  stmt_for_node_set_update(n, nullptr);
  EXPECT_EQ(stmt_for_node_get_init(n), nullptr);
  EXPECT_EQ(stmt_for_node_get_condition(n), nullptr);
  EXPECT_EQ(stmt_for_node_get_update(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtWhile) {
  node_t *n = create_stmt_while_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_WHILE);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_WHILE_CHILDREN_SIZE);
  node_t *condition = make_leaf(a);
  node_t *body = make_leaf(a);
  stmt_while_node_set_condition(n, condition);
  stmt_while_node_set_body(n, body);
  EXPECT_EQ(stmt_while_node_get_condition(n), condition);
  EXPECT_EQ(stmt_while_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtIf) {
  node_t *n = create_stmt_if_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_IF);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_IF_CHILDREN_SIZE);
  node_t *condition = make_leaf(a);
  node_t *then_branch = make_leaf(a);
  node_t *else_branch = make_leaf(a);
  stmt_if_node_set_condition(n, condition);
  stmt_if_node_set_then(n, then_branch);
  stmt_if_node_set_else(n, else_branch);
  EXPECT_EQ(stmt_if_node_get_condition(n), condition);
  EXPECT_EQ(stmt_if_node_get_then(n), then_branch);
  EXPECT_EQ(stmt_if_node_get_else(n), else_branch);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtComptimeIf) {
  node_t *n = create_stmt_comptime_if_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_COMPTIME_IF);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_IF_CHILDREN_SIZE);
  node_t *condition = make_leaf(a);
  node_t *then_branch = make_leaf(a);
  stmt_if_node_set_condition(n, condition);
  stmt_if_node_set_then(n, then_branch);
  EXPECT_EQ(stmt_if_node_get_condition(n), condition);
  EXPECT_EQ(stmt_if_node_get_then(n), then_branch);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtIfWithoutElse) {
  node_t *n = create_stmt_if_node(a);
  stmt_if_node_set_else(n, nullptr);
  EXPECT_EQ(stmt_if_node_get_else(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtDefer) {
  node_t *n = create_stmt_defer_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_DEFER);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STMT_DEFER_CHILDREN_SIZE);
  node_t *captures = create_func_captures_node(a);
  node_t *body = create_stmt_block_node(a);
  stmt_defer_node_set_captures(n, captures);
  stmt_defer_node_set_body(n, body);
  EXPECT_EQ(stmt_defer_node_get_captures(n), captures);
  EXPECT_EQ(stmt_defer_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtDeferWithoutCaptures) {
  node_t *n = create_stmt_defer_node(a);
  stmt_defer_node_set_captures(n, nullptr);
  stmt_defer_node_set_body(n, nullptr);
  EXPECT_EQ(stmt_defer_node_get_captures(n), nullptr);
  EXPECT_EQ(stmt_defer_node_get_body(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtEnum) {
  node_t *n = create_stmt_enum_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_ENUM);
  EXPECT_EQ(stmt_enum_node_get_decorator_count(n), 0u);
  node_t *decl = create_enum_decl_node(a);
  stmt_enum_node_set_enum_decl(n, decl);
  EXPECT_EQ(stmt_enum_node_get_enum_decl(n), decl);
  node_t *d0 = create_decorator_node(a);
  node_t *d1 = create_decorator_node(a);
  stmt_enum_node_add_decorator(n, d0);
  stmt_enum_node_add_decorator(n, d1);
  EXPECT_EQ(stmt_enum_node_get_decorator_count(n), 2u);
  EXPECT_EQ(stmt_enum_node_get_decorator(n, 0), d0);
  EXPECT_EQ(stmt_enum_node_get_decorator(n, 1), d1);
  EXPECT_EQ(stmt_enum_node_get_decorator(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtUnion) {
  node_t *n = create_stmt_union_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_UNION);
  EXPECT_EQ(stmt_union_node_get_decorator_count(n), 0u);
  node_t *decl = create_union_node(a);
  stmt_union_node_set_union_decl(n, decl);
  EXPECT_EQ(stmt_union_node_get_union_decl(n), decl);
  node_t *d0 = create_decorator_node(a);
  stmt_union_node_add_decorator(n, d0);
  EXPECT_EQ(stmt_union_node_get_decorator_count(n), 1u);
  EXPECT_EQ(stmt_union_node_get_decorator(n, 0), d0);
  EXPECT_EQ(stmt_union_node_get_decorator(n, 1), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StmtStruct) {
  node_t *n = create_stmt_struct_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STMT_STRUCT);
  EXPECT_EQ(stmt_struct_node_get_decorator_count(n), 0u);
  node_t *decl = create_struct_decl_node(a);
  stmt_struct_node_set_struct_decl(n, decl);
  EXPECT_EQ(stmt_struct_node_get_struct_decl(n), decl);
  node_t *d0 = create_decorator_node(a);
  node_t *d1 = create_decorator_node(a);
  stmt_struct_node_add_decorator(n, d0);
  stmt_struct_node_add_decorator(n, d1);
  EXPECT_EQ(stmt_struct_node_get_decorator_count(n), 2u);
  EXPECT_EQ(stmt_struct_node_get_decorator(n, 0), d0);
  EXPECT_EQ(stmt_struct_node_get_decorator(n, 1), d1);
  EXPECT_EQ(stmt_struct_node_get_decorator(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StructDecl) {
  node_t *n = create_struct_decl_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_EXPR_STRUCT_DECL);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_EXPR_STRUCT_DECL_CHILDREN_SIZE);
  node_t *keyword = make_leaf(a);
  node_t *name = make_leaf(a);
  node_t *generics = make_leaf(a);
  node_t *implements = make_leaf(a);
  node_t *body = make_leaf(a);
  struct_decl_node_set_keyword(n, keyword);
  struct_decl_node_set_name(n, name);
  struct_decl_node_set_generics(n, generics);
  struct_decl_node_set_implements(n, implements);
  struct_decl_node_set_body(n, body);
  EXPECT_EQ(struct_decl_node_get_keyword(n), keyword);
  EXPECT_EQ(struct_decl_node_get_name(n), name);
  EXPECT_EQ(struct_decl_node_get_generics(n), generics);
  EXPECT_EQ(struct_decl_node_get_implements(n), implements);
  EXPECT_EQ(struct_decl_node_get_body(n), body);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StructDeclOptionalSlots) {
  node_t *n = create_struct_decl_node(a);
  struct_decl_node_set_generics(n, nullptr);
  struct_decl_node_set_implements(n, nullptr);
  EXPECT_EQ(struct_decl_node_get_generics(n), nullptr);
  EXPECT_EQ(struct_decl_node_get_implements(n), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StructMembers) {
  node_t *n = create_struct_members_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STRUCT_MEMBERS);
  EXPECT_EQ(struct_members_node_get_member_count(n), 0u);
  node_t *first = make_leaf(a);
  node_t *second = make_leaf(a);
  struct_members_node_add_member(n, first);
  struct_members_node_add_member(n, second);
  EXPECT_EQ(struct_members_node_get_member_count(n), 2u);
  EXPECT_EQ(struct_members_node_get_member(n, 0), first);
  EXPECT_EQ(struct_members_node_get_member(n, 1), second);
  EXPECT_EQ(struct_members_node_get_member(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, StructEmbed) {
  node_t *n = create_struct_embed_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_STRUCT_EMBED);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_STRUCT_EMBED_CHILDREN_SIZE);
  node_t *type = make_leaf(a);
  struct_embed_node_set_type(n, type);
  EXPECT_EQ(struct_embed_node_get_type(n), type);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, Implements) {
  node_t *n = create_implements_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_IMPLEMENTS);
  EXPECT_EQ(implements_node_get_type_count(n), 0u);
  node_t *first = make_leaf(a);
  node_t *second = make_leaf(a);
  implements_node_add_type(n, first);
  implements_node_add_type(n, second);
  EXPECT_EQ(implements_node_get_type_count(n), 2u);
  EXPECT_EQ(implements_node_get_type(n, 0), first);
  EXPECT_EQ(implements_node_get_type(n, 1), second);
  EXPECT_EQ(implements_node_get_type(n, 2), nullptr);
  delete_node(&n);
}

TEST_F(NodeSemanticTest, FieldDecorators) {
  node_t *n = create_field_node(a);
  EXPECT_EQ(field_node_get_decorator_count(n), 0u);
  node_t *d0 = create_decorator_node(a);
  field_node_add_decorator(n, d0);
  EXPECT_EQ(field_node_get_decorator_count(n), 1u);
  EXPECT_EQ(field_node_get_decorator(n, 0), d0);
  EXPECT_EQ(field_node_get_decorator(n, 1), nullptr);
  delete_node(&n);
}

/* ---- decorator ---- */

TEST_F(NodeSemanticTest, Decorator) {
  node_t *n = create_decorator_node(a);
  EXPECT_EQ(node_get_kind(n), CLUX_NODE_DECORATOR);
  EXPECT_EQ(vec_get_size(node_get_children(n)),
            (size_t)CLUX_DECORATOR_CHILDREN_SIZE);
  node_t *expr = make_leaf(a);
  decorator_node_set_expr(n, expr);
  EXPECT_EQ(decorator_node_get_expr(n), expr);
  decorator_node_set_expr(n, nullptr);
  EXPECT_EQ(decorator_node_get_expr(n), nullptr);
  delete_node(&expr);
  delete_node(&n);
}
