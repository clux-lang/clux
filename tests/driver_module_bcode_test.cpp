#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "driver/driver.h"
#include "vm/bcode_asm.h"
#include "vm/bcode_serial.h"

}

#include "test_common.h"

namespace {


std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/* 三个模块的最小 M5 程序：main → api（代理导出）→ math（真正定义）。
   `add`/`PI` 的值归 math.cx，api.cx 只做 `export *` 转发。 */
struct MultiModuleProgram {
  std::string math, api, main;
};

MultiModuleProgram make_program(const std::string &dir_hint) {
  MultiModuleProgram p;
  p.math = dir_hint + "math.cx";
  p.api = dir_hint + "api.cx";
  p.main = dir_hint + "main.cx";
  FILE *f = fopen(p.math.c_str(), "wb");
  fputs("export func add(a: i32, b: i32): i32 { return a + b; }\n"
        "export var PI: i32 = 42;\n", f);
  fclose(f);
  f = fopen(p.api.c_str(), "wb");
  fputs("export * from \"./math\";\n", f);
  fclose(f);
  f = fopen(p.main.c_str(), "wb");
  fputs("import api from \"./api\";\n"
        "func main(): i32 {\n"
        "  if (api::add(1, 2) != 3) { return 1; }\n"
        "  if (api::PI != 42) { return 2; }\n"
        "  return 0;\n"
        "}\n", f);
  fclose(f);
  return p;
}

} // namespace

/* ================================================================
 * M5 模块系统 × .cxb 容器：字节码层闭环
 *
 * 修复前：`bc emit` 只写入口模块的字节码，`.cxb` 里没有依赖模块 →
 * `run x.cxb` 在 IMPORT 指令处报 "module not found"。M5 的模块语义因此在
 * 字节码层断裂（源码运行正常、字节码运行失败）。
 * 现在容器 v2 携带整张模块表（路径 + 字节码 + 导出表 (name, owner_index)），
 * 加载方按依赖序重建模块图，与源码路径行为一致。
 * ================================================================ */

TEST(DriverModuleBcode, EmittedCxbEmbedsDependenciesAndRuns) {
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir()) / "clux_mbc_prog";
  std::filesystem::create_directories(dir);
  std::string hint = (dir / "").string();
  MultiModuleProgram p = make_program(hint);
  std::string bin = hint + "prog.cxb";

  /* 源码路径基线 */
  EXPECT_EQ(driver_run_file(p.main.c_str()), 0);
  /* emit 后字节码路径必须与源码路径一致（依赖模块随容器携带） */
  EXPECT_EQ(driver_build_bin(p.main.c_str(), bin.c_str()), 0);
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);

  std::remove(p.math.c_str());
  std::remove(p.api.c_str());
  std::remove(p.main.c_str());
  std::remove(bin.c_str());
}

TEST(DriverModuleBcode, DisasmAsmRoundTripIsByteIdentical) {
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir()) / "clux_mbc_rt";
  std::filesystem::create_directories(dir);
  std::string hint = (dir / "").string();
  MultiModuleProgram p = make_program(hint);
  std::string bin = hint + "a.cxb", cxs = hint + "a.cxs", bin2 = hint + "b.cxb";

  ASSERT_EQ(driver_build_bin(p.main.c_str(), bin.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(bin.c_str(), cxs.c_str()), 0);

  /* 多模块文本带 MODULE 指令与导出表：3 个模块 → 指令行 MODULE/export */
  std::string text = read_file(cxs);
  EXPECT_NE(text.find("MODULE"), std::string::npos);
  EXPECT_NE(text.find("export add 0"), std::string::npos); /* 代理导出归 math.cx */
  EXPECT_NE(text.find("export PI 0"), std::string::npos);

  ASSERT_EQ(driver_asm_to_bin(cxs.c_str(), bin2.c_str()), 0);
  EXPECT_EQ(read_file(bin), read_file(bin2)); /* 字节一致 */
  EXPECT_EQ(driver_run_bin(bin2.c_str()), 0); /* 往返产物仍可执行 */

  std::remove(p.math.c_str());
  std::remove(p.api.c_str());
  std::remove(p.main.c_str());
  std::remove(bin.c_str());
  std::remove(cxs.c_str());
  std::remove(bin2.c_str());
}

