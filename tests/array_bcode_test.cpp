#include <gtest/gtest.h>
#include <cstring>
#include "test_common.h"

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/exec.h"
#include "vm/bcode.h"
#include "vm/bcode_asm.h"
#include "vm/bcode_disasm.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "core/allocator.h"
#include "core/string.h"
#include "core/strslice.h"
}

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

class ArrayBcodeTest : public ::testing::Test {
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

    /* 汇编 .cxs 源并 exec_run（teardown 的 allocator 空断言会捕获去重泄漏） */
    bool assemble_and_run(const char *src) {
        bcode_destroy(&bc);
        if (bcode_asm_parse(alloc, src, strlen(src), &bc) != 0) return false;
        value_t *r = exec_run(vm, bc);
        return r == NULL || !value_is_error(vm, r);
    }

    value_t *stack_top() { return exec_stack_peek(vm, 0); }
};

/* 按类型宽度读取有符号整数（测试辅助） */
static int64_t read_sint(const value_t *v) {
    switch (value_type(v)->size) {
        case 1: return (int64_t)*(const int8_t  *)value_data(v);
        case 2: return (int64_t)*(const int16_t *)value_data(v);
        case 4: return (int64_t)*(const int32_t *)value_data(v);
        default: return *(const int64_t *)value_data(v);
    }
}

/* ================================================================ */
/* 值构造 / 下标访问（construct / set_item / get_item 端到端）         */
/* ================================================================ */

/* construct 收尾：push_array...seal 留下 [i32;3] 类型位，压 3 个元素后
   construct 3 弹出成员值 + 类型位，构造出数组值。 */
TEST_F(ArrayBcodeTest, ConstructArrayValueFromElements) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top)->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_elem(value_type(top)), vm->type_i32);
    EXPECT_EQ(array_type_len(value_type(top)), (size_t)3);
}

/* get_item（INDEX_GET）：construct 后 self[index] 弹出并返回元素副本 */
TEST_F(ArrayBcodeTest, ConstructThenIndexGet) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    push_i32 1\n"
        "    index_get\n"
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 20);
}

/* set_item（INDEX_SET）：self[index] = val 返回 self，随后可再 index_get 验证 */
TEST_F(ArrayBcodeTest, ConstructThenIndexSetThenGet) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    push_i32 2\n"
        "    push_i32 99\n"
        "    index_set\n"
        "    push_i32 2\n"
        "    index_get\n"
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 99);
}

