#include "vm/type_ptr.h"
#include "vm/type_ownership.h"
#include "vm/bcode_function.h"
#include "vm/type.h"
#include "vm/type_opaque.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_array.h"
#include "vm/type_slice.h"
#include "vm/type_union.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"

#include <string.h>
#include <stdalign.h>

/* ---- 前向声明：slice_free_elems / slice_clear_elems 在 type_slice.c 中
   定义，切片元素级递归销毁/清空由下方 ptr_free_owned_recursive /
   ptr_clear_owned_recursive 的 SLICE_OWN 分支调用。 ---- */
void slice_free_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len);
void slice_clear_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len);

/* ===========================================================================
 * 指针类型（*T，m3-design §3）
 *
 * ptr_type_t 继承 type_t 持 base_type 指针。指针值 data 块布局 =
 * ptr_value_t { void *ptr; }（见 type_ptr.h）。
 *
 * 所有权纯编译期——类型不区分 own/ref/fatal，只有一种指针类型 *T。
 * 运行期零标志：clone/dispose/cast 不按所有权分派，堆释放由 sema 生成
 * FREE 指令负责。
 *
 * 生命周期（运行期简化）：
 *   - clone：复制指针值（浅拷贝——堆块所有权由编译期 ownership 语义控制，
 *     运行期不做深拷贝/转移分派）
 *   - dispose：no-op（堆释放由 FREE 指令负责）
 *   - eq/ne：比较 ptr 字段
 *   - implicit_cast：身份拷贝 / 指针向上转换 / → opaque 隐式
 *   - explicit_cast：opaque → 任意指针（as，§8.4）
 *   - type_equal / type_extends：按 base 递归
 * =========================================================================== */

/* ===========================================================================
 * 递归销毁（m3-design §3.1/§13）：own 销毁沿字段递归。
 *
 * 纯静态分派（type + 裸 data 块，不依赖 value_t 包装）——供 scope_destroy
 * 在 value_dispose 前扫描 owned 值调用（销毁变量前先销毁变量内部所有 own
 * 指向的堆内存，含结构体字段内嵌的 own 指针）。
 *
 * 双释放防线：ref 总借用跳过；own/fatal ptr=NULL（已转移）跳过；递归
 * 销毁后堆块随即释放并置 ptr=NULL，不重复扫描（每个 ptr_value_t 只被
 * 消费一次）。
 * =========================================================================== */

/* 目标是否含需递归销毁的 own 指针（指针 + 切片，**递归含复合类型字段**）。
   所有权纯编译期——运行期不再按 ownership 区分，只要类型（递归）是指针/切片
   就可能有堆块需要释放（调用方——FREE/DISPOSE——保证只对需要的值调用）。

   **cunion 不参与**：其所有成员共享同一块内存且无 tag，无法静态判断哪个成员
   是活跃的——按成员递归销毁会误释放/双释放。因此 cunion 内的指针归开发者
   自负（与 cunion 的 dispose 恒为 no-op 一致）。 */
bool ptr_type_needs_scan(const type_t *t) {
    if (type_has_rc_slots(t)) return true;
    if (!t) return false;
    if (type_is_ptr_or_slice(t)) return true;
    switch (t->kind) {
    case TYPE_KIND_STRUCT: {
        const struct_type_t *st = (const struct_type_t *)t;
        for (size_t i = 0; i < st->field_count; i++)
            if (ptr_type_needs_scan(st->fields[i].type)) return true;
        return false;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        for (size_t i = 0; i < tt->elem_count; i++)
            if (ptr_type_needs_scan(tt->elems[i].type)) return true;
        return false;
    }
    case TYPE_KIND_UNION: {
        const union_type_t *ut = (const union_type_t *)t;
        for (size_t i = 0; i < ut->member_count; i++)
            if (ptr_type_needs_scan(ut->members[i].payload_type)) return true;
        return false;
    }
    case TYPE_KIND_OPTION:
        return ptr_type_needs_scan(((const option_type_t *)t)->inner);
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
        return ptr_type_needs_scan(type_qualifier_sub(t));
    case TYPE_KIND_ARRAY: {
        const array_type_t *at = (const array_type_t *)t;
        return ptr_type_needs_scan(at->elem_type);
    }
    default:
        /* cunion：见上（无 tag，无法判定活跃成员）；标量/字符串/opaque 等 */
        return false;
    }
}

/* 只释放**这一个**指针/切片的堆块，**不递归**被指向值/元素。
   供 `FREE` 指令使用——A 方案（编译期 AST 生成销毁序列）下，被指向值的
   own 槽位由 sema 生成的 `FREE v.f` 逐条释放，运行期不再按类型递归扫描。

   这是一处**语义收紧**：旧 `ptr_free_owned_recursive` 会沿被指向类型无差别
   递归所有指针字段（含 ref 槽位），一旦聚合能携带 ref 指针就会把借用目标的
   栈地址也 free 掉（实测 0xC0000005）。改为非递归后，销毁序列**只**含 sema
   判定为 own 的槽位，该错误没有产生路径。 */
void ptr_free_block_only(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    /* 槽位类型在运行期保留所有权修饰（LOAD_TYPE_OWNED）；结构判定看裸类型。 */
    t = type_strip_quals(t);
    switch (t->kind) {
    case TYPE_KIND_PTR: {
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        void *heap = pv->ptr;
        pv->ptr = NULL;   /* 置空防双释放 */
        allocator_free(vm->alloc, &heap);
        return;
    }
    case TYPE_KIND_SLICE: {
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        void *heap = sv->ptr;
        sv->ptr = NULL;
        sv->len = 0;
        allocator_free(vm->alloc, &heap);
        return;
    }
    default:
        return;
    }
}

/* 槽位判定（与 sema 的 gen_destroy 同一规则，type 对象是唯一权威）：
   - 叶子（ptr / slice）：只有槽位 ownership == OWN 才归本值所有 → 释放/递归；
   - 复合（struct/tuple/union/option/array/…）：值本身没有独立分配，其内部
     own 槽位在结构上属于外层值 → **无条件递归**（由内层按自己的槽位再判）。
   这条区分是"运行期不再无差别释放 ref"的关键——旧实现只看
   ptr_type_needs_scan（只问"有没有指针"），于是会把 ref 借用目标也 free 掉
   （实测：含 ref member 的 own union 重新赋值 → 0xC0000005）。 */
static bool slot_owns_value(const type_t *ft, ownership_t own) {
    if (!ft) return false;
    const type_t *s = type_strip_quals(ft);
    if (!s) return false;
    if (s->kind == TYPE_KIND_PTR || s->kind == TYPE_KIND_SLICE)
        return own == OWN_OWN;
    return true; /* 复合槽位：递归进去，由内层槽位决定释放什么 */
}

