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
#include "vm/scope.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_enum_ref.h"
#include "parser/ast_struct_def.h"
#include "vm/type_struct.h"
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

/* ================================================================ */
/* optional（?T，docs m2-design §12）                               */
/* ================================================================ */

TEST_F(SemaTest, OptionalVarInitNil) {
    /* var n:?i32 = nil → NONE 态；n == nil 恒真 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var n:?i32 = nil;"
        "  if (n == nil) { }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, OptionalVarInitValueLifts) {
    /* var n:?i32 = 5 → D1 隐式提升 some；n != nil 判定合法 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var n:?i32 = 5;"
        "  if (n != nil) { }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, OptionalConstructorNilAndValue) {
    /* ?T 构造器二值单槽位：.{nil} none / .{v} some */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:?i32 = .?i32{nil};"
        "  var b:?i32 = .?i32{42};"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, OptionalConstructorFieldCountMustBeOne) {
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:?i32 = .?i32{1, 2};"
        "}"));
    expect_message(0, "optional constructor expects exactly 1 field, got 2");
}

TEST_F(SemaTest, OptionalConstructorWrongFieldType) {
    /* 字段类型与 inner 不匹配 → 赋值校验报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:?i32 = .?i32{\"s\"};"
        "}"));
    expect_message(0, "cannot initialize optional");
}

TEST_F(SemaTest, UnwrapAssertSomeRead) {
    /* .! assert 解包：SOME 分支内 x 仍为 ?i32，解包后得 inner i32 可赋值/算术 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x:?i32 = 5;"
        "  if (x != nil) {"
        "    var y:i32 = x.!;"
        "    var z:i32 = x.! + 1;"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, UnwrapAssertOnNonOptionalErrors) {
    /* .! 只接受 optional 操作数；对非 optional 值报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x:i32 = 5;"
        "  var y:i32 = x.!;"
        "}"));
    expect_message(0, "operator '.!' requires an optional operand");
}

TEST_F(SemaTest, UnwrapTryNotImplementedErrors) {
    /* .? try 仅词法预留，语义未实现 → 编译错误 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x:?i32 = 5;"
        "  var y = x.?;"
        "}"));
    expect_message(0, "operator '.?' (try) is not implemented yet");
}

TEST_F(SemaTest, UnwrapTryOnNonOptionalErrors) {
    /* .? 同样要求 optional 操作数；先过操作数类型校验再报未实现 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x:i32 = 5;"
        "  var y = x.?;"
        "}"));
    expect_message(0, "operator '.?' requires an optional operand");
}

TEST_F(SemaTest, UnwrapAssertElseBranch) {
    /* 无窄化记录：else 分支 x 仍为 ?i32，同样须 .! 解包 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x:?i32 = 5;"
        "  if (x != nil) { }"
        "  else { var y:i32 = x.!; }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, UnwrapAssertAndCombinationThen) {
    /* 复合条件：&& then 分支两侧各自解包 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:?i32 = 5;"
        "  var b:?i32 = 7;"
        "  if ((a != nil) && (b != nil)) {"
        "    var x:i32 = a.! + b.!;"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, UnwrapAssertOrCombinationElse) {
    /* 复合条件：|| else 分支两侧各自解包 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:?i32 = 5;"
        "  var b:?i32 = 7;"
        "  if (a == nil || b == nil) { }"
        "  else { var x:i32 = a.! + b.!; }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, UnwrapRequiredElseConservativeArithmeticErrors) {
    /* 无窄化：&& else 分支 a 未解包直接算术 → 报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:?i32 = 5;"
        "  var b:?i32 = 7;"
        "  if (a != nil && b != nil) { }"
        "  else { var x:i32 = a + 1; }"
        "}"));
    expect_message(0, "cannot apply '+' to ?i32");
}

TEST_F(SemaTest, UnwrapRequiredConflictConservativeArithmeticErrors) {
    /* 无窄化：矛盾约束（a != nil && a == nil）不产生流记录 → 算术报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:?i32 = 5;"
        "  if (a != nil && a == nil) {"
        "    var x:i32 = a + 1;"
        "  }"
        "}"));
    expect_message(0, "cannot apply '+' to ?i32");
}

TEST_F(SemaTest, UnwrapAssertNotFlip) {
    /* 一元 ! 翻转判定后解包 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var a:?i32 = 5;"
        "  if (!(a == nil)) {"
        "    var z:i32 = a.! + 1;"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, NilCompareNonOptionalErrors) {
    /* str/func 无空值（§12.3）：非 ?T 变量与 nil 比较 → 编译错误 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var s:str = \"a\";"
        "  if (s == nil) { }"
        "}"));
    expect_message(0, "is not optional; cannot compare with nil");
}

TEST_F(SemaTest, NilCanOnlyCompareWithOptionalVariable) {
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  if (nil == 0) { }"
        "}"));
    expect_message(0, "nil can only be compared with an optional variable");
}

TEST_F(SemaTest, NilAssignToNonOptionalErrors) {
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var z:u64 = nil;"
        "}"));
    expect_message(0, "cannot initialize variable 'z' of type u64 with nil");
}

TEST_F(SemaTest, NilNoTypeInference) {
    /* var n = nil → 无法推断类型（nil 非 value，须显式 ?T 注解） */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var n = nil;"
        "}"));
    expect_message(0, "cannot infer type of optional variable 'n' from nil");
}

TEST_F(SemaTest, NilNotTypeName) {
    /* nil 不是类型名（type_lookup("nil")=NULL） */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var a:nil = nil;"
        "}"));
    expect_message(0, "unsupported type expression");
}

TEST_F(SemaTest, NilAssignToOptionalInBranchLegal) {
    /* 无窄化：SOME 分支内 x 仍为 ?i32，nil 赋值合法（可再置回 none） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x:?i32 = 5;"
        "  if (x != nil) { x = nil; }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, OptionalNoNarrowingAfterCallArithmeticErrors) {
    /* 无窄化：函数调用后 x 仍为 ?i32，未解包算术报错（不再依赖调用后清窄化） */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x:?i32 = 5;"
        "  if (x != nil) {"
        "    foo();"
        "    var y:i32 = x + 1;"
        "  }"
        "}"
        "func foo() : void { }"));
    expect_message(0, "cannot apply '+' to ?i32");
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

