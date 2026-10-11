/* `T → const T`（加限定）的隐式转换：构造、参数传递、TDZ 进入三处都合法
 *
 * C 语义：**限定符只能加不能去**——
 *   - 加限定永远安全（同一份数据的只读视图）：`T → const T`、`T → volatile T`、
 *     `ref *T → ref *const T`、`own *T → ref *const T`；
 *   - 去限定必须显式 `as`（本文件末三个负例覆盖"不许去"与"不可重赋值"）。
 *
 * 落地位置（按类型族，而不是在通用入口一刀切）：
 *   - struct / cunion：各自 vtable 的 implicit_cast 识别"目标只是给源加了限定"；
 *   - 指针：ptr_implicit_cast（含 share/weak 引用计数维护）；
 *   - 标量：int/float vtable（原本就支持 volatile/const 视图）；
 *   - 字段读取：sema 的 ex_member 先剥限定再查字段（**读** const 值合法，
 *     写由赋值路径按 const 规则拒绝）；
 *   - 运行期字段操作（FIELD_GET/SET）：先剥限定再按底层布局定位。
 *
 * TDZ 进入（`var x: const T = undefined; x = v;`）由 sema 决定：只有
 * `sym->flow_init == false`（尚未初始化）时才允许给 const 变量赋值——即
 * "const 的首次初始化"，之后再赋值报错。
 *
 * 判定方式：clux 程序把检查结果写进测试库的 td_i32，宿主侧读回。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "driver/driver.h"
#include "vm/ffi_lib.h"
}

#include "test_common.h"

namespace {

#if defined(_WIN32)
const char *kTestLib = "clux_ffi_testdata.dll";
#else
const char *kTestLib = "libclux_ffi_testdata.so";
#endif

void *cq_alloc(size_t size) { return std::malloc(size); }
void cq_free(void *p) { std::free(p); }

struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(cq_alloc, cq_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

class ConstConvertTest : public ::testing::Test {
 protected:
  static LibKeeper *keep;
  int32_t saved = 0;

  static void SetUpTestSuite() { keep = new LibKeeper(); }
  static void TearDownTestSuite() {
    delete keep;
    keep = nullptr;
  }

  void SetUp() override {
    ASSERT_NE(keep->lib, nullptr) << "cannot open " << kTestLib;
    int32_t *v = verdict();
    ASSERT_NE(v, nullptr);
    saved = *v;
    *v = -1;
  }
  void TearDown() override {
    if (int32_t *v = verdict()) *v = saved;
  }

  int32_t *verdict() {
    char err[256] = {0};
    return keep->lib
               ? (int32_t *)ffi_lib_sym(keep->lib, "td_i32", err, sizeof err)
               : nullptr;
  }

  void expect_verdict(const std::string &body) {
    static int counter = 0;
    std::string src =
        "extern lib \"" +
        std::string(kTestLib) +
        "\" as td;\n"
        "extern var td::td_i32: i32;\n" +
        body +
        "func report(v: i32): i32 { td::td_i32 = v; return v; }\n";
    std::string path =
        (std::filesystem::temp_directory_path() /
         ("clux_const_convert_" + std::to_string(counter++) + ".cx"))
            .string();
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(src.data(), 1, src.size(), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    ASSERT_EQ(rc, 0) << src;
    int32_t *v = verdict();
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(*v, 0) << "clux verdict=" << *v << "\n" << src;
  }

  void expect_compile_error(const char *src) {
    static int counter = 0;
    std::string path =
        (std::filesystem::temp_directory_path() /
         ("clux_const_convert_bad_" + std::to_string(counter++) + ".cx"))
            .string();
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(src, 1, strlen(src), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    EXPECT_NE(rc, 0) << src;
  }
};

LibKeeper *ConstConvertTest::keep = nullptr;

/* 覆盖三种合法场景：构造 / 参数传递（按值 + 指针）/ TDZ 进入；标量同理 */
const char *kProgram =
    "struct Test { id: i32; }\n"
    "struct Outer { t: Test; }\n"
    "func byConst(x: const Test): i32 { return x.id; }\n"
    "func byConstOuter(x: const Outer): i32 { return x.t.id; }\n" /* 嵌套 struct */
    "func byPtr(x: ref *const Test): i32 { return x.id; }\n"
    "func main(): i32 {\n"
    "  var verdict: i32 = 0;\n"
    /* ① 构造：T → const T */
    "  var c: const Test = .Test{ .id = 5 };\n"
    "  if (c.id != 5) { return report(1); }\n"
    /* ② 参数传递：值 → const T（嵌套也能读字段） */
    "  if (byConst(.Test{ .id = 6 }) != 6) { return report(2); }\n"
    "  var t: Test = .Test{ .id = 7 };\n"
    "  if (byConst(t) != 7) { return report(3); }\n"
    "  var o: Outer = .Outer{ .t = .Test{ .id = 11 } };\n"
    "  if (byConstOuter(o) != 11) { return report(4); }\n"
    /* ②' 参数传递：ref *T → ref *const T */
    "  if (byPtr(t.&) != 7) { return report(5); }\n"
    /* ③ TDZ 进入：先未初始化声明，再一次性初始化 */
    "  var late: const Test = undefined;\n"
    "  late = .Test{ .id = 8 };\n"
    "  if (late.id != 8) { return report(6); }\n"
    /* ④ 标量同上（构造 + TDZ） */
    "  var k: const i32 = 42;\n"
    "  var m: const i32 = undefined;\n"
    "  m = 43;\n"
    "  if (k != 42 || m != 43) { return report(7); }\n"
    /* 只读视图可以继续参与运算/比较 */
    "  if (c.id + k != 47) { return report(8); }\n"
    "  return report(0);\n"
    "}\n";

}  // namespace

