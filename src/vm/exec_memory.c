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
value_t *op_new(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_ptr_get(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_ptr_set(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_addr(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    /* 取地址只看裸类型（槽位类型可能带所有权修饰）。 */
    const type_t *t = v ? type_strip_quals(value_type(v)) : NULL;
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
   （见 value_move / func_vcall 的 fatal 形参分支）。 */

/* MOVE（无操作数）：**所有权转移**（move(x)，m3-design §7）。
 * 所有权纯编译期——move 没有运行期逻辑，只是 sema 把变量的 ownership
 * 从 own 标记为 fatal。运行期 MOVE = 纯 memcpy（位复制），不清空源指针。
 * 堆释放由 sema 生成 FREE 指令负责。
 * shadow → 同类型 shadow。 */
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
value_t *op_dispose(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_free(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (!v || value_is_shadow(v)) return NULL;
    /* 槽位类型带所有权修饰：销毁分派看裸类型。 */
    const type_t *t = type_strip_quals(value_type(v));
    if (!t || !value_data(v)) return NULL;
    /* **闭包 = 捕获聚合结构体**：FREE 一个闭包值 = 按 capture_type 的槽位
       所有权释放 env 里的 own 捕获（与结构体字段销毁同构），随后释放 env 块
       本身。这覆盖"闭包变量不是由字面量初始化"的形态（`var f = clone(g)`
       —— sema 无法静态枚举 g 的捕获）；字面量初始化路径上 sema 已生成的
       `FREE f.<cap>` 会先把捕获槽置 NULL，这里跳过（防双释放）。
       own 闭包是 move-only（唯一所有者），因此在 FREE 处释放 env 是安全的；
       ref/无资源闭包（NONE）不生成 FREE，env 随函数对象统一回收。 */
    if (t->kind == TYPE_KIND_FUNC) {
        func_t **fp = (func_t **)value_data(v);
        func_t *fn = fp ? *fp : NULL;
        /* **vtable 优先**：闭包 env 的 own 捕获由**生成函数**释放
           `__destroy_<env>(dv: ref *Env)`（sema 为每个含捕获的函数生成，fid
           存在函数对象上）。这正是"函数签名不带闭包信息"的解法：生成函数在
           定义点就知道 capture_type，运行期只需间接调用，不必再做类型内省。 */
        if (fn && fn->env_dispose_fid != 0 && fn->env && fn->capture_type) {
            func_t *df = vm_func_load(vm, fn->env_dispose_fid);
            const type_t *pt =
                type_ptr_intern(vm, (const type_t *)fn->capture_type);
            if (df && pt) {
                value_t *pv = NULL;
                void *pdata = value_alloc_data_copy(vm->alloc, pt, &fn->env);
                if (pdata) pv = value_make(vm, pt, pdata);
                value_t *fv = func_value_ref(vm, df);
                if (pv && fv) {
                    value_t *args[1] = {pv};
                    (void)value_call(vm, fv, args, 1);
                }
            }
            allocator_free(vm->alloc, &fn->env);
            return NULL;
        }
        if (fn && fn->capture_type && fn->env) {
            const struct_type_t *ct = fn->capture_type;
            for (size_t i = 0; i < ct->field_count; i++) {
                const struct_field_t *f = &ct->fields[i];
                if (!f->type) continue;
                ptr_free_slot_value(vm, f->type, type_ownership_of(f->type),
                                    (uint8_t *)fn->env + f->offset);
            }
            allocator_free(vm->alloc, &fn->env);
        }
        return NULL;
    }
    /* **share/weak：递减计数**（真实类型，RC 语义在 vtable 的 dispose 里）。
       计数归零才由 RC 块析构 payload；weak 观察者因此永不悬空。 */
    if (type_is_rc_ptr(t)) {
        if (t->vtable && t->vtable->dispose) t->vtable->dispose(vm, v);
        return NULL;
    }
    /* A 方案：sema 已按 type 对象生成"被指向值 own 槽位"的逐条 FREE，
       这里只释放**这一个**堆块，不再按类型递归——否则会把 ref 槽位指向的
       借用目标也 free 掉（实测 0xC0000005）。 */
    ptr_free_block_only(vm, t, value_data(v));
    return NULL;
}

/* TAKE_SLOT：弹**指向槽位的借用值** → 拷贝该值压回独立新值，并把槽位
 * memset 为 0（"真转移"）。cunion 成员 move 专用：cunion 是不安全类型
 * （无 tag、不参与所有权运算），置零后重复 move 变 no-op、不留悬垂指针。 */
value_t *op_take_slot(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *v = exec_stack_pop(vm);
    if (!v) return NULL;
    if (value_is_shadow(v)) return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    if (!t || !value_data(v)) return v;
    void *dst = value_alloc_data(vm->alloc, t);
    memcpy(dst, value_data(v), t->size);
    memset(value_data(v), 0, t->size);   /* 槽位置零：真转移 */
    return value_make(vm, t, dst);
}

/* FREE_RECURSIVE：**自引用类型**的销毁边。
 * 弹指针值 → 按槽位所有权递归释放**被指向值**的 own 槽位（与
 * ptr_free_owned_recursive 同一权威），再释放块本体。
 * 普通类型由 sema 逐槽位静态展开成 FREE 序列（flat，C 后端可直接翻译）；
 * 只有链表/树这类编译期无法穷尽的环才退化为本指令（运行期递归）。 */
value_t *op_free_recursive(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_push_slice(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_slice(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_make(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t init_count = bcode_read_u32(bc, pc);
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
        type_slice_intern(vm, elem);
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
value_t *op_is_tag(vm_t *vm, bytecode_t *bc, size_t *pc) {
    uint32_t tag = bcode_read_u32(bc, pc);
    value_t *v = exec_stack_pop(vm);
    /* 槽位类型可能带所有权修饰：tag 判定看裸类型。 */
    const type_t *t = v ? type_strip_quals(value_type(v)) : NULL;
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
        return value_make_array(vm, et, n > 0 ? elems : NULL, n);
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
        /* 运行期槽位类型保留所有权，而值类型是布局相同的裸类型：按裸类型比较。 */
        if (inner && type_strip_ownership(mt) == type_strip_ownership(inner))
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
            /* 先试隐式转换（字面量 i32 → i64 等）；失败再试**显式转换**——
               `.<type>{<value>}` 显式写出了目标类型，语义等同 C 的复合字面量
               `(T){v}`，因此允许收窄（`.u8{200}`、`.i32{3.7}`）。 */
            value_t *casted = value_implicit_cast(vm, src, t);
            if (value_is_error(vm, casted))
                casted = value_explicit_cast(vm, src, t);
            if (value_is_error(vm, casted)) return casted;
            src = casted;
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
value_t *op_construct(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_index_get(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *index = exec_stack_pop(vm);
    value_t *self  = exec_stack_pop(vm);
    return value_get_index(vm, self, index);
}

/* ---- 下标写入（set_item）：self[index] = val -> self ----
 * 栈布局：..., self, index, val（val 在顶）。弹 val、index、self，分派 vtable->set_index。 */
value_t *op_index_set(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_field_get(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
        /* **闭包 = 捕获聚合结构体**（env 块）：字段访问与结构体完全同构 ——
           `f.env` 给 env 块（capture_type 的借用 struct 值），`f.<cap>` 按
           capture_type 的字段偏移给借用槽位（FREE/读取都就地生效）。 */
        if (value_is_shadow(self))
            return value_make_shadow(vm, t);   /* shadow：无布局可读 */
        const func_t *const *fp = (const func_t *const *)value_data(self);
        const func_t *fn = fp ? *fp : NULL;
        if (!fn || !fn->capture_type || !fn->env)
            return value_make_error(vm, "exec: closure has no captures");
        const type_t *ct = &fn->capture_type->base;
        if (name.len == 3 && memcmp(name.ptr, "env", 3) == 0)
            return value_make_borrowed(vm, ct, fn->env);
        int fi = struct_type_find_field(ct, name);
        if (fi < 0)
            return value_make_error(vm, "exec: no such closure capture");
        const struct_field_t *f = struct_type_field(ct, (size_t)fi);
        if (!f || !f->type)
            return value_make_error(vm, "exec: closure capture type missing");
        return value_make_borrowed(vm, f->type, (uint8_t *)fn->env + f->offset);
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
value_t *op_field_set(vm_t *vm, bytecode_t *bc, size_t *pc) {
    strslice_t name = bcode_read_str(bc, pc);
    value_t *val  = exec_stack_pop(vm);
    value_t *self = exec_stack_pop(vm);
    const type_t *t = self ? value_type(self) : NULL;
    /* const/volatile 视图：剥掉再按底层布局定位字段（写入本身由 sema
       按 const 规则拦截——这里只负责布局）。 */
    t = type_strip_quals(t);
    if (t && t->kind == TYPE_KIND_FUNC) {
        /* 闭包捕获写入（与结构体 FIELD_SET 同构）：按 capture_type 槽位
           所有权先销毁旧值，再 implicit_cast + memcpy 写入 env 块。
           实现与"编译期常量闭包物化"共用（func_closure_store）。 */
        if (value_is_shadow(self)) return self;
        func_t *fn = *(func_t **)value_data(self);
        const char *cerr = func_closure_store(vm, fn, name, val);
        if (cerr) return value_make_error(vm, cerr);
        return self;
    }
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
            ptr_free_slot_value(vm, m->payload_type,
                                     type_ownership_of(m->payload_type), dst);
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
    ptr_free_slot_value(vm, f->type, type_ownership_of(f->type), dst);
    /* data 全平凡：直接覆盖字段（memcpy 可拷贝） */
    memcpy(dst, value_data(casted), f->type->size);
    return self;
}

/* ---- 长度查询（length）：弹 self → value_length(self)（代理到 vtable->length）
 * 当前仅数组实现：返回 u64 元素个数 shadow 下返回 u64 shadow）。 */
value_t *op_length(vm_t *vm, bytecode_t *bc, size_t *pc) {
    (void)bc; (void)pc;
    value_t *self = exec_stack_pop(vm);
    return value_length(vm, self);
}

/* OPT_IS_NONE（无操作数）：nil 判定专用。弹 ?T 值 → 压 bool（ok tag ==
 * false）。x==nil / nil==x → OPT_IS_NONE；x!=nil / nil!=x → OPT_IS_NONE
 * + NOT。tag 比较不进入 vtable eq 分派（nil 非 value，option eq/ne 无
 * 实现）。shadow → 压 bool shadow。 */
value_t *op_opt_is_none(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_push_opt_none(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
value_t *op_unwrap(vm_t *vm, bytecode_t *bc, size_t *pc) {
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
