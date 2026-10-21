#include "vm/exec.h"
#include "vm/vm.h"
#include "vm/value.h"
#include "vm/type_error.h"
#include "vm/value_owncopy.h"  /* value_own_copy_into（构造字段写入） */
#include "vm/type.h"
#include "vm/type_ownership.h"
#include "vm/type_cfunc.h"
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


#include "exec_internal.h"

/* ---- 二元/一元运算 ---- */

#define BINARY_OP(name, value_fn)                                              \
    value_t *op_##name(vm_t *vm, bytecode_t *bc, size_t *pc) {         \
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

value_t *op_neg(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_neg(vm, exec_stack_pop(vm));
}

value_t *op_not(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_lnot(vm, exec_stack_pop(vm));
}

value_t *op_bnot(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_bnot(vm, exec_stack_pop(vm));
}

/* ---- 显式转换 ---- */

value_t *op_cast(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* 类型经栈顶 type value 传递（LOAD 压入），无立即数操作数：
       弹 type value → 弹被转换值 → value_explicit_cast（error 内部短路） */
    (void)bc; (void)pc;
    value_t *vtype = exec_stack_pop(vm);
    value_t *value = exec_stack_pop(vm);
    const type_t *target = *(const type_t **)value_data(vtype);
    return value_explicit_cast(vm, value, target);
}

/* ---- 限定类型构造（const/volatile） ---- */

value_t *op_create_const(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* 弹 type value → intern const 类型 → type value 压回 */
    (void)bc; (void)pc;
    value_t *vtype = exec_stack_pop(vm);
    const type_t *t = *(const type_t **)value_data(vtype);
    const type_t *ct = type_const_intern(vm, t);
    if (!ct) return value_make_error(vm, "exec: cannot make const type");
    return type_as_value(vm, ct);
}

value_t *op_create_volatile(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *vtype = exec_stack_pop(vm);
    const type_t *t = *(const type_t **)value_data(vtype);
    const type_t *vt = type_volatile_intern(vm, t);
    if (!vt) return value_make_error(vm, "exec: cannot make volatile type");
    return type_as_value(vm, vt);
}

/* ---- 所有权修饰的开放构造（PUSH_OWN/PUSH_REF/PUSH_FATAL） ----
 *
 * 与 const/volatile 同样的两步协议（PUSH_x → SET_TYPE sub → SEAL），但 SEAL
 * 结果是**裸类型**：所有权是纯编译期概念，运行期剥离/穿透（零行为）。于是
 * 类型的 program id 映射到 sub 的实例，`LOAD_TYPE <id>` 拿到的就是裸类型。
 */
static value_t *op_push_ownership(vm_t *vm, type_kind_t kind) {
  ownership_type_t *ot = type_ownership_push_open(vm, kind);
  if (!ot) return value_make_error(vm, "exec: PUSH_OWN: out of memory");
  vec_push(vm->stack, vm->alloc, type_as_value(vm, &ot->base));
  return NULL;
}

value_t *op_push_own(vm_t *vm, bytecode_t *bc, size_t *pc) {
  (void)bc; (void)pc;
  return op_push_ownership(vm, TYPE_KIND_OWN);
}

value_t *op_push_ref(vm_t *vm, bytecode_t *bc, size_t *pc) {
  (void)bc; (void)pc;
  return op_push_ownership(vm, TYPE_KIND_REF);
}

value_t *op_push_fatal(vm_t *vm, bytecode_t *bc, size_t *pc) {
  (void)bc; (void)pc;
  return op_push_ownership(vm, TYPE_KIND_FATAL);
}

/* ---- 限定类型开放构造（PUSH_CONST/PUSH_VOLATILE + SET_TYPE + SEAL） ---- */

/* PUSH_CONST：分配空 const type（开放，sub=NULL，不入池）+ 压其 type value
 * （type_const_push 压栈；对应两遍构造声明阶段的起点） */
value_t *op_push_const(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_const_push(vm);
    return NULL;
}

/* PUSH_VOLATILE：分配空 volatile type（开放，sub=NULL，不入池）+ 压其 type value */
value_t *op_push_volatile(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_volatile_push(vm);
    return NULL;
}

/* PUSH_OPT：分配空 optional type（开放，inner=NULL，不入池）+ 压其 type value
 * （type_option_push 压栈；对应两遍构造声明阶段的起点） */
value_t *op_push_opt(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_option_push(vm);
    return NULL;
}

/* PUSH_ENUM：分配空 enum type（开放，underlying=NULL，不入池）+ 压其 type
 * value（type_enum_push 压栈；对应两遍构造声明阶段的起点） */
value_t *op_push_enum(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_enum_push(vm);
    return NULL;
}

/* ENUM_VARIANT <name> <value>：peek 栈顶开放 enum type（不弹栈——后续
 * ENUM_VARIANT / SEAL 继续消费），追加一个 variant（名从 strtable 拷贝，
 * 值按底层宽度截断）。 */
