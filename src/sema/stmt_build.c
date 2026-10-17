#include "sema/sema.h"
#include "core/panic.h"
#include "vm/type_ptr.h"
#include "parser/ast_alignof.h"
#include "parser/ast_array.h"
#include "parser/ast_block.h"
#include "parser/ast_const.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
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
static void name_to_cstr(strslice_t s, char *buf, size_t cap) {
  size_t n = s.len < cap - 1 ? s.len : cap - 1;
  memcpy(buf, s.ptr, n);
  buf[n] = '\0';
}

typedef struct build_result {
  bool definitely_returns; /* 该块保证返回（所有路径都 return） */
} build_result_t;

static build_result_t build_block(sema_t *sema, ast_block_t *block,
                                  sema_scope_t *scope);
static build_result_t build_func_if(sema_t *sema, ast_if_t *it,
                                    sema_scope_t *scope);

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
static bool type_shadowed_by_local_def(sema_t *sema, sema_scope_t *scope,
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
static bool type_has_ct_op(const ast_node_t *type_expr) {
  return type_slot_has(type_expr, PROBE_CT_OP);
}

/* 作用域链上是否存在**尚未求值的 comptime var**（同块靠前的 `comptime var C`） */
static bool scope_has_unevaluated_comptime(sema_scope_t *scope) {
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
static bool type_has_nonliteral_array_bound(const ast_node_t *type_expr) {
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
  sema_symbol_t *fn_sym = sema_lookup(parent, fn->name);
  const func_sig_t *fn_sig = NULL;
  if (fn_sym && fn_sym->type && fn_sym->type->kind == TYPE_KIND_FUNC)
    fn_sig = &((const func_type_t *)fn_sym->type)->sig;
  size_t param_idx = 0;
  for (ast_node_t *p = fn->params; p; p = p->next, param_idx++) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    ownership_t pown = (fn_sig && fn_sig->param_ownerships &&
                        param_idx < fn_sig->param_count)
                           ? fn_sig->param_ownerships[param_idx]
                           : OWN_NONE;
    sema_symbol_t init = {
        .kind = SEMA_SYM_VAR,
        .type = sema_resolve_type_slot(sema, &vd->type_expr),
        .ownership = pown};
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
        ownership_t cown = cv->type_expr
                              ? sema_type_expr_ownership(cv->type_expr)
                              : OWN_NONE;
        const type_t *ct = NULL;
        if (cv->type_expr) {
          ct = sema_resolve_type_slot(sema, &cv->type_expr);
        }
        sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = ct,
                              .ownership = cown};
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
  if (rt && rt->kind != TYPE_KIND_VOID && !r.definitely_returns) {
    diag_error(sema->diag, sema_loc(sema, &fn->base),
               "function '%.*s' must return a value on all paths",
               (int)fn->name.len, fn->name.ptr);
  }
}

static void build_func(sema_t *sema, sema_func_t *sf) {
  build_func_tree(sema, sf, sema->global_scope);
}

/* 局部函数注册（Pass 3a build_block AST_FUNC_DEF 分支）：
   - 遮蔽全局函数名 → 显式拒绝（compiler func_ids 是全局平铺名字→fid 映射，
     局部插入会覆盖全局 fid，导致块外引用错绑；M1 明确不支持）
   - 注册符号（SEMA_SYM_FUNC，注册即激活——提升语义，与全局函数一致）
   - 登记 sema_func_t 追加队列（is_local=true）并分配 fid（创建函数对象即分配）
   - 建 fscope 树（parent = 定义点块作用域）；comptime 局部函数同样建树——
     Pass 3b 对其 shadow walk（类型检查 + body 内语句折叠），不生成运行时
     字节码（compiler 预扫描跳过 comptime） */
static void build_local_func(sema_t *sema, ast_node_t *s, sema_scope_t *scope) {
  ast_func_def_t *fn = (ast_func_def_t *)s;

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
  sema_func_id_alloc(sema, fn); /* fid 单一来源：创建函数对象即分配 */
  {
    sema_symbol_t *lsym = sema_scope_find_local(scope, fn->name);
    if (lsym) lsym->fid = fn->fid; /* 符号表同步（AST_FUNC_REF 替换用） */
  }
  vec_push(sema->funcs, sema->vm->alloc, sf);

  build_func_tree(sema, sf, scope); /* comptime 亦建树（3b shadow walk 检查） */
}

static build_result_t build_block(sema_t *sema, ast_block_t *block,
                                  sema_scope_t *scope);

/* ---- defer 语法糖改写（Pass 3a 内联） ----
 *
 * defer 本质是匿名无参 void 函数 + 作用域退出点逆序调用。sema 在 Pass 3a
 * build_block 遍历语句链时内联改写：
 *   1. AST_DEFER → 取 closure（已是 ast_func_def_t），设名 __defer_N，
 *      替换语句链中 AST_DEFER 节点 → 走 build_local_func 正常注册符号 +
 *      分配 fid + 建 fscope 树；登记进当前 scope->defers
 *   2. 块尾追加逆序 call 序列（每层 LIFO）
 *   3. return 前：沿 scope parent 链到 SEMA_SCOPE_FUNCTION，收集沿途各层
 *      defers 逆序插入 call
 *   4. break/continue 前：沿 scope parent 链到最近循环 scope，收集沿途
 *      各层 defers 逆序插入 call
 *
 * 改写后 AST 中不再有 AST_DEFER——Pass 3b / compiler / ctfe 完全不感知
 * defer，只看到普通局部函数定义 + 普通函数调用语句。
 */

/* 构造 `__defer_N()` call 语句（AST_EXPR_STMT(AST_CALL(callee=IDENT, args=NULL))）。
   fn_name 是 arena 持久化的 NUL 终止字符串。 */
static ast_node_t *make_defer_call(sema_t *sema, const char *fn_name,
                                   uint32_t tok_begin, uint32_t tok_end) {
  arena_t *a = sema->arena;
  /* tok_begin=0 标记为编译器生成节点——build_block 的 unreachable 检查
     跳过 tok_begin==0 的节点（defer call 是退出点清理代码，非用户语句）。 */
  (void)tok_begin; (void)tok_end;
  ast_node_t *ident = ast_ident_new(a, 0, 0);
  size_t nlen = strlen(fn_name);
  char *name_buf = (char *)arena_calloc(a, nlen + 1, 1, ALIGNOF(max_align_t));
  memcpy(name_buf, fn_name, nlen);
  ((ast_ident_t *)ident)->name = strslice_from_bytes(name_buf, nlen);

  ast_node_t *call = ast_call_new(a, 0, 0);
  ((ast_call_t *)call)->callee = ident;
  ((ast_call_t *)call)->args = NULL;
  ((ast_call_t *)call)->args_last = NULL;

  ast_node_t *stmt = ast_expr_stmt_new(a, 0, 0);
  ((ast_expr_stmt_t *)stmt)->expr = call;
  return stmt;
}

/* 沿 scope parent 链收集 defers 并构造逆序 call 语句链。
   stop_scope 不含（defers 在 stop_scope 之前各层）。
   返回 call 语句链首（已按执行顺序排列），*out_last 设为链尾。
   无 defer 时返回 NULL。 */
static ast_node_t *collect_defer_calls(sema_t *sema, sema_scope_t *from_scope,
                                       const sema_scope_t *stop_scope,
                                       ast_node_t **out_last) {
  ast_node_t *calls[64];
  size_t n = sema_scope_collect_defers(from_scope, stop_scope, calls, 64);
  if (n == 0) {
    if (out_last) *out_last = NULL;
    return NULL;
  }
  ast_node_t *head = NULL;
  ast_node_t *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    ast_func_def_t *fn = (ast_func_def_t *)calls[i];
    /* 名字是 arena NUL 终止字符串（设名时已分配） */
    char fname[64];
    snprintf(fname, sizeof(fname), "%.*s", (int)fn->name.len, fn->name.ptr);
    ast_node_t *call = make_defer_call(sema, fname, fn->base.tok_begin,
                                       fn->base.tok_end);
    if (!head) { head = call; tail = call; }
    else { tail->next = call; tail = call; }
  }
  if (out_last) *out_last = tail;
  return head;
}

