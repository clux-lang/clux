#include "vm/type.h"
#include "vm/type_ownership.h"
#include "vm/value.h"
#include "vm/vm.h"
#include "core/panic.h"

#include <string.h>

/* ===========================================================================
 * 所有权修饰类型（own T / ref T / fatal T）
 *
 * **纯编译期概念**：运行期一律剥离/穿透（见 docs/ownership-as-type.md §2.5），
 * 因此这三类类型与 const/volatile 同构——持 sub、布局 = sub、运算代理到 sub，
 * 区别只在语义（由 sema 判定），运行期指令只引用剥过的裸类型。
 *
 * 三种 kind 共用一个结构体布局与**同一张 vtable**（VTABLE_OWNERSHIP）：所有
 * 槽位都把操作数临时换成 sub 后转发（与 const 的代理方式相同），这样
 * `own i32 + own i32` 之类在运行期（若真有值）行为与 `i32 + i32` 一致。
 * =========================================================================== */

const type_t *type_ownership_sub(const type_t *t) {
  if (!t) return NULL;
  switch (t->kind) {
  case TYPE_KIND_OWN:
  case TYPE_KIND_REF:
  case TYPE_KIND_FATAL:
    return ((const ownership_type_t *)t)->sub;
  default:
    return NULL;
  }
}

/* ---- 解包代理（与 const 的代理同构） ---- */

typedef value_t *(*ow_bin_slot_t)(vm_t *, value_t *, value_t *);
typedef value_t *(*ow_un_slot_t)(vm_t *, value_t *);

static value_t *ow_proxy_binary(vm_t *vm, value_t *a, value_t *b,
                                const type_t *sub, ow_bin_slot_t slot) {
  const type_t *sa = value_swap_type(a, sub);
  const type_t *sb = NULL;
  if (type_ownership_sub(value_type(b))) sb = value_swap_type(b, sub);
  value_t *r = slot(vm, a, b);
  if (sb) value_restore_type(b, sb);
  value_restore_type(a, sa);
  return r;
}

static value_t *ow_proxy_unary(vm_t *vm, value_t *a, const type_t *sub,
                               ow_un_slot_t slot) {
  const type_t *sa = value_swap_type(a, sub);
  value_t *r = slot(vm, a);
  value_restore_type(a, sa);
  return r;
}

#define OW_BIN(op)                                                        \
  static value_t *ow_##op(vm_t *vm, value_t *a, value_t *b) {             \
    const type_t *sub = type_ownership_sub(value_type(a));                            \
    if (!sub || !sub->vtable || !sub->vtable->op)                         \
      return value_make_error(vm, "ownership type: unsupported operation");\
    return ow_proxy_binary(vm, a, b, sub, sub->vtable->op);               \
  }

#define OW_UN(op)                                                         \
  static value_t *ow_##op(vm_t *vm, value_t *a) {                         \
    const type_t *sub = type_ownership_sub(value_type(a));                            \
    if (!sub || !sub->vtable || !sub->vtable->op)                         \
      return value_make_error(vm, "ownership type: unsupported operation");\
    return ow_proxy_unary(vm, a, sub, sub->vtable->op);                   \
  }

OW_BIN(add) OW_BIN(sub) OW_BIN(mul) OW_BIN(div) OW_BIN(mod)
OW_BIN(eq) OW_BIN(ne) OW_BIN(lt) OW_BIN(le) OW_BIN(gt) OW_BIN(ge)
OW_BIN(band) OW_BIN(bor) OW_BIN(bxor) OW_BIN(shl) OW_BIN(shr)
OW_BIN(assign)
OW_UN(neg) OW_UN(bnot) OW_UN(lnot)

static value_t *ow_implicit_cast(vm_t *vm, value_t *v, const type_t *target) {
  const type_t *sub = type_ownership_sub(value_type(v));
  if (!sub || !sub->vtable || !sub->vtable->implicit_cast)
    return value_make_error(vm, "ownership type: unsupported implicit cast");
  /* `own T` → `T`（同布局的身份拷贝）：运行期等价，允许显式/隐式落到裸类型 */
  if (target == sub) {
    if (value_is_shadow(v)) return value_make_shadow(vm, target);
    void *data = value_alloc_data_copy(vm->alloc, target, value_data(v));
    return value_make(vm, target, data);
  }
  const type_t *sv = value_swap_type(v, sub);
  value_t *r = sub->vtable->implicit_cast(vm, v, target);
  value_restore_type(v, sv);
  return r;
}

