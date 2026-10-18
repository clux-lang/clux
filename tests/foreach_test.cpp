/*
 * Description: `foreach` 端到端（CLI 运行：parser → 前脱糖 → sema → compiler → VM）
 * Create: 2026-10
 *
 * foreach 是**纯前脱糖语法糖**：展开成索引 for 循环后 sema/compiler 完全看不到
 * 它（见 include/desugar/desugar.h、tests/desugar_test.cpp 的 AST 形状断言）。
 * 本文件验证**运行结果**，特别是不容易在 AST 层看出来的语义点：
 *   - `continue` 必须仍然步进（展开成 while + 体尾自增会死循环）；
 *   - 迭代器表达式只求值一次（形态不是裸标识符时先落临时变量）；
 *   - own 容器（move-only）能直接遍历（裸标识符形态不复制迭代器）；
 *   - 闭包 / defer / comptime func 体内的 foreach 同样正确。
 *
 * **为什么用子进程跑 `clux run` 而不是进程内 `driver_run_file`**：
 * `driver_run_file` 成功时恒返回 0（main 的返回值不回传），要断言**计算结果**
 * 只能看程序自己打印的东西；而 clux 程序的 `libc::printf` 由 FFI 从**已加载
 * 模块**里按名字解析（见 `ffi_sym_current_process`），命中的是系统 UCRT 那份
 * `printf`，与 gtest 进程自身链接的 CRT **不是同一个实例**——两份 CRT 的 fd 表
 * 互相独立，进程内 `_dup2(1)` 抓不到那段输出（实测：既不进捕获文件也不进
 * 控制台）。子进程 + 管道捕获与 CRT 实现无关，同时顺带覆盖真正的 CLI 入口。
 * stderr 用 `2>` 单独落到文件：内存泄漏报告（driver 用自己那份 CRT 的
 * `fprintf(stderr)`）与诊断文本都在那里。
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "test_common.h"

namespace {

/* 与测试二进制同目录的 CLI（ctest 与手动 `./build/clux_test` 都在 build/ 下）。 */
std::string clux_cli_path() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(nullptr, buf, (DWORD)sizeof buf);
  std::filesystem::path self(std::string(buf, n));
  return (self.parent_path() / "clux.exe").string();
#else
  std::error_code ec;
  std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec || self.empty()) return "clux";
  return (self.parent_path() / "clux").string();
#endif
}

std::string quoted(const std::string &s) { return "\"" + s + "\""; }

/* 管道按二进制读回；把子进程文本模式写出的 CRLF 归一化成 LF，便于逐字节断言。 */
std::string normalize_newlines(std::string s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n') continue;
    out.push_back(s[i]);
  }
  return out;
}

/**
 * 跑 `<cli> run <src>`，返回退出码并捕获 stdout / stderr。
 *
 * 两个流都用 shell 重定向落到文件（而不是管道读取），原因是输出必须与 CRT
 * 无关（见文件头的说明）。**Windows 上命令必须以 `(` 开头**：`system()` 走
 * `cmd.exe /c <cmd>`，而 cmd 对"以引号开头"的命令行有"吞掉首尾各一个引号"的
 * 旧行为 → 重定向目标少一个引号，报 "The filename, directory name, or volume
 * label syntax is incorrect."。用括号分组一条命令即可跳过该规则，退出码仍
 * 透传（`cmd /c ( … )` 的最后一条命令的退出码）。
 */
int run_cli(const std::string &src_path, std::string *out, std::string *err) {
  std::filesystem::create_directories(clux_test_temp_dir());
  const std::filesystem::path base =
      std::filesystem::absolute(clux_test_temp_dir());
  const std::filesystem::path out_file = base / "foreach_cli_stdout.txt";
  const std::filesystem::path err_file = base / "foreach_cli_stderr.txt";
  std::error_code ec;
  std::filesystem::remove(out_file, ec);
  std::filesystem::remove(err_file, ec);

  const std::string inner =
      quoted(clux_cli_path()) + " run " +
      quoted(std::filesystem::absolute(src_path).string()) + " > " +
      quoted(out_file.string()) + " 2> " + quoted(err_file.string());
#ifdef _WIN32
  const std::string cmd = "(" + inner + ")";
#else
  const std::string cmd = inner;
#endif

#ifdef _WIN32
  const int rc = std::system(cmd.c_str());
#else
  const int status = std::system(cmd.c_str());
  const int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
  if (out) *out = normalize_newlines(slurp(out_file.string()));
  if (err) *err = slurp(err_file.string());
  return rc;
}