/* 越界读取（INDEX_GET）：index >= len 触发运行时硬错误并停机 */
TEST_F(ArrayBcodeTest, IndexGetOutOfBoundsReturnsError) {
    const char *src =
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    push_i32 3\n"      /* 索引越界（len=3，合法 0..2） */
        "    index_get\n"
        "    halt\n";
    bcode_destroy(&bc);
    ASSERT_EQ(bcode_asm_parse(alloc, src, strlen(src), &bc), 0);
    value_t *r = exec_run(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 越界写入（INDEX_SET）：index >= len 触发运行时硬错误并停机 */
TEST_F(ArrayBcodeTest, IndexSetOutOfBoundsReturnsError) {
    const char *src =
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    push_i32 5\n"      /* 越界索引 */
        "    push_i32 99\n"
        "    index_set\n"
        "    halt\n";
    bcode_destroy(&bc);
    ASSERT_EQ(bcode_asm_parse(alloc, src, strlen(src), &bc), 0);
    value_t *r = exec_run(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 负索引同样视为越界（按无符号读入后 >= len，触发硬错误） */
TEST_F(ArrayBcodeTest, NegativeIndexIsOutOfBounds) {
    const char *src =
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    push_i32 -1\n"     /* 负索引 */
        "    index_get\n"
        "    halt\n";
    bcode_destroy(&bc);
    ASSERT_EQ(bcode_asm_parse(alloc, src, strlen(src), &bc), 0);
    value_t *r = exec_run(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 长度查询（LENGTH）：构造 [i32;3] 后 len → u64 元素个数 3 */
TEST_F(ArrayBcodeTest, LengthReturnsElementCount) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    length\n"
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_u64);
    EXPECT_EQ(read_sint(top), 3);
}

/* 长度查询不支持的类型返回硬错误（scalar 无 length 回调） */
TEST_F(ArrayBcodeTest, LengthOnScalarReturnsError) {
    const char *src =
        "    push_i32 42\n"
        "    length\n"
        "    halt\n";
    bcode_destroy(&bc);
    ASSERT_EQ(bcode_asm_parse(alloc, src, strlen(src), &bc), 0);
    value_t *r = exec_run(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 定长数组成员数不匹配立即报错（construct 校验 len） */
TEST_F(ArrayBcodeTest, ConstructCountMismatchReturnsError) {
    const char *src =
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    construct 2\n"
        "    halt\n";
    /* 汇编成功（语法合法），运行时 construct 报错 */
    bcode_destroy(&bc);
    ASSERT_EQ(bcode_asm_parse(alloc, src, strlen(src), &bc), 0);
    value_t *r = exec_run(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 非 array 类型走 construct 目前报错（struct/tuple 待后续） */
TEST_F(ArrayBcodeTest, ConstructNonArrayTypeReturnsError) {
    const char *src =
        "    load \"i32\"\n"
        "    push_i32 1\n"
        "    construct 1\n"
        "    halt\n";
    bcode_destroy(&bc);
    ASSERT_EQ(bcode_asm_parse(alloc, src, strlen(src), &bc), 0);
    value_t *r = exec_run(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 两次 construct 构造两个数组值：value_make_array 不得残留 type value 到
 * 操作数栈（回归：type_array_intern 曾经 array_type_push 压栈，导致第二个
 * construct 后栈顶被残留 type value 占据，此处断言栈顶为第二个数组值）。 */
TEST_F(ArrayBcodeTest, ConstructTwiceLeavesNoTypeValueOnStack) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 1\n"
        "    push_i32 2\n"
        "    construct 2\n"     /* [arr1] */
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 3\n"
        "    push_i32 4\n"
        "    construct 2\n"     /* [arr1, arr2] */
        "    halt\n"));

    value_t *t2 = stack_top();
    value_t *t1 = exec_stack_peek(vm, 1);
    ASSERT_NE(t2, nullptr);
    ASSERT_NE(t1, nullptr);
    /* 栈顶必须是第二个数组值，而非 value_make_array 残留的 type value */
    EXPECT_EQ(value_type(t2)->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(value_type(t2)), (size_t)2);
    EXPECT_EQ(value_type(t1)->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(value_type(t1)), (size_t)2);
}

/* 嵌套数组构造（[2][2]i32）：内层 construct 完成后栈上仅剩外层类型位与
 * 内层数组值，无残留 type value 干扰外层 construct 弹类型位。
 * 序列与 compiler 生成一致：外层类型先成形（define_bound 弹 t_inner 设给
 * open_outer），随后每次内层构造前重新 push_array...seal（去重 intern 复用
 * t_inner，value_seal 重定向栈上引用）。 */
TEST_F(ArrayBcodeTest, ConstructNestedArray) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"         /* [open_outer] */
        "    define_type 65\n"     /* 声明：open_outer 绑 id 65 + 登记 */
        "    load_type 65\n"       /* 拉回 open_outer（定义起点） */
        "    push_array\n"         /* [open_outer, open_inner] */
        "    define_type 64\n"     /* 声明：open_inner 绑 id 64 + 登记 */
        "    load_type 64\n"       /* 拉回 open_inner */
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"               /* 弹 t_inner=[i32;2]，密封（按自身 id 64 重绑登记） */
        "    load_type 64\n"       /* [open_outer, t_inner] */
        "    define_bound 2\n"     /* 弹 t_inner 设为 open_outer 元素类型 */
        "    seal\n"               /* 弹 t_outer=[[i32;2];2]，密封（按自身 id 65 重绑登记） */
        "    load_type 65\n"       /* [t_outer] */
        /* 内层 1：重新压类型位（去重 → t_inner，id 64 幂等） */
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"       /* [t_outer, t_inner] */
        "    push_i32 1\n"
        "    push_i32 2\n"
        "    construct 2\n"        /* [t_outer, arr_inner1] */
        /* 内层 2 */
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"       /* [t_outer, arr_inner1, t_inner2] */
        "    push_i32 3\n"
        "    push_i32 4\n"
        "    construct 2\n"        /* [t_outer, arr_inner1, arr_inner2] */
        "    construct 2\n"        /* [arr_outer] */
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top)->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(value_type(top)), (size_t)2);
    /* 外层元素类型是 [i32;2] */
    const type_t *et = array_type_elem(value_type(top));
    ASSERT_NE(et, nullptr);
    EXPECT_EQ(et->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_len(et), (size_t)2);
    EXPECT_EQ(array_type_elem(et), vm->type_i32);
}

/* 复合赋值语义：PUSH_VALUE dup 保留 self/index 引用 → INDEX_GET → op →
 * INDEX_SET。栈序 [self, index, old, v] → op → [self, index, new] → SET。
 * 验证复合写回真实生效（对应 compiler compile_assign_index 的展开）。 */
TEST_F(ArrayBcodeTest, IndexCompoundViaPushValueDup) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"         /* [arr] */
        "    push_i32 2\n"          /* [arr, 2] */
        "    push_value 1\n"        /* dup self → [arr, 2, arr] */
        "    push_value 1\n"        /* dup index → [arr, 2, arr, 2] */
        "    index_get\n"           /* [arr, 2, old=30] */
        "    push_i32 1\n"          /* [arr, 2, 30, 1] */
        "    add\n"                 /* [arr, 2, 31] */
        "    index_set\n"           /* 弹 val,index,self → [arr] */
        "    push_value 0\n"        /* dup arr */
        "    push_i32 2\n"
        "    index_get\n"           /* [arr, 31] */
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 31);
}

/* 越界下标读取（INDEX_GET）：index >= len 触发运行时硬错误并停机 */
TEST_F(ArrayBcodeTest, IndexGetOutOfBoundsViaExpr) {
    const char *src =
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 7\n"
        "    push_i32 8\n"
        "    construct 2\n"
        "    push_i32 5\n"          /* 越界索引（len=2） */
        "    index_get\n"
        "    halt\n";
    bcode_destroy(&bc);
    ASSERT_EQ(bcode_asm_parse(alloc, src, strlen(src), &bc), 0);
    value_t *r = exec_run(vm, bc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* 多维 SET（借用引用）：m[1] 经 INDEX_GET 返回借用值（data 指向父数组槽位），
 * INDEX_SET 写回借用 → 直达原数组。序列：
 *   [m] → 1 → INDEX_GET → [m[1]借用] → 0 → 7 → INDEX_SET → [m[1]借用]
 *   → 0 → INDEX_GET → [7]（元素副本）——同一数组内写后读验证写回生效 */
TEST_F(ArrayBcodeTest, MultidimIndexSetViaBorrowedRef) {
    ASSERT_TRUE(assemble_and_run(
        /* 构造外层 [2][2]i32（同 ConstructNestedArray 序列） */
        "    push_array\n"         /* [open_outer] */
        "    define_type 65\n"     /* 声明：open_outer 绑 id 65 + 登记 */
        "    load_type 65\n"       /* 拉回 open_outer（定义起点） */
        "    push_array\n"         /* [open_outer, open_inner] */
        "    define_type 64\n"     /* 声明：open_inner 绑 id 64 + 登记 */
        "    load_type 64\n"       /* 拉回 open_inner */
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"               /* 弹 t_inner，密封（按自身 id 64 重绑登记） */
        "    load_type 64\n"       /* [open_outer, t_inner] */
        "    define_bound 2\n"     /* 弹 t_inner 设外层元素类型 */
        "    seal\n"               /* 弹 t_outer，密封（按自身 id 65 重绑登记） */
        "    load_type 65\n"       /* [t_outer] */
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"       /* [t_outer, t_inner] */
        "    push_i32 1\n"
        "    push_i32 2\n"
        "    construct 2\n"        /* 内层行0 */
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"       /* [t_outer, row0, t_inner] */
        "    push_i32 3\n"
        "    push_i32 4\n"
        "    construct 2\n"        /* 内层行1 */
        "    construct 2\n"        /* 外层 m */
        "    push_i32 1\n"
        "    index_get\n"           /* [m[1]借用] */
        "    push_i32 0\n"
        "    push_i32 7\n"
        "    index_set\n"           /* m[1][0] = 7，返回 self → [m[1]借用] */
        "    push_value 0\n"        /* dup m[1]借用 → [m[1], m[1]] */
        "    push_i32 1\n"
        "    index_get\n"           /* [m[1], 4]（m[1][1] 未被改写） */
        "    push_value 1\n"        /* dup m[1]借用 → [m[1], 4, m[1]] */
        "    push_i32 0\n"
        "    index_get\n"           /* [m[1], 4, 7]（m[1][0] 写回生效） */
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top), vm->type_i32);
    EXPECT_EQ(read_sint(top), 7);
}

/* 借用引用（is_own=false）：INDEX_GET 复合元素返回借用 value，data 指向父数组
 * 槽位。验证 value_is_borrowed + data 指向槽位 + 借用值可继续链式索引。
 * 绑定 materialize（clone 独立性）由 driver 测试 RunFileMultidimBorrowIsIndependentOnBind
 * 覆盖（DEFINE → scope_define → value_clone）。 */
TEST_F(ArrayBcodeTest, BorrowedRefIsNonOwning) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"         /* [open_outer] */
        "    define_type 65\n"     /* 声明：open_outer 绑 id 65 + 登记 */
        "    load_type 65\n"       /* 拉回 open_outer（定义起点） */
        "    push_array\n"         /* [open_outer, open_inner] */
        "    define_type 64\n"     /* 声明：open_inner 绑 id 64 + 登记 */
        "    load_type 64\n"       /* 拉回 open_inner */
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"               /* 弹 t_inner，密封（按自身 id 64 重绑登记） */
        "    load_type 64\n"       /* [open_outer, t_inner] */
        "    define_bound 2\n"     /* 弹 t_inner 设外层元素类型 */
        "    seal\n"               /* 弹 t_outer，密封（按自身 id 65 重绑登记） */
        "    load_type 65\n"       /* [t_outer] */
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 1\n"
        "    push_i32 2\n"
        "    construct 2\n"
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 2\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 3\n"
        "    push_i32 4\n"
        "    construct 2\n"
        "    construct 2\n"         /* [m] */
        "    push_i32 1\n"
        "    index_get\n"           /* [m[1]借用] */
        "    halt\n"));

    value_t *top = stack_top();
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(value_type(top)->kind, TYPE_KIND_ARRAY);
    EXPECT_TRUE(value_is_borrowed(top));       /* 借用引用 */
    EXPECT_EQ(array_type_len(value_type(top)), (size_t)2);
}


/* 汇编 → 反汇编 稳定往返（CONSTRUCT / INDEX_GET / INDEX_SET 助记符正确编解码）。
 * index_get / index_set 均消费 self（与 op_call 一致），故每次访问前用
 * push_value 0 复制数组引用（借用引用，不重复持有 value）。 */
TEST_F(ArrayBcodeTest, ConstructAndIndexAsmDisasmRoundTrip) {
    const char *src =
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_i32 10\n"
        "    push_i32 20\n"
        "    push_i32 30\n"
        "    construct 3\n"
        "    push_value 0\n"     /* dup arr */
        "    push_i32 2\n"
        "    push_i32 99\n"
        "    index_set\n"        /* 写 arr[2]=99，返回 arr */
        "    push_value 0\n"     /* dup arr */
        "    push_i32 2\n"
        "    index_get\n"        /* 读 arr[2] → 99 */
        "    push_value 1\n"     /* dup arr（index_get 结果在顶，复制其下一位） */
        "    length\n"           /* len(arr) → 3 */
        "    pop\n"
        "    halt\n";
    ASSERT_TRUE(assemble_and_run(src));

    allocator_t *da = create_allocator(test_alloc, test_free);
    char *text = bcode_disasm_mem(da, bc, NULL);
    ASSERT_NE(text, nullptr);
    EXPECT_NE(strstr(text, "CONSTRUCT 3"), nullptr);
    EXPECT_NE(strstr(text, "INDEX_GET"), nullptr);
    EXPECT_NE(strstr(text, "INDEX_SET"), nullptr);
    EXPECT_NE(strstr(text, "LENGTH"), nullptr);
    allocator_free(da, (void **)&text);
    delete_allocator(&da);
}

/* PUSH_ARRAY → DEFINE_TYPE <id> 声明 → LOAD_TYPE 拉回 → LOAD elem →
   DEFINE_BOUND N → SEAL（无操作数）两步构造出 [i32;3] */
TEST_F(ArrayBcodeTest, PushArrayDefineBoundSealBuildsArrayType) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    halt\n"));

    value_t *tv = stack_top();
    ASSERT_NE(tv, nullptr);
    EXPECT_EQ(value_type(tv), vm->type_type);

    const type_t *at = value_as(tv, const type_t *);
    EXPECT_EQ(at->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_elem(at), vm->type_i32);
    EXPECT_EQ(array_type_len(at), (size_t)3);
    EXPECT_TRUE(type_is_sealed(at));
}