/* 沿 parent 链找到最近的循环作用域（while/dowhile/for 的 body scope 的
   parent，或 for scope 本身）。
   当前 sema_scope_kind_t 没有 LOOP 标记——循环体的 scope kind 是
   SEMA_SCOPE_BLOCK，for 的 init scope 是 SEMA_SCOPE_FOR。
   我们靠 sema->fn.loop_depth > 0 判断在循环内，沿 parent 找到
   loop_depth 减为 0 的边界——但 scope 没有 loop_depth 字段。
   替代方案：build_block 的循环分支传入循环 scope，通过 sema 临时字段
   记录当前循环 scope 栈（类似 loop_stack）。这里先用 parent 链上溯到
   SEMA_SCOPE_FUNCTION（与 return 相同的 stop），break/continue 的位置
   检查已在 Pass 3a 做过（loop_depth==0 报错），所以到这里一定在循环内。
   精确的循环边界稍后用 sema 循环 scope 栈处理。 */
/* break/continue 的 defer 收集范围：从当前 scope 沿 parent 链收集到
   循环体 scope **包含**。stop 设为循环 scope 的 parent。
   TODO: 精确循环边界——当前回退到函数根（break/continue 的 defer call
   会执行到函数根所有层，而非只到循环层）。需要 sema 循环 scope 栈来
   精确定位。先保持与 return 相同的行为（安全过保守）。 */
static const sema_scope_t *find_loop_stop(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FUNCTION) return s->parent;
  }
  return NULL;
}

/* return 的 defer 收集范围：从当前 scope 沿 parent 链收集到函数 scope
   **包含**（函数体顶层的 defers 也要执行）。因此 stop 设为函数 scope 的
   parent（函数调用方的 scope），使 collect_defer_calls 的 `s != stop`
   循环能包含函数 scope 自身的 defers。 */
static const sema_scope_t *find_func_stop(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FUNCTION) return s->parent;
  }
  return NULL;
}

/* defer desugar：改写 block 语句链中的 AST_DEFER → AST_FUNC_DEF，
   块尾追加逆序 call，return/break/continue 前插入沿途 defer call。
   在 build_block 正式遍历**之前**调用——改写后 build_block 看到的
   是纯 AST_FUNC_DEF + AST_EXPR_STMT。 */