/**
 * 正例：退出码 0、stderr 无内存泄漏、stdout **逐字节等于** expected。
 *
 * 精确匹配 stdout 是这些用例的主要断言手段：foreach 的展开若把步进 / 长度 /
 * 元素绑定写错，输出一定会变（值错、少一轮、多一轮、死循环）。
 */
void expect_output(const std::string &src, const std::string &expected,
                   const std::string &tag = "foreach") {
  const std::string path = write_temp_file(src, tag);
  ASSERT_FALSE(path.empty());
  std::string out, err;
  const int rc = run_cli(path, &out, &err);
  EXPECT_EQ(rc, 0) << src << "\n--- stderr ---\n" << err;
  EXPECT_EQ(err.find("memory leak"), std::string::npos)
      << "destruction sequence leaked:\n" << err;
  EXPECT_EQ(out, expected) << "--- src ---\n" << src << "\n--- stderr ---\n"
                           << err;
}

/** 负例：编译期拒绝（CLI 退出码 1），stderr 含 needle。 */
void expect_reject(const std::string &src, const char *needle,
                   const std::string &tag = "foreach_bad") {
  const std::string path = write_temp_file(src, tag);
  ASSERT_FALSE(path.empty());
  std::string out, err;
  const int rc = run_cli(path, &out, &err);
  EXPECT_EQ(rc, 1) << src << "\n--- stderr ---\n" << err;
  if (needle)
    EXPECT_NE(err.find(needle), std::string::npos)
        << "expected diagnostic containing '" << needle << "'\n--- stderr ---\n"
        << err << "\n--- src ---\n" << src;
}

} // namespace

/* ================================================================ */
/* 基本形态                                                         */
/* ================================================================ */

TEST(Foreach, ArraySumVarForm) {
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[5]i32{ 10, 20, 30, 40, 50 };\n"
      "  var sum = 0;\n"
      "  foreach (var x of a) {\n"
      "    sum = sum + x;\n"
      "  }\n"
      "  libc::printf(\"%d\\n\", sum);\n"
      "  return 0;\n"
      "}\n",
      "150\n");
}

TEST(Foreach, AssignmentFormUsesExistingVariable) {
  /* 无 `var`：每轮写回**已定义**变量（不是新声明） */
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[3]i32{ 7, 8, 9 };\n"
      "  var cur = 0;\n"
      "  foreach (cur of a) {\n"
      "    libc::printf(\"cur=%d\\n\", cur);\n"
      "  }\n"
      "  return 0;\n"
      "}\n",
      "cur=7\ncur=8\ncur=9\n");
}

TEST(Foreach, EmptyArrayRunsZeroTimes) {
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[0]i32{};\n"
      "  var n = 0;\n"
      "  foreach (var x of a) {\n"
      "    n = n + 1;\n"
      "  }\n"
      "  libc::printf(\"n=%d\\n\", n);\n"
      "  return 0;\n"
      "}\n",
      "n=0\n");
}

/* ================================================================ */
/* 控制流：continue / break                                         */
/* ================================================================ */

TEST(Foreach, ContinueStillAdvancesIndex) {
  /* 展开成 `for`（步进在 update 段）而不是 `while`（步进在体尾）——
     否则 continue 跳过自增 → 死循环。跳过 30 后应还剩 4 次迭代。 */
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[5]i32{ 10, 20, 30, 40, 50 };\n"
      "  var n = 0;\n"
      "  var sum = 0;\n"
      "  foreach (var x of a) {\n"
      "    if (x == 30) { continue; }\n"
      "    n = n + 1;\n"
      "    sum = sum + x;\n"
      "  }\n"
      "  libc::printf(\"n=%d sum=%d\\n\", n, sum);\n"
      "  return 0;\n"
      "}\n",
      "n=4 sum=120\n");
}

