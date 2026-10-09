#include "vm/type_ptr.h"
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
 * 指针类型（own *T / ref *T / fatal *T，m3-design §3）
 *
 * ptr_type_t 继承 type_t 持 base_type 指针。指针值 data 块布局 =
 * ptr_value_t { void *ptr; }（见 type_ptr.h）——运行期零标志（m3-design
 * §13.2），所有权完全由 type kind 区分：
 *   - own *T：总拥有堆块，dispose 释放（new 产物接管后）
 *   - ref *T：总借用，dispose 跳过（指向栈值 / 他人堆块 / ref 派生）
 *   - fatal *T：ptr≠NULL 时拥有（将亡值），转移后 ptr=NULL（接管方拥有）
 *
 * 三种所有权修饰（own/ref/fatal）是同一结构、独立 intern 实例（kind 区分
 * 所有权，own *i32 != ref *i32）。同一 base_type 的三种指针各自去重。
 *
 * 生命周期（m3-design §7）：
 *   - clone（own）：深拷贝整个对象图——分配新堆块 + memcpy 内容，新指针
 *     指向新堆块。own 唯一所有权 + 递归销毁，浅拷贝必双释放。
 *   - clone（ref / ptr=NULL）：复制指针值（借用，不复制被指物）。
 *   - clone（fatal）：转移——新值接管裸指针，源置 NULL（fatal 是
 *     将亡值，DEFINE/参数接收处接管，不深拷贝，避免 move/clone 双分配）。
 *   - assign：覆盖前释放旧堆块（own/fatal 且 ptr≠NULL）。
 *   - dispose：own/fatal 且 ptr≠NULL → 释放堆块；借用（ref）no-op。
 *   - eq/ne：比较 ptr 字段。
 *   - implicit_cast：own → ref 身份拷贝（借用，只复制指针值）；任意指针 →
 *     opaque 隐式（§8.4）。
 *   - explicit_cast：opaque → 任意指针（as，§8.4）。
 *   - type_equal / type_extends：按 base 递归（三种所有权修饰是独立类型）。
 * =========================================================================== */

/* ---- 内部：释放指针拥有的堆块（own/fatal 且 ptr≠NULL） ---- */

static void ptr_free_heap(vm_t *vm, const value_t *v) {
    if (!v || value_is_shadow(v)) return;
    const type_t *t = value_type(v);
    if (!t) return;
    ptr_value_t *pv = (ptr_value_t *)value_data(v);
    if (!pv || !pv->ptr) return;
    /* own/fatal：释放堆块；share：递减 strong；weak：递减 weak；ref 跳过 */
    switch (t->kind) {
    case TYPE_KIND_PTR_OWN:
    case TYPE_KIND_PTR_FATAL: {
        void *heap = pv->ptr;
        pv->ptr = NULL;
        allocator_free(vm->alloc, &heap);
        break;
    }
    case TYPE_KIND_PTR_SHARE: {
        rc_block_t *rc = (rc_block_t *)pv->ptr;
        pv->ptr = NULL;
        rc_block_strong_dec(vm, rc);
        break;
    }
    case TYPE_KIND_PTR_WEAK: {
        rc_block_t *rc = (rc_block_t *)pv->ptr;
        pv->ptr = NULL;
        rc_block_weak_dec(vm, rc);
        break;
    }
    default:
        break;  /* ref/opaque：无堆块 */
    }
}

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

/* 目标是否含需递归销毁的 own 指针（own 指针 + 含 own 的复合类型）。
   fatal 不在此列：fatal 是将亡值，sema 保证必被 own 接管（R1，§3.3），
   接管后源 ptr=NULL，无堆块可销毁；`_ =` 丢弃场景由 DISPOSE 指令
   特殊处理（递归释放后源同样置 NULL）。 */
bool ptr_type_needs_scan(const type_t *t) {
    if (!t) return false;
    switch (t->kind) {
    case TYPE_KIND_PTR_OWN:
    case TYPE_KIND_PTR_SHARE:
    case TYPE_KIND_PTR_WEAK:
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_TUPLE:
    case TYPE_KIND_ARRAY:
    case TYPE_KIND_OPTION:
    case TYPE_KIND_UNION:
    case TYPE_KIND_SLICE_OWN:
        return true;
    default:
        return false;
    }
}

