#include <gtest/gtest.h>
#include "test_common.h"

#include <stdexcept>

extern "C" {
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/type_error.h"
#include "vm/type_interrupt.h"
#include "vm/function.h"
#include "vm/type_func.h"
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

/* 按类型宽度读取 double */
static double read_float(const value_t *v) {
    if (value_type(v)->size == sizeof(float))
        return (double)*(const float *)value_data(v);
    return *(const double *)value_data(v);
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

/* 构造一个未 track 的 value_t*（堆分配，需手动释放） */
static value_t *make_i32_raw(vm_t *vm, int32_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i32, &v);
    return value_make_untracked(vm->alloc, vm->type_i32, data);
}
static value_t *make_i8_raw(vm_t *vm, int8_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i8, &v);
    return value_make_untracked(vm->alloc, vm->type_i8, data);
}
static value_t *make_u32_raw(vm_t *vm, uint32_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u32, &v);
    return value_make_untracked(vm->alloc, vm->type_u32, data);
}
static value_t *make_u64_raw(vm_t *vm, uint64_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u64, &v);
    return value_make_untracked(vm->alloc, vm->type_u64, data);
}
static value_t *make_f32_raw(vm_t *vm, float v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_f32, &v);
    return value_make_untracked(vm->alloc, vm->type_f32, data);
}
static value_t *make_f64_raw(vm_t *vm, double v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_f64, &v);
    return value_make_untracked(vm->alloc, vm->type_f64, data);
}
[[maybe_unused]] static value_t *make_bool_raw(vm_t *vm, bool v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &v);
    return value_make_untracked(vm->alloc, vm->type_bool, data);
}
static value_t *make_str_raw(vm_t *vm, const char *s) {
    string_t *str = string_from_cstr(vm->alloc, s);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_str, &str);
    return value_make_untracked(vm->alloc, vm->type_str, data);
}
[[maybe_unused]] static value_t *make_i16_raw(vm_t *vm, int16_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i16, &v);
    return value_make_untracked(vm->alloc, vm->type_i16, data);
}
[[maybe_unused]] static value_t *make_i64_raw(vm_t *vm, int64_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i64, &v);
    return value_make_untracked(vm->alloc, vm->type_i64, data);
}
[[maybe_unused]] static value_t *make_u8_raw(vm_t *vm, uint8_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u8, &v);
    return value_make_untracked(vm->alloc, vm->type_u8, data);
}
[[maybe_unused]] static value_t *make_u16_raw(vm_t *vm, uint16_t v) {
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u16, &v);
    return value_make_untracked(vm->alloc, vm->type_u16, data);
}

/* 释放未 track 的 raw value（dispose data + free struct） */
static void raw_free(vm_t *vm, value_t *v) {
    value_dispose(vm, v);
    allocator_free(vm->alloc, (void **)&v);
}

/* ================================================================ */
/* 1. VM 生命周期                                                    */
/* ================================================================ */

class VmLifecycle : public ::testing::Test {
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

TEST_F(VmLifecycle, CreateDestroy) {
    ASSERT_NE(vm, nullptr);
}

TEST_F(VmLifecycle, ScopeChainInitial) {
    /* vm_new 后: global -> root -> current(root) */
    EXPECT_EQ(vm->global_scope, vm->root_scope->parent);
    EXPECT_EQ(vm->current_scope, vm->root_scope);
}

TEST_F(VmLifecycle, PushPopScope) {
    vm_push_scope(vm);
    EXPECT_NE(vm->current_scope, vm->root_scope);
    EXPECT_EQ(scope_parent(vm->current_scope), vm->root_scope);

    vm_pop_scope(vm);
    EXPECT_EQ(vm->current_scope, vm->root_scope);
}

TEST_F(VmLifecycle, NestedScopes) {
    vm_push_scope(vm);
    scope_t *s1 = vm->current_scope;
    vm_push_scope(vm);
    scope_t *s2 = vm->current_scope;
    EXPECT_EQ(scope_parent(s2), s1);

    vm_pop_scope(vm);
    EXPECT_EQ(vm->current_scope, s1);
    vm_pop_scope(vm);
    EXPECT_EQ(vm->current_scope, vm->root_scope);
}

/* pop global scope 应 panic — panic 调用 abort()，用 EXPECT_DEATH 测试 */
TEST_F(VmLifecycle, PopGlobalPanics) {
    EXPECT_DEATH({
        vm->current_scope = vm->global_scope;
        vm_pop_scope(vm);
    }, "cannot pop the global scope");
}

/* ================================================================ */
/* 2. 内置类型注册表 (type_lookup：作用域链查找 type value)           */
/* ================================================================ */

class VmBuiltinTypes : public ::testing::Test {
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

TEST_F(VmBuiltinTypes, AllIntTypesRegistered) {
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("i8")),  vm->type_i8);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("i16")), vm->type_i16);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("i32")), vm->type_i32);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("i64")), vm->type_i64);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("u8")),  vm->type_u8);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("u16")), vm->type_u16);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("u32")), vm->type_u32);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("u64")), vm->type_u64);
}

TEST_F(VmBuiltinTypes, FloatTypesRegistered) {
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("f32")), vm->type_f32);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("f64")), vm->type_f64);
}

TEST_F(VmBuiltinTypes, OtherTypesRegistered) {
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("bool")), vm->type_bool);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("str")),  vm->type_str);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("void")), vm->type_void);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("type")), vm->type_type);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("func")), vm->type_func);
}

TEST_F(VmBuiltinTypes, UnknownTypeReturnsNull) {
    /* error 类型值未注册作用域（引擎内部类型，不可作为类型表达式引用） */
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("error")), nullptr);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("nonexistent")), nullptr);
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("")), nullptr);
}

TEST_F(VmBuiltinTypes, TypeSizeAndAlign) {
    EXPECT_EQ(vm->type_i8->size,   sizeof(int8_t));
    EXPECT_EQ(vm->type_i8->align,  alignof(int8_t));
    EXPECT_EQ(vm->type_i32->size,  sizeof(int32_t));
    EXPECT_EQ(vm->type_i32->align, alignof(int32_t));
    EXPECT_EQ(vm->type_i64->size,  sizeof(int64_t));
    EXPECT_EQ(vm->type_i64->align, alignof(int64_t));
    EXPECT_EQ(vm->type_u8->size,   sizeof(uint8_t));
    EXPECT_EQ(vm->type_u32->size,  sizeof(uint32_t));
    EXPECT_EQ(vm->type_u64->size,  sizeof(uint64_t));
    EXPECT_EQ(vm->type_f32->size,  sizeof(float));
    EXPECT_EQ(vm->type_f32->align, alignof(float));
    EXPECT_EQ(vm->type_f64->size,  sizeof(double));
    EXPECT_EQ(vm->type_f64->align, alignof(double));
    EXPECT_EQ(vm->type_bool->size, sizeof(bool));
    EXPECT_EQ(vm->type_void->size, 0u);
    EXPECT_EQ(vm->type_str->size,  sizeof(string_t *));
    EXPECT_EQ(vm->type_func->size, sizeof(func_t *));
}

TEST_F(VmBuiltinTypes, TypeEqIsPointerIdentity) {
    EXPECT_TRUE(type_eq(vm->type_i32, vm->type_i32));
    EXPECT_FALSE(type_eq(vm->type_i32, vm->type_i64));
}

TEST_F(VmBuiltinTypes, TypeAsValue) {
    value_t *tv = type_as_value(vm, vm->type_i32);
    EXPECT_EQ(value_type(tv), vm->type_type);
    EXPECT_EQ(*(const type_t **)value_data(tv), vm->type_i32);
    /* type_as_value auto-track 到 current_scope，vm_destroy 释放 */
}

/* ================================================================ */
/* 2.5 const/volatile 限定类型                                        */
/* ================================================================ */

