#include "sema/reflect.h"
#include "sema/sema.h"
#include "sema/struct_compose.h"
#include "sema/symbol.h"

#include "core/arena.h"
#include "core/vec.h"
#include "vm/type.h"
#include "vm/type_ownership.h"
#include "vm/type_array.h"
#include "vm/type_cfunc.h"
#include "vm/type_cunion.h"
#include "vm/type_enum.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/value.h"
#include "vm/vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===========================================================================
 * 反射 key 的取值核心（sema 折叠与 CTFE 共用）
 *
 * 设计：docs/reflect-builtin-design.md。要点：
 *   - 只读 type_t 里既有的元数据，不新增运行期存储；
 *   - kind / ownership 返回**编译期字符串**（自描述）；
 *   - 计数 + 索引，不返回数组；
 *   - 属性表是无序 strmap → **按名排序**输出（确定性）。
 * =========================================================================== */

value_t *sema_reflect_value(vm_t *vm, const sema_rv_t *rv) {
  if (!vm || !rv) return NULL;
  switch (rv->kind) {
  case SEMA_RV_STR: {
    const char *p = rv->s.ptr ? (const char *)rv->s.ptr : "";
    void *d = value_alloc_data_copy(vm->alloc, vm->type_str, &p);
    return value_make(vm, vm->type_str, d);
  }
  case SEMA_RV_USIZE: {
    uint64_t u = rv->u;
    const type_t *ut = vm_type_usize(vm);
    void *d = value_alloc_data_copy(vm->alloc, ut, &u);
    return value_make(vm, ut, d);
  }
  case SEMA_RV_I64: {
    int64_t i = rv->i;
    void *d = value_alloc_data_copy(vm->alloc, vm->type_i64, &i);
    return value_make(vm, vm->type_i64, d);
  }
  case SEMA_RV_BOOL: {
    bool b = rv->b;
    void *d = value_alloc_data_copy(vm->alloc, vm->type_bool, &b);
    return value_make(vm, vm->type_bool, d);
  }
  case SEMA_RV_TYPE:
    return rv->t ? type_as_value(vm, rv->t) : NULL;
  default:
    return NULL;
  }
}

const char *sema_reflect_kind_name(const type_t *T) {
  if (!T) return "unknown";
  switch (T->kind) {
  case TYPE_KIND_VOID: return "void";
  case TYPE_KIND_BOOL: return "bool";
  case TYPE_KIND_INT: return "int";
  case TYPE_KIND_FLOAT: return "float";
  case TYPE_KIND_STR: return "str";
  case TYPE_KIND_TYPE: return "type";
  case TYPE_KIND_FUNC: return "func";
  case TYPE_KIND_ERROR: return "error";
  case TYPE_KIND_INTERRUPT: return "interrupt";
  case TYPE_KIND_CONST: return "const";
  case TYPE_KIND_VOLATILE: return "volatile";
  case TYPE_KIND_OPTION: return "option";
  case TYPE_KIND_STRUCT: return "struct";
  case TYPE_KIND_ARRAY: return "array";
  case TYPE_KIND_TUPLE: return "tuple";
  case TYPE_KIND_ENUM: return "enum";
  case TYPE_KIND_UNION: return "union";
  case TYPE_KIND_CUNION: return "cunion";
  case TYPE_KIND_PTR: return "ptr";
  case TYPE_KIND_SLICE: return "slice";
  case TYPE_KIND_OPAQUE: return "opaque";
  case TYPE_KIND_MODULE: return "module";
  case TYPE_KIND_CFUNC: return "cfunc";
  case TYPE_KIND_OWN: return "own";
  case TYPE_KIND_REF: return "ref";
  case TYPE_KIND_FATAL: return "fatal";
  default: return "unknown";
  }
}

const char *sema_reflect_ownership_name(ownership_t own) {
  switch (own) {
  case OWN_OWN: return "own";
  case OWN_REF: return "ref";
  case OWN_FATAL: return "fatal";
  case OWN_NONE:
  default: return "none";
  }
}

static bool key_is(strslice_t k, const char *lit) {
  size_t n = strlen(lit);
  return k.len == n && k.ptr && memcmp(k.ptr, lit, n) == 0;
}

