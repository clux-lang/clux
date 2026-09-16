#include <gtest/gtest.h>
#include "test_common.h"

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/exec.h"
#include "vm/bcode.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "core/allocator.h"
#include "core/string.h"
#include "core/strslice.h"
}

/* ---- helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

/* 按类型宽度读取有符号整数 */
static int64_t read_sint(const value_t *v) {
    switch (value_type(v)->size) {
        case 1: return (int64_t)*(const int8_t  *)value_data(v);
        case 2: return (int64_t)*(const int16_t *)value_data(v);
        case 4: return (int64_t)*(const int32_t *)value_data(v);
        default: return *(const int64_t *)value_data(v);
    }
}

/* 按类型宽度读取无符号整数，零扩展到 uint64_t */
static uint64_t read_uint(const value_t *v) {
    switch (value_type(v)->size) {
        case 1: return (uint64_t)*(const uint8_t  *)value_data(v);
        case 2: return (uint64_t)*(const uint16_t *)value_data(v);
        case 4: return (uint64_t)*(const uint32_t *)value_data(v);
        default: return *(const uint64_t *)value_data(v);
    }
}

/* 按类型宽度读取 double */
static double read_float(const value_t *v) {
    if (value_type(v)->size == sizeof(float))
        return (double)*(const float *)value_data(v);
    return *(const double *)value_data(v);
}

class ExecTest : public ::testing::Test {
protected:
    allocator_t *alloc = nullptr;
    vm_t        *vm    = nullptr;
    bytecode_t  *bc    = nullptr;

    void SetUp() override {
        alloc = create_allocator(test_alloc, test_free);
        vm    = vm_new(alloc);
        bc    = bcode_new(alloc);
    }
    void TearDown() override {
        bcode_destroy(&bc);
        vm_destroy(&vm);
        EXPECT_EQ(vm, nullptr);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
    }

    /* 执行已写入的字节码，返回 exec_run 结果（error 或 NULL） */
    value_t *run() { return exec_run(vm, bc); }

    /* 栈顶值（借用引用，exec_run 后仍可读） */
    value_t *stack_top() { return exec_stack_peek(vm, 0); }

    /* 查变量（沿 scope 链），未找到返回 NULL */
    value_t *lookup(const char *name) {
        return scope_lookup(vm->current_scope, STRSLICE_LIT(name));
    }
};

/* ================================================================ */
/* 1. 字面量压栈 + HALT                                             */
/* ================================================================ */

TEST_F(ExecTest, PushI32ThenHalt) {
    bcode_write_op(bc, BCODE_PUSH_I32);
    bcode_write_i32(bc, 42);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 42);
}

TEST_F(ExecTest, PushAllIntWidths) {
    bcode_write_op(bc, BCODE_PUSH_I8);  bcode_write_i8(bc, -5);
    bcode_write_op(bc, BCODE_PUSH_I16); bcode_write_i16(bc, -1234);
    bcode_write_op(bc, BCODE_PUSH_I64); bcode_write_i64(bc, -9007199254740993LL);
    bcode_write_op(bc, BCODE_PUSH_U8);  bcode_write_u8(bc, 200);
    bcode_write_op(bc, BCODE_PUSH_U64); bcode_write_u64(bc, 0xFEDCBA9876543210ULL);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);

    /* 栈底 -> 栈顶：i8, i16, i64, u8, u64 */
    size_t sp = 5;
    EXPECT_EQ(read_sint(exec_stack_peek(vm, sp - 1 - 0)), -5);
    EXPECT_EQ(read_sint(exec_stack_peek(vm, sp - 1 - 1)), -1234);
    EXPECT_EQ(read_sint(exec_stack_peek(vm, sp - 1 - 2)), -9007199254740993LL);
    EXPECT_EQ(read_uint(exec_stack_peek(vm, sp - 1 - 3)), 200u);
    EXPECT_EQ(read_uint(exec_stack_peek(vm, sp - 1 - 4)), 0xFEDCBA9876543210ULL);
}

