#include "vm/type_cfunc.h"

#include "core/arena.h"  /* ALIGNOF */
#include "core/panic.h"
#include "core/string.h"
#include "vm/type_func.h"
#include "vm/type_opaque.h"
#include "vm/vm.h"

#include <string.h>

/* ===========================================================================
 * cfunc 类型（C 函数指针，TYPE_KIND_CFUNC）
 *
 * - 布局：size/align = 指针宽；data = 裸代码指针。
 * - 运算：复用 VTABLE_OPAQUE（平凡拷贝、no-op dispose、指针相等）。
 * - 身份：按 (kind, 签名) intern；与 `func(...)` 类型**永不相等**。
 * =========================================================================== */

static char *cfunc_sig_name(allocator_t *alloc, const func_sig_t *sig) {
    string_t *s = string_new(alloc);
    if (!s) panic("vm: out of memory building cfunc type name");
    string_append_cstr(s, "cfunc(");
    if (sig->is_variadic && sig->param_count == 0) {
        string_append_cstr(s, "...");
    } else {
        for (size_t i = 0; i < sig->param_count; i++) {
            if (i > 0) string_append_cstr(s, ", ");
            const type_t *p = sig->params ? sig->params[i] : NULL;
            if (p && p->name.ptr)
                string_append_bytes(s, p->name.ptr, p->name.len);
            else
                string_append_cstr(s, "?");
        }
        if (sig->is_variadic) string_append_cstr(s, ", ...");
    }
    string_append_cstr(s, "): ");
    if (sig->return_type && sig->return_type->name.ptr)
        string_append_bytes(s, sig->return_type->name.ptr,
                            sig->return_type->name.len);
    else
        string_append_cstr(s, "void");
    char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                                 string_len(s) + 1);
    if (buf) memcpy(buf, string_cstr(s), string_len(s) + 1);
    string_free((string_t **)&s);
    return buf;
}

const func_sig_t *cfunc_type_sig(const type_t *t) {
    if (!type_is_cfunc(t)) return NULL;
    return &((const cfunc_type_t *)t)->sig;
}

const type_t *type_cfunc_sig(vm_t *vm, const type_t *const *params,
                             size_t param_count, const type_t *ret,
                             bool is_variadic) {
    if (!vm) return NULL;
    if (!vm->sig_types) vm->sig_types = vec_new(vm->alloc, false);

    /* 去重：按 (cfunc, 签名) 命中即复用。 */
    size_t n = vec_len(vm->sig_types);
    for (size_t i = 0; i < n; i++) {
        const type_t *other = (const type_t *)vec_get(vm->sig_types, i);
        if (!type_is_cfunc(other)) continue;
        const func_sig_t *os = &((const cfunc_type_t *)other)->sig;
        if (os->param_count != param_count) continue;
        if (os->is_variadic != is_variadic) continue;
        if (os->return_type != ret) continue;
        bool same = true;
        for (size_t k = 0; k < param_count && same; k++) {
            const type_t *a = os->params ? os->params[k] : NULL;
            const type_t *b = params ? params[k] : NULL;
            if (a != b) same = false;
        }
        if (same) return other;
    }

    cfunc_type_t *ct = (cfunc_type_t *)allocator_new_ex(
        vm->alloc, "cfunc_type_t", sizeof(cfunc_type_t), NULL, NULL, NULL, 1);
    if (!ct) panic("vm: out of memory allocating cfunc type");
    memset(ct, 0, sizeof(cfunc_type_t));
    ct->base.vtable = &VTABLE_OPAQUE; /* 指针语义（平凡拷贝/指针相等） */
    ct->base.kind = TYPE_KIND_CFUNC;
    ct->base.size = sizeof(void *);
    ct->base.align = ALIGNOF(void *);
    ct->base.sealed = true;
    ct->sig.return_type = ret;
    ct->sig.is_variadic = is_variadic;
    if (param_count > 0 && params) {
        const type_t **pcopy = (const type_t **)allocator_new_ex(
            vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL,
            param_count);
        if (!pcopy) panic("vm: out of memory allocating cfunc params");
        memcpy(pcopy, params, param_count * sizeof(type_t *));
        ct->sig.params = pcopy;
        ct->sig.param_count = param_count;
    }
    char *name = cfunc_sig_name(vm->alloc, &ct->sig);
    if (!name) panic("vm: out of memory building cfunc type name");
    ct->base.name = (strslice_t){ name, strlen(name) };
    vec_push(vm->sig_types, vm->alloc, ct);
    return &ct->base;
}

