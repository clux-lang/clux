#include "sema/sema.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_option.h"
#include "core/panic.h"
#include "core/string.h"
#include "ctfe/ctfe.h"
#include "parser/ast_array.h"
#include "parser/ast_const.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_option.h"
#include "parser/ast_program.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/lexer.h"
#include "sema/comptime.h"
#include <string.h>
#include <stdio.h>

/* ===========================================================================
 * 上下文管理
 * =========================================================================== */

sema_t *sema_create(vm_t *vm, diag_buf_t *diag, vec_t *tokens,
                    arena_t *arena) {
  if (!vm || !diag || !tokens || !arena) return NULL;
  sema_t *sema =
      allocator_new_ex(vm->alloc, "sema_t", sizeof(sema_t), NULL, NULL, NULL,
                       1);
  sema->vm = vm;
  sema->diag = diag;
  sema->tokens = tokens;
  sema->arena = arena;
  sema->global_scope = NULL;
  sema->funcs = vec_new(vm->alloc, false); /* 元素手动释放（sema_func_t 无 dispose） */
  sema->types = vec_new(vm->alloc, false); /* 元素手动释放（sema_type_t 无 dispose） */
  sema->func_return_type = NULL;
  sema->func_has_return = false;
  sema->local_func_base = NULL;
  sema->local_func_param_scope = NULL;
  sema->loop_depth = 0;
  sema->func_id_next = FUNC_ID_PROGRAM_BASE;
  return sema;
}

void sema_destroy(sema_t **sema) {
  if (!sema || !*sema) return;
  allocator_t *alloc = (*sema)->vm->alloc;
  if ((*sema)->funcs) {
    size_t n = vec_len((*sema)->funcs);
    for (size_t i = 0; i < n; i++) {
      sema_func_t *sf = (sema_func_t *)vec_get((*sema)->funcs, i);
      allocator_free(alloc, (void **)&sf);
    }
    vec_free(alloc, &(*sema)->funcs);
  }
  if ((*sema)->types) {
    size_t n = vec_len((*sema)->types);
    for (size_t i = 0; i < n; i++) {
      sema_type_t *st = (sema_type_t *)vec_get((*sema)->types, i);
      allocator_free(alloc, (void **)&st);
    }
    vec_free(alloc, &(*sema)->types);
  }
  allocator_free(alloc, (void **)sema);
}

/* ===========================================================================
 * 公共工具
 * =========================================================================== */

/* 编译期求值数组边界 N（[N]T 的长度槽位）。
 *
 * N 是编译期常量表达式（m2-design §6）：字面量直接读值；复杂表达式
 * （[2+3]i32 / [sizeof(a)]i32 / comptime var 引用）走 ctfe 真实求值
 * （vm->comptime 模式）。成功后把非字面量边界折叠为 AST_INT_LIT 写回
 * （m2-design §comptime"常量折叠写回 AST"）——编译器消费类型槽位时
 * 直接读立即数，无需再次编译期求值。
 *
 * 失败返回 false（诊断已记录），len 不写入。
 */
bool sema_eval_array_bound(sema_t *sema, ast_node_t **bound,
                                  size_t *len) {
  if (!sema || !bound || !*bound) return false;
  ast_node_t *b = *bound;

  /* 字面量：直接读（折叠目标亦为 AST_INT_LIT，天然命中） */
  if (b->kind == AST_INT_LIT) {
    ast_int_lit_t *il = (ast_int_lit_t *)b;
    if (il->value > (uint64_t)SIZE_MAX) {
      diag_error(sema->diag, sema_loc(sema, b), "array length %llu too large",
                 (unsigned long long)il->value);
      return false;
    }
    *len = (size_t)il->value;
    return true;
  }

  /* 复杂表达式：ctfe 编译期求值（shadow 值不可用，须真实常量） */
  vm_t *vm = sema->vm;
  bool saved = vm->comptime;
  vm->comptime = true;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, b);
  vm->comptime = saved;

  if (!r || value_is_error(vm, r)) {
    diag_error(sema->diag, sema_loc(sema, b),
               "array length must be a compile-time constant");
    return false;
  }
  const type_t *t = value_type(r);
  if (!t || t->kind != TYPE_KIND_INT) {
    diag_error(sema->diag, sema_loc(sema, b),
               "array length must be an integer constant");
    return false;
  }
  uint64_t raw = 0;
  switch (t->size) {
    case 1: raw = *(const uint8_t  *)value_data(r); break;
    case 2: raw = *(const uint16_t *)value_data(r); break;
    case 4: raw = *(const uint32_t *)value_data(r); break;
    default: raw = *(const uint64_t *)value_data(r); break;
  }
  if (raw > (uint64_t)SIZE_MAX) {
    diag_error(sema->diag, sema_loc(sema, b), "array length %llu too large",
               (unsigned long long)raw);
    return false;
  }
  *len = (size_t)raw;

  /* 折叠边界为字面量写回（编译器消费立即数，零感知） */
  ast_node_t *lit = ast_int_lit_new(sema->arena, b->tok_begin, b->tok_end);
  if (lit) {
    ((ast_int_lit_t *)lit)->value = raw;
    /* 类型后缀留空（编译器不编译边界表达式，只读 value） */
    lit->next = b->next;
    *bound = lit;
  }
  return true;
}

