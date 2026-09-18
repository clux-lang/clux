#include "vm/type_option.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"

#include <string.h>

/* ===========================================================================
 * optional 修饰类型（?T）
 *
 * option_type_t 继承 type_t 持 inner 指针。C 内存映射：
 *   struct Optional_T { bool ok; T value; }
 * value 字段偏移 voff = align_up(sizeof(bool), alignof(T)) = max(1, inner->align)
 * size = voff + inner->size；align = max(1, inner->align)（密封时计算）。
 *
 * 运算行为（m2-design §9/§12.1）：
 *   - eq/ne 无分派：?T 只能与 nil 比较，tag 比较由编译器发专用指令
 *     （比较 ok tag 的 bool），vtable eq/ne 返回错误。
 *   - clone/assign/dispose 转发 inner（复用 value_blit_raw / value_dispose_raw，
 *     递归深拷贝/释放 value 字段），ok tag 平凡拷贝/无资源。
 *   - implicit_cast：?T → ?T 同 inner 身份拷贝；?T → inner 窄化是运行期
 *     tag 检查 + 数据移动，由编译器发专用指令（sema 窄化），vtable 不处理。
 *   - T → ?T 隐式提升（some）在 value_implicit_cast 公共入口特判
 *     （value_lift_option，见 value.c），此处 vtable 只处理 ?T 自身。
 * =========================================================================== */

/* ---- 生命周期：clone / assign / dispose（复用 value_blit_raw / value_dispose_raw） ---- */

static value_t *option_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    value_blit_raw(vm, data, value_data(v), t);
    return value_make(vm, t, data);
}

static value_t *option_assign(vm_t *vm, value_t *dst, value_t *src) {
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    const type_t *t = value_type(dst);
    if (value_type(src) != t) {
        return value_make_error(vm,
            "assign: optional type mismatch on assignment");
    }
    value_dispose_raw(vm, value_data(dst), t);
    value_blit_raw(vm, value_data(dst), value_data(src), t);
    return dst;
}

static void option_dispose(vm_t *vm, value_t *v) {
    /* 借用引用不拥有 data（指向父值内部），跳过；由 value_dispose 统一拦截 */
    if (value_is_borrowed(v)) return;
    value_dispose_raw(vm, value_data(v), value_type(v));
}

/* ---- eq/ne：?T 只能与 nil 比较，tag 比较由编译器发专用指令 ---- */

static value_t *option_eq(vm_t *vm, value_t *a, value_t *b) {
    (void)a; (void)b;
    return value_make_error(vm, "?: optional can only be compared with nil");
}

static value_t *option_ne(vm_t *vm, value_t *a, value_t *b) {
    (void)a; (void)b;
    return value_make_error(vm, "?: optional can only be compared with nil");
}

/* ---- 类型转换：?T → ?T 同 inner 身份拷贝；其他不支持（窄化/提升走专用路径） ---- */

static value_t *option_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    /* ?T → ?U 当 T == U：同一 inner 的身份拷贝 */
    if (target && target->kind == TYPE_KIND_OPTION &&
        type_option_inner(target) == type_option_inner(value_type(v))) {
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    return value_make_error(vm, "?: unsupported implicit cast for optional");
}

static value_t *option_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* 显式窄化 ?T → T（!ok 报错）在编译器发专用指令；vtable 只允许同 inner
       ?T 间的拷贝与到 inner 的降级由窄化指令处理。此处仅支持同 inner ?T */
    return option_implicit_cast(vm, v, target);
}

/* ---- 鸭子类型判断：?T 是独立类型（?i32 == i32 → false）；按 inner 递归 ---- */

static bool option_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    if (a == b) return true;
    if (!b || b->kind != TYPE_KIND_OPTION) return false;
    return type_equal(vm, type_option_inner(a), type_option_inner(b));
}

static bool option_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    if (sub == sup) return true;
    /* ?T 不兼容到 T（反向需窄化）；T 可提升到 ?T（D1，type_extends 是
       类型值层面的鸭子判断，兼容方向由 sema/编译器层控制）。此处仅支持
       ?T extends ?U 当 T extends U。 */
    if (!sup || sup->kind != TYPE_KIND_OPTION) return false;
    return type_extends(vm, type_option_inner(sub), type_option_inner(sup));
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_OPTION = {
    .eq = option_eq, .ne = option_ne,
    .dispose = option_dispose,
    .clone = option_clone,
    .assign = option_assign,
    .implicit_cast = option_implicit_cast,
    .explicit_cast = option_explicit_cast,
    .type_equal = option_type_equal,
    .type_extends = option_type_extends,
    .type_seal = type_option_seal, /* 开放构造路径（PUSH_OPT → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_OPT / SET_TYPE / SEAL，与 const/volatile 统一的两遍构造）
 * =========================================================================== */

/* 构造名 "?<inner 名>"（堆分配，vm 拥有） */
static char *option_name(allocator_t *alloc, const type_t *inner) {
    size_t il = inner && inner->name.ptr ? inner->name.len : 0;
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 il + 2);
    if (!buf) return NULL;
    buf[0] = '?';
    if (il) memcpy(buf + 1, inner->name.ptr, il);
    buf[1 + il] = '\0';
    return buf;
}

