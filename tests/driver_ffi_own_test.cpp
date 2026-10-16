// driver_ffi_own_test.cpp —— extern cfunc/var 签名不得含 own
//
// 规则（m6-design §2.3/§9.3"禁止 `own *T`"）：**C 侧没有所有权概念**，所以
// own 资源不能出现在 C ABI 签名里——顶层 `own *T` / `own []T` 在**参数位、
// 返回位、extern var 类型位**都必须被拒。
//
// 这是"**签名即约束**"：非法在**声明处**被拒，而不是等调用点。
// ref *T / fatal *T 不持有资源，仍按指针宽度放行（§2.3 只允许 ref/fatal 指针）。
//
// **判定入口是 ownership 而不是类型**（2026-10 重构后）：所有权已从类型层迁移到
// 值/符号层，`type_t` 不携带 ownership，所以 `ffi_c_abi_reject_reason_own` 需要
// 调用方传入该位置的所有权。extern 声明的类型槽位在 Pass 1 就被折叠
// （AST_PTR→AST_TYPE_REF，ownership token 丢失），因此解析器在折叠前把所有权
// 缓存进 `ast_var_def_t::ownership`，sema 从那里读取——否则 `own *T` 会**静默
// 漏过** C ABI 检查（本文件即该回归的护栏）。

#include <gtest/gtest.h>
#include <cstdio>
#include <string>

extern "C" {
#include "driver/driver.h"
}

#include "test_common.h"

namespace {


/* 只关心**声明期**的拒绝：诊断出现在编译阶段（driver_run_file 返回 1）。
   这些声明引用的 C 符号不存在，但类型约束先于 dlsym 解析失败。 */
const char *kPrelude = "extern lib \"*\" as libc;\n";

} // namespace

TEST(DriverFfiOwn, OwnPointerParamIsRejected) {
  std::string path = write_temp_file(
      std::string(kPrelude) +
      "extern cfunc libc::f(p: own *i32): i32;\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFfiOwn, OwnSliceParamIsRejected) {
  std::string path = write_temp_file(
      std::string(kPrelude) +
      "extern cfunc libc::f(p: own []i32): i32;\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFfiOwn, StructWithOwnFieldParamIsRejected) {
  std::string path = write_temp_file(
      "struct Box { p: own *i32; n: i32 }\n" +
      std::string(kPrelude) +
      "extern cfunc libc::f(b: Box): i32;\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFfiOwn, StructWithOwnFieldReturnIsRejected) {
  /* 返回值同理：C 侧无法构造含 own 字段的值，也无法表达"谁拥有"。 */
  std::string path = write_temp_file(
      "struct Box { p: own *i32; n: i32 }\n" +
      std::string(kPrelude) +
      "extern cfunc libc::f(): Box;\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFfiOwn, OwnPointerExternVarIsRejected) {
  std::string path = write_temp_file(
      std::string(kPrelude) +
      "extern var libc::v: own *i32;\n"
      "func main(): i32 { return 0; }\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 1);
  std::remove(path.c_str());
}

TEST(DriverFfiOwn, RefAndFatalPointersAreStillAccepted) {
  /* §2.3：只允许 ref/fatal 指针——它们不持有资源，仍按指针宽度放行。
     **真的调用真实 C 符号**（msvcrt 的 strlen + 测试库的 clux_first_i32），
     确保 exit 0 来自"跑通"而不是"被拒后没崩"。
     注：`extern lib "*"` 这类**通配库**在加载期就解析全部声明符号，所以正例必须
     挂真实库（惰性解析只对具体库路径生效）。 */
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "extern cfunc crt::strlen(p: ref *u8): u64;\n"
      "extern lib \"clux_ffi_testdata.dll\" as td;\n"
      "extern cfunc td::clux_first_i32(p: fatal *i32): i32;\n"
      "func main(): i32 {\n"
      "  var s: str = \"hello\";\n"
      "  if (crt::strlen(data(s) as ref *u8) != 5u64) { return 1; }\n"
      "  var x: own *i32 = new i32{42};\n"
      "  if (td::clux_first_i32(move(x)) != 42) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverFfiOwn, PlainStructIsStillAccepted) {
  /* 不含 own 的平凡 struct 按值跨 C ABI 依旧合法（布局 1:1）——真调用测试库的
     `clux_point_sum(CluxPoint)` 验证。 */
  std::string path = write_temp_file(
      "struct Point { x: i32; y: i32 }\n"
      "extern lib \"clux_ffi_testdata.dll\" as td;\n"
      "extern cfunc td::clux_point_sum(p: Point): i32;\n"
      "extern cfunc td::clux_point_make(x: i32, y: i32): Point;\n"
      "func main(): i32 {\n"
      "  var p: Point = .Point{ .x = 3, .y = 4 };\n"
      "  if (td::clux_point_sum(p) != 7) { return 1; }\n"
      "  var q: Point = td::clux_point_make(5, 6);\n"
      "  if (q.x != 5 || q.y != 6) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}
