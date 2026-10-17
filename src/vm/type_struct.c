#include "vm/type_struct.h"
#include "vm/type.h"
#include "vm/type_ptr.h"
#include "vm/value_compound_assign.h" /* compound_assign_transfer（赋值共享实现） */
#include "vm/str_pool.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"

#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * 结构体类型（struct）
 *
 * struct_type_t 继承 type_t 持字段表 field_t { name, offset, type }。struct
 * value 的 data 是连续内存块（size = type->size），字段按偏移 O(1) 读写。
 * 布局在密封时按 C 对齐规则一次性计算：
 *   offset_0 = 0；offset_i = align_up(prev_end, field_i.align)
 *   size = align_up(last_end, max_align)；align = max(字段 align)
 *
 * 生命周期（clone/assign/dispose）：data 全平凡（字段为 memcpy 可拷贝的
 * 裸字节块，字符串归 vm 字符串池），整块 memcpy / 无需释放。
 *
 * 严格类型：implicit_cast / explicit_cast 仅同 struct 实例（身份拷贝）；
 * eq/ne 同实例按字段递归比较；type_equal / type_extends 按指针（具名类型，
 * M2 鸭子类型检查留待后续 Phase）。
 *
 * M2 结构兼容（鸭子类型，2026-09-22）：跨具名类型的赋值/判等/隐式转换按
 * 结构兼容判断（struct_type_compatible：字段名 + 字段类型 type_equal +
 * 字段顺序完全一致 ⟹ 布局相同）——struct A 与 struct B 字段一致时可互
 * 赋值；匿名构造 .{...} 推断的匿名类型也按此校验。
 *
 * 结构体组合（`...Base;`，2026-xx）：类型的字段表与嵌入表（embeds）在
 * **seal 之前**可编辑（type_struct_insert_embed_fields / type_struct_set_embeds），
 * seal 之后不可变——两个写入 API 对 sealed 类型拒绝，把不变量固化在代码里。
 * =========================================================================== */

/* ---- 生命周期：clone / assign / dispose ---- */

/* 结构体字段禁止携带指针/切片（sema 保证），因此结构体永远不含 own 资源——
   clone/dispose 全部走平凡 memcpy 路径，无需字段级递归释放/深拷贝。 */

static value_t *struct_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v))
        return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);

    /* 平凡结构体（字段全为标量/嵌套复合/字符串，无指针/切片）：
       整块 memcpy 即可。 */
    void *data = value_alloc_data(vm->alloc, t);
    memcpy(data, value_data(v), t->size);
    return value_make(vm, t, data);
}

static value_t *struct_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* safe_cast 语义（与 int_assign/float_assign 同款）：类型不同先向左值
       类型 implicit_cast——结构兼容（鸭子类型，字段名+类型+顺序一致）的
       A→B 跨具名类型赋值经 struct_implicit_cast 转换通过；不兼容 → cast
       返回 error，此处直接传播。shadow 检查在 cast 之后（类型校验先于
       shadow 短路，sema 编译期靠此拦截结构不兼容赋值）。 */
    if (value_type(src) != value_type(dst)) {
        value_t *casted = value_implicit_cast(vm, src, value_type(dst));
        if (value_is_error(vm, casted)) return casted;
        src = casted;
    }
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    /* 共享实现：释放左值旧值 → memcpy → 清空源（详见 value_compound_assign.h）。
       整块 memcpy 覆盖是**错的**：旧字段的裸指针被覆盖 → 泄漏；源与 dst 共享
       内嵌堆块 → 双释放。 */
    return compound_assign_transfer(vm, dst, src);
}

static void struct_dispose(vm_t *vm, value_t *v) {
    /* 结构体字段禁止携带指针/切片——无 own 堆块需递归释放。
       data 块本身由 value_dispose 统一释放。 */
    (void)vm; (void)v;
}

/* ---- 判等：同实例按字段递归比较（值相等，非指针） ---- */

/* 按类型读标量值并比较（int/float/bool/str 指针等标量字段） */
static bool struct_field_equal(vm_t *vm, const void *a, const void *b,
                               const type_t *ft) {
    if (!ft || ft->kind == TYPE_KIND_STR) {
        /* str：data 为池内指针（池去重：同指针即同内容；兜底 strcmp） */
        const char *sa = *(const char *const *)a;
        const char *sb = *(const char *const *)b;
        if (sa == sb) return true;
        if (!sa || !sb) return false;
        return strcmp(sa, sb) == 0;
    }
    switch (ft->kind) {
        case TYPE_KIND_INT:
        case TYPE_KIND_FLOAT:
        case TYPE_KIND_BOOL:
            /* 标量按字节比较（同类型 size 一致，无 padding） */
            return memcmp(a, b, ft->size) == 0;
        case TYPE_KIND_ARRAY:
        case TYPE_KIND_OPTION:
        case TYPE_KIND_STRUCT: {
            /* 复合：递归比较。构造临时借用 value 分派 value_eq。 */
            value_t *va = value_make_borrowed(vm, ft, (void *)a);
            value_t *vb = value_make_borrowed(vm, ft, (void *)b);
            value_t *r = value_eq(vm, va, vb);
            if (value_is_error(vm, r)) return false;
            return value_as(r, bool);
        }
        default:
            return memcmp(a, b, ft->size) == 0;
    }
}