static value_t *ow_explicit_cast(vm_t *vm, value_t *v, const type_t *target) {
  const type_t *sub = type_ownership_sub(value_type(v));
  if (!sub || !sub->vtable || !sub->vtable->explicit_cast)
    return value_make_error(vm, "ownership type: unsupported explicit cast");
  const type_t *sv = value_swap_type(v, sub);
  value_t *r = sub->vtable->explicit_cast(vm, v, target);
  value_restore_type(v, sv);
  return r;
}

static value_t *ow_call(vm_t *vm, value_t *callee, value_t **args,
                        size_t argc) {
  const type_t *sub = type_ownership_sub(value_type(callee));
  if (!sub || !sub->vtable || !sub->vtable->call)
    return value_make_error(vm, "ownership type: value is not callable");
  const type_t *sc = value_swap_type(callee, sub);
  value_t *r = sub->vtable->call(vm, callee, args, argc);
  value_restore_type(callee, sc);
  return r;
}

static void ow_dispose(vm_t *vm, value_t *v) {
  const type_t *sub = type_ownership_sub(value_type(v));
  if (!sub || !sub->vtable || !sub->vtable->dispose) return;
  const type_t *sv = value_swap_type(v, sub);
  sub->vtable->dispose(vm, v);
  value_restore_type(v, sv);
}

static value_t *ow_clone(vm_t *vm, value_t *v) {
  void *data = value_alloc_data_copy(vm->alloc, value_type(v), value_data(v));
  return value_make(vm, value_type(v), data);
}

/* 鸭子类型判断：`own i32` 与 `i32` 是**不同类型**（相等由指针身份判定） */
static bool ow_type_equal(vm_t *vm, const type_t *a, const type_t *b) {
  (void)vm;
  return a == b;
}

/* 兼容性：所有权修饰类型**不**隐式兼容到裸类型（转换必须显式，由 sema 判），
   仅同为等价形态时成立。 */
static bool ow_type_extends(vm_t *vm, const type_t *sub, const type_t *sup) {
  (void)vm;
  return sub == sup;
}

static const type_t *ownership_type_seal(vm_t *vm, const type_t *t);

const vtable_t VTABLE_OWNERSHIP = {
    .add = ow_add, .sub = ow_sub, .mul = ow_mul, .div = ow_div,
    .mod = ow_mod, .neg = ow_neg,
    .eq = ow_eq, .ne = ow_ne, .lt = ow_lt, .le = ow_le, .gt = ow_gt,
    .ge = ow_ge,
    .band = ow_band, .bor = ow_bor, .bxor = ow_bxor, .bnot = ow_bnot,
    .shl = ow_shl, .shr = ow_shr,
    .lnot = ow_lnot,
    .call = ow_call,
    .extends = NULL,
    .dispose = ow_dispose,
    .clone = ow_clone,
    .assign = ow_assign,
    .implicit_cast = ow_implicit_cast,
    .explicit_cast = ow_explicit_cast,
    .type_equal = ow_type_equal,
    .type_extends = ow_type_extends,
    .type_seal = ownership_type_seal,
};

/* ---- 构造 / 密封 / intern ---- */

static const char *kind_prefix(type_kind_t kind) {
  switch (kind) {
  case TYPE_KIND_OWN: return "own";
  case TYPE_KIND_REF: return "ref";
  case TYPE_KIND_FATAL: return "fatal";
  default: return "?";
  }
}

static char *qual_name(allocator_t *alloc, const char *prefix,
                       const type_t *sub) {
  size_t pl = strlen(prefix);
  size_t sl = sub && sub->name.ptr ? sub->name.len : 0;
  char *buf = allocator_new_ex(alloc, "char", sizeof(char), NULL, NULL, NULL,
                               pl + 1 + sl + 1);
  if (!buf) return NULL;
  memcpy(buf, prefix, pl);
  buf[pl] = ' ';
  if (sl) memcpy(buf + pl + 1, sub->name.ptr, sl);
  buf[pl + 1 + sl] = '\0';
  return buf;
}

static ownership_type_t *ownership_type_create_open(vm_t *vm, type_kind_t kind) {
  ownership_type_t *ot = (ownership_type_t *)allocator_new_ex(
      vm->alloc, "ownership_type_t", sizeof(ownership_type_t), NULL, NULL, NULL,
      1);
  if (!ot) panic("vm: out of memory allocating ownership type");
  memset(ot, 0, sizeof(ownership_type_t));
  ot->base.vtable = &VTABLE_OWNERSHIP;
  ot->base.kind = kind;
  return ot;
}