/* ===========================================================================
 * 运行期构造协议（PUSH_CFUNC_TYPE / FUNC_TYPE_PARAM|RETURN|VARARG / SEAL）
 *
 * 与 func 类型同构：先 push 一个**开放**类型（未入池），逐个追加参数/返回，
 * 最后 seal（按签名去重 intern）。开放类型的 vtable 用 VTABLE_CFUNC（= opaque
 * 的指针语义 + type_seal 槽位），这样 op_seal 走通用的 vtable 分发即可。
 * =========================================================================== */

static value_t *cfunc_clone(vm_t *vm, value_t *v);
static value_t *cfunc_assign(vm_t *vm, value_t *dst, value_t *src);
static value_t *cfunc_eq(vm_t *vm, value_t *a, value_t *b);
static value_t *cfunc_ne(vm_t *vm, value_t *a, value_t *b);
static void cfunc_dispose(vm_t *vm, value_t *v);
static value_t *cfunc_implicit_cast(vm_t *vm, value_t *v, const type_t *to);
static value_t *cfunc_explicit_cast(vm_t *vm, value_t *v, const type_t *to);
static bool cfunc_type_equal(vm_t *vm, const type_t *a, const type_t *b);
static bool cfunc_type_extends(vm_t *vm, const type_t *sub, const type_t *super);

/* cfunc 值 = 裸代码指针：比较/拷贝复用 opaque 语义（这里直接给出最小的
   eq/ne/clone/assign；其它槽位 NULL = 不支持，运行时触发 panic）。 */
const vtable_t VTABLE_CFUNC = {
    .eq            = cfunc_eq,
    .ne            = cfunc_ne,
    .dispose       = cfunc_dispose,
    .clone         = cfunc_clone,
    .assign        = cfunc_assign,
    .implicit_cast = cfunc_implicit_cast,
    .explicit_cast = cfunc_explicit_cast,
    .type_equal    = cfunc_type_equal,
    .type_extends  = cfunc_type_extends,
    .type_seal     = cfunc_type_seal,
};

static value_t *cfunc_clone(vm_t *vm, value_t *v) {
    if (value_is_shadow(v)) return value_make_shadow(vm, value_type(v));
    const type_t *t = value_type(v);
    void *data = value_alloc_data(vm->alloc, t);
    if (!data) return NULL;
    memcpy(data, value_data(v), t->size);
    return value_make(vm, t, data);
}

static value_t *cfunc_assign(vm_t *vm, value_t *dst, value_t *src) {
    /* 类型检查**先于** shadow 短路：sema 的初始化/赋值校验用 shadow 值调
       value_assign，短路掉就永远查不出 func(...) → cfunc(...) 这类错误
       （用户要求：两者互不互通）。 */
    const type_t *dt = value_type(dst);
    const type_t *st = value_type(src);
    if (st != dt)
        return value_make_error(vm, "cfunc: cannot assign a value of another type");
    if (value_is_shadow(dst) || value_is_shadow(src)) return dst;
    memcpy(value_data(dst), value_data(src), dt->size);
    return dst;
}

/* ---- 最小运算集（渲染/比较/拷贝；指针语义与 opaque 同） ---- */
static value_t *cfunc_eq(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *t = value_type(a);
    if (value_type(b) != t)
        return value_make_error(vm, "cfunc: == requires the same cfunc type");
    bool eq = memcmp(value_data(a), value_data(b), t->size) == 0;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &eq, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}