static value_t *struct_eq(vm_t *vm, value_t *a, value_t *b) {
    /* safe_cast 语义（与标量 VTABLE_BINARY 同款）：类型不同时尝试右值
       implicit_cast 到左值类型——结构兼容（鸭子类型）的 A→B 比较经
       struct_implicit_cast 转换通过；不兼容 → cast error 传播。 */
    VTABLE_BINARY(vm, a, b, eq, "==");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);

    const type_t *t = value_type(a);
    size_t n = struct_type_field_count(t);
    bool r = true;
    for (size_t i = 0; i < n && r; i++) {
        const struct_field_t *f = struct_type_field(t, i);
        r = struct_field_equal(vm, (const uint8_t *)value_data(a) + f->offset,
                               (const uint8_t *)value_data(b) + f->offset,
                               f->type);
    }
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

static value_t *struct_ne(vm_t *vm, value_t *a, value_t *b) {
    VTABLE_BINARY(vm, a, b, ne, "!=");
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    value_t *eq = struct_eq(vm, a, b);
    if (value_is_error(vm, eq)) return eq;
    bool r = !value_as(eq, bool);
    void *data = value_alloc_data_copy(vm->alloc, vm->type_bool, &r);
    return value_make(vm, vm->type_bool, data);
}

/* ---- 类型转换：结构兼容（鸭子类型）即可转换 ---- */

static value_t *struct_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* fatal 变体 → 同布局的非 fatal 类型（`T` / `const T`）：**接管转换**（memcpy
       顶层 + 清空源的 own/fatal 指针），不是普通整块拷贝。理由见
       value_take_fatal_into（共享内嵌 own 堆块 → 双释放，实测 access violation）。 */
    value_t *taken = value_take_fatal_into(vm, v, target,
                                           struct_type_compatible);
    if (taken) return taken;
    /* T → const T / volatile T（可嵌套）：**只加限定**的隐式转换（C 语义：
       限定符只能加不能去）——同一份数据的视图，身份拷贝即可。sema 允许该转换
       （构造、参数传递、TDZ 初始化三处都合法），运行期必须落地；不加这一支就会
       报 "struct: implicit cast only within same struct"。 */
    if (target && type_has_quals(target) && !type_has_quals(value_type(v)) &&
        struct_type_compatible(vm, type_strip_quals(target), value_type(v))) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    if (!target || !struct_type_compatible(vm, target, value_type(v)))
        return value_make_error(vm, "struct: implicit cast only within same struct");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    /* 结构体字段禁止携带指针/切片——平凡 memcpy 即可（无内嵌 own 堆块
       需深拷贝/清空）。 */
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

static value_t *struct_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
    /* `as` 同样允许加限定（与隐式一致；去限定仍要求类型完全相同） */
    if (target && type_has_quals(target) && !type_has_quals(value_type(v)) &&
        struct_type_compatible(vm, type_strip_quals(target), value_type(v))) {
        if (value_is_shadow(v)) return value_make_shadow(vm, target);
        void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
        return value_make(vm, target, data);
    }
    if (!target || !struct_type_compatible(vm, target, value_type(v)))
        return value_make_error(vm, "struct: explicit cast only within same struct");
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
}

/* ---- 鸭子类型判断：struct 按结构兼容（字段名+类型+顺序） ---- */

static bool struct_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    return struct_type_compatible(vm, a, b);
}

static bool struct_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
    return struct_type_compatible(vm, sub, sup);
}

/* ===========================================================================
 * vtable 定义
 * =========================================================================== */

const vtable_t VTABLE_STRUCT = {
    .eq = struct_eq, .ne = struct_ne,
    .dispose = struct_dispose,
    .clone = struct_clone,
    .assign = struct_assign,
    .implicit_cast = struct_implicit_cast,
    .explicit_cast = struct_explicit_cast,
    .type_equal = struct_type_equal,
    .type_extends = struct_type_extends,
    .type_seal = type_struct_seal, /* 开放构造路径（PUSH_STRUCT → SEAL）密封入口 */
};

/* ===========================================================================
 * 开放构造（PUSH_STRUCT / DEFINE_FIELD / SEAL）
 * =========================================================================== */

/* dispose_fn：allocator_free 时自动释放字段名 + 字段表 + 嵌入表 + 显示名
   + 运行时成员容器（attributes map + methods vec）
   （open 对象分配时注册，seal 去重回收与 vm_destroy 都只需裸 allocator_free）

   member_scope 是**借用**——生命周期由 module global_scope 子树管理
   （scope_destroy_subtree 自动销毁），此处不释放。 */
