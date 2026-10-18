#include "vm/exec.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/value_owncopy.h"  /* value_own_copy_into（构造字段写入） */
#include "vm/type.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_option.h"
#include "vm/type_enum.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/str_pool.h"
#include "vm/type_type.h"
#include "vm/type_interrupt.h"
#include "vm/bcode_function.h"
#include "vm/module.h"
#include "vm/ffi_lib.h"   /* M6 FFI：EXTERN_CALL */
#include "module/module_path.h" /* 运行期 IMPORT 路径重新规范化（VM 层） */
#include "core/panic.h"
#include "core/string.h"
#include "core/vec.h"

#include <string.h>
#include <stdio.h>  /* snprintf */
#include <stdlib.h> /* malloc/free：FFI 返回槽（struct 返回可能大于 16 字节） */

/* ================================================================ */
/* 操作数栈（借用引用，归 scope 管理生命周期）                         */
/* ================================================================ */

void exec_stack_push(vm_t *vm, value_t *v) {
    vec_push(vm->stack, vm->alloc, v);
}

value_t *exec_stack_pop(vm_t *vm) {
    return (value_t *)vec_pop(vm->stack);
}

value_t *exec_stack_peek(const vm_t *vm, size_t offset) {
    size_t sp = vec_len(vm->stack);
    if (offset >= sp) panic("exec: stack underflow (sp=%zu, offset=%zu)", sp, offset);
    return (value_t *)vec_get(vm->stack, sp - 1 - offset);
}

/* ================================================================ */
/* 指令回调                                                          */
/* ================================================================ */

/* ---- 变量访问 ---- */

static value_t *op_push(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *v = scope_lookup(vm->current_scope, name);
    if (!v) return value_make_error(vm, "exec: undefined variable");
    /* **变量读取 = 借用视图**（data 指向变量值的业务内存，ownership=OWN_REF）：
       所有权始终留在变量所属作用域，读取只是"看"它一眼；消费点各自负责
       materialize——DEFINE 经 scope_define 深拷贝、赋值经 value_assign
       就地深拷、RET/传参经 clone。于是"采用 vs 拷贝"只需看 is_borrowed，
       不再需要"该值是否已绑定到名字"的附加标记。
       shadow（sema 侧）与无 data 的值直接返回自身（无业务内存可视图）。 */
    if (value_is_shadow(v) || !value_data(v)) return v;
    return value_make_borrowed(vm, value_type(v), value_data(v));
}

static value_t *op_store(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *src = exec_stack_pop(vm);
    value_t *dst = scope_lookup(vm->current_scope, name);
    if (!dst) return value_make_error(vm, "exec: undefined variable in assignment");
    /* ?T 目标且 src 为 inner T：隐式提升（T → some，与 op_define 同语义）。
       option_assign 本身要求严格同类型，提升在此前置完成（sema 已校验
       src 为 inner，影子短路接受；运行期补齐）。 */
    const type_t *dt = value_type(dst);
    if (dt && dt->kind == TYPE_KIND_OPTION && value_type(src) != dt) {
        value_t *casted = value_implicit_cast(vm, src, dt);
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    /* 赋值统一走 value_assign（深语义：先释放 dst 旧值内嵌 own 块再拷贝）。
       此前的"fresh 临时值整块接管 + scope_untrack"分支会让 value 的归属游离于
       作用域之外（本次所有权重构已删除该轨道）：作用域唯一所有权下，赋值是
       就地更新目标值；临时值仍由本作用域 owned 列表统一释放。 */
    return value_assign(vm, dst, src);
}

/* STORE_REINIT <name>：**重新初始化已 move（TDZ）的变量**（R2 恢复）。
 *
 * move 之后变量内容的所有权已转移（内存里的字节是陈旧值，sema 因 moved 跳过
 * 销毁）。此时 `x = new …` 若走普通 STORE → value_assign →
 * compound_assign_transfer 会按旧字节再释放一遍 own 槽位 → 与接管方重复释放
 * （实测 0xC0000005：`var c = move(b); b = .Box{…};`）。
 * 本指令只做 implicit_cast + memcpy（C 后端 `x = v;` 的等价语义），不触碰旧值。
 * 栈：弹 value → 压回 dst（赋值表达式值）。 */
static value_t *op_store_reinit(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *src = exec_stack_pop(vm);
    value_t *dst = scope_lookup(vm->current_scope, name);
    if (!dst) return value_make_error(vm, "exec: undefined variable in assignment");
    if (value_is_error(vm, src)) return src;
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *dt = value_type(dst);
    if (!dt || !value_data(dst))
        return value_make_error(vm, "exec: reinit requires an initialized variable");
    if (dt->kind == TYPE_KIND_OPTION && value_type(src) != dt) {
        value_t *casted = value_implicit_cast(vm, src, dt);
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_type(src) != dt) {
        value_t *casted = value_implicit_cast(vm, src, dt);
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

/* STORE_NIL <name>：?T 变量置 none（a = nil）。非真实 assign 操作——只置
 * ok tag = false，value 字段不动（nil 无值可写；旧 value 资源归 scope 统一
 * 释放）。压回 dst（赋值表达式值）。 */
static value_t *op_store_nil(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *dst = scope_lookup(vm->current_scope, name);
    if (!dst) return value_make_error(vm, "exec: undefined variable in assignment");
    const type_t *t = value_type(dst);
    if (!t || t->kind != TYPE_KIND_OPTION)
        return value_make_error(vm, "exec: nil assignment requires an optional variable");
    if (value_is_shadow(dst)) return dst;  /* shadow：只算类型 */
    *(bool *)value_data(dst) = false;      /* 只置 ok=false */
    return dst;
}

/* ---- 字面量 ---- */

static value_t *op_push_i8(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int8_t v = bcode_read_i8(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i8, &v);
    return value_make(vm, vm->type_i8, data);
}
static value_t *op_push_i16(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int16_t v = bcode_read_i16(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i16, &v);
    return value_make(vm, vm->type_i16, data);
}
static value_t *op_push_i32(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int32_t v = bcode_read_i32(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i32, &v);
    return value_make(vm, vm->type_i32, data);
}
static value_t *op_push_i64(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int64_t v = bcode_read_i64(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i64, &v);
    return value_make(vm, vm->type_i64, data);
}
static value_t *op_push_u8(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint8_t v = bcode_read_u8(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u8, &v);
    return value_make(vm, vm->type_u8, data);
}
static value_t *op_push_u16(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint16_t v = bcode_read_u16(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u16, &v);
    return value_make(vm, vm->type_u16, data);
}
static value_t *op_push_u32(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t v = bcode_read_u32(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u32, &v);
    return value_make(vm, vm->type_u32, data);
}
static value_t *op_push_u64(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint64_t v = bcode_read_u64(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u64, &v);
    return value_make(vm, vm->type_u64, data);
}
static value_t *op_push_f32(vm_t *vm, bytecode_t *bc, size_t *pc) {
    float v = bcode_read_f32(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_f32, &v);
    return value_make(vm, vm->type_f32, data);
}
static value_t *op_push_f64(vm_t *vm, bytecode_t *bc, size_t *pc) {
    double v = bcode_read_f64(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_f64, &v);
    return value_make(vm, vm->type_f64, data);
}
static value_t *op_push_bool(vm_t *vm, bytecode_t *bc, size_t *pc) {
    bool v = bcode_read_bool(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &v);
    return value_make(vm, vm->type_bool, data);
}
static value_t *op_push_str(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t s = bcode_read_str(bc, pc);
    /* 字符串走 vm 字符串池 intern（data 全平凡：直接存池内指针） */
    const char *sv = vm_str_intern_len(vm, s.ptr, s.len);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_str, &sv);
    return value_make(vm, vm->type_str, data);
}

/* ---- 栈操作 ---- */

static value_t *op_push_value(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t offset = bcode_read_u32(bc, pc);
    return exec_stack_peek(vm, offset); /* offset=0 即 dup 栈顶 */
}

static value_t *op_pop(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    exec_stack_pop(vm); /* 丢弃借用引用，不释放 */
    return NULL;
}

/* ---- 类型与变量定义 ---- */

static value_t *op_load(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *v = scope_lookup(vm->global_scope, name);
    if (!v) return value_make_error(vm, "exec: undefined type");
    return v; /* type value 借用引用 */
}

/* ---- 类型 id 指令（LOAD_TYPE / SEAL / SET_TYPE_NAME） ---- */

/* LOAD_TYPE <id>：从 types_by_id 查表，压入该类型的 type value */
static value_t *op_load_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    const type_t *t = vm_type_load(vm, id);
    if (!t) return value_make_error(vm, "exec: unknown type id");
    return type_as_value(vm, t);
}

/* LOAD_FUNCTION <id>：从 functions_by_id 查表，压入该函数的 func value。
   对标 LOAD_TYPE：函数值运行期加载（变量/参数/返回值传递）。签名类型取自
   func_t->type（func_new 写入，归 vm 类型池所有），包装为 func value——
   data 存 func_t*，type 即签名类型，与 PUSH_FUNCTION 构造的 func value
   同构。 */
static value_t *op_load_function(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    func_t *fn = vm_func_load(vm, id);
    if (!fn || !fn->type)
        return value_make_error(vm, "exec: unknown function id");
    void *data = value_alloc_data_copy(vm->alloc, fn->type, &fn);
    return value_make(vm, fn->type, data);
}

/* MAKE_FUNCTION <id>：从 functions_by_id 拉基底函数对象 → func_instantiate
   实例化独立新实例（新 closure_scope，捕获槽 undefined 占位）→ 压栈。
   函数定义点每次求值调用——循环内 `fns[i] = func|(val=items[i])|...` 三个
   槽位各持独立对象，捕获绑定互不干扰（对标 LOAD_FUNCTION 同一对象语义）。 */
static value_t *op_make_function(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    func_t *base = vm_func_load(vm, id);
    if (!base || !base->type)
        return value_make_error(vm, "exec: unknown function id");
    return func_instantiate(vm, base);
}

/* SET_TYPE_NAME <name>：peek 栈顶 type value（不弹栈），设置其显示名
 * （覆盖规范名）。与 SET_FUNC_NAME 同款 peek 语义——允许后续 DEFINE_TYPE
 * 消费同一 type value。 */
static value_t *op_set_type_name(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *tv = exec_stack_peek(vm, 0);
    if (!tv || value_type(tv) != vm->type_type)
        return value_make_error(vm, "exec: set type name expects a type value");
    const type_t *t = *(const type_t **)value_data(tv);
    if (!type_set_name(vm, t, name))
        return value_make_error(vm, "exec: cannot rename this type");
    return NULL;
}

static value_t *op_push_undefined(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_make_undefined(vm);
}

static value_t *op_define(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);

    /* 永远双弹 [value, type-spec]（value 在底、类型说明符在顶）：
       type-spec = type value（LOAD 压入）或 undefined（PUSH_UNDEFINED 压入） */
    value_t *spec = exec_stack_pop(vm); /* 类型说明符 */
    value_t *init = exec_stack_pop(vm);

    const type_t *decl_type = NULL;
    if (value_type(spec) == vm->type_type) {
        decl_type = value_as(spec, const type_t *);
    }
    /* spec 是 undefined：无显式类型，从值推断。
       fatal 是将亡值——推断时 fatal 复合类型剥离为 OWN_NONE（fatal T → T），
       fatal 指针/切片升级为 OWN_OWN（fatal *T → own *T）。否则变量持有
       fatal 类型，scope 退出时不释放（fatal 释放权在 own 侧）→ 内存泄漏。 */
    if (!decl_type) {
        decl_type = value_type(init);
    }

    /* 无初始值（init 为 void/undefined，来自 var x:T = undefined）：
       以声明类型分配零值占位（sema 确定性赋值分析已保证编译期拦截读取，
       VM 值层不感知未初始化状态） */
    if (value_is_undefined(vm, init)) {
        void *data = value_alloc_data(vm->alloc, decl_type);
        value_t *placeholder = value_make(vm, decl_type, data);
        value_t *stored = scope_define(vm, vm->current_scope, name.ptr, placeholder);
        if (value_is_error(vm, stored)) return stored;
        return NULL;
    }

    /* 显式类型标注（spec 是 type value）且 init 类型不同：按声明类型
       implicit_cast init（sema 已校验转换合法，运行时与 int_assign 同语义；
       var x:i64 = 7 的 7 是 i32 字面量，须拓宽为 i64 后定义，否则运行时
       x 实为 i32，与 sema 符号表类型不一致）。同类型或类型推断时跳过。 */
    if (value_type(spec) == vm->type_type && value_type(init) != decl_type) {
        value_t *casted = value_implicit_cast(vm, init, decl_type);
        if (value_is_error(vm, casted)) return casted;
        init = casted;
    }

    /* 绑定统一走 scope_define：作用域唯一所有权下，init 若已由本作用域拥有
       （即新建的临时值）就被**直接采用**（零拷贝，own 复合类型因此不会与临时
       共享内嵌堆块）；借用视图（`var b = a` 压入的是 a 的借用视图）才在
       scope_define 内 materialize 成独立深拷贝。原先"fresh 才接管、否则 clone"
       的双轨（scope_define_owned + is_fresh 标记）已随本次重构删除。 */
    value_t *stored = scope_define(vm, vm->current_scope, name.ptr, init);
    if (value_is_error(vm, stored)) return stored;
    return NULL;
}

/* ---- 二元/一元运算 ---- */

#define BINARY_OP(name, value_fn)                                              \
    static value_t *op_##name(vm_t *vm, bytecode_t *bc, size_t *pc) {         \
        (void)bc; (void)pc;                                                    \
        value_t *b = exec_stack_pop(vm);                                       \
        value_t *a = exec_stack_pop(vm);                                       \
        return value_fn(vm, a, b);                                             \
    }

BINARY_OP(add, value_add)   BINARY_OP(sub, value_sub)   BINARY_OP(mul, value_mul)
BINARY_OP(div, value_div)   BINARY_OP(mod, value_mod)
BINARY_OP(eq, value_eq)     BINARY_OP(ne, value_ne)     BINARY_OP(lt, value_lt)
BINARY_OP(le, value_le)     BINARY_OP(gt, value_gt)     BINARY_OP(ge, value_ge)
BINARY_OP(band, value_band) BINARY_OP(bor, value_bor)   BINARY_OP(bxor, value_bxor)
BINARY_OP(shl, value_shl)   BINARY_OP(shr, value_shr)

static value_t *op_neg(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_neg(vm, exec_stack_pop(vm));
}
static value_t *op_not(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_lnot(vm, exec_stack_pop(vm));
}
static value_t *op_bnot(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_bnot(vm, exec_stack_pop(vm));
}

/* ---- 显式转换 ---- */

static value_t *op_cast(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* 类型经栈顶 type value 传递（LOAD 压入），无立即数操作数：
       弹 type value → 弹被转换值 → value_explicit_cast（error 内部短路） */
    (void)bc; (void)pc;
    value_t *vtype = exec_stack_pop(vm);
    value_t *value = exec_stack_pop(vm);
    const type_t *target = *(const type_t **)value_data(vtype);
    return value_explicit_cast(vm, value, target);
}

/* ---- 限定类型构造（const/volatile） ---- */

static value_t *op_create_const(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* 弹 type value → intern const 类型 → type value 压回 */
    (void)bc; (void)pc;
    value_t *vtype = exec_stack_pop(vm);
    const type_t *t = *(const type_t **)value_data(vtype);
    const type_t *ct = type_const_intern(vm, t);
    if (!ct) return value_make_error(vm, "exec: cannot make const type");
    return type_as_value(vm, ct);
}

static value_t *op_create_volatile(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *vtype = exec_stack_pop(vm);
    const type_t *t = *(const type_t **)value_data(vtype);
    const type_t *vt = type_volatile_intern(vm, t);
    if (!vt) return value_make_error(vm, "exec: cannot make volatile type");
    return type_as_value(vm, vt);
}

/* ---- 限定类型开放构造（PUSH_CONST/PUSH_VOLATILE + SET_TYPE + SEAL） ---- */

/* PUSH_CONST：分配空 const type（开放，sub=NULL，不入池）+ 压其 type value
 * （type_const_push 压栈；对应两遍构造声明阶段的起点） */
static value_t *op_push_const(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_const_push(vm);
    return NULL;
}

/* PUSH_VOLATILE：分配空 volatile type（开放，sub=NULL，不入池）+ 压其 type value */
static value_t *op_push_volatile(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_volatile_push(vm);
    return NULL;
}

/* PUSH_OPT：分配空 optional type（开放，inner=NULL，不入池）+ 压其 type value
 * （type_option_push 压栈；对应两遍构造声明阶段的起点） */
