/* 结构体指针**向上自动转换**（大 → 小，前缀字段兼容）
 *
 * 规则：`大结构体* → 小结构体*` 只要「小结构体的全部字段恰是大结构体字段表的前缀
 * （名字 + 类型逐一相等）」就隐式成立——C 布局自左向右分配，前缀字段的偏移与类型
 * 完全一致，因此同一地址按小结构体解释是对的（C 的"公共起始序列"规则）。
 *
 * 允许：`ref *Big → ref *Small`、`own *Big → ref *Small`（借用目标）。
 * 不允许（本文件负例覆盖）：
 *   - 反向（小 → 大，向下转换）：大结构体多出的字段没有存储；
 *   - 字段名或类型不在前缀上匹配；
 *   - **值**转换（`var b: Base = d;`）：值必须类型完全相同，否则静默切片（丢尾部字段）；
 *   - 丢限定（`ref *const T → ref *T`）：向上转换不得去掉 const。
 * own → own 目标的向上转换也不隐式允许：按小结构体释放会漏掉尾部字段的
 * own 资源（泄漏），需要时请显式表达。
 *
 * 这是配合结构体语法糖的能力：把"派生"结构体指针直接喂给按"基"结构体声明的接收者/
 * 参数（`Base::getId(d.&)`）。判定方式：写测试库 td_i32，宿主侧读回。
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

void *up_alloc(size_t size) { return std::malloc(size); }
void up_free(void *p) { std::free(p); }

struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(up_alloc, up_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

class StructUpcastTest : public ::testing::Test {
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
        "extern lib td from \"" +
        std::string(kTestLib) +
        "\";\n"
        "extern var td::td_i32: i32;\n" +
        body +
        "func report(v: i32): i32 { td::td_i32 = v; return v; }\n";
    std::string path =
        (clux_test_path() /
         ("clux_upcast_" + std::to_string(counter++) + ".cx"))
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
        (clux_test_path() /
         ("clux_upcast_bad_" + std::to_string(counter++) + ".cx"))
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

LibKeeper *StructUpcastTest::keep = nullptr;

}  // namespace

/* 正向：ref 向上转换（含写穿）、own → ref、按基声明的函数/方法参数、3→2 前缀 */
TEST_F(StructUpcastTest, PointerUpcast) {
  expect_verdict(
      "struct Base { id: i32; kind: i32; }\n"
      "struct Derived { id: i32; kind: i32; extra: i64; name: str; }\n"
      "struct Big { a: i32; b: i32; c: i32; }\n"
      "struct Small { a: i32; b: i32; }\n"
      "func baseId(b: ref *const Base): i32 { return b.id; }\n"
      "func baseKind(b: ref *Base): i32 { b.kind = b.kind + 1; return b.kind; }\n"
      "struct Holder {\n"
      "  b: Base;\n"
      "  func idOf(self: ref *Holder): i32 { return self.b.id; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var verdict: i32 = 0;\n"
      "  var d: Derived = .Derived{ .id = 11, .kind = 22, .extra = 33, .name = \"x\" };\n"
      "  if (baseId(d.&) != 11) { return report(1); }\n"          /* ref *Derived → ref *const Base */
      "  if (baseKind(d.&) != 23) { return report(2); }\n"        /* 可变借用向上转换 */
      "  if (d.kind != 23) { return report(3); }\n"               /* 写穿回原存储 */
      "  var p: ref *Base = d.&;\n"
      "  if (p.id != 11 || p.kind != 23) { return report(4); }\n"
      "  p.id = 99;\n"
      "  if (d.id != 99) { return report(5); }\n"                  /* 同一块存储 */
      "  var o: own *Derived = new Derived{ .id = 7, .kind = 8, .extra = 9, .name = \"y\" };\n"
      "  var q: ref *Base = o;\n"
      "  if (q.id != 7 || q.kind != 8) { return report(6); }\n"    /* own → ref 向上转换 */
      "  var big: Big = .Big{ .a = 1, .b = 2, .c = 3 };\n"
      "  var sp: ref *Small = big.&;\n"
      "  if (sp.a != 1 || sp.b != 2) { return report(7); }\n"      /* 3 → 2 前缀 */
      "  var h: Holder = .Holder{ .b = .Base{ .id = 5, .kind = 6 } };\n"  /* Holder.b 字段可读 */
      "  if (h.idOf() != 5) { return report(8); }\n"
      "  return report(0);\n"
      "}\n");
}

/* 负例：向下转换 / 前缀不匹配（名字、类型）/ 值转换 / 丢限定 */
TEST_F(StructUpcastTest, RejectedCases) {
  /* 反向：小 → 大 */
  expect_compile_error(
      "struct Base { id: i32; }\n"
      "struct Derived { id: i32; extra: i64; }\n"
      "func main(): i32 { var b: Base = .Base{ .id = 1 };\n"
      "  var p: ref *Derived = b.&; return p.id; }\n");
  /* 前缀上字段名不同 */
  expect_compile_error(
      "struct Base { nope: i32; }\n"
      "struct Derived { id: i32; extra: i64; }\n"
      "func main(): i32 { var d: Derived = .Derived{ .id = 1, .extra = 2 };\n"
      "  var p: ref *Base = d.&; return 0; }\n");
  /* 前缀上字段类型不同 */
  expect_compile_error(
      "struct Base { id: i64; }\n"
      "struct Derived { id: i32; extra: i64; }\n"
      "func main(): i32 { var d: Derived = .Derived{ .id = 1, .extra = 2 };\n"
      "  var p: ref *Base = d.&; return 0; }\n");
  /* 值转换仍要求完全相同（否则静默丢掉尾部字段） */
  expect_compile_error(
      "struct Base { id: i32; }\n"
      "struct Derived { id: i32; extra: i64; }\n"
      "func main(): i32 { var d: Derived = .Derived{ .id = 1, .extra = 2 };\n"
      "  var b: Base = d; return b.id; }\n");
  /* 向上转换不得丢限定：ref *const T → ref *T 仍须拒绝 */
  expect_compile_error(
      "struct Base { id: i32; }\n"
      "struct Derived { id: i32; extra: i64; }\n"
      "func byRef(x: ref *Base): i32 { return x.id; }\n"
      "func main(): i32 { var d: Derived = .Derived{ .id = 1, .extra = 2 };\n"
      "  var c: const Derived = d;\n"
      "  return byRef(c.&); }\n");
}
