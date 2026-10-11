/* 结构体组合（`...Base;`）——语法树/类型层的"嵌入继承"
 *
 *   struct Base { id: i32; var count = 0; func getId(self: ref *const Base): i32 { ... } }
 *   struct Test { ...Base; extra: i64; }
 *
 * 语义（用户规格）：
 *   - `...Base;` 只是**语法**（parser 只记录名字 + 出现在自己第几个字段之前）；
 *   - 组合发生在 **sema、目标类型 seal 之前**的开放窗口：遍历 Base 的**类型**
 *     字段表（含未来装饰器对它的修改），把字段插进 Test 的开放类型字段表，
 *     位置 = `...Base` 在源码中的位置，登记嵌入表，然后**只 seal 一次**并按
 *     C 对齐规则定型布局。seal 之后类型不可变（VM 写入 API 拒绝 sealed 类型）。
 *   - 因此"Test 的字段表 = 在自己字段之间插入 Base 的字段表"：
 *     `...Base` 在开头 ⟹ Test 的字段前缀 == Base 的字段 ⟹ 既有的**结构体指针
 *     向上转换**（struct_type_prefix_compatible）自然生效，`t.getId()` 可用；
 *     `...Base` 前面还有字段 ⟹ 前缀不成立 ⟹ `t.getId()` **自然编译失败**；
 *   - 成员（`var`/`func`）**不复制**：组合后 `Test` **自己没声明**的同名成员通过
 *     嵌入链**继承名字查找**（`t.getId()` / `Test::getId` / `Test::count` 都解析到
 *     `Base::...` 的同一份存储，静态属性是同一个全局变量）；多个嵌入同名 → 歧义报错；
 *   - **attribute 覆盖**：`Test` **自己声明**的同名 attribute（`var` 属性 / `func`
 *     方法）**静默覆盖**继承来的，解析到**自己那一份**且与 `Base::...` **互相独立**
 *     （不共享存储、不报歧义/重复）。候选=结构体命名空间成员；字段是唯一例外；
 *   - **字段不允许覆盖**：自有字段与嵌入字段重名、两个嵌入字段重名都报错
 *     （只有类型的 attribute 可以在 seal 前被改写覆盖）。
 *
 * 判定方式与 struct_upcast_test.cpp / struct_members_test.cpp 同款：clux 程序把
 * 检查结果写进测试库的 td_i32，宿主侧读回；`expect_verdict` 断言 rc==0 且
 * td_i32==0，`expect_compile_error` 断言 rc!=0（编译失败）。
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

void *sc_alloc(size_t size) { return std::malloc(size); }
void sc_free(void *p) { std::free(p); }

struct LibKeeper {
  allocator_t *alloc = nullptr;
  ffi_lib_t *lib = nullptr;
  LibKeeper() {
    alloc = create_allocator(sc_alloc, sc_free);
    if (!alloc) return;
    char err[256] = {0};
    lib = ffi_lib_open(alloc, kTestLib, err, sizeof err);
  }
  ~LibKeeper() {
    if (lib) ffi_lib_close(lib);
    if (alloc) delete_allocator(&alloc);
  }
};

class StructComposeTest : public ::testing::Test {
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
         ("clux_struct_compose_" + std::to_string(counter++) + ".cx"))
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
         ("clux_struct_compose_bad_" + std::to_string(counter++) + ".cx"))
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

LibKeeper *StructComposeTest::keep = nullptr;

}  // namespace

/* ---- 正向 1：嵌入在开头 —— 字段访问 / 方法糖 / 显式限定 / 向上转换 ---- */
TEST_F(StructComposeTest, EmbedAtFrontBasic) {
  expect_verdict(
      "struct Base {\n"
      "  id: i32;\n"
      "  func getId(self: ref *const Base): i32 { return self.id; }\n"
      "}\n"
      "struct Test { ...Base; extra: i64; }\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{ .id = 7, .extra = 2 };\n"
      "  if (t.getId() != 7) { return report(1); }\n"           /* 方法糖 */
      "  if (Test::getId(t.&) != 7) { return report(2); }\n"    /* 显式限定 */
      "  if (t.id != 7) { return report(3); }\n"                /* 继承字段读 */
      "  if (t.extra != 2) { return report(4); }\n"             /* 自有字段读 */
      "  t.id = 11;\n"
      "  t.extra = 12;\n"
      "  if (t.id != 11 || t.extra != 12) { return report(5); }\n"
      "  var b: ref *const Base = t.&;\n"                       /* 向上转换 */
      "  if (b.id != 11) { return report(6); }\n"
      "  var m: ref *Base = t.&;\n"
      "  m.id = 21;\n"
      "  if (t.id != 21) { return report(7); }\n"               /* 同一块存储 */
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 2：成员共享 —— 静态属性是同一份全局变量（不复制） ---- */
TEST_F(StructComposeTest, EmbeddedMemberSharedStorage) {
  expect_verdict(
      "struct Base {\n"
      "  id: i32;\n"
      "  var count: i32 = 0;\n"
      "  func getId(self: ref *const Base): i32 { return self.id; }\n"
      "}\n"
      "struct Test { ...Base; extra: i64; }\n"
      "func main(): i32 {\n"
      "  Base::count = 5;\n"
      "  if (Test::count != 5) { return report(1); }\n"   /* 继承名 → 同一份 */
      "  Test::count = 6;\n"
      "  if (Base::count != 6) { return report(2); }\n"   /* 写回同一份 */
      "  var t: Test = .Test{ .id = 7, .extra = 2 };\n"
      "  if (Test::count != 6) { return report(3); }\n"
      "  if (t.getId() != 7) { return report(4); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 3：传递嵌入（A ← B ← C，深度优先按序展开） ---- */
TEST_F(StructComposeTest, TransitiveEmbed) {
  expect_verdict(
      "struct A { a: i32; }\n"
      "struct B { ...A; b: i32; }\n"
      "struct C { ...B; c: i32; }\n"
      "func takeA(x: ref *const A): i32 { return x.a; }\n"
      "func main(): i32 {\n"
      "  var c: C = .C{ .a = 1, .b = 2, .c = 3 };\n"
      "  if (c.a != 1 || c.b != 2 || c.c != 3) { return report(1); }\n"
      "  if (takeA(c.&) != 1) { return report(2); }\n"   /* C → A 向上转换 */
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 4：嵌入在中间（合法，只是后续字段不参与前缀） ---- */
TEST_F(StructComposeTest, EmbedInMiddle) {
  expect_verdict(
      "struct Base { id: i32; }\n"
      "struct M { x: i32; ...Base; y: i32; }\n"
      "func main(): i32 {\n"
      "  var m: M = .M{ .x = 1, .id = 2, .y = 3 };\n"   /* 字段序 x,id,y */
      "  if (m.x != 1 || m.id != 2 || m.y != 3) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向 5：多个嵌入 + 前后都插入 ---- */
TEST_F(StructComposeTest, MultipleEmbeds) {
  expect_verdict(
      "struct P { p: i32; }\n"
      "struct Q { q: i32; }\n"
      "struct R { ...P; mid: i32; ...Q; }\n"           /* 字段序 p, mid, q */
      "func main(): i32 {\n"
      "  var r: R = .R{ .p = 1, .mid = 2, .q = 3 };\n"
      "  if (r.p != 1 || r.mid != 2 || r.q != 3) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 布局正确性（可观察行为）：对齐、反向写入、数组步长 ---- */
TEST_F(StructComposeTest, LayoutAndAlignment) {
  expect_verdict(
      "struct Base { id: i32; }\n"
      "struct Test { ...Base; extra: i64; }\n"          /* 16 = 4 + (4 填充) + 8 */
      "struct Plain { id: i32; extra: i64; }\n"         /* 手写同样字段 */
      "struct M { x: i32; ...Base; y: i32; }\n"         /* 12 */
      "struct Mixed { ...Base; small: u8; }\n"          /* 8 = 4 + 1 + 3 填充 */
      "func main(): i32 {\n"
      "  if (sizeof(Test) as i32 != 16) { return report(1); }\n"
      "  if (sizeof(Test) as i32 != sizeof(Plain) as i32) { return report(2); }\n"
      "  if (sizeof(M) as i32 != 12) { return report(3); }\n"
      "  if (sizeof(Mixed) as i32 != 8) { return report(4); }\n"
      /* 数组步长 = 组合后布局大小：跨元素写读（偏移错误的可观察证据） */
      "  var arr: [3]Test = .[3]Test{ .Test{ .id = 1, .extra = 10 },"
      " .Test{ .id = 2, .extra = 20 }, .Test{ .id = 3, .extra = 30 } };\n"
      "  var p: ref *const Base = arr[1].&;\n"
      "  if (p.id != 2) { return report(5); }\n"
      "  if (arr[1].extra != 20) { return report(6); }\n"
      "  var p2: ref *const Base = arr[2].&;\n"
      "  if (p2.id != 3) { return report(7); }\n"
      "  if (arr[2].extra != 30) { return report(8); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 负例：`...Base` 前面有字段 → 前缀不成立 → 方法糖自然拒绝 ---- */
TEST_F(StructComposeTest, MethodSugarRejectedWhenNotPrefix) {
  /* t.getId()：接收者 t.& 是 ref *T，T 的字段前缀不是 Base 的字段 → 转换失败 */
  expect_compile_error(
      "struct Base { id: i32; func getId(self: ref *const Base): i32 { return self.id; } }\n"
      "struct Test { x: i32; ...Base; }\n"
      "func main(): i32 { var t: Test = .Test{ .x = 1, .id = 2 };\n"
      "  return t.getId(); }\n");
  /* Test::getId(t.&)：同样因为接收者类型不是 Base 的前缀而失败 */
  expect_compile_error(
      "struct Base { id: i32; func getId(self: ref *const Base): i32 { return self.id; } }\n"
      "struct Test { x: i32; ...Base; }\n"
      "func main(): i32 { var t: Test = .Test{ .x = 1, .id = 2 };\n"
      "  return Test::getId(t.&); }\n");
  /* 显式向上转换也不成立（前缀是 x 而不是 id） */
  expect_compile_error(
      "struct Base { id: i32; }\n"
      "struct Test { x: i32; ...Base; }\n"
      "func main(): i32 { var t: Test = .Test{ .x = 1, .id = 2 };\n"
      "  var b: ref *const Base = t.&; return b.id; }\n");
}

/* ---- 负例：未知 / 非结构体（enum、标量） ---- */
TEST_F(StructComposeTest, BadEmbedTargets) {
  expect_compile_error(
      "struct Test { ...Unknown; }\n"
      "func main(): i32 { return 0; }\n");
  expect_compile_error(
      "enum Color: i32 { Red = 1 }\n"
      "struct Test { ...Color; }\n"
      "func main(): i32 { return 0; }\n");
  expect_compile_error(
      "struct Test { ...i32; }\n"
      "func main(): i32 { return 0; }\n");
  /* 函数名不是类型 */
  expect_compile_error(
      "func f(): i32 { return 1; }\n"
      "struct Test { ...f; }\n"
      "func main(): i32 { return 0; }\n");
}

/* ---- 负例：循环嵌入（不崩溃、不死循环、报错） ---- */
TEST_F(StructComposeTest, CircularEmbed) {
  expect_compile_error(
      "struct X { ...Y; }\n"
      "struct Y { ...X; }\n"
      "func main(): i32 { return 0; }\n");
  expect_compile_error(
      "struct X { ...X; }\n"
      "func main(): i32 { return 0; }\n");
  expect_compile_error(
      "struct A { a: i32; }\n"
      "struct B { ...A; b: i32; }\n"
      "struct C { ...B; c: i32; }\n"
      "struct D { ...C; ...A; ...B; ...D; }\n"   /* 自环 + 传递环 */
      "func main(): i32 { return 0; }\n");
}

/* ---- 负例：成员歧义（两个嵌入都提供同名成员） ---- */
TEST_F(StructComposeTest, AmbiguousMember) {
  /* 两个嵌入都提供 f：R::f / r.f() 都是歧义 */
  expect_compile_error(
      "struct P { func f(self: ref *const P): i32 { return 1; } }\n"
      "struct Q { func f(self: ref *const Q): i32 { return 2; } }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { return R::f; }\n");
  expect_compile_error(
      "struct P { func f(self: ref *const P): i32 { return 1; } }\n"
      "struct Q { func f(self: ref *const Q): i32 { return 2; } }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { var r: R = .R{}; return r.f(); }\n");
  /* 静态属性同名同样歧义 */
  expect_compile_error(
      "struct P { var n: i32 = 1; }\n"
      "struct Q { var n: i32 = 2; }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { return R::n; }\n");
  /* 歧义也覆盖"自己 + 嵌入"之外的路径：嵌入链里两个不同 Base 提供同一名字 */
  expect_compile_error(
      "struct P { func f(self: ref *const P): i32 { return 1; } }\n"
      "struct M { ...P; }\n"
      "struct Q { func f(self: ref *const Q): i32 { return 2; } }\n"
      "struct R { ...M; ...Q; }\n"
      "func main(): i32 { return R::f; }\n");
}

/* ---- 负例：同一个 Base 被重复嵌入 → 字段会重复（拒绝） ---- */
TEST_F(StructComposeTest, DuplicateEmbedSameBase) {
  expect_compile_error(
      "struct Base { id: i32; func getId(self: ref *const Base): i32 { return self.id; } }\n"
      "struct Wrap { ...Base; ...Base; }\n"
      "func main(): i32 { var w: Wrap = .Wrap{ .id = 7 }; return w.id; }\n");
}

/* ---- 正向：组合后的类型与"手写同样字段"的类型可互赋值（结构兼容） ---- */
TEST_F(StructComposeTest, CompatibleWithHandwritten) {
  expect_verdict(
      "struct Base { id: i32; }\n"
      "struct Test { ...Base; extra: i64; }\n"
      "struct Plain { id: i32; extra: i64; }\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{ .id = 7, .extra = 2 };\n"
      "  var p: Plain = t;\n"               /* 字段名+类型+顺序一致 → 结构兼容 */
      "  if (p.id != 7 || p.extra != 2) { return report(1); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 负例：嵌入中字段与自有字段重名（重复字段会被拒绝） ---- */
TEST_F(StructComposeTest, DuplicateFieldNameRejected) {
  expect_compile_error(
      "struct Base { id: i32; }\n"
      "struct Test { id: i32; ...Base; }\n"
      "func main(): i32 { return 0; }\n");
  expect_compile_error(
      "struct P { v: i32; }\n"
      "struct Q { v: i64; }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { return 0; }\n");
}

/* ---- 正向：定义顺序无关 —— Base 声明在 Test **之后**也能组合 ----
 * `...Base` 的依赖由 sema 的组合遍历（依赖序遍历 + 反复尝试）解决：Base 在
 * 同一轮更早的尝试里就绪，或下一轮就绪；两边都不需要"语法树嫁接"。 */
TEST_F(StructComposeTest, BaseDeclaredAfterUser) {
  expect_verdict(
      "struct Test { ...Base; extra: i64; }\n"          /* Base 在后面 */
      "struct Base {\n"
      "  id: i32;\n"
      "  func getId(self: ref *const Base): i32 { return self.id; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{ .id = 7, .extra = 2 };\n"
      "  if (t.getId() != 7) { return report(1); }\n"
      "  if (t.id != 7 || t.extra != 2) { return report(2); }\n"
      "  var b: ref *const Base = t.&;\n"                /* 前缀兼容仍成立 */
      "  if (b.id != 7) { return report(3); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向：嵌入链跨多个定义、书写顺序完全倒置（最长链） ----
 * C→B→A 全部逆序书写：靠"每轮至少完成一个"迭代到不动点。 */
TEST_F(StructComposeTest, ChainWrittenInReverseOrder) {
  expect_verdict(
      "struct C { ...B; c: i32; }\n"
      "struct B { ...A; b: i32; }\n"
      "struct A { a: i32; }\n"
      "func takeA(x: ref *const A): i32 { return x.a; }\n"
      "func main(): i32 {\n"
      "  var c: C = .C{ .a = 1, .b = 2, .c = 3 };\n"
      "  if (c.a != 1 || c.b != 2 || c.c != 3) { return report(1); }\n"
      "  if (takeA(c.&) != 1) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向：菱形嵌入（A→B、A→C、B→D、C→D）不是环，必须被接受 ----
 * D 只有成员（没有实例字段），因此 B、C 各自嵌入 D 时字段表不产生重名；
 * 成员查找按类型指针去重（D 经两条路径只算一次命中），不会误报歧义。 */
TEST_F(StructComposeTest, DiamondMemberInheritance) {
  expect_verdict(
      "struct D { var tag: i32 = 3;\n"
      "  func who(self: ref *const D): i32 { return 9; } }\n"
      "struct B { z: i32; ...D; }\n"
      "struct C { ...D; c: i32; }\n"
      "struct A { ...B; ...C; a: i32; }\n"
      "func main(): i32 {\n"
      /* 字段序：B 的字段（z；D 无实例字段）+ 自有 a —— 构造只列实际存在的 */
      "  var x: A = .A{ .z = 1, .c = 2, .a = 3 };\n"
      "  if (x.z != 1 || x.c != 2 || x.a != 3) { return report(1); }\n"
      "  if (x.who() != 9) { return report(2); }\n"     /* 唯一命中（D 去重） */
      "  if (A::tag != 3) { return report(3); }\n"      /* 传递继承的静态属性 */
      "  D::tag = 8;\n"
      "  if (A::tag != 8 || B::tag != 8 || C::tag != 8) { return report(4); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 负例：菱形里共享的 Base 实例字段经两条路径各出现一次 → 字段冲突 ----
 * 字段不参与覆盖语义，因此这是错误（而不是"后嵌入的覆盖先嵌入的"）。 */
TEST_F(StructComposeTest, DiamondFieldConflictRejected) {
  expect_compile_error(
      "struct D { d: i32; }\n"
      "struct B { ...D; b: i32; }\n"
      "struct C { ...D; c: i32; }\n"
      "struct A { ...B; ...C; a: i32; }\n"   /* d 经 B、C 两条路径各出现一次 */
      "func main(): i32 { return 0; }\n");
}

/* ---- 负例：循环嵌入的三种形态（直接自嵌入 / 双环 / 长环） ---- */
TEST_F(StructComposeTest, SelfEmbedCycle) {
  expect_compile_error(
      "struct X { ...X; }\n"
      "func main(): i32 { return 0; }\n");
}

TEST_F(StructComposeTest, MutualEmbedCycle) {
  expect_compile_error(
      "struct X { ...Y; }\n"
      "struct Y { ...X; }\n"
      "func main(): i32 { return 0; }\n");
}

TEST_F(StructComposeTest, LongEmbedCycle) {
  expect_compile_error(
      "struct A { a: i32; }\n"
      "struct B { ...A; b: i32; }\n"
      "struct C { ...B; c: i32; }\n"
      "struct D { ...C; e: i32; }\n"
      "struct E { ...D; ...A; ...E; }\n"   /* E 自环 + 长链 */
      "func main(): i32 { return 0; }\n");
}

/* ---- 正向：seal 前窗口的写法与"手写同样字段"逐位等价（身份不共享） ----
 * Test（组合）与 Plain（手写）字段一致 → 结构兼容可互赋值；但两者是不同的
 * 具名类型（type_t* 身份不因字段相同而强行合并）。 */
TEST_F(StructComposeTest, ComposedAndHandwrittenStayDistinct) {
  expect_verdict(
      "struct Base { id: i32; }\n"
      "struct Test { ...Base; extra: i64; }\n"
      "struct Plain { id: i32; extra: i64; }\n"
      "func mutate(p: ref *Plain): void { p.extra = 42; }\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{ .id = 7, .extra = 2 };\n"
      "  mutate(t.&);\n"                       /* 结构兼容 → 隐式转换成立 */
      "  if (t.extra != 42) { return report(1); }\n"
      "  var p: Plain = t;\n"
      "  if (p.id != 7 || p.extra != 42) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ===========================================================================
 * attribute 覆盖语义（用户规格）：
 *
 *   "新 struct 在聚合后的 attribute 可以覆盖原来的（重写，但是字段不允许覆盖，
 *    冲突报错），也就是一条隐含的规则，在 seal 之前可以复写编辑类型的 attribute。"
 *
 * 口径（attribute = 结构体字段 + `var` 静态属性 + `func` 方法）：
 *   - 新 struct 自己声明的 attribute **静默覆盖**继承来的同名 attribute——
 *     解析到**自己那一份**（名字优先、自己的胜出），不报歧义、不报重复；
 *   - 覆盖后是**独立的两份**：`Test::count` 与 `Base::count` 各自独立，
 *     写一个不影响另一个；
 *   - 自己**没有**声明时才继承：解析到 `Base::name`（静态属性是同一份全局变量）；
 *   - 歧义只保留在"自己没有、而多个嵌入都提供同名 attribute"的情形；
 *   - **字段是唯一例外**：字段不允许覆盖，重名一律报错。
 * =========================================================================== */

/* ---- 正向：`var` 覆盖 —— Test 自己的 count 静默胜出，且与 Base 各一份 ---- */
TEST_F(StructComposeTest, OwnVarOverridesInheritedVar) {
  expect_verdict(
      "struct Base { var count: i32 = 0; }\n"
      "struct Test { ...Base; var count: i32 = 7; }\n"
      "func main(): i32 {\n"
      "  if (Test::count != 7) { return report(1); }\n"   /* 自己的那一份 */
      "  if (Base::count != 0) { return report(2); }\n"   /* Base 不受影响 */
      "  Test::count = 9;\n"                              /* 写自己的 */
      "  if (Test::count != 9) { return report(3); }\n"
      "  if (Base::count != 0) { return report(4); }\n"   /* 仍独立（不共享） */
      "  Base::count = 11;\n"                             /* 写 Base 的 */
      "  if (Test::count != 9) { return report(5); }\n"   /* 自己的不受影响 */
      "  if (Base::count != 11) { return report(6); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向：`func` 覆盖 —— `t.f()` 取 Test 的；显式 `Base::f(...)` 仍取 Base 的 ---- */
TEST_F(StructComposeTest, OwnFuncOverridesInheritedFunc) {
  expect_verdict(
      "struct Base {\n"
      "  func f(self: ref *const Base): i32 { return 1; }\n"
      "}\n"
      "struct Test {\n"
      "  ...Base;\n"
      "  func f(self: ref *const Test): i32 { return 2; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{};\n"
      "  if (t.f() != 2) { return report(1); }\n"          /* 方法糖 → Test::f */
      "  if (Test::f(t.&) != 2) { return report(2); }\n"   /* 显式限定 → Test::f */
      "  if (Base::f(t.&) != 1) { return report(4); }\n"   /* 显式 Base::f 仍取 Base */
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向：显式 Base::f 与 t.f() 在**同一对象**上是两份不同实现 ---- */
TEST_F(StructComposeTest, OverriddenFuncKeepsBaseReachable) {
  expect_verdict(
      "struct Base { v: i32; func get(self: ref *const Base): i32 { return self.v; } }\n"
      "struct Test { ...Base; func get(self: ref *const Test): i32 { return 100 + self.v; } }\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{ .v = 5 };\n"
      "  if (t.get() != 105) { return report(1); }\n"
      "  if (Test::get(t.&) != 105) { return report(2); }\n"
      "  if (Base::get(t.&) != 5) { return report(3); }\n"  /* 向上转换后调 Base 的实现 */
      "  var b: ref *const Base = t.&;\n"
      "  if (b.get() != 5) { return report(4); }\n"        /* 经基指针：静态派发到 Base */
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向：不覆盖时共享（继承查找 → 同一份存储） ---- */
TEST_F(StructComposeTest, UnowneredMemberStillShared) {
  expect_verdict(
      "struct Base { var count: i32 = 0; }\n"
      "struct Test { ...Base; extra: i32; }\n"           /* Test 不声明 count */
      "func main(): i32 {\n"
      "  Test::count = 6;\n"
      "  if (Base::count != 6) { return report(1); }\n"   /* 同一份全局变量 */
      "  Base::count = 8;\n"
      "  if (Test::count != 8) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向：自己声明后歧义消失（覆盖优先于歧义） ---- */
TEST_F(StructComposeTest, OwnMemberSilencesAmbiguity) {
  /* 两个嵌入都提供 f、count；Test 自己声明两者 → 解析到自己的，不报歧义 */
  expect_verdict(
      "struct P { var count: i32 = 1; func f(self: ref *const P): i32 { return 1; } }\n"
      "struct Q { var count: i32 = 2; func f(self: ref *const Q): i32 { return 2; } }\n"
      "struct R {\n"
      "  ...P; ...Q;\n"
      "  var count: i32 = 3;\n"
      "  func f(self: ref *const R): i32 { return 3; }\n"
      "}\n"
      "func main(): i32 {\n"
      "  if (R::count != 3) { return report(1); }\n"
      "  if (P::count != 1 || Q::count != 2) { return report(2); }\n"  /* 各自独立一份 */
      "  R::count = 30;\n"
      "  if (P::count != 1 || Q::count != 2) { return report(3); }\n"
      "  var r: R = .R{};\n"
      "  if (r.f() != 3) { return report(4); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 正向：跨 kind 覆盖 —— 名字优先、自己的胜出（kind 不同也以自己为准） ---- */
TEST_F(StructComposeTest, CrossKindOverrideResolvesToOwn) {
  /* Test 用 func count 覆盖 Base 的 var count：`t.count()` = Test 的函数，
     不再解析到 Base 的属性（也不报歧义/重复） */
  expect_verdict(
      "struct Base { var count: i32 = 5; }\n"
      "struct Test { ...Base; func count(self: ref *const Test): i32 { return 8; } }\n"
      "func main(): i32 {\n"
      "  var t: Test = .Test{};\n"
      "  if (t.count() != 8) { return report(1); }\n"    /* 自己的 func 胜出 */
      "  if (Base::count != 5) { return report(2); }\n"  /* Base 的属性仍在自己那份 */
      "  return report(0);\n"
      "}\n");
  /* 反向：Test 用 var f 覆盖 Base 的 func f —— `Test::f` 读 Test 的变量 */
  expect_verdict(
      "struct Base { func f(self: ref *const Base): i32 { return 5; } }\n"
      "struct Test { ...Base; var f: i32 = 8; }\n"
      "func main(): i32 {\n"
      "  if (Test::f != 8) { return report(1); }\n"      /* 自己的 var 胜出 */
      "  Test::f = 9;\n"
      "  if (Test::f != 9) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
}

/* ---- 负例：自己没声明 → 两个嵌入同名仍歧义（静态属性 / 方法 / 字段读） ---- */
TEST_F(StructComposeTest, AmbiguityRemainsWithoutOwnDeclaration) {
  expect_compile_error(
      "struct P { func f(self: ref *const P): i32 { return 1; } }\n"
      "struct Q { func f(self: ref *const Q): i32 { return 2; } }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { var r: R = .R{}; return r.f(); }\n");
  expect_compile_error(
      "struct P { var n: i32 = 1; }\n"
      "struct Q { var n: i32 = 2; }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { return R::n; }\n");
  expect_compile_error(
      "struct P { var n: i32 = 1; }\n"
      "struct Q { var n: i32 = 2; }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { R::n = 5; return 0; }\n");
}

/* ---- 负例：跨 kind 覆盖不静默回退（用点处明确报"是变量 / 是方法"） ---- */
TEST_F(StructComposeTest, CrossKindOverrideIsAUseSiteError) {
  /* Test 自己声明了 var f（覆盖 Base 的 func f）→ `t.f()` 不能调用它 */
  expect_compile_error(
      "struct Base { func f(self: ref *const Base): i32 { return 5; } }\n"
      "struct Test { ...Base; var f: i32 = 8; }\n"
      "func main(): i32 { var t: Test = .Test{}; return t.f(); }\n");
  /* Test 自己声明了 func f（覆盖 Base 的 var f）→ `Test::f = v` 不能赋值 */
  expect_compile_error(
      "struct Base { var f: i32 = 5; }\n"
      "struct Test { ...Base; func f(self: ref *const Test): i32 { return 8; } }\n"
      "func main(): i32 { Test::f = 3; return 0; }\n");
  /* 成员当字段读（不带 `()`）也不行 */
  expect_compile_error(
      "struct Base { var n: i32 = 1; }\n"
      "struct Test { ...Base; }\n"
      "func main(): i32 { var t: Test = .Test{}; return t.n; }\n");
}

/* ---- 负例：字段是唯一例外 —— 覆盖语义不适用于字段，重名一律报错 ---- */
TEST_F(StructComposeTest, FieldsNeverOverridable) {
  /* 自有字段 vs 继承字段 */
  expect_compile_error(
      "struct Base { id: i32; }\n"
      "struct Test { id: i32; ...Base; }\n"
      "func main(): i32 { return 0; }\n");
  /* 继承字段 vs 继承字段（两个嵌入同名） */
  expect_compile_error(
      "struct P { v: i32; }\n"
      "struct Q { v: i64; }\n"
      "struct R { ...P; ...Q; }\n"
      "func main(): i32 { return 0; }\n");
  /* 自有字段也不能"覆盖"嵌入字段（顺序反过来一样） */
  expect_compile_error(
      "struct Base { id: i32; }\n"
      "struct Test { ...Base; id: i32; }\n"
      "func main(): i32 { return 0; }\n");
}

/* ---- 正向：只有嵌入字段（无自有字段）的 struct 照样拿到继承成员 ----
 * 这条覆盖一个曾经的缺陷：命名的 `struct P` / `struct Q` 若在 seal 去重时被
 * 合并成同一个 type_t 对象（字段表相同），组合出的 R 的嵌入表会退化成 [X, X]，
 * 成员继承查找按类型去重后只剩一个命中 → 歧义消失、继承成员丢失。 */
TEST_F(StructComposeTest, DistinctNamedStructsAreNotMerged) {
  /* P、Q 字段表都为 0 字段、各自有成员：必须仍是**两个**类型 */
  expect_verdict(
      "struct P { var n: i32 = 1; }\n"
      "struct Q { var n: i32 = 2; }\n"
      "func main(): i32 {\n"
      "  if (P::n != 1 || Q::n != 2) { return report(1); }\n"  /* 各一份、名不串 */
      "  P::n = 10;\n"
      "  if (Q::n != 2) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
  /* 单嵌入且无自有字段：`t.f()` 必须仍然可用（继承查找依赖嵌入表存续） */
  expect_verdict(
      "struct Base { func f(self: ref *const Base): i32 { return 7; } }\n"
      "struct Test { ...Base; }\n"                       /* 只有嵌入字段 */
      "func main(): i32 {\n"
      "  var t: Test = .Test{};\n"
      "  if (t.f() != 7) { return report(1); }\n"
      "  if (Test::f(t.&) != 7) { return report(2); }\n"
      "  return report(0);\n"
      "}\n");
  /* 嵌入表不同的两个 struct 不能被字段去重合并：只嵌 D 的 W 不能"继承"到
     别的同字段类型的方法 */
  expect_compile_error(
      "struct D { func only(self: ref *const D): i32 { return 1; } }\n"
      "struct W { ...D; }\n"
      "struct Plain { }\n"                               /* 与 W 字段表相同 */
      "func main(): i32 { var p: Plain = .Plain{}; return p.only(); }\n");
}