static value_t *op_push_opt(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_option_push(vm);
    return NULL;
}

/* PUSH_ENUM：分配空 enum type（开放，underlying=NULL，不入池）+ 压其 type
 * value（type_enum_push 压栈；对应两遍构造声明阶段的起点） */
static value_t *op_push_enum(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_enum_push(vm);
    return NULL;
}

/* ENUM_VARIANT <name> <value>：peek 栈顶开放 enum type（不弹栈——后续
 * ENUM_VARIANT / SEAL 继续消费），追加一个 variant（名从 strtable 拷贝，
 * 值按底层宽度截断）。 */
static value_t *op_enum_variant(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    int64_t value = bcode_read_i64(bc, pc);
    value_t *ev = exec_stack_peek(vm, 0);
    const type_t *open = (ev && value_type(ev) == vm->type_type)
                             ? value_as(ev, const type_t *) : NULL;
    if (!open || open->kind != TYPE_KIND_ENUM)
        return value_make_error(vm, "exec: enum variant expects an open enum type on top");
    type_enum_add_variant(vm, open, name, value);
    return NULL;
}

/* MAKE_ENUM：弹 type value + 弹底层整数值 → 构造 enum 类型 value（data 为
 * 底层宽度的整数值块，按 underlying->size 截断）。 */
static value_t *op_make_enum(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    /* 协议与 op_cast 一致：整数值在栈底、类型 value 在栈顶（compiler 发
       PUSH_I*; LOAD_TYPE），先弹类型再弹值。 */
    value_t *vtype = exec_stack_pop(vm);
    value_t *value = exec_stack_pop(vm);
    const type_t *t = (vtype && value_type(vtype) == vm->type_type)
                          ? value_as(vtype, const type_t *) : NULL;
    if (!t || t->kind != TYPE_KIND_ENUM)
        return value_make_error(vm, "exec: make enum expects an enum type");
    if (value_is_shadow(value))
        return value_make_shadow(vm, t);
    /* 读底层整数值（源 value 类型可能为 i32 字面量等），按底层宽度截断 */
    const type_t *u = enum_type_underlying(t);
    if (!u) return value_make_error(vm, "exec: enum has no underlying type");
    int64_t iv = 0;
    switch (value_type(value)->size) {
    case 1: iv = (int64_t)*(const int8_t  *)value_data(value); break;
    case 2: iv = (int64_t)*(const int16_t *)value_data(value); break;
    case 4: iv = (int64_t)*(const int32_t *)value_data(value); break;
    default: iv = *(const int64_t *)value_data(value); break;
    }
    void *data = value_alloc_data(vm->alloc, t);
    switch (u->size) {
    case 1: *(int8_t  *)data = (int8_t)iv;  break;
    case 2: *(int16_t *)data = (int16_t)iv; break;
    case 4: *(int32_t *)data = (int32_t)iv; break;
    default: *(int64_t *)data = iv;         break;
    }
    return value_make(vm, t, data);
}

/* SET_TYPE：弹栈顶 sub type value → peek 栈顶开放对象 → 设为 sub。
 * 弹 sub 后，栈顶即当前构造的限定类型（由 PUSH_CONST/PUSH_VOLATILE 压入），
 * 与 DEFINE_BOUND 同款协议。 */
static value_t *op_set_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *sub_v = exec_stack_pop(vm);
    const type_t *sub = *(const type_t **)value_data(sub_v);
    value_t *open_v = exec_stack_peek(vm, 0);
    const type_t *open = (open_v && value_type(open_v) == vm->type_type)
                             ? value_as(open_v, const type_t *) : NULL;
    if (open && open->kind == TYPE_KIND_OPTION) {
        type_option_set_inner(vm, open, sub);  /* option：设 inner */
        return NULL;
    }
    if (open && open->kind == TYPE_KIND_ENUM) {
        type_enum_set_underlying(vm, open, sub);  /* enum：设底层类型 */
        return NULL;
    }
    if (open && type_is_ptr(open)) {
        type_ptr_set_base(vm, open, sub);  /* 指针：设被指向类型 T */
        return NULL;
    }
    if (open && type_is_slice(open)) {
        type_slice_set_elem(vm, open, sub);  /* 切片：设元素类型 T */
        return NULL;
    }
    type_qual_set_sub(vm, open, sub);
    return NULL;
}

/* ---- func type 构造（与 array type 统一：PUSH → SET → SEAL） ---- */

/* PUSH_FUNC_TYPE：分配空 func type 入池并压其 type value */
static value_t *op_push_func_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    func_type_push(vm);  /* 压入 func type 的 type value（见 func_type_push） */
    return NULL;
}

/* 取栈顶的 func type（调用前已确保其为当前构造目标，位于栈顶） */
static const type_t *stack_top_func_type(vm_t *vm) {
    value_t *ftv = exec_stack_peek(vm, 0);
    return (ftv && value_type(ftv) == vm->type_type)
               ? value_as(ftv, const type_t *)
               : NULL;
}

/* FUNC_TYPE_PARAM：弹栈 type value → 追加为下一参数 */
static value_t *op_func_type_param(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *pv = exec_stack_pop(vm);
    const type_t *param = *(const type_t **)value_data(pv);
    const type_t *ft = stack_top_func_type(vm);
    func_type_add_param(vm, ft, param);
    return NULL;
}

/* FUNC_TYPE_RETURN：弹栈 type value → 设为返回类型 */
static value_t *op_func_type_return(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *rv = exec_stack_pop(vm);
    const type_t *ret = *(const type_t **)value_data(rv);
    const type_t *ft = stack_top_func_type(vm);
    func_type_set_return(vm, ft, ret);
    return NULL;
}

/* FUNC_TYPE_VARARG：标记可变参数 */
static value_t *op_func_type_vararg(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    const type_t *ft = stack_top_func_type(vm);
    func_type_set_variadic(vm, ft, true);
    return NULL;
}

/* DEFINE_TYPE <id>：**类型声明**。弹栈顶 type value → 绑定程序 id
 * （>= TYPE_ID_PROGRAM_BASE 才写 t->id；sema 路径已设 t->id，此处幂等
 * 冗余；asm 手写路径依赖此同步）→ 登记 id→type 进 types_by_id（幂等；
 * 多 id 别名同一 type_t）。此后 LOAD_TYPE <id> 可拉回该类型（开放或
 * 密封均可）。两遍扫描 pass 1 对所有程序类型（数组 / 签名 / const /
 * volatile 开放对象）发本指令声明登记；内建别名（无开放构造阶段）
 * LOAD_TYPE <内建 id> 拉回已密封实例后 DEFINE_TYPE <id> 一步登记。 */
static value_t *op_define_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    value_t *tv = exec_stack_pop(vm);
    if (!tv || value_type(tv) != vm->type_type)
        return value_make_error(vm, "exec: define type expects a type value");
    const type_t *t = value_as(tv, const type_t *);
    if (id >= TYPE_ID_PROGRAM_BASE) ((type_t *)t)->id = id;
    vm_type_bind(vm, id, t);
    return NULL;
}

/* SEAL（无操作数）：**类型定义收尾**。**消费**栈顶 type value——弹栈 →
 * value_seal 密封（vtable->type_seal 去重 intern + 布局计算；幂等，无开放
 * 构造阶段的内建类型原样返回）→ 密封前读开放对象自身 id（DEFINE_TYPE
 * 已登记），密封后按该 id 幂等更新登记——去重复用时 value_seal 已把栈
 * 引用重定向到缓存实例，此处同步重绑 types_by_id[id]，避免登记表指向
 * 已被回收的开放对象。统一 SEAL 命令，func/array/const/volatile/struct/
 * tuple 通用；密封后栈被清理（消费），需要类型时由后续 LOAD_TYPE <id>
 * 主动拉取，不留残值在栈上。 */
static value_t *op_seal(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *tv = exec_stack_pop(vm);
    if (!tv || value_type(tv) != vm->type_type)
        return value_make_error(vm, "exec: seal expects a type value");
    const type_t *open = value_as(tv, const type_t *);
    uint32_t id = (open && open->id >= TYPE_ID_PROGRAM_BASE) ? open->id : 0;
    value_seal(vm, tv); /* 密封（去重时 value_seal 已重定向 tv->data） */
    const type_t *t = value_as(tv, const type_t *);
    if (id != 0) vm_type_bind(vm, id, t); /* 幂等更新登记（去重 → 重绑新实例） */
    return NULL;
}

/* ---- array type 构造（与 func type 统一：PUSH → SET → SEAL） ---- */

/* PUSH_ARRAY：分配空 array type（开放，暂不入池）+ 压其 type value 到栈 */
static value_t *op_push_array(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    array_type_push(vm);  /* 压入 array type 的 type value（见 array_type_push） */
    return NULL;
}

/* DEFINE_BOUND N：弹栈顶元素 type value → 设为元素类型，边界立即数 N 设为长度。
 * 弹元素类型后，栈顶即当前构造的 array type（由 PUSH_ARRAY 压入）。 */
static value_t *op_define_bound(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t count = bcode_read_u32(bc, pc);
    value_t *elem_v = exec_stack_pop(vm);
    const type_t *elem = *(const type_t **)value_data(elem_v);
    value_t *arr_v = exec_stack_peek(vm, 0);
    const type_t *arr = (arr_v && value_type(arr_v) == vm->type_type)
                            ? value_as(arr_v, const type_t *) : NULL;
    array_type_set_elem(vm, arr, elem);
    array_type_set_count(vm, arr, count);
    return NULL;
}

/* ---- struct type 构造（与 array type 统一：PUSH → DEFINE_FIELD×N → SEAL） ---- */

/* PUSH_STRUCT：分配空 struct type（开放，fields=NULL，不入池）+ 压其 type
 * value（type_struct_push 压栈；对应两遍构造声明阶段的起点） */
static value_t *op_push_struct(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_struct_push(vm);
    return NULL;
}

/* DEFINE_FIELD <name>：弹栈顶 type value（字段类型）→ peek 开放 struct →
 * 追加字段（名从 strtable 拷贝）。弹字段类型后，栈顶即当前构造的 struct
 * type（由 PUSH_STRUCT 压入），与 DEFINE_BOUND 同款协议。
 * union 分支：栈顶是开放 union 时，字段追加到当前（最后追加）member 的
 * 开放 payload struct（type_union_add_field 内部处理）。 */
static value_t *op_define_field(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *ft_v = exec_stack_pop(vm);
    const type_t *ft = *(const type_t **)value_data(ft_v);
    value_t *st_v = exec_stack_peek(vm, 0);
    const type_t *st = (st_v && value_type(st_v) == vm->type_type)
                           ? value_as(st_v, const type_t *) : NULL;
    if (!st) return value_make_error(vm, "exec: define field expects an open type on top");
    if (st->kind == TYPE_KIND_STRUCT) {
        type_struct_add_field(vm, st, name, ft);
        return NULL;
    }
    if (st->kind == TYPE_KIND_UNION) {
        type_union_set_member_type(vm, st, ft);
        return NULL;
    }
    if (st->kind == TYPE_KIND_CUNION) {
        type_cunion_add_member(vm, st, name);
        type_cunion_set_member_type(vm, st, ft);
        return NULL;
    }
    return value_make_error(vm, "exec: define field expects an open struct, union or cunion type on top");
}

/* SET_FIELD_OWNERSHIP <u8 own>：把栈顶开放类型**最后追加**的字段 / 元素 /
 * member 的槽位所有权设为立即数（seal 后忽略；无弹栈）。
 *
 * 为什么需要：DEFINE_FIELD 只带字段名/类型，运行期构造的类型对象槽位所有权
 * 恒为 OWN_NONE；而"就地覆盖旧值"的运行期路径（FIELD_SET / 复合赋值 /
 * DISPOSE）按槽位所有权决定释放什么（own 叶子释放、ref 借用跳过）。缺了它
 * own 槽位会被当成普通字段（泄漏），ref 借用目标会被误释放（悬垂 free）。
 * 具名 struct 还带 MARK_UNIQUE（seal 跳过去重），也不能靠"去重到 sema 那份
 * 带 ownership 的类型"来补。 */
static value_t *op_set_field_ownership(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint8_t own = bcode_read_u8(bc, pc);
    value_t *tv = exec_stack_peek(vm, 0);
    const type_t *t = (tv && value_type(tv) == vm->type_type)
                          ? value_as(tv, const type_t *) : NULL;
    if (!t)
        return value_make_error(vm,
            "exec: set field ownership expects an open type on top");
    switch (t->kind) {
    case TYPE_KIND_STRUCT:
        type_struct_set_last_field_ownership(vm, t, (ownership_t)own);
        return NULL;
    case TYPE_KIND_UNION:
        type_union_set_last_member_ownership(vm, t, (ownership_t)own);
        return NULL;
    case TYPE_KIND_CUNION:
        type_cunion_set_last_member_ownership(vm, t, (ownership_t)own);
        return NULL;
    case TYPE_KIND_ARRAY:
        array_type_set_elem_ownership(vm, t, (ownership_t)own);
        return NULL;
    case TYPE_KIND_SLICE:
        type_slice_set_elem_ownership(vm, t, (ownership_t)own);
        return NULL;
    case TYPE_KIND_OPTION:
        type_option_set_inner_ownership(vm, t, (ownership_t)own);
        return NULL;
    default:
        return NULL;   /* 其它类型无槽位表：防御性忽略 */
    }
}

/* ADD_EMBED <embed_type_id>：peek 栈顶开放 struct → vm_type_load 拉回 embed
 * 类型 → type_struct_add_embed 追加到 embeds 表（seal 前可编辑）。
 * 无栈深变化（peek 不弹）。逐个追加，与 DEFINE_FIELD 同构。 */
static value_t *op_add_embed(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t eid = bcode_read_u32(bc, pc);
    value_t *st_v = exec_stack_peek(vm, 0);
    const type_t *st = (st_v && value_type(st_v) == vm->type_type)
                           ? value_as(st_v, const type_t *) : NULL;
    if (!st || st->kind != TYPE_KIND_STRUCT)
        return value_make_error(vm, "exec: add embed expects an open struct on top");
    const type_t *embed = vm_type_load(vm, eid);
    if (!embed || embed->kind != TYPE_KIND_STRUCT)
        return value_make_error(vm, "exec: add embed: invalid embed type id");
    if (!type_struct_add_embed(vm, st, embed))
        return value_make_error(vm, "exec: add embed failed (sealed?)");
    return NULL;
}

/* MARK_UNIQUE（无操作数）：peek 栈顶开放 struct → type_struct_mark_unique
 *（seal 前调用，seal 跳过去重 intern——命名 struct 不与字段表相同的既有
 * struct 合并）。无栈深变化。 */
static value_t *op_mark_unique(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *st_v = exec_stack_peek(vm, 0);
    const type_t *st = (st_v && value_type(st_v) == vm->type_type)
                           ? value_as(st_v, const type_t *) : NULL;
    if (!st || st->kind != TYPE_KIND_STRUCT)
        return value_make_error(vm, "exec: mark unique expects an open struct on top");
    if (!type_struct_mark_unique(vm, st))
        return value_make_error(vm, "exec: mark unique failed (sealed?)");
    return NULL;
}

/* ---- tuple type 构造（与 struct 统一：PUSH → APPEND_ELEM×N → SEAL） ---- */

/* PUSH_TUPLE：分配空 tuple type（开放，elems=NULL，不入池）+ 压其 type
 * value（type_tuple_push 压栈；对应两遍构造声明阶段的起点） */
static value_t *op_push_tuple(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_tuple_push(vm);
    return NULL;
}

/* APPEND_ELEM（无操作数）：弹栈顶 type value（元素类型）→ peek 开放 tuple →
 * 追加元素（元素匿名，长度=追加次数）。弹元素类型后，栈顶即当前构造的
 * tuple type（由 PUSH_TUPLE 压入），与 DEFINE_FIELD/DEFINE_BOUND 同款协议。 */
static value_t *op_append_elem(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *et_v = exec_stack_pop(vm);
    const type_t *et = (et_v && value_type(et_v) == vm->type_type)
                           ? value_as(et_v, const type_t *) : NULL;
    value_t *tt_v = exec_stack_peek(vm, 0);
    const type_t *tt = (tt_v && value_type(tt_v) == vm->type_type)
                           ? value_as(tt_v, const type_t *) : NULL;
    if (!tt || tt->kind != TYPE_KIND_TUPLE)
        return value_make_error(vm, "exec: append elem expects an open tuple type on top");
    type_tuple_add_elem(vm, tt, et);
    return NULL;
}

/* ---- union type 构造（与 struct 统一：PUSH → UNION_MEMBER×N → SEAL） ---- */