/* 带一个索引实参的 key（按名字；数组/切片的 elem_* 由 needs_index 排除） */
static bool key_has_index(strslice_t key) {
  static const char *kIndexed[] = {
      "field_name",       "field_type",      "field_offset",
      "embed_type",       "method_name",     "attribute_name",
      "attribute_type",   "variant_name",    "variant_value",
      "member_name",      "member_type",     "member_tag",
      "param_type",       "elem_type",       "elem_offset",
  };
  for (size_t i = 0; i < sizeof(kIndexed) / sizeof(kIndexed[0]); i++)
    if (key_is(key, kIndexed[i])) return true;
  return false;
}

bool sema_reflect_is_key(strslice_t key) {
  static const char *kAll[] = {
      "name",           "size",             "align",
      "kind",           "ownership",        "field_count",
      "field_name",
      "field_type",     "field_offset",     "embed_count",
      "embed_type",     "method_count",     "method_name",
      "method_type",    "attribute_count",  "attribute_name",
      "attribute_type", "variant_count",    "variant_name",
      "variant_value",  "underlying",       "member_count",
      "member_name",    "member_type",      "member_tag",
      "param_count",    "param_type",       "return_type",
      "is_variadic",    "is_c_abi",         "elem_count",
      "elem_type",      "elem_offset",      "target_type",
      "rc",             "inner_type",       "unqualified",
      "length",
  };
  for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); i++)
    if (key_is(key, kAll[i])) return true;
  return false;
}

bool sema_reflect_needs_index(const type_t *T, strslice_t key) {
  if (!T) return false;
  if (type_kind_of(T) == TYPE_KIND_CONST || type_kind_of(T) == TYPE_KIND_VOLATILE) {
    const type_t *sub = type_qualifier_sub(T);
    return sub ? sema_reflect_needs_index(sub, key) : key_has_index(key);
  }
  /* 数组 / 切片的 elem_type、elem_ownership **不带索引**（元素类型唯一定义）；
     元组的同名 key 按索引取第 i 个元素类型。 */
  if (type_kind_of(T) == TYPE_KIND_ARRAY || type_is_slice(T)) {
    if (key_is(key, "elem_type")) return false;
  }
  return key_has_index(key);
}

static void rv_str(sema_rv_t *out, strslice_t s) {
  out->kind = SEMA_RV_STR;
  out->s = s;
}
static void rv_usize(sema_rv_t *out, uint64_t v) {
  out->kind = SEMA_RV_USIZE;
  out->u = v;
}
static void rv_i64(sema_rv_t *out, int64_t v) {
  out->kind = SEMA_RV_I64;
  out->i = v;
}
static void rv_bool(sema_rv_t *out, bool v) {
  out->kind = SEMA_RV_BOOL;
  out->b = v;
}
static void rv_type(sema_rv_t *out, const type_t *t) {
  out->kind = SEMA_RV_TYPE;
  out->t = t;
}

