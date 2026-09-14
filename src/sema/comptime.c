#include "sema/comptime.h"

#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"
#include "ctfe/ctfe.h"
#include "parser/ast_array.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_construct.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_string_lit.h"
#include "vm/type_array.h"
#include "vm/type_error.h"
#include "vm/value.h"

#include <string.h>

/* ===========================================================================
 * 工具：真实 value ↔ 编译期常量编码 ↔ 字面量 AST 节点
 * =========================================================================== */

/* 整型常量：按类型宽度写入 i/u 字段 */
static bool encode_int(vm_t *vm, value_t *v, sema_ct_const_t *out) {
  const type_t *t = value_type(v);
  uint64_t raw = 0;
  switch (t->size) {
  case 1: raw = *(const uint8_t  *)value_data(v); break;
  case 2: raw = *(const uint16_t *)value_data(v); break;
  case 4: raw = *(const uint32_t *)value_data(v); break;
  default: raw = *(const uint64_t *)value_data(v); break;
  }
  if (t == vm->type_u64) {
    out->u = raw;
  } else {
    out->i = (int64_t)raw;
  }
  return true;
}

/* 浮点常量 */
static bool encode_float(vm_t *vm, value_t *v, sema_ct_const_t *out) {
  (void)vm;
  out->f = *(const double *)value_data(v);
  return true;
}

bool sema_ct_encode(sema_t *sema, value_t *v, sema_ct_const_t *out) {
  if (!sema || !v || !out) return false;
  vm_t *vm = sema->vm;
  const type_t *t = value_type(v);
  if (!t) return false;
  memset(out, 0, sizeof(*out));
  out->type = t;

  if (t == vm->type_i8 || t == vm->type_i16 || t == vm->type_i32 ||
      t == vm->type_i64 || t == vm->type_u8 || t == vm->type_u16 ||
      t == vm->type_u32 || t == vm->type_u64) {
    return encode_int(vm, v, out);
  }
  if (t == vm->type_f32 || t == vm->type_f64) {
    return encode_float(vm, v, out);
  }
  if (t == vm->type_bool) {
    out->b = *(const bool *)value_data(v);
    return true;
  }
  if (t == vm->type_str) {
    /* 字符串：复制到 sema arena（跨 sema/compile 阶段安全） */
    string_t *s = *(string_t **)value_data(v);
    size_t len = string_len(s);
    const char *c = string_cstr(s);
    char *buf = (char *)arena_calloc(sema->arena, 1, len + 1,
                                     ALIGNOF(max_align_t));
    if (!buf) return false;
    memcpy(buf, c, len + 1);
    out->s = strslice_from_bytes(buf, len);
    return true;
  }
  if (t->kind == TYPE_KIND_ARRAY) {
    /* 数组常量：逐个元素递归编码到 arena 连续块（借用引用
       value_array_at 遍历块内业务内存，元素深拷贝递归）。 */
    size_t count = value_array_count(v);
    out->count = count;
    if (count == 0) {
      out->elems = NULL;
      return true;
    }
    sema_ct_const_t *es = (sema_ct_const_t *)arena_calloc(
        sema->arena, count, sizeof(sema_ct_const_t), ALIGNOF(max_align_t));
    if (!es) return false;
    for (size_t i = 0; i < count; i++) {
      value_t *e = value_array_at(vm, v, i);
      if (!e) return false;
      if (!sema_ct_encode(sema, e, &es[i])) return false;
    }
    out->elems = es;
    return true;
  }
  return false; /* void/type/func/其他复合：不可折叠 */
}

/* 从常量类型构造类型表达式 AST（折叠写回 AST_CONSTRUCT 的类型位用）：
   内置标量类型 → AST_IDENT(类型名)；嵌套数组 → 递归 AST_ARRAY。
   位置借用 origin（折叠节点位置，诊断定位不变）。 */
static ast_node_t *sema_ct_type_expr(sema_t *sema, const type_t *t,
                                     const ast_node_t *origin) {
  if (!sema || !t) return NULL;
  arena_t *arena = sema->arena;
  uint32_t tb = origin->tok_begin;
  uint32_t te = origin->tok_end;

  if (t->kind == TYPE_KIND_ARRAY) {
    const type_t *et = array_type_elem(t);
    size_t len = array_type_len(t);
    ast_node_t *base = sema_ct_type_expr(sema, et, origin);
    if (!base) return NULL;
    ast_node_t *an = ast_array_new(arena, tb, te);
    if (!an) return NULL;
    ast_array_t *arr = (ast_array_t *)an;
    arr->base_type = base;
    ast_node_t *len_lit = ast_int_lit_new(arena, tb, te);
    if (!len_lit) return NULL;
    ((ast_int_lit_t *)len_lit)->value = (uint64_t)len;
    arr->length = len_lit;
    return an;
  }

  /* 标量类型：AST_IDENT 引用类型名（type->name 生命周期 = vm 池） */
  if (!t->name.ptr) return NULL;
  ast_node_t *id = ast_ident_new(arena, tb, te);
  if (!id) return NULL;
  ((ast_ident_t *)id)->name = t->name;
  return id;
}