class VmQualifiedTypes : public ::testing::Test {
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

TEST_F(VmQualifiedTypes, TypeKindClassification) {
    /* type_kind 粗粒度分类：基础类型按 kind 归类，不依赖实例指针 */
    EXPECT_EQ(vm->type_i32->kind, TYPE_KIND_INT);
    EXPECT_EQ(vm->type_u64->kind, TYPE_KIND_INT);
    EXPECT_EQ(vm->type_f32->kind, TYPE_KIND_FLOAT);
    EXPECT_EQ(vm->type_bool->kind, TYPE_KIND_BOOL);
    EXPECT_EQ(vm->type_str->kind, TYPE_KIND_STR);
    EXPECT_EQ(vm->type_void->kind, TYPE_KIND_VOID);
    EXPECT_EQ(vm->type_type->kind, TYPE_KIND_TYPE);
    EXPECT_EQ(vm->type_func->kind, TYPE_KIND_FUNC);
    EXPECT_EQ(vm->type_error->kind, TYPE_KIND_ERROR);

    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    const type_t *vi32 = type_volatile_intern(vm, vm->type_i32);
    EXPECT_EQ(ci32->kind, TYPE_KIND_CONST);
    EXPECT_EQ(vi32->kind, TYPE_KIND_VOLATILE);
    /* 鸭子类型：INT kind 覆盖全部整型宽度变体 */
    EXPECT_EQ(type_const_intern(vm, vm->type_i64)->kind, TYPE_KIND_CONST);
}

TEST_F(VmQualifiedTypes, ConstIsRealDistinctType) {
    /* const i32 是独立类型，与 i32 指针不相等（type identity 分离） */
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    ASSERT_NE(ci32, nullptr);
    EXPECT_NE(ci32, vm->type_i32);
    EXPECT_TRUE(type_is_const(ci32));
    EXPECT_FALSE(type_is_const(vm->type_i32));
}

TEST_F(VmQualifiedTypes, ConstInternDeduplicates) {
    const type_t *a = type_const_intern(vm, vm->type_i32);
    const type_t *b = type_const_intern(vm, vm->type_i32);
    EXPECT_EQ(a, b); /* 同 sub 指针去重 */
    const type_t *c = type_const_intern(vm, vm->type_i64);
    EXPECT_NE(a, c);
}

TEST_F(VmQualifiedTypes, VolatileInternDeduplicates) {
    const type_t *a = type_volatile_intern(vm, vm->type_i32);
    const type_t *b = type_volatile_intern(vm, vm->type_i32);
    EXPECT_EQ(a, b);
    EXPECT_TRUE(type_is_volatile(a));
    /* const 与 volatile 是不同限定，互不等价 */
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    EXPECT_NE(a, ci32);
}

TEST_F(VmQualifiedTypes, QualifierSubAndUnwrap) {
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    EXPECT_EQ(type_qualifier_sub(ci32), vm->type_i32);
    EXPECT_EQ(type_qualifier_sub(vm->type_i32), nullptr);

    /* 组合固定顺序 volatile(const(i32))：volatile 外层 */
    const type_t *vci = type_volatile_intern(vm, ci32);
    EXPECT_EQ(type_qualifier_sub(vci), ci32);
    EXPECT_EQ(type_qualifier_sub(type_qualifier_sub(vci)), vm->type_i32);
}

TEST_F(VmQualifiedTypes, ConstHasConstAlongChain) {
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    const type_t *vci = type_volatile_intern(vm, ci32);
    EXPECT_TRUE(type_has_const(ci32));
    EXPECT_TRUE(type_has_const(vci)); /* 沿 sub 链查到 const */
    EXPECT_FALSE(type_has_const(vm->type_i32));
    EXPECT_FALSE(type_has_const(type_volatile_intern(vm, vm->type_i32)));
}

TEST_F(VmQualifiedTypes, QualifiedTypeSizeAlignMatchSub) {
    /* 限定类型不改变布局：size/align 与子类型一致 */
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    EXPECT_EQ(ci32->size, vm->type_i32->size);
    EXPECT_EQ(ci32->align, vm->type_i32->align);
    const type_t *vi64 = type_volatile_intern(vm, vm->type_i64);
    EXPECT_EQ(vi64->size, vm->type_i64->size);
    EXPECT_EQ(vi64->align, vm->type_i64->align);
}

TEST_F(VmQualifiedTypes, ValueKindAndIsType) {
    /* value 层类型查询：kind 分类 + is_type 判定（type is value 入口） */
    value_t *iv = make_i32_raw(vm, 42);
    EXPECT_EQ(value_kind(iv), TYPE_KIND_INT);
    EXPECT_TRUE(value_is_type(iv, TYPE_KIND_INT));
    EXPECT_FALSE(value_is_type(iv, TYPE_KIND_FLOAT));
    EXPECT_FALSE(value_is_type(iv, TYPE_KIND_CONST));

    value_t *bv = make_bool_raw(vm, true);
    EXPECT_EQ(value_kind(bv), TYPE_KIND_BOOL);
    EXPECT_TRUE(value_is_type(bv, TYPE_KIND_BOOL));

    /* NULL type value → VOID kind（value_is_void 语义：无类型） */
    value_t *voidv = value_make_untracked(vm->alloc, NULL, NULL);
    EXPECT_EQ(value_kind(voidv), TYPE_KIND_VOID);
    EXPECT_TRUE(value_is_type(voidv, TYPE_KIND_VOID));

    raw_free(vm, iv);
    raw_free(vm, bv);
    raw_free(vm, voidv);
}

TEST_F(VmQualifiedTypes, ValueHasConst) {
    /* value 层 const 查询：沿 sub 链递归（type is value 接口） */
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    const type_t *vci = type_volatile_intern(vm, ci32);

    value_t *plain = value_make_untracked(vm->alloc, vm->type_i32, NULL);
    value_t *ci = value_make_untracked(vm->alloc, ci32, NULL);
    value_t *vci_v = value_make_untracked(vm->alloc, vci, NULL);
    value_t *vi = value_make_untracked(
        vm->alloc, type_volatile_intern(vm, vm->type_i32), NULL);

    EXPECT_FALSE(value_has_const(plain));
    EXPECT_TRUE(value_has_const(ci));
    EXPECT_TRUE(value_has_const(vci_v));  /* volatile(const(i32)) → 查到 const */
    EXPECT_FALSE(value_has_const(vi));    /* volatile(i32) → 无 const */

    allocator_free(vm->alloc, (void **)&plain);
    allocator_free(vm->alloc, (void **)&ci);
    allocator_free(vm->alloc, (void **)&vci_v);
    allocator_free(vm->alloc, (void **)&vi);
}

TEST_F(VmQualifiedTypes, ImplicitCastQualifiesScalar) {
    /* 加限定符身份转换：i32 → volatile i32 / const i32 /
       const volatile i32（限定符不改变底层表示，身份拷贝非拓宽）。
       入口经标量 vtable implicit_cast（sint/uint/float），
       value_implicit_qualify 处理限定符 sub 链剥到源类型。
       转换结果 auto-track 到当前 scope，fixture 清理，不手动释放。 */
    value_t *v = make_i32_raw(vm, 42);

    const type_t *vi32 = type_volatile_intern(vm, vm->type_i32);
    value_t *qv = value_implicit_cast(vm, v, vi32);
    ASSERT_FALSE(value_is_error(vm, qv));
    EXPECT_EQ(value_type(qv), vi32);
    EXPECT_EQ(*(int32_t *)value_data(qv), 42);

    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    value_t *qc = value_implicit_cast(vm, v, ci32);
    ASSERT_FALSE(value_is_error(vm, qc));
    EXPECT_EQ(value_type(qc), ci32);
    EXPECT_EQ(*(int32_t *)value_data(qc), 42);

    /* 组合：i32 → const volatile i32（固定顺序 volatile(const(i32))） */
    const type_t *vci = type_volatile_intern(vm, ci32);
    value_t *qvc = value_implicit_cast(vm, v, vci);
    ASSERT_FALSE(value_is_error(vm, qvc));
    EXPECT_EQ(value_type(qvc), vci);
    EXPECT_EQ(*(int32_t *)value_data(qvc), 42);

    raw_free(vm, v);
}

TEST_F(VmQualifiedTypes, ImplicitCastQualifyRejectsWrongTarget) {
    /* 目标限定符 sub 链剥不到源类型 → 拒绝（如 i32 → volatile i64） */
    value_t *v = make_i32_raw(vm, 42);
    const type_t *vi64 = type_volatile_intern(vm, vm->type_i64);
    value_t *r = value_implicit_cast(vm, v, vi64);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(VmQualifiedTypes, ImplicitCastQualifyShadow) {
    /* shadow 值加限定符：只检查类型兼容性返回 shadow（sema 类型检查）。
       shadow 值 auto-track 到 scope，fixture 清理，不手动释放。 */
    value_t *sv = value_make_shadow(vm, vm->type_i32);
    const type_t *vi32 = type_volatile_intern(vm, vm->type_i32);
    value_t *r = value_implicit_cast(vm, sv, vi32);
    ASSERT_FALSE(value_is_error(vm, r));
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vi32);
}

TEST_F(VmQualifiedTypes, TypeEqualDuckDispatch) {
    /* type value 的 == 经 type_equal 分派：
       const i32 == const i32 → true；const i32 == i32 → false（类型身份分离） */
    const type_t *a = type_const_intern(vm, vm->type_i32);
    const type_t *b = type_const_intern(vm, vm->type_i32);
    EXPECT_TRUE(type_equal(vm, a, b));
    EXPECT_FALSE(type_equal(vm, a, vm->type_i32));
    EXPECT_FALSE(type_equal(vm, vm->type_i32, a));
    EXPECT_TRUE(type_equal(vm, vm->type_i32, vm->type_i32));
}

TEST_F(VmQualifiedTypes, ConstTypeExtendsUnwraps) {
    /* const T extends U = T extends U（复制语义：const 值可赋给非 const 变量）。
       反向 i32 extends const i32 不成立（无 type_extends 槽位的类型默认严格相等） */
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    EXPECT_TRUE(type_extends(vm, ci32, vm->type_i32));  /* const i32 可赋给 i32 */
    EXPECT_FALSE(type_extends(vm, vm->type_i32, ci32)); /* 反向不成立 */
    EXPECT_FALSE(type_extends(vm, ci32, vm->type_i64));
}

TEST_F(VmQualifiedTypes, VolatileTypeExtendsUnwraps) {
    const type_t *vi32 = type_volatile_intern(vm, vm->type_i32);
    EXPECT_TRUE(type_extends(vm, vi32, vm->type_i32));
    EXPECT_FALSE(type_extends(vm, vi32, vm->type_i64));
}

TEST_F(VmQualifiedTypes, CombinedTypeExtendsToPlain) {
    /* volatile(const(i32)) extends i32 → 沿链解包 true */
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    const type_t *vci = type_volatile_intern(vm, ci32);
    EXPECT_TRUE(type_extends(vm, vci, vm->type_i32));
    EXPECT_FALSE(type_extends(vm, vci, vm->type_i64));
}

TEST_F(VmQualifiedTypes, ConstValueArithmeticProxies) {
    /* const/volatile 值的运算代理到子类型 vtable（解包代理） */
    const type_t *ci32 = type_const_intern(vm, vm->type_i32);
    int32_t lhs = 3, rhs = 4;
    void *ld = value_alloc_data_copy(vm->alloc, ci32, &lhs);
    value_t *lv = value_make_untracked(vm->alloc, ci32, ld);
    void *rd = value_alloc_data_copy(vm->alloc, vm->type_i32, &rhs);
    value_t *rv = value_make_untracked(vm->alloc, vm->type_i32, rd);

    value_t *sum = value_add(vm, lv, rv);
    ASSERT_NE(sum, nullptr);
    EXPECT_EQ(value_type(sum), vm->type_i32);
    EXPECT_EQ(read_sint(sum), 7);

    /* sum auto-track 到 current_scope，vm_destroy 释放 */
    raw_free(vm, lv);
    raw_free(vm, rv);
}

TEST_F(VmQualifiedTypes, VolatileValueAssignProxies) {
    /* volatile 值赋值（volatile 是存取提示，不影响赋值语义）。
       int_assign 返回 dst 自身（非新值），raw_free(res) 即释放 dst */
    const type_t *vi32 = type_volatile_intern(vm, vm->type_i32);
    int32_t src = 42;
    void *sd = value_alloc_data_copy(vm->alloc, vm->type_i32, &src);
    value_t *sv = value_make_untracked(vm->alloc, vm->type_i32, sd);
    void *dd = value_alloc_data_copy(vm->alloc, vi32, &src);
    value_t *dv = value_make_untracked(vm->alloc, vi32, dd);

    value_t *res = value_assign(vm, dv, sv);
    ASSERT_NE(res, nullptr);
    EXPECT_EQ(res, dv); /* assign 返回 dst 自身 */
    EXPECT_EQ(read_sint(dv), 42);

    raw_free(vm, dv);
    raw_free(vm, sv);
}


/* ================================================================ */
/* 3. Value 核心机制                                                 */
/* ================================================================ */

class ValueCore : public ::testing::Test {
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

/* ---- value_make / value_is_void ---- */

TEST_F(ValueCore, ValueMakeBasic) {
    value_t *v = make_i32_raw(vm, 42);
    EXPECT_EQ(value_type(v), vm->type_i32);
    EXPECT_NE(value_data(v), nullptr);
    EXPECT_EQ(read_sint(v), 42);
    raw_free(vm, v);
}

TEST_F(ValueCore, ValueIsVoid) {
    value_t *v = NULL;
    EXPECT_TRUE(value_is_void(v));

    value_t *vi = make_i32_raw(vm, 1);
    EXPECT_FALSE(value_is_void(vi));
    raw_free(vm, vi);
}

/* ---- value_clone auto-track ---- */

TEST_F(ValueCore, CloneAutoTracksToCurrentScope) {
    value_t *raw = make_i32_raw(vm, 100);
    value_t *cloned = value_clone(vm, raw);

    EXPECT_EQ(value_type(cloned), vm->type_i32);
    EXPECT_EQ(read_sint(cloned), 100);

    /* clone 后应自动 track 到 current_scope->owned */
    /* pop_scope 时应正确销毁 cloned，不泄漏 */
    vm_push_scope(vm);
    value_t *cloned2 = value_clone(vm, raw);
    EXPECT_EQ(read_sint(cloned2), 100);
    vm_pop_scope(vm); /* 销毁 cloned2 */

    /* 释放 raw */
    raw_free(vm, raw);
}

TEST_F(ValueCore, CloneProducesIndependentCopy) {
    value_t *raw = make_i32_raw(vm, 7);
    value_t *cloned = value_clone(vm, raw);

    /* 修改 cloned 的 data 不影响 raw（独立内存） */
    *(int32_t *)value_data(cloned) = 999;
    EXPECT_EQ(read_sint(raw), 7);
    EXPECT_EQ(read_sint(cloned), 999);

    raw_free(vm, raw);
    /* cloned 被 track 到 root_scope，vm_destroy 时释放 */
}

TEST_F(ValueCore, CloneVoidReturnsNull) {
    value_t *v = NULL;
    value_t *cloned = value_clone(vm, v);
    EXPECT_EQ(cloned, nullptr);
}

/* ---- value_dispose ---- */

TEST_F(ValueCore, DisposeNullIsSafe) {
    value_dispose(vm, nullptr); /* 不应崩溃 */

    value_t *v = value_make_untracked(vm->alloc, nullptr, nullptr);
    value_dispose(vm, v); /* type==NULL，no-op on data */
    allocator_free(vm->alloc, (void **)&v);
}

TEST_F(ValueCore, DisposeSetsTypeNull) {
    value_t *v = make_i32_raw(vm, 42);
    value_dispose(vm, v);
    EXPECT_EQ(value_type(v), nullptr);
    EXPECT_EQ(value_data(v), nullptr);
    allocator_free(vm->alloc, (void **)&v);
}

/* ---- error 机制 ---- */

TEST_F(ValueCore, MakeErrorIsTracked) {
    value_t *err = value_make_error(vm, "test error");
    EXPECT_EQ(value_type(err), vm->type_error);
    EXPECT_TRUE(value_is_error(vm, err));

    error_data_t *ed = (error_data_t *)value_data(err);
    EXPECT_STREQ(string_cstr(ed->message), "test error");
    EXPECT_EQ(ed->location, nullptr);

    /* error auto-track 到 current_scope，vm_destroy 释放 */
}

TEST_F(ValueCore, MakeErrorWithLocation) {
    value_t *err = value_make_error_loc(vm, "boom", "file.clux:10");
    EXPECT_TRUE(value_is_error(vm, err));

    error_data_t *ed = (error_data_t *)value_data(err);
    EXPECT_STREQ(string_cstr(ed->message), "boom");
    EXPECT_STREQ(string_cstr(ed->location), "file.clux:10");
}

TEST_F(ValueCore, NonErrorIsNotError) {
    value_t *v = make_i32_raw(vm, 1);
    EXPECT_FALSE(value_is_error(vm, v));
    raw_free(vm, v);

    value_t *err = value_make_error(vm, "e");
    EXPECT_TRUE(value_is_error(vm, err));
}

/* ---- 运算分派：error 短路 ---- */

TEST_F(ValueCore, ErrorShortCircuitsAdd) {
    value_t *err = value_make_error(vm, "err");
    value_t *v   = make_i32_raw(vm, 1);

    /* a 是 error → 直接返回 a */
    value_t *r1 = value_add(vm, err, v);
    EXPECT_TRUE(value_is_error(vm, r1));

    /* b 是 error → 直接返回 b */
    value_t *r2 = value_add(vm, v, err);
    EXPECT_TRUE(value_is_error(vm, r2));

    raw_free(vm, v);
    /* err, r1, r2 都 track 到 current_scope */
}

/* ---- 运算分派：类型不支持 → error ---- */

TEST_F(ValueCore, UnsupportedOperatorReturnsError) {
    value_t *s = make_str_raw(vm, "hello");
    value_t *s2 = make_str_raw(vm, "world");

    /* str 不支持 add */
    value_t *r = value_add(vm, s, s2);
    EXPECT_TRUE(value_is_error(vm, r));

    /* str 不支持 neg */
    value_t *r2 = value_neg(vm, s);
    EXPECT_TRUE(value_is_error(vm, r2));

    raw_free(vm, s);
    raw_free(vm, s2);
}

/* ---- 运算分派：正常路径 ---- */

TEST_F(ValueCore, IntAdditionDispatch) {
    value_t *a = make_i32_raw(vm, 10);
    value_t *b = make_i32_raw(vm, 32);

    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 42);

