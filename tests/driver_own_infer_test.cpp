// driver_own_infer_test.cpp —— 推断即可：`var a = <含 own 的构造/返回值>`
//
// 本文件锁的是一条**跨提交的交互**，两个方向都必须同时成立：
//
//   A) "fatal 自动推断"（17839d9 / ca00b91）：`var` 推断时把 fatal 剥离成可用类型
//      —— fatal 复合类型 → 基础类型 T、fatal *T → own *T、fatal []T → own []T。
//      目的是 `var a = .Test{ .data = new i32{123} };` 这种写法**不需要**显式标注
//      `a` 是 `own Test` 还是 `Test`。
//
//   B) "构造产 fatal T"（所有权扩散）：构造表达式 `.T{...}` 的产物类型是 fatal T，
//      编译器在构造点发 `LOAD_TYPE <fatal 变体 id>`，运行期值带 fatal 类型。
//
// 两者合流的正确形态（逐字节看过 bytecode 确认）：
//      LOAD_TYPE <fatal 变体>            ← 构造点（B）
//      ... CONSTRUCT N                   ← 运行期产出 fatal 值
//      LOAD_TYPE <基础类型 T>            ← DEFINE 类型槽位（A 推断写回的显式类型）
//      DEFINE "a"                        ← op_define 走 value_implicit_cast
//                                            fatal T → T 的**接管转移**
//
// 若只有 A 没有 B：构造产普通 T，接管无从发生（但也不崩）。
// 若只有 B 没有 A：推断写回的槽位缺失，运行期值带 fatal 类型而变量声明为 T，
// 转移判定落空 → 变量析构不释放堆块（泄漏）或与临时值双释放。
// 所以本文件的用例是这两条提交的**联合回归**。

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

extern "C" {
#include "driver/driver.h"
}

#include "test_common.h"

namespace {

std::string write_temp_file(const std::string &content) {
  static int counter = 0;
  std::string path = "clux_own_infer_" + std::to_string(counter++) + ".cx";
  FILE *fp = fopen(path.c_str(), "wb");
  if (!fp) {
    ADD_FAILURE() << "cannot open temp source file: " << path;
    return path;
  }
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path;
}

} // namespace

TEST(DriverOwnInfer, ConstructInferenceWithoutAnnotation) {
  /* 用户给的确切形态：不写 a 的类型，也无需 own Test / Test 标注 */
  std::string path = write_temp_file(
      "struct Test { data: own *i32; }\n"
      "func main(): i32 {\n"
      "  var a = .Test{ .data = new i32{123} };\n"
      "  if (a.data.* != 123) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnInfer, FatalReturnInferenceWithoutAnnotation) {
  /* `func mk(): fatal Test` 的返回值 → 推断为 Test（无需显式标注） */
  std::string path = write_temp_file(
      "struct Test { data: own *i32; n: i32 }\n"
      "func mk(n: i32): fatal Test { return move(.Test{ .data = new i32{n}, .n = n }); }\n"
      "func main(): i32 {\n"
      "  var a = mk(7);\n"
      "  if (a.data.* != 7 || a.n != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnInfer, FatalPointerInfersOwnPointer) {
  /* `var p = new i32{...}` → 推断为 own *i32（17839d9 的行为） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var p = new i32{9};\n"
      "  if (p.* != 9) { return 1; }\n"
      "  p.* = 10;\n"
      "  if (p.* != 10) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnInfer, NestedAndAggregateInference) {
  /* 嵌套 struct / 含 own 的元组 / 含 own 元素的数组：推断路径都要覆盖 */
  std::string path = write_temp_file(
      "struct Inner { p: own *i32; }\n"
      "struct Outer { inner: Inner; n: i32; }\n"
      "func main(): i32 {\n"
      "  var o = .Outer{ .inner = .Inner{ .p = new i32{5} }, .n = 2 };\n"
      "  if (o.inner.p.* != 5) { return 1; }\n"
      "  var t = .<own *i32, i32>{ new i32{8}, 9 };\n"
      "  if (t[0].* != 8 || t[1] != 9) { return 2; }\n"
      "  var arr = .[2]Inner{ .Inner{ .p = new i32{10} }, .Inner{ .p = new i32{11} } };\n"
      "  if (arr[0].p.* != 10 || arr[1].p.* != 11) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnInfer, ScopedInferredVariableDestroysCorrectly) {
  /* 内层作用域里的推断变量：作用域退出时堆块必须被释放（不能因类型不一致而漏）。
     真泄漏时 allocator 的 leak 报告会打出来（本用例断言的是不崩 + 值正确）。 */
  std::string path = write_temp_file(
      "struct Test { data: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var k: i32 = 0;\n"
      "  {\n"
      "    var scoped = .Test{ .data = new i32{99}, .n = 3 };\n"
      "    if (scoped.data.* != 99) { return 1; }\n"
      "    k = scoped.n;\n"
      "  }\n"
      "  if (k != 3) { return 2; }\n"
      "  var many = .Test{ .data = new i32{1}, .n = 1 };\n"
      "  if (many.n != 1) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverOwnInfer, ExplicitAnnotationAlsoWorks) {
  /* 显式标注（own 语义的基础类型）与推断两条路都要通——避免"推断能过、
     显式写反而报错"这类分叉。 */
  std::string path = write_temp_file(
      "struct Test { data: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var a: Test = .Test{ .data = new i32{123}, .n = 1 };\n"
      "  if (a.data.* != 123) { return 1; }\n"
      "  var b: own *i32 = new i32{7};\n"
      "  if (b.* != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