/* 叶子槽位在非 own 时**不**递归；复合槽位只在内部还有 own 槽位时下钻。 */
static bool slot_needs_visit(const type_t *ft, ownership_t own) {
    const type_t *s0 = type_strip_quals(ft);
    /* share/weak 是真实类型：无论槽位标注如何都持有一份 RC 引用 → 要访问。 */
    if (type_is_rc_ptr(s0)) return true;
    /* **闭包槽位**（FUNC）：只有 **own** 槽位才释放（own 闭包字段 → 释放其
       env 的 own 捕获）；ref/none 槽位一律不碰。
       注意：不能走下面"复合槽位无条件递归"的规则——闭包可以**捕获自身**
       （`func |fib| fib(...)`），共享同一实例，无条件递归会沿自引用无限下降
       （实测栈溢出）。 */
    if (s0 && s0->kind == TYPE_KIND_FUNC) return own == OWN_OWN;
    if (!slot_owns_value(ft, own)) return false;
    const type_t *s = s0;
    if (s && (s->kind == TYPE_KIND_PTR || s->kind == TYPE_KIND_SLICE))
        return true; /* own 叶子 → 释放 */
    return type_has_own_slots(ft); /* 复合：内部无 own 槽位就不必下钻 */
}

void ptr_free_slot_value(vm_t *vm, const type_t *slot_type, ownership_t own,
                         void *data) {
    (void)own;
    if (!vm || !slot_type || !data) return;
    /* 所有权以槽位类型为准（运行期槽位类型带修饰）。 */
    ownership_t slot_own = type_ownership_of(slot_type);
    if (!slot_needs_visit(slot_type, slot_own)) return;
    slot_type = type_strip_quals(slot_type);
    if (!slot_type) return;
    ptr_free_owned_recursive(vm, slot_type, data);
}

void ptr_free_owned_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    /* 槽位类型可能带所有权修饰（LOAD_TYPE_OWNED）；递归销毁看裸类型。 */
    t = type_strip_quals(t);
    if (!t) return;
    switch (t->kind) {
    case TYPE_KIND_FUNC: {
        /* **闭包 = 捕获聚合结构体 + vtable**：优先调用生成函数
           `__destroy_<env>(dv: ref *Env)` 释放 env 的 own 捕获，再释放 env 块；
           没有 vtable（老/退化路径）才按 capture_type 内省。用于结构体的 own
           闭包字段在运行期递归销毁（赋值点/丢弃/复合赋值）路径。 */
        func_t **fp = (func_t **)data;
        func_t *fn = fp ? *fp : NULL;
        if (!fn) return;
        if (fn->env_dispose_fid != 0 && fn->env && fn->capture_type) {
            func_t *df = vm_func_load(vm, fn->env_dispose_fid);
            const type_t *pt =
                type_ptr_intern(vm, (const type_t *)fn->capture_type);
            if (df && pt) {
                void *pdata = value_alloc_data_copy(vm->alloc, pt, &fn->env);
                value_t *pv = pdata ? value_make(vm, pt, pdata) : NULL;
                value_t *fv = func_value_ref(vm, df);
                if (pv && fv) {
                    value_t *args[1] = {pv};
                    (void)value_call(vm, fv, args, 1);
                }
            }
            allocator_free(vm->alloc, &fn->env);
            return;
        }
        if (!fn->capture_type || !fn->env) return;
        const struct_type_t *ct = fn->capture_type;
        for (size_t i = 0; i < ct->field_count; i++) {
            const struct_field_t *f = &ct->fields[i];
            if (!f->type) continue;
            ptr_free_slot_value(vm, f->type, type_ownership_of(f->type),
                                (uint8_t *)fn->env + f->offset);
        }
        allocator_free(vm->alloc, &fn->env);
        return;
    }
    case TYPE_KIND_PTR: {
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        /* share/weak：递减计数（计数归零才析构 payload），不深入、不释放。 */
        if (type_is_rc_ptr(t)) {
            const type_t *base = ptr_type_base(t);
            void *payload = pv->ptr;
            pv->ptr = NULL;
            if (base) {
                if (type_is_share(t))
                    rc_block_strong_dec(vm, rc_block_of(base, payload));
                else
                    rc_block_weak_dec(vm, rc_block_of(base, payload));
            }
            return;
        }
        /* 指针：ptr≠NULL → 先递归销毁堆块内容（T 含 own 字段时内嵌堆块
           一并释放），再释放本堆块，置 ptr=NULL 防双释放。
           所有权纯编译期——调用方（FREE 指令）保证只对 own 变量调用。 */
        const type_t *base = ptr_type_base(t);
        if (base && ptr_type_needs_scan(base))
            ptr_free_owned_recursive(vm, base, pv->ptr);
        void *heap = pv->ptr;
        pv->ptr = NULL;
        allocator_free(vm->alloc, &heap);
        return;
    }
    case TYPE_KIND_SLICE: {
        /* []T：拥有堆块 → 先按**元素槽位所有权**销毁元素内嵌 own 槽位，
           再释放堆块，置 ptr=NULL 防双释放。 */
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        const type_t *et = slice_type_elem(t);
        if (et && slot_needs_visit(et, slice_type_elem_ownership(t))) {
            uint8_t *p = (uint8_t *)sv->ptr;
            for (uint64_t i = 0; i < sv->len; i++, p += et->size)
                ptr_free_owned_recursive(vm, et, p);
        }
        void *heap = sv->ptr;
        sv->ptr = NULL;
        sv->len = 0;
        allocator_free(vm->alloc, &heap);
        return;
    }
    case TYPE_KIND_STRUCT: {
        /* 结构体：按字段偏移逐个递归销毁**own 槽位**（ref/fatal 槽位跳过）。 */
        const struct_type_t *st = (const struct_type_t *)t;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < st->field_count; i++) {
            const struct_field_t *f = &st->fields[i];
            if (f->type && slot_needs_visit(f->type, type_ownership_of(f->type)))
                ptr_free_owned_recursive(vm, f->type, p + f->offset);
        }
        return;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < tt->elem_count; i++) {
            const tuple_elem_t *e = &tt->elems[i];
            if (e->type && slot_needs_visit(e->type, type_ownership_of(e->type)))
                ptr_free_owned_recursive(vm, e->type, p + e->offset);
        }
        return;
    }
    case TYPE_KIND_UNION: {
        /* tag union：只有**当前 tag 对应的 payload** 是活的，只销毁它。 */
        const union_type_t *ut = (const union_type_t *)t;
        if (ut->member_count == 0) return;
        uint64_t tag = union_read_tag_raw(data, union_type_tag_size(t));
        if (tag >= ut->member_count) return;   /* 非法 tag：防御性跳过 */
        const union_member_t *m = &ut->members[tag];
        if (m->payload_type && slot_needs_visit(m->payload_type,
                                 type_ownership_of(m->payload_type)))
            ptr_free_owned_recursive(vm, m->payload_type,
                                     (uint8_t *)data + union_type_payload_offset(t));
        return;
    }
    case TYPE_KIND_OPTION: {
        /* ?T：仅 ok 时 value 字段是活的；inner 槽位的所有权决定释放/跳过。 */
        if (!option_ok(data)) return;
        const type_t *inner = type_option_inner(t);
        if (inner &&
            slot_needs_visit(inner, type_option_inner_ownership(t)))
            ptr_free_owned_recursive(
                vm, inner,
                (uint8_t *)data + option_value_offset(t));
        return;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE: {
        const type_t *sub = type_qualifier_sub(t);
        if (sub && ptr_type_needs_scan(sub))
            ptr_free_owned_recursive(vm, sub, data);
        return;
    }
    case TYPE_KIND_ARRAY: {
        /* [N]T：按**元素槽位所有权**逐元素释放（own 叶子释放、ref 跳过；
           复合元素无条件递归）。 */
        const array_type_t *at = (const array_type_t *)t;
        const type_t *et = at->elem_type;
        if (!et || !slot_needs_visit(et, type_ownership_of(et))) return;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < at->length; i++, p += et->size)
            ptr_free_owned_recursive(vm, et, p);
        return;
    }
    default:
        /* 标量/字符串/cunion/opaque/func 等：无 own 堆块可销毁
           （cunion 无 tag，无法判定活跃成员——开发者自负）。 */
        return;
    }
}