void ptr_free_owned_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    switch (t->kind) {
    case TYPE_KIND_PTR_OWN: {
        /* own 指针：ptr≠NULL 拥有堆块 → 先递归销毁堆块内容（T 含 own 字段
           时内嵌堆块一并释放），再释放本堆块，置 ptr=NULL 防双释放。
           ref 总借用跳过（由 type kind 区分，不走此分支）。 */
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        const type_t *base = ptr_type_base(t);
        if (base && ptr_type_needs_scan(base))
            ptr_free_owned_recursive(vm, base, pv->ptr);
        void *heap = pv->ptr;
        pv->ptr = NULL;
        allocator_free(vm->alloc, &heap);
        return;
    }
    case TYPE_KIND_PTR_SHARE: {
        /* share 字段析构：递减 strong 计数（strong→0 时 rc_block_strong_dec
           递归析构 payload 内嵌 own/share 字段）。 */
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        rc_block_t *rc = (rc_block_t *)pv->ptr;
        rc_block_strong_dec(vm, rc);
        pv->ptr = NULL;
        return;
    }
    case TYPE_KIND_PTR_WEAK: {
        /* weak 字段析构：递减 weak 计数。 */
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        rc_block_t *rc = (rc_block_t *)pv->ptr;
        rc_block_weak_dec(vm, rc);
        pv->ptr = NULL;
        return;
    }
    case TYPE_KIND_STRUCT: {
        const struct_type_t *st = (const struct_type_t *)t;
        for (size_t i = 0; i < st->field_count; i++) {
            const struct_field_t *f = &st->fields[i];
            if (ptr_type_needs_scan(f->type))
                ptr_free_owned_recursive(vm, f->type,
                                         (uint8_t *)data + f->offset);
        }
        return;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        for (size_t i = 0; i < tt->elem_count; i++) {
            const tuple_elem_t *e = &tt->elems[i];
            if (ptr_type_needs_scan(e->type))
                ptr_free_owned_recursive(vm, e->type,
                                         (uint8_t *)data + e->offset);
        }
        return;
    }
    case TYPE_KIND_ARRAY: {
        const type_t *et = array_type_elem(t);
        size_t len = array_type_len(t);
        if (!et || len == SIZE_MAX || !ptr_type_needs_scan(et)) return;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < len; i++, p += et->size)
            ptr_free_owned_recursive(vm, et, p);
        return;
    }
    case TYPE_KIND_OPTION: {
        const type_t *inner = type_option_inner(t);
        if (!ptr_type_needs_scan(inner)) return;
        if (!option_ok(data)) return;  /* none：无值可销毁 */
        ptr_free_owned_recursive(vm, inner, option_value(data, t));
        return;
    }
    case TYPE_KIND_UNION: {
        /* 按当前激活 member 的 payload 类型递归（tag 决定解释方式） */
        const union_type_t *ut = (const union_type_t *)t;
        uint64_t tag = union_read_tag_raw(data, ut->tag_size);
        if (tag >= ut->member_count) return;  /* 防御：非法 tag 跳过 */
        const union_member_t *m = &ut->members[tag];
        if (ptr_type_needs_scan(m->payload_type))
            ptr_free_owned_recursive(vm, m->payload_type,
                                     (uint8_t *)data + ut->payload_offset);
        return;
    }
    case TYPE_KIND_SLICE_OWN: {
        /* own []T：拥有堆块 → 先递归销毁元素内嵌 own 字段，再释放堆块，
           置 ptr=NULL 防双释放。ref/fatal 切片不在此列（ref 借用；fatal
           将亡值必被 own 接管，接管后 ptr=NULL）。 */
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        const type_t *et = slice_type_elem(t);
        if (et) slice_free_elems(vm, et, sv->ptr, sv->len);
        void *heap = sv->ptr;
        sv->ptr = NULL;
        sv->len = 0;
        allocator_free(vm->alloc, &heap);
        return;
    }
    default:
        /* 标量/字符串/ref *T/cunion/opaque 等：无 own 堆块可销毁 */
        return;
    }
}

/* 所有权转移清空（接管辅助，m3-design §3.3）：递归把 data 块内所有
   own/fatal 指针置 ptr=NULL（**不释放**——指针值已随 memcpy 复制进
   接管方堆块，所有权随拷贝转移）。
   供 op_new/op_construct 在成员值 memcpy 进目标块后调用，清空源对象图内
   所有内嵌 own 指针，防作用域退出时与接管方双释放（与
   ptr_free_owned_recursive 同遍历，但不释放）。 */