/* ===========================================================================
 * 类型登记（sema->types 队列）
 *
 * 与 funcs 同构：sema 解析出的每个类型登记为 sema_type_t（type 指针 +
 * "__type_N" 名字 + TYPE_ID_PROGRAM_BASE+index id），驱动编译器 hoist
 * 类型提升区。AST 类型槽位经 AST_TYPE_REF 名字引用，保持平凡可解耦。
 * 登记覆盖：复合类型（数组/const/volatile）+ **func 签名类型**（签名本质
 * 是普通类型；函数指针作参数时签名引用签名，须经 hoist 依赖后序构造）。
 *
 * 名字 "__type_N"（N = 队列下标）由 sema arena 分配（生命周期 = arena，
 * 存活到编译完成）；id 与名字的 N 一一对应（id = 64 + N），compiler 经
 * 名字查表拿 id 发 LOAD_TYPE。
 * =========================================================================== */

/* 复合类型的结构依赖递归登记：数组的元素类型、const/volatile 的 sub、
 * func 签名类型的参数与返回类型（签名可引用签名——函数指针作参数/返回，
 * 依赖链递归覆盖）。内建标量无结构依赖。 */
static void sema_type_register_deps(sema_t *sema, const type_t *t) {
  if (!t) return;
  switch (t->kind) {
    case TYPE_KIND_ARRAY: {
      const type_t *et = array_type_elem(t);
      if (et) sema_type_register(sema, et);
      break;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE: {
      const type_t *sub = type_qualifier_sub(t);
      if (sub) sema_type_register(sema, sub);
      break;
    }
    case TYPE_KIND_FUNC: {
      /* 签名类型的参数/返回类型也是程序类型（含 func——函数指针作参数，
         签名引用签名）。依赖后序：先登记依赖，hoist pass 2 先定义依赖。 */
      size_t n = func_type_param_count(t);
      for (size_t i = 0; i < n; i++) {
        const type_t *pt = func_type_param(t, i);
        if (pt) sema_type_register(sema, pt);
      }
      const type_t *rt = func_type_return(t);
      if (rt) sema_type_register(sema, rt);
      break;
    }
    default:
      break; /* 标量/str/bool/type/error：无结构依赖 */
  }
}

const sema_type_t *sema_type_register(sema_t *sema, const type_t *t) {
  if (!sema || !t || !sema->types) return NULL;

  /* 内建类型跳过登记：id 段 0..16 已由 vm_register_builtin_types 固定绑定，
     LOAD_TYPE <内建 id> 直接可查，无需 program id 别名。登记了反而让
     hoist 区发冗余的 SEAL→LOAD_TYPE 别名绑定（字节码膨胀）。
     kind 段判断：内建标量（VOID..ERROR）+ INTERRUPT 哨兵跳过；**FUNC 签名
     类型除外**——用户函数签名登记（函数指针作参数时签名引用签名，须经
     hoist 依赖后序构造），复合段（CONST..CUNION）登记。func 签名类型
     kind=6 ≤ INTERRUPT=8，不能用 kind<=INTERRUPT 一刀切判断，须显式列。 */
  switch (t->kind) {
    case TYPE_KIND_VOID:
    case TYPE_KIND_BOOL:
    case TYPE_KIND_INT:
    case TYPE_KIND_FLOAT:
    case TYPE_KIND_STR:
    case TYPE_KIND_TYPE:
    case TYPE_KIND_ERROR:
    case TYPE_KIND_INTERRUPT:
      return NULL; /* 内建标量/哨兵 */
    case TYPE_KIND_FUNC:
      break; /* 用户函数签名类型：登记（提升进 hoist 区） */
    default:
      break; /* 复合段（CONST..CUNION）：登记 */
  }

  /* 按 type_t 指针去重：同一 intern 实例只登记一次 */
  const sema_type_t *found = sema_type_find(sema, t);
  if (found) return found;

  size_t n = vec_len(sema->types);
  if (n > 0xFFFFFFFFull - TYPE_ID_PROGRAM_BASE) return NULL; /* id 溢出 */

  sema_type_t *st = (sema_type_t *)allocator_new_ex(
      sema->vm->alloc, "sema_type_t", sizeof(sema_type_t), NULL, NULL, NULL, 1);
  if (!st) return NULL;
  memset(st, 0, sizeof(*st));
  st->type = t;
  st->id   = (uint32_t)(TYPE_ID_PROGRAM_BASE + n);

  /* 名字 "__type_N"（arena 分配，跨 sema/compile 阶段安全） */
  char name[32];
  int nl = snprintf(name, sizeof name, "__type_%zu", n);
  char *buf = (char *)arena_calloc(sema->arena, 1, (size_t)nl + 1,
                                   ALIGNOF(max_align_t));
  if (!buf) {
    allocator_free(sema->vm->alloc, (void **)&st);
    return NULL;
  }
  memcpy(buf, name, (size_t)nl);
  st->name = (strslice_t){ buf, (size_t)nl };

  vec_push(sema->types, sema->vm->alloc, st);

  /* 程序类型（复合段：INTERRUPT < kind < COUNT）同步 t->id = st->id：
     运行时 SET_TYPE_NAME 按 t->id >= TYPE_ID_PROGRAM_BASE 判断"可改名"。
     内建类型已在上方过滤，不会到达此处。 */
  ((type_t *)t)->id = st->id;

  /* 结构依赖递归登记（hoist 构造完备性） */
  sema_type_register_deps(sema, t);
  return st;
}

const sema_type_t *sema_type_find(sema_t *sema, const type_t *t) {
  if (!sema || !sema->types || !t) return NULL;
  size_t n = vec_len(sema->types);
  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(sema->types, i);
    if (st && st->type == t) return st;
  }
  return NULL;
}

