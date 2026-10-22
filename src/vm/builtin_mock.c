#include "vm/vm.h"
#include "vm/function.h"
#include "vm/value.h"
#include "vm/scope.h"
#include "vm/type.h"
#include "vm/type_func.h"
#include "vm/type_struct.h"
#include "vm/type_ownership.h"
#include "core/vec.h"
#include "core/allocator.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    CLUX_MOCK_FREE = 0,
    CLUX_MOCK_METHOD,
} clux_mock_kind_t;

typedef struct {
    clux_mock_kind_t kind;
    char            *name;
    const type_t    *type;     /* method mock 的 struct 类型；free 为 NULL */
    value_t         *slot;     /* 被替换的绑定（借用） */
    value_t         *original; /* 原值 clone（registry 拥有） */
} clux_mock_entry_t;

static const char *arg_str(value_t *v) {
    if (!v || !value_type(v) || type_kind_of(value_type(v)) != TYPE_KIND_STR ||
        !value_data(v))
        return NULL;
    return *(const char *const *)value_data(v);
}

static bool ensure_entries(vm_t *vm) {
    if (vm->mock_entries) return true;
    vm->mock_entries = vec_new(vm->alloc, /*owns_element=*/false);
    return vm->mock_entries != NULL;
}

static void entry_free(vm_t *vm, clux_mock_entry_t *e) {
    if (!e) return;
    if (e->name) allocator_free(vm->alloc, (void **)&e->name);
    if (e->original) {
        scope_untrack(vm, vm->current_scope, e->original);
        value_dispose(vm, e->original);
        allocator_free(vm->alloc, (void **)&e->original);
    }
    allocator_free(vm->alloc, (void **)&e);
}

static bool restore_entry(vm_t *vm, clux_mock_entry_t *e) {
    if (!e || !e->slot || !e->original) return false;
    value_t *r = value_assign(vm, e->slot, e->original);
    return !value_is_error(vm, r);
}

static const struct_type_t *find_method_owner(const type_t *t,
                                              const char *name,
                                              value_t **out_slot) {
    if (!t || type_kind_of(t) != TYPE_KIND_STRUCT) return NULL;
    strmap_t *attrs = type_struct_attributes(t);
    if (attrs) {
        value_t *v = (value_t *)strmap_get(attrs, name);
        if (v) {
            if (out_slot) *out_slot = v;
            return (const struct_type_t *)type_strip_quals(t);
        }
    }
    size_t ne = 0;
    const type_t *const *emb = type_struct_embeds(t, &ne);
    for (size_t i = 0; i < ne; i++) {
        const struct_type_t *o = find_method_owner(emb[i], name, out_slot);
        if (o) return o;
    }
    return NULL;
}

static value_t *install(vm_t *vm, clux_mock_kind_t kind, const char *name,
                        const type_t *type, value_t *slot, value_t *repl) {
    if (!vm->in_test)
        return value_make_error(vm, "mock: not in test context");
    if (!ensure_entries(vm)) return value_make_error(vm, "mock: out of memory");
    value_t *orig = value_clone(vm, slot);
    if (!orig || value_is_error(vm, orig))
        return orig ? orig : value_make_error(vm, "mock: cannot save original");
    scope_untrack(vm, vm->current_scope, orig);
    clux_mock_entry_t *e = (clux_mock_entry_t *)allocator_new_ex(
        vm->alloc, "mock.entry", sizeof(*e), NULL, NULL, NULL, 1);
    if (!e) {
        value_dispose(vm, orig);
        allocator_free(vm->alloc, (void **)&orig);
        return value_make_error(vm, "mock: out of memory");
    }
    size_t nlen = strlen(name);
    e->name = (char *)allocator_new_ex(vm->alloc, "mock.name", nlen + 1,
                                       NULL, NULL, NULL, 1);
    if (!e->name) {
        allocator_free(vm->alloc, (void **)&e);
        value_dispose(vm, orig);
        allocator_free(vm->alloc, (void **)&orig);
        return value_make_error(vm, "mock: out of memory");
    }
    memcpy(e->name, name, nlen + 1);
    e->kind = kind;
    e->type = type;
    e->slot = slot;
    e->original = orig;
    value_t *r = value_assign(vm, slot, repl);
    if (value_is_error(vm, r)) {
        entry_free(vm, e);
        return r;
    }
    vec_push(vm->mock_entries, vm->alloc, e);
    return NULL;
}

static value_t *builtin_mock_cfunc(vm_t *vm, func_t *self, size_t argc,
                                   value_t **args) {
    (void)self;
    if (argc != 3 || !args[0] || !args[1] || !args[2])
        return value_make_error(vm, "mock: expects (name, target, replacement)");
    const char *name = arg_str(args[0]);
    if (!name) return value_make_error(vm, "mock: name must be str");
    const type_t *tt = value_type(args[1]);
    const type_t *rt = value_type(args[2]);
    if (!tt || type_kind_of(tt) != TYPE_KIND_FUNC ||
        !rt || type_kind_of(rt) != TYPE_KIND_FUNC)
        return value_make_error(vm, "mock: target/replacement must be functions");
    if (!type_eq(tt, rt))
        return value_make_error(vm, "mock: replacement signature mismatch");
    value_t *slot = scope_lookup(vm->current_scope, strslice_from_cstr(name));
    if (!slot) {
        /* 内建 cfunc 调用时 current_scope 是内建自己的 scope，不是调用者
           模块；用 target 函数对象的 root_scope 找它所在模块的绑定。 */
        func_t *tf = NULL;
        if (value_type(args[1]) && value_data(args[1]))
            tf = *(func_t **)value_data(args[1]);
        if (tf && tf->root_scope) {
            strslice_t nm = tf->name.ptr ? tf->name : strslice_from_cstr(name);
            slot = scope_lookup(tf->root_scope, nm);
        }
    }
    if (!slot) return value_make_error(vm, "mock: function binding not found");
    if (!value_type(slot) || type_kind_of(value_type(slot)) != TYPE_KIND_FUNC)
        return value_make_error(vm, "mock: target binding is not a function");
    value_t *r = install(vm, CLUX_MOCK_FREE, name, NULL, slot, args[2]);
    return r;
}