/* 所有权转移清空（接管辅助）：递归把 data 块内所有指针置 ptr=NULL
   （**不释放**——指针值已随 memcpy 复制进接管方堆块，所有权随拷贝转移）。
   供 op_new/op_construct 在成员值 memcpy 进目标块后调用，清空源对象图内
   所有指针，防作用域退出时与接管方双释放。 */
/* 把数据块里的**每一个 share/weak 槽位**的计数 +1（"复制进新槽位"的语义）。
   用于构造/字段写入/闭包捕获/数组元素等 memcpy 之后：RC 是真实类型，拷贝
   就是引用 +1。own 指针不在此列（那是转移，源会被清空）。 */
void ptr_rc_inc_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    const type_t *s = type_strip_quals(t);
    if (!s) return;
    switch (s->kind) {
    case TYPE_KIND_PTR: {
        if (!type_is_rc_ptr(s)) return;
        ptr_value_t *pv = (ptr_value_t *)data;
        const type_t *base = ptr_type_base(s);
        if (!pv->ptr || !base) return;
        if (type_is_share(s))
            rc_block_strong_inc(rc_block_of(base, pv->ptr));
        else
            rc_block_weak_inc(rc_block_of(base, pv->ptr));
        return;
    }
    case TYPE_KIND_STRUCT: {
        const struct_type_t *st = (const struct_type_t *)s;
        for (size_t i = 0; i < st->field_count; i++)
            ptr_rc_inc_recursive(vm, st->fields[i].type,
                                 (uint8_t *)data + st->fields[i].offset);
        return;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)s;
        for (size_t i = 0; i < tt->elem_count; i++)
            ptr_rc_inc_recursive(vm, tt->elems[i].type,
                                 (uint8_t *)data + tt->elems[i].offset);
        return;
    }
    case TYPE_KIND_ARRAY: {
        const type_t *et = array_type_elem(s);
        size_t n = array_type_len(s);
        if (!et || n == SIZE_MAX) return;
        for (size_t i = 0; i < n; i++)
            ptr_rc_inc_recursive(vm, et, (uint8_t *)data + i * et->size);
        return;
    }
    case TYPE_KIND_SLICE: {
        const type_t *et = slice_type_elem(s);
        const slice_value_t *sv = (const slice_value_t *)data;
        if (!et || !sv->ptr || !sv->len) return;
        for (size_t i = 0; i < sv->len; i++)
            ptr_rc_inc_recursive(vm, et, (uint8_t *)sv->ptr + i * et->size);
        return;
    }
    case TYPE_KIND_OPTION: {
        const type_t *inner = type_option_inner(s);
        if (!inner) return;
        if (!*(const bool *)data) return;   /* none：无值 */
        ptr_rc_inc_recursive(vm, inner,
                             (uint8_t *)data + option_value_offset(s));
        return;
    }
    case TYPE_KIND_UNION: {
        /* tag union：只有当前 tag 对应的 payload 是活的 */
        const union_type_t *ut = (const union_type_t *)s;
        if (ut->member_count == 0) return;
        uint64_t tag = union_read_tag_raw(data, union_type_tag_size(s));
        if (tag >= ut->member_count) return;
        const union_member_t *m = &ut->members[tag];
        if (m->payload_type)
            ptr_rc_inc_recursive(vm, m->payload_type,
                                 (uint8_t *)data + union_type_payload_offset(s));
        return;
    }
    default:
        return;
    }
}

void ptr_clear_owned_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    switch (t->kind) {
    case TYPE_KIND_PTR: {
        /* **只清指针，绝不进被指向值**（2026-10 修正）。
           所有权随 memcpy 转移：堆块现在归接管方；被指向值内部的 own 指针
           也归**该堆块**（接管方持有），与源无关。旧实现递归进 pointee 清
           内层指针，会**改写共享堆块**——`new Mid{ .i = move(inner) }` 之后
           `mid.i.p` 变成 NULL（实测：三层链构造后读 p 报 null deref）。
           源侧防双释放只需要把指针置 NULL：源不再引用该块，销毁时跳过。 */
        ptr_value_t *pv = (ptr_value_t *)data;
        if (type_is_rc_ptr(t) && pv->ptr) {
            /* share/weak：清空 = 递减计数 + 置 NULL（源不再持有这份引用）。 */
            const type_t *base = ptr_type_base(t);
            if (base) {
                if (type_is_share(t))
                    rc_block_strong_dec(vm, rc_block_of(base, pv->ptr));
                else
                    rc_block_weak_dec(vm, rc_block_of(base, pv->ptr));
            }
        }
        pv->ptr = NULL;
        return;
    }
    case TYPE_KIND_SLICE: {
        /* []T：堆块已随 memcpy 复制进接管方，清源 ptr=NULL 防作用域
           退出时与接管方双释放。递归清空元素内嵌指针（堆块归接管方，
           不释放）。 */
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        const type_t *et = slice_type_elem(t);
        if (et) slice_clear_elems(vm, et, sv->ptr, sv->len);
        sv->ptr = NULL;
        sv->len = 0;
        return;
    }
    case TYPE_KIND_STRUCT: {
        const struct_type_t *st = (const struct_type_t *)t;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < st->field_count; i++) {
            const struct_field_t *f = &st->fields[i];
            if (f->type && ptr_type_needs_scan(f->type))
                ptr_clear_owned_recursive(vm, f->type, p + f->offset);
        }
        return;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < tt->elem_count; i++) {
            const tuple_elem_t *e = &tt->elems[i];
            if (e->type && ptr_type_needs_scan(e->type))
                ptr_clear_owned_recursive(vm, e->type, p + e->offset);
        }
        return;
    }
    case TYPE_KIND_UNION: {
        const union_type_t *ut = (const union_type_t *)t;
        if (ut->member_count == 0) return;
        uint64_t tag = union_read_tag_raw(data, union_type_tag_size(t));
        if (tag >= ut->member_count) return;
        const type_t *pt = ut->members[tag].payload_type;
        if (pt && ptr_type_needs_scan(pt))
            ptr_clear_owned_recursive(vm, pt,
                                      (uint8_t *)data + union_type_payload_offset(t));
        return;
    }
    case TYPE_KIND_OPTION: {
        if (!option_ok(data)) return;
        const type_t *inner = type_option_inner(t);
        if (inner && ptr_type_needs_scan(inner))
            ptr_clear_owned_recursive(vm, inner,
                                      (uint8_t *)data + option_value_offset(t));
        return;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE: {
        const type_t *sub = type_qualifier_sub(t);
        if (sub && ptr_type_needs_scan(sub))
            ptr_clear_owned_recursive(vm, sub, data);
        return;
    }
    case TYPE_KIND_ARRAY: {
        const array_type_t *at = (const array_type_t *)t;
        const type_t *et = at->elem_type;
        if (!et || !ptr_type_needs_scan(et)) return;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < at->length; i++, p += et->size)
            ptr_clear_owned_recursive(vm, et, p);
        return;
    }
    default:
        /* 标量/字符串/cunion/opaque/func 等：无 own 指针可清 */
        return;
    }
}