TEST_F(SemaTest, ComptimeVarArrayPartialFillZeroPads) {
    /* comptime var 数组 fill 值包（M2 construct 完全显式）：.<3>i32{ 1, <0,2> }
       总元素数 = 1 + 2 = 3 == 长度，CTFE 求值 fill 展开为逐元素零值 */
    EXPECT_TRUE(analyze(
        "comptime var A: [3]i32 = .[3]i32{ 1, <0,2> };"
        "func main(): void { var x = A; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *a = sema_lookup(sema_->global_scope, STRSLICE_LIT("A"));
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->ct_valid);
    ASSERT_NE(a->ct.type, nullptr);
    EXPECT_EQ(array_type_len(a->ct.type), 3u);
    ASSERT_NE(a->ct.elems, nullptr);
    EXPECT_EQ(a->ct.elems[0].i, 1);
    EXPECT_EQ(a->ct.elems[1].i, 0); /* fill <0,2> 展开 */
    EXPECT_EQ(a->ct.elems[2].i, 0);
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
/* 全局变量（运行时实体，init 编译期折叠为字面量/函数引用）             */
/* ================================================================ */

TEST_F(SemaTest, GlobalVarBasic) {
    /* 全局变量：符号注册 + 激活（is_comptime 不设——运行期实体）。
       函数体引用不折叠（保持 AST_IDENT），shadow value 定义到
       vm->root_scope（与运行时 DEFINE 落 root_scope 对齐）。 */
    EXPECT_TRUE(analyze(
        "var g: i32 = 42;"
        "func main(): void { var x = g; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *g = sema_lookup(sema_->global_scope, STRSLICE_LIT("g"));
    ASSERT_NE(g, nullptr);
    EXPECT_FALSE(g->is_comptime);
    EXPECT_FALSE(g->ct_valid);
    EXPECT_TRUE(g->flow_init);
    EXPECT_TRUE(g->is_active);
    EXPECT_EQ(g->type, vm_->type_i32);

    /* shadow value 在 vm root_scope（3b walk 时函数体可查到） */
    value_t *sv = scope_lookup(vm_->root_scope, STRSLICE_LIT("g"));
    ASSERT_NE(sv, nullptr);
    EXPECT_EQ(value_type(sv), vm_->type_i32);
    EXPECT_TRUE(value_is_shadow(sv));
}

TEST_F(SemaTest, GlobalVarInference) {
    /* 无显式类型：从折叠右值推断类型（i32 字面量） */
    EXPECT_TRUE(analyze(
        "var n = 7;"
        "func main(): void { var x = n; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *n = sema_lookup(sema_->global_scope, STRSLICE_LIT("n"));
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->type, vm_->type_i32);
}

TEST_F(SemaTest, GlobalVarString) {
    /* 字符串全局变量：str 类型 + init 折叠为 AST_STRING_LIT */
    EXPECT_TRUE(analyze(
        "var s: str = \"hi\";"
        "func main(): void { var x = s; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *s = sema_lookup(sema_->global_scope, STRSLICE_LIT("s"));
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->type, vm_->type_str);

    /* init 折叠为字面量节点（AST_STRING_LIT）写回 vd->init */
    ast_program_t *prog = (ast_program_t *)ast_;
    ASSERT_NE(prog, nullptr);
    ast_node_t *f = prog->funcs;
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->kind, AST_VAR_DEF);
    EXPECT_EQ(((ast_var_def_t *)f)->init->kind, AST_STRING_LIT);
}

TEST_F(SemaTest, GlobalVarExprFold) {
    /* 右值编译期可计算表达式：折叠为字面量写回 init */
    EXPECT_TRUE(analyze(
        "var x = (1 + 2) * 3 - 4;"
        "func main(): void { var y = x; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *x = sema_lookup(sema_->global_scope, STRSLICE_LIT("x"));
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(x->type, vm_->type_i32);

    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *f = prog->funcs;
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->kind, AST_VAR_DEF);
    EXPECT_EQ(((ast_var_def_t *)f)->init->kind, AST_INT_LIT);
}

TEST_F(SemaTest, GlobalVarFuncRef) {
    /* 右值 = 函数名引用：折叠为 AST_FUNC_REF（compiler 发 LOAD_FUNCTION） */
    EXPECT_TRUE(analyze(
        "func add(a:i32, b:i32):i32 { return a + b; }"
        "var f = add;"
        "func main(): void { var x = f; }"));
    EXPECT_FALSE(diag_has_error(diag_));

    ast_program_t *prog = (ast_program_t *)ast_;
    ast_node_t *f = prog->funcs;
    ASSERT_NE(f, nullptr);
    /* prog->funcs = [add(func), f(var), main(func)] → f 在第二个 */
    ast_node_t *second = f->next;
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(second->kind, AST_VAR_DEF);
    EXPECT_EQ(((ast_var_def_t *)second)->init->kind, AST_FUNC_REF);
}

TEST_F(SemaTest, GlobalVarTypeMismatch) {
    /* 显式类型与折叠右值不匹配 → 诊断 */
    EXPECT_FALSE(analyze(
        "var a: i32 = \"hello\";"
        "func main(): void { }"));
    expect_message(0, "cannot initialize global variable 'a' of type i32 with str");
}

TEST_F(SemaTest, GlobalVarNotConstant) {
    /* 右值引用运行期实体（其他全局变量）→ 不可折叠诊断 */
    EXPECT_FALSE(analyze(
        "var a: i32 = 1;"
        "var b: i32 = a;"
        "func main(): void { }"));
    expect_message(0, "initializer must be a compile-time constant");
}

TEST_F(SemaTest, GlobalVarUnknownType) {
    /* 非法类型名（如 "string"，内建为 "str"）→ unknown type 诊断，
       不静默放行到运行时（对齐 shadow_var_def 语义） */
    EXPECT_FALSE(analyze(
        "var s: string = \"hi\";"
        "func main(): void { }"));
    expect_message(0, "unknown type");
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

TEST_F(SemaTest, LocalFuncCaptureTdzCompileTimeRejected) {
    /* 闭包捕获 TDZ（编译期）：有捕获的局部函数在定义点前被引用 → 捕获值
       尚未绑定（resolve_func_captures 未执行，is_active=false）→ 编译期
       报错，不静默到运行期（捕获槽 undefined 占位） */
    EXPECT_FALSE(analyze(
        "func outer(): i32 {"
        "  var r = f(1);" /* 定义点前调用有捕获局部函数 → TDZ */
        "  var x = 42;"
        "  func |x| f(v: i32): i32 { return x + v; }"
        "  return r;"
        "}"
        "func main(): void { var r = outer(); }"));
    expect_message(0, "used before its captures are bound (TDZ)");
}

TEST_F(SemaTest, LocalFuncCaptureTdzAfterDefinitionOk) {
    /* 定义点之后引用有捕获局部函数：捕获已绑定（is_active=true）→ 合法 */
    EXPECT_TRUE(analyze(
        "func outer(): i32 {"
        "  var x = 42;"
        "  func |x| f(v: i32): i32 { return x + v; }"
        "  var r = f(1);" /* 定义点后调用 → 合法 */
        "  return r;"
        "}"
        "func main(): void { var r = outer(); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncCaptureValueRefBeforeDefOk) {
    /* 定义点前值引用有捕获局部函数（var f = b，b 定义在后）：块入口已实例化
       绑定（MAKE_FUNCTION+DEFINE），f/b 浅拷贝共享同一 func_t → TDZ 仅对
       调用点生效，值引用放行 */
    EXPECT_TRUE(analyze(
        "func outer(): i32 {"
        "  var base = 3;"
        "  var f = b;" /* 定义点前值引用有捕获局部函数 → 放行 */
        "  func |base| b(n: i32): i32 { return base * n; }"
        "  var r = f(2);"
        "  return r;"
        "}"
        "func main(): void { var r = outer(); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncSiblingCaptureBackwardWithCapturesOk) {
    /* 后向兄弟捕获 + 兄弟自身也有捕获：a 捕获 b（定义在后、b 捕获 base）。
       块入口 b 实例已绑定名字，a 的定义点捕获绑定拿到 b 实例浅拷贝，b 的
       捕获在其定义点 SET_CLOSURE 填齐 → 合法 */
    EXPECT_TRUE(analyze(
        "func outer(): i32 {"
        "  var base = 3;"
        "  func |b| a(n: i32): i32 { return b(n); }"
        "  func |base| b(n: i32): i32 { return base * n; }"
        "  var r = a(2);"
        "  return r;"
        "}"
        "func main(): void { var r = outer(); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncCaptureCallBeforeDefStillRejected) {
    /* 定义点前调用有捕获局部函数：值引用放行后调用点 TDZ 拦截保持不变
       （in_call_callee 区分调用/值引用） */
    EXPECT_FALSE(analyze(
        "func outer(): i32 {"
        "  var base = 3;"
        "  var r = b(2);" /* 定义点前调用 → 仍 TDZ */
        "  func |base| b(n: i32): i32 { return base * n; }"
        "  return r;"
        "}"
        "func main(): void { var r = outer(); }"));
    expect_message(0, "used before its captures are bound (TDZ)");
}

TEST_F(SemaTest, LocalFuncNoCaptureForwardRefOk) {
    /* 无捕获局部函数前向引用（定义点前调用）：hoist 只绑定地址，无捕获槽
       → 合法（与 LocalFuncHoistForwardReference 语义一致，此处为 TDZ 检查
       不误伤无捕获函数的回归用例） */
    EXPECT_TRUE(analyze(
        "func outer(): i32 {"
        "  var r = g(1);" /* 定义点前调用无捕获局部函数 → 合法 */
        "  func g(v: i32): i32 { return v + 1; }"
        "  return r;"
        "}"
        "func main(): void { var r = outer(); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncSelfCaptureRecursionOk) {
    /* 显式捕获自身递归：func |fib| fib(...) 捕获列表含自身 → 定义点 STORE
       后绑定新实例，函数体内自调用合法 */
    EXPECT_TRUE(analyze(
        "func outer(n:i32): i32 {"
        "  func |fib| fib(x:i32): i32 {"
        "    if (x <= 1) { return x; }"
        "    return fib(x - 1) + fib(x - 2);"
        "  }"
        "  return fib(n);"
        "}"
        "func main(): void { var r = outer(10); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncSelfCaptureWithOuterVarOk) {
    /* 捕获外层变量 + 自身递归：捕获列表 [base, pow] 中 base 是外层 var，
       pow 是自身——函数符号捕获跳过 flow_init 检查（提升即存在） */
    EXPECT_TRUE(analyze(
        "func outer(): i32 {"
        "  var base: i32 = 2;"
        "  func |base, pow| pow(x:i32): i32 {"
        "    if (x == 0) { return 1; }"
        "    return base * pow(x - 1);"
        "  }"
        "  return pow(4);"
        "}"
        "func main(): void { var r = outer(); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncSiblingCaptureOk) {
    /* 兄弟函数捕获：a 捕获 b（定义在后，提升即存在），b 无捕获 → 合法。
       兄弟互调须显式捕获列表声明 */
    EXPECT_TRUE(analyze(
        "func outer(n:i32): i32 {"
        "  func |b| a(x:i32): i32 { return b(x); }"
        "  func b(x:i32): i32 { return x * 10; }"
        "  return a(n);"
        "}"
        "func main(): void { var r = outer(5); }"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, LocalFuncNoCaptureSelfRefStillRejected) {
    /* 无捕获自身引用仍拦截：不加捕获列表的自递归保持编译期报错
       （与 LocalFuncRecursionRejected 一致，提示加入捕获列表） */
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

/* ---- switch 语句（docs m2-design §4：if 语法糖） ---- */

TEST_F(SemaTest, SwitchBasicValid) {
    /* 基本 switch：cond 为运行期 i32 变量，模式列表匹配 → 合法 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x = 2;"
        "  switch (x) {"
        "    (1)->{ var a = 10; }"
        "    (2)->{ var a = 20; }"
        "    default->{ var a = 30; }"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, SwitchMultiPatternValid) {
    /* 多模式（逗号 = || 链）：同一分支多个模式 → 合法 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x = 2;"
        "  switch (x) {"
        "    (1, 2)->{ var a = 10; }"
        "    (3)->{ var a = 20; }"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, SwitchRuntimeCondValid) {
    /* 条件可为任意运行期表达式（文档：不限常量） */
    EXPECT_TRUE(analyze(
        "func f():i32 { return 1; }"
        "func main(): void {"
        "  var x = 5;"
        "  switch (x + f()) {"
        "    (6)->{ }"
        "    default->{ }"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, SwitchPatternTypeMismatchErrors) {
    /* 模式与 cond 类型不可比（i32 vs str）→ 报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x = 2;"
        "  switch (x) {"
        "    (\"a\")->{ }"
        "    default->{ }"
        "  }"
        "}"));
    expect_message(0, "switch pattern type mismatch: cannot match i32 against str");
}

TEST_F(SemaTest, SwitchMissingDefaultDefiniteAssignmentErrors) {
    /* 无 default：仅单分支赋值，其他分支/穿透路径未赋值 → 读取报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x = 2;"
        "  var r:i32 = undefined;"
        "  switch (x) {"
        "    (1)->{ r = 10; }"
        "    (2)->{ }"
        "  }"
        "  var y = r + 1;"
        "}"));
    expect_message(0, "variable 'r' used before initialization");
}

TEST_F(SemaTest, SwitchDefaultCoversDefiniteAssignment) {
    /* 有 default：全部分支赋值 → 确定性初始化，读取合法 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x = 2;"
        "  var r:i32 = undefined;"
        "  switch (x) {"
        "    (1)->{ r = 10; }"
        "    (2)->{ r = 20; }"
        "    default->{ r = 30; }"
        "  }"
        "  var y = r + 1;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, SwitchAllCasesAssignNoDefaultStillErrors) {
    /* 无 default：即使所有 case 都赋值，穿透路径仍未赋值 → 保守报错 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x = 2;"
        "  var r:i32 = undefined;"
        "  switch (x) {"
        "    (1)->{ r = 10; }"
        "    (2)->{ r = 20; }"
        "  }"
        "  var y = r + 1;"
        "}"));
    expect_message(0, "variable 'r' used before initialization");
}

TEST_F(SemaTest, SwitchNoDefaultMissingReturnErrors) {
    /* 无 default：全分支 return 仍可能穿透 → 非 void 函数报错 */
    EXPECT_FALSE(analyze(
        "func f(x:i32):i32 {"
        "  switch (x) {"
        "    (1)->{ return 10; }"
        "    (2)->{ return 20; }"
        "  }"
        "}"));
    expect_message(0, "must return a value on all paths");
}

TEST_F(SemaTest, SwitchDefaultAllBranchesReturnOk) {
    /* 有 default 且全分支 return → definitely_returns */
    EXPECT_TRUE(analyze(
        "func f(x:i32):i32 {"
        "  switch (x) {"
        "    (1)->{ return 10; }"
        "    (2)->{ return 20; }"
        "    default->{ return 30; }"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, SwitchMissingDefaultReturnStillErrors) {
    /* 有 default 但某分支缺 return → 不保证全路径返回 */
    EXPECT_FALSE(analyze(
        "func f(x:i32):i32 {"
        "  switch (x) {"
        "    (1)->{ return 10; }"
        "    (2)->{ }"
        "    default->{ return 30; }"
        "  }"
        "}"));
    expect_message(0, "must return a value on all paths");
}

TEST_F(SemaTest, SwitchNestedValid) {
    /* 嵌套 switch：内外层各自独立子作用域 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x = 1;"
        "  var y = 2;"
        "  switch (x) {"
        "    (1)->{"
        "      switch (y) {"
        "        (2)->{ }"
        "        default->{ }"
        "      }"
        "    }"
        "    default->{ }"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, SwitchBranchScopeIsolation) {
    /* 分支体独立作用域：不同分支可定义同名变量，互不冲突 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x = 1;"
        "  switch (x) {"
        "    (1)->{ var tmp = 10; }"
        "    (2)->{ var tmp = 20; }"
        "    default->{ var tmp = 30; }"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, SwitchCondExprUnusedNoError) {
    /* switch 条件不要求是语句：cond 求值结果本身不算"表达式结果未使用" */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var x = 3;"
        "  switch (x * 2) {"
        "    (6)->{ }"
        "    default->{ }"
        "  }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

/* ================================================================ */
/* enum 类型（M2：enum Name:Underlying { Var = val, ... }）           */
/* ================================================================ */

TEST_F(SemaTest, EnumDefBasic) {
    /* 合法 enum：variant 显式值 + 引用 Color::Red + 同 enum 赋值 */
    EXPECT_TRUE(analyze(
        "enum Color:i32 { Red = 1, Green = 2, Blue = 3 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  var d: Color = c;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* 符号激活 + 类型登记 */
    sema_symbol_t *sym =
        sema_lookup(sema_->global_scope, STRSLICE_LIT("Color"));
    ASSERT_NE(sym, nullptr);
    EXPECT_TRUE(sym->is_active);
    EXPECT_TRUE(sym->flow_init);
    ASSERT_NE(sym->type, nullptr);
    EXPECT_EQ(sym->type->kind, TYPE_KIND_ENUM);

    /* enum 类型登记 + id 绑定（compiler LOAD_TYPE 用） */
    const sema_type_t *st = sema_type_find(sema_, sym->type);
    ASSERT_NE(st, nullptr);
    EXPECT_GE(st->id, TYPE_ID_PROGRAM_BASE);

    /* variant 表内容 */
    EXPECT_EQ(enum_type_variant_count(sym->type), 3u);
    ASSERT_NE(enum_type_variant(sym->type, 0), nullptr);
    EXPECT_TRUE(strslice_eq(enum_type_variant(sym->type, 0)->name,
                            STRSLICE_LIT("Red")));
    EXPECT_EQ(enum_type_variant(sym->type, 0)->value, 1);
    EXPECT_EQ(enum_type_underlying(sym->type), vm_->type_i32);

    /* var c/d 类型解析为 enum 类型 */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *c = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("c"));
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->type, sym->type);
}

TEST_F(SemaTest, EnumDefU8Underlying) {
    /* u8 底层 + 显式 as i8 的 variant 值（1 是 i32 字面量，须 as 到底层宽度） */
    EXPECT_TRUE(analyze(
        "enum Small:i8 { A = 1 as i8, B = 2 as i8 }"
        "func main(): void {"
        "  var s: Small = Small::B;"
        "  if (s == Small::B) { }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *sym =
        sema_lookup(sema_->global_scope, STRSLICE_LIT("Small"));
    ASSERT_NE(sym, nullptr);
    EXPECT_EQ(enum_type_underlying(sym->type), vm_->type_i8);
}

TEST_F(SemaTest, EnumRefInGlobalVarInit) {
    /* 全局 enum 变量 init 经 ctfe 折叠：AST_ENUM_REF 须在 ctfe 求值成功 */
    EXPECT_TRUE(analyze(
        "enum Color:i32 { Red = 1 }"
        "var g: Color = Color::Red;"
        "func main(): void {"
        "  var c: Color = g;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, EnumVariantValueNarrowingError) {
    /* 用户契约：variant 值与底层不兼容（i32 → i8 底层）编译报错 */
    EXPECT_FALSE(analyze(
        "enum Color:i8 { Red = 300 }"
        "func main(): void { }"));
    expect_message(0, "is not compatible with underlying type i8");
}

TEST_F(SemaTest, EnumVariantNonIntValueError) {
    /* variant 值必须整型 */
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = true }"
        "func main(): void { }"));
    expect_message(0, "value must be an integer, got bool");
}

TEST_F(SemaTest, EnumVariantNonConstError) {
    /* variant 值必须编译期常量 */
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = foo() }"
        "func main(): void { }"
        "func foo(): i32 { return 1; }"));
    expect_message(0, "must be a compile-time integer constant");
}

TEST_F(SemaTest, EnumUnderlyingNotIntError) {
    /* 底层类型必须整型（bool 报错） */
    EXPECT_FALSE(analyze(
        "enum C:bool { A = true }"
        "func main(): void { }"));
    expect_message(0, "underlying type must be an integer, got bool");
}

TEST_F(SemaTest, EnumUnknownUnderlyingError) {
    EXPECT_FALSE(analyze(
        "enum C:NoSuch { A = 1 }"
        "func main(): void { }"));
    expect_message(0, "unknown underlying type");
}

TEST_F(SemaTest, EnumDuplicateVariantNameError) {
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1, Red = 2 }"
        "func main(): void { }"));
    expect_message(0, "duplicate variant name 'Red'");
}

TEST_F(SemaTest, EnumDuplicateVariantValueError) {
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1, Green = 1 }"
        "func main(): void { }"));
    expect_message(0, "duplicate variant value 1");
}

TEST_F(SemaTest, EnumAssignIntToEnumError) {
    /* i32 → enum：无隐式转换，编译报错 */
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1 }"
        "func main(): void {"
        "  var x: Color = 1;"
        "}"));
    expect_message(0, "cannot initialize variable 'x' of type Color with i32");
}

TEST_F(SemaTest, EnumAssignEnumToIntError) {
    /* enum → i32：无隐式转换（严格分离），编译报错 */
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  var x: i32 = c;"
        "}"));
    expect_message(0, "cannot initialize variable 'x' of type i32 with Color");
}

TEST_F(SemaTest, EnumAssignDifferentEnumError) {
    /* 不同 enum 实例间赋值：严格分离 */
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1 }"
        "enum Mood:i32 { Happy = 1 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  var m: Mood = c;"
        "}"));
    expect_message(0, "cannot initialize variable 'm' of type Mood with Color");
}

TEST_F(SemaTest, EnumCastSkipStepError) {
    /* 跳步 cast（enum → i8 而非声明底层 i32）：严格分离 */
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  var y: i8 = c as i8;"
        "}"));
    expect_message(0, "cannot cast Color to i8");
}

TEST_F(SemaTest, EnumCastTwoStepOk) {
    /* 两步 cast：enum → i32 → i8 合法 */
    EXPECT_TRUE(analyze(
        "enum Color:i32 { Red = 1 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  var v: i8 = c as i32 as i8;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, EnumEqSameEnumOk) {
    EXPECT_TRUE(analyze(
        "enum Color:i32 { Red = 1, Green = 2 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  if (c == Color::Green) { }"
        "  if (c != Color::Red) { }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, EnumEqVsUnderlyingError) {
    /* enum 与底层整型判等：严格分离 */
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  if (c == 1) { }"
        "}"));
    expect_message(0, "cannot apply '==' to Color and i32");
}

TEST_F(SemaTest, EnumEqDifferentEnumError) {
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1 }"
        "enum Mood:i32 { Happy = 1 }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  var m: Mood = Mood::Happy;"
        "  if (c == m) { }"
        "}"));
    expect_message(0, "cannot apply '==' to Color and Mood");
}

TEST_F(SemaTest, EnumLocalDefInFunc) {
    /* 局部 enum（函数体内定义）：声明/引用/同 enum 赋值/判等 */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  enum Color:i32 { Red = 1, Green = 2 }"
        "  var c: Color = Color::Red;"
        "  var d: Color = c;"
        "  if (c == Color::Green) { }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* 符号注册在函数体块作用域（fscope 的 param_scope 子） */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *sym =
        sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("Color"));
    ASSERT_NE(sym, nullptr);
    EXPECT_TRUE(sym->is_active);
    EXPECT_EQ(sym->type->kind, TYPE_KIND_ENUM);
    EXPECT_EQ(enum_type_underlying(sym->type), vm_->type_i32);

    /* enum 类型登记 + id 绑定（compiler LOAD_TYPE 用） */
    const sema_type_t *st = sema_type_find(sema_, sym->type);
    ASSERT_NE(st, nullptr);
    EXPECT_GE(st->id, TYPE_ID_PROGRAM_BASE);
}

TEST_F(SemaTest, EnumLocalNestedBlockShadow) {
    /* 局部 enum 在嵌套块定义 + 遮蔽外层同名 enum */
    EXPECT_TRUE(analyze(
        "enum A:i32 { X = 1 }"
        "func main(): void {"
        "  {"
        "    enum A:i32 { X = 10 }"
        "    var a: A = A::X;"
        "  }"
        "  var g: A = A::X;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, EnumLocalForwardRef) {
    /* 局部 enum 前向引用（提升语义：与局部 type def 一致） */
    EXPECT_TRUE(analyze(
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  enum Color:i32 { Red = 1 }"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, EnumLocalStrictSeparationError) {
    /* 局部 enum 严格分离同样生效：variant 不能赋给整型 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  enum Color:i32 { Red = 1 }"
        "  var x: i32 = Color::Red;"
        "}"));
    expect_message(0, "cannot initialize variable 'x' of type i32 with Color");
}

TEST_F(SemaTest, EnumLocalSkipStepCastError) {
    /* 局部 enum 跳步 cast 拒绝 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  enum Color:i32 { Red = 1 }"
        "  var c: Color = Color::Red;"
        "  var y: i8 = c as i8;"
        "}"));
    expect_message(0, "cannot cast Color to i8");
}

TEST_F(SemaTest, EnumLocalDuplicateNameError) {
    /* 局部 enum 与块内 var 重名 → 3a 建树报重复 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  enum Color:i32 { Red = 1 }"
        "  var Color: i32 = 5;"
        "}"));
    EXPECT_TRUE(diag_has_error(diag_));
}

TEST_F(SemaTest, EnumRefUnknownVariantError) {
    EXPECT_FALSE(analyze(
        "enum Color:i32 { Red = 1 }"
        "func main(): void {"
        "  var c = Color::Xyz;"
        "}"));
    expect_message(0, "enum 'Color' has no variant 'Xyz'");
}

TEST_F(SemaTest, EnumRefNonEnumTypeError) {
    EXPECT_FALSE(analyze(
        "type MyInt = i32;"
        "func main(): void {"
        "  var c = MyInt::Red;"
        "}"));
    expect_message(0, "'i32' is not an enum type");
}

TEST_F(SemaTest, EnumDefUsedInSignature) {
    /* enum 类型可作函数签名/参数类型（pass1b 先于签名解析） */
    EXPECT_TRUE(analyze(
        "enum Color:i32 { Red = 1 }"
        "func is_red(c: Color): bool { return c == Color::Red; }"
        "func main(): void {"
        "  var c: Color = Color::Red;"
        "  var b: bool = is_red(c);"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

/* ================================================================ */
/* struct 类型定义                                                    */
/* ================================================================ */

TEST_F(SemaTest, StructDefBasic) {
    /* 合法 struct：字段显式类型 + 声明 struct 类型变量 */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func main(): void {"
        "  var p: Point = undefined;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    /* 符号激活 + 类型登记 */
    sema_symbol_t *sym =
        sema_lookup(sema_->global_scope, STRSLICE_LIT("Point"));
    ASSERT_NE(sym, nullptr);
    EXPECT_TRUE(sym->is_active);
    EXPECT_TRUE(sym->flow_init);
    ASSERT_NE(sym->type, nullptr);
    EXPECT_EQ(sym->type->kind, TYPE_KIND_STRUCT);

    /* struct 类型登记 + id 绑定（compiler LOAD_TYPE 用） */
    const sema_type_t *st = sema_type_find(sema_, sym->type);
    ASSERT_NE(st, nullptr);
    EXPECT_GE(st->id, TYPE_ID_PROGRAM_BASE);

    /* 字段表内容 */
    EXPECT_EQ(struct_type_field_count(sym->type), 2u);
    ASSERT_NE(struct_type_field(sym->type, 0), nullptr);
    EXPECT_TRUE(strslice_eq(struct_type_field(sym->type, 0)->name,
                            STRSLICE_LIT("x")));
    EXPECT_EQ(struct_type_field(sym->type, 0)->type, vm_->type_i32);
    ASSERT_NE(struct_type_field(sym->type, 1), nullptr);
    EXPECT_TRUE(strslice_eq(struct_type_field(sym->type, 1)->name,
                            STRSLICE_LIT("y")));
    EXPECT_EQ(struct_type_field(sym->type, 1)->type, vm_->type_i32);

    /* C 对齐布局：x: i32(0), y: i32(4), size=8, align=4 */
    EXPECT_EQ(struct_type_field(sym->type, 0)->offset, 0u);
    EXPECT_EQ(struct_type_field(sym->type, 1)->offset, 4u);
    EXPECT_EQ(sym->type->size, 8u);
    EXPECT_EQ(sym->type->align, 4u);

    /* var p 类型解析为 struct 类型 */
    sema_scope_t *fscope = sema_scope_child(sema_->global_scope, 0);
    ASSERT_NE(fscope, nullptr);
    sema_symbol_t *p = sema_scope_find_local(func_param_scope(fscope), STRSLICE_LIT("p"));
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->type, sym->type);
}

TEST_F(SemaTest, StructDefUsedInSignature) {
    /* struct 类型可作函数签名/参数类型（pass1b 先于签名解析） */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func x_of(p: Point): i32 { return 0; }"
        "func main(): void {"
        "  var p: Point = undefined;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));
}

TEST_F(SemaTest, StructDefNestedFieldType) {
    /* 字段类型可为其他 struct 类型（布局依赖：先构造字段类型） */
    EXPECT_TRUE(analyze(
        "struct Inner { a: i32; }"
        "struct Outer { i: Inner; b: i64; }"
        "func main(): void {"
        "  var o: Outer = undefined;"
        "}"));
    EXPECT_FALSE(diag_has_error(diag_));

    sema_symbol_t *outer =
        sema_lookup(sema_->global_scope, STRSLICE_LIT("Outer"));
    ASSERT_NE(outer, nullptr);
    ASSERT_EQ(outer->type->kind, TYPE_KIND_STRUCT);
    /* Inner: size=4 align=4 → Outer: i@0, b@8(align_up(4,8)), size=16, align=8 */
    ASSERT_NE(struct_type_field(outer->type, 0), nullptr);
    EXPECT_EQ(struct_type_field(outer->type, 0)->offset, 0u);
    ASSERT_NE(struct_type_field(outer->type, 1), nullptr);
    EXPECT_EQ(struct_type_field(outer->type, 1)->offset, 8u);
    EXPECT_EQ(outer->type->size, 16u);
    EXPECT_EQ(outer->type->align, 8u);
}

TEST_F(SemaTest, StructDefDuplicateFieldNameError) {
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; x: i64; }"
        "func main(): void { }"));
    expect_message(0, "duplicate field name");
}

TEST_F(SemaTest, StructDefUnknownFieldTypeError) {
    EXPECT_FALSE(analyze(
        "struct Point { x: Nope; }"
        "func main(): void { }"));
    expect_message(0, "unknown field type");
}

TEST_F(SemaTest, StructDefNameCollisionError) {
    /* struct 名与已有类型重名（pass1_names dup） */
    EXPECT_FALSE(analyze(
        "type MyInt = i32;"
        "struct MyInt { x: i32; }"
        "func main(): void { }"));
    expect_message(0, "duplicate name 'MyInt'");
}

/* ---- struct 构造 + 字段访问（M2 构造/字段流程） ---- */

TEST_F(SemaTest, StructConstructNamedAndAnonPasses) {
    /* 具名构造 .Point{...} + 匿名构造 .{...}（鸭子类型按字段名匹配目标） */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = 1, .y = 2 };"
        "  var q: Point = .{ .x = 7, .y = 8 };"
        "}"));
}

TEST_F(SemaTest, StructConstructFieldOrderFreePasses) {
    /* 具名字段构造字段序可乱（按名匹配，与声明序无关） */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func main(): void {"
        "  var a: Point = .Point { .y = 2, .x = 1 };"
        "}"));
}

TEST_F(SemaTest, StructFieldAccessReadWritePasses) {
    /* 字段读取 + 写入 + 复合赋值 + 嵌套链式访问 */
    EXPECT_TRUE(analyze(
        "struct Inner { v: i32; }"
        "struct Outer { a: Inner; b: i32; }"
        "func main(): void {"
        "  var o: Outer = .Outer { .a = .Inner { .v = 3 }, .b = 9 };"
        "  var x = o.a.v;"
        "  o.b = 5;"
        "  o.b += 1;"
        "  o.a.v *= 2;"
        "}"));
}

TEST_F(SemaTest, StructConstructUnknownFieldError) {
    /* 构造时给出不存在的字段 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .z = 1 };"
        "}"));
    expect_message(0, "has no field 'z'");
}

TEST_F(SemaTest, StructConstructMissingFieldError) {
    /* 构造漏字段 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = 1 };"
        "}"));
}

TEST_F(SemaTest, StructConstructFieldTypeMismatchError) {
    /* 构造字段类型不匹配 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = \"s\" };"
        "}"));
}

TEST_F(SemaTest, StructFieldGetUnknownFieldError) {
    /* 读取不存在的字段 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = 1 };"
        "  var y = p.nope;"
        "}"));
    expect_message(0, "has no field 'nope'");
}

TEST_F(SemaTest, StructFieldGetNonStructError) {
    /* 非 struct 值取字段 → 诊断 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var x = 1;"
        "  var y = x.a;"
        "}"));
}

TEST_F(SemaTest, StructFieldAssignTypeMismatchError) {
    /* 字段赋值类型不匹配 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = 1 };"
        "  p.x = \"s\";"
        "}"));
}

TEST_F(SemaTest, StructFieldAssignUnknownError) {
    /* 写入不存在的字段 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = 1 };"
        "  p.z = 2;"
        "}"));
    expect_message(0, "has no field 'z'");
}

TEST_F(SemaTest, StructAssignWholeValuePasses) {
    /* struct 变量整体赋值（同类型深拷贝） */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = 1 };"
        "  var q: Point = .Point { .x = 2 };"
        "  q = p;"
        "}"));
}

/* ---- 结构兼容赋值（鸭子类型，m2-design §2）：布局+字段完全相等可赋值 ---- */

TEST_F(SemaTest, StructAssignLayoutCompatiblePasses) {
    /* 两个具名 struct 字段名+类型+顺序一致 → 结构兼容，双向赋值合法 */
    EXPECT_TRUE(analyze(
        "struct A { x: i32; y: i32; }"
        "struct B { x: i32; y: i32; }"
        "func main(): void {"
        "  var a: A = .A { .x = 1, .y = 2 };"
        "  var b: B = .B { .x = 3, .y = 4 };"
        "  b = a;"
        "  a = b;"
        "}"));
}

TEST_F(SemaTest, StructAssignLayoutMismatchError) {
    /* 字段类型不同（i32 vs i64）→ 不兼容，赋值报错 */
    EXPECT_FALSE(analyze(
        "struct A { x: i32; }"
        "struct B { x: i64; }"
        "func main(): void {"
        "  var a: A = .A { .x = 1 };"
        "  var b: B = .B { .x = 2 };"
        "  b = a;"
        "}"));
}

TEST_F(SemaTest, StructAssignFieldOrderMismatchError) {
    /* 字段顺序不同 → 布局不同，不兼容，赋值报错 */
    EXPECT_FALSE(analyze(
        "struct A { x: i32; y: i32; }"
        "struct B { y: i32; x: i32; }"
        "func main(): void {"
        "  var a: A = .A { .x = 1, .y = 2 };"
        "  var b: B = .B { .y = 3, .x = 4 };"
        "  b = a;"
        "}"));
}

TEST_F(SemaTest, StructAssignFieldNameMismatchError) {
    /* 字段名不同（x vs v）→ 不兼容，赋值报错 */
    EXPECT_FALSE(analyze(
        "struct A { x: i32; }"
        "struct B { v: i32; }"
        "func main(): void {"
        "  var a: A = .A { .x = 1 };"
        "  var b: B = .B { .v = 2 };"
        "  b = a;"
        "}"));
}

TEST_F(SemaTest, StructEqLayoutCompatiblePasses) {
    /* 跨具名类型结构兼容判等 ==/!= */
    EXPECT_TRUE(analyze(
        "struct A { x: i32; y: i32; }"
        "struct B { x: i32; y: i32; }"
        "func main(): void {"
        "  var a: A = .A { .x = 1, .y = 2 };"
        "  var b: B = .B { .x = 1, .y = 2 };"
        "  if (a == b) { }"
        "  if (a != b) { }"
        "}"));
}

TEST_F(SemaTest, StructEqLayoutMismatchError) {
    /* 字段类型不同 → 判等报错 */
    EXPECT_FALSE(analyze(
        "struct A { x: i32; }"
        "struct B { x: i64; }"
        "func main(): void {"
        "  var a: A = .A { .x = 1 };"
        "  var b: B = .B { .x = 2 };"
        "  if (a == b) { }"
        "}"));
}

/* ---- 匿名构造 .{...} 创建匿名 struct 类型 ---- */

TEST_F(SemaTest, StructAnonConstructCreatesAnonTypePasses) {
    /* 匿名构造推断匿名类型，字段序乱（.y 在前）按名匹配重排到目标表序 */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func main(): void {"
        "  var p: Point = .{ .y = 2, .x = 1 };"
        "  var q: Point = .{ .x = 3, .y = 4 };"
        "}"));
}

TEST_F(SemaTest, StructAnonConstructMissingFieldError) {
    /* 匿名构造漏字段（按目标表校验）→ 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func main(): void {"
        "  var p: Point = .{ .x = 1 };"
        "}"));
}

TEST_F(SemaTest, StructAnonConstructUnknownFieldError) {
    /* 匿名构造给出不存在字段 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .{ .z = 1 };"
        "}"));
    expect_message(0, "has no field 'z'");
}

TEST_F(SemaTest, StructAnonConstructFieldTypeMismatchError) {
    /* 匿名构造字段类型不匹配（str → i32 字段）→ 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func main(): void {"
        "  var p: Point = .{ .x = \"s\" };"
        "}"));
}

TEST_F(SemaTest, StructAnonConstructOptionalFieldRequiresExplicitCtorError) {
    /* optional 字段裸 nil 无法推断类型 → 引导显式构造 .?T{nil} */
    EXPECT_FALSE(analyze(
        "struct Box { opt: ?i32; }"
        "func main(): void {"
        "  var b: Box = .{ .opt = nil };"
        "}"));
    expect_message(0, "use .?T{nil}");
}

TEST_F(SemaTest, StructAnonConstructOptionalFieldExplicitCtorPasses) {
    /* optional 字段显式构造 .?i32{nil}/.?i32{42}（用户确认的语义） */
    EXPECT_TRUE(analyze(
        "struct Box { opt: ?i32; name: str; }"
        "func main(): void {"
        "  var b: Box = .{ .opt = .?i32{nil}, .name = \"b\" };"
        "  var c: Box = .{ .opt = .?i32{42}, .name = \"c\" };"
        "}"));
}

TEST_F(SemaTest, StructAnonConstructNoContextError) {
    /* 无类型上下文（表达式位置）→ 诊断 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var p = .{ .x = 1 };"
        "}"));
}

TEST_F(SemaTest, StructAnonConstructOptionTargetError) {
    /* 目标为 ?T 的匿名构造 → 引导显式构造 */
    EXPECT_FALSE(analyze(
        "func main(): void {"
        "  var p: ?i32 = .{ 1 };"
        "}"));
    expect_message(0, "use .?T{...}");
}

TEST_F(SemaTest, StructAnonConstructInCallArgPasses) {
    /* 函数实参位置匿名构造：参数类型已知 → 推断匿名类型（乱序字段按名匹配） */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func sum(p: Point):i32 { return p.x + p.y; }"
        "func main(): void {"
        "  var s = sum(.{ .x = 1, .y = 2 });"
        "  var t = sum(.{ .y = 4, .x = 3 });"
        "}"));
}

TEST_F(SemaTest, StructAnonConstructInCallArgTypeMismatchError) {
    /* 实参匿名构造字段类型不匹配 → 诊断 */
    EXPECT_FALSE(analyze(
        "struct Point { x: i32; }"
        "func f(p: Point):i32 { return p.x; }"
        "func main(): void {"
        "  var s = f(.{ .x = \"s\" });"
        "}"));
}

TEST_F(SemaTest, StructAnonConstructInAssignRhsPasses) {
    /* 赋值 RHS 匿名构造：左值类型已知 → 推断匿名类型 */
    EXPECT_TRUE(analyze(
        "struct Point { x: i32; y: i32; }"
        "func main(): void {"
        "  var p: Point = .Point { .x = 0, .y = 0 };"
        "  p = .{ .x = 1, .y = 2 };"
        "  p = .{ .y = 4, .x = 3 };"
        "}"));
}

} /* namespace */
