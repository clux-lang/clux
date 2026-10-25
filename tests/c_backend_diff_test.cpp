/* C 后端与 VM 的差分测试。
 *
 * 同一个 `.cx` 程序分别用：
 *   - `clux run`（VM 后端，main 的整数返回值即进程退出码）；
 *   - `clux build` + 生成的可执行文件（C 后端）
 * 运行，要求两者的退出码都与期望值一致。任何语义分歧都会表现为退出码不同。
 */
#include "test_common.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace {

std::string cli_path() {
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

std::string q(const std::string &s) { return "\"" + s + "\""; }

int run_cli(const std::vector<std::string> &args) {
  std::string inner = q(cli_path());
  for (const std::string &a : args) inner += " " + q(a);
#ifdef _WIN32
  const std::string cmd = "(" + inner + ")";
#else
  const std::string cmd = inner;
#endif
  std::fflush(stdout);
  std::fflush(stderr);
  int status = std::system(cmd.c_str());
#ifdef _WIN32
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

int run_exe(const std::string &exe) {
  std::string cmd = q(exe);
#ifdef _WIN32
  cmd = "(" + cmd + ")";
#endif
  std::fflush(stdout);
  std::fflush(stderr);
  int status = std::system(cmd.c_str());
#ifdef _WIN32
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

std::filesystem::path exe_of(const std::string &src) {
  std::filesystem::path exe(src);
#ifdef _WIN32
  exe.replace_extension(".exe");
#else
  exe.replace_extension();
#endif
  return exe;
}

/* VM 后端：`clux run <src>` 的退出码。 */
int run_vm(const std::string &src) {
  return run_cli({"run", std::filesystem::absolute(src).string()});
}

/* C 后端：构建并运行，返回可执行文件退出码。 */
int run_c(const std::string &src) {
  std::filesystem::path exe = exe_of(src);
  std::error_code ec;
  std::filesystem::remove(exe, ec);
  const int build_rc =
      run_cli({"build", std::filesystem::absolute(src).string()});
  if (build_rc != 0) return -1000 - build_rc; /* 构建失败：不会与真实退出码混淆 */
  if (!std::filesystem::exists(exe)) return -2000;
  const int rc = run_exe(exe.string());
  std::filesystem::remove(exe, ec);
  std::filesystem::remove(exe.string() + ".clux.c", ec);
  return rc;
}

struct DiffCase {
  const char *tag;
  const char *source;
  int expected;
};

void expect_diff(const DiffCase &c) {
  const std::string src = write_temp_file(c.source, c.tag);
  ASSERT_FALSE(src.empty());
  const int vm = run_vm(src);
  const int cc = run_c(src);
  EXPECT_EQ(vm, c.expected) << "VM backend exit code differs\n--- src ---\n"
                            << c.source;
  EXPECT_EQ(cc, c.expected) << "C backend exit code differs\n--- src ---\n"
                            << c.source;
  EXPECT_EQ(vm, cc) << "VM and C backends disagree\n--- src ---\n"
                    << c.source;
}

} // namespace

TEST(CBackendDiff, ScalarsAndControlFlow) {
  static const DiffCase c = {
      "diff_scalars",
      "func main(): i32 {\n"
      "  var x = (1 + 2) * 3;\n"
      "  var i = 0;\n"
      "  while (i < 4) {\n"
      "    x += i;\n"
      "    i += 1;\n"
      "  }\n"
      "  if (x == 15) { return 7; }\n"
      "  return 99;\n"
      "}\n",
      7,
  };
  expect_diff(c);
}

TEST(CBackendDiff, RecursionForAndDoWhile) {
  static const DiffCase c = {
      "diff_recursion",
      "func fact(n: i32): i32 {\n"
      "  if (n <= 1) { return 1; }\n"
      "  return n * fact(n - 1);\n"
      "}\n"
      "func main(): i32 {\n"
      "  var n = 0;\n"
      "  do { n += 1; } while (n < 3);\n"
      "  var s = 0;\n"
      "  for (var i = 0; i < fact(n); i = i + 1) { s += 1; }\n"
      "  return s;\n"
      "}\n",
      6,
  };
  expect_diff(c);
}

TEST(CBackendDiff, StructArrayPointer) {
  static const DiffCase c = {
      "diff_struct",
      "struct Point { x: i32; y: i32; }\n"
      "func sum(p: Point): i32 { return p.x + p.y; }\n"
      "func main(): i32 {\n"
      "  var p: Point = .Point{ .x = 2, .y = 3 };\n"
      "  p.x = 5;\n"
      "  var r: ref *Point = p.&;\n"
      "  r.y = 7;\n"
      "  var a: [3]i32 = .[3]i32{ 1, 2, 3 };\n"
      "  a[1] += 4;\n"
      "  return sum(p) + a[1];\n"
      "}\n",
      18,
  };
  expect_diff(c);
}

TEST(CBackendDiff, OwnershipAndSlices) {
  static const DiffCase c = {
      "diff_own",
      "struct Box { p: own *i32; }\n"
      "func main(): i32 {\n"
      "  var b: own *Box = new Box{ .p = new i32{7} };\n"
      "  var s: own []i32 = make(i32, 3, 1, 2, 3);\n"
      "  var t: ref []i32 = s[1:3];\n"
      "  return b.p.* + t[0] + t[1];\n"
      "}\n",
      12,
  };
  expect_diff(c);
}

TEST(CBackendDiff, StringsAndLength) {
  static const DiffCase c = {
      "diff_str",
      "func main(): i32 {\n"
      "  var a: str = \"abcdef\";\n"
      "  if (a != \"abcdef\") { return 50; }\n"
      "  var t: ref []const u8 = a[1:4];\n"
      "  if (len(a) != 6usize || len(t) != 3usize) { return 51; }\n"
      "  return (t[0] as i32) - 96 + (t[2] as i32) - 96;\n"
      "}\n",
      6,
  };
  expect_diff(c);
}

TEST(CBackendDiff, OptionUnionAndCUnion) {
  static const DiffCase c = {
      "diff_union",
      "cunion U { i: i32; f: f32; }\n"
      "union Shape { radius: i32; w: i32; }\n"
      "func main(): i32 {\n"
      "  var a: ?i32 = 5;\n"
      "  if (a == nil || a.! != 5) { return 60; }\n"
      "  var v = .U{ .i = 4 };\n"
      "  var s = .Shape{ .w = 2 };\n"
      "  if (s is w != true) { return 61; }\n"
      "  return a.! + v.i + s.w;\n"
      "}\n",
      11,
  };
  expect_diff(c);
}

TEST(CBackendDiff, FunctionValuesClosuresAndMethods) {  static const DiffCase c = {
      "diff_closure",
      "struct Point {\n"
      "  x: i32;\n"
      "  func plus(self: ref *Point, y: i32): i32 { return self.x + y; }\n"
      "}\n"
      "func apply[F](f: F, x: i32): i32 { return f(x); }\n"
      "func main(): i32 {\n"
      "  var p = .Point{ .x = 4 };\n"
      "  var a = 2;\n"
      "  var cap: func |a: i32| (i32) -> i32 =\n"
      "      func |a| (x: i32): i32 { return x + a; };\n"
      "  return p.plus(apply[func |a: i32| (i32) -> i32](cap, 3));\n"
      "}\n",
      9,
  };
  expect_diff(c);
}

/* 捕获可写 + 闭包拷贝的值语义：`var g = f` 必须深拷贝 env（VM 与 C 一致）。 */
TEST(CBackendDiff, ClosureCopyIsValueSemantic) {
  static const DiffCase c = {
      "diff_closure_copy",
      "func main(): i32 {\n"
      "  var f = func |(n: i32 = 0)| (): i32 { n = n + 1; return n; };\n"
      "  if (f() != 1) { return 1; }\n"
      "  var g = f;\n"
      "  if (g() != 2) { return 2; }\n"
      "  if (f() != 2) { return 3; }\n"
      "  if (g() != 3) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      0,
  };
  expect_diff(c);
}

/* 聚合内部闭包字段：struct 拷贝/赋值在 C（内联 env 字节拷贝）与 VM
 * （__deep_clone 深拷贝闭包槽位）都必须独立。 */
TEST(CBackendDiff, StructClosureCopyIsValueSemantic) {
  static const DiffCase c = {
      "diff_struct_closure_copy",
      "struct Holder { f: func |n: i32| () -> i32; }\n"
      "func main(): i32 {\n"
      "  var f = func |(n: i32 = 0)| (): i32 { n = n + 1; return n; };\n"
      "  var h = .Holder{ .f = f };\n"
      "  var h2 = h;\n"
      "  if (h.f() != 1) { return 1; }\n"
      "  if (h2.f() != 1) { return 2; }\n"
      "  if (h.f() != 2) { return 3; }\n"
      "  if (h2.f() != 2) { return 4; }\n"
      "  return 0;\n"
      "}\n",
      0,
  };
  expect_diff(c);
}

TEST(CBackendDiff, Bitfields) {  static const DiffCase c = {
      "diff_bitfield",
      "bitfield Flags: u8 { a: 3; b: 5; }\n"
      "bitfield Big: u8 { p: 4; q: 8; r: 5; }\n"
      "func id8(v: u8): u8 { return v; }\n"
      "func main(): i32 {\n"
      "  var f: Flags = .Flags{ .a = 1u8, .b = 3u8 };\n"
      "  f.a = 7u8;\n"
      "  f.b += 2u8;\n"
      "  var g: Big = .Big{ .p = id8(0xFu8), .q = id8(0xABu8), .r = id8(3u8) };\n"
      "  return (f.a as i32) + (f.b as i32) + (g.q as i32) - 0xAB - 12;\n"
      "}\n",
      0,
  };
  expect_diff(c);
}

TEST(CBackendDiff, MetadataGlobalConstants) {
  static const DiffCase c = {
      "diff_metadata",
      "[[metadata(\"name\", \"user\")]]\n"
      "[[metadata(\"level\", 3)]]\n"
      "struct User { }\n"
      "var n: str = get_metadata[User](\"name\");\n"
      "var lvl: i32 = get_metadata[User](\"level\");\n"
      "func main(): i32 {\n"
      "  if (n != \"user\") { return 70; }\n"
      "  return lvl + 10;\n"
      "}\n",
      13,
  };
  expect_diff(c);
}

TEST(CBackendDiff, TernaryAndSwitch) {
  static const DiffCase c = {
      "diff_ternary_switch",
      "func classify(s: str): i32 {\n"
      "  switch (s) {\n"
      "    (\"a\", \"b\") -> { return 1; }\n"
      "    (\"c\") -> { return 2; }\n"
      "    default -> { return 3; }\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (classify(\"a\") != 1 || classify(\"c\") != 2 || classify(\"z\") != 3)"
      " { return 80; }\n"
      "  var n = 0;\n"
      "  for (var i = 0; i < 10; i += 1) {\n"
      "    switch (i) {\n"
      "      (0) -> { continue; }\n"
      "      (5) -> { break; }\n"
      "      default -> { n += 1; }\n"
      "    }\n"
      "  }\n"
      "  var t = (n < 5) ? 10 : 20;\n"
      "  return n + t;\n"
      "}\n",
      14,
  };
  expect_diff(c);
}

TEST(CBackendDiff, EnumValues) {
  static const DiffCase c = {
      "diff_enum",
      "enum Color: i32 { Red = 1, Green = 2, Blue = 3 }\n"
      "enum Small: u8 { A = 0u8, B = 200u8 }\n"
      "struct Paint { color: Color; shade: i32; }\n"
      "var G: Color = Color::Blue;\n"
      "func name_of(c: Color): i32 {\n"
      "  switch (c) {\n"
      "    (Color::Red) -> { return 10; }\n"
      "    (Color::Green) -> { return 20; }\n"
      "    default -> { return 30; }\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var c: Color = Color::Green;\n"
      "  if (c != Color::Green) { return 1; }\n"
      "  if (G != Color::Blue) { return 2; }\n"
      "  var p: Paint = .Paint{ .color = Color::Red, .shade = 7 };\n"
      "  if (p.color != Color::Red) { return 3; }\n"
      "  var s: Small = Small::B;\n"
      "  if (s != Small::B) { return 4; }\n"
      "  if ((c as i32) != 2) { return 5; }\n"
      "  var arr: [2]Color = .[2]Color{ Color::Red, Color::Blue };\n"
      "  if (arr[1] != Color::Blue) { return 6; }\n"
      "  if (sizeof(Color) != 4usize) { return 7; }\n"
      "  return name_of(Color::Red) + name_of(Color::Green) + p.shade;\n"
      "}\n",
      10 + 20 + 7,
  };
  expect_diff(c);
}

TEST(CBackendDiff, GenericInstances) {
  static const DiffCase c = {
      "diff_generics",
      "struct Box[T] { v: T; }\n"
      "func id[T](x: T): T { return x; }\n"
      "func maxv[T](a: T, b: T): T { if (a > b) { return a; } return b; }\n"
      "func main(): i32 {\n"
      "  var x: i32 = id(40);\n"
      "  var b: Box[i32] = .Box[i32]{ .v = 2 };\n"
      "  return x + maxv(b.v, 1);\n"
      "}\n",
      42,
  };
  expect_diff(c);
}

TEST(CBackendDiff, ResultSugarAndOptionalFields) {
  static const DiffCase c = {
      "diff_result",
      "struct Result[T, E] {\n"
      "  good: bool;\n"
      "  val: ?T;\n"
      "  err: ?E;\n"
      "  func ok(self: ref *const Result[T, E]): bool { return self.good; }\n"
      "  func value(self: ref *const Result[T, E]): T { return self.val.!; }\n"
      "  func error(self: ref *const Result[T, E]): E { return self.err.!; }\n"
      "  func of_value(v: T): Result[T, E] {\n"
      "    return .Result[T, E]{ .good = true, .val = .?T{ v }, .err = nil };\n"
      "  }\n"
      "  func of_error(e: E): Result[T, E] {\n"
      "    return .Result[T, E]{ .good = false, .val = nil, .err = .?E{ e } };\n"
      "  }\n"
      "}\n"
      "func half(x: i32): Result[i32, str] {\n"
      "  if (x < 0) { return Result[i32, str]::of_error(\"neg\"); }\n"
      "  return Result[i32, str]::of_value(x / 2);\n"
      "}\n"
      "func use(x: i32): Result[i32, str] {\n"
      "  var v = half(x).?;\n"
      "  return Result[i32, str]::of_value(v + 1);\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a = use(8);\n"
      "  if (!a.ok()) { return 90; }\n"
      "  var b = half(0 - 1);\n"
      "  if (b.ok()) { return 91; }\n"
      "  return a.value() + 8;\n"
      "}\n",
      13,
  };
  expect_diff(c);
}

TEST(CBackendDiff, GenericStructInstanceMethods) {
  static const DiffCase c = {
      "diff_generic_methods",
      "struct Box[T] {\n"
      "  v: T;\n"
      "  func get(self: ref *Box[T]): T { return self.v; }\n"
      "  func mk(x: T): Box[T] { return .Box[T]{ .v = x }; }\n"
      "  func map[U](self: ref *Box[T], f: func(T) -> U): U { return f(self.v); }\n"
      "}\n"
      "func inc(x: i32): i32 { return x + 1; }\n"
      "func main(): i32 {\n"
      "  var b = Box[i32]::mk(20);\n"
      "  var o: Box[Box[i32]] = .Box[Box[i32]]{ .v = b };\n"
      "  return o.get().get() + b.map(inc);\n"
      "}\n",
      41,
  };
  expect_diff(c);
}

TEST(CBackendDiff, NonZeroMainResultPropagates) {
  static const DiffCase c = {
      "diff_exit42",
      "func main(): i32 { return 42; }\n",
      42,
  };
  expect_diff(c);
}
