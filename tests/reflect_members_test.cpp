#include "test_common.h"

#include <cstdio>
#include <string>

/* ================================================================
 * 反射：结构体成员（方法 / 静态属性）—— 路线 A（sema 成员作用域）
 * `T.method_count` / `T.method_name(i)` / `T.method_type("run")` /
 * `T.attribute_count` / `T.attribute_name(i)` / `T.attribute_type(i)`
 * ================================================================ */

/* 方法与签名（按名）：参数类型 / 返回类型 */
TEST(ReflectMembers, Methods) {
  clux_expect_ok(
      "struct Runner {\n"
      "  var speed: i32 = 0;\n"
      "  var tag: i64 = 0;\n"
      "  func run(self: *Runner, a: i32): i64 { return 1i64; }\n"
      "  func stop(self: *Runner): void { }\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (Runner.method_count != 2usize) { return 1; }\n"
      "  if (Runner.method_type(\"run\").kind != \"func\") { return 2; }\n"
      "  if (Runner.method_type(\"run\").param_count != 2usize) { return 3; }\n"
      "  if (Runner.method_type(\"run\").param_type(0).name != \"*Runner\") { return 4; }\n"
      "  if (Runner.method_type(\"run\").param_type(1).name != \"i32\") { return 5; }\n"
      "  if (Runner.method_type(\"run\").return_type.name != \"i64\") { return 6; }\n"
      "  if (Runner.method_type(\"stop\").return_type.name != \"void\") { return 7; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_methods");
}

/* 方法名按索引枚举（按名排序，确定性） */
TEST(ReflectMembers, MethodNamesSorted) {
  clux_expect_ok(
      "struct R {\n"
      "  func zeta(self: *R): void { }\n"
      "  func alpha(self: *R): void { }\n"
      "  func mid(self: *R): void { }\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (R.method_count != 3usize) { return 1; }\n"
      "  if (R.method_name(0) != \"alpha\") { return 2; }\n"
      "  if (R.method_name(1) != \"mid\") { return 3; }\n"
      "  if (R.method_name(2) != \"zeta\") { return 4; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_method_names");
}

/* 静态属性：计数 / 名字 / 类型 */
TEST(ReflectMembers, Attributes) {
  clux_expect_ok(
      "struct Cfg {\n"
      "  var zeta: i64 = 0;\n"
      "  var alpha: i32 = 0;\n"
      "  func f(self: *Cfg): void { }\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (Cfg.attribute_count != 2usize) { return 1; }\n"
      "  if (Cfg.attribute_name(0) != \"alpha\") { return 2; }\n"
      "  if (Cfg.attribute_name(1) != \"zeta\") { return 3; }\n"
      "  if (Cfg.attribute_type(0).name != \"i32\") { return 4; }\n"
      "  if (Cfg.attribute_type(1).name != \"i64\") { return 5; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_attributes");
}

/* 嵌入继承：方法按名可查（继承来的方法也能取签名） */
TEST(ReflectMembers, InheritedMethod) {
  clux_expect_ok(
      "struct Base {\n"
      "  func base_run(self: *Base, a: i32): i32 { return a; }\n"
      "}\n"
      "struct Derived { ...Base; var extra: i32 = 0; }\n"
      "func main(): i32 {\n"
      "  if (Derived.method_type(\"base_run\").param_count != 2usize) { return 1; }\n"
      "  if (Derived.method_type(\"base_run\").return_type.name != \"i32\") { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "reflect_inherited");
}

/* 未知方法名 */
TEST(ReflectMembers, UnknownMethodRejected) {
  std::string path = write_temp_file(
      "struct R { func f(self: *R): void { } }\n"
      "func main(): i32 { var t = R.method_type(\"nope\"); return 0; }\n",
      "reflect_unknown_method");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("no member named 'nope'"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 属性不是方法 */
TEST(ReflectMembers, AttributeIsNotMethod) {
  std::string path = write_temp_file(
      "struct R { var x: i32 = 0; }\n"
      "func main(): i32 { var t = R.method_type(\"x\"); return 0; }\n",
      "reflect_attr_not_method");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("is an attribute, not a method"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 索引越界 / 没有成员 */
TEST(ReflectMembers, MemberIndexErrors) {
  std::string p1 = write_temp_file(
      "struct R { func f(self: *R): void { } }\n"
      "func main(): i32 { var s = R.method_name(3); return 0; }\n",
      "reflect_member_oor");
  std::string err1;
  EXPECT_EQ(clux_run_capture(p1, &err1), 1);
  EXPECT_NE(err1.find("out of range"), std::string::npos) << err1;
  std::remove(p1.c_str());

  std::string p2 = write_temp_file(
      "func main(): i32 { var n = i32.method_count; return 0; }\n",
      "reflect_member_none");
  std::string err2;
  EXPECT_EQ(clux_run_capture(p2, &err2), 1);
  EXPECT_NE(err2.find("has no reflection member"), std::string::npos) << err2;
  std::remove(p2.c_str());
}
