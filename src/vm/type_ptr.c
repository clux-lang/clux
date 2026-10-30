#include "vm/type_ptr.h"
#include "vm/function.h"
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

/* ---- 前向声明：slice_clear_elems 在 type_slice.c 中定义，切片元素级递归
   清空由 ptr_clear_recursive 的 SLICE 分支调用。 ---- */
void slice_clear_elems(vm_t *vm, const type_t *elem, void *ptr, uint64_t len);

/* ===========================================================================
 * 指针类型（*T）
 *
 * ptr_type_t 继承 type_t 持 base_type 指针。指针值 data 块布局 =
 * ptr_value_t { void *ptr; }（见 type_ptr.h）。
 *
 * 裸指针：编译器不追踪所有权，只有一种指针类型 *T。运行期零标志：
 * clone/dispose/cast 不按所有权分派，堆释放由显式 `free` / 生成的 FREE
 * 语句负责。
 *
 * 生命周期（运行期简化）：
 *   - clone：复制指针值（浅拷贝）
 *   - dispose：no-op（堆释放由 FREE 指令负责）
 *   - eq/ne：比较 ptr 字段
 *   - implicit_cast：身份拷贝 / 指针向上转换 / → opaque 隐式
 *   - explicit_cast：opaque → 任意指针（as，§8.4）
 *   - type_equal / type_extends：按 base 递归
 * =========================================================================== */

/* ===========================================================================
 * 递归销毁（m3-design §3.1/§13）：沿字段递归释放指针/切片堆块。
 *
 * 纯静态分派（type + 裸 data 块，不依赖 value_t 包装）——供 scope_destroy
 * 在 value_dispose 前扫描 owned 值调用。
 *
 * 双释放防线：ptr=NULL（已转移/已释放）跳过；递归销毁后堆块随即释放并置
 * ptr=NULL，不重复扫描（每个 ptr_value_t 只被消费一次）。
 * =========================================================================== */

/* 目标是否（递归）含需扫描的指针/切片槽位。

   **cunion 不参与**：其所有成员共享同一块内存且无 tag，无法静态判断哪个成员
   是活跃的——按成员递归销毁会误释放/双释放。因此 cunion 内的指针归开发者
   自负（与 cunion 的 dispose 恒为 no-op 一致）。 */
bool ptr_type_needs_scan(const type_t *t) {
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

void ptr_free_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    /* 递归销毁看去限定后的裸类型。 */
    t = type_strip_quals(t);
    if (!t) return;
    switch (t->kind) {
    case TYPE_KIND_FUNC: {
        /* **闭包 = 捕获聚合结构体**：释放 env 块。 */
        func_t **fp = (func_t **)data;
        func_t *fn = fp ? *fp : NULL;
        if (!fn || !fn->capture_type || !fn->env) return;
        if (!fn->env_inline) allocator_free(vm->alloc, &fn->env);
        else fn->env = NULL;
        return;
    }
    case TYPE_KIND_PTR: {
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        /* 指针：ptr≠NULL → 先递归销毁堆块内容（T 含指针字段时内嵌堆块
           一并释放），再释放本堆块，置 ptr=NULL 防双释放。
           调用方（FREE 指令）保证只对拥有堆块的变量调用。 */
        const type_t *base = ptr_type_base(t);
        if (base && ptr_type_needs_scan(base))
            ptr_free_recursive(vm, base, pv->ptr);
        void *heap = pv->ptr;
        pv->ptr = NULL;
        allocator_free(vm->alloc, &heap);
        return;
    }
    case TYPE_KIND_SLICE: {
        /* []T：释放堆块，置 ptr=NULL 防双释放。元素内部无独立堆块
           （复合类型禁止携带指针/切片）。 */
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        void *heap = sv->ptr;
        sv->ptr = NULL;
        sv->len = 0;
        allocator_free(vm->alloc, &heap);
        return;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE: {
        const type_t *sub = type_qualifier_sub(t);
        if (sub && ptr_type_needs_scan(sub))
            ptr_free_recursive(vm, sub, data);
        return;
    }
    default:
        /* 标量/字符串/复合类型/cunion/opaque 等：无可释放的独立堆块
           （复合类型禁止携带指针/切片；cunion 无 tag，由开发者自负）。 */
        return;
    }
}

/* 转移清空（接管辅助）：递归把 data 块内所有指针置 ptr=NULL
   （**不释放**——指针值已随 memcpy 复制进接管方堆块）。
   供 op_new/op_construct 在成员值 memcpy 进目标块后调用，清空源对象图内
   所有指针，防作用域退出时与接管方双释放。 */
void ptr_clear_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    switch (t->kind) {
    case TYPE_KIND_PTR: {
        /* **只清指针，绝不进被指向值**（2026-10 修正）。
           堆块现在归接管方；被指向值内部的指针也归**该堆块**（接管方持有），
           与源无关。旧实现递归进 pointee 清内层指针，会**改写共享堆块**——
           `new Mid{ .i = move(inner) }` 之后 `mid.i.p` 变成 NULL（实测：
           三层链构造后读 p 报 null deref）。源侧防双释放只需要把指针置
           NULL：源不再引用该块，销毁时跳过。 */
        ptr_value_t *pv = (ptr_value_t *)data;
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
                ptr_clear_recursive(vm, f->type, p + f->offset);
        }
        return;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < tt->elem_count; i++) {
            const tuple_elem_t *e = &tt->elems[i];
            if (e->type && ptr_type_needs_scan(e->type))
                ptr_clear_recursive(vm, e->type, p + e->offset);
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
            ptr_clear_recursive(vm, pt,
                                      (uint8_t *)data + union_type_payload_offset(t));
        return;
    }
    case TYPE_KIND_OPTION: {
        if (!option_ok(data)) return;
        const type_t *inner = type_option_inner(t);
        if (inner && ptr_type_needs_scan(inner))
            ptr_clear_recursive(vm, inner,
                                      (uint8_t *)data + option_value_offset(t));
        return;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE: {
        const type_t *sub = type_qualifier_sub(t);
        if (sub && ptr_type_needs_scan(sub))
            ptr_clear_recursive(vm, sub, data);
        return;
    }
    case TYPE_KIND_ARRAY: {
        const array_type_t *at = (const array_type_t *)t;
        const type_t *et = at->elem_type;
        if (!et || !ptr_type_needs_scan(et)) return;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < at->length; i++, p += et->size)
            ptr_clear_recursive(vm, et, p);
        return;
    }
    default:
        /* 标量/字符串/cunion/opaque/func 等：无指针可清 */
        return;
    }
}

