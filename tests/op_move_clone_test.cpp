/*
 * MOVE 与 CLONE 指令均已彻底移除。
 *
 * 所有权是**纯编译期**的：`move(x)` 的转移语义由 sema 完成（源进 TDZ、销毁
 * 序列跳过），运行期就是普通取值 → 不再需要 MOVE 指令。`clone(x)` 由 sema
 * 改写为生成的 clone 函数调用；闭包值 / cunion 被指向值 / 无 prelude 的切片
 * 改写为 `__deep_clone(v)` 内置函数调用（对象图深拷贝）——CLONE 指令已删除。
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

} // namespace

/* ==================== move 不产生任何指令 ==================== */

TEST(OpMoveRemoved, MoveEmitsNoOpcode) {
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "struct Box { p: own *i32; n: i32 }\n"
                  "func main(): i32 {\n"
                  "  var a: own *i32 = new i32{1};\n"
                  "  var b: own *i32 = move(a);\n"
                  "  var c: own Box = .Box{ .p = move(b), .n = 2 };\n"
                  "  return c.n;\n"
                  "}\n",
                  "op_move_removed", &text))
      << text;
  EXPECT_EQ(text.find("\nMOVE\n"), std::string::npos)
      << "MOVE 指令应已移除（move 是纯编译期转移）\n"
      << text;
}

TEST(OpMoveRemoved, MoveSemanticsStillCorrect) {
  /* move 后源进 TDZ、目标接管；作用域退出各销毁一次（无泄漏、无双释放）。 */
  clux_expect_ok(
      "struct Box { p: own *i32; n: i32 }\n"
      "func main(): i32 {\n"
      "  var a: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
      "  var b: own Box = move(a);\n"
      "  if (b.p.* != 7) { return 1; }\n"
      "  return 0;\n"
      "}\n",
      "op_move_semantics");
}

/* ==================== clone：不再用 CLONE（生成函数） ==================== */

TEST(OpCloneNarrowed, StructCloneHasNoCloneOpcode) {
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "struct Box { p: own *i32; n: i32 }\n"
                  "func main(): i32 {\n"
                  "  var a: own Box = .Box{ .p = new i32{7}, .n = 1 };\n"
                  "  var b: own Box = clone(a);\n"
                  "  return b.n;\n"
                  "}\n",
                  "op_clone_struct", &text))
      << text;
  EXPECT_EQ(text.find("\nCLONE\n"), std::string::npos)
      << "struct clone 应走生成的 __clone_* 函数，不再发 CLONE\n"
      << text;
  EXPECT_NE(text.find("PUSH \"__clone_0\""), std::string::npos) << text;
}

TEST(OpCloneNarrowed, ScalarPointerCloneUsesGeneratedFunction) {
  /* `own *i32` 的 clone：`__cloneptr_*(p)` → `new i32{ p.* }`（单字段构造），
     不再发 CLONE。 */
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "func main(): i32 {\n"
                  "  var a: own *i32 = new i32{7};\n"
                  "  var b: own *i32 = clone(a);\n"
                  "  return b.*;\n"
                  "}\n",
                  "op_clone_ptr", &text))
      << text;
  EXPECT_EQ(text.find("\nCLONE\n"), std::string::npos) << text;
  EXPECT_NE(text.find("PUSH \"__cloneptr_0\""), std::string::npos) << text;
  EXPECT_NE(text.find("CONSTRUCT 1"), std::string::npos) << text;
}

TEST(OpCloneNarrowed, SliceCloneHasNoCloneOpcode) {
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "func main(): i32 {\n"
                  "  var s: own []i32 = make(i32, 2, 1, 2);\n"
                  "  var c: own []i32 = clone(s);\n"
                  "  return c[0];\n"
                  "}\n",
                  "op_clone_slice", &text))
      << text;
  EXPECT_EQ(text.find("\nCLONE\n"), std::string::npos) << text;
  EXPECT_NE(text.find("PUSH \"__cloneslice_0\""), std::string::npos) << text;
}

/* ==================== 闭包 clone：__deep_clone 内置函数 ==================== */

TEST(OpCloneNarrowed, ClosureCloneStillDeepCopiesCaptures) {
  /* 闭包 = 捕获聚合结构体（env）；env 的捕获类型只有运行期才知道，静态类型
     表达不了 → sema 改写为 `__deep_clone(v)` 内置函数调用（对象图深拷贝）。
     语义：克隆独立于原闭包。 */
  clux_expect_ok(
      "func main(): i32 {\n"
      "  var p: own *i32 = new i32{42};\n"
      "  var f = clone(func |(q = clone(p))| get(): i32 { return q.*; });\n"
      "  if (f() != 42) { return 1; }\n"
      "  if (p.* != 42) { return 2; }\n"
      "  return 0;\n"
      "}\n",
      "op_clone_closure");
}

TEST(OpCloneNarrowed, ClosureVariableCloneUsesDeepCloneBuiltin) {
  /* `clone(闭包变量)`：静态类型是签名 → 走 __deep_clone 内置函数（不再是 CLONE
     指令）；深拷贝 env，源闭包与克隆互不影响。 */
  std::string text;
  ASSERT_TRUE(disasm_of(
                  "func main(): i32 {\n"
                  "  var p: own *i32 = new i32{42};\n"
                  "  var g = clone(func |(q: own *i32 = move(p))| get(): i32 {"
                  " return q.*; });\n"
                  "  var f = clone(g);\n"
                  "  if (f() != 42) { return 1; }\n"
                  "  return 0;\n"
                  "}\n",
                  "op_clone_closure_var", &text))
      << text;
  EXPECT_EQ(text.find("\nCLONE\n"), std::string::npos) << text;
  EXPECT_NE(text.find("PUSH \"__deep_clone\""), std::string::npos) << text;
}
