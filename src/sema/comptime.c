#include "sema/comptime.h"

#include "core/panic.h"
#include "core/string.h"
#include "core/strslice.h"
#include "ctfe/ctfe.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_construct.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "vm/function.h"
#include "vm/scope.h"
#include "vm/type_array.h"
#include "vm/type_error.h"
#include "vm/type_type.h"
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
  if (t->kind == TYPE_KIND_FUNC) {
    /* 函数引用（comptime 函数体内引用的普通/内建函数值，或函数字面量值）：
       data 存 func_t*（CTFE 经 func_new_program_ref 构造的引用对象，或 scope
       里真实函数值）。编码函数 id（func_t->id：sema 分配的程序 fid 或内建
       id），折叠为 AST_FUNC_REF（compiler 发 LOAD_FUNCTION <id>）——与函数
       是否有 name 无关（匿名字面量无名字也可折叠）。 */
    func_t *fn = *(func_t **)value_data(v);
    if (!fn) return false;
    out->func_id = fn->id;
    return true;
  }
  return false; /* void/type/其他复合：不可折叠 */
}

/* 从常量类型构造类型表达式 AST（折叠写回 AST_CONSTRUCT 的类型位用）：
   登记类型到 sema->types 队列 → 产出 AST_TYPE_REF。程序类型名字 =
   登记名（"__type_N"），hoist 提升区负责构造，编译器零感知；内建类型
   不登记 → 名字 = 内建规范名（"i32"），编译器经 type_lookup 兜底 →
   LOAD_TYPE <内建 id>（别名透明）。复合类型结构依赖由登记递归覆盖。
   位置借用 origin（折叠节点位置，诊断定位不变）。 */