/* ---- 生命周期：clone / assign / dispose ---- */

/* 深拷贝 type 类型的数据块（对象图克隆）：
   所有权纯编译期——运行期 clone 不按 ownership 分派。
   - 指针且 ptr≠NULL：分配新堆块 + 递归克隆被指向类型，新指针指向独立新堆块
   - ptr=NULL：复制指针值
   - 切片且 ptr≠NULL：分配新堆块 + 逐元素递归克隆
   - 切片 ptr=NULL：胖指针值拷贝
   - struct/tuple/array/option/union：按字段/元素/tag 递归克隆
   - cunion：整块 memcpy
   - 标量/字符串：整块 memcpy
   注意：clone_block 只做深拷贝——是否深拷贝指针由调用方（value_clone /
   CLONE 指令）决定，不在本函数内按 ownership 分派。 */
void *ptr_clone_block(vm_t *vm, const type_t *type, const void *src) {
    /* 槽位类型带所有权修饰：克隆只关心裸类型的布局/分派。 */
    type = type_strip_quals(type);
    if (!type) return NULL;
    void *dst = value_alloc_data(vm->alloc, type);
    switch (type->kind) {
    case TYPE_KIND_FUNC: {
        /* **闭包 = 捕获聚合结构体**（env 块）：clone 与结构体同构——
           新建函数实例（env 块按 capture_type 分配），逐捕获槽按**槽位所有权**
           深/浅拷贝进新 env（own → ptr_clone_block；ref/无资源 → memcpy）。
           cfunc（无 capture_type）走平凡拷贝 = 函数指针值。 */
        const func_t *const *fp = (const func_t *const *)src;
        const func_t *sfn = fp ? *fp : NULL;
        if (!sfn || !sfn->capture_type || !sfn->env) {
            memcpy(dst, src, type->size);
            break;
        }
        func_t *nf = func_instantiate_raw(vm, (func_t *)sfn);
        if (!nf || !nf->env) {
            memcpy(dst, src, type->size);
            break;
        }
        /* **vtable 优先**：调用生成函数 `__clone_<env>(dv: ref *Env): fatal Env`
           深拷贝 env，把结果字节拷进新实例的 env 块。生成函数在定义点就知道
           捕获布局（含切片捕获需要的 prelude），比运行期内省更精确。 */
        if (sfn->env_clone_fid != 0) {
            func_t *cf = vm_func_load(vm, sfn->env_clone_fid);
            const type_t *pt =
                type_ptr_intern(vm, (const type_t *)sfn->capture_type);
            if (cf && pt) {
                const void *src_env = sfn->env;
                void *pdata = value_alloc_data_copy(vm->alloc, pt, &src_env);
                value_t *pv = pdata ? value_make(vm, pt, pdata) : NULL;
                value_t *fv = func_value_ref(vm, cf);
                if (pv && fv) {
                    value_t *args[1] = {pv};
                    value_t *r = value_call(vm, fv, args, 1);
                    if (r && !value_is_error(vm, r) && value_data(r) &&
                        sfn->capture_type->base.size > 0) {
                        memcpy(nf->env, value_data(r),
                               sfn->capture_type->base.size);
                        memcpy(dst, &nf, sizeof(nf));
                        break;
                    }
                }
            }
            /* vtable 调用失败 → 退回按槽位内省克隆（保守路径）。 */
        }
        const struct_type_t *ct = sfn->capture_type;
        for (size_t i = 0; i < ct->field_count; i++) {
            const struct_field_t *f = &ct->fields[i];
            if (!f->type || f->type->size == 0) continue;
            const void *cs = (const uint8_t *)sfn->env + f->offset;
            void *cd = (uint8_t *)nf->env + f->offset;
            if (slot_needs_visit(f->type, type_ownership_of(f->type))) {
                void *nb = ptr_clone_block(vm, f->type, cs);
                memcpy(cd, nb, f->type->size);
                allocator_free(vm->alloc, &nb);
            } else {
                memcpy(cd, cs, f->type->size);
            }
        }
        memcpy(dst, &nf, sizeof(nf));
        break;
    }
    case TYPE_KIND_PTR: {
        const ptr_value_t *sp = (const ptr_value_t *)src;
        /* share/weak：拷贝 = 计数 +1（不深拷贝被指向值——那是共享语义）。 */
        if (type_is_rc_ptr(type)) {
            const type_t *base = ptr_type_base(type);
            if (sp->ptr && base) {
                if (type_is_share(type))
                    rc_block_strong_inc(rc_block_of(base, sp->ptr));
                else
                    rc_block_weak_inc(rc_block_of(base, sp->ptr));
            }
            memcpy(dst, src, type->size);
            break;
        }
        if (sp->ptr) {
            const type_t *base = ptr_type_base(type);
            if (!base) {
                memcpy(dst, src, type->size);
                break;
            }
            void *heap = ptr_clone_block(vm, base, sp->ptr);
            memcpy(dst, &heap, sizeof(heap));
        } else {
            memcpy(dst, src, type->size);
        }
        break;
    }
    case TYPE_KIND_SLICE: {
        const slice_value_t *ss = (const slice_value_t *)src;
        if (!ss->ptr) {
            memcpy(dst, src, type->size);
            break;
        }
        const type_t *et = slice_type_elem(type);
        if (!et || et->size == 0) {
            memcpy(dst, src, type->size);
            break;
        }
        /* 切片**总是**深拷贝缓冲区（own []T 的缓冲区归本值所有，clone 必须
           独立）——浅拷贝会让两个切片共享同一堆块，作用域退出各 FREE 一次
           → 双释放（实测 0xC0000005）。元素内容是否递归深拷贝由**元素槽位
           所有权**决定：own 元素/含 own 槽位的元素要克隆，ref 元素只拷贝
           借用指针（借用目标不归本值所有）。 */
        size_t total = (size_t)ss->len * et->size;
        if (total == 0) {
            memcpy(dst, src, type->size);
            break;
        }
        void *new_heap = allocator_new_ex(vm->alloc, "slice_heap", total,
                                          NULL, NULL, NULL, 1);
        if (!new_heap) panic("vm: out of memory cloning slice block");
        const uint8_t *sp = (const uint8_t *)ss->ptr;
        uint8_t *dp = (uint8_t *)new_heap;
        bool deep = slot_needs_visit(et, slice_type_elem_ownership(type));
        for (uint64_t i = 0; i < ss->len; i++, sp += et->size, dp += et->size) {
            if (!deep) {
                memcpy(dp, sp, et->size);
                continue;
            }
            void *se = ptr_clone_block(vm, et, sp);
            memcpy(dp, se, et->size);
            allocator_free(vm->alloc, (void **)&se);
        }
        slice_value_t new_sv = { new_heap, ss->len };
        memcpy(dst, &new_sv, sizeof(slice_value_t));
        break;
    }
    case TYPE_KIND_STRUCT: {
        /* 结构体：整块 memcpy 后，把**own 槽位**替换成深拷贝结果
           （ref 槽位保持浅拷贝 = 借用目标不变）。 */
        memcpy(dst, src, type->size);
        const struct_type_t *st = (const struct_type_t *)type;
        for (size_t i = 0; i < st->field_count; i++) {
            const struct_field_t *f = &st->fields[i];
            if (!f->type || !slot_needs_visit(f->type, type_ownership_of(f->type))) continue;
            void *sub = ptr_clone_block(vm, f->type,
                                        (const uint8_t *)src + f->offset);
            memcpy((uint8_t *)dst + f->offset, sub, f->type->size);
            allocator_free(vm->alloc, &sub);
        }
        break;
    }
    case TYPE_KIND_TUPLE: {
        memcpy(dst, src, type->size);
        const tuple_type_t *tt = (const tuple_type_t *)type;
        for (size_t i = 0; i < tt->elem_count; i++) {
            const tuple_elem_t *e = &tt->elems[i];
            if (!e->type || !slot_needs_visit(e->type, type_ownership_of(e->type))) continue;
            void *sub = ptr_clone_block(vm, e->type,
                                        (const uint8_t *)src + e->offset);
            memcpy((uint8_t *)dst + e->offset, sub, e->type->size);
            allocator_free(vm->alloc, &sub);
        }
        break;
    }
    case TYPE_KIND_UNION: {
        /* tag union：tag 字节原样拷贝，只深拷贝**当前 tag 的 own payload**。 */
        memcpy(dst, src, type->size);
        const union_type_t *ut = (const union_type_t *)type;
        if (ut->member_count == 0) break;
        uint64_t tag = union_read_tag_raw(src, union_type_tag_size(type));
        if (tag >= ut->member_count) break;   /* 非法 tag：保持 memcpy 结果 */
        const union_member_t *m = &ut->members[tag];
        const type_t *pt = m->payload_type;
        if (!pt || !slot_needs_visit(pt, type_ownership_of(pt))) break;
        size_t poff = union_type_payload_offset(type);
        void *sub = ptr_clone_block(vm, pt, (const uint8_t *)src + poff);
        memcpy((uint8_t *)dst + poff, sub, pt->size);
        allocator_free(vm->alloc, &sub);
        break;
    }
    case TYPE_KIND_OPTION: {
        memcpy(dst, src, type->size);
        if (!option_ok(src)) break;
        const type_t *inner = type_option_inner(type);
        if (!inner ||
            !slot_needs_visit(inner, type_option_inner_ownership(type)))
            break;
        size_t voff = option_value_offset(type);
        void *sub = ptr_clone_block(vm, inner, (const uint8_t *)src + voff);
        memcpy((uint8_t *)dst + voff, sub, inner->size);
        allocator_free(vm->alloc, &sub);
        break;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE: {
        const type_t *sub_t = type_qualifier_sub(type);
        if (sub_t && ptr_type_needs_scan(sub_t)) {
            void *sub = ptr_clone_block(vm, sub_t, src);
            memcpy(dst, sub, sub_t->size);
            allocator_free(vm->alloc, &sub);
        } else {
            memcpy(dst, src, type->size);
        }
        break;
    }
    case TYPE_KIND_ARRAY: {
        const array_type_t *at = (const array_type_t *)type;
        const type_t *et = at->elem_type;
        if (!et || !slot_needs_visit(et, type_ownership_of(et))) {
            memcpy(dst, src, type->size);
            break;
        }
        const uint8_t *sp2 = (const uint8_t *)src;
        uint8_t *dp2 = (uint8_t *)dst;
        for (size_t i = 0; i < at->length; i++, sp2 += et->size, dp2 += et->size) {
            void *se = ptr_clone_block(vm, et, sp2);
            memcpy(dp2, se, et->size);
            allocator_free(vm->alloc, &se);
        }
        break;
    }
    default:
        memcpy(dst, src, type->size);
        break;
    }
    return dst;
}

/* clone：所有权纯编译期——运行期 clone 统一为浅拷贝指针值。
   深拷贝由生成的 clone 函数 / __deep_clone 内置函数调用 ptr_clone_block 完成，
   vtable->clone 只做浅拷贝（指针值复制）。 */
static value_t *ptr_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
    return ptr_make_value(vm, t, pv->ptr);
}

