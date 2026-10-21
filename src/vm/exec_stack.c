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

/* ================================================================ */
/* 指令回调                                                          */
/* ================================================================ */

/* ---- 变量访问 ---- */

value_t *op_push(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

value_t *op_store(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_store_reinit(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_store_nil(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

value_t *op_push_i8(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int8_t v = bcode_read_i8(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i8, &v);
    return value_make(vm, vm->type_i8, data);
}

value_t *op_push_i16(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int16_t v = bcode_read_i16(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i16, &v);
    return value_make(vm, vm->type_i16, data);
}

value_t *op_push_i32(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int32_t v = bcode_read_i32(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i32, &v);
    return value_make(vm, vm->type_i32, data);
}

value_t *op_push_i64(vm_t *vm, bytecode_t *bc, size_t *pc) {
    int64_t v = bcode_read_i64(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_i64, &v);
    return value_make(vm, vm->type_i64, data);
}

value_t *op_push_u8(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint8_t v = bcode_read_u8(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u8, &v);
    return value_make(vm, vm->type_u8, data);
}

value_t *op_push_u16(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint16_t v = bcode_read_u16(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u16, &v);
    return value_make(vm, vm->type_u16, data);
}

value_t *op_push_u32(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t v = bcode_read_u32(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u32, &v);
    return value_make(vm, vm->type_u32, data);
}

value_t *op_push_u64(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint64_t v = bcode_read_u64(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_u64, &v);
    return value_make(vm, vm->type_u64, data);
}

/* usize 立即数：操作数恒 8 字节（`.cxb` 跨架构可搬运），按平台宽度截断后
   造成 **usize 类型**的值——PUSH_U64 造的是 u64，identity 严格处会类型不匹配。 */
value_t *op_push_usize(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint64_t v = bcode_read_u64(bc, pc);
    const type_t *ut = vm_type_usize(vm);
    if (ut->size == 4) {
        uint32_t v32 = (uint32_t)v;
        void *data = value_alloc_data_copy(vm->alloc, ut, &v32);
        return value_make(vm, ut, data);
    }
    void *data = value_alloc_data_copy(vm->alloc, ut, &v);
    return value_make(vm, ut, data);
}

value_t *op_push_f32(vm_t *vm, bytecode_t *bc, size_t *pc) {
    float v = bcode_read_f32(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_f32, &v);
    return value_make(vm, vm->type_f32, data);
}

value_t *op_push_f64(vm_t *vm, bytecode_t *bc, size_t *pc) {
    double v = bcode_read_f64(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_f64, &v);
    return value_make(vm, vm->type_f64, data);
}

value_t *op_push_bool(vm_t *vm, bytecode_t *bc, size_t *pc) {
    bool v = bcode_read_bool(bc, pc);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &v);
    return value_make(vm, vm->type_bool, data);
}

value_t *op_push_str(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t s = bcode_read_str(bc, pc);
    /* 字符串走 vm 字符串池 intern（data 全平凡：直接存池内指针） */
    const char *sv = vm_str_intern_len(vm, s.ptr, s.len);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_str, &sv);
    return value_make(vm, vm->type_str, data);
}

/* ---- 栈操作 ---- */

value_t *op_push_value(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t offset = bcode_read_u32(bc, pc);
    return exec_stack_peek(vm, offset); /* offset=0 即 dup 栈顶 */
}

value_t *op_pop(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    exec_stack_pop(vm); /* 丢弃借用引用，不释放 */
    return NULL;
}

/* ---- 类型与变量定义 ---- */

value_t *op_load(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *v = scope_lookup(vm->global_scope, name);
    if (!v) return value_make_error(vm, "exec: undefined type");
    return v; /* type value 借用引用 */
}

/* ---- 类型 id 指令（LOAD_TYPE / SEAL / SET_TYPE_NAME） ---- */

/* LOAD_TYPE <id>：从 types_by_id 查表，压入该类型的 type value */
value_t *op_load_type(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    const type_t *t = vm_type_load(vm, id);
    if (!t) return value_make_error(vm, "exec: unknown type id");
    return type_as_value(vm, t);
}

/* LOAD_TYPE_OWNED <u8 own>：弹栈顶**裸类型** type value，套上所有权修饰后压回。
   槽位类型用它保留所有权（运行期销毁/克隆按槽位类型判定，不再读镜像字段）。 */
value_t *op_load_type_owned(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint8_t own = bcode_read_u8(bc, pc);
    value_t *tv = exec_stack_pop(vm);
    if (!tv || value_type(tv) != vm->type_type)
        return value_make_error(vm, "exec: load type owned expects a type value");
    const type_t *sub = value_as(tv, const type_t *);
    type_kind_t kind = own == 1   ? TYPE_KIND_OWN
                       : own == 2 ? TYPE_KIND_REF
                       : own == 3 ? TYPE_KIND_FATAL
                                  : TYPE_KIND_VOID;
    if (kind == TYPE_KIND_VOID || !sub)
        return value_make_error(vm, "exec: invalid ownership modifier");
    const type_t *w = type_ownership_intern(vm, kind, sub);
    if (!w) return value_make_error(vm, "exec: failed to intern ownership type");
    return type_as_value(vm, w);
}

/* LOAD_FUNCTION <id>：从 functions_by_id 查表，压入该函数的 func value。
   对标 LOAD_TYPE：函数值运行期加载（变量/参数/返回值传递）。签名类型取自
   func_t->type（func_new 写入，归 vm 类型池所有），包装为 func value——
   data 存 func_t*，type 即签名类型，与 PUSH_FUNCTION 构造的 func value
   同构。 */
value_t *op_load_function(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_make_function(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    func_t *base = vm_func_load(vm, id);
    if (!base || !base->type)
        return value_make_error(vm, "exec: unknown function id");
    return func_instantiate(vm, base);
}

/* SET_TYPE_NAME <name>：peek 栈顶 type value（不弹栈），设置其显示名
 * （覆盖规范名）。与 SET_FUNC_NAME 同款 peek 语义——允许后续 DEFINE_TYPE
 * 消费同一 type value。 */
value_t *op_set_type_name(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *tv = exec_stack_peek(vm, 0);
    if (!tv || value_type(tv) != vm->type_type)
        return value_make_error(vm, "exec: set type name expects a type value");
    const type_t *t = *(const type_t **)value_data(tv);
    if (!type_set_name(vm, t, name))
        return value_make_error(vm, "exec: cannot rename this type");
    return NULL;
}

value_t *op_push_undefined(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    return value_make_undefined(vm);
}

value_t *op_define(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
