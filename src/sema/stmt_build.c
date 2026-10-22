#include "sema/comptime.h"
#include "sema/sema.h"
#include "vm/type_ownership.h"
#include "core/panic.h"
#include "vm/type_ptr.h"
#include "vm/type_struct.h"
#include "vm/type_error.h"
#include "parser/ast_alignof.h"
#include "parser/ast_array.h"
#include "parser/ast_block.h"
#include "parser/ast_const.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "vm/type_tuple.h"
#include "vm/type_array.h"
#include "parser/ast_construct.h"
#include "parser/parser.h"
#include "parser/ast_assign.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_index.h"
#include "parser/ast_foreach.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "ctfe/ctfe.h"
#include "parser/ast_comptime_if.h"
#include "parser/ast_option.h"
#include "parser/ast_return.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_typeof.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_switch.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/ast_walk.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_defer.h"
#include "parser/ast_call.h"
#include "parser/ast_expr_stmt.h"
#include "vm/type_func.h"

#include <stdio.h>
#include <string.h>

#include "sema_build_internal.h"

/* 类型槽位是否含 sizeof/alignof/typeof（编译期运算符，m2-design §8）：
 * 这些运算符是 SEMA→CTFE 桥梁——操作数按 shadow 语义取类型，依赖 Pass 3b
 * 的 shadow 状态（局部变量定义点后才进 VM scope）。3a 求值会因变量未就绪
 * 失败 → 与"待绑定局部 type 遮蔽"同机制推迟到 3b 定义点兜底
 * （var_type_slot_reparse）。纯类型操作数（sizeof(i32)）本可在 3a 解析，
 * 统一推迟简化（3b 重解析幂等）。 */
/* ---- 类型槽位查询（基于共享 AST 遍历，见 parser/ast_walk.h）----
 *
 * 这两个查询过去各自手写递归遍历，覆盖面互不一致（见 ast_walk.h 说明）。
 * 现在共享同一份"子节点事实"，只写谓词：同一类型槽位不会因查询不同而得到
 * 不同结论。 */

typedef struct {
  unsigned want;  /* 需要探测的性质（PROBE_* 位或） */
  unsigned found; /* 已命中的性质 */
} type_slot_probe_t;

#define PROBE_CT_OP 1u          /* sizeof / alignof / typeof 出现 */
#define PROBE_NONLIT_BOUND 2u   /* 非字面量数组边界出现 */

/* ===========================================================================
 * Pass 3a：作用域树构建 + 控制流分析
 *
 * 遍历函数体，按词法块结构建树。只注册符号（名字 + 声明类型），
 * 不做类型检查。推断类型的变量 type=NULL，留给 Pass 3b 填充。
 * 子作用域按出现顺序追加（Pass 3b 每层独立索引按序取用，严格对应）。
 *
 * 建树同时做纯结构性的返回路径完整性分析（build_result_t.definitely_returns）：
 * 与类型无关，因此不需要等 Pass 3b 的 shadow 运行。非 void 函数所有路径
 * 必须 return 在此阶段即可检查。
 * =========================================================================== */

/* 标识符转 C 字符串（诊断/VM scope 注册用） */
void build_name_to_cstr(strslice_t s, char *buf, size_t cap) {
  size_t n = s.len < cap - 1 ? s.len : cap - 1;
  memcpy(buf, s.ptr, n);
  buf[n] = '\0';
}

/* 类型名是否被"待绑定的局部符号"遮蔽。
 *
 * 3a 阶段局部符号只注册 sema 符号表，尚未绑定 vm scope（type def 在 3b
 * 定义点求值后才绑定；var/参数在 3b 定义点才入 vm scope）。此时若 var
 * 类型槽位经 type_lookup（vm scope）解析，会错误落到外层同名全局 type
 * （或内建）上。沿 sema scope 链查找：命中同名符号（type def 或
 * var/参数）→ 遮蔽成立，槽位推迟到 3b 解析（定义点绑定后 shadow_var_def
 * 兜底）。顺序敏感由 3b 兜底天然承担：遮蔽符号定义在槽位前 → vm scope
 * 已遮蔽 → 解析失败报 "is a variable"；定义在槽位后 → vm scope 未遮蔽
 * → 解析到外层全局 type（顺序敏感语义正确）。
 * 仅命名类型槽位（AST_IDENT / AST_TYPE_REF）可判；复合类型（AST_ARRAY
 * 等）不含裸名字，直接返回 false 走常规解析。 */
