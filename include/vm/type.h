#ifndef _H_CLUX_VM_TYPE_
#define _H_CLUX_VM_TYPE_
#ifdef __cplusplus
extern "C" {
#endif

#include "vm/vtable.h"
#include "core/strslice.h"
#include <stdbool.h>
#include <stddef.h>

/**
 * type_kind_t: 类型粗粒度分类（type_t.kind）
 *
 * 鸭子类型判断的基础：复合/修饰类型按 kind 而非实例指针分类。
 * 粗粒度（INT/FLOAT 覆盖全部宽度变体），细粒度差异由 name/size 承载。
 * M2 预留 STRUCT/ARRAY/TUPLE/ENUM/CUNION。
 */
typedef enum type_kind_t {
    TYPE_KIND_VOID     = 0,
    TYPE_KIND_BOOL,
    TYPE_KIND_INT,       /* i8/i16/i32/i64/u8/u16/u32/u64 */
    TYPE_KIND_FLOAT,     /* f32/f64 */
    TYPE_KIND_STR,
    TYPE_KIND_TYPE,      /* type value 的元类型 */
    TYPE_KIND_FUNC,
    TYPE_KIND_ERROR,
    TYPE_KIND_INTERRUPT, /* 引擎级控制流哨兵（interrupt 类型） */
    /* ---- M2 复合类型段（内建标量之后；sema 只登记此段类型） ---- */
    TYPE_KIND_CONST,     /* const 修饰（持 sub） */
    TYPE_KIND_VOLATILE,  /* volatile 修饰（持 sub） */
    TYPE_KIND_OPTION,    /* optional 修饰（持 inner，?T） */
    TYPE_KIND_STRUCT,
    TYPE_KIND_ARRAY,
    TYPE_KIND_TUPLE,
    TYPE_KIND_ENUM,
    TYPE_KIND_CUNION,
    TYPE_KIND_COUNT,     /* 哨兵：复合段上界（> TYPE_KIND_INTERRUPT 且 < COUNT 即复合类型） */
} type_kind_t;

/**
 * func_sig_t: 函数签名（参数 + 返回值信息）
 *
 * 内联在 func_type_t 中。同一签名的所有函数共享同一个签名类型
 * func_type_t（由 vm 类型池按签名去重 intern），因此 func value 的
 * type 即携带完整签名，shadow 值（data=NULL）也能完成调用类型检查。
 */
typedef struct func_sig_t {
    const type_t **params;      /* array[param_count]; 元素可为 NULL = 无类型约束 */
    size_t         param_count;
    const type_t  *return_type; /* NULL = void */
    bool           is_variadic;
} func_sig_t;

/**
 * type_t: 类型描述符（基类）
 *
 * 每种类型对应一个 type_t 单例，由 vm 统一管理。
 * 类型本身也是一种 value（通过 type_as_value 获取）。
 *
 * 需要附加信息的类型通过 C 继承扩展：子结构体首成员为 type_t base，
 * 向上转型 (type_t *) 使用，向下转型 (func_type_t *) 读取扩展字段。
 */
typedef struct type_t {
    const vtable_t *vtable;
    strslice_t      name;       /* 类型名，如 "i32", "f64", "str" */
    size_t          size;       /* 该类型数据的字节大小 */
    size_t          align;      /* 该类型数据的对齐要求 */
    type_kind_t     kind;       /* 粗粒度分类（鸭子类型判断用） */
    bool            sealed;     /* 是否已密封（构造完成后置位；内置基础类型创建即 true） */
    uint32_t        id;         /* 类型全局唯一 id：内建类型固定（vm_init_builtins 按序），
                                   程序类型由编译器按 sema 解析的实例指针去重分配。
                                   LOAD_TYPE <id> 从 vm->types_by_id 查表压栈。 */
} type_t;

/**
 * const_type_t / volatile_type_t: 前导修饰类型（type_t 的扩展）
 *
 * 真实类型（const i32 != i32），持 sub 指针指向被修饰的底层类型。
 * 由 vm 类型池 intern（type_const_intern / type_volatile_intern），vm
 * 拥有生命周期。运算行为（vtable）代理到 sub（解包语义，见 m2-design §10）：
 * volatile(const(i32)) 组合时 volatile 外层、const 内层（固定顺序）。
 */
typedef struct const_type_t {
    type_t      base;
    const type_t *sub;
} const_type_t;

typedef struct volatile_type_t {
    type_t      base;
    const type_t *sub;
} volatile_type_t;

/**
 * option_type_t: optional 类型（type_t 的扩展，?T，见 m2-design §9/§12.1）
 *
 * 持 inner 指针指向被 optional 包裹的类型 T。C 内存映射：
 *   struct Optional_T { bool ok; T value; }（value 偏移 align_up(1, alignof(T))）。
 * size/align 按 C 对齐规则密封时计算；value 偏移 = option_value_offset。
 *
 * ?T 只能与 nil 比较（四种判定形式，tag 比较由编译器发专用指令，非 vtable
 * eq 分派）；T → ?T 隐式提升（some）在 value_implicit_cast 公共入口特判；
 * clone/assign/dispose 转发 inner 且额外处理 ok（ok 平凡拷贝；value 字段
 * 递归——复用 array_blit_raw / array_dispose_raw，OPTION 分支与嵌套数组同构）。
 */
typedef struct option_type_t {
    type_t       base;
    const type_t *inner;  /* 被 optional 包裹的类型 T（引用，不拥有） */
} option_type_t;

/**
 * enum_type_t / enum_variant_t: 枚举类型（type_t 的扩展，见 m2-design §3）
 *
 * 枚举是"有特殊类型的全局变量"：每个 variant 是编译期常量，类型为枚举自身。
 * 底层类型 underlying 必须为整型（TYPE_KIND_INT）；enum value 的 data 按
 * underlying->size 宽度存储整数值（MAKE_ENUM 构造时截断）。
 *
 * variant 表由 vm 拥有（SEAL 时深拷贝；去重 intern 命中已有类型时手工回收）。
 * 严格类型分离（m2-design §5）：enum 与底层互不隐式转换，显式 cast 仅允许
 * enum → 声明底层类型（c as i8 报错，须 c as i32 as i8 两次 cast）。
 */
typedef struct enum_variant_t {
    strslice_t name;   /* variant 名（seal 时从 strtable/源拷贝，vm 拥有） */
    int64_t    value;  /* 底层整数值（按 underlying->size 截断存储） */
} enum_variant_t;

typedef struct enum_type_t {
    type_t          base;
    const type_t   *underlying;    /* 底层类型（引用，不拥有；须 TYPE_KIND_INT） */
    enum_variant_t *variants;      /* variant 表（seal 时拷贝，vm 拥有） */
    size_t          variant_count;
} enum_type_t;

/** 判断类型是否为 optional 修饰类型（type_kind 分类） */
static inline bool type_is_option(const type_t *t) {
    return t && t->kind == TYPE_KIND_OPTION;
}

/* 取 option 类型的 inner（非 option 返回 NULL）：见 vm/type_option.h
 * （static inline 定义，type.h 不重复声明避免冲突）。 */

/* 数组类型 array_type_t 的定义、构造 API 与访问器见 vm/type_array.h
 * （array_type_t 继承 type_t：首成员 base 为 type_t，向上转型安全）。 */

/**
 * 按名从当前作用域链解析类型（类型即表达式）
 *
 * 类型名与变量同机制：内建类型值注册在 global scope（vm_register_builtin_types），
 * 自定义类型（M2 type 定义）注册到定义点当前作用域。scope_lookup 沿
 * current_scope → root_scope → global_scope 链查找，变量遮蔽类型天然成立
 * （命中非 type value 时返回 NULL）。
 */
const type_t *type_lookup(const vm_t *vm, strslice_t name);

/** 判断两个类型是否相同（指针比较，因为类型是单例） */
static inline bool type_eq(const type_t *a, const type_t *b) {
    return a == b;
}

/** 类型是否已密封（构造完成，不可再修改签名/布局）。内置基础类型创建即 sealed。 */
static inline bool type_is_sealed(const type_t *t) {
    return t && t->sealed;
}

/**
 * 鸭子类型相等判断：分派 a->vtable->type_equal(vm, a, b)。
 * NULL 槽位 = 默认指针比较（基础类型单例）。
 * 复合类型（M2 struct/array/tuple）按成员结构递归判断。
 */
bool type_equal(vm_t *vm, const type_t *a, const type_t *b);

/**
 * 类型兼容性判断（T extends U）：sub 是否可兼容 sup（鸭子类型转换）。
 * 分派 sub->vtable->type_extends(vm, sub, sup)；NULL 槽位 = 默认同类型
 * （type_equal）。M2 复合类型按成员递归判断。
 */
bool type_extends(vm_t *vm, const type_t *sub, const type_t *sup);

/** 判断类型是否为 const 修饰类型（type_kind 分类） */
static inline bool type_is_const(const type_t *t) {
    return t && t->kind == TYPE_KIND_CONST;
}

/** 判断类型是否为 volatile 修饰类型（type_kind 分类） */
static inline bool type_is_volatile(const type_t *t) {
    return t && t->kind == TYPE_KIND_VOLATILE;
}

/** 取修饰类型的 sub（非 const/volatile 时返回 NULL） */
const type_t *type_qualifier_sub(const type_t *t);

/**
 * 类型是否含 const 修饰（递归 sub 链）：const i32 → true；
 * volatile(const(i32)) → true；volatile(i32) → false。
 * sema 赋值左值检查用。
 */
bool type_has_const(const type_t *t);

/** const 类型 intern（按 sub 指针去重，vm 拥有生命周期） */
const type_t *type_const_intern(vm_t *vm, const type_t *sub);

/** volatile 类型 intern（按 sub 指针去重，vm 拥有生命周期） */
const type_t *type_volatile_intern(vm_t *vm, const type_t *sub);

/* const/volatile 类型开放构造（M2 字节码协议 PUSH_CONST / PUSH_VOLATILE +
 * SET_TYPE + SEAL，与数组/签名类型统一的两遍构造，获得向前声明能力）：
 *   - type_const_push / type_volatile_push：分配空限定类型（sub=NULL，
 *     不入池）并压其 type value，返回开放 type（未密封）
 *   - type_qual_set_sub：设被修饰的底层类型（SET_TYPE 运行期用；密封后
 *     静默忽略，与 array/func 的 set 系列一致）
 *   - type_const_seal / type_volatile_seal：密封——按 sub 指针去重 intern +
 *     拷贝 size/align（const 值存储与 sub 相同）+ 置 sealed + 入池
 *     （vtable type_seal 槽位；去重复用时手工回收本开放类型） */
const type_t *type_const_push(vm_t *vm);
const type_t *type_volatile_push(vm_t *vm);
void type_qual_set_sub(vm_t *vm, const type_t *t, const type_t *sub);
const type_t *type_const_seal(vm_t *vm, const type_t *t);
const type_t *type_volatile_seal(vm_t *vm, const type_t *t);

/* 数组类型构造 API（array_type_push / array_type_set_elem / array_type_set_count /
 * array_type_seal / type_array_intern）与访问器（array_type_elem / array_type_len /
 * array_type_is_sealed / array_type_layout_size / array_type_layout_align）见
 * vm/type_array.h。 */

/* optional 类型构造 API（type_option_push / type_option_set_inner /
 * type_option_seal / type_option_intern）与访问器（type_option_inner /
 * option_value_offset / option_ok / option_value）见 vm/type_option.h。
 * ?T 与 const/volatile 同族：开放构造（PUSH_OPT → DEFINE_TYPE → LOAD_TYPE
 * → SET_TYPE 设 inner → SEAL）获得向前声明能力；sema 侧 type_option_intern
 * 一次性快捷。 */

/** 将 type 转为 value_t*（type 作为 first-class value） */
value_t *type_as_value(vm_t *vm, const type_t *t);

/* ---- 类型 id 表（id → type_t*，LOAD_TYPE <id> 查表压栈） ---- */

/* 类型 id 分段（type.c / sema.c / compile_hoist.c 共用）：
   内建类型固定 id 0..(TYPE_ID_BUILTIN_COUNT-1)（vm_register_builtin_types
   按序登记，error/interrupt 紧随其后）；程序类型 id 由 sema 分配，从
   TYPE_ID_PROGRAM_BASE 起（预留扩展空隙，见 vm.h 注释）。 */
#define TYPE_ID_BUILTIN_COUNT 17u
#define TYPE_ID_PROGRAM_BASE   64u

/**
 * 登记类型到 id 表（vm->types_by_id，索引即 id）。DEFINE_TYPE <id> 运行期
 * 用（类型声明：创建后绑定 id + 登记）；SEAL 密封去重复用时重绑。幂等——
 * 同一类型重复登记（多 id 别名）无害。
 * Panics on out-of-memory.
 */
void vm_type_bind(vm_t *vm, uint32_t id, const type_t *t);

/**
 * 按 id 查类型：id 越界或未登记返回 NULL。LOAD_TYPE <id> 运行期用。
 */
const type_t *vm_type_load(vm_t *vm, uint32_t id);

/**
 * 设置类型的显示名（覆盖默认规范名）。SET_TYPE_NAME 运行期用。
 *
 * 语义：仅对程序类型（id 由编译器分配，>= TYPE_ID_PROGRAM_BASE）生效；
 * 内置类型 name 指向静态存储不可改名，返回 false。旧 name（若为堆分配
 * 的规范名）先释放，再分配 name 的拷贝并替换。
 */
bool type_set_name(vm_t *vm, const type_t *t, strslice_t name);

/**
 * 类型提升（二元运算前协商结果类型）
 *
 * 优先级：f64 > f32 > u64 > i64 > u32 > i32 > u16 > i16 > u8 > i8 > bool
 * - 同类型直接返回
 * - 两个数值类型返回高 rank 的那个
 * - 非数值类型（str/void/type/func/error）或类型不兼容返回 NULL
 */
const type_t *type_promote(const vm_t *vm, const type_t *a, const type_t *b);

/* 函数签名类型构造 API（func_type_push / func_type_add_param /
 * func_type_set_return / func_type_set_variadic / func_type_seal /
 * type_func_sig 一次性快捷）与访问器（func_type_return / func_type_param /
 * func_type_param_count / func_type_is_variadic / func_type_is_sealed）见
 * vm/type_func.h。func type 仅描述签名；函数对象由 func_new /
 * bcode_function_new / BCODE_PUSH_FUNCTION 构造，二者不可混淆。 */

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_TYPE_ */
