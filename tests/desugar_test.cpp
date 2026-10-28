/*
 * Description: 前脱糖层（parser → sema 之间的纯 AST 变换）单元测试
 * Create: 2026-10
 *
 * 这些用例**不经过 sema**：直接 lex → parse → desugar_program，然后对 AST
 * 形状断言。这正是前脱糖层"只认识语法形状、可独立测试"的契约（见
 * include/desugar/desugar.h）。
 *
 * 覆盖：
 *   - foreach 展开形状（裸标识符迭代器 / 其他表达式迭代器两种形态）；
 *   - `var` 形态（每轮声明）与无 `var` 形态（写回已定义变量）；
 *   - 显式类型标注；步进/比较/赋值运算符来自合成 token；下标类型 usize；
 *   - 嵌套 foreach / 闭包体内的 foreach 同样被展开；
 *   - 展开后**整个程序不再有 AST_FOREACH**（sema/compiler/CTFE 零感知）。
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/stream.h"
#include "core/vec.h"
#include "desugar/desugar.h"
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_call.h"
#include "parser/ast_comptime_if.h"
#include "parser/ast_defer.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_return.h"
#include "parser/ast_switch.h"
#include "parser/ast_var_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"
#include "parser/parser.h"
}

#include "test_common.h"

namespace {

/* ================================================================ */
/* lex / parse / desugar 管道                                        */
/* ================================================================ */

struct Lexed {
  vec_t   *tokens = nullptr;
  lexer_t *lexer = nullptr;
  char    *src = nullptr;
  size_t   len = 0;
};

/* token pool + 源 buffer 由测试持有（AST 的名字切片指向源 buffer）。 */
Lexed lex_source(allocator_t *alloc, const char *text) {
  Lexed out;
  out.len = strlen(text);
  out.src = (char *)malloc(out.len + 1);
  memcpy(out.src, text, out.len + 1);

  stream_source_t mem = stream_source_mem(alloc, out.src, out.len, false);
  istream_t *stream = istream_open(alloc, mem);
  out.lexer = lexer_create(alloc, stream, "desugar_test.cx");
  out.tokens = vec_new(alloc, true);
  for (;;) {
    token_t *t = lexer_next(out.lexer);
    token_kind_t k = token_get_kind(t);
    vec_push(out.tokens, alloc, t);
    if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
  }
  return out;
}

void lexed_free(allocator_t *alloc, Lexed &lx) {
  lexer_close(&lx.lexer); /* dispose 关闭其 stream */
  vec_free(alloc, &lx.tokens);
  free(lx.src);
  lx.src = nullptr;
}

std::string sv(strslice_t s) { return std::string(s.ptr ? s.ptr : "", s.len); }

std::string tok_text(const token_t *t) {
  size_t len = 0;
  const char *p = t ? token_get_text(t, &len) : nullptr;
  return std::string(p ? p : "", len);
}

/* ================================================================ */
/* 语句 / 表达式遍历辅助（独立实现，不复用 desugar 的 walker）          */
/* ================================================================ */

bool chain_has_foreach(ast_node_t *n);
bool scan_has_foreach(ast_node_t *n);

ast_walk_action_t scan_expr_visit(ast_node_t *n, void *user) {
  bool *found = (bool *)user;
  if (*found) return AST_WALK_STOP;
  if (n && n->kind == AST_FUNC_DEF) {
    ast_func_def_t *fn = (ast_func_def_t *)n;
    if (chain_has_foreach(fn->captures) || chain_has_foreach(fn->params) ||
        scan_has_foreach(fn->body)) {
      *found = true;
      return AST_WALK_STOP;
    }
  }
  return AST_WALK_CONTINUE;
}

bool expr_has_foreach(ast_node_t *e) {
  if (!e) return false;
  bool found = false;
  ast_walk_preorder(e, scan_expr_visit, &found);
  return found;
}

bool chain_has_foreach(ast_node_t *n) {
  for (; n; n = n->next)
    if (scan_has_foreach(n)) return true;
  return false;
}