static value_t *ptr_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    /* 所有权纯编译期——assign 不释放旧堆块（由 sema 在赋值点生成 FREE）。 */
    const type_t *dt = value_type(dst);
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

static void ptr_dispose(vm_t *vm, value_t *v) {
    /* 所有权纯编译期——dispose 不释放堆块（由 FREE 指令负责）。 */
    (void)vm; (void)v;
}

/* ---- eq/ne：指针目标地址比较 ---- */

static value_t *ptr_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *ta = value_type(a);
    const type_t *tb = value_type(b);
    if (ta != tb || ptr_type_base(ta) != ptr_type_base(tb)) {
        return value_make_error(vm,
            "ptr: == requires same pointer type");
    }
    const ptr_value_t *pa = (const ptr_value_t *)value_data(a);
    const ptr_value_t *pb = (const ptr_value_t *)value_data(b);
    bool eq = pa->ptr == pb->ptr;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &eq, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

static value_t *ptr_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *ta = value_type(a);
    const type_t *tb = value_type(b);
    if (ta != tb || ptr_type_base(ta) != ptr_type_base(tb)) {
        return value_make_error(vm,
            "ptr: != requires same pointer type");
    }
    const ptr_value_t *pa = (const ptr_value_t *)value_data(a);
    const ptr_value_t *pb = (const ptr_value_t *)value_data(b);
    bool ne = pa->ptr != pb->ptr;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &ne, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换 ---- */