TEST_F(ExecTest, PushFloatAndBoolAndStr) {
    bcode_write_op(bc, BCODE_PUSH_F64); bcode_write_f64(bc, 3.14159);
    bcode_write_op(bc, BCODE_PUSH_BOOL); bcode_write_bool(bc, true);
    bcode_write_op(bc, BCODE_PUSH_STR); bcode_write_str(bc, STRSLICE_LIT("hi"));
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);

    value_t *f = exec_stack_peek(vm, 2);
    value_t *b = exec_stack_peek(vm, 1);
    value_t *s = exec_stack_peek(vm, 0);
    EXPECT_EQ(value_type(f), vm->type_f64);
    EXPECT_EQ(read_float(f), 3.14159);
    EXPECT_EQ(value_type(b), vm->type_bool);
    EXPECT_TRUE(*(const bool *)value_data(b));
    EXPECT_EQ(value_type(s), vm->type_str);
    const string_t *str = *(const string_t *const *)value_data(s);
    EXPECT_EQ(string_len(str), 2u);
    EXPECT_EQ(strncmp(string_cstr(str), "hi", 2), 0);
}

/* ================================================================ */
/* 2. 变量定义与访问（DEFINE / PUSH / STORE）                        */
/* ================================================================ */

/* var a:i32 = 42; => PUSH_I32 42; LOAD "i32"; DEFINE "a" */
TEST_F(ExecTest, DefineTypedVar) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 42);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *a = lookup("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(value_type(a), vm->type_i32);
    EXPECT_EQ(read_sint(a), 42);
}

/* var a = 42; => PUSH_I32 42; PUSH_UNDEFINED; DEFINE "a"（类型从值推断） */
TEST_F(ExecTest, DefineInferredVar) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 7);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *a = lookup("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(value_type(a), vm->type_i32);
    EXPECT_EQ(read_sint(a), 7);
}

/* var a:i32; 无初始值 → 零值占位（TDZ 检查由 sema 编译期完成，VM 值层不感知） */
TEST_F(ExecTest, DefineUninitVarReadsZero) {
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    /* 读取 a → 零值占位，不报 error */
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *a = lookup("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(value_type(a), vm->type_i32);
    EXPECT_EQ(read_sint(a), 0);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(read_sint(top), 0);
}

/* var a:i32; a = 5; 赋值后正常读取 */
TEST_F(ExecTest, DefineUninitVarThenAssignReadsValue) {
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));

    /* a = 5: PUSH_I32 5; STORE "a"（STORE 只弹一个值，dst 按名查） */
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 5);
    bcode_write_op(bc, BCODE_STORE); bcode_write_str(bc, STRSLICE_LIT("a"));

    /* 读取 a */
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *a = lookup("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(read_sint(a), 5);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(read_sint(top), 5);
}

/* 重复定义同一变量 → 报 error */
TEST_F(ExecTest, RedefineVarReturnsError) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 2);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 未定义变量读取 → 报 error */
TEST_F(ExecTest, UndefinedVariablePushReturnsError) {
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("nope"));
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ================================================================ */
/* 3. 运算                                                           */
/* ================================================================ */

TEST_F(ExecTest, AddTwoI32) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 20);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 22);
    bcode_write_op(bc, BCODE_ADD);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 42);
}

TEST_F(ExecTest, MixedWidthSubPromotes) {
    /* i8 - i32 → promote 到 i32 */
    bcode_write_op(bc, BCODE_PUSH_I8);  bcode_write_i8(bc, 10);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 3);
    bcode_write_op(bc, BCODE_SUB);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 7);
}

TEST_F(ExecTest, CompareGtBool) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 5);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 3);
    bcode_write_op(bc, BCODE_GT);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_bool);
    EXPECT_TRUE(*(const bool *)value_data(top));
}

/* 类型不支持运算 → 报 error（如 str + i32） */
TEST_F(ExecTest, UnsupportedOpReturnsError) {
    bcode_write_op(bc, BCODE_PUSH_STR); bcode_write_str(bc, STRSLICE_LIT("x"));
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_ADD);
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ================================================================ */
/* 4. 控制流（JMP / JZ / JNZ）                                       */
/* ================================================================ */

TEST_F(ExecTest, JmpSkipsMiddle) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 111);
    bcode_write_op(bc, BCODE_POP);

    /* jmp over PUSH_I32 222 */
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0); /* placeholder，稍后回填 */

    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 222);
    bcode_write_op(bc, BCODE_POP);

    /* 回填 JMP 目标：跳到 333 压栈处（dest 须在写指令前取） */
    size_t dest = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 333);
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)dest);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(read_sint(top), 333);
}