    raw_free(vm, a);
    raw_free(vm, b);
    /* r auto-track 到 current_scope，vm_destroy 释放 */
}

TEST_F(ValueCore, IntComparisonReturnsBool) {
    value_t *a = make_i32_raw(vm, 5);
    value_t *b = make_i32_raw(vm, 10);

    value_t *r = value_lt(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));

    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 跨类型运算：类型协商 + 隐式转换 ---- */

TEST_F(ValueCore, PromoteI32PlusU32ReturnsError) {
    value_t *a = make_i32_raw(vm, -1);
    value_t *b = make_u32_raw(vm, 1);

    /* 有符号和无符号之间不允许隐式转换 */
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, PromoteI32PlusF64ReturnsError) {
    value_t *a = make_i32_raw(vm, 10);
    value_t *b = make_f64_raw(vm, 32.5);

    /* int 和 float 之间不允许隐式转换 */
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, PromoteF32PlusF64ResultsF64) {
    value_t *a = make_f32_raw(vm, 1.5f);
    value_t *b = make_f64_raw(vm, 2.5);

    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 4.0);

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, PromoteI8PlusU64ReturnsError) {
    value_t *a = make_i8_raw(vm, 5);
    value_t *b = make_u64_raw(vm, 1000);

    /* 有符号和无符号之间不允许隐式转换 */
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, PromoteI32LtU32ReturnsError) {
    value_t *a = make_i32_raw(vm, -1);
    value_t *b = make_u32_raw(vm, 1);

    /* 有符号和无符号之间不允许隐式转换 */
    value_t *r = value_lt(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, PromoteIncompatibleTypesReturnsError) {
    value_t *a = make_i32_raw(vm, 1);
    value_t *b = make_str_raw(vm, "hello");

    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, StrPlusIntReturnsError) {
    value_t *a = make_str_raw(vm, "hello");
    value_t *b = make_i32_raw(vm, 42);

    /* "str" + 42: str vtable 的 add 不存在 → error */
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, IntEqStrReturnsError) {
    value_t *a = make_i32_raw(vm, 1);
    value_t *b = make_str_raw(vm, "1");

    /* 1 == "1": int_eq 尝试将 "str" implicit_cast 到 i32 → 失败 → error */
    value_t *r = value_eq(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BoolEqStrReturnsError) {
    value_t *a = make_bool_raw(vm, true);
    value_t *b = make_str_raw(vm, "true");

    /* true == "true": bool_eq 尝试将 "str" implicit_cast 到 bool → 失败 → error */
    value_t *r = value_eq(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, StrEqStrSameType) {
    value_t *a = make_str_raw(vm, "hello");
    value_t *b = make_str_raw(vm, "hello");

    value_t *r = value_eq(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));

    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- value_call 分派 ---- */

TEST_F(ValueCore, CallNonCallableReturnsError) {
    value_t *a = make_i32_raw(vm, 1);
    value_t *r = value_call(vm, a, nullptr, 0);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
}

TEST_F(ValueCore, CallWithErrorArgPropagates) {
    value_t *err = value_make_error(vm, "arg error");
    value_t *a = make_i32_raw(vm, 1);

    value_t *args[] = { err, a };
    value_t *r = value_call(vm, a, args, 2);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
}

/* ---- 可变参数函数 (FFI variadic) ---- */

static value_t *sum_variadic(vm_t *vm, func_t *self, size_t argc, value_t **args) {
    (void)self;
    int64_t total = 0;
    for (size_t i = 0; i < argc; i++) {
        total += read_sint(args[i]);
    }
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i64, &total);
    return value_make(vm, vm->type_i64, data);
}

TEST_F(ValueCore, VariadicFuncAcceptsExtraArgs) {
    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_i64, true);
    /* func value：data 存 func_t*，type 即签名类型；untracked，手动释放 */
    value_t *fv = func_new(vm, sum_variadic, vm->global_scope,
                           vm->root_scope, sig, STRSLICE_LIT("sum_variadic"));
    ASSERT_NE(fv, nullptr);
    EXPECT_EQ(value_type(fv), sig);

    value_t *a = make_i32_raw(vm, 1);
    value_t *b = make_i32_raw(vm, 2);
    value_t *c = make_i32_raw(vm, 3);
    value_t *args[] = { a, b, c };
    value_t *r = value_call(vm, fv, args, 3);

    ASSERT_NE(r, nullptr);
    ASSERT_EQ(value_type(r), vm->type_i64);
    EXPECT_EQ(read_sint(r), 6);

    raw_free(vm, a);
    raw_free(vm, b);
    raw_free(vm, c);
    raw_free(vm, fv);
}

TEST_F(ValueCore, NonVariadicFuncRejectsExtraArgs) {
    /* 固定参数函数：1 个 i32 参数 */
    const type_t *params[1] = { NULL };  /* 无类型约束，但 param_count=1 */
    const type_t *sig = type_func_sig(vm, params, 1, vm->type_void, false);
    /* func value：data 存 func_t*，type 即签名类型；untracked，手动释放 */
    value_t *fv = func_new(vm, sum_variadic, vm->global_scope,
                           vm->root_scope, sig, STRSLICE_LIT("fixed"));
    ASSERT_NE(fv, nullptr);
    EXPECT_EQ(value_type(fv), sig);

    value_t *a = make_i32_raw(vm, 1);
    value_t *b = make_i32_raw(vm, 2);
    value_t *args[] = { a, b };
    value_t *r = value_call(vm, fv, args, 2);

    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, a);
    raw_free(vm, b);
    raw_free(vm, fv);
}

