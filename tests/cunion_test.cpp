#include <gtest/gtest.h>
#include "test_common.h"

#include <cstdint>
#include <cstring>

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/type_cunion.h"
#include "vm/type_error.h"
#include "core/allocator.h"
#include "core/strslice.h"
}

/* ---- helpers ---- */

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr)     { free(ptr); }

static int32_t read_i32(const value_t *v) { return *(const int32_t *)value_data(v); }
static int64_t read_i64(const value_t *v) { return *(const int64_t *)value_data(v); }
static float   read_f32(const value_t *v) { return *(const float *)value_data(v); }

/* U { i: i32; f: f32; b: bool } member 表（name 借用栈上字面量，intern 深拷贝） */
static cunion_member_t three_members[3] = {
    { STRSLICE_LIT("i"), nullptr },
    { STRSLICE_LIT("f"), nullptr },
    { STRSLICE_LIT("b"), nullptr },
};

/* ================================================================ */
/* C 语义 union 类型基础设施（VM 层）                                  */
/* ================================================================ */

class CUnionTypeTest : public ::testing::Test {
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

/* 去重 intern：按 (member 名 + 类型 + 顺序) 去重；顺序/类型不同 → 不同类型 */
TEST_F(CUnionTypeTest, InternDedup) {
    three_members[0].type = vm->type_i32;
    three_members[1].type = vm->type_f32;
    three_members[2].type = vm->type_bool;
    const type_t *a = type_cunion_intern(vm, three_members, 3);
    const type_t *b = type_cunion_intern(vm, three_members, 3);
    /* 不同 member 顺序 → 不同类型 */
    cunion_member_t swapped[3] = {
        { STRSLICE_LIT("i"), vm->type_i32 },
        { STRSLICE_LIT("b"), vm->type_bool },
        { STRSLICE_LIT("f"), vm->type_f32 },
    };
    const type_t *c = type_cunion_intern(vm, swapped, 3);
    /* 同 member 名但类型不同 → 不同类型 */
    cunion_member_t i64_i32[3] = {
        { STRSLICE_LIT("i"), vm->type_i64 },
        { STRSLICE_LIT("f"), vm->type_f32 },
        { STRSLICE_LIT("b"), vm->type_bool },
    };
    const type_t *d = type_cunion_intern(vm, i64_i32, 3);
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
    EXPECT_NE(a, d);
    EXPECT_EQ(a->kind, TYPE_KIND_CUNION);
    EXPECT_TRUE(type_is_sealed(a));
    EXPECT_EQ(cunion_type_member_count(a), 3u);
    EXPECT_EQ(cunion_type_member(a, 0)->type, vm->type_i32);
    EXPECT_EQ(cunion_type_member(a, 1)->type, vm->type_f32);
    EXPECT_EQ(cunion_type_find_member(a, STRSLICE_LIT("i")), 0);
    EXPECT_EQ(cunion_type_find_member(a, STRSLICE_LIT("f")), 1);
    EXPECT_EQ(cunion_type_find_member(a, STRSLICE_LIT("z")), -1);
}

/* 分步构造（对应字节码 push_cunion / define_field "i" / define_field "f" /
   define_field "b" / seal） */
TEST_F(CUnionTypeTest, BuildAndSeal) {
    const type_t *open = type_cunion_push(vm);
    ASSERT_NE(open, nullptr);
    EXPECT_FALSE(type_is_sealed(open));
    EXPECT_EQ(cunion_type_member_count(open), 0u);

    type_cunion_add_member(vm, open, STRSLICE_LIT("i"));
    type_cunion_set_member_type(vm, open, vm->type_i32);
    type_cunion_add_member(vm, open, STRSLICE_LIT("f"));
    type_cunion_set_member_type(vm, open, vm->type_f32);
    type_cunion_add_member(vm, open, STRSLICE_LIT("b"));
    type_cunion_set_member_type(vm, open, vm->type_bool);
    EXPECT_EQ(cunion_type_member_count(open), 3u);

    const type_t *t = type_cunion_seal(vm, open);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(type_is_sealed(t));
    EXPECT_EQ(cunion_type_member_count(t), 3u);
    /* C union 布局：size = max(member size) 对齐，align = max(member align)，
       所有 member 共享 offset 0（无 tag） */
    EXPECT_EQ(t->size, 4u);   /* max(4,4,1)=4 */
    EXPECT_EQ(t->align, 4u);  /* max(4,4,1)=4 */
    /* 与一次性 intern 结果去重一致 */
    three_members[0].type = vm->type_i32;
    three_members[1].type = vm->type_f32;
    three_members[2].type = vm->type_bool;
    EXPECT_EQ(t, type_cunion_intern(vm, three_members, 3));
}

/* 布局：i64 + f32 → size = align_up(8,8) = 8，align = 8（C union max 语义） */
TEST_F(CUnionTypeTest, LayoutMaxSizeAlign) {
    cunion_member_t fields[2] = {
        { STRSLICE_LIT("a"), vm->type_i64 },
        { STRSLICE_LIT("b"), vm->type_f32 },
    };
    const type_t *t = type_cunion_intern(vm, fields, 2);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->size, 8u);
    EXPECT_EQ(t->align, 8u);
    EXPECT_EQ(cunion_type_member(t, 0)->type, vm->type_i64);
    EXPECT_EQ(cunion_type_member(t, 1)->type, vm->type_f32);
}