TEST(DriverModuleBcode, SingleModuleTextStaysDirectiveFree) {
  /* 单模块产物：文本与旧格式逐字一致（无 MODULE/export 指令行），
     examples/asm 的 5 个手写 .cxs 样本因此继续可用。 */
  std::string src = write_temp_file(
      "func main(): i32 { return 7 - 7; }\n", "clux_mbc_one");
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir());
  std::string bin = (dir / "clux_mbc_one.cxb").string();
  std::string cxs = (dir / "clux_mbc_one.cxs").string();

  ASSERT_EQ(driver_build_bin(src.c_str(), bin.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(bin.c_str(), cxs.c_str()), 0);
  std::string text = read_file(cxs);
  EXPECT_EQ(text.find("MODULE"), std::string::npos);
  EXPECT_EQ(text.find("export "), std::string::npos);
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);

  std::remove(src.c_str());
  std::remove(bin.c_str());
  std::remove(cxs.c_str());
}

TEST(DriverModuleBcode, LegacyV1ContainerStillRuns) {
  /* v1 单模块容器（旧 bcode_serial 产物）必须继续可运行：加载方按
     "单模块、空路径、无导出" 兼容读取。这里真的用旧 API 产出 v1 文件：
     源码 → v2 容器 → 反汇编成文本 → **旧汇编器 + 旧序列化器** → v1 容器。 */
  std::string src = write_temp_file(
      "func main(): i32 { return 3 - 3; }\n", "clux_mbc_v1");
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir());
  std::string v2 = (dir / "clux_mbc_v1.v2.cxb").string();
  std::string cxs = (dir / "clux_mbc_v1.cxs").string();
  std::string v1 = (dir / "clux_mbc_v1.v1.cxb").string();

  ASSERT_EQ(driver_build_bin(src.c_str(), v2.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(v2.c_str(), cxs.c_str()), 0);

  allocator_t *alloc = create_allocator(malloc, free);
  ASSERT_NE(alloc, nullptr);
  bytecode_t *bc = NULL;
  ASSERT_EQ(bcode_asm_from_file(alloc, cxs.c_str(), &bc), 0);
  ASSERT_EQ(bcode_serial(bc, v1.c_str()), 0); /* 旧 API：写 v1 单模块格式 */
  bcode_destroy(&bc);
  delete_allocator(&alloc);

  EXPECT_EQ(driver_run_bin(v1.c_str()), 0);

  std::remove(src.c_str());
  std::remove(v2.c_str());
  std::remove(cxs.c_str());
  std::remove(v1.c_str());
}

TEST(DriverModuleBcode, CyclicImportRejectedBeforeEmit) {
  /* 循环依赖在编译期拒绝（不会产出可执行容器） */
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir()) / "clux_mbc_cycle";
  std::filesystem::create_directories(dir);
  std::string hint = (dir / "").string();
  std::string a = hint + "a.cx", b = hint + "b.cx";
  FILE *f = fopen(a.c_str(), "wb");
  fputs("import bb from \"./b\";\n"
        "export func fa(): i32 { return 1; }\n"
        "func main(): i32 { return 0; }\n", f);
  fclose(f);
  f = fopen(b.c_str(), "wb");
  fputs("import aa from \"./a\";\n"
        "export func fb(): i32 { return 2; }\n", f);
  fclose(f);

  EXPECT_NE(driver_build_bin(a.c_str(), (hint + "a.cxb").c_str()), 0);

  std::remove(a.c_str());
  std::remove(b.c_str());
  std::remove((hint + "a.cxb").c_str());
}

/* ================================================================
 * M6 FFI：extern lib 语法定稿 + Phase 2 前端落地
 *
 * 判据是关键字（`extern lib`），**路径只是 dlopen 参数**——绝不按 .so/.dll/
 * .dylib 后缀判类（后缀不代表文件类型）。`import` 语义保持 M5 不变。
 * Phase 2 完成前端（lexer/parser/AST/fmt）；运行期（dlopen/dlsym）尚未接入，
 * 因此 sema 必须给出**明确诊断**而不是静默放行。
 * ================================================================ */