TEST(Foreach, BreakStopsIteration) {
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[5]i32{ 1, 2, 3, 4, 5 };\n"
      "  var last = 0;\n"
      "  foreach (var x of a) {\n"
      "    if (x > 3) { break; }\n"
      "    last = x;\n"
      "  }\n"
      "  libc::printf(\"last=%d\\n\", last);\n"
      "  return 0;\n"
      "}\n",
      "last=3\n");
}

TEST(Foreach, ReturnInsideLoop) {
  expect_output(
      "func first_big(a: [4]i32): i32 {\n"
      "  foreach (var x of a) {\n"
      "    if (x > 100) { return x; }\n"
      "  }\n"
      "  return -1;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a = .[4]i32{ 1, 200, 3, 400 };\n"
      "  libc::printf(\"%d\\n\", first_big(a));\n"
      "  return 0;\n"
      "}\n",
      "200\n");
}

/* ================================================================ */
/* 迭代器表达式：只求值一次                                          */
/* ================================================================ */

TEST(Foreach, IterableEvaluatedExactlyOnce) {
  /* 非裸标识符的迭代器先落临时变量（`var __arr_N = …`）：函数只调用一次，
     哪怕循环体迭代 3 次。 */
  expect_output(
      "var CALLS = 0;\n"
      "func make_arr(): [3]i32 {\n"
      "  CALLS = CALLS + 1;\n"
      "  return .[3]i32{ 5, 6, 7 };\n"
      "}\n"
      "func main(): i32 {\n"
      "  var sum = 0;\n"
      "  foreach (var x of make_arr()) {\n"
      "    sum = sum + x;\n"
      "  }\n"
      "  libc::printf(\"calls=%d sum=%d\\n\", CALLS, sum);\n"
      "  return 0;\n"
      "}\n",
      "calls=1 sum=18\n");
}

TEST(Foreach, SliceExpressionIterable) {
  /* 切片表达式（非标识符）→ 临时变量；切片本身是 ref []T，不涉及所有权 */
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[5]i32{ 1, 2, 3, 4, 5 };\n"
      "  var sum = 0;\n"
      "  foreach (var x of a[1:4]) {\n"
      "    sum = sum + x;\n"
      "  }\n"
      "  libc::printf(\"sum=%d\\n\", sum);\n"
      "  return 0;\n"
      "}\n",
      "sum=9\n");
}

TEST(Foreach, StructFieldArrayIsSnapshot) {
  /* 迭代器是成员表达式 → 临时变量快照：循环体内改原数组不影响本轮迭代 */
  expect_output(
      "struct Row { cells: [3]i32; }\n"
      "func main(): i32 {\n"
      "  var r = .Row{ .cells = .[3]i32{ 1, 1, 1 } };\n"
      "  var sum = 0;\n"
      "  foreach (var x of r.cells) {\n"
      "    r.cells[1] = 9;\n"
      "    sum = sum + x;\n"
      "  }\n"
      "  libc::printf(\"sum=%d\\n\", sum);\n"
      "  return 0;\n"
      "}\n",
      "sum=3\n");
}

/* ================================================================ */
/* own / ref 容器                                                    */
/* ================================================================ */

TEST(Foreach, OwnSliceLocalIteratesWithoutMoving) {
  /* own []T 是 move-only：展开若写成 `var __arr = s` 会被拒（sema 要求
     move()/clone()）。裸标识符迭代器因此**不复制**——循环结束后 s 仍可用。 */
  expect_output(
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 3, 10, 20, 30);\n"
      "  var sum = 0;\n"
      "  foreach (var x of s) {\n"
      "    sum = sum + x;\n"
      "  }\n"
      "  libc::printf(\"sum=%d len=%d\\n\", sum, len(s));\n"
      "  return 0;\n"
      "}\n",
      "sum=60 len=3\n");
}