void ptr_clear_owned_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    switch (t->kind) {
    case TYPE_KIND_PTR_OWN:
    case TYPE_KIND_PTR_FATAL: {
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        const type_t *base = ptr_type_base(t);
        if (base && ptr_type_needs_scan(base))
            ptr_clear_owned_recursive(vm, base, pv->ptr);
        pv->ptr = NULL;  /* 只清指针：堆块已归接管方，不释放 */
        return;
    }
    case TYPE_KIND_PTR_SHARE:
    case TYPE_KIND_PTR_WEAK: {
        /* share/weak 随 memcpy 复制后，计数已在 clone/assign 时递增，
           清源 ptr=NULL 防作用域退出递减两次。不递减计数——接管方
           持有的 share/weak 已有独立计数。 */
        ptr_value_t *pv = (ptr_value_t *)data;
        if (!pv->ptr) return;
        pv->ptr = NULL;
        return;
    }
    case TYPE_KIND_STRUCT: {
        const struct_type_t *st = (const struct_type_t *)t;
        for (size_t i = 0; i < st->field_count; i++) {
            const struct_field_t *f = &st->fields[i];
            if (ptr_type_needs_scan(f->type))
                ptr_clear_owned_recursive(vm, f->type,
                                          (uint8_t *)data + f->offset);
        }
        return;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)t;
        for (size_t i = 0; i < tt->elem_count; i++) {
            const tuple_elem_t *e = &tt->elems[i];
            if (ptr_type_needs_scan(e->type))
                ptr_clear_owned_recursive(vm, e->type,
                                          (uint8_t *)data + e->offset);
        }
        return;
    }
    case TYPE_KIND_ARRAY: {
        const type_t *et = array_type_elem(t);
        size_t len = array_type_len(t);
        if (!et || len == SIZE_MAX || !ptr_type_needs_scan(et)) return;
        uint8_t *p = (uint8_t *)data;
        for (size_t i = 0; i < len; i++, p += et->size)
            ptr_clear_owned_recursive(vm, et, p);
        return;
    }
    case TYPE_KIND_OPTION: {
        const type_t *inner = type_option_inner(t);
        if (!ptr_type_needs_scan(inner)) return;
        if (!option_ok(data)) return;  /* none：无值可清 */
        ptr_clear_owned_recursive(vm, inner, option_value(data, t));
        return;
    }
    case TYPE_KIND_UNION: {
        const union_type_t *ut = (const union_type_t *)t;
        uint64_t tag = union_read_tag_raw(data, ut->tag_size);
        if (tag >= ut->member_count) return;  /* 防御：非法 tag 跳过 */
        const union_member_t *m = &ut->members[tag];
        if (ptr_type_needs_scan(m->payload_type))
            ptr_clear_owned_recursive(vm, m->payload_type,
                                      (uint8_t *)data + ut->payload_offset);
        return;
    }
    case TYPE_KIND_SLICE_OWN: {
        /* own []T：堆块已随 memcpy 复制进接管方，清源 ptr=NULL 防作用域
           退出时与接管方双释放。递归清空元素内嵌 own 指针（堆块归接管方，
           不释放——同 PTR_OWN 语义）。 */
        slice_value_t *sv = (slice_value_t *)data;
        if (!sv->ptr) return;
        const type_t *et = slice_type_elem(t);
        if (et) slice_clear_elems(vm, et, sv->ptr, sv->len);
        sv->ptr = NULL;
        sv->len = 0;
        return;
    }
    default:
        /* 标量/字符串/ref *T/cunion/opaque 等：无 own 指针可清 */
        return;
    }
}

/* ---- 生命周期：clone / assign / dispose ---- */

/* 深拷贝 type 类型的数据块（对象图克隆，m3-design §7"深拷贝整个对象图"）：
   - own/fatal 指针且 ptr≠NULL：分配新堆块 + 递归克隆被指向类型（内嵌
     own 字段一并深拷贝），新指针指向独立新堆块——两个实例各自独占完整
     对象图，递归销毁互不干扰（无共享指针 = 无双释放）
   - 借用（ref / ptr=NULL）：复制指针值，不深拷贝被指物（§7"深
     拷贝遇到 ref 字段直接 copy"）
   - struct/tuple/array/option/union：按字段/元素/tag 递归克隆
   - cunion：整块 memcpy（无 tag，无法确定激活 member，开发者自负）
   - 标量/字符串：整块 memcpy */