/* 函数字面量 / defer 闭包 / 局部函数：捕获、形参、函数体都要看。 */
bool func_has_foreach(ast_func_def_t *fn) {
  if (!fn) return false;
  return chain_has_foreach(fn->captures) ||
         chain_has_foreach(fn->params) || scan_has_foreach(fn->body);
}

bool scan_has_foreach(ast_node_t *n) {
  if (!n) return false;
  switch (n->kind) {
    case AST_FOREACH:
      return true;
    case AST_BLOCK:
      return chain_has_foreach(((ast_block_t *)n)->stmts);
    case AST_IF: {
      ast_if_t *it = (ast_if_t *)n;
      return expr_has_foreach(it->cond) || scan_has_foreach(it->then_body) ||
             scan_has_foreach(it->else_body);
    }
    case AST_COMPTIME_IF: {
      ast_comptime_if_t *ci = (ast_comptime_if_t *)n;
      return expr_has_foreach(ci->cond) || scan_has_foreach(ci->then_body) ||
             scan_has_foreach(ci->else_body);
    }
    case AST_WHILE: {
      ast_while_t *w = (ast_while_t *)n;
      return expr_has_foreach(w->cond) || scan_has_foreach(w->body);
    }
    case AST_DOWHILE: {
      ast_dowhile_t *d = (ast_dowhile_t *)n;
      return expr_has_foreach(d->cond) || scan_has_foreach(d->body);
    }
    case AST_FOR: {
      ast_for_t *f = (ast_for_t *)n;
      return scan_has_foreach(f->init) || expr_has_foreach(f->cond) ||
             expr_has_foreach(f->update) || scan_has_foreach(f->body);
    }
    case AST_SWITCH: {
      ast_switch_t *sw = (ast_switch_t *)n;
      if (expr_has_foreach(sw->cond)) return true;
      for (ast_node_t *c = sw->cases; c; c = c->next) {
        ast_switch_case_t *sc = (ast_switch_case_t *)c;
        if (expr_has_foreach(sc->patterns) || scan_has_foreach(sc->body))
          return true;
      }
      return scan_has_foreach(sw->default_body);
    }
    case AST_RETURN:
      return expr_has_foreach(((ast_return_t *)n)->value);
    case AST_EXPR_STMT:
      return expr_has_foreach(((ast_expr_stmt_t *)n)->expr);
    case AST_VAR_DEF: {
      ast_var_def_t *v = (ast_var_def_t *)n;
      return expr_has_foreach(v->type_expr) || expr_has_foreach(v->init);
    }
    case AST_ASSIGN: {
      ast_assign_t *a = (ast_assign_t *)n;
      return expr_has_foreach(a->target) || expr_has_foreach(a->value);
    }
    case AST_DEFER:
      return func_has_foreach(((ast_defer_t *)n)->closure);
    case AST_FUNC_DEF:
      return func_has_foreach((ast_func_def_t *)n);
    default:
      return expr_has_foreach(n);
  }
}

/* ================================================================ */
/* Fixture                                                          */
/* ================================================================ */

class DesugarTest : public ::testing::Test {
 protected:
  void SetUp() override {
    alloc_ = create_allocator(malloc, free);
    arena_ = arena_new_default(alloc_);
  }

  void TearDown() override {
    lexed_free(alloc_, lexed_);
    arena_destroy(alloc_, &arena_);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc_);
  }

  /* lex → parse → desugar_program；返回 program 节点。 */
  ast_node_t *analyze(const char *src) {
    lexed_ = lex_source(alloc_, src);
    parser_t *p = parser_create(alloc_, arena_, lexed_.tokens);
    EXPECT_NE(p, nullptr);
    ast_node_t *root = parser_parse(p);
    parser_destroy(&p);
    EXPECT_NE(root, nullptr);
    EXPECT_EQ(root ? root->kind : AST_ERROR, AST_PROGRAM);
    if (!root) return nullptr;
    desugar_t ds = desugar_make(arena_, nullptr, lexed_.tokens);
    desugar_program(&ds, (ast_program_t *)root);
    return root;
  }

  /* 第 idx 个顶层函数的函数体块。 */
  ast_node_t *func_body(ast_node_t *prog, size_t idx) {
    ast_node_t *f = ((ast_program_t *)prog)->funcs;
    while (f && idx-- > 0) f = f->next;
    if (!f || f->kind != AST_FUNC_DEF) return nullptr;
    return ((ast_func_def_t *)f)->body;
  }

  /* `var` 形态的元素绑定断言：`var <name> [: T] = <init>` */
  void expect_var_def(ast_node_t *n, const char *name) {
    ASSERT_NE(n, nullptr);
    ASSERT_EQ(n->kind, AST_VAR_DEF);
    EXPECT_EQ(sv(((ast_var_def_t *)n)->name), name);
  }

  allocator_t *alloc_ = nullptr;
  arena_t     *arena_ = nullptr;
  Lexed        lexed_;
};