const sema_type_t *sema_type_find_name(sema_t *sema, strslice_t name) {
  if (!sema || !sema->types || !name.ptr) return NULL;
  size_t n = vec_len(sema->types);
  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(sema->types, i);
    if (st && st->name.len == name.len &&
        memcmp(st->name.ptr, name.ptr, name.len) == 0)
      return st;
  }
  return NULL;
}

const sema_type_t *sema_type_by_id(sema_t *sema, uint32_t id) {
  if (!sema || !sema->types) return NULL;
  size_t n = vec_len(sema->types);
  for (size_t i = 0; i < n; i++) {
    const sema_type_t *st = (const sema_type_t *)vec_get(sema->types, i);
    if (st && st->id == id) return st;
  }
  return NULL;
}

/* ===========================================================================
 * 类型表达式求值
 * =========================================================================== */

/* 纯求值（不登记、不替换槽位）：switch 分派类型表达式节点 → 类型单例。
 * 由 resolve_type_expr（登记）与递归 sub 解析共用。 */
static const type_t *sema_resolve_inner(sema_t *sema, ast_node_t *type_expr) {
  if (!sema || !type_expr) return NULL;

  switch (type_expr->kind) {
    case AST_IDENT: {
      /* 类型名引用：i32/bool 等关键字类型名与用户类型名（IDENTIFIER）
         统一为 AST_IDENT（类型即表达式），查类型表。 */
      ast_ident_t *id = (ast_ident_t *)type_expr;
      return type_lookup(sema->vm, id->name);
    }
    case AST_CONST: {
      /* const 类型修饰：嵌套递归（const const T 收敛为 const T） */
      const type_t *sub = resolve_type_expr(sema, ((ast_const_t *)type_expr)->sub);
      return sub ? type_const_intern(sema->vm, sub) : NULL;
    }
    case AST_VOLATILE: {
      const type_t *sub =
          resolve_type_expr(sema, ((ast_volatile_t *)type_expr)->sub);
      return sub ? type_volatile_intern(sema->vm, sub) : NULL;
    }
    case AST_OPTION: {
      /* ?T optional 类型修饰：递归解析内层 T → 按 (inner) 去重 intern。
         与 const/volatile 同族（类型即表达式，嵌套递归）。 */
      const type_t *sub = resolve_type_expr(sema, ((ast_option_t *)type_expr)->sub);
      return sub ? type_option_intern(sema->vm, sub) : NULL;
    }
    case AST_ARRAY: {
      /* [N]T 数组类型：递归解析基础类型 + 编译期求值边界 N →
         按 (elem_type, length) 去重 intern。边界槽位支持嵌套数组
         （[2][3]i32 = 元素类型是 [3]i32）。 */
      ast_array_t *arr = (ast_array_t *)type_expr;
      const type_t *base = resolve_type_expr(sema, arr->base_type);
      if (!base) return NULL;
      size_t len;
      if (!sema_eval_array_bound(sema, &arr->length, &len)) return NULL;
      return type_array_intern(sema->vm, base, len);
    }
    case AST_FUNC_TYPE: {
      /* 函数签名类型：func(param_types...)->ret。逐参数/返回递归解析
         （可为任意类型表达式，含嵌套签名）→ type_func_sig 按签名去重
         intern。返回类型缺省 NULL = void。 */
      ast_func_type_t *ft = (ast_func_type_t *)type_expr;
      size_t n = sema_count_siblings(ft->params);
      const type_t *params_arr[n > 0 ? n : 1];
      size_t i = 0;
      for (ast_node_t *pr = ft->params; pr; pr = pr->next, i++) {
        const type_t *pt = resolve_type_expr(sema, pr);
        if (!pt) return NULL;
        params_arr[i] = pt;
      }
      const type_t *rt = ft->return_type
                             ? resolve_type_expr(sema, ft->return_type)
                             : NULL;
      if (ft->return_type && !rt) return NULL;
      return type_func_sig(sema->vm, n > 0 ? params_arr : NULL, n, rt,
                           /*is_variadic=*/false);
    }
    case AST_TYPE_REF: {
      /* 具名类型引用（sema 登记过的类型，如 "__type_0"）→ 查 types 队列。
         折叠写回 / 重复解析（Pass 3 复查）时命中。
         内建类型引用（名字 = 规范名 "i32"）不在登记表 → type_lookup 兜底
         （内建 type value 注册在编译期 vm global scope，别名透明解析）。 */
      ast_type_ref_t *ref = (ast_type_ref_t *)type_expr;
      const sema_type_t *st = sema_type_find_name(sema, ref->name);
      if (st) return st->type;
      return type_lookup(sema->vm, ref->name);
    }
    default:
      /* M2 扩展点：元组/func 类型表达式 + 类型计算 */
      diag_error(sema->diag, sema_loc(sema, type_expr),
                 "unsupported type expression");
      return NULL;
  }
}