/* 目标是否为 opaque 类型（vtable 是 VTABLE_OPAQUE） */
static bool is_opaque_type(const type_t *t) {
    return t && t->vtable == &VTABLE_OPAQUE;
}

/* target_base 是否等于 src_base、或只是它外层加了 const/volatile（可嵌套）。
 * C 语义：**限定符只能加不能去**——`*T → *const T` 永远安全，
 * 反向（去 const）必须显式 `as`。 */
static bool ptr_upcast_keeps_quals(const type_t *src_base,
                                   const type_t *target_base) {
    if (type_has_const(src_base) && !type_has_const(target_base)) return false;
    return true;
}

static bool ptr_base_adds_quals(const type_t *src_base, const type_t *target_base) {
    if (src_base == target_base) return true;
    const type_t *t = target_base;
    while (t && (t->kind == TYPE_KIND_CONST || t->kind == TYPE_KIND_VOLATILE)) {
        const type_t *sub = (t->kind == TYPE_KIND_CONST)
                                ? ((const const_type_t *)t)->sub
                                : ((const volatile_type_t *)t)->sub;
        if (!sub) break;
        if (sub == src_base) return true;
        t = sub;
    }
    return false;
}

/* 隐式转换的**类型层判据**（不接触 data）：判断 `src → target` 是否允许。
 * 所有权纯编译期——类型不区分 own/ref/fatal，转换判据只看 base 类型兼容性。
 * ownership 降级（own→ref）和接管（fatal→own）不再在此处理——它们是同类型
 * 间的值层操作，sema 在编译期控制。 */
static bool ptr_implicit_cast_ok(vm_t *vm, const type_t *src,
                                 const type_t *target) {
    if (!src || !target) return false;
    if (src == target) return true; /* 同类型身份拷贝 */
    if (is_opaque_type(target)) return true; /* 任意指针 → opaque */

    /* 只给 pointee 加限定：身份拷贝 */
    if (ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target)))
        return true;

    /* 结构体指针向上转换（大 → 小，前缀字段兼容） */
    if (ptr_upcast_keeps_quals(ptr_type_base(src), ptr_type_base(target)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(target)),
            type_strip_quals(ptr_type_base(src))))
        return true;

    /* **RC 系单向弱化**（§12.1）：`share *T → weak *T`（同 base）。
       反向（weak → share）禁止，必须先 `.get()` 升级。 */
    if (type_is_share(src) && type_is_weak(target) &&
        ptr_type_base(src) == ptr_type_base(target))
        return true;

    return false;
}

static value_t *ptr_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    const type_t *src = value_type(v);

    if (value_is_shadow(v)) {
        if (!ptr_implicit_cast_ok(vm, src, target))
            return value_make_error(vm, "ptr: unsupported implicit cast for pointer");
        return value_make_shadow(vm, target);
    }

    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);

    /* **share → weak**（§12.1）：弱计数 +1，指向同一 payload。 */
    if (type_is_share(src) && type_is_weak(target)) {
        const type_t *base = ptr_type_base(src);
        if (pv->ptr && base) rc_block_weak_inc(rc_block_of(base, pv->ptr));
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* 任意指针 → opaque 隐式（m3-design §8.4）：身份拷贝指针目标地址。 */
    if (is_opaque_type(target)) {
        void *data = value_alloc_data_copy(vm->alloc, target, &pv->ptr);
        return value_make(vm, target, data);
    }

    /* 目标只是给 pointee 加了 const/volatile：身份拷贝 */
    if (target && ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target))) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* 结构体指针向上自动转换（大 → 小，前缀字段兼容） */
    if (target &&
        ptr_upcast_keeps_quals(ptr_type_base(src), ptr_type_base(target)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(target)),
            type_strip_quals(ptr_type_base(src)))) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* 同类型身份拷贝 */
    if (src == target) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    return value_make_error(vm, "ptr: unsupported implicit cast for pointer");
}

static value_t *ptr_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);

    /* 同类型身份拷贝 */
    if (value_type(v) == target) {
        const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
        return ptr_make_value(vm, target, pv->ptr);
    }

    return value_make_error(vm, "ptr: unsupported explicit cast for pointer");
}

/* ---- 鸭子类型判断：按 base 递归（所有权纯编译期，类型不区分 ownership） ---- */

static bool ptr_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return true;
    if (!b) return false;
    if (!type_is_ptr(a) || !type_is_ptr(b)) return false;
    return type_equal(vm, ptr_type_base(a), ptr_type_base(b));
}

static bool ptr_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    if (!sup) return false;
    if (!type_is_ptr(sub)) return false;
    /* 结构体指针向上转换：Small 的字段是 Big 的前缀（C 公共起始序列）。 */
    if (ptr_upcast_keeps_quals(ptr_type_base(sub), ptr_type_base(sup)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(sup)),
            type_strip_quals(ptr_type_base(sub))))
        return true;
    return type_extends(vm, ptr_type_base(sub), ptr_type_base(sup));
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

/* ===========================================================================
 * RC 系（share / weak，m3-design §12）
 *
 * 表示：share/weak 的 value data 与普通指针相同（`ptr_value_t { void *ptr }`），
 * 但 ptr 指向 **RC 块内的 payload**（`rc_block_payload`）——于是 `p.*` 语义
 * 不变；控制块 = payload 之前按 align 对齐的 `rc_block_t`。
 *
 * 计数规则：
 *   - 创建（fatal/own → share）     strong=1, weak=1
 *   - copy（clone/assign/传参/返回） strong+1（weak：weak+1）
 *   - drop（dispose）                strong-1（weak：weak-1）
 *   - strong→0：析构 payload（递归释放内嵌 own/share 字段）→ weak-1（归还
 *     strong 自占的弱引用），**不释放内存**（weak 仍观察）
 *   - weak→0：释放控制块 + payload 内存
 * =========================================================================== */

rc_block_t *rc_block_new(vm_t *vm, const type_t *payload_type) {
    if (!vm || !payload_type) return NULL;
    size_t hdr = sizeof(rc_block_t);
    size_t align = payload_type->align ? payload_type->align : 1;
    size_t payload_off = (hdr + align - 1) & ~(align - 1);
    size_t total = payload_off + (payload_type->size ? payload_type->size : 1);
    rc_block_t *rc = (rc_block_t *)allocator_new_ex(
        vm->alloc, "rc_block_t", sizeof(rc_block_t), NULL, NULL, NULL, total);
    if (!rc) return NULL;
    memset(rc, 0, total);
    __atomic_store_n(&rc->strong, 1u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&rc->weak, 1u, __ATOMIC_SEQ_CST);
    rc->alloc = vm->alloc;
    rc->type = payload_type;
    rc->destroyed = false;
    return rc;
}