/* 开放构造（字节码 PUSH_OWN/REF/FATAL 用）：sub=NULL，不入池 */
ownership_type_t *type_ownership_push_open(vm_t *vm, type_kind_t kind) {
  if (!vm) return NULL;
  if (kind != TYPE_KIND_OWN && kind != TYPE_KIND_REF && kind != TYPE_KIND_FATAL)
    return NULL;
  return ownership_type_create_open(vm, kind);
}

/* 开放构造 + 设 sub + 密封 + 按 (kind, sub) 去重 intern，一次完成 */
const type_t *type_ownership_intern(vm_t *vm, type_kind_t kind,
                                    const type_t *sub) {
  if (!vm || !sub) return NULL;
  if (kind != TYPE_KIND_OWN && kind != TYPE_KIND_REF && kind != TYPE_KIND_FATAL)
    return NULL;
  if (!vm->ownership_types)
    vm->ownership_types = vec_new(vm->alloc, /*owns_element=*/false);

  /* 去重：同 kind + 同 sub 的已密封类型直接复用 */
  size_t n = vec_len(vm->ownership_types);
  for (size_t i = 0; i < n; i++) {
    const ownership_type_t *other =
        (const ownership_type_t *)vec_get(vm->ownership_types, i);
    if (other && other->base.kind == kind && other->sub == sub &&
        type_is_sealed(&other->base))
      return &other->base;
  }

  ownership_type_t *ot = ownership_type_create_open(vm, kind);
  ot->sub = sub;
  ot->base.size = sub->size;
  ot->base.align = sub->align;
  ot->base.sealed = true;
  char *name = qual_name(vm->alloc, kind_prefix(kind), sub);
  if (!name) panic("vm: out of memory allocating ownership type name");
  ot->base.name = (strslice_t){name, strlen(name)};
  vec_push(vm->ownership_types, vm->alloc, ot);
  return &ot->base;
}

/* SEAL：**运行期剥离**——所有权是纯编译期概念，组装出的类型就是 sub（裸类型）。
 * 于是 program id 映射到 sub 实例，`LOAD_TYPE <id>` / 值分配拿到的都是裸类型；
 * 编译期反射看到的修饰类型由 sema 自己的类型表提供（不经运行期 SEAL）。 */
static const type_t *ownership_type_seal(vm_t *vm, const type_t *t) {
  if (!t) return NULL;
  const type_t *sub = ((const ownership_type_t *)t)->sub;
  if (!sub) return NULL;
  /* 开放对象用完即弃（未入池，名字尚未生成） */
  ownership_type_t *ot = (ownership_type_t *)t;
  if (ot->base.name.ptr) {
    char *np = (char *)ot->base.name.ptr;
    allocator_free(vm->alloc, (void **)&np);
  }
  allocator_free(vm->alloc, (void **)&ot);
  return sub;
}

/* ---- 查询辅助 ---- */

ownership_t type_ownership_of(const type_t *t) {
  for (const type_t *p = t; p;) {
    switch (p->kind) {
    case TYPE_KIND_OWN: return OWN_OWN;
    case TYPE_KIND_REF: return OWN_REF;
    case TYPE_KIND_FATAL: return OWN_FATAL;
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
      p = type_qualifier_sub(p);
      continue;
    default: return OWN_NONE;
    }
  }
  return OWN_NONE;
}

const type_t *type_strip_ownership(const type_t *t) {
  const type_t *p = t;
  bool stripped = false;
  while (p) {
    if (p->kind == TYPE_KIND_OWN || p->kind == TYPE_KIND_REF ||
        p->kind == TYPE_KIND_FATAL) {
      p = ((const ownership_type_t *)p)->sub;
      stripped = true;
      continue;
    }
    break;
  }
  if (!stripped) return t;
  /* 剥完后若外层是 const/volatile，重建一个对应修饰（保持限定语义） */
  if (t->kind == TYPE_KIND_CONST || t->kind == TYPE_KIND_VOLATILE) {
    /* const/volatile 自身会穿透到 sub；此处按 const/volatile 重包 */
    return t; /* 极少见：调用方应先用 type_qualifier_sub 剥限定 */
  }
  return p;
}