const type_t *resolve_type_expr(sema_t *sema, ast_node_t *type_expr) {
  if (!sema || !type_expr) return NULL;
  const type_t *t = sema_resolve_inner(sema, type_expr);
  if (t) sema_type_register(sema, t); /* 解析出的类型登记（含递归 sub） */
  return t;
}

/* 槽位替换：resolve + 就地替换为 AST_TYPE_REF（携带类型名）。
 * 已是 AST_TYPE_REF 时幂等（重新解析 + 同名字引用替换）。
 * 程序类型名字 = 登记名（"__type_N"）；内建类型不登记（sema_type_register
 * 跳过）→ 名字 = 内建规范名（如 "i32"），编译器经 type_lookup 兜底 →
 * LOAD_TYPE <内建 id>。统一折叠为 AST_TYPE_REF：类型槽位一律走
 * LOAD_TYPE <id> 直接加载真实 type_t（别名透明，m2-design 关键决策——
 * 复合类型组装用 value->data 而非名字），消除运行时作用域查找。 */
const type_t *sema_resolve_type_slot(sema_t *sema, ast_node_t **slot) {
  if (!sema || !slot || !*slot) return NULL;
  ast_node_t *old = *slot;
  const type_t *t = resolve_type_expr(sema, old);
  if (!t) return NULL;
  const sema_type_t *st = sema_type_find(sema, t);
  if (!st && t->name.ptr == NULL) return t; /* 无名类型（异常防御）不替换 */

  ast_node_t *ref = ast_type_ref_new(sema->arena, old->tok_begin, old->tok_end);
  if (!ref) return NULL;
  ((ast_type_ref_t *)ref)->name = st ? st->name : t->name;
  ref->next = old->next; /* 保留兄弟链 */
  *slot = ref;
  return t;
}