/* extern lib 现在**真的加载**动态库（driver 在 lexer pool 释放前扫描 AST →
   ffi_lib_open → 句柄挂 module_t.ffi，模块销毁时关闭）。判据是关键字，路径只是
   加载参数；能否加载由操作系统按内容判定（不按后缀）。 */
#if defined(_WIN32)
const char *kDriverSystemLib = "msvcrt.dll";
#elif defined(__APPLE__)
const char *kDriverSystemLib = "libSystem.B.dylib";
#else
const char *kDriverSystemLib = "libc.so.6";
#endif

TEST(DriverM6, ExternLibLoadsRealLibrary) {
  std::string src = std::string("extern lib \"") + kDriverSystemLib +
                    "\" as crt;\nfunc main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "clux_m6_lib");
  EXPECT_EQ(driver_run_file(path.c_str()), 0); /* 加载成功且无泄漏 */
  std::remove(path.c_str());
}

TEST(DriverM6, ExternLibMissingLibraryIsDiagnosed) {
  std::string path = write_temp_file(
      "extern lib \"definitely_no_such_library_xyz\" as bad;\n"
      "func main(): i32 { return 0; }\n", "clux_m6_missing");
  /* 加载失败 → 诊断（文案来自操作系统），不得静默放行 */
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* Phase 3 起支持**库表**：多库（不同别名）允许；同别名重复则诊断。 */
TEST(DriverM6, TwoExternLibsWithDistinctAliasesAreAllowed) {
  std::string src = std::string("extern lib \"") + kDriverSystemLib +
                    "\" as a;\n" + "extern lib \"" + kDriverSystemLib +
                    "\" as b;\nfunc main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "clux_m6_two");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM6, DuplicateExternLibAliasIsDiagnosed) {
  std::string src = std::string("extern lib \"") + kDriverSystemLib +
                    "\" as a;\n" + "extern lib \"" + kDriverSystemLib +
                    "\" as a;\nfunc main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "clux_m6_dup");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM6, ExternCfuncMissingSymbolIsDiagnosed) {
  std::string src = std::string("extern lib \"") + kDriverSystemLib +
                    "\" as crt;\n" +
                    "extern cfunc crt::clux_no_such_symbol_xyz(n: u64): opaque;\n"
                    "func main(): i32 { return 0; }\n";
  std::string path = write_temp_file(src, "clux_m6_sym");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* dlsym 解析失败 → 诊断 */
  std::remove(path.c_str());
}

TEST(DriverM6, ExternLibRequiresAsClause) {
  std::string path = write_temp_file(
      "extern lib \"libc.so.6\";\n"
      "func main(): i32 { return 0; }\n", "clux_m6_bad");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* 语法错误：缺 as <alias> */
  std::remove(path.c_str());
}

TEST(DriverM6, ExternRequiresLibKeyword) {
  std::string path = write_temp_file(
      "extern func f(): void;\n"
      "func main(): i32 { return 0; }\n", "clux_m6_bad2");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* extern 后必须是 lib（cfunc 待 Phase 2b） */
  std::remove(path.c_str());
}

/* ---- M6 Phase 2b：extern cfunc 声明（无体，dlsym 在 Phase 3/4 接入）---- */

TEST(DriverM6, ExternCfuncParsesAndIsDiagnosed) {
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "extern cfunc crt::malloc(size: u64): opaque;\n"
      "func main(): i32 { return 0; }\n", "clux_m6_ecf");
  /* M6 打通后：仅**声明** extern cfunc 合法（占位"未实现"诊断已移除）；调用由 SourceLevelFfiCall* 覆盖 */
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM6, ExternCfuncRequiresQualifiedName) {
  std::string path = write_temp_file(
      "extern cfunc crt.malloc(size: u64): opaque;\n", "clux_m6_ecf2");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* 缺 :: → 语法错 */
  std::remove(path.c_str());
}

TEST(DriverM6, ExternCfuncRejectsCaptureList) {
  std::string path = write_temp_file(
      "extern cfunc crt::f|a|(x: i32): i32;\n", "clux_m6_ecf3");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* C ABI 无闭包 */
  std::remove(path.c_str());
}