/* ---- value_assign 分派 ---- */

TEST_F(ValueCore, AssignSameTypeMemcpy) {
    value_t *dst = make_i32_raw(vm, 100);
    value_t *src = make_i32_raw(vm, 42);
    value_t *r = value_assign(vm, dst, src);
    ASSERT_EQ(r, dst);
    EXPECT_EQ(read_sint(dst), 42);
    EXPECT_EQ(read_sint(src), 42);

    raw_free(vm, dst);
    raw_free(vm, src);
}

TEST_F(ValueCore, AssignWideningCast) {
    value_t *dst = make_i64_raw(vm, 0);
    value_t *src = make_i32_raw(vm, -7);
    value_t *r = value_assign(vm, dst, src);
    ASSERT_EQ(r, dst);
    EXPECT_EQ(read_sint(dst), -7);

    raw_free(vm, dst);
    raw_free(vm, src);
}

TEST_F(ValueCore, AssignBoolSameType) {
    value_t *dst = make_bool_raw(vm, false);
    value_t *src = make_bool_raw(vm, true);
    value_t *r = value_assign(vm, dst, src);
    ASSERT_EQ(r, dst);
    EXPECT_TRUE(value_as(dst, bool));

    raw_free(vm, dst);
    raw_free(vm, src);
}

TEST_F(ValueCore, AssignBoolTypeMismatchReturnsError) {
    value_t *dst = make_bool_raw(vm, false);
    value_t *src = make_i32_raw(vm, 1);
    value_t *r = value_assign(vm, dst, src);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, dst);
    raw_free(vm, src);
}

TEST_F(ValueCore, AssignStrDeepCopy) {
    string_t *s1 = string_from_cstr(vm->alloc, "hello");
    void *d1 = value_alloc_data_copy(vm->alloc, vm->type_str, &s1);
    value_t *dst = value_make_untracked(vm->alloc, vm->type_str, d1);

    string_t *s2 = string_from_cstr(vm->alloc, "world");
    void *d2 = value_alloc_data_copy(vm->alloc, vm->type_str, &s2);
    value_t *src = value_make_untracked(vm->alloc, vm->type_str, d2);

    value_t *r = value_assign(vm, dst, src);
    ASSERT_EQ(r, dst);
    EXPECT_STREQ(string_cstr(*(string_t **)value_data(dst)), "world");
    /* src 不变 */
    EXPECT_STREQ(string_cstr(*(string_t **)value_data(src)), "world");

    raw_free(vm, dst);
    raw_free(vm, src);
}

TEST_F(ValueCore, AssignToVoidReturnsError) {
    value_t *src = make_i32_raw(vm, 42);
    value_t *r = value_assign(vm, NULL, src);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, src);
}

TEST_F(ValueCore, AssignErrorPropagates) {
    value_t *err = value_make_error(vm, "err");
    value_t *dst = make_i32_raw(vm, 1);
    value_t *r = value_assign(vm, dst, err);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, dst);
}

/* ---- 类型转换分派 ---- */

TEST_F(ValueCore, ImplicitCastSameTypeClones) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 42);
    /* clone 到 current_scope */

    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastUnsupportedReturnsError) {
    value_t *v = make_i32_raw(vm, 42);
    /* int 没有 implicit_cast vtable 回调 */
    value_t *r = value_implicit_cast(vm, v, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastUnsupportedReturnsError) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_explicit_cast(vm, v, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));

    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastVoidReturnsError) {
    value_t *v = NULL;
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
}

TEST_F(ValueCore, ImplicitCastErrorPropagates) {
    value_t *err = value_make_error(vm, "err");
    value_t *r = value_implicit_cast(vm, err, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ================================================================ */
/* 3a. 隐式类型转换 (ImplicitCast)                                   */
/* ================================================================ */

/* ---- 有符号整数拓宽 ---- */

TEST_F(ValueCore, ImplicitCastI8ToI16) {
    value_t *v = make_i8_raw(vm, -42);
    value_t *r = value_implicit_cast(vm, v, vm->type_i16);
    EXPECT_EQ(value_type(r), vm->type_i16);
    EXPECT_EQ(read_sint(r), -42);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI8ToI32) {
    value_t *v = make_i8_raw(vm, 100);
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 100);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI8ToI64) {
    value_t *v = make_i8_raw(vm, -1);
    value_t *r = value_implicit_cast(vm, v, vm->type_i64);
    EXPECT_EQ(value_type(r), vm->type_i64);
    EXPECT_EQ(read_sint(r), -1);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI16ToI32) {
    value_t *v = make_i16_raw(vm, -1000);
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), -1000);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI16ToI64) {
    value_t *v = make_i16_raw(vm, 30000);
    value_t *r = value_implicit_cast(vm, v, vm->type_i64);
    EXPECT_EQ(value_type(r), vm->type_i64);
    EXPECT_EQ(read_sint(r), 30000);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI32ToI64) {
    value_t *v = make_i32_raw(vm, -100000);
    value_t *r = value_implicit_cast(vm, v, vm->type_i64);
    EXPECT_EQ(value_type(r), vm->type_i64);
    EXPECT_EQ(read_sint(r), -100000);
    raw_free(vm, v);
}

/* ---- 无符号整数拓宽 ---- */

TEST_F(ValueCore, ImplicitCastU8ToU16) {
    value_t *v = make_u8_raw(vm, 200);
    value_t *r = value_implicit_cast(vm, v, vm->type_u16);
    EXPECT_EQ(value_type(r), vm->type_u16);
    EXPECT_EQ(read_uint(r), 200u);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU8ToU32) {
    value_t *v = make_u8_raw(vm, 255);
    value_t *r = value_implicit_cast(vm, v, vm->type_u32);
    EXPECT_EQ(value_type(r), vm->type_u32);
    EXPECT_EQ(read_uint(r), 255u);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU8ToU64) {
    value_t *v = make_u8_raw(vm, 0xFF);
    value_t *r = value_implicit_cast(vm, v, vm->type_u64);
    EXPECT_EQ(value_type(r), vm->type_u64);
    EXPECT_EQ(read_uint(r), 0xFFu);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU16ToU32) {
    value_t *v = make_u16_raw(vm, 60000);
    value_t *r = value_implicit_cast(vm, v, vm->type_u32);
    EXPECT_EQ(value_type(r), vm->type_u32);
    EXPECT_EQ(read_uint(r), 60000u);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU16ToU64) {
    value_t *v = make_u16_raw(vm, 50000);
    value_t *r = value_implicit_cast(vm, v, vm->type_u64);
    EXPECT_EQ(value_type(r), vm->type_u64);
    EXPECT_EQ(read_uint(r), 50000u);
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU32ToU64) {
    value_t *v = make_u32_raw(vm, 4000000000u);
    value_t *r = value_implicit_cast(vm, v, vm->type_u64);
    EXPECT_EQ(value_type(r), vm->type_u64);
    EXPECT_EQ(read_uint(r), 4000000000ull);
    raw_free(vm, v);
}