location_t sema_loc(sema_t *sema, ast_node_t *node) {
  location_t zero = {0};
  if (!sema || !node) return zero;
  const token_t *t = (const token_t *)vec_get(sema->tokens, node->tok_begin);
  const location_t *loc = t ? token_get_location(t) : NULL;
  return loc ? *loc : zero;
}

void sema_type_name(const type_t *t, char *buf, size_t cap) {
  if (!buf || cap == 0) return;
  buf[0] = '\0';
  if (!t || !t->name.ptr) return;
  size_t n = t->name.len < cap - 1 ? t->name.len : cap - 1;
  memcpy(buf, t->name.ptr, n);
  buf[n] = '\0';
}

size_t sema_count_siblings(const ast_node_t *node) {
  size_t n = 0;
  for (const ast_node_t *p = node; p; p = p->next) n++;
  return n;
}

/* 函数 id 分配（幂等）：创建 sema 函数对象时调用——fid 单一来源在 sema，
   compiler 预扫描只读取。fn->fid 已分配则复用（CTFE 求值字面量可能早于
   sema_check_func_literal，两者幂等）。 */
uint32_t sema_func_id_alloc(sema_t *sema, ast_func_def_t *fn) {
  if (!sema || !fn) return 0;
  if (fn->fid != 0) return fn->fid;
  if (sema->func_id_next >= FUNC_ID_PROGRAM_BASE &&
      (uint64_t)sema->func_id_next + 1u <= 0xFFFFFFFFull) {
    fn->fid = sema->func_id_next++;
    return fn->fid;
  }
  return 0; /* id 溢出 */
}

/* 按函数 id 查 sema 函数对象（线性扫描，函数数量少）。函数定义 AST 托管在
   sema->funcs，sema/ctfe 经此按 id 查询（折叠产物按 fid 取签名构造引用）。 */
sema_func_t *sema_func_by_id(sema_t *sema, uint32_t fid) {
  if (!sema || !sema->funcs) return NULL;
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    if (!sf || !sf->def || sf->def->kind != AST_FUNC_DEF) continue;
    if (((ast_func_def_t *)sf->def)->fid == fid) return sf;
  }
  return NULL;
}

/* ===========================================================================
 * Pass 1/2：函数名收集 + 类型解析（func_t 签名）
 * =========================================================================== */

static void pass1_names(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    /* 全局 comptime var：注册变量符号（暂不激活，pass_globals 求值后激活）。
       不创建 sema_func_t（不入 Pass 3 队列——不是函数，无函数体）。 */
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_VAR};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, vd->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)vd->name.len, vd->name.ptr);
      }
      continue;
    }

    /* 全局 type 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 DEFINE 绑定 type value）。 */
    if (f->kind == AST_TYPE_DEF) {
      ast_type_def_t *td = (ast_type_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, td->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)td->name.len, td->name.ptr);
      }
      continue;
    }

    /* 全局 enum 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_ENUM_DEF) {
      ast_enum_def_t *ed = (ast_enum_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, ed->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)ed->name.len, ed->name.ptr);
      }
      continue;
    }

    ast_func_def_t *fn = (ast_func_def_t *)f;
    sema_symbol_t init = {.kind = SEMA_SYM_FUNC}; /* 函数定义顺序自由：Pass 1 全部注册，无遮罩问题 */
    sema_symbol_t *sym = sema_scope_define(sema->global_scope, fn->name, &init);
    if (!sym) {
      diag_error(sema->diag, sema_loc(sema, f), "duplicate function '%.*s'",
                 (int)fn->name.len, fn->name.ptr);
      continue; /* 重复定义不入队 */
    }
    /* 函数名全局可见（无 TDZ）→ 注册即激活 */
    sym->is_active = true;
    sym->is_comptime = fn->is_comptime;
    /* 登记 sema 层函数对象（Pass 3 队列驱动；局部函数/泛型实例将来追加） */
    sema_func_t *sf = allocator_new_ex(sema->vm->alloc, "sema_func_t",
                                       sizeof(sema_func_t), NULL, NULL, NULL,
                                       1);
    if (!sf) panic("sema: out of memory allocating sema_func");
    sf->def = f;
    sf->scope = NULL;
    sf->param_scope = NULL;
    sf->name = fn->name;
    /* fid 单一来源：创建 sema 函数对象即分配（comptime func 亦分配——统一
       流程；compiler 预扫描跳过 comptime 不读取，占位无冲突）。符号表 fid
       字段同步（AST_FUNC_REF 替换 / 折叠按 id 查询用） */
    sema_func_id_alloc(sema, fn);
    sym->fid = fn->fid;
    vec_push(sema->funcs, sema->vm->alloc, sf);
  }
}

