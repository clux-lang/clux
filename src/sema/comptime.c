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
#include "parser/ast_enum_def.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "vm/function.h"
#include "vm/scope.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_struct.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_tuple.h"
#include "vm/type_ownership.h"
#include "vm/type_option.h"
#include "vm/type_error.h"
#include "vm/type_type.h"
#include "vm/value.h"
/* 结构体定义求值（含 `...Base;` 组合）——见 src/sema/struct_compose.c */
#include "sema/struct_compose.h"

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

/* 从真实枚举值读取底层整数值（int64，按底层宽度读——enum 的 data 是
   底层宽度的整数值块）。encode_int 同模式。 */
static int64_t enum_read_ct_int(const value_t *v) {
  const type_t *t = value_type(v);
  switch (t->size) {
  case 1: return (int64_t)*(const int8_t  *)value_data(v);
  case 2: return (int64_t)*(const int16_t *)value_data(v);
  case 4: return (int64_t)*(const int32_t *)value_data(v);
  default: return *(const int64_t *)value_data(v);
  }
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
      t == vm->type_u32 || t == vm->type_u64 || t == vm->type_usize) {
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
    /* 字符串：复制到 sema arena（跨 sema/compile 阶段安全）。
       data 是池内指针（全平凡） */
    const char *sv = *(const char *const *)value_data(v);
    size_t len = sv ? strlen(sv) : 0;
    const char *c = sv ? sv : "";
    char *buf = (char *)arena_calloc(sema->arena, 1, len + 1,
                                     ALIGNOF(max_align_t));
    if (!buf) return false;
    memcpy(buf, c, len + 1);
    out->s = strslice_from_bytes(buf, len);
    return true;
  }
  if (type_kind_of(t) == TYPE_KIND_ARRAY) {
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
  if (type_kind_of(t) == TYPE_KIND_STRUCT || type_kind_of(t) == TYPE_KIND_TUPLE) {
    /* struct/tuple 常量（comptime var 支持所有类型）：按类型表序逐字段递归编码，
       `elems` 复用为字段值数组（与数组元素同一表示）。字段值以**借用 value**
       （value_make_borrowed 指向目标块内的字段偏移）递归编码，布局语义与运行期
       op_construct 的 struct/tuple 分支一致。 */
    size_t count = (type_kind_of(t) == TYPE_KIND_STRUCT) ? struct_type_field_count(t)
                                                 : tuple_type_elem_count(t);
    out->count = count;
    if (count == 0) {
      out->elems = NULL;
      return true;
    }
    sema_ct_const_t *es = (sema_ct_const_t *)arena_calloc(
        sema->arena, count, sizeof(sema_ct_const_t), ALIGNOF(max_align_t));
    if (!es) return false;
    const char *base = (const char *)value_data(v);
    for (size_t i = 0; i < count; i++) {
      const type_t *ft = NULL;
      size_t off = 0;
      if (type_kind_of(t) == TYPE_KIND_STRUCT) {
        const struct_field_t *f = struct_type_field(t, i);
        if (f) { ft = f->type; off = f->offset; }
      } else {
        const tuple_elem_t *e = tuple_type_elem(t, i);
        if (e) { ft = e->type; off = e->offset; }
      }
      if (!ft) return false;
      value_t *fv = value_make_borrowed(vm, ft, (void *)(base + off));
      if (!fv || !sema_ct_encode(sema, fv, &es[i])) return false;
    }
    out->elems = es;
    return true;
  }
  if (type_kind_of(t) == TYPE_KIND_ENUM) {
    /* 枚举常量：底层恒为整型，编码底层整数值（i 字段按底层宽度读）。
       折叠为 AST_SCOPE_REF（compiler 发 LOAD_TYPE + PUSH_I* + MAKE_ENUM，
       与源码引用 Color::Red 同构）。 */
    out->i = enum_read_ct_int(v);
    return true;
  }
  if (type_kind_of(t) == TYPE_KIND_FUNC) {
    /* 函数引用（comptime 函数体内引用的普通/内建函数值，或函数字面量值）：
       data 存 func_t*（CTFE 经 func_new_program_ref 构造的引用对象，或 scope
       里真实函数值）。编码函数 id（func_t->id：sema 分配的程序 fid 或内建
       id），折叠为 AST_FUNC_REF（compiler 发 LOAD_FUNCTION <id>）——与函数
       是否有 name 无关（匿名字面量无名字也可折叠）。
       带捕获的闭包（CTFE 已实例化出 env）额外编码捕获表：折叠时生成
       MAKE_FUNCTION + SET_CLOSURE 序列，否则运行期调用会报
       "closure has no captures"。 */
    func_t *fn = *(func_t **)value_data(v);
    if (!fn) return false;
    out->func_id = fn->id;
    out->lit_ast = (const struct ast_node *)fn->ct_literal;
    if (fn->capture_type && fn->env) {
      const type_t *ct = &fn->capture_type->base;
      size_t count = struct_type_field_count(ct);
      if (count > 0) {
        sema_ct_capture_t *caps = (sema_ct_capture_t *)arena_calloc(
            sema->arena, count, sizeof(sema_ct_capture_t),
            ALIGNOF(max_align_t));
        if (!caps) return false;
        const char *base = (const char *)fn->env;
        for (size_t i = 0; i < count; i++) {
          const struct_field_t *f = struct_type_field(ct, i);
          if (!f || !f->type) return false;
          caps[i].name = f->name; /* type_struct_intern 深拷贝过捕获名 */
          value_t *fv =
              value_make_borrowed(vm, f->type, (void *)(base + f->offset));
          if (!fv || !sema_ct_encode(sema, fv, &caps[i].value)) return false;
        }
        out->captures = caps;
        out->capture_count = count;
      }
    }
    return true;
  }
  return false; /* void/type/其他复合：不可折叠 */
}

