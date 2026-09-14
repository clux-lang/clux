#include "vm/exec.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/type.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_type.h"
#include "vm/type_interrupt.h"
#include "vm/bcode_function.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/vec.h"

#include <string.h>

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
    return v; /* 借用引用 */
}

static value_t *op_store(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *src = exec_stack_pop(vm);
    value_t *dst = scope_lookup(vm->current_scope, name);
    if (!dst) return value_make_error(vm, "exec: undefined variable in assignment");
    return value_assign(vm, dst, src);
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
    string_t *str = string_from_bytes(vm->alloc, s.ptr, s.len);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_str, &str);
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

/* ---- 类型 id 指令（LOAD_TYPE / BIND_TYPE / SET_TYPE_NAME） ---- */

/* LOAD_TYPE <id>：从 types_by_id 查表，压入该类型的 type value */
static value_t *op_load_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    const type_t *t = vm_type_load(vm, id);
    if (!t) return value_make_error(vm, "exec: unknown type id");
    return type_as_value(vm, t);
}

/* BIND_TYPE <id>：弹栈顶 type value，登记 id→type（幂等；seal 去重后重绑）。
   程序 id（>= TYPE_ID_PROGRAM_BASE）同步写 t->id——SET_TYPE_NAME 按 id 判
   定"可改名"。sema 路径已设 t->id（sema_type_register），此处幂等冗余；
   asm 手写路径（无 sema）依赖此同步。 */
static value_t *op_bind_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    value_t *tv = exec_stack_pop(vm);
    if (!tv || value_type(tv) != vm->type_type)
        return value_make_error(vm, "exec: bind type expects a type value");
    const type_t *t = *(const type_t **)value_data(tv);
    if (id >= TYPE_ID_PROGRAM_BASE) ((type_t *)t)->id = id;
    vm_type_bind(vm, id, t);
    return NULL;
}

/* SET_TYPE_NAME <name>：弹栈顶 type value，设置其显示名（覆盖规范名） */
static value_t *op_set_type_name(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *tv = exec_stack_pop(vm);
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
    /* spec 是 undefined：无显式类型，从值推断 */
    if (!decl_type) decl_type = value_type(init);

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

/* SEAL：密封栈顶 type value（统一 SEAL 命令，func/array/struct/tuple 通用）。
 * 经 value_seal 代理到 type->vtable->type_seal（去重复用时由 value_seal
 * 重定向操作数栈中所有引用旧 type 的 type value，无悬空、零泄漏）。 */
static value_t *op_seal(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_seal(vm, exec_stack_peek(vm, 0));
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

/* ---- 值构造（construct N）：弹 N 个成员值 + 类型位 → value ----
 * 栈布局（构造期）：[..., type_value, v1, v2, ..., vN]（type 在底、vN 在顶）。
 * 先弹 type_value（栈上类型构造产物，如 push_array...seal 留下的 array type
 * value），再逐个弹 N 个成员值（栈顶为 vN，逆序收集为 elems[0..n-1] = v1..vN）。
 * 按类型种类分派：当前仅实现 array 分支（struct/tuple 待后续 Phase）。 */
static value_t *op_construct(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t n = bcode_read_u32(bc, pc);

    /* 栈上收集 N 个成员值（仅指针，不拥有 value）。VLA 与 op_call 的 args 同款。
       栈布局：..., type_value, v1, ..., vN（type 在底、vN 在顶），故先弹 vN..v1，
       逆序写入使 elems[0..n-1] = v1..vN，最后再弹类型位。 */
    value_t *elems[n > 0 ? n : 1];
    for (uint32_t i = 0; i < n; i++)
        elems[n - 1 - i] = exec_stack_pop(vm);

    /* 类型位在 N 个成员值之下 */
    value_t *type_v = exec_stack_pop(vm);
    const type_t *t = (type_v && value_type(type_v) == vm->type_type)
                          ? value_as(type_v, const type_t *) : NULL;
    if (!t)
        return value_make_error(vm, "construct: missing type slot");

    /* 仅实现 array 分支 */
    if (t->kind == TYPE_KIND_ARRAY) {
        const type_t *et = array_type_elem(t);
        /* 校验成员数（定长数组须与边界一致；动态数组 SIZE_MAX 不限） */
        size_t len = array_type_len(t);
        if (len != SIZE_MAX && len != (size_t)n)
            return value_make_error(vm,
                "construct: array element count mismatch");
        return value_make_array(vm, et, n > 0 ? elems : NULL, n);
    }

    /* 非 array 类型暂未实现 */
    return value_make_error(vm,
        "construct: unsupported type (only array implemented)");
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

/* ---- 长度查询（length）：弹 self → value_length(self)（代理到 vtable->length）
 * 当前仅数组实现：返回 u64 元素个数 shadow 下返回 u64 shadow）。 */
static value_t *op_length(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *self = exec_stack_pop(vm);
    return value_length(vm, self);
}

static value_t *op_push_function(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* entry_pc 立即数；弹栈顶签名类型（CREATE_FUNC_TYPE 产物），
       构造 bcode_function_t（封装在 bcode_function 模块内） */
    uint32_t entry_pc = bcode_read_u32(bc, pc);
    value_t *sig_v = exec_stack_pop(vm);
    const type_t *sig = *(const type_t **)value_data(sig_v);
    return bcode_function_new(vm, sig, entry_pc, vm->root_scope);
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
    [BCODE_BIND_TYPE]      = op_bind_type,
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
    [BCODE_PUSH_FUNC_TYPE]   = op_push_func_type,
    [BCODE_FUNC_TYPE_PARAM]  = op_func_type_param,
    [BCODE_FUNC_TYPE_RETURN] = op_func_type_return,
    [BCODE_FUNC_TYPE_VARARG] = op_func_type_vararg,
    [BCODE_SEAL]             = op_seal,
    [BCODE_PUSH_FUNCTION]  = op_push_function,
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
    [BCODE_CONSTRUCT]       = op_construct,
    [BCODE_INDEX_GET]       = op_index_get,
    [BCODE_INDEX_SET]       = op_index_set,
    [BCODE_LENGTH]          = op_length,
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