bool type_shadowed_by_local_def(sema_t *sema, sema_scope_t *scope,
                                       ast_node_t *type_expr) {
  strslice_t name;
  if (type_expr->kind == AST_IDENT) {
    name = ((ast_ident_t *)type_expr)->name;
  } else if (type_expr->kind == AST_TYPE_REF) {
    name = ((ast_type_ref_t *)type_expr)->name;
  } else {
    return false;
  }
  (void)sema;
  for (sema_scope_t *s = scope; s; s = s->parent) {
    sema_symbol_t *sym = sema_scope_find_local(s, name);
    if (!sym) continue;
    return true; /* 最近同名符号：type def / var / 参数均遮蔽 */
  }
  return false;
}

/* 探针：只在**所求性质**命中时终止遍历。
   注意不能把两个性质合并进一次固定终止条件的遍历——例如 `[sizeof(a)]i32`
   会先命中"非字面量边界"而终止，从而永远看不到 `sizeof`，使
   type_has_ct_op 误报 false（槽位被 3a 提前解析 → 失败 → 快速失败闸门挡掉
   3b）。请求驱动的 `want/found` 位掩码保证每个谓词都得到完整遍历。 */
static ast_walk_action_t probe_type_slot(ast_node_t *n, void *user) {
  type_slot_probe_t *p = (type_slot_probe_t *)user;
  if ((p->want & PROBE_CT_OP) && !(p->found & PROBE_CT_OP) &&
      (n->kind == AST_SIZEOF || n->kind == AST_ALIGNOF ||
       n->kind == AST_TYPEOF)) {
    p->found |= PROBE_CT_OP;
  }
  if ((p->want & PROBE_NONLIT_BOUND) && !(p->found & PROBE_NONLIT_BOUND) &&
      n->kind == AST_ARRAY) {
    ast_node_t *len = ((ast_array_t *)n)->length;
    if (!len || len->kind != AST_INT_LIT) p->found |= PROBE_NONLIT_BOUND;
  }
  return (p->found & p->want) == p->want ? AST_WALK_STOP : AST_WALK_CONTINUE;
}

/* 类型槽位是否具备 want 中的任一性质（各自完整遍历，见探针说明） */
static bool type_slot_has(const ast_node_t *type_expr, unsigned want) {
  if (!type_expr) return false;
  type_slot_probe_t p = {.want = want, .found = 0};
  ast_walk_preorder((ast_node_t *)type_expr, probe_type_slot, &p);
  return (p.found & want) != 0;
}

/* 类型槽位是否含 sizeof/alignof/typeof（编译期运算符，m2-design §8）：
 * 这些运算符是 SEMA→CTFE 桥梁——操作数按 shadow 语义取类型，依赖 Pass 3b
 * 的 shadow 状态（局部变量定义点后才进 VM scope）。3a 求值会因变量未就绪
 * 失败 → 与"待绑定局部 type 遮蔽"同机制推迟到 3b 定义点兜底
 * （var_type_slot_reparse）。纯类型操作数（sizeof(i32)）本可在 3a 解析，
 * 统一推迟简化（3b 重解析幂等）。 */
bool type_has_ct_op(const ast_node_t *type_expr) {
  return type_slot_has(type_expr, PROBE_CT_OP);
}

/* 作用域链上是否存在**尚未求值的 comptime var**（同块靠前的 `comptime var C`） */
bool scope_has_unevaluated_comptime(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    const vec_t *keys = s->symbols ? strmap_keys(s->symbols) : NULL;
    size_t n = keys ? vec_len((vec_t *)keys) : 0;
    for (size_t i = 0; i < n; i++) {
      const char *k = (const char *)vec_get((vec_t *)keys, i);
      sema_symbol_t *sym = k ? (sema_symbol_t *)strmap_get(s->symbols, k) : NULL;
      if (sym && sym->is_comptime && !sym->ct_valid) return true;
    }
  }
  return false;
}

/* 类型槽位里是否含**非字面量数组边界**（`[N]T` / `[N+1]T`：长度在 3b 定义点
   才求值——comptime var 的常量此时才绑定进 VM 作用域。与"作用域内有未求值
   comptime var"联合判定是否推迟槽位解析） */