static void set_err(char *err, size_t cap, const char *fmt, ...) {
  if (!err || cap == 0) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

/* 每种 kind 可用的 key 列表（诊断用） */
static const char *kind_keys(const type_t *T) {
  switch (T->kind) {
  case TYPE_KIND_STRUCT:
    return "name, size, align, kind, field_count, field_name(i), "
           "field_type(i), field_offset(i), embed_count, embed_type(i), "
           "method_count, method_name(i), attribute_count, attribute_name(i), "
           "attribute_type(i)";
  case TYPE_KIND_ENUM:
    return "name, size, align, kind, variant_count, variant_name(i), "
           "variant_value(i), underlying";
  case TYPE_KIND_UNION:
    return "name, size, align, kind, member_count, member_name(i), "
           "member_type(i), member_tag(i)";
  case TYPE_KIND_CUNION:
    return "name, size, align, kind, member_count, member_name(i), "
           "member_type(i)";
  case TYPE_KIND_FUNC:
  case TYPE_KIND_CFUNC:
    return "name, size, align, kind, param_count, param_type(i), "
           "return_type, is_variadic, is_c_abi";
  case TYPE_KIND_ARRAY:
    return "name, size, align, kind, elem_type, elem_ownership, len";
  case TYPE_KIND_SLICE:
    return "name, size, align, kind, elem_type";
  case TYPE_KIND_PTR:
    return "name, size, align, kind, target_type, rc";
  case TYPE_KIND_TUPLE:
    return "name, size, align, kind, elem_count, elem_type(i), "
           "elem_offset(i)";
  case TYPE_KIND_OPTION:
    return "name, size, align, kind, inner_type";
  case TYPE_KIND_CONST:
  case TYPE_KIND_VOLATILE:
    return "name, size, align, kind, unqualified, <任意底层类型的 key>";
  default:
    return "name, size, align, kind";
  }
}

/* 通用 key（所有 kind 都有）：返回 true 表示已处理 */
static bool lookup_common(const type_t *T, strslice_t key, sema_rv_t *out,
                          char *err, size_t ecap) {
  if (key_is(key, "name")) {
    rv_str(out, T->name.ptr ? T->name : STRSLICE_LIT("?"));
    return true;
  }
  if (key_is(key, "size")) {
    rv_usize(out, (uint64_t)T->size);
    return true;
  }
  if (key_is(key, "align")) {
    rv_usize(out, (uint64_t)T->align);
    return true;
  }
  if (key_is(key, "ownership")) {
    const char *own = sema_reflect_ownership_name(type_ownership_of(T));
    rv_str(out, strslice_from_bytes(own, strlen(own)));
    return true;
  }
  if (key_is(key, "kind")) {
    rv_str(out, strslice_from_bytes(sema_reflect_kind_name(T),
                                    strlen(sema_reflect_kind_name(T))));
    return true;
  }
  (void)err;
  (void)ecap;
  return false;
}

/* ===========================================================================
 * 结构体成员（方法 / 静态属性）反射 —— 路线 A：读 sema 的成员作用域
 *
 * `struct_type_t.attributes` / `methods` 是**运行期** STORE_ATTR 才填的，sema
 * 阶段为空；但 sema 在 `SEMA_SYM_TYPE` 符号上维护了**成员作用域**
 * （`sym->member_scope`，成员名 → sema_symbol_t，方法带 func 类型、属性带变量
 * 类型），`T::x` 查找（sema_struct_find_member）走的就是它。反射直接复用这份
 * 编译期登记，不另建表。
 * =========================================================================== */

static sema_scope_t *struct_member_scope(struct sema_t *sema, const type_t *T) {
  if (!sema || !T || !T->name.ptr) return NULL;
  const type_t *base = type_strip_quals(T);
  if (!base || type_kind_of(base) != TYPE_KIND_STRUCT) return NULL;
  sema_symbol_t *sym = sema_lookup(sema->global_scope, base->name);
  return sym ? sym->member_scope : NULL;
}

static bool member_key(strslice_t key) {
  return key_is(key, "method_count") || key_is(key, "method_name") ||
         key_is(key, "method_type") || key_is(key, "attribute_count") ||
         key_is(key, "attribute_name") || key_is(key, "attribute_type");
}

/* 收集成员名（methods_only: FUNC / VAR），按名排序（strmap 无序 → 确定性）。
   返回 malloc 数组（名字借用成员作用域），调用方 free 数组本身。 */
static size_t collect_member_names(struct sema_t *sema, const type_t *T,
                                   bool methods_only, const char ***out) {
  *out = NULL;
  sema_scope_t *ms = struct_member_scope(sema, T);
  if (!ms || !ms->symbols) return 0;
  const vec_t *keys = strmap_keys(ms->symbols);
  size_t n = keys ? vec_len(keys) : 0;
  const char **arr = (const char **)malloc((n ? n : 1) * sizeof(char *));
  if (!arr) return 0;
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++) {
    const char *k = (const char *)vec_get(keys, i);
    if (!k) continue;
    sema_symbol_t *sym = sema_lookup(ms, strslice_from_bytes(k, strlen(k)));
    if (!sym) continue;
    bool is_method = sym->kind == SEMA_SYM_FUNC;
    if (is_method != methods_only) continue;
    arr[cnt++] = k;
  }
  for (size_t i = 1; i < cnt; i++) {
    const char *v = arr[i];
    size_t j = i;
    while (j > 0 && strcmp(arr[j - 1], v) > 0) {
      arr[j] = arr[j - 1];
      j--;
    }
    arr[j] = v;
  }
  *out = arr;
  return cnt;
}