/* PUSH_UNION：分配空 union type（开放，members=NULL，不入池）+ 压其 type
 * value（type_union_push 压栈；对应两遍构造声明阶段的起点） */
static value_t *op_push_union(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_union_push(vm);
    return NULL;
}

/* PUSH_CUNION：分配空 cunion type（开放，members=NULL，不入池）+ 压其 type
 * value（type_cunion_push 压栈；对应两遍构造声明阶段的起点） */
static value_t *op_push_cunion(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_cunion_push(vm);
    return NULL;
}

/* UNION_MEMBER <name>：peek 栈顶开放 union type（不弹栈——后续 UNION_MEMBER /
 * DEFINE_FIELD / SEAL 继续消费），追加一个 member（名从 strtable 拷贝，
 * tag = 追加序）。 */
static value_t *op_union_member(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *uv = exec_stack_peek(vm, 0);
    const type_t *u = (uv && value_type(uv) == vm->type_type)
                          ? value_as(uv, const type_t *) : NULL;
    if (!u || u->kind != TYPE_KIND_UNION)
        return value_make_error(vm, "exec: union member expects an open union type on top");
    type_union_add_member(vm, u, name);
    return NULL;
}

/* ---- 指针值级指令（M3，m3-design §3/§8.1/§8.2） ----
   类型构造（PUSH_PTR → SET_TYPE → SEAL）见 op_set_type PTR 分支。
   NEW：堆分配构造（new T{...}）；PTR_GET/PTR_SET：解引用 GET/SET 严格分离
   （r.* / r.* = v）；ADDR：后置取址（x.&）；MOVE/CLONE：所有权原语。
   指针值 data 布局 = ptr_value_t { void *ptr; }（type_ptr.h），运行期零标志
   （m3-design §13.2），所有权由 type kind 区分：
     - NEW 产物：fatal *T，ptr = 堆块（接管为 own 后释放）
     - ADDR 产物：ref *T，ptr = 栈上值 data（借用，不释放）
   MOVE = 转移（O(1)，源 ptr=NULL TDZ，产物 fatal）；CLONE = 深拷贝（own 分配
   新堆块，产物 fatal；ref 复制指针值）。 */

/* PUSH_PTR：分配空 ptr type（开放，base_type=NULL，不入池）+ 压其
 * type value（type_ptr_push 压栈；对应两遍构造声明阶段的起点）。
 * 所有权纯编译期——只有一种指针类型 *T，无 ownership 操作数。 */
static value_t *op_push_ptr(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_ptr_push(vm);
    return NULL;
}

/* NEW（无操作数）：**堆分配构造**（new T{...}，m3-design §8.1）。
 * 栈布局 [member, type_value]（type_value 在顶）：先弹类型位，再弹 1 个成员值。
 * 按 T 的类型构造堆块（分配 size = T->size 的裸数据块，blit 成员值）：
 *   - 标量等无字段类型（new i32{123}）：member 值 implicit_cast → T 后拷贝
 *   - 复合类型（struct/tuple/array/option/union/cunion）：member 值是嵌套
 *     CONSTRUCT 已构造好的 T 值 → 整块 memcpy（data 全平凡；struct 字段/
 *     tuple 元素已是构造产物）
 * 成员值类型与 T 同构时身份直接收下（sema 已校验字段）；不同类型 implicit_cast。
 * 返回 fatal *T 类型 value（ptr_value_t{堆块}——接管后变 own，作用域退出时
 * ptr_dispose 释放堆块，m3-design §3.1/§3.3）。 */
static value_t *op_new(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* NEW（无操作数）：**只申请内存**，构造交给 CONSTRUCT（2026-10 统一）。
     * 弹一个整数（字节数）→ 申请该大小的原始块（清零）→ 压 opaque 句柄
     * （data 里存裸指针）。后续 `CONSTRUCT` 以该句柄为 dest，在这块内存上
     * 构造对象。C 后端等价：`void *blk = calloc(1, size);`。
     * 与 CONSTRUCT 分离后，"申请"与"逐字段赋值"各自可翻译——这是 C 后端
     * 等价编译的关键（同 FREE 的理由）。 */
    (void)bc; (void)pc;
    value_t *size_v = exec_stack_pop(vm);
    if (size_v && value_is_shadow(size_v))
        return value_make_shadow(vm, vm->type_opaque);
    uint64_t size = 0;
    if (size_v && value_data(size_v)) {
        const type_t *st = value_type(size_v);
        void *sd = value_data(size_v);
        int64_t sv = 0;
        switch (st->size) {
        case 1: sv = *(const int8_t  *)sd; break;
        case 2: sv = *(const int16_t *)sd; break;
        case 4: sv = *(const int32_t *)sd; break;
        default: sv = *(const int64_t *)sd; break;
        }
        size = (uint64_t)(sv < 0 ? 0 : sv);
    }
    void *blk = size > 0 ? value_alloc_raw(vm->alloc, (size_t)size) : NULL;
    void *data = value_alloc_data(vm->alloc, vm->type_opaque);
    if (!data) return value_make_error(vm, "new: out of memory");
    *(void **)data = blk;
    return value_make(vm, vm->type_opaque, data);
}
/* PTR_GET（无操作数）：**解引用取值**（r.*，m3-design §8.2）。
 * 弹指针值 → 取被指向 T 值副本（data 指向 ptr 目标，借用到 T 值；零拷贝——
 * 引用的是堆块/栈值业务内存，生命周期 = 持有者）。shadow → base 的 shadow。 */
static value_t *op_ptr_get(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *pv = exec_stack_pop(vm);
    const type_t *pt = pv ? value_type(pv) : NULL;
    const type_t *base = ptr_type_base(pt);
    if (!base)
        return value_make_error(vm,
            "exec: ptr get expects a pointer value on stack");
    if (value_is_shadow(pv))
        return value_make_shadow(vm, base);
    void *target = ptr_value_target(pv);
    if (!target)
        return value_make_error(vm,
            "exec: ptr get on null pointer (dereference of uninitialized pointer)");
    return value_make_borrowed(vm, base, target);
}

/* PTR_SET（无操作数）：**解引用写入**（r.* = v，m3-design §8.2）。
 * 栈布局 [ptr, val]（val 在顶）：弹 val、ptr → val implicit_cast → T →
 * 拷贝到指针指向的 T 值（覆盖目标）→ 压回指针（表达式值，供链式复用）。
 * shadow → 返回指针 shadow（sema 只做类型检查）。 */
static value_t *op_ptr_set(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *val = exec_stack_pop(vm);
    value_t *pv  = exec_stack_pop(vm);
    const type_t *pt = pv ? value_type(pv) : NULL;
    const type_t *base = ptr_type_base(pt);
    if (!base)
        return value_make_error(vm,
            "exec: ptr set expects a pointer value on stack");
    if (value_is_shadow(pv))
        return pv;
    void *target = ptr_value_target(pv);
    if (!target)
        return value_make_error(vm,
            "exec: ptr set on null pointer (write through uninitialized pointer)");
    value_t *casted = value_implicit_cast(vm, val, base);
    if (value_is_error(vm, casted)) return casted;
    memcpy(target, value_data(casted), base->size);  /* data 全平凡：覆盖 T */
    return pv;
}

/* ADDR（无操作数）：**后置取地址**（x.&，m3-design §8.2）。
 * 弹值 → 返回 ref *T 指针（ptr_value_t{指向值 data 块}——
 * 借用指向栈上值，ref 总借用不释放，防释放栈值。与 sema AST_ADDR 返回
 * ref *T 一致，m3-design §13.2 运行期零标志）。shadow → 指针 shadow。 */
static value_t *op_addr(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    const type_t *t = v ? value_type(v) : NULL;
    if (!t || t->kind == TYPE_KIND_VOID || t->kind == TYPE_KIND_ERROR)
        return value_make_error(vm, "addr: cannot take address of void/error value");
    const type_t *pt = type_ptr_intern(vm, t);
    if (!pt)
        return value_make_error(vm, "addr: cannot make pointer type");
    if (value_is_shadow(v))
        return value_make_shadow(vm, pt);
    return ptr_make_value(vm, pt, value_data(v)); /* ref 借用：不拥有 */
}

/* 注：曾经有过 compound_fatal_variant（把复合值包成运行期 fatal 变体类型）。
   已删除——运行期不为同一逻辑类型维护两个实例：fatal 是**编译期**的所有权
   标记（sema 的类型解析据此产将亡值），运行期的转移在"转移点"按语义完成
   （见 value_move / func_vcall 的 fatal 形参分支）。保留 `SET_OWNERSHIP`
   指令与 `type_*_intern_with_ownership` 供 hoist 物化声明里的 fatal 类型。 */

/* MOVE（无操作数）：**所有权转移**（move(x)，m3-design §7）。
 * 所有权纯编译期——move 没有运行期逻辑，只是 sema 把变量的 ownership
 * 从 own 标记为 fatal。运行期 MOVE = 纯 memcpy（位复制），不清空源指针。
 * 堆释放由 sema 生成 FREE 指令负责。
 * shadow → 同类型 shadow。 */
static value_t *op_move(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return value_clone(vm, v);
    /* 纯 memcpy：位复制，不清空源。 */
    void *data = value_alloc_data(vm->alloc, t);
    memcpy(data, value_data(v), t->size);
    return value_make(vm, t, data);
}

/* CLONE（无操作数）：**深拷贝**（clone(x)，m3-design §7）。
 * 所有权纯编译期——clone 做深拷贝（ptr_clone_block 对象图克隆），
 * 不再包 fatal 类型、不清空源指针。
 * shadow → 同类型 shadow。 */
static value_t *op_clone(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return value_clone(vm, v);
    /* 深拷贝：对象图克隆（指针分配新堆块，复合类型逐字段递归）。 */
    void *data = ptr_clone_block(vm, t, value_data(v));
    return value_make(vm, t, data);
}

/* value_move（供 MOVE 指令与函数调用/返回的**所有权转移**共用，m3-design §7）：
 * 所有权纯编译期——move = 纯 memcpy（位复制），不清空源指针。
 * 堆释放由 sema 生成 FREE 指令负责。 */
value_t *value_move(vm_t *vm, value_t *v) {
    if (!v) return v;
    if (value_is_shadow(v)) return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return v;
    void *data = value_alloc_data(vm->alloc, t);
    memcpy(data, value_data(v), t->size);
    return value_make(vm, t, data);
}

/* DISPOSE（无操作数）：**显式丢弃**（`_ = expr`，m3-design §3.3）。
 * 弹值 → 递归释放值内部所有 own 指针指向的堆块（ptr_free_owned_recursive
 * 同款静态分派：own/fatal + 含 own 的复合类型；ref/借用跳过），随后按
 * 常规 POP 语义丢弃该值（不释放 data 块——值本体仍归 scope owned 列表，
 * 作用域退出统一销毁，防双释放）。不压回（表达式语句消费）。
 *
 * 语义：`_ = move(x)` / `_ = new T{...}` 等将亡值（fatal）被显式丢弃——
 * 无接管方时在此递归释放其堆块，避免 leak；源 data 内 ptr 随释放
 * 置 NULL，作用域退出扫描看到 ptr=NULL 跳过（防双释放）。借用
 * （ref）无堆块可释，no-op。 */
static value_t *op_dispose(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (!v || value_is_shadow(v)) return NULL;
    const type_t *t = value_type(v);
    if (!t) return NULL;
    /* 借用引用 data 指向父值内部（如 arr[i]），递归释放会误伤父值——
       跳过（ref 借用无堆块）。仅拥有 data 的值（非借用）其内嵌 own
       堆块才属本值所有。 */
    if (!value_is_borrowed(v))
        ptr_free_owned_recursive(vm, t, value_data(v));
    return NULL;
}

/* FREE：释放**栈顶的值**（无操作数）。
 *
 * sema 在作用域退出点为每个 own 变量生成 `PUSH <name>` + `FREE` 两条指令
 * ——变量解析由 PUSH 完成，本指令只管"释放栈顶这个块"。
 *
 * **不做所有权判定**：只有 own 变量才会被生成这对指令，这是编译期事实；
 * 对借用值释放是无意义的（其 data 指向父值内部），而 sema 不会那样生成。
 *
 * 只释放堆资源（ptr_free_owned_recursive），不释放 value_t 壳与 data 块——
 * 那些由 POP_SCOPE 的 scope 结构释放统一处理。栈深 -1。
 *
 * 注：PUSH 给出的是**借用视图**（data 指向变量本体的同一内存），而
 * ptr_free_owned_recursive 就地把 ptr 置 NULL，所以释放会作用到变量本体，
 * 作用域退出时不会二次释放。 */
static value_t *op_free(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (!v || value_is_shadow(v)) return NULL;
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return NULL;
    /* A 方案：sema 已按 type 对象生成"被指向值 own 槽位"的逐条 FREE，
       这里只释放**这一个**堆块，不再按类型递归——否则会把 ref 槽位指向的
       借用目标也 free 掉（实测 0xC0000005）。 */
    ptr_free_block_only(vm, t, value_data(v));
    return NULL;
}

/* FREE_RECURSIVE：**自引用类型**的销毁边。
 * 弹指针值 → 按槽位所有权递归释放**被指向值**的 own 槽位（与
 * ptr_free_owned_recursive 同一权威），再释放块本体。
 * 普通类型由 sema 逐槽位静态展开成 FREE 序列（flat，C 后端可直接翻译）；
 * 只有链表/树这类编译期无法穷尽的环才退化为本指令（运行期递归）。 */
static value_t *op_free_recursive(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (!v || value_is_shadow(v)) return NULL;
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return NULL;
    if (t->kind == TYPE_KIND_PTR) {
        ptr_value_t *pv = (ptr_value_t *)value_data(v);
        const type_t *base = ptr_type_base(t);
        if (pv->ptr && base) ptr_free_owned_recursive(vm, base, pv->ptr);
    }
    /* 块本体照常释放（ptr 释放被指向块 / slice 释放缓冲） */
    ptr_free_block_only(vm, t, value_data(v));
    return NULL;
}

/* ---- M4 切片段（m4-design §1/§2/§4） ---- */

/* PUSH_SLICE：分配空 slice type（开放，elem=NULL，不入池）+ 压其
 * type value（type_slice_push 压栈；对应两遍构造声明阶段的起点）。
 * 所有权纯编译期——只有一种切片类型 []T，无 ownership 操作数。
 * 与 op_push_ptr 同构。 */
static value_t *op_push_slice(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_slice_push(vm);
    return NULL;
}

/* SLICE（无操作数）：**切片表达式** s[low:high]（m4-design §4）。
 * 栈布局 [self, high, low]（low 在顶）：先弹 low、再弹 high、再弹 self →
 * 压 ref []T 胖指针（借用，指向 self 内部元素序列首址 + (high-low) 长度）。
 * self 可为数组 / 切片 / str：
 *   - 数组 [N]T：data 内联元素序列，ptr = data + low*elem_size
 *   - 切片 []T：data 是 slice_value_t{ptr, len}，ptr = sv.ptr + low*elem_size
 *   - str：data 是 const char*，ptr = str + low（字节切片，elem=u8）
 * 边界由 sema 静态校验（low <= high <= len），运行期直接计算偏移。
 * shadow → ref []T shadow（sema 只做类型检查）。
 * u8 flags 操作数：bit 0 = low present, bit 1 = high present。
 * 省略的参数不压栈，op_slice 按 flags 决定弹栈次数。 */