void *ptr_clone_block(vm_t *vm, const type_t *type, const void *src) {
    void *dst = value_alloc_data(vm->alloc, type);
    switch (type->kind) {
    case TYPE_KIND_PTR_OWN:
    case TYPE_KIND_PTR_FATAL: {
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
            memcpy(dst, src, type->size);  /* 借用 / 已转移：复制指针值 */
        }
        break;
    }
    case TYPE_KIND_PTR_SHARE: {
        /* share clone：递增 strong 计数，复制指针值（共享同一 RC 块）。 */
        const ptr_value_t *sp = (const ptr_value_t *)src;
        if (sp->ptr) rc_block_strong_inc((rc_block_t *)sp->ptr);
        memcpy(dst, src, type->size);
        break;
    }
    case TYPE_KIND_PTR_WEAK: {
        /* weak clone：递增 weak 计数，复制指针值。 */
        const ptr_value_t *sp = (const ptr_value_t *)src;
        if (sp->ptr) rc_block_weak_inc((rc_block_t *)sp->ptr);
        memcpy(dst, src, type->size);
        break;
    }
    case TYPE_KIND_STRUCT: {
        const struct_type_t *st = (const struct_type_t *)type;
        for (size_t i = 0; i < st->field_count; i++) {
            const struct_field_t *f = &st->fields[i];
            void *sf = ptr_clone_block(vm, f->type,
                                       (const uint8_t *)src + f->offset);
            memcpy((uint8_t *)dst + f->offset, sf, f->type->size);
            allocator_free(vm->alloc, (void **)&sf);
        }
        break;
    }
    case TYPE_KIND_TUPLE: {
        const tuple_type_t *tt = (const tuple_type_t *)type;
        for (size_t i = 0; i < tt->elem_count; i++) {
            const tuple_elem_t *e = &tt->elems[i];
            void *se = ptr_clone_block(vm, e->type,
                                       (const uint8_t *)src + e->offset);
            memcpy((uint8_t *)dst + e->offset, se, e->type->size);
            allocator_free(vm->alloc, (void **)&se);
        }
        break;
    }
    case TYPE_KIND_ARRAY: {
        const type_t *et = array_type_elem(type);
        size_t len = array_type_len(type);
        if (!et || len == SIZE_MAX) {
            memcpy(dst, src, type->size);
            break;
        }
        const uint8_t *sp = (const uint8_t *)src;
        uint8_t *dp = (uint8_t *)dst;
        for (size_t i = 0; i < len; i++, sp += et->size, dp += et->size) {
            void *se = ptr_clone_block(vm, et, sp);
            memcpy(dp, se, et->size);
            allocator_free(vm->alloc, (void **)&se);
        }
        break;
    }
    case TYPE_KIND_OPTION: {
        memcpy(dst, src, type->size);  /* 先拷贝 ok tag + 原值 */
        const type_t *inner = type_option_inner(type);
        if (inner && option_ok(src)) {
            void *sv = ptr_clone_block(vm, inner, option_value(src, type));
            memcpy(option_value(dst, type), sv, inner->size);
            allocator_free(vm->alloc, (void **)&sv);
        }
        break;
    }
    case TYPE_KIND_UNION: {
        memcpy(dst, src, type->size);  /* 先拷贝 tag + 原 payload */
        const union_type_t *ut = (const union_type_t *)type;
        uint64_t tag = union_read_tag_raw(src, ut->tag_size);
        if (tag < ut->member_count) {
            const union_member_t *m = &ut->members[tag];
            void *sv = ptr_clone_block(vm, m->payload_type,
                                       (const uint8_t *)src + ut->payload_offset);
            memcpy((uint8_t *)dst + ut->payload_offset, sv,
                   m->payload_type->size);
            allocator_free(vm->alloc, (void **)&sv);
        }
        break;
    }
    case TYPE_KIND_SLICE_OWN:
    case TYPE_KIND_SLICE_FATAL: {
        /* own/fatal 切片深拷贝：分配新堆块 + 逐元素递归克隆，新胖指针指向
           独立新堆块（同 PTR_OWN/PTR_FATAL 的深拷贝语义）。ptr=NULL 时
           浅拷贝（已转移/空切片）。 */
        const slice_value_t *ss = (const slice_value_t *)src;
        if (!ss->ptr) {
            memcpy(dst, src, type->size);
            break;
        }
        const type_t *et = slice_type_elem(type);
        if (!et) {
            memcpy(dst, src, type->size);
            break;
        }
        size_t total = (size_t)ss->len * et->size;
        void *new_heap = allocator_new_ex(vm->alloc, "slice_heap", total,
                                          NULL, NULL, NULL, 1);
        if (!new_heap) panic("vm: out of memory cloning slice block");
        const uint8_t *sp = (const uint8_t *)ss->ptr;
        uint8_t *dp = (uint8_t *)new_heap;
        for (uint64_t i = 0; i < ss->len; i++, sp += et->size, dp += et->size) {
            void *se = ptr_clone_block(vm, et, sp);
            memcpy(dp, se, et->size);
            allocator_free(vm->alloc, (void **)&se);
        }
        slice_value_t new_sv = { new_heap, ss->len };
        memcpy(dst, &new_sv, sizeof(slice_value_t));
        break;
    }
    default:
        memcpy(dst, src, type->size);  /* 标量/字符串/ref 切片/cunion/opaque 平凡 */
        break;
    }
    return dst;
}

static value_t *ptr_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);

    switch (t->kind) {
    case TYPE_KIND_PTR_OWN:
        if (pv->ptr) {
            /* own + 拥有堆块：深拷贝整个对象图——递归克隆被指向类型（含
               内嵌 own 字段），新指针指向独立新堆块。两指针各自独占完整
               对象图，各自释放，无双释放。 */
            const type_t *base = ptr_type_base(t);
            if (!base) return value_make_error(vm, "ptr: owner pointer missing base type");
            void *heap = ptr_clone_block(vm, base, pv->ptr);
            return ptr_make_value(vm, t, heap);
        }
        /* own ptr=NULL（已转移）：复制空指针值 */
        return ptr_make_value(vm, t, NULL);
    case TYPE_KIND_PTR_FATAL:
        /* fatal 流转：转移——新值接管裸指针，源置 NULL（fatal 是
           将亡值，接管点在 DEFINE/参数接收处，不深拷贝——避免 move/clone
           产物再深拷贝一次导致双分配）。 */
        {
            value_t *nv = ptr_make_value(vm, t, pv->ptr);
            ((ptr_value_t *)value_data(v))->ptr = NULL;
            return nv;
        }
    case TYPE_KIND_PTR_SHARE:
        /* share clone：递增 strong 计数，共享同一 RC 块（§12.2）。 */
        if (pv->ptr) rc_block_strong_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, t, pv->ptr);
    case TYPE_KIND_PTR_WEAK:
        /* weak clone：递增 weak 计数，共享同一 RC 块（§12.2）。 */
        if (pv->ptr) rc_block_weak_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, t, pv->ptr);
    default:
        /* ref 及一切借用：复制指针值（ref 是值拷贝语义，§3.2） */
        return ptr_make_value(vm, t, pv->ptr);
    }
}

