#include <gtest/gtest.h>
#include "test_common.h"

#include <cstdint>
#include <cstring>

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/type_struct.h"
#include "vm/type_error.h"
#include "core/allocator.h"
#include "core/strslice.h"
}

/* ---- helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

static int32_t read_i32(const value_t *v) { return *(const int32_t *)value_data(v); }

/* 构造 Point { x: i32; y: i32 } 的字段表（name 借用栈上字面量，intern 深拷贝） */
static struct_field_t point_fields[2] = {
    { STRSLICE_LIT("x"), 0, nullptr },
    { STRSLICE_LIT("y"), 0, nullptr },
};

/* ================================================================ */
/* 结构体类型基础设施（VM 层）                                        */
/* ================================================================ */

class StructTypeTest : public ::testing::Test {
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

TEST_F(StructTypeTest, InternDedup) {
    point_fields[0].type = vm->type_i32;
    point_fields[1].type = vm->type_i32;
    const type_t *a = type_struct_intern(vm, point_fields, 2);
    const type_t *b = type_struct_intern(vm, point_fields, 2);
    /* 不同字段顺序 → 不同类型 */
    struct_field_t swapped[2] = {
        { STRSLICE_LIT("y"), 0, vm->type_i32 },
        { STRSLICE_LIT("x"), 0, vm->type_i32 },
    };
    const type_t *c = type_struct_intern(vm, swapped, 2);
    /* 同字段名但类型不同 → 不同类型 */
    struct_field_t x_i64[2] = {
        { STRSLICE_LIT("x"), 0, vm->type_i64 },
        { STRSLICE_LIT("y"), 0, vm->type_i32 },
    };
    const type_t *d = type_struct_intern(vm, x_i64, 2);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_EQ(a->kind, TYPE_KIND_STRUCT);
    EXPECT_TRUE(type_is_sealed(a));
    EXPECT_EQ(struct_type_field_count(a), 2u);
    EXPECT_EQ(struct_type_field(a, 0)->type, vm->type_i32);
    EXPECT_EQ(struct_type_field(a, 1)->type, vm->type_i32);
    EXPECT_EQ(struct_type_find_field(a, STRSLICE_LIT("x")), 0);
    EXPECT_EQ(struct_type_find_field(a, STRSLICE_LIT("y")), 1);
    EXPECT_EQ(struct_type_find_field(a, STRSLICE_LIT("z")), -1);
}

TEST_F(StructTypeTest, BuildAndSeal) {
    /* 分步构造（对应字节码 push_struct / load "i32" / define_field "x" ... / seal） */
    const type_t *open = type_struct_push(vm);
    ASSERT_NE(open, nullptr);
    EXPECT_FALSE(type_is_sealed(open));
    EXPECT_EQ(struct_type_field_count(open), 0u);

    type_struct_add_field(vm, open, STRSLICE_LIT("x"), vm->type_i32);
    type_struct_add_field(vm, open, STRSLICE_LIT("y"), vm->type_i32);
    EXPECT_EQ(struct_type_field_count(open), 2u);

    const type_t *t = type_struct_seal(vm, open);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(type_is_sealed(t));
    EXPECT_EQ(struct_type_field_count(t), 2u);
    /* C 对齐布局：x: i32(0), y: i32(4), size=8, align=4 */
    EXPECT_EQ(struct_type_field(t, 0)->offset, 0u);
    EXPECT_EQ(struct_type_field(t, 1)->offset, 4u);
    EXPECT_EQ(t->size, 8u);
    EXPECT_EQ(t->align, 4u);
    /* 与一次性 intern 结果去重一致 */
    point_fields[0].type = vm->type_i32;
    point_fields[1].type = vm->type_i32;
    EXPECT_EQ(t, type_struct_intern(vm, point_fields, 2));
}

TEST_F(StructTypeTest, LayoutMixedAlign) {
    /* i8 + i64：offset_0=0(size 1)；offset_1=align_up(1,8)=8；size=align_up(9,8)=16 */
    struct_field_t fields[2] = {
        { STRSLICE_LIT("a"), 0, vm->type_i8 },
        { STRSLICE_LIT("b"), 0, vm->type_i64 },
    };
    const type_t *t = type_struct_intern(vm, fields, 2);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(struct_type_field(t, 0)->offset, 0u);
    EXPECT_EQ(struct_type_field(t, 1)->offset, 8u);
    EXPECT_EQ(t->size, 16u);
    EXPECT_EQ(t->align, 8u);
}

TEST_F(StructTypeTest, NestedStructLayout) {
    /* Inner { a: i32 } size=4 align=4；
       Outer { i: Inner; b: i64 }：i@0(size 4)；b@align_up(4,8)=8；size=16 */
    struct_field_t inner_f[1] = { { STRSLICE_LIT("a"), 0, vm->type_i32 } };
    const type_t *inner = type_struct_intern(vm, inner_f, 1);
    ASSERT_NE(inner, nullptr);

    struct_field_t outer_f[2] = {
        { STRSLICE_LIT("i"), 0, inner },
        { STRSLICE_LIT("b"), 0, vm->type_i64 },
    };
    const type_t *outer = type_struct_intern(vm, outer_f, 2);
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(struct_type_field(outer, 0)->offset, 0u);
    EXPECT_EQ(struct_type_field(outer, 1)->offset, 8u);
    EXPECT_EQ(outer->size, 16u);
    EXPECT_EQ(outer->align, 8u);
}

/* ================================================================
 * seal 窗口不变量：类型的字段表/嵌入表**只能在 seal 之前**编辑；
 * seal 之后写入 API 一律拒绝（结构体组合必须走 seal 前窗口）。
 * ================================================================ */

TEST_F(StructTypeTest, EmbedFieldsOnlyEditableBeforeSeal) {
    /* Base { id: i32 }（已 seal，作为嵌入来源） */
    struct_field_t base_f[1] = { { STRSLICE_LIT("id"), 0, vm->type_i32 } };
    const type_t *base = type_struct_intern(vm, base_f, 1);
    ASSERT_NE(base, nullptr);
    ASSERT_TRUE(type_is_sealed(base));

    /* 开放窗口：自有字段 extra 之后再插入 Base 的字段表（layer=1 → 排在 extra 后） */
    const type_t *open = type_struct_alloc_open(vm);
    ASSERT_NE(open, nullptr);
    ASSERT_FALSE(type_is_sealed(open));
    type_struct_add_field(vm, open, STRSLICE_LIT("extra"), vm->type_i64);
    ASSERT_EQ(struct_type_field_count(open), 1u);

    const type_t *emb[1] = { base };
    struct_embed_fields_t span = {
        struct_type_field(base, 0), 1, 1 /* layer 1 = 末尾（自有字段之后） */
    };
    EXPECT_TRUE(type_struct_insert_embed_fields(vm, open, &span, 1));
    EXPECT_TRUE(type_struct_set_embeds(vm, open, emb, 1));
    EXPECT_EQ(struct_type_field_count(open), 2u);

    const type_t *sealed = type_struct_seal(vm, open);
    ASSERT_NE(sealed, nullptr);
    EXPECT_TRUE(type_is_sealed(sealed));
    /* 字段序 extra, id（layer=1）；布局 8 + 4 → 16 */
    EXPECT_EQ(struct_type_field_count(sealed), 2u);
    EXPECT_EQ(struct_type_field(sealed, 1)->offset, 8u);
    EXPECT_EQ(sealed->size, 16u);
    size_t ec = 0;
    const type_t *const *et = type_struct_embeds(sealed, &ec);
    ASSERT_EQ(ec, 1u);
    EXPECT_EQ(et[0], base);

    /* seal 之后：同样的两个写入 API 必须**拒绝**且不改动类型 */
    size_t before = struct_type_field_count(sealed);
    EXPECT_FALSE(type_struct_insert_embed_fields(vm, sealed, &span, 1));
    EXPECT_FALSE(type_struct_set_embeds(vm, sealed, NULL, 0));
    EXPECT_EQ(struct_type_field_count(sealed), before);
    size_t ec2 = 0;
    type_struct_embeds(sealed, &ec2);
    EXPECT_EQ(ec2, 1u);
}

TEST_F(StructTypeTest, AddFieldAfterSealIgnored) {
    point_fields[0].type = vm->type_i32;
    point_fields[1].type = vm->type_i32;
    const type_t *t = type_struct_intern(vm, point_fields, 2);
    /* 密封后追加字段被静默忽略 */
    type_struct_add_field(vm, t, STRSLICE_LIT("z"), vm->type_i32);
    EXPECT_EQ(struct_type_field_count(t), 2u);
    EXPECT_EQ(struct_type_find_field(t, STRSLICE_LIT("z")), -1);
}

TEST_F(StructTypeTest, ValueRoundTrip) {
    /* struct value 是连续内存块：字段按偏移写 → 读回一致 */
    point_fields[0].type = vm->type_i32;
    point_fields[1].type = vm->type_i32;
    const type_t *t = type_struct_intern(vm, point_fields, 2);
    ASSERT_NE(t, nullptr);

    void *data = value_alloc_data(vm->alloc, t);
    memset(data, 0, t->size);
    *(int32_t *)((uint8_t *)data + struct_type_field(t, 0)->offset) = 10;
    *(int32_t *)((uint8_t *)data + struct_type_field(t, 1)->offset) = 20;
    value_t *v = value_make(vm, t, data);

    value_t *x = value_make_borrowed(vm, vm->type_i32,
                                     (uint8_t *)value_data(v) + struct_type_field(t, 0)->offset);
    ASSERT_NE(x, nullptr);
    EXPECT_EQ(read_i32(x), 10);
    value_t *y = value_make_borrowed(vm, vm->type_i32,
                                     (uint8_t *)value_data(v) + struct_type_field(t, 1)->offset);
    ASSERT_NE(y, nullptr);
    EXPECT_EQ(read_i32(y), 20);
}

TEST_F(StructTypeTest, EqFieldWise) {
    /* 同 struct 实例按字段递归比较（值相等） */
    point_fields[0].type = vm->type_i32;
    point_fields[1].type = vm->type_i32;
    const type_t *t = type_struct_intern(vm, point_fields, 2);
    ASSERT_NE(t, nullptr);

    void *da = value_alloc_data(vm->alloc, t);
    void *db = value_alloc_data(vm->alloc, t);
    memset(da, 0, t->size);
    memset(db, 0, t->size);
    *(int32_t *)((uint8_t *)da + struct_type_field(t, 0)->offset) = 10;
    *(int32_t *)((uint8_t *)da + struct_type_field(t, 1)->offset) = 20;
    *(int32_t *)((uint8_t *)db + struct_type_field(t, 0)->offset) = 10;
    *(int32_t *)((uint8_t *)db + struct_type_field(t, 1)->offset) = 20;
    value_t *a = value_make(vm, t, da);
    value_t *b = value_make(vm, t, db);

    value_t *r = value_eq(vm, a, b);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_TRUE(value_as(r, bool));

    /* 改 y 值 → 不相等 */
    *(int32_t *)((uint8_t *)value_data(b) + struct_type_field(t, 1)->offset) = 99;
    value_t *r2 = value_eq(vm, a, b);
    ASSERT_NE(r2, nullptr);
    EXPECT_FALSE(value_is_error(vm, r2));
    EXPECT_FALSE(value_as(r2, bool));
}
