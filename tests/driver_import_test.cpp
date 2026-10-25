#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <string>

extern "C" {
#include "driver/driver.h"

}

#include "test_common.h"

namespace {

/* 每个测试用独立的临时子目录：模块与被导入文件放在同一目录下，import 一律
   使用**相对路径**（`./mod`）——这是 M5 规定的导入形式（sema 拒绝非相对路径）。
   独立子目录保证同一测试二进制并发/重复运行时文件名不冲突。 */
class ImportCase {
public:
  explicit ImportCase(const std::string &name) {
    static int counter = 0;
    dir_ = clux_test_path() /
           ("clux_import_" + name + "_" + std::to_string(counter++));
    std::filesystem::create_directories(dir_);
  }
  ~ImportCase() { std::filesystem::remove_all(dir_); }

  /* 写文件，返回**相对被导入文件所在目录**的 import 路径（`./stem`）。
     注意 `\` 需转成 `/`——clux 字符串字面量中反斜杠是转义符。 */
  std::string write(const std::string &stem, const std::string &content) {
    auto p = dir_ / (stem + ".cx");
    FILE *fp = fopen(p.string().c_str(), "wb");
    const std::string src_ = clux_with_printf_prelude(content);
  fwrite(src_.data(), 1, src_.size(), fp);
    fclose(fp);
    return "./" + stem;
  }

  /* 入口文件路径（driver_run_file 用绝对路径，便于从任意工作目录运行） */
  std::string entry(const std::string &stem, const std::string &content) {
    auto p = dir_ / (stem + ".cx");
    FILE *fp = fopen(p.string().c_str(), "wb");
    const std::string src_ = clux_with_printf_prelude(content);
  fwrite(src_.data(), 1, src_.size(), fp);
    fclose(fp);
    std::string s = p.string();
    for (auto &c : s) if (c == '\\') c = '/';
    return s;
  }

  /* 子目录下的模块文件（用于 ../ 相对路径测试） */
  std::string write_sub(const std::string &sub, const std::string &stem,
                        const std::string &content) {
    std::filesystem::create_directories(dir_ / sub);
    auto p = dir_ / sub / (stem + ".cx");
    FILE *fp = fopen(p.string().c_str(), "wb");
    const std::string src_ = clux_with_printf_prelude(content);
  fwrite(src_.data(), 1, src_.size(), fp);
    fclose(fp);
    std::string s = (dir_ / sub / (stem + ".cx")).string();
    for (auto &c : s) if (c == '\\') c = '/';
    return s;
  }

  const std::filesystem::path &dir() const { return dir_; }

private:
  std::filesystem::path dir_;
};

} // namespace

/* ================================================================
 * M5 模块系统端到端：import（相对路径）+ :: 模块成员访问
 * ================================================================ */

