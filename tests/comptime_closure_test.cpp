/*
 * File: comptime_closure_test.cpp
 * Description: **comptime 函数返回正常函数**——含闭包（捕获常量/函数值）。
 *
 * 背景（装饰器的前置能力）：装饰器 = comptime 函数，接收源函数并返回新函数；
 * 因此"函数是编译期的值"必须成立：
 *   - comptime 函数里定义的普通函数可以返回（无捕获，折叠为 LOAD_FUNCTION）；
 *   - comptime 函数返回的**闭包**（`func |f, n| ...`）必须能把捕获值物化成
 *     编译期常量，运行期构造出真实闭包（MAKE_FUNCTION + SET_CLOSURE）。
 * 物化路径：CTFE 求值字面量时按捕获值建实例 env → sema 编码为
 * "func_id + 捕获常量表" → 折叠成**克隆的 func 字面量**（括号捕获形态）→
 * compiler 走既有函数字面量路径（零新指令）。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "driver/driver.h"
#include "sema/comptime.h"
#include "vm/function.h"
#include "vm/vm.h"
}

#include "test_common.h"

namespace {

const char *kPrelude =
    "func double(x: i32): i32 { return x * 2; }\n"
    "func triple(x: i32): i32 { return x * 3; }\n";

std::string with_prelude(const std::string &body) {
  return std::string(kPrelude) + body;
}

} // namespace

/* comptime 函数返回**普通函数**（无捕获）——编译期常量函数引用。 */
TEST(ComptimeClosure, ReturnsPlainFunction) {
  clux_expect_ok(with_prelude(
                     "comptime func pick(): func(i32) -> i32 { return triple; }\n"
                     "var g = pick();\n"
                     "func main(): i32 { return g(5) == 15 ? 0 : 1; }\n"),
                 "cc_plain");
}

/* comptime 函数在体内定义函数并返回它（fid 由 sema 分配、hoist 编译）。 */
TEST(ComptimeClosure, ReturnsInnerFunction) {
  clux_expect_ok(
      with_prelude(
          "comptime func make_inc(): func(i32) -> i32 {\n"
          "  func inc(x: i32): i32 { return x + 1; }\n"
          "  return inc;\n"
          "}\n"
          "var inc1 = make_inc();\n"
          "func main(): i32 { return inc1(5) == 6 ? 0 : 1; }\n"),
      "cc_inner");
}

/* 返回**捕获常量**的闭包：捕获值物化为常量，运行期构造真实闭包。 */
TEST(ComptimeClosure, ReturnsClosureCapturingConstant) {
  clux_expect_ok(with_prelude(
                     "comptime func make_add(n: i32): func |n: i32| (i32) -> i32 {\n"
                     "  return func |n| addn(x: i32): i32 { return x + n; };\n"
                     "}\n"
                     "var add10 = make_add(10);\n"
                     "func main(): i32 { return add10(5) == 15 ? 0 : 1; }\n"),
                 "cc_const_capture");
}

/* 返回**捕获函数值**的闭包——装饰器的核心形态（包一层）。 */
TEST(ComptimeClosure, ReturnsClosureCapturingFunctionValue) {
  clux_expect_ok(
      with_prelude(
          "comptime func add100(f: func(i32) -> i32):\n"
          "    func |f: func(i32) -> i32| (i32) -> i32 {\n"
          "  return func |f| wrap(x: i32): i32 { return f(x) + 100; };\n"
          "}\n"
          "var b = add100(double);\n"
          "func main(): i32 {\n"
          "  if (b(5) != 110) { return 1; }\n"
          "  var local = add100(triple);\n"
          "  if (local(5) != 115) { return 2; }\n"
          "  return 0;\n"
          "}\n"),
      "cc_func_capture");
}

