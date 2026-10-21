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

value_t *op_push_function(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_bind_func(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_set_func_name(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

/* SET_FUNC_CAPTURES <type_id>：peek 栈顶 func value（不弹栈），登记闭包的
   **捕获聚合类型**（struct，字段 = 捕获槽 + 槽位所有权）。这是"闭包 = 捕获结构体"
   重构的第 1 步：先把 env 的**具名 struct 类型**挂到函数对象上（对 C 后端而言，
   就是 `struct __xxx_closure { callee; struct { ... } env; }` 里那个 env 类型）。
   后续步骤把它用于捕获存储 / clone / free / 调用上下文。 */
value_t *op_set_func_captures(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t id = bcode_read_u32(bc, pc);
    uint32_t dispose_fid = bcode_read_u32(bc, pc);
    uint32_t clone_fid = bcode_read_u32(bc, pc);
    value_t *fv = exec_stack_peek(vm, 0);
    const type_t *t = fv ? value_type(fv) : NULL;
    if (!t || t->kind != TYPE_KIND_FUNC)
        return value_make_error(vm, "exec: set func captures expects a function value");
    const type_t *ct = vm_type_load(vm, id);
    if (!ct || ct->kind != TYPE_KIND_STRUCT)
        return value_make_error(vm, "exec: bad capture type id");
    func_t *fn = *(func_t **)value_data(fv);
    fn->capture_type = (const struct_type_t *)ct;
    fn->env_dispose_fid = dispose_fid;
    fn->env_clone_fid = clone_fid;
    return NULL;
}

/* SET_CLOSURE <name>：弹栈顶捕获值 → clone 进栈下函数对象的 closure_scope
   捕获槽。调用约定（compiler 保证）：定义点先 LOAD_FUNCTION <fid> 压函数
   值，随后每个捕获 [值, SET_CLOSURE "name"]，栈下 1 位即目标函数。闭包为
   clone 值语义（捕获独立副本，非引用）；提升区已用 undefined 占位（scope
   层 define-or-replace：scope_set 替换，void 占位无 assign 槽不走 value_assign）。 */
const char *func_closure_store(vm_t *vm, func_t *fn, strslice_t name,
                               value_t *val) {
    if (!fn || !fn->capture_type || !fn->env)
        return "exec: closure has no captures";
    const type_t *ct = &fn->capture_type->base;
    int fi = struct_type_find_field(ct, name);
    if (fi < 0) return "exec: no such closure capture";
    const struct_field_t *f = struct_type_field(ct, (size_t)fi);
    if (!f || !f->type) return "exec: closure capture type missing";
    value_t *casted = value_implicit_cast(vm, val, f->type);
    if (value_is_error(vm, casted)) return "exec: closure capture type mismatch";
    void *dst = (uint8_t *)fn->env + f->offset;
    ptr_free_slot_value(vm, f->type, type_ownership_of(f->type), dst);
    memcpy(dst, value_data(casted), f->type->size);
    return NULL;
}

value_t *op_set_closure(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *cap = exec_stack_pop(vm);
    value_t *fv = exec_stack_peek(vm, 0);
    const type_t *t = fv ? value_type(fv) : NULL;
    if (!t || t->kind != TYPE_KIND_FUNC)
        return value_make_error(vm, "exec: set closure expects a function value below");
    func_t *fn = *(func_t **)value_data(fv);
    if (!fn->capture_type)
        return value_make_error(vm, "exec: function has no capture env");
    if (!fn->env) {
        /* 基底对象（hoist 注册）惰性分配 env：实例化时已按 capture_type 分配，
           这里只兜底直接对基底写捕获的情况。 */
        void *env = allocator_new_ex(vm->alloc, "closure_env",
                                     fn->capture_type->base.size, NULL, NULL,
                                     NULL, 1);
        if (!env) return value_make_error(vm, "exec: out of memory for capture env");
        memset(env, 0, fn->capture_type->base.size);
        fn->env = env;
    }

    /* 写 env 块的捕获槽（与结构体 FIELD_SET 同构）：按槽位所有权销毁旧值，
       implicit_cast 到字段类型，memcpy 进 env 偏移。 */
    const type_t *ct = &fn->capture_type->base;
    int fi = struct_type_find_field(ct, name);
    if (fi < 0)
        return value_make_error(vm, "exec: no such closure capture");
    const struct_field_t *f = struct_type_field(ct, (size_t)fi);
    if (!f || !f->type)
        return value_make_error(vm, "exec: closure capture type missing");
    value_t *casted = value_implicit_cast(vm, cap, f->type);
    if (value_is_error(vm, casted)) return casted;
    void *dst = (uint8_t *)fn->env + f->offset;
    ptr_free_slot_value(vm, f->type, type_ownership_of(f->type), dst);
    memcpy(dst, value_data(casted), f->type->size);
    return NULL;
}

value_t *op_call(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

value_t *op_ret(vm_t *vm, bytecode_t *bc, size_t *pc) {
    /* 栈顶即返回值；返回 interrupt 哨兵由 bcode_call_cfunc 子循环捕获 */
    (void)bc; (void)pc;
    return value_make_interrupt(vm, INTERRUPT_RETURN);
}

/* ---- 控制流 ---- */

value_t *op_jmp(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)vm;
    uint32_t target = bcode_read_u32(bc, pc);
    *pc = target;
    return NULL;
}

value_t *op_jz(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t target = bcode_read_u32(bc, pc);
    value_t *v = exec_stack_pop(vm);
    if (value_is_error(vm, v)) return v;
    /* 严格 bool：safe_cast 到 bool（不可转换 → error 传播） */
    value_t *cond = value_explicit_cast(vm, v, vm->type_bool);
    if (value_is_error(vm, cond)) return cond;
    if (!*(const bool *)value_data(cond)) *pc = target;
    return NULL;
}

value_t *op_jnz(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

value_t *op_push_scope(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    vm_push_scope(vm);
    return NULL;
}

value_t *op_pop_scope(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    vm_pop_scope(vm);
    return NULL;
}

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
                 "(declare `extern lib %s from \"<name>\";`)", what, alias, alias);
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
value_t *op_extern_load(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_extern_store(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
    case BCODE_FFI_RET_USIZE: return BCODE_FFI_DATA_USIZE;
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

value_t *op_extern_call(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_data(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

value_t *op_import(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_get_member(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

value_t *op_load_attr(vm_t *vm, bytecode_t *bc, size_t *pc) {
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

value_t *op_store_attr(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_member_call(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
