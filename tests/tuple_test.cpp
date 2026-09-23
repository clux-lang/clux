#include <gtest/gtest.h>
#include "test_common.h"

#include <cstdint>
#include <cstring>

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/type_tuple.h"
#include "vm/type_array.h"
#include "vm/type_error.h"
#include "core/allocator.h"
#include "core/strslice.h"
}

/* ---- helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

static int32_t read_i32(const value_t *v) { return *(const int32_t *)value_data(v); }

static value_t *make_i64_raw(vm_t *vm, int64_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i64, &v);
    return value_make_untracked(vm->alloc, vm->type_i64, data);
}

/* 构造 <i32, i32> 的元素表（type 借用 vm 内建类型） */
static tuple_elem_t two_i32_elems[2] = {
    { 0, nullptr },
    { 0, nullptr },
};

/* ================================================================ */
/* 元组类型基础设施（VM 层）                                          */
/* ================================================================ */

class TupleTypeTest : public ::testing::Test {
protected:
    allocator_t *alloc = nullptr;
    vm_t        *vm    = nullptr;

    void SetUp() override {
        alloc = create_allocator(test_alloc, test_free);
        vm    = vm_new(alloc);
    }
    void TearDown() override {
        vm_destroy(&vm);
        EXPECT_EQ(vm, nullptr);
        EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
    }
};

TEST_F(TupleTypeTest, InternDedup) {
    two_i32_elems[0].type = vm->type_i32;
    two_i32_elems[1].type = vm->type_i32;
    const type_t *a = type_tuple_intern(vm, two_i32_elems, 2);
    const type_t *b = type_tuple_intern(vm, two_i32_elems, 2);
    /* 元素顺序不同 → 不同类型 */
    tuple_elem_t swapped[2] = {
        { 0, vm->type_i64 },
        { 0, vm->type_i32 },
    };
    const type_t *c = type_tuple_intern(vm, swapped, 2);
    /* 元素类型不同 → 不同类型 */
    tuple_elem_t x_i64[2] = {
        { 0, vm->type_i32 },
        { 0, vm->type_i64 },
    };
    const type_t *d = type_tuple_intern(vm, x_i64, 2);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_EQ(a->kind, TYPE_KIND_TUPLE);
    EXPECT_TRUE(type_is_sealed(a));
    EXPECT_EQ(tuple_type_elem_count(a), 2u);
    EXPECT_EQ(tuple_type_elem(a, 0)->type, vm->type_i32);
    EXPECT_EQ(tuple_type_elem(a, 1)->type, vm->type_i32);
    /* 越界访问返回 NULL */
    EXPECT_EQ(tuple_type_elem(a, 2), nullptr);
}

TEST_F(TupleTypeTest, BuildAndSeal) {
    /* 分步构造（对应字节码 push_tuple / load "i32" / append_elem x2 / seal） */
    const type_t *open = type_tuple_push(vm);
    ASSERT_NE(open, nullptr);
    EXPECT_FALSE(type_is_sealed(open));
    EXPECT_EQ(tuple_type_elem_count(open), 0u);

    type_tuple_add_elem(vm, open, vm->type_i32);
    type_tuple_add_elem(vm, open, vm->type_i32);
    EXPECT_EQ(tuple_type_elem_count(open), 2u);

    const type_t *t = type_tuple_seal(vm, open);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(type_is_sealed(t));
    EXPECT_EQ(tuple_type_elem_count(t), 2u);
    /* C 对齐布局：e0: i32(0), e1: i32(4), size=8, align=4 */
    EXPECT_EQ(tuple_type_elem(t, 0)->offset, 0u);
    EXPECT_EQ(tuple_type_elem(t, 1)->offset, 4u);
    EXPECT_EQ(t->size, 8u);
    EXPECT_EQ(t->align, 4u);
    /* 与一次性 intern 结果去重一致 */
    two_i32_elems[0].type = vm->type_i32;
    two_i32_elems[1].type = vm->type_i32;
    EXPECT_EQ(t, type_tuple_intern(vm, two_i32_elems, 2));
}

TEST_F(TupleTypeTest, LayoutMixedAlign) {
    /* i8 + i64：offset_0=0(size 1)；offset_1=align_up(1,8)=8；size=align_up(9,8)=16 */
    tuple_elem_t elems[2] = {
        { 0, vm->type_i8 },
        { 0, vm->type_i64 },
    };
    const type_t *t = type_tuple_intern(vm, elems, 2);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(tuple_type_elem(t, 0)->offset, 0u);
    EXPECT_EQ(tuple_type_elem(t, 1)->offset, 8u);
    EXPECT_EQ(t->size, 16u);
    EXPECT_EQ(t->align, 8u);
}

