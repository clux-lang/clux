#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include "core/allocator.h"
#include "core/vec.h"
#include "driver/driver.h"
#include "parser/lexer.h"
}

#include "test_common.h"

namespace {

std::string write_temp_file(const std::string &content) {
  auto path = std::filesystem::temp_directory_path() / "clux_test_XXXXXX";
  auto path_str = path.string();
  /* mkstemps is not available on Windows; use a simple unique name. */
  static int counter = 0;
  path_str += std::to_string(counter++);
  FILE *fp = fopen(path_str.c_str(), "wb");
  fwrite(content.data(), 1, content.size(), fp);
  fclose(fp);
  return path_str;
}

} // namespace

/* ---- Stage ①+②+③: run entry point ---- */

TEST(Driver, RunFileValidReturnsZero) {
  /* 合法程序：词法 + 语法 + 语义全通过。main 无返回类型（void），
     函数体不含 return 值，sema 无诊断。 */
  std::string path = write_temp_file("func main(): void { var x = 1; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileNestedArrayConstructPasses) {
  /* 多维数组构造：内层 construct 的 value_make_array 不得残留 type value
     到操作数栈（回归：曾因此外层 construct 栈布局错位报 missing type slot）。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 {\n"
      "    .[2]i32 { 1, 2 },\n"
      "    .[2]i32 { 3, 4 }\n"
      "  };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultipleArrayConstructsPasses) {
  /* 同一作用域内多次数组构造（三个独立同类型数组）不应残留 type value */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var b = .[2]i32 { 3, 4 };\n"
      "  var c = .[2]i32 { 5, 6 };\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileArrayConstructAssignPasses) {
  /* 数组构造 + 数组变量间赋值（不涉及索引） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var b = a;\n"
      "  var c = .[2]i32 { 3, 4 };\n"
      "  var d = c;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexGetPasses) {
  /* 下标右值读取（INDEX_GET）：一维 + 多维 + 变量下标 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { 10, 20, 30 };\n"
      "  var x = a[0] + a[2];\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  var y = m[1][0];\n"
      "  var i = 1;\n"
      "  var z = a[i];\n"
      "  return x + y + z;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexSetPasses) {
  /* 下标左值赋值（INDEX_SET）：直接赋值 + 复合赋值 + 变量下标 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { 10, 20, 30 };\n"
      "  a[1] = 99;\n"
      "  a[2] += 1;\n"
      "  a[0] *= 2;\n"
      "  var i = 1;\n"
      "  a[i] -= 2;\n"
      "  var s = a[0] + a[1] + a[2];\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileFillZeroPads) {
  /* fill 值包显式 0 填充（M2 construct 完全显式，无自动填充）：
     .[3]i32{ <0,2>, 10 } 总元素数 = 2 + 1 = 3 == 长度 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a = .[3]i32 { <0,2>, 10 };\n"
      "  return a[0] + a[1] + a[2];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileEmptyOptionalFuncArrayFillsNil) {
  /* 显式 nil 元素（M2 construct 完全显式，无自动填充）：.[N]?func()->T{ nil, ... }，
     元素经 ?func() 变量取出后 == nil 判定（nil 判定只支持标识符） */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var fns = .[3]?func()->i32{ nil, nil, nil };\n"
      "  var e0:?func()->i32 = fns[0];\n"
      "  var e1:?func()->i32 = fns[1];\n"
      "  var e2:?func()->i32 = fns[2];\n"
      "  if (e0 == nil && e1 == nil && e2 == nil) {\n"
      "    printf(\"all nil\\n\");\n"
      "  } else {\n"
      "    printf(\"not nil\\n\");\n"
      "  }\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFilePartialFillOptionalFuncArray) {
  /* optional 函数数组：显式 f + 显式 nil（M2 无自动 0 填充）。nil 元素判空 +
     SOME 分支 .! 解包后调用返回 7 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var fns = .[2]?func()->i32{ f, nil };\n"
      "  var e1:?func()->i32 = fns[1];\n"
      "  if (e1 != nil) { return 1; }\n"
      "  var e0:?func()->i32 = fns[0];\n"
      "  if (e0 != nil) { var f0 = e0.!; return f0(); }\n"
      "  return 2;\n"
      "}\n"
      "func f():i32 { return 7; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileConstructCountExceedsRejected) {
  /* 超出声明长度的字段数 → 编译期诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var a = .[2]i32 { 1, 2, 3 };\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultiIndexSubscriptRejected) {
  /* a[i,j] 多索引（泛型实参语法预留）落到数组下标 → 诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var x = a[0, 1];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexNonArrayRejected) {
  /* 对非数组类型下标 → 诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var x = 42;\n"
      "  var y = x[0];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileIndexStringIndexRejected) {
  /* 非整数下标 → 诊断 */
  std::string path = write_temp_file(
      "func main(): void {\n"
      "  var a = .[2]i32 { 1, 2 };\n"
      "  var x = a[\"k\"];\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultidimIndexSetPasses) {
  /* 多维下标左值赋值（借用引用写回直达原数组）：m[1][0] = 7 应生效 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  m[1][0] = 7;\n"
      "  m[0][1] += 10;\n"
      "  var i = 1;\n"
      "  var j = 1;\n"
      "  m[i][j] = 8;\n"
      "  var s = m[0][0] + m[0][1] + m[1][0] + m[1][1];\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMultidimBorrowIsIndependentOnBind) {
  /* 借用引用绑定变量时 materialize 深拷贝：var row = m[1] 是独立副本，
     修改 row 不影响 m（用户确认的借用生命周期语义） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var m = .[2][2]i32 { .[2]i32 { 1, 2 }, .[2]i32 { 3, 4 } };\n"
      "  var row = m[1];\n"
      "  row[0] = 100;\n"
      "  var s = m[1][0] + m[1][1];\n"
      "  return s;\n"   /* m[1] 仍是 3,4 → 7 */
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileMissingReturnsOne) {
  EXPECT_EQ(driver_run_file("no/such/file.cx"), 1);
}

TEST(Driver, RunFileLexErrorReturnsOne) {
  std::string path = write_temp_file("@ not a token\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}