/* ---- 生命周期：clone / assign / dispose ---- */

/* 深拷贝 type 类型的数据块（对象图克隆）：
   运行期 clone 不按所有权分派。
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
    /* 克隆只关心去限定后的布局/分派。 */
    type = type_strip_quals(type);
    if (!type) return NULL;
    void *dst = value_alloc_data(vm->alloc, type);
    switch (type->kind) {
    case TYPE_KIND_FUNC: {
        /* **闭包 = 捕获聚合结构体**（env 块）：clone 与结构体同构——
           新建函数实例（env 块按 capture_type 分配），逐捕获槽按值拷贝进
           新 env。cfunc（无 capture_type）走平凡拷贝 = 函数指针值。 */
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
        const struct_type_t *ct = sfn->capture_type;
        for (size_t i = 0; i < ct->field_count; i++) {
            const struct_field_t *f = &ct->fields[i];
            if (!f->type || f->type->size == 0) continue;
            const void *cs = (const uint8_t *)sfn->env + f->offset;
            void *cd = (uint8_t *)nf->env + f->offset;
            memcpy(cd, cs, f->type->size);
        }
        memcpy(dst, &nf, sizeof(nf));
        break;
    }
    case TYPE_KIND_PTR: {
        const ptr_value_t *sp = (const ptr_value_t *)src;
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
        /* 切片**总是**深拷贝缓冲区（clone 必须
           独立）——浅拷贝会让两个切片共享同一堆块，作用域退出各 FREE 一次
           → 双释放（实测 0xC0000005）。元素内容是否递归深拷贝由**元素槽位
           元素是否递归深拷贝由元素类型决定：含闭包槽位的元素要克隆，其余只拷贝
           借用指针（借用目标不归本值所有）；含捕获闭包元素按值语义深拷贝。 */
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
        bool deep = type_has_func_slots(et);
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
        /* 结构体：整块 memcpy 后，把**含闭包槽位的字段**替换成深拷贝结果
           （ref 槽位保持浅拷贝 = 借用目标不变）。 */
        memcpy(dst, src, type->size);
        const struct_type_t *st = (const struct_type_t *)type;
        for (size_t i = 0; i < st->field_count; i++) {
            const struct_field_t *f = &st->fields[i];
            if (!f->type ||
                !type_has_func_slots(f->type))
                continue;
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
            if (!e->type ||
                !type_has_func_slots(e->type))
                continue;
            void *sub = ptr_clone_block(vm, e->type,
                                        (const uint8_t *)src + e->offset);
            memcpy((uint8_t *)dst + e->offset, sub, e->type->size);
            allocator_free(vm->alloc, &sub);
        }
        break;
    }
    case TYPE_KIND_UNION: {
        /* tag union：tag 字节原样拷贝，只深拷贝**当前 tag 的 payload**。 */
        memcpy(dst, src, type->size);
        const union_type_t *ut = (const union_type_t *)type;
        if (ut->member_count == 0) break;
        uint64_t tag = union_read_tag_raw(src, union_type_tag_size(type));
        if (tag >= ut->member_count) break;   /* 非法 tag：保持 memcpy 结果 */
        const union_member_t *m = &ut->members[tag];
        const type_t *pt = m->payload_type;
        if (!pt ||
            !type_has_func_slots(pt))
            break;
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
            !type_has_func_slots(inner))
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
        if (!et || !type_has_func_slots(et)) {
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

/* clone：运行期 clone 统一为浅拷贝指针值。
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
    /* assign 不释放旧堆块（由显式 free / 生成的 FREE 负责）。 */
    const type_t *dt = value_type(dst);
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

static void ptr_dispose(vm_t *vm, value_t *v) {
    /* dispose 不释放堆块（由 FREE 指令负责）。 */
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
 * 转换判据只看 base 类型兼容性。
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
    if (!target) return value_make_error(vm, "ptr: cast target is not a type");

    const type_t *src_t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);

    /* 同类型身份拷贝 */
    if (src_t == target) return ptr_make_value(vm, target, pv->ptr);

    /* C 显式转换 `as`：任意指针 → 任意指针（允许改 const/指向类型；
       与 C 的 `(T*)p` 同语义）。opaque 的 data 就是裸指针，可双向转换。 */
    if (type_is_ptr(target) && type_is_ptr(src_t))
        return ptr_make_value(vm, target, pv->ptr);
    if (type_is_ptr(target) && src_t == vm->type_opaque) {
        void *raw = NULL;
        memcpy(&raw, value_data(v), sizeof raw);
        return ptr_make_value(vm, target, raw);
    }
    if (target == vm->type_opaque && type_is_ptr(src_t)) {
        int64_t raw = (int64_t)(intptr_t)pv->ptr;
        return value_make(vm, target,
                          value_alloc_data_copy(vm->alloc, target, &raw));
    }

    return value_make_error(vm, "ptr: unsupported explicit cast for pointer");
}

/* ---- 鸭子类型判断：按 base 递归 ---- */

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

/* ===========================================================================
 * 开放构造（PUSH_PTR / SET_TYPE / SEAL，与 const/volatile 统一的两遍构造）
 * =========================================================================== */

static char *ptr_name(allocator_t *alloc, const type_t *base) {
    size_t bl = base && base->name.ptr ? base->name.len : 0;
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 bl + 2);
    if (!buf) return NULL;
    buf[0] = '*';
    if (bl) memcpy(buf + 1, base->name.ptr, bl);
    buf[bl + 1] = '\0';
    return buf;
}