static void struct_type_dispose(void *self, allocator_t *allocator) {
    struct_type_t *st = (struct_type_t *)self;
    if (st->fields) {
        for (size_t i = 0; i < st->field_count; i++) {
            if (st->fields[i].name.ptr) {
                char *np = (char *)st->fields[i].name.ptr;
                allocator_free(allocator, (void **)&np);
            }
        }
        allocator_free(allocator, (void **)&st->fields);
    }
    if (st->base.name.ptr) {
        char *np = (char *)st->base.name.ptr;
        allocator_free(allocator, (void **)&np);
    }
    if (st->embeds) allocator_free(allocator, (void **)&st->embeds);
    /* 运行时成员容器：attributes 是借用映射（不拥有 value），只释放 map 本身；
       methods vec 拥有 char* 名字字符串，逐个释放再释放 vec。 */
    if (st->attributes) strmap_free(allocator, &st->attributes);
    if (st->methods) {
        size_t n = vec_len(st->methods);
        for (size_t i = 0; i < n; i++) {
            char *name = (char *)vec_get(st->methods, i);
            if (name) allocator_free(allocator, (void **)&name);
        }
        vec_free(allocator, &st->methods);
    }
    /* member_scope 是借用，不在此释放 */
}

static struct_type_t *struct_type_create_open(vm_t *vm) {
    struct_type_t *st = (struct_type_t *)allocator_new_ex(
        vm->alloc, "struct_type_t", sizeof(struct_type_t), NULL, NULL,
        struct_type_dispose, 1);
    if (!st) panic("vm: out of memory allocating struct type");
    memset(st, 0, sizeof(struct_type_t));
    st->base.vtable = &VTABLE_STRUCT;
    st->base.kind   = TYPE_KIND_STRUCT;
    return st;
}

/* 字段名深拷贝到 vm 堆（本类型拥有；dispose / seal 去重回收都只释放名字）。
 * 字段表的**唯一**名字所有权入口：add_field（开放追加）、
 * insert_embed_fields（seal 前插嵌入字段）、intern（一次性构造）共用，
 * 因此"字段名归类型所有"这条不变量只有一份实现。 */
static strslice_t struct_field_name_dup(vm_t *vm, strslice_t name) {
    char *nbuf = (char *)allocator_new_ex(vm->alloc, "char", sizeof(char), NULL,
                                          NULL, NULL, name.len + 1);
    if (!nbuf) panic("vm: out of memory copying struct field name");
    if (name.len && name.ptr) memcpy(nbuf, name.ptr, name.len);
    nbuf[name.len] = '\0';
    return (strslice_t){ nbuf, name.len };
}

/* 释放字段表里的名字 + 数组本身（**只有本类型拥有的字段表**可以走这里：
 * seal 去重回收开放对象 / 换表前回收旧表）。名字归类型所有，两者必须成对释放，
 * 否则换表/回收会漏掉名字那块内存。 */
static void struct_fields_dispose(vm_t *vm, struct_field_t *fields,
                                  size_t count) {
    if (!fields) return;
    for (size_t i = 0; i < count; i++) {
        if (fields[i].name.ptr) {
            char *np = (char *)fields[i].name.ptr;
            allocator_free(vm->alloc, (void **)&np);
        }
    }
    allocator_free(vm->alloc, (void **)&fields);
}

/* PUSH_STRUCT：分配空 struct_type（fields=NULL，不入池）+ 压其 type value */
const type_t *type_struct_push(vm_t *vm) {
    if (!vm) return NULL;
    struct_type_t *st = struct_type_create_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &st->base));
    return &st->base;
}

/* 分配开放 struct type（fields=NULL，不入池，不压栈）——union member payload
 * 内部构造用（type_union_add_field 向开放 payload struct 追加字段）。
 * 不向操作数栈压 type value；密封由 type_struct_seal 完成。 */
const type_t *type_struct_alloc_open(vm_t *vm) {
    if (!vm) return NULL;
    struct_type_t *st = struct_type_create_open(vm);
    return &st->base;
}

/* DEFINE_FIELD 运行期用：追加字段（名拷贝到 vm 堆；密封后静默忽略）。
 * 开放阶段动态扩容（field_count 递增）。 */
void type_struct_add_field_ex(vm_t *vm, const type_t *t, strslice_t name,
                              const type_t *ftype, ownership_t ownership) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT || type_is_sealed(t)) return;
    struct_type_t *st = (struct_type_t *)t;

    /* 名拷贝（vm 拥有；seal 时整表再拷贝一次，开放期追加名在 seal 后释放） */
    strslice_t nbuf = struct_field_name_dup(vm, name);

    size_t n = st->field_count;
    struct_field_t *nf = (struct_field_t *)allocator_new_ex(
        vm->alloc, "struct_field_t", sizeof(struct_field_t), NULL, NULL, NULL,
        n + 1);
    if (!nf) panic("vm: out of memory adding struct field");
    if (n > 0) memcpy(nf, st->fields, n * sizeof(struct_field_t));
    if (st->fields) allocator_free(vm->alloc, (void **)&st->fields);

    nf[n].name      = nbuf;
    nf[n].offset    = 0;  /* 布局在 seal 时统一计算 */
    nf[n].type      = ftype;
    /* 槽位所有权：类型描述的一部分（销毁序列从 type 对象反推的依据） */
    nf[n].ownership = ownership;
    st->fields = nf;
    st->field_count = n + 1;
}