static value_t *op_slice(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint8_t flags = bcode_read_u8(bc, pc);
    bool has_low  = (flags & 0x01) != 0;
    bool has_high = (flags & 0x02) != 0;
    value_t *low_v  = has_low  ? exec_stack_pop(vm) : NULL;
    value_t *high_v = has_high ? exec_stack_pop(vm) : NULL;
    value_t *self   = exec_stack_pop(vm);
    const type_t *st = self ? value_type(self) : NULL;
    if (!st)
        return value_make_error(vm, "exec: slice expects a value on stack");

    /* 解析元素类型 + 源指针 + 源长度 */
    const type_t *elem = NULL;
    const void *base_ptr = NULL;
    uint64_t src_len = 0;
    if (st->kind == TYPE_KIND_ARRAY) {
        elem = array_type_elem(st);
        if (!value_is_shadow(self)) base_ptr = value_data(self);
        src_len = array_type_len(st);
    } else if (type_is_slice(st)) {
        elem = slice_type_elem(st);
        if (!value_is_shadow(self)) {
            slice_value_t sv = slice_view(self);
            base_ptr = sv.ptr;
            src_len = sv.len;
        }
    } else if (st == vm->type_str) {
        /* str 字节切片：elem = const u8（元素不可写，m4-design §8.3） */
        elem = type_const_intern(vm, vm->type_u8);
        if (!value_is_shadow(self)) {
            base_ptr = *(const char *const *)value_data(self);
            src_len = strlen((const char *)base_ptr);
        }
    } else {
        return value_make_error(vm,
            "exec: slice requires array, slice, or str value");
    }
    if (!elem)
        return value_make_error(vm, "exec: slice cannot determine element type");

    /* 构造 ref []T 切片类型 */
    const type_t *ref_slice = type_slice_intern(vm, elem);
    if (!ref_slice)
        return value_make_error(vm, "exec: slice cannot make slice type");

    if (value_is_shadow(self))
        return value_make_shadow(vm, ref_slice);

    /* 读 low / high（按实际整数类型宽度读 → u64） */
    uint64_t low = 0, high = src_len;
    if (low_v) {
        const type_t *lt = value_type(low_v);
        void *ld = value_data(low_v);
        int64_t lv = 0;
        switch (lt->size) {
        case 1: lv = *(const int8_t *)ld; break;
        case 2: lv = *(const int16_t *)ld; break;
        case 4: lv = *(const int32_t *)ld; break;
        default: lv = *(const int64_t *)ld; break;
        }
        low = (uint64_t)(lv < 0 ? 0 : lv);
    }
    if (high_v) {
        const type_t *lt = value_type(high_v);
        void *ld = value_data(high_v);
        int64_t hv = 0;
        switch (lt->size) {
        case 1: hv = *(const int8_t *)ld; break;
        case 2: hv = *(const int16_t *)ld; break;
        case 4: hv = *(const int32_t *)ld; break;
        default: hv = *(const int64_t *)ld; break;
        }
        high = (uint64_t)(hv < 0 ? 0 : hv);
    }
    /* §4.1 运行期边界检查：0 <= low <= high <= len，越界 → 运行期错误 */
    if (low > src_len || high > src_len || low > high) {
        char buf[128];
        snprintf(buf, sizeof buf,
                 "slice index out of bounds: [%llu:%llu] (len=%llu)",
                 (unsigned long long)low, (unsigned long long)high,
                 (unsigned long long)src_len);
        return value_make_error(vm, buf);
    }

    /* 计算切片指针：base_ptr + low * elem_size */
    const char *slice_ptr = (const char *)base_ptr + low * elem->size;
    uint64_t slice_len = high - low;

    return slice_make_value(vm, ref_slice, (void *)slice_ptr, slice_len);
}

/* MAKE <init_count>：**make 堆分配构造**（make(T, N, init...)，m4-design §2）。
 * 栈布局 [elem_type, length, init1..initN]（elem_type 在底、initN 在顶）：
 * 先弹 N 个初始值（逆序），再弹 length，再弹 elem_type → 分配 N*elem_size
 * 堆块 + 逐元素布值（init implicit_cast → elem_type 后 memcpy）→ 压 fatal []T。
 * clux 无默认零值，init_count 须 == length（sema 已校验）。
 * 与 op_new 同构：堆块所有权随构造转移给 fatal []T 产物，源 init 值内嵌
 * own 指针清空（ptr_clear_owned_recursive 防双释放）。 */
static value_t *op_make(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t init_count = bcode_read_u32(bc, pc);
    uint8_t elem_own = bcode_read_u8(bc, pc);

    /* 逆序弹 N 个初始值 */
    value_t *inits[init_count > 0 ? init_count : 1];
    for (uint32_t i = 0; i < init_count; i++)
        inits[init_count - 1 - i] = exec_stack_pop(vm);

    value_t *len_v   = exec_stack_pop(vm);
    value_t *type_v  = exec_stack_pop(vm);

    const type_t *elem = (type_v && value_type(type_v) == vm->type_type)
                             ? value_as(type_v, const type_t *) : NULL;
    if (!elem)
        return value_make_error(vm, "make: missing element type slot");

    /* 构造 fatal []T 切片类型（元素槽位所有权随指令立即数——`make(own *i32,…)`
       的元素要按 own 释放，类型必须与 sema 侧一致才能去重成功） */
    const type_t *fatal_slice =
        type_slice_intern_own(vm, elem, (ownership_t)elem_own);
    if (!fatal_slice)
        return value_make_error(vm, "make: cannot make slice type");

    /* shadow 模式：只返回类型 shadow（sema 只做类型检查） */
    bool any_shadow = (len_v && value_is_shadow(len_v));
    for (uint32_t i = 0; i < init_count && !any_shadow; i++)
        if (value_is_shadow(inits[i])) any_shadow = true;
    if (any_shadow)
        return value_make_shadow(vm, fatal_slice);

    /* 读 length（按实际整数类型宽度读 → u64） */
    uint64_t length = 0;
    if (len_v) {
        const type_t *lt = value_type(len_v);
        void *ld = value_data(len_v);
        int64_t lv = 0;
        switch (lt->size) {
        case 1: lv = *(const int8_t *)ld; break;
        case 2: lv = *(const int16_t *)ld; break;
        case 4: lv = *(const int32_t *)ld; break;
        default: lv = *(const int64_t *)ld; break;
        }
        length = (uint64_t)(lv < 0 ? 0 : lv);
    }

    /* 分配 len * elem_size 堆块（清零） */
    size_t total = (size_t)length * elem->size;
    void *heap = NULL;
    if (total > 0) {
        heap = allocator_new_ex(vm->alloc, "slice_make", total,
                                NULL, NULL, NULL, 1);
        if (!heap) panic("vm: out of memory in make");
        memset(heap, 0, total);
    }
    if (length > 0 && elem->size > 0) {
        /* 逐元素布值：init implicit_cast → elem_type 后写入；持有资源的元素
           深拷贝（见 value_own_copy_into —— 平凡 memcpy 会让新块与源临时值
           共享内嵌堆块，源头仍归作用域所有 → 双释放）。 */
        for (uint32_t i = 0; i < init_count && i < length; i++) {
            value_t *casted = inits[i];
            if (value_type(inits[i]) != elem) {
                casted = value_implicit_cast(vm, inits[i], elem);
                if (value_is_error(vm, casted)) return casted;
            }
            value_own_copy_into(vm, (char *)heap + i * elem->size, casted, elem);
        }
    }

    return slice_make_value(vm, fatal_slice, heap, length);
}

/* IS_TAG <tag>：判 tag 专用指令（`x is Member`，编译期已解析 member → tag
 * 值）。弹 union 值 → 读 data 首部 tag 整数与立即数比较 → 压 bool。
 * shadow → 压 bool shadow（sema 只做类型检查）。 */
static value_t *op_is_tag(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t tag = bcode_read_u32(bc, pc);
    value_t *v = exec_stack_pop(vm);
    const type_t *t = v ? value_type(v) : NULL;
    if (!t || t->kind != TYPE_KIND_UNION)
        return value_make_error(vm, "exec: is tag requires a union value");
    if (value_is_shadow(v))
        return value_make_shadow(vm, vm->type_bool);
    bool b = (union_read_tag(v) == (uint64_t)tag);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &b);
    return value_make(vm, vm->type_bool, data);
}

/* 构造期字段写入：持有资源则深拷贝（见 vm/value_owncopy.h）。
   前向声明：slice 字面量构造（make）在 op_construct 之前也要用。 */
static void *construct_field_copy(vm_t *vm, void *dst, const value_t *src,
                                  const type_t *ft) {
    return value_own_copy_into(vm, dst, src, ft);
}

/* ---- 值构造（construct N）：弹 N 个成员值 + 类型位 → value ----
 * 栈布局（构造期）：[..., type_value, v1, v2, ..., vN]（type 在底、vN 在顶）。
 * 先弹 type_value（栈上类型构造产物，如 push_array...seal 留下的 array type
 * value），再逐个弹 N 个成员值（栈顶为 vN，逆序收集为 elems[0..n-1] = v1..vN）。
 * 按类型种类分派：当前仅实现 array 分支（struct/tuple 待后续 Phase）。 */
/* ---- 在既定类型上"构造值"（不关心 dest） ----
 * 返回新值（数据块已分配）；失败返回 error。从 op_construct 拆出：统一
 * NEW/CONSTRUCT 后，栈值构造（dest=undefined）与堆块构造（dest=opaque 原始块，
 * 见 op_construct 尾部的搬运逻辑）共用同一套逐槽位写入。 */
static value_t *construct_value(vm_t *vm, const type_t *t, value_t **elems,
                                uint32_t n) {
    /* 仅实现 array + option 分支 */
    if (t->kind == TYPE_KIND_ARRAY) {
        const type_t *et = array_type_elem(t);
        /* 校验成员数：定长数组允许部分填充（不足部分编译器补发
           undefined 占位，块清零自动补零值）；动态数组 SIZE_MAX 不限 */
        size_t len = array_type_len(t);
        if (len != SIZE_MAX && (size_t)n > len)
            return value_make_error(vm,
                "construct: array element count mismatch");
        return value_make_array_own(vm, et, n > 0 ? elems : NULL, n,
                                    array_type_elem_ownership(t));
    }

    /* ?T 构造器（二值单槽位，docs m2-design §12.2）：n 必须 == 1（sema
       已校验）。字段 nil → PUSH_OPT_NONE 压的就是 ?T none 值块（member
       type == t → 身份直接收下）；字段 T → 隐式提升 some（member type
       == inner → value_lift_option 压 ok=true + T 值深拷贝到 value 字段）。
       其余成员类型组合为 sema/编译期契约破坏，报错。 */
    if (t->kind == TYPE_KIND_OPTION) {
        const type_t *inner = type_option_inner(t);
        if (n != 1)
            return value_make_error(vm,
                "construct: optional constructor expects exactly 1 field");
        value_t *member = elems[0];
        const type_t *mt = value_type(member);
        if (mt == t)
            return member;                     /* 已是 ?T（nil 字段 none 值块） */
        if (inner && mt == inner)
            return value_lift_option(vm, member, t);  /* T → some 提升 */
        return value_make_error(vm,
            "construct: optional field type mismatch");
    }

    /* struct 构造：连续内存块（size = type->size），逐字段按偏移平凡拷贝
     * （data 全平凡：memcpy）。字段数必须与类型字段表一致（sema 已校验
     * "字段数完全显式"）。每字段先 value_implicit_cast 到字段类型（字面量
     * i32 → i64 字段等宽度提升；同类型身份短路），与 array 构造的
     * value_make_array 逐元素 cast 行为一致。 */
    if (t->kind == TYPE_KIND_STRUCT) {
        const struct_type_t *st = (const struct_type_t *)t;
        if ((size_t)n != st->field_count)
            return value_make_error(vm,
                "construct: struct field count mismatch");
        void *data = value_alloc_data(vm->alloc, t);  /* 清零：未指定字段自动零值 */
        for (uint32_t i = 0; i < n; i++) {
            value_t *src = elems[i];
            const type_t *ft = st->fields[i].type;
            /* 同类型直接用（身份，不 clone——value_implicit_cast 同类型短路
               value_clone 浅拷贝，会与成员共享内嵌 own 堆块）；异类型才隐式
               转换（字面量 i32 → i64 字段等宽度提升；fatal → own 接管产物
               在 cast 内转移、源清零）。 */
            if (value_type(src) != ft) {
                src = value_implicit_cast(vm, src, ft);
                if (value_is_error(vm, src)) return src;
            }
            /* data 全平凡：字段按偏移写入；持有资源的字段深拷贝（见
               construct_field_copy），源临时值继续归作用域，各自释放各自的。 */
            construct_field_copy(vm, (uint8_t *)data + st->fields[i].offset,
                                 src, ft);
        }
        return value_make(vm, t, data);
    }

    /* tuple 构造：连续内存块（size = type->size），逐元素按偏移平凡拷贝
     * （data 全平凡：memcpy）。元素数必须与类型元素表一致（sema 已校验
     * "元素数完全显式"）。每元素先 value_implicit_cast 到元素类型（字面量
     * i32 → i64 元素等宽度提升；同类型身份短路），与 struct 构造逐字段
     * cast 行为一致。 */
    if (t->kind == TYPE_KIND_TUPLE) {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        if ((size_t)n != tt->elem_count)
            return value_make_error(vm,
                "construct: tuple element count mismatch");
        void *data = value_alloc_data(vm->alloc, t);  /* 清零：未指定元素自动零值 */
        for (uint32_t i = 0; i < n; i++) {
            value_t *src = elems[i];
            const type_t *et = tt->elems[i].type;
            /* 同类型直接用（身份，不 clone——同类型短路 value_clone 浅拷贝会
               与成员共享内嵌 own 堆块）；异类型才隐式转换（等宽度提升；
               fatal → own 接管产物在 cast 内转移、源清零）。 */
            if (value_type(src) != et) {
                src = value_implicit_cast(vm, src, et);
                if (value_is_error(vm, src)) return src;
            }
            /* data 全平凡：元素按偏移写入；持有资源的元素深拷贝（见
               construct_field_copy）。 */
            construct_field_copy(vm, (uint8_t *)data + tt->elems[i].offset,
                                 src, et);
        }
        return value_make(vm, t, data);
    }

    /* cunion 构造：.C{.i = 42}（struct 同构单字段构造，docs m2-design
       §cunion）——C union 无 tag（所有 member 共享 offset 0，无 tag 校验）。
       与 tag union 同款显式 member 哨兵协议（消除同类型 member 歧义，指令
       单一路径）：栈布局 [type_value, member_idx, payload_value]（idx 在底、
       payload 在顶），n == 2（idx 值 + payload 值两个成员）。idx 值读自
       成员[0]（整数值 = member 下标），payload 按该 member 的类型
       implicit_cast 后 blit 到 data 首部（offset 0，所有 member 共享）。
       data 清零未覆盖位自动零值。 */
    if (t->kind == TYPE_KIND_CUNION) {
        const cunion_type_t *ct = (const cunion_type_t *)t;
        if (n != 2)
            return value_make_error(vm,
                "construct: cunion constructor expects member index + value (n=2)");
        value_t *idx_v = elems[0];
        value_t *payload = elems[1];
        if (value_is_shadow(payload) || value_is_shadow(idx_v))
            return value_make_shadow(vm, t);
        /* member 下标值：整数（member 下标） */
        const type_t *it = idx_v ? value_type(idx_v) : NULL;
        if (!it || it->kind != TYPE_KIND_INT)
            return value_make_error(vm,
                "construct: cunion member index value must be an integer");
        int64_t mi = 0;
        switch (it->size) {
        case 1: mi = (int64_t)*(const int8_t  *)value_data(idx_v); break;
        case 2: mi = (int64_t)*(const int16_t *)value_data(idx_v); break;
        case 4: mi = (int64_t)*(const int32_t *)value_data(idx_v); break;
        default: mi = *(const int64_t *)value_data(idx_v); break;
        }
        if (mi < 0 || (size_t)mi >= ct->member_count)
            return value_make_error(vm,
                "construct: cunion member index value out of range");
        const cunion_member_t *m = &ct->members[(size_t)mi];
        if (!m->type)
            return value_make_error(vm, "construct: cunion member type missing");
        /* 隐式转换 payload 值 → 成员类型（同类型身份短路；字面量 i32 → i64
           成员宽度提升） */
        value_t *casted = value_implicit_cast(vm, payload, m->type);
        if (value_is_error(vm, casted)) return casted;
        void *data = value_alloc_data(vm->alloc, t);  /* 清零 */
        /* data 全平凡：member 为 memcpy 可拷贝字节块，直接拷贝到 offset 0
           （C union：所有 member 共享同一块内存，无 tag） */
        memcpy((uint8_t *)data, value_data(casted), m->type->size);
        return value_make(vm, t, data);
    }

    /* tag union 构造：.Value{.i = 42}（struct 同构单字段构造，docs m2-design
       §tag union）——显式 tag 哨兵协议（消除同类型 member 歧义，指令单一路径）：
       栈布局 [type_value, tag_value, payload_value]（tag 在底、payload 在顶），
       n == 2（tag 值 + payload 值两个成员）。tag 值读自成员[0]（整数值 =
       member 下标），payload 按该 member 的 payload_type implicit_cast 后
       blit 到 payload 区（data 清零未覆盖位自动零值）。 */
    if (t->kind == TYPE_KIND_UNION) {
        const union_type_t *ut = (const union_type_t *)t;
        if (n != 2)
            return value_make_error(vm,
                "construct: union constructor expects tag + payload (n=2)");
        value_t *tag_v = elems[0];
        value_t *payload = elems[1];
        if (value_is_shadow(payload) || value_is_shadow(tag_v))
            return value_make_shadow(vm, t);
        /* tag 值：整数（member 下标） */
        const type_t *tt = tag_v ? value_type(tag_v) : NULL;
        if (!tt || tt->kind != TYPE_KIND_INT)
            return value_make_error(vm,
                "construct: union member tag value must be an integer");
        int64_t tagv = 0;
        switch (tt->size) {
        case 1: tagv = (int64_t)*(const int8_t  *)value_data(tag_v); break;
        case 2: tagv = (int64_t)*(const int16_t *)value_data(tag_v); break;
        case 4: tagv = (int64_t)*(const int32_t *)value_data(tag_v); break;
        default: tagv = *(const int64_t *)value_data(tag_v); break;
        }
        if (tagv < 0 || (size_t)tagv >= ut->member_count)
            return value_make_error(vm,
                "construct: union member tag value out of range");
        const union_member_t *m = &ut->members[(size_t)tagv];
        if (!m->payload_type)
            return value_make_error(vm, "construct: union member type missing");
        /* 隐式转换 payload 值 → member 类型（同类型身份短路；字面量 i32 →
           i64 成员宽度提升；fatal 指针 → own 接管产物在 cast 内转移、源清零） */
        value_t *casted = value_implicit_cast(vm, payload, m->payload_type);
        if (value_is_error(vm, casted)) return casted;
        void *data = value_alloc_data(vm->alloc, t);  /* 清零 */
        union_store_tag_raw(data, ut->tag_size, m->tag);
        /* 成员值写入 data：持有资源的 payload 深拷贝（见
           construct_field_copy）。 */
        construct_field_copy(vm, (uint8_t *)data + ut->payload_offset,
                             casted, m->payload_type);
        return value_make(vm, t, data);
    }

    /* 标量 / 指针 / opaque 等（无可展开的槽位）：单成员值 → 该类型本身。
       `new i32{123}`、`new *T{…}` 的"构造"落在这一支：隐式转换 + memcpy 到
       新值块（与 C 后端 `*(T*)dst = (T)value` 对应）。 */
    if (n >= 1 && elems[0]) {
        value_t *src = elems[0];
        if (value_type(src) != t) {
            src = value_implicit_cast(vm, src, t);
            if (value_is_error(vm, src)) return src;
        }
        void *data = value_alloc_data(vm->alloc, t);
        memcpy(data, value_data(src), t->size);
        return value_make(vm, t, data);
    }

    return value_make_error(vm,
        "construct: unsupported type (only array, optional, struct, tuple, "
        "union, cunion and scalar implemented)");
}