TEST(DriverImport, ImportExportedFunc) {
  /* 被导入模块导出函数，主模块通过 alias::func 调用 */
  ImportCase c("func");
  std::string mod = c.write("math", "export func add(a: i32, b: i32): i32 { return a + b; }\n");
  std::string main_path = c.entry("main",
      "import math from \"" + mod + "\";\n"
      "func main(): void {\n"
      "  var x = math::add(3, 4);\n"
      "  _ = libc::printf(\"%d\\n\", x);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ImportExportedVar) {
  /* 被导入模块导出全局变量，主模块通过 alias::var 读取 */
  ImportCase c("var");
  std::string mod = c.write("math", "export var PI: i32 = 42;\n");
  std::string main_path = c.entry("main",
      "import math from \"" + mod + "\";\n"
      "func main(): void {\n"
      "  var p = math::PI;\n"
      "  _ = libc::printf(\"%d\\n\", p);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ImportMixedExports) {
  /* 同时导出函数和变量 */
  ImportCase c("mixed");
  std::string mod = c.write("math",
      "export var PI: i32 = 42;\n"
      "export func add(a: i32, b: i32): i32 { return a + b; }\n");
  std::string main_path = c.entry("main",
      "import math from \"" + mod + "\";\n"
      "func main(): void {\n"
      "  var x = math::add(3, 4);\n"
      "  var p = math::PI;\n"
      "  _ = libc::printf(\"%d %d\\n\", x, p);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ImportComptimeFoldedExports) {
  /* comptime var / comptime func 折叠结果通过 export var 导出。
     - comptime var BASE = 100 → 折叠到 export var COMPUTED = BASE + 50 = 150
     - comptime func helper(7) = 21 → 折叠到 export var HELPED = 21 */
  ImportCase c("comptime");
  std::string mod = c.write("ct",
      "comptime var BASE = 100;\n"
      "export var COMPUTED: i32 = BASE + 50;\n"
      "comptime func helper(x: i32): i32 { return x * 3; }\n"
      "export var HELPED: i32 = helper(7);\n");
  std::string main_path = c.entry("main",
      "import ct from \"" + mod + "\";\n"
      "func main(): void {\n"
      "  var x = ct::COMPUTED;\n"
      "  var h = ct::HELPED;\n"
      "  _ = libc::printf(\"%d %d\\n\", x, h);\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

/* ================================================================
 * M5 收尾：相对路径解析 / 扩展名省略 / 归一化 / 父目录 / 代理导出 / 诊断
 * ================================================================ */

TEST(DriverImport, RelativePathResolvedAgainstImporterDir) {
  /* 核心契约：`./math` 相对**被导入文件所在目录**解析（而非进程工作目录）。
     入口文件位于临时目录而进程 CWD 是仓库根，修复前会 "cannot open file './math'"。 */
  ImportCase c("reldir");
  std::string mod = c.write("math", "export func add(a: i32, b: i32): i32 { return a + b; }\n");
  ASSERT_EQ(mod, "./math");
  std::string main_path = c.entry("main",
      "import math from \"./math\";\n"
      "func main(): void { _ = libc::printf(\"%d\\n\", math::add(20, 22)); }\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ExtensionAppendedWhenOmitted) {
  /* 省略 `.cx` 自动补全；显式写 `.cx` 亦可（两种写法指向同一模块） */
  ImportCase c("ext");
  c.write("math", "export var V: i32 = 7;\n");
  std::string main_path = c.entry("main",
      "import m1 from \"./math\";\n"
      "import m2 from \"./math.cx\";\n"
      "func main(): void { _ = libc::printf(\"%d %d\\n\", m1::V, m2::V); }\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ParentRelativePath) {
  /* `../x`：子目录中的模块引用上级目录的模块 */
  ImportCase c("parent");
  std::string sub_entry = c.write_sub("sub", "inner",
      "import math from \"../math\";\n"
      "func main(): void { _ = libc::printf(\"%d\\n\", math::add(1, 2)); }\n");
  c.write("math", "export func add(a: i32, b: i32): i32 { return a + b; }\n");
  EXPECT_EQ(driver_run_file(sub_entry.c_str()), 0);
}

TEST(DriverImport, PathNormalizationCollapsesDotSegments) {
  /* 路径归一化：`./sub/../math` ≡ `./math`（分隔符统一 + `.`/`..` 折叠），
     保证规范路径作为模块 key 稳定（同一文件不被重复编译）。 */
  ImportCase c("norm");
  c.write_sub("sub", "placeholder", "");
  c.write("math", "export var V: i32 = 9;\n");
  std::string main_path = c.entry("main",
      "import a from \"./math\";\n"
      "import b from \"./sub/../math\";\n"
      "func main(): void { _ = libc::printf(\"%d %d\\n\", a::V, b::V); }\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ReexportForwardsDependencyExports) {
  /* `export * from "./math"` 代理导出：转发方自身不定义符号，使用方仍可
     通过转发方命名空间访问到被代理模块的导出（含函数与变量）。 */
  ImportCase c("reexport");
  c.write("math",
      "export func add(a: i32, b: i32): i32 { return a + b; }\n"
      "export var PI: i32 = 42;\n");
  c.write("api", "export * from \"./math\";\n");
  std::string main_path = c.entry("main",
      "import api from \"./api\";\n"
      "func main(): void { _ = libc::printf(\"%d %d\\n\", api::add(20, 22), api::PI); }\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ReexportChainForwardsThroughTwoLevels) {
  /* 链式代理：a → b → c，最终使用方经 a 访问 c 的导出符号 */
  ImportCase c("chain");
  c.write("c", "export func deep(): i32 { return 5; }\n");
  c.write("b", "export * from \"./c\";\n");
  c.write("a", "export * from \"./b\";\n");
  std::string main_path = c.entry("main",
      "import a from \"./a\";\n"
      "func main(): void { _ = libc::printf(\"%d\\n\", a::deep()); }\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ReexportConflictIsRejected) {
  /* 代理导出与本模块已有导出同名 → 编译错误（符号冲突，m5-design §1.3） */
  ImportCase c("conflict");
  c.write("math", "export var V: i32 = 1;\n");
  c.write("api",
      "export var V: i32 = 2;\n"
      "export * from \"./math\";\n");
  std::string main_path = c.entry("main",
      "import api from \"./api\";\n"
      "func main(): void { _ = libc::printf(\"%d\\n\", api::V); }\n");
  EXPECT_NE(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, NonRelativePathRejected) {
  /* 非相对路径（既非 ./ 也非 ../）→ 统一诊断；**不按扩展名猜类型**
     （`.so` / `.dll` 等后缀不表示文件类型，因此不做"动态库"特判）。 */
  ImportCase c("nonrel");
  std::string main_path = c.entry("main",
      "import libc from \"libc.so.6\";\n"
      "func main(): void {}\n");
  EXPECT_NE(driver_run_file(main_path.c_str()), 0);

  std::string pkg = c.entry("main_pkg",
      "import std from \"std\";\n"
      "func main(): void {}\n");
  EXPECT_NE(driver_run_file(pkg.c_str()), 0);
}

TEST(DriverImport, ModuleNotFoundDiagnostic) {
  /* 相对路径但文件不存在 → 编译错误（module not found），而非静默成功 */
  ImportCase c("missing");
  std::string main_path = c.entry("main",
      "import nope from \"./nope\";\n"
      "func main(): void {}\n");
  EXPECT_NE(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, CircularDependencyRejected) {
  /* 循环依赖：a → b → a，编译期报错并给出依赖链 */
  ImportCase c("cycle");
  c.write("a",
      "import b from \"./b\";\n"
      "export var VA: i32 = 1;\n");
  c.write("b",
      "import a from \"./a\";\n"
      "export var VB: i32 = 2;\n");
  std::string main_path = c.entry("main",
      "import a from \"./a\";\n"
      "func main(): void { _ = libc::printf(\"%d\\n\", a::VA); }\n");
  EXPECT_NE(driver_run_file(main_path.c_str()), 0);
}

/* ================================================================
 * 回归：program 类型 id 必须全 VM 唯一（跨模块）
 *
 * 缺陷：`sema_type_register` 曾按 `TYPE_ID_PROGRAM_BASE + 模块内下标` 分配 id，
 * 而运行期 `vm->types_by_id` 是**全 VM 单表**（多模块共享同一 vm），且模块字节码
 * 用 `LOAD_TYPE <id>` 直接索引。两模块 id 空间重叠 ⇒ 后编译模块的 `DEFINE_TYPE`
 * 覆盖先者槽位 ⇒ 先者函数体的 `LOAD_TYPE` 取到**别的模块的类型**。
 * 症状：被导入模块内的 struct 构造运行期报
 *       `construct: unsupported type (only array, optional, struct, tuple, union
 *       and cunion implemented)`。
 * 数组字面量当时不受影响（走 PUSH_ARRAY 内联重建类型、不查 id 表），故必须用
 * struct/union/enum 这类走 LOAD_TYPE 的类型覆盖。
 * ================================================================ */

TEST(DriverImport, StructConstructInsideDependencyModule) {
  ImportCase c("depstruct");
  std::string mod = c.write("geom",
      "struct P { x: i32; y: i32; }\n"
      "export func f(): i32 {\n"
      "  var p: P = .P{ .x = 1, .y = 2 };\n"
      "  return p.x + p.y;\n"
      "}\n");
  std::string main_path = c.entry("main",
      "import g from \"" + mod + "\";\n"
      "func main(): i32 {\n"
      "  if (g::f() != 3) { return 1; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, ProgramTypeIdsUniqueAcrossModules) {
  /* 两个依赖模块各有 struct/union/enum/数组，入口模块自己也定义 struct：
     id 若按模块重复起算会互相覆盖，此处三个模块的类型都必须正确解析。 */
  ImportCase c("typeids");
  std::string m1 = c.write("m1",
      "struct P { x: i32; y: i32; }\n"
      "union U { a: i32; b: i32; }\n"
      "export func f1(): i32 {\n"
      "  var p: P = .P{ .x = 1, .y = 2 };\n"
      "  var u: U = .U{ .a = 3 };\n"
      "  var arr: [2]i32 = .[2]i32{ 4, 5 };\n"
      "  return p.x + p.y + u.a + arr[0] + arr[1];\n" /* 15 */
      "}\n");
  std::string m2 = c.write("m2",
      "enum E:i32 { A = 0, B = 1, C = 2 }\n"
      "struct Q { v: i32; }\n"
      "export func f2(): i32 {\n"
      "  var q: Q = .Q{ .v = 10 };\n"
      "  var e: E = E::B;\n"
      "  return q.v + (e as i32);\n" /* 11 */
      "}\n");
  std::string main_path = c.entry("main",
      "import a from \"" + m1 + "\";\n"
      "import b from \"" + m2 + "\";\n"
      "struct M { z: i32; }\n"
      "func main(): i32 {\n"
      "  var m: M = .M{ .z = 100 };\n"
      "  if (m.z != 100) { return 1; }\n"
      "  if (a::f1() != 15) { return 2; }\n"
      "  if (b::f2() != 11) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

/* ================================================================
 * M7 P7：跨模块泛型实例化
 * ================================================================ */

TEST(DriverImport, CrossModuleGenericTypeFunctionAlias) {
  ImportCase c("generic_tfa");
  std::string mod = c.write("glib",
      "export struct Box[T] { value: T; }\n"
      "export func id[T](x: T): T { return x; }\n"
      "export type Pair[T] = <T, T>;\n");
  std::string main_path = c.entry("main",
      "import glib from \"" + mod + "\";\n"
      "func main(): i32 {\n"
      "  var b: glib::Box[i32] = .{ .value = 5 };\n"
      "  var x = glib::id[i32](7);\n"
      "  var p: glib::Pair[i32] = .<i32, i32>{ 1, 2 };\n"
      "  return b.value + x + p[0] + p[1] - 15;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, CrossModuleGenericMethod) {
  ImportCase c("generic_method");
  std::string mod = c.write("glib",
      "export struct Box[T] {\n"
      "  value: T;\n"
      "  func get(self: ref *const Box[T]): T { return self.value; }\n"
      "  func map_into[U](self: ref *const Box[T], f: func(T) -> U): Box[U] {\n"
      "    var b: Box[U] = .{ .value = f(self.value) };\n"
      "    return b;\n"
      "  }\n"
      "}\n");
  std::string main_path = c.entry("main",
      "import glib from \"" + mod + "\";\n"
      "func main(): i32 {\n"
      "  var b: glib::Box[i32] = .{ .value = 5 };\n"
      "  var c = b.map_into(func(v: i32): i32 { return v + 1; });\n"
      "  return b.get() + c.get() - 11;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, CrossModuleGenericSameNameIsolated) {
  ImportCase c("generic_dup");
  std::string m1 = c.write("l1",
      "export struct Box[T] { value: T; func get(self: ref *const Box[T]): T { return self.value; } }\n");
  std::string m2 = c.write("l2",
      "export struct Box[T] { value: T; func get(self: ref *const Box[T]): T { return self.value + 100; } }\n");
  std::string main_path = c.entry("main",
      "import l1 from \"" + m1 + "\";\n"
      "import l2 from \"" + m2 + "\";\n"
      "func main(): i32 {\n"
      "  var a: l1::Box[i32] = .{ .value = 5 };\n"
      "  var b: l2::Box[i32] = .{ .value = 1 };\n"
      "  return a.get() + b.get() - 106;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, CrossModuleGenericReexport) {
  ImportCase c("generic_reexport");
  std::string lib = c.write("glib",
      "export struct Box[T] { value: T; func get(self: ref *const Box[T]): T { return self.value; } }\n");
  std::string api = c.write("gapi",
      "export * from \"" + lib + "\";\n");
  std::string main_path = c.entry("main",
      "import api from \"" + api + "\";\n"
      "func main(): i32 {\n"
      "  var b: api::Box[i32] = .{ .value = 12 };\n"
      "  return b.get() - 12;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

/* ================================================================
 * M7 归属：跨模块泛型实例按定义模块解析全局/私有/import 别名
 * ================================================================ */

TEST(DriverImport, CrossModuleGenericUsesDefinitionGlobals) {
  ImportCase c("generic_owner_globals");
  std::string mod = c.write("glib",
      "export var base: i32 = 100;\n"
      "export func add_base[T](x: T): i32 { return base + x as i32; }\n"
      "export struct Counter[T] {\n"
      "  value: T;\n"
      "  func bump(self: ref *Counter[T]): i32 { base = base + 1; return base; }\n"
      "}\n");
  std::string main_path = c.entry("main",
      "import gl from \"" + mod + "\";\n"
      "var base: i32 = 5;\n"
      "func main(): i32 {\n"
      "  var a = gl::add_base[i32](1);\n"
      "  if (a != 101) { return 1; }\n"
      "  var c: gl::Counter[i32] = .{ .value = 1 };\n"
      "  var b = c.bump();\n"
      "  if (b != 102) { return 2; }\n"
      "  if (gl::base != 102) { return 3; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, CrossModuleGenericUsesDefinitionImportAndPrivate) {
  ImportCase c("generic_owner_env");
  std::string helper = c.write("helper",
      "export func inc(x: i32): i32 { return x + 1; }\n");
  std::string mod = c.write("glib",
      "import helper from \"" + helper + "\";\n"
      "func private_fn(): i32 { return 7; }\n"
      "export func call_helper[T](x: T): i32 { return helper::inc(41); }\n"
      "export func call_private[T](x: T): i32 { return private_fn(); }\n");
  std::string main_path = c.entry("main",
      "import gl from \"" + mod + "\";\n"
      "func main(): i32 {\n"
      "  if (gl::call_helper[i32](1) != 42) { return 1; }\n"
      "  if (gl::call_private[i32](1) != 7) { return 2; }\n"
      "  return 0;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}

TEST(DriverImport, CrossModuleGenericGlobalsCxb) {
  ImportCase c("generic_owner_cxb");
  std::string mod = c.write("glib",
      "export var base: i32 = 100;\n"
      "export func add_base[T](x: T): i32 { return base + x as i32; }\n");
  std::string main_path = c.entry("main",
      "import gl from \"" + mod + "\";\n"
      "func main(): i32 { return gl::add_base[i32](1) - 101; }\n");
  auto bin = (c.dir() / "prog.cxb").string();
  ASSERT_EQ(driver_build_bin(main_path.c_str(), bin.c_str()), 0);
  std::remove(main_path.c_str());
  std::remove((c.dir() / "glib.cx").string().c_str());
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);
}

TEST(DriverImport, CrossModuleInterfaceImplementsAndConstraint) {
  ImportCase c("cross_interface");
  std::string mod = c.write("iface",
      "export interface IW[T] {\n"
      "  var fd: i32;\n"
      "  func w(self: ref *T);\n"
      "}\n");
  std::string main_path = c.entry("main",
      "import iface from \"" + mod + "\";\n"
      "func use_cross[T extends iface::IW[T]](x: T): i32 { return x.fd; }\n"
      "struct S implements iface::IW[S] {\n"
      "  fd: i32;\n"
      "  func w(self: ref *S): void {}\n"
      "}\n"
      "func main(): i32 {\n"
      "  var s: S = .S{ .fd = 7 };\n"
      "  s.w();\n"
      "  return use_cross[S](s) - 7;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);

  auto bin = (c.dir() / "prog.cxb").string();
  ASSERT_EQ(driver_build_bin(main_path.c_str(), bin.c_str()), 0);
  std::remove(main_path.c_str());
  std::remove((c.dir() / "iface.cx").string().c_str());
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);
}

TEST(DriverImport, CrossModuleGenericMethodExplicitTypeArgs) {
  ImportCase c("generic_method_explicit");
  std::string mod = c.write("gbox",
      "export struct Box[T] {\n"
      "  value: T;\n"
      "  func map_into[U](self: ref *const Box[T], f: func(T) -> U): Box[U] {\n"
      "    var b: Box[U] = .{ .value = f(self.value) };\n"
      "    return b;\n"
      "  }\n"
      "  func get(self: ref *const Box[T]): T { return self.value; }\n"
      "}\n");
  std::string main_path = c.entry("main",
      "import gb from \"" + mod + "\";\n"
      "func main(): i32 {\n"
      "  var b: gb::Box[i32] = .{ .value = 5 };\n"
      "  var c = b.map_into[i32](func(v: i32): i32 { return v + 1; });\n"
      "  return c.get() - 6;\n"
      "}\n");
  EXPECT_EQ(driver_run_file(main_path.c_str()), 0);
}