static ast_node_t *sema_ct_type_expr(sema_t *sema, const type_t *t,
                                     const ast_node_t *origin) {
  if (!sema || !t) return NULL;
  const sema_type_t *st = sema_type_register(sema, t);
  /* 内建标量（VOID..ERROR）+ INTERRUPT 哨兵不登记（func 签名类型可登记，
     但此处只处理数组折叠产物，func 不会到达——保持与 sema_type_register
     登记策略一致） */
  bool builtin;
  switch (t->kind) {
    case TYPE_KIND_VOID:
    case TYPE_KIND_BOOL:
    case TYPE_KIND_INT:
    case TYPE_KIND_FLOAT:
    case TYPE_KIND_STR:
    case TYPE_KIND_TYPE:
    case TYPE_KIND_ERROR:
    case TYPE_KIND_INTERRUPT:
      builtin = true;
      break;
    default:
      builtin = false; /* FUNC / 复合段：登记 */
      break;
  }
  if (!st && !builtin) return NULL; /* 复合类型登记失败（OOM） */
  ast_node_t *ref =
      ast_type_ref_new(sema->arena, origin->tok_begin, origin->tok_end);
  if (!ref) return NULL;
  ((ast_type_ref_t *)ref)->name = st ? st->name : t->name;
  return ref;
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
  if (t->kind == TYPE_KIND_FUNC) {
    /* 函数引用常量：折叠为 AST_FUNC_REF（纯 id 标识——compiler 直接发
       LOAD_FUNCTION <fid> 加载真实函数值，与函数是否有 name 无关：
       匿名字面量无名字也可折叠）。 */
    ast_node_t *ref =
        ast_func_ref_new(arena, origin->tok_begin, origin->tok_end);
    if (!ref) return NULL;
    ((ast_func_ref_t *)ref)->fid = ct->func_id;
    return ref;
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
      sym->type = sema_resolve_type_slot(sema, &vd->type_expr);
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
  ctx.sema_scope = scope; /* 局部符号（函数/变量）按当前词法作用域解析 */
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
 * 全局变量求值（运行时实体，init 编译期折叠）
 * =========================================================================== */

bool sema_eval_global_var(sema_t *sema, ast_var_def_t *vd,
                          sema_scope_t *scope) {
  sema_symbol_t *sym = sema_scope_find_local(scope, vd->name);
  if (!sym) return false; /* 重复定义已诊断，符号未注册 */

  /* 全局变量必须带编译期可折叠的右值（用户契约：init 可折叠为字面量或
     函数引用）。无 init（未初始化声明）无法折叠 → 报错。 */
  if (!vd->init || vd->init->kind == AST_UNDEF) {
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
               "global variable '%.*s' must have a compile-time initializer "
               "foldable to a literal or function",
               (int)vd->name.len, vd->name.ptr);
    return false;
  }

  /* 1. 表达式求值（shadow 类型检查 + 改写 init 中的 comptime 引用）。
     错误恢复产物（error/void shadow）跳过 ctfe（避免二次诊断） */
  value_t *sh = sema_expr(sema, &vd->init, scope);
  bool bad = value_is_error(sema->vm, sh) ||
             value_is_type(sh, TYPE_KIND_VOID);
  if (bad) return false;

  /* 2. 类型校验（显式类型 / 推断，与 comptime var 一致） */
  if (vd->type_expr) {
    if (!sym->type) {
      sym->type = sema_resolve_type_slot(sema, &vd->type_expr);
    }
    if (!sym->type) {
      /* 类型槽位解析失败（非法/未知类型名，如 "string"）：补报诊断防
         静默放行到运行时（对齐 shadow_var_def 的 unknown type 语义）。
         提取槽位名字区分 var 遮蔽来源（完全遮罩语义）。 */
      strslice_t tn = {0};
      if (vd->type_expr->kind == AST_IDENT) {
        tn = ((ast_ident_t *)vd->type_expr)->name;
      } else if (vd->type_expr->kind == AST_TYPE_REF) {
        tn = ((ast_type_ref_t *)vd->type_expr)->name;
      }
      sema_symbol_t *shadow = tn.ptr ? sema_lookup(scope, tn) : NULL;
      if (shadow && shadow->kind == SEMA_SYM_VAR) {
        diag_error(sema->diag, sema_loc(sema, vd->type_expr),
                   "'%.*s' is a variable, not a type", (int)tn.len, tn.ptr);
      } else {
        diag_error(sema->diag, sema_loc(sema, vd->type_expr),
                   "unknown type");
      }
      return false;
    }
    if (sym->type) {
      value_t *dst = value_make_shadow(sema->vm, sym->type);
      if (value_is_error(sema->vm, value_assign(sema->vm, dst, sh))) {
        char tn[64], itn[64];
        sema_type_name(sym->type, tn, sizeof(tn));
        sema_type_name(value_type(sh), itn, sizeof(itn));
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "cannot initialize global variable '%.*s' of type %s "
                   "with %s",
                   (int)vd->name.len, vd->name.ptr, tn, itn);
        return false;
      }
    }
  } else {
    sym->type = value_type(sh);
  }

  /* 3. ctfe 强制编译期求值（vm->comptime 状态标记）：init 必须可折叠为
     字面量/函数引用。引用运行期实体（局部/其他全局变量）或不可计算
     表达式 → ctfe error → 报错（诊断信息含 ctfe 原因）。 */
  vm_t *vm = sema->vm;
  bool saved = vm->comptime;
  vm->comptime = true;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.sema_scope = scope; /* 局部符号（函数/变量）按当前词法作用域解析 */
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
               "global variable '%.*s': initializer must be a compile-time "
               "constant foldable to a literal or function%s%s",
               (int)vd->name.len, vd->name.ptr, msg ? ": " : "",
               msg ? msg : "");
    return false;
  }

  /* 4. 编码常量 + 折叠 init AST 回字面量（写回 vd->init，compiler 直接
     编译发射字面量字节码）。 */
  sema_ct_const_t ct;
  if (!sema_ct_encode(sema, r, &ct)) {
    char tn[64];
    sema_type_name(value_type(r), tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, vd->init),
               "global variable '%.*s': value of type %s cannot be folded "
               "to a constant",
               (int)vd->name.len, vd->name.ptr, tn);
    return false;
  }
  ast_node_t *folded = sema_ct_lit(sema, vd->init, &ct);
  if (!folded) {
    diag_error(sema->diag, sema_loc(sema, vd->init),
               "global variable '%.*s': initializer cannot be folded to a "
               "literal",
               (int)vd->name.len, vd->name.ptr);
    return false;
  }
  folded->next = vd->init->next; /* 保留兄弟链 */
  vd->init = folded;

  /* 5. 符号激活 + VM shadow scope 定义：全局变量函数体内可见（pass_globals
     先于 Pass 3a/3b；sema_walk_function 的 VM scope 链参数层→捕获层→
     root_scope→global_scope 经 scope_lookup 查到 shadow value——与运行时
     DEFINE 落 root_scope 对齐）。不设 is_comptime/ct_valid——引用点不折叠
     （保持 AST_IDENT，运行期 PUSH 读 root_scope）。 */
  char nb[256];
  ctfe_slice_to_cstr(vd->name, nb, sizeof nb);
  value_t *sv =
      value_make_shadow(vm, sym->type ? sym->type : vm->type_void);
  scope_define(vm, vm->root_scope, nb, sv);
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
     报参数数量/类型错误，避免落入模糊的"非编译期常量"诊断）。
     按调用点作用域查符号（局部 comptime 函数符号在定义块，非全局） */
  ast_ident_t *name = (ast_ident_t *)call->callee;
  sema_symbol_t *csym = sema_lookup(scope, name->name);
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
  ctx.sema_scope = scope; /* 局部 comptime 函数符号按当前词法作用域解析 */
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