/* ---- 值构造（construct N）：[dest, type_value, v1..vN] → 值 / fatal *T ----
 *
 * **NEW / CONSTRUCT 职责分离**（2026-10 统一）：
 *   - `NEW`（无操作数）只**申请内存**：弹一个字节数（usize）→ 压 opaque 原始块
 *     句柄（data 存裸指针）；
 *   - `CONSTRUCT` 在**给定 dest 上**构造对象：
 *       dest = undefined → 新建值数据块（栈值/临时值；C 后端即局部变量）；
 *       dest = opaque    → NEW 申请的原始块（C 后端即 malloc 出来的堆块）。
 * 因此两种源码形态归一为同一条指令序列：
 *   `new T{...}`  → `PUSH_U64 sizeof(T); NEW; <type>; <fields>; CONSTRUCT n`
 *                   （结果 fatal *T，由 DEFINE/赋值接管为 own）
 *   `.T{...}`     → `PUSH_UNDEFINED;     <type>; <fields>; CONSTRUCT n`（结果值 T）
 * 构造 = 逐槽位写入（申请与赋值分离），与 C 后端可逐条对应。
 */
static value_t *op_construct(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t n = bcode_read_u32(bc, pc);

    /* 栈上收集 N 个成员值（仅指针，不拥有 value）。栈布局：
       ..., dest, type_value, v1, ..., vN（vN 在顶），逆序收集使
       elems[0..n-1] = v1..vN，再弹类型位与 dest。 */
    value_t *elems[n > 0 ? n : 1];
    for (uint32_t i = 0; i < n; i++)
        elems[n - 1 - i] = exec_stack_pop(vm);

    value_t *type_v = exec_stack_pop(vm);
    const type_t *t = (type_v && value_type(type_v) == vm->type_type)
                          ? value_as(type_v, const type_t *) : NULL;
    if (!t)
        return value_make_error(vm, "construct: missing type slot");

    value_t *dest = exec_stack_pop(vm);
    bool heap = dest && value_type(dest) == vm->type_opaque;
    if (dest && !heap && !value_is_undefined(vm, dest))
        return value_make_error(
            vm, "construct: dest must be undefined or an allocated block "
                "(NEW)");

    value_t *v = construct_value(vm, t, elems, n);
    if (value_is_error(vm, v)) return v;
    if (!heap) return v;   /* 栈值/临时值：构造结果就是值本身 */

    /* 堆块：把构造结果搬进 NEW 申请的原始块，返回 fatal *T。
       源容器的 own 指针在搬运后清空——所有权随字节转移给堆块（与旧 NEW 的
       清源逻辑一致；防源容器与堆块共享同一批块 → 双释放）。 */
    const type_t *pt = type_ptr_intern(vm, t);
    if (!pt) return value_make_error(vm, "construct: cannot make pointer type");
    if (value_is_shadow(v)) return value_make_shadow(vm, pt);
    void *blk = value_data(dest) ? *(void **)value_data(dest) : NULL;
    if (!blk) return value_make_error(vm, "construct: allocation is NULL");
    memcpy(blk, value_data(v), t->size);
    ptr_clear_owned_recursive(vm, (type_t *)t, value_data(v));
    return ptr_make_value(vm, pt, blk);
}

/* ---- 下标读取（get_item）：self[index] -> 元素 ----
 * 栈布局：..., self, index（index 在顶）。弹 index 再弹 self，分派 vtable->get_index。 */
static value_t *op_index_get(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *index = exec_stack_pop(vm);
    value_t *self  = exec_stack_pop(vm);
    return value_get_index(vm, self, index);
}

/* ---- 下标写入（set_item）：self[index] = val -> self ----
 * 栈布局：..., self, index, val（val 在顶）。弹 val、index、self，分派 vtable->set_index。 */
static value_t *op_index_set(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *val   = exec_stack_pop(vm);
    value_t *index = exec_stack_pop(vm);
    value_t *self  = exec_stack_pop(vm);
    return value_set_index(vm, self, index, val);
}

/* ---- 字段读取（field_get <name>）：self.field → 借用引用 ----
 * 字段名是编译期常量（strtable 索引），运行时按名查偏移 → 返回借用引用
 * （data 指向 self data 块内偏移，零拷贝；绑定/返回时经 value_clone
 * materialize 深拷贝）。shadow → 返回字段类型 shadow（sema 走此路径）。
 * 嵌套字段 p.a.b：FIELD_GET 返回的借用值再进 FIELD_GET，data 偏移链正确。 */
static value_t *op_field_get(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *self = exec_stack_pop(vm);
    const type_t *t = self ? value_type(self) : NULL;
    /* const/volatile 限定只是同一份数据的视图：剥掉再按底层布局查字段
       （`ref *const T` 自动解引用后得到的就是 `const T` 值）。 */
    t = type_strip_quals(t);
    /* ---- 闭包分支（**仅 VM 层面**，sema 不放行用户写 `f.capture`）----
       闭包的本质是「捕获结构体 + 函数实现」：捕获槽就是它的字段。
       编译器为"销毁 own 捕获"生成的 `PUSH f; FIELD_GET "cap"; FREE`
       序列依赖这条路径——sema 不提供该语法，只有编译器生成的字节码走它。
       返回捕获值的**借用引用**（data 指向捕获槽的 value data），
       于是 FREE 能就地把槽位置 NULL，与结构体字段的语义一致。 */
    if (t && t->kind == TYPE_KIND_FUNC) {
        if (value_is_shadow(self))
            return value_make_shadow(vm, t);   /* shadow：无捕获可读 */
        const func_t *const *fp = (const func_t *const *)value_data(self);
        const func_t *fn = fp ? *fp : NULL;
        if (!fn || !fn->closure_scope)
            return value_make_error(vm, "exec: closure has no captures");
        value_t *cap = scope_lookup(fn->closure_scope, name);
        if (!cap)
            return value_make_error(vm, "exec: no such closure capture");
        /* 捕获槽本身可能就是 shadow（占位）→ 直接返回它 */
        if (value_is_shadow(cap))
            return cap;
        return value_make_borrowed(vm, value_type(cap), value_data(cap));
    }
    if (!t || t->kind != TYPE_KIND_STRUCT) {
        /* union 分支：字段名 = member 名，按名反查 → 读 data 首部 tag →
           tag 不符 → error value（引擎级硬错误 = panic，对齐数组越界）；
           相符 → 绝对偏移 = payload_offset（单字段，无内部偏移） */
        if (t && t->kind == TYPE_KIND_UNION) {
            int mi = union_type_find_member(t, name);
            if (mi < 0)
                return value_make_error(vm, "exec: no such union member field");
            const union_member_t *m = union_type_member(t, (size_t)mi);
            if (!m->payload_type)
                return value_make_error(vm, "exec: union member type missing");
            if (value_is_shadow(self))
                return value_make_shadow(vm, m->payload_type);
            uint64_t tag = union_read_tag(self);
            if (tag != (uint64_t)m->tag) {
                char buf[128];
                snprintf(buf, sizeof buf,
                         "exec: union field '%.*s' accessed on mismatched tag (member %u)",
                         (int)name.len, name.ptr, (unsigned)m->tag);
                return value_make_error(vm, buf);
            }
            return value_make_borrowed(vm, m->payload_type,
                (uint8_t *)value_data(self) + union_type_payload_offset(t));
        }
        /* cunion 分支：字段名 = member 名，按名反查 → 无 tag 校验（C union
           语义，开发者自负安全）→ 借用引用 data 指向 offset 0（所有 member
           共享内存，data 即联合体本身）。shadow → member 类型 shadow。 */
        if (t && t->kind == TYPE_KIND_CUNION) {
            int mi = cunion_type_find_member(t, name);
            if (mi < 0)
                return value_make_error(vm, "exec: no such cunion member field");
            const cunion_member_t *m = cunion_type_member(t, (size_t)mi);
            if (!m->type)
                return value_make_error(vm, "exec: cunion member type missing");
            if (value_is_shadow(self))
                return value_make_shadow(vm, m->type);
            return value_make_borrowed(vm, m->type, (uint8_t *)value_data(self));
        }
        return value_make_error(vm, "exec: field get expects a struct, union or cunion value");
    }
    int idx = struct_type_find_field(t, name);
    if (idx < 0)
        return value_make_error(vm, "exec: no such struct field");
    const struct_field_t *f = struct_type_field(t, (size_t)idx);
    if (value_is_shadow(self))
        return value_make_shadow(vm, f->type);
    return value_make_borrowed(vm, f->type,
                               (uint8_t *)value_data(self) + f->offset);
}

/* ---- 字段写入（field_set <name>）：self.field = val → self ----
 * 栈布局：..., self, val（val 在顶）。弹 val、self，隐式转换 val → 字段类型
 * （同类型身份短路），dispose 旧字段值（回收资源），blit 深拷贝新值到偏移，
 * 返回 self（引用，供链式复用；绑定/返回经 value_clone materialize）。
 * shadow → 直接返回 self shadow（sema 只做类型检查）。 */
static value_t *op_field_set(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *val  = exec_stack_pop(vm);
    value_t *self = exec_stack_pop(vm);
    const type_t *t = self ? value_type(self) : NULL;
    /* const/volatile 视图：剥掉再按底层布局定位字段（写入本身由 sema
       按 const 规则拦截——这里只负责布局）。 */
    t = type_strip_quals(t);
    if (!t || t->kind != TYPE_KIND_STRUCT) {
        /* union 分支：与 FIELD_GET 同款 tag 检查——字段存在但 tag 不符
           → error value（引擎级硬错误，对齐数组越界） */
        if (t && t->kind == TYPE_KIND_UNION) {
            int mi = union_type_find_member(t, name);
            if (mi < 0)
                return value_make_error(vm, "exec: no such union member field");
            const union_member_t *m = union_type_member(t, (size_t)mi);
            if (!m->payload_type)
                return value_make_error(vm, "exec: union member type missing");
            if (value_is_shadow(self))
                return self;
            uint64_t tag = union_read_tag(self);
            if (tag != (uint64_t)m->tag) {
                char buf[128];
                snprintf(buf, sizeof buf,
                         "exec: union field '%.*s' accessed on mismatched tag (member %u)",
                         (int)name.len, name.ptr, (unsigned)m->tag);
                return value_make_error(vm, buf);
            }
            value_t *casted = value_implicit_cast(vm, val, m->payload_type);
            if (value_is_error(vm, casted)) return casted;
            void *dst = (uint8_t *)value_data(self) +
                        union_type_payload_offset(t);
            /* 覆盖旧 payload：先按 member 槽位所有权销毁旧值（own member
               释放、ref member 跳过），否则旧 payload 的 own 堆块泄漏。 */
            ptr_free_slot_value(vm, m->payload_type, m->ownership, dst);
            /* data 全平凡：直接覆盖 payload（memcpy 可拷贝） */
            memcpy(dst, value_data(casted), m->payload_type->size);
            return self;
        }
        /* cunion 分支：与 FIELD_GET 同款 member 名反查 → 无 tag 校验（C union
           语义）→ implicit_cast 后 memcpy 到 offset 0（共享内存直接覆盖） */
        if (t && t->kind == TYPE_KIND_CUNION) {
            int mi = cunion_type_find_member(t, name);
            if (mi < 0)
                return value_make_error(vm, "exec: no such cunion member field");
            const cunion_member_t *m = cunion_type_member(t, (size_t)mi);
            if (!m->type)
                return value_make_error(vm, "exec: cunion member type missing");
            if (value_is_shadow(self))
                return self;
            value_t *casted = value_implicit_cast(vm, val, m->type);
            if (value_is_error(vm, casted)) return casted;
            /* data 全平凡：写入 offset 0（联合体本体，直接覆盖） */
            memcpy((uint8_t *)value_data(self), value_data(casted), m->type->size);
            return self;
        }
        return value_make_error(vm, "exec: field set expects a struct, union or cunion value");
    }
    int idx = struct_type_find_field(t, name);
    if (idx < 0)
        return value_make_error(vm, "exec: no such struct field");
    const struct_field_t *f = struct_type_field(t, (size_t)idx);
    if (value_is_shadow(self))
        return self;
    value_t *casted = value_implicit_cast(vm, val, f->type);
    if (value_is_error(vm, casted)) return casted;
    void *dst = (uint8_t *)value_data(self) + f->offset;
    /* 覆盖旧字段：先按字段槽位所有权销毁旧值（own 槽位释放、ref 槽位跳过）。 */
    ptr_free_slot_value(vm, f->type, f->ownership, dst);
    /* data 全平凡：直接覆盖字段（memcpy 可拷贝） */
    memcpy(dst, value_data(casted), f->type->size);
    return self;
}

/* ---- 长度查询（length）：弹 self → value_length(self)（代理到 vtable->length）
 * 当前仅数组实现：返回 u64 元素个数 shadow 下返回 u64 shadow）。 */
static value_t *op_length(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *self = exec_stack_pop(vm);
    return value_length(vm, self);
}

/* OPT_IS_NONE（无操作数）：nil 判定专用。弹 ?T 值 → 压 bool（ok tag ==
 * false）。x==nil / nil==x → OPT_IS_NONE；x!=nil / nil!=x → OPT_IS_NONE
 * + NOT。tag 比较不进入 vtable eq 分派（nil 非 value，option eq/ne 无
 * 实现）。shadow → 压 bool shadow。 */
static value_t *op_opt_is_none(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    const type_t *t = v ? value_type(v) : NULL;
    if (!t || t->kind != TYPE_KIND_OPTION)
        return value_make_error(vm, "exec: opt is none requires an optional value");
    if (value_is_shadow(v))
        return value_make_shadow(vm, vm->type_bool);
    bool b = !*(const bool *)value_data(v);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &b);
    return value_make(vm, vm->type_bool, data);
}

/* PUSH_OPT_NONE <id>：从 types_by_id 查 option 类型（校验），压 ok=false +
 * value 全零的 ?T 值块（构造器字段/fill 的 nil——value_alloc_data 清零，
 * value 字段零值即"无残留"）。带类型 id 操作数而非弹栈顶类型值，省一条
 * LOAD_TYPE。 */
static value_t *op_push_opt_none(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    const type_t *t = vm_type_load(vm, id);
    if (!t) return value_make_error(vm, "exec: unknown type id in push opt none");
    if (t->kind != TYPE_KIND_OPTION)
        return value_make_error(vm, "exec: push opt none requires an optional type");
    void *data = value_alloc_data(vm->alloc, t);  /* 清零：ok=false + value 零值 */
    return value_make(vm, t, data);
}