TEST_F(ExecTest, JzJumpsWhenFalse) {
    /* 1 > 3 → false → JZ 跳过 111，落到 333 */
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 3);
    bcode_write_op(bc, BCODE_GT);

    size_t placeholder = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JZ);
    bcode_write_u32(bc, 0);

    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 111);
    bcode_write_op(bc, BCODE_POP);

    /* dest = 333 PUSH 起点（跳过 111） */
    size_t dest = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 333);
    bcode_write_op(bc, BCODE_HALT);

    bcode_patch_u32(bc, placeholder + 4, (uint32_t)dest);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(read_sint(top), 333);
}

TEST_F(ExecTest, JnzDoesNotJumpWhenFalse) {
    /* 1 > 3 → false → JNZ 不跳，继续顺序执行（111 被 POP 丢弃，栈顶 333） */
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 3);
    bcode_write_op(bc, BCODE_GT);

    size_t placeholder = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JNZ);
    bcode_write_u32(bc, 0);

    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 111);
    bcode_write_op(bc, BCODE_POP);

    /* dest = 333 PUSH 起点（条件为真才跳，这里应为 false 不跳） */
    size_t dest = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 333);
    bcode_write_op(bc, BCODE_HALT);

    bcode_patch_u32(bc, placeholder + 4, (uint32_t)dest);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(read_sint(top), 333);
}

/* TDZ 值作为跳转条件 → 报 error */
/* var a:i32; 读取零值占位，JZ 正常跳转（TDZ 检查已下沉 sema，无运行时 error） */
TEST_F(ExecTest, JzOnUninitVarReadsZeroAndJumps) {
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("a"));

    size_t placeholder = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JZ);
    bcode_write_u32(bc, 0);
    size_t dest = bcode_tell(bc);
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, placeholder + 4, (uint32_t)dest); /* 零值 cast false → 跳转 */

    EXPECT_EQ(run(), nullptr); /* 无 error */
}

/* 非 bool 值作为跳转条件（str → bool 不可转换）→ 报 error（严格 bool） */
TEST_F(ExecTest, JzOnNonBoolReturnsError) {
    bcode_write_op(bc, BCODE_PUSH_STR); bcode_write_str(bc, STRSLICE_LIT("x"));

    size_t placeholder = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JZ);
    bcode_write_u32(bc, 0);
    size_t dest = bcode_tell(bc);
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, placeholder + 4, (uint32_t)dest);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ================================================================ */
/* 5. 块作用域（PUSH_SCOPE / POP_SCOPE）                             */
/* ================================================================ */

TEST_F(ExecTest, BlockScopeVarHiddenAfterPop) {
    /* 根作用域定义 a=1 */
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));

    /* 块内重新定义 a=2（同层遮罩） */
    bcode_write_op(bc, BCODE_PUSH_SCOPE);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 2);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_POP_SCOPE);

    /* 块外 a 仍是 1 */
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *a = lookup("a");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(read_sint(a), 1);
}

/* POP_SCOPE 回收块内临时值：块内定义变量块外不可见 */
TEST_F(ExecTest, BlockScopeTempDestroyedOnPop) {
    bcode_write_op(bc, BCODE_PUSH_SCOPE);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 9);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("inner"));
    bcode_write_op(bc, BCODE_POP_SCOPE);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    EXPECT_EQ(lookup("inner"), nullptr);
}

/* ================================================================ */
/* 6. 栈操作（POP / PUSH_VALUE）                                     */
/* ================================================================ */

TEST_F(ExecTest, PopDiscardsTop) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 2);
    bcode_write_op(bc, BCODE_POP);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(read_sint(top), 1);
}

TEST_F(ExecTest, PushValueDupsDeep) {
    /* 栈：10, 20；PUSH_VALUE offset=1 → 压入 10 的借用引用 → 栈：10, 20, 10 */
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 10);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 20);
    bcode_write_op(bc, BCODE_PUSH_VALUE); bcode_write_u32(bc, 1);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    EXPECT_EQ(read_sint(exec_stack_peek(vm, 0)), 10);
    EXPECT_EQ(read_sint(exec_stack_peek(vm, 1)), 20);
    EXPECT_EQ(read_sint(exec_stack_peek(vm, 2)), 10);
}