/* 兼容包装：槽位所有权缺省 OWN_NONE（普通字段）。 */
void type_struct_add_field(vm_t *vm, const type_t *t, strslice_t name,
                           const type_t *ftype) {
    type_struct_add_field_ex(vm, t, name, ftype, OWN_NONE);
}

/* 按 (字段名 + 类型 + 顺序) 判断两结构体是否等价（去重 intern 用）。所有权
 * 已迁移到值层，类型层恒为 OWN_NONE，不再参与比较。 */
static bool struct_same(const struct_type_t *a, const struct_type_t *b) {
    if (a->field_count != b->field_count) return false;
    for (size_t i = 0; i < a->field_count; i++) {
        const struct_field_t *fa = &a->fields[i];
        const struct_field_t *fb = &b->fields[i];
        if (fa->type != fb->type) return false;
        if (fa->name.len != fb->name.len) return false;
        if (fa->name.ptr && fb->name.ptr &&
            memcmp(fa->name.ptr, fb->name.ptr, fa->name.len) != 0) return false;
        if ((fa->name.ptr == NULL) != (fb->name.ptr == NULL)) return false;
    }
    return true;
}

/* 结构兼容（鸭子类型，m2-design §2）：字段名 + 字段类型（type_equal，嵌套
 * 复合类型递归）+ 字段顺序完全一致即兼容。布局（offset/size/align）由字段
 * 表唯一决定（C 对齐规则），字段兼容 ⟹ size/align 相同——无需单独比较。
 * 匿名构造 .{...} 推断的匿名类型与目标类型也走此判断。 */
bool struct_type_compatible(vm_t *vm, const type_t *a, const type_t *b) {
    if (!a || !b || a->kind != TYPE_KIND_STRUCT ||
        b->kind != TYPE_KIND_STRUCT)
        return false;
    if (a == b) return true;
    const struct_type_t *sa = (const struct_type_t *)a;
    const struct_type_t *sb = (const struct_type_t *)b;
    if (sa->field_count != sb->field_count) return false;
    for (size_t i = 0; i < sa->field_count; i++) {
        const struct_field_t *fa = &sa->fields[i];
        const struct_field_t *fb = &sb->fields[i];
        if (fa->name.len != fb->name.len) return false;
        if (fa->name.ptr && fb->name.ptr &&
            memcmp(fa->name.ptr, fb->name.ptr, fa->name.len) != 0) return false;
        if ((fa->name.ptr == NULL) != (fb->name.ptr == NULL)) return false;
        /* **槽位所有权参与去重**：`{ r: ref *i32 }` 与 `{ r: own *i32 }`
           布局相同但销毁语义不同（前者不释放、后者释放），必须是不同类型。 */
        if (fa->ownership != fb->ownership) return false;
        if (!type_equal(vm, fa->type, fb->type)) return false;
    }
    return true;
}

/* 结构体指针**向上转换**的字段兼容判定：target（小/基）的字段是否构成
 * source（大/派生）字段表的**前缀**（名字 + 类型逐一相等）。
 *
 * 语义：`大结构体* → 小结构体*` 只有在"小结构体的全部字段恰好是大结构体的前
 * 几个字段"时才安全——C 布局自左向右分配，前缀字段的偏移与类型完全一致，因此
 * 同一个地址按小结构体解释是对的（即 C 的"公共起始序列"规则）。
 * 反向（小 → 大，向下转换）不成立：大结构体多出来的字段没有存储。
 *
 * 只用于**指针**转换；值转换仍要求类型完全相同（struct_type_compatible），
 * 否则会静默丢掉尾部字段（切片）。 */
bool struct_type_prefix_compatible(vm_t *vm, const type_t *target,
                                   const type_t *source) {
    if (!target || !source || target->kind != TYPE_KIND_STRUCT ||
        source->kind != TYPE_KIND_STRUCT)
        return false;
    if (target == source) return true;
    const struct_type_t *st = (const struct_type_t *)target;
    const struct_type_t *ss = (const struct_type_t *)source;
    /* 目标（小）字段数必须 ≤ 源（大）字段数 */
    if (st->field_count > ss->field_count) return false;
    for (size_t i = 0; i < st->field_count; i++) {
        const struct_field_t *ft = &st->fields[i];
        const struct_field_t *fs = &ss->fields[i];
        if (ft->name.len != fs->name.len) return false;
        if (ft->name.ptr && fs->name.ptr &&
            memcmp(ft->name.ptr, fs->name.ptr, ft->name.len) != 0)
            return false;
        if ((ft->name.ptr == NULL) != (fs->name.ptr == NULL)) return false;
        if (!type_equal(vm, ft->type, fs->type)) return false;
    }
    return true;
}

/* C 对齐规则向上取整 */
static size_t align_up(size_t v, size_t a) {
    if (a <= 1) return v;
    return (v + a - 1) / a * a;
}

/* 按 C 对齐规则就地计算字段表布局 + 结构体 size/align（offset_0 = 0；
 * offset_i = align_up(prev_end, align_i)；size = align_up(last_end, max_align)；
 * align = max(字段 align)；空 struct：C 语义 size=1）。
 * SEAL 与"seal 前组合好的字段表"共用，布局规则**只此一份**（组合不再有
 * "seal 之后重算布局"这条路径）。 */