static void desugar_defers(sema_t *sema, ast_block_t *block,
                           sema_scope_t *scope) {
  /* 第一遍：替换 AST_DEFER → AST_FUNC_DEF，登记进 scope->defers。
     同时记录块内是否有 return/break/continue 需要插入 defer call。 */
  ast_node_t *prev = NULL;
  ast_node_t *defer_calls_head = NULL;
  ast_node_t *defer_calls_tail = NULL;

  for (ast_node_t *s = block->stmts; s; s = s->next) {
    if (s->kind == AST_DEFER) {
      ast_defer_t *d = (ast_defer_t *)s;
      ast_func_def_t *fn = d->closure;
      if (!fn) continue;

      /* 设名 __defer_N */
      char fname[64];
      snprintf(fname, sizeof(fname), "__defer_%u", sema->defer_seq++);
      size_t nlen = strlen(fname);
      char *name_buf = (char *)arena_calloc(
          sema->arena, nlen + 1, 1, ALIGNOF(max_align_t));
      memcpy(name_buf, fname, nlen);
      fn->name = strslice_from_bytes(name_buf, nlen);

      /* 替换语句链：AST_DEFER 节点 → closure（AST_FUNC_DEF） */
      fn->base.next = s->next;
      if (prev) prev->next = &fn->base;
      else block->stmts = &fn->base;

      /* 登记 defers（build_local_func 在正式遍历时走，
         但 defers 收集需要在 build_block 遍历前完成——
         这里先登记，build_local_func 在遍历时处理注册）。
         注意：build_local_func 会注册符号 + 分配 fid + 建 fscope，
         我们在正式遍历时让它走，这里只登记 defers 数组。 */
      sema_scope_add_defer(scope, &fn->base);

      /* 准备块尾 call 语句（逆序：后登记的先 call，插到 call 链首） */
      ast_node_t *call = make_defer_call(sema, fname, s->tok_begin, s->tok_end);
      if (!defer_calls_head) {
        defer_calls_head = call;
        defer_calls_tail = call;
      } else {
        call->next = defer_calls_head;
        defer_calls_head = call;
      }

      prev = &fn->base;
      continue;
    }

    /* return/break/continue：沿 scope parent 链收集沿途 defer calls，
       挂载到退出点节点的 defers 字段（不修改语句链）。
       compiler 编译 return 时先求值返回值压栈，然后编译 defers 链中的
       call 语句，最后发 RET——保证返回值在 defer 之前求值，defer 在
       实际跳转之前执行。break/continue 同理：先编译 defers 再 POP+JMP。
       不引入临时变量，避免 fatal/ref/own 语义被 var 定义改变。 */
    if (s->kind == AST_RETURN || s->kind == AST_BREAK ||
        s->kind == AST_CONTINUE) {
      const sema_scope_t *stop = (s->kind == AST_RETURN)
                                     ? find_func_stop(scope)
                                     : find_loop_stop(scope);
      ast_node_t *tail = NULL;
      ast_node_t *calls = collect_defer_calls(sema, scope, stop, &tail);
      s->defers = calls; /* NULL 或 call 链首（compiler 消费） */
    }

    prev = s;
  }

  /* 块尾正常退出路径：追加逆序 call 序列到语句链末尾。
     这些 defer calls 只在块正常走到末尾（无 return/break/continue 跳出）
     时执行——有 return 的路径由 return 节点的 defers 字段处理，不会
     走到块尾。 */
  if (defer_calls_head) {
    if (!block->stmts) {
      block->stmts = defer_calls_head;
    } else {
      ast_node_t *tail = block->stmts;
      while (tail->next) tail = tail->next;
      tail->next = defer_calls_head;
    }
    block->stmts_last = defer_calls_tail;
  }
}

