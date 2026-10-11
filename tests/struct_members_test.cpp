/* 结构体命名空间成员 + 方法调用糖（语法糖）
 *
 *   struct Test {
 *     id: i32;
 *     var  data = 123;                          // 静态属性（本质：全局变量）
 *     func getId(self: ref *const Test): i32 {  // 方法（第一个参数即接收者）
 *       return self.id;
 *     }
 *   }
 *
 *   Test::data          // 命名空间访问静态属性（读/写/复合赋值）
 *   Test::getId(t.&)    // 显式限定调用
 *   t.getId()           // ≡ typeof(t)::getId(t.&)   —— t 是值：取地址
 *   p.getId()           // ≡ typeof(p.*)::getId(p)   —— p 是指针：自动解引用
 *
 * 设计要点（见 docs/m6-design.md §14.22）：
 *  - 成员**本质是模块级全局声明**：编译流水线在 parse 之后把它们改名为
 *    `Test::data` / `Test::getId` 并追加进程序声明表，此后 sema/compiler/运行期
 *    完全走既有全局路径（没有新的运行时机制）。
 *  - `self` **不是关键字**，只是普通参数名——方法糖按**位置**取第一个参数当接收者。
 *  - 接收者规则：值 → `t.&`；指针 → `t` 本身。
 *  - 指针上的成员访问（`self.id`）自动解引用（`p.x` ≡ `p.*.x`）。
 *
 * 判定方式：clux 程序把检查结果写进测试库的 td_i32，宿主侧读回
 * （driver_run_file 的返回值只表示"编译并运行成功"，不代表 main 的返回值）。
 */
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <cstring>
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

void *sm_alloc(size_t size) { return std::malloc(size); }
void sm_free(void *p) { std::free(p); }

struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(sm_alloc, sm_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

class StructMembersTest : public ::testing::Test {
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

  /* 跑一段 clux 源码（自动补测试库前言与判定收尾） */
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
         ("clux_struct_members_" + std::to_string(counter++) + ".cx"))
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
};

LibKeeper *StructMembersTest::keep = nullptr;

}  // namespace

/* 静态属性：`Test::data` 读 / 写 / 复合赋值；与成员内的引用共享同一份存储 */
TEST_F(StructMembersTest, StaticPropertyAccess) {
  expect_verdict(
      "struct Test {\n"
      "  id: i32;\n"
      "  var data = 123;\n"
      "  func bump(): i32 { Test::data += 1; return Test::data; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (Test::data != 123) { return report(1); }\n"
      "  Test::data = 200;\n"
      "  if (Test::data != 200) { return report(2); }\n"
      "  Test::data += 1;\n"
      "  if (Test::data != 201) { return report(3); }\n"
      "  if (Test::bump() != 202) { return report(4); }\n"  /* 函数内也改同一份 */
      "  if (Test::data != 202) { return report(5); }\n"
      "  return report(0);\n"
      "}\n");
}

/* 方法糖：值取地址、指针自动解引用、显式限定调用三者等价 */
TEST_F(StructMembersTest, MethodCallSugar) {
  expect_verdict(
      "struct Test {\n"
      "  id: i32;\n"
      "  func getId(self: ref *const Test): i32 { return self.id; }\n"
      "  func add(self: ref *const Test, n: i32): i32 { return self.id + n; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{ .id = 234 };\n"
      "  if (t.getId() != 234) { return report(1); }\n"        /* 值 → t.& */
      "  if (t.add(6) != 240) { return report(2); }\n"
      "  var p: ref *Test = t.&;\n"
      "  if (p.getId() != 234) { return report(3); }\n"        /* 指针 → p */
      "  if (Test::getId(t.&) != 234) { return report(4); }\n" /* 显式等价写法 */
      "  if (Test::getId(p) != 234) { return report(5); }\n"
      "  var o: own *Test = new Test{ .id = 7 };\n"
      "  if (o.getId() != 7) { return report(6); }\n"          /* own 指针 */
      "  return report(0);\n"
      "}\n");
}

/* 方法体内 `self.id`（指针成员访问自动解引用）读与写；写穿 const 被拒 */
TEST_F(StructMembersTest, MethodBodyAutoDeref) {
  expect_verdict(
      "struct Test {\n"
      "  id: i32;\n"
      "  func bump(self: ref *Test): i32 { self.id += 1; return self.id; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{ .id = 1 };\n"
      "  if (t.bump() != 2) { return report(1); }\n"
      "  if (t.id != 2) { return report(2); }\n"
      "  var p: ref *Test = t.&;\n"
      "  if (p.bump() != 3) { return report(3); }\n"
      "  if (t.id != 3) { return report(4); }\n"
      "  return report(0);\n"
      "}\n");
}

/* 诊断：写 const 接收者 / 不存在的成员 */
TEST_F(StructMembersTest, Diagnostics) {
  const char *cases[] = {
      /* 方法体内写 const 接收者 */
      "struct Test {\n"
      "  id: i32;\n"
      "  func bad(self: ref *const Test): i32 { self.id = 1; return 0; }\n"
      "}\n"
      "func main(): i32 { return 0; }\n",
      /* 不存在的成员方法 */
      "struct Test { id: i32; }\n"
      "func main(): i32 { var t: Test = .Test{ .id = 1 }; return t.nope(); }\n",
  };
  for (const char *src : cases) {
    static int counter = 0;
    std::string path =
        (std::filesystem::temp_directory_path() /
         ("clux_struct_members_bad_" + std::to_string(counter++) + ".cx"))
            .string();
    FILE *fp = fopen(path.c_str(), "wb");
    ASSERT_NE(fp, nullptr);
    fwrite(src, 1, strlen(src), fp);
    fclose(fp);
    int rc = driver_run_file(path.c_str());
    std::remove(path.c_str());
    EXPECT_NE(rc, 0) << src;
  }
}