static value_t *cfunc_ne(vm_t *vm, value_t *a, value_t *b) {
    if (value_is_shadow(a) || value_is_shadow(b))
        return value_make_shadow(vm, vm->type_bool);
    const type_t *t = value_type(a);
    if (value_type(b) != t)
        return value_make_error(vm, "cfunc: != requires the same cfunc type");
    bool ne = memcmp(value_data(a), value_data(b), t->size) != 0;
    void *data = value_alloc_data(vm->alloc, vm->type_bool);
    memcpy(data, &ne, sizeof(bool));
    return value_make(vm, vm->type_bool, data);
}
static void cfunc_dispose(vm_t *vm, value_t *v) {
    (void)vm; (void)v; /* 裸代码指针：无资源可释放 */
}
static value_t *cfunc_implicit_cast(vm_t *vm, value_t *v, const type_t *to) {
    /* cfunc → opaque（C 的 void*）；其它交给显式 as。 */
    if (value_is_shadow(v)) return value_make_shadow(vm, to);
    if (to == vm->type_opaque || value_type(v) == to) {
        void *data = value_alloc_data_copy(vm->alloc, to, value_data(v));
        if (!data) return NULL;
        return value_make(vm, to, data);
    }
    return value_make_error(vm, "cfunc: unsupported implicit cast");
}
static value_t *cfunc_explicit_cast(vm_t *vm, value_t *v, const type_t *to) {
    if (value_is_shadow(v)) return value_make_shadow(vm, to);
    if (!to || to->size != sizeof(void *))
        return value_make_error(vm, "cfunc: cannot cast to this type");
    void *data = value_alloc_data_copy(vm->alloc, to, value_data(v));
    if (!data) return NULL;
    return value_make(vm, to, data);
}
static bool cfunc_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
    (void)vm;
    return a == b; /* 按签名 intern → 身份比较 */
}
static bool cfunc_type_extends(vm_t *vm, const type_t *sub, const type_t *super) {
    (void)vm;
    return sub == super;
}
static cfunc_type_t *cfunc_open(vm_t *vm) {
    cfunc_type_t *ct = (cfunc_type_t *)allocator_new_ex(
        vm->alloc, "cfunc_type_t", sizeof(cfunc_type_t), NULL, NULL, NULL, 1);
    if (!ct) panic("vm: out of memory allocating cfunc type");
    memset(ct, 0, sizeof(cfunc_type_t));
    ct->base.vtable = &VTABLE_CFUNC;
    ct->base.kind = TYPE_KIND_CFUNC;
    ct->base.size = sizeof(void *);
    ct->base.align = ALIGNOF(void *);
    return ct;
}

void cfunc_type_push(vm_t *vm) {
    if (!vm) return;
    cfunc_type_t *ct = cfunc_open(vm);
    vec_push(vm->stack, vm->alloc, type_as_value(vm, &ct->base));
}

void cfunc_type_add_param(vm_t *vm, const type_t *t, const type_t *param) {
    if (!vm || !type_is_cfunc(t) || type_is_sealed(t)) return;
    cfunc_type_t *ct = (cfunc_type_t *)t;
    const type_t **pcopy = (const type_t **)allocator_new_ex(
        vm->alloc, "type_t*", sizeof(type_t *), NULL, NULL, NULL,
        ct->sig.param_count + 1);
    if (!pcopy) panic("vm: out of memory appending cfunc param");
    if (ct->sig.param_count > 0)
        memcpy(pcopy, ct->sig.params, ct->sig.param_count * sizeof(type_t *));
    pcopy[ct->sig.param_count] = param;
    if (ct->sig.params) allocator_free(vm->alloc, (void **)&ct->sig.params);
    ct->sig.params = pcopy;
    ct->sig.param_count++;
}

void cfunc_type_set_return(vm_t *vm, const type_t *t, const type_t *ret) {
    (void)vm;
    if (!type_is_cfunc(t) || type_is_sealed(t)) return;
    ((cfunc_type_t *)t)->sig.return_type = ret;
}

void cfunc_type_set_variadic(vm_t *vm, const type_t *t, bool variadic) {
    (void)vm;
    if (!type_is_cfunc(t) || type_is_sealed(t)) return;
    ((cfunc_type_t *)t)->sig.is_variadic = variadic;
}

const type_t *cfunc_type_seal(vm_t *vm, const type_t *t) {
    if (!vm || !type_is_cfunc(t)) return NULL;
    if (type_is_sealed(t)) return t;
    cfunc_type_t *ct = (cfunc_type_t *)t;
    if (!vm->sig_types) vm->sig_types = vec_new(vm->alloc, false);

    /* 去重：命中已密封的同签名 cfunc → 复用并回收本开放类型。 */
    size_t n = vec_len(vm->sig_types);
    for (size_t i = 0; i < n; i++) {
        const type_t *other = (const type_t *)vec_get(vm->sig_types, i);
        if (other == t || !type_is_cfunc(other) || !type_is_sealed(other))
            continue;
        const func_sig_t *os = &((const cfunc_type_t *)other)->sig;
        if (os->param_count != ct->sig.param_count) continue;
        if (os->is_variadic != ct->sig.is_variadic) continue;
        if (os->return_type != ct->sig.return_type) continue;
        bool same = true;
        for (size_t k = 0; k < ct->sig.param_count && same; k++)
            if (os->params[k] != ct->sig.params[k]) same = false;
        if (!same) continue;
        if (ct->sig.params) allocator_free(vm->alloc, (void **)&ct->sig.params);
        allocator_free(vm->alloc, (void **)&ct);
        return other;
    }

    char *name = cfunc_sig_name(vm->alloc, &ct->sig);
    if (!name) panic("vm: out of memory building cfunc type name");
    ct->base.name = (strslice_t){ name, strlen(name) };
    ct->base.sealed = true;
    vec_push(vm->sig_types, vm->alloc, ct);
    return &ct->base;
}
