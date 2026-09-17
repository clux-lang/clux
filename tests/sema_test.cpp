/*
 * Description: sema (semantic analysis) unit tests
 * Create: 2026-09-08
 *
 * 覆盖：sema 生命周期、三遍扫描（Pass 1 函数名 / Pass 2 签名 / Pass 3a
 * 作用域树 / Pass 3b shadow VM 类型检查）、诊断产出、作用域树结构、
 * VM scope 遮罩、返回路径完整性分析。
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>

extern "C" {
#include "core/allocator.h"
#include "core/arena.h"
#include "core/vec.h"
#include "core/stream.h"
#include "diag/diagnostic.h"
#include "parser/lexer.h"
#include "parser/parser.h"
#include "parser/ast_array.h"
#include "parser/ast_block.h"
#include "parser/ast_construct.h"
#include "parser/ast_func_def.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_node.h"
#include "parser/ast_program.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "sema/sema.h"
#include "sema/symbol.h"
#include "vm/vm.h"
#include "vm/type_array.h"
}

#include "test_common.h"

namespace {

/* ---- Helper: lex source text into a token pool (lexer kept alive) ---- */

struct LexResult {
    vec_t   *tokens;
    lexer_t *lexer;
    char    *source_buf;
};

static LexResult lex_source(allocator_t *alloc, const char *src) {
    LexResult result;
    result.source_buf = (char *)malloc(strlen(src) + 1);
    strcpy(result.source_buf, src);

    stream_source_t mem_src =
        stream_source_mem(alloc, result.source_buf, strlen(src), false);
    istream_t *stream = istream_open(alloc, mem_src);
    result.lexer = lexer_create(alloc, stream, "test.clx");

    result.tokens = vec_new(alloc, true);
    for (;;) {
        token_t *t = lexer_next(result.lexer);
        token_kind_t k = token_get_kind(t);
        vec_push(result.tokens, alloc, t);
        if (k == TOKEN_TYPE_EOF || k == TOKEN_TYPE_ERROR) break;
    }
    return result;
}

static void lex_result_destroy(allocator_t *alloc, LexResult &lr) {
    lexer_close(&lr.lexer);
    vec_free(alloc, &lr.tokens);
    free(lr.source_buf);
    lr.source_buf = nullptr;
}

/* 函数 fscope（捕获层）→ 参数层（fscope 的 child 0）：参数与函数体符号
   挂参数层（镜像运行时 closure_scope → 参数匿名层结构）。 */
static sema_scope_t *func_param_scope(sema_scope_t *fscope) {
    return sema_scope_child(fscope, 0);
}

/* ---- Fixture ---- */

class SemaTest : public ::testing::Test {
protected:
    void SetUp() override {
        alloc_ = create_allocator(malloc, free);
        arena_ = arena_new_default(alloc_);
        vm_    = vm_new(alloc_);
        diag_  = diag_buf_new(alloc_);
    }

    void TearDown() override {
        if (sema_) {
            sema_scope_t *tree = sema_->global_scope;
            sema_destroy(&sema_);
            if (tree) sema_scope_destroy(&tree);
        }
        if (lex_.lexer) lex_result_destroy(alloc_, lex_);
        vm_destroy(&vm_);
        diag_buf_destroy(&diag_);
        arena_destroy(alloc_, &arena_);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc_);
    }

    /** 完整流水线：lex → parse → sema_analyze（lexer 保持存活保证 slice 有效） */
    bool analyze(const char *src) {
        lex_ = lex_source(alloc_, src);

        parser_t *parser = parser_create(alloc_, arena_, lex_.tokens);
        ast_ = parser_parse(parser);
        parser_destroy(&parser);
        if (!ast_ || ast_->kind != AST_PROGRAM) return false;

        sema_ = sema_create(vm_, diag_, lex_.tokens, arena_);
        return sema_analyze(sema_, ast_);
    }

    /** 断言第 i 条诊断消息包含 substr（i 从 0 起） */
    void expect_message(size_t i, const char *substr) const {
        ASSERT_GT(diag_count(diag_), i);
        const diagnostic_t *items = diag_items(diag_);
        ASSERT_NE(items, nullptr);
        EXPECT_NE(std::strstr(items[i].message, substr), nullptr)
            << "message[" << i << "] = " << items[i].message;
    }

    allocator_t *alloc_ = nullptr;
    arena_t     *arena_ = nullptr;
    vm_t        *vm_    = nullptr;
    diag_buf_t  *diag_  = nullptr;
    LexResult    lex_{};
    ast_node_t  *ast_   = nullptr;
    sema_t      *sema_  = nullptr;
};

/* ================================================================ */
/* 生命周期                                                          */
/* ================================================================ */

TEST_F(SemaTest, CreateWithNullArgsReturnsNull) {
    EXPECT_EQ(sema_create(nullptr, diag_, nullptr, arena_), nullptr);
    EXPECT_EQ(sema_create(vm_, nullptr, nullptr, arena_), nullptr);
    EXPECT_EQ(sema_create(vm_, diag_, nullptr, arena_), nullptr);
    EXPECT_EQ(sema_create(vm_, diag_, lex_.tokens, nullptr), nullptr);
}

TEST_F(SemaTest, AnalyzeNullProgram) {
    lex_ = lex_source(alloc_, "");
    sema_t *s = sema_create(vm_, diag_, lex_.tokens, arena_);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(sema_analyze(s, nullptr));
    sema_destroy(&s);
    EXPECT_EQ(s, nullptr);
}

/* ================================================================ */
/* 合法程序：无诊断                                                   */
/* ================================================================ */

TEST_F(SemaTest, EmptyFunction) {
    EXPECT_TRUE(analyze("func main(): void { }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, VarInferenceAndUse) {
    EXPECT_TRUE(analyze("func main(): void { var x = 1; var y = x; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* 作用域树：global → main 函数 fscope（捕获层）→ 参数层（body 挂此层） */
    ASSERT_EQ(sema_scope_children_count(sema_->global_scope), 1u);
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_scope_t *param = func_param_scope(fscope);
    ASSERT_NE(param, nullptr);

    sema_symbol_t *x = sema_scope_find_local(param, STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i32); /* 推断出 i32 */

    sema_symbol_t *y = sema_scope_find_local(param, STRSLICE_LIT("y"));
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->type, vm_->type_i32); /* 从 x 传播 */
}

TEST_F(SemaTest, ExplicitTypeAnnotation) {
    EXPECT_TRUE(analyze("func main(): void { var x:i64 = 1; var y:f64 = 1.5; }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, ImplicitCastInit) {
    /* 同类别加宽可隐式转换：i32→i64、f32→f64（f32 值经 var 显式标注产生） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:i64 = 1;"
        "  var c:f64 = 1.5;"
        "  var d:f64 = 1.5;"
        "  var e:f64 = d;"
        "}"));
    if (diag_has_error(diag_)) {
        size_t n = diag_count(diag_);
        const diagnostic_t *all = diag_items(diag_);
        std::string joined;
        for (size_t k = 0; all && k < n; k++) {
            joined += std::string(" [") + all[k].message + "]";
        }
        ADD_FAILURE() << "unexpected diagnostics:" << joined;
    }
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, BinaryOpsTypeCheck) {
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a = 1 + 2;"
        "  var b = 3.0 * 4.0;"
        "  var c = a < 10;"
        "  var d = c && true;"
        "  var e = ~1;"
        "  var f = -a;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, FunctionOrderFreedom) {
    /* Pass 1 先收集全部函数名：main 可调用定义在后面的 foo */
    EXPECT_TRUE(analyze("func main(): void { foo(); } func foo(): void { }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, CallReturnTypeShadow) {
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var r = add(1, 2); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 1);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *r =
        sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("r"));
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->type, vm_->type_i32); /* shadow_call 返回 return_type shadow */
}

TEST_F(SemaTest, VoidCallAsStatement) {
    EXPECT_TRUE(analyze("func foo(): void { } func main(): void { foo(); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, AllPathsReturn) {
    EXPECT_TRUE(analyze(
        "func f(b:bool):i32 { if (b) { return 1; } else { return 2; } }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, ReturnThenUnreachableStatement) {
    /* 严格检查：return 后不可达语句报错 */
    EXPECT_FALSE(analyze(
        "func f():i32 { return 1; var x = 2; }"));
    expect_message(0, "unreachable statement");
}

TEST_F(SemaTest, WhileAndForLoops) {
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var i:i32 = 0;"
        "  while (i < 10) { i += 1; }"
        "  for (var j:i32 = 0; j < 5; j = j + 1) { }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, DiscardUnderscore) {
    EXPECT_TRUE(analyze("func main(): void { _ = 1 + 2; }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, BreakContinueInsideLoop) {
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  while (true) { break; }"
        "  for (var i:i32 = 0; i < 3; i = i + 1) { continue; }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, ShadowingSelfReferenceResolvesOuter) {
    /* if body 内 var x = x + 1 的 x 在 init 求值时未激活 → 解析到外层 x */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x = 1;"
        "  if (true) { var x = x + 1; }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

/* ================================================================ */
/* 确定性赋值分析（flow_init 数据流）：TDZ 编译期检查                */
/* ================================================================ */

TEST_F(SemaTest, UninitDeclThenAssignThenRead) {
    /* var a = undefined; a = 1; 赋值后读取 OK */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  a = 1;"
        "  var b = a;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, UninitDeclReadBeforeAssign) {
    /* var a = undefined; 直接读取 → used before initialization */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  var b = a;"
        "}"));
    expect_message(0, "used before initialization");
}

TEST_F(SemaTest, UninitDeclInExpr) {
    /* 未初始化变量参与运算 → used before initialization */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  var b = a + 1;"
        "}"));
    expect_message(0, "used before initialization");
}

TEST_F(SemaTest, UninitDeclNoExplicitType) {
    /* var a = undefined 无显式类型 → 无法推断类型 */
    EXPECT_FALSE(analyze(
        "func main(): void { var a = undefined; }"));
    expect_message(0, "cannot infer type of uninitialized variable 'a'");
}

TEST_F(SemaTest, UndefinedOutsideVarInit) {
    /* undefined 只允许作为变量初始化的未初始化声明 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  a = undefined;"
        "}"));
    expect_message(0, "'undefined' can only be used as a variable initializer");
}

TEST_F(SemaTest, IfBothBranchesAssignThenRead) {
    /* if 两分支都赋值 → 合并点确定已初始化 → 后读取 OK（用户正确场景） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  if (true) { a = 1; } else { a = 2; }"
        "  var b = a;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, IfSingleBranchAssignThenReadFails) {
    /* if 单分支赋值 → 合并点仍 UNKNOWN → 后读取报错（用户错误场景，保守） */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  if (true) { a = 1; } else { }"
        "  var b = a;"
        "}"));
    expect_message(0, "used before initialization");
}

TEST_F(SemaTest, IfNoElseAssignThenReadFails) {
    /* if 无 else 单分支赋值 → 保守策略：else 视为未赋值 → 报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  if (true) { a = 1; }"
        "  var b = a;"
        "}"));
    expect_message(0, "used before initialization");
}

TEST_F(SemaTest, NestedIfBothBranchesAssignThenRead) {
    /* 嵌套 if：外层两分支各自内层都赋值 → 合并后确定已初始化 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  if (true) { if (true) { a = 1; } else { a = 2; } }"
        "  else { if (true) { a = 3; } else { a = 4; } }"
        "  var b = a;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, NestedIfOneBranchMissesThenReadFails) {
    /* 嵌套 if 内层缺分支 → 外层合并点仍 UNKNOWN */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  if (true) { if (true) { a = 1; } else { } }"
        "  else { a = 3; }"
        "  var b = a;"
        "}"));
    expect_message(0, "used before initialization");
}

TEST_F(SemaTest, WhileBodyAssignDoesNotInitOuter) {
    /* while 体可能执行 0 次：体内赋值不提升确定性 → 循环后读取报错（保守） */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  while (true) { a = 1; }"
        "  var b = a;"
        "}"));
    expect_message(0, "used before initialization");
}

TEST_F(SemaTest, ForBodyAssignDoesNotInitOuter) {
    /* for 体可能执行 0 次：体内赋值不提升确定性 → 循环后读取报错（保守） */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  for (var i:i32 = 0; i < 3; i = i + 1) { a = 1; }"
        "  var b = a;"
        "}"));
    expect_message(0, "used before initialization");
}

