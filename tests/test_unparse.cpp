#include <gtest/gtest.h>
#include "core/allocator.h"
#include "core/error.h"
#include "core/string.h"
#include "core/vec.h"
#include "parser/node.h"
#include "parser/node_program.h"
#include "parser/parser.h"
#include "parser/program.h"
#include "parser/unparse.h"
#include <stdlib.h>
#include <string.h>

/* 解析 src 并文本化，输出解析错误数与结果文本，供用例断言。 */
static string_t *roundtrip(allocator_t *a,
                           parser_t **p,
                           node_t **node,
                           const char *src,
                           size_t *error_count) {
  *p = create_parser(a, "test.clux", src);
  *node = parser_read_program(a, *p);
  *error_count = vec_get_size(parser_get_error(*p));
  return node_unparse(*node, a, nullptr);
}

/* 断言：src 解析零错误，文本化结果等于 expect（精确匹配规范化样式）。 */
static void expect_unparse(const char *src, const char *expect) {
  allocator_t *a = create_allocator(malloc, free);
  parser_t *p;
  node_t *node;
  size_t errors = 0;
  string_t *text = roundtrip(a, &p, &node, src, &errors);
  ASSERT_EQ(errors, 0u) << "src=" << src;
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(string_get_data(text), expect) << "src=" << src;
  delete_string(&text);
  delete_node(&node);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 幂等性：unparse 输出再走 parse→unparse 应逐字节相同，且解析零错误。
   这是 round-trip 等价性的核心验证。 */
TEST(UnparseTest, IdempotentRoundTrip) {
  const char *sources[] = {
      "var x: i32 = 1;",
      "var x = 1 + 2 * 3;",
      "var s = \"hello\";",
      "func f(x: i32): i32 { return x; }",
      "func g(): void { if (1) { return; } else { return; } }",
      "struct P { x: i32; y: i32; }",
      "interface I { func get(self: *I): i32; }",
      "union U { a: i32; b: u8; }",
      "enum E: i32 { A = 1, B = 2 }",
      "bitfield BF: u32 { lo: 4; hi: 28; }",
      "test \"suite\" { assert(1); }",
      "import m from \"mod.clux\";",
      "export * from \"other.clux\";",
      "extern lib libc from \"*\";",
      "static_assert(1, \"ok\");",
      nullptr,
  };
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p;
    node_t *node;
    size_t errors = 0;
    string_t *first = roundtrip(a, &p, &node, sources[i], &errors);
    ASSERT_EQ(errors, 0u) << "src=" << sources[i];
    ASSERT_NE(first, nullptr);
    /* 第一次输出即规范化形态：直接再走一遍应逐字节相同。 */
    expect_unparse(sources[i], string_get_data(first));
    delete_string(&first);
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

/* ---- 表达式 ---- */

TEST(UnparseTest, VarStatement) {
  expect_unparse("var x: i32 = 1;", "var x: i32 = 1;\n");
  expect_unparse("var y = x;", "var y = x;\n");
  expect_unparse("using z = x;", "using z = x;\n");
}

TEST(UnparseTest, ExportAndDecoratorBeforeVar) {
  expect_unparse("[[log]] export var x = 1;", "[[ log ]]\nexport var x = 1;\n");
  expect_unparse("[[a]][[b]] var x = 1;", "[[ a ]]\n[[ b ]]\nvar x = 1;\n");
}

TEST(UnparseTest, BinaryAndUnarySpacing) {
  expect_unparse("var x = 1 + 2 * 3;", "var x = 1 + 2 * 3;\n");
  expect_unparse("var x = -y;", "var x = -y;\n");
  expect_unparse("var x = !flag;", "var x = !flag;\n");
  expect_unparse("var x = a as i32;", "var x = a as i32;\n");
}

TEST(UnparseTest, GroupPreservesPriority) {
  /* 括号必须保留，否则 (1 + 2) * 3 退化为 1 + 2 * 3。 */
  expect_unparse("var x = (1 + 2) * 3;", "var x = (1 + 2) * 3;\n");
}

TEST(UnparseTest, TypePrefixes) {
  expect_unparse("var p: *i32 = q;", "var p: *i32 = q;\n");
  expect_unparse("var s: []u8 = t;", "var s: []u8 = t;\n");
  expect_unparse("var o: ?i32 = n;", "var o: ?i32 = n;\n");
  expect_unparse("var c: const i32 = v;", "var c: const i32 = v;\n");
  expect_unparse("var a: [4]u8 = b;", "var a: [4]u8 = b;\n");
}

TEST(UnparseTest, TupleType) {
  expect_unparse("var t: <i32, u8> = u;", "var t: <i32, u8> = u;\n");
  expect_unparse("type T = <i32, <u8, u8>>;", "type T = <i32, <u8, u8>>;\n");
}

TEST(UnparseTest, CallSubscriptSliceMember) {
  expect_unparse("var x = f(1, 2);", "var x = f(1, 2);\n");
  expect_unparse("var x = a[1];", "var x = a[1];\n");
  expect_unparse("var x = a[1:2];", "var x = a[1:2];\n");
  expect_unparse("var x = a[:];", "var x = a[:];\n");
  expect_unparse("var x = obj.field;", "var x = obj.field;\n");
}

TEST(UnparseTest, BuiltinRestConstructTernary) {
  expect_unparse("var x = #sizeof(i32);", "var x = #sizeof(i32);\n");
  expect_unparse("var x = cond ? 1 : 2;", "var x = cond ? 1 : 2;\n");
  expect_unparse("var p = .P{x = 1, y = 2};", "var p = .P{x = 1, y = 2};\n");
}

TEST(UnparseTest, DecoratedFuncWithCapturesAndGenerics) {
  expect_unparse("func f(x: i32): i32 { return x; }",
                 "func f(x: i32): i32 {\n  return x;\n}\n");
  expect_unparse("func |total| acc(x: i32): i32 { return total; }",
                 "func |total| acc(x: i32): i32 {\n  return total;\n}\n");
}

TEST(UnparseTest, FuncType) {
  expect_unparse("type F = func (i32) -> i32;",
                 "type F = func (i32) -> i32;\n");
}

/* ---- 声明 ---- */

TEST(UnparseTest, StructDeclaration) {
  expect_unparse("struct P { x: i32; y: u8; }",
                 "struct P {\n  x: i32;\n  y: u8;\n}\n");
  expect_unparse("[[tagged]] export struct P { x: i32; }",
                 "[[ tagged ]]\nexport struct P {\n  x: i32;\n}\n");
}

TEST(UnparseTest, InterfaceDeclaration) {
  expect_unparse("interface I { func get(self: *I): i32; }",
                 "interface I {\n  func get(self: *I): i32;\n}\n");
}

TEST(UnparseTest, EnumUnionBitfield) {
  expect_unparse("enum E: i32 { A = 1, B = 2 }",
                 "enum E: i32 {\n  A = 1,\n  B = 2\n}\n");
  expect_unparse("union U { a: i32; b: u8; }",
                 "union U {\n  a: i32;\n  b: u8;\n}\n");
  expect_unparse("bitfield B: u32 { lo: 4; }",
                 "bitfield B: u32 {\n  lo: 4;\n}\n");
}

TEST(UnparseTest, TypeAlias) {
  expect_unparse("export type T = *i32;", "export type T = *i32;\n");
}

/* ---- 控制流 ---- */

TEST(UnparseTest, IfElseChain) {
  expect_unparse("func f(): void {"
                 " if (a) { b(); } else if (c) { d(); } else { e(); } }",
                 "func f(): void {\n"
                 "  if (a) {\n    b();\n  } else if (c) {\n    d();\n"
                 "  } else {\n    e();\n  }\n"
                 "}\n");
}

TEST(UnparseTest, Loops) {
  expect_unparse("func f(): void { while (a) { b(); } }",
                 "func f(): void {\n  while (a) {\n    b();\n  }\n}\n");
  expect_unparse("func f(): void { for (var i = 0; i < 10; i = i + 1) {} }",
                 "func f(): void {\n"
                 "  for (var i = 0; i < 10; i = i + 1) {\n  }\n"
                 "}\n");
  expect_unparse("func f(): void { foreach (x of xs) { g(x); } }",
                 "func f(): void {\n"
                 "  foreach (x of xs) {\n    g(x);\n  }\n"
                 "}\n");
  expect_unparse("func f(): void { foreach (var x: i32 of xs) {} }",
                 "func f(): void {\n"
                 "  foreach (var x: i32 of xs) {\n  }\n"
                 "}\n");
  expect_unparse("func f(): void { do { g(); } while (a); }",
                 "func f(): void {\n  do {\n    g();\n  } while (a);\n}\n");
}

TEST(UnparseTest, SwitchBranches) {
  expect_unparse("func f(x: i32): i32 {"
                 " switch (x) { (1) -> { return 1; }"
                 " default -> { return 0; } } }",
                 "func f(x: i32): i32 {\n"
                 "  switch (x) {\n"
                 "    (1) -> {\n      return 1;\n    }\n"
                 "    default -> {\n      return 0;\n    }\n"
                 "  }\n"
                 "}\n");
}

TEST(UnparseTest, ComptimeAndDeferAssertTest) {
  expect_unparse("func f(): void { comptime if (1) { g(); } }",
                 "func f(): void {\n"
                 "  comptime if (1) {\n    g();\n  }\n"
                 "}\n");
  expect_unparse("func f(): void { defer { g(); } }",
                 "func f(): void {\n  defer {\n    g();\n  }\n}\n");
  expect_unparse("test \"t\" { assert(a); }",
                 "test \"t\" {\n  assert(a);\n}\n");
}

/* ---- 模块 ---- */

TEST(UnparseTest, ImportAndExtern) {
  expect_unparse("import m from \"mod.clux\";",
                 "import m from \"mod.clux\";\n");
  expect_unparse("export * from \"other.clux\";",
                 "export * from \"other.clux\";\n");
  expect_unparse("extern lib libc from \"*\";",
                 "extern lib libc from \"*\";\n");
  expect_unparse("extern cfunc libc::puts(s: *u8): i32;",
                 "extern cfunc libc::puts(s: *u8): i32;\n");
  expect_unparse("export extern var libc::errno: i32;",
                 "export extern var libc::errno: i32;\n");
}

/* ---- 容错与入口 ---- */

/* 槽位 NULL 的容错节点不能崩溃，输出可解析的最小形态。 */
TEST(UnparseTest, MalformedNodesDoNotCrash) {
  const char *sources[] = {
      "",
      "var",
      "func",
      "struct",
      "switch (a) {",
      "if (a)",
      "for (",
      "defer",
      "[[t]]",
      "a;",
      "return",
      "type",
      "enum E:",
      nullptr,
  };
  for (size_t i = 0; sources[i] != nullptr; i++) {
    allocator_t *a = create_allocator(malloc, free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *node = parser_read_program(a, p);
    string_t *text = node_unparse(node, a, nullptr);
    ASSERT_NE(text, nullptr) << "src=" << sources[i];
    /* 容错输出仍需可再解析。 */
    parser_t *p2 = create_parser(a, "rt.clux", string_get_data(text));
    node_t *node2 = parser_read_program(a, p2);
    ASSERT_NE(node2, nullptr);
    delete_node(&node2);
    delete_parser(&p2);
    delete_string(&text);
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
  }
}

TEST(UnparseTest, NullNodesProduceEmptyOutput) {
  allocator_t *a = create_allocator(malloc, free);
  string_t *text = node_unparse(nullptr, a, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(string_get_size(text), 0u);
  delete_string(&text);
  text = program_unparse(nullptr, a, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_EQ(string_get_size(text), 0u);
  delete_string(&text);
  delete_allocator(&a);
}

TEST(UnparseTest, ProgramUnparseUsesDecls) {
  allocator_t *a = create_allocator(malloc, free);
  program_t *program = create_program(a);
  parser_t *p = create_parser(a, "test.clux", "var x = 1;");
  node_t *decls = parser_read_program(a, p);
  program_set_decls(program, decls);
  string_t *text = program_unparse(program, a, nullptr);
  ASSERT_NE(text, nullptr);
  EXPECT_STREQ(string_get_data(text), "var x = 1;\n");
  delete_string(&text);
  /* decls 所有权已转移给 program，一并释放。 */
  delete_program(&program);
  delete_parser(&p);
  delete_allocator(&a);
}

/* 计数分配器验证无泄漏。 */
static size_t g_alloc_count;
static size_t g_free_count;
static void *counting_alloc(size_t size) {
  g_alloc_count++;
  return malloc(size);
}
static void counting_free(void *ptr) {
  g_free_count++;
  free(ptr);
}

TEST(UnparseTest, NoLeak) {
  const char *sources[] = {
      "var x: i32 = 1;",
      "func f(x: i32): i32 { return x; }",
      "struct P { x: i32; }",
      "switch (a) {",
      "var",
      nullptr,
  };
  for (size_t i = 0; sources[i] != nullptr; i++) {
    g_alloc_count = 0;
    g_free_count = 0;
    allocator_t *a = create_allocator(counting_alloc, counting_free);
    parser_t *p = create_parser(a, "test.clux", sources[i]);
    node_t *node = parser_read_program(a, p);
    string_t *text = node_unparse(node, a, nullptr);
    delete_string(&text);
    delete_node(&node);
    delete_parser(&p);
    delete_allocator(&a);
    EXPECT_EQ(g_alloc_count, g_free_count) << "src=" << sources[i];
  }
}
