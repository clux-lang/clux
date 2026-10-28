#include "test_common.h"

#include <cstdio>
#include <string>

/* ================================================================
 * 反射注入：union / cunion / tuple / option / ptr / const-volatile
 * （`T.<key>`，sema 折叠为常量；设计见 docs/reflect-builtin-design.md）
 * ================================================================ */

/* union（tag union）：member 名 / payload 类型 / tag 编号 */
TEST(Reflect, UnionMembers) {
  clux_expect_ok(
      "union Value { i: i32; f: f64; }\n"
      "func main(): i32 {\n"
      "  if (Value.kind != \"union\") { return 1; }\n"
      "  if (Value.member_count != 2usize) { return 2; }\n"
      "  if (Value.member_name(0) != \"i\") { return 3; }\n"
      "  if (Value.member_name(1) != \"f\") { return 4; }\n"
      "  if (Value.member_type(0).name != \"i32\") { return 5; }\n"
      "  if (Value.member_type(1).name != \"f64\") { return 6; }\n"
      "  if (Value.member_tag(0) != 0usize) { return 7; }\n"
      "  if (Value.member_tag(1) != 1usize) { return 8; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_union");
}

/* cunion：无 tag */
TEST(Reflect, CunionMembers) {
  clux_expect_ok(
      "cunion Raw { a: i32; b: f32; }\n"
      "func main(): i32 {\n"
      "  if (Raw.kind != \"cunion\") { return 1; }\n"
      "  if (Raw.member_count != 2usize) { return 2; }\n"
      "  if (Raw.member_name(1) != \"b\") { return 3; }\n"
      "  if (Raw.member_type(1).name != \"f32\") { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_cunion");
}

/* tuple：元素个数 / 类型 / 偏移 */
TEST(Reflect, TupleElems) {
  clux_expect_ok(
      "struct H { t: <i32, f64, i64>; }\n"
      "func main(): i32 {\n"
      "  if (H.field_type(0).kind != \"tuple\") { return 1; }\n"
      "  if (H.field_type(0).elem_count != 3usize) { return 2; }\n"
      "  if (H.field_type(0).elem_type(2).name != \"i64\") { return 3; }\n"
      "  if (H.field_type(0).elem_offset(0) != 0usize) { return 4; }\n"
      "  if (H.field_type(0).elem_offset(1) != 8usize) { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_tuple");
}

/* option：inner_type + kind */
TEST(Reflect, OptionInner) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var o: ?i32 = .?i32{ 7 };\n"
      "  if (typeof(o).kind != \"option\") { return 1; }\n"
      "  if (typeof(o).inner_type.name != \"i32\") { return 2; }\n"
      "  if (typeof(o).size < 4usize) { return 3; }\n"
      "  var p: ?f64 = .?f64{ 1.5 };\n"
      "  if (typeof(p).inner_type.name != \"f64\") { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_option");
}

/* ptr：target_type / rc（plain / share / weak） */
TEST(Reflect, PtrTargetAndRc) {
  clux_expect_ok(
      "struct P { x: i32; }\n"
      "func main(): i32 {\n"
      "  var v: P = .P{ .x = 1 };\n"
      "  var r: *P = v.&;\n"
      "  if (typeof(r).kind != \"ptr\") { return 1; }\n"
      "  if (typeof(r).target_type.name != \"P\") { return 2; }\n"
      "  if (typeof(r).rc != \"plain\") { return 3; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_ptr");
}

/* const / volatile：kind 如实返回，属性自动穿透，unqualified 剥限定 */
TEST(Reflect, ConstVolatile) {
  clux_expect_ok(
      "struct P { x: i32; y: i64; }\n"
      "type CP = const P;\n"
      "func main(): i32 {\n"
      "  if (CP.kind != \"const\") { return 1; }\n"
      "  if (CP.field_count != 2usize) { return 2; }\n" /* 自动穿透 */
      "  if (CP.unqualified.name != \"P\") { return 3; }\n"
      "  if (CP.unqualified.kind != \"struct\") { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_qual");
}
