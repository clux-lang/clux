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


} // namespace

/* ---- const/volatile 限定类型 ---- */

TEST(Driver, ConstTdzFirstAssignAllowed) {
  /* const 变量 TDZ 首次赋值 = 初始化，豁免合法：
     var a:const i32 = undefined; a = 123; */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = undefined; a = 123; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstReassignAfterInitRejected) {
  /* const 变量已初始化后再赋值 → 语义错误 */
  std::string path = write_temp_file(
      "func main(): void { var a:const i32 = undefined; a = 123; a = 456; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstInitThenAssignRejected) {
  /* const 变量带初始值定义（flow_init=true）后再赋值 → 语义错误 */
  std::string path =
      write_temp_file("func main(): void { var a:const i32 = 1; a = 2; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstCompoundAssignRejected) {
  /* const 变量复合赋值（读+写）同样禁止 */
  std::string path =
      write_temp_file("func main(): void { var a:const i32 = 1; a += 1; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, ConstReadAndArithmeticAllowed) {
  /* const 变量只读 + 参与运算合法 */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = 5; var b:i32 = a * 2; return b; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, VolatileVarNormalOps) {
  /* volatile 变量读写/运算/赋值全合法（代理子类型） */
  std::string path = write_temp_file(
      "func main():i32 { var a:volatile i32 = 1; a = a + 1; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstVolatileCombined) {
  /* const volatile i32（固定组合顺序 volatile(const(i32))）：
     首次赋值豁免，之后禁止 */
  std::string path = write_temp_file(
      "func main():i32 { var a:const volatile i32 = undefined; a = 7; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstVolatileReassignRejected) {
  std::string path = write_temp_file(
      "func main(): void { var a:volatile const i32 = undefined; a = 1; a = 2; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(Driver, VolatileCompoundAssignAllowed) {
  /* volatile 不影响赋值（非 const），复合赋值合法 */
  std::string path = write_temp_file(
      "func main():i32 { var a:volatile i32 = 1; a += 2; return a; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, ConstValueCopyToNonConst) {
  /* const 值可复制到非 const 变量（const T extends T 复制语义） */
  std::string path = write_temp_file(
      "func main():i32 { var a:const i32 = 10; var b:i32 = a; return b; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarKeepsDeclaredType) {
  /* 显式类型标注 + 默认宽度字面量初始化：DEFINE 须按声明类型 implicit_cast
     init（var x:i64 = 7 的 7 是 i32 字面量，须拓宽为 i64）。此前 DEFINE 直接
     clone init 原值，运行时 x 实为 i32 与 sema 符号表 i64 不一致，后续
     `x = 9 as i64`（同类型赋值）误报 not a widening conversion。 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i64 = 7;\n"
      "  x = 9 as i64;\n"
      "  x = x + 5;\n"
      "  _ = libc::printf(\"%d\\n\", x as i32);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarInferenceMatchesDeclared) {
  /* var t = x（推断自显式标注的 i64 变量）→ t 运行时亦为 i64，
     与 sema 推断一致；赋值 i64 值合法 */
  std::string path = write_temp_file(
      "func main():i32 {\n"
      "  var x:i64 = 7;\n"
      "  var t = x;\n"
      "  t = 9 as i64;\n"
      "  _ = libc::printf(\"%d\\n\", t as i32);\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(Driver, RunFileAnnotatedWideVarNarrowLiteralRejected) {
  /* 显式 i64 标注 + 不可 widening 的初始化（i64 → i8 收窄）：
     sema 应拦截，运行期不执行 */
  std::string path = write_temp_file(
      "func main(): void { var x:i8 = 300; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

/* 源码 → .cxb → .cxs → .cxb：两次 .cxb 应字节级一致（往返稳定），
 * 且中间 .cxs 可被执行。 */


/* 内容嗅探：按内容（而非扩展名）判定输入类型。 */


/* 内容嗅探驱动的互转：即使扩展名不标准也能正确转换。 */


/* 互转的失败路径：文件不存在 / 输入内容损坏。 */