static void struct_layout_apply(struct_type_t *st) {
    size_t off = 0, max_align = 1;
    for (size_t i = 0; i < st->field_count; i++) {
        struct_field_t *f = &st->fields[i];
        size_t fa = f->type && f->type->align > 0 ? f->type->align : 1;
        off = align_up(off, fa);
        f->offset = off;
        off += f->type ? f->type->size : 0;
        if (fa > max_align) max_align = fa;
    }
    st->base.size  = align_up(off, max_align);
    if (st->field_count == 0) st->base.size = 1; /* 空 struct：C 语义 1 字节 */
    st->base.align = max_align;
}

/* SEAL：拷贝字段表 + C 对齐布局 + 按 (字段名+类型+顺序) 去重 intern + 置
 * sealed。返回密封后的 const type（可能 != self）。
 *
 * 嵌入表（st->embeds）**原样保留**：它是 seal 前（组合窗口）设好的，与字段表
 * 一起构成 sealed 类型的一次性快照；seal 之后两者都不可变（写入 API 拒绝
 * sealed 类型）。**具名类型（unique 标记）不参与字段表去重**（见下文
 * struct_type_t::unique）：字段表相同但成员命名空间/嵌入表不同的两个具名
 * struct 是**不同类型**。
 *
 * **seal 是分界线**：调用本函数之后该类型对象的字段表/嵌入表/布局全部冻结。 */
const type_t *type_struct_seal(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT) return NULL;
    if (type_is_sealed(t)) return t; /* 幂等 */

    struct_type_t *st = (struct_type_t *)t;

    /* 防御：字段类型必须已解析（NULL 会产生 size=0 的 struct，后续
       value_alloc_data 返回 NULL 崩溃）。非法输入拒绝密封。 */
    for (size_t i = 0; i < st->field_count; i++) {
        if (!st->fields[i].type) return NULL;
    }

    if (!vm->struct_types) vm->struct_types = vec_new(vm->alloc, /*owns_element=*/false);

    /* 去重 intern：命中已有 sealed 类型则复用并手工回收本开放类型
       （allocator_free 自动调 dispose_fn 释放字段表）。
       **具名类型（unique）不参与去重**：命名的 `struct A` / `struct B` 即使
       字段表逐位相同，也是两个不同的具名类型（独立成员命名空间与嵌入表）；
       合并会让显示名互相覆盖、并让结构体组合的嵌入表退化成 [X, X]（成员继承
       查找按类型去重后歧义/覆盖判定失效）。普通匿名/内部构造的类型保持既有
       dedup 语义（见 include/vm/type.h 的 struct_type_t::unique 说明）。 */
    bool skip_dedup = st->unique;
    size_t n = vec_len(vm->struct_types);
    for (size_t i = 0; !skip_dedup && i < n; i++) {
        const struct_type_t *other = (const struct_type_t *)vec_get(vm->struct_types, i);
        if (other && other != st && type_is_sealed(&other->base) &&
            !other->unique && struct_same(other, st)) {
            allocator_free(vm->alloc, (void **)&st);
            return &other->base;
        }
    }

    /* 计算 C 对齐布局（seal 前已组合好的字段表在这里一次性定型） */
    struct_layout_apply(st);
    st->base.sealed = true;

    /* 递归类型的"布局延迟定型"：本类型 seal 之前，以它为布局来源的派生包装
       （const/volatile：size/align = sub；数组：N * elem.size）已经按"当时还
       没定型的 sub"拷贝过了 size/align（例：`struct Node { next: ref *const
       Node; }` 里的 `const Node`）。这里在 sub 定稿后把这些派生包装就地同步到
       最终布局（链式到不动点）。编译期（sema 物化路径）与运行期（hoist 构造
       → op_seal）都经过本函数，因此两侧一起修好。 */
    type_sync_derived_layout(vm, &st->base);

    /* 入池（去重 intern 用）。**物化的开放对象**（sema 递归类型：先建对象、
       字段后填）在分配时就已入池——那里是为了让"永不 seal"的错误路径也能被
       vm_destroy 回收（开放对象不入池就是无人拥有的裸分配）。此处避免重复入池
       （vm_destroy 逐个裸释放，重复项会双重释放）。 */
    bool in_pool = false;
    for (size_t i = 0; i < vec_len(vm->struct_types); i++) {
        if (vec_get(vm->struct_types, i) == st) { in_pool = true; break; }
    }
    if (!in_pool) vec_push(vm->struct_types, vm->alloc, st);
    return &st->base;
}

