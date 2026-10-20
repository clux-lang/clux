#include "sema/reflect.h"

#include "core/arena.h"
#include "core/vec.h"
#include "vm/type.h"
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
      "kind",           "field_count",      "field_name",
      "field_type",     "field_offset",     "embed_count",
      "embed_type",     "method_count",     "method_name",
      "attribute_count", "attribute_name",  "attribute_type",
      "variant_count",  "variant_name",     "variant_value",
      "underlying",     "member_count",     "member_name",
      "member_type",    "member_tag",       "param_count",
      "param_type",     "return_type",      "is_variadic",
      "is_c_abi",       "elem_count",       "elem_type",
      "elem_offset",    "target_type",      "rc",
      "inner_type",     "unqualified",      "length",
  };
  for (size_t i = 0; i < sizeof(kAll) / sizeof(kAll[0]); i++)
    if (key_is(key, kAll[i])) return true;
  return false;
}

bool sema_reflect_needs_index(const type_t *T, strslice_t key) {
  if (!T) return false;
  if (T->kind == TYPE_KIND_CONST || T->kind == TYPE_KIND_VOLATILE) {
    const type_t *sub = type_qualifier_sub(T);
    return sub ? sema_reflect_needs_index(sub, key) : key_has_index(key);
  }
  /* 数组 / 切片的 elem_type、elem_ownership **不带索引**（元素类型唯一定义）；
     元组的同名 key 按索引取第 i 个元素类型。 */
  if (T->kind == TYPE_KIND_ARRAY || type_is_slice(T)) {
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

/* 属性名排序（确定性输出）：strmap 无序 → 取 key 列表排序后按索引取 */
static const char *sorted_attr_name(const struct_type_t *st, size_t idx,
                                    size_t *count_out) {
  *count_out = st->attributes ? strmap_size(st->attributes) : 0;
  if (!st->attributes || idx >= *count_out) return NULL;
  const vec_t *keys = strmap_keys(st->attributes);
  size_t n = keys ? vec_len(keys) : 0;
  if (n == 0) return NULL;
  const char **arr = (const char **)malloc(n * sizeof(char *));
  if (!arr) return NULL;
  for (size_t i = 0; i < n; i++) arr[i] = (const char *)vec_get(keys, i);
  /* 简单插入排序（属性数量很小） */
  for (size_t i = 1; i < n; i++) {
    const char *v = arr[i];
    size_t j = i;
    while (j > 0 && strcmp(arr[j - 1], v) > 0) {
      arr[j] = arr[j - 1];
      j--;
    }
    arr[j] = v;
  }
  const char *pick = idx < n ? arr[idx] : NULL;
  /* 拷贝到静态缓冲：调用方马上用，且只取一个 */
  static char buf[256];
  if (pick) {
    snprintf(buf, sizeof buf, "%s", pick);
    pick = buf;
  }
  free(arr);
  return pick;
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
  if (key_is(key, "kind")) {
    rv_str(out, strslice_from_bytes(sema_reflect_kind_name(T),
                                    strlen(sema_reflect_kind_name(T))));
    return true;
  }
  (void)err;
  (void)ecap;
  return false;
}

bool sema_reflect_lookup(const type_t *T, strslice_t key, bool has_index,
                         uint64_t index, sema_rv_t *out, char *err,
                         size_t ecap) {
  if (!T || !out) return false;
  memset(out, 0, sizeof *out);

  /* 通用 key 对任何 kind 都成立（含 const/volatile 自身） */
  if (lookup_common(T, key, out, err, ecap)) return true;

  /* const/volatile：`unqualified` 剥限定；其余 key **自动穿透**到被限定类型 */
  if (T->kind == TYPE_KIND_CONST || T->kind == TYPE_KIND_VOLATILE) {
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
    return sub ? sema_reflect_lookup(sub, key, has_index, index, out, err, ecap)
               : false;
  }

  const char *kname = sema_reflect_kind_name(T);

  /* ---- struct ---- */
  if (T->kind == TYPE_KIND_STRUCT) {
    const struct_type_t *st = (const struct_type_t *)T;
    if (key_is(key, "field_count")) {
      rv_usize(out, (uint64_t)st->field_count);
      return true;
    }
    if (key_is(key, "embed_count")) {
      rv_usize(out, (uint64_t)st->embed_count);
      return true;
    }
    if (key_is(key, "method_count")) {
      rv_usize(out, st->methods ? (uint64_t)vec_len(st->methods) : 0);
      return true;
    }
    if (key_is(key, "attribute_count")) {
      rv_usize(out, st->attributes ? (uint64_t)strmap_size(st->attributes) : 0);
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
    if (key_is(key, "attribute_name") || key_is(key, "attribute_type")) {
      if (!has_index) {
        set_err(err, ecap, "'%s': '%.*s' requires an index argument", kname,
                (int)key.len, key.ptr);
        return false;
      }
      size_t cnt = 0;
      const char *nm = sorted_attr_name(st, index, &cnt);
      if (!nm) {
        set_err(err, ecap, "%s: attribute index %llu out of range (%zu attrs)",
                kname, (unsigned long long)index, cnt);
        return false;
      }
      if (key_is(key, "attribute_name")) {
        rv_str(out, strslice_from_bytes(nm, strlen(nm)));
      } else {
        value_t *v = st->attributes ? (value_t *)strmap_get(st->attributes, nm)
                                    : NULL;
        if (!v || !value_type(v)) {
          set_err(err, ecap, "%s: cannot read attribute '%s'", kname, nm);
          return false;
        }
        rv_type(out, value_type(v));
      }
      return true;
    }
  }

  /* ---- enum ---- */
  if (T->kind == TYPE_KIND_ENUM) {
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
  if (T->kind == TYPE_KIND_UNION) {
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
  if (T->kind == TYPE_KIND_CUNION) {
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
  if (T->kind == TYPE_KIND_FUNC || T->kind == TYPE_KIND_CFUNC) {
    const func_sig_t *sig =
        T->kind == TYPE_KIND_FUNC ? &((const func_type_t *)T)->sig
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
        rv_bool(out, T->kind == TYPE_KIND_CFUNC);
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
  if (T->kind == TYPE_KIND_ARRAY) {
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
  if (T->kind == TYPE_KIND_TUPLE) {
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
  if (T->kind == TYPE_KIND_OPTION) {
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
