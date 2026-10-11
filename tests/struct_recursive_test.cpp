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
    std::string src = "extern lib \"" + std::string(kTestLib) +
                      "\" as td;\n"
                      "extern var td::td_i32: i32;\n" +
                      body +
                      "func report(v: i32): i32 { td::td_i32 = v; return v; }\n";
    std::string path =
        (std::filesystem::temp_directory_path() /
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
        (std::filesystem::temp_directory_path() /
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

/* ---- 正向 1：自引用指针字段（单链表）——构造 / 遍历 / 字段读 ---- */
TEST_F(StructRecursiveTest, SelfPointerLinkedList) {
  expect_verdict(
      "struct Node { v: i32; next: ?ref *const Node; }\n"
      "func sum(n: ref *const Node): i32 {\n"
      "  var acc: i32 = 0;\n"
      "  var cur: ref *const Node = n;\n"
      "  while (true) {\n"
      "    acc += cur.v;\n"
      "    var nx: '<*> ?ref *const Node = cur.next;\n"
      "    if (nx == nil) { break; }\n"
      "    cur = nx.!;\n"
      "  }\n"
      "  return acc;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: '<*> Node = .Node{ .v = 1, .next = .?ref *const Node{nil} };\n"
      "  var b: '<*> Node = .Node{ .v = 2, .next = a.& };\n"
      "  var c: '<*> Node = .Node{ .v = 3, .next = b.& };\n"
      "  if (sum(c.&) != 6) { return report(1); }\n"
      "  if (c.next.!.v != 2) { return report(2); }\n"
      "  if (c.v != 3) { return report(3); }\n"
      "  if (c.next.!.next.!.v != 1) { return report(4); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 2：自引用 + 方法糖（方法体里沿 next 递归遍历） ---- */
TEST_F(StructRecursiveTest, MethodWalksSelfReference) {
  expect_verdict(
      "struct Node {\n"
      "  v: i32;\n"
      "  next: ?ref *const Node;\n"
      "  func sum(self: ref *const Node): i32 {\n"
      "    var acc: i32 = 0;\n"
      "    var cur: ref *const Node = self;\n"
      "    while (true) {\n"
      "      acc += cur.v;\n"
      "      var nx: '<*> ?ref *const Node = cur.next;\n"
      "      if (nx == nil) { break; }\n"
      "      cur = nx.!;\n"
      "    }\n"
      "    return acc;\n"
      "  }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var a: '<*> Node = .Node{ .v = 4, .next = .?ref *const Node{nil} };\n"
      "  var b: '<*> Node = .Node{ .v = 5, .next = a.& };\n"
      "  if (b.sum() != 9) { return report(1); }\n"
      "  if (Node::sum(a.&) != 4) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 3：互相引用（A ↔ B），两侧都是可空指针字段 ---- */
TEST_F(StructRecursiveTest, MutualReferences) {
  expect_verdict(
      "struct A { name: i32; b: ?ref *const B; }\n"
      "struct B { tag: i32; a: ?ref *const A; }\n"
      "func main(): i32 {\n"
      "  var x: '<*> B = .B{ .tag = 2, .a = .?ref *const A{nil} };\n"
      "  var y: '<*> A = .A{ .name = 1, .b = x.& };\n"
      "  x.a = y.&;\n"
      "  if (y.b.!.tag != 2) { return report(1); }\n"
      "  if (x.a.!.name != 1) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 4：递归函数 + 二叉树（左右子树都是自身） ---- */
TEST_F(StructRecursiveTest, BinaryTreeDepth) {
  expect_verdict(
      "struct Tree { v: i32; left: ?ref *const Tree; right: ?ref *const Tree; }\n"
      "func depth(t: ref *const Tree): i32 {\n"
      "  var l: i32 = 0;\n"
      "  var r: i32 = 0;\n"
      "  var ln: '<*> ?ref *const Tree = t.left;\n"
      "  if (ln != nil) { l = depth(ln.!); }\n"
      "  var rn: '<*> ?ref *const Tree = t.right;\n"
      "  if (rn != nil) { r = depth(rn.!); }\n"
      "  if (l > r) { return l + 1; }\n"
      "  return r + 1;\n"
      "}\n"
      "func main(): i32 {\n"
      "  var leaf: '<*> Tree = .Tree{ .v = 1, .left = .?ref *const Tree{nil},"
      " .right = .?ref *const Tree{nil} };\n"
      "  var mid: '<*> Tree = .Tree{ .v = 2, .left = leaf.&,"
      " .right = .?ref *const Tree{nil} };\n"
      "  var root: '<*> Tree = .Tree{ .v = 3, .left = mid.&, .right = leaf.& };\n"
      "  if (depth(root.&) != 3) { return report(1); }\n"
      "  if (depth(leaf.&) != 1) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 5：函数类型字段的签名直接点名自身（手写虚表不再需要 opaque） ---- */
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

/* ---- 正向 6：递归字段里的数组/切片包装（布局延迟定型） ---- */
TEST_F(StructRecursiveTest, DerivedWrapperLayoutIsFinalized) {
  expect_verdict(
      "struct Node { v: i32; kids: ?ref *[2]Node; }\n"
      "struct Plain { v: i32; kids: ?ref *[2]Plain; }\n"
      "struct List { v: i32; rest: own []ref *const List; }\n"
      "func main(): i32 {\n"
      "  if (sizeof(Node) != sizeof(Plain)) { return report(1); }\n"
      "  if (sizeof([2]Node) != 2u64 * sizeof(Node)) { return report(2); }\n"
      "  if (sizeof([3]Node) != 3u64 * sizeof(Node)) { return report(3); }\n"
      "  if (sizeof(List) != sizeof(ref []ref *const List) + 8u64) {"
      " return report(4); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 7：T → ?T 提升同时加限定符（ref *T → ?ref *const T） ---- */
TEST_F(StructRecursiveTest, OptionLiftAddsQualifier) {
  expect_verdict(
      "struct Box { p: ?ref *const i32; }\n"
      "func main(): i32 {\n"
      "  var v: i32 = 7;\n"
      "  var o: '<*> ?ref *const i32 = v.&;\n"
      "  if (o.!.* != 7) { return report(1); }\n"
      "  var b: '<*> Box = .Box{ .p = v.& };\n"
      "  if (b.p.!.* != 7) { return report(2); }\n"
      "  b.p = .?ref *const i32{nil};\n"
      "  var chk: '<*> ?ref *const i32 = b.p;\n"
      "  if (chk != nil) { return report(3); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 8：前向引用（自身不参与环）仍然照旧 ---- */
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