bool type_has_nonliteral_array_bound(const ast_node_t *type_expr) {
  return type_slot_has(type_expr, PROBE_NONLIT_BOUND);
}

/* 函数作用域树构建（全局 / 局部函数共用）：
   - parent = 全局作用域（全局函数）/ 定义点块作用域（局部函数——同块
     局部函数互相可见 + 捕获检查拦截外层局部）
   - fscope 追加为 parent 的子作用域（3b walk_block 在定义点消费该子作用域
     索引；sema_scope_destroy 递归回收）——捕获层，捕获符号注册在此
   - param_scope 追加为 fscope 的子作用域——参数层，参数符号注册在此，
     函数体 block 挂 param_scope（参数遮蔽捕获，镜像运行时 func_vcall 的
     closure_scope → 参数匿名层结构）
   - 捕获检查（expr.c local_func_base）：函数自身符号 = fscope 直系（捕获/
     body 局部）+ param_scope 直系（参数） */
static void build_func_tree(sema_t *sema, sema_func_t *sf,
                            sema_scope_t *parent) {
  ast_func_def_t *fn = (ast_func_def_t *)sf->def;

  sema_scope_t *fscope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FUNCTION, parent);
  sema_scope_add_child(parent, fscope);
  sf->scope = fscope;

  sema_scope_t *pscope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FUNCTION, fscope);
  sema_scope_add_child(fscope, pscope);
  sf->param_scope = pscope;

  /* 注册参数（已解析类型，运行时值在 Pass 3b 进入函数时定义到 VM scope）。
     所有权从函数签名类型的 param_ownerships 数组读取——pass2_types 在
     build_func_tree 之前已建好签名（含 ownership），AST 节点此时已折叠为
     AST_TYPE_REF，无法再从 token 提取。 */
  size_t param_idx = 0;
  for (ast_node_t *p = fn->params; p; p = p->next, param_idx++) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    const type_t *ptype = sema_resolve_type_slot(sema, &vd->type_expr);
    sema_symbol_t init = {
        .kind = SEMA_SYM_VAR,
        .type = ptype,
        .declared = true};   /* 参数在函数入口即存在 */
    if (!sema_scope_define(pscope, vd->name, &init)) {
      diag_error(sema->diag, sema_loc(sema, p),
                 "duplicate parameter '%.*s'", (int)vd->name.len,
                 vd->name.ptr);
    }
  }

  /* 注册闭包捕获符号（3a 只注册名字，类型 3b 定义点解析——外层变量类型
     此时可能未就绪）。纯 id 捕获 = 外层同名变量（type 留 NULL 由 3b 从
     外层符号解析）；括号捕获 = 临时构造（显式 type_expr 解析，无则 3b
     从 init 推断）。comptime func / 全局函数拒绝捕获（无运行时闭包场景）。
     捕获注册在 fscope（父层）；参数在 param_scope（子层）——参数遮蔽
     捕获（同名时参数优先，语言语义与运行时查找链一致）。 */
  if (fn->captures) {
    if (fn->is_comptime || !sf->is_local) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                 "%s function cannot have captures",
                 fn->is_comptime ? "comptime" : "global");
      fn->captures = NULL; /* 摘除：3b 不再处理（错误已诊断，防级联） */
    } else {
      for (ast_node_t *c = fn->captures; c; c = c->next) {
        ast_var_def_t *cv = (ast_var_def_t *)c;
        const type_t *ct = NULL;
        if (cv->type_expr) {
          ct = sema_resolve_type_slot(sema, &cv->type_expr);
        }
        sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = ct, .declared = true};
        if (!sema_scope_define(fscope, cv->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, c),
                     "capture '%.*s' duplicates another capture",
                     (int)cv->name.len, cv->name.ptr);
        }
      }
    }
  }

  /* 函数体 block 挂参数层（不再直接挂 fscope）。
     设 sema->fn.def/walking_comptime 供 desugar_defers 检查 comptime defer。
     保存/恢复：Pass 3a 全局函数循环不嵌套，但局部函数字面量可能嵌套。 */
  ast_func_def_t *saved_def = sema->fn.def;
  bool saved_ct = sema->fn.walking_comptime;
  sema->fn.def = fn;
  sema->fn.walking_comptime = fn->is_comptime;

  build_result_t r = build_block(sema, (ast_block_t *)fn->body, pscope);

  sema->fn.def = saved_def;
  sema->fn.walking_comptime = saved_ct;

  /* 控制流分析：非 void 函数所有路径必须 return（纯结构，不依赖类型） */
  const type_t *rt = fn->return_expr
                         ? sema_resolve_type_slot(sema, &fn->return_expr)
                         : NULL;
  if (rt && type_kind_of(rt) != TYPE_KIND_VOID && !r.definitely_returns) {
    diag_error(sema->diag, sema_loc(sema, &fn->base),
               "function '%.*s' must return a value on all paths",
               (int)fn->name.len, fn->name.ptr);
  }
}

