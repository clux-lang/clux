/*
 * 闭包 = 捕获聚合结构体（2026-10，与 C 后端的 `struct __x_closure { callee; env }` 对应）。
 *
 * 运行时表示：func 值 = { 函数实现, env 块 }；env 是**捕获聚合结构体**的实例
 * （`capture_type`：字段 = 捕获槽 + 槽位所有权）。
 *   - 捕获读取：sema 改写为 `__ctx.env.<cap>` → 普通结构体字段访问
 *     （`__ctx` 是调用帧绑定的隐藏局部，VM 的 FIELD_GET 闭包分支给 env 借用值）；
 *   - 捕获写入：`SET_CLOSURE <name>` → 写 env 字段（槽位所有权 + implicit_cast）；
 *   - clone：按 env 的槽位所有权深/浅拷贝到**新实例**的新 env；
 *   - free：`FREE <闭包>` → 释放 env 里的 own 捕获 + env 块本体。
 * cfunc 不受影响：没有捕获、没有 capture_type，就是普通函数指针值。
 */
#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "driver/driver.h"
#include "test_common.h"

namespace {

bool disasm_of(const std::string &src, const std::string &tag,
               std::string *out_text) {
  const std::string src_path = write_temp_file(src, tag);
  const std::string bin_path = clux_test_temp_dir() + "/" + tag + ".cxb";
  const std::string asm_path = clux_test_temp_dir() + "/" + tag + ".cxs";
  if (driver_build_bin(src_path.c_str(), bin_path.c_str()) != 0) return false;
  if (driver_bin_to_asm(bin_path.c_str(), asm_path.c_str()) != 0) return false;
  std::ifstream in(asm_path);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out_text = ss.str();
  std::remove(bin_path.c_str());
  std::remove(asm_path.c_str());
  return true;
}

const char *kClosure =
    "func main(): i32 {\n"
    "  var a: i32 = 2;\n"
    "  var f = func |(q: i32 = a + 1)| get(): i32 { return q * 2; };\n"
    "  if (f() != 6) { return 1; }\n"
    "  return 0;\n"
    "}\n";

} // namespace

/* ==================== 捕获经 env 结构体访问 ==================== */

TEST(ClosureAggregate, CaptureReadViaCtxEnv) {
  std::string text;
  ASSERT_TRUE(disasm_of(kClosure, "ca_bc", &text)) << text;
  /* 捕获读取 = __ctx.env.q（结构体字段链），不再是按名查 closure_scope */
  EXPECT_NE(text.find("PUSH \"__ctx\""), std::string::npos) << text;
  EXPECT_NE(text.find("FIELD_GET \"env\""), std::string::npos) << text;
  /* 捕获写入走 SET_CLOSURE（VM 写 env 字段） */
  EXPECT_NE(text.find("SET_CLOSURE"), std::string::npos) << text;
  /* env 类型在 hoist 注册 */
  EXPECT_NE(text.find("SET_FUNC_CAPTURES"), std::string::npos) << text;
}

TEST(ClosureAggregate, CaptureFromStructAggregate) {
  /* 捕获含 own 槽位的聚合：捕获值读/写/销毁都按结构体槽位规则 */
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var b: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
      "  var f = clone(func |(x: own Box = move(b))| get(): i32 {\n"
      "      return x.p.* + x.n; });\n"
      "  if (f() != 8) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ca_struct_capture");
}

/* ==================== clone = 聚合克隆（独立 env） ==================== */

TEST(ClosureAggregate, CloneOfOwnCaptureNoLeak) {
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{42};\n"
      "  var g = clone(func |(q: own *i32 = move(p))| get(): i32 { return q.*; });\n"
      "  var f = clone(g);\n"
      "  if (f() != 42) { return 1; }\n"
      "  if (g() != 42) { return 2; }\n"
      "  return 0;\n"              /* 两个实例的 own 捕获各释放一次（无泄漏） */
      "}\n",
      "ca_clone_own");
}

/* ==================== free = 聚合释放 ==================== */

TEST(ClosureAggregate, RefCaptureNotFreed) {
  /* ref 捕获是借用：闭包销毁不得释放借用目标（否则源变量作用域退出时双释放） */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var x: i32 = 7;\n"
      "  {\n"
      "    var f = func |(r = x.&)| get(): i32 { return r.*; };\n"
      "    if (f() != 7) { return 1; }\n"
      "  }\n"
      "  if (x != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "ca_ref_capture");
}

/* ==================== cfunc 不受影响 ==================== */

TEST(ClosureAggregate, CfuncHasNoCaptureEnv) {
  /* cfunc 声明 = 普通函数指针（无捕获、无 capture_type）：调用路径与闭包无关。 */
  clux_expect_ok(
      "extern lib \"*\" as libc;\n"
      "extern cfunc libc::abs(x: i32): i32;\n"
      "func main(): i32 {\n"
      "  if (libc::abs(-5) != 5) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "ca_cfunc");
}