TEST_F(SemaTest, AssignBeforeIfThenReadInBranches) {
    /* if 前已初始化：分支内重新赋值不影响（读取在分支内） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:i32 = 1;"
        "  if (true) { var b = a; } else { var c = a; }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, IfBranchAssignExitsUninitInBranch) {
    /* 未初始化变量在分支内赋值后可立即读取（分支内数据流） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:i32 = undefined;"
        "  if (true) { a = 1; var b = a; } else { }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

/* ================================================================ */
/* 类型错误：诊断产出                                                 */
/* ================================================================ */

TEST_F(SemaTest, UndefinedVariable) {
    EXPECT_FALSE(analyze("func main(): void { x = 1; }"));
    expect_message(0, "undefined variable 'x'");
}

TEST_F(SemaTest, UndefinedVariableInExpr) {
    EXPECT_FALSE(analyze("func main(): void { var y = x + 1; }"));
    expect_message(0, "undefined variable 'x'");
}

TEST_F(SemaTest, SelfReferenceUndefined) {
    /* var x = x + 1 自引用：自身未激活，外层无 x → undefined */
    EXPECT_FALSE(analyze("func main(): void { var x = x + 1; }"));
    expect_message(0, "undefined variable 'x'");
}

TEST_F(SemaTest, VarInitTypeMismatch) {
    EXPECT_FALSE(analyze("func main(): void { var x:i32 = \"s\"; }"));
    expect_message(0, "cannot initialize variable 'x' of type i32");
}

TEST_F(SemaTest, VarInitNotAssignableFloatToInt) {
    EXPECT_FALSE(analyze("func main(): void { var x:i32 = 1.5; }"));
    expect_message(0, "cannot initialize variable 'x'");
}

TEST_F(SemaTest, AssignTypeMismatch) {
    EXPECT_FALSE(analyze("func main(): void { var x:i32 = 1; x = \"s\"; }"));
    expect_message(0, "cannot assign str to variable 'x' of type i32");
}

TEST_F(SemaTest, UndefinedFunctionCall) {
    EXPECT_FALSE(analyze("func main(): void { bar(); }"));
    expect_message(0, "undefined variable 'bar'");
}

TEST_F(SemaTest, CallArgCountMismatch) {
    EXPECT_FALSE(analyze(
        "func foo(a:i32): void { }"
        "func main(): void { foo(); }"));
    expect_message(0, "expects 1 arguments, got 0");
}

TEST_F(SemaTest, CallArgTooMany) {
    EXPECT_FALSE(analyze(
        "func foo(a:i32): void { }"
        "func main(): void { foo(1, 2); }"));
    expect_message(0, "expects 1 arguments, got 2");
}

TEST_F(SemaTest, CallArgTypeMismatch) {
    EXPECT_FALSE(analyze(
        "func foo(a:i32): void { }"
        "func main(): void { foo(\"s\"); }"));
    expect_message(0, "cannot convert str to i32");
}

TEST_F(SemaTest, BinaryTypeMismatch) {
    EXPECT_FALSE(analyze("func main(): void { var x = 1 + \"s\"; }"));
    expect_message(0, "cannot apply '+' to i32 and str");
}

TEST_F(SemaTest, UnaryTypeMismatch) {
    EXPECT_FALSE(analyze("func main(): void { var x = !1; }"));
    expect_message(0, "logical not operand must be bool");
}

TEST_F(SemaTest, LogicalOpRequiresBool) {
    EXPECT_FALSE(analyze("func main(): void { var x = 1 && true; }"));
    expect_message(0, "logical operator operand must be bool");
}

TEST_F(SemaTest, IfCondRequiresBool) {
    EXPECT_FALSE(analyze("func main(): void { if (1) { } }"));
    expect_message(0, "if condition operand must be bool");
}

TEST_F(SemaTest, WhileCondRequiresBool) {
    EXPECT_FALSE(analyze("func main(): void { while (1) { } }"));
    expect_message(0, "while condition operand must be bool");
}

TEST_F(SemaTest, ForCondRequiresBool) {
    EXPECT_FALSE(analyze(
        "func main(): void { for (var i:i32 = 0; i + 1; i = i + 1) { } }"));
    expect_message(0, "for condition operand must be bool");
}

TEST_F(SemaTest, ExprResultUnused) {
    EXPECT_FALSE(analyze("func main(): void { 1 + 2; }"));
    expect_message(0, "expression result of type i32 is unused");
}

TEST_F(SemaTest, NonVoidReturnMismatch) {
    EXPECT_FALSE(analyze("func f():i32 { return \"s\"; }"));
    expect_message(0, "cannot return str from function returning i32");
}

TEST_F(SemaTest, VoidFunctionReturnsValue) {
    EXPECT_FALSE(analyze("func f(): void { return 1; }"));
    expect_message(0, "cannot return i32 from function returning void");
}

TEST_F(SemaTest, NonVoidMissingReturnOnAllPaths) {
    EXPECT_FALSE(analyze(
        "func f(b:bool):i32 { if (b) { return 1; } }"));
    expect_message(0, "must return a value on all paths");
}

TEST_F(SemaTest, MissingReturnEntirely) {
    EXPECT_FALSE(analyze("func f():i32 { var x = 1; }"));
    expect_message(0, "must return a value on all paths");
}

TEST_F(SemaTest, MissingReturnElseIfChainEnd) {
    /* else-if 链缺最终 else：最后一条路径不返回 → 非 void 函数必须报错 */
    EXPECT_FALSE(analyze(
        "func f(a:bool, b:bool):i32 {"
        "  if (a) { return 1; }"
        "  else if (b) { return 2; }"
        "}"));
    expect_message(0, "must return a value on all paths");
}

TEST_F(SemaTest, ReturnInLoopDoesNotGuaranteeAllPaths) {
    /* 循环体内 return 不贡献 definitely_returns：条件可能为 false 直接跳过 */
    EXPECT_FALSE(analyze(
        "func f():i32 { while (true) { return 1; } }"));
    expect_message(0, "must return a value on all paths");
}

TEST_F(SemaTest, ReturnAfterIfNoElseNotGuaranteed) {
    /* if 无 else + 尾部 return：if 可能不执行，但尾部 return 兜底 → 合法 */
    EXPECT_TRUE(analyze(
        "func f(b:bool):i32 { if (b) { return 1; } return 2; }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, ExplicitVoidFunctionNoReturnRequired) {
    /* 显式 :void 不要求 return */
    EXPECT_TRUE(analyze("func f():void { var x = 1; }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, UnreachableAfterIfAllPathsReturn) {
    /* then/else 全 return 后的语句不可达 */
    EXPECT_FALSE(analyze(
        "func f(b:bool):i32 {"
        "  if (b) { return 1; } else { return 2; }"
        "  var x = 3;"
        "}"));
    expect_message(0, "unreachable statement");
}

TEST_F(SemaTest, BreakInsideLoopIsLegal) {
    EXPECT_TRUE(analyze(
        "func main(): void { while (true) { break; } }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, ContinueInsideForIsLegal) {
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  for (var i:i32 = 0; i < 5; i = i + 1) { continue; }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, BreakOutsideLoop) {
    EXPECT_FALSE(analyze("func main(): void { break; }"));
    expect_message(0, "'break' outside loop");
}

TEST_F(SemaTest, ContinueOutsideLoop) {
    EXPECT_FALSE(analyze("func main(): void { continue; }"));
    expect_message(0, "'continue' outside loop");
}

TEST_F(SemaTest, DuplicateVariable) {
    EXPECT_FALSE(analyze("func main(): void { var x = 1; var x = 2; }"));
    expect_message(0, "duplicate variable 'x'");
}

TEST_F(SemaTest, DuplicateParameter) {
    EXPECT_FALSE(analyze("func f(a:i32, a:i32): void { }"));
    expect_message(0, "duplicate parameter 'a'");
}

TEST_F(SemaTest, DuplicateFunction) {
    EXPECT_FALSE(analyze("func foo(): void { } func foo(): void { }"));
    expect_message(0, "duplicate function 'foo'");
}

TEST_F(SemaTest, NarrowingInitRejected) {
    /* i32 字面量收窄到 i16：隐式转换不允许（VM 只允许同类别加宽） */
    EXPECT_FALSE(analyze("func main(): void { var x:i16 = 1; }"));
    EXPECT_TRUE(diag_has_error(diag_));
}

TEST_F(SemaTest, CompoundAssignTypeMismatch) {
    EXPECT_FALSE(analyze(
        "func main(): void { var x:i32 = 1; x += \"s\"; }"));
    expect_message(0, "cannot apply '+=' to i32 and str");
}

TEST_F(SemaTest, MultipleErrorsAccumulate) {
    /* 两条独立错误：undefined var 不再级联二次 type mismatch */
    EXPECT_FALSE(analyze(
        "func f(): void {"
        "  var b = missing + 1;"
        "  var c = 1 + \"s\";"
        "}"));
    EXPECT_EQ(diag_count(diag_), 2u);
    expect_message(0, "undefined variable 'missing'");
    expect_message(1, "cannot apply '+' to i32 and str");
}

/* ================================================================ */
/* 作用域树结构                                                      */
/* ================================================================ */

TEST_F(SemaTest, ScopeTreeGlobalToFunction) {
    analyze("func main(): void { } func other(): void { }");

    ASSERT_EQ(sema_scope_children_count(sema_->global_scope), 2u);
    sema_scope_t *main_scope = sema_scope_child(sema_->global_scope, 0);
    sema_scope_t *other_scope = sema_scope_child(sema_->global_scope, 1);
    ASSERT_NE(main_scope, nullptr);
    ASSERT_NE(other_scope, nullptr);
    EXPECT_EQ(main_scope->kind, SEMA_SCOPE_FUNCTION);
    EXPECT_EQ(other_scope->kind, SEMA_SCOPE_FUNCTION);
    EXPECT_EQ(main_scope->parent, sema_->global_scope);
}

TEST_F(SemaTest, ScopeTreeNestedBlocks) {
    /* M1 无裸块语句：用 if / while / for 构造嵌套作用域 */
    analyze(
        "func main(): void {"
        "  var a = 1;"
        "  if (true) { var b = 2; }"
        "  while (true) { var c = 3; }"
        "}");

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_scope_t *param = func_param_scope(fscope);
    ASSERT_NE(param, nullptr);

    /* a 在参数层；b/c 各自在独立子作用域（if body / while body） */
    EXPECT_NE(sema_scope_find_local(param, STRSLICE_LIT("a")), nullptr);
    EXPECT_EQ(sema_scope_find_local(param, STRSLICE_LIT("b")), nullptr);
    EXPECT_EQ(sema_scope_find_local(param, STRSLICE_LIT("c")), nullptr);

    ASSERT_EQ(sema_scope_children_count(param), 2u);
    sema_scope_t *b1 = sema_scope_child(param, 0);
    sema_scope_t *b2 = sema_scope_child(param, 1);
    ASSERT_NE(b1, nullptr);
    ASSERT_NE(b2, nullptr);
    EXPECT_EQ(b1->kind, SEMA_SCOPE_BLOCK);
    EXPECT_EQ(b2->kind, SEMA_SCOPE_BLOCK);
    EXPECT_NE(sema_scope_find_local(b1, STRSLICE_LIT("b")), nullptr);
    EXPECT_NE(sema_scope_find_local(b2, STRSLICE_LIT("c")), nullptr);

    /* 子作用域中 lookup 沿 parent 链可看到外层 a */
    EXPECT_NE(sema_lookup(b1, STRSLICE_LIT("a")), nullptr);
}

TEST_F(SemaTest, ScopeTreeIfElse) {
    analyze("func main(): void { if (true) { } else { } }");

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_scope_t *param = func_param_scope(fscope);
    ASSERT_NE(param, nullptr);
    ASSERT_EQ(sema_scope_children_count(param), 2u); /* then / else */
    EXPECT_NE(sema_scope_child(param, 0), nullptr);
    EXPECT_NE(sema_scope_child(param, 1), nullptr);
}

TEST_F(SemaTest, ScopeTreeElseIfChain) {
    analyze("func main(): void { if (true) { } else if (true) { } else { } }");

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_scope_t *param = func_param_scope(fscope);
    ASSERT_NE(param, nullptr);
    /* then + else-if-then + else = 3 个同层子作用域 */
    ASSERT_EQ(sema_scope_children_count(param), 3u);
}

TEST_F(SemaTest, ScopeTreeFor) {
    analyze("func main(): void { for (var i:i32 = 0; i < 5; i = i + 1) { } }");

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_scope_t *param = func_param_scope(fscope);
    ASSERT_NE(param, nullptr);
    ASSERT_EQ(sema_scope_children_count(param), 1u);

    sema_scope_t *for_scope = sema_scope_child(param, 0);
    ASSERT_NE(for_scope, nullptr);
    EXPECT_EQ(for_scope->kind, SEMA_SCOPE_FOR);

    /* init 变量注册到 for scope */
    sema_symbol_t *i = sema_scope_find_local(for_scope, STRSLICE_LIT("i"));
    ASSERT_NE(i, nullptr);
    EXPECT_EQ(i->type, vm_->type_i32);

    /* body 是 for scope 的子作用域 */
    ASSERT_EQ(sema_scope_children_count(for_scope), 1u);
    EXPECT_EQ(sema_scope_child(for_scope, 0)->kind, SEMA_SCOPE_BLOCK);
}

TEST_F(SemaTest, ScopeTreeWhileBody) {
    analyze("func main(): void { while (true) { var t = 1; } }");

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_scope_t *param = func_param_scope(fscope);
    ASSERT_NE(param, nullptr);
    ASSERT_EQ(sema_scope_children_count(param), 1u);

    sema_scope_t *body = sema_scope_child(param, 0);
    ASSERT_NE(body, nullptr);
    EXPECT_NE(sema_scope_find_local(body, STRSLICE_LIT("t")), nullptr);
}

TEST_F(SemaTest, ShadowingBlocksOuterNotVisible) {
    /* 同名变量在不同作用域：子作用域内遮罩外层（VM scope 链），外层符号类型不受影响 */
    analyze(
        "func main(): void {"
        "  var x = 1;"
        "  if (true) { var x = 2.5; }"
        "  var y = x;"
        "}");
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_scope_t *param = func_param_scope(fscope);
    ASSERT_NE(param, nullptr);
    sema_symbol_t *x = sema_scope_find_local(param, STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i32); /* 外层 x 保持 i32 */
    sema_symbol_t *y = sema_scope_find_local(param, STRSLICE_LIT("y"));
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->type, vm_->type_i32); /* var y = x 解析到外层 i32 x */
}

TEST_F(SemaTest, BlockVariableDoesNotLeakOut) {
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  if (true) { var x = 1; }"
        "  x = 2;"
        "}"));
    expect_message(0, "undefined variable 'x' in assignment");
}

/* ================================================================ */
/* comptime：编译期常量求值（M2，CTFE 集成）                        */
/* ================================================================ */

TEST_F(SemaTest, ComptimeVarGlobal) {
    /* 全局 comptime var：符号表编码常量，定义点从语句链摘除（不进运行时） */
    EXPECT_TRUE(analyze(
        "comptime var A: i32 = 42;"
        "func main(): void { var x = A; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *a = sema_lookup(sema_->global_scope, STRSLICE_LIT("A"));
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->is_comptime);
    EXPECT_TRUE(a->ct_valid);
    EXPECT_TRUE(a->flow_init);
    EXPECT_EQ(a->type, vm_->type_i32);
    EXPECT_EQ(a->ct.i, 42);
}

TEST_F(SemaTest, ComptimeVarInference) {
    /* 无显式类型：从右值推断 */
    EXPECT_TRUE(analyze(
        "comptime var N = 7;"
        "func main(): void { var x = N; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *n = sema_lookup(sema_->global_scope, STRSLICE_LIT("N"));
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->type, vm_->type_i32);
    EXPECT_EQ(n->ct.i, 7);
}

TEST_F(SemaTest, ComptimeVarTypes) {
    /* 各类标量 + 字符串折叠编码 */
    EXPECT_TRUE(analyze(
        "comptime var I: i64 = 1;"
        "comptime var U: u64 = 2;"
        "comptime var F: f64 = 3.5;"
        "comptime var B: bool = true;"
        "comptime var S: str = \"hi\";"
        "func main(): void {"
        "  var a:i64 = I;"
        "  var b:u64 = U;"
        "  var c:f64 = F;"
        "  var d:bool = B;"
        "  var e:str = S;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *b = sema_lookup(sema_->global_scope, STRSLICE_LIT("B"));
    ASSERT_NE(b, nullptr);
    EXPECT_TRUE(b->ct.b);
    sema_symbol_t *f = sema_lookup(sema_->global_scope, STRSLICE_LIT("F"));
    ASSERT_NE(f, nullptr);
    EXPECT_DOUBLE_EQ(f->ct.f, 3.5);
    sema_symbol_t *s = sema_lookup(sema_->global_scope, STRSLICE_LIT("S"));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->ct.s.len, 2u);
    EXPECT_EQ(std::memcmp(s->ct.s.ptr, "hi", 2), 0);
}

TEST_F(SemaTest, ComptimeVarExprFold) {
    /* 右值可以是任意编译期可计算表达式（二元运算 + 字面量） */
    EXPECT_TRUE(analyze(
        "comptime var X = (1 + 2) * 3 - 4;"
        "func main(): void { var y = X; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *x = sema_lookup(sema_->global_scope, STRSLICE_LIT("X"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->ct.i, 5);
}

TEST_F(SemaTest, ComptimeFuncDefinition) {
    /* comptime func 定义：符号注册 + is_comptime 标记，不要求调用 */
    EXPECT_TRUE(analyze(
        "comptime func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *add = sema_lookup(sema_->global_scope, STRSLICE_LIT("add"));
    ASSERT_NE(add, nullptr);
    EXPECT_TRUE(add->is_comptime);
    EXPECT_NE(add->ast, nullptr);
    EXPECT_EQ(add->ast->kind, AST_FUNC_DEF);
}

TEST_F(SemaTest, ComptimeFuncCallFold) {
    /* 调用点折叠：var r = add(1,2) 类型为返回类型（i32） */
    EXPECT_TRUE(analyze(
        "comptime func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var r = add(1, 2); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* comptime func 同样建作用域树（Pass 3b 对其 shadow walk 做类型检查），
       global_scope 子节点 = [add, main]，main 在 child 1 */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 1);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *r =
        sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("r"));
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->type, vm_->type_i32);
}

TEST_F(SemaTest, ComptimeVarFromComptimeFunc) {
    /* comptime var 右值 = comptime func 调用（跨函数折叠） */
    EXPECT_TRUE(analyze(
        "comptime func add(a:i32, b:i32):i32 { return a + b; }"
        "comptime var SUM = add(1, 2);"
        "func main(): void { var x = SUM; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *sum = sema_lookup(sema_->global_scope, STRSLICE_LIT("SUM"));
    ASSERT_NE(sum, nullptr);
    EXPECT_TRUE(sum->ct_valid);
    EXPECT_EQ(sum->ct.i, 3);
}

TEST_F(SemaTest, ComptimeNestedCalls) {
    /* comptime func 调用另一 comptime func（嵌套解释 + 作用域往返） */
    EXPECT_TRUE(analyze(
        "comptime func double_(a:i32):i32 { return a * 2; }"
        "comptime func quad(a:i32):i32 { return double_(double_(a)); }"
        "comptime var Q = quad(3);"
        "func main(): void { var x = Q; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *q = sema_lookup(sema_->global_scope, STRSLICE_LIT("Q"));
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->ct.i, 12);
}

TEST_F(SemaTest, ComptimeFuncCallsPlainFunc) {
    /* comptime func 调用普通纯函数：ctfe 解释执行普通函数（实参实值），
       调用链允许 comptime → 普通（普通函数自身仍做 shadow walk，body 无
       comptime 调用即无冲突） */
    EXPECT_TRUE(analyze(
        "func double_(a:i32):i32 { return a * 2; }"
        "comptime func quad(a:i32):i32 { return double_(a) + double_(a); }"
        "comptime var Q = quad(3);"
        "func main(): void { var x = Q; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *q = sema_lookup(sema_->global_scope, STRSLICE_LIT("Q"));
    ASSERT_NE(q, nullptr);
    EXPECT_EQ(q->ct.i, 12);
}

TEST_F(SemaTest, ComptimeChainPropagation) {
    /* comptime 属性沿调用链显式传播：comptime var 右值 = comptime 函数链
       （helper→add 全 comptime）。普通函数内部调用 comptime func 且实参为
       运行期变量是非法用法（comptime func 不注册运行时）。 */
    EXPECT_TRUE(analyze(
        "comptime func add(a:i32, b:i32):i32 { return a + b; }"
        "comptime func helper(x:i32):i32 { return add(x, 1); }"
        "comptime var R = helper(5);"
        "func main(): void { var r = R; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *r = sema_lookup(sema_->global_scope, STRSLICE_LIT("R"));
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->ct.i, 6);
}

TEST_F(SemaTest, ComptimePlainFuncRuntimeArg) {
    /* 普通函数内部调用 comptime func 且实参为运行期变量（参数 shadow）：
       comptime func 不注册运行时，运行期实参无法编译期求值 → 诊断 */
    EXPECT_FALSE(analyze(
        "comptime func add(a:i32, b:i32):i32 { return a + b; }"
        "func bad(x:i32): void { var y = add(x, 1); }"
        "func main(): void { bad(1); }"));
    expect_message(0, "not a compile-time constant");
}

TEST_F(SemaTest, ComptimeFuncCallsSideEffect) {
    /* comptime func 调用带副作用函数：ctfe 执行时副作用发生在编译期，
       且 void 返回值不可折叠 → 诊断。comptime func 应为纯函数 */
    EXPECT_FALSE(analyze(
        "func p(x:i32):void { }"
        "comptime func f():void { p(1); }"
        "comptime var T = f();"
        "func main(): void { }"));
    expect_message(0, "cannot be folded");
}

TEST_F(SemaTest, ComptimeLocalVar) {
    /* 局部 comptime var：定义点从语句链摘除，引用点折叠 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  comptime var L = 10;"
        "  var y = L + 1;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *l =
        sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("L"));
    ASSERT_NE(l, nullptr);
    EXPECT_TRUE(l->is_comptime);
    EXPECT_TRUE(l->ct_valid);
    EXPECT_EQ(l->ct.i, 10);
    /* 折叠后编译器看到的语句链不再包含 comptime var 定义 */
    EXPECT_EQ(l->type, vm_->type_i32);
}

TEST_F(SemaTest, ComptimeFuncWithControlFlow) {
    /* comptime func 内含 if/while：CTFE 语句解释路径 */
    EXPECT_TRUE(analyze(
        "comptime func fact(n:i32):i32 {"
        "  var r:i32 = 1;"
        "  var i:i32 = 1;"
        "  while (i <= n) { r = r * i; i = i + 1; }"
        "  return r;"
        "}"
        "comptime var F = fact(5);"
        "func main(): void { var x = F; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *f = sema_lookup(sema_->global_scope, STRSLICE_LIT("F"));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->ct.i, 120); /* 5! */
}

TEST_F(SemaTest, ComptimeVarArrayEncode) {
    /* comptime var 数组常量：递归编码进符号表（elems 连续块 + count） */
    EXPECT_TRUE(analyze(
        "comptime var A: [3]i32 = .[3]i32{ 1, 2, 3 };"
        "func main(): void { var x = A; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *a = sema_lookup(sema_->global_scope, STRSLICE_LIT("A"));
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->ct_valid);
    ASSERT_NE(a->ct.type, nullptr);
    EXPECT_EQ(a->ct.type->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(a->ct.count, 3u);
    ASSERT_NE(a->ct.elems, nullptr);
    EXPECT_EQ(array_type_len(a->ct.type), 3u);
    EXPECT_EQ(array_type_elem(a->ct.type), vm_->type_i32);
    EXPECT_EQ(a->ct.elems[0].i, 1);
    EXPECT_EQ(a->ct.elems[1].i, 2);
    EXPECT_EQ(a->ct.elems[2].i, 3);
}

TEST_F(SemaTest, ComptimeFuncReturnArrayFoldToConstruct) {
    /* comptime func 返回数组 → 调用点折叠为 AST_CONSTRUCT 节点
       （.<type>{ fields }），类型位为 AST_TYPE_REF（sema 登记的
       "__type_N" 名字引用，类型构造收敛到 hoist 提升区），字段为
       折叠字面量。 */
    EXPECT_TRUE(analyze(
        "comptime func make_arr(): [3]i32 {"
        "  return .[3]i32{ 4, 5, 6 };"
        "}"
        "func main(): void { var r = make_arr(); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* main 函数体首个语句 var r = <AST_CONSTRUCT> */
    ast_program_t *prog = (ast_program_t *)ast_;
    /* 找到名为 main 的函数（comptime func 也可能在链上，按名定位） */
    ast_node_t *walk = prog->funcs;
    ast_node_t *main_def = nullptr;
    for (; walk; walk = walk->next) {
        if (walk->kind == AST_FUNC_DEF &&
            strslice_eq(((ast_func_def_t *)walk)->name, STRSLICE_LIT("main"))) {
            main_def = walk;
            break;
        }
    }
    ASSERT_NE(main_def, nullptr);
    ast_block_t *body = (ast_block_t *)((ast_func_def_t *)main_def)->body;
    ASSERT_NE(body->stmts, nullptr);
    ast_var_def_t *vd = (ast_var_def_t *)body->stmts;
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, AST_CONSTRUCT);

    /* 类型位：AST_TYPE_REF（名字引用登记过的 [3]i32），可查回数组类型 */
    ast_construct_t *cn = (ast_construct_t *)vd->init;
    ASSERT_NE(cn->type, nullptr);
    EXPECT_EQ(cn->type->kind, AST_TYPE_REF);
    ast_type_ref_t *ref = (ast_type_ref_t *)cn->type;
    const sema_type_t *st = sema_type_find_name(sema_, ref->name);
    ASSERT_NE(st, nullptr);
    ASSERT_NE(st->type, nullptr);
    EXPECT_EQ(st->type->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(st->type), 3u);
    EXPECT_EQ(array_type_elem(st->type), vm_->type_i32);

    /* 字段链：3 个折叠的 i32 字面量 */
    int values[3] = {0, 0, 0};
    size_t nf = 0;
    for (ast_node_t *fd = cn->fields; fd; fd = fd->next) {
        ASSERT_EQ(fd->kind, AST_INT_LIT);
        values[nf++] = (int)((ast_int_lit_t *)fd)->value;
    }
    ASSERT_EQ(nf, 3u);
    EXPECT_EQ(values[0], 4);
    EXPECT_EQ(values[1], 5);
    EXPECT_EQ(values[2], 6);
}

TEST_F(SemaTest, ComptimeFuncReturnNestedArrayFold) {
    /* 嵌套数组常量：comptime func 返回 [2][3]i32，调用点折叠 AST_CONSTRUCT，
       字段为嵌套 AST_CONSTRUCT，类型位为 AST_TYPE_REF（登记过 [2][3]i32，
       结构依赖：elem 类型 [3]i32 亦已登记）。 */
    EXPECT_TRUE(analyze(
        "comptime func make_mat(): [2][3]i32 {"
        "  return .[2][3]i32{ .[3]i32{1,2,3}, .[3]i32{4,5,6} };"
        "}"
        "func main(): void { var m = make_mat(); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *main_def = nullptr;
    for (ast_node_t *w = prog->funcs; w; w = w->next) {
        if (w->kind == AST_FUNC_DEF &&
            strslice_eq(((ast_func_def_t *)w)->name, STRSLICE_LIT("main"))) {
            main_def = w;
            break;
        }
    }
    ASSERT_NE(main_def, nullptr);
    ast_block_t *body = (ast_block_t *)((ast_func_def_t *)main_def)->body;
    ast_var_def_t *vd = (ast_var_def_t *)body->stmts;
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, AST_CONSTRUCT);

    ast_construct_t *cn = (ast_construct_t *)vd->init;
    ASSERT_NE(cn->type, nullptr);
    EXPECT_EQ(cn->type->kind, AST_TYPE_REF);
    /* 类型可查回 [2][3]i32：elem 是 [3]i32（已 intern 的嵌套数组） */
    ast_type_ref_t *ref = (ast_type_ref_t *)cn->type;
    const sema_type_t *st = sema_type_find_name(sema_, ref->name);
    ASSERT_NE(st, nullptr);
    ASSERT_NE(st->type, nullptr);
    EXPECT_EQ(st->type->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(st->type), 2u);
    const type_t *elem = array_type_elem(st->type);
    ASSERT_NE(elem, nullptr);
    EXPECT_EQ(elem->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(elem), 3u);
    EXPECT_EQ(array_type_elem(elem), vm_->type_i32);

    /* 外层 2 字段，每个是嵌套 AST_CONSTRUCT */
    size_t nf = 0;
    for (ast_node_t *fd = cn->fields; fd; fd = fd->next) {
        EXPECT_EQ(fd->kind, AST_CONSTRUCT);
        nf++;
    }
    EXPECT_EQ(nf, 2u);
}

TEST_F(SemaTest, ComptimeFuncIndexAssignFold) {
    /* comptime func 内数组下标赋值（= 与复合 +=）后返回数组，调用点折叠
       AST_CONSTRUCT，字段应为赋值后的最终值。ctfe 语义与运行期一致：
       object → index → value 求值序，value_set_index 写块内偏移。 */
    EXPECT_TRUE(analyze(
        "comptime func make_arr(): [3]i32 {"
        "  var r = .[3]i32{0,0,0};"
        "  r[0] = 7;"
        "  r[1] += 2;"
        "  r[2] = r[0] * 3;"
        "  return r;"
        "}"
        "func main(): void { var a = make_arr(); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *main_def = nullptr;
    for (ast_node_t *w = prog->funcs; w; w = w->next) {
        if (w->kind == AST_FUNC_DEF &&
            strslice_eq(((ast_func_def_t *)w)->name, STRSLICE_LIT("main"))) {
            main_def = w;
            break;
        }
    }
    ASSERT_NE(main_def, nullptr);
    ast_block_t *body = (ast_block_t *)((ast_func_def_t *)main_def)->body;
    ast_var_def_t *vd = (ast_var_def_t *)body->stmts;
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, AST_CONSTRUCT);

    ast_construct_t *cn = (ast_construct_t *)vd->init;
    int values[3] = {0, 0, 0};
    size_t nf = 0;
    for (ast_node_t *fd = cn->fields; fd; fd = fd->next) {
        ASSERT_EQ(fd->kind, AST_INT_LIT);
        values[nf++] = (int)((ast_int_lit_t *)fd)->value;
    }
    ASSERT_EQ(nf, 3u);
    EXPECT_EQ(values[0], 7);  /* r[0] = 7 */
    EXPECT_EQ(values[1], 2);  /* r[1] += 2 → 0+2 */
    EXPECT_EQ(values[2], 21); /* r[2] = r[0]*3 → 7*3 */
}

TEST_F(SemaTest, ComptimeFuncNestedIndexAssignFold) {
    /* 多维数组下标赋值：r[1][2] = v 是链式嵌套下标（((r[1])[2])），
       ctfe 按运行期 INDEX_GET/INDEX_SET 逐维语义求值 */
    EXPECT_TRUE(analyze(
        "comptime func make_mat(): [2][3]i32 {"
        "  var r = .[2][3]i32{ .[3]i32{1,2,3}, .[3]i32{4,5,6} };"
        "  r[1][2] = 99;"
        "  r[0][0] += 10;"
        "  return r;"
        "}"
        "func main(): void { var m = make_mat(); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *main_def = nullptr;
    for (ast_node_t *w = prog->funcs; w; w = w->next) {
        if (w->kind == AST_FUNC_DEF &&
            strslice_eq(((ast_func_def_t *)w)->name, STRSLICE_LIT("main"))) {
            main_def = w;
            break;
        }
    }
    ASSERT_NE(main_def, nullptr);
    ast_block_t *body = (ast_block_t *)((ast_func_def_t *)main_def)->body;
    ast_var_def_t *vd = (ast_var_def_t *)body->stmts;
    ASSERT_NE(vd->init, nullptr);
    EXPECT_EQ(vd->init->kind, AST_CONSTRUCT);

    /* 外层 2 字段；每个是嵌套 AST_CONSTRUCT，其内层元素应含赋值结果。
       折叠后字段顺序：{{11,2,3},{4,5,99}} */
    ast_construct_t *cn = (ast_construct_t *)vd->init;
    size_t nf = 0;
    for (ast_node_t *fd = cn->fields; fd; fd = fd->next, nf++) {
        ASSERT_EQ(fd->kind, AST_CONSTRUCT);
        ast_construct_t *inner = (ast_construct_t *)fd;
        int vals[3] = {0, 0, 0};
        size_t ni = 0;
        for (ast_node_t *ef = inner->fields; ef; ef = ef->next, ni++) {
            ASSERT_EQ(ef->kind, AST_INT_LIT);
            vals[ni] = (int)((ast_int_lit_t *)ef)->value;
        }
        ASSERT_EQ(ni, 3u);
        if (nf == 0) {
            EXPECT_EQ(vals[0], 11); /* r[0][0] += 10 → 1+10 */
            EXPECT_EQ(vals[1], 2);
            EXPECT_EQ(vals[2], 3);
        } else {
            EXPECT_EQ(vals[0], 4);
            EXPECT_EQ(vals[1], 5);
            EXPECT_EQ(vals[2], 99); /* r[1][2] = 99 */
        }
    }
    EXPECT_EQ(nf, 2u);
}

/* ---- comptime 错误场景 ---- */

TEST_F(SemaTest, ComptimeVarMissingInit) {
    EXPECT_FALSE(analyze(
        "comptime var A: i32 = undefined;"
        "func main(): void { }"));
    expect_message(0, "must have a compile-time initializer");
}

TEST_F(SemaTest, ComptimeVarNotConstant) {
    /* 右值引用运行期变量 → 非编译期常量 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x = 1;"
        "  comptime var A = x;"
        "}"));
    expect_message(0, "not a compile-time constant");
}

TEST_F(SemaTest, ComptimeVarTypeMismatch) {
    EXPECT_FALSE(analyze(
        "comptime var A: i32 = 1.5;"
        "func main(): void { }"));
    expect_message(0, "cannot initialize comptime variable 'A'");
}

TEST_F(SemaTest, ComptimeVarAssignForbidden) {
    EXPECT_FALSE(analyze(
        "comptime var A = 1;"
        "func main(): void { A = 2; }"));
    expect_message(0, "cannot assign to compile-time constant 'A'");
}

TEST_F(SemaTest, ComptimeCallArgCountMismatch) {
    EXPECT_FALSE(analyze(
        "comptime func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var r = add(1); }"));
    expect_message(0, "expects 2 arguments, got 1");
}

TEST_F(SemaTest, ComptimeCallArgTypeMismatch) {
    EXPECT_FALSE(analyze(
        "comptime func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var r = add(\"s\", 2); }"));
    expect_message(0, "cannot convert str to i32");
}

TEST_F(SemaTest, ComptimeFuncNotConstantPath) {
    /* comptime func 内部引用未定义/非编译期实体 → 求值失败诊断 */
    EXPECT_FALSE(analyze(
        "comptime func f():i32 { return g(); }"
        "func main(): void { var r = f(); }"));
    /* g 未定义 → 调用点 lookup 失败诊断（sema 阶段） */
    expect_message(0, "undefined variable 'g'");
}

/* ================================================================ */
/* type 定义（type name = <type-expr>;）：type value 绑定             */
/* ================================================================ */

TEST_F(SemaTest, TypeDefBuiltinRhs) {
    /* 内建类型 rhs：type MyInt = i64。内建也折叠为 AST_TYPE_REF
       （名字 = 规范名 "i64"，编译器经 type_lookup 兜底 → LOAD_TYPE
       <内建 id>），别名透明。符号激活。 */
    EXPECT_TRUE(analyze(
        "type MyInt = i64;"
        "func main(): void { var x:MyInt = 7; var y = x; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *sym =
        sema_lookup(sema_->global_scope, STRSLICE_LIT("MyInt"));
    ASSERT_NE(sym, nullptr);
    EXPECT_TRUE(sym->is_active);
    EXPECT_TRUE(sym->flow_init);

    /* 符号类型槽为空：type def 的值在编译期 vm scope（type_lookup 路径），
       不经 sym->type。验证 var 显式类型解析到 i64。 */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i64);

    /* 内建 rhs 槽位折叠为 AST_TYPE_REF（名字 = 内建规范名） */
    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *td = prog->funcs;
    ASSERT_NE(td, nullptr);
    ASSERT_EQ(td->kind, AST_TYPE_DEF);
    EXPECT_EQ(((ast_type_def_t *)td)->expr->kind, AST_TYPE_REF);
    ast_type_ref_t *ref = (ast_type_ref_t *)((ast_type_def_t *)td)->expr;
    EXPECT_TRUE(strslice_eq(ref->name, STRSLICE_LIT("i64")));
}

TEST_F(SemaTest, TypeDefCompositeRhsFoldToRef) {
    /* 复合类型 rhs：[2]i32 → 登记 + 槽位折叠为 AST_TYPE_REF（名字引用）。
       折叠幂等：重跑 sema 不重复登记。 */
    EXPECT_TRUE(analyze(
        "type Pair = [2]i32;"
        "func main(): void { var p:Pair = .[2]i32{1, 2}; var q = p[0]; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* 全局 type def 在 funcs 链上（保留，进入字节码） */
    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *td = prog->funcs;
    ASSERT_NE(td, nullptr);
    ASSERT_EQ(td->kind, AST_TYPE_DEF);
    EXPECT_EQ(((ast_type_def_t *)td)->expr->kind, AST_TYPE_REF);
    ast_type_ref_t *ref = (ast_type_ref_t *)((ast_type_def_t *)td)->expr;
    const sema_type_t *st = sema_type_find_name(sema_, ref->name);
    ASSERT_NE(st, nullptr);
    ASSERT_NE(st->type, nullptr);
    EXPECT_EQ(st->type->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(st->type), 2u);
    EXPECT_EQ(array_type_elem(st->type), vm_->type_i32);
}

TEST_F(SemaTest, TypeDefAliasChain) {
    /* type 别名链：B = A = i64；var 显式类型与推断都走 type value */
    EXPECT_TRUE(analyze(
        "type A = i64;"
        "type B = A;"
        "func main(): void { var x:B = 5; var t = B; var y = x; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i64);
}

TEST_F(SemaTest, TypeDefInFunctionSignature) {
    /* 全局 type def 先于函数签名解析（pass1b）：参数/返回类型可引用 */
    EXPECT_TRUE(analyze(
        "type MyInt = i64;"
        "func id(v:MyInt):MyInt { return v; }"
        "func main(): void { var r = id(42); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, TypeDefLocal) {
    /* 局部 type def：3a 延迟解析 var 槽位 → 3b 定义点求值后兜底成功 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  type Local = i64;"
        "  var x:Local = 7;"
        "  var y = x;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i64);
}

TEST_F(SemaTest, TypeDefLocalNestedBlock) {
    /* 嵌套块局部 type：块作用域遮蔽，出块不可见（TDZ/未知类型） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:i32 = 1;"
        "  { type Inner = i32; var x:Inner = 2; a = x; }"
        "  var y = a;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, TypeDefTypeValueExpr) {
    /* type value 是真实值（非 shadow）：可作表达式（typeof 桥梁） */
    EXPECT_TRUE(analyze(
        "type T = i32;"
        "func main(): void { var t = T; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *t = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("t"));
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->type, vm_->type_type); /* 推断为 type 类型 */
}

TEST_F(SemaTest, TypeDefRhsNotType) {
    /* rhs 非类型值 → 报错 */
    EXPECT_FALSE(analyze(
        "func main(): void { type NotType = 42; }"));
    expect_message(0, "must evaluate to a type value, got i32");
}

TEST_F(SemaTest, TypeDefFuncSignature) {
    /* 函数签名类型 rhs：func(i32,i32)->i32 → 登记 + 槽位折叠为 AST_TYPE_REF。
       符号激活，var 显式类型解析到 func 签名（TYPE_KIND_FUNC）。 */
    EXPECT_TRUE(analyze(
        "type add_fn_t = func(i32,i32)->i32;"
        "func main(): void { var f:add_fn_t = undefined; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *sym =
        sema_lookup(sema_->global_scope, STRSLICE_LIT("add_fn_t"));
    ASSERT_NE(sym, nullptr);
    EXPECT_TRUE(sym->is_active);

    /* 复合 rhs 折叠为 AST_TYPE_REF，名字指向登记的类型 */
    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *td = prog->funcs;
    ASSERT_NE(td, nullptr);
    ASSERT_EQ(td->kind, AST_TYPE_DEF);
    ASSERT_EQ(((ast_type_def_t *)td)->expr->kind, AST_TYPE_REF);
    ast_type_ref_t *ref = (ast_type_ref_t *)((ast_type_def_t *)td)->expr;
    const sema_type_t *st = sema_type_find_name(sema_, ref->name);
    ASSERT_NE(st, nullptr);
    ASSERT_NE(st->type, nullptr);
    EXPECT_EQ(st->type->kind, TYPE_KIND_FUNC);

    /* var f:add_fn_t 类型解析到 func 签名 */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *f = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("f"));
    ASSERT_NE(f, nullptr);
    ASSERT_NE(f->type, nullptr);
    EXPECT_EQ(f->type->kind, TYPE_KIND_FUNC);
}

TEST_F(SemaTest, TypeDefFuncSignatureNoReturn) {
    /* 显式 void 返回：func(i32)->void → sema 正常 */
    EXPECT_TRUE(analyze(
        "type void_fn_t = func(i32)->void;"
        "func main(): void { var f:void_fn_t = undefined; }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, TypeDefFuncSignatureNoArrowFails) {
    /* 无 '->' 返回类型 func(i32) → 不允许隐式 void，解析报错 */
    EXPECT_FALSE(analyze(
        "type void_fn_t = func(i32);"
        "func main(): void { var f:void_fn_t = undefined; }"));
}

TEST_F(SemaTest, TypeDefFuncSignatureNested) {
    /* 嵌套复合签名 func([4]i32)->func(i32)->i32 → sema 正常 */
    EXPECT_TRUE(analyze(
        "type complex_t = func([4]i32)->func(i32)->i32;"
        "func main(): void { var f:complex_t = undefined; }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, TypeDefFuncSignatureInSignature) {
    /* 函数签名类型作为参数类型：全局 type def 先于函数签名解析 */
    EXPECT_TRUE(analyze(
        "type add_fn_t = func(i32,i32)->i32;"
        "func apply(f:add_fn_t): i32 { return 0; }"
        "func main(): void { }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, TypeDefDuplicate) {
    EXPECT_FALSE(analyze(
        "type A = i32;"
        "type A = i64;"
        "func main(): void { }"));
    expect_message(0, "duplicate name 'A'");
}

TEST_F(SemaTest, TypeDefDuplicateLocal) {
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  type A = i32;"
        "  type A = i64;"
        "}"));
    expect_message(0, "duplicate name 'A'");
}

TEST_F(SemaTest, TypeDefForwardRefHoisted) {
    /* 局部 type 提升：前向引用（定义在后使用）→ 类型名字块内可见，无 TDZ。
       var 槽位 3a 推迟解析，3b 块入口提升绑定后兜底重解析成功。 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x:Local = 5;"
        "  type Local = i32;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i32);
}

TEST_F(SemaTest, UndefInitUnknownTypeFails) {
    /* undefined 初始化 + 未知类型标注：3b 兜底重解析须报 unknown type
       （此前静默当 void，无诊断）。 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x:NoSuch = undefined;"
        "}"));
    expect_message(0, "unknown type");
}

TEST_F(SemaTest, UndefInitVarShadowTypeFails) {
    /* undefined 初始化 + 类型名被 var 遮蔽：报 "is a variable, not a type" */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var T:i32 = 1;"
        "  var x:T = undefined;"
        "}"));
    expect_message(0, "'T' is a variable, not a type");
}

TEST_F(SemaTest, TypeDefUsedBeforeActivation) {
    /* 全局 type def TDZ：函数体内使用在 pass1b 已绑定 → OK */
    EXPECT_TRUE(analyze(
        "type T = i32;"
        "func main(): void { var x:T = 1; }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, TypeDefVarTypeMismatch) {
    /* 显式类型 + 值类型不匹配：走常规 var 校验路径 */
    EXPECT_FALSE(analyze(
        "type T = i32;"
        "func main(): void { var x:T = \"s\"; }"));
    expect_message(0, "cannot initialize variable 'x' of type i32 with str");
}

TEST_F(SemaTest, TypeDefLocalShadowGlobal) {
    /* 局部 type 遮蔽全局同名 type */
    EXPECT_TRUE(analyze(
        "type T = i32;"
        "func main(): void {"
        "  type T = i64;"
        "  var x:T = 7;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i64);
}

TEST_F(SemaTest, TypeDefLocalHoistedShadowGlobalForward) {
    /* 提升遮蔽：局部 type 定义在引用之后，仍遮蔽全局同名 type（整个块内
       T 都是局部 i64，而非全局 i32——提升使遮蔽覆盖块内所有位置） */
    EXPECT_TRUE(analyze(
        "type T = i32;"
        "func main(): void {"
        "  var x:T = 7;"
        "  type T = i64;"
        "  var y:T = 9;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i64);
    sema_symbol_t *y = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("y"));
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(y->type, vm_->type_i64);
}

TEST_F(SemaTest, TypeDefLocalHoistedNestedBlockUse) {
    /* 提升跨嵌套块：外层块提升的 type 内层块可见（作用域链查找） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  type A = i32;"
        "  { var x:A = 2; var y = x; }"
        "  var z:A = 3;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, TypeDefLocalHoistedDepOrder) {
    /* 提升依赖序：type RHS 引用前序 type（声明序求值，与全局 pass1b 一致） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  type A = i32;"
        "  type B = A;"
        "  var x:B = 5;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i32);
}

TEST_F(SemaTest, TypeDefLocalHoistedVarShadowRhsNested) {
    /* 提升遮蔽预检递归：复合 type RHS 引用块内声明序靠前的 var → 报错 */
    EXPECT_FALSE(analyze(
        "type T = i32;"
        "type A = i64;"
        "func main(): void {"
        "  var T = 42;"
        "  type U = T extends i32 ? A : T;"
        "}"));
    expect_message(0, "'T' is a variable, not a type");
}

TEST_F(SemaTest, VarShadowGlobalTypeInTypeSlot) {
    /* var 完全遮罩：定义后类型槽位引用 T → 报 "is a variable, not a type" */
    EXPECT_FALSE(analyze(
        "type T = i32;"
        "func main(): void {"
        "  var T = 42;"
        "  var t2:T = 5;"
        "}"));
    expect_message(0, "'T' is a variable, not a type");
}

TEST_F(SemaTest, VarShadowGlobalTypeOrderSensitive) {
    /* 完全遮罩顺序敏感：var T 定义前，类型槽位仍见全局 type T=i32 */
    EXPECT_TRUE(analyze(
        "type T = i32;"
        "func main(): void {"
        "  var t2:T = 5;"
        "  var T = 42;"
        "  var z = T;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *t2 = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("t2"));
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(t2->type, vm_->type_i32);
}

TEST_F(SemaTest, ParamShadowGlobalTypeInTypeSlot) {
    /* 参数遮蔽：func f(T: i64) 内类型槽位 T → "is a variable, not a type" */
    EXPECT_FALSE(analyze(
        "type T = i32;"
        "func f(T:i64): void { var t2:T = 5; }"
        "func main(): void { f(7); }"));
    expect_message(0, "'T' is a variable, not a type");
}

TEST_F(SemaTest, VarShadowTypeInTypeRhs) {
    /* var 完全遮罩：type RHS 引用块内声明序靠前的 var T → 提升预检报错
       （var 遮蔽平等，type RHS 不可引用被 var 遮罩的名字） */
    EXPECT_FALSE(analyze(
        "type T = i32;"
        "func main(): void {"
        "  var T = 42;"
        "  type U = T;"
        "}"));
    expect_message(0, "'T' is a variable, not a type");
}

TEST_F(SemaTest, TypeDefExtendsTernaryFoldTrueBranch) {
    /* extends+三元选择类型：type RHS 经 ctfe 真实求值（extends 二元折叠
       为 bool，三元按真值选 then 分支 i64），整个 rhs 收敛为
       AST_TYPE_REF（名字 = 规范名）。 */
    EXPECT_TRUE(analyze(
        "type A = i32;"
        "type B = i64;"
        "type T = A extends i32 ? B : A;"
        "func main(): void { var x:T = 5; var y = x; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* funcs 链第 3 个节点是 T（A、B、T、main 顺序） */
    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *td = prog->funcs;
    for (int i = 0; i < 2 && td; i++) td = td->next;
    ASSERT_NE(td, nullptr);
    ASSERT_EQ(td->kind, AST_TYPE_DEF);
    ASSERT_EQ(((ast_type_def_t *)td)->expr->kind, AST_TYPE_REF);
    ast_type_ref_t *ref = (ast_type_ref_t *)((ast_type_def_t *)td)->expr;
    EXPECT_TRUE(strslice_eq(ref->name, STRSLICE_LIT("i64")));

    /* var x:T 显式类型解析到 i64（选择分支生效） */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i64);
}

TEST_F(SemaTest, TypeDefExtendsTernaryFoldFalseBranch) {
    /* extends 条件为假选 else 分支（i32） */
    EXPECT_TRUE(analyze(
        "type A = i32;"
        "type B = i64;"
        "type U = A extends i64 ? B : A;"
        "func main(): void { var x:U = 5; var y = x; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *td = prog->funcs;
    for (int i = 0; i < 2 && td; i++) td = td->next;
    ASSERT_NE(td, nullptr);
    ASSERT_EQ(td->kind, AST_TYPE_DEF);
    ASSERT_EQ(((ast_type_def_t *)td)->expr->kind, AST_TYPE_REF);
    ast_type_ref_t *ref = (ast_type_ref_t *)((ast_type_def_t *)td)->expr;
    EXPECT_TRUE(strslice_eq(ref->name, STRSLICE_LIT("i32")));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *x = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i32);
}

TEST_F(SemaTest, TypeDefExtendsTernaryArrayBranch) {
    /* extends 操作数是数组类型：cond 为假选 [3]i32 分支，折叠为登记引用
       （名字查 sema_type_find_name 命中数组类型）。 */
    EXPECT_TRUE(analyze(
        "type V = [2]i32 extends [3]i32 ? [2]i32 : [3]i32;"
        "func main(): void { var p:V = .[3]i32{1, 2, 3}; var q = p[0]; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *td = prog->funcs;
    ASSERT_NE(td, nullptr);
    ASSERT_EQ(td->kind, AST_TYPE_DEF);
    ASSERT_EQ(((ast_type_def_t *)td)->expr->kind, AST_TYPE_REF);
    ast_type_ref_t *ref = (ast_type_ref_t *)((ast_type_def_t *)td)->expr;
    const sema_type_t *st = sema_type_find_name(sema_, ref->name);
    ASSERT_NE(st, nullptr);
    ASSERT_NE(st->type, nullptr);
    EXPECT_EQ(st->type->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(st->type), 3u);
    EXPECT_EQ(array_type_elem(st->type), vm_->type_i32);
}

/* ================================================================ */
/* 函数值（function as value）                                         */
/* ================================================================ */

TEST_F(SemaTest, FuncValueAssignInferFuncType) {
    /* var f = add; 折叠为函数值，变量类型推断为签名 func(i32,i32)->i32 */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var f = add; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 1);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *f = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("f"));
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->kind, SEMA_SYM_VAR);
    ASSERT_NE(f->type, nullptr);
    EXPECT_EQ(f->type->kind, TYPE_KIND_FUNC);
}

TEST_F(SemaTest, FuncValueCallThroughVariable) {
    /* 函数值经变量调用：f(1,2) 的 callee 是变量（kind=VAR）不折叠，
       值类型是 func，按签名交验参数 */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var f = add; var r = f(1, 2); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 1);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *r = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("r"));
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->type, vm_->type_i32); /* 调用返回 return_type shadow */
}

TEST_F(SemaTest, FuncValueCallArgCountMismatch) {
    EXPECT_FALSE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var f = add; var r = f(1); }"));
    expect_message(0, "expects 2 arguments");
}

TEST_F(SemaTest, FuncValueCallArgTypeMismatch) {
    EXPECT_FALSE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var f = add; var r = f(1, true); }"));
    expect_message(0, "cannot convert bool to i32");
}

TEST_F(SemaTest, FuncValueExplicitTypeAssign) {
    /* 显式 func 类型槽位 + 函数值初始化 + 再赋值 */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func sub(a:i32, b:i32):i32 { return a - b; }"
        "func main(): void {"
        "  var f: func(i32,i32)->i32 = add;"
        "  f = sub;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, FuncValueTypeMismatchRejected) {
    /* 签名不匹配：func(i32)->i32 槽位不能收 func(i32,i32)->i32 */
    EXPECT_FALSE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var f: func(i32)->i32 = add; }"));
    expect_message(0, "cannot initialize");
}

TEST_F(SemaTest, FuncValueAsArgumentAndReturn) {
    /* 函数值传参 + 返回 */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func apply(f:func(i32,i32)->i32, x:i32, y:i32):i32 { return f(x, y); }"
        "func get_add():func(i32,i32)->i32 { return add; }"
        "func main(): void {"
        "  var r = apply(add, 1, 2);"
        "  var g = get_add();"
        "  var q = g(3, 4);"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, FuncValueVarShadowsFunctionName) {
    /* 遮蔽平等：局部变量遮蔽函数名后，调用走变量类型检查。
       var add=2 是 i32 → 报不可调用（不是"函数不存在"） */
    EXPECT_FALSE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func main(): void { var add = 2; var val = add(1,2); }"));
    expect_message(0, "cannot call");
}

TEST_F(SemaTest, FuncValueParamShadowsFunctionName) {
    /* 参数遮蔽函数名：apply(add, ...) 内 add 是参数（func 值）可调用 */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func apply(add:func(i32,i32)->i32, x:i32, y:i32):i32 { return add(x, y); }"
        "func main(): void { var r = apply(add, 1, 2); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, FuncValueBuiltinPrintf) {
    /* 内建 printf 与普通函数同等看待：函数值赋值 + 调用 */
    EXPECT_TRUE(analyze(
        "func main(): void { var p = printf; p(\"%d\", 42); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, FuncValueComptimeFold) {
    /* comptime func 返回函数值：编译期折叠为签名 shadow */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "comptime func get_add():func(i32,i32)->i32 { return add; }"
        "func main(): void {"
        "  var add_fn = get_add();"
        "  var val = add_fn(1,2);"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* comptime func 同样建作用域树（Pass 3b 对其 shadow walk），
       global 下 add(0)、get_add(1)、main(2) 三个函数作用域 */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 2);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *val = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("val"));
    ASSERT_NE(val, nullptr);
    EXPECT_EQ(val->type, vm_->type_i32); /* 折叠后的函数调用返回 i32 */
}

/* ================================================================ */
/* 局部函数（local function）                                          */
/* ================================================================ */

TEST_F(SemaTest, LocalFuncBasic) {
    /* 局部函数定义：函数体内块作用域注册，无诊断。
       outer 体内局部函数 inc 只能访问参数 + 全局函数 add。 */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func outer(n:i32): i32 {"
        "  func inc(x:i32): i32 { return x + 1; }"
        "  func dbl(x:i32): i32 { return x * 2; }"
        "  return add(dbl(n), inc(n));"
        "}"
        "func main(): void { var r = outer(3); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* outer 的 fscope 下应有 inc / dbl 符号（局部函数提升注册） */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 1);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *inc = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("inc"));
    ASSERT_NE(inc, nullptr);
    EXPECT_EQ(inc->kind, SEMA_SYM_FUNC);
    sema_symbol_t *dbl = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("dbl"));
    ASSERT_NE(dbl, nullptr);
    EXPECT_EQ(dbl->kind, SEMA_SYM_FUNC);
}

TEST_F(SemaTest, LocalFuncHoistForwardReference) {
    /* 提升语义：局部函数调用先于定义点（前向引用），块内名字整个可见 */
    EXPECT_TRUE(analyze(
        "func outer(n:i32): i32 {"
        "  var r = caller(n);" /* 调用先于定义 */
        "  func caller(x:i32): i32 { return x + 1; }" /* 定义在后 */
        "  return r;"
        "}"
        "func main(): void { var r = outer(3); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncNestedBlock) {
    /* 嵌套块局部函数：块内定义 + 调用合法 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  { func inner(x:i32): i32 { return x; } var a = inner(1); }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncNestedBlockOutOfScopeRejected) {
    /* 出块引用 → 调用点 lookup 失败 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  { func inner(x:i32): i32 { return x; } }"
        "  var a = inner(1);"
        "}"));
    expect_message(0, "undefined variable");
}

TEST_F(SemaTest, LocalFuncCallsGlobalFunction) {
    /* 局部函数体内调用全局函数：合法（函数体查找链 = 参数 + 全局） */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func outer(n:i32): i32 {"
        "  func inc(x:i32): i32 { return add(x, 1); }"
        "  return inc(n);"
        "}"
        "func main(): void { var r = outer(3); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncRecursionRejected) {
    /* 局部函数递归：自身调用需闭包捕获，当前不支持 → 编译期报错 */
    EXPECT_FALSE(analyze(
        "func outer(n:i32): i32 {"
        "  func dec(x:i32): i32 {"
        "    if (x <= 0) { return 0; }"
        "    return dec(x - 1);"
        "  }"
        "  return dec(n);"
        "}"
        "func main(): void { var r = outer(5); }"));
    expect_message(0, "local function cannot reference sibling or self");
}

TEST_F(SemaTest, LocalFuncSiblingCallRejected) {
    /* 局部函数调用兄弟函数：符号只在定义作用域可见，调用需闭包 → 报错
       （统一 exec callee 后由 AST_IDENT 函数引用闭包检查拦截） */
    EXPECT_FALSE(analyze(
        "func outer(n:i32): i32 {"
        "  func caller(x:i32): i32 { return callee(x) + 1; }"
        "  func callee(x:i32): i32 { return x * 2; }"
        "  return caller(n);"
        "}"
        "func main(): void { var r = outer(3); }"));
    expect_message(0, "local function cannot reference sibling or self");
}

TEST_F(SemaTest, LocalFuncSiblingValueRefRejected) {
    /* 局部函数值引用兄弟函数（var f = callee）：同样需闭包 → 报错 */
    EXPECT_FALSE(analyze(
        "func outer(n:i32): i32 {"
        "  func caller(x:i32): i32 {"
        "    var f = callee;"
        "    return f(x);"
        "  }"
        "  func callee(x:i32): i32 { return x * 2; }"
        "  return caller(n);"
        "}"
        "func main(): void { var r = outer(3); }"));
    expect_message(0, "local function cannot reference sibling or self");
}

TEST_F(SemaTest, LocalFuncCaptureOuterLocalRejected) {
    /* 局部函数捕获外层局部变量：无闭包 → 报错 */
    EXPECT_FALSE(analyze(
        "func outer(n:i32): i32 {"
        "  var k = 10;"
        "  func inc(x:i32): i32 { return x + k; }"
        "  return inc(n);"
        "}"
        "func main(): void { var r = outer(3); }"));
    expect_message(0, "local function cannot access outer local");
}

TEST_F(SemaTest, LocalFuncCaptureOuterParamRejected) {
    /* 局部函数捕获外层函数参数：无闭包 → 报错 */
    EXPECT_FALSE(analyze(
        "func outer(n:i32): i32 {"
        "  func inc(x:i32): i32 { return x + n; }"
        "  return inc(n);"
        "}"
        "func main(): void { var r = outer(3); }"));
    expect_message(0, "local function cannot access outer local");
}

TEST_F(SemaTest, LocalFuncShadowsGlobalFunctionRejected) {
    /* 局部函数遮蔽全局函数名：compiler func_ids 平铺表会错绑 → 显式拒绝 */
    EXPECT_FALSE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "func outer(n:i32): i32 {"
        "  func add(x:i32): i32 { return x; }"
        "  return add(n);"
        "}"
        "func main(): void { var r = outer(3); }"));
    expect_message(0, "shadows a global function");
}

TEST_F(SemaTest, LocalFuncComptimeSkipped) {
    /* comptime 局部函数：调用点折叠，不建作用域树、不注册运行时 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  comptime func twice(x:i32): i32 { return x * 2; }"
        "  var a = twice(3);"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* comptime 局部函数不消费 3a 建的 fscope 子作用域（未建树） */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *a = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("a"));
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->type, vm_->type_i32);
}

TEST_F(SemaTest, LocalFuncSignatureRegistered) {
    /* 局部函数签名登记进 sema->types + fn->sig_id（compiler LOAD_TYPE 用）：
       类型表在全局函数注册段之后出现局部签名 */
    EXPECT_TRUE(analyze(
        "func outer(n:i32): i32 {"
        "  func inc(x:i32): i32 { return x + 1; }"
        "  return inc(n);"
        "}"
        "func main(): void { var r = outer(3); }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *inc = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("inc"));
    ASSERT_NE(inc, nullptr);
    EXPECT_EQ(inc->type->kind, TYPE_KIND_FUNC); /* 签名类型 */
}

} /* namespace */