/* 兄弟链连接辅助：返回链头（首个非 NULL 节点） */
static ast_node_t *sema_ct_link(ast_node_t **head, ast_node_t **last,
                                ast_node_t *node) {
  if (!node) return *head;
  if (*last) (*last)->next = node;
  else *head = node;
  *last = node;
  return *head;
}

ast_node_t *sema_ct_lit(sema_t *sema, const ast_node_t *origin,
                        const sema_ct_const_t *ct) {
  if (!sema || !origin || !ct || !ct->type) return NULL;
  vm_t *vm = sema->vm;
  arena_t *arena = sema->arena;
  const type_t *t = ct->type;
  uint32_t tb = origin->tok_begin;
  uint32_t te = origin->tok_end;

  if (t == vm->type_i8 || t == vm->type_i16 || t == vm->type_i32 ||
      t == vm->type_i64 || t == vm->type_u8 || t == vm->type_u16 ||
      t == vm->type_u32 || t == vm->type_u64) {
    ast_node_t *n = ast_int_lit_new(arena, tb, te);
    if (!n) return NULL;
    ast_int_lit_t *il = (ast_int_lit_t *)n;
    il->type = t->name; /* 内置类型名 strslice（生命周期 = vm） */
    il->value = (t == vm->type_u64) ? ct->u : (uint64_t)ct->i;
    return n;
  }
  if (t == vm->type_f32 || t == vm->type_f64) {
    ast_node_t *n = ast_float_lit_new(arena, tb, te);
    if (!n) return NULL;
    ast_float_lit_t *fl = (ast_float_lit_t *)n;
    fl->type = t->name;
    fl->value = ct->f;
    return n;
  }
  if (t == vm->type_bool) {
    ast_node_t *n = ast_bool_lit_new(arena, tb, te);
    if (!n) return NULL;
    ((ast_bool_lit_t *)n)->value = ct->b;
    return n;
  }
  if (t == vm->type_str) {
    ast_node_t *n = ast_string_lit_new(arena, tb, te);
    if (!n) return NULL;
    ((ast_string_lit_t *)n)->text = ct->s; /* arena 复制，生命周期 = arena */
    return n;
  }
  if (t->kind == TYPE_KIND_ARRAY) {
    /* 复杂类型常量：写回为 AST_CONSTRUCT（.<type>{ fields }）节点。
       type 位递归构造类型表达式（[N]T → AST_ARRAY 嵌套），fields 为
       逐个元素递归折叠的字面量兄弟链。下游 compiler 按标准构造路径
       （compile_type_expr + CONSTRUCT N）编译，零感知。 */
    ast_node_t *type_expr = sema_ct_type_expr(sema, t, origin);
    if (!type_expr) return NULL;
    ast_node_t *cnode = ast_construct_new(arena, tb, te);
    if (!cnode) return NULL;
    ast_construct_t *cn = (ast_construct_t *)cnode;
    cn->type = type_expr;

    ast_node_t *fhead = NULL, *flast = NULL;
    for (size_t i = 0; i < ct->count; i++) {
      ast_node_t *el = sema_ct_lit(sema, origin, &ct->elems[i]);
      if (!el) return NULL;
      sema_ct_link(&fhead, &flast, el);
    }
    cn->fields = fhead;
    cn->fields_last = flast;
    return cnode;
  }
  return NULL;
}

/* ===========================================================================
 * comptime var 求值
 * =========================================================================== */

