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

/* ---- sizeof / alignof / typeof（m2-design §8，完全编译期）---- */

TEST(Driver, RunFileSizeofAlignofTypeofPasses) {
  /* 端到端：三个运算符全部编译期折叠（反汇编零指令残留由 bcode 层测试
     覆盖），运行期只消费折叠后的 u64 常量 / 类型 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a: i32 = 1;\n"
      "  var b: [sizeof(a)]i32 = .[4]i32{1,2,3,4};\n"
      "  var c: [sizeof(i32)]u32 = .[4]u32{1u32,2u32,3u32,4u32};\n"
      "  var d: [alignof(u64)]i32 = .[8]i32{1,2,3,4,5,6,7,8};\n"
      "  if (sizeof(u8) != 1u64) { return 1; }\n"
      "  if (sizeof(u32) != 4u64) { return 2; }\n"
      "  if (sizeof(i64) != 8u64) { return 3; }\n"
      "  if (alignof(u64) != 8u64) { return 4; }\n"
      "  if (sizeof(a) != 4u64) { return 5; }\n"
      "  if (sizeof(b) != 16u64) { return 6; }\n"
      "  if (sizeof(c) != 16u64) { return 7; }\n"
      "  if (sizeof(d) != 32u64) { return 8; }\n"
      "  var t: typeof(a) = 42;\n"
      "  if (t != 42) { return 9; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSizeofNonConstRejected) {
  /* sizeof 操作数必须是编译期可求值的类型/表达式；值表达式走 shadow
     求值后取类型——合法。此处验证运行期无 SIZEOF 指令残留路径可跑 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var s: u64 = sizeof(i32);\n"
      "  if (s != 4u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSignedUnsignedImplicitRejected) {
  /* 设计：signed→unsigned 隐式转换禁止（sema 编译期报错，非运行时） */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var u: u64 = 1;\n"
      "  return 0;\n"
      "}\n");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileSignedUnsignedExplicitSuffixPasses) {
  /* 显式后缀字面量放行 */
  std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var u: u64 = 1u64;\n"
      "  if (u != 1u64) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