TEST_F(TupleTypeTest, NestedTupleLayout) {
    /* Inner <i32> size=4 align=4；
       Outer <Inner, i64>：e0@0(size 4)；e1@align_up(4,8)=8；size=16 */
    tuple_elem_t inner_e[1] = { { 0, vm->type_i32 } };
    const type_t *inner = type_tuple_intern(vm, inner_e, 1);
    ASSERT_NE(inner, nullptr);

    tuple_elem_t outer_e[2] = {
        { 0, inner },
        { 0, vm->type_i64 },
    };
    const type_t *outer = type_tuple_intern(vm, outer_e, 2);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(tuple_type_elem(outer, 0)->offset, 0u);
    EXPECT_EQ(tuple_type_elem(outer, 1)->offset, 8u);
    EXPECT_EQ(outer->size, 16u);
    EXPECT_EQ(outer->align, 8u);
}

TEST_F(TupleTypeTest, AddElemAfterSealIgnored) {
    two_i32_elems[0].type = vm->type_i32;
    two_i32_elems[1].type = vm->type_i32;
    const type_t *t = type_tuple_intern(vm, two_i32_elems, 2);
    /* 密封后追加元素被静默忽略 */
    type_tuple_add_elem(vm, t, vm->type_i32);
    EXPECT_EQ(tuple_type_elem_count(t), 2u);
}

TEST_F(TupleTypeTest, EmptyTuple) {
    /* 空元组 <>：C 语义 size=1（与空 struct 一致） */
    const type_t *t = type_tuple_intern(vm, nullptr, 0);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(type_is_sealed(t));
    EXPECT_EQ(tuple_type_elem_count(t), 0u);
    EXPECT_EQ(t->size, 1u);
    /* 空元组去重唯一 */
    EXPECT_EQ(t, type_tuple_intern(vm, nullptr, 0));
}

TEST_F(TupleTypeTest, ValueRoundTrip) {
    /* tuple value 是连续内存块：元素按偏移写 → 读回一致 */
    two_i32_elems[0].type = vm->type_i32;
    two_i32_elems[1].type = vm->type_i32;
    const type_t *t = type_tuple_intern(vm, two_i32_elems, 2);
    ASSERT_NE(t, nullptr);

    void *data = value_alloc_data(vm->alloc, t);
    memset(data, 0, t->size);
    *(int32_t *)((uint8_t *)data + tuple_type_elem(t, 0)->offset) = 10;
    *(int32_t *)((uint8_t *)data + tuple_type_elem(t, 1)->offset) = 20;
    value_t *v = value_make(vm, t, data);

    /* value_tuple_count / value_tuple_at 借用访问器（dump 同款） */
    EXPECT_EQ(value_tuple_count(v), 2u);
    value_t *e0 = value_tuple_at(vm, v, 0);
    ASSERT_NE(e0, nullptr);
    EXPECT_EQ(read_i32(e0), 10);
    value_t *e1 = value_tuple_at(vm, v, 1);
    ASSERT_NE(e1, nullptr);
    EXPECT_EQ(read_i32(e1), 20);
    /* 越界返回 NULL */
    EXPECT_EQ(value_tuple_at(vm, v, 2), nullptr);
}

TEST_F(TupleTypeTest, EqElemWise) {
    /* 同 tuple 实例按元素递归比较（值相等） */
    two_i32_elems[0].type = vm->type_i32;
    two_i32_elems[1].type = vm->type_i32;
    const type_t *t = type_tuple_intern(vm, two_i32_elems, 2);
    ASSERT_NE(t, nullptr);

    void *da = value_alloc_data(vm->alloc, t);
    void *db = value_alloc_data(vm->alloc, t);
    memset(da, 0, t->size);
    memset(db, 0, t->size);
    *(int32_t *)((uint8_t *)da + tuple_type_elem(t, 0)->offset) = 10;
    *(int32_t *)((uint8_t *)da + tuple_type_elem(t, 1)->offset) = 20;
    *(int32_t *)((uint8_t *)db + tuple_type_elem(t, 0)->offset) = 10;
    *(int32_t *)((uint8_t *)db + tuple_type_elem(t, 1)->offset) = 20;
    value_t *a = value_make(vm, t, da);
    value_t *b = value_make(vm, t, db);

    value_t *r = value_eq(vm, a, b);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_TRUE(value_as(r, bool));

    /* 改第二个元素值 → 不相等 */
    *(int32_t *)((uint8_t *)value_data(b) + tuple_type_elem(t, 1)->offset) = 99;
    value_t *r2 = value_eq(vm, a, b);
    ASSERT_NE(r2, nullptr);
    EXPECT_FALSE(value_is_error(vm, r2));
    EXPECT_FALSE(value_as(r2, bool));
}