void rc_block_strong_inc(rc_block_t *rc) {
    if (!rc) return;
    __atomic_fetch_add(&rc->strong, 1u, __ATOMIC_SEQ_CST);
}

void rc_block_weak_inc(rc_block_t *rc) {
    if (!rc) return;
    __atomic_fetch_add(&rc->weak, 1u, __ATOMIC_SEQ_CST);
}

void rc_block_strong_dec(vm_t *vm, rc_block_t *rc) {
    if (!vm || !rc) return;
    unsigned prev = __atomic_fetch_sub(&rc->strong, 1u, __ATOMIC_SEQ_CST);
    if (prev == 1u) {
        /* strong 归零：析构 payload（内嵌 own/share 递归释放），但保留内存 */
        if (!rc->destroyed) {
            rc->destroyed = true;
            void *payload = rc_block_payload(rc);
            if (rc->type && ptr_type_needs_scan(rc->type))
                ptr_free_owned_recursive(vm, rc->type, payload);
        }
        rc_block_weak_dec(vm, rc); /* 归还 strong 自占的弱引用 */
    }
}

void rc_block_weak_dec(vm_t *vm, rc_block_t *rc) {
    if (!vm || !rc) return;
    unsigned prev = __atomic_fetch_sub(&rc->weak, 1u, __ATOMIC_SEQ_CST);
    if (prev == 1u) {
        /* 最后一份弱引用：释放控制块 + payload（同一块内存） */
        allocator_t *alloc = rc->alloc ? rc->alloc : vm->alloc;
        allocator_free(alloc, (void **)&rc);
    }
}

void *rc_block_upgrade(rc_block_t *rc) {
    if (!rc) return NULL;
    unsigned cur = __atomic_load_n(&rc->strong, __ATOMIC_SEQ_CST);
    for (;;) {
        if (cur == 0u) return NULL;
        if (__atomic_compare_exchange_n(&rc->strong, &cur, cur + 1u, 0,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return rc_block_payload(rc);
    }
}

/* share/weak/普通指针的 vtable 前置声明（定义在下方） */
static value_t *ptr_clone(vm_t *vm, value_t *v);
static value_t *ptr_assign(vm_t *vm, value_t *dst, value_t *src);
static void ptr_dispose(vm_t *vm, value_t *v);
static value_t *ptr_implicit_cast(vm_t *vm, value_t *v, const type_t *target);
static value_t *ptr_explicit_cast(vm_t *vm, value_t *v, const type_t *target);
static value_t *ptr_eq(vm_t *vm, value_t *a, value_t *b);
static value_t *ptr_ne(vm_t *vm, value_t *a, value_t *b);

/* ---- share 的 vtable 操作：copy = strong+1，drop = strong-1 ---- */

static value_t *ptr_share_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v)) return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
    if (pv->ptr && ptr_type_base(t))
        rc_block_strong_inc(rc_block_of(ptr_type_base(t), pv->ptr));
    return ptr_make_value(vm, t, pv->ptr);
}

static void ptr_share_dispose(vm_t *vm, value_t *v) {
    const type_t *t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
    if (pv && pv->ptr && t && ptr_type_base(t))
        rc_block_strong_dec(vm, rc_block_of(ptr_type_base(t), pv->ptr));
}

static value_t *ptr_share_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *dt = value_type(dst);
    const type_t *base = ptr_type_base(dt);
    const ptr_value_t *dp = (const ptr_value_t *)value_data(dst);
    const ptr_value_t *sp = (const ptr_value_t *)value_data(src);
    /* inc 新值在前（自赋值安全：同一块先 +1 后 -1 抵消） */
    if (sp->ptr && base) rc_block_strong_inc(rc_block_of(base, sp->ptr));
    if (dp->ptr && base) rc_block_strong_dec(vm, rc_block_of(base, dp->ptr));
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

/* ---- weak 的 vtable 操作：copy = weak+1，drop = weak-1 ---- */

static value_t *ptr_weak_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v)) return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
    if (pv->ptr && ptr_type_base(t))
        rc_block_weak_inc(rc_block_of(ptr_type_base(t), pv->ptr));
    return ptr_make_value(vm, t, pv->ptr);
}

static void ptr_weak_dispose(vm_t *vm, value_t *v) {
    const type_t *t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);
    if (pv && pv->ptr && t && ptr_type_base(t))
        rc_block_weak_dec(vm, rc_block_of(ptr_type_base(t), pv->ptr));
}

static value_t *ptr_weak_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *dt = value_type(dst);
    const type_t *base = ptr_type_base(dt);
    const ptr_value_t *dp = (const ptr_value_t *)value_data(dst);
    const ptr_value_t *sp = (const ptr_value_t *)value_data(src);
    if (sp->ptr && base) rc_block_weak_inc(rc_block_of(base, sp->ptr));
    if (dp->ptr && base) rc_block_weak_dec(vm, rc_block_of(base, dp->ptr));
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

/* ---- 创建通道 ---- */

value_t *ptr_make_share_from_fatal(vm_t *vm, value_t *fatal_val) {
    if (!vm || !fatal_val) return NULL;
    const type_t *t = value_type(fatal_val);
    if (!t || t->kind != TYPE_KIND_PTR) return NULL;
    const type_t *base = ptr_type_base(t);
    if (!base) return NULL;
    const ptr_value_t *pv = (const ptr_value_t *)value_data(fatal_val);
    rc_block_t *rc = rc_block_new(vm, base);
    if (!rc) return NULL;
    if (pv->ptr) {
        /* payload = 被指向值拷贝（含内嵌资源的所有权转移） */
        memcpy(rc_block_payload(rc), pv->ptr, base->size);
        /* 原堆块：内容已转移（ptr_clear 清空其内 own 指针防双释放），释放块 */
        ptr_clear_owned_recursive(vm, base, pv->ptr);
        void *heap = pv->ptr;
        allocator_free(vm->alloc, &heap);
    }
    const type_t *st = type_ptr_intern_rc(vm, base, 1);
    return ptr_make_value(vm, st, rc_block_payload(rc));
}

value_t *ptr_make_weak_from_share(vm_t *vm, value_t *share_val) {
    if (!vm || !share_val) return NULL;
    const type_t *t = value_type(share_val);
    if (!type_is_share(t)) return NULL;
    const type_t *base = ptr_type_base(t);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(share_val);
    if (pv->ptr) rc_block_weak_inc(rc_block_of(base, pv->ptr));
    const type_t *wt = type_ptr_intern_rc(vm, base, 2);
    return ptr_make_value(vm, wt, pv->ptr);
}