/* ================================================================ */
/* 7. LOAD 基本类型（type value）                                    */
/* ================================================================ */

TEST_F(ExecTest, LoadBuiltinType) {
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("f64"));
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_type);
    EXPECT_EQ(value_as(top, const type_t *), vm->type_f64);
}

TEST_F(ExecTest, LoadUnknownTypeReturnsError) {
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("nope"));
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ================================================================ */
/* 8. error 短路与恢复                                               */
/* ================================================================ */

TEST_F(ExecTest, ErrorStopsExecution) {
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("nope"));
    /* 之后的指令不应执行 */
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 999);
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* error 出现在运算参数中 → 短路返回 error，不 panic */
TEST_F(ExecTest, ErrorPropagatesThroughBinary) {
    /* undefined 参与 ADD：void vtable 无 add 槽 → 运算产生 error */
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 2);
    bcode_write_op(bc, BCODE_ADD);
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ================================================================ */
/* 5. 函数（PUSH_FUNC_TYPE / FUNC_TYPE_* / PUSH_FUNCTION / define 统一绑定 / */
/*    CALL / RET）                                                  */
/* ================================================================ */

/* exec_run 注册函数后按名查模块作用域并调用（clux 无顶层语句，
   入口函数由调用方显式触发）；未找到返回 NULL 由断言捕获 */
static value_t *call_main(vm_t *vm, bytecode_t *bc) {
    exec_run(vm, bc);
    value_t *fn = scope_lookup(vm->current_scope, STRSLICE_LIT("main"));
    if (!fn) return NULL;
    return value_call(vm, fn, NULL, 0);
}

/* main() -> i32 { return 1 + 2; } → 3 */
TEST_F(ExecTest, FunctionRegisterAndCallMain) {
    /* JMP 守卫：跳过函数体直达注册段 */
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    /* 函数体：1 + 2 → RET */
    size_t body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 2);
    bcode_write_op(bc, BCODE_ADD);
    bcode_write_op(bc, BCODE_RET);

    /* 注册段：签名类型声明-定义两步构造（PUSH_FUNC_TYPE → DEFINE_TYPE
       <sig_id> 声明登记 → LOAD_TYPE 拉回定义 → FUNC_TYPE_RETURN 设返回 →
       SEAL 封闭）→ LOAD_TYPE <sig_id> 主动拉取 → PUSH_FUNCTION →
       BIND_FUNC → SET_FUNC_NAME → push_undefined + DEFINE。
       注：编译器 hoist 提升区把该序列整体前移为两遍扫描（pass 1 所有签名
       PUSH_FUNC_TYPE → DEFINE_TYPE 声明、pass 2 逐个 LOAD_TYPE → SEAL 定义），
       注册段只留 LOAD_TYPE；此处单类型内联两步构造等价于 hoist 区单个签名的
       完整构造序列，验证 VM 类型指令语义。 */
    size_t end = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL); /* 无操作数：封闭算布局（sig_id 64 已声明登记） */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    value_t *r = call_main(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 3);
}

/* void 函数返回 undefined（PUSH_UNDEFINED + RET） */
TEST_F(ExecTest, VoidFunctionReturnsUndefined) {
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    size_t body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_RET);

    size_t end = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64); /* 签名类型 id（types_by_id），独立于函数 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("void"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    value_t *r = call_main(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_TRUE(value_is_undefined(vm, r));
}

/* 显式转换：目标类型经栈顶 type value（LOAD 压入），弹 type + 值 */
TEST_F(ExecTest, CastExplicitUsesTypeValueOnStack) {
    /* i64 9999999999 → CAST(i32) → 截断为低 32 位 1410065407 */
    bcode_write_op(bc, BCODE_PUSH_I64); bcode_write_i64(bc, 9999999999LL);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_CAST);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 1410065407);
}

