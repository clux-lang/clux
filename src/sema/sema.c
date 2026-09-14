#include "sema/sema.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "core/panic.h"
#include "core/string.h"
#include "ctfe/ctfe.h"
#include "parser/ast_array.h"
#include "parser/ast_const.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_program.h"
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
  sema->loop_depth = 0;
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
static bool sema_eval_array_bound(sema_t *sema, ast_node_t **bound,
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
 *
 * 名字 "__type_N"（N = 队列下标）由 sema arena 分配（生命周期 = arena，
 * 存活到编译完成）；id 与名字的 N 一一对应（id = 64 + N），compiler 经
 * 名字查表拿 id 发 LOAD_TYPE。
 * =========================================================================== */

/* 复合类型的结构依赖递归登记：数组的元素类型、const/volatile 的 sub。
 * func 签名类型不在 AST 类型槽位出现（签名在注册段内联构造），跳过。 */
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
    default:
      break; /* 标量/str/bool/type/func/error：无结构依赖 */
  }
}

const sema_type_t *sema_type_register(sema_t *sema, const type_t *t) {
  if (!sema || !t || !sema->types) return NULL;

  /* 内建类型跳过登记：id 段 0..16 已由 vm_register_builtin_types 固定绑定，
     LOAD_TYPE <内建 id> 直接可查，无需 program id 别名。登记了反而让
     hoist 区发冗余的 LOAD_TYPE→BIND_TYPE 别名绑定（字节码膨胀）。
     kind 段判断（INTERRUPT < kind < COUNT 即 M2 复合类型段）：复合类型
     首次登记时 t->id 尚未赋值（仍为 0），不能用 id < TYPE_ID_BUILTIN_COUNT
     判断，会误伤跳过。 */
  if (t->kind <= TYPE_KIND_INTERRUPT || t->kind >= TYPE_KIND_COUNT) return NULL;

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
    case AST_TYPE_REF: {
      /* 具名类型引用（sema 登记过的类型，如 "__type_0"）→ 查 types 队列。
         折叠写回 / 重复解析（Pass 3 复查）时命中。 */
      ast_type_ref_t *ref = (ast_type_ref_t *)type_expr;
      const sema_type_t *st = sema_type_find_name(sema, ref->name);
      return st ? st->type : NULL;
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

/* 槽位替换：resolve + 就地替换为 AST_TYPE_REF（携带登记名字）。
 * 已是 AST_TYPE_REF 时幂等（重新解析 + 同名字引用替换）。
 * 内建类型不登记（sema_type_register 跳过）→ 找不到登记项，不替换
 * 槽位：保持原 AST_IDENT 等，编译器走 PUSH <名字> 从作用域查内建
 * type value 的原有路径，避免冗余的 LOAD_TYPE 别名引用。 */
const type_t *sema_resolve_type_slot(sema_t *sema, ast_node_t **slot) {
  if (!sema || !slot || !*slot) return NULL;
  ast_node_t *old = *slot;
  const type_t *t = resolve_type_expr(sema, old);
  if (!t) return NULL;
  const sema_type_t *st = sema_type_find(sema, t);
  if (!st) return t; /* 内建类型：不替换，保持原槽位 */

  ast_node_t *ref = ast_type_ref_new(sema->arena, old->tok_begin, old->tok_end);
  if (!ref) return NULL;
  ((ast_type_ref_t *)ref)->name = st->name;
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

/* ===========================================================================
 * Pass 1/2：函数名收集 + 类型解析（func_t 签名）
 * =========================================================================== */

static void pass1_names(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    /* 全局 comptime var：注册变量符号（暂不激活，pass_globals 求值后激活）。
       不创建 sema_func_t（不入 Pass 3 队列——不是函数，无函数体）。 */
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      sema_symbol_t init = {0};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, vd->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)vd->name.len, vd->name.ptr);
      }
      continue;
    }

    ast_func_def_t *fn = (ast_func_def_t *)f;
    sema_symbol_t init = {0}; /* 函数定义顺序自由：Pass 1 全部注册，无遮罩问题 */
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
    sf->name = fn->name;
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
       sym->type，调用点经 value_make_shadow(vm, sym->type) 构造 shadow callee */
    const type_t *sig = type_func_sig(sema->vm, params, nparams, rt, false);
    sym->type = sig;
    sym->ast = f;

    /* params 临时数组已被 type_func_sig 复制，此处释放 */
    if (params) allocator_free(sema->vm->alloc, (void **)&params);
  }
}

/* ===========================================================================
 * pass_globals：全局 comptime var 求值
 *
 * 遍历 prog->funcs 链上的 AST_VAR_DEF（parser 仅把 comptime var 挂上链），
 * 逐一定义点求值（sema_eval_comptime_var：改写 init → ctfe 求值 → 符号表
 * 编码）。无论成功失败都从链摘除——comptime var 不进入运行时。
 * 在 pass2_types 之后执行：函数签名已解析，init 可调用函数（含 comptime func）。
 * =========================================================================== */

static void pass_globals(sema_t *sema, ast_program_t *prog) {
  ast_node_t **prev = &prog->funcs;
  for (ast_node_t *f = prog->funcs; f;) {
    ast_node_t *next = f->next;
    if (f->kind == AST_VAR_DEF) {
      /* 失败（诊断已记录）：仍摘除，避免下游误以为全局 var 存在 */
      sema_eval_comptime_var(sema, (ast_var_def_t *)f, sema->global_scope);
      *prev = next;
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
     与 VM 侧 vm_register_printf 对应；签名类型经 type_func_sig intern。 */
  {
    const type_t *pparams[1] = { sema->vm->type_str };
    const type_t *psig = type_func_sig(sema->vm, pparams, 1, NULL,
                                       /*is_variadic=*/true);
    sema_symbol_t init = {.type = psig, .ast = NULL, .is_active = true};
    sema_scope_define(sema->global_scope, STRSLICE_LIT("printf"), &init);
  }

  pass1_names(sema, prog);
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
    ast_func_def_t *fn = (ast_func_def_t *)sf->def;
    /* comptime func：调用点折叠为字面量（sema_eval_comptime_call），
       不 shadow walk（参数为 shadow 无法编译期求值，body 求值在调用点）。 */
    if (fn->is_comptime) continue;
    sema_walk_function(sema, sf);
  }

  return !diag_has_error(sema->diag);
}
