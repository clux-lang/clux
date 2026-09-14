#include <gtest/gtest.h>
#include "test_common.h"

#include <stdexcept>
#include <cstdint>

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_error.h"
#include "core/allocator.h"
#include "core/string.h"
#include "core/strslice.h"
}

/* ---- helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

static int32_t read_i32(const value_t *v) { return *(const int32_t *)value_data(v); }
static int64_t read_i64(const value_t *v) { return *(const int64_t *)value_data(v); }
static uint64_t read_u64(const value_t *v) { return *(const uint64_t *)value_data(v); }

static value_t *make_i32(vm_t *vm, int32_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i32, &v);
    return value_make(vm, vm->type_i32, data);
}
static value_t *make_i64(vm_t *vm, int64_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i64, &v);
    return value_make(vm, vm->type_i64, data);
}

/* ================================================================ */
/* 数组类型基础设施（VM 层）                                          */
/* ================================================================ */

class ArrayTypeTest : public ::testing::Test {
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

TEST_F(ArrayTypeTest, InternDedup) {
    const type_t *a = type_array_intern(vm, vm->type_i32, 3);
    const type_t *b = type_array_intern(vm, vm->type_i32, 3);
    const type_t *c = type_array_intern(vm, vm->type_i64, 3);
    const type_t *d = type_array_intern(vm, vm->type_i32, SIZE_MAX);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_EQ(a->kind, TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_elem(a), vm->type_i32);
    EXPECT_EQ(array_type_len(a), 3u);
}

TEST_F(ArrayTypeTest, BuildAndSeal) {
    /* 分步构造（对应字节码 push_array / load "i32" / define_bound 3 / seal）。
     * 外部只持有 type_t*，不感知 array_type_t 子类。 */
    const type_t *open = array_type_push(vm);          /* push_array：入池 + 压 type value */
    ASSERT_NE(open, nullptr);
    EXPECT_FALSE(array_type_is_sealed(open));
    EXPECT_EQ(array_type_len(open), SIZE_MAX);         /* 未成形：元素数量待定 */

    array_type_set_elem(vm, open, vm->type_i32);       /* load "i32" */
    array_type_set_count(vm, open, 3);                 /* define_bound 3（成形） */
    EXPECT_EQ(array_type_len(open), 3u);               /* 仍未密封 */

    const type_t *t = array_type_seal(vm, open);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(array_type_is_sealed(t));
    EXPECT_EQ(array_type_elem(t), vm->type_i32);
    EXPECT_EQ(array_type_len(t), 3u);
    /* 密封计算内存布局：[i32;3] => size = 4*3 = 12, align = 4 */
    EXPECT_EQ(array_type_layout_size(t), 12u);
    EXPECT_EQ(array_type_layout_align(t), 4u);
    /* 与一次性 intern 结果去重一致 */
    EXPECT_EQ(t, type_array_intern(vm, vm->type_i32, 3));

    /* set_count 重复调用应被忽略（数组仅一个元素类型槽位） */
    const type_t *dup = array_type_push(vm);
    array_type_set_elem(vm, dup, vm->type_i32);
    array_type_set_count(vm, dup, 3);
    array_type_set_count(vm, dup, 99);  /* 第二次忽略 */
    const type_t *dt2 = array_type_seal(vm, dup);
    EXPECT_EQ(array_type_len(dt2), 3u);

    /* 动态切片（省略 set_count，length 保持 SIZE_MAX）：无静态布局 */
    const type_t *dyn = array_type_push(vm);
    array_type_set_elem(vm, dyn, vm->type_i64);
    const type_t *dt = array_type_seal(vm, dyn);
    ASSERT_NE(dt, nullptr);
    EXPECT_EQ(array_type_len(dt), SIZE_MAX);
    EXPECT_EQ(array_type_layout_size(dt), 0u);
}

TEST_F(ArrayTypeTest, MakeAndLength) {
    value_t *elems[3] = { make_i32(vm, 10), make_i32(vm, 20), make_i32(vm, 30) };
    value_t *arr = value_make_array(vm, vm->type_i32, elems, 3);
    ASSERT_NE(arr, nullptr);
    EXPECT_FALSE(value_is_error(vm, arr));
    EXPECT_EQ(value_kind(arr), TYPE_KIND_ARRAY);
    EXPECT_EQ(array_type_elem(value_type(arr)), vm->type_i32);
    EXPECT_EQ(array_type_len(value_type(arr)), 3u);

    value_t *len = value_length(vm, arr);
    ASSERT_NE(len, nullptr);
    EXPECT_FALSE(value_is_error(vm, len));
    EXPECT_EQ(value_type(len), vm->type_u64);
    EXPECT_EQ(read_u64(len), 3u);
}

TEST_F(ArrayTypeTest, GetIndex) {
    value_t *elems[3] = { make_i32(vm, 10), make_i32(vm, 20), make_i32(vm, 30) };
    value_t *arr = value_make_array(vm, vm->type_i32, elems, 3);
    value_t *idx = make_i64(vm, 1);
    value_t *got = value_get_index(vm, arr, idx);
    ASSERT_NE(got, nullptr);
    EXPECT_FALSE(value_is_error(vm, got));
    EXPECT_EQ(value_type(got), vm->type_i32);
    EXPECT_EQ(read_i32(got), 20);
}

TEST_F(ArrayTypeTest, GetIndexOutOfBounds) {
    value_t *elems[2] = { make_i32(vm, 1), make_i32(vm, 2) };
    value_t *arr = value_make_array(vm, vm->type_i32, elems, 2);
    value_t *idx = make_i64(vm, 5);
    value_t *got = value_get_index(vm, arr, idx);
    EXPECT_TRUE(value_is_error(vm, got));
}

TEST_F(ArrayTypeTest, SetIndex) {
    value_t *elems[3] = { make_i32(vm, 10), make_i32(vm, 20), make_i32(vm, 30) };
    value_t *arr = value_make_array(vm, vm->type_i32, elems, 3);
    value_t *idx   = make_i64(vm, 1);
    value_t *newv  = make_i32(vm, 99);
    value_t *ret   = value_set_index(vm, arr, idx, newv);
    EXPECT_FALSE(value_is_error(vm, ret));
    value_t *got = value_get_index(vm, arr, idx);
    ASSERT_NE(got, nullptr);
    EXPECT_FALSE(value_is_error(vm, got));
    EXPECT_EQ(read_i32(got), 99);
}

TEST_F(ArrayTypeTest, CloneIndependent) {
    value_t *elems[2] = { make_i32(vm, 1), make_i32(vm, 2) };
    value_t *arr = value_make_array(vm, vm->type_i32, elems, 2);
    value_t *cp  = value_clone(vm, arr);
    value_set_index(vm, arr, make_i64(vm, 0), make_i32(vm, 100));
    value_t *orig0 = value_get_index(vm, arr, make_i64(vm, 0));
    value_t *cp0   = value_get_index(vm, cp,  make_i64(vm, 0));
    ASSERT_NE(orig0, nullptr);
    ASSERT_NE(cp0, nullptr);
    EXPECT_FALSE(value_is_error(vm, orig0));
    EXPECT_FALSE(value_is_error(vm, cp0));
    EXPECT_EQ(read_i32(orig0), 100);
    EXPECT_EQ(read_i32(cp0), 1);
}