static value_t *ptr_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_type(src) != value_type(dst)) {
        /* 向左值类型 implicit_cast（与 int_assign 对齐：shadow 模式也要
           校验类型兼容性——否则 sema 的 `var r: ref *i32 = 0` 静默通过）。
           fatal → own 接管、own → ref 借用在此协商。 */
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    /* shadow：只检查类型兼容性，不拷贝 data */
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    /* 覆盖前释放旧值（own/fatal 释放堆块；share/weak 递减计数；ref 跳过） */
    ptr_free_heap(vm, dst);
    /* share/weak 赋值：src 计数递增（memcpy 复制指针值前递增，避免
       src==dst 自赋值时先递减后递增导致中间态归零） */
    const type_t *dt = value_type(dst);
    if (dt->kind == TYPE_KIND_PTR_SHARE || dt->kind == TYPE_KIND_PTR_WEAK) {
        ptr_value_t *sp = (ptr_value_t *)value_data(src);
        if (sp->ptr) {
            if (dt->kind == TYPE_KIND_PTR_SHARE)
                rc_block_strong_inc((rc_block_t *)sp->ptr);
            else
                rc_block_weak_inc((rc_block_t *)sp->ptr);
        }
    }
    memcpy(value_data(dst), value_data(src), dt->size);  /* 指针值覆盖 */
    return dst;
}

static void ptr_dispose(vm_t *vm, value_t *v) {
    /* own/fatal 且 ptr≠NULL → 释放堆块（作用域退出销毁，m3-design §3.1）；
       share → 递减 strong（→0 析构 payload）；weak → 递减 weak（→0 释放块）；
       ref 总借用 / ptr=NULL → no-op，防双释放。 */
    ptr_free_heap(vm, v);
}

/* ---- eq/ne：指针目标地址比较 ---- */

