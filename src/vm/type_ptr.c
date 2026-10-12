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
 * §13.2），所有权完全由 type_t.ownership 区分：
 *   - own *T：总拥有堆块，dispose 释放（new 产物接管后）
 *   - ref *T：总借用，dispose 跳过（指向栈值 / 他人堆块 / ref 派生）
 *   - fatal *T：ptr≠NULL 时拥有（将亡值），转移后 ptr=NULL（接管方拥有）
 *
 * 五种所有权修饰（own/ref/fatal/share/weak）是同一结构、独立 intern 实例
 *（ownership 区分所有权，own *i32 != ref *i32）。同一 base_type 的五种指针各自去重。
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
    if (!type_is_ptr(t)) return;  /* 非指针：无堆块 */
    switch (t->ownership) {
    case OWN_OWN:
    case OWN_FATAL: {
        void *heap = pv->ptr;
        pv->ptr = NULL;
        allocator_free(vm->alloc, &heap);
        break;
    }
    case OWN_SHARE: {
        rc_block_t *rc = (rc_block_t *)pv->ptr;
        pv->ptr = NULL;
        rc_block_strong_dec(vm, rc);
        break;
    }
    case OWN_WEAK: {
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
   fatal **不在此列**：fatal 是"将亡值"标记，其语义是"顶层必被 own 接管"
   （sema R1，§3.3），接管后源 ptr=NULL，扫描本身就是空操作；而**字段位置**的
   fatal（`struct S { p: fatal *i32; }`）不被移动，必须由释放路径处理——那条
   路径走 ptr_free_owned_recursive 的 PTR 分支（已含 OWN_FATAL），不依赖本
   谓词。因此这里不把 fatal 列为"需扫描"是刻意的：本谓词只回答"该字段是否
   可能是**可在别处析构的共享资源**（own/share/weak）或**含此类字段的聚合**"。
   `_ = move(x)` 丢弃场景由 DISPOSE 指令特殊处理。 */
bool ptr_type_needs_scan(const type_t *t) {
    if (!t) return false;
    switch (t->kind) {
    case TYPE_KIND_PTR:
        /* own/share/weak 拥有资源需扫描；ref 借用跳过；fatal 将亡值必被接管 */
        return t->ownership == OWN_OWN ||
               t->ownership == OWN_SHARE ||
               t->ownership == OWN_WEAK;
    case TYPE_KIND_SLICE:
        return t->ownership == OWN_OWN;
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_TUPLE:
    case TYPE_KIND_ARRAY:
    case TYPE_KIND_OPTION:
    case TYPE_KIND_UNION:
        return true;
    default:
        return false;
    }
}

void ptr_free_owned_recursive(vm_t *vm, const type_t *t, void *data) {
    if (!vm || !t || !data) return;
    switch (t->kind) {
    case TYPE_KIND_PTR: {
        if (t->ownership == OWN_OWN || t->ownership == OWN_FATAL) {
            /* own/fatal 指针：ptr≠NULL 拥有堆块 → 先递归销毁堆块内容（T 含 own
               字段时内嵌堆块一并释放），再释放本堆块，置 ptr=NULL 防双释放。

               fatal 为何也在这里释放：顶层 fatal 值（move/clone/new 产物）由
               sema 保证必被消费（ptr=NULL），但 fatal 也可能出现在**结构体字段
               位置**（`struct S { p: fatal *i32; }`）——字段不被移动，作用域退出
               时它就是唯一释放点，跳过即泄漏。释放后置 NULL 使重复扫描幂等。

               ref 总借用跳过（由 ownership 区分，不走此分支）。 */
            ptr_value_t *pv = (ptr_value_t *)data;
            if (!pv->ptr) return;
            const type_t *base = ptr_type_base(t);
            if (base && ptr_type_needs_scan(base))
                ptr_free_owned_recursive(vm, base, pv->ptr);
            void *heap = pv->ptr;
            pv->ptr = NULL;
            allocator_free(vm->alloc, &heap);
        } else if (t->ownership == OWN_SHARE) {
            /* share 字段析构：递减 strong 计数（strong→0 时 rc_block_strong_dec
               递归析构 payload 内嵌 own/share 字段）。 */
            ptr_value_t *pv = (ptr_value_t *)data;
            if (!pv->ptr) return;
            rc_block_t *rc = (rc_block_t *)pv->ptr;
            rc_block_strong_dec(vm, rc);
            pv->ptr = NULL;
        } else if (t->ownership == OWN_WEAK) {
            /* weak 字段析构：递减 weak 计数。 */
            ptr_value_t *pv = (ptr_value_t *)data;
            if (!pv->ptr) return;
            rc_block_t *rc = (rc_block_t *)pv->ptr;
            rc_block_weak_dec(vm, rc);
            pv->ptr = NULL;
        }
        /* ref/fatal：ref 借用跳过；fatal 将亡值必被 own 接管（ptr=NULL） */
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
    case TYPE_KIND_SLICE: {
        if (t->ownership != OWN_OWN) return;  /* ref/fatal 切片无 own 堆块 */
        /* own []T：拥有堆块 → 先递归销毁元素内嵌 own 字段，再释放堆块，
           置 ptr=NULL 防双释放。 */
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
    case TYPE_KIND_PTR: {
        if (t->ownership == OWN_OWN || t->ownership == OWN_FATAL) {
            ptr_value_t *pv = (ptr_value_t *)data;
            if (!pv->ptr) return;
            const type_t *base = ptr_type_base(t);
            if (base && ptr_type_needs_scan(base))
                ptr_clear_owned_recursive(vm, base, pv->ptr);
            pv->ptr = NULL;  /* 只清指针：堆块已归接管方，不释放 */
        } else if (t->ownership == OWN_SHARE || t->ownership == OWN_WEAK) {
            /* share/weak 随 memcpy 复制后，计数已在 clone/assign 时递增，
               清源 ptr=NULL 防作用域退出递减两次。不递减计数——接管方
               持有的 share/weak 已有独立计数。 */
            ptr_value_t *pv = (ptr_value_t *)data;
            if (!pv->ptr) return;
            pv->ptr = NULL;
        }
        /* ref：借用，无 own 指针可清 */
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
    case TYPE_KIND_SLICE: {
        if (t->ownership != OWN_OWN) return;  /* ref/fatal 切片无 own 指针可清 */
        /* own []T：堆块已随 memcpy 复制进接管方，清源 ptr=NULL 防作用域
           退出时与接管方双释放。递归清空元素内嵌 own 指针（堆块归接管方，
           不释放——同 OWN_OWN 语义）。 */
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
    case TYPE_KIND_PTR: {
        if (type->ownership == OWN_OWN || type->ownership == OWN_FATAL) {
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
        } else if (type->ownership == OWN_SHARE) {
            /* share clone：递增 strong 计数，复制指针值（共享同一 RC 块）。 */
            const ptr_value_t *sp = (const ptr_value_t *)src;
            if (sp->ptr) rc_block_strong_inc((rc_block_t *)sp->ptr);
            memcpy(dst, src, type->size);
        } else if (type->ownership == OWN_WEAK) {
            /* weak clone：递增 weak 计数，复制指针值。 */
            const ptr_value_t *sp = (const ptr_value_t *)src;
            if (sp->ptr) rc_block_weak_inc((rc_block_t *)sp->ptr);
            memcpy(dst, src, type->size);
        } else {
            /* ref：复制指针值（借用，不深拷贝被指物） */
            memcpy(dst, src, type->size);
        }
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
    case TYPE_KIND_SLICE: {
        if (type->ownership == OWN_OWN || type->ownership == OWN_FATAL) {
            /* own/fatal 切片深拷贝：分配新堆块 + 逐元素递归克隆，新胖指针指向
               独立新堆块（同 OWN_OWN/OWN_FATAL 指针的深拷贝语义）。ptr=NULL 时
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
        } else {
            /* ref 切片：胖指针值拷贝（借用，不深拷贝） */
            memcpy(dst, src, type->size);
        }
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

    if (t->ownership == OWN_OWN) {
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
    }
    if (t->ownership == OWN_FATAL) {
        /* fatal 流转：转移——新值接管裸指针，源置 NULL（fatal 是
           将亡值，接管点在 DEFINE/参数接收处，不深拷贝——避免 move/clone
           产物再深拷贝一次导致双分配）。 */
        value_t *nv = ptr_make_value(vm, t, pv->ptr);
        ((ptr_value_t *)value_data(v))->ptr = NULL;
        return nv;
    }
    if (t->ownership == OWN_SHARE) {
        /* share clone：递增 strong 计数，共享同一 RC 块（§12.2）。 */
        if (pv->ptr) rc_block_strong_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, t, pv->ptr);
    }
    if (t->ownership == OWN_WEAK) {
        /* weak clone：递增 weak 计数，共享同一 RC 块（§12.2）。 */
        if (pv->ptr) rc_block_weak_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, t, pv->ptr);
    }
    /* ref 及一切借用：复制指针值（ref 是值拷贝语义，§3.2） */
    return ptr_make_value(vm, t, pv->ptr);
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
    if (dt->ownership == OWN_SHARE || dt->ownership == OWN_WEAK) {
        ptr_value_t *sp = (ptr_value_t *)value_data(src);
        if (sp->ptr) {
            if (dt->ownership == OWN_SHARE)
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
 * C 语义：**限定符只能加不能去**——`ref *T → ref *const T` 永远安全，
 * 反向（去 const）必须显式 `as`。 */
/* 向上转换**不得丢限定**：源 pointee 带 const 时目标也必须带 const。
 * （加 const/volatile 由 ptr_base_adds_quals 分支处理；volatile 已被 sema 折叠，
 * 源码类型里不会出现。缺这一条时 `ref *const T → ref *T` 会因为"剥掉限定后前缀
 * 兼容"被误判成向上转换，从而绕过 const 保护。） */
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
 * 与下方 ptr_implicit_cast 的转换分支**逐一对应**——之所以抽出来，是因为
 * shadow 值（sema 的纯类型检查，data=NULL）也必须走同一套判据：
 * 早前 shadow 直接 "return value_make_shadow(vm, target)" 无条件放行，
 * 于是**非法**的指针隐式转换（例如 `ref *Test → ref *const Base`，Test 的字段
 * 前缀不是 Base 的字段）在 sema 阶段被静默放行，直到运行期才报
 * "ptr: unsupported implicit cast for pointer"（编译期该报的错漏到运行期）。 */
static bool ptr_implicit_cast_ok(vm_t *vm, const type_t *src,
                                 const type_t *target) {
    if (!src || !target) return false;
    if (src == target) return true; /* 同类型身份拷贝 */
    if (is_opaque_type(target)) return true; /* 任意指针 → opaque */

    /* 同种所有权修饰 + 只给 pointee 加限定：身份拷贝 */
    if (src->ownership == target->ownership &&
        ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target)))
        return true;

    /* 结构体指针向上转换（大 → 小，前缀字段兼容；只允许借用目标） */
    if (target->ownership == OWN_REF &&
        (src->ownership == OWN_REF || src->ownership == OWN_OWN) &&
        ptr_upcast_keeps_quals(ptr_type_base(src), ptr_type_base(target)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(target)),
            type_strip_quals(ptr_type_base(src))))
        return true;

    /* own → ref（借用的表达） */
    if (src->ownership == OWN_OWN && target->ownership == OWN_REF &&
        ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target)))
        return true;

    /* fatal → own 接管 */
    if (src->ownership == OWN_FATAL && target->ownership == OWN_OWN &&
        ptr_type_base(src) == ptr_type_base(target))
        return true;

    /* fatal → share 创建 */
    if (src->ownership == OWN_FATAL && target->ownership == OWN_SHARE &&
        ptr_type_base(src) == ptr_type_base(target))
        return true;

    /* share → weak 派生 */
    if (src->ownership == OWN_SHARE && target->ownership == OWN_WEAK &&
        ptr_type_base(src) == ptr_type_base(target))
        return true;

    /* share → share / weak → weak（引用计数拷贝） */
    if (src->ownership == OWN_SHARE && target->ownership == OWN_SHARE &&
        ptr_type_base(src) == ptr_type_base(target))
        return true;
    if (src->ownership == OWN_WEAK && target->ownership == OWN_WEAK &&
        ptr_type_base(src) == ptr_type_base(target))
        return true;

    return false;
}

static value_t *ptr_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    const type_t *src = value_type(v);

    /* shadow（sema 纯类型检查，data=NULL）：只做**类型层**判据，通过则返回
       target 的 shadow。判据与下面真实转换完全一致，因此 sema 报的错与运行期
       一致（非法转换在编译期就被拒绝，不再漏到运行期）。 */
    if (value_is_shadow(v)) {
        if (!ptr_implicit_cast_ok(vm, src, target))
            return value_make_error(vm, "ptr: unsupported implicit cast for pointer");
        return value_make_shadow(vm, target);
    }

    const ptr_value_t *pv = (const ptr_value_t *)value_data(v);

    /* 任意指针 → opaque 隐式（m3-design §8.4）：身份拷贝指针目标地址。
       opaque 是裸指针，不拥有堆块；堆块仍归源指针释放。 */
    if (is_opaque_type(target)) {
        void *data = value_alloc_data_copy(vm->alloc, target, &pv->ptr);
        return value_make(vm, target, data);
    }

    /* 同种所有权修饰、目标只是给 pointee **加了 const/volatile**：身份拷贝。
       不加这一支的话 sema 放行、运行期报 "ptr: unsupported implicit cast for
       pointer"——`ref *T` 传给 `ref *const T` 形参（例如方法的第一参数写成
       `ref *const Test`）就会踩到。share/weak 需要维护引用计数。 */
    if (target && src->ownership == target->ownership &&
        ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target))) {
        if (src->ownership == OWN_SHARE && pv->ptr)
            rc_block_strong_inc((rc_block_t *)pv->ptr);
        if (src->ownership == OWN_WEAK && pv->ptr)
            rc_block_weak_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* ---- 结构体指针**向上自动转换**（大 → 小，前缀字段兼容）----
     * `ref *Big → ref *Small`、`own *Big → ref *Small`：Small 的字段恰是 Big 的
     * 前缀 → C 布局下前缀字段偏移与类型完全一致，指针身份拷贝即可（不涉及所有权）。
     * 这是配合结构体语法糖的能力：把"派生"结构体的指针直接喂给按"基"结构体声明的
     * 接收者/参数（`Base::getId(d.&)`）。
     * 只允许**借用目标**（ref）：own/share 目标会按小结构体释放/析构，尾部字段
     * 的 own 资源会被漏掉（泄漏），故不隐式允许；反向（小 → 大）没有存储，永不允许。 */
    if (target && target->ownership == OWN_REF &&
        (src->ownership == OWN_REF || src->ownership == OWN_OWN) &&
        ptr_upcast_keeps_quals(ptr_type_base(src), ptr_type_base(target)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(target)),
            type_strip_quals(ptr_type_base(src)))) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* own → ref 身份拷贝（借用的表达，§3.2）：同 base 的 own → ref
       （含给 pointee 加 const/volatile 的情形：own *T → ref *const T）。
       ref 总借用——不拥有堆块，堆块归原 own 释放。 */
    if (src->ownership == OWN_OWN && target &&
        target->ownership == OWN_REF &&
        ptr_base_adds_quals(ptr_type_base(src), ptr_type_base(target))) {
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* fatal → own 接管（§3.3）：var c: own *i32 = clone(p) / move(p) 的
       DEFINE 路径。fatal 是将亡值——新值接管裸指针（转移语义，
       与 ptr_clone fatal 分支一致），源置 NULL 防双释放。 */
    if (src->ownership == OWN_FATAL && target &&
        target->ownership == OWN_OWN &&
        ptr_type_base(src) == ptr_type_base(target)) {
        value_t *nv = ptr_make_value(vm, target, pv->ptr);
        ((ptr_value_t *)value_data(v))->ptr = NULL;
        return nv;
    }

    /* fatal → share 创建（§12.1）：var s: share *T = <fatal 值>。
       fatal 接管三路之一：从 fatal 堆块创建 RC 块（payload = 堆块内容拷贝），
       源置 NULL 防双释放。单向隔离：share 不能转回 own/ref。 */
    if (src->ownership == OWN_FATAL && target &&
        target->ownership == OWN_SHARE &&
        ptr_type_base(src) == ptr_type_base(target)) {
        value_t *sv = ptr_make_share_from_fatal(vm, v);
        return sv;
    }

    /* share → weak 派生（§12.1）：weak 只从 share 派生。递增 weak 计数，
       复制 RC 块指针。share 保持不变（不递减 strong——weak 是额外观察者）。 */
    if (src->ownership == OWN_SHARE && target &&
        target->ownership == OWN_WEAK &&
        ptr_type_base(src) == ptr_type_base(target)) {
        return ptr_make_weak_from_share(vm, v);
    }

    /* share → share（copy = 计数+1，§12.2） */
    if (src->ownership == OWN_SHARE && target &&
        target->ownership == OWN_SHARE &&
        ptr_type_base(src) == ptr_type_base(target)) {
        if (pv->ptr) rc_block_strong_inc((rc_block_t *)pv->ptr);
        return ptr_make_value(vm, target, pv->ptr);
    }

    /* weak → weak（copy = 计数+1，§12.2） */
    if (src->ownership == OWN_WEAK && target &&
        target->ownership == OWN_WEAK &&
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
    if (!type_is_ptr(a) || !type_is_ptr(b)) return false;
    if (a->ownership != b->ownership) return false;
    return type_equal(vm, ptr_type_base(a), ptr_type_base(b));
}

static bool ptr_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    if (!sup) return false;
    if (!type_is_ptr(sub)) return false;
    /* 指针按 base 递归：同所有权修饰下 base 兼容即指针兼容。
       （跨所有权修饰的兼容由 sema 层控制——own → ref 的借用在转换层
       处理，类型值层面的鸭子判断不混用所有权。） */
    if (sub->ownership != sup->ownership) return false;
    /* 结构体指针**向上转换**：`ref *Big`（sub，实参）用在 `ref *Small`（sup，形参）
       处合法——Small 的字段是 Big 的前缀（C 公共起始序列）。反向不成立。
       与 ptr_implicit_cast 的对应分支必须成对存在（sema 放行 + 运行期落地）。 */
    if (sup->ownership == OWN_REF &&
        ptr_upcast_keeps_quals(ptr_type_base(sub), ptr_type_base(sup)) &&
        struct_type_prefix_compatible(vm,
            type_strip_quals(ptr_type_base(sup)),
            type_strip_quals(ptr_type_base(sub))))
        return true;
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
static char *ptr_name(allocator_t *alloc, ownership_t ownership, const type_t *base) {
    const char *owner;
    switch (ownership) {
    case OWN_OWN:   owner = "own ";   break;
    case OWN_REF:   owner = "ref ";   break;
    case OWN_FATAL: owner = "fatal "; break;
    case OWN_SHARE: owner = "share "; break;
    case OWN_WEAK:  owner = "weak ";  break;
    default:        owner = "ptr ";   break;
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
static ptr_type_t *ptr_type_create_open(vm_t *vm, ownership_t ownership) {
    ptr_type_t *pt = (ptr_type_t *)allocator_new_ex(
        vm->alloc, "ptr_type_t", sizeof(ptr_type_t), NULL, NULL, NULL, 1);
    if (!pt) panic("vm: out of memory allocating pointer type");
    memset(pt, 0, sizeof(ptr_type_t));
    switch (ownership) {
    case OWN_OWN:   pt->base.vtable = &VTABLE_PTR_OWN;   break;
    case OWN_REF:   pt->base.vtable = &VTABLE_PTR_REF;   break;
    case OWN_FATAL: pt->base.vtable = &VTABLE_PTR_FATAL; break;
    case OWN_SHARE: pt->base.vtable = &VTABLE_PTR_SHARE; break;
    case OWN_WEAK:  pt->base.vtable = &VTABLE_PTR_WEAK;  break;
    default:
        panic("vm: invalid pointer ownership");
        break;
    }
    pt->base.kind = TYPE_KIND_PTR;
    pt->base.ownership = ownership;
    /* name/size/align 由 seal 按 base 填充；base_type 由 SET_TYPE 设定；sealed 置 0 */
    return pt;
}

/* PUSH_PTR：分配开放 ptr_type（base_type=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 base_type → SEAL 密封收尾。
 * ownership = 所有权修饰（OWN_OWN/REF/FATAL）。 */
const type_t *type_ptr_push(vm_t *vm, ownership_t ownership) {
    if (!vm) return NULL;
    ptr_type_t *pt = ptr_type_create_open(vm, ownership);
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

/* SEAL：按 base_type 去重 intern（同一 base 的五种所有权是五个独立实例），
 * 标记 sealed，返回该 const type（允许链式）。t 须为已设 base_type 的开放
 * ptr 类型（type_ptr_push 产物）。 */
const type_t *type_ptr_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t) return NULL;
    if (!type_is_ptr(t)) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    ptr_type_t *pt = (ptr_type_t *)t;

    /* 必须已设 base_type（SET_TYPE） */
    if (!pt->base_type) return NULL;

    if (!vm->ptr_types) vm->ptr_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 base_type + ownership）：命中已有 sealed 类型则复用
     * 并手工回收本开放类型 */
    size_t n = vec_len(vm->ptr_types);
    for (size_t i = 0; i < n; i++) {
        const ptr_type_t *other = (const ptr_type_t *)vec_get(vm->ptr_types, i);
        if (other && other != pt && type_is_sealed(&other->base) &&
            other->base.ownership == pt->base.ownership &&
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
    char *name = ptr_name(vm->alloc, pt->base.ownership, pt->base_type);
    if (!name) panic("vm: out of memory allocating pointer type name");
    pt->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->ptr_types, vm->alloc, pt);  /* 密封后入池（去重 intern） */
    return &pt->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_ptr_intern(vm_t *vm, ownership_t ownership, const type_t *base) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_ptr_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_ptr_seal 完成。 */
    if (!vm || !base) return NULL;
    if (ownership != OWN_OWN && ownership != OWN_REF &&
        ownership != OWN_FATAL &&
        ownership != OWN_SHARE && ownership != OWN_WEAK) {
        return NULL;
    }
    ptr_type_t *pt = ptr_type_create_open(vm, ownership);
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
    if (!st || st->ownership != OWN_SHARE)
        return value_make_error(vm, "weak: can only derive from share");
    const ptr_value_t *pv = (const ptr_value_t *)value_data(share_val);
    const type_t *wt = type_ptr_intern(vm, OWN_WEAK, ptr_type_base(st));
    if (!wt) return value_make_error(vm, "weak: cannot make weak pointer type");
    if (pv->ptr) rc_block_weak_inc((rc_block_t *)pv->ptr);
    return ptr_make_value(vm, wt, pv->ptr);
}

/* share 创建：从 fatal 值接管堆块 → 分配 RC 块（payload = 堆块内容拷贝），
 * 返回 share *T value（§12.1：fatal → share 创建通道）。 */
value_t *ptr_make_share_from_fatal(vm_t *vm, value_t *fatal_val) {
    if (!vm || !fatal_val) return value_make_error(vm, "share: null fatal value");
    const type_t *ft = value_type(fatal_val);
    if (!ft || ft->ownership != OWN_FATAL)
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

    const type_t *share_t = type_ptr_intern(vm, OWN_SHARE, base);
    if (!share_t) {
        rc_block_strong_dec(vm, rc);
        return value_make_error(vm, "share: cannot make share pointer type");
    }
    return ptr_make_value(vm, share_t, rc);
}