static build_result_t build_block(sema_t *sema, ast_block_t *block,
                                  sema_scope_t *scope) {
  /* defer 语法糖改写：在正式遍历前把 AST_DEFER 替换为 AST_FUNC_DEF +
     退出点 call，后续遍历与 Pass 3b 完全不感知 defer。 */
  desugar_defers(sema, block, scope);

  build_result_t r = {0};
  for (ast_node_t *s = block->stmts; s; s = s->next) {
    if (r.definitely_returns) {
      /* 编译器生成的 defer call 语句（tok_begin==0）跳过 unreachable 检查——
         它们是作用域退出点清理代码，在 return 之后仍需存在（正常落到块尾
         路径执行），不是用户代码。Pass 3b 同步跳过。 */
      if (s->tok_begin == 0) continue;
      /* 严格检查：return 后不可达语句报错。
         不建作用域、不注册符号——Pass 3b 同步跳过（索引保持对齐）。 */
      diag_error(sema->diag, sema_loc(sema, s), "unreachable statement");
      continue;
    }
    switch (s->kind) {
      case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)s;
        const type_t *vt = NULL;
        /* 所有权须在 sema_resolve_type_slot 之前提取——该函数会把
           AST_PTR/AST_SLICE_TYPE 折叠为 AST_TYPE_REF，丢失 ownership token。 */
        ownership_t var_own = vd->type_expr
                                  ? sema_type_expr_ownership(vd->type_expr)
                                  : OWN_NONE;
        if (vd->type_expr) {
          /* 类型槽位解析：若名字被待绑定局部 type 遮蔽（type_shadowed_by_local_def），
             跳过 vm scope 解析（会错误落到外层全局），sym->type 留 NULL 由 3b
             shadow_var_def 定义点后兜底；否则按常规解析，失败也留 NULL
             （3b 补报 "unknown type"）。

             另两类同样推迟到 3b：
             - 槽位含 sizeof/alignof/typeof（type_has_ct_op，依赖 3b shadow 状态）；
             - 槽位含**非字面量数组边界**且作用域里有尚未求值的 comptime var
               （`comptime var C = 3; var a: [C]i32` —— C 的常量在 3b 定义点求值，
               3a 解析会因值未就绪失败并记录硬诊断，被 sema 的快速失败闸门挡掉
               3b，使本地 comptime var 完全不可用）。 */
          if (!type_shadowed_by_local_def(sema, scope, vd->type_expr) &&
              !type_has_ct_op(vd->type_expr) &&
              !(type_has_nonliteral_array_bound(vd->type_expr) &&
                scope_has_unevaluated_comptime(scope))) {
            vt = sema_resolve_type_slot(sema, &vd->type_expr);
          }
        }
        sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = vt,
                              .is_comptime = vd->is_comptime,
                              .ownership = var_own};
        if (!sema_scope_define(scope, vd->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate variable '%.*s'", (int)vd->name.len,
                     vd->name.ptr);
        }
        break;
      }
      case AST_TYPE_DEF: {
        /* 局部 type 定义：注册符号（暂不激活，Pass 3b 定义点求值后激活）。
           类型名经 type_lookup（vm scope）解析，无需 type 字段；ast 指向
           定义节点（3a 判别"待绑定局部 type"遮蔽场景用）。 */
        ast_type_def_t *td = (ast_type_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)td};
        if (!sema_scope_define(scope, td->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)td->name.len,
                     td->name.ptr);
        }
        break;
      }
      case AST_ENUM_DEF: {
        /* 局部 enum 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_enum_def 求值后激活）。与局部 type 定义同构——ast
           指向定义节点；sema_eval_enum_def 经 sema_scope_find_local 取符号
           后写 type/激活。 */
        ast_enum_def_t *ed = (ast_enum_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)ed};
        if (!sema_scope_define(scope, ed->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)ed->name.len,
                     ed->name.ptr);
        }
        break;
      }
      case AST_STRUCT_DEF: {
        /* 局部 struct 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_struct_def 求值后激活）。与局部 enum/type 定义同构。 */
        ast_struct_def_t *sd = (ast_struct_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)sd};
        if (!sema_scope_define(scope, sd->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)sd->name.len,
                     sd->name.ptr);
        }
        break;
      }
      case AST_UNION_DEF: {
        /* 局部 union 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_union_def 求值后激活）。与局部 struct/enum/type 定义
           同构（struct 同构平铺字段语法）。 */
        ast_union_def_t *ud = (ast_union_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)ud};
        if (!sema_scope_define(scope, ud->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)ud->name.len,
                     ud->name.ptr);
        }
        break;
      }
      case AST_CUNION_DEF: {
        /* 局部 cunion 定义：注册符号（暂不激活，Pass 3b walk_block 入口提升
           调 sema_eval_cunion_def 求值后激活）。与局部 union/struct/enum/type
           定义同构（struct 同构平铺字段语法）。 */
        ast_cunion_def_t *cd = (ast_cunion_def_t *)s;
        sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = (ast_node_t *)cd};
        if (!sema_scope_define(scope, cd->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, s),
                     "duplicate name '%.*s'", (int)cd->name.len,
                     cd->name.ptr);
        }
        break;
      }
      case AST_FUNC_DEF:
        /* 局部函数定义：注册符号 + 登记队列 + 建 fscope 树（提升语义，
           整个块内可见）。定义点不摘除（进入字节码，运行时 DEFINE 绑定）。
           3b walk_block 入口提升签名、主循环跳过（消费 fscope 子作用域）。 */
        build_local_func(sema, s, scope);
        break;
      /* AST_DEFER 已在 desugar_defers 中改写为 AST_FUNC_DEF + 退出点 call，
         到达此处时已不存在 AST_DEFER 节点。 */
      case AST_BLOCK: {
        sema_scope_t *child =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, child);
        build_result_t cr = build_block(sema, (ast_block_t *)s, child);
        if (cr.definitely_returns) r.definitely_returns = true;
        break;
      }
      case AST_IF: {
        ast_if_t *it = (ast_if_t *)s;
        sema_scope_t *then_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, then_scope);
        build_result_t tr = build_block(sema, (ast_block_t *)it->then_body,
                                        then_scope);
        build_result_t er = {0};
        if (it->else_body) {
          if (it->else_body->kind == AST_IF) {
            /* else-if 链：同层递归（子作用域顺序与 3b 一致） */
            er = build_func_if(sema, (ast_if_t *)it->else_body, scope);
          } else {
            sema_scope_t *else_scope =
                sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
            sema_scope_add_child(scope, else_scope);
            er = build_block(sema, (ast_block_t *)it->else_body, else_scope);
          }
        }
        if (tr.definitely_returns && er.definitely_returns)
          r.definitely_returns = true;
        break;
      }
      case AST_SWITCH: {
        /* switch 分支体各建子作用域（声明序，与 3b walk 消费序一致）。
           default 分支最后消费。无 fallthrough，break/continue 不涉及
           switch（loop_depth 不变）——case 体自然结束。 */
        ast_switch_t *sw = (ast_switch_t *)s;
        bool all_return = true;
        for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
          ast_switch_case_t *sc = (ast_switch_case_t *)cs;
          sema_scope_t *case_scope =
              sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
          sema_scope_add_child(scope, case_scope);
          build_result_t br = build_block(sema, (ast_block_t *)sc->body,
                                          case_scope);
          if (!br.definitely_returns) all_return = false;
        }
        if (sw->default_body) {
          sema_scope_t *def_scope =
              sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
          sema_scope_add_child(scope, def_scope);
          build_result_t dr = build_block(sema, (ast_block_t *)sw->default_body,
                                          def_scope);
          if (!dr.definitely_returns) all_return = false;
        }
        /* 全部分支返回才贡献 definitely_returns（无 default 时可能不匹配
           任何分支直接穿透，不贡献） */
        if (sw->default_body && all_return)
          r.definitely_returns = true;
        break;
      }
      case AST_WHILE: {
        ast_while_t *wl = (ast_while_t *)s;
        sema_scope_t *body_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, body_scope);
        sema->fn.loop_depth++;
        build_block(sema, (ast_block_t *)wl->body, body_scope);
        sema->fn.loop_depth--;
        break; /* 循环体可能不执行，不贡献 definitely_returns */
      }
      case AST_DOWHILE: {
        /* do-while 与 while 同构：body 建子作用域、loop_depth 包裹。
           循环体至少执行一次，但条件不满足时仍可能不返回，保守不贡献
           definitely_returns（与 while 一致）。 */
        ast_dowhile_t *dw = (ast_dowhile_t *)s;
        sema_scope_t *body_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
        sema_scope_add_child(scope, body_scope);
        sema->fn.loop_depth++;
        build_block(sema, (ast_block_t *)dw->body, body_scope);
        sema->fn.loop_depth--;
        break;
      }
      case AST_FOR: {
        ast_for_t *fr = (ast_for_t *)s;
        sema_scope_t *for_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FOR, scope);
        sema_scope_add_child(scope, for_scope);
        /* init 变量注册到 for scope */
        if (fr->init && fr->init->kind == AST_VAR_DEF) {
          ast_var_def_t *vd = (ast_var_def_t *)fr->init;
          ownership_t for_own = vd->type_expr
                                    ? sema_type_expr_ownership(vd->type_expr)
                                    : OWN_NONE;
          const type_t *vt = NULL;
          if (vd->type_expr) {
            /* 同 AST_VAR_DEF：局部 type 遮蔽则推迟，否则常规解析（失败留
               NULL 由 3b 兜底） */
            if (!type_shadowed_by_local_def(sema, scope, vd->type_expr) &&
                !type_has_ct_op(vd->type_expr)) {
              vt = sema_resolve_type_slot(sema, &vd->type_expr);
            }
          }
          sema_symbol_t init_sym = {.kind = SEMA_SYM_VAR, .type = vt,
                                    .ownership = for_own};
          if (!sema_scope_define(for_scope, vd->name, &init_sym)) {
            diag_error(sema->diag, sema_loc(sema, fr->init),
                       "duplicate variable '%.*s'", (int)vd->name.len,
                       vd->name.ptr);
          }
        }
        /* body 是 for scope 的子 scope */
        sema_scope_t *body_scope =
            sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, for_scope);
        sema_scope_add_child(for_scope, body_scope);
        sema->fn.loop_depth++;
        build_block(sema, (ast_block_t *)fr->body, body_scope);
        sema->fn.loop_depth--;
        break; /* 循环体可能不执行，不贡献 definitely_returns */
      }
      case AST_RETURN:
        r.definitely_returns = true;
        break; /* 后续语句在循环顶部报 unreachable */
      case AST_BREAK:
      case AST_CONTINUE:
        if (sema->fn.loop_depth == 0) {
          diag_error(sema->diag, sema_loc(sema, s), "'%.*s' outside loop",
                     (int)(s->kind == AST_BREAK ? 5 : 8),
                     s->kind == AST_BREAK ? "break" : "continue");
        }
        break;
      default:
        break; /* 其他语句不创建作用域 */
    }
  }
  return r;
}