value_t *op_enum_variant(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_make_enum(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_set_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
/* MATERIALIZE（无操作数）：栈顶值 → 独立副本（value_clone）。
 *
 * 为什么需要：`return <借用视图>`（典型 `return a0.*`，a0 是 own 形参）压栈的
 * 是**指向参数堆块**的借用；随后 defers 链里的 FREE 释放该堆块，RET 就把悬垂
 * 借用交给调用方 → 调用方读到已释放内存（Windows 调试堆 0xDDDDDDDD、Linux 0）。
 * 语义上形参就是临时变量：返回值必须**先落成独立值**，再做作用域清理。 */
value_t *op_materialize(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (!v) return NULL;
    if (value_is_error(vm, v) || value_is_interrupt(vm, v)) return v;
    if (value_is_shadow(v)) return v; /* 无数据可物化 */
    value_t *copy = value_clone(vm, v);
    if (!copy || value_is_error(vm, copy)) return copy ? copy : v;
    return copy;
}

value_t *op_push_cfunc_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    cfunc_type_push(vm); /* 压入**开放** cfunc 类型（与 PUSH_FUNC_TYPE 同构） */
    return NULL;
}

value_t *op_push_func_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    func_type_push(vm);  /* 压入 func type 的 type value（见 func_type_push） */
    return NULL;
}

/* 取栈顶的**签名类型**（func 或 cfunc；调用前已确保它是当前构造目标） */
static const type_t *stack_top_func_type(vm_t *vm) {
    value_t *ftv = exec_stack_peek(vm, 0);
    return (ftv && value_type(ftv) == vm->type_type)
               ? value_as(ftv, const type_t *)
               : NULL;
}

/* FUNC_TYPE_PARAM：弹栈 type value → 追加为下一参数 */
value_t *op_func_type_param(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *pv = exec_stack_pop(vm);
    const type_t *param = *(const type_t **)value_data(pv);
    const type_t *ft = stack_top_func_type(vm);
    if (type_is_cfunc(ft))
        cfunc_type_add_param(vm, ft, param);
    else
        func_type_add_param(vm, ft, param);
    return NULL;
}

/* FUNC_TYPE_RETURN：弹栈 type value → 设为返回类型 */
value_t *op_func_type_return(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *rv = exec_stack_pop(vm);
    const type_t *ret = *(const type_t **)value_data(rv);
    const type_t *ft = stack_top_func_type(vm);
    if (type_is_cfunc(ft))
        cfunc_type_set_return(vm, ft, ret);
    else
        func_type_set_return(vm, ft, ret);
    return NULL;
}

/* FUNC_TYPE_VARARG：标记可变参数 */
value_t *op_func_type_vararg(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    const type_t *ft = stack_top_func_type(vm);
    if (type_is_cfunc(ft))
        cfunc_type_set_variadic(vm, ft, true);
    else
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
value_t *op_define_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_seal(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_push_array(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    array_type_push(vm);  /* 压入 array type 的 type value（见 array_type_push） */
    return NULL;
}

/* DEFINE_BOUND N：弹栈顶元素 type value → 设为元素类型，边界立即数 N 设为长度。
 * 弹元素类型后，栈顶即当前构造的 array type（由 PUSH_ARRAY 压入）。 */
value_t *op_define_bound(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_push_struct(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_struct_push(vm);
    return NULL;
}

/* DEFINE_FIELD <name>：弹栈顶 type value（字段类型）→ peek 开放 struct →
 * 追加字段（名从 strtable 拷贝）。弹字段类型后，栈顶即当前构造的 struct
 * type（由 PUSH_STRUCT 压入），与 DEFINE_BOUND 同款协议。
 * union 分支：栈顶是开放 union 时，字段追加到当前（最后追加）member 的
 * 开放 payload struct（type_union_add_field 内部处理）。 */
value_t *op_define_field(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

/* ADD_EMBED <embed_type_id>：peek 栈顶开放 struct → vm_type_load 拉回 embed
 * 类型 → type_struct_add_embed 追加到 embeds 表（seal 前可编辑）。
 * 无栈深变化（peek 不弹）。逐个追加，与 DEFINE_FIELD 同构。 */
value_t *op_add_embed(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_mark_unique(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_push_tuple(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_tuple_push(vm);
    return NULL;
}

/* APPEND_ELEM（无操作数）：弹栈顶 type value（元素类型）→ peek 开放 tuple →
 * 追加元素（元素匿名，长度=追加次数）。弹元素类型后，栈顶即当前构造的
 * tuple type（由 PUSH_TUPLE 压入），与 DEFINE_FIELD/DEFINE_BOUND 同款协议。 */
value_t *op_append_elem(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_push_union(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_union_push(vm);
    return NULL;
}

/* PUSH_CUNION：分配空 cunion type（开放，members=NULL，不入池）+ 压其 type
 * value（type_cunion_push 压栈；对应两遍构造声明阶段的起点） */
value_t *op_push_cunion(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    type_cunion_push(vm);
    return NULL;
}

/* UNION_MEMBER <name>：peek 栈顶开放 union type（不弹栈——后续 UNION_MEMBER /
 * DEFINE_FIELD / SEAL 继续消费），追加一个 member（名从 strtable 拷贝，
 * tag = 追加序）。 */
value_t *op_union_member(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_push_ptr(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint8_t rc = bcode_read_u8(bc, pc);
    type_ptr_push(vm, rc);
    return NULL;
}