ast_node_t *nth_stmt(ast_node_t *block, size_t idx) {
  if (!block || block->kind != AST_BLOCK) return nullptr;
  ast_node_t *s = ((ast_block_t *)block)->stmts;
  while (s && idx-- > 0) s = s->next;
  return s;
}

bool is_ident(ast_node_t *n, const char *name) {
  return n && n->kind == AST_IDENT && sv(((ast_ident_t *)n)->name) == name;
}

/* `for` 的 init：`var __idx_N: usize = 0u64` */
void expect_idx_init(ast_node_t *for_node, const char *idx_name) {
  ASSERT_NE(for_node, nullptr);
  ASSERT_EQ(for_node->kind, AST_FOR);
  ast_node_t *init = ((ast_for_t *)for_node)->init;
  ASSERT_NE(init, nullptr);
  ASSERT_EQ(init->kind, AST_VAR_DEF);
  ast_var_def_t *vd = (ast_var_def_t *)init;
  EXPECT_EQ(sv(vd->name), idx_name);
  ASSERT_NE(vd->type_expr, nullptr);
  EXPECT_TRUE(is_ident(vd->type_expr, "usize"));
  ASSERT_NE(vd->init, nullptr);
  ASSERT_EQ(vd->init->kind, AST_INT_LIT);
  EXPECT_EQ(((ast_int_lit_t *)vd->init)->value, 0u);
  /* 字面量后缀必须归一化成平台宽度名（compiler 只认 u64 / u32） */
  const char *want = sizeof(void *) == 8 ? "u64" : "u32";
  EXPECT_EQ(sv(((ast_int_lit_t *)vd->init)->type), want);
}

/* `for` 的条件：`__idx_N < len(<iter_name>)` */
void expect_cond(ast_node_t *for_node, const char *idx_name,
                 const char *iter_name) {
  ast_node_t *cond = ((ast_for_t *)for_node)->cond;
  ASSERT_NE(cond, nullptr);
  ASSERT_EQ(cond->kind, AST_BINARY);
  ast_binary_t *b = (ast_binary_t *)cond;
  EXPECT_EQ(tok_text(b->op), "<");
  EXPECT_TRUE(is_ident(b->lhs, idx_name));
  ASSERT_NE(b->rhs, nullptr);
  ASSERT_EQ(b->rhs->kind, AST_CALL);
  ast_call_t *call = (ast_call_t *)b->rhs;
  EXPECT_TRUE(is_ident(call->callee, "len"));
  EXPECT_TRUE(is_ident(call->args, iter_name));
  EXPECT_EQ(call->args_last, call->args);
  EXPECT_EQ(call->args ? call->args->next : nullptr, nullptr);
}

/* `for` 的步进：`__idx_N = __idx_N + 1u64` */
void expect_update(ast_node_t *for_node, const char *idx_name) {
  ast_node_t *upd = ((ast_for_t *)for_node)->update;
  ASSERT_NE(upd, nullptr);
  ASSERT_EQ(upd->kind, AST_ASSIGN);
  ast_assign_t *a = (ast_assign_t *)upd;
  EXPECT_EQ(tok_text(a->op), "=");
  EXPECT_TRUE(is_ident(a->target, idx_name));
  ASSERT_NE(a->value, nullptr);
  ASSERT_EQ(a->value->kind, AST_BINARY);
  ast_binary_t *sum = (ast_binary_t *)a->value;
  EXPECT_EQ(tok_text(sum->op), "+");
  EXPECT_TRUE(is_ident(sum->lhs, idx_name));
  ASSERT_NE(sum->rhs, nullptr);
  ASSERT_EQ(sum->rhs->kind, AST_INT_LIT);
  EXPECT_EQ(((ast_int_lit_t *)sum->rhs)->value, 1u);
}