/* if 语句的作用域构建入口（含 else-if 同层递归） */
static build_result_t build_func_if(sema_t *sema, ast_if_t *it,
                                    sema_scope_t *scope) {
  sema_scope_t *then_scope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
  sema_scope_add_child(scope, then_scope);
  build_result_t tr =
      build_block(sema, (ast_block_t *)it->then_body, then_scope);
  build_result_t er = {0};
  if (it->else_body) {
    if (it->else_body->kind == AST_IF) {
      er = build_func_if(sema, (ast_if_t *)it->else_body, scope);
    } else {
      sema_scope_t *else_scope =
          sema_scope_new(sema->vm->alloc, SEMA_SCOPE_BLOCK, scope);
      sema_scope_add_child(scope, else_scope);
      er = build_block(sema, (ast_block_t *)it->else_body, else_scope);
    }
  }
  return (build_result_t){.definitely_returns =
                              tr.definitely_returns && er.definitely_returns};
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

/* 捕获解析（函数"定义点"，外层 scope 在线）：
   - 纯 id 捕获：外层符号查找（变量必须确定已初始化——捕获即读值）+ 类型
     解析（从外层符号 type）写回 fscope 捕获符号
   - 括号捕获：init 在外层作用域求值（定义点）+ 显式 type_expr 校验 /
     无类型时从 init 推断，类型写回 fscope 捕获符号
   调用点：walk_block AST_FUNC_DEF 分支（局部函数，定义点在父函数 walk 内，
   先于 sema_walk_function）与 sema_check_func_literal（函数字面量，表达式
   求值点同步）。fscope 捕获符号 type 就绪后，sema_walk_function /
   sema_check_func_literal 据此定义捕获 shadow value（函数体内 lookup 命中）。 */
void resolve_func_captures(sema_t *sema, ast_func_def_t *fn,
                           sema_scope_t *fscope, sema_scope_t *outer) {
  if (!fn->captures || !fscope) return;
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (!cs) continue; /* 3a 拒绝（comptime/全局/重名）已诊断，跳过防级联 */
    cs->is_capture = true;  /* 捕获槽：闭包内不可 move（见 symbol.h 说明） */

    if (cv->init) {
      /* 括号捕获：init 定义点在外层作用域求值（c+d 引用外层变量） */
      value_t *init = sema_expr(sema, &cv->init, outer);
      bool init_bad = value_is_error(sema->vm, init) ||
                      value_is_type(init, TYPE_KIND_VOID);
      if (cv->type_expr) {
        /* 显式类型：3a 已解析（或失败留 NULL）→ 兜底重解析 + 赋性校验 */
        if (!cs->type) {
          cs->type = sema_resolve_type_slot(sema, &cv->type_expr);
        }
        if (!init_bad && cs->type) {
          value_t *dst = value_make_shadow(sema->vm, cs->type);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, init))) {
            char tn[64], itn[64];
            sema_type_name(cs->type, tn, sizeof(tn));
            sema_type_name(value_type(init), itn, sizeof(itn));
            diag_error(sema->diag, sema_loc(sema, cv->init),
                       "cannot initialize capture '%.*s' of type %s with %s",
                       (int)cv->name.len, cv->name.ptr, tn, itn);
          }
        }
      } else {
        /* 推断类型：init 类型即捕获类型。
           §9.2 fatal 接管为 own：move(p)/clone(p) 产 fatal *T，闭包内
           无 fatal 变量——fatal 自动转 own *T（闭包成为所有权持有者）。 */
        const type_t *it = init_bad ? sema->vm->type_void : value_type(init);
        if (!init_bad && sema_value_is_fatal(init)) {
          const type_t *base = ptr_type_base(it);
          it = base ? type_ptr_intern(sema->vm, base) : it;
          /* 捕获接管了将亡值 → 该捕获槽**持有所有权**（OWN）。
             这决定了闭包本身的产出状态：捕获了 own 的闭包即 OWN。 */
          cs->ownership = OWN_OWN;
        }
        cs->type = it;
      }
    } else {
      /* 纯 id 捕获：外层符号（定义点 block scope 沿链查找） */
      sema_symbol_t *outer_sym = sema_lookup(outer, cv->name);
      if (!outer_sym) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture undefined variable '%.*s'",
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
      if (outer_sym->kind != SEMA_SYM_VAR &&
          outer_sym->kind != SEMA_SYM_FUNC) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture '%.*s': not a variable or function",
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
      /* 函数符号（局部函数自身/兄弟）提升即存在（is_active=true），无
         flow_init 概念——跳过初始化检查（运行时绑定在 STORE 后沿作用域链
         取到定义点实例或提升基底）。变量捕获要求确定已初始化。 */
      if (outer_sym->kind == SEMA_SYM_VAR && !outer_sym->flow_init) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture '%.*s' before initialization",
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
      cs->type = outer_sym->type; /* 捕获 clone 值，类型即外层变量类型 */
      cs->ownership = outer_sym->ownership; /* 所有权跟随 value/symbol */
      /* §9.1 own 裸捕获禁止：纯 id 捕获 own *T = copy（own 禁止 copy，
         §3.1）→ 必须显式 move()/clone()。fatal 同理（裸捕获 fatal 变量
         = copy 将亡值，须 move/clone 接管）。 */
      if (outer_sym->type &&
          (outer_sym->ownership == OWN_OWN ||
           sema_sym_is_fatal(outer_sym))) {
        diag_error(sema->diag, sema_loc(sema, c),
                   "cannot capture own/fatal pointer '%.*s' by value "
                   "(use move() or clone(), e.g. |(%.*s = move(%.*s))|)",
                   (int)cv->name.len, cv->name.ptr,
                   (int)cv->name.len, cv->name.ptr,
                   (int)cv->name.len, cv->name.ptr);
        continue;
      }
    }
    cs->flow_init = true; /* 捕获即初始化（闭包持有 clone 值） */
    cs->is_active = true; /* 函数体 walk 时可见（sema_lookup 激活过滤） */
    /* 把解析出的所有权**回写进 AST 节点**（`ast_var_def_t::ownership`）。
       用途：闭包字面量产出值的状态判定——捕获了 own 的闭包本身是 **OWN**
       （唯一的 own 产生式：函数定义即"定义 + 绑定名字"，折叠了
       fatal→move→own）。消费者是 `ex_func_def`，它只能从 AST 读。 */
    cv->ownership = (uint8_t)cs->ownership;
  }

  /* §9.3 ref/opaque 捕获强制标注（m3-design §9）：闭包是隐式 struct，
     捕获 ref/opaque 视同 struct ref 字段——闭包整体必须带作用域标注
     func '<x> |...|（同 §5 强制规则 B）。无标注 → 编译错误。
     有标注 → 逃逸检查：闭包定义作用域须嵌套于标注锚点作用域。 */
  bool has_ref_capture = false;
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (!cs || !cs->type) continue;
    if (cs->ownership == OWN_REF ||
        cs->type->kind == TYPE_KIND_OPAQUE) {
      has_ref_capture = true;
      break;
    }
  }
  if (has_ref_capture && fn->scope_annot_count == 0) {
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
               "closure capturing ref/opaque must declare a scope annotation "
               "(e.g. func '<x> |...| or defer '<x> |...|)");
  }
  /* §9.3 逃逸检查：有标注时，闭包定义作用域（outer）须嵌套于每个锚点。
     复用 stmt.c 的 annot_anchor_scope / scope_within——但它们是 static，
     此处内联简化版本：标注名解析为符号 own_scope，比较 outer ⊆ anchor。 */
  if (has_ref_capture && fn->scope_annot_count > 0) {
    for (size_t i = 0; i < fn->scope_annot_count; i++) {
      strslice_t nm = fn->scope_annot_names[i];
      sema_scope_t *anchor = NULL;
      if (nm.len == 1 && nm.ptr[0] == '*') {
        anchor = sema->global_scope;
      } else if (sema->fn.def) {
        for (ast_node_t *p = sema->fn.def->params; p; p = p->next) {
          ast_var_def_t *pvd = (ast_var_def_t *)p;
          if (pvd->name.len == nm.len &&
              memcmp(pvd->name.ptr, nm.ptr, nm.len) == 0) {
            anchor = sema->fn.local_func_param_scope;
            break;
          }
        }
      }
      if (!anchor) {
        sema_symbol_t *sym = sema_lookup(outer, nm);
        if (sym && sym->kind == SEMA_SYM_VAR) anchor = sym->own_scope;
      }
      if (!anchor) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "scope annotation '%.*s' does not name a parameter, "
                   "global or local variable",
                   (int)nm.len, nm.ptr);
        continue;
      }
      if (!sema_scope_within(outer, anchor)) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                   "closure escapes scope annotation '%.*s' (its scope is "
                   "not nested within the annotated scope)",
                   (int)nm.len, nm.ptr);
      }
    }
  }
}