/* ---- 浮点拓宽 ---- */

TEST_F(ValueCore, ImplicitCastF32ToF64) {
    value_t *v = make_f32_raw(vm, 3.14f);
    value_t *r = value_implicit_cast(vm, v, vm->type_f64);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_NEAR(read_float(r), 3.14, 1e-6);
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：有符号→无符号 ---- */

TEST_F(ValueCore, ImplicitCastI32ToU32ReturnsError) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_u32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI64ToU64ReturnsError) {
    value_t *v = make_i64_raw(vm, 100);
    value_t *r = value_implicit_cast(vm, v, vm->type_u64);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI8ToU8ReturnsError) {
    value_t *v = make_i8_raw(vm, 1);
    value_t *r = value_implicit_cast(vm, v, vm->type_u8);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：无符号→有符号 ---- */

TEST_F(ValueCore, ImplicitCastU32ToI32ReturnsError) {
    value_t *v = make_u32_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU8ToI16ReturnsError) {
    value_t *v = make_u8_raw(vm, 200);
    value_t *r = value_implicit_cast(vm, v, vm->type_i16);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：int→float ---- */

TEST_F(ValueCore, ImplicitCastI32ToF64ReturnsError) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_f64);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU32ToF32ReturnsError) {
    value_t *v = make_u32_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_f32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：float→int ---- */

TEST_F(ValueCore, ImplicitCastF64ToI32ReturnsError) {
    value_t *v = make_f64_raw(vm, 3.14);
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：float 窄化 ---- */

TEST_F(ValueCore, ImplicitCastF64ToF32ReturnsError) {
    value_t *v = make_f64_raw(vm, 3.14);
    value_t *r = value_implicit_cast(vm, v, vm->type_f32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：整数窄化 ---- */

TEST_F(ValueCore, ImplicitCastI32ToI8ReturnsError) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_i8);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastU64ToU32ReturnsError) {
    value_t *v = make_u64_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_u32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI64ToI16ReturnsError) {
    value_t *v = make_i64_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_i16);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：bool ---- */

TEST_F(ValueCore, ImplicitCastBoolToI32ReturnsError) {
    value_t *v = make_bool_raw(vm, true);
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI32ToBoolReturnsError) {
    value_t *v = make_i32_raw(vm, 1);
    value_t *r = value_implicit_cast(vm, v, vm->type_bool);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 隐式转换禁止场景：str ---- */

TEST_F(ValueCore, ImplicitCastStrToI32ReturnsError) {
    value_t *v = make_str_raw(vm, "hello");
    value_t *r = value_implicit_cast(vm, v, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ImplicitCastI32ToStrReturnsError) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_implicit_cast(vm, v, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ================================================================ */
/* 3b. 显式类型转换 (ExplicitCast)                                   */
/* ================================================================ */

/* ---- 整数互转：有符号→有符号（含窄化截断） ---- */

TEST_F(ValueCore, ExplicitCastI32ToI8Truncates) {
    value_t *v = make_i32_raw(vm, 300);
    value_t *r = value_explicit_cast(vm, v, vm->type_i8);
    EXPECT_EQ(value_type(r), vm->type_i8);
    EXPECT_EQ(read_sint(r), (int8_t)300);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastI64ToI16Truncates) {
    value_t *v = make_i64_raw(vm, 70000);
    value_t *r = value_explicit_cast(vm, v, vm->type_i16);
    EXPECT_EQ(value_type(r), vm->type_i16);
    EXPECT_EQ(read_sint(r), (int16_t)70000);
    raw_free(vm, v);
}

/* ---- 整数互转：有符号→无符号 ---- */

TEST_F(ValueCore, ExplicitCastI32ToU32) {
    value_t *v = make_i32_raw(vm, -1);
    value_t *r = value_explicit_cast(vm, v, vm->type_u32);
    EXPECT_EQ(value_type(r), vm->type_u32);
    EXPECT_EQ(read_uint(r), (uint32_t)(-1));
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastI8ToU64) {
    value_t *v = make_i8_raw(vm, -1);
    value_t *r = value_explicit_cast(vm, v, vm->type_u64);
    EXPECT_EQ(value_type(r), vm->type_u64);
    EXPECT_EQ(read_uint(r), (uint64_t)(int64_t)(-1));
    raw_free(vm, v);
}

/* ---- 整数互转：无符号→有符号 ---- */

TEST_F(ValueCore, ExplicitCastU32ToI32) {
    value_t *v = make_u32_raw(vm, 0xFFFFFFFFu);
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), (int32_t)0xFFFFFFFFu);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastU8ToI16) {
    value_t *v = make_u8_raw(vm, 200);
    value_t *r = value_explicit_cast(vm, v, vm->type_i16);
    EXPECT_EQ(value_type(r), vm->type_i16);
    EXPECT_EQ(read_sint(r), 200);
    raw_free(vm, v);
}

/* ---- 整数互转：无符号→无符号（含窄化） ---- */

TEST_F(ValueCore, ExplicitCastU64ToU8Truncates) {
    value_t *v = make_u64_raw(vm, 0x1FF);
    value_t *r = value_explicit_cast(vm, v, vm->type_u8);
    EXPECT_EQ(value_type(r), vm->type_u8);
    EXPECT_EQ(read_uint(r), 0xFFu);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastU32ToU16Truncates) {
    value_t *v = make_u32_raw(vm, 0x1FFFF);
    value_t *r = value_explicit_cast(vm, v, vm->type_u16);
    EXPECT_EQ(value_type(r), vm->type_u16);
    EXPECT_EQ(read_uint(r), 0xFFFFu);
    raw_free(vm, v);
}

/* ---- int→float ---- */

TEST_F(ValueCore, ExplicitCastI32ToF64) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_explicit_cast(vm, v, vm->type_f64);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 42.0);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastI32ToF32) {
    value_t *v = make_i32_raw(vm, -7);
    value_t *r = value_explicit_cast(vm, v, vm->type_f32);
    EXPECT_EQ(value_type(r), vm->type_f32);
    EXPECT_FLOAT_EQ((float)read_float(r), -7.0f);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastI64ToF64) {
    value_t *v = make_i64_raw(vm, 9007199254740992LL);
    value_t *r = value_explicit_cast(vm, v, vm->type_f64);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 9007199254740992.0);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastU64ToF64) {
    value_t *v = make_u64_raw(vm, 18014398509481984ull);
    value_t *r = value_explicit_cast(vm, v, vm->type_f64);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 18014398509481984.0);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastU8ToF32) {
    value_t *v = make_u8_raw(vm, 255);
    value_t *r = value_explicit_cast(vm, v, vm->type_f32);
    EXPECT_EQ(value_type(r), vm->type_f32);
    EXPECT_FLOAT_EQ((float)read_float(r), 255.0f);
    raw_free(vm, v);
}

/* ---- float→int（向零截断） ---- */

TEST_F(ValueCore, ExplicitCastF64ToI32Truncates) {
    value_t *v = make_f64_raw(vm, 3.9);
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 3);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastF64ToI32NegativeTruncates) {
    value_t *v = make_f64_raw(vm, -3.9);
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), -3);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastF32ToI64) {
    value_t *v = make_f32_raw(vm, 1234.5f);
    value_t *r = value_explicit_cast(vm, v, vm->type_i64);
    EXPECT_EQ(value_type(r), vm->type_i64);
    EXPECT_EQ(read_sint(r), 1234);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastF64ToU32) {
    value_t *v = make_f64_raw(vm, 42.7);
    value_t *r = value_explicit_cast(vm, v, vm->type_u32);
    EXPECT_EQ(value_type(r), vm->type_u32);
    EXPECT_EQ(read_uint(r), 42u);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastF64ToU8Truncates) {
    value_t *v = make_f64_raw(vm, 300.9);
    value_t *r = value_explicit_cast(vm, v, vm->type_u8);
    EXPECT_EQ(value_type(r), vm->type_u8);
    EXPECT_EQ(read_uint(r), (uint8_t)300);
    raw_free(vm, v);
}

/* ---- float→float 窄化 ---- */

TEST_F(ValueCore, ExplicitCastF64ToF32) {
    value_t *v = make_f64_raw(vm, 3.141592653589793);
    value_t *r = value_explicit_cast(vm, v, vm->type_f32);
    EXPECT_EQ(value_type(r), vm->type_f32);
    EXPECT_FLOAT_EQ((float)read_float(r), 3.141592653589793f);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastF32ToF64) {
    value_t *v = make_f32_raw(vm, 1.5f);
    value_t *r = value_explicit_cast(vm, v, vm->type_f64);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 1.5);
    raw_free(vm, v);
}

/* ---- int→bool ---- */

TEST_F(ValueCore, ExplicitCastI32ToBoolTrue) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_explicit_cast(vm, v, vm->type_bool);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastI32ToBoolFalse) {
    value_t *v = make_i32_raw(vm, 0);
    value_t *r = value_explicit_cast(vm, v, vm->type_bool);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_FALSE(value_as(r, bool));
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastU64ToBoolTrue) {
    value_t *v = make_u64_raw(vm, 1);
    value_t *r = value_explicit_cast(vm, v, vm->type_bool);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));
    raw_free(vm, v);
}

/* ---- float→bool ---- */

TEST_F(ValueCore, ExplicitCastF64ToBoolTrue) {
    value_t *v = make_f64_raw(vm, 0.001);
    value_t *r = value_explicit_cast(vm, v, vm->type_bool);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastF64ToBoolFalse) {
    value_t *v = make_f64_raw(vm, 0.0);
    value_t *r = value_explicit_cast(vm, v, vm->type_bool);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_FALSE(value_as(r, bool));
    raw_free(vm, v);
}

/* ---- bool→int ---- */