/* 循环体第一条语句：`[var] <elem> = <iter>[__idx_N]` */
void expect_elem_binding(ast_node_t *for_node, const char *elem_name,
                         bool is_decl, const char *iter_name,
                         const char *idx_name) {
  ast_node_t *body = ((ast_for_t *)for_node)->body;
  ast_node_t *bind = nth_stmt(body, 0);
  ASSERT_NE(bind, nullptr);
  ast_node_t *init = nullptr;
  if (is_decl) {
    ASSERT_EQ(bind->kind, AST_VAR_DEF);
    EXPECT_EQ(sv(((ast_var_def_t *)bind)->name), elem_name);
    init = ((ast_var_def_t *)bind)->init;
  } else {
    ASSERT_EQ(bind->kind, AST_ASSIGN);
    EXPECT_TRUE(is_ident(((ast_assign_t *)bind)->target, elem_name));
    EXPECT_EQ(tok_text(((ast_assign_t *)bind)->op), "=");
    init = ((ast_assign_t *)bind)->value;
  }
  ASSERT_NE(init, nullptr);
  ASSERT_EQ(init->kind, AST_INDEX);
  ast_index_t *ix = (ast_index_t *)init;
  EXPECT_TRUE(is_ident(ix->object, iter_name));
  EXPECT_TRUE(is_ident(ix->indices, idx_name));
  EXPECT_EQ(ix->indices_last, ix->indices);
}

} // namespace

/* ================================================================ */
/* 展开形状                                                         */
/* ================================================================ */

/**
 * 裸标识符迭代器（`foreach (var e of v)`）：不需要临时变量——语句链上直接
 * 是 for。容器因此可遍历（`var __arr = v` 会被 sema 拒为 move-only 拷贝）。
 */
TEST_F(DesugarTest, IdentIterableExpandsToBareFor) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  var s = 0;\n"
      "  foreach (var e of v) { s = s + e; }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);

  ast_node_t *body = func_body(prog, 0);
  ASSERT_NE(body, nullptr);
  ast_node_t *st = nth_stmt(body, 2);
  ASSERT_NE(st, nullptr);
  ASSERT_EQ(st->kind, AST_FOR);
  EXPECT_EQ(nth_stmt(body, 3), nullptr);

  expect_idx_init(st, "__idx_0");
  expect_cond(st, "__idx_0", "v");
  expect_update(st, "__idx_0");
  expect_elem_binding(st, "e", /*is_decl=*/true, "v", "__idx_0");
}

/**
 * 非裸标识符迭代器（`foreach (var e of a[1:3])`）：先求值到临时变量——
 * 包一层块把 `__arr_N` 圈在循环内，条件/下标都读临时变量。
 */
TEST_F(DesugarTest, ExprIterableExpandsToBlockWithTemp) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var a = .[4]i32{ 1, 2, 3, 4 };\n"
      "  var s = 0;\n"
      "  foreach (var e of a[1:3]) { s = s + e; }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);

  ast_node_t *body = func_body(prog, 0);
  ast_node_t *outer = nth_stmt(body, 2);
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(outer->kind, AST_BLOCK);

  ast_node_t *arr_def = nth_stmt(outer, 0);
  expect_var_def(arr_def, "__arr_0");
  EXPECT_EQ(((ast_var_def_t *)arr_def)->type_expr, nullptr);
  ASSERT_NE(((ast_var_def_t *)arr_def)->init, nullptr);
  EXPECT_EQ(((ast_var_def_t *)arr_def)->init->kind, AST_SLICE);

  ast_node_t *f = nth_stmt(outer, 1);
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(f->kind, AST_FOR);
  EXPECT_EQ(nth_stmt(outer, 2), nullptr);
  expect_idx_init(f, "__idx_0");
  expect_cond(f, "__idx_0", "__arr_0");
  expect_update(f, "__idx_0");
  expect_elem_binding(f, "e", /*is_decl=*/true, "__arr_0", "__idx_0");
}