/* ===========================================================================
 * 函数字面量（表达式内 AST_FUNC_DEF，sema_expr 求值用）
 *
 * 与局部函数共享 AST_FUNC_DEF 节点，但语义差异：不注册作用域符号、不提升。
 * 在表达式求值点同步完成全部检查：
 *
 *   1. 临时 fscope（parent = 定义点作用域）——不 add_child 挂树：不消费
 *      外层 walk_block 的子作用域索引（idx 对齐不受影响），树由本函数
 *      自持并销毁（含 body 内局部函数的 fscope）。
 *   2. 建树（build_block）：注册参数 + body 内局部函数登记队列 +
 *      嵌套块作用域（临时树）；返回路径完整性分析（非 void 必须全路径
 *      return，与函数定义同语义）。
 *   3. 签名解析（与 resolve_local_func_sig 同构）：type_func_sig 建签名 →
 *      sema_type_register 登记 → fn->sig_id（compiler LOAD_TYPE 用）。
 *   4. walk body（sema_walk_block，仿 sema_walk_function）：参数 shadow
 *      定义到临时 VM scope；local_func_base = fscope → 字面量引用外层局部
 *      （闭包）编译期拒绝。
 *   5. 同步 walk body 内登记的局部函数（sema->funcs[n0..] 队列尾）并标记
 *      is_literal_owned——Pass 3b 驱动循环据此跳过（防二次 walk 访问已
 *      销毁的临时作用域树）。嵌套字面量已递归完成，跳过。
 *   6. 销毁临时作用域树。
 *
 * 返回签名类型（func_type_t，vm 池 intern）；失败返回 NULL（已诊断）。
 * =========================================================================== */