static ptr_type_t *ptr_type_create_open(vm_t *vm) {
    ptr_type_t *pt = (ptr_type_t *)allocator_new_ex(
        vm->alloc, "ptr_type_t", sizeof(ptr_type_t), NULL, NULL, NULL, 1);
    if (!pt) panic("vm: out of memory allocating pointer type");
    memset(pt, 0, sizeof(ptr_type_t));
    pt->base.vtable = &VTABLE_PTR;
    pt->base.kind = TYPE_KIND_PTR;
    return pt;
}

const type_t *type_ptr_push(vm_t *vm) {
    if (!vm) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &pt->base));
    return &pt->base;
}

void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (!type_is_ptr(t)) return;
    ((ptr_type_t *)t)->base_type = base;
}

const type_t *type_ptr_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (!type_is_ptr(t)) return NULL;
    if (type_is_sealed(t)) return t;
    ptr_type_t *pt = (ptr_type_t *)t;
    if (!pt->base_type) return NULL;
    if (!vm->ptr_types) vm->ptr_types = vec_new(vm->alloc, /*owns_element=*/false);
    size_t n = vec_len(vm->ptr_types);
    for (size_t i = 0; i < n; i++) {
        const ptr_type_t *other = (const ptr_type_t *)vec_get(vm->ptr_types, i);
        if (other && other != pt && type_is_sealed(&other->base) &&
            other->base_type == pt->base_type) {
            if (pt->base.name.ptr) {
                char *np = (char *)pt->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&pt);
            return &other->base;
        }
    }
    pt->base.size   = sizeof(ptr_value_t);
    pt->base.align  = alignof(ptr_value_t);
    pt->base.sealed = true;
    pt->base.vtable = &VTABLE_PTR;
    char *name = ptr_name(vm->alloc, pt->base_type);
    if (!name) panic("vm: out of memory allocating pointer type name");
    pt->base.name = (strslice_t){ name, strlen(name) };
    vec_push(vm->ptr_types, vm->alloc, pt);
    return &pt->base;
}

const type_t *type_ptr_intern(vm_t *vm, const type_t *base) {
    if (!vm || !base) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm);
    pt->base_type = base;
    return type_ptr_seal(vm, &pt->base);
}

/* 构造指针值：目标地址（只有一种指针类型 *T）。
 * data 块 = ptr_value_t（值布局，见 type_ptr.h）。 */
value_t *ptr_make_value(vm_t *vm, const type_t *pt, void *target) {
    ptr_value_t pv = { target };
    void *data = value_alloc_data_copy(vm->alloc, pt, &pv);
    return value_make(vm, pt, data);
}