static value_t *ptr_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *ta = value_type(a);
    const type_t *tb = value_type(b);
    if (ta->kind != tb->kind || ptr_type_base(ta) != ptr_type_base(tb)) {
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
    if (ta->kind != tb->kind || ptr_type_base(ta) != ptr_type_base(tb)) {
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

static value_t *ptr_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    const type_t *src = value_type(v);
    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);

    /* 任意指针 → opaque 隐式（m3-design §8.4）：身份拷贝指针目标地址。
       opaque 是裸指针，不拥有堆块；堆块仍归源指针释放。 */
    if (is_opaque_type(target)) {
        void *data = value_alloc_data_copy(vm->alloc, target, &pv->ptr);
        return value_make(vm, target, data);
    }

    /* own → ref 身份拷贝（借用的表达，§3.2）：同 base 的 own → ref。
       ref 总借用——不拥有堆块，堆块归原 own 释放。 */
    if (src->kind == TYPE_KIND_PTR_OWN && target &&
        target->kind == TYPE_KIND_PTR_REF &&
        ptr_type_base(src) == ptr_type_base(target)) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* fatal → own 接管（§3.3）：var c: own *i32 = clone(p) / move(p) 的
       DEFINE 路径。fatal 是将亡值——新值接管裸指针（转移语义，
       与 ptr_clone fatal 分支一致），源置 NULL 防双释放。 */
    if (src->kind == TYPE_KIND_PTR_FATAL && target &&
        target->kind == TYPE_KIND_PTR_OWN &&
        ptr_type_base(src) == ptr_type_base(target)) {
        value_t *nv = ptr_make_value(vm, target, pv->ptr);
        ((ptr_value_t *)value_data(v))->ptr = NULL;
        return nv;
    }

    /* fatal → share 创建（§12.1）：var s: share *T = <fatal 值>。
       fatal 接管三路之一：从 fatal 堆块创建 RC 块（payload = 堆块内容拷贝），
       源置 NULL 防双释放。单向隔离：share 不能转回 own/ref。 */
    if (src->kind == TYPE_KIND_PTR_FATAL && target &&
        target->kind == TYPE_KIND_PTR_SHARE &&
        ptr_type_base(src) == ptr_type_base(target)) {
        value_t *sv = ptr_make_share_from_fatal(vm, v);
        return sv;
    }

    /* share → weak 派生（§12.1）：weak 只从 share 派生。递增 weak 计数，
       复制 RC 块指针。share 保持不变（不递减 strong——weak 是额外观察者）。 */
    if (src->kind == TYPE_KIND_PTR_SHARE && target &&
        target->kind == TYPE_KIND_PTR_WEAK &&
        ptr_type_base(src) == ptr_type_base(target)) {
        return ptr_make_weak_from_share(vm, v);
    }

    /* share → share（copy = 计数+1，§12.2） */
    if (src->kind == TYPE_KIND_PTR_SHARE && target &&
        target->kind == TYPE_KIND_PTR_SHARE &&
        ptr_type_base(src) == ptr_type_base(target)) {
        if (pv->ptr) rc_block_strong_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* weak → weak（copy = 计数+1，§12.2） */
    if (src->kind == TYPE_KIND_PTR_WEAK && target &&
        target->kind == TYPE_KIND_PTR_WEAK &&
        ptr_type_base(src) == ptr_type_base(target)) {
        if (pv->ptr) rc_block_weak_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* 同类型身份拷贝（ref → ref 等，clone 语义） */
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

/* ---- 鸭子类型判断：按 base 递归（五种所有权修饰是独立类型） ---- */

static bool ptr_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return true;
    if (!b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind != TYPE_KIND_PTR_OWN && a->kind != TYPE_KIND_PTR_REF &&
        a->kind != TYPE_KIND_PTR_FATAL &&
        a->kind != TYPE_KIND_PTR_SHARE && a->kind != TYPE_KIND_PTR_WEAK) {
        return false;
    }
    return type_equal(vm, ptr_type_base(a), ptr_type_base(b));
}

static bool ptr_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    if (!sup) return false;
    if (sub->kind != TYPE_KIND_PTR_OWN && sub->kind != TYPE_KIND_PTR_REF &&
        sub->kind != TYPE_KIND_PTR_FATAL &&
        sub->kind != TYPE_KIND_PTR_SHARE && sub->kind != TYPE_KIND_PTR_WEAK) {
        return false;
    }
    /* 指针按 base 递归：同所有权修饰下 base 兼容即指针兼容。
       （跨所有权修饰的兼容由 sema 层控制——own → ref 的借用在转换层
       处理，类型值层面的鸭子判断不混用所有权。） */
    if (sub->kind != sup->kind) return false;
    return type_extends(vm, ptr_type_base(sub), ptr_type_base(sup));
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_PTR_OWN = {
    .eq = ptr_eq, .ne = ptr_ne,
    .dispose = ptr_dispose,
    .clone = ptr_clone,
    .assign = ptr_assign,
    .implicit_cast = ptr_implicit_cast,
    .explicit_cast = ptr_explicit_cast,
    .type_equal = ptr_type_equal,
    .type_extends = ptr_type_extends,
    .type_seal = type_ptr_seal, /* 开放构造路径（PUSH_PTR → SEAL）密封入口 */
};

const vtable_t VTABLE_PTR_REF = {
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

const vtable_t VTABLE_PTR_FATAL = {
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

const vtable_t VTABLE_PTR_SHARE = {
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

const vtable_t VTABLE_PTR_WEAK = {
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

/* 构造名 "own *T" / "ref *T" / "fatal *T" / "share *T" / "weak *T" */
static char *ptr_name(allocator_t *alloc, type_kind_t kind, const type_t *base) {
    const char *owner;
    switch (kind) {
    case TYPE_KIND_PTR_OWN:   owner = "own ";   break;
    case TYPE_KIND_PTR_REF:   owner = "ref ";   break;
    case TYPE_KIND_PTR_FATAL: owner = "fatal "; break;
    case TYPE_KIND_PTR_SHARE: owner = "share "; break;
    case TYPE_KIND_PTR_WEAK:  owner = "weak ";  break;
    default:                  owner = "ptr ";   break;
    }
    size_t ol = strlen(owner);
    size_t bl = base && base->name.ptr ? base->name.len : 0;
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 ol + bl + 2);
    if (!buf) return NULL;
    memcpy(buf, owner, ol);
    if (bl) memcpy(buf + ol, base->name.ptr, bl);
    buf[ol + bl] = '\0';
    return buf;
}

/* 分配开放 ptr_type（base_type=NULL，不入池，不压栈）。供字节码路径
 * （type_ptr_push）与 intern 快捷（type_ptr_intern）共用。 */
static ptr_type_t *ptr_type_create_open(vm_t *vm, type_kind_t kind) {
    ptr_type_t *pt = (ptr_type_t *)allocator_new_ex(
        vm->alloc, "ptr_type_t", sizeof(ptr_type_t), NULL, NULL, NULL, 1);
    if (!pt) panic("vm: out of memory allocating pointer type");
    memset(pt, 0, sizeof(ptr_type_t));
    switch (kind) {
    case TYPE_KIND_PTR_OWN:   pt->base.vtable = &VTABLE_PTR_OWN;   break;
    case TYPE_KIND_PTR_REF:   pt->base.vtable = &VTABLE_PTR_REF;   break;
    case TYPE_KIND_PTR_FATAL: pt->base.vtable = &VTABLE_PTR_FATAL; break;
    case TYPE_KIND_PTR_SHARE: pt->base.vtable = &VTABLE_PTR_SHARE; break;
    case TYPE_KIND_PTR_WEAK:  pt->base.vtable = &VTABLE_PTR_WEAK;  break;
    default:
        panic("vm: invalid pointer ownership kind");
        break;
    }
    pt->base.kind = kind;
    /* name/size/align 由 seal 按 base 填充；base_type 由 SET_TYPE 设定；sealed 置 0 */
    return pt;
}

/* PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 base_type → SEAL 密封收尾。
 * kind = 所有权修饰（TYPE_KIND_PTR_OWN/REF/FATAL）。 */
const type_t *type_ptr_push(vm_t *vm, type_kind_t kind) {
    if (!vm) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm, kind);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &pt->base));
    return &pt->base; /* 外部只持有 type_t*，不感知 ptr_type_t 子类 */
}

/* SET_TYPE 运行期用：设被指向类型 T（密封后静默忽略） */
void type_ptr_set_base(vm_t *vm, const type_t *t, const type_t *base) {
    (void)vm;
    if (!t || type_is_sealed(t)) return;
    if (t->kind != TYPE_KIND_PTR_OWN && t->kind != TYPE_KIND_PTR_REF &&
        t->kind != TYPE_KIND_PTR_FATAL &&
        t->kind != TYPE_KIND_PTR_SHARE && t->kind != TYPE_KIND_PTR_WEAK) {
        return;
    }
    ((ptr_type_t *)t)->base_type = base;
}

/* SEAL：按 base_type 去重 intern（同一 base 的五种所有权是五个独立实例），
 * 标记 sealed，返回该 const type（允许链式）。t 须为已设 base_type 的开放
 * ptr 类型（type_ptr_push 产物）。 */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (t->kind != TYPE_KIND_PTR_OWN && t->kind != TYPE_KIND_PTR_REF &&
        t->kind != TYPE_KIND_PTR_FATAL &&
        t->kind != TYPE_KIND_PTR_SHARE && t->kind != TYPE_KIND_PTR_WEAK) {
        return NULL;
    }
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    ptr_type_t *pt = (ptr_type_t *)t;

    /* 必须已设 base_type（SET_TYPE） */
    if (!pt->base_type) return NULL;

    if (!vm->ptr_types) vm->ptr_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 base_type + 所有权 kind）：命中已有 sealed 类型则复用
     * 并手工回收本开放类型 */
    size_t n = vec_len(vm->ptr_types);
    for (size_t i = 0; i < n; i++) {
        const ptr_type_t *other = (const ptr_type_t *)vec_get(vm->ptr_types, i);
        if (other && other != pt && type_is_sealed(&other->base) &&
            other->base.kind == pt->base.kind &&
            other->base_type == pt->base_type) {
            /* 本开放类型与已有 sealed 类型重复：复用 other。
             * 操作数栈中引用本开放类型 pt 的 type value 由 value_seal 负责
             * 重定向到 other（避免悬空）；此处仅手工回收 pt。 */
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

    /* 重建类型名 "own *T" / "ref *T" / "fatal *T" */
    char *name = ptr_name(vm->alloc, pt->base.kind, pt->base_type);
    if (!name) panic("vm: out of memory allocating pointer type name");
    pt->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->ptr_types, vm->alloc, pt);  /* 密封后入池（去重 intern） */
    return &pt->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_ptr_intern(vm_t *vm, type_kind_t kind, const type_t *base) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_ptr_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_ptr_seal 完成。 */
    if (!vm || !base) return NULL;
    if (kind != TYPE_KIND_PTR_OWN && kind != TYPE_KIND_PTR_REF &&
        kind != TYPE_KIND_PTR_FATAL &&
        kind != TYPE_KIND_PTR_SHARE && kind != TYPE_KIND_PTR_WEAK) {
        return NULL;
    }
    ptr_type_t *pt = ptr_type_create_open(vm, kind);
    pt->base_type = base;
    return type_ptr_seal(vm, &pt->base);
}

/* 构造指针值：目标地址（五种指针统一，所有权由 type kind 区分，
 * m3-design §13.2 运行期零标志）。
 * data 块 = ptr_value_t（值布局，见 type_ptr.h）。 */
value_t *ptr_make_value(vm_t *vm, const type_t *pt, void *target) {
    ptr_value_t pv = { target };
    void *data = value_alloc_data_copy(vm->alloc, pt, &pv);
    return value_make(vm, pt, data);
}

/* ===========================================================================
 * RC 控制块（share/weak，m3-design §12）
 *
 * 布局：rc_block_t 头部 + payload（T 的实际数据，按 type->align 对齐）。
 * share/weak 的 ptr_value_t.ptr 指向 rc_block_t。strong 计数归零时析构
 * payload（递归释放内嵌 own/share 字段），但保留控制块（weak 仍观察）。
 * weak 计数归零时释放控制块 + payload 内存。
 *
 * 线程安全（§12.5）：strong/weak 皆为 atomic，upgrade 用 CAS。
 * =========================================================================== */

rc_block_t *rc_block_new(allocator_t *alloc, const type_t *payload_type) {
    if (!alloc || !payload_type) return NULL;
    /* 分配：头部 + 对齐填充 + payload */
    size_t hdr = sizeof(rc_block_t);
    size_t align = payload_type->align;
    size_t aligned_hdr = (hdr + align - 1) & ~(align - 1);
    size_t total = aligned_hdr + payload_type->size;
    rc_block_t *rc = (rc_block_t *)allocator_new_ex(
        alloc, "rc_block_t", total, NULL, NULL, NULL, align);
    if (!rc) panic("vm: out of memory allocating rc block");
    memset(rc, 0, total);
    atomic_init(&rc->strong, 1);
    atomic_init(&rc->weak, 1);  /* strong 自身占一个 weak 引用 */
    rc->alloc = alloc;
    rc->type = payload_type;
    rc->destroyed = false;
    return rc;
}

void rc_block_strong_inc(rc_block_t *rc) {
    if (!rc) return;
    atomic_fetch_add(&rc->strong, 1);
}

void rc_block_strong_dec(vm_t *vm, rc_block_t *rc) {
    if (!rc || !vm) return;
    /* strong-- ：归零时析构 payload（递归释放内嵌 own/share 字段，
       类似 C++ 析构），然后 weak--（strong 释放其占有的 weak 引用）。 */
    if (atomic_fetch_sub(&rc->strong, 1) == 1) {
        /* 析构 payload：递归释放内嵌 own/share 堆块 */
        if (!rc->destroyed) {
            rc->destroyed = true;
            void *payload = rc_block_payload(rc);
            ptr_free_owned_recursive(vm, rc->type, payload);
        }
        /* strong 释放其占有的 weak 引用 */
        rc_block_weak_dec(vm, rc);
    }
}

void rc_block_weak_inc(rc_block_t *rc) {
    if (!rc) return;
    atomic_fetch_add(&rc->weak, 1);
}

void rc_block_weak_dec(vm_t *vm, rc_block_t *rc) {
    if (!rc || !vm) return;
    /* weak-- ：归零时释放控制块 + payload 内存（此时 strong 已为 0，
       payload 已析构，但内存仍保留供 weak 观察）。 */
    if (atomic_fetch_sub(&rc->weak, 1) == 1) {
        allocator_t *alloc = rc->alloc;
        allocator_free(alloc, (void **)&rc);
    }
}

void *rc_block_upgrade(rc_block_t *rc) {
    if (!rc) return NULL;
    /* CAS 原子检查 strong>0 并递增——防止销毁竞态 */
    unsigned int expected;
    do {
        expected = atomic_load(&rc->strong);
        if (expected == 0) return NULL;  /* 对象已析构 */
    } while (!atomic_compare_exchange_weak(&rc->strong, &expected, expected + 1));
    return rc_block_payload(rc);
}

/* weak 从 share 派生（§12.1）：递增 weak 计数，返回 weak value。 */
value_t *ptr_make_weak_from_share(vm_t *vm, value_t *share_val) {
    if (!vm || !share_val) return value_make_error(vm, "weak: null share value");
    const type_t *st = value_type(share_val);
    if (!st || st->kind != TYPE_KIND_PTR_SHARE)
        return value_make_error(vm, "weak: can only derive from share");
    const ptr_value_t *pv = (const ptr_value_t *)value_data(share_val);
    const type_t *wt = type_ptr_intern(vm, TYPE_KIND_PTR_WEAK, ptr_type_base(st));
    if (!wt) return value_make_error(vm, "weak: cannot make weak pointer type");
    if (pv->ptr) rc_block_weak_inc((rc_block_t *)pv->ptr);
    return ptr_make_value(vm, wt, pv->ptr);
}

/* share 创建：从 fatal 值接管堆块 → 分配 RC 块（payload = 堆块内容拷贝），
 * 返回 share *T value（§12.1：fatal → share 创建通道）。 */
value_t *ptr_make_share_from_fatal(vm_t *vm, value_t *fatal_val) {
    if (!vm || !fatal_val) return value_make_error(vm, "share: null fatal value");
    const type_t *ft = value_type(fatal_val);
    if (!ft || ft->kind != TYPE_KIND_PTR_FATAL)
        return value_make_error(vm, "share: can only create from fatal");
    const type_t *base = ptr_type_base(ft);
    if (!base) return value_make_error(vm, "share: fatal missing base type");
    const ptr_value_t *pv = (const ptr_value_t *)value_data(fatal_val);
    if (!pv->ptr) return value_make_error(vm, "share: fatal has null pointer");

    /* 分配 RC 块，payload 拷贝 fatal 指向的堆块内容 */
    rc_block_t *rc = rc_block_new(vm->alloc, base);
    void *payload = rc_block_payload(rc);
    memcpy(payload, pv->ptr, base->size);

    /* 清空源对象图内嵌 own/share 指针（所有权随拷贝转移到 RC 块 payload） */
    ptr_clear_owned_recursive(vm, base, pv->ptr);

    /* 释放 fatal 的原始堆块（内容已拷贝进 RC 块） */
    void *heap = pv->ptr;
    ((ptr_value_t *)value_data(fatal_val))->ptr = NULL;
    allocator_free(vm->alloc, &heap);

    const type_t *share_t = type_ptr_intern(vm, TYPE_KIND_PTR_SHARE, base);
    if (!share_t) {
        rc_block_strong_dec(vm, rc);
        return value_make_error(vm, "share: cannot make share pointer type");
    }
    return ptr_make_value(vm, share_t, rc);
}