/* 实参多于形参（foo(a:i32) 传 2 个）→ error 传播 */
TEST_F(ExecTest, CallArgCountMismatchPropagatesError) {
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    /* foo 函数体：绑定参数 a（push_undefined + define，从值推断）→ return a */
    size_t foo_body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("a"));
    bcode_write_op(bc, BCODE_RET);

    /* main 函数体：foo(1, 2) → CALL 2 */
    size_t main_body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("foo"));
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 2);
    bcode_write_op(bc, BCODE_CALL); bcode_write_u32(bc, 2);
    bcode_write_op(bc, BCODE_RET);

    /* 注册段 */
    size_t end = bcode_tell(bc);
    /* foo: (i32) -> i32 */
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64); /* foo 签名类型 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_PARAM);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)foo_body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("foo"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("foo"));
    /* main: () -> i32 */
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 65); /* main 签名类型 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 65);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 65);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)main_body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 65);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    value_t *r = call_main(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 递归：fact(5) = 120（函数体可看到自身符号，参数倒序 DEFINE 绑定） */
TEST_F(ExecTest, RecursiveFactorial) {
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    /* fact 函数体：n <= 1 ? 1 : n * fact(n - 1) */
    size_t fact_body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("n"));
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("n"));
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_LE);
    size_t jz = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JZ);
    bcode_write_u32(bc, 0);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_RET);
    size_t recur = bcode_tell(bc);
    bcode_patch_u32(bc, jz + 4, (uint32_t)recur);
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("n"));
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("fact"));
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("n"));
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_SUB);
    bcode_write_op(bc, BCODE_CALL); bcode_write_u32(bc, 1);
    bcode_write_op(bc, BCODE_MUL);
    bcode_write_op(bc, BCODE_RET);

    /* main 函数体：fact(5) */
    size_t main_body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("fact"));
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 5);
    bcode_write_op(bc, BCODE_CALL); bcode_write_u32(bc, 1);
    bcode_write_op(bc, BCODE_RET);

    /* 注册段 */
    size_t end = bcode_tell(bc);
    /* fact: (i32) -> i32 */
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64); /* fact 签名类型 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_PARAM);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)fact_body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("fact"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("fact"));
    /* main: () -> i32 */
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 65); /* main 签名类型 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 65);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 65);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)main_body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 65);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    value_t *r = call_main(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 120);
}

/* 函数体可查看到模块变量（closure_scope 临时接线 root_scope）：
   x = 5；main() -> i32 { return x + 1; } → 6 */
TEST_F(ExecTest, FunctionBodySeesModuleVariables) {
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    size_t main_body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH); bcode_write_str(bc, STRSLICE_LIT("x"));
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_ADD);
    bcode_write_op(bc, BCODE_RET);

    size_t end = bcode_tell(bc);
    /* 模块级变量 x = 5（注册段 DEFINE 到 root_scope） */
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 5);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("x"));
    /* main: () -> i32 */
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64); /* 签名类型 id（types_by_id），独立于函数 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)main_body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("main"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    value_t *r = call_main(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 6);
}

/* 匿名函数表达式：var add = func():i32{ return 1; } → 与命名函数同走统一
   push_undefined + define（从值推断），函数值 auto-track 归 scope 统一回收（无泄漏） */
TEST_F(ExecTest, FunctionExpressionDefinedViaPlainDefine) {
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    size_t body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_RET);

    size_t end = bcode_tell(bc);
    /* 函数表达式：构造签名 func():i32（PUSH_FUNC_TYPE → DEFINE_TYPE <64>
       声明登记 → LOAD_TYPE 拉回定义 → RETURN → SEAL 封闭）→ LOAD_TYPE 拉取
       → PUSH_FUNCTION → push_undefined → DEFINE "add"
       匿名函数表达式不写 SET_FUNC_NAME（name 留空），也不强制 BIND_FUNC */
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64); /* 签名类型 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)body);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("add"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    /* 注册段执行完，add 应已在作用域内，且可直接调用 */
    EXPECT_EQ(run(), nullptr);
    value_t *fn = scope_lookup(vm->current_scope, STRSLICE_LIT("add"));
    ASSERT_NE(fn, nullptr);
    EXPECT_FALSE(value_is_error(vm, fn));

    value_t *r = value_call(vm, fn, NULL, 0);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 1);
}

/* ================================================================ */
/* 9. 类型 id 指令（LOAD_TYPE / SEAL / SET_TYPE_NAME）              */
/* ================================================================ */

/* 类型 id 表：程序类型 id 从 TYPE_ID_PROGRAM_BASE(=64) 起，内建类型
   0..16。以下测试直接写字节码驱动类型指令（与 compiler hoist
   提升区 / 槽位 LOAD_TYPE 的运行时语义一致）。DEFINE_TYPE <id> 弹栈
   密封+登记，LOAD_TYPE <id> 主动拉取。 */