/* ===========================================================================
 * 结构体组合（`...Base;`）——seal **之前**（开放窗口）的字段/嵌入表编辑
 *
 * 不变量（见 type_struct.h 的 SEAL 注释）：类型在 seal 前可编辑，seal 后不可变。
 * 组合因此**不再**事后改写已 sealed 的类型，而是在 sema 的开放构造窗口里完成：
 *   开放对象 → 追加自有字段 → 按位置插入各 `...Base` 的字段表 → 设嵌入表 →
 *   seal（一次）。
 * 这样：
 *   - 布局只在 seal 里算一次（struct_layout_apply），不存在"seal 后再改布局"；
 *   - `type_t*` 身份不变：同一个开放对象就地 seal（seal 去重命中时返回既有
 *     类型对象，与仓库其它开放构造路径完全一致——sema 用返回值做符号/登记）；
 *   - 两个写入 API 在 sealed 类型上**拒绝**（返回 false，类型一字不改），把
 *     不变量固化在代码里：任何"seal 后改类型"的路径都会立刻失败，而不是悄悄
 *     生效。
 * =========================================================================== */

bool type_struct_insert_embed_fields(vm_t *vm, const type_t *t,
                                     const struct_embed_fields_t *embeds,
                                     size_t embed_count) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT) return false;
    if (embed_count > 0 && !embeds) return false;
    /* 不变量：只有 seal 前的开放类型可以编辑字段表。sealed 类型**拒绝**（返回
       false，类型一字不改）——组合必须走 seal 前窗口，没有"事后改写"这条路径。
       与 type_struct_add_field 的 sealed 处理一致：拒绝时静默返回，由调用方
       （sema 组合入口）报明确诊断。 */
    if (type_is_sealed(t)) return false;

    struct_type_t *st = (struct_type_t *)t;
    size_t own = st->field_count;
    size_t total = own;
    for (size_t e = 0; e < embed_count; e++) total += embeds[e].field_count;

    if (total > 0) {
        struct_field_t *nf = (struct_field_t *)allocator_new_ex(
            vm->alloc, "struct_field_t", sizeof(struct_field_t), NULL, NULL,
            NULL, total);

        /* 合并（单趟两指针，**不做原地 memmove**）：按"自有字段之间插入嵌入
           字段块"的顺序直接铺出最终表——
             对每个自有字段 fi（fi = 0..own）：
               先按 `...` 声明序放入所有 layer == fi 的嵌入字段块，
               再放入第 fi 个自有字段（fi == own 时无）。
           字段名**深拷贝**（名归本类型所有，与 add_field / intern 同一所有权
           语义），字段类型引用记录；offset 留给 seal 统一布局。 */
        size_t out = 0;
        for (size_t fi = 0; fi <= own; fi++) {
            for (size_t e = 0; e < embed_count; e++) {
                const struct_embed_fields_t *sp = &embeds[e];
                if (sp->layer != fi) continue;
                for (size_t k = 0; k < sp->field_count; k++) {
                    nf[out].name   = struct_field_name_dup(vm, sp->fields[k].name);
                    nf[out].offset = 0;
                    nf[out].type   = sp->fields[k].type;
                    out++;
                }
            }
            if (fi < own) {
                nf[out].name   = struct_field_name_dup(vm, st->fields[fi].name);
                nf[out].offset = 0;
                nf[out].type   = st->fields[fi].type;
                out++;
            }
        }

        /* 旧字段表整体回收（**名字归本类型所有**，必须连名字一起释放）：新表
           里的名字都是上面深拷贝出来的，旧表的名字没有别处持有——只回收数组
           会漏掉名字那块内存。 */
        struct_fields_dispose(vm, st->fields, st->field_count);
        st->fields      = nf;
        st->field_count = total;
    }
    return true;
}

bool type_struct_set_embeds(vm_t *vm, const type_t *t,
                            const type_t *const *embeds, size_t embed_count) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT) return false;
    if (embed_count > 0 && !embeds) return false;
    /* 不变量：嵌入表同样只能在 seal 前设置（sealed → 拒绝，不改动类型） */
    if (type_is_sealed(t)) return false;

    struct_type_t *st = (struct_type_t *)t;
    if (st->embeds) allocator_free(vm->alloc, (void **)&st->embeds);
    st->embeds = NULL;
    st->embed_count = 0;
    if (embed_count == 0) return true;

    const type_t **copy = (const type_t **)allocator_new_ex(
        vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL, embed_count);
    if (!copy) panic("vm: out of memory recording struct embeds");
    for (size_t i = 0; i < embed_count; i++) copy[i] = embeds[i];
    st->embeds = copy;
    st->embed_count = embed_count;
    return true;
}

bool type_struct_add_embed(vm_t *vm, const type_t *t, const type_t *embed) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT || !embed) return false;
    if (type_is_sealed(t)) return false;
    struct_type_t *st = (struct_type_t *)t;
    const type_t **copy = (const type_t **)allocator_new_ex(
        vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL,
        st->embed_count + 1);
    if (!copy) panic("vm: out of memory adding struct embed");
    for (size_t i = 0; i < st->embed_count; i++) copy[i] = st->embeds[i];
    copy[st->embed_count] = embed;
    if (st->embeds) allocator_free(vm->alloc, (void **)&st->embeds);
    st->embeds = copy;
    st->embed_count++;
    return true;
}

const type_t *const *type_struct_embeds(const type_t *t, size_t *count) {
    if (count) *count = 0;
    if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
    const struct_type_t *st = (const struct_type_t *)t;
    if (count) *count = st->embed_count;
    return st->embeds;
}