bool sema_eval_comptime_var(sema_t *sema, ast_var_def_t *vd,
                            sema_scope_t *scope) {
  sema_symbol_t *sym = sema_scope_find_local(scope, vd->name);
  if (!sym) return false; /* 3a 重复定义已诊断，符号未注册 */

  /* comptime var 必须带编译期可计算的右值 */
  if (!vd->init || vd->init->kind == AST_UNDEF) {
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
               "comptime variable '%.*s' must have a compile-time initializer",
               (int)vd->name.len, vd->name.ptr);
    return false;
  }

  /* 1. 表达式求值（shadow 类型检查 + 改写 init 中的 comptime 引用）。
     错误恢复产物（error/void shadow）跳过 ctfe（避免二次诊断） */
  value_t *sh = sema_expr(sema, &vd->init, scope);
  bool bad = value_is_error(sema->vm, sh) ||
             value_is_type(sh, TYPE_KIND_VOID);
  if (bad) return false;

  /* 2. 类型校验（显式类型 / 推断，与普通 var 一致）。显式类型在 Pass 3a
     未填充（sym->type 为 NULL），此处先 resolve（与 shadow_var_def 的
     "显式类型校验"语义对齐）。 */
  if (vd->type_expr) {
    if (!sym->type) {
      sym->type = resolve_type_expr(sema, vd->type_expr);
    }
    if (sym->type) {
      value_t *dst = value_make_shadow(sema->vm, sym->type);
      if (value_is_error(sema->vm, value_assign(sema->vm, dst, sh))) {
        char tn[64], itn[64];
        sema_type_name(sym->type, tn, sizeof(tn));
        sema_type_name(value_type(sh), itn, sizeof(itn));
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "cannot initialize comptime variable '%.*s' of type %s "
                   "with %s",
                   (int)vd->name.len, vd->name.ptr, tn, itn);
        return false;
      }
    }
  } else {
    sym->type = value_type(sh);
  }

  /* 3. ctfe 强制编译期求值（vm->comptime 状态标记） */
  vm_t *vm = sema->vm;
  bool saved = vm->comptime;
  vm->comptime = true;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, vd->init);
  vm->comptime = saved;

  if (!r || value_is_error(vm, r)) {
    const char *msg = NULL;
    if (r) {
      error_data_t *ed = (error_data_t *)value_data(r);
      msg = ed && ed->message ? string_cstr(ed->message) : NULL;
    }
    diag_error(sema->diag, sema_loc(sema, vd->init),
               "comptime variable '%.*s': expression is not a compile-time "
               "constant%s%s",
               (int)vd->name.len, vd->name.ptr, msg ? ": " : "",
               msg ? msg : "");
    return false;
  }

  /* 4. 编码常量到符号表 */
  sema_ct_const_t ct;
  if (!sema_ct_encode(sema, r, &ct)) {
    char tn[64];
    sema_type_name(value_type(r), tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, vd->init),
               "comptime variable '%.*s': value of type %s cannot be folded "
               "to a constant",
               (int)vd->name.len, vd->name.ptr, tn);
    return false;
  }

  sym->is_comptime = true;
  sym->ct_valid = true;
  sym->ct = ct;
  sym->flow_init = true;
  sym->is_active = true;
  return true;
}

/* ===========================================================================
 * comptime func 调用求值（调用点折叠为字面量）
 * =========================================================================== */

value_t *sema_eval_comptime_call(sema_t *sema, ast_node_t **node,
                                 sema_scope_t *scope) {
  ast_call_t *call = (ast_call_t *)*node;
  vm_t *vm = sema->vm;

  /* 1. 实参逐个 sema_expr：shadow 类型检查 + 改写 comptime var 引用为
     字面量（ctfe 求值时实参即纯字面量，无运行期依赖）。经链上指针
     （&call->args 逐级推进）传递，折叠就地写回实参链。 */
  size_t argc = sema_count_siblings(call->args);
  value_t *arg_shadows[argc > 0 ? argc : 1];
  ast_node_t **link = &call->args;
  for (size_t i = 0; *link; link = &(*link)->next, i++) {
    value_t *av = sema_expr(sema, link, scope);
    if (value_is_error(vm, av) ||
        value_is_type(av, TYPE_KIND_VOID)) {
      /* 实参错误已诊断，返回错误恢复产物 */
      return value_make_shadow(vm, vm->type_void);
    }
    arg_shadows[i] = av;
  }

  /* 1.5 签名校验：shadow callee + value_call（与普通调用一致，ctfe 前先
     报参数数量/类型错误，避免落入模糊的"非编译期常量"诊断） */
  ast_ident_t *name = (ast_ident_t *)call->callee;
  sema_symbol_t *csym = sema_lookup(sema->global_scope, name->name);
  if (csym && csym->type) {
    value_t *callee_shadow = value_make_shadow(vm, csym->type);
    value_t *chk = value_call(vm, callee_shadow, arg_shadows, argc);
    if (value_is_error(vm, chk)) {
      error_data_t *ed = (error_data_t *)value_data(chk);
      const char *msg =
          ed && ed->message ? string_cstr(ed->message) : "call failed";
      diag_error(sema->diag, sema_loc(sema, *node), "%s", msg);
      return value_make_shadow(vm, vm->type_void);
    }
  }

  /* 2. ctfe 强制编译期求值整个调用 */
  bool saved = vm->comptime;
  vm->comptime = true;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, *node);
  vm->comptime = saved;

  if (!r || value_is_error(vm, r)) {
    const char *msg = NULL;
    if (r) {
      error_data_t *ed = (error_data_t *)value_data(r);
      msg = ed && ed->message ? string_cstr(ed->message) : NULL;
    }
    diag_error(sema->diag, sema_loc(sema, *node),
               "call to comptime function is not a compile-time constant%s%s",
               msg ? ": " : "", msg ? msg : "");
    return value_make_shadow(vm, vm->type_void);
  }

  /* 3. 编码 + 折叠调用点为字面量 */
  sema_ct_const_t ct;
  if (!sema_ct_encode(sema, r, &ct)) {
    char tn[64];
    sema_type_name(value_type(r), tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, *node),
               "comptime function call result of type %s cannot be folded",
               tn);
    return value_make_shadow(vm, vm->type_void);
  }
  ast_node_t *lit = sema_ct_lit(sema, *node, &ct);
  if (!lit) {
    diag_error(sema->diag, sema_loc(sema, *node),
               "comptime function call result cannot be folded");
    return value_make_shadow(vm, vm->type_void);
  }
  lit->next = (*node)->next; /* 保留兄弟链 */
  *node = lit;
  return value_make_shadow(vm, ct.type);
}