/* 两次构造 [i32;3] 应 intern 为同一密封类型（第二次去重，开放类型被回收） */
TEST_F(ArrayBcodeTest, DuplicateArrayTypeInternedOnce) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    halt\n"));

    value_t *t1 = exec_stack_peek(vm, 1);
    value_t *t2 = exec_stack_peek(vm, 0);
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(value_as(t1, const type_t *), value_as(t2, const type_t *));
    EXPECT_TRUE(type_is_sealed(value_as(t2, const type_t *)));
}

/* 不同元素类型 / 长度 → 不同密封类型（互不混用） */
TEST_F(ArrayBcodeTest, DistinctElemAndLengthAreDistinct) {
    ASSERT_TRUE(assemble_and_run(
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 3\n"
        "    seal\n"
        "    load_type 64\n"
        "    push_array\n"
        "    define_type 65\n"
        "    load_type 65\n"
        "    load \"u8\"\n"
        "    define_bound 4\n"
        "    seal\n"
        "    load_type 65\n"
        "    halt\n"));

    const type_t *t_i32_3 = value_as(exec_stack_peek(vm, 1), const type_t *);
    const type_t *t_u8_4  = value_as(exec_stack_peek(vm, 0), const type_t *);
    EXPECT_NE(t_i32_3, t_u8_4);
    EXPECT_EQ(array_type_elem(t_i32_3), vm->type_i32);
    EXPECT_EQ(array_type_len(t_i32_3), (size_t)3);
    EXPECT_EQ(array_type_elem(t_u8_4), vm->type_u8);
    EXPECT_EQ(array_type_len(t_u8_4), (size_t)4);
}