/* 从常量类型构造类型表达式 AST（折叠写回 AST_CONSTRUCT 的类型位用）：
   登记类型到 sema->types 队列 → 产出 AST_TYPE_REF。程序类型名字 =
   登记名（"__type_N"），hoist 提升区负责构造，编译器零感知；内建类型
   不登记 → 名字 = 内建规范名（"i32"），编译器经 type_lookup 兜底 →
   LOAD_TYPE <内建 id>（别名透明）。复合类型结构依赖由登记递归覆盖。
   位置借用 origin（折叠节点位置，诊断定位不变）。
   公共入口：union 构造分支改写内层 member 构造的 type 位（member 名 →
   payload 类型 → AST_TYPE_REF）也用此函数。 */
ast_node_t *sema_ct_type_ref(sema_t *sema, const type_t *t,
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
      t == vm->type_u32 || t == vm->type_u64 || t == vm->type_usize) {
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
  if (type_kind_of(t) == TYPE_KIND_ARRAY) {
    /* 复杂类型常量：写回为 AST_CONSTRUCT（.<type>{ fields }）节点。
       type 位递归构造类型表达式（[N]T → AST_ARRAY 嵌套），fields 为
       逐个元素递归折叠的字面量兄弟链。下游 compiler 按标准构造路径
       （compile_type_expr + CONSTRUCT N）编译，零感知。 */
    ast_node_t *type_expr = sema_ct_type_ref(sema, t, origin);
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
  if (type_kind_of(t) == TYPE_KIND_STRUCT || type_kind_of(t) == TYPE_KIND_TUPLE) {
    /* struct/tuple 常量：与数组同机制折叠为 AST_CONSTRUCT——type 位递归构造
       类型表达式，fields 为按**类型表序**位置化的字面量兄弟链（struct 用具名
       构造亦可，位置化更简单且运行期 op_construct 按表序布值，语义等价）。 */
    ast_node_t *type_expr = sema_ct_type_ref(sema, t, origin);
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
  if (type_kind_of(t) == TYPE_KIND_FUNC) {
    /* 带捕获的闭包常量：折叠为**克隆的 func 字面量**——共享原字面量的
       params/body/fid/capture_type_id，把 captures 换成"括号捕获"形态
       （`|(name: T = <值常量>)|`）。compiler 对函数字面量本就发
       MAKE_FUNCTION <fid> + 逐捕获 <值> + SET_CLOSURE <name>，因此**零感知**，
       不需要新的 AST kind / 指令。 */
    if (ct->capture_count > 0 && ct->captures) {
      ast_func_def_t *orig = NULL;
      /* 优先用 CTFE 记下的字面量 AST：pass_globals 阶段该字面量可能还没登记
         进 sema->funcs（Pass 3b 才 walk 到函数体里的字面量）。 */
      if (ct->lit_ast && ct->lit_ast->kind == AST_FUNC_DEF) {
        orig = (ast_func_def_t *)ct->lit_ast;
      } else {
        sema_func_t *sf = sema_func_by_id(sema, ct->func_id);
        if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
          orig = (ast_func_def_t *)sf->def;
      }
      if (!orig) return NULL;
      ast_func_def_t *lit = (ast_func_def_t *)arena_calloc(
          arena, 1, sizeof(ast_func_def_t), ALIGNOF(max_align_t));
      if (!lit) return NULL;
      *lit = *orig; /* 共享参数/函数体/fid/签名/捕获类型 id */
      lit->base.next = NULL;
      lit->captures = NULL;
      ast_node_t *head = NULL, *last = NULL;
      for (size_t i = 0; i < ct->capture_count; i++) {
        ast_node_t *cn =
            ast_var_def_new(arena, origin->tok_begin, origin->tok_end);
        if (!cn) return NULL;
        ast_var_def_t *cv = (ast_var_def_t *)cn;
        cv->name = ct->captures[i].name;
        cv->type_expr =
            sema_ct_type_ref(sema, ct->captures[i].value.type, origin);
        cv->init = sema_ct_lit(sema, origin, &ct->captures[i].value);
        if (!cv->init) return NULL;
        if (last)
          last->next = cn;
        else
          head = cn;
        last = cn;
      }
      lit->captures = head;
      return &lit->base;
    }
    /* 纯函数引用常量：折叠为 AST_FUNC_REF（纯 id 标识——compiler 直接发
       LOAD_FUNCTION <fid> 加载真实函数值，与函数是否有 name 无关：
       匿名字面量无名字也可折叠）。 */
    ast_node_t *ref =
        ast_func_ref_new(arena, origin->tok_begin, origin->tok_end);
    if (!ref) return NULL;
    ((ast_func_ref_t *)ref)->fid = ct->func_id;
    return ref;
  }
  if (type_kind_of(t) == TYPE_KIND_ENUM) {
    /* 枚举常量：折叠为 AST_SCOPE_REF（type_expr 登记为 AST_TYPE_REF +
       底层整数值）。variant 名留空——compiler 分支只用 type_expr/value
       （LOAD_TYPE + PUSH_I* + MAKE_ENUM），与源码引用同构。 */
    ast_node_t *type_expr = sema_ct_type_ref(sema, t, origin);
    if (!type_expr) return NULL;
    ast_node_t *ref =
        ast_scope_ref_new(arena, origin->tok_begin, origin->tok_end);
    if (!ref) return NULL;
    ast_scope_ref_t *er = (ast_scope_ref_t *)ref;
    er->type_expr = type_expr;
    er->value     = ct->i;
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
  bool is_global = (scope == sema->global_scope);
  if (vd->type_expr) {
    if (!sym->type) {
      sym->type = sema_resolve_type_slot(sema, &vd->type_expr);
    }
    if (sym->type) {
      /* M5 约束：全局 comptime var 类型必须是 const T（值在 sema 求值后
         编译为字节码常量，不可变更；防止 import 顺序导致值不一致）。
         局部 comptime var 不受此约束。 */
      if (is_global && !type_is_const(sym->type)) {
        char tn[64];
        sema_type_name(sym->type, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
                   "global comptime variable '%.*s' type must be const "
                   "(got '%s'); use 'const %s' or omit the type to auto-infer",
                   (int)vd->name.len, vd->name.ptr, tn, tn);
        return false;
      }
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
    /* 无显式类型 → 推断。全局 comptime var 自动加 const 修饰
       （const T = init 表达式类型）。局部 comptime var 原样推断。 */
    if (is_global) {
      sym->type = type_const_intern(sema->vm, value_type(sh));
      if (!sym->type) sym->type = value_type(sh); /* 兜底 */
    } else {
      sym->type = value_type(sh);
    }
  }

  /* 3. ctfe 编译期真实求值（comptime 与 shadow 的区别在值的形态：shadow 的
     data==NULL，ctfe 产出带真实 data 的 value，无需模式标志） */
  vm_t *vm = sema->vm;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.sema_scope = scope; /* 局部符号（函数/变量）按当前词法作用域解析 */
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, vd->init);

  if (!r || value_is_error(vm, r)) {
    const char *msg = NULL;
    if (r) {
      error_data_t *ed = (error_data_t *)value_data(r);
      msg = ed && ed->message ? ed->message : NULL;
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

  /* 5. 常量值绑定进 VM 作用域（**常量槽位解析的前提**）。
     常量槽位（数组边界 `[N]T`、type 别名计算、enum 值、make 长度……）由
     CTFE 求值，而 CTFE 的标识符解析只查 `vm->current_scope` 这条链；
     comptime var 不生成运行期字节码，运行期 scope 里没有它——必须把**真实
     值**（非 shadow）落在链上，`[N]i32` 之类写法才能被解析到。
     落点与普通变量的 shadow value 保持一致：
       - 全局：`vm->root_scope`（3b 的函数体 VM scope 链
         参数层→捕获层→root_scope 经 scope_lookup 可查到）；
       - 局部：当前块作用域（定义点同级；嵌套块经 parent 链可见）。
     `scope_define` 会把值 clone 到目标作用域（生命周期归该作用域 owned），
     因此这里可以直接交 CTFE 产出的 `r`。 */
  char nb[256];
  ctfe_slice_to_cstr(vd->name, nb, sizeof nb);
  scope_define(vm, is_global ? vm->root_scope : vm->current_scope, nb, r);
  return true;
}

/* ===========================================================================
 * 全局变量求值（运行时实体，init 编译期折叠）
 * =========================================================================== */

/* §10 全局规则（m3-design §10）：禁止全局 own（含任何嵌套位置）。
   递归判定类型是否含 own 指针（顶层 own *T 或 struct/tuple/union/cunion
   字段、option/const/volatile 内层、数组元素）。全局变量类型含 own →
   不合法（检查器失去确定性：undefined 延迟初始化无法保守感知何时赋值）。 */
static bool type_contains_own(const type_t *t) {
  if (!t) return false;
  switch (t->kind) {
    case TYPE_KIND_PTR:
      /* §10 禁止全局 own 指针（own 需作用域退出销毁，
         全局静态期无法完成）。ref/fatal 不持有资源，允许。
         所有权已从 type 层迁移到 symbol/value 层——类型不携带 ownership，
         顶层指针/切片的 own 判定由调用方通过 sema_sym_ownership(sym) 检查。 */
      return false;
    case TYPE_KIND_SLICE:
      /* §9.4 禁止全局 own []T（同 own *T）。ref/fatal 不持有资源，允许。
         同上，类型不携带 ownership。 */
      return false;
    case TYPE_KIND_STRUCT: {
      size_t n = struct_type_field_count(t);
      for (size_t i = 0; i < n; i++) {
        const struct_field_t *f = struct_type_field(t, i);
        if (f && type_contains_own(f->type)) return true;
      }
      return false;
    }
    case TYPE_KIND_TUPLE: {
      size_t n = tuple_type_elem_count(t);
      for (size_t i = 0; i < n; i++) {
        const tuple_elem_t *e = tuple_type_elem(t, i);
        if (e && type_contains_own(e->type)) return true;
      }
      return false;
    }
    case TYPE_KIND_UNION: {
      size_t n = union_type_member_count(t);
      for (size_t i = 0; i < n; i++) {
        const union_member_t *m = union_type_member(t, i);
        if (m && type_contains_own(m->payload_type)) return true;
      }
      return false;
    }
    case TYPE_KIND_CUNION: {
      size_t n = cunion_type_member_count(t);
      for (size_t i = 0; i < n; i++) {
        const cunion_member_t *m = cunion_type_member(t, i);
        if (m && type_contains_own(m->type)) return true;
      }
      return false;
    }
    case TYPE_KIND_OPTION:
      return type_contains_own(type_option_inner(t));
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
      return type_contains_own(type_qualifier_sub(t));
    case TYPE_KIND_ARRAY:
      return type_contains_own(array_type_elem(t));
    default:
      return false;
  }
}

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

  /* §10 全局规则（m3-design §10）：禁止全局 own（含任何嵌套位置）。
     全局变量类型含 own 指针 → 不合法（undefined 延迟初始化使检查器
     失去确定性）。 */
  if (sym->type && type_contains_own(sym->type)) {
    char tn[64];
    sema_type_name(sym->type, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
               "global variable '%.*s' of type '%s' contains own pointer and "
               "cannot be global (own requires deterministic scope-bound "
               "lifecycle; use a local variable)",
               (int)vd->name.len, vd->name.ptr, tn);
    return false;
  }

  /* 3. ctfe 编译期真实求值：init 必须可折叠为
     字面量/函数引用。引用运行期实体（局部/其他全局变量）或不可计算
     表达式 → ctfe error → 报错（诊断信息含 ctfe 原因）。 */
  vm_t *vm = sema->vm;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.sema_scope = scope; /* 局部符号（函数/变量）按当前词法作用域解析 */
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, vd->init);

  if (!r || value_is_error(vm, r)) {
    const char *msg = NULL;
    if (r) {
      error_data_t *ed = (error_data_t *)value_data(r);
      msg = ed && ed->message ? ed->message : NULL;
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
          ed && ed->message ? ed->message : "call failed";
      diag_error(sema->diag, sema_loc(sema, *node), "%s", msg);
      return value_make_shadow(vm, vm->type_void);
    }
  }

  /* 2. ctfe 编译期真实求值整个调用 */
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.sema_scope = scope; /* 局部 comptime 函数符号按当前词法作用域解析 */
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, *node);

  if (!r || value_is_error(vm, r)) {
    const char *msg = NULL;
    if (r) {
      error_data_t *ed = (error_data_t *)value_data(r);
      msg = ed && ed->message ? ed->message : NULL;
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
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.vm = vm;
  ctx.sema = sema;
  /* 作用域：CTFE 的标识符回退（ctfe_operand_type）按 sema 作用域链取符号的
     **静态类型**——`type T = typeof(a)` 里的局部变量 a 在块入口提升求值时还没
     进 VM scope，只能从 sema 符号表拿类型。不传 scope 时该回退会跳过局部符号，
     误报 "ctfe: cannot determine operand type"。 */
  ctx.sema_scope = scope;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *sh = ctfe_eval(&ctx, td->expr);
  if (!sh || value_is_error(vm, sh)) {
    const char *msg = NULL;
    if (sh) {
      error_data_t *ed = (error_data_t *)value_data(sh);
      msg = ed && ed->message ? ed->message : NULL;
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
     由 sema_ct_type_ref 递归登记，整个 rhs 收敛为单个类型引用。 */
  if (td->expr->kind != AST_TYPE_REF) {
    ast_node_t *folded = sema_ct_type_ref(sema, t, td->expr);
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

/* ===========================================================================
 * enum 定义求值（enum Name:Underlying { Var = val, ... }）
 *
 * enum 是"有特殊类型的全局变量"：variant 是编译期常量（全局），类型为
 * enum 类型（enum_type_t，TYPE_KIND_ENUM）。定义点在 pass1b 与全局 type
 * def 一起求值（先于函数签名解析——签名/变量可引用 enum 类型）：
 *   1. 底层类型解析（sema_resolve_type_slot）→ 必须 TYPE_KIND_INT（用户
 *      契约：底层须整型；bool/str/复合报错）
 *   2. 逐 variant 值 ctfe 求值 → 必须整型常量；值按底层
 *      类型校验兼容（value_assign 复用整型"只拓宽"语义——i32 赋给 i8 底层
 *      报错，符合"值与类型不兼容编译报错"契约）；重复名/重复值报错
 *   3. type_enum_intern 构造 enum 类型（立即密封 + 去重 intern）
 *   4. sema_type_register 登记（hoist 构造用）+ scope_define 绑定 type
 *      value 到编译期 vm 作用域（var c: Color 经 type_lookup 解析）
 *   5. 激活符号；定义点保留（进字节码，运行时 hoist 构造 + 名字绑定）
 * =========================================================================== */

bool sema_eval_enum_def(sema_t *sema, ast_enum_def_t *ed, sema_scope_t *scope) {
  if (!sema || !ed) return false;
  sema_symbol_t *sym = sema_scope_find_local(scope, ed->name);
  if (!sym) return false; /* pass1 重复定义已诊断，符号未注册 */

  vm_t *vm = sema->vm;

  /* 1. 底层类型解析 + 校验（须整型） */
  const type_t *underlying =
      sema_resolve_type_slot(sema, &ed->underlying_type);
  if (!underlying) {
    diag_error(sema->diag, sema_loc(sema, ed->underlying_type),
               "enum '%.*s': unknown underlying type",
               (int)ed->name.len, ed->name.ptr);
    return false;
  }
  bool is_int = (underlying == vm->type_i8 || underlying == vm->type_i16 ||
                 underlying == vm->type_i32 || underlying == vm->type_i64 ||
                 underlying == vm->type_u8 || underlying == vm->type_u16 ||
                 underlying == vm->type_u32 || underlying == vm->type_u64 ||
                 underlying == vm->type_usize);
  if (!is_int) {
    char tn[64];
    sema_type_name(underlying, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, ed->underlying_type),
               "enum '%.*s': underlying type must be an integer, got %s",
               (int)ed->name.len, ed->name.ptr, tn);
    return false;
  }

  /* 2. 逐 variant 值 ctfe 求值 + 收集 variant 表 */
  size_t n = 0;
  for (ast_node_t *vn = ed->variants; vn; vn = vn->next) n++;
  enum_variant_t *variants = NULL;
  if (n > 0) {
    variants = (enum_variant_t *)allocator_new_ex(
        vm->alloc, "enum_variant_t", sizeof(enum_variant_t), NULL, NULL, NULL,
        n);
    if (!variants) panic("sema: out of memory allocating enum variants");
  }

  size_t i = 0;
  bool ok = true;
  for (ast_node_t *vn = ed->variants; vn; vn = vn->next, i++) {
    ast_enum_variant_t *ev = (ast_enum_variant_t *)vn;

    /* ctfe 编译期求值 variant 值（与 comptime var / type def 同协议）。
       值须为编译期整型常量：函数调用（pass1b 阶段函数 AST 未绑定）由
       ctfe AST_CALL 防御分支干净报错，不落 sema_expr（会误报变量 TDZ）。 */
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vm = vm;
    ctx.sema = sema;
    ctx.sema_scope = scope;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *val = ctfe_eval(&ctx, ev->value);
    if (!val || value_is_error(vm, val)) {
      const char *msg = NULL;
      if (val) {
        error_data_t *ed_ = (error_data_t *)value_data(val);
        msg = ed_ && ed_->message ? ed_->message : NULL;
      }
      diag_error(sema->diag, sema_loc(sema, ev->value),
                 "enum '%.*s': variant '%.*s' value must be a compile-time "
                 "integer constant%s%s",
                 (int)ed->name.len, ed->name.ptr, (int)ev->name.len,
                 ev->name.ptr, msg ? ": " : "", msg ? msg : "");
      ok = false;
      continue;
    }
    const type_t *vt = value_type(val);
    bool is_int_v = (vt == vm->type_i8 || vt == vm->type_i16 ||
                     vt == vm->type_i32 || vt == vm->type_i64 ||
                     vt == vm->type_u8 || vt == vm->type_u16 ||
                     vt == vm->type_u32 || vt == vm->type_u64 ||
                     vt == vm->type_usize);
    if (!is_int_v) {
      char tn[64];
      sema_type_name(vt, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, ev->value),
                 "enum '%.*s': variant '%.*s' value must be an integer, got %s",
                 (int)ed->name.len, ed->name.ptr, (int)ev->name.len,
                 ev->name.ptr, tn);
      ok = false;
      continue;
    }

    /* 值与底层类型兼容性校验（用户契约：i32 赋给 i8 底层编译报错）：
       value_assign 复用整型"只拓宽"语义——窄化/跨符号（u→s）报 error */
    value_t *dst = value_make_shadow(vm, underlying);
    value_t *res = value_assign(vm, dst, val);
    if (value_is_error(vm, res)) {
      char itn[64], btn[64];
      sema_type_name(vt, itn, sizeof(itn));
      sema_type_name(underlying, btn, sizeof(btn));
      diag_error(sema->diag, sema_loc(sema, ev->value),
                 "enum '%.*s': variant '%.*s' value of type %s is not "
                 "compatible with underlying type %s",
                 (int)ed->name.len, ed->name.ptr, (int)ev->name.len,
                 ev->name.ptr, itn, btn);
      ok = false;
      continue;
    }

    /* 查重：重复名 / 重复值报错 */
    bool dup_name = false, dup_val = false;
    for (size_t j = 0; j < i; j++) {
      if (variants[j].name.len == ev->name.len && variants[j].name.ptr &&
          memcmp(variants[j].name.ptr, ev->name.ptr, ev->name.len) == 0)
        dup_name = true;
      if (variants[j].value == enum_read_ct_int(val)) dup_val = true;
    }
    if (dup_name) {
      diag_error(sema->diag, sema_loc(sema, vn),
                 "enum '%.*s': duplicate variant name '%.*s'",
                 (int)ed->name.len, ed->name.ptr, (int)ev->name.len,
                 ev->name.ptr);
      ok = false;
      continue;
    }
    if (dup_val) {
      diag_error(sema->diag, sema_loc(sema, vn),
                 "enum '%.*s': duplicate variant value %lld",
                 (int)ed->name.len, ed->name.ptr,
                 (long long)enum_read_ct_int(val));
      ok = false;
      continue;
    }

    variants[i].name  = ev->name;
    variants[i].value = enum_read_ct_int(val);
  }

  if (!ok) {
    if (variants) allocator_free(vm->alloc, (void **)&variants);
    return false;
  }

  /* 3. type_enum_intern 构造 enum 类型（立即密封 + 去重 intern） */
  const type_t *t = type_enum_intern(vm, underlying, variants, n);
  if (variants) allocator_free(vm->alloc, (void **)&variants);
  if (!t) return false;

  /* 4. 登记（hoist 构造用）+ 绑定 type value 到编译期 vm 作用域 */
  const sema_type_t *st = sema_type_register(sema, t);
  if (!st) return false;
  ed->type_id = st->id; /* compiler 顶层名字绑定 LOAD_TYPE 用 */
  /* 显示名（诊断/值 dump 用）：须在 sema_type_register 分配 id 后调用——
     type_set_name 要求 t->id >= TYPE_ID_PROGRAM_BASE 才可改名。 */
  type_set_name(vm, t, ed->name);
  value_t *tv = type_as_value(vm, t);
  if (!tv) return false;
  char nb[256];
  if (ed->name.len >= sizeof nb) return false;
  memcpy(nb, ed->name.ptr, ed->name.len);
  nb[ed->name.len] = '\0';
  scope_define(vm, vm->current_scope, nb, tv);

  /* 5. 符号激活（TDZ：定义前不可见） */
  sym->type = t;
  sym->is_active = true;
  sym->flow_init = true;
  return true;
}

/* ===========================================================================
 * struct 定义求值（struct Name { field: type; ... }）
 *
 * struct 是"具名字段表类型"：字段类型表（field_t { name, offset, type }）
 * 密封时按 C 对齐布局。定义点在 pass1b 与全局 type/enum def 一起求值
 * （先于函数签名解析——签名/变量可引用 struct 类型）。
 *
 * 实现收敛在 struct_compose.c 的 `sema_struct_compose_def`：它按用户规格在类型
 * **seal 之前**（开放对象窗口）完成字段表构造与结构体组合（`...Base;` 的字段
 * 按位置插入 + 嵌入表登记），然后**只 seal 一次**。由此：
 *   - 类型的属性/元数据（含未来装饰器对类型的修改）只能在这个窗口里编辑；
 *     seal 之后不可变（VM 写入 API 拒绝 sealed 类型，见 include/vm/type_struct.h
 *     的不变量）；
 *   - `type_t*` 身份由"同一个开放对象就地 seal"保持（sema_type_register /
 *     types_by_id / CTFE 拿到的都是 seal 的返回值）。
 *
 * 本函数保留为语义入口（全局 pass1b 与局部块提升都调它），返回 bool 以兼容
 * 既有调用点：DEFERRED（依赖尚未就绪）与 FAILED 都返回 false——前者不激活
 * 符号（交给不动点轮次/组合遍历再试），后者诊断已由组合入口记录。
 * =========================================================================== */

bool sema_eval_struct_def(sema_t *sema, ast_struct_def_t *sd, sema_scope_t *scope) {
  if (!sema || !sd) return false;
  return sema_struct_compose_def(sema, sd, scope) == SEMA_COMPOSE_OK;
}

/* ===========================================================================
 * tag union 定义求值（union Name { field: type; ... }）
 *
 * union 与 struct 同构语法：字段直接平铺，每个字段即一个 member（member 名
 * = tag 名，member 类型 = payload 类型）。定义点在 pass1b 与全局
 * type/enum/struct def 一起求值（先于函数签名解析）：
 *   1. 逐字段类型解析（sema_resolve_type_slot 折叠为 AST_TYPE_REF）→
 *      未知/非类型槽位报错；字段名查重（member 名 = tag 名，须全局唯一）
 *   2. type_union_intern 构造 union 类型（拷贝 member 表 + tag 前缀/联合体
 *      布局 + 去重 intern，立即密封）
 *   3. sema_type_register 登记（hoist 构造用）+ scope_define 绑定 type
 *      value 到编译期 vm 作用域（var s: Shape 经 type_lookup 解析）
 *   4. 激活符号；定义点保留（进字节码，运行时 hoist 构造 + 名字绑定）
 * =========================================================================== */

bool sema_eval_union_def(sema_t *sema, ast_union_def_t *ud, sema_scope_t *scope) {
  if (!sema || !ud) return false;
  sema_symbol_t *sym = sema_scope_find_local(scope, ud->name);
  if (!sym) return false; /* pass1 重复定义已诊断，符号未注册 */

  vm_t *vm = sema->vm;

  /* 1. 逐字段（member）类型解析 + 查重（字段名 = tag 名，重复报错） */
  size_t n = 0;
  for (ast_node_t *fn = ud->fields; fn; fn = fn->next) n++;
  union_member_t *members = NULL;
  if (n > 0) {
    members = (union_member_t *)allocator_new_ex(
        vm->alloc, "union_member_t", sizeof(union_member_t), NULL, NULL, NULL,
        n);
    if (!members) panic("sema: out of memory allocating union members");
  }

  size_t i = 0;
  bool ok = true;
  for (ast_node_t *fn = ud->fields; fn; fn = fn->next, i++) {
    ast_struct_field_t *sf = (ast_struct_field_t *)fn;
    members[i].name         = sf->name;
    members[i].tag          = (uint32_t)i;
    members[i].payload_type = NULL;

    /* 字段名（member 名 = tag 名）查重 */
    bool dup = false;
    for (size_t j = 0; j < i; j++) {
      if (members[j].name.len == sf->name.len && members[j].name.ptr &&
          memcmp(members[j].name.ptr, sf->name.ptr, sf->name.len) == 0) {
        diag_error(sema->diag, sema_loc(sema, fn),
                   "union '%.*s': duplicate member name '%.*s'",
                   (int)ud->name.len, ud->name.ptr, (int)sf->name.len,
                   sf->name.ptr);
        ok = false;
        dup = true;
        break;
      }
    }
    if (dup) continue;

    /* 槽位所有权读**解析期缓存**（推测轮折叠后 token 已丢失）。 */
    /* 字段类型解析（sema_resolve_type_slot：折叠为 AST_TYPE_REF，别名透明）
       ——内建类型名与已登记的具名类型均可用；未知类型报错。 */
    const type_t *ft = sema_resolve_type_slot(sema, &sf->type);
    if (!ft) {
      diag_error(sema->diag, sema_loc(sema, sf->type),
                 "union '%.*s': unknown field type for '%.*s'",
                 (int)ud->name.len, ud->name.ptr, (int)sf->name.len,
                 sf->name.ptr);
      ok = false;
      continue;
    }
    /* tag union 的 member **可以携带指针/切片**（2026-10 放开）。
       曾经禁止是因为放开后运行期栈溢出（0xC00000FD）；根因是 sema 在
       Pass 3b 重新 walk 自己生成的销毁语句（`if (v is m) { FREE v.m }`）：
       AST_IF 会去消费 3a 没建的作用域、AST_IS_TAG 不是用户表达式。
       修复：生成节点用 `ast_node_t::is_generated` 标记、walk_stmt 跳过
       （见 stmt.c）。销毁序列本身由 gen_destroy_union 按 type 对象的
       槽位所有权生成（`if (v is m) { <payload 销毁序列> }`），
       member 的槽位所有权在解析期缓存（sf->ownership），并参与 intern。 */
    members[i].payload_type = ft;
    /* 槽位所有权（类型描述的一部分；union 的销毁序列按 tag 生成 if） */
  }

  if (!ok) {
    if (members) allocator_free(vm->alloc, (void **)&members);
    return false;
  }

  /* 2. type_union_intern 构造 union 类型（拷贝 member 表 + tag 前缀/联合体
     布局 + 去重 intern，立即密封） */
  const type_t *t = type_union_intern(vm, members, n);
  if (members) allocator_free(vm->alloc, (void **)&members);
  if (!t) return false;

  /* 3. 登记（hoist 构造用）+ 绑定 type value 到编译期 vm 作用域 */
  const sema_type_t *st = sema_type_register(sema, t);
  if (!st) return false;
  ud->type_id = st->id; /* compiler 顶层名字绑定 LOAD_TYPE 用 */
  /* 显示名（诊断/值 dump 用）：须在 sema_type_register 分配 id 后调用——
     type_set_name 要求 t->id >= TYPE_ID_PROGRAM_BASE 才可改名。 */
  type_set_name(vm, t, ud->name);
  value_t *tv = type_as_value(vm, t);
  if (!tv) return false;
  char nb[256];
  if (ud->name.len >= sizeof nb) return false;
  memcpy(nb, ud->name.ptr, ud->name.len);
  nb[ud->name.len] = '\0';
  scope_define(vm, vm->current_scope, nb, tv);

  /* 4. 符号激活（TDZ：定义前不可见） */
  sym->type = t;
  sym->is_active = true;
  sym->flow_init = true;
  return true;
}

/* ===========================================================================
 * C 语义 union 定义求值（cunion Name { field: type; ... }）
 *
 * cunion 与 struct 同构语法：字段直接平铺，每个字段即一个 member（member 名
 * = 字段名，member 类型 = 字段类型）。定义点在 pass1b 与全局
 * type/enum/struct/union def 一起求值（先于函数签名解析）：
 *   1. 逐字段类型解析（sema_resolve_type_slot 折叠为 AST_TYPE_REF）→
 *      未知/非类型槽位报错；字段名查重（member 名须全局唯一）
 *   2. type_cunion_intern 构造 cunion 类型（拷贝 member 表 + 布局
 *      size=max/align=max/offset 全 0 + 去重 intern，立即密封）
 *   3. sema_type_register 登记（hoist 构造用）+ scope_define 绑定 type
 *      value 到编译期 vm 作用域（var s: C 经 type_lookup 解析）
 *   4. 激活符号；定义点保留（进字节码，运行时 hoist 构造 + 名字绑定）
 *
 * 与 tag union 的区别：无 tag——member 不携带 tag 编号；布局共享 offset 0，
 * 不做安全检查（开发者自负安全，对齐 C union FFI）。
 * =========================================================================== */

bool sema_eval_cunion_def(sema_t *sema, ast_cunion_def_t *cd, sema_scope_t *scope) {
  if (!sema || !cd) return false;
  sema_symbol_t *sym = sema_scope_find_local(scope, cd->name);
  if (!sym) return false; /* pass1 重复定义已诊断，符号未注册 */

  vm_t *vm = sema->vm;

  /* 1. 逐字段（member）类型解析 + 查重（member 名重复报错） */
  size_t n = 0;
  for (ast_node_t *fn = cd->fields; fn; fn = fn->next) n++;
  cunion_member_t *members = NULL;
  if (n > 0) {
    members = (cunion_member_t *)allocator_new_ex(
        vm->alloc, "cunion_member_t", sizeof(cunion_member_t), NULL, NULL, NULL,
        n);
    if (!members) panic("sema: out of memory allocating cunion members");
  }

  size_t i = 0;
  bool ok = true;
  for (ast_node_t *fn = cd->fields; fn; fn = fn->next, i++) {
    ast_struct_field_t *sf = (ast_struct_field_t *)fn;
    members[i].name = sf->name;
    members[i].type = NULL;

    /* 字段名（member 名）查重 */
    bool dup = false;
    for (size_t j = 0; j < i; j++) {
      if (members[j].name.len == sf->name.len && members[j].name.ptr &&
          memcmp(members[j].name.ptr, sf->name.ptr, sf->name.len) == 0) {
        diag_error(sema->diag, sema_loc(sema, fn),
                   "cunion '%.*s': duplicate member name '%.*s'",
                   (int)cd->name.len, cd->name.ptr, (int)sf->name.len,
                   sf->name.ptr);
        ok = false;
        dup = true;
        break;
      }
    }
    if (dup) continue;

    /* 字段类型解析（sema_resolve_type_slot：折叠为 AST_TYPE_REF，别名透明）
       ——内建类型名与已登记的具名类型均可用；未知类型报错。 */
    const type_t *ft = sema_resolve_type_slot(sema, &sf->type);
    if (!ft) {
      diag_error(sema->diag, sema_loc(sema, sf->type),
                 "cunion '%.*s': unknown field type for '%.*s'",
                 (int)cd->name.len, cd->name.ptr, (int)sf->name.len,
                 sf->name.ptr);
      ok = false;
      continue;
    }
    /* 槽位所有权读**解析期缓存**（推测轮折叠后 token 已丢失）。
       **cunion 不参与所有权运算**：无论成员类型写了什么 own/ref 修饰，
       槽位所有权一律记 OWN_NONE —— cunion 无 tag，运行期无法判定哪个
       member 活跃，销毁序列不为它生成 FREE，构造也不要求"own 槽位收
       FATAL"。指针/切片的生命周期由开发者自负（与 C union 语义一致）。 */
    members[i].type = ft;
  }

  if (!ok) {
    if (members) allocator_free(vm->alloc, (void **)&members);
    return false;
  }

  /* 2. type_cunion_intern 构造 cunion 类型（拷贝 member 表 + 布局
     size=max/align=max/offset 全 0 + 去重 intern，立即密封） */
  const type_t *t = type_cunion_intern(vm, members, n);
  if (members) allocator_free(vm->alloc, (void **)&members);
  if (!t) return false;

  /* 3. 登记（hoist 构造用）+ 绑定 type value 到编译期 vm 作用域 */
  const sema_type_t *st = sema_type_register(sema, t);
  if (!st) return false;
  cd->type_id = st->id; /* compiler 顶层名字绑定 LOAD_TYPE 用 */
  /* 显示名（诊断/值 dump 用）：须在 sema_type_register 分配 id 后调用——
     type_set_name 要求 t->id >= TYPE_ID_PROGRAM_BASE 才可改名。 */
  type_set_name(vm, t, cd->name);
  value_t *tv = type_as_value(vm, t);
  if (!tv) return false;
  char nb[256];
  if (cd->name.len >= sizeof nb) return false;
  memcpy(nb, cd->name.ptr, cd->name.len);
  nb[cd->name.len] = '\0';
  scope_define(vm, vm->current_scope, nb, tv);

  /* 4. 符号激活（TDZ：定义前不可见） */
  sym->type = t;
  sym->is_active = true;
  sym->flow_init = true;
  return true;
}