/* 无 `var` 形态：每轮写回**已定义**变量（AST_ASSIGN，而不是声明）。 */
TEST_F(DesugarTest, AssignmentFormBindsExistingVariable) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  var e = 0;\n"
      "  foreach (e of v) { }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);

  ast_node_t *body = func_body(prog, 0);
  ast_node_t *f = nth_stmt(body, 2);
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(f->kind, AST_FOR);
  expect_elem_binding(f, "e", /*is_decl=*/false, "v", "__idx_0");
}

/* 显式类型标注原样搬运到元素声明上（元素遍历必需 `: *T`）。 */
TEST_F(DesugarTest, ExplicitElementTypeIsPreserved) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  foreach (var e: *i32 of v) { }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);

  ast_node_t *f = nth_stmt(func_body(prog, 0), 1);
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(f->kind, AST_FOR);
  ast_node_t *bind = nth_stmt(((ast_for_t *)f)->body, 0);
  expect_var_def(bind, "e");
  ast_node_t *te = ((ast_var_def_t *)bind)->type_expr;
  ASSERT_NE(te, nullptr);
  EXPECT_EQ(te->kind, AST_PTR);
}

/* 多个 foreach / 嵌套 foreach：序号唯一，内层也被展开。 */
TEST_F(DesugarTest, NestedForeachExpandsRecursivelyWithDistinctNames) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  foreach (var x of v) {\n"
      "    foreach (var y of v) { }\n"
      "  }\n"
      "  foreach (var z of v) { }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);

  ast_node_t *body = func_body(prog, 0);
  ast_node_t *outer = nth_stmt(body, 1);
  ASSERT_NE(outer, nullptr);
  ASSERT_EQ(outer->kind, AST_FOR);
  expect_idx_init(outer, "__idx_0");

  /* 外层循环体：[var x = v[__idx_0]; <内层 for>] */
  ast_node_t *outer_body = ((ast_for_t *)outer)->body;
  expect_elem_binding(outer, "x", true, "v", "__idx_0");
  ast_node_t *inner = nth_stmt(outer_body, 1);
  ASSERT_NE(inner, nullptr);
  ASSERT_EQ(inner->kind, AST_FOR);
  expect_idx_init(inner, "__idx_1");
  expect_elem_binding(inner, "y", true, "v", "__idx_1");

  ast_node_t *second = nth_stmt(body, 2);
  ASSERT_NE(second, nullptr);
  ASSERT_EQ(second->kind, AST_FOR);
  expect_idx_init(second, "__idx_2");
  expect_elem_binding(second, "z", true, "v", "__idx_2");
}

/* 闭包体内的 foreach：函数字面量是表达式，walker 必须下钻其函数体。 */
TEST_F(DesugarTest, ForeachInsideClosureBodyExpands) {  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  var f = func |v| (): i32 {\n"
      "    var s = 0;\n"
      "    foreach (var e of v) { s = s + e; }\n"
      "    return s;\n"
      "  };\n"
      "}\n");
  ASSERT_NE(prog, nullptr);

  ast_node_t *fdef = nth_stmt(func_body(prog, 0), 1);
  ASSERT_NE(fdef, nullptr);
  ASSERT_EQ(fdef->kind, AST_VAR_DEF);
  ast_node_t *lit = ((ast_var_def_t *)fdef)->init;
  ASSERT_NE(lit, nullptr);
  ASSERT_EQ(lit->kind, AST_FUNC_DEF);

  ast_node_t *inner = nth_stmt(((ast_func_def_t *)lit)->body, 1);
  ASSERT_NE(inner, nullptr);
  ASSERT_EQ(inner->kind, AST_FOR);
  expect_elem_binding(inner, "e", true, "v", "__idx_0");
}