TEST_F(TupleTypeTest, Compatible) {
    /* 元素类型+顺序一致 → 兼容；任一不同 → 不兼容 */
    tuple_elem_t a_e[2] = { { 0, vm->type_i32 }, { 0, vm->type_i32 } };
    tuple_elem_t b_e[2] = { { 0, vm->type_i32 }, { 0, vm->type_i32 } };
    tuple_elem_t c_e[2] = { { 0, vm->type_i32 }, { 0, vm->type_i64 } };
    tuple_elem_t d_e[2] = { { 0, vm->type_i64 }, { 0, vm->type_i32 } };
    const type_t *a = type_tuple_intern(vm, a_e, 2);
    const type_t *b = type_tuple_intern(vm, b_e, 2);
    const type_t *c = type_tuple_intern(vm, c_e, 2);
    const type_t *d = type_tuple_intern(vm, d_e, 2);
    /* 同实例（intern 去重）与跨实例（结构兼容）都返回 true */
    EXPECT_TRUE(tuple_type_compatible(vm, a, a));
    EXPECT_TRUE(tuple_type_compatible(vm, a, b));
    EXPECT_FALSE(tuple_type_compatible(vm, a, c));
    EXPECT_FALSE(tuple_type_compatible(vm, a, d));
    /* 非 tuple 输入返回 false */
    EXPECT_FALSE(tuple_type_compatible(vm, a, vm->type_i32));
    EXPECT_FALSE(tuple_type_compatible(vm, vm->type_i32, a));
}

TEST_F(TupleTypeTest, TupleArrayLayoutCompatible) {
    /* Tuple↔Array 布局兼容：元素 type_equal + count 相等 → 可互转 */
    two_i32_elems[0].type = vm->type_i32;
    two_i32_elems[1].type = vm->type_i32;
    const type_t *t = type_tuple_intern(vm, two_i32_elems, 2);
    const type_t *arr = type_array_intern(vm, vm->type_i32, 2);
    ASSERT_NE(t, nullptr);
    ASSERT_NE(arr, nullptr);

    /* 隐式转换双向成立 */
    void *data = value_alloc_data(vm->alloc, t);
    memset(data, 0, t->size);
    value_t *tv = value_make(vm, t, data);
    value_t *cast_arr = value_implicit_cast(vm, tv, arr);
    ASSERT_NE(cast_arr, nullptr);
    EXPECT_FALSE(value_is_error(vm, cast_arr));
    EXPECT_EQ(value_type(cast_arr), arr);

    void *adata = value_alloc_data(vm->alloc, arr);
    memset(adata, 0, arr->size);
    value_t *av = value_make(vm, arr, adata);
    value_t *cast_t = value_implicit_cast(vm, av, t);
    ASSERT_NE(cast_t, nullptr);
    EXPECT_FALSE(value_is_error(vm, cast_t));
    EXPECT_EQ(value_type(cast_t), t);
}

TEST_F(TupleTypeTest, AssignSafeCast) {
    /* tuple 赋值走 vtable assign（safe_cast）：
       i32 字面量（同宽度）→ i64 元素提升 */
    tuple_elem_t mixed[2] = { { 0, vm->type_i32 }, { 0, vm->type_i64 } };
    const type_t *t = type_tuple_intern(vm, mixed, 2);
    ASSERT_NE(t, nullptr);

    void *data = value_alloc_data(vm->alloc, t);
    memset(data, 0, t->size);
    value_t *v = value_make(vm, t, data);

    /* i32 值写入 i64 元素位置：assign 目标元素类型 i64，i32 提升 */
    value_t *i32v = make_i64_raw(vm, 42);
    value_t *dst  = value_tuple_at(vm, v, 1);
    ASSERT_NE(dst, nullptr);
    value_t *r = value_assign(vm, dst, i32v);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_EQ(*(int64_t *)((uint8_t *)value_data(v) + tuple_type_elem(t, 1)->offset),
              (int64_t)42);

    /* 释放手动创建的 untracked value（value + data） */
    value_dispose(vm, i32v);
    allocator_free(alloc, (void **)&i32v);
}