const type_t *sema_check_func_literal(sema_t *sema, ast_func_def_t *fn,
                                      sema_scope_t *outer) {
  /* fid 单一来源：创建函数对象（字面量）即分配。CTFE 求值（comptime body
     内字面量）可能已分配——幂等复用；compiler LOAD_FUNCTION <fid> 用 */
  sema_func_id_alloc(sema, fn);

  /* 队列登记起点：body 内局部函数（含嵌套字面量内）追加到队尾 */
  size_t n0 = vec_len(sema->funcs);

  sema_scope_t *fscope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FUNCTION, outer);
  if (!fscope) panic("sema: out of memory allocating literal fscope");

  sema_scope_t *pscope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_FUNCTION, fscope);
  sema_scope_add_child(fscope, pscope); /* 参数层（fscope 子）——参数遮蔽捕获 */

  /* 参数注册（与 build_func_tree 同构，注册到参数层）+ 签名参数类型收集 */
  size_t nparams = sema_count_siblings(fn->params);
  const type_t **ptypes = NULL;
  ownership_t *powns = NULL;
  if (nparams > 0) {
    ptypes = allocator_new_ex(sema->vm->alloc, "type_t*", sizeof(type_t *),
                              NULL, NULL, NULL, nparams);
    powns = allocator_new_ex(sema->vm->alloc, "ownership_t", sizeof(ownership_t),
                             NULL, NULL, NULL, nparams);
  }
  size_t j = 0;
  for (ast_node_t *p = fn->params; p; p = p->next, j++) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    ownership_t pown = sema_type_expr_ownership(vd->type_expr);
    const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
    if (!t) {
      diag_error(sema->diag, sema_loc(sema, p), "unknown type in parameter '%.*s'",
                 (int)vd->name.len, vd->name.ptr);
    }
    if (ptypes) ptypes[j] = t; /* 失败置 NULL，位置对齐，签名校验时跳过 */
    if (powns) powns[j] = pown;
    sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = t,
                          .ownership = pown};
    if (!sema_scope_define(pscope, vd->name, &init)) {
      diag_error(sema->diag, sema_loc(sema, p), "duplicate parameter '%.*s'",
                 (int)vd->name.len, vd->name.ptr);
    }
  }

  /* 捕获符号注册（与 build_func_tree 同构，注册到 fscope 父层）：纯 id =
     外层变量（type 3b 从外层符号解析）；括号 = 临时构造（显式 type_expr
     解析，无则 init 推断）。comptime 字面量无运行时闭包，拒绝。 */
  if (fn->captures) {
    if (fn->is_comptime) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                 "comptime function literal cannot have captures");
      fn->captures = NULL; /* 摘除：错误已诊断，防级联 */
    } else {
      for (ast_node_t *c = fn->captures; c; c = c->next) {
        ast_var_def_t *cv = (ast_var_def_t *)c;
        ownership_t cown = cv->type_expr
                              ? sema_type_expr_ownership(cv->type_expr)
                              : OWN_NONE;
        const type_t *ct = NULL;
        if (cv->type_expr) {
          ct = sema_resolve_type_slot(sema, &cv->type_expr);
        }
        sema_symbol_t init = {.kind = SEMA_SYM_VAR, .type = ct,
                              .ownership = cown};
        if (!sema_scope_define(fscope, cv->name, &init)) {
          diag_error(sema->diag, sema_loc(sema, c),
                     "capture '%.*s' duplicates another capture",
                     (int)cv->name.len, cv->name.ptr);
        }
      }
    }
  }

  /* 建树：body 内局部函数登记队列 + 嵌套块作用域（临时树，自持） */
  build_result_t r = build_block(sema, (ast_block_t *)fn->body, pscope);

  /* 捕获解析（定义点即表达式求值点，外层 scope 在线）：纯 id 类型/flow_init、
     括号 init 求值与类型校验，写回 fscope 捕获符号。 */
  resolve_func_captures(sema, fn, fscope, outer);

  /* 返回路径完整性：非 void 字面量所有路径必须 return */
  ownership_t lit_ret_own = fn->return_expr ? sema_type_expr_ownership(fn->return_expr) : OWN_NONE;
  const type_t *rt = NULL;
  if (fn->return_expr) {
    rt = sema_resolve_type_slot(sema, &fn->return_expr);
    if (!rt) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                 "unknown return type in function literal");
    }
  }
  /* §5 强制规则（m3-design §5）：返回 ref 必标注（集合 = 参数名 ∪ {*}）。 */
  sema_check_ret_ref_annot(sema, fn, rt, lit_ret_own);
  if (rt && rt->kind != TYPE_KIND_VOID && !r.definitely_returns) {
    char nb[128];
    if (fn->name.len)
      name_to_cstr(fn->name, nb, sizeof nb);
    else
      snprintf(nb, sizeof nb, "<anonymous>");
    diag_error(sema->diag, sema_loc(sema, &fn->base),
               "function literal '%s' must return a value on all paths", nb);
  }

  /* 签名解析 + 登记（compiler hoist LOAD_TYPE <sig_id> 用） */
  ownership_t ret_own = fn->return_expr ? sema_type_expr_ownership(fn->return_expr) : OWN_NONE;
  const type_t *sig = type_func_sig(sema->vm, ptypes, powns, nparams, rt, ret_own, false);
  const sema_type_t *st = sema_type_register(sema, sig);
  if (st) fn->sig_id = st->id;
  if (ptypes) allocator_free(sema->vm->alloc, (void **)&ptypes);
  if (powns) allocator_free(sema->vm->alloc, (void **)&powns);

  /* walk body：捕获 shadow 定义到捕获层 VM scope，参数 shadow 定义到参数层
     VM scope（子层，遮蔽捕获——与运行时 func_vcall 结构一致）；捕获检查按
     字面量 fscope + param_scope 生效（引用外层局部 = 闭包，拒绝）。保存/
     恢复外层状态——本函数嵌套在外层 walk 中调用（外层可能正 walk 局部
     函数体，local_func_base 非空）。**整体保存/恢复 sema->fn**：逐字段
     备份极易漏字段（上下文新增字段时静默失配）。 */
  sema_fn_ctx_t saved = sema_fn_ctx_enter(sema);

  sema->fn.local_func_base = fscope;
  sema->fn.local_func_param_scope = pscope;
  sema->fn.def = fn; /* walk_return §6/§9 判定用（captures 查当前闭包） */
  sema->fn.return_type = rt;
  sema->fn.return_ownership = lit_ret_own; /* 折叠前提取（同 L1091） */
  /* 字面量 body 是真实调用点（最终进入运行时字节码）：body 内 comptime
     调用照常折叠（即使本字面量定义在 comptime func body 内）。 */
  sema->fn.walking_comptime = false;

  vm_push_scope(sema->vm); /* 捕获层（父） */
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (!cs) continue; /* 3a 拒绝已诊断，跳过防级联 */
    value_t *cv_value = value_make_shadow(
        sema->vm, cs->type ? cs->type : sema->vm->type_void);
    char nb[256];
    name_to_cstr(cv->name, nb, sizeof nb);
    scope_define(sema->vm, sema->vm->current_scope, nb, cv_value);
  }
  vm_push_scope(sema->vm); /* 参数层（子，遮蔽捕获） */
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    sema_symbol_t *ps = sema_scope_find_local(pscope, vd->name);
    if (ps) ps->flow_init = true; /* 参数由调用方传入，确定已初始化 */
    value_t *pv = value_make_shadow(
        sema->vm, ps && ps->type ? ps->type : sema->vm->type_void);
    char nb[256];
    name_to_cstr(vd->name, nb, sizeof nb);
    scope_define(sema->vm, sema->vm->current_scope, nb, pv);
  }
  sema_walk_block(sema, fn->body, pscope);
  vm_pop_scope(sema->vm);
  vm_pop_scope(sema->vm);

  /* 同步 walk body 内登记的局部函数（sema->funcs[n0..]）并标记。
     comptime 局部函数同样 walk（类型检查 + body 内语句折叠），不生成
     运行时字节码。 */
  for (size_t i = n0; i < vec_len(sema->funcs); i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    if (sf->is_literal_owned) continue; /* 嵌套字面量已同步 walk */
    sf->is_literal_owned = true;
    sema_walk_function(sema, sf);
  }

  /* 恢复外层 walk 上下文（整体恢复，见入口保存） */
  sema_fn_ctx_leave(sema, saved);

  /* 销毁临时作用域树（含 body 局部函数 fscope） */
  sema_scope_destroy(&fscope);

  return sig;
}