static void pass2_types(sema_t *sema) {
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    ast_node_t *f = sf->def;
    ast_func_def_t *fn = (ast_func_def_t *)f;
    sema_symbol_t *sym = sema_lookup(sema->global_scope, fn->name);
    if (!sym) continue;
    if (sym->ast) continue; /* 理论上不可达：队列只含有效函数 */

    size_t nparams = sema_count_siblings(fn->params);
    const type_t **params = NULL;
    if (nparams > 0) {
      params = allocator_new_ex(sema->vm->alloc, "type_t*", sizeof(type_t *),
                                NULL, NULL, NULL, nparams);
      size_t j = 0;
      for (ast_node_t *p = fn->params; p; p = p->next, j++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t) {
          diag_error(sema->diag, sema_loc(sema, p),
                     "unknown type in parameter '%.*s'",
                     (int)vd->name.len, vd->name.ptr);
        }
        params[j] = t; /* 失败置 NULL，位置对齐，func_shadow_call 校验时跳过 */
      }
    }

    const type_t *rt = NULL;
    if (fn->return_expr) {
      rt = sema_resolve_type_slot(sema, &fn->return_expr);
      if (!rt) {
        diag_error(sema->diag, sema_loc(sema, f),
                   "unknown return type");
      }
    }

    /* 注册签名类型（按签名去重 intern 到 vm 类型池，type_func_sig 复制 params）；
       符号统一记录定义 AST 节点（函数 = AST_FUNC_DEF）；签名类型存于
       sym->type，调用点经 value_make_shadow(vm, sym->type) 构造 shadow callee。
       签名类型**登记进 sema->types**（与数组/const 同机制）——签名本质是
       普通类型，且函数指针作参数时签名引用签名，须经 hoist 两遍扫描
       依赖后序构造（参数/返回类型已在此前解析登记，父先入队）。 */
    const type_t *sig = type_func_sig(sema->vm, params, nparams, rt, false);
    sym->type = sig;
    sym->ast = f;
    /* 签名类型提升：登记 + 递归依赖登记；fn->sig_id 记录（compiler
       compile_func_reg 发 LOAD_TYPE <sig_id>，不再作用域查找） */
    const sema_type_t *st = sema_type_register(sema, sig);
    if (st) fn->sig_id = st->id;

    /* params 临时数组已被 type_func_sig 复制，此处释放 */
    if (params) allocator_free(sema->vm->alloc, (void **)&params);
  }
}

/* ===========================================================================
 * pass1b_types：全局 type def 求值
 *
 * 在 pass2_types（函数签名解析）之前求值全局 type def（sema_eval_type_def：
 * rhs 真实求值 → 折叠 AST_TYPE_REF → 绑定 type value 到编译期 vm 作用域），
 * 使函数签名/参数/返回类型可引用全局 type 定义（type Foo = ... 先于 func
 * 使用）。定义点不摘除——type def 进入字节码，运行时 DEFINE 绑定 type value
 * 到运行时作用域（编译期 vm 与运行时 vm 解耦）。
 * 注：局部 type def 由 3b 按语句顺序求值（定义点激活，TDZ 与 var 一致）。
 * =========================================================================== */

static void pass1b_types(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind == AST_TYPE_DEF) {
      sema_eval_type_def(sema, (ast_type_def_t *)f, sema->global_scope);
    } else if (f->kind == AST_ENUM_DEF) {
      /* 与全局 type def 同 pass：enum 类型先于函数签名解析（签名/变量
         可引用 enum 类型）。定义点保留（进字节码，运行时 hoist 构造）。 */
      sema_eval_enum_def(sema, (ast_enum_def_t *)f, sema->global_scope);
    }
  }
}