/* 汇编 → 反汇编 → 再汇编 稳定往返（PUSH_ARRAY / DEFINE_BOUND 助记符正确编解码） */
TEST_F(ArrayBcodeTest, AsmDisasmRoundTrip) {
    const char *src =
        "    push_array\n"
        "    define_type 64\n"
        "    load_type 64\n"
        "    load \"i32\"\n"
        "    define_bound 5\n"
        "    seal\n"
        "    load_type 64\n"
        "    halt\n";
    ASSERT_TRUE(assemble_and_run(src));

    /* 反汇编到临时 allocator（避免污染 fixture 的 vm->alloc：否则 teardown 的
       空分配断言会把 disasm 缓冲误判为泄漏）。缓冲随 da 销毁一并回收。 */
    allocator_t *da = create_allocator(test_alloc, test_free);
    char *text = bcode_disasm_mem(da, bc, NULL);
    ASSERT_NE(text, nullptr);
    EXPECT_NE(strstr(text, "PUSH_ARRAY"), nullptr);
    EXPECT_NE(strstr(text, "DEFINE_TYPE 64"), nullptr);
    EXPECT_NE(strstr(text, "DEFINE_BOUND 5"), nullptr);
    EXPECT_NE(strstr(text, "SEAL"), nullptr);
    allocator_free(da, (void **)&text); /* delete_allocator 仅报告泄漏、不释放，需显式回收 */
    delete_allocator(&da);
}
