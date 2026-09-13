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
#include "vm/value_dump.h"
#include "core/allocator.h"
#include "core/string.h"
#include "core/strslice.h"
}

/* ---- helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

static value_t *make_i32(vm_t *vm, int32_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i32, &v);
    return value_make(vm, vm->type_i32, data);
}
static value_t *make_u32(vm_t *vm, uint32_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u32, &v);
    return value_make(vm, vm->type_u32, data);
}
static value_t *make_bool(vm_t *vm, bool v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &v);
    return value_make(vm, vm->type_bool, data);
}
static value_t *make_f64(vm_t *vm, double v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_f64, &v);
    return value_make(vm, vm->type_f64, data);
}
static value_t *make_str(vm_t *vm, const char *s) {
    string_t *str = string_from_cstr(vm->alloc, s);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_str, &str);
    return value_make(vm, vm->type_str, data);
}
static value_t *make_const_i32(vm_t *vm, int32_t v) {
    const type_t *ct = type_const_intern(vm, vm->type_i32);
    void *data = value_alloc_data_copy(vm->alloc, ct, &v);
    return value_make(vm, ct, data);
}

class ValueDumpTest : public ::testing::Test {
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

    /* 将单值 dump 为 std::string（临时缓冲用完即释放） */
    std::string dump(const value_t *v) {
        string_t *s = string_new(vm->alloc);
        value_dump_string(vm, v, s);
        std::string out = string_cstr(s);
        string_free(&s);
        return out;
    }
};

TEST_F(ValueDumpTest, Int32) {
    EXPECT_EQ(dump(make_i32(vm, 42)), ".i32{42}");
    EXPECT_EQ(dump(make_i32(vm, -7)), ".i32{-7}");
}

TEST_F(ValueDumpTest, UnsignedInt) {
    EXPECT_EQ(dump(make_u32(vm, 42)), ".u32{42}");
    EXPECT_EQ(dump(make_u32(vm, 0xF0000000)), ".u32{4026531840}");
}

TEST_F(ValueDumpTest, Bool) {
    EXPECT_EQ(dump(make_bool(vm, true)),  ".bool{true}");
    EXPECT_EQ(dump(make_bool(vm, false)), ".bool{false}");
}

TEST_F(ValueDumpTest, Float) {
    EXPECT_EQ(dump(make_f64(vm, 1.5)), ".f64{1.5}");
}

TEST_F(ValueDumpTest, Str) {
    EXPECT_EQ(dump(make_str(vm, "hi")),        ".str{\"hi\"}");
    EXPECT_EQ(dump(make_str(vm, "a b c")),     ".str{\"a b c\"}");
}

TEST_F(ValueDumpTest, ConstInt) {
    EXPECT_EQ(dump(make_const_i32(vm, 7)), ".const i32{7}");
}

TEST_F(ValueDumpTest, ArrayOfInt) {
    value_t *e[3] = { make_i32(vm, 1), make_i32(vm, 2), make_i32(vm, 3) };
    value_t *arr = value_make_array(vm, vm->type_i32, e, 3);
    EXPECT_EQ(dump(arr), ".[3]i32 { .i32{1}, .i32{2}, .i32{3} }");
}

TEST_F(ValueDumpTest, ArrayOfStr) {
    value_t *e[2] = { make_str(vm, "x"), make_str(vm, "yy") };
    value_t *arr = value_make_array(vm, vm->type_str, e, 2);
    EXPECT_EQ(dump(arr), ".[2]str { .str{\"x\"}, .str{\"yy\"} }");
}

TEST_F(ValueDumpTest, ArrayOfConstInt) {
    value_t *e[2] = { make_const_i32(vm, 1), make_const_i32(vm, 2) };
    const type_t *ct = type_const_intern(vm, vm->type_i32);
    value_t *arr = value_make_array(vm, ct, e, 2);
    EXPECT_EQ(dump(arr), ".[2]const i32 { .const i32{1}, .const i32{2} }");
}

TEST_F(ValueDumpTest, NestedArray) {
    value_t *e1[3] = { make_i32(vm, 1), make_i32(vm, 2), make_i32(vm, 3) };
    value_t *e2[3] = { make_i32(vm, 4), make_i32(vm, 5), make_i32(vm, 6) };
    value_t *i1 = value_make_array(vm, vm->type_i32, e1, 3);
    value_t *i2 = value_make_array(vm, vm->type_i32, e2, 3);
    const type_t *inner_t = type_array_intern(vm, vm->type_i32, 3);
    value_t *inner_elems[2] = { i1, i2 };
    value_t *outer = value_make_array(vm, inner_t, inner_elems, 2);
    EXPECT_EQ(dump(outer),
        ".[2][3]i32 { .[3]i32 { .i32{1}, .i32{2}, .i32{3} }, "
                    ".[3]i32 { .i32{4}, .i32{5}, .i32{6} } }");
}