TEST(Foreach, OwnSliceParameterIterates) {
  expect_output(
      "func sum_own(s: own []i32): i32 {\n"
      "  var acc = 0;\n"
      "  foreach (var x of s) {\n"
      "    acc = acc + x;\n"
      "  }\n"
      "  return acc;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s: own []i32 = make(i32, 4, 1, 2, 3, 4);\n"
      "  libc::printf(\"%d\\n\", sum_own(move(s)));\n"
      "  return 0;\n"
      "}\n",
      "10\n");
}

TEST(Foreach, RefSliceIterates) {
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[4]i32{ 5, 6, 7, 8 };\n"
      "  var r: ref []i32 = a[:];\n"
      "  var sum = 0;\n"
      "  foreach (var x of r) {\n"
      "    sum = sum + x;\n"
      "  }\n"
      "  libc::printf(\"sum=%d\\n\", sum);\n"
      "  return 0;\n"
      "}\n",
      "sum=26\n");
}

TEST(Foreach, OwnSliceOfOwnPointersLendsElements) {
  /* own 元素的容器：`own [] own *i32`（元素带 own 的切片）。元素以
     `ref *i32` **借出**——只读借用，不搬走元素所有权，循环后容器仍可用。 */
  expect_output(
      "func main(): i32 {\n"
      "  var s: own [] own *i32 = make(own *i32, 3, new i32{ 11 },\n"
      "                               new i32{ 22 }, new i32{ 33 });\n"
      "  var sum = 0;\n"
      "  foreach (var p: ref *i32 of s) {\n"
      "    sum = sum + p.*;\n"
      "  }\n"
      "  libc::printf(\"sum=%d len=%d\\n\", sum, len(s));\n"
      "  return 0;\n"
      "}\n",
      "sum=66 len=3\n");
}

/* ================================================================ */
/* 嵌套 / 作用域                                                     */
/* ================================================================ */

TEST(Foreach, NestedForeach) {
  expect_output(
      "func main(): i32 {\n"
      "  var m = .[2][2]i32{ .[2]i32{ 1, 2 }, .[2]i32{ 3, 4 } };\n"
      "  var sum = 0;\n"
      "  foreach (var row of m) {\n"
      "    foreach (var cell of row) {\n"
      "      sum = sum + cell;\n"
      "    }\n"
      "  }\n"
      "  libc::printf(\"sum=%d\\n\", sum);\n"
      "  return 0;\n"
      "}\n",
      "sum=10\n");
}

TEST(Foreach, ShadowedElementNameInNestedLoop) {
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[2]i32{ 1, 2 };\n"
      "  var b = .[2]i32{ 10, 20 };\n"
      "  var sum = 0;\n"
      "  foreach (var x of a) {\n"
      "    foreach (var x of b) {\n"
      "      sum = sum + x;\n"
      "    }\n"
      "  }\n"
      "  libc::printf(\"sum=%d\\n\", sum);\n"
      "  return 0;\n"
      "}\n",
      "sum=60\n");
}

TEST(Foreach, ElementVariableDiesWithIteration) {
  /* 元素变量是每轮**新声明**：循环结束后不可见（作用域不泄漏）。 */
  const std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a = .[2]i32{ 1, 2 };\n"
      "  foreach (var x of a) { }\n"
      "  libc::printf(\"%d\\n\", x);\n"
      "  return 0;\n"
      "}\n",
      "foreach_scope");
  std::string err;
  const int rc = clux_run_capture(path, &err);
  EXPECT_EQ(rc, 1) << "iteration variable must not escape the loop";
  EXPECT_NE(err.find("undefined variable 'x'"), std::string::npos) << err;
}

/* ================================================================ */
/* 与其他语法结构的组合                                              */
/* ================================================================ */

TEST(Foreach, InsideClosureBody) {
  /* 闭包体内的 foreach：展开出的 for 变量属于闭包自身作用域
     （blocks/for 作用域都在 fscope 子树内，不是"外层局部"） */
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[3]i32{ 1, 2, 3 };\n"
      "  var f = func |a| (): i32 {\n"
      "    var acc = 0;\n"
      "    foreach (var x of a) {\n"
      "      acc = acc + x;\n"
      "    }\n"
      "    return acc;\n"
      "  };\n"
      "  libc::printf(\"%d\\n\", f());\n"
      "  return 0;\n"
      "}\n",
      "6\n");
}