TEST_F(ValueCore, ExplicitCastBoolToI32True) {
    value_t *v = make_bool_raw(vm, true);
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 1);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastBoolToI32False) {
    value_t *v = make_bool_raw(vm, false);
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 0);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastBoolToU64True) {
    value_t *v = make_bool_raw(vm, true);
    value_t *r = value_explicit_cast(vm, v, vm->type_u64);
    EXPECT_EQ(value_type(r), vm->type_u64);
    EXPECT_EQ(read_uint(r), 1ull);
    raw_free(vm, v);
}

/* ---- bool→float ---- */

TEST_F(ValueCore, ExplicitCastBoolToF64True) {
    value_t *v = make_bool_raw(vm, true);
    value_t *r = value_explicit_cast(vm, v, vm->type_f64);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 1.0);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastBoolToF32False) {
    value_t *v = make_bool_raw(vm, false);
    value_t *r = value_explicit_cast(vm, v, vm->type_f32);
    EXPECT_EQ(value_type(r), vm->type_f32);
    EXPECT_FLOAT_EQ((float)read_float(r), 0.0f);
    raw_free(vm, v);
}

/* ---- 显式转换禁止场景：str ---- */

TEST_F(ValueCore, ExplicitCastStrToI32ReturnsError) {
    value_t *v = make_str_raw(vm, "123");
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastI32ToStrReturnsError) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_explicit_cast(vm, v, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastStrToF64ReturnsError) {
    value_t *v = make_str_raw(vm, "3.14");
    value_t *r = value_explicit_cast(vm, v, vm->type_f64);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastBoolToStrReturnsError) {
    value_t *v = make_bool_raw(vm, true);
    value_t *r = value_explicit_cast(vm, v, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 显式转换禁止场景：void ---- */

TEST_F(ValueCore, ExplicitCastVoidToI32ReturnsError) {
    value_t *v = NULL;
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ---- 显式转换：error 传播 ---- */

TEST_F(ValueCore, ExplicitCastErrorPropagates) {
    value_t *err = value_make_error(vm, "err");
    value_t *r = value_explicit_cast(vm, err, vm->type_i32);
    EXPECT_TRUE(value_is_error(vm, r));
}

/* ---- 显式转换：同类型 clone ---- */

TEST_F(ValueCore, ExplicitCastSameTypeClones) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *r = value_explicit_cast(vm, v, vm->type_i32);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 42);
    raw_free(vm, v);
}

/* ---- 显式转换补充：uint→str 禁止 ---- */

TEST_F(ValueCore, ExplicitCastU32ToStrReturnsError) {
    value_t *v = make_u32_raw(vm, 42);
    value_t *r = value_explicit_cast(vm, v, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ---- 显式转换补充：float→i16/u16 ---- */

TEST_F(ValueCore, ExplicitCastF64ToI16Truncates) {
    value_t *v = make_f64_raw(vm, 3.9);
    value_t *r = value_explicit_cast(vm, v, vm->type_i16);
    EXPECT_EQ(value_type(r), vm->type_i16);
    EXPECT_EQ(read_sint(r), 3);
    raw_free(vm, v);
}

TEST_F(ValueCore, ExplicitCastF64ToU16Truncates) {
    value_t *v = make_f64_raw(vm, 40000.9);
    value_t *r = value_explicit_cast(vm, v, vm->type_u16);
    EXPECT_EQ(value_type(r), vm->type_u16);
    EXPECT_EQ(read_uint(r), (uint16_t)40000);
    raw_free(vm, v);
}

/* ---- 显式转换补充：float→str 禁止 ---- */

TEST_F(ValueCore, ExplicitCastF64ToStrReturnsError) {
    value_t *v = make_f64_raw(vm, 3.14);
    value_t *r = value_explicit_cast(vm, v, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, v);
}

/* ================================================================ */
/* 3c. 二元运算类型协商 (BinaryOpPromotion)                          */
/* ================================================================ */

/* ---- 同类别不同宽度：有符号整数 promote ---- */

TEST_F(ValueCore, BinaryOpI8PlusI32PromotesToI32) {
    value_t *a = make_i8_raw(vm, 5);
    value_t *b = make_i32_raw(vm, 100);
    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 105);
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpI16PlusI64PromotesToI64) {
    value_t *a = make_i16_raw(vm, 1000);
    value_t *b = make_i64_raw(vm, 100000);
    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_i64);
    EXPECT_EQ(read_sint(r), 101000);
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpI32LtI8PromotesToI32) {
    value_t *a = make_i32_raw(vm, 5);
    value_t *b = make_i8_raw(vm, 10);
    value_t *r = value_lt(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 同类别不同宽度：无符号整数 promote ---- */

TEST_F(ValueCore, BinaryOpU16PlusU64PromotesToU64) {
    value_t *a = make_u16_raw(vm, 100);
    value_t *b = make_u64_raw(vm, 100000);
    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_u64);
    EXPECT_EQ(read_uint(r), 100100ull);
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpU8PlusU32PromotesToU32) {
    value_t *a = make_u8_raw(vm, 200);
    value_t *b = make_u32_raw(vm, 1000);
    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_u32);
    EXPECT_EQ(read_uint(r), 1200u);
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 同类别不同宽度：浮点 promote ---- */

TEST_F(ValueCore, BinaryOpF32PlusF64PromotesToF64) {
    value_t *a = make_f32_raw(vm, 1.5f);
    value_t *b = make_f64_raw(vm, 2.5);
    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 4.0);
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpF64MulF32PromotesToF64) {
    value_t *a = make_f64_raw(vm, 3.0);
    value_t *b = make_f32_raw(vm, 2.0f);
    value_t *r = value_mul(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 6.0);
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 跨类别：有符号+无符号 → error ---- */

TEST_F(ValueCore, BinaryOpI32PlusU32ReturnsError) {
    value_t *a = make_i32_raw(vm, -1);
    value_t *b = make_u32_raw(vm, 1);
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpI8PlusU64ReturnsError) {
    value_t *a = make_i8_raw(vm, 5);
    value_t *b = make_u64_raw(vm, 1000);
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpU32LtI32ReturnsError) {
    value_t *a = make_u32_raw(vm, 1);
    value_t *b = make_i32_raw(vm, -1);
    value_t *r = value_lt(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 跨类别：int+float → error ---- */

TEST_F(ValueCore, BinaryOpI32PlusF64ReturnsError) {
    value_t *a = make_i32_raw(vm, 10);
    value_t *b = make_f64_raw(vm, 32.5);
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpU64MulF32ReturnsError) {
    value_t *a = make_u64_raw(vm, 3);
    value_t *b = make_f32_raw(vm, 2.0f);
    value_t *r = value_mul(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpI64EqF64ReturnsError) {
    value_t *a = make_i64_raw(vm, 1);
    value_t *b = make_f64_raw(vm, 1.0);
    value_t *r = value_eq(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 跨类别：bool+int → error ---- */

TEST_F(ValueCore, BinaryOpBoolPlusI32ReturnsError) {
    value_t *a = make_bool_raw(vm, true);
    value_t *b = make_i32_raw(vm, 1);
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpBoolEqI32ReturnsError) {
    value_t *a = make_bool_raw(vm, true);
    value_t *b = make_i32_raw(vm, 1);
    value_t *r = value_eq(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 跨类别：str+非str → error ---- */

TEST_F(ValueCore, BinaryOpStrPlusI32ReturnsError) {
    value_t *a = make_str_raw(vm, "hello");
    value_t *b = make_i32_raw(vm, 42);
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpI32PlusStrReturnsError) {
    value_t *a = make_i32_raw(vm, 42);
    value_t *b = make_str_raw(vm, "hello");
    value_t *r = value_add(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpStrEqI32ReturnsError) {
    value_t *a = make_str_raw(vm, "1");
    value_t *b = make_i32_raw(vm, 1);
    value_t *r = value_eq(vm, a, b);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ---- 同类型运算：正常路径 ---- */

TEST_F(ValueCore, BinaryOpI32PlusI32SameType) {
    value_t *a = make_i32_raw(vm, 10);
    value_t *b = make_i32_raw(vm, 32);
    value_t *r = value_add(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_i32);
    EXPECT_EQ(read_sint(r), 42);
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpF64SubF64SameType) {
    value_t *a = make_f64_raw(vm, 10.5);
    value_t *b = make_f64_raw(vm, 3.5);
    value_t *r = value_sub(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_f64);
    EXPECT_DOUBLE_EQ(read_float(r), 7.0);
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpStrEqStrSameType) {
    value_t *a = make_str_raw(vm, "hello");
    value_t *b = make_str_raw(vm, "hello");
    value_t *r = value_eq(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));
    raw_free(vm, a);
    raw_free(vm, b);
}

TEST_F(ValueCore, BinaryOpBoolEqBoolSameType) {
    value_t *a = make_bool_raw(vm, true);
    value_t *b = make_bool_raw(vm, true);
    value_t *r = value_eq(vm, a, b);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(value_as(r, bool));
    raw_free(vm, a);
    raw_free(vm, b);
}

/* ================================================================ */
/* 4. Scope 机制                                                     */
/* ================================================================ */

class ScopeMech : public ::testing::Test {
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

/* ---- scope_define / scope_lookup ---- */

TEST_F(ScopeMech, DefineAndLookup) {
    value_t *v = make_i32_raw(vm, 42);
    value_t *stored = scope_define(vm, vm->current_scope, "x", v);
    ASSERT_NE(stored, nullptr);
    EXPECT_EQ(value_type(stored), vm->type_i32);
    EXPECT_EQ(read_sint(stored), 42);

    /* lookup 能找到 */
    value_t *found = scope_lookup(vm->current_scope, STRSLICE_LIT("x"));
    EXPECT_EQ(found, stored);

    /* 释放 raw value */
    raw_free(vm, v);
}

TEST_F(ScopeMech, LookupNotFoundReturnsNull) {
    value_t *found = scope_lookup(vm->current_scope, STRSLICE_LIT("nonexistent"));
    EXPECT_EQ(found, nullptr);
}

TEST_F(ScopeMech, LookupTraversesParentChain) {
    vm_push_scope(vm);
    scope_t *child = vm->current_scope;

    /* 在 root_scope 定义变量 */
    value_t *v = make_i32_raw(vm, 99);
    scope_define(vm, vm->root_scope, "parent_var", v);
    raw_free(vm, v);

    /* 在 child scope 能查找到 parent 的变量 */
    value_t *found = scope_lookup(child, STRSLICE_LIT("parent_var"));
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(read_sint(found), 99);

    vm_pop_scope(vm);
}

/* ---- scope_define 重复定义检查 ---- */

TEST_F(ScopeMech, DuplicateDefineReturnsError) {
    value_t *v = make_i32_raw(vm, 1);
    scope_define(vm, vm->current_scope, "x", v);
    raw_free(vm, v);

    value_t *v2 = make_i32_raw(vm, 2);
    value_t *result = scope_define(vm, vm->current_scope, "x", v2);
    EXPECT_TRUE(value_is_error(vm, result));
    raw_free(vm, v2);

    /* 原值不变 */
    value_t *found = scope_lookup(vm->current_scope, STRSLICE_LIT("x"));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(read_sint(found), 1);
}

/* ---- owned 生命周期 ---- */

TEST_F(ScopeMech, PopScopeDisposesOwnedValues) {
    vm_push_scope(vm);

    value_t *v = make_i32_raw(vm, 42);
    value_t *cloned = value_clone(vm, v); /* track 到 current_scope（push 出来的 scope） */
    EXPECT_EQ(read_sint(cloned), 42);

    raw_free(vm, v);

    vm_pop_scope(vm); /* 应销毁 cloned，不泄漏 */

    /* allocator_live_count 在 TearDown 验证 */
}

TEST_F(ScopeMech, DefineInChildScopeDestroyedOnPop) {
    vm_push_scope(vm);

    value_t *v = make_i32_raw(vm, 123);
    scope_define(vm, vm->current_scope, "child_var", v);
    raw_free(vm, v);

    /* 在 child scope 能查到 */
    value_t *found = scope_lookup(vm->current_scope, STRSLICE_LIT("child_var"));
    EXPECT_NE(found, nullptr);

    vm_pop_scope(vm);

    /* pop 后在 root scope 查不到 */
    found = scope_lookup(vm->root_scope, STRSLICE_LIT("child_var"));
    EXPECT_EQ(found, nullptr);
}

/* ---- 跨 scope clone ---- */

TEST_F(ScopeMech, CrossScopeCloneTrackedToTargetScope) {
    vm_push_scope(vm);
    scope_t *inner = vm->current_scope;

    /* 在 root_scope 定义变量 */
    value_t *v = make_i32_raw(vm, 55);
    scope_define(vm, vm->root_scope, "root_var", v);
    raw_free(vm, v);

    /* 从 inner scope 查找到 root_var，clone 到 inner scope */
    value_t *found = scope_lookup(inner, STRSLICE_LIT("root_var"));
    ASSERT_NE(found, nullptr);

    /* 临时切换 current_scope 到 inner，clone 后 track 到 inner */
    vm->current_scope = inner;
    value_t *cloned = value_clone(vm, found);
    EXPECT_EQ(read_sint(cloned), 55);

    vm_pop_scope(vm); /* 销毁 inner 及 cloned */

    /* root_var 仍存在 */
    found = scope_lookup(vm->root_scope, STRSLICE_LIT("root_var"));
    EXPECT_NE(found, nullptr);
}

/* ---- str 类型 owned 生命周期（dispose 释放 string_t） ---- */

TEST_F(ScopeMech, StrValueDisposedOnPopScope) {
    vm_push_scope(vm);

    value_t *s = make_str_raw(vm, "hello world");
    value_t *cloned = value_clone(vm, s); /* str_clone 深拷贝 string_t */
    (void)cloned;
    raw_free(vm, s);

    /* cloned track 到 current_scope，pop 时 str_dispose 释放 string_t */
    vm_pop_scope(vm);
}

/* ---- define NULL name ---- */

TEST_F(ScopeMech, DefineNullNameReturnsNull) {
    value_t *v = make_i32_raw(vm, 1);
    EXPECT_EQ(scope_define(vm, vm->current_scope, nullptr, v), nullptr);
    raw_free(vm, v);
}

TEST_F(ScopeMech, DefineNullScopeReturnsNull) {
    value_t *v = make_i32_raw(vm, 1);
    EXPECT_EQ(scope_define(vm, nullptr, "x", v), nullptr);
    raw_free(vm, v);
}

/* ---- scope_track 直接使用 ---- */

TEST_F(ScopeMech, TrackRegistersToOwned) {
    vm_push_scope(vm);

    /* value_make auto-track 到 current_scope */
    int32_t val = 777;
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i32, &val);
    (void)value_make(vm, vm->type_i32, data);
    /* value track 到 current_scope->owned，pop_scope 时统一释放 */

    vm_pop_scope(vm);
}

/* ================================================================ */
/* 10. Shadow Value                                                  */
/* ================================================================ */

TEST_F(ValueCore, ShadowMakeAndIsShadow) {
    value_t *s = value_make_shadow(vm, vm->type_i32);
    EXPECT_TRUE(value_is_shadow(s));
    EXPECT_EQ(value_type(s), vm->type_i32);
    EXPECT_EQ(value_data(s), nullptr);
    /* shadow auto-track 到 current_scope，vm_destroy 释放 */
}

TEST_F(ValueCore, ShadowCloneReturnsShadow) {
    value_t *s = value_make_shadow(vm, vm->type_i32);
    value_t *cloned = value_clone(vm, s);
    EXPECT_TRUE(value_is_shadow(cloned));
    EXPECT_EQ(value_type(cloned), vm->type_i32);
    EXPECT_EQ(value_data(cloned), nullptr);
}

TEST_F(ValueCore, ShadowCloneStrReturnsShadow) {
    value_t *s = value_make_shadow(vm, vm->type_str);
    value_t *cloned = value_clone(vm, s);
    EXPECT_TRUE(value_is_shadow(cloned));
    EXPECT_EQ(value_type(cloned), vm->type_str);
    EXPECT_EQ(value_data(cloned), nullptr);
}

TEST_F(ValueCore, ShadowAddNormalReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *b  = make_i32_raw(vm, 42);
    value_t *r  = value_add(vm, sa, b);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
    raw_free(vm, b);
}

TEST_F(ValueCore, ShadowAddShadowReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *sb = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_add(vm, sa, sb);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
}

TEST_F(ValueCore, ShadowAddWithPromotionReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i8);
    value_t *b  = make_i32_raw(vm, 42);
    value_t *r  = value_add(vm, sa, b);
    EXPECT_TRUE(value_is_shadow(r));
    /* i8 + i32 → promote to i32 */
    EXPECT_EQ(value_type(r), vm->type_i32);
    raw_free(vm, b);
}

TEST_F(ValueCore, ShadowComparisonReturnsBoolShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *sb = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_lt(vm, sa, sb);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_bool);
}

TEST_F(ValueCore, ShadowFloatAddReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_f64);
    value_t *sb = value_make_shadow(vm, vm->type_f64);
    value_t *r  = value_add(vm, sa, sb);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_f64);
}

TEST_F(ValueCore, ShadowBoolLnotReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_bool);
    value_t *r  = value_lnot(vm, sa);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_bool);
}

TEST_F(ValueCore, ShadowStrEqReturnsBoolShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_str);
    value_t *sb = value_make_shadow(vm, vm->type_str);
    value_t *r  = value_eq(vm, sa, sb);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_bool);
}

TEST_F(ValueCore, ShadowImplicitCastReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i8);
    value_t *r  = value_implicit_cast(vm, sa, vm->type_i32);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
}

TEST_F(ValueCore, ShadowImplicitCastSameTypeReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_implicit_cast(vm, sa, vm->type_i32);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
}

TEST_F(ValueCore, ShadowImplicitCastF32ToF64ReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_f32);
    value_t *r  = value_implicit_cast(vm, sa, vm->type_f64);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_f64);
}