/* 空 member 列表 → size=1（对齐 struct 空类型 C 语义，保证 data 块可分配） */
TEST_F(CUnionTypeTest, EmptyMemberList) {
    const type_t *t = type_cunion_intern(vm, nullptr, 0);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->size, 1u);
    EXPECT_EQ(t->align, 1u);
    EXPECT_EQ(cunion_type_member_count(t), 0u);
}

/* 密封后追加 member 被静默忽略（与 struct 同款） */
TEST_F(CUnionTypeTest, AddMemberAfterSealIgnored) {
    three_members[0].type = vm->type_i32;
    const type_t *t = type_cunion_intern(vm, three_members, 1);
    type_cunion_add_member(vm, t, STRSLICE_LIT("z"));
    type_cunion_set_member_type(vm, t, vm->type_i32);
    EXPECT_EQ(cunion_type_member_count(t), 1u);
    EXPECT_EQ(cunion_type_find_member(t, STRSLICE_LIT("z")), -1);
}

/* 值往返：cunion value 连续内存块，共享 offset 0（写 member i → member f
   同一块内存按各自类型解释） */
TEST_F(CUnionTypeTest, ValueRoundTripSharedMemory) {
    cunion_member_t fields[2] = {
        { STRSLICE_LIT("i"), vm->type_i32 },
        { STRSLICE_LIT("f"), vm->type_f32 },
    };
    const type_t *t = type_cunion_intern(vm, fields, 2);
    ASSERT_NE(t, nullptr);

    void *data = value_alloc_data(vm->alloc, t);
    memset(data, 0, t->size);
    value_t *v = value_make(vm, t, data);

    /* 写 member "i"（offset 0）→ 同一块内存读 member "f"（按 f32 解释） */
    *(int32_t *)((uint8_t *)value_data(v)) = 0x40490FDB; /* 3.14159f32 位模式 */
    value_t *f = value_make_borrowed(vm, vm->type_f32, value_data(v));
    ASSERT_NE(f, nullptr);
    EXPECT_FLOAT_EQ(read_f32(f), 3.14159274f);
}