static bool member_lookup(struct sema_t *sema, const type_t *T, strslice_t key,
                          const sema_reflect_arg_t *arg, sema_rv_t *out,
                          char *err, size_t ecap) {
  const bool is_method = !key_is(key, "attribute_count") &&
                         !key_is(key, "attribute_name") &&
                         !key_is(key, "attribute_type");
  if (!struct_member_scope(sema, T)) {
    set_err(err, ecap, "'%s' has no %s members",
            T && T->name.ptr ? (const char *)T->name.ptr : "?",
            is_method ? "method" : "attribute");
    return false;
  }
  if (key_is(key, "method_count") || key_is(key, "attribute_count")) {
    const char **arr = NULL;
    size_t n = collect_member_names(sema, T, is_method, &arr);
    free((void *)arr);
    rv_usize(out, (uint64_t)n);
    return true;
  }
  if (key_is(key, "method_name") || key_is(key, "attribute_name") ||
      key_is(key, "attribute_type")) {
    if (!arg || arg->kind != SEMA_RARG_INDEX) {
      set_err(err, ecap, "'%.*s' requires an index argument", (int)key.len,
              key.ptr);
    return false;
    }
    const char **arr = NULL;
    size_t n = collect_member_names(sema, T, is_method, &arr);
    if (arg->index >= n) {
      set_err(err, ecap, "%s index %llu out of range (there are %zu %ss)",
              is_method ? "method" : "attribute",
              (unsigned long long)arg->index, n, is_method ? "method" : "attribute");
      free((void *)arr);
    return false;
    }
    const char *nm = arr[arg->index];
    if (key_is(key, "method_name") || key_is(key, "attribute_name")) {
      rv_str(out, strslice_from_bytes(nm, strlen(nm)));
    } else {
      sema_symbol_t *sym = sema_struct_find_member(
          sema, T, strslice_from_bytes(nm, strlen(nm)), NULL, NULL);
      if (!sym || !sym->type) {
        set_err(err, ecap, "attribute '%s' has no type", nm);
        free((void *)arr);
    return false;
      }
      rv_type(out, sym->type);
    }
    free((void *)arr);
    return true;
  }
  if (key_is(key, "method_type")) {
    if (!arg || arg->kind != SEMA_RARG_NAME) {
      set_err(err, ecap, "'method_type' requires a method name argument "
                         "(e.g. T.method_type(\"run\"))");
    return false;
    }
    sema_symbol_t *sym =
        sema_struct_find_member(sema, T, arg->name, NULL, NULL);
    if (!sym) {
      set_err(err, ecap, "no member named '%.*s' in '%s'",
              (int)arg->name.len, arg->name.ptr,
              T->name.ptr ? (const char *)T->name.ptr : "?");
    return false;
    }
    if (sym->kind != SEMA_SYM_FUNC) {
      set_err(err, ecap, "'%.*s' is an attribute, not a method",
              (int)arg->name.len, arg->name.ptr);
    return false;
    }
    if (!sym->type || type_kind_of(sym->type) != TYPE_KIND_FUNC) {
      set_err(err, ecap, "method '%.*s' has no signature", (int)arg->name.len,
              arg->name.ptr);
    return false;
    }
    rv_type(out, sym->type);
    return true;
  }
  return false;
}

bool sema_reflect_eval(struct sema_t *sema, const type_t *T, strslice_t key,
                       const sema_reflect_arg_t *arg, sema_rv_t *out,
                       char *err, size_t ecap) {
  const type_t *base = T ? type_strip_quals(T) : NULL;
  if (base && type_kind_of(base) == TYPE_KIND_STRUCT && member_key(key))
    return member_lookup(sema, base, key, arg, out, err, ecap);
  return sema_reflect_lookup(T, key, arg, out, err, ecap);
}

sema_rarg_t sema_reflect_arg_kind(const type_t *T, strslice_t key) {
  const type_t *base = T ? type_strip_quals(T) : NULL;
  if (!base) return SEMA_RARG_NONE;
  if (key_is(key, "method_type")) return SEMA_RARG_NAME;
  return sema_reflect_needs_index(base, key) ? SEMA_RARG_INDEX
                                             : SEMA_RARG_NONE;
}