/* 分配开放 option_type（inner=NULL，不入池，不压栈）。供字节码路径
 * （type_option_push）与 intern 快捷（type_option_intern）共用。 */
static option_type_t *option_type_create_open(vm_t *vm) {
    option_type_t *ot = (option_type_t *)allocator_new_ex(
        vm->alloc, "option_type_t", sizeof(option_type_t), NULL, NULL, NULL, 1);
    if (!ot) panic("vm: out of memory allocating optional type");
    memset(ot, 0, sizeof(option_type_t));
    ot->base.vtable = &VTABLE_OPTION;
    ot->base.kind   = TYPE_KIND_OPTION;
    /* name/size/align 由 seal 按 inner 填充；inner 由 SET_TYPE 设定；sealed 置 0 */
    return ot;
}

/* PUSH_OPT：分配开放 option_type（inner=NULL，不入池）+ 压其 type value。
 * 返回开放 type（未密封），经 SET_TYPE 设 inner → SEAL 密封收尾。 */
const type_t *type_option_push(vm_t *vm) {
    if (!vm) return NULL;
    option_type_t *ot = option_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &ot->base));
    return &ot->base; /* 外部只持有 type_t*，不感知 option_type_t 子类 */
}

/* SET_TYPE 运行期用：设被包裹类型 T（密封后静默忽略） */
void type_option_set_inner(vm_t *vm, const type_t *t, const type_t *inner) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_OPTION || type_is_sealed(t)) return;
    ((option_type_t *)t)->inner = inner;
}

/* SEAL：按 C 对齐规则计算布局（struct Optional_T{bool ok;T value;}，
 * value 偏移 align_up(1, alignof(T))），按 inner 指针去重 intern，标记
 * sealed，返回该 const type（允许链式）。t 须为已设 inner 的开放
 * option 类型（type_option_push 产物）。 */
const type_t *type_option_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_OPTION) return NULL;
    if (type_is_sealed(t)) return t;  /* 已密封直接返回（幂等） */

    option_type_t *ot = (option_type_t *)t;

    /* 必须已设 inner（SET_TYPE） */
    if (!ot->inner) return NULL;

    if (!vm->option_types) vm->option_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern（按 inner 指针）：命中已有 sealed 类型则复用并手工回收本开放类型 */
    size_t n = vec_len(vm->option_types);
    for (size_t i = 0; i < n; i++) {
        const option_type_t *other = (const option_type_t *)vec_get(vm->option_types, i);
        if (other && other != ot && type_is_sealed(&other->base) &&
            other->inner == ot->inner) {
            /* 本开放类型与已有 sealed 类型重复：复用 other。
             * 操作数栈中引用本开放类型 ot 的 type value 由 value_seal 负责
             * 重定向到 other（避免悬空）；此处仅手工回收 ot。 */
            if (ot->base.name.ptr) {
                char *np = (char *)ot->base.name.ptr;
                allocator_free(vm->alloc, (void **)&np);
            }
            allocator_free(vm->alloc, (void **)&ot);
            return &other->base;
        }
    }

    /* 计算内存布局：struct Optional_T{bool ok;T value;}
       voff = align_up(sizeof(bool), inner->align) = max(1, inner->align)
       size = voff + inner->size；align = max(1, inner->align) */
    size_t voff = option_value_offset(t);
    ot->base.size   = voff + ot->inner->size;
    ot->base.align  = ot->inner->align > 1 ? ot->inner->align : 1;
    ot->base.sealed = true;

    /* 重建类型名 "?<inner 名>"（inner 密封后名字已定） */
    char *name = option_name(vm->alloc, ot->inner);
    if (!name) panic("vm: out of memory allocating optional type name");
    ot->base.name = (strslice_t){ name, strlen(name) };

    vec_push(vm->option_types, vm->alloc, ot);  /* 密封后入池（去重 intern） */
    return &ot->base;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_option_intern(vm_t *vm, const type_t *inner) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。
     * 与 type_option_push 的区别：不向操作数栈压入 type value（嵌套构造
     * 场景避免栈布局污染）。去重 intern 由 type_option_seal 完成。 */
    if (!vm || !inner) return NULL;
    option_type_t *ot = option_type_create_open(vm);
    ot->inner = inner;
    return type_option_seal(vm, &ot->base);
}

/* ===========================================================================
 * T → ?T 隐式提升（some）
 * =========================================================================== */

/* 压 ok=true + T 值深拷贝到 value 字段。返回提升后的 ?T value（auto-track）
 * 或 error value。 */
value_t *value_lift_option(vm_t *vm, value_t *v, const type_t *target) {
    if (!v || !value_type(v) || !target || target->kind != TYPE_KIND_OPTION)
        return value_make_error(vm, "?: invalid option lift target");

    /* shadow：只算类型 */
    if (value_is_shadow(v))
        return value_make_shadow(vm, target);

    const type_t *inner = type_option_inner(target);
    if (inner != value_type(v))
        return value_make_error(vm, "?: option lift requires matching inner type");

    void *data = value_alloc_data(vm->alloc, target);
    *(bool *)data = true;   /* ok tag */
    value_blit_raw(vm, (uint8_t *)data + option_value_offset(target),
                   value_data(v), inner);
    return value_make(vm, target, data);
}