/* UNWRAP（无操作数）：optional 解包（a.! assert）。弹 ?T 值 → ok 则借用
 * 返回 value 字段的借用引用（零拷贝，data 指向 option 值块内偏移；与
 * ownership=OWN_REF 借用字段同构，生命周期 = 源 scope 值）；ok=false（none）→ panic
 * 错误值（用户范式先判空再解包：if (x != nil) { var v = x.!; ... }，
 * 未判空直接解包是编程错误，panic 终止）。 */
static value_t *op_unwrap(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    const type_t *t = v ? value_type(v) : NULL;
    if (!t || t->kind != TYPE_KIND_OPTION)
        return value_make_error(vm, "exec: unwrap requires an optional value");
    const type_t *inner = type_option_inner(t);
    if (value_is_shadow(v)) {
        /* shadow：只算类型，退化 inner 的 shadow 引用 */
        return value_make_shadow(vm, inner);
    }
    if (!*(const bool *)value_data(v))
        return value_make_error(vm,
                                "panic: unwrap '.!' on none optional value");
    return value_make_borrowed(vm, inner,
        (uint8_t *)value_data(v) + option_value_offset(t));
}

static value_t *op_push_function(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* entry_pc 立即数；弹栈顶签名类型（CREATE_FUNC_TYPE 产物），
       构造 bcode_function_t（封装在 bcode_function 模块内，id 默认 0） */
    uint32_t entry_pc = bcode_read_u32(bc, pc);
    value_t *sig_v = exec_stack_pop(vm);
    const type_t *sig = *(const type_t **)value_data(sig_v);
    return bcode_function_new(vm, sig, entry_pc, vm->root_scope, vm->bc);
}

/* BIND_FUNC <id>：peek 栈顶 func value（不弹栈——注册段 DEFINE 需保留
   func value 在栈上），填充 fn->id 并登记 id→func 到 functions_by_id
   （幂等）。id 单一来源——只在此出现一次。 */
static value_t *op_bind_func(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    value_t *fv = exec_stack_peek(vm, 0);
    const type_t *t = fv ? value_type(fv) : NULL;
    if (!t || t->kind != TYPE_KIND_FUNC)
        return value_make_error(vm, "exec: bind func expects a function value");
    func_t *fn = *(func_t **)value_data(fv);
    fn->id = id;
    vm_func_bind(vm, id, fn);
    return NULL;
}

/* SET_FUNC_NAME <name>：peek 栈顶 func value（不弹栈），拷贝名字到 vm 堆
   （owns_name=true，func_destroy 释放），覆盖函数显示名 */
static value_t *op_set_func_name(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *fv = exec_stack_peek(vm, 0);
    const type_t *t = fv ? value_type(fv) : NULL;
    if (!t || t->kind != TYPE_KIND_FUNC)
        return value_make_error(vm, "exec: set func name expects a function value");
    func_t *fn = *(func_t **)value_data(fv);

    /* 旧 name（若为 SET_FUNC_NAME 堆拷贝）先释放 */
    if (fn->owns_name && fn->name.ptr) {
        char *np = (char *)fn->name.ptr;
        allocator_free(vm->alloc, (void **)&np);
        fn->name = (strslice_t){ NULL, 0 };
        fn->owns_name = false;
    }

    /* 拷贝新 name（NUL 结尾，vm 拥有生命周期） */
    char *buf = (char *)allocator_new_ex(vm->alloc, "char", sizeof(char),
                                         NULL, NULL, NULL, name.len + 1);
    if (!buf) return value_make_error(vm, "exec: out of memory setting func name");
    memcpy(buf, name.ptr, name.len);
    buf[name.len] = '\0';
    fn->name      = (strslice_t){ buf, name.len };
    fn->owns_name = true;
    return NULL;
}

/* SET_CLOSURE <name>：弹栈顶捕获值 → clone 进栈下函数对象的 closure_scope
   捕获槽。调用约定（compiler 保证）：定义点先 LOAD_FUNCTION <fid> 压函数
   值，随后每个捕获 [值, SET_CLOSURE "name"]，栈下 1 位即目标函数。闭包为
   clone 值语义（捕获独立副本，非引用）；提升区已用 undefined 占位（scope
   层 define-or-replace：scope_set 替换，void 占位无 assign 槽不走 value_assign）。 */
static value_t *op_set_closure(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *cap = exec_stack_pop(vm);
    value_t *fv = exec_stack_peek(vm, 0);
    const type_t *t = fv ? value_type(fv) : NULL;
    if (!t || t->kind != TYPE_KIND_FUNC)
        return value_make_error(vm, "exec: set closure expects a function value below");
    func_t *fn = *(func_t **)value_data(fv);
    if (!fn->closure_scope)
        return value_make_error(vm, "exec: function has no closure scope");

    /* clone 进 closure_scope（scope_set 临时切 current_scope，clone 自动 track
       到 owned；占位 undefined 先被销毁替换） */
    char buf[256];
    if (name.len >= sizeof(buf))
        return value_make_error(vm, "exec: closure capture name too long");
    memcpy(buf, name.ptr, name.len);
    buf[name.len] = '\0';
    value_t *stored = scope_set(vm, fn->closure_scope, buf, cap);
    if (!stored) return value_make_error(vm, "exec: failed to set closure capture");
    return NULL;
}

static value_t *op_call(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* callee 在 stack[sp-1-argc]，args 为紧随其后的 argc 个引用。
       args 复制进 VLA（value_call 内部压栈可能 realloc 使栈缓冲悬垂）；
       借用引用，值本体归 scope。 */
    uint32_t argc = bcode_read_u32(bc, pc);
    size_t sp = vec_len(vm->stack);
    value_t *callee = exec_stack_peek(vm, argc);
    value_t *args[argc > 0 ? argc : 1];
    for (uint32_t i = 0; i < argc; i++) {
        args[i] = (value_t *)vec_get(vm->stack, sp - argc + i);
    }
    for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm);
    return value_call(vm, callee, args, argc);
}

static value_t *op_ret(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* 栈顶即返回值；返回 interrupt 哨兵由 bcode_call_cfunc 子循环捕获 */
    (void)bc; (void)pc;
    return value_make_interrupt(vm, INTERRUPT_RETURN);
}

/* ---- 控制流 ---- */

static value_t *op_jmp(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)vm;
    uint32_t target = bcode_read_u32(bc, pc);
    *pc = target;
    return NULL;
}

static value_t *op_jz(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t target = bcode_read_u32(bc, pc);
    value_t *v = exec_stack_pop(vm);
    if (value_is_error(vm, v)) return v;
    /* 严格 bool：safe_cast 到 bool（不可转换 → error 传播） */
    value_t *cond = value_explicit_cast(vm, v, vm->type_bool);
    if (value_is_error(vm, cond)) return cond;
    if (!*(const bool *)value_data(cond)) *pc = target;
    return NULL;
}

static value_t *op_jnz(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t target = bcode_read_u32(bc, pc);
    value_t *v = exec_stack_pop(vm);
    if (value_is_error(vm, v)) return v;
    /* 严格 bool：safe_cast 到 bool（不可转换 → error 传播） */
    value_t *cond = value_explicit_cast(vm, v, vm->type_bool);
    if (value_is_error(vm, cond)) return cond;
    if (*(const bool *)value_data(cond)) *pc = target;
    return NULL;
}

/* ---- 作用域 ---- */

static value_t *op_push_scope(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    vm_push_scope(vm);
    return NULL;
}

static value_t *op_pop_scope(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    vm_pop_scope(vm);
    return NULL;
}

/* ---- M5 模块系统 ---- */

/* IMPORT <str>：模块规范路径。查 vm->modules 表，构造模块值压栈。
 * 幂等：同一路径返回同一模块值。模块在 sema 阶段递归 compile_module
 * 时已注册到 vm->modules，运行期只查表不触发编译。 */
/* IMPORT <源码路径>：操作数是**源码里写的路径**（`"./math"`），不是规范路径。
 * 这里以"当前正在执行的模块"（bc->module_path）为基准**重新规范化**，得到
 * vm->modules 的 key —— 与加载 `.cxb`/`.cxs` 时重建模块图用的是同一套规则
 * （module/module_path.h），因此产物与宿主目录布局解耦：容器搬到任何目录，
 * 两侧的规范化同步平移，key 依旧吻合。
 * bc->module_path 为 NULL（独立 .cxs 片段、无模块上下文）时以进程工作目录为
 * 基准——与旧行为一致。 */
/* M6 FFI：EXTERN_CALL <str "lib::name"> <u32 argc> <u32 ret_kind>
 *
 * 从**当前执行模块**取动态库句柄并调用外部 C 函数：
 *   bc->module_path → vm->modules（vm_module_lookup）→ module_t.ffi_libs（别名 → 句柄）
 *   → ffi_lib_sym（dlsym/GetProcAddress）→ 按 argc 弹实参 → 类型化调用层。
 *
 * 实参一律按 int64 宽度传递（整数/指针）；ret_kind 决定返回值如何归一化——
 * **必须造出与源码声明一致的类型**（见 vm/bcode.h 的 BCODE_FFI_RET_* 表：
 * void / i8..i64 / u8..u64 / bool / ptr / f64）。
 * 浮点走 BCODE_FFI_RET_F64 + 全 double 实参（见 docs/m6-design.md §14.5 的
 * ffi_call_f64；本仓库 Windows x64 的 libffi 汇编路径不可用，故按类型分派）。
 */
#define FFI_MAX_ARGS 16

/* M6 FFI 共用：拆 "lib::name" → 当前模块库表查句柄 → ffi_lib_sym 解析符号。
 * 成功返回 NULL 并写 *out_sym；失败返回 error value（文案含别名/符号名与 OS 文本）。
 * `what` 只用于诊断前缀（"extern call" / "extern load" / "extern store"）。
 * 函数与数据符号走同一条路：GetProcAddress/dlsym 对数据符号返回的**就是变量
 * 地址**，因此 extern var 的读写直接作用于库里那块内存。 */
static value_t *ffi_resolve_symbol(vm_t *vm, bytecode_t *bc, strslice_t qual,
                                  const char *what, void **out_sym) {
    *out_sym = NULL;
    size_t sep = (size_t)-1;
    for (size_t i = 0; i + 1 < qual.len; i++) {
        if (qual.ptr[i] == ':' && qual.ptr[i + 1] == ':') { sep = i; break; }
    }
    if (sep == (size_t)-1) {
        char msg[128];
        snprintf(msg, sizeof(msg), "exec: %s: malformed 'lib::name'", what);
        return value_make_error(vm, msg);
    }
    char alias[256], name[256];
    size_t alen = sep < sizeof(alias) - 1 ? sep : sizeof(alias) - 1;
    size_t nlen = qual.len - sep - 2;
    if (nlen > sizeof(name) - 1) nlen = sizeof(name) - 1;
    memcpy(alias, qual.ptr, alen);
    alias[alen] = '\0';
    memcpy(name, qual.ptr + sep + 2, nlen);
    name[nlen] = '\0';

    module_t *mod = bc->module_path ? vm_module_lookup(vm, bc->module_path) : NULL;
    ffi_lib_t *lib = (mod && mod->ffi_libs)
                         ? (ffi_lib_t *)strmap_get(mod->ffi_libs, alias)
                         : NULL;
    if (!lib) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "exec: %s: no loaded library '%s' in this module "
                 "(declare `extern lib \"...\" as %s;`)", what, alias, alias);
        return value_make_error(vm, msg);
    }
    char err[512] = {0};
    void *sym = ffi_lib_sym(lib, name, err, sizeof(err));
    if (!sym)
        return value_make_error(vm, err[0] ? err : "exec: cannot resolve symbol");
    *out_sym = sym;
    return NULL;
}

/* BCODE_FFI_DATA_* → clux 内建类型 + 字节宽度（实现在 vm/ffi_lib.c，与 sema 侧的
 * 编码函数成对，避免两处清单漂移）。 */
static const type_t *ffi_data_type(vm_t *vm, uint32_t kind, size_t *size) {
    return ffi_data_type_of(vm, kind, size);
}

/* EXTERN_LOAD <STR "lib::name"> <U32 kind>：读**数据符号**（extern var 的读取）。
 * 地址来自 dlsym（数据符号的"符号地址"就是变量地址），按 kind 读 size 字节 →
 * 压对应类型的值。值是新分配的副本，与库内存储解耦（写回需 EXTERN_STORE）。
 *
 * kind = BCODE_FFI_DATA_TYPED 时，指令前有一个**类型值**（compiler 压入）：
 * 宽度/布局查该类型（struct/cunion/enum），size = 类型的 size。 */
static value_t *op_extern_load(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t qual = bcode_read_str(bc, pc);
    uint32_t kind = bcode_read_u32(bc, pc);

    size_t size = 0;
    const type_t *t = NULL;
    if (kind == BCODE_FFI_DATA_TYPED) {
        value_t *tv = exec_stack_pop(vm);
        if (tv && value_type(tv) == vm->type_type)
            t = value_as(tv, const type_t *);
        if (!t) return value_make_error(vm, "exec: extern load: missing type slot");
        size = t->size;
    } else {
        t = ffi_data_type(vm, kind, &size);
        if (!t || size == 0 || size > 8)
            return value_make_error(vm, "exec: extern load: bad data type code");
    }

    void *sym = NULL;
    value_t *err = ffi_resolve_symbol(vm, bc, qual, "extern load", &sym);
    if (err) return err;

    if (size <= 8) {
        uint8_t buf[8] = {0};
        memcpy(buf, sym, size);
        return value_make(vm, t, value_alloc_data_copy(vm->alloc, t, buf));
    }
    /* 大对象（struct）：直接按类型的 size 复制（值块与 C 布局同构） */
    return value_make(vm, t, value_alloc_data_copy(vm->alloc, t, sym));
}

/* EXTERN_STORE <STR "lib::name"> <U32 kind>：写**数据符号**（extern var 的写入）。
 * 弹栈顶值 → 按 kind 把 size 字节写进符号地址（不压回结果：赋值是语句）。
 * 先解析符号再弹值，符号找不到时操作数栈保持原样。
 * TYPED 时栈布局是 [type, value]（类型在值之下）。 */
static value_t *op_extern_store(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t qual = bcode_read_str(bc, pc);
    uint32_t kind = bcode_read_u32(bc, pc);

    size_t size = 0;
    const type_t *t = NULL;
    if (kind != BCODE_FFI_DATA_TYPED) {
        t = ffi_data_type(vm, kind, &size);
        if (!t || size == 0 || size > 8)
            return value_make_error(vm, "exec: extern store: bad data type code");
    }

    void *sym = NULL;
    value_t *err = ffi_resolve_symbol(vm, bc, qual, "extern store", &sym);
    if (err) return err;

    value_t *v = exec_stack_pop(vm);
    if (value_is_error(vm, v)) return v;
    if (kind == BCODE_FFI_DATA_TYPED) {
        value_t *tv = exec_stack_pop(vm);
        if (tv && value_type(tv) == vm->type_type)
            t = value_as(tv, const type_t *);
        if (!t) return value_make_error(vm, "exec: extern store: missing type slot");
        size = t->size;
    }
    if (!v || !value_data(v))
        return value_make_error(vm, "exec: extern store: bad value");
    /* 类型安全网：sema 已保证类型可赋；字节码层被改坏时按隐式转换兜底 */
    if (value_type(v) != t) {
        value_t *casted = value_implicit_cast(vm, v, t);
        if (value_is_error(vm, casted)) return casted;
        v = casted;
    }
    if (size <= 8) {
        uint8_t buf[8] = {0};
        memcpy(buf, value_data(v), size);
        memcpy(sym, buf, size);
        return NULL;
    }
    memcpy(sym, value_data(v), size);
    return NULL;
}

/* BCODE_FFI_RET_*（返回类型码）→ BCODE_FFI_DATA_*（类型/描述符码）。
   两套码描述的是同一批 clux 标量，只是用途不同。 */