/* eq / ne：memcmp 字节比较（C union 语义） */
TEST_F(CUnionTypeTest, EqMemcmp) {
    cunion_member_t fields[2] = {
        { STRSLICE_LIT("i"), vm->type_i32 },
        { STRSLICE_LIT("f"), vm->type_f32 },
    };
    const type_t *t = type_cunion_intern(vm, fields, 2);
    ASSERT_NE(t, nullptr);

    void *da = value_alloc_data(vm->alloc, t);
    void *db = value_alloc_data(vm->alloc, t);
    memset(da, 0, t->size);
    memset(db, 0, t->size);
    *(int32_t *)da = 10;
    *(int32_t *)db = 10;
    value_t *a = value_make(vm, t, da);
    value_t *b = value_make(vm, t, db);

    value_t *r = value_eq(vm, a, b);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_TRUE(value_as(r, bool));

    /* 改 b 的字节 → 不相等 */
    *(int32_t *)value_data(b) = 99;
    value_t *r2 = value_eq(vm, a, b);
    ASSERT_NE(r2, nullptr);
    EXPECT_FALSE(value_is_error(vm, r2));
    EXPECT_FALSE(value_as(r2, bool));

    value_t *rn = value_ne(vm, a, b);
    ASSERT_NE(rn, nullptr);
    EXPECT_FALSE(value_is_error(vm, rn));
    EXPECT_TRUE(value_as(rn, bool));
}

/* assign：同 cunion 实例 memcpy 覆盖（平凡拷贝，与 struct_assign 同款） */
TEST_F(CUnionTypeTest, AssignMemcpy) {
    cunion_member_t fields[1] = {
        { STRSLICE_LIT("i"), vm->type_i64 },
    };
    const type_t *t = type_cunion_intern(vm, fields, 1);
    ASSERT_NE(t, nullptr);

    void *da = value_alloc_data(vm->alloc, t);
    void *db = value_alloc_data(vm->alloc, t);
    *(int64_t *)da = 100;
    *(int64_t *)db = 200;
    value_t *a = value_make(vm, t, da);
    value_t *b = value_make(vm, t, db);

    value_t *r = value_assign(vm, a, b);
    ASSERT_NE(r, nullptr);
    EXPECT_FALSE(value_is_error(vm, r));
    EXPECT_EQ(read_i64(a), 200);
}

/* clone：整块 memcpy 深拷贝（独立副本） */
TEST_F(CUnionTypeTest, CloneMemcpy) {
    cunion_member_t fields[1] = {
        { STRSLICE_LIT("i"), vm->type_i64 },
    };
    const type_t *t = type_cunion_intern(vm, fields, 1);
    ASSERT_NE(t, nullptr);

    void *da = value_alloc_data(vm->alloc, t);
    *(int64_t *)da = 42;
    value_t *a = value_make(vm, t, da);

    value_t *c = value_clone(vm, a);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(read_i64(c), 42);
    /* 独立副本：改原值不影响 clone */
    *(int64_t *)value_data(a) = 7;
    EXPECT_EQ(read_i64(c), 42);
}

/* 类型转换：仅同实例身份拷贝；不同 cunion 实例 → error */
TEST_F(CUnionTypeTest, CastSameInstanceIdentityOnly) {
    cunion_member_t fields[1] = {
        { STRSLICE_LIT("i"), vm->type_i32 },
    };
    const type_t *t = type_cunion_intern(vm, fields, 1);
    ASSERT_NE(t, nullptr);

    void *da = value_alloc_data(vm->alloc, t);
    *(int32_t *)da = 5;
    value_t *a = value_make(vm, t, da);

    value_t *c = value_implicit_cast(vm, a, t);
    ASSERT_NE(c, nullptr);
    EXPECT_FALSE(value_is_error(vm, c));
    EXPECT_EQ(value_type(c), t);
    EXPECT_EQ(read_i32(c), 5);

    /* 不同 cunion 实例（同构但独立 intern）→ 隐式转换拒绝 */
    cunion_member_t fields3[1] = {
        { STRSLICE_LIT("i"), vm->type_i64 },
    };
    const type_t *t3 = type_cunion_intern(vm, fields3, 1);
    value_t *r = value_implicit_cast(vm, a, t3);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));
}
