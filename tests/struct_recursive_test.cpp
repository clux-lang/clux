/* 递归结构体（自引用 / 互相引用）——type 层"先物化、后解析字段"
 *
 *   struct Node { v: i32; next: ?ref *const Node; }        // 自引用（链表）
 *   struct A { b: ?ref *const B; } struct B { a: ?ref *const A; }  // 互相引用
 *
 * 语义（本次实现）：
 *   - sema 在求值结构体定义前先**物化**它的类型对象（分配 + 类型 id + 显示名 +
 *     编译期类型名绑定）；字段类型表达式因此能解析到"正在定义中的自己"或环上的
 *     另一个定义。构造侧本就支持成环：hoist 的 pass 1 先声明**所有**类型 id，
 *     pass 2 里指针/切片/func 的密封不依赖 base 布局（指针宽/切片头恒定）。
 *   - **按值内联**的字段仍要求被引用类型已 seal（struct 自身布局、数组/option/
 *     tuple/union 的布局都在构造时结算）→ 成环即明确报错（递归 by value 无解）；
 *     指针 / 切片 / func 签名 / const / volatile / 数组是允许的递归形态，其中
 *     const/volatile/数组这些"布局派生自 sub"的包装由 VM 侧
 *     type_sync_derived_layout 在 sub seal 后**延迟定型**。
 *
 * 判定方式与 struct_compose_test.cpp 同款：clux 程序把检查结果写进测试库的
 * td_i32，宿主侧读回；`expect_verdict` 断言 rc==0 且 td_i32==0，
 * `expect_compile_error` 断言 rc!=0（编译失败）。
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

void *sr_alloc(size_t size) { return std::malloc(size); }
void sr_free(void *p) { std::free(p); }

struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(sr_alloc, sr_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

class StructRecursiveTest : public ::testing::Test {
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
    std::string src = "extern lib td from \"" + std::string(kTestLib) +
                      "\";\n"
                      "extern var td::td_i32: i32;\n" +
                      body +
                      "func report(v: i32): i32 { td::td_i32 = v; return v; }\n";
    std::string path =
        (clux_test_path() /
         ("clux_struct_rec_" + std::to_string(counter++) + ".cx"))
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
         ("clux_struct_rec_bad_" + std::to_string(counter++) + ".cx"))
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

LibKeeper *StructRecursiveTest::keep = nullptr;

}  // namespace

/* ---- 正向 2：函数类型字段的签名直接点名自身（手写虚表不再需要 opaque） ---- */
TEST_F(StructRecursiveTest, FunctionFieldWithSelfSignature) {
  expect_verdict(
      "struct Shape { area: func(ref *const Shape) -> i32; r: i32; }\n"
      "func circle_area(s: ref *const Shape): i32 { return s.r * s.r * 3; }\n"
      "func rect_area(s: ref *const Shape): i32 { return s.r * s.r; }\n"
      "func area_of(s: ref *const Shape): i32 { return s.area(s); }\n"
      "func main(): i32 {\n"
      "  var c: '<*> Shape = .Shape{ .area = circle_area, .r = 3 };\n"
      "  var q: '<*> Shape = .Shape{ .area = rect_area, .r = 3 };\n"
      "  if (area_of(c.&) != 27) { return report(1); }\n"
      "  if (area_of(q.&) != 9) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 3：前向引用（自身不参与环）仍然照旧 ---- */
TEST_F(StructRecursiveTest, ForwardReferenceUnaffected) {
  expect_verdict(
      "struct Early { l: Late; n: i32; }\n"
      "struct Late { v: i32; }\n"
      "func main(): i32 {\n"
      "  var e: '<*> Early = .Early{ .l = .Late{ .v = 5 }, .n = 1 };\n"
      "  if (e.l.v != 5) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 负向 1：按值自含（无限大小）→ 编译错误 ---- */
TEST_F(StructRecursiveTest, RejectsSelfByValue) {
  expect_compile_error(
      "struct Self { v: i32; me: Self; }\n"
      "func main(): i32 { return 0; }\n");
}

/* ---- 负向 2：按值互相包含 → 编译错误 ---- */
TEST_F(StructRecursiveTest, RejectsMutualByValue) {
  expect_compile_error(
      "struct P { q: Q; }\n"
      "struct Q { p: P; }\n"
      "func main(): i32 { return 0; }\n");
}

/* ---- 负向 3：数组按值自含 → 编译错误 ---- */
TEST_F(StructRecursiveTest, RejectsArrayByValue) {
  expect_compile_error(
      "struct Bad { xs: [2]Bad; }\n"
      "func main(): i32 { return 0; }\n");
}

/* ---- 负向 4：option 包装一个正在定义的类型 → 编译错误（无延迟定型路径） ---- */
TEST_F(StructRecursiveTest, RejectsOptionWrapperOnDefiningType) {
  expect_compile_error(
      "struct Bad { o: ?Bad; }\n"
      "func main(): i32 { return 0; }\n");
  expect_compile_error(
      "struct Bad2 { p: ref *?Bad2; }\n"
      "func main(): i32 { return 0; }\n");
}
