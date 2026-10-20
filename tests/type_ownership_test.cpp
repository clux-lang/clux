/*
 * 所有权修饰类型（own T / ref T / fatal T）—— P0 基础设施（docs/ownership-as-type.md）
 *
 * 目标语义：所有权是**类型修饰**（与 const/volatile 同构），参与类型相等与 intern；
 * 但**运行期剥离**——运行期指令只引用裸类型，因此 size/align/vtable 全部与 sub 相同。
 *
 * 本文件只测 P0（构造/intern/剥链/查询）；类型解析产出、sema 判定切换在后续阶段。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "test_common.h"

extern "C" {
#include "core/allocator.h"
#include "vm/type.h"
#include "vm/type_ownership.h"
#include "vm/vm.h"
#include "sema/reflect.h" /* 复用 kind/ownership 名字表（反射与类型层同一套字符串） */
}

namespace {

static void *test_alloc(size_t size) { return malloc(size); }
static void test_free(void *ptr) { free(ptr); }

class OwnershipTypeTest : public ::testing::Test {
protected:
  allocator_t *alloc = nullptr;
  vm_t        *vm    = nullptr;

  void SetUp() override {
    alloc = create_allocator(test_alloc, test_free);
    vm    = vm_new(alloc);
  }
  void TearDown() override {
    vm_destroy(&vm);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&alloc);
  }
};

/* 三个 kind 都能构造，布局与 sub 相同 */
TEST_F(OwnershipTypeTest, InternAndLayout) {
  const type_t *i32t = vm->type_i32;
  const type_t *own = type_ownership_intern(vm, TYPE_KIND_OWN, i32t);
  const type_t *ref = type_ownership_intern(vm, TYPE_KIND_REF, i32t);
  const type_t *fatal = type_ownership_intern(vm, TYPE_KIND_FATAL, i32t);
  ASSERT_NE(own, nullptr);
  ASSERT_NE(ref, nullptr);
  ASSERT_NE(fatal, nullptr);

  /* 是独立类型（三者互不相等，也不等于裸 i32） */
  EXPECT_NE(own, i32t);
  EXPECT_NE(own, ref);
  EXPECT_NE(ref, fatal);

  /* kind / 查询 */
  EXPECT_TRUE(type_is_own(own));
  EXPECT_TRUE(type_is_ref(ref));
  EXPECT_TRUE(type_is_fatal(fatal));
  EXPECT_FALSE(type_is_own(i32t));

  /* 布局 = sub（运行期等价） */
  EXPECT_EQ(own->size, i32t->size);
  EXPECT_EQ(own->align, i32t->align);
  EXPECT_EQ(own->size, ref->size);
  EXPECT_EQ(own->size, fatal->size);

  /* 类型名带修饰前缀 */
  EXPECT_EQ(std::string(own->name.ptr, own->name.len), "own i32");
  EXPECT_EQ(std::string(ref->name.ptr, ref->name.len), "ref i32");
  EXPECT_EQ(std::string(fatal->name.ptr, fatal->name.len), "fatal i32");

  /* kind 字符串（反射与类型层共用） */
  EXPECT_STREQ(sema_reflect_kind_name(own), "own");
  EXPECT_STREQ(sema_reflect_kind_name(ref), "ref");
  EXPECT_STREQ(sema_reflect_kind_name(fatal), "fatal");
}

/* 去重 intern：同 (kind, sub) 返回同一实例；不同 kind 不复用 */
TEST_F(OwnershipTypeTest, InternDedup) {
  const type_t *i32t = vm->type_i32;
  const type_t *a = type_ownership_intern(vm, TYPE_KIND_OWN, i32t);
  const type_t *b = type_ownership_intern(vm, TYPE_KIND_OWN, i32t);
  EXPECT_EQ(a, b);

  const type_t *r = type_ownership_intern(vm, TYPE_KIND_REF, i32t);
  EXPECT_NE(a, r);

  /* 不同 sub 不共用 */
  const type_t *o64 = type_ownership_intern(vm, TYPE_KIND_OWN, vm->type_i64);
  EXPECT_NE(a, o64);
}

/* 剥链：own/ref/fatal → sub；qualifier_sub 也能剥；ownership_of 穿透 */
TEST_F(OwnershipTypeTest, StripAndQuery) {
  const type_t *i32t = vm->type_i32;
  const type_t *own = type_ownership_intern(vm, TYPE_KIND_OWN, i32t);
  const type_t *ref = type_ownership_intern(vm, TYPE_KIND_REF, own); /* ref own i32 */

  EXPECT_EQ(type_strip_ownership(own), i32t);
  EXPECT_EQ(type_strip_ownership(i32t), i32t);
  /* 嵌套：ref (own i32) → i32（一路剥） */
  EXPECT_EQ(type_strip_ownership(ref), i32t);

  /* qualifier_sub 统一剥（const/volatile/own/ref/fatal） */
  EXPECT_EQ(type_qualifier_sub(own), i32t);
  EXPECT_EQ(type_qualifier_sub(ref), own);

  /* ownership_of 穿透 const/volatile 取第一个所有权修饰 */
  EXPECT_EQ(type_ownership_of(i32t), OWN_NONE);
  EXPECT_EQ(type_ownership_of(own), OWN_OWN);
  EXPECT_EQ(type_ownership_of(ref), OWN_REF);

  const type_t *ct = type_const_intern(vm, i32t);
  ASSERT_NE(ct, nullptr);
  EXPECT_EQ(type_ownership_of(ct), OWN_NONE); /* const 无所有权 */
  const type_t *ct_own = type_const_intern(vm, own);
  ASSERT_NE(ct_own, nullptr);
  EXPECT_EQ(type_ownership_of(ct_own), OWN_OWN); /* 穿透 const */
}

/* 运行期等价：type_equal 按身份（own i32 ≠ i32），但 extends 不做隐式兼容 */
TEST_F(OwnershipTypeTest, EqualityAndExtends) {
  const type_t *i32t = vm->type_i32;
  const type_t *own = type_ownership_intern(vm, TYPE_KIND_OWN, i32t);
  EXPECT_TRUE(type_equal(vm, own, own));
  EXPECT_FALSE(type_equal(vm, own, i32t));
  /* 所有权不隐式兼容到裸类型（转换由 sema 显式判） */
  EXPECT_FALSE(type_extends(vm, own, i32t));
}

} // namespace