/* ---- M6：`...` 作为**三字符 token**（lexer 合成）+ 只允许出现在 extern cfunc ---- */

TEST(DriverM6, VariadicExternCfuncIsParsedNotSyntaxError) {
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "extern cfunc crt::printf(format: str, ...): i32;\n"
      "func main(): i32 { return 0; }\n", "clux_m6_var");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

TEST(DriverM6, VariadicRejectedInNormalFunc) {
  std::string path = write_temp_file(
      "func f(a: i32, ...): i32 { return a; }\n"
      "func main(): i32 { return 0; }\n", "clux_m6_var2");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* C 可变参数只属于 extern cfunc */
  std::remove(path.c_str());
}

/* ---- M6：FFI 调用点诊断（把含混错误变成明确诊断）----
 *
 * 别名注册为 SEMA_SYM_EXTERN_LIB 后，`crt::malloc(16)` 不再掉进枚举 variant 路径
 * 报 "unknown enum type in enum reference"，而是给出
 * "FFI call 'crt::malloc' is not implemented yet (M6 Phase 4/5: ...)"。
 */
TEST(DriverM6, FfiCallSiteGetsExplicitNotImplementedDiagnostic) {
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "extern cfunc crt::malloc(size: u64): opaque;\n"
      "func main(): i32 {\n"
      "  var p: opaque = crt::malloc(16);\n"
      "  return 0;\n"
      "}\n", "clux_m6_callsite");
  /* M6 打通后：调用点不再报"未实现"，源码级调用由 SourceLevelFfiCall* 覆盖 */
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ---- M6 §2：extern cfunc 签名必须是 C ABI（切片/option 等一律拒绝）---- */
TEST(DriverM6, ExternCfuncNonCApiSignatureRejected) {
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "extern cfunc crt::f(s: fatal []i32): i32;\n"
      "func main(): i32 { return 0; }\n", "clux_m6_cabi");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* 切片：胖指针，非 C ABI */
  std::remove(path.c_str());
}

TEST(DriverM6, ExternCfuncOptionReturnRejected) {
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "extern cfunc crt::g(x: i32): ?i32;\n"
      "func main(): i32 { return 0; }\n", "clux_m6_cabi2");
  EXPECT_NE(driver_run_file(path.c_str()), 0); /* option：无 C 表示 */
  std::remove(path.c_str());
}

/* ---- M6：EXTERN_CALL 的三操作数（STR + U32 + U32）在 .cxs 往返中稳定 ----
 *
 * 现有 examples/asm 样本都不含该指令，属真实覆盖缺口：这里自造单模块文本，
 * 走 asm → disasm → asm，要求两次汇编产物**字节一致**（同时验证反汇编能正确
 * 渲染 "lib::name" + argc + ret_kind 三个操作数）。
 */
namespace {
std::string read_all(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
}
}  // namespace