bool type_struct_mark_unique(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT) return false;
    /* 与其它类型属性写入 API 同一不变量：seal 之后类型不可变 → 拒绝 */
    if (type_is_sealed(t)) return false;
    ((struct_type_t *)t)->unique = true;
    return true;
}

bool type_struct_is_unique(const type_t *t) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return false;
    return ((const struct_type_t *)t)->unique;
}

/* ===========================================================================
 * intern
 * =========================================================================== */

const type_t *type_struct_intern(vm_t *vm, const struct_field_t *fields,
                                 size_t count) {
    /* 一次性快捷（开放构造 + 立即密封）：供 sema/C 侧直接使用。不向操作数栈
     * 压入 type value。去重 intern 由 type_struct_seal 完成。 */
    if (!vm) return NULL;
    struct_type_t *st = struct_type_create_open(vm);

    /* 拷贝字段表（vm 拥有；名深拷贝） */
    if (count > 0) {
        struct_field_t *copy = (struct_field_t *)allocator_new_ex(
            vm->alloc, "struct_field_t", sizeof(struct_field_t), NULL, NULL,
            NULL, count);
        if (!copy) panic("vm: out of memory interning struct type");
        for (size_t i = 0; i < count; i++) {
            copy[i].name   = struct_field_name_dup(vm, fields[i].name);
            copy[i].offset = 0; /* seal 统一计算 */
            copy[i].type   = fields[i].type;
        }
        st->fields = copy;
        st->field_count = count;
    }
    const type_t *t = type_struct_seal(vm, &st->base);
    if (!t) allocator_free(vm->alloc, (void **)&st); /* 密封失败（如字段类型
                                                       未解析）：回收开放对象 */
    return t;
}

/* 带指定 ownership 的 struct intern：复制 base 的字段表 + 嵌入表，密封为
 * 新的 struct_type_t（与 base 同布局）。所有权已迁移到值层，类型层恒为
 * OWN_NONE；保留 ownership 参数仅为兼容既有调用方签名，不再写入类型。
 *
 * **布局与 base 完全相同**（size/align/字段偏移逐字相同）。
 *
 * 显示名**不在此设定**：本对象由 struct_type_create_open 创建，其 dispose 会
 * 释放 base.name，而 base 的名字可能借用外部存储（编译期 arena）。调用方
 * （sema）在密封后用 type_set_name 设一个 vm 自有的名——与具名 struct 的
 * 既有做法一致（见 struct_compose.c）。 */
const type_t *type_struct_intern_with_ownership(vm_t *vm, const type_t *base,
                                                ownership_t ownership) {
    if (!vm || !base || base->kind != TYPE_KIND_STRUCT) return NULL;
    (void)ownership;  /* 所有权迁移到值层，类型不再携带 ownership */
    const struct_type_t *src = (const struct_type_t *)base;
    struct_type_t *st = struct_type_create_open(vm);
    /* 拷贝字段表（vm 拥有；名深拷贝；offset 拷贝——布局与 base 相同） */
    if (src->field_count > 0) {
        struct_field_t *copy = (struct_field_t *)allocator_new_ex(
            vm->alloc, "struct_field_t", sizeof(struct_field_t), NULL, NULL,
            NULL, src->field_count);
        if (!copy) panic("vm: out of memory interning struct type (ownership)");
        for (size_t i = 0; i < src->field_count; i++) {
            copy[i].name   = struct_field_name_dup(vm, src->fields[i].name);
            copy[i].offset = src->fields[i].offset;
            copy[i].type   = src->fields[i].type;
        }
        st->fields = copy;
        st->field_count = src->field_count;
    }
    /* 拷贝嵌入表（组合 struct 的 embeds——与字段表同构快照） */
    if (src->embed_count > 0) {
        const type_t **ecopy = (const type_t **)allocator_new_ex(
            vm->alloc, "type_t*", sizeof(const type_t *), NULL, NULL, NULL,
            src->embed_count);
        if (!ecopy) panic("vm: out of memory interning struct embeds (ownership)");
        for (size_t i = 0; i < src->embed_count; i++)
            ecopy[i] = src->embeds[i];
        st->embeds = ecopy;
        st->embed_count = src->embed_count;
    }
    /* unique 跟随 base：具名 struct 的 fatal 副本仍保持独立身份 */
    st->unique = src->unique;
    /* 运行时成员共享 base 的容器（借用——不拷贝、不拥有）：
       fatal 变体与 base 是同一类型的不同所有权视图，成员（方法/静态属性）
       是同一份。dispose 只在 base 对象上释放（fatal 副本的 dispose 时这些
       字段是借用，不会重复释放——strmap_free/vec_free 对 NULL 安全，
       但这里它们非 NULL 且是借用的，所以 dispose 中需要跳过）。
       为避免 fatal 副本 dispose 时误释放 base 的容器，fatal 副本不拷贝
       这三个指针——成员访问走 base 类型（type_strip_quals 去掉 fatal 后
       得到 base，sema/VM 成员查找都先 strip 限定符）。 */
    /* st->member_scope = NULL; st->attributes = NULL; st->methods = NULL;
       — memset 已清零，保持 NULL：成员访问经 type_strip_quals 走 base 类型 */
    const type_t *t = type_struct_seal(vm, &st->base);
    if (!t) allocator_free(vm->alloc, (void **)&st);
    return t;
}