/* LOAD_TYPE 内建 id：id=2 是 i32（vm_init_builtins 固定序）。hoist_builtin
   即发 LOAD_TYPE <内建 id>，此路径验证内建 id 段可直接查表。 */
TEST_F(ExecTest, LoadTypeBuiltinId) {
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 2);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_type);
    EXPECT_EQ(value_as(top, const type_t *), vm->type_i32);
}

/* DEFINE_TYPE <64> 声明登记程序类型 → 定义封闭 → LOAD_TYPE 64 查回同一实例。
   构造序列与 hoist 区一致：PUSH_ARRAY → DEFINE_TYPE <id> 声明 → LOAD_TYPE
   拉回 → LOAD elem → DEFINE_BOUND → SEAL（无操作数，封闭+按自身 id 重绑）。 */
TEST_F(ExecTest, BindThenLoadProgramType) {
    /* 构造 [i32;3]（同 hoist_array 序列） */
    bcode_write_op(bc, BCODE_PUSH_ARRAY);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE_BOUND); bcode_write_u32(bc, 3);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_type);

    const type_t *t = value_as(top, const type_t *);
    EXPECT_EQ(t->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_elem(t), vm->type_i32);
    EXPECT_EQ(array_type_len(t), (size_t)3);
}

/* 重复登记幂等：同一密封实例绑 64/65 两个 id，LOAD_TYPE 两者都返回
   同一 type_t*（seal 去重 intern 后同一实例多 id 别名）。 */
TEST_F(ExecTest, BindTypeIdempotentAlias) {
    bcode_write_op(bc, BCODE_PUSH_ARRAY);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE_BOUND); bcode_write_u32(bc, 3);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64); /* 复制引用 */
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 65); /* 弹顶（同一实例）别名绑 65 */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 65);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    const type_t *t64 = value_as(exec_stack_peek(vm, 1), const type_t *);
    const type_t *t65 = value_as(exec_stack_peek(vm, 0), const type_t *);
    ASSERT_NE(t64, nullptr);
    ASSERT_NE(t65, nullptr);
    EXPECT_EQ(t64, t65); /* 同一 intern 实例 */
    EXPECT_TRUE(type_is_sealed(t65));
}

/* LOAD_TYPE 未登记 id → 硬错误（types_by_id 查表 miss） */
TEST_F(ExecTest, LoadTypeUnknownIdReturnsError) {
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 100);
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* SEAL 栈顶非 type value → 硬错误（SEAL 无操作数，只弹栈检查类型） */
TEST_F(ExecTest, SealNonTypeValueReturnsError) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 42);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* SET_TYPE_NAME：程序类型（id >= TYPE_ID_PROGRAM_BASE）可覆盖显示名。
   绑定后改名，type 的 name 字段应更新为新名。 */
TEST_F(ExecTest, SetTypeNameOnProgramType) {
    bcode_write_op(bc, BCODE_PUSH_ARRAY);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_DEFINE_BOUND); bcode_write_u32(bc, 2);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_SET_TYPE_NAME); bcode_write_str(bc, STRSLICE_LIT("Row"));
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_HALT);

    EXPECT_EQ(run(), nullptr);
    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    const type_t *t = value_as(top, const type_t *);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->kind, TYPE_KIND_ARRAY);
    ASSERT_NE(t->name.ptr, nullptr);
    EXPECT_EQ(t->name.len, strlen("Row"));
    EXPECT_EQ(strncmp(t->name.ptr, "Row", 3), 0);
}

/* SET_TYPE_NAME 内建类型（id < TYPE_ID_PROGRAM_BASE）不可改名 → 硬错误 */
TEST_F(ExecTest, SetTypeNameOnBuiltinRejected) {
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 2); /* i32 */
    bcode_write_op(bc, BCODE_SET_TYPE_NAME); bcode_write_str(bc, STRSLICE_LIT("X"));
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* SET_TYPE_NAME 栈顶非 type value → 硬错误 */
TEST_F(ExecTest, SetTypeNameNonTypeValueReturnsError) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_SET_TYPE_NAME); bcode_write_str(bc, STRSLICE_LIT("X"));
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ================================================================ */
/* 9b. 函数 id 指令（PUSH_FUNCTION / BIND_FUNC 填 id / SET_FUNC_NAME） */
/* ================================================================ */