static uint32_t ffi_ret_to_data_kind(uint32_t rk) {
    switch (rk) {
    case BCODE_FFI_RET_I8:  return BCODE_FFI_DATA_I8;
    case BCODE_FFI_RET_I16: return BCODE_FFI_DATA_I16;
    case BCODE_FFI_RET_I32: return BCODE_FFI_DATA_I32;
    case BCODE_FFI_RET_I64: return BCODE_FFI_DATA_I64;
    case BCODE_FFI_RET_U8:  return BCODE_FFI_DATA_U8;
    case BCODE_FFI_RET_U16: return BCODE_FFI_DATA_U16;
    case BCODE_FFI_RET_U32: return BCODE_FFI_DATA_U32;
    case BCODE_FFI_RET_U64: return BCODE_FFI_DATA_U64;
    case BCODE_FFI_RET_F32: return BCODE_FFI_DATA_F32;
    case BCODE_FFI_RET_F64: return BCODE_FFI_DATA_F64;
    case BCODE_FFI_RET_BOOL: return BCODE_FFI_DATA_BOOL;
    case BCODE_FFI_RET_PTR: return BCODE_FFI_DATA_PTR;
    default: return (uint32_t)-1;
    }
}

/* 实参 → (libffi 描述符, C 表示地址)。
 *
 * **clux 的 value->data 与 C 同构**（编译器保证平凡布局）：i32 的 data 就是一个
 * `int32_t`、f64 就是一个 `double`、指针就是一个指针、struct 就是那块 C 布局的
 * 字节……因此实参可以直接把 `value_data(v)` 交给 libffi（它按描述符读 size 字节）。
 *   - `str` 与各种指针（own/ref/fatal/share/weak）、opaque 在 C ABI 里都是裸指针；
 *   - `enum` 按其底层整数类型（data 就是那个整数）；
 *   - **struct/cunion** 按值传：描述符按字段递归新建（`owned` 置 true，调用方
 *     用完 `ffi_type_release`）；
 *   - 切片是胖指针（ptr+len），**不是** C 可表达的单个值 → 拒绝。
 * 返回 false = 该类型不可作 FFI 实参。 */
static bool ffi_arg_desc(vm_t *vm, const value_t *v, const void **out_type,
                         void **out_ptr, bool *out_owned, char *err,
                         size_t ecap) {
    const type_t *t = value_type(v);
    if (out_owned) *out_owned = false;
    if (!t || !value_data(v)) return false;
    /* const/volatile 是同一份数据的限定视图：剥掉再判（描述符与底层类型一致） */
    while (t->kind == TYPE_KIND_CONST || t->kind == TYPE_KIND_VOLATILE) {
        const type_t *sub = (t->kind == TYPE_KIND_CONST)
                                ? ((const const_type_t *)t)->sub
                                : ((const volatile_type_t *)t)->sub;
        if (!sub) break;
        t = sub;
    }
    bool owned = false;
    const void *desc = ffi_type_for(t, vm, &owned, err, ecap);
    if (!desc) return false;
    if (out_owned) *out_owned = owned;
    *out_type = desc;
    *out_ptr = value_data(v);
    return true;
}

/* 变参位置的 **C 默认实参提升**：float → double、窄整型/bool → int。
 * 提升后的值要放进调用方提供的 8 字节槽（libffi 按提升后的类型读取）。
 * 返回 true 表示"已提升"（描述符与槽都写好）；false = 无需提升（原样传）。 */
static bool ffi_promote_vararg(vm_t *vm, const value_t *v, const void **out_type,
                               void **out_ptr, void *slot8) {
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return false;
    while (t->kind == TYPE_KIND_CONST || t->kind == TYPE_KIND_VOLATILE) {
        const type_t *sub = (t->kind == TYPE_KIND_CONST)
                                ? ((const const_type_t *)t)->sub
                                : ((const volatile_type_t *)t)->sub;
        if (!sub) break;
        t = sub;
    }
    if (t == vm->type_f32) {
        double d = (double)*(const float *)value_data(v);
        memcpy(slot8, &d, sizeof d);
        *out_type = ffi_data_ffi_type(BCODE_FFI_DATA_F64);
        *out_ptr = slot8;
        return true;
    }
    const type_t *vt = t;
    if (vt->kind == TYPE_KIND_ENUM) {
        const type_t *u = enum_type_underlying(vt);
        if (u) vt = u;
    }
    int promoted = 0;
    if (vt == vm->type_i8)        promoted = (int)*(const int8_t *)value_data(v);
    else if (vt == vm->type_i16)  promoted = (int)*(const int16_t *)value_data(v);
    else if (vt == vm->type_bool) promoted = *(const bool *)value_data(v) ? 1 : 0;
    else if (vt == vm->type_u8)   promoted = (int)*(const uint8_t *)value_data(v);
    else if (vt == vm->type_u16)  promoted = (int)*(const uint16_t *)value_data(v);
    else return false; /* 32/64 位整数与指针已是寄存器槽宽度，无需提升 */
    memcpy(slot8, &promoted, sizeof promoted);
    *out_type = ffi_data_ffi_type(BCODE_FFI_DATA_I32);
    *out_ptr = slot8;
    return true;
}

static value_t *op_extern_call(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t qual = bcode_read_str(bc, pc);
    uint32_t argc = bcode_read_u32(bc, pc);
    uint32_t packed = bcode_read_u32(bc, pc);
    uint32_t ret_kind = BCODE_FFI_RET_UNPACK_KIND(packed);
    bool variadic = BCODE_FFI_RET_UNPACK_VARIADIC(packed);
    size_t nfixed = BCODE_FFI_RET_UNPACK_NFIXED(packed);

    if (argc > FFI_MAX_ARGS)
        return value_make_error(vm, "exec: extern call: too many arguments");
    if (variadic && nfixed > argc) nfixed = argc; /* 旧产物/异常编码兜底 */

    /* 返回类型：码 → clux 类型 + libffi 描述符（void 用 NULL 表示"无返回值"）。
       STR → clux str（读回 char*）；TYPED → 类型值在栈上（见下）。 */
    const type_t *rt = vm->type_void;
    const void *rffi = NULL;
    bool ret_owned = false;
    if (ret_kind == BCODE_FFI_RET_STR) {
        rt = vm->type_str;
        rffi = ffi_data_ffi_type(BCODE_FFI_DATA_PTR);
    } else if (ret_kind != BCODE_FFI_RET_VOID &&
               ret_kind != BCODE_FFI_RET_TYPED) {
        uint32_t rdk = ffi_ret_to_data_kind(ret_kind);
        size_t rsize = 0;
        rt = (rdk == (uint32_t)-1) ? NULL : ffi_data_type_of(vm, rdk, &rsize);
        rffi = (rdk == (uint32_t)-1) ? NULL : ffi_data_ffi_type(rdk);
        if (!rt || !rffi)
            return value_make_error(vm, "exec: extern call: bad return type code");
    }

    /* 拆 "lib::name" → 库表 → 符号地址（与 extern load/store 共用同一实现） */
    void *sym = NULL;
    value_t *rerr = ffi_resolve_symbol(vm, bc, qual, "extern call", &sym);
    if (rerr) return rerr;
    char err[512] = {0};

    /* 弹实参（栈顶是最后一个实参）→ 逐个取 C 表示 + libffi 描述符。
       定参直接借用 value->data（与 C 同构）；**变参位置**做 C 默认实参提升
       （f32→double、窄整型→int），提升结果放在下面的槽里。 */
    value_t *argv[FFI_MAX_ARGS];
    const void *argtypes[FFI_MAX_ARGS];
    void *argvals[FFI_MAX_ARGS];
    bool argowned[FFI_MAX_ARGS];
    uint8_t vararg_slots[FFI_MAX_ARGS][8];
    for (size_t i = argc; i > 0; i--) {
        value_t *a = exec_stack_pop(vm);
        if (value_is_error(vm, a)) return a;
        argv[i - 1] = a;
    }
    memset(argowned, 0, sizeof(argowned));
    for (size_t i = 0; i < argc; i++) {
        const void *at_type = NULL;
        void *at_ptr = NULL;
        bool is_vararg = variadic && i >= nfixed;
        if (is_vararg &&
            ffi_promote_vararg(vm, argv[i], &at_type, &at_ptr, vararg_slots[i])) {
            argtypes[i] = at_type;
            argvals[i] = at_ptr;
            continue;
        }
        if (!ffi_arg_desc(vm, argv[i], &at_type, &at_ptr, &argowned[i], err,
                          sizeof(err))) {
            for (size_t k = 0; k < i; k++)
                if (argowned[k]) ffi_type_release(argtypes[k]);
            return value_make_error(
                vm, "exec: extern call: argument type cannot cross the C ABI");
        }
        argtypes[i] = at_type;
        argvals[i] = at_ptr;
    }

    /* TYPED 返回：类型值在实参**之下**（compiler 先压类型再压实参）→ 现在弹它，
       并据此构造返回描述符（struct/cunion/enum/带类型的指针）。 */
    uint8_t *retbuf = NULL;
    if (ret_kind == BCODE_FFI_RET_TYPED) {
        value_t *tv = exec_stack_pop(vm);
        /* 类型值的数据就是 type_t 本身（与 op_construct 同一约定） */
        if (tv && value_type(tv) == vm->type_type)
            rt = value_as(tv, const type_t *);
        if (!rt || rt == vm->type_void || rt == vm->type_type) {
            for (size_t k = 0; k < argc; k++)
                if (argowned[k]) ffi_type_release(argtypes[k]);
            return value_make_error(
                vm, "exec: extern call: missing type slot for typed return");
        }
        rffi = ffi_type_for(rt, vm, &ret_owned, err, sizeof(err));
        if (!rffi) {
            for (size_t k = 0; k < argc; k++)
                if (argowned[k]) ffi_type_release(argtypes[k]);
            return value_make_error(vm, err);
        }
    }

    /* libffi 精确类型调用（定参走 ffi_prep_cif，变参走 ffi_prep_cif_var）。
       返回槽：标量 16 字节足够；struct/cunion 按类型的 size 现分配。 */
    uint8_t small_ret[16] = {0};
    size_t ret_size = sizeof(small_ret);
    if (ret_kind == BCODE_FFI_RET_TYPED && rt && rt->size > sizeof(small_ret)) {
        retbuf = (uint8_t *)malloc(rt->size);
        if (!retbuf) {
            for (size_t k = 0; k < argc; k++)
                if (argowned[k]) ffi_type_release(argtypes[k]);
            if (ret_owned) ffi_type_release(rffi);
            return value_make_error(vm, "exec: extern call: out of memory");
        }
        ret_size = rt->size;
    } else {
        retbuf = small_ret;
    }
    if (ret_size > sizeof(small_ret)) memset(retbuf, 0, ret_size);

    bool ok = ffi_call_typed_items(sym, variadic ? nfixed : argc, argc, argtypes,
                                   rffi, argvals,
                                   ret_kind == BCODE_FFI_RET_VOID ? NULL : retbuf,
                                   err, sizeof(err));
    for (size_t k = 0; k < argc; k++)
        if (argowned[k]) ffi_type_release(argtypes[k]);
    if (ret_owned) ffi_type_release(rffi);
    if (!ok) {
        if (ret_size > sizeof(small_ret)) free(retbuf);
        return value_make_error(vm, err);
    }

    /* 返回值：retbuf 里是**该 C 类型的原生表示**，与 clux 类型的 data 同构
       （str 是指针、struct 是那块 C 布局字节），因此直接按键值复制即可
       （类型必须与源码声明一致，见 vm/bcode.h）。 */
    value_t *out;
    if (ret_kind == BCODE_FFI_RET_VOID) {
        out = value_make_undefined(vm);
    } else {
        out = value_make(vm, rt, value_alloc_data_copy(vm->alloc, rt, retbuf));
    }
    if (ret_size > sizeof(small_ret)) free(retbuf);
    return out;
}

/* DATA（无操作数）：从**序列值**取首元素指针（C ABI 适配）。
 *
 * 弹一个 array/slice/str 值（借用视图即可，本指令只读它的存储身份）→ 压
 * `ref *T`（T = 元素类型；str → u8，即 C 的 `const char *`），指向首元素。
 * 与 C 的 `(T*, size_t)` 对接：`f(data(s), len(s))`。
 *
 * 恒 `ref`（借用、永不拥有），dispose 跳过；空序列可能得到 NULL（C 侧不得解引用）。
 * 借用有效期 = 序列值的生命周期（与 `x.&` 同一约定）。 */
static value_t *op_data(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *sv = exec_stack_pop(vm);
    if (value_is_error(vm, sv)) return sv;
    const type_t *st = value_type(sv);
    if (!st) return value_make_error(vm, "data: argument has no type");

    const type_t *elem = NULL;
    void *first = NULL;
    switch (st->kind) {
    case TYPE_KIND_SLICE: {
        slice_value_t sval = slice_view(sv);
        elem = slice_type_elem(st);
        first = sval.ptr;
        break;
    }
    case TYPE_KIND_ARRAY:
        /* 数组元素连续存放在值的数据块里 → value_data 即元素 0 的地址 */
        elem = array_type_elem(st);
        first = value_data(sv);
        break;
    case TYPE_KIND_STR:
        /* str 的 data 槽里存的是 `const char *`（与 C 同构），解一层引用 */
        elem = vm->type_u8;
        first = value_data(sv) ? *(void **)value_data(sv) : NULL;
        break;
    default:
        return value_make_error(
            vm, "data: argument must be array/slice/str");
    }
    if (!elem)
        return value_make_error(vm, "data: sequence has no element type");

    const type_t *pt = type_ptr_intern(vm, elem);
    if (!pt) return value_make_error(vm, "data: cannot make pointer type");
    if (value_is_shadow(sv)) return value_make_shadow(vm, pt);
    return ptr_make_value(vm, pt, first); /* ref 借用：不拥有 */
}

static value_t *op_import(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t raw = bcode_read_str(bc, pc);

    module_import_kind_t kind = MODULE_IMPORT_MODULE;
    char *canonical =
        module_resolve_import(vm->alloc, raw, bc->module_path, &kind);
    if (!canonical) {
        char msg[1152];
        snprintf(msg, sizeof(msg), "exec: cannot resolve module path '%.*s'",
                 (int)raw.len, raw.ptr);
        return value_make_error(vm, msg);
    }

    value_t *mod_val = vm_import(vm, canonical);
    if (!mod_val) {
        char msg[1152];
        snprintf(msg, sizeof(msg), "exec: module not found '%s'", canonical);
        allocator_free(vm->alloc, (void **)&canonical);
        return value_make_error(vm, msg);
    }
    allocator_free(vm->alloc, (void **)&canonical);
    return mod_val;
}

/* GET_MEMBER <str>：成员名。弹模块值 → 从 **module->exports** 按名取成员值压栈。
 *
 * 导出表是唯一的成员可见性来源，与 sema 的 `::` 解析（sema/expr.c 查
 * module->exports）以及 m5-design-v2 实现步骤 7 一致。此前查 global_scope：
 * 虽然 sema 已在编译期拦截未导出成员，但**代理导出**（`export * from`）只并入
 * exports 而不进 global_scope——若运行期查 global_scope，`app::add` 这类经代理
 * 导出可见的成员会在运行期找不到（sema 通过、运行期失败）。 */
static value_t *op_get_member(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *mod_val = exec_stack_pop(vm);
    if (!mod_val || value_type(mod_val) != vm->type_module) {
        return value_make_error(vm, "exec: GET_MEMBER expects a module value");
    }
    module_t *mod = value_as(mod_val, module_t *);
    if (!mod || !mod->exports) {
        return value_make_error(vm, "exec: invalid module in GET_MEMBER");
    }

    char buf[256];
    size_t n = name.len < sizeof(buf) - 1 ? name.len : sizeof(buf) - 1;
    memcpy(buf, name.ptr, n);
    buf[n] = '\0';

    value_t *member = (value_t *)strmap_get(mod->exports, buf);
    if (!member) {
        char msg[320];
        snprintf(msg, sizeof(msg), "exec: module has no member '%s'", buf);
        return value_make_error(vm, msg);
    }
    return member; /* 借用引用，归被导出符号所在模块的 global_scope 拥有 */
}

/* ---- struct 静态成员（attribute）读写 ----
 * struct_type_t 运行时持有 attributes（name→value_t* 借用映射）。value 生命
 * 周期归 member_scope（挂在 module global_scope 下的子作用域）。
 *
 * LOAD_ATTR <type_id> <name>：按 type_id 查 struct 类型 → 从 attributes 按
 *   名取值压栈。沿嵌入链继承查找（自己未命中时递归查各嵌入类型）。
 * STORE_ATTR <type_id> <name>：弹栈顶值 → 写入 attributes[name]。首次写入
 *   时初始化 member_scope + attributes + 注册 value 到 member_scope（生命
 *   周期管理）。后续写入用 scope_set 替换值（旧值由 scope 释放）。
 */

/* 沿嵌入链递归查找拥有指定属性名的 struct 类型（自己优先，再沿 embeds 深度
 * 优先）。返回命中的 struct_type_t，未找到返回 NULL。 */