const vtable_t VTABLE_PTR = {
    .eq = ptr_eq, .ne = ptr_ne,
    .dispose = ptr_dispose,
    .clone = ptr_clone,
    .assign = ptr_assign,
    .implicit_cast = ptr_implicit_cast,
    .explicit_cast = ptr_explicit_cast,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
    .type_seal = type_ptr_seal,
};

/* share/weak 的 vtable：与普通指针同构，只替换 clone/assign/dispose（计数） */
const vtable_t VTABLE_PTR_SHARE = {
    .eq = ptr_eq, .ne = ptr_ne,
    .dispose = ptr_share_dispose,
    .clone = ptr_share_clone,
    .assign = ptr_share_assign,
    .implicit_cast = ptr_implicit_cast,
    .explicit_cast = ptr_explicit_cast,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
    .type_seal = type_ptr_seal,
};

const vtable_t VTABLE_PTR_WEAK = {
    .eq = ptr_eq, .ne = ptr_ne,
    .dispose = ptr_weak_dispose,
    .clone = ptr_weak_clone,
    .assign = ptr_weak_assign,
    .implicit_cast = ptr_implicit_cast,
    .explicit_cast = ptr_explicit_cast,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
    .type_seal = type_ptr_seal,
};

/* ===========================================================================
 * 开放构造（PUSH_PTR / SET_TYPE / SEAL，与 const/volatile 统一的两遍构造）
 * =========================================================================== */

/* 构造名 "*T"（所有权纯编译期，类型名不携带 ownership 前缀） */
static char *ptr_name(allocator_t *alloc, const type_t *base, uint8_t rc) {
    const char *pre = rc == 1 ? "share " : rc == 2 ? "weak " : "";
    size_t ol = strlen(pre) + 1;
    size_t bl = base && base->name.ptr ? base->name.len : 0;
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 ol + bl + 1);
    if (!buf) return NULL;
    memcpy(buf, pre, strlen(pre));
    buf[strlen(pre)] = '*';
    if (bl) memcpy(buf + ol, base->name.ptr, bl);
    buf[ol + bl] = '\0';
    return buf;
}

/* 分配开放 ptr_type（base_type=NULL，不入池，不压栈）。供字节码路径
 * （type_ptr_push）与 intern 快捷（type_ptr_intern）共用。
 * 所有权纯编译期——只有一种指针类型 *T。 */
static ptr_type_t *ptr_type_create_open(vm_t *vm, uint8_t rc) {
    ptr_type_t *pt = (ptr_type_t *)allocator_new_ex(
        vm->alloc, "ptr_type_t", sizeof(ptr_type_t), NULL, NULL, NULL, 1);
    if (!pt) panic("vm: out of memory allocating pointer type");
    memset(pt, 0, sizeof(ptr_type_t));
    pt->rc = rc;
    pt->base.vtable = rc == 1   ? &VTABLE_PTR_SHARE
                      : rc == 2 ? &VTABLE_PTR_WEAK
                                : &VTABLE_PTR;
    pt->base.kind = TYPE_KIND_PTR;
    /* name/size/align 由 seal 按 base 填充；base_type 由 SET_TYPE 设定；sealed 置 0 */
    return pt;
}

/* PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 base_type → SEAL 密封收尾。
 * 所有权纯编译期——只有一种指针类型 *T。 */
const type_t *type_ptr_push(vm_t *vm, uint8_t rc) {
    if (!vm) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm, rc);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &pt->base));
    return &pt->base; /* 外部只持有 type_t*，不感知 ptr_type_t 子类 */
}

/* SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (!type_is_ptr(t)) return;
    ((ptr_type_t *)t)->base_type = base;
}

/* SEAL：按 base_type 去重 intern（所有权纯编译期——只有一种指针类型 *T，
 * 同一 base 只有一个实例），标记 sealed，返回该 const type（允许链式）。
 * t 须为已设 base_type 的开放 ptr 类型（type_ptr_push 产物）。 */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (!type_is_ptr(t)) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    ptr_type_t *pt = (ptr_type_t *)t;

    /* 必须已设 base_type（SET_TYPE） */
    if (!pt->base_type) return NULL;

    if (!vm->ptr_types) vm->ptr_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 base_type）：命中已有 sealed 类型则复用
     * 并手工回收本开放类型 */
    size_t n = vec_len(vm->ptr_types);
    for (size_t i = 0; i < n; i++) {
        const ptr_type_t *other = (const ptr_type_t *)vec_get(vm->ptr_types, i);
        if (other && other != pt && type_is_sealed(&other->base) &&
            other->base_type == pt->base_type && other->rc == pt->rc) {
            if (pt->base.name.ptr) {
                char *np = (char *)pt->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&pt);
            return &other->base;
        }
    }

    /* 指针值 size/align 恒定（ptr_value_t = 裸指针，对齐指针宽），
       不依赖 base_type 布局 */
    pt->base.size   = sizeof(ptr_value_t);
    pt->base.align  = alignof(ptr_value_t);
    pt->base.sealed = true;

    /* vtable 随 RC 形态选择（copy=+1 / drop=-1 全在 vtable 里，见 §12） */
    pt->base.vtable = pt->rc == 1   ? &VTABLE_PTR_SHARE
                      : pt->rc == 2 ? &VTABLE_PTR_WEAK
                                    : &VTABLE_PTR;
    /* 重建类型名 "share *T" / "weak *T" / "*T" */
    char *name = ptr_name(vm->alloc, pt->base_type, pt->rc);
    if (!name) panic("vm: out of memory allocating pointer type name");
    pt->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->ptr_types, vm->alloc, pt);  /* 密封后入池（去重 intern） */
    return &pt->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_ptr_intern_rc(vm_t *vm, const type_t *base, uint8_t rc) {
    if (!vm || !base) return NULL;
    if (rc == 0) return type_ptr_intern(vm, base);
    ptr_type_t *pt = ptr_type_create_open(vm, rc);
    pt->base_type = base;
    return type_ptr_seal(vm, &pt->base);
}

const type_t *type_ptr_intern(vm_t *vm, const type_t *base) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_ptr_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_ptr_seal 完成。
     * 所有权纯编译期——只有一种指针类型 *T。 */
    if (!vm || !base) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm, 0);
    pt->base_type = base;
    return type_ptr_seal(vm, &pt->base);
}

/* 构造指针值：目标地址（所有权纯编译期，只有一种指针类型 *T）。
 * data 块 = ptr_value_t（值布局，见 type_ptr.h）。 */
value_t *ptr_make_value(vm_t *vm, const type_t *pt, void *target) {
    ptr_value_t pv = { target };
    void *data = value_alloc_data_copy(vm->alloc, pt, &pv);
    return value_make(vm, pt, data);
}