TEST_F(ValueCore, ShadowExplicitCastReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_explicit_cast(vm, sa, vm->type_f64);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_f64);
}

TEST_F(ValueCore, ShadowExplicitCastFloatToIntReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_f64);
    value_t *r  = value_explicit_cast(vm, sa, vm->type_i32);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
}

TEST_F(ValueCore, ShadowExplicitCastBoolToIntReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_bool);
    value_t *r  = value_explicit_cast(vm, sa, vm->type_i32);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
}

TEST_F(ValueCore, ShadowExplicitCastIncompatibleReturnsError) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_explicit_cast(vm, sa, vm->type_str);
    EXPECT_TRUE(value_is_error(vm, r));
}

TEST_F(ValueCore, ShadowImplicitCastNotWideningReturnsError) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_implicit_cast(vm, sa, vm->type_i8);
    EXPECT_TRUE(value_is_error(vm, r));
}

TEST_F(ValueCore, ShadowAssignReturnsDst) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *b  = make_i32_raw(vm, 42);
    value_t *r  = value_assign(vm, sa, b);
    EXPECT_EQ(r, sa);
    EXPECT_TRUE(value_is_shadow(sa));
    raw_free(vm, b);
}

TEST_F(ValueCore, ShadowAssignWithImplicitCast) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *b  = make_i8_raw(vm, 10);
    value_t *r  = value_assign(vm, sa, b);
    EXPECT_EQ(r, sa);
    EXPECT_TRUE(value_is_shadow(sa));
    raw_free(vm, b);
}