TEST(DriverM6, ExternCallAsmRoundTripIsByteIdentical) {
  std::string a1 = write_temp_file(
      "_start:\n"
      "EXTERN_CALL \"crt::strlen\" 1 2\n"
      "HALT\n", "clux_m6_asm");
  std::string b1 = a1 + ".1.cxb";
  std::string a2 = a1 + ".2.cxs";
  std::string b2 = a1 + ".2.cxb";

  ASSERT_EQ(driver_asm_to_bin(a1.c_str(), b1.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(b1.c_str(), a2.c_str()), 0);
  /* 反汇编应原样渲染三个操作数 */
  std::string text = read_all(a2);
  EXPECT_NE(text.find("EXTERN_CALL \"crt::strlen\" 1 2"), std::string::npos)
      << "disasm output:\n" << text;
  ASSERT_EQ(driver_asm_to_bin(a2.c_str(), b2.c_str()), 0);
  EXPECT_EQ(read_all(b1), read_all(b2)); /* 字节一致 */

  std::remove(a1.c_str());
  std::remove(b1.c_str());
  std::remove(a2.c_str());
  std::remove(b2.c_str());
}

/* ---- M6：源码级 FFI 调用打通（sema → compiler EXTERN_CALL → VM → C 库）----
 *
 * `crt::abs(-7) == 7`（整数）与 `crt::sqrt(9.0) == 3.0`（浮点，ret_kind=F64 走
 * ffi_call_f64）。library 由 `extern lib` 声明，driver 在编译期 dlopen + dlsym。
 */
TEST(DriverM6, SourceLevelFfiCallIntegerAndFloat) {
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "extern cfunc crt::abs(x: i32): i32;\n"
      "extern cfunc crt::sqrt(x: f64): f64;\n"
      "func main(): i32 {\n"
      "  if (crt::abs(-7) != 7) { return 1; }\n"
      "  if (crt::sqrt(9.0) != 3.0) { return 2; }\n"
      "  return 0;\n"
      "}\n", "clux_m6_srccall");
  EXPECT_EQ(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* 反例：调用未声明的 FFI 符号 → sema 明确诊断（no matching extern cfunc） */
TEST(DriverM6, SourceLevelFfiCallUndeclaredSymbolRejected) {
  std::string path = write_temp_file(
      "extern lib \"msvcrt.dll\" as crt;\n"
      "func main(): i32 { return crt::nope(1); }\n", "clux_m6_srcbad");
  EXPECT_NE(driver_run_file(path.c_str()), 0);
  std::remove(path.c_str());
}

/* ================================================================
 * M6：**FFI 声明随容器落盘**（.cxb/.cxs 里保住 extern lib）
 *
 * 修复前：`module_t.ffi_libs` 只由**编译期**扫描 AST 建立，字节码里只剩
 * `EXTERN_CALL "crt::sym"` —— 加载方无从知道该 dlopen 什么，于是
 * `bc emit x.cx -o x.cxb && run x.cxb` 在第一次 FFI 调用处报
 * `no loaded library 'crt' in this module`（源码直跑一切正常）。
 * 修法：容器 v3 每个模块携带 (别名, 路径) 表；`.cxs` 文本用
 * `externlib "<路径>" as <别名>` 指令；加载侧按表重新 dlopen 建库表。
 *
 * 载荷用**纯 clux 的 qsort 回调程序**：一次覆盖 EXTERN_CALL（调用方向）与
 * `cfunc_ptr` 闭包（回调用方向）两条运行期路径，因此这几条用例能同时证明
 * "库表恢复了"且"闭包在容器加载后仍可用"。
 * ================================================================ */

namespace {

/* 纯 clux qsort 回调程序（`main` 自检：升序 + 首尾 + 回调计数）。 */
std::string ffi_qsort_program(const std::string &lib) {
  return std::string("extern lib \"") + lib + "\" as crt;\n" +
         "extern cfunc crt::qsort(base: opaque, n: u64, size: u64, "
         "cmp: opaque): void;\n" +
         "var CALLS: i32 = 0;\n" +
         "cfunc cmp(a: opaque, b: opaque): i32 {\n" +
         "  CALLS = CALLS + 1;\n" +
         "  var x: i32 = (a as ref *i32).*;\n" +
         "  var y: i32 = (b as ref *i32).*;\n" +
         "  if (x < y) { return -1; }\n" +
         "  if (x > y) { return 1; }\n" +
         "  return 0;\n" +
         "}\n" +
         "func main(): i32 {\n" +
         "  var arr = .[5]i32{4, 2, 5, 1, 3};\n" +
         "  crt::qsort(arr.&, 5, 4, cfunc_ptr(cmp));\n" +
         "  if (arr[0] != 1 || arr[1] != 2 || arr[2] != 3) { return 1; }\n" +
         "  if (arr[3] != 4 || arr[4] != 5) { return 2; }\n" +
         "  if (CALLS < 4) { return 3; }\n" +
         "  return 0;\n" +
         "}\n";
}

}  // namespace

/* 核心用例（修复的正是这条）：FFI 程序从 .cxb 运行 */
TEST(DriverM6, FfiProgramRunsFromCxb) {
  std::string src = write_temp_file(ffi_qsort_program(kDriverSystemLib),
                                    "clux_m6_cxb");
  std::string bin = src + ".cxb";
  ASSERT_EQ(driver_build_bin(src.c_str(), bin.c_str()), 0);
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0); /* 修复前：no loaded library */
  std::remove(src.c_str());
  std::remove(bin.c_str());
}

/* FFI 程序从 .cxs（汇编文本）运行：externlib 指令必须被还原成库表 */
TEST(DriverM6, FfiProgramRunsFromCxs) {
  std::string src = write_temp_file(ffi_qsort_program(kDriverSystemLib),
                                    "clux_m6_cxs");
  std::string asm_path = src + ".cxs";
  ASSERT_EQ(driver_build_asm(src.c_str(), asm_path.c_str()), 0);
  std::string text = read_all(asm_path);
  EXPECT_NE(text.find("externlib \"" + std::string(kDriverSystemLib) +
                      "\" as crt"),
            std::string::npos)
      << "asm output:\n" << text;
  EXPECT_EQ(driver_run_asm(asm_path.c_str()), 0);
  std::remove(src.c_str());
  std::remove(asm_path.c_str());
}

/* FFI 声明在 .cxb → .cxs → .cxb 往返中不丢，且产物字节一致 */
TEST(DriverM6, FfiProgramCxbCxsRoundTripIsByteIdentical) {
  std::string src = write_temp_file(ffi_qsort_program(kDriverSystemLib),
                                    "clux_m6_ffirt");
  std::string b1 = src + ".1.cxb";
  std::string a2 = src + ".2.cxs";
  std::string b2 = src + ".2.cxb";
  ASSERT_EQ(driver_build_bin(src.c_str(), b1.c_str()), 0);
  ASSERT_EQ(driver_bin_to_asm(b1.c_str(), a2.c_str()), 0);
  ASSERT_EQ(driver_asm_to_bin(a2.c_str(), b2.c_str()), 0);
  EXPECT_EQ(read_all(b1), read_all(b2)); /* 逐字节一致（含 FFI 段） */
  EXPECT_EQ(driver_run_bin(b2.c_str()), 0);

  std::remove(src.c_str());
  std::remove(b1.c_str());
  std::remove(a2.c_str());
  std::remove(b2.c_str());
}

/* FFI 声明属于**依赖模块**时同样落盘（每模块一段，section 归属必须正确） */
TEST(DriverM6, FfiInDependencyModuleRunsFromCxb) {
  std::filesystem::path dir = std::filesystem::path(clux_test_temp_dir()) / "clux_mbc_ffi_dep";
  std::filesystem::create_directories(dir);
  std::string hint = (dir / "").string();

  std::string dep = hint + "ffi.cx";
  std::string main_src = hint + "main.cx";
  FILE *f = fopen(dep.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  fputs((std::string("extern lib \"") + kDriverSystemLib +
         "\" as crt;\n"
         "extern cfunc crt::abs(x: i32): i32;\n"
         "export func dist(a: i32, b: i32): i32 { return crt::abs(a - b); }\n")
            .c_str(),
        f);
  fclose(f);
  f = fopen(main_src.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  fputs("import m from \"./ffi\";\n"
        "func main(): i32 {\n"
        "  if (m::dist(3, 10) != 7) { return 1; }\n"
        "  return 0;\n"
        "}\n",
        f);
  fclose(f);

  std::string bin = hint + "main.cxb";
  ASSERT_EQ(driver_build_bin(main_src.c_str(), bin.c_str()), 0);
  std::string asm_path = hint + "main.cxs";
  ASSERT_EQ(driver_bin_to_asm(bin.c_str(), asm_path.c_str()), 0);
  /* externlib 行必须落在**依赖模块**的段里（MODULE "./ffi" 之后） */
  std::string text = read_all(asm_path);
  size_t mod_pos = text.find("MODULE \"./ffi\"");
  size_t ffi_pos = text.find("externlib");
  EXPECT_NE(mod_pos, std::string::npos) << text;
  EXPECT_NE(ffi_pos, std::string::npos) << text;
  EXPECT_LT(mod_pos, ffi_pos) << text;
  EXPECT_EQ(driver_run_bin(bin.c_str()), 0);

  std::remove(dep.c_str());
  std::remove(main_src.c_str());
  std::remove(bin.c_str());
  std::remove(asm_path.c_str());
}