TEST(Foreach, WithDeferPerIteration) {
  /* defer 挂在**每轮**的块作用域上：三条 body 之后各弹一次，LIFO。 */
  expect_output(
      "func main(): i32 {\n"
      "  var a = .[2]i32{ 1, 2 };\n"
      "  foreach (var x of a) {\n"
      "    defer |x| { libc::printf(\"defer%d\\n\", x); }\n"
      "    libc::printf(\"body%d\\n\", x);\n"
      "  }\n"
      "  return 0;\n"
      "}\n",
      "body1\ndefer1\nbody2\ndefer2\n");
}

TEST(Foreach, InsideComptimeIfBranch) {
  /* 前脱糖先于 comptime 裁剪：两个分支都展开；丢弃分支只做语法检查 */
  expect_output(
      "comptime var N = 2;\n"
      "func main(): i32 {\n"
      "  var a = .[2]i32{ 3, 4 };\n"
      "  comptime if (N == 2) {\n"
      "    foreach (var x of a) { libc::printf(\"taken%d\\n\", x); }\n"
      "  } else {\n"
      "    foreach (var x of a) { libc::printf(\"dropped%d\\n\", x); }\n"
      "  }\n"
      "  return 0;\n"
      "}\n",
      "taken3\ntaken4\n");
}

TEST(Foreach, InsideLocalFunction) {
  /* 局部函数（语句级 func）体内的 foreach 同样展开（AST_FUNC_DEF 语句） */
  expect_output(
      "func main(): i32 {\n"
      "  func total(a: [3]i32): i32 {\n"
      "    var acc = 0;\n"
      "    foreach (var x of a) {\n"
      "      acc = acc + x;\n"
      "    }\n"
      "    return acc;\n"
      "  }\n"
      "  var a = .[3]i32{ 4, 5, 6 };\n"
      "  libc::printf(\"%d\\n\", total(a));\n"
      "  return 0;\n"
      "}\n",
      "15\n");
}

TEST(Foreach, InsideComptimeFunc) {
  /* comptime func 由 CTFE 解释执行——展开出的 for/len/下标也要能被解释 */
  expect_output(
      "comptime func sum_arr(): i32 {\n"
      "  var a = .[4]i32{ 1, 2, 3, 4 };\n"
      "  var s = 0;\n"
      "  foreach (var x of a) {\n"
      "    s = s + x;\n"
      "  }\n"
      "  return s;\n"
      "}\n"
      "comptime var TOTAL = sum_arr();\n"
      "func main(): i32 {\n"
      "  libc::printf(\"%d\\n\", TOTAL);\n"
      "  return 0;\n"
      "}\n",
      "10\n");
}

/* ================================================================ */
/* 负例                                                             */
/* ================================================================ */

TEST(Foreach, NonSequenceIterableRejected) {
  /* `foreach (x of 5)` → 展开成 `len(5)`，sema 报错（不是内部崩溃） */
  const std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  foreach (x of 5) { }\n"
      "  return 0;\n"
      "}\n",
      "foreach_bad_seq");
  std::string err;
  const int rc = clux_run_capture(path, &err);
  EXPECT_EQ(rc, 1);
  EXPECT_NE(err.find("len: argument must be array/slice/str"),
            std::string::npos)
      << err;
}

TEST(Foreach, UndefinedIterationVariableRejected) {
  const std::string path = write_temp_file(
      "func main(): i32 {\n"
      "  var a = .[2]i32{ 1, 2 };\n"
      "  foreach (nope of a) { }\n"
      "  return 0;\n"
      "}\n",
      "foreach_bad_var");
  std::string err;
  const int rc = clux_run_capture(path, &err);
  EXPECT_EQ(rc, 1);
  EXPECT_NE(err.find("undefined variable 'nope'"), std::string::npos) << err;
}