TEST_F(ValueCore, ShadowDisposeIsSafe) {
    /* shadow value dispose 不应崩溃（无 data 需要释放） */
    vm_push_scope(vm);
    value_t *sa = value_make_shadow(vm, vm->type_str);
    (void)sa;
    vm_pop_scope(vm);  /* pop_scope 会 dispose + free shadow value */
}

TEST_F(ValueCore, ShadowNegReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_neg(vm, sa);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
}

TEST_F(ValueCore, ShadowBnotReturnsShadow) {
    value_t *sa = value_make_shadow(vm, vm->type_i32);
    value_t *r  = value_bnot(vm, sa);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i32);
}

TEST_F(ValueCore, ShadowMixedIntPromotion) {
    /* shadow i8 + normal i64 → promote to i64, shadow result */
    value_t *sa = value_make_shadow(vm, vm->type_i8);
    value_t *b  = make_i64_raw(vm, 42);
    value_t *r  = value_add(vm, sa, b);
    EXPECT_TRUE(value_is_shadow(r));
    EXPECT_EQ(value_type(r), vm->type_i64);
    raw_free(vm, b);
}

/* ---- interrupt 机制（引擎级控制流哨兵） ---- */

TEST_F(ValueCore, MakeInterruptIsTracked) {
    value_t *it = value_make_interrupt(vm, INTERRUPT_RETURN);
    EXPECT_EQ(value_type(it), vm->type_interrupt);
    EXPECT_TRUE(value_is_interrupt(vm, it));
    EXPECT_EQ(value_interrupt_kind(vm, it), INTERRUPT_RETURN);
    /* auto-track 到 current_scope，vm_destroy 释放 */
}

TEST_F(ValueCore, NonInterruptIsNotInterrupt) {
    value_t *v = make_i32_raw(vm, 1);
    EXPECT_FALSE(value_is_interrupt(vm, v));
    raw_free(vm, v);

    value_t *it = value_make_interrupt(vm, INTERRUPT_RETURN);
    EXPECT_TRUE(value_is_interrupt(vm, it));
}

TEST_F(ValueCore, InterruptKindOnNonInterruptReturnsReturn) {
    value_t *v = make_i32_raw(vm, 1);
    EXPECT_EQ(value_interrupt_kind(vm, v), INTERRUPT_RETURN);
    raw_free(vm, v);
}

TEST_F(ValueCore, InterruptNotErrorAndViceVersa) {
    /* interrupt 与 error 同级但互斥：哨兵不短路运算，error 短路 */
    value_t *it = value_make_interrupt(vm, INTERRUPT_RETURN);
    value_t *err = value_make_error(vm, "e");
    EXPECT_FALSE(value_is_error(vm, it));
    EXPECT_FALSE(value_is_interrupt(vm, err));
}

TEST_F(ValueCore, InterruptClonePropagates) {
    /* scope_define/值传递对 interrupt clone：kind 保持 */
    value_t *it = value_make_interrupt(vm, INTERRUPT_RETURN);
    value_t *c  = value_clone(vm, it);
    EXPECT_TRUE(value_is_interrupt(vm, c));
    EXPECT_EQ(value_interrupt_kind(vm, c), INTERRUPT_RETURN);
}

/* ---- undefined（void 类型 value） ---- */

TEST_F(ValueCore, MakeUndefinedIsVoidType) {
    value_t *u = value_make_undefined(vm);
    EXPECT_EQ(value_type(u), vm->type_void);
    EXPECT_TRUE(value_is_undefined(vm, u));
    /* auto-track 到 current_scope，vm_destroy 释放 */
}

TEST_F(ValueCore, NonUndefinedIsNotUndefined) {
    value_t *v = make_i32_raw(vm, 1);
    EXPECT_FALSE(value_is_undefined(vm, v));
    raw_free(vm, v);

    value_t *u = value_make_undefined(vm);
    EXPECT_TRUE(value_is_undefined(vm, u));
}

TEST_F(ValueCore, UndefinedCloneableForScopeDefine) {
    /* VTABLE_VOID 补 clone 槽后：undefined 可被 scope_define clone 持有 */
    value_t *u = value_make_undefined(vm);
    value_t *c = value_clone(vm, u);
    EXPECT_FALSE(value_is_error(vm, c));
    EXPECT_TRUE(value_is_undefined(vm, c));
}

TEST_F(ValueCore, UndefinedScopeDefineAndPop) {
    /* undefined 入 scope 后可正常弹出销毁，无泄漏 */
    value_t *u = value_make_undefined(vm);
    vm_push_scope(vm);
    value_t *stored = scope_define(vm, vm->current_scope, "u", u);
    ASSERT_NE(stored, nullptr);
    EXPECT_TRUE(value_is_undefined(vm, stored));
    vm_pop_scope(vm);
}

/* ---- nil（内置类型唯一值，函数 0 初始化/未来空指针） ---- */

TEST_F(ValueCore, MakeNilIsNilType) {
    value_t *n = value_make_nil(vm);
    EXPECT_EQ(value_type(n), vm->type_nil);
    EXPECT_TRUE(value_is_nil(vm, n));
    EXPECT_EQ(value_kind(n), TYPE_KIND_NIL);
    /* data 为 func_t* 宽度零块 = NULL 指针（函数 0 初始化语义） */
    const func_t *fn = *(const func_t **)value_data(n);
    EXPECT_EQ(fn, nullptr);
}

TEST_F(ValueCore, NilNotRegisteredAsTypeName) {
    /* nil 不注册进 global scope：type_lookup("nil") = NULL，
       因此 var a:nil 无法解析（与 i32/bool 等类型名不同） */
    EXPECT_EQ(type_lookup(vm, STRSLICE_LIT("nil")), nullptr);
}

TEST_F(ValueCore, NilCloneAndAssign) {
    value_t *n = value_make_nil(vm);
    value_t *c = value_clone(vm, n);
    EXPECT_TRUE(value_is_nil(vm, c));
    EXPECT_EQ(value_type(c), vm->type_nil);

    value_t *dst = value_make_nil(vm);
    value_t *r = value_assign(vm, dst, n);
    ASSERT_EQ(r, dst);
    EXPECT_TRUE(value_is_nil(vm, dst));
}

TEST_F(ValueCore, NilEqNilIsTrue) {
    value_t *a = value_make_nil(vm);
    value_t *b = value_make_nil(vm);
    value_t *r = value_eq(vm, a, b);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(*(bool *)value_data(r));
}

TEST_F(ValueCore, NilEqFuncIsNullCheck) {
    /* nil == func：func 指针是否为 NULL。0 初始化的 func value 与 nil 相等 */
    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_void, false);
    value_t *fn = func_new(vm, sum_variadic, vm->global_scope,
                           vm->root_scope, sig, STRSLICE_LIT("f"));

    /* 真实函数指针 → nil != fn */
    value_t *r1 = value_eq(vm, value_make_nil(vm), fn);
    EXPECT_FALSE(*(bool *)value_data(r1));

    /* 0 初始化 func value（data 为 NULL 指针）→ nil == fn */
    void *zd = value_alloc_data(vm->alloc, sig);
    *(func_t **)zd = NULL;
    value_t *zero_fn = value_make_untracked(vm->alloc, sig, zd);
    value_t *r2 = value_eq(vm, value_make_nil(vm), zero_fn);
    EXPECT_TRUE(*(bool *)value_data(r2));

    raw_free(vm, fn);
    raw_free(vm, zero_fn);
}

TEST_F(ValueCore, FuncEqNil) {
    /* func == nil（func vtable 侧）：NULL 函数指针 == nil */
    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_void, false);
    void *zd = value_alloc_data(vm->alloc, sig);
    *(func_t **)zd = NULL;
    value_t *zero_fn = value_make_untracked(vm->alloc, sig, zd);
    value_t *r = value_eq(vm, zero_fn, value_make_nil(vm));
    EXPECT_EQ(value_type(r), vm->type_bool);
    EXPECT_TRUE(*(bool *)value_data(r));

    raw_free(vm, zero_fn);
}

TEST_F(ValueCore, NilExplicitCastToU64IsZero) {
    value_t *n = value_make_nil(vm);
    value_t *r = value_explicit_cast(vm, n, vm->type_u64);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(value_type(r), vm->type_u64);
    EXPECT_EQ(read_sint(r), 0);
}

TEST_F(ValueCore, NilExplicitCastToFunc) {
    value_t *n = value_make_nil(vm);
    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_void, false);
    value_t *r = value_explicit_cast(vm, n, sig);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(value_type(r), sig);
    /* data 为 NULL 指针（0 初始化函数值） */
    const func_t *fn = *(const func_t **)value_data(r);
    EXPECT_EQ(fn, nullptr);
}

TEST_F(ValueCore, NilImplicitCastToFunc) {
    value_t *n = value_make_nil(vm);
    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_void, false);
    value_t *r = value_implicit_cast(vm, n, sig);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(value_type(r), sig);
    const func_t *fn = *(const func_t **)value_data(r);
    EXPECT_EQ(fn, nullptr);
}

TEST_F(ValueCore, NilImplicitCastToU64Fails) {
    /* nil → u64 仅显式：隐式转换报错 */
    value_t *n = value_make_nil(vm);
    value_t *r = value_implicit_cast(vm, n, vm->type_u64);
    EXPECT_TRUE(value_is_error(vm, r));
}

TEST_F(ValueCore, NilCompareWithIntFails) {
    value_t *n = value_make_nil(vm);
    value_t *i = make_i32_raw(vm, 0);
    value_t *r = value_eq(vm, n, i);
    EXPECT_TRUE(value_is_error(vm, r));
    raw_free(vm, i);
}