/* ===========================================================================
 * pass_globals：全局 comptime var 求值
 *
 * 遍历 prog->funcs 链上的 AST_VAR_DEF（comptime var），逐一定义点求值
 * （sema_eval_comptime_var：改写 init → ctfe 求值 → 符号表编码）。
 * 无论成功失败都从链摘除——comptime var 不进入运行时。
 * 在 pass2_types 之后执行：函数签名已解析（pass1b 已绑定全局 type def），
 * init 可调用函数（含 comptime func）与引用自定义类型。
 * =========================================================================== */

static void pass_globals(sema_t *sema, ast_program_t *prog) {
  ast_node_t **prev = &prog->funcs;
  for (ast_node_t *f = prog->funcs; f;) {
    ast_node_t *next = f->next;
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      if (vd->is_comptime) {
        /* comptime var：求值编码常量，定义点摘除（不进运行时） */
        sema_eval_comptime_var(sema, vd, sema->global_scope);
        *prev = next;
      } else {
        /* 普通全局变量：init 折叠为字面量/函数引用，保留进字节码（运行时
           DEFINE 到 root_scope，函数体可见）。失败（诊断已记录）：仍摘除，
           避免下游误以为全局 var 存在。 */
        if (!sema_eval_global_var(sema, vd, sema->global_scope))
          *prev = next;
        else
          prev = &f->next;
      }
    } else {
      prev = &f->next;
    }
    f = next;
  }
}

/* ===========================================================================
 * 三遍编排
 * =========================================================================== */

bool sema_analyze(sema_t *sema, ast_node_t *program) {
  if (!sema || !program || program->kind != AST_PROGRAM) return false;
  ast_program_t *prog = (ast_program_t *)program;

  sema->global_scope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_GLOBAL, NULL);
  if (!sema->global_scope) return false;

  /* 预注册内置函数符号（printf：variadic 签名，ast=NULL 表示无 AST 定义）。
     与 VM 侧 vm_register_printf 对应；签名类型经 type_func_sig intern。
     fid = 内建函数 id（vm->functions 按名字查询；内建段 < FUNC_ID_PROGRAM_BASE，
     printf=0）。内建函数无 AST_FUNC_DEF 托管（sema->funcs 不含内建），
     源码引用替换 AST_FUNC_REF 时从符号表 fid 字段取内建 id。 */
  {
    const type_t *pparams[1] = { sema->vm->type_str };
    const type_t *psig = type_func_sig(sema->vm, pparams, 1, NULL,
                                       /*is_variadic=*/true);
    uint32_t pfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 6 &&
            memcmp(bf->name.ptr, "printf", 6) == 0) {
          pfid = bf->id;
          break;
        }
      }
    }
    sema_symbol_t init = {.kind = SEMA_SYM_FUNC, .type = psig,
                          .is_active = true, .fid = pfid};
    sema_scope_define(sema->global_scope, STRSLICE_LIT("printf"), &init);
  }

  pass1_names(sema, prog);
  pass1b_types(sema, prog); /* 全局 type def 先于函数签名解析（签名可引用） */
  pass2_types(sema);
  pass_globals(sema, prog);

  /* Pass 3a：作用域树构建 + 控制流分析（符号注册、break/continue 位置检查、
     不可达语句、非 void 函数返回路径完整性）。快速失败：3a 有错误则
     不进入 3b——控制流/结构错误已使作用域树不可信，继续 shadow run
     只会产生级联的二次诊断。 */
  sema_build_scope_tree(sema);
  if (diag_has_error(sema->diag)) return false;

  /* Pass 3b：shadow VM 运行（按作用域树严格对应遍历，纯类型检查与推导）。
     队列驱动：遍历 sema->funcs，解析过程中队列可增长（局部函数提升 /
     泛型单态化追加到队尾），len 每次重取自动覆盖新函数。 */
  for (size_t i = 0; i < vec_len(sema->funcs); i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    /* comptime func 同样 shadow walk：body 内语句做类型检查 + 折叠
       （函数字面量分配 fid / 登记签名、comptime var 求值折叠）。不生成
       运行时字节码（compiler 预扫描跳过 comptime）——walk 只为编译期
       求值服务（调用点 CTFE 解释执行，sema_eval_comptime_call）。 */
    /* 函数字面量 body 内登记的局部函数：sema_check_func_literal 已同步
       walk 并标记，其 fscope 挂在临时作用域树上已销毁——跳过防二次 walk
       访问悬空作用域。 */
    if (sf->is_literal_owned) continue;
    sema_walk_function(sema, sf);
  }

  return !diag_has_error(sema->diag);
}