bool sema_reflect_lookup(const type_t *T, strslice_t key,
                         const sema_reflect_arg_t *arg, sema_rv_t *out,
                         char *err, size_t ecap) {
  if (!T || !out) return false;
  memset(out, 0, sizeof *out);
  const bool has_index = arg && arg->kind == SEMA_RARG_INDEX;
  const uint64_t index = has_index ? arg->index : 0;

  /* 通用 key 对任何 kind 都成立（含 const/volatile 自身） */
  if (lookup_common(T, key, out, err, ecap)) return true;

  /* const/volatile：`unqualified` 剥限定；其余 key **自动穿透**到被限定类型 */
  if (type_kind_of(T) == TYPE_KIND_CONST || type_kind_of(T) == TYPE_KIND_VOLATILE ||
      T->kind == TYPE_KIND_OWN || T->kind == TYPE_KIND_REF ||
      T->kind == TYPE_KIND_FATAL) {
    const type_t *sub = type_qualifier_sub(T);
    if (key_is(key, "unqualified")) {
      if (!sub) {
        set_err(err, ecap, "'%s' has no qualifying type",
                T->name.ptr ? (const char *)T->name.ptr : "?");
        return false;
      }
      rv_type(out, sub);
      return true;
    }
    return sub ? sema_reflect_lookup(sub, key, arg, out, err, ecap) : false;
  }

  const char *kname = sema_reflect_kind_name(T);

  /* ---- struct ---- */
  if (type_kind_of(T) == TYPE_KIND_STRUCT) {
    const struct_type_t *st = (const struct_type_t *)T;
    if (key_is(key, "field_count")) {
      rv_usize(out, (uint64_t)st->field_count);
      return true;
    }
    if (key_is(key, "embed_count")) {
      rv_usize(out, (uint64_t)st->embed_count);
      return true;
    }
    if (key_is(key, "field_name") || key_is(key, "field_type") ||
        key_is(key, "field_offset")) {
      if (!has_index) {
        set_err(err, ecap,
                "'%s': '%.*s' requires an index argument (e.g. T.%.*s(0))",
                kname, (int)key.len, key.ptr, (int)key.len, key.ptr);
        return false;
      }
      if (index >= st->field_count) {
        set_err(err, ecap,
                "%s: field index %llu out of range ('%s' has %zu fields)",
                kname, (unsigned long long)index,
                T->name.ptr ? (const char *)T->name.ptr : "?",
                st->field_count);
        return false;
      }
      const struct_field_t *f = &st->fields[index];
      if (key_is(key, "field_name")) rv_str(out, f->name);
      else if (key_is(key, "field_type")) rv_type(out, f->type);
      else rv_usize(out, (uint64_t)f->offset);
      return true;
    }
    if (key_is(key, "embed_type")) {
      if (!has_index || index >= st->embed_count) {
        set_err(err, ecap, "%s: embed index %llu out of range", kname,
                (unsigned long long)index);
        return false;
      }
      rv_type(out, st->embeds[index]);
      return true;
    }
    if (key_is(key, "method_name")) {
      if (!has_index || !st->methods || index >= vec_len(st->methods)) {
        set_err(err, ecap, "%s: method index %llu out of range", kname,
                (unsigned long long)index);
        return false;
      }
      const char *m = (const char *)vec_get(st->methods, index);
      rv_str(out, strslice_from_bytes(m ? m : "", m ? strlen(m) : 0));
      return true;
    }
  }

  /* ---- enum ---- */
  if (type_kind_of(T) == TYPE_KIND_ENUM) {
    const enum_type_t *et = (const enum_type_t *)T;
    if (key_is(key, "variant_count")) {
      rv_usize(out, (uint64_t)et->variant_count);
      return true;
    }
    if (key_is(key, "underlying")) {
      rv_type(out, et->underlying);
      return true;
    }
    if (key_is(key, "variant_name") || key_is(key, "variant_value")) {
      if (!has_index || index >= et->variant_count) {
        set_err(err, ecap, "%s: variant index %llu out of range (%zu variants)",
                kname, (unsigned long long)index, et->variant_count);
        return false;
      }
      if (key_is(key, "variant_name"))
        rv_str(out, et->variants[index].name);
      else
        rv_i64(out, et->variants[index].value);
      return true;
    }
  }

  /* ---- union / cunion ---- */
  if (type_kind_of(T) == TYPE_KIND_UNION) {
    const union_type_t *ut = (const union_type_t *)T;
    if (key_is(key, "member_count")) {
      rv_usize(out, (uint64_t)ut->member_count);
      return true;
    }
    if (key_is(key, "member_name") || key_is(key, "member_type") ||
        key_is(key, "member_tag")) {
      if (!has_index || index >= ut->member_count) {
        set_err(err, ecap, "%s: member index %llu out of range (%zu members)",
                kname, (unsigned long long)index, ut->member_count);
        return false;
      }
      const union_member_t *m = &ut->members[index];
      if (key_is(key, "member_name")) rv_str(out, m->name);
      else if (key_is(key, "member_type")) rv_type(out, m->payload_type);
      else rv_usize(out, (uint64_t)m->tag);
      return true;
    }
  }
  if (type_kind_of(T) == TYPE_KIND_CUNION) {
    const cunion_type_t *ct = (const cunion_type_t *)T;
    if (key_is(key, "member_count")) {
      rv_usize(out, (uint64_t)ct->member_count);
      return true;
    }
    if (key_is(key, "member_name") || key_is(key, "member_type")) {
      if (!has_index || index >= ct->member_count) {
        set_err(err, ecap, "%s: member index %llu out of range (%zu members)",
                kname, (unsigned long long)index, ct->member_count);
        return false;
      }
      const cunion_member_t *m = &ct->members[index];
      if (key_is(key, "member_name")) rv_str(out, m->name);
      else rv_type(out, m->type);
      return true;
    }
  }

  /* ---- func / cfunc ---- */
  if (type_kind_of(T) == TYPE_KIND_FUNC || type_kind_of(T) == TYPE_KIND_CFUNC) {
    const func_sig_t *sig =
        type_kind_of(T) == TYPE_KIND_FUNC ? &((const func_type_t *)T)->sig
                                  : cfunc_type_sig(T);
    if (sig) {
      if (key_is(key, "param_count")) {
        rv_usize(out, (uint64_t)sig->param_count);
        return true;
      }
      if (key_is(key, "return_type")) {
        rv_type(out, sig->return_type ? sig->return_type : NULL);
        return true;
      }
      if (key_is(key, "is_variadic")) {
        rv_bool(out, sig->is_variadic);
        return true;
      }
      if (key_is(key, "is_c_abi")) {
        rv_bool(out, type_kind_of(T) == TYPE_KIND_CFUNC);
        return true;
      }
      if (key_is(key, "param_type")) {
        if (!has_index || index >= sig->param_count) {
          set_err(err, ecap, "%s: parameter index %llu out of range (%zu params)",
                  kname, (unsigned long long)index, sig->param_count);
          return false;
        }
        rv_type(out, sig->params ? sig->params[index] : NULL);
        return true;
      }
    }
  }

  /* ---- array / slice / ptr / tuple / option ---- */
  if (type_kind_of(T) == TYPE_KIND_ARRAY) {
    if (key_is(key, "elem_type")) {
      rv_type(out, array_type_elem(T));
      return true;
    }
    if (key_is(key, "length")) {
      rv_usize(out, (uint64_t)array_type_len(T));
      return true;
    }
  }
  if (type_is_slice(T)) {
    if (key_is(key, "elem_type")) {
      rv_type(out, slice_type_elem(T));
      return true;
    }
  }
  if (type_is_ptr(T)) {
    if (key_is(key, "target_type")) {
      rv_type(out, ptr_type_base(T));
      return true;
    }
    if (key_is(key, "rc")) {
      const char *rc = "plain";
      if (type_is_share(T)) rc = "share";
      else if (type_is_weak(T)) rc = "weak";
      rv_str(out, strslice_from_bytes(rc, strlen(rc)));
      return true;
    }
  }
  if (type_kind_of(T) == TYPE_KIND_TUPLE) {
    const tuple_type_t *tt = (const tuple_type_t *)T;
    if (key_is(key, "elem_count")) {
      rv_usize(out, (uint64_t)tt->elem_count);
      return true;
    }
    if (key_is(key, "elem_type") || key_is(key, "elem_offset")) {
      if (!has_index || index >= tt->elem_count) {
        set_err(err, ecap, "%s: element index %llu out of range (%zu elems)",
                kname, (unsigned long long)index, tt->elem_count);
        return false;
      }
      const tuple_elem_t *e = &tt->elems[index];
      if (key_is(key, "elem_type")) rv_type(out, e->type);
      else rv_usize(out, (uint64_t)e->offset);
      return true;
    }
  }
  if (type_kind_of(T) == TYPE_KIND_OPTION) {
    if (key_is(key, "inner_type")) {
      rv_type(out, type_option_inner(T));
      return true;
    }
  }

  /* 未命中：给出该 kind 的可用 key 列表 */
  if (key_has_index(key)) {
    set_err(err, ecap,
            "%s '%s' has no reflection member '%.*s'; available: %s", kname,
            T->name.ptr ? (const char *)T->name.ptr : "?", (int)key.len,
            key.ptr, kind_keys(T));
  } else {
    set_err(err, ecap,
            "%s '%s' has no reflection member '%.*s'; available: %s", kname,
            T->name.ptr ? (const char *)T->name.ptr : "?", (int)key.len,
            key.ptr, kind_keys(T));
  }
  return false;
}
