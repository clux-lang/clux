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

/* ---- 三元条件表达式 cond ? a : b ---- */

TEST(Driver, RunFileTernaryRuntime) {
  /* 运行时三元：cond 为真取 then、为假取 else；两分支惰性（未选中分支
     不求值）；结果可用于运算/返回。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var a:i32 = 3;\n"
      "  var b:i32 = 7;\n"
      "  var r:i32 = (a > b) ? a : b;\n"
      "  if ((a < b) ? true : false) { r = r + 1; }\n"
      "  return r;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryNestedAndMixed) {
  /* 嵌套三元右结合 + 与二元运算混合：a ? b : c ? d : e → a ? b : (c ? d : e)。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i32 = 0;\n"
      "  var y:i32 = 1;\n"
      "  var z:i32 = 2;\n"
      "  var w:i32 = 3;\n"
      "  var r:i32 = (x == 0) ? x : (y == 1) ? y : (z == 2) ? z : w;\n"
      "  if (r != 0) { return 9; }\n"
      "  var s:i32 = (x != 0) ? x : (y == 1) ? y : (z == 2) ? z : w;\n"
      "  return s;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryLazyBranches) {
  /* 惰性求值验证：未选中分支不执行（若执行会因副作用/非法访问而失败）。
     三元右侧是数组越界读（运行期会报错）——cond 为真时 else 分支不得求值。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var arr:[2]i32 = .[2]i32{10, 20};\n"
      "  var i:i32 = 0;\n"
      "  var r:i32 = (i < 2) ? arr[i] : arr[99];\n"
      "  if (r != 10) { return 7; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryCompileTimeFold) {
  /* ctfe 折叠验证：comptime var 的初始化含三元 → 编译期求值折叠为常量。
     运行时仍按折叠后的常量执行。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  comptime var a = (1 < 2) ? 5 : 9;\n"
      "  comptime var b = (3 > 4) ? 11 : 13;\n"
      "  return a + b;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryBranchTypeMismatchRejected) {
  /* 两分支类型不一致：sema 诊断（三元结果类型须一致），编译失败退出 1。 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var x:i32 = 1;\n"
                                     "  var r = (x > 0) ? 1 : true;\n"
                                     "  return 0;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTernaryCondNotBoolRejected) {
  /* 条件非 bool：sema 诊断（ternary condition operand must be bool），退出 1。 */
  std::string path = write_temp_file("func main():i32 {\n"
                                     "  var x:i32 = 1;\n"
                                     "  var r = x ? 1 : 2;\n"
                                     "  return r;\n"
                                     "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefGlobal) {
  /* 全局 type def：内建 rhs（PUSH 内建名）→ var 显式类型标注与运算 */
  std::string path = write_temp_file(
      "type MyInt = i64;\n"
      "func main():i32 {\n"
      "  var x:MyInt = 42;\n"
      "  var y = x + 1;\n"
      "  return y as i32;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefCompositeAndAlias) {
  /* 复合类型 rhs 折叠 LOAD_TYPE + 别名链 + 函数签名引用 */
  std::string path = write_temp_file(
      "type Pair = [2]i32;\n"
      "type Alias = Pair;\n"
      "func first(p:Pair):i32 { return p[0]; }\n"
      "func main():i32 {\n"
      "  var p:Alias = .[2]i32{10, 20};\n"
      "  return first(p);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocal) {
  /* 局部 type def：块作用域遮蔽 + var 显式类型 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  type Local = i32;\n"
      "  var x:Local = 7;\n"
      "  {\n"
      "    type Inner = i64;\n"
      "    var y:Inner = 9;\n"
      "    x = x + (y as i32);\n"
      "  }\n"
      "  return x;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocalHoisted) {
  /* 局部 type def 提升：使用在定义之前（前向引用）→ 运行期块入口先 DEFINE
     类型名，var 槽位解析成功，结果正确 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:Local = 7;\n"
      "  type Local = i32;\n"
      "  {\n"
      "    var y:Inner = 9;\n"
      "    type Inner = i64;\n"
      "    x = x + (y as i32);\n"
      "  }\n"
      "  return x;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileTypeDefLocalHoistedLoop) {
  /* 局部 type def 提升进循环体：每次迭代 PUSH_SCOPE 后入口 DEFINE（幂等），
     var 使用定义之前的类型名 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var sum:i32 = 0;\n"
      "  var i:i32 = 0;\n"
      "  while (i < 3) {\n"
      "    var v:LoopT = i + 1;\n"
      "    type LoopT = i32;\n"
      "    sum = sum + v;\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return sum;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileDoWhileBasic) {
  /* do-while 基础：体先执行一次再判条件；0+1+2+3+4 = 10 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var sum:i32 = 0;\n"
      "  var i:i32 = 0;\n"
      "  do {\n"
      "    sum = sum + i;\n"
      "    i = i + 1;\n"
      "  } while (i < 5);\n"
      "  if (sum != 10) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileDoWhileFalseCondRunsOnce) {
  /* 条件初始即 false：体仍执行一次（后置条件循环语义） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var n:i32 = 0;\n"
      "  do { n = n + 1; } while (false);\n"
      "  if (n != 1) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileDoWhileContinue) {
  /* do-while continue：跳回条件判断（跳过本次迭代剩余语句） */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var sum:i32 = 0;\n"
      "  var i:i32 = 0;\n"
      "  do {\n"
      "    i = i + 1;\n"
      "    if (i == 3) { continue; }\n"
      "    sum = sum + i;\n"
      "  } while (i < 5);\n"
      "  if (sum != 12) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileDoWhileBreak) {
  /* do-while break：提前退出循环 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var acc:i32 = 0;\n"
      "  var j:i32 = 0;\n"
      "  do {\n"
      "    j = j + 1;\n"
      "    if (j == 4) { break; }\n"
      "    acc = acc + j;\n"
      "  } while (j < 10);\n"
      "  if (acc != 6) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileDoWhileNested) {
  /* 嵌套 do-while：内层 3 次 × 外层 2 次 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var inner:i32 = 0;\n"
      "  var outer:i32 = 0;\n"
      "  var k:i32 = 0;\n"
      "  do {\n"
      "    outer = outer + 1;\n"
      "    var m:i32 = 0;\n"
      "    do { inner = inner + 1; m = m + 1; } while (m < 3);\n"
      "    k = k + 1;\n"
      "  } while (k < 2);\n"
      "  if (inner != 6) { return 1; }\n"
      "  if (outer != 2) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileDoWhileInComptimeFunc) {
  /* comptime func 内 do-while：CTFE 语句解释路径（do-while 折叠） */
  std::string path = write_temp_file(
      "comptime func count(n:i32):i32 {\n"
      "  var c:i32 = 0;\n"
      "  do { c = c + 1; } while (c < n);\n"
      "  return c;\n"
      "}\n"
      "func main():i32 {\n"
      "  var r:i32 = count(4);\n"
      "  if (r != 4) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