/* 闭包藏在 `new T{ … }` / `make(T, N, …)` 的字段/实参里：
   ast_walk 必须下钻 AST_NEW / AST_MAKE / AST_SLICE 的子表达式，否则函数字面量
   里的 foreach 漏展开（这两类节点一度漏在 ast_visit_children 之外）。 */
TEST_F(DesugarTest, ForeachInClosureInsideNewAndMakeExpands) {
  ast_node_t *prog = analyze(
      "struct Box { maker: func()->i32; }\n"
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  var b = new Box{ .maker = func |v| (): i32 {\n"
      "    foreach (var e of v) { }\n"
      "    return 0;\n"
      "  } };\n"
      "  var s: []func()->i32 = make(func()->i32, 1, func |v| (): i32 {\n"
      "    foreach (var e of v) { }\n"
      "    return 0;\n"
      "  });\n"
      "}\n");
  ASSERT_NE(prog, nullptr);
  EXPECT_FALSE(scan_has_foreach(prog))
      << "new / make 内的函数字面量没有被下钻（walker 覆盖面缺口）";
}

/* defer 闭包体里的 foreach：同样要展开（defer 的闭包是 AST_FUNC_DEF）。 */
TEST_F(DesugarTest, ForeachInsideDeferBodyExpands) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  defer { foreach (var e of v) { } }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);

  ast_node_t *d = nth_stmt(func_body(prog, 0), 1);
  ASSERT_NE(d, nullptr);
  ASSERT_EQ(d->kind, AST_DEFER);
  ast_func_def_t *fn = ((ast_defer_t *)d)->closure;
  ASSERT_NE(fn, nullptr);
  ast_node_t *f = nth_stmt(fn->body, 0);
  ASSERT_NE(f, nullptr);
  ASSERT_EQ(f->kind, AST_FOR);
}

/* if / while / switch / for 体内的 foreach：所有语句链都被遍历到。 */
TEST_F(DesugarTest, ForeachInNestedStatementKindsExpands) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  if (true) { foreach (var a of v) { } }\n"
      "  while (false) { foreach (var b of v) { } }\n"
      "  for (var i = 0; i < 1; i = i + 1) { foreach (var c of v) { } }\n"
      "  switch (1) { (1) -> { foreach (var d of v) { } } }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);
  EXPECT_FALSE(scan_has_foreach(prog))
      << "desugar 后仍有 AST_FOREACH 残留（sema 会看到未知语句）";
}

/* 顶层无 foreach 的程序：desugar 不改动任何语句。 */
TEST_F(DesugarTest, ProgramWithoutForeachIsUnchanged) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[2]i32{ 1, 2 };\n"
      "  var s = 0;\n"
      "}\n");
  ASSERT_NE(prog, nullptr);
  ast_node_t *body = func_body(prog, 0);
  ASSERT_NE(body, nullptr);
  EXPECT_EQ(nth_stmt(body, 0)->kind, AST_VAR_DEF);
  EXPECT_EQ(nth_stmt(body, 1)->kind, AST_VAR_DEF);
  EXPECT_EQ(nth_stmt(body, 2), nullptr);
  EXPECT_FALSE(scan_has_foreach(prog));
}

/* 展开后程序里不再有 AST_FOREACH（所有形态一并验证）。 */
TEST_F(DesugarTest, NoForeachRemainsAfterDesugar) {
  ast_node_t *prog = analyze(
      "func main(): void {\n"
      "  var v = .[3]i32{ 1, 2, 3 };\n"
      "  var s = 0;\n"
      "  foreach (var a of v) { s = s + a; }\n"
      "  foreach (b of v) { s = s + b; }\n"
      "  foreach (var c of v[0:2]) { s = s + c; }\n"
      "  foreach (var d: *i32 of v) { s = s + d.*; }\n"
      "  var f = func |v| (): i32 {\n"
      "    foreach (var e of v) { return e; }\n"
      "    return 0;\n"
      "  };\n"
      "  if (true) { foreach (var g of v) { } }\n"
      "}\n");
  ASSERT_NE(prog, nullptr);
  EXPECT_FALSE(scan_has_foreach(prog));
}