static value_t *builtin_unmock_cfunc(vm_t *vm, func_t *self, size_t argc,
                                     value_t **args) {
    (void)self;
    if (!vm->in_test)
        return value_make_error(vm, "unmock: not in test context");
    if (argc != 1 || !args[0])
        return value_make_error(vm, "unmock: expects (name)");
    const char *name = arg_str(args[0]);
    if (!name) return value_make_error(vm, "unmock: name must be str");
    if (!vm->mock_entries) return value_make_error(vm, "unmock: no such mock");
    for (size_t i = vec_len(vm->mock_entries); i > 0; i--) {
        clux_mock_entry_t *e = (clux_mock_entry_t *)vec_get(vm->mock_entries, i - 1);
        if (e && e->kind == CLUX_MOCK_FREE && strcmp(e->name, name) == 0) {
            bool ok = restore_entry(vm, e);
            vec_remove(vm->mock_entries, i - 1);
            entry_free(vm, e);
            return ok ? NULL : value_make_error(vm, "unmock: restore failed");
        }
    }
    return value_make_error(vm, "unmock: no such mock");
}

static value_t *builtin_mock_method_cfunc(vm_t *vm, func_t *self, size_t argc,
                                          value_t **args) {
    (void)self;
    if (!vm->in_test)
        return value_make_error(vm, "mock_method: not in test context");
    if (argc != 3 || !args[0] || !args[1] || !args[2])
        return value_make_error(vm,
                                "mock_method: expects (Type, name, replacement)");
    if (value_type(args[0]) != vm->type_type)
        return value_make_error(vm, "mock_method: first argument must be a type");
    const type_t *ty = value_as(args[0], const type_t *);
    const char *name = arg_str(args[1]);
    if (!name) return value_make_error(vm, "mock_method: name must be str");
    const type_t *rt = value_type(args[2]);
    if (!rt || type_kind_of(rt) != TYPE_KIND_FUNC)
        return value_make_error(vm, "mock_method: replacement must be a function");
    value_t *slot = NULL;
    const struct_type_t *owner = find_method_owner(ty, name, &slot);
    if (!owner || !slot)
        return value_make_error(vm, "mock_method: method not found");
    if (!type_eq(value_type(slot), rt))
        return value_make_error(vm, "mock_method: replacement signature mismatch");
    value_t *r = install(vm, CLUX_MOCK_METHOD, name, ty, slot, args[2]);
    return r;
}

static value_t *builtin_unmock_method_cfunc(vm_t *vm, func_t *self, size_t argc,
                                            value_t **args) {
    (void)self;
    if (!vm->in_test)
        return value_make_error(vm, "unmock_method: not in test context");
    if (argc != 2 || !args[0] || !args[1])
        return value_make_error(vm, "unmock_method: expects (Type, name)");
    if (value_type(args[0]) != vm->type_type)
        return value_make_error(vm, "unmock_method: first argument must be a type");
    const type_t *ty = value_as(args[0], const type_t *);
    const char *name = arg_str(args[1]);
    if (!name) return value_make_error(vm, "unmock_method: name must be str");
    if (!vm->mock_entries)
        return value_make_error(vm, "unmock_method: no such mock");
    for (size_t i = vec_len(vm->mock_entries); i > 0; i--) {
        clux_mock_entry_t *e = (clux_mock_entry_t *)vec_get(vm->mock_entries, i - 1);
        if (e && e->kind == CLUX_MOCK_METHOD && e->type == ty &&
            strcmp(e->name, name) == 0) {
            bool ok = restore_entry(vm, e);
            vec_remove(vm->mock_entries, i - 1);
            entry_free(vm, e);
            return ok ? NULL : value_make_error(vm, "unmock_method: restore failed");
        }
    }
    return value_make_error(vm, "unmock_method: no such mock");
}

void vm_mock_unmock_all(vm_t *vm) {
    if (!vm || !vm->mock_entries) return;
    for (size_t i = vec_len(vm->mock_entries); i > 0; i--) {
        clux_mock_entry_t *e = (clux_mock_entry_t *)vec_get(vm->mock_entries, i - 1);
        if (e) {
            restore_entry(vm, e);
            entry_free(vm, e);
        }
    }
    vec_free(vm->alloc, &vm->mock_entries);
    vm->mock_entries = NULL;
}

static void register_builtin(vm_t *vm, const char *name, cfunc_t fn) {
    const type_t *sig = type_func_sig(vm, NULL, 0, vm->type_void, true);
    if (!sig) return;
    value_t *fnv = func_new(vm, fn, NULL, vm->root_scope, sig,
                            strslice_from_cstr(name));
    if (!fnv) return;
    scope_define(vm, vm->global_scope, name, fnv);
}

void vm_register_mock(vm_t *vm) {
    if (!vm || !vm->global_scope) return;
    register_builtin(vm, "mock", builtin_mock_cfunc);
    register_builtin(vm, "unmock", builtin_unmock_cfunc);
    register_builtin(vm, "mock_method", builtin_mock_method_cfunc);
    register_builtin(vm, "unmock_method", builtin_unmock_method_cfunc);
}