TEST_F(ConstConvertTest, ConstructionParamsAndTdzInit) { expect_verdict(kProgram); }

/* `volatile` **在脚本态完全无视**：sema 与 compiler 一致地折叠为 sub（类型解析与
 * 类型构造两处），因此 volatile 不参与任何类型检查——读写/运算/复合赋值/双向赋值/
 * 传参/比较/取字段/指针 base 全部放行；`const volatile` 只保留 const 语义。
 * AST/fmt 不动（格式化仍原样输出 volatile，不丢关键字）。 */
TEST_F(ConstConvertTest, VolatileFullyIgnored) {
  expect_verdict(
      "struct Test { id: i32; }\n"
      "func byVal(x: i32): i32 { return x; }\n"
      "func byConstTest(x: const Test): i32 { return x.id; }\n"
      "func main(): i32 {\n"
      "  var verdict: i32 = 0;\n"
      "  var a: volatile i32 = 1;\n"
      "  a = a + 1;\n"
      "  a += 2;\n"
      "  if (a != 4) { return report(1); }\n"
      "  var b: i32 = a;\n"
      "  var c: volatile i32 = b;\n"
      "  if (byVal(a) != 4 || byVal(c) != 4) { return report(2); }\n"
      "  var d: const volatile i32 = 7;\n"
      "  var e: volatile const i32 = 8;\n"
      "  if (d != 7 || e != 8) { return report(3); }\n"
      "  if (sizeof(volatile i32) != sizeof(i32)) { return report(4); }\n"
      "  var t: Test = .Test{ .id = 9 };\n"
      "  var p: ref *volatile Test = t.&;\n"
      "  if (p.id != 9) { return report(5); }\n"
      "  p.id = 10;\n"
      "  if (t.id != 10) { return report(6); }\n"
      "  var v: volatile Test = .Test{ .id = 11 };\n"
      "  if (byConstTest(v) != 11) { return report(7); }\n"
      "  return report(0);\n"
      "}\n");
}

/* 负例：**去**限定不允许；const 变量重赋值不允许；写 const 值的字段不允许 */
TEST_F(ConstConvertTest, NegativeCases) {
  /* 去限定：const Test → ref *Test（反过来加限定才合法） */
  expect_compile_error(
      "struct Test { id: i32; }\n"
      "func byRef(x: ref *Test): i32 { return x.id; }\n"
      "func main(): i32 {\n"
      "  var c: const Test = .Test{ .id = 1 };\n"
      "  return byRef(c.&);\n"
      "}\n");
  /* const 变量初始化之后再赋值 → 报错（TDZ 之外） */
  expect_compile_error(
      "struct Test { id: i32; }\n"
      "func main(): i32 {\n"
      "  var c: const Test = .Test{ .id = 1 };\n"
      "  c = .Test{ .id = 2 };\n"
      "  return 0;\n"
      "}\n");
  /* 写 const 值的字段 → 报错 */
  expect_compile_error(
      "struct Test { id: i32; }\n"
      "func main(): i32 {\n"
      "  var c: const Test = .Test{ .id = 1 };\n"
      "  c.id = 2;\n"
      "  return 0;\n"
      "}\n");
}