static const struct_type_t *find_attr_owner(const type_t *t, const char *name) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
    const struct_type_t *st = (const struct_type_t *)t;
    if (st->attributes && strmap_contains(st->attributes, name))
        return st;
    size_t ne = 0;
    const type_t *const *emb = type_struct_embeds(t, &ne);
    for (size_t i = 0; i < ne; i++) {
        const struct_type_t *owner = find_attr_owner(emb[i], name);
        if (owner) return owner;
    }
    return NULL;
}

static value_t *op_load_attr(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t type_id = bcode_read_u32(bc, pc);
    strslice_t name = bcode_read_str(bc, pc);

    char buf[256];
    size_t n = name.len < sizeof(buf) - 1 ? name.len : sizeof(buf) - 1;
    memcpy(buf, name.ptr, n);
    buf[n] = '\0';

    const type_t *t = vm_type_load(vm, type_id);
    if (!t || t->kind != TYPE_KIND_STRUCT) {
        return value_make_error(vm, "exec: LOAD_ATTR: invalid struct type");
    }
    /* 沿嵌入链查找拥有该属性的 struct 类型 */
    const struct_type_t *owner = find_attr_owner(t, buf);
    if (!owner || !owner->attributes) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "exec: struct '%.*s' has no attribute '%s'",
                 (int)t->name.len, t->name.ptr, buf);
        return value_make_error(vm, msg);
    }
    value_t *val = (value_t *)strmap_get(owner->attributes, buf);
    if (!val) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "exec: struct '%.*s' attribute '%s' not initialized",
                 (int)t->name.len, t->name.ptr, buf);
        return value_make_error(vm, msg);
    }
    return val; /* 借用引用，归 member_scope 拥有 */
}

static value_t *op_store_attr(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t type_id = bcode_read_u32(bc, pc);
    strslice_t name = bcode_read_str(bc, pc);

    char buf[256];
    size_t n = name.len < sizeof(buf) - 1 ? name.len : sizeof(buf) - 1;
    memcpy(buf, name.ptr, n);
    buf[n] = '\0';

    const type_t *t = vm_type_load(vm, type_id);
    if (!t || t->kind != TYPE_KIND_STRUCT) {
        return value_make_error(vm, "exec: STORE_ATTR: invalid struct type");
    }
    /* type_id 已由 sema 解析为实际拥有属性的 struct 类型（覆盖语义→自身，
       共享语义→继承源），直接在其 attributes 上写入。 */
    struct_type_t *st = (struct_type_t *)t;

    value_t *val = exec_stack_pop(vm);
    if (!val) return value_make_error(vm, "exec: STORE_ATTR: stack underflow");

    /* 确保成员容器已初始化 */
    if (!type_struct_members_init(vm, t)) {
        return value_make_error(vm, "exec: STORE_ATTR: failed to init members");
    }
    /* 首次写入：创建 member_scope（挂在 vm->current_scope 下）+ 注册 value */
    if (!st->member_scope) {
        scope_t *mscope = scope_new(vm->alloc, vm->current_scope);
        if (!mscope) return value_make_error(vm, "exec: STORE_ATTR: OOM creating member scope");
        type_struct_set_member_scope(vm, &st->base, mscope);
    }
    /* value 注册到 member_scope（生命周期管理）。
       首次写入用 scope_define，后续写入用 scope_set（覆盖旧值防重复定义报错）。 */
    value_t *stored;
    if (strmap_contains(st->attributes, buf)) {
        stored = scope_set(vm, st->member_scope, buf, val);
    } else {
        stored = scope_define(vm, st->member_scope, buf, val);
    }
    if (value_is_error(vm, stored)) return stored;
    /* attributes 存借用引用（scope_define/scope_set 返回的就是 scope 拥有的 value） */
    strmap_insert(st->attributes, vm->alloc, buf, (void *)stored);
    return NULL; /* 赋值是语句，不压回结果 */
}

/* ---- struct 方法调用（MEMBER_CALL）----
 * 不复用 CALL：方法 func value 存在 struct_type 的 attributes 表中（非全局
 * 作用域），运行期从 receiver 的 struct 类型中解析。sema 标记 AST_CALL，
 * compiler 据此发 MEMBER_CALL（不改写为 AST_FUNC_REF + 接收者插首参）。
 *
 * 栈: [receiver, arg1..argN] → [result]。净变化 = -argc。
 * - 实例方法：receiver 是值或指针。值 → 取地址（ref *T）作为 self 参数；
 *   指针 → 原样作为 self 参数。总参数 = argc + 1。
 * - 静态方法：receiver 是 type value → 只用于确定在哪个 struct 类型的
 *   attributes 中查找方法，不作为参数传递。总参数 = argc。 */
static value_t *op_member_call(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    uint32_t argc = bcode_read_u32(bc, pc);

    char buf[256];
    size_t n = name.len < sizeof(buf) - 1 ? name.len : sizeof(buf) - 1;
    memcpy(buf, name.ptr, n);
    buf[n] = '\0';

    size_t sp = vec_len(vm->stack);
    /* receiver 在 stack[sp-1-argc]（与 CALL 的 callee 同位） */
    value_t *receiver = exec_stack_peek(vm, argc);

    /* 确定 struct 类型 + 是否静态调用（receiver 是 type value） */
    const type_t *struct_type = NULL;
    bool is_static = false;
    if (value_type(receiver) == vm->type_type) {
        /* 静态方法 T::f(args)：receiver 是 type value */
        is_static = true;
        struct_type = value_as(receiver, const type_t *);
    } else {
        /* 实例方法 a.b(args)：receiver 是值或指针 */
        const type_t *rt = value_type(receiver);
        if (!rt) {
            for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm);
            return value_make_error(vm, "exec: MEMBER_CALL: receiver has no type");
        }
        /* 指针 → 取 base 类型 */
        const type_t *base = ptr_type_base(rt);
        if (base) rt = base;
        struct_type = type_strip_quals(rt);
    }
    if (!struct_type || struct_type->kind != TYPE_KIND_STRUCT) {
        for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm);
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "exec: MEMBER_CALL: receiver is not a struct (method '%s')", buf);
        return value_make_error(vm, msg);
    }

    /* 沿嵌入链查找拥有该方法的 struct 类型（方法 func value 在 attributes 中） */
    const struct_type_t *owner = find_attr_owner(struct_type, buf);
    if (!owner || !owner->attributes) {
        for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm);
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "exec: struct '%.*s' has no method '%s'",
                 (int)struct_type->name.len, struct_type->name.ptr, buf);
        return value_make_error(vm, msg);
    }
    value_t *method = (value_t *)strmap_get(owner->attributes, buf);
    if (!method) {
        for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm);
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "exec: struct '%.*s' method '%s' not initialized",
                 (int)struct_type->name.len, struct_type->name.ptr, buf);
        return value_make_error(vm, msg);
    }

    /* 收集实参到 VLA（value_call 内部压栈可能 realloc 使栈缓冲悬垂） */
    if (is_static) {
        /* 静态方法：argc 个参数，无 self */
        value_t *args[argc > 0 ? argc : 1];
        for (uint32_t i = 0; i < argc; i++)
            args[i] = (value_t *)vec_get(vm->stack, sp - argc + i);
        for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm); /* pop args + receiver */
        return value_call(vm, method, args, argc);
    } else {
        /* 实例方法：self + argc 个参数 */
        const type_t *rt = value_type(receiver);
        const type_t *base = ptr_type_base(rt);
        bool by_ptr = (base != NULL);

        /* 构造 self 参数：指针 → 原样；值 → 取地址（ref *T） */
        value_t *self_arg;
        if (by_ptr) {
            self_arg = receiver;
        } else {
            /* 值 receiver → ref *T 借用指针（与 op_addr 同款） */
            const type_t *pt = type_ptr_intern(vm, rt);
            if (!pt) {
                for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm);
                return value_make_error(vm, "exec: MEMBER_CALL: cannot make pointer");
            }
            if (value_is_shadow(receiver))
                self_arg = value_make_shadow(vm, pt);
            else
                self_arg = ptr_make_value(vm, pt, value_data(receiver));
        }

        uint32_t total = argc + 1;
        value_t *args[total];
        args[0] = self_arg;
        for (uint32_t i = 0; i < argc; i++)
            args[1 + i] = (value_t *)vec_get(vm->stack, sp - argc + i);
        for (uint32_t i = 0; i <= argc; i++) exec_stack_pop(vm); /* pop args + receiver */
        return value_call(vm, method, args, total);
    }
}

/* ---- 终止 ---- */

static value_t *op_halt(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    vm->halted = true;
    return NULL;
}

/* ================================================================ */
/* 指令分派表（opcode → 回调）                                         */
/* ================================================================ */

static const bcode_handler_t HANDLERS[] = {
    [BCODE_PUSH]           = op_push,
    [BCODE_STORE]          = op_store,
    [BCODE_STORE_REINIT]   = op_store_reinit,
    [BCODE_STORE_NIL]      = op_store_nil,
    [BCODE_PUSH_STR]       = op_push_str,
    [BCODE_PUSH_I8]        = op_push_i8,
    [BCODE_PUSH_I16]       = op_push_i16,
    [BCODE_PUSH_I32]       = op_push_i32,
    [BCODE_PUSH_I64]       = op_push_i64,
    [BCODE_PUSH_U8]        = op_push_u8,
    [BCODE_PUSH_U16]       = op_push_u16,
    [BCODE_PUSH_U32]       = op_push_u32,
    [BCODE_PUSH_U64]       = op_push_u64,
    [BCODE_PUSH_F32]       = op_push_f32,
    [BCODE_PUSH_F64]       = op_push_f64,
    [BCODE_PUSH_BOOL]      = op_push_bool,
    [BCODE_PUSH_VALUE]     = op_push_value,
    [BCODE_LOAD]           = op_load,
    [BCODE_LOAD_TYPE]      = op_load_type,
    [BCODE_LOAD_FUNCTION]  = op_load_function,
    [BCODE_SET_TYPE_NAME]  = op_set_type_name,
    [BCODE_PUSH_UNDEFINED] = op_push_undefined,
    [BCODE_DEFINE]         = op_define,
    [BCODE_ADD]            = op_add,
    [BCODE_SUB]            = op_sub,
    [BCODE_MUL]            = op_mul,
    [BCODE_DIV]            = op_div,
    [BCODE_MOD]            = op_mod,
    [BCODE_EQ]             = op_eq,
    [BCODE_NE]             = op_ne,
    [BCODE_LT]             = op_lt,
    [BCODE_LE]             = op_le,
    [BCODE_GT]             = op_gt,
    [BCODE_GE]             = op_ge,
    [BCODE_AND]            = op_band,
    [BCODE_OR]             = op_bor,
    [BCODE_BXOR]           = op_bxor,
    [BCODE_SHL]            = op_shl,
    [BCODE_SHR]            = op_shr,
    [BCODE_NEG]            = op_neg,
    [BCODE_NOT]            = op_not,
    [BCODE_BNOT]           = op_bnot,
    [BCODE_CAST]           = op_cast,
    [BCODE_CREATE_CONST]   = op_create_const,
    [BCODE_CREATE_VOLATILE]= op_create_volatile,
    [BCODE_PUSH_CONST]     = op_push_const,
    [BCODE_PUSH_VOLATILE]  = op_push_volatile,
    [BCODE_PUSH_OPT]       = op_push_opt,
    [BCODE_PUSH_ENUM]      = op_push_enum,
    [BCODE_ENUM_VARIANT]   = op_enum_variant,
    [BCODE_MAKE_ENUM]      = op_make_enum,
    [BCODE_SET_TYPE]       = op_set_type,
    [BCODE_PUSH_FUNC_TYPE]   = op_push_func_type,
    [BCODE_FUNC_TYPE_PARAM]  = op_func_type_param,
    [BCODE_FUNC_TYPE_RETURN] = op_func_type_return,
    [BCODE_FUNC_TYPE_VARARG] = op_func_type_vararg,
    [BCODE_DEFINE_TYPE]      = op_define_type,
    [BCODE_SEAL]             = op_seal,
    [BCODE_PUSH_FUNCTION]  = op_push_function,
    [BCODE_BIND_FUNC]      = op_bind_func,
    [BCODE_SET_FUNC_NAME]  = op_set_func_name,
    [BCODE_SET_CLOSURE]    = op_set_closure,
    [BCODE_MAKE_FUNCTION]  = op_make_function,
    [BCODE_CALL]           = op_call,
    [BCODE_RET]            = op_ret,
    [BCODE_JMP]            = op_jmp,
    [BCODE_JZ]             = op_jz,
    [BCODE_JNZ]            = op_jnz,
    [BCODE_PUSH_SCOPE]     = op_push_scope,
    [BCODE_POP_SCOPE]      = op_pop_scope,
    [BCODE_POP]            = op_pop,
    [BCODE_HALT]           = op_halt,
    [BCODE_PUSH_ARRAY]      = op_push_array,
    [BCODE_DEFINE_BOUND]    = op_define_bound,
    [BCODE_PUSH_STRUCT]     = op_push_struct,
    [BCODE_DEFINE_FIELD]    = op_define_field,
    [BCODE_ADD_EMBED]       = op_add_embed,
    [BCODE_PUSH_TUPLE]      = op_push_tuple,
    [BCODE_APPEND_ELEM]     = op_append_elem,
    [BCODE_PUSH_UNION]      = op_push_union,
    [BCODE_UNION_MEMBER]    = op_union_member,
    [BCODE_IS_TAG]          = op_is_tag,
    [BCODE_PUSH_CUNION]     = op_push_cunion,
    [BCODE_CONSTRUCT]       = op_construct,
    [BCODE_INDEX_GET]       = op_index_get,
    [BCODE_INDEX_SET]       = op_index_set,
    [BCODE_FIELD_GET]       = op_field_get,
    [BCODE_FIELD_SET]       = op_field_set,
    [BCODE_LENGTH]          = op_length,
    [BCODE_PUSH_OPT_NONE]   = op_push_opt_none,
    [BCODE_UNWRAP]          = op_unwrap,
    [BCODE_OPT_IS_NONE]     = op_opt_is_none,
    [BCODE_PUSH_PTR]        = op_push_ptr,
    [BCODE_NEW]             = op_new,
    [BCODE_PTR_GET]         = op_ptr_get,
    [BCODE_PTR_SET]         = op_ptr_set,
    [BCODE_ADDR]            = op_addr,
    [BCODE_MOVE]            = op_move,
    [BCODE_CLONE]           = op_clone,
    [BCODE_DISPOSE]         = op_dispose,
    [BCODE_PUSH_SLICE]      = op_push_slice,
    [BCODE_SLICE]           = op_slice,
    [BCODE_MAKE]            = op_make,
    [BCODE_IMPORT]          = op_import,
    [BCODE_GET_MEMBER]      = op_get_member,
    [BCODE_EXTERN_CALL]     = op_extern_call,
    [BCODE_EXTERN_LOAD]     = op_extern_load,
    [BCODE_EXTERN_STORE]    = op_extern_store,
    [BCODE_DATA]            = op_data,
    [BCODE_LOAD_ATTR]       = op_load_attr,
    [BCODE_STORE_ATTR]      = op_store_attr,
    [BCODE_MARK_UNIQUE]     = op_mark_unique,
    [BCODE_MEMBER_CALL]     = op_member_call,
    [BCODE_FREE]            = op_free,
    [BCODE_FREE_RECURSIVE]  = op_free_recursive,
    [BCODE_SET_FIELD_OWNERSHIP] = op_set_field_ownership,
};

/* ================================================================ */
/* 驱动循环 + 主循环                                                  */
/* ================================================================ */

value_t *exec_drive(vm_t *vm, bytecode_t *bc, size_t start_pc) {
    vm->pc = start_pc;
    for (;;) {
        if (vm->halted) return NULL;  /* HALT 指令设 halted，正常退出 */
        bcode_op_t op = bcode_read_op(bc, &vm->pc);
        bcode_handler_t h = HANDLERS[op];
        if (!h) return value_make_error(vm, "exec: unimplemented opcode");
        value_t *r = h(vm, bc, &vm->pc);
        if (r) exec_stack_push(vm, r);
        if (value_is_error(vm, r)) return r;      /* 引擎级硬错误，停止 */
        if (value_is_interrupt(vm, r)) return r;  /* RET 哨兵，函数子循环捕获 */
    }
}

value_t *exec_run(vm_t *vm, bytecode_t *bc) {
    vm->bc     = bc;
    vm->halted = false;

    /* 清空操作数栈（借用引用，不释放） */
    while (!vec_is_empty(vm->stack)) vec_pop(vm->stack);

    /* clux 无顶层语句：只执行函数注册段，入口函数由调用方
       scope_lookup + value_call 显式触发 */
    return exec_drive(vm, bc, 0);
}