/* ===========================================================================
 * type 定义求值（type name = <type-expr>;）
 *
 * type 定义不是创建新类型，而是创建新的 type value 绑定到当前作用域：
 * rhs（类型表达式）在 sema 阶段即真实求值（type value 恒非 shadow，
 * data 为 type_t*），校验为类型值后：
 *   1. rhs 折叠为 AST_TYPE_REF（"__type_N" 名字引用，内建类型保持原
 *      AST_IDENT——编译器对 AST_IDENT 发 PUSH 从作用域查内建 type value）
 *   2. 绑定 type value 到编译期 vm 当前作用域（type_lookup / 后续引用
 *      经 sema_expr 的 scope_lookup 命中；运行时绑定由字节码 DEFINE
 *      完成——编译期 vm 与运行时 vm 完全解耦）
 *   3. 激活符号（TDZ：定义前引用不可见，与 var 一致）
 * =========================================================================== */

bool sema_eval_type_def(sema_t *sema, ast_type_def_t *td, sema_scope_t *scope) {
  if (!sema || !td) return false;
  sema_symbol_t *sym = sema_scope_find_local(scope, td->name);
  if (!sym) return false; /* 3a 重复定义已诊断，符号未注册 */

  vm_t *vm = sema->vm;

  /* 1. rhs 求值：type RHS 是类型表达式，须编译期真实求值得到 type value
     （data=type_t*）。走 ctfe 整体求值而非 sema_expr shadow 求值：
     - extends 二元表达式遇到即折叠为 bool（value_extends），三元用真实
       bool 惰性选分支（ctfe 与运行期语义一致）
     - 复合类型（AST_ARRAY）ctfe 也支持（intern 出数组类型）
     - 别名/内建类型名走 scope_lookup 拿 type value
     求值后统一由步骤 2 折叠为 AST_TYPE_REF（整个 rhs 收敛为类型引用，
     运行时 LOAD_TYPE，无 extends/三元残留）。 */
  bool saved = vm->comptime;
  vm->comptime = true;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *sh = ctfe_eval(&ctx, td->expr);
  vm->comptime = saved;
  if (!sh || value_is_error(vm, sh)) {
    const char *msg = NULL;
    if (sh) {
      error_data_t *ed = (error_data_t *)value_data(sh);
      msg = ed && ed->message ? string_cstr(ed->message) : NULL;
    }
    diag_error(sema->diag, sema_loc(sema, td->expr),
               "type definition '%.*s': expression is not a compile-time "
               "type computation%s%s",
               (int)td->name.len, td->name.ptr, msg ? ": " : "",
               msg ? msg : "");
    return false;
  }
  if (!value_is_type(sh, TYPE_KIND_TYPE)) {
    char tn[64];
    sema_type_name(value_type(sh), tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, td->expr),
               "type definition '%.*s' must evaluate to a type value, got %s",
               (int)td->name.len, td->name.ptr, tn);
    return false;
  }
  const type_t *t = value_as(sh, const type_t *);

  /* 2. rhs 折叠为 AST_TYPE_REF（登记 + 名字引用；内建类型不登记 → 名字 =
     规范名，编译器经 type_lookup 兜底 → LOAD_TYPE <内建 id>）。复合类型
     由 sema_ct_type_expr 递归登记，整个 rhs 收敛为单个类型引用。 */
  if (td->expr->kind != AST_TYPE_REF) {
    ast_node_t *folded = sema_ct_type_expr(sema, t, td->expr);
    if (folded) {
      folded->next = td->expr->next; /* 保留兄弟链 */
      td->expr = folded;
    }
  }

  /* 3. 绑定 type value 到编译期 vm 当前作用域（type_lookup / ctfe 引用
     需要；运行时绑定由字节码 DEFINE 完成，与编译期 vm 解耦）。 */
  value_t *tv = type_as_value(vm, t);
  if (!tv) return false;
  char nb[256];
  if (td->name.len >= sizeof nb) return false;
  memcpy(nb, td->name.ptr, td->name.len);
  nb[td->name.len] = '\0';
  scope_define(vm, vm->current_scope, nb, tv);

  /* 4. 符号激活（TDZ：定义前不可见） */
  sym->is_active = true;
  sym->flow_init = true;
  return true;
}
