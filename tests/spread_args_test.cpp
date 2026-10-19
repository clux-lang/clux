/*
 * File: spread_args_test.cpp
 * Description: 实参展开 `...expr`（函数调用里的摊开语法）。
 *
 *   fn(a, ...b, c)   ⇒   fn(a, b[0], b[1], …, c)     —— sema 阶段展开
 *
 * 约束：
 *   - 操作数必须是**编译期长度已知**的数组（`[N]T`）或元组（`<T1,T2,…>`）；
 *     切片（长度运行期才知道）拒绝。
 *   - 操作数必须是**可复读的位置表达式**（变量 / 成员访问）——展开后每个元素
 *     都是一次 `operand[i]` 读取，带副作用的表达式会被重复求值，直接拒绝。
 *   - 空数组展开出 0 个实参。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "driver/driver.h"
#include "parser/fmt.h"
}

#include "test_common.h"

namespace {

const char *kPrelude =
    "struct Holder { arr: [2]i32; }\n"
    "struct P {\n"
    "  x: i32;\n"
    "  func add(self: ref *P, a: i32, b: i32): i32 { return self.x + a + b; }\n"
    "}\n"
    "func sum2(a: i32, b: i32): i32 { return a + b; }\n"
    "func sum3(a: i32, b: i32, c: i32): i32 { return a + b + c; }\n"
    "func sum4(a: i32, b: i32, c: i32, d: i32): i32 { return a + b + c + d; }\n";

std::string src(const std::string &body) { return std::string(kPrelude) + body; }

} // namespace

/* 数组展开（中间位置）：sum4(1, ...arr, 4) ⇒ sum4(1, 2, 3, 4) */
TEST(SpreadArgs, ArrayInMiddlePosition) {
  clux_expect_ok(src("func main(): i32 {\n"
                     "  var arr = .[2]i32{ 2, 3 };\n"
                     "  return sum4(1, ...arr, 4) == 10 ? 0 : 1;\n"
                     "}\n"),
                 "spread_array");
}

/* 元组展开（前置）：sum3(...tup, 5) ⇒ sum3(10, 20, 5) */
TEST(SpreadArgs, TupleInLeadingPosition) {
  clux_expect_ok(src("func main(): i32 {\n"
                     "  var tup = .<i32, i32>{ 10, 20 };\n"
                     "  return sum3(...tup, 5) == 35 ? 0 : 1;\n"
                     "}\n"),
                 "spread_tuple");
}

/* 成员访问作操作数：sum2(...holder.arr) ⇒ sum2(7, 8) */
TEST(SpreadArgs, MemberOperand) {
  clux_expect_ok(src("func main(): i32 {\n"
                     "  var h = .Holder{ .arr = .[2]i32{ 7, 8 } };\n"
                     "  return sum2(...h.arr) == 15 ? 0 : 1;\n"
                     "}\n"),
                 "spread_member");
}

/* 空数组：展开出 0 个实参 → sum2(1, 2) */
TEST(SpreadArgs, EmptyArrayExpandsToNothing) {
  clux_expect_ok(src("func main(): i32 {\n"
                     "  var empty = .[0]i32{};\n"
                     "  return sum2(1, 2, ...empty) == 3 ? 0 : 1;\n"
                     "}\n"),
                 "spread_empty");
}

/* FFI 变参 + 展开：printf("%d+%d", ...arr) */
TEST(SpreadArgs, ExternVariadicCall) {
  clux_expect_ok(src("extern lib libc from \"*\";\n"
                     "extern func libc::printf(format: str, ...): void;\n"
                     "func main(): i32 {\n"
                     "  var arr = .[2]i32{ 3, 4 };\n"
                     "  libc::printf(\"%d+%d\\n\", ...arr);\n"
                     "  return 0;\n"
                     "}\n"),
                 "spread_ffi");
}

/* 方法调用 + 展开：p.add(...arr)（self 之外的两个参数） */
TEST(SpreadArgs, MemberCallWithSpread) {
  clux_expect_ok(src("func main(): i32 {\n"
                     "  var p = .P{ .x = 100 };\n"
                     "  var arr = .[2]i32{ 3, 4 };\n"
                     "  return p.add(...arr) == 107 ? 0 : 1;\n"
                     "}\n"),
                 "spread_member_call");
}

/* 切片：长度运行期才知道 → 拒绝 */
TEST(SpreadArgs, SliceOperandRejected) {
  std::string path = write_temp_file(
      src("func main(): i32 {\n"
          "  var arr = .[3]i32{ 1, 2, 3 };\n"
          "  var s = arr[0:2];\n"
          "  return sum2(...s);\n"
          "}\n"),
      "spread_slice");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("compile-time-known length"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 带副作用的位置（调用表达式）→ 拒绝（展开会重复求值） */
TEST(SpreadArgs, SideEffectingOperandRejected) {
  std::string path = write_temp_file(
      src("func pair(a: i32, b: i32): i32 { return a + b; }\n"
          "func main(): i32 { return sum2(...pair(1, 2)); }\n"),
      "spread_call");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("must be a variable or a field access"), std::string::npos)
      << err;
  std::remove(path.c_str());
}

/* 展开后实参数量不符 → 照常报数量错误 */
TEST(SpreadArgs, WrongArityAfterExpansion) {
  std::string path = write_temp_file(
      src("func main(): i32 {\n"
          "  var arr = .[3]i32{ 1, 2, 3 };\n"
          "  return sum2(1, ...arr);\n"
          "}\n"),
      "spread_arity");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("argument"), std::string::npos) << err;
  std::remove(path.c_str());
}

/* 展开结果可随容器落盘：emit → run .cxb */
TEST(SpreadArgs, SurvivesCxbRoundTrip) {
  const std::string program =
      src("func main(): i32 {\n"
          "  var arr = .[2]i32{ 2, 3 };\n"
          "  var tup = .<i32, i32>{ 4, 5 };\n"
          "  if (sum4(1, ...arr, 4) != 10) { return 1; }\n"
          "  if (sum3(...tup, 6) != 15) { return 2; }\n"
          "  return 0;\n"
          "}\n");
  std::string path = write_temp_file(program, "spread_rt");
  std::string bin = path + ".cxb";
  ASSERT_EQ(driver_build_bin(path.c_str(), bin.c_str()), 0);
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);
  std::remove(path.c_str());
  std::remove(bin.c_str());
}

/* fmt：`...` 必须原样保留（漏渲染会把展开悄悄改写成单实参） */
TEST(SpreadArgs, FmtPreservesSpread) {
  const char *code = "func sum2(a: i32, b: i32): i32 { return a + b; }\n"
                     "func main(): i32 {\n"
                     "    var arr = .[2]i32{ 1, 2 };\n"
                     "    return sum2(...arr);\n"
                     "}\n";
  allocator_t *a = create_allocator(malloc, free);
  char *out = fmt_format_source(a, code, std::strlen(code), nullptr);
  ASSERT_NE(out, nullptr);
  const std::string once(out);
  allocator_free(a, (void **)&out);
  delete_allocator(&a);
  EXPECT_NE(once.find("sum2(...arr)"), std::string::npos) << once;
}