/* 多个捕获（函数值 + 两个常量）按序写入 env。 */
TEST(ComptimeClosure, ReturnsClosureWithMultipleCaptures) {
  clux_expect_ok(
      with_prelude(
          "comptime func sp(f: func(i32) -> i32, k: i32, c: i32): "
          "func |f: func(i32) -> i32, k: i32, c: i32| (i32) -> i32 {\n"
          "  return func |f, k, c| w(x: i32): i32 { return f(x) * k + c; };\n"
          "}\n"
          "var s = sp(triple, 10, 1);\n"
          "func main(): i32 { return s(5) == 151 ? 0 : 1; }\n"),
      "cc_multi_capture");
}

/* 闭包常量随容器落盘：emit → run .cxb，以及 .cxb → .cxs → .cxb 往返。 */
TEST(ComptimeClosure, SurvivesCxbRoundTrip) {
  const std::string src = with_prelude(
      "comptime func add100(f: func(i32) -> i32):\n"
      "    func |f: func(i32) -> i32| (i32) -> i32 {\n"
      "  return func |f| wrap(x: i32): i32 { return f(x) + 100; };\n"
      "}\n"
      "var b = add100(double);\n"
      "func main(): i32 { return b(5) == 110 ? 0 : 1; }\n");
  std::string path = write_temp_file(src, "cc_rt");
  std::string bin = path + ".cxb";
  std::string asm_path = path + ".cxs";
  std::string bin2 = path + ".2.cxb";
  ASSERT_EQ(driver_build_bin(path.c_str(), bin.c_str()), 0);
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(bin.c_str(), asm_path.c_str()), 0);
  /* 反汇编里能看到闭包构造（MAKE_FUNCTION + SET_CLOSURE） */
  const std::string text = slurp(asm_path);
  EXPECT_NE(text.find("MAKE_FUNCTION"), std::string::npos) << text;
  EXPECT_NE(text.find("SET_CLOSURE"), std::string::npos) << text;
  ASSERT_EQ(driver_asm_to_bin(asm_path.c_str(), bin2.c_str()), 0);
  EXPECT_EQ(driver_run_bin(bin2.c_str()), 0);
  std::remove(path.c_str());
  std::remove(bin.c_str());
  std::remove(asm_path.c_str());
  std::remove(bin2.c_str());
}

/* comptime 闭包捕获**运行期变量**必须报错（不能静默产出坏闭包）。 */
TEST(ComptimeClosure, RejectsCapturingRuntimeVariable) {
  const std::string src = with_prelude(
      "var rt: i32 = 5;\n"
      "comptime func leak(): func(i32) -> i32 {\n"
      "  return func |rt| w(x: i32): i32 { return x + rt; };\n"
      "}\n"
      "var g = leak();\n"
      "func main(): i32 { return 0; }\n");
  std::string path = write_temp_file(src, "cc_bad");
  std::string err;
  EXPECT_EQ(clux_run_capture(path, &err), 1);
  EXPECT_NE(err.find("cannot capture runtime variable 'rt'"), std::string::npos)
      << err;
  std::remove(path.c_str());
}

/* 装饰后的函数可再被当作值传递/返回（函数值的一等性不被破坏）。 */
TEST(ComptimeClosure, DecoratedFunctionRemainsFirstClass) {
  clux_expect_ok(
      with_prelude(
          "comptime func add100(f: func(i32) -> i32):\n"
          "    func |f: func(i32) -> i32| (i32) -> i32 {\n"
          "  return func |f| wrap(x: i32): i32 { return f(x) + 100; };\n"
          "}\n"
          "var b: func |f: func(i32) -> i32| (i32) -> i32 = add100(double);\n"
          "func apply[F](f: F, x: i32): i32 { return f(x); }\n"
          "func main(): i32 {\n"
          "  if (apply[func |f: func(i32) -> i32| (i32) -> i32](b, 5) != 110) { return 1; }\n"
          "  var g = b;\n"
          "  if (g(1) != 102) { return 2; }\n"
          "  return 0;\n"
          "}\n"),
      "cc_first_class");
}
