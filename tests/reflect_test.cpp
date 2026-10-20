#include "test_common.h"

#include <cstdio>
#include <string>

/* ================================================================
 * 反射注入：`T.<key>` / `T.<key>(i)`（sema 折叠为常量）
 * 设计见 docs/reflect-builtin-design.md
 * ================================================================ */

/* 通用 key + 别名透明 */
TEST(Reflect, UniversalKeys) {
  clux_expect_ok(
      "type I = i32;\n"
      "var R: i32 = 0;\n"
      "func main(): i32 {\n"
      "  if (i32.name != \"i32\") { return 1; }\n"
      "  if (i32.size != 4usize) { return 2; }\n"
      "  if (i32.align != 4usize) { return 3; }\n"
      "  if (i32.kind != \"int\") { return 4; }\n"
      "  if (I.name != \"i32\") { return 5; }\n" /* 别名透明 */
      "  if (I.kind != \"int\") { return 6; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_universal");
}

/* struct：字段计数 / 名字 / 类型 / 偏移 */
TEST(Reflect, StructFields) {
  clux_expect_ok(
      "struct Point { x: i32; y: i64; }\n"
      "func main(): i32 {\n"
      "  if (Point.kind != \"struct\") { return 1; }\n"
      "  if (Point.field_count != 2usize) { return 2; }\n"
      "  if (Point.field_name(0) != \"x\") { return 3; }\n"
      "  if (Point.field_name(1) != \"y\") { return 4; }\n"
      "  if (Point.field_type(0).name != \"i32\") { return 5; }\n"
      "  if (Point.field_type(1).name != \"i64\") { return 6; }\n"
      "  if (Point.field_offset(0) != 0usize) { return 7; }\n"
      "  if (Point.field_offset(1) != 8usize) { return 8; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_struct");
}

/* enum：变体数 / 名字 / 值 / 底层类型；数组/切片/元组 */
TEST(Reflect, EnumArrayTuple) {
  clux_expect_ok(
      "enum Color: i32 { Red = 1, Green = 2 }\n"
      "struct H { t: <i32, f64>; }\n"
      "type A3 = [3]i32;\n"
      "func main(): i32 {\n"
      "  if (Color.kind != \"enum\") { return 1; }\n"
      "  if (Color.variant_count != 2usize) { return 2; }\n"
      "  if (Color.variant_name(0) != \"Red\") { return 3; }\n"
      "  if (Color.variant_value(1) != 2i64) { return 4; }\n"
      "  if (Color.underlying.name != \"i32\") { return 5; }\n"
      "  if (A3.kind != \"array\") { return 6; }\n"
      "  if (A3.length != 3usize) { return 7; }\n"
      "  if (A3.elem_type.name != \"i32\") { return 8; }\n"
      "  if (H.field_type(0).elem_type(1).name != \"f64\") { return 9; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_enum_array");
}

/* func / cfunc：参数与返回类型、变参、C ABI 标记 */
TEST(Reflect, FuncSignature) {
  clux_expect_ok(
      "type F = func(i32, ref *i32) -> i64;\n"
      "type C = cfunc(i32) -> i32;\n"
      "func main(): i32 {\n"
      "  if (F.param_count != 2usize) { return 1; }\n"
      "  if (F.param_type(0).name != \"i32\") { return 2; }\n"
      "  if (F.param_type(1).name != \"*i32\") { return 3; }\n"
      "  if (F.return_type.name != \"i64\") { return 4; }\n"
      "  if (F.is_variadic) { return 5; }\n"
      "  if (F.is_c_abi) { return 6; }\n"
      "  if (!C.is_c_abi) { return 7; }\n"
      "  if (C.kind != \"cfunc\") { return 8; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_func");
}

/* 链式：field_type(i).<key>、typeof 入口、`type` 绑定 */
TEST(Reflect, ChainsAndBindings) {
  clux_expect_ok(
      "struct H { t: <i32, f64>; }\n"
      "type F0 = H.field_type(0);\n"
      "func main(): i32 {\n"
      "  if (F0.elem_count != 2usize) { return 1; }\n"
      "  if (H.field_type(0).elem_type(0).name != \"i32\") { return 2; }\n"
      "  var v: i64 = 3;\n"
      "  if (typeof(v).name != \"i64\") { return 3; }\n"
      "  if (sizeof(typeof(v)) != 8usize) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_chain");
}

/* 未知 key：报错并列出可用 key */
TEST(Reflect, UnknownKeyRejected) {
  std::string path = write_temp_file(
      "struct Point { x: i32; }\n"
      "func main(): i32 {\n"
      "  if (Point.fild_count != 1usize) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_unknown");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("has no reflection member 'fild_count'"), std::string::npos)
      << err;
  EXPECT_NE(err.find("field_count"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* key 不适用于该 kind */
TEST(Reflect, WrongKindRejected) {
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  if (i32.field_count != 1usize) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_wrongkind");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("int 'i32' has no reflection member 'field_count'"),
            std::string::npos)
      << err;
  std::remove(path.c_str());
}

/* 索引越界 */
TEST(Reflect, IndexOutOfRange) {
  std::string path = write_temp_file(
      "struct Point { x: i32; }\n"
      "func main(): i32 {\n"
      "  var s = Point.field_name(3);\n"
      "  return 0;\n"
      "}\n",
      "reflect_oor");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("field index 3 out of range"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 缺索引 / 索引非常量 */
TEST(Reflect, IndexErrors) {
  std::string p1 = write_temp_file(
      "struct Point { x: i32; }\n"
      "func main(): i32 { var s = Point.field_name; return 0; }\n",
      "reflect_noindex");
  std::string err1;
  EXPECT_EQ(clux_run_capture(p1, &err1), 1);
  EXPECT_NE(err1.find("requires an index argument"), std::string::npos) << err1;
  std::remove(p1.c_str());

  std::string p2 = write_temp_file(
      "struct Point { x: i32; y: i32; }\n"
      "func main(): i32 {\n"
      "  var i: usize = 1;\n"
      "  var s = Point.field_name(i);\n"
      "  return 0;\n"
      "}\n",
      "reflect_ncindex");
  std::string err2;
  EXPECT_EQ(clux_run_capture(p2, &err2), 1);
  EXPECT_NE(err2.find("index argument must be a compile-time constant"),
            std::string::npos)
      << err2;
  std::remove(p2.c_str());
}

/* 所有权 key 已移除（用户修正：所有权是类型的一部分，无需反射） */
TEST(Reflect, OwnershipKeysRemoved) {
  std::string path = write_temp_file(
      "struct N { next: own *N; }\n"
      "func main(): i32 { var s = N.field_ownership(0); return 0; }\n",
      "reflect_own_key");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("has no reflection member 'field_ownership'"),
            std::string::npos)
      << err;
  std::remove(path.c_str());
}