/* ===========================================================================
 * 字段查找
 * =========================================================================== */

int struct_type_find_field(const type_t *t, strslice_t name) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return -1;
    const struct_type_t *st = (const struct_type_t *)t;
    for (size_t i = 0; i < st->field_count; i++) {
        const struct_field_t *f = &st->fields[i];
        if (f->name.len == name.len && f->name.ptr &&
            memcmp(f->name.ptr, name.ptr, name.len) == 0)
            return (int)i;
    }
    return -1;
}

/* ===========================================================================
 * 运行时成员管理（静态变量 + 方法）
 *
 * struct_type 持有自己的成员生命周期（非语法糖）。attributes 是 name→value_t*
 * 借用映射（不拥有 value——value 归 member_scope 的 owned 向量），methods 是
 * 方法名白名单（char* owned——只记录"这个名字是方法"，func value 仍在 attributes）。
 * member_scope 是借用（生命周期由 module global_scope 子树管理）。
 * =========================================================================== */

bool type_struct_members_init(vm_t *vm, const type_t *t) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT) return false;
    struct_type_t *st = (struct_type_t *)t;
    if (st->attributes) return true; /* 幂等 */
    /* attributes: 借用映射（owns_value=false——value 归 member_scope） */
    st->attributes = strmap_new(vm->alloc, /*owns_value=*/false);
    if (!st->attributes) panic("vm: out of memory allocating struct attributes");
    /* methods: 不 owns_element（名字字符串由我们手工 allocator_new + 手工释放，
       vec_free 不会调 dispose_fn——但我们用 owns_element=false 自己逐个释放） */
    st->methods = vec_new(vm->alloc, /*owns_element=*/false);
    if (!st->methods) panic("vm: out of memory allocating struct methods vec");
    return true;
}

bool type_struct_set_member_scope(vm_t *vm, const type_t *t, scope_t *scope) {
    (void)vm;
    if (!t || t->kind != TYPE_KIND_STRUCT) return false;
    struct_type_t *st = (struct_type_t *)t;
    st->member_scope = scope; /* 借用，不拥有 */
    return true;
}

scope_t *type_struct_member_scope(const type_t *t) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
    return ((const struct_type_t *)t)->member_scope;
}

bool type_struct_add_attribute(vm_t *vm, const type_t *t,
                               const char *name, value_t *value) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT || !name || !value) return false;
    struct_type_t *st = (struct_type_t *)t;
    if (!st->attributes) return false; /* 未初始化成员容器 */
    /* 已有同名 → 不覆盖（调用方负责报诊断） */
    if (strmap_contains(st->attributes, name)) return false;
    /* strmap_insert 会拷贝 key（name → NUL 终止副本，map 拥有）；value 借用 */
    strmap_insert(st->attributes, vm->alloc, name, (void *)value);
    return true;
}

value_t *type_struct_get_attribute(const type_t *t, const char *name) {
    if (!t || t->kind != TYPE_KIND_STRUCT || !name) return NULL;
    const struct_type_t *st = (const struct_type_t *)t;
    if (!st->attributes) return NULL;
    return (value_t *)strmap_get(st->attributes, name);
}

bool type_struct_add_method(vm_t *vm, const type_t *t, const char *name) {
    if (!vm || !t || t->kind != TYPE_KIND_STRUCT || !name) return false;
    struct_type_t *st = (struct_type_t *)t;
    if (!st->methods) return false; /* 未初始化成员容器 */
    /* 去重：已有同名方法名 → 不覆盖 */
    size_t n = vec_len(st->methods);
    for (size_t i = 0; i < n; i++) {
        const char *existing = (const char *)vec_get(st->methods, i);
        if (existing && strcmp(existing, name) == 0) return false;
    }
    /* NUL 终止拷贝（methods vec 拥有名字字符串，dispose 时逐个释放） */
    size_t len = strlen(name);
    char *copy = (char *)allocator_new_ex(vm->alloc, "char", sizeof(char), NULL,
                                          NULL, NULL, len + 1);
    if (!copy) panic("vm: out of memory copying method name");
    memcpy(copy, name, len);
    copy[len] = '\0';
    vec_push(st->methods, vm->alloc, (void *)copy);
    return true;
}

bool type_struct_is_method(const type_t *t, const char *name) {
    if (!t || t->kind != TYPE_KIND_STRUCT || !name) return false;
    const struct_type_t *st = (const struct_type_t *)t;
    if (!st->methods) return false;
    size_t n = vec_len(st->methods);
    for (size_t i = 0; i < n; i++) {
        const char *m = (const char *)vec_get(st->methods, i);
        if (m && strcmp(m, name) == 0) return true;
    }
    return false;
}

vec_t *type_struct_methods(const type_t *t) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
    return ((const struct_type_t *)t)->methods;
}

strmap_t *type_struct_attributes(const type_t *t) {
    if (!t || t->kind != TYPE_KIND_STRUCT) return NULL;
    return ((const struct_type_t *)t)->attributes;
}