/* 函数 id 表：程序函数 id 从 FUNC_ID_PROGRAM_BASE(=64) 起（compiler 分配），
   内建函数（printf）固定 id 0（func_new 自动分配）。以下测试直接写字节码
   驱动三条函数指令（与 compiler 注册段的运行时语义一致）。 */

/* PUSH_FUNCTION <body> 构造（id 默认 0）→ BIND_FUNC <id> 填充 fn->id 并
   登记 → 查表取回同一实例。id 单一来源：只在 BIND_FUNC 出现。 */
TEST_F(ExecTest, PushFunctionSetsIdAndBindFuncRegisters) {
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    size_t body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 7);
    bcode_write_op(bc, BCODE_RET);

    size_t end = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64); /* 签名类型 id（types_by_id 类型表） */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("i32"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("f"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    EXPECT_EQ(run(), nullptr);
    func_t *fn = vm_func_load(vm, 64); /* 函数 id 64（functions_by_id 函数表） */
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn->id, 64u);

    /* 调用按名路径不受 id 影响 */
    value_t *fv = scope_lookup(vm->current_scope, STRSLICE_LIT("f"));
    ASSERT_NE(fv, nullptr);
    value_t *r = value_call(vm, fv, NULL, 0);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_EQ(read_sint(r), 7);
}

/* SET_FUNC_NAME：命名函数定义写入显示名，fn->name 应更新为新名 */
TEST_F(ExecTest, SetFuncNameOnProgramFunction) {
    size_t jmp_pc = bcode_tell(bc);
    bcode_write_op(bc, BCODE_JMP);
    bcode_write_u32(bc, 0);

    size_t body = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_RET);

    size_t end = bcode_tell(bc);
    bcode_write_op(bc, BCODE_PUSH_FUNC_TYPE);
    bcode_write_op(bc, BCODE_DEFINE_TYPE); bcode_write_u32(bc, 64); /* 签名类型 id（types_by_id），独立于函数 id */
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_LOAD); bcode_write_str(bc, STRSLICE_LIT("void"));
    bcode_write_op(bc, BCODE_FUNC_TYPE_RETURN);
    bcode_write_op(bc, BCODE_SEAL);
    bcode_write_op(bc, BCODE_LOAD_TYPE); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_PUSH_FUNCTION); bcode_write_u32(bc, (uint32_t)body);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("greet"));
    bcode_write_op(bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(bc, BCODE_DEFINE); bcode_write_str(bc, STRSLICE_LIT("greet"));
    bcode_write_op(bc, BCODE_HALT);
    bcode_patch_u32(bc, jmp_pc + 4, (uint32_t)end);

    EXPECT_EQ(run(), nullptr);
    func_t *fn = vm_func_load(vm, 64);
    ASSERT_NE(fn, nullptr);
    ASSERT_NE(fn->name.ptr, nullptr);
    EXPECT_EQ(fn->name.len, strlen("greet"));
    EXPECT_EQ(strncmp(fn->name.ptr, "greet", 5), 0);
}

/* BIND_FUNC 未登记 id → vm_func_load 返回 NULL（表 miss） */
TEST_F(ExecTest, BindFuncUnknownIdReturnsNull) {
    bcode_write_op(bc, BCODE_HALT);
    EXPECT_EQ(run(), nullptr);
    EXPECT_EQ(vm_func_load(vm, 64), nullptr);
}

/* BIND_FUNC 栈顶非 func value → 硬错误 */
TEST_F(ExecTest, BindFuncNonFuncValueReturnsError) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 42);
    bcode_write_op(bc, BCODE_BIND_FUNC); bcode_write_u32(bc, 64);
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* SET_FUNC_NAME 栈顶非 func value → 硬错误 */
TEST_F(ExecTest, SetFuncNameNonFuncValueReturnsError) {
    bcode_write_op(bc, BCODE_PUSH_I32); bcode_write_i32(bc, 1);
    bcode_write_op(bc, BCODE_SET_FUNC_NAME); bcode_write_str(bc, STRSLICE_LIT("X"));
    bcode_write_op(bc, BCODE_HALT);

    value_t *r = run();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}