static void build_func(sema_t *sema, sema_func_t *sf) {
  build_func_tree(sema, sf, sema->global_scope);
}

/* M7：单个函数建树（泛型实例在 Pass 3b 实例化时调用）。 */
void sema_build_func_scope(sema_t *sema, sema_func_t *sf) {
  if (!sema || !sf) return;
  build_func(sema, sf);
}

/* 局部函数注册（Pass 3a build_block AST_FUNC_DEF 分支）：
   - 遮蔽全局函数名 → 显式拒绝（compiler func_ids 是全局平铺名字→fid 映射，
     局部插入会覆盖全局 fid，导致块外引用错绑；M1 明确不支持）
   - 注册符号（SEMA_SYM_FUNC，注册即激活——提升语义，与全局函数一致）
   - 登记 sema_func_t 追加队列（is_local=true）并分配 fid（创建函数对象即分配）
   - 建 fscope 树（parent = 定义点块作用域）；comptime 局部函数同样建树——
     Pass 3b 对其 shadow walk（类型检查 + body 内语句折叠），不生成运行时
     字节码（compiler 预扫描跳过 comptime） */
void build_local_func(sema_t *sema, ast_node_t *s, sema_scope_t *scope) {
  ast_func_def_t *fn = (ast_func_def_t *)s;

  /* M7：局部泛型函数（含泛型函数字面量里的局部函数）暂不支持——
     单态化实例登记在全局语义（mangling/名称绑定），局部作用域的实例化
     留给后续阶段。明确报错，避免把 T 当普通未知类型。 */
  if (fn->generic.count > 0) {
    diag_error(sema->diag, sema_loc(sema, s),
               "generic local functions are not implemented yet (M7 P2 scope: "
               "top-level generic functions only)");
    return;
  }

  sema_symbol_t *gsym = sema_lookup(sema->global_scope, fn->name);
  if (gsym && gsym->kind == SEMA_SYM_FUNC) {
    diag_error(sema->diag, sema_loc(sema, s),
               "local function '%.*s' shadows a global function (unsupported)",
               (int)fn->name.len, fn->name.ptr);
    return; /* 不注册（3b 提升经符号存在性跳过） */
  }

  sema_symbol_t init = {.kind = SEMA_SYM_FUNC,
                        .ast = s,
                        .is_active = true,
                        .is_comptime = fn->is_comptime};
  if (!sema_scope_define(scope, fn->name, &init)) {
    diag_error(sema->diag, sema_loc(sema, s), "duplicate name '%.*s'",
               (int)fn->name.len, fn->name.ptr);
    return;
  }

  sema_func_t *sf = allocator_new_ex(sema->vm->alloc, "sema_func_t",
                                     sizeof(sema_func_t), NULL, NULL, NULL, 1);
  if (!sf) panic("sema: out of memory allocating sema_func");
  sf->def = s;
  sf->scope = NULL;
  sf->param_scope = NULL;
  sf->is_local = true;
  sf->is_walked = false;
  sema_func_id_alloc(sema, fn); /* fid 单一来源：创建函数对象即分配 */
  {
    sema_symbol_t *lsym = sema_scope_find_local(scope, fn->name);
    if (lsym) lsym->fid = fn->fid; /* 符号表同步（AST_FUNC_REF 替换用） */
  }
  vec_push(sema->funcs, sema->vm->alloc, sf);

  build_func_tree(sema, sf, scope); /* comptime 亦建树（3b shadow walk 检查） */
}

void sema_build_scope_tree(sema_t *sema) {
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    /* comptime func 同样建树：Pass 3b 对其 shadow walk（类型检查 + body 内
       语句折叠）。不生成运行时字节码（compiler 预扫描跳过 comptime），
       fscope 仅供 shadow walk 使用。 */
    build_func(sema, sf);
  }
}
