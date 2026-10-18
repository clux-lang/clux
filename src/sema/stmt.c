#include "sema/sema.h"
#include <stdio.h>
#include "parser/ast_array.h"
#include "parser/ast_walk.h"
#include "parser/ast_assign.h"
#include "parser/ast_walk.h"
#include "parser/ast_binary.h"
#include "parser/ast_walk.h"
#include "parser/ast_call.h"
#include "parser/ast_walk.h"
#include "parser/ast_const.h"
#include "parser/ast_walk.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"  /* ast_construct_field_t::value */
#include "parser/ast_walk.h"
#include "parser/ast_fill.h"
#include "parser/ast_walk.h"
#include "parser/ast_func_type.h"
#include "parser/ast_walk.h"
#include "parser/ast_ident.h"
#include "parser/ast_walk.h"
#include "parser/ast_nil.h"
#include "parser/ast_walk.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_for_index.h"
#include "parser/ast_move.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_addr.h"
#include "parser/ast_new.h"
#include "parser/ast_undef.h"
#include "parser/ast_var_def.h"
#include "parser/ast_index.h"
#include "parser/ast_walk.h"
#include "parser/ast_member.h"
#include "parser/ast_walk.h"
#include "parser/ast_block.h"
#include "parser/ast_walk.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_walk.h"
#include "parser/ast_for.h"
#include "parser/ast_walk.h"
#include "parser/ast_func_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_if.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_walk.h"
#include "parser/ast_return.h"
#include "parser/ast_walk.h"
#include "parser/ast_switch.h"
#include "parser/ast_walk.h"
#include "parser/ast_ternary.h"
#include "parser/ast_walk.h"
#include "parser/ast_type_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_walk.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_union_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_unary.h"
#include "parser/ast_walk.h"
#include "parser/ast_var_def.h"
#include "parser/ast_walk.h"
#include "parser/ast_volatile.h"
#include "parser/ast_walk.h"
#include "parser/ast_while.h"
#include "parser/ast_walk.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_walk.h"
#include "parser/lexer.h"
#include "parser/ast_move.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_addr.h"
#include "parser/ast_new.h"   /* ast_move_t（借源收集剥 move/clone） */
#include "parser/ast_addr.h"
#include "parser/ast_new.h"   /* ast_addr_t（x.& 的借源是取址目标） */
#include "parser/ast_walk.h"
#include "sema/comptime.h"
#include "sema/struct_compose.h" /* 嵌入继承的成员名字查找 */
#include "vm/type_array.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "parser/ast_deref.h"
#include "parser/ast_scope_ref.h" /* M6 FFI：extern var 赋值目标的 AST 标志 */
#include "parser/ast_walk.h"
#include "vm/ffi_lib.h"          /* M6 FFI：ffi_data_kind_of（数据符号类型码） */
#include "parser/ast_free.h"     /* AST_FREE：sema 生成的 own 变量释放语句 */
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * Pass 3b：Shadow VM 运行
 *
 * 按预建作用域树严格对应遍历 AST。每个块用独立局部索引遍历自己的
 * children：build 阶段子作用域按出现顺序 add_child，walk 必须逐层
 * 独立取用（共享索引会在嵌套时污染外层，导致后续作用域错位）。
 * =========================================================================== */

/* ---- Step B 所有权检查辅助（m3-design §3/§7） ---- */

/* R1/R2 判定：赋值 rhs 是否是 own 指针的合法初始化源。
   own *T 变量只允许两种初始化来源：
   - new 表达式（AST_NEW）：新堆块直接产 own *T（§8.1）
   - fatal 接管源（move/clone 产物或 fatal 参数，§3.3）
   ref 派生（own → ref 隐式转换）是借用（ref 总借用），不能赋回 own。
   own → own 直接赋值 = copy → 编译错误。
   非 static：AST_NEW/AST_CONSTRUCT 字段校验（sema/expr.c）共用。 */

typedef struct block_result {
  bool definitely_returns; /* 该块保证返回（所有路径都 return） */
} block_result_t;

static block_result_t walk_block(sema_t *sema, ast_node_t *block,
                                 sema_scope_t *scope, size_t *idx);
static block_result_t walk_stmt(sema_t *sema, ast_node_t *stmt,
                                sema_scope_t *scope, size_t *idx);
static block_result_t walk_if(sema_t *sema, ast_if_t *it, sema_scope_t *scope,
                              size_t *idx);
static block_result_t walk_switch(sema_t *sema, ast_switch_t *sw,
                                  sema_scope_t *scope, size_t *idx);

/* ---- 所有权显式释放：为 scope 中 own 变量生成 `PUSH <name>` + `FREE` ----
 *
 * 只在**编译期**决定释放什么：sema 在作用域退出点遍历自己的变量符号，对
 * 所有权为 own 的变量生成两条语句——`PUSH <name>`（AST_IDENT，编译为
 * BCODE_PUSH）把变量压栈，`FREE` 消费栈顶释放它。
 *
 * **FREE 不按名解析**（不携带名字）：它只是"释放栈顶那个块"，因此类型无关，
 * 且同样能作用于无名临时值（`_ = new T{...}`）。运行期不做任何所有权判定
 * ——只为 own 变量生成，这是编译期事实。类型相关的展开（optional→判断 /
 * 数组·切片→循环 / tuple→get_index / struct→get_field）待后续按类型生成。
 *
 * 类似 DEFER 的语法糖，但由 sema 在 Pass 3b（类型已确定）自动生成。
 * 返回语句链首（按变量声明序），NULL = 无 own 变量。
 * **仅遍历本 scope 自身符号**（不沿 parent 链）——每个 scope 各自在自己的
 * 退出点生成自己的释放。 */
/* ===========================================================================
 * 销毁序列生成器（A 方案：完全 AST 生成，从 **type 对象**反推）
 *
 * 为什么不读 AST：装饰器会改造类型，所以"定义 AST"描述的是装饰器之前的形态、
 * 构造式只覆盖被写入的字段——两者都可能与最终类型不符。type 对象在装饰器之后
 * 仍是权威（槽位所有权记在 struct_field_t::ownership 上，见 type.h）。
 *
 * 判定规则（关键：区分"叶子"与"复合"）：
 *   - **叶子**（ptr / slice）：有独立分配，**仅当该槽位 ownership == OWN**
 *     才生成 FREE。ref / fatal 槽位是借用/将亡值，不归本值所有 → 不释放。
 *   - **复合**（struct / tuple / …）：值本身没有独立分配，其内部 own 槽位
 *     在结构上属于外层值 → **无条件递归**（与字段自身的 ownership 无关）。
 *
 * 这样 `struct Holder { r: ref *i32; p: own *i32 }` 的销毁序列只含 `FREE v.p`
 * ——`r` 根本不会进序列，此前"运行期递归释放所有指针字段"导致的
 * 0xC0000005（把 ref 指向的栈地址也 free 了）从根上消失。
 * =========================================================================== */

/* 把 n 追加到 (head, tail) 单链尾部。 */
static void destroy_append(ast_node_t **head, ast_node_t **tail,
                           ast_node_t *n) {
  if (!n) return;
  n->next = NULL;
  if (!*head) {
    *head = *tail = n;
  } else {
    (*tail)->next = n;
    *tail = n;
  }
}

/* 为 `access` 所指、类型为 `t` 的值生成销毁语句，追加进 (head, tail)。
 * 返回 false = 生成失败（内存不足等）。
 *
 * **方案 B（2026-10）**：含 own 槽位的**复合类型**统一走生成的销毁函数
 * `__destroy_<n>(dv: ref *T)` —— 销毁点只发 `CALL __destroy_<n>(&value)`，
 * 槽位树在函数体里只展开一次（不再在每个销毁点内联）。自引用类型（链表/树）
 * 由函数自身递归调用，天然终止（不再需要 FREE_RECURSIVE）。
 * 叶子（own 指针/切片）仍在调用点内联：`CALL 被指向值的销毁函数` + `FREE`。 */
static bool gen_destroy_impl(sema_t *sema, const type_t *t, ast_node_t *access,
                             ast_node_t **head, ast_node_t **tail);
static bool gen_destroy(sema_t *sema, const type_t *t, ast_node_t *access,
                        ast_node_t **head, ast_node_t **tail);
static bool gen_destroy_fn_call(sema_t *sema, const type_t *t,
                                ast_node_t *access, ast_node_t **head,
                                ast_node_t **tail, bool by_address);
static bool type_needs_destroy_fn(const type_t *t) {
  if (!t) return false;
  const type_t *s = type_strip_quals(t);
  if (!s) return false;
  if (s->kind == TYPE_KIND_PTR || s->kind == TYPE_KIND_SLICE) return false;
  return type_has_own_slots(s);
}

/* 注册表查找：类型对象指针（intern 后唯一）→ 已生成的销毁函数。 */
static sema_destroy_fn_t *destroy_fn_find(sema_t *sema, const type_t *t) {
  for (sema_destroy_fn_t *f = sema->destroy_fns; f; f = f->next)
    if (f->type == t) return f;
  return NULL;
}

static ast_node_t *destroy_fn_callee_node(sema_t *sema,
                                          const sema_destroy_fn_t *reg);

/* 取（必要时**生成**）类型 t 的销毁函数，返回调用用的 callee（AST_FUNC_REF）。
 *
 * 生成过程（一次）：分配 `__destroy_<n>` 名字 + fid；构造 AST_FUNC_DEF：
 *   - 形参 `dv: ref *T`（类型是 intern 的 `*T`，所有权在槽位上 = REF）；
 *   - 签名 `void(ref *T)` 登记进 sema->types（compiler hoist 用 sig_id）；
 *   - body = 销毁 T 的 own 槽位（access = `dv.*`），**先登记再生成 body**，
 *     自引用类型在此终止递归（生成 body 时再次请求自己 → 命中注册表 → CALL）。
 * 函数本身在 sema_analyze 收尾统一追加到 prog->funcs（compiler 预扫描 +
 * 全局名字绑定），因此销毁点只发 `PUSH __destroy_<n>` + `CALL 1`。 */
static ast_node_t *destroy_fn_callee(sema_t *sema, const type_t *t) {
  const type_t *s = type_strip_quals(t);
  if (!s) return NULL;
  sema_destroy_fn_t *reg = destroy_fn_find(sema, s);
  if (reg) return destroy_fn_callee_node(sema, reg);

  char nb[32];
  int nl = snprintf(nb, sizeof(nb), "__destroy_%u", sema->destroy_fn_seq++);
  if (nl <= 0 || (size_t)nl >= sizeof(nb)) return NULL;
  char *name_buf = (char *)arena_calloc(sema->arena, (size_t)nl + 1, 1,
                                        ALIGNOF(max_align_t));
  if (!name_buf) return NULL;
  memcpy(name_buf, nb, (size_t)nl);
  strslice_t name = strslice_from_bytes(name_buf, (size_t)nl);

  ast_func_def_t *fn = (ast_func_def_t *)arena_calloc(
      sema->arena, 1, sizeof(ast_func_def_t), ALIGNOF(max_align_t));
  if (!fn) return NULL;
  fn->base.kind = AST_FUNC_DEF;
  fn->base.is_generated = true;
  fn->name = name;
  sema_func_id_alloc(sema, fn);
  if (fn->fid == 0) return NULL;

  /* 形参 dv: ref *T —— 类型 = intern 的 `*T`；槽位所有权 = REF（不拥有） */
  const type_t *pt = type_ptr_intern(sema->vm, s);
  if (!pt) return NULL;
  ast_node_t *pnode = ast_var_def_new(sema->arena, 0, 0);
  ast_node_t *ptexpr = sema_gen_type_ref(sema, pt); /* 登记 *T + AST_TYPE_REF */
  if (!pnode || !ptexpr) return NULL;
  ((ast_var_def_t *)pnode)->name = strslice_from_bytes("dv", 2);
  ((ast_var_def_t *)pnode)->type_expr = ptexpr;
  ((ast_var_def_t *)pnode)->ownership = 2; /* AST_OWN_REF（与 ownership_t 同编码） */
  fn->params = pnode;

  /* 签名 void(ref *T)：登记进 sema->types（hoist 两遍扫描构造），记 sig_id */
  const type_t *sigparams[1] = {pt};
  ownership_t sigowns[1] = {OWN_REF};
  const type_t *sig = type_func_sig(sema->vm, sigparams, sigowns, 1,
                                    sema->vm->type_void, OWN_NONE, false);
  if (!sig) return NULL;
  const sema_type_t *st = sema_type_register(sema, sig);
  if (st) fn->sig_id = st->id;

  ast_node_t *blk = ast_block_new(sema->arena, 0, 0);
  if (!blk) return NULL;
  blk->is_generated = true;
  fn->body = blk;

  /* **先登记再生成 body**：自引用类型（Node/Tree）生成 body 时会再次请求
     自己的销毁函数 → 命中注册表 → 直接发 CALL，递归终止。 */
  sema_destroy_fn_t *wrap = (sema_destroy_fn_t *)arena_calloc(
      sema->arena, 1, sizeof(sema_destroy_fn_t), ALIGNOF(max_align_t));
  if (!wrap) return NULL;
  wrap->type = s;
  wrap->fn = (ast_node_t *)fn;
  wrap->name = name;
  wrap->next = sema->destroy_fns;
  sema->destroy_fns = wrap;

  /* body：销毁 T 的 own 槽位（access = `dv.*`；T 自身不释放） */
  ast_node_t *vid = ast_ident_new(sema->arena, 0, 0);
  ast_node_t *deref = ast_deref_new(sema->arena, 0, 0);
  if (!vid || !deref) return NULL;
  ((ast_ident_t *)vid)->name = strslice_from_bytes("dv", 2);
  ((ast_deref_t *)deref)->operand = vid;
  ast_node_t *bh = NULL, *bt = NULL;
  if (!gen_destroy_impl(sema, s, deref, &bh, &bt)) return NULL;
  ((ast_block_t *)blk)->stmts = bh;

  return destroy_fn_callee_node(sema, wrap);
}

/* 生成"调用类型 t 的销毁函数"语句：`CALL __destroy_<n>(<arg>)`。
 * by_address = true → 实参是 `&access`（**值**的存储地址）；
 * by_address = false → 实参就是 access 本身（它已经是**指向 T 的指针**，
 * 例如 `own *T` 槽位/变量的值）。 */
static bool gen_destroy_fn_call(sema_t *sema, const type_t *t,
                                ast_node_t *access, ast_node_t **head,
                                ast_node_t **tail, bool by_address) {
  if (!t || !access) return true;
  ast_node_t *callee = destroy_fn_callee(sema, t);
  if (!callee) return false;
  ast_node_t *arg = access;
  if (by_address) {
    arg = ast_addr_new(sema->arena, 0, 0);
    if (!arg) return false;
    ((ast_addr_t *)arg)->operand = access;   /* &value（ADDR 指令） */
  }
  ast_node_t *call = ast_call_new(sema->arena, 0, 0);
  ast_node_t *stmt = ast_expr_stmt_new(sema->arena, 0, 0);
  if (!call || !stmt) return false;
  ((ast_call_t *)call)->callee = callee;
  ((ast_call_t *)call)->args = arg;
  call->is_generated = true;
  ((ast_expr_stmt_t *)stmt)->expr = call;   /* 表达式语句：结果 POP */
  stmt->is_generated = true;
  destroy_append(head, tail, stmt);
  return true;
}

/* 闭包 env 的 vtable 生成（见 ast_func_def_t::env_dispose_fid / env_clone_fid）：
   直接复用"生成的销毁函数 / clone 函数"两个生成器 —— env 是普通 struct 类型，
   所以正是 `__destroy_<env>(dv: ref *Env)` 与 `__clone_<env>(dv: ref *Env):
   fatal Env`。生成的函数在 sema_analyze 收尾统一追加到 prog->funcs。 */
bool sema_gen_env_vtable(sema_t *sema, const type_t *env_type,
                         uint32_t *dispose_fid, uint32_t *clone_fid) {
  if (dispose_fid) *dispose_fid = 0;
  if (clone_fid) *clone_fid = 0;
  const type_t *env = env_type ? type_strip_quals(env_type) : NULL;
  if (!sema || !env || env->kind != TYPE_KIND_STRUCT) return false;

  /* 销毁：只有含 own 槽位的 env 才需要 dispose。 */
  if (type_has_own_slots(env)) {
    ast_node_t *callee = destroy_fn_callee(sema, env);
    if (!callee) return false;
    if (dispose_fid) *dispose_fid = ((const ast_func_ref_t *)callee)->fid;
  }
  /* clone：含 own 槽位（或指针/切片槽位）才需要深拷贝 env。 */
  {
    bool need = type_has_own_slots(env);
    if (!need) {
      /* env 里可能有 own 指针/切片槽位但 type_has_own_slots 已覆盖；这里只在
         明确含资源时生成（否则 clone = 字节拷贝，不需要函数）。 */
      for (size_t i = 0; i < ((const struct_type_t *)env)->field_count && !need;
           i++) {
        const struct_field_t *f = &((const struct_type_t *)env)->fields[i];
        const type_t *ft = f ? type_strip_quals(f->type) : NULL;
        if (ft && (ft->kind == TYPE_KIND_PTR || ft->kind == TYPE_KIND_SLICE) &&
            f->ownership == OWN_OWN)
          need = true;
      }
    }
    if (need) {
      uint32_t cf = sema_gen_clone_fn_fid(sema, 0 /*value*/, env);
      if (!cf) return false;
      if (clone_fid) *clone_fid = cf;
    }
  }
  return true;
}

/* 收尾：把生成的销毁函数追加到程序函数链尾部（sema_analyze 调用）。 */
void sema_attach_destroy_fns(sema_t *sema, ast_node_t *program) {
  if (!sema || !program || program->kind != AST_PROGRAM) return;
  ast_program_t *prog = (ast_program_t *)program;
  for (sema_destroy_fn_t *f = sema->destroy_fns; f; f = f->next) {
    if (!f->fn) continue;
    f->fn->next = NULL;   /* 链表尾：避免把注册表顺序带进函数链 */
    if (prog->funcs_last) prog->funcs_last->next = f->fn;
    else prog->funcs = f->fn;
    prog->funcs_last = f->fn;
  }
}

static ast_node_t *destroy_fn_callee_node(sema_t *sema,
                                          const sema_destroy_fn_t *reg);

/* 销毁函数的 callee 节点（每个调用点一个新 AST_FUNC_REF，节点不能共享）。 */
static ast_node_t *destroy_fn_callee_node(sema_t *sema,
                                          const sema_destroy_fn_t *reg) {
  ast_node_t *callee = ast_func_ref_new(sema->arena, 0, 0);
  if (!callee) return NULL;
  ((ast_func_ref_t *)callee)->fid = ((const ast_func_def_t *)reg->fn)->fid;
  ((ast_func_ref_t *)callee)->name = reg->name;
  return callee;
}

/* tag union：按 member 逐个生成 `if (v is m) { <payload 的销毁序列> }`。
   判 tag 用 AST_IS_TAG（sema 生成的直接形态——`is` 的 op 是 token_t*，
   sema 无法合成，故该节点直接携带 tag 编号）。 */
static bool gen_destroy_union(sema_t *sema, const union_type_t *ut,
                              ast_node_t *access, ast_node_t **head,
                              ast_node_t **tail) {
  for (size_t i = 0; i < ut->member_count; i++) {
    const union_member_t *m = &ut->members[i];
    if (!m->payload_type || !m->name.ptr) continue;
    const type_t *pt = type_strip_quals(m->payload_type);
    bool leaf = pt && (pt->kind == TYPE_KIND_PTR || pt->kind == TYPE_KIND_SLICE);
    if (leaf && m->ownership != OWN_OWN) continue;   /* ref/fatal 槽位不释放 */

    /* then 体：该 member 的销毁序列 */
    ast_node_t *field = ast_member_new(sema->arena, 0, 0);
    if (!field) return false;
    ((ast_member_t *)field)->object = access;
    ((ast_member_t *)field)->field  = m->name;

    ast_node_t *body_head = NULL, *body_tail = NULL;
    if (!gen_destroy(sema, pt, field, &body_head, &body_tail)) return false;
    if (!body_head) continue;   /* 该 member 无 own 槽位 → 不需要条件释放 */

    ast_node_t *blk = ast_block_new(sema->arena, 0, 0);
    if (!blk) return false;
    ((ast_block_t *)blk)->stmts = body_head;
    blk->is_generated = true;   /* Pass 3b 不回走（3a 未为其建作用域） */

    ast_node_t *cond = ast_is_tag_new(sema->arena, 0, 0);
    if (!cond) return false;
    ((ast_is_tag_t *)cond)->object = access;
    ((ast_is_tag_t *)cond)->tag    = m->tag;

    ast_node_t *ifn = ast_if_new(sema->arena, 0, 0);
    if (!ifn) return false;
    ((ast_if_t *)ifn)->cond      = cond;
    ((ast_if_t *)ifn)->then_body = blk;
    ifn->is_generated = true;

    destroy_append(head, tail, ifn);
  }
  return true;
}

/* 销毁一个**值**（`access` 所指、类型 t）：
   - 含 own 槽位的复合类型 → `CALL __destroy_<t>(&access)`（方案 B：函数调用）；
   - 叶子 / 无 own 槽位的类型 → 内联（own 指针/切片 FREE；指针还先调用被指向
     值的销毁函数）。 */
static bool gen_destroy(sema_t *sema, const type_t *t, ast_node_t *access,
                        ast_node_t **head, ast_node_t **tail) {
  if (!t || !access) return true;
  const type_t *s = type_strip_quals(t);
  if (!s) return true;
  if (type_needs_destroy_fn(s))
    return gen_destroy_fn_call(sema, s, access, head, tail, /*by_address=*/true);
  return gen_destroy_impl(sema, t, access, head, tail);
}

/* 结构体：逐字段展开。 */
static bool gen_destroy_struct(sema_t *sema, const struct_type_t *st,
                               ast_node_t *access, ast_node_t **head,
                               ast_node_t **tail) {
  for (size_t i = 0; i < st->field_count; i++) {
    const struct_field_t *f = &st->fields[i];
    if (!f->type || !f->name.ptr) continue;
    const type_t *ft = type_strip_quals(f->type);
    if (ft && (ft->kind == TYPE_KIND_PTR || ft->kind == TYPE_KIND_SLICE)) {
      /* 叶子：只有 own 槽位才释放。**必须递归进被指向值**（PTR/SLICE 分支会
         展开被指向聚合的 own 槽位 / 切片元素），否则 `struct Outer { b: own
         *Box }` 只 FREE 掉 Box 块，Box.p 泄漏（实测 leak）。 */
      if (f->ownership != OWN_OWN) continue;
      ast_node_t *mem = ast_member_new(sema->arena, 0, 0);
      if (!mem) return false;
      ((ast_member_t *)mem)->object = access;
      ((ast_member_t *)mem)->field  = f->name; /* vm 拥有，生命周期长于 AST */
      if (!gen_destroy(sema, ft, mem, head, tail)) return false;
      continue;
    }
    /* 复合字段：递归（无论字段自身 ownership——其 own 槽位归外层值） */
    ast_node_t *mem = ast_member_new(sema->arena, 0, 0);
    if (!mem) return false;
    ((ast_member_t *)mem)->object = access;
    ((ast_member_t *)mem)->field  = f->name;
    if (!gen_destroy(sema, ft, mem, head, tail)) return false;
  }
  return true;
}

/* ---- 逐元素 / 条件销毁的生成辅助 ----
 *
 * tuple / array / slice / option 的槽位不是具名成员，sema 需要现造访问表达式：
 *   - tuple / array / slice 元素：`<access>[i]`（AST_INDEX，索引 = 字面量或
 *     循环变量）；
 *   - option inner：`<access>.!`（AST_UNWRAP，借用引用；先由
 *     AST_OPT_IS_SOME 的 `if (v != nil)` 守卫）；
 *   - 循环：AST_FOR_INDEX（`for i in 0..len(src)`）。计数器名 `__del_i_N`
 *     由 sema 分配（arena NUL 串），compiler 侧是私有 i64 变量。
 * 这些都是**直接形态**节点：`<` / `+` / `!= nil` 的 token 由 lexer 拥有，
 * sema 无法合成 AST_BINARY/AST_UNARY（与 `is` 同一问题，见 AST_IS_TAG）。 */

/* 现造 `<object>[<index>]` 下标访问节点（indices 兄弟链单元素）。 */
static ast_node_t *make_index_access(sema_t *sema, ast_node_t *object,
                                     ast_node_t *index) {
  ast_node_t *ix = ast_index_new(sema->arena, 0, 0);
  if (!ix) return NULL;
  ((ast_index_t *)ix)->object       = object;
  ((ast_index_t *)ix)->indices      = index;
  ((ast_index_t *)ix)->indices_last = index;
  return ix;
}

/* 现造整数字面量节点（i64 后缀：与生成循环的 i64 计数器同宽，避免比较时
   隐式提升出歧义）。 */
static ast_node_t *make_int_lit_i64(sema_t *sema, uint64_t v) {
  ast_node_t *n = ast_int_lit_new(sema->arena, 0, 0);
  if (!n) return NULL;
  ((ast_int_lit_t *)n)->value = v;
  ((ast_int_lit_t *)n)->type  = strslice_from_bytes("i64", 3);
  return n;
}

/* 把任意 C 串复制成 arena 持久化的 NUL 串 → strslice（生成变量名用）。 */
static strslice_t discard_name(sema_t *sema, const char *s) {
  size_t n = strlen(s);
  char *buf = (char *)arena_calloc(sema->arena, n + 1, 1,
                                   ALIGNOF(max_align_t));
  if (!buf) return STRSLICE_EMPTY;
  memcpy(buf, s, n);
  return strslice_from_bytes(buf, n);
}

/* 现造循环计数器变量名 `__del_i_<seq>`（arena NUL 串，块内唯一）。 */
static bool make_loop_var_name(sema_t *sema, char *buf, size_t cap,
                               strslice_t *out) {
  int n = snprintf(buf, cap, "__del_i_%u", sema->destroy_seq++);
  if (n <= 0 || (size_t)n >= cap) return false;
  char *heap = (char *)arena_calloc(sema->arena, (size_t)n + 1, 1,
                                    ALIGNOF(max_align_t));
  if (!heap) return false;
  memcpy(heap, buf, (size_t)n);
  *out = strslice_from_bytes(heap, (size_t)n);
  return true;
}

/* 单个**槽位**的销毁：叶子（ptr/slice）只在 own 槽位释放；复合无条件递归
   （其内部 own 槽位归外层值）。与 struct/tuple/union member 的判定同一规则。 */
static bool gen_destroy_slot(sema_t *sema, const type_t *slot_type,
                             ownership_t slot_own, ast_node_t *access,
                             ast_node_t **head, ast_node_t **tail) {
  if (!slot_type || !access) return true;
  const type_t *s = type_strip_quals(slot_type);
  if (s && (s->kind == TYPE_KIND_PTR || s->kind == TYPE_KIND_SLICE)) {
    if (slot_own != OWN_OWN) return true;   /* ref/fatal 槽位不释放 */
  }
  return gen_destroy(sema, slot_type, access, head, tail);
}

/* 为**元素类型**（array / slice）生成逐元素销毁循环体：
   `for <var> in 0..len(src) { <body> }` 的 body（引用 <var> 访问当前元素）。
   `slot_own` 是元素槽位所有权（[N]own *T 的 own / ref / none）。
   元素无 own 槽位时不生成任何语句（body_head=NULL，调用方跳过整个循环）。 */
static bool gen_destroy_elems(sema_t *sema, const type_t *elem_type,
                              ownership_t slot_own, ast_node_t *access,
                              strslice_t var_name, ast_node_t **body_head,
                              ast_node_t **body_tail) {
  *body_head = *body_tail = NULL;
  if (!elem_type) return true;
  const type_t *es = type_strip_quals(elem_type);
  bool leaf = es && (es->kind == TYPE_KIND_PTR || es->kind == TYPE_KIND_SLICE);
  if (leaf && slot_own != OWN_OWN) return true;   /* ref/fatal 元素不释放 */

  /* 元素访问：<access>[<var>] */
  ast_node_t *idx = ast_ident_new(sema->arena, 0, 0);
  if (!idx) return false;
  ((ast_ident_t *)idx)->name = var_name;
  ast_node_t *elem = make_index_access(sema, access, idx);
  if (!elem) return false;

  ast_node_t *bh = NULL, *bt = NULL;
  if (!gen_destroy_slot(sema, elem_type, slot_own, elem, &bh, &bt))
    return false;
  if (!bh) return true;   /* 元素没有 own 槽位 → 无需循环 */
  *body_head = bh;
  *body_tail = bt;
  return true;
}

static bool gen_destroy_impl(sema_t *sema, const type_t *t, ast_node_t *access,
                             ast_node_t **head, ast_node_t **tail) {
  if (!t || !access) return true;
  t = type_strip_quals(t);
  switch (t->kind) {
    case TYPE_KIND_FUNC:
      /* **闭包 = 捕获聚合结构体**：销毁一个 func 值 = `FREE <access>` ——
         VM 的 FREE 分支按 capture_type 的槽位所有权释放 env 里的 own 捕获，
         再释放 env 块本体（§7.1.10）。这条分支覆盖**赋值点销毁**
         （`f = clone(...)` 覆盖旧闭包）、结构体里的 func 字段、显式销毁等
         一切"按类型生成销毁序列"的位置；作用域退出走的是 make_free_stmts 的
         FUNC 专用分支（同样生成单个 FREE，不会重复）。 */
      destroy_append(head, tail, ast_free_new(sema->arena, access));
      return true;
    case TYPE_KIND_PTR: {
      /* 被指向值的 own 槽位**先**释放（`v.f` 会自动解引用到堆块内容），
         再释放堆块自身。顺序不能反：先 FREE v 会让 `v.f` 无从访问。
         **必须显式解引用**：用户写的 `p.f` 由 sema_expr 就地改写成
         `p.*.f`（AST_MEMBER 的指针自动解引用），但本生成器不过 sema_expr，
         直接写 `p.f` 编译出来就是 `PUSH p; FIELD_GET "f"` —— VM 的
         FIELD_GET / IS_TAG 只接受 struct/union/cunion **值**，拿到指针
         会报 "field get expects a struct..."（实测）。包一层 AST_DEREF
         后与用户写法逐字节一致（编译器发 PTR_GET）。
         这一层对**嵌套指针**也必不可少：`own *own *T` 的被指向值本身是
         一个 own 指针，销毁它 = 释放 `p.*` 指向的堆块（FREE p.*），再
         FREE p；缺了解引用就会把同一个 `p` FREE 两次（实测：双层 FREE、
         内层堆块泄漏）。 */
      const type_t *base = ptr_type_base(t);
      if (base) {
        const type_t *bs = type_strip_quals(base);
        if (bs && (bs->kind == TYPE_KIND_PTR || bs->kind == TYPE_KIND_SLICE)) {
          /* 被指向值本身是叶子（`own *own *T` / `own *[]T`）：地址就是它自身，
             交给 impl 释放（`FREE access.*`；切片还带元素循环）。 */
          ast_node_t *deref = ast_deref_new(sema->arena, 0, 0);
          if (!deref) return false;
          ((ast_deref_t *)deref)->operand = access;
          if (!gen_destroy_impl(sema, bs, deref, head, tail)) return false;
        } else if (type_needs_destroy_fn(bs)) {
          /* 被指向值含 own 槽位：**调用它的销毁函数**（地址 = 指针值本身；
             自引用类型即递归调用自身）。 */
          if (!gen_destroy_fn_call(sema, bs, access, head, tail,
                                   /*by_address=*/false))
            return false;
        }
      }
      destroy_append(head, tail, ast_free_new(sema->arena, access));
      return true;
    }
    case TYPE_KIND_SLICE: {
      /* []T：先逐元素销毁（元素槽位 own 才释放，运行期长度 → for 循环），
         再 FREE 整个切片堆块（顺序不能反：先 FREE 就找不到元素了）。 */
      char name_buf[64];
      strslice_t var_name = STRSLICE_EMPTY;
      if (!make_loop_var_name(sema, name_buf, sizeof name_buf, &var_name))
        return false;
      ast_node_t *bh = NULL, *bt = NULL;
      if (!gen_destroy_elems(sema, slice_type_elem(t),
                             slice_type_elem_ownership(t), access, var_name,
                             &bh, &bt))
        return false;
      if (bh) {
        ast_node_t *blk = ast_block_new(sema->arena, 0, 0);
        if (!blk) return false;
        ((ast_block_t *)blk)->stmts = bh;
        blk->is_generated = true;

        ast_node_t *loop = ast_for_index_new(sema->arena, 0, 0);
        if (!loop) return false;
        ((ast_for_index_t *)loop)->var    = var_name;
        ((ast_for_index_t *)loop)->source = access;
        ((ast_for_index_t *)loop)->body   = blk;
        destroy_append(head, tail, loop);
      }
      destroy_append(head, tail, ast_free_new(sema->arena, access));
      return true;
    }
    case TYPE_KIND_STRUCT:
      return gen_destroy_struct(sema, (const struct_type_t *)t, access, head,
                                tail);
    case TYPE_KIND_UNION:
      return gen_destroy_union(sema, (const union_type_t *)t, access, head,
                               tail);
    case TYPE_KIND_TUPLE: {
      /* <T1,T2,…>：逐元素展开（元素槽位 own 才释放；复合递归）。
         元素访问 = `access[i]`（INDEX_GET；i 是编译期字面量）。 */
      const tuple_type_t *tt = (const tuple_type_t *)t;
      for (size_t i = 0; i < tt->elem_count; i++) {
        const type_t *et = tt->elems[i].type;
        if (!et) continue;
        ast_node_t *idxv = make_int_lit_i64(sema, (uint64_t)i);
        if (!idxv) return false;
        ast_node_t *elem = make_index_access(sema, access, idxv);
        if (!elem) return false;
        if (!gen_destroy_slot(sema, et, tt->elems[i].ownership, elem, head,
                              tail))
          return false;
      }
      return true;
    }
    case TYPE_KIND_ARRAY: {
      /* [N]T：`for i in 0..len(a) { <T 的销毁序列> }`。
         定长数组也走运行期 LENGTH（长度语义统一、无需另发常量）。 */
      const array_type_t *at = (const array_type_t *)t;
      char name_buf[64];
      strslice_t var_name = STRSLICE_EMPTY;
      if (!make_loop_var_name(sema, name_buf, sizeof name_buf, &var_name))
        return false;
      ast_node_t *bh = NULL, *bt = NULL;
      if (!gen_destroy_elems(sema, at->elem_type, at->elem_ownership, access,
                             var_name, &bh, &bt))
        return false;
      if (bh) {
        ast_node_t *blk = ast_block_new(sema->arena, 0, 0);
        if (!blk) return false;
        ((ast_block_t *)blk)->stmts = bh;
        blk->is_generated = true;

        ast_node_t *loop = ast_for_index_new(sema->arena, 0, 0);
        if (!loop) return false;
        ((ast_for_index_t *)loop)->var    = var_name;
        ((ast_for_index_t *)loop)->source = access;
        ((ast_for_index_t *)loop)->body   = blk;
        destroy_append(head, tail, loop);
      }
      return true;
    }
    case TYPE_KIND_OPTION: {
      /* ?T：`if (v != nil) { <T 的销毁序列，作用于 v.! > }`。
         inner 槽位 own 才释放（ref 跳过）；复合 inner 递归。
         判空用 AST_OPT_IS_SOME（直接形态：compiler 发 OPT_IS_NONE + NOT），
         取值用 AST_UNWRAP（编译器发 UNWRAP，借用引用 value 字段）。 */
      const option_type_t *ot = (const option_type_t *)t;
      const type_t *inner = ot->inner;
      if (!inner) return true;
      ownership_t inner_own = ot->inner_ownership;
      const type_t *is = type_strip_quals(inner);
      bool leaf = is && (is->kind == TYPE_KIND_PTR || is->kind == TYPE_KIND_SLICE);
      if (leaf && inner_own != OWN_OWN) return true;

      ast_node_t *unwrap = ast_unwrap_new(sema->arena, 0, 0);
      if (!unwrap) return false;
      ((ast_unwrap_t *)unwrap)->operand = access;

      ast_node_t *bh = NULL, *bt = NULL;
      if (!gen_destroy_slot(sema, inner, inner_own, unwrap, &bh, &bt))
        return false;
      if (!bh) return true;

      ast_node_t *blk = ast_block_new(sema->arena, 0, 0);
      if (!blk) return false;
      ((ast_block_t *)blk)->stmts = bh;
      blk->is_generated = true;

      ast_node_t *cond = ast_opt_is_some_new(sema->arena, 0, 0);
      if (!cond) return false;
      ((ast_opt_is_some_t *)cond)->object    = access;
      ((ast_opt_is_some_t *)cond)->want_some = true;

      ast_node_t *ifn = ast_if_new(sema->arena, 0, 0);
      if (!ifn) return false;
      ((ast_if_t *)ifn)->cond      = cond;
      ((ast_if_t *)ifn)->then_body = blk;
      ifn->is_generated = true;
      destroy_append(head, tail, ifn);
      return true;
    }
    case TYPE_KIND_CUNION:
    default:
      return true;
  }
}

static ast_node_t *make_free_stmts(sema_t *sema, sema_scope_t *scope) {
  if (!scope || !scope->symbols) return NULL;
  const vec_t *keys = strmap_keys(scope->symbols);
  if (!keys) return NULL;
  size_t n = vec_len(keys);
  ast_node_t *head = NULL;
  ast_node_t *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    const char *name = (const char *)vec_get(keys, i);
    if (!name) continue;
    sema_symbol_t *sym = (sema_symbol_t *)strmap_get(scope->symbols, name);
    if (!sym) continue;
    /* ---- 局部函数 / defer 闭包（SEMA_SYM_FUNC 符号）----
       闭包 = 捕获聚合结构体（env 块）：定义点创建的实例持有 env，捕获里的
       own 值必须随**作用域退出**释放（`FREE <name>` → VM 按 capture_type
       聚合释放 env 的 own 捕获 + env 块）。无捕获的局部函数
       （capture_type_id == 0）没有 env → 跳过。
       defer 的语义顺序由调用方保证：块尾先执行 defer calls，再追加 FREE。 */
    if (sym->kind == SEMA_SYM_FUNC) {
      /* comptime 函数体：CTFE 解释执行，不支持 FREE 节点——跳过（编译期
         闭包的捕获不进入运行时生命周期）。 */
      if (sema->fn.walking_comptime) continue;
      const ast_func_def_t *fd = (const ast_func_def_t *)sym->ast;
      if (!fd || fd->base.kind != AST_FUNC_DEF || fd->capture_type_id == 0)
        continue;
      size_t flen = strlen(name);
      char *fbuf = (char *)arena_calloc(sema->arena, flen + 1, 1,
                                        ALIGNOF(max_align_t));
      if (!fbuf) continue;
      memcpy(fbuf, name, flen);
      ast_node_t *fid = ast_ident_new(sema->arena, 0, 0);
      if (!fid) continue;
      ((ast_ident_t *)fid)->name = strslice_from_bytes(fbuf, flen);
      ast_node_t *fs = ast_free_new(sema->arena, fid);
      if (!fs) continue;
      if (!head) head = tail = fs;
      else { tail->next = fs; tail = fs; }
      continue;
    }
    if (sym->kind != SEMA_SYM_VAR) continue;
    if (!sym->type) continue;
    /* 仅 **own** 变量需要显式释放。判据用符号层的 `sym->ownership`
       （所有权已从类型层迁移到值/符号层——类型不携带 ownership，
       所以**不能**用类型层的谓词判断）。
       moved 变量已被转移（TDZ），不需要释放。
       comptime 变量不进入运行时，不需要释放。 */
    if (sym->moved || sym->is_comptime) continue;
    /* 定义点尚未走到（提前 return/break 的路径）→ 该变量在运行时还不存在，
       不能生成销毁（否则 PUSH 一个尚未 DEFINE 的名字 → undefined variable）。 */
    if (!sym->declared) continue;
    if (sym->ownership != OWN_OWN) continue;
    /* 语义是 `FREE <expr>`：由 sema 生成"释放什么"的表达式。
       目前只有简单指针 → 操作数就是变量引用（AST_IDENT，编译为 PUSH <name>）。
       将来复杂类型在此按类型展开（optional→判断 / 数组→循环 / tuple→
       get_index / struct→get_field），生成的仍是 AST_FREE + 对应 expr，
       compiler 不需要知道类型结构。 */
    /* arena 持久化变量名（strslice → NUL 终止字符串；AST_IDENT 引用它） */
    size_t nlen = strlen(name);
    char *name_buf = (char *)arena_calloc(
        sema->arena, nlen + 1, 1, ALIGNOF(max_align_t));
    memcpy(name_buf, name, nlen);
    strslice_t name_slice = strslice_from_bytes(name_buf, nlen);

    ast_node_t *ident = ast_ident_new(sema->arena, 0, 0);
    if (!ident) continue;
    ((ast_ident_t *)ident)->name = name_slice;

    /* ---- 含 own 捕获的闭包：按捕获槽**逐个展开**释放 ----
       闭包 = 捕获结构体 + 函数实现，捕获槽就是它的字段。**捕获槽类型来自
       type 对象**（cv->resolved_type，sema 在捕获解析时写入），交给同一个
       销毁序列生成器展开：
       - `own *T` 捕获 → `FREE f.<cap>`（外加被指向值的 own 槽位）；
       - 含 own 槽位的聚合捕获（struct/union/…）→ 逐槽位 `FREE f.<cap>.x`
         （tag union 还会带 `if (f.<cap> is m)` 条件）——旧实现只发
         `FREE f.<cap>`，对聚合是空操作，捕获块内嵌的堆块全部泄漏
         （实测：`move` 捕获含 own 指针的 struct → 4 字节泄漏）。
       ref 捕获是借用，不释放。 */
    /* ---- 闭包（func 值）：**单个 FREE** ----
       闭包 = 捕获聚合结构体（env 块）。VM 的 FREE 分支按 capture_type 的槽位
       所有权释放 env 里的 own 捕获，再释放 env 块本体 —— 与结构体销毁同构。
       因此 sema 不再需要枚举捕获槽（旧实现按字面量捕获表生成 `FREE f.<cap>`，
       对 `var f = clone(g)` 这种非字面量初始化无法枚举，会漏释放）。 */
    if (type_strip_quals(sym->type)->kind == TYPE_KIND_FUNC) {
      ast_node_t *obj = ast_ident_new(sema->arena, 0, 0);
      if (obj) {
        ((ast_ident_t *)obj)->name = name_slice;
        ast_node_t *fs = ast_free_new(sema->arena, obj);
        if (fs) {
          if (!head) head = tail = fs;
          else { tail->next = fs; tail = fs; }
        }
      }
      continue;
    }

    /* ---- 一般形态：交给销毁序列生成器（按 type 对象递归展开）----
       叶子（ptr/slice）→ `FREE v`；struct → 逐 own 槽位 `FREE v.f`。
       生成器已按 type 对象判定哪些槽位是 own，故 `ref` 槽位不会进序列。 */
    if (!gen_destroy(sema, sym->type, ident, &head, &tail)) continue;
  }
  return head;
}
/* 沿 scope parent 链到 stop_scope（不含）收集各层 own 变量的 FREE 语句链。
   与 collect_defer_calls 同构——FREE 在 defer calls 之后执行（追加到 defers
   链末尾）。返回 FREE 语句链首（按执行顺序：内层 scope 先），NULL = 无。 */
static ast_node_t *collect_free_stmts(sema_t *sema, sema_scope_t *from_scope,
                                      const sema_scope_t *stop_scope) {
  ast_node_t *head = NULL;
  ast_node_t *tail = NULL;
  for (sema_scope_t *s = from_scope; s && s != stop_scope; s = s->parent) {
    ast_node_t *free_stmts = make_free_stmts(sema, s);
    if (!free_stmts) continue;
    if (!head) {
      head = free_stmts;
    } else {
      tail->next = free_stmts;
    }
    ast_node_t *ft = free_stmts;
    while (ft->next) ft = ft->next;
    tail = ft;
  }
  return head;
}

/* 找到函数 scope 的 parent（return 的 FREE 收集范围 stop）。 */
static const sema_scope_t *find_func_stop_sema(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FUNCTION) return s->parent;
  }
  return NULL;
}

/* 找到最近循环 scope 的 parent（break/continue 的 FREE 收集范围 stop）。 */
static const sema_scope_t *find_loop_stop_sema(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FOR) return s->parent;
  }
  return NULL;
}

/* ---- 确定性赋值分析（definite assignment analysis） ---- */

/*
 * 变量初始化状态（flow_init）挂在 sema_symbol_t 上，随 Pass 3b walk 更新：
 *   - var x = expr         → flow_init = true（定义即初始化）
 *   - var x:T = undefined  → flow_init = false（未初始化声明，TDZ）
 *   - x = expr             → flow_init = true（赋值退出未初始化）
 *   - if 合并点            → meet（AND）：两分支都 flow_init 才 true
 *   - while/for            → 循环体可能执行 0 次，体内赋值不提升确定性
 *
 * 分支模拟通过"快照 → 执行 → 恢复"实现：快照收集分支前可见符号的
 * flow_init，分支执行后取 then/else 交集写回（保守策略：非全部分支
 * 赋值 → 仍视为未初始化，读取报编译错误）。
 */

typedef struct flow_snap {
  allocator_t     *alloc;   /* 快照数组分配器（scope 链 alloc，共享 vm alloc） */
  sema_symbol_t  **syms;    /* 分支前可见的变量符号 */
  bool            *before;  /* 分支前 flow_init */
  bool            *after;   /* then 分支后 flow_init（meet 用） */
  size_t           n;
} flow_snap_t;

static flow_snap_t flow_capture(sema_t *sema, sema_scope_t *scope) {
  flow_snap_t snap = {0};
  allocator_t *alloc = scope ? scope->alloc : sema->vm->alloc;
  snap.alloc = alloc;

  /* 计数：沿 scope 链所有符号（含函数符号——函数 flow_init 恒 false，无害） */
  size_t cap = 0;
  for (const sema_scope_t *s = scope; s; s = s->parent)
    cap += strmap_size(s->symbols);
  if (cap == 0) return snap;

  snap.syms = allocator_new_ex(alloc, "flow_snap_syms", sizeof(sema_symbol_t *),
                               NULL, NULL, NULL, cap);
  snap.before = allocator_new_ex(alloc, "flow_snap_before", sizeof(bool), NULL,
                                 NULL, NULL, cap);
  snap.after = allocator_new_ex(alloc, "flow_snap_after", sizeof(bool), NULL,
                                NULL, NULL, cap);

  size_t i = 0;
  for (const sema_scope_t *s = scope; s; s = s->parent) {
    const vec_t *keys = strmap_keys(s->symbols);
    size_t nk = vec_len(keys);
    for (size_t k = 0; k < nk; k++) {
      const char *key = (const char *)vec_get(keys, k);
      sema_symbol_t *sym = (sema_symbol_t *)strmap_get(s->symbols, key);
      snap.syms[i] = sym;
      snap.before[i] = sym->flow_init;
      i++;
    }
  }
  snap.n = cap;
  return snap;
}

static void flow_restore(const flow_snap_t *snap) {
  for (size_t i = 0; i < snap->n; i++)
    snap->syms[i]->flow_init = snap->before[i];
}

static void flow_release(flow_snap_t *snap) {
  if (!snap || !snap->syms) return;
  allocator_free(snap->alloc, (void **)&snap->syms);
  allocator_free(snap->alloc, (void **)&snap->before);
  allocator_free(snap->alloc, (void **)&snap->after);
  snap->n = 0;
}

/* ---- 语句：变量定义 ---- */

/* strslice → NUL 终止临时缓冲（scope_define 内部复制 key，栈缓冲安全） */
static void name_to_cstr(strslice_t s, char *buf, size_t cap) {
  size_t n = s.len < cap - 1 ? s.len : cap - 1;
  memcpy(buf, s.ptr, n);
  buf[n] = '\0';
}

/* ---- R3 借用登记/释放（m3-design §7，2026-10 多源） ---- */

/* 借源集合释放：每个来源 borrow_count--（借用载体作用域退出 / 换绑前调用）。 */
static void borrow_srcs_release(sema_symbol_t *var) {
  if (!var) return;
  for (borrow_src_node_t *n = var->borrow_srcs; n; n = n->next) {
    if (n->sym) n->sym->borrow_count--;
  }
  var->borrow_srcs = NULL;
  var->borrow_src_count = 0;
}

/* 借源集合登记（幂等切换）：先释放旧集合，再按 srcs[0..n) 建立新集合
   （去重；每个来源 borrow_count++；节点 arena 分配）。 */
static void borrow_srcs_set(sema_t *sema, sema_symbol_t *var,
                            sema_symbol_t **srcs, size_t n) {
  if (!var) return;
  borrow_srcs_release(var);
  borrow_src_node_t *tail = NULL;
  for (size_t i = 0; i < n; i++) {
    sema_symbol_t *s = srcs[i];
    if (!s) continue;
    bool dup = false;
    for (borrow_src_node_t *k = var->borrow_srcs; k; k = k->next) {
      if (k->sym == s) { dup = true; break; }
    }
    if (dup) continue;
    borrow_src_node_t *node = (borrow_src_node_t *)arena_calloc(
        sema->arena, 1, sizeof(borrow_src_node_t), ALIGNOF(max_align_t));
    if (!node) break;
    node->sym = s;
    if (tail) tail->next = node;
    else var->borrow_srcs = node;
    tail = node;
    s->borrow_count++;
    var->borrow_src_count++;
  }
}

/* ---- R3 派生规则：**从聚合的 own 槽位借出** → 聚合本身是借源（2026-10） ----
 *
 * 旧登记只认**裸变量标识符**（`var r: ref *T = p`）——那是"复合类型禁止携带
 * 指针/切片"时代的边界（见 shadow_var_def 的注）：当时不存在"从聚合的 own
 * 字段借出"的形态。聚合现在可以携带 own 槽位，派生规则必须补上：
 *
 *   `var r: ref *i32 = b.p`（p 是 **own** 字段）借的是 **b 拥有的堆块**。
 *   这与"有 ref 存活的 own 不可 move"是同一条规则（R3）：move(b) 会把 b 拥有
 *   的堆块交给新 owner，新 owner 可能更早销毁 → 活借用悬垂。所以把 b 登记为
 *   r 的借源（b->borrow_count++），move(b) / `_ = move(b)` 即被拒绝。
 *
 * 精确边界（"**own** 字段有 ref 存活才禁 move"）：
 *   - 借 own 字段 / own 元素 / own inner / own 指针的被指向内容
 *     （`b.p`、`a[i]`、`o.!`、`p.*.f`、`p.f`）→ 登记根变量；
 *   - 借 **ref 字段**（`b.q`，q: ref *T）→ **不登记**（借的是 b 之外的块）；
 *   - `x.&` / `b.&`：借的是该**存储**，存储带 own 槽位（或本身是根拥有的
 *     指针/切片）→ 登记根变量；
 *   - 路径无法静态确定（调用/强制转换/非字面量元组下标）→ 不登记（保守跳过，
 *     与旧行为一致）。
 */
typedef struct {
  ast_kind_t kind;
  ast_node_t *node;
} borrow_hop_t;

/* 把访问路径（叶→根）拆成 hop 数组；根必须是变量标识符。
   返回 hop 数（0 = 根就是 ident）；-1 = 不是可识别的访问路径。 */
static int borrow_path_hops(ast_node_t *e, borrow_hop_t *hops, int cap,
                            ast_node_t **root_ident) {
  int n = 0;
  while (e && e->kind != AST_IDENT) {
    if (n >= cap) return -1;
    switch (e->kind) {
    case AST_MEMBER:
      hops[n].kind = AST_MEMBER;
      hops[n].node = e;
      e = ((ast_member_t *)e)->object;
      break;
    case AST_INDEX:
      hops[n].kind = AST_INDEX;
      hops[n].node = e;
      e = ((ast_index_t *)e)->object;
      break;
    case AST_DEREF:
      hops[n].kind = AST_DEREF;
      hops[n].node = e;
      e = ((ast_deref_t *)e)->operand;
      break;
    case AST_UNWRAP:
      hops[n].kind = AST_UNWRAP;
      hops[n].node = e;
      e = ((ast_unwrap_t *)e)->operand;
      break;
    default:
      return -1;
    }
    n++;
  }
  if (!e || e->kind != AST_IDENT) return -1;
  *root_ident = e;
  return n;
}

/* 路径借用的目标是否归根变量所有（也用于 `x.&` 的"存储归根"判定）。
   返回借源符号；NULL = 不是"从根 own 槽位借出"。 */
static sema_symbol_t *borrow_source_from_owned_path(sema_t *sema,
                                                    ast_node_t *init,
                                                    sema_scope_t *scope) {
  (void)sema;   /* 只用符号表与类型对象 */
  if (!init) return NULL;
  bool is_addr = init->kind == AST_ADDR;
  ast_node_t *path = is_addr ? ((ast_addr_t *)init)->operand : init;
  if (!path) return NULL;
  borrow_hop_t hops[16];
  ast_node_t *root_ident = NULL;
  int n = borrow_path_hops(path, hops, 16, &root_ident);
  if (n < 0) return NULL;
  if (n == 0 && !is_addr) return NULL;   /* 裸变量：init_borrow_src 负责 */
  sema_symbol_t *root =
      sema_lookup(scope, ((ast_ident_t *)root_ident)->name);
  if (!root || root->kind != SEMA_SYM_VAR || !root->type) return NULL;

  const type_t *cur = root->type;
  bool root_owned =
      (root->ownership == OWN_OWN || root->ownership == OWN_FATAL);
  const type_t *ct0 = type_strip_quals(cur);
  bool ptr_owned = root_owned && ct0 &&
                   (ct0->kind == TYPE_KIND_PTR ||
                    ct0->kind == TYPE_KIND_SLICE);
  bool storage = true;   /* 当前值存储是否归根（根的值存储归根） */

  for (int i = n - 1; i >= 0; i--) {
    const type_t *ct = type_strip_quals(cur);
    ast_node_t *h = hops[i].node;
    if (hops[i].kind == AST_DEREF) {
      if (!ct || ct->kind != TYPE_KIND_PTR) return NULL;
      storage = ptr_owned;   /* 解引用得到的内存归根当且仅当指针值归根 */
      cur = ptr_type_base(ct);
      continue;              /* ptr_owned 继承：own *own *T 链式解引用仍归根 */
    }
    if (hops[i].kind == AST_UNWRAP) {
      if (!ct || ct->kind != TYPE_KIND_OPTION) return NULL;
      if (storage) ptr_owned = (type_option_inner_ownership(ct) == OWN_OWN);
      cur = type_option_inner(ct);
      continue;
    }
    if (hops[i].kind == AST_INDEX) {
      ast_index_t *ix = (ast_index_t *)h;
      if (!ix->indices || ix->indices->kind != AST_INT_LIT) return NULL;
      uint64_t k = ((ast_int_lit_t *)ix->indices)->value;
      if (!ct) return NULL;
      if (ct->kind == TYPE_KIND_ARRAY) {
        if (storage) ptr_owned = (array_type_elem_ownership(ct) == OWN_OWN);
        cur = array_type_elem(ct);
        continue;
      }
      if (ct->kind == TYPE_KIND_SLICE) {
        if (storage || ptr_owned)
          ptr_owned = (slice_type_elem_ownership(ct) == OWN_OWN);
        storage = storage || ptr_owned;   /* 元素在缓冲里 */
        cur = slice_type_elem(ct);
        continue;
      }
      if (ct->kind == TYPE_KIND_TUPLE) {
        const tuple_elem_t *te = tuple_type_elem(ct, (size_t)k);
        if (!te) return NULL;
        if (storage) ptr_owned = (te->ownership == OWN_OWN);
        cur = te->type;
        continue;
      }
      return NULL;
    }
    /* MEMBER */
    {
      ast_member_t *m = (ast_member_t *)h;
      if (!ct) return NULL;
      if (ct->kind == TYPE_KIND_PTR) {
        if (!ptr_owned) return NULL;   /* 穿过 ref/借用指针：借的是别人的块 */
        ct = type_strip_quals(ptr_type_base(ct));
        storage = true;
        ptr_owned = false;
      }
      if (!ct) return NULL;
      if (ct->kind == TYPE_KIND_STRUCT) {
        int fi = struct_type_find_field(ct, m->field);
        const struct_field_t *f =
            fi >= 0 ? struct_type_field(ct, (size_t)fi) : NULL;
        if (!f || !f->type) return NULL;
        const type_t *ft = type_strip_quals(f->type);
        if (ft && (ft->kind == TYPE_KIND_PTR || ft->kind == TYPE_KIND_SLICE)) {
          ptr_owned = storage && (f->ownership == OWN_OWN);
          storage = false;
        } else {
          ptr_owned = false;   /* 复合字段：存储仍是根的存储 */
        }
        cur = f->type;
        continue;
      }
      if (ct->kind == TYPE_KIND_UNION) {
        int mi = union_type_find_member(ct, m->field);
        const union_member_t *mm =
            mi >= 0 ? union_type_member(ct, (size_t)mi) : NULL;
        if (!mm || !mm->payload_type) return NULL;
        const type_t *pt = type_strip_quals(mm->payload_type);
        if (pt && (pt->kind == TYPE_KIND_PTR || pt->kind == TYPE_KIND_SLICE)) {
          ptr_owned = storage && (mm->ownership == OWN_OWN);
          storage = false;
        } else {
          ptr_owned = false;
        }
        cur = mm->payload_type;
        continue;
      }
      /* cunion 不参与所有权运算：指针成员不登记（借用/开发者自负） */
      return NULL;
    }
  }

  const type_t *ft = type_strip_quals(cur);
  bool owned = ptr_owned || (storage && ft && type_has_own_slots(ft));
  if (is_addr) {
    if (!storage) return NULL;   /* 借的是根之外的存储 */
    owned = owned || (ft && type_has_own_slots(ft));
  }
  return owned ? root : NULL;
}

/* 借源收集（**保守**）：把一个"借用表达式"的静态可判来源**全部**收集。
 *
 * 编译期不可能精确判定（停机问题）——例如
 *     func fn(a: ref *T, b: ref *T): '<a,b> ref *T {
 *         if (a.* == 1) { return a; } else { return b; }
 *     }
 * 返回值到底来自 a 还是 b 无法静态分析。因此**保守**处理：返回值同时继承
 * 返回标注里**每个参数名**对应实参的来源（本函数按标注名映射到实参并递归）。
 *
 * 支持形态：
 *   - 裸变量（`= p`，own/ref 指针或 opaque；opaque 链式继承 p 的来源）；
 *   - 从聚合 own 槽位借出（`= b.p` / `= a[i]` / `= o.!` / `= p.*.f`）；
 *   - `x.&`（取址借存储）；
 *   - 用户函数调用：返回标注（`'<a,b> ref *T`）→ 形参名 → 实参递归；
 *   - 内建 `data(s)`：借 s 的存储 → 收集 s 的来源。
 * 返回收集数量（去重；上限 cap，超出丢弃已收集项之后的——保守限度）。
 */
static size_t borrow_collect(sema_t *sema, ast_node_t *e, sema_scope_t *scope,
                             sema_symbol_t **out, size_t cap);

static size_t borrow_collect_add(sema_symbol_t **out, size_t n, size_t cap,
                                 sema_symbol_t *s) {
  if (!s || n >= cap) return n;
  for (size_t i = 0; i < n; i++)
    if (out[i] == s) return n;   /* 去重 */
  out[n] = s;
  return n + 1;
}

static size_t borrow_collect(sema_t *sema, ast_node_t *e, sema_scope_t *scope,
                             sema_symbol_t **out, size_t cap) {
  if (!e || cap == 0) return 0;
  size_t n = 0;
  switch (e->kind) {
  case AST_IDENT: {
    sema_symbol_t *s = sema_lookup(scope, ((ast_ident_t *)e)->name);
    if (!s || s->kind != SEMA_SYM_VAR) return 0;
    bool carrier = s->type && (s->ownership == OWN_OWN ||
                               s->ownership == OWN_REF ||
                               s->type->kind == TYPE_KIND_OPAQUE);
    if (!carrier) return 0;
    n = borrow_collect_add(out, n, cap, s);
    /* 链式借用（opaque / ref 变量再借出）：继承其来源 */
    for (borrow_src_node_t *k = s->borrow_srcs; k; k = k->next)
      n = borrow_collect_add(out, n, cap, k->sym);
    return n;
  }
  case AST_MEMBER:
  case AST_INDEX:
  case AST_DEREF:
  case AST_UNWRAP: {
    sema_symbol_t *root = borrow_source_from_owned_path(sema, e, scope);
    if (root) n = borrow_collect_add(out, n, cap, root);
    return n;
  }
  case AST_ADDR: {
    ast_addr_t *a = (ast_addr_t *)e;
    return borrow_collect(sema, a->operand, scope, out, cap);
  }
  case AST_CALL: {
    ast_call_t *call = (ast_call_t *)e;
    sema_symbol_t *csym = NULL;
    if (call->callee && call->callee->kind == AST_IDENT) {
      csym = sema_lookup(scope, ((ast_ident_t *)call->callee)->name);
    } else if (call->callee && call->callee->kind == AST_FUNC_REF) {
      /* sema 常把 callee 折叠成 AST_FUNC_REF（名字保留，供内建判定） */
      const ast_func_ref_t *fr = (const ast_func_ref_t *)call->callee;
      if (fr->name.ptr) csym = sema_lookup(scope, fr->name);
    }
    /* 内建 data(s)：借 s 的存储（§8.4）→ 收集 s 的来源 */
    if (csym && csym->kind == SEMA_SYM_FUNC &&
        csym->builtin_op == SEMA_BUILTIN_DATA) {
      return borrow_collect(sema, call->args, scope, out, cap);
    }
    /* 用户函数：返回标注里的**参数名**即来源声明（可多个）→ 映射到实参 */
    const ast_func_def_t *fn = NULL;
    if (csym && csym->kind == SEMA_SYM_FUNC && csym->ast &&
        csym->ast->kind == AST_FUNC_DEF) {
      fn = (const ast_func_def_t *)csym->ast;
    } else if (call->callee && call->callee->kind == AST_FUNC_REF) {
      sema_func_t *sf =
          sema_func_by_id(sema, ((const ast_func_ref_t *)call->callee)->fid);
      if (sf && sf->def && sf->def->kind == AST_FUNC_DEF)
        fn = (const ast_func_def_t *)sf->def;
    }
    if (!fn || !fn->return_expr) return 0;
    strslice_t *names = NULL;
    size_t ncnt = 0;
    sema_annot_peek(fn->return_expr, &names, &ncnt);
    if (!names || ncnt == 0) return 0;
    for (size_t k = 0; k < ncnt; k++) {
      ast_node_t *arg = call->args;
      for (ast_node_t *p = fn->params; p && arg; p = p->next, arg = arg->next) {
        const ast_var_def_t *pd = (const ast_var_def_t *)p;
        if (!strslice_eq(pd->name, names[k])) continue;
        n += borrow_collect(sema, arg, scope, out + n, cap - n);
        break;
      }
    }
    return n;
  }
  default:
    return 0;
  }
}

/* 变量定义点借用登记：声明类型是 ref *T 或 opaque → 收集初始化表达式的
   **全部**静态可判来源并登记（集合，见 sema_symbol_t::borrow_srcs）。
   兜底：表达式来源不可判（外部调用/未知来源）时，**注解即来源**——
   调用点标注里写的名字（`var r: '<b,c> ref *T = …`）本身就是借源声明。 */
static void shadow_borrow_register(sema_t *sema, sema_symbol_t *sym,
                                   ast_node_t *init, sema_scope_t *scope) {
  if (!sym || !sym->type) return;
  if (sym->ownership != OWN_REF &&
      sym->type->kind != TYPE_KIND_OPAQUE)
    return;
  sema_symbol_t *srcs[16];
  size_t n = borrow_collect(sema, init, scope, srcs, 16);
  if (n == 0 && sym->annot_names) {
    for (size_t i = 0; i < sym->annot_count; i++) {
      sema_symbol_t *a = sema_lookup(scope, sym->annot_names[i]);
      if (a && a->kind == SEMA_SYM_VAR) n = borrow_collect_add(srcs, n, 16, a);
    }
  }
  borrow_srcs_set(sema, sym, srcs, n);
}


/* 作用域退出：遍历本作用域直接符号，释放借源集合（每个来源 borrow_count--）。
   借用载体随作用域消亡，源符号恢复可 move（§7"有 ref 存活禁止 move"的
   存活边界 = 借用变量所在作用域）。 */
void sema_scope_release_borrows(sema_scope_t *scope) {
  if (!scope || !scope->symbols) return;
  const vec_t *keys = strmap_keys(scope->symbols);
  size_t nk = vec_len(keys);
  for (size_t k = 0; k < nk; k++) {
    const char *key = (const char *)vec_get(keys, k);
    sema_symbol_t *sym = (sema_symbol_t *)strmap_get(scope->symbols, key);
    if (sym) borrow_srcs_release(sym);
  }
}


/* ==== 聚合 ref 槽位的借源载体（2026-10）====
 *
 * 构造本质上是"逐字段赋值"的语法糖：
 *     var s2 = <undefined>;  s2.q = b.p;          // 底层逻辑
 * 因此 `var s2: S = .S{ .q = b.p };` 与 `s2.q = b.p;` 都必须把 **b** 登记为
 * s2 的借源——s2 里存着指向 b 拥有堆块的 ref；b 被 move 后该 ref 悬垂。
 * 聚合变量（carrier）因此也拥有借源集合（与 ref 变量同一机制），字段赋值 /
 * 构造 / 拷贝 / new 都往里合并来源；scope 退出统一释放。
 */

/* 类型里是否存在 ref 槽位（借用载体）。cunion 不参与所有权运算 → 无 ref 槽位。 */
static bool type_has_ref_slots_rec(const type_t *t, const type_t **path,
                                size_t depth) {
  if (!t || depth >= 32) return false;
  const type_t *s = type_strip_quals(t);
  for (size_t i = 0; i < depth; i++)
    if (path[i] == s) return false;   /* 自引用类型：环上已判定无 ref 槽位 */
  path[depth] = s;
  depth++;
  switch (s->kind) {
  case TYPE_KIND_STRUCT: {
    const struct_type_t *st = (const struct_type_t *)s;
    for (size_t i = 0; i < st->field_count; i++) {
      const struct_field_t *f = &st->fields[i];
      const type_t *ft = type_strip_quals(f->type);
      if (!ft) continue;
      if ((ft->kind == TYPE_KIND_PTR || ft->kind == TYPE_KIND_SLICE) &&
          f->ownership == OWN_REF)
        return true;
      if (type_has_ref_slots_rec(ft, path, depth)) return true;
    }
    return false;
  }
  case TYPE_KIND_TUPLE: {
    const tuple_type_t *tt = (const tuple_type_t *)s;
    for (size_t i = 0; i < tt->elem_count; i++) {
      const tuple_elem_t *e = &tt->elems[i];
      const type_t *et = type_strip_quals(e->type);
      if (!et) continue;
      if ((et->kind == TYPE_KIND_PTR || et->kind == TYPE_KIND_SLICE) &&
          e->ownership == OWN_REF)
        return true;
      if (type_has_ref_slots_rec(et, path, depth)) return true;
    }
    return false;
  }
  case TYPE_KIND_UNION: {
    const union_type_t *ut = (const union_type_t *)s;
    for (size_t i = 0; i < ut->member_count; i++) {
      const union_member_t *m = &ut->members[i];
      const type_t *pt = type_strip_quals(m->payload_type);
      if (!pt) continue;
      if ((pt->kind == TYPE_KIND_PTR || pt->kind == TYPE_KIND_SLICE) &&
          m->ownership == OWN_REF)
        return true;
      if (type_has_ref_slots_rec(pt, path, depth)) return true;
    }
    return false;
  }
  case TYPE_KIND_OPTION: {
    const option_type_t *ot = (const option_type_t *)s;
    const type_t *in = type_strip_quals(ot->inner);
    if (!in) return false;
    if ((in->kind == TYPE_KIND_PTR || in->kind == TYPE_KIND_SLICE) &&
        ot->inner_ownership == OWN_REF)
      return true;
    return type_has_ref_slots_rec(in, path, depth);
  }
  case TYPE_KIND_ARRAY: {
    const array_type_t *at = (const array_type_t *)s;
    const type_t *et = type_strip_quals(at->elem_type);
    if (!et) return false;
    if ((et->kind == TYPE_KIND_PTR || et->kind == TYPE_KIND_SLICE) &&
        at->elem_ownership == OWN_REF)
      return true;
    return type_has_ref_slots_rec(et, path, depth);
  }
  case TYPE_KIND_SLICE: {
    const type_t *et = type_strip_quals(slice_type_elem(s));
    if (!et) return false;
    if ((et->kind == TYPE_KIND_PTR || et->kind == TYPE_KIND_SLICE) &&
        slice_type_elem_ownership(s) == OWN_REF)
      return true;
    return type_has_ref_slots_rec(et, path, depth);
  }
  case TYPE_KIND_PTR:
    /* 指针本身无“槽位所有权”；其被指向值里的 ref 槽位仍让载体成立
       （销毁该指针会连带销毁 ref 槽位所在的块 → 借源必须存活）。 */
    return type_has_ref_slots_rec(ptr_type_base(s), path, depth);
  default:
    return false;   /* 标量 / cunion / func / opaque */
  }
}

/* 入口（带环检测：自引用类型 `struct Node { next: ?own *Node }` 不能无限递归）。 */
static bool type_has_ref_slots(const type_t *t) {
  const type_t *path[32];
  return type_has_ref_slots_rec(t, path, 0);
}

/* 构造字段链里收集 ref 槽位的借源：type 是构造目标类型，fields 是构造
   字段值链（AST_FILL 只取一次、nil 跳过）。 */
static size_t borrow_collect_fields(sema_t *sema, const type_t *t,
                                    ast_node_t *fields, sema_scope_t *scope,
                                    sema_symbol_t **out, size_t cap, size_t n);

/* 收集"类型 t、槽位所有权 slot_own 的值表达式 e"中所有 ref 槽位的借源。 */
static size_t borrow_collect_value(sema_t *sema, const type_t *t,
                                   ownership_t slot_own, ast_node_t *e,
                                   sema_scope_t *scope, sema_symbol_t **out,
                                   size_t cap, size_t n) {
  if (!t || !e || n >= cap) return n;
  const type_t *s = type_strip_quals(t);
  if (!s) return n;
  /* 叶子槽位：ref 槽位的值表达式本身就是借用 → 收集来源 */
  if (s->kind == TYPE_KIND_PTR || s->kind == TYPE_KIND_SLICE) {
    if (slot_own == OWN_REF) {
      sema_symbol_t *tmp[16];
      size_t m = borrow_collect(sema, e, scope, tmp, 16);
      for (size_t i = 0; i < m; i++)
        n = borrow_collect_add(out, n, cap, tmp[i]);
      return n;
    }
    /* own 指针的 pointee 里若有 ref 槽位：构造是 `new T{…}`（按字段收集） */
    if (s->kind == TYPE_KIND_PTR && e->kind == AST_NEW) {
      const ast_new_t *nv = (const ast_new_t *)e;
      const type_t *base = type_strip_quals(ptr_type_base(s));
      if (base && type_has_ref_slots(base))
        return borrow_collect_fields(sema, base, nv->fields, scope, out, cap, n);
    }
    return n;
  }
  /* 复合：构造表达式 → 逐字段；拷贝（直读变量）→ 继承其借源集合 */
  if (e->kind == AST_CONSTRUCT) {
    return borrow_collect_fields(sema, s, ((const ast_construct_t *)e)->fields,
                                 scope, out, cap, n);
  }
  if (e->kind == AST_IDENT) {
    sema_symbol_t *src = sema_lookup(scope, ((const ast_ident_t *)e)->name);
    if (!src || src->kind != SEMA_SYM_VAR) return n;
    for (borrow_src_node_t *k = src->borrow_srcs; k; k = k->next)
      n = borrow_collect_add(out, n, cap, k->sym);
  }
  return n;
}

/* 构造字段链：按声明序（sema 已把具名构造重排为声明序）逐槽位收集。 */
static size_t borrow_collect_fields(sema_t *sema, const type_t *t,
                                    ast_node_t *fields, sema_scope_t *scope,
                                    sema_symbol_t **out, size_t cap, size_t n) {
  if (!t || !fields) return n;
  const type_t *s = type_strip_quals(t);
  if (!s) return n;
  size_t idx = 0;
  if (s->kind == TYPE_KIND_STRUCT) {
    const struct_type_t *st = (const struct_type_t *)s;
    for (ast_node_t *f = fields; f && idx < st->field_count; f = f->next, idx++) {
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((const ast_construct_field_t *)f)->value;
      if (!value || value->kind == AST_NIL) continue;
      const struct_field_t *fd = &st->fields[idx];
      n = borrow_collect_value(sema, fd->type, fd->ownership, value, scope,
                               out, cap, n);
    }
    return n;
  }
  if (s->kind == TYPE_KIND_TUPLE) {
    const tuple_type_t *tt = (const tuple_type_t *)s;
    for (ast_node_t *f = fields; f && idx < tt->elem_count; f = f->next, idx++) {
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((const ast_construct_field_t *)f)->value;
      if (!value || value->kind == AST_NIL) continue;
      n = borrow_collect_value(sema, tt->elems[idx].type,
                               tt->elems[idx].ownership, value, scope, out, cap,
                               n);
    }
    return n;
  }
  if (s->kind == TYPE_KIND_ARRAY) {
    const array_type_t *at = (const array_type_t *)s;
    for (ast_node_t *f = fields; f; f = f->next) {
      if (f->kind == AST_FILL) {
        n = borrow_collect_value(sema, at->elem_type, at->elem_ownership,
                                 ((const ast_fill_t *)f)->value, scope, out,
                                 cap, n);
        continue;   /* fill 重复同一值，来源相同 */
      }
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((const ast_construct_field_t *)f)->value;
      if (!value || value->kind == AST_NIL) continue;
      n = borrow_collect_value(sema, at->elem_type, at->elem_ownership, value,
                               scope, out, cap, n);
    }
    return n;
  }
  if (s->kind == TYPE_KIND_OPTION) {
    const option_type_t *ot = (const option_type_t *)s;
    ast_node_t *f = fields;
    if (f && f->kind == AST_CONSTRUCT_FIELD)
      f = ((const ast_construct_field_t *)f)->value;
    if (!f || f->kind == AST_NIL) return n;
    return borrow_collect_value(sema, ot->inner, ot->inner_ownership, f, scope,
                                out, cap, n);
  }
  if (s->kind == TYPE_KIND_UNION) {
    const union_type_t *ut = (const union_type_t *)s;
    ast_node_t *f = fields;
    if (f && f->kind == AST_CONSTRUCT_FIELD)
      f = ((const ast_construct_field_t *)f)->value;
    if (!f || f->kind == AST_NIL) return n;
    /* 具名构造：按字段名定位 member；否则用 AST 上的 member_tag（sema 填） */
    return borrow_collect_value(sema, ut->member_count ? ut->members[0].payload_type
                                                       : NULL,
                                ut->member_count ? ut->members[0].ownership
                                                 : OWN_NONE,
                                f, scope, out, cap, n);
  }
  return n;
}

/* 把来源合并进载体（carrier）的借源集合：**不释放旧集合**——字段级重绑定
   无法逐槽位精确回收，保守保留（只会更严格，不会漏拦）。 */
static void borrow_carry_merge(sema_t *sema, sema_symbol_t *carrier,
                               sema_symbol_t **srcs, size_t n) {
  if (!carrier) return;
  for (size_t i = 0; i < n; i++) {
    sema_symbol_t *s = srcs[i];
    if (!s) continue;
    bool dup = false;
    for (borrow_src_node_t *k = carrier->borrow_srcs; k; k = k->next) {
      if (k->sym == s) { dup = true; break; }
    }
    if (dup) continue;
    borrow_src_node_t *node = (borrow_src_node_t *)arena_calloc(
        sema->arena, 1, sizeof(borrow_src_node_t), ALIGNOF(max_align_t));
    if (!node) return;
    node->sym = s;
    node->next = carrier->borrow_srcs;
    carrier->borrow_srcs = node;
    carrier->borrow_src_count++;
    s->borrow_count++;
  }
}

/* 访问路径的根变量符号（`s2.q` → s2；`p.a.b` → p）。 */
static sema_symbol_t *borrow_root_symbol(sema_t *sema, ast_node_t *e,
                                         sema_scope_t *scope) {
  ast_node_t *cur = e;
  while (cur) {
    switch (cur->kind) {
    case AST_IDENT: {
      sema_symbol_t *s = sema_lookup(scope, ((const ast_ident_t *)cur)->name);
      return (s && s->kind == SEMA_SYM_VAR) ? s : NULL;
    }
    case AST_MEMBER: cur = ((const ast_member_t *)cur)->object; break;
    case AST_INDEX: cur = ((const ast_index_t *)cur)->object; break;
    case AST_DEREF: cur = ((const ast_deref_t *)cur)->operand; break;
    case AST_UNWRAP: cur = ((const ast_unwrap_t *)cur)->operand; break;
    default: return NULL;
    }
  }
  return NULL;
}

/* 作用域嵌套比较：a 沿 parent 链可达 b（或 a == b）→ a 嵌套于 b。
   b 为 NULL（锚点解析失败）恒 true（保守放行，错误由解析方报）。 */
bool sema_scope_within(const sema_scope_t *a, const sema_scope_t *b) {
  for (const sema_scope_t *s = a; s; s = s->parent)
    if (s == b) return true;
  return false;
}

/* 标注锚点解析（m3-design §5 检查算法）：标注名字 → 锚点作用域。
   - "*"   → global（global 作用域，恒放行：全局值存活到程序结束）
   - 参数名 → 参数层作用域（参数整个函数体期间存活，恒放行）
   - 局部变量名 → 该符号定义所在作用域（own_scope，3b 定义点记录）
   未解析（非参数/全局/局部变量名）返回 NULL（错误由调用方报）。 */
static sema_scope_t *annot_anchor_scope(sema_t *sema, strslice_t name,
                                        sema_scope_t *scope) {
  if (name.len == 1 && name.ptr[0] == '*') return sema->global_scope;

  /* 参数判定：遍历当前函数真实参数列表（参数符号在 param_scope 直系，但
     函数体顶层变量也注册在 param_scope，sema_scope_find_local 无法区分）。 */
  if (sema->fn.def) {
    for (ast_node_t *p = sema->fn.def->params; p; p = p->next) {
      ast_var_def_t *pvd = (ast_var_def_t *)p;
      if (pvd->name.len == name.len &&
          memcmp(pvd->name.ptr, name.ptr, name.len) == 0)
        return sema->fn.local_func_param_scope;
    }
  }

  /* 局部变量：沿 scope 链解析符号，取其定义作用域（own_scope 3b 记录；
     未走到定义点的符号 own_scope 为 NULL → 锚点解析失败，调用方报错）。 */
  sema_symbol_t *sym = sema_lookup(scope, name);
  if (sym && sym->kind == SEMA_SYM_VAR) return sym->own_scope;
  return NULL;
}

/* §5 逃逸比较（m3-design §5 检查算法：ref 的作用域 ≤ own 的作用域）：
   持有 ref 的变量（own_scope = 变量定义作用域，即 ref 的推断存活）须被
   标注集合的 min 锚点涵盖。**AND 语义**：'<a,b,c> = 存活不超过 min(a,b,c)，
   每个锚点都必须涵盖 ref 定义作用域（任一不涵盖 → 逃逸 → 悬垂）。参数/
   全局锚点恒成立（scope_within 天然处理：函数体内所有块 ⊆ 参数层，所有
   作用域 ⊆ global 根）。 */
static void check_annot_escape(sema_t *sema, ast_node_t *at, sema_scope_t *own_scope,
                               strslice_t *names, size_t count) {
  if (count == 0) return;
  for (size_t i = 0; i < count; i++) {
    sema_scope_t *anchor = annot_anchor_scope(sema, names[i], own_scope);
    if (!anchor) {
      diag_error(sema->diag, sema_loc(sema, at),
                 "scope annotation '%.*s' does not name a parameter, global "
                 "or local variable",
                 (int)names[i].len, names[i].ptr);
      continue;
    }
    if (!sema_scope_within(own_scope, anchor)) {
      diag_error(sema->diag, sema_loc(sema, at),
                 "variable escapes scope annotation '%.*s' (its scope is not "
                 "nested within the annotated scope)",
                 (int)names[i].len, names[i].ptr);
    }
  }
}

/* §5 逃逸检查统一入口（shadow_var_def 调用）：声明类型含 ref 指针（顶层
   ref *T 或含 ref 字段的 struct/tuple/union）时做逃逸比较。
   - 显式标注（顶层 AST_SCOPE_ANNOT）→ 锚点集合 = 标注名字（'<p> ref *T
     借自 p；'<*> = global）
   - 未标注 → 按定义作用域推断（§5"推断：ref 变量未显式标注时推断为
     当前定义作用域"）→ 锚点 = 借源对象：init 直读变量（var r: ref *i32
     = p）取该符号定义作用域；init 非直读（x.&、函数调用返回、new 产物）
     无静态可判借源 → 跳过（ref 存活 = 当前作用域，逃逸点由 §6/调用点
     检查兜底）。
   - §5 强制规则 B：含 ref 字段的聚合类型（struct/tuple/union/cunion 等
     非顶层 ref）变量定义**必须标注**——聚合内 ref 字段借源无法从 init
     静态推断（构造器可能多源），强制用户显式声明。顶层 ref *T 仍走推断
     语义（单源可判）。 */
static void check_var_annot(sema_t *sema, ast_var_def_t *vd,
                            sema_scope_t *scope, const type_t *vt,
                            sema_symbol_t *sym) {
  /* 只有**顶层 ref**（sym->ownership == OWN_REF）需要标注检查与 annot 记录
     ——重绑定逃逸校验依赖 annot_count > 0。
     注：含 ref 字段的聚合类型已不支持（复合类型禁止携带指针/切片），
     所以不再有类型层的 `type_contains_ref` 判据。 */
  if (!vt || !(sym && sym->ownership == OWN_REF))
    return;

  strslice_t *names = NULL;
  size_t count = 0;
  sema_annot_peek(vd->type_expr, &names, &count);
  if (count == 0) {
    /* §5 强制规则 B：含 ref 字段的聚合类型（非顶层 ref）必须标注。
       顶层 ref *T 走推断语义（不强制）。 */
    if (!sym || sym->ownership != OWN_REF) {
      char tn[64];
      sema_type_name(vt, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, vd->type_expr ? vd->type_expr
                                                           : (ast_node_t *)vd),
                 "variable of type '%s' contains ref fields and must declare a "
                 "scope annotation (e.g. '<a> %s)",
                 tn, tn);
      return;
    }
    if (!vd->init) return;
    if (vd->init->kind == AST_IDENT) {
      /* 推断：init 直读变量（var r: ref *i32 = p）→ 锚点 = 借源变量 */
      names = &((ast_ident_t *)vd->init)->name;
      count = 1;
    } else if (vd->init->kind == AST_MEMBER) {
      /* §5 字段访问继承（m3-design §5"var p = s.aaa; 中 p 继承 s 的
         作用域绑定"）：init 是 s.a（object 直读变量 s）→ 继承 s 的
         标注集合（p 借自 s 的字段，p 存活 ≤ s 存活）。s 无标注（推断
         或未含 ref）→ 跳过（s 自身无约束可继承）。 */
      ast_member_t *m = (ast_member_t *)vd->init;
      if (!m->object || m->object->kind != AST_IDENT) return;
      sema_symbol_t *obj_sym =
          sema_lookup(scope, ((ast_ident_t *)m->object)->name);
      if (!obj_sym || obj_sym->annot_count == 0) return;
      names = obj_sym->annot_names;
      count = obj_sym->annot_count;
    } else {
      return;
    }
  }
  /* 记录标注集合到符号（重绑定逃逸校验用）。显式标注 names 指向
     ast_scope_annot_t->names（arena 管理）；推断分支 names 指向
     ast_ident_t->name（arena 管理）——均随 AST 存活，重绑定时可安全读取。 */
  if (sym) {
    sym->annot_names = names;
    sym->annot_count = count;
  }
  check_annot_escape(sema, vd->type_expr ? vd->type_expr : vd->init,
                     scope, names, count);
}

/* 显式类型槽位兜底重解析：3a 槽位解析可能失败（引用同块后续局部 type /
   被 type_shadowed_by_local_def 判"待绑定局部遮蔽"推迟）→ 此处定义点兜底。
   仍失败补报诊断：激活的同名 var/参数遮蔽 → "is a variable, not a type"
   （完全遮罩语义），其余（拼写错误/前向引用 type def）保持 "unknown type"。
   返回 true 表示解析成功（或无需解析）。 */
static bool var_type_slot_reparse(sema_t *sema, ast_var_def_t *vd,
                                  sema_scope_t *scope) {
  sema_symbol_t *sym = sema_scope_find_local(scope, vd->name);
  if (!sym || sym->type || !vd->type_expr) return true;
  sym->type = sema_resolve_type_slot(sema, &vd->type_expr);
  if (sym->type) return true;

  /* 提取槽位名字（仅命名类型可判遮蔽来源；复合类型报 unknown） */
  strslice_t tn = {0};
  if (vd->type_expr->kind == AST_IDENT) {
    tn = ((ast_ident_t *)vd->type_expr)->name;
  } else if (vd->type_expr->kind == AST_TYPE_REF) {
    tn = ((ast_type_ref_t *)vd->type_expr)->name;
  }
  sema_symbol_t *shadow = tn.ptr ? sema_lookup(scope, tn) : NULL;
  if (shadow && shadow->kind == SEMA_SYM_VAR) {
    /* 激活的 var/参数遮蔽（kind==SYM_VAR 区分于 type/函数符号）：
       完全遮罩语义，类型槽位引用的是变量 → 报错 */
    diag_error(sema->diag, sema_loc(sema, vd->type_expr),
               "'%.*s' is a variable, not a type", (int)tn.len, tn.ptr);
  } else {
    diag_error(sema->diag, sema_loc(sema, vd->type_expr), "unknown type");
  }
  return false;
}

/* 从 fatal 类型剥离 OWN_FATAL：
 * - 复合类型（struct/tuple/array/option/union）→ OWN_NONE（fatal T → T）
 * - 指针/切片 → OWN_OWN（fatal *T → own *T，fatal []T → own []T）
 *   fatal 是将亡值，只能赋值给 own（move/clone/new 产物），所以自动推断
 *   为 own 而非要求显式标注。
 * 返回 NULL = 非 fatal 类型或无法处理。 */
static const type_t *strip_fatal(sema_t *sema, const type_t *t) {
  /* 所有权迁移到值/symbol 层后，fatal 不再是类型层概念——type_is_fatal(t)
     恒为 false，fatal *T 的类型即 *T（类型登记不携带 ownership）。本函数
     退化为恒等返回 t（非 NULL 输入），与 expr.c 的 expr_strip_fatal 同源。 */
  (void)sema;
  return t;
}

/* 该表达式是否"含 own 捕获的闭包"（穿透 move/clone）。
   所有权模型：闭包 = 捕获结构体 + 函数实现，捕获了 own 的闭包**本身即 OWN**
   （唯一的 own 产生式——函数定义把 fatal→move→own 折叠了）。
   因此 `var f = <closure>` 被拒（OWN 不能给 NONE 位置），必须
   `var f = move(<closure>)`：move 产 FATAL → 变量推断为 own 绑定。
   捕获槽的所有权由 resolve_func_captures 回写进 `ast_var_def_t::ownership`。 */
static const ast_func_def_t *own_closure_of(ast_node_t *e) {
  if (!e) return NULL;
  if (e->kind == AST_MOVE) return own_closure_of(((ast_move_t *)e)->operand);
  if (e->kind != AST_FUNC_DEF) return NULL;
  ast_func_def_t *fd = (ast_func_def_t *)e;
  for (ast_node_t *c = fd->captures; c; c = c->next)
    if (((ast_var_def_t *)c)->ownership == AST_OWN_OWN) return fd;
  return NULL;
}

static void shadow_var_def(sema_t *sema, ast_var_def_t *vd,
                           sema_scope_t *scope) {
  sema_symbol_t *sym = sema_scope_find_local(scope, vd->name);
  if (!sym) return; /* 3a 重复定义已诊断，符号未注册 */
  /* Pass 3b 已走到定义点：此后收集销毁/借源时才包含该变量（提前 return 的
     路径不会销毁后面才声明的变量）。 */
  sym->declared = true;

  /* comptime var：编译期求值 + 符号表编码（sema_eval_comptime_var）。
     定义点不进入 VM scope——调用方（walk_block）负责从语句链摘除；
     引用点在 sema_expr 折叠为字面量。comptime 值是编译期常量（无运行时
     生命周期），不参与 §5 逃逸检查。
     已求值的跳过：walk_block 的提升预扫会先按源序求值同块 comptime var
     （该块后面的 type def 如 `type T = [C]i32` 在提升阶段就要用到常量值），
     此处不能重复求值（会二次绑定同名常量）。 */
  if (vd->is_comptime) {
    if (!sym->ct_valid) sema_eval_comptime_var(sema, vd, scope);
    return;
  }

  /* §5 锚点解析基础：记录定义所在作用域（own_scope）。作用域标注
     '<p> ref *T 中的 p 解析为符号后取其 own_scope 作锚点作用域。 */
  sym->own_scope = scope;

  value_t *var_value;
  if (vd->init && vd->init->kind == AST_UNDEF) {
    /* 未初始化声明：var x:T = undefined。要求显式类型（undefined 无类型
       可推断）。flow_init=false（确定性赋值分析 UNKNOWN，TDZ），读取
       编译错误，只可赋值退出。 */
    if (!vd->type_expr) {
      diag_error(sema->diag, sema_loc(sema, vd->init),
                 "cannot infer type of uninitialized variable '%.*s'; "
                 "add an explicit type annotation",
                 (int)vd->name.len, vd->name.ptr);
    } else {
      var_type_slot_reparse(sema, vd, scope); /* 3a 推迟的槽位定义点兜底 */
    }
    sym->flow_init = false;
    var_value =
        value_make_shadow(sema->vm, sym->type ? sym->type : sema->vm->type_void);
    value_set_ownership(var_value, sym->ownership);
  } else if (vd->init && vd->init->kind == AST_NIL) {
    /* ?T 声明初始化 none（docs m2-design §12.1）：var x:?T = nil。
       nil 非 value（无类型可推断），要求显式 ?T 类型标注；声明类型须
       optional。编译为 STORE_NIL（compiler 发码），运行期置 ok=false。
       确定性 = 已初始化（nil 是确定的值）。 */
    if (!vd->type_expr) {
      diag_error(sema->diag, sema_loc(sema, vd->init),
                 "cannot infer type of optional variable '%.*s' from nil; "
                 "add an explicit type annotation",
                 (int)vd->name.len, vd->name.ptr);
    } else {
      var_type_slot_reparse(sema, vd, scope);
      if (sym->type && sym->type->kind != TYPE_KIND_OPTION) {
        char tn[64];
        sema_type_name(sym->type, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "cannot initialize variable '%.*s' of type %s with nil "
                   "(only optional types accept nil)",
                   (int)vd->name.len, vd->name.ptr, tn);
      }
    }
    sym->flow_init = true; /* nil 是确定值（已初始化） */
    var_value =
        value_make_shadow(sema->vm, sym->type ? sym->type : sema->vm->type_void);
    value_set_ownership(var_value, sym->ownership);
  } else {
    /* 已初始化：先求值 init（定义尚未入 VM scope → 自引用解析到外层同名变量）。
       匿名构造注入：显式声明类型已知 → push anon_ct，使 init 的 .{...}
       推断目标类型（var p: Point = .{ .x = 1 };）。 */
    const type_t *dt = NULL;
    if (vd->type_expr && sema->ex.anon_ct_depth < 16) {
      /* 类型槽位可能未解析（3a 推迟），先尝试解析出声明类型用于注入 */
      ast_node_t *save = vd->type_expr;
      const type_t *rt = resolve_type_expr(sema, vd->type_expr);
      if (rt) dt = rt;
      vd->type_expr = save; /* 不在此处折叠——3a/var_type_slot_reparse 负责 */
    }
    bool ct_pushed = sema_anon_push(sema, dt);
    value_t *init = sema_expr(sema, &vd->init, scope);
    sema_anon_pop(sema, ct_pushed);
    bool init_bad = value_is_error(sema->vm, init) ||
                    value_is_type(init, TYPE_KIND_VOID);
    /* 错误恢复产物（init 已诊断）不提升确定性 */
    sym->flow_init = !init_bad;

    if (vd->type_expr) {
      /* 显式类型：3a 槽位解析可能失败（引用同块后续局部 type/被 var 遮蔽，
         当时未绑定 vm scope）→ 此处兜底重解析（局部 type 已在定义点求值
         绑定）；仍失败补报诊断（见 var_type_slot_reparse）。 */
      var_type_slot_reparse(sema, vd, scope);
      /* R1（fatal 禁止命名，§3.3）：fatal *T 只能作为 move/clone 表达式的
         临时值流，不能存进变量（fatal = 将亡值，必须直接接管）。函数参数
         是唯一例外（§4，sema 参数路径不经过此分支）。 */
      if (sym->type && sema_sym_is_fatal(sym)) {
        diag_error(sema->diag, sema_loc(sema, vd->type_expr),
                   "cannot declare variable '%.*s' of fatal pointer type "
                   "(fatal is a temporary; receive it as own or pass to a "
                   "function)",
                   (int)vd->name.len, vd->name.ptr);
        sym->flow_init = false;
      }
      /* value_assign 校验 init 可赋给声明类型（单一校验点） */
      if (!init_bad && sym->type) {
        /* R1（own 不可 copy，§3.1）：own *T 声明只能由 fatal 将亡值
           （move/clone/new/make 产物或函数返回值）初始化——直接赋 own/ref
           变量 = copy → 编译错误。 */
        if (sym->ownership == OWN_OWN && !sema_value_is_fatal(init)) {
          char itn[64];
          sema_type_name(value_type(init), itn, sizeof(itn));
          /* 措辞不用"pointer"：所有权已与类型解耦，`own` 是**位置对值的
             状态要求**（需要将亡值/FATAL），任何类型都可能出现在这里。
             `own i32` 正是经由这条规则被淘汰——i32 没有 FATAL 产生式。 */
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot initialize own value '%.*s' with %s: an 'own' "
                     "binding must take over a dying value (FATAL), produced by "
                     "new / move / clone (or a fatal-returning call)",
                     (int)vd->name.len, vd->name.ptr, itn);
          sym->flow_init = false; /* copy 失败 = 未初始化 */
        } else if (sym->ownership != OWN_OWN &&
                   sema_value_is_fatal(init)) {
          /* **反向规则**（§0.4）：FATAL 必须被 own 位置接住。非 own 位置
             接收将亡值 = 值消失而内部 own 槽位无人释放 → 静默泄漏；更糟
             时源与目标同名不同状态（双释放）。聚合（含 own 槽位的
             struct/union/tuple）的 CONSTRUCT 就产 FATAL，因此
             `var b: Box = .Box{ .p = new i32{1} }` 必须写成
             `var b: own Box = …`（或 move() 进已有 own 变量）。 */
          char tn[64], itn[64];
          sema_type_name(sym->type, tn, sizeof(tn));
          sema_type_name(value_type(init), itn, sizeof(itn));
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot initialize '%.*s' of type %s with the dying value "
                     "(FATAL) of type %s: a value holding owned resources must "
                     "be caught by an 'own' position (declare it `own %s`, or "
                     "use move()/clone() and store the result there)",
                     (int)vd->name.len, vd->name.ptr, tn, itn, tn);
          sym->flow_init = false;
        } else if (sym->ownership == OWN_NONE &&
                   value_ownership(init) == OWN_OWN &&
                   type_has_own_slots(sym->type)) {
          /* 非 own 位置的 copy：读 own **聚合**变量得到 OWN 值，复制会让
             两个位置共享同一批内嵌堆块 → 双释放。own 是 move-only。
             判据限定 `type_has_own_slots`：只有"内部挂 own 槽位"的聚合
             才有这个问题；指针 → opaque 的擦除（`var q: opaque = p`）是
             借用式视图，不转移所有权，仍按原语义放行。 */
          char tn[64];
          sema_type_name(sym->type, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot initialize '%.*s' of type %s by copying an own "
                     "value: own is move-only (use move() or clone(), or "
                     "declare the target `own %s`)",
                     (int)vd->name.len, vd->name.ptr, tn, tn);
          sym->flow_init = false;
        } else {
          value_t *dst = value_make_shadow(sema->vm, sym->type);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, init))) {
            char tn[64], itn[64];
            sema_type_name(sym->type, tn, sizeof(tn));
            sema_type_name(value_type(init), itn, sizeof(itn));
            diag_error(sema->diag, sema_loc(sema, vd->init),
                       "cannot initialize variable '%.*s' of type %s with %s",
                       (int)vd->name.len, vd->name.ptr, tn, itn);
          }
        }
      }
      var_value =
          value_make_shadow(sema->vm, sym->type ? sym->type : sema->vm->type_void);
      value_set_ownership(var_value, sym->ownership);
    } else {
      /* 推断类型：init 类型即变量类型。
         R1（own 不可 copy，§3.1）：var p = q; 且 init 是 own *T 且非
         fatal 接管 → copy → 编译错误（own 唯一所有权只能 move/clone）。 */
      const type_t *vt = init_bad ? sema->vm->type_void : value_type(init);
      /* R1（fatal 禁止命名，§3.3）：fatal 是临时值，不能直接命名。
         - fatal 复合类型（fatal Test）：自动剥离为 T（OWN_NONE）——复合类型
           的 own 字段所有权由字段自身的 ownership 编码，fatal 仅标记将亡值，
           变量接管后类型为 T。
         - fatal 指针/切片（fatal *T / fatal []T）：自动升级为 own *T / own []T
           ——fatal 是将亡值，只能赋值给 own（move/clone/new 产物），自动推断
           为 own 而非要求显式标注。 */
      if (vt && sema_value_is_fatal(init)) {
        const type_t *stripped = strip_fatal(sema, vt);
        if (stripped) {
          vt = stripped;
          /* fatal 指针/切片剥离为 own（变量接管将亡值 → own 所有权）；
             **fatal 复合类型同理**：变量接管后就是内部 own 槽位的所有者，
             否则作用域退出时没有任何 free 序列 → 静默泄漏（实测
             `var u = .U{ .p = new i32{1} }` 的 4 字节堆块从未释放）。
             类型仍是 T（所有权不进类型），只是符号层记 OWN_OWN，
             让 make_free_stmts 按 type 对象展开销毁序列。 */
          if (vt->kind == TYPE_KIND_PTR || vt->kind == TYPE_KIND_SLICE)
            sym->ownership = OWN_OWN;
          /* 含 own 捕获的闭包：move 产 FATAL → 变量接管为 own 绑定，
             并记住捕获链供 FREE 展开（逐捕获槽释放）。 */
          else if (vt->kind == TYPE_KIND_FUNC) {
            /* 闭包 = 捕获聚合结构体：**含 own 捕获**的闭包是 own（move-only），
               销毁 = `FREE f`（VM 按 capture_type 释放 env 里的 own 捕获）。 */
            const ast_func_def_t *oc = own_closure_of(vd->init);
            if (oc) {
              sym->ownership = OWN_OWN;
            } else if (sema_value_is_fatal(init) ||
                       value_ownership(init) == OWN_OWN) {
              /* 非字面量初始化（clone(g) / move(g)）：结果依赖运行期 capture_type
                 → 保守标 own，FREE 时由 VM 按槽位所有权释放。 */
              sym->ownership = OWN_OWN;
            }
          } else if (type_has_own_slots(vt)) {
            /* 聚合（含 own 槽位的 struct/tuple/union/array）接管：
               变量成为内部 own 槽位的所有者（销毁序列由 type 对象展开）。 */
            sym->ownership = OWN_OWN;
          }
          /* 将推断出的类型写回 type_expr（AST_TYPE_REF），让 compiler 发
             LOAD_TYPE <id> 而非 PUSH_UNDEFINED。运行时 op_define 走显式
             类型路径：value_implicit_cast(fatal init → own/none 接管），
             避免变量持有 fatal 类型在 scope 退出时不释放 → 内存泄漏。
             函数参数不走此路径（参数有显式 fatal 类型标注，合法保留）。 */
          const sema_type_t *st = sema_type_find(sema, vt);
          if (st && st->name.ptr) {
            ast_node_t *ref = ast_type_ref_new(sema->arena,
                                               vd->init->tok_begin,
                                               vd->init->tok_end);
            if (ref) {
              ((ast_type_ref_t *)ref)->name = st->name;
              vd->type_expr = ref;
            }
          }
        } else {
          /* strip_fatal 返回 NULL 意味着遇到不支持的 fatal 类型（理论上
             不会发生——所有 fatal 变体都覆盖了），保守报错。 */
          diag_error(sema->diag, sema_loc(sema, vd->init),
                     "cannot infer fatal type for variable '%.*s'",
                     (int)vd->name.len, vd->name.ptr);
          sym->flow_init = false;
          vt = sema->vm->type_void;
        }
      } else if (vt && value_ownership(init) == OWN_OWN &&
                 !sema_value_is_fatal(init)) {
        char itn[64];
        sema_type_name(vt, itn, sizeof(itn));
        diag_error(sema->diag, sema_loc(sema, vd->init),
                   "cannot initialize variable '%.*s' with %s "
                   "(own is move-only; use move() or clone())",
                   (int)vd->name.len, vd->name.ptr, itn);
        sym->flow_init = false; /* copy 失败 = 未初始化 */
        vt = sema->vm->type_void;
      }
      sym->type = vt;
      var_value = value_make_shadow(sema->vm, vt);
      value_set_ownership(var_value, sym->ownership);
    }
  }

  /* §5 逃逸检查（m3-design §5）：声明类型含 ref 指针（顶层 ref *T 或含
     ref 字段的 struct/tuple/union）时定义必须带作用域标注；带标注则做
     逃逸比较（ref 存活 ≤ 锚点存活）。类型未就绪（解析失败已诊断）跳过
     ——防级联。 */
  if (sym->type) check_var_annot(sema, vd, scope, sym->type, sym);

  /* 注：原先这里还有"含 ref 字段的聚合变量记录 init 借源集合"的一步
     （`borrow_collect`），供赋值时校验"新值存活不超出承诺上限"。
     复合类型禁止携带指针/切片时代该形态不存在，整条借源集合机制
     （`borrow_set` / `borrow_collect` / …）随之删除。
     聚合恢复携带 own 槽位后（2026-10），**从聚合 own 槽位借出**的借源由
     `shadow_borrow_register` 里的 `borrow_source_from_owned_path` 补上：
     沿访问路径（MEMBER/INDEX/DEREF/UNWRAP/ADDR）判定借出的目标是否归根变量
     所有（own 字段/元素/inner/payload、own 指针的被指向内容、含 own 槽位的
     存储），是则把**根变量**登记为借源（单源 `borrow_src`）——
     move(根变量) 因此被 R3 拒绝。ref 字段/标量字段/借用指针穿透不登记。 */

  /* R3 借用登记（m3-design §7）：ref *T / opaque 变量绑定借源（显式类型
     分支与推断分支统一——推断出 ref 同样登记；opaque 恒显式）。 */
  shadow_borrow_register(sema, sym, vd->init, scope);

  /* 聚合 ref 槽位的**借源载体**：构造 = 逐字段赋值的语法糖
     （var s = .S{ .q = b.p } ≡ s.q = b.p），因此把构造/拷贝里 ref 值的来源
     合并到该变量；scope 退出时统一释放（sema_scope_release_borrows）。 */
  if (sym && sym->type && vd->init &&
      !(sym->ownership == OWN_REF ||
        sym->type->kind == TYPE_KIND_OPAQUE) &&
      type_has_ref_slots(sym->type)) {
    sema_symbol_t *csrcs[16];
    size_t cn = borrow_collect_value(sema, sym->type, OWN_NONE, vd->init,
                                     scope, csrcs, 16, 0);
    if (cn) borrow_carry_merge(sema, sym, csrcs, cn);
  }

  /* 定义 shadow value 到当前 VM scope（与 sema scope 树同构；名字取自 ast）。
     定义完成 → 符号激活（sema_lookup 跳过未激活符号，自引用解析到外层） */
  char nb[256];
  name_to_cstr(vd->name, nb, sizeof nb);
  scope_define(sema->vm, sema->vm->current_scope, nb, var_value);
  sym->is_active = true;
}

/* ---- 语句：赋值 ---- */

/* 复合赋值 token（+= 等）→ 基础二元运算 */
static value_t *(*compound_binop(const token_t *op))(vm_t *, value_t *,
                                                     value_t *) {
  if (token_is(op, "+=")) return value_add;
  if (token_is(op, "-=")) return value_sub;
  if (token_is(op, "*=")) return value_mul;
  if (token_is(op, "/=")) return value_div;
  if (token_is(op, "%=")) return value_mod;
  return NULL;
}

static void shadow_assign_index(sema_t *sema, ast_assign_t *as,
                                sema_scope_t *scope);
static void shadow_assign_member(sema_t *sema, ast_assign_t *as,
                                 sema_scope_t *scope);
static void shadow_assign_deref(sema_t *sema, ast_assign_t *as,
                                sema_scope_t *scope);
static void shadow_assign_extern_var(sema_t *sema, ast_assign_t *as,
                                     sema_scope_t *scope);

static void shadow_assign(sema_t *sema, ast_assign_t *as,
                          sema_scope_t *scope) {
  /* 左值：下标表达式 a[i] = v（sema 校验下标合法） */
  if (as->target->kind == AST_INDEX) {
    shadow_assign_index(sema, as, scope);
    return;
  }
  /* 左值：字段赋值 p.field = v（读+写+复合赋值，嵌套 p.a.b 递归） */
  if (as->target->kind == AST_MEMBER) {
    shadow_assign_member(sema, as, scope);
    return;
  }
  /* 左值：解引用赋值 r.* = v（m3-design §8.2 解引用 SET 严格分离） */
  if (as->target->kind == AST_DEREF) {
    shadow_assign_deref(sema, as, scope);
    return;
  }
  /* 左值：`<库>::<数据符号>` = v（M6 FFI，extern var 的写入）。
     与读取同源：C ABI 层面就是"往 dlsym 得到的变量地址写 size 字节"。
     复合赋值（`crt::x += 1`）走 LOAD → op → STORE，与普通变量同构。 */
  if (as->target->kind == AST_SCOPE_REF) {
    /* 结构体静态属性：`T::x = v`。在类型的 member_scope 中查成员。
       comptime var 成员是 const（不可赋值）；func 成员不可赋值。
       type_expr 可能是 AST_IDENT（首次访问）或 AST_TYPE_REF（被 ex_scope_ref
       折叠过）——两种情况都取类型名查全局符号。 */
    ast_scope_ref_t *er = (ast_scope_ref_t *)as->target;
    strslice_t type_name = STRSLICE_EMPTY;
    if (er->type_expr && er->type_expr->kind == AST_IDENT)
      type_name = ((ast_ident_t *)er->type_expr)->name;
    else if (er->type_expr && er->type_expr->kind == AST_TYPE_REF)
      type_name = ((ast_type_ref_t *)er->type_expr)->name;
    if (type_name.ptr) {
      sema_symbol_t *ls = sema_lookup(scope, type_name);
      if (ls && ls->kind == SEMA_SYM_TYPE) {
        /* 在 member_scope 中查成员名（自己优先 + 嵌入链递归） */
        const struct_type_t *owner = NULL;
        sema_symbol_t *ms = sema_struct_find_member(sema, ls->type,
                                                    er->variant, as->target,
                                                    &owner);
        /* 名字命中的是**函数成员**：`T::f = v` 非法 */
        if (ms && ms->kind == SEMA_SYM_FUNC) {
          diag_error(sema->diag, sema_loc(sema, as->target),
                     "cannot assign to '%.*s::%.*s': it is a `func` member "
                     "(a method, not a variable)",
                     (int)type_name.len, type_name.ptr,
                     (int)er->variant.len, er->variant.ptr);
          sema_expr(sema, &as->value, scope);
          return;
        }
        /* comptime var 成员是 const：不可赋值 */
        if (ms && ms->kind == SEMA_SYM_VAR && ms->is_comptime) {
          diag_error(sema->diag, sema_loc(sema, as->target),
                     "cannot assign to '%.*s::%.*s': it is a `comptime var` "
                     "(const, cannot be reassigned)",
                     (int)type_name.len, type_name.ptr,
                     (int)er->variant.len, er->variant.ptr);
          sema_expr(sema, &as->value, scope);
          return;
        }
        /* 普通静态属性：类型检查右值后放行（compiler 发 STORE_ATTR）。
           置 is_struct_attr + struct_attr_type_id 供 compiler 识别。 */
        if (ms && ms->kind == SEMA_SYM_VAR) {
          /* struct_attr_type_id 用**实际拥有属性的 struct 类型**（owner），
             覆盖语义下写自身、共享语义下写继承源。 */
          if (owner) {
            const sema_type_t *reg = sema_type_find(sema, &owner->base);
            if (reg) {
              er->is_struct_attr = true;
              er->struct_attr_type_id = reg->id;
            }
          }
          /* 求值右值做类型检查 */
          value_t *rv = sema_expr(sema, &as->value, scope);
          if (rv && !value_is_error(sema->vm, rv) && ms->type) {
            /* 隐式转换检查 */
            value_t *casted = value_implicit_cast(sema->vm, rv, ms->type);
            if (value_is_error(sema->vm, casted)) {
              char exp[256], got[256];
              sema_type_name(ms->type, exp, sizeof exp);
              sema_type_name(value_type(rv), got, sizeof got);
              diag_error(sema->diag, sema_loc(sema, as->value),
                         "type mismatch in assignment: expected '%s', got '%s'",
                         exp, got);
            }
          }
          return;
        }
      }
    }
  }
  if (as->target->kind == AST_SCOPE_REF) {
    shadow_assign_extern_var(sema, as, scope);
    return;
  }
  /* 左值必须是标识符表达式（目前仅支持 ID_LIT） */
  if (as->target->kind != AST_IDENT) {
    diag_error(sema->diag, sema_loc(sema, as->target),
               "invalid assignment target");
    sema_expr(sema, &as->value, scope);
    return;
  }
  strslice_t name = ((ast_ident_t *)as->target)->name;

  /* ===== 显式销毁 / 丢弃：`_ = expr`（m3-design §3.3）=====
     sema 在**当前位置**生成销毁序列（与作用域退出同一套扁平槽位展开），把
     "提前释放"编成显式 FREE 序列——与 `move` 一起构成"显式销毁"：
         `_ = move(x)`        —— 销毁整个变量 x（x 标 moved，退出不再释放）
         `_ = new T{...}`     —— 丢弃将亡值（临时变量 + 销毁）
         `_ = clone(x)`       —— 同上（源不受影响）
         `_ = <借用/无资源>`  —— 只求值 + POP（**不得 DISPOSE**：借用不拥有）
     `as->discard_mode` 告诉 compiler 走哪条路径（见 ast_assign.h）。
     拒绝：`_ = x`（own/fatal 变量**不 move**）——own 不可 copy；
           `move(ref)` —— 借用不拥有堆块（ex_move 约束 0）。 */
  if (strslice_eq(name, STRSLICE_LIT("_"))) {
    if (!as->op || !token_is(as->op, "=")) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "discard '_' only supports simple assignment '='");
      sema_expr(sema, &as->value, scope);
      return;
    }
    /* move(x) 形态先记下（sema_expr 之后节点仍是 AST_MOVE） */
    bool is_move = as->value && as->value->kind == AST_MOVE &&
                   ((ast_move_t *)as->value)->op &&
                   token_is(((ast_move_t *)as->value)->op, "move");
    ast_node_t *mv_ident = NULL;
    if (is_move) {
      ast_move_t *mv = (ast_move_t *)as->value;
      if (mv->operand && mv->operand->kind == AST_IDENT) mv_ident = mv->operand;
    }

    value_t *rhs = sema_expr(sema, &as->value, scope);
    if (!rhs || value_is_error(sema->vm, rhs) ||
        value_is_type(rhs, TYPE_KIND_VOID))
      return;
    const type_t *rt = value_type(rhs);

    /* 目标变量符号（ident 或 move(ident)） */
    ast_node_t *vsym_node = mv_ident;
    if (!vsym_node && !is_move && as->value &&
        as->value->kind == AST_IDENT)
      vsym_node = as->value;
    sema_symbol_t *vsym = vsym_node
        ? sema_lookup(scope, ((ast_ident_t *)vsym_node)->name)
        : NULL;

    /* `_ = x`（own/fatal 变量不 move）→ 拒绝：这不是"销毁"，是 own 拷贝。 */
    if (vsym && vsym->kind == SEMA_SYM_VAR && !is_move &&
        (vsym->ownership == OWN_OWN || sema_sym_is_fatal(vsym))) {
      strslice_t vn = ((ast_ident_t *)vsym_node)->name;
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot discard '%.*s' by copy: an own value must be "
                 "transferred — use `_ = move(%.*s)` for explicit destruction "
                 "(or clone() to copy)",
                 (int)vn.len, vn.ptr, (int)vn.len, vn.ptr);
      as->discard_mode = 1;   /* 编译已失败，防后续再生成 */
      return;
    }

    /* 该值是否有**自有资源**需要释放：来源是 own/fatal（而不是借用）且类型
       含资源（指针/切片/含 own 槽位的聚合）。 */
    bool own_value = false;
    if (vsym && vsym->kind == SEMA_SYM_VAR)
      own_value = (vsym->ownership == OWN_OWN || sema_sym_is_fatal(vsym));
    if (!own_value) own_value = sema_value_is_fatal(rhs);
    bool has_res = rt && (rt->kind == TYPE_KIND_PTR ||
                          rt->kind == TYPE_KIND_SLICE ||
                          rt->kind == TYPE_KIND_FUNC ||
                          type_has_own_slots(rt));
    if (!own_value || !has_res) {
      as->discard_mode = 1;   /* 借用 / 无资源：expr + POP */
      return;
    }

    ast_node_t *dh = NULL, *dt = NULL;
    if (vsym && vsym->type &&
        (vsym->ownership == OWN_OWN || sema_sym_is_fatal(vsym))) {
      /* 变量：直接销毁它（move 已在 ex_move 标 moved / 消费 fatal） */
      ast_node_t *id = ast_ident_new(sema->arena, 0, 0);
      if (id) {
        ((ast_ident_t *)id)->name = ((ast_ident_t *)vsym_node)->name;
        if (!gen_destroy(sema, vsym->type, id, &dh, &dt)) dh = NULL;
      }
      if (!dh) {
        as->discard_mode = 3;   /* 回退：运行期 DISPOSE */
        return;
      }
      if (!sema_append_prelude(sema, dh)) {
        as->discard_mode = 3;
        return;
      }
      as->discard_mode = 2;
      return;
    }

    /* 将亡值（new / 构造 / clone / 返回 fatal 的调用）：临时变量 + 赋值 +
       销毁序列。临时变量是 own，符号标 moved（销毁由生成的序列完成，块退出
       不再 FREE）。 */
    uint32_t dn = sema->clone_seq++;
    char nb[64];
    snprintf(nb, sizeof nb, "__disc_%u", dn);
    strslice_t tname = discard_name(sema, nb);
    if (!tname.ptr) { as->discard_mode = 3; return; }
    sema_symbol_t dsym;
    memset(&dsym, 0, sizeof dsym);
    dsym.kind      = SEMA_SYM_VAR;
    dsym.type      = rt;
    dsym.ownership = OWN_OWN;
    dsym.flow_init = true;
    dsym.moved     = true;
    dsym.is_active = true;
    dsym.own_scope = scope;
    if (!sema_scope_define(scope, tname, &dsym)) {
      as->discard_mode = 3;
      return;
    }
    ast_node_t *vd   = ast_var_def_new(sema->arena, 0, 0);
    ast_node_t *ud   = ast_undef_new(sema->arena, 0, 0);
    ast_node_t *tre  = sema_gen_type_ref(sema, rt);
    ast_node_t *asgn = ast_assign_new(sema->arena, 0, 0);
    ast_node_t *lhs  = ast_ident_new(sema->arena, 0, 0);
    ast_node_t *did  = ast_ident_new(sema->arena, 0, 0);
    if (!vd || !ud || !tre || !asgn || !lhs || !did) {
      as->discard_mode = 3;
      return;
    }
    vd->is_generated = true;
    ((ast_var_def_t *)vd)->name      = tname;
    ((ast_var_def_t *)vd)->type_expr = tre;
    ((ast_var_def_t *)vd)->init      = ud;
    asgn->is_generated = true;
    ((ast_assign_t *)asgn)->target = lhs;
    ((ast_assign_t *)asgn)->op     = NULL;   /* NULL = 普通赋值（compiler 特判） */
    ((ast_assign_t *)asgn)->value  = as->value;
    ((ast_ident_t *)lhs)->name = tname;
    ((ast_ident_t *)did)->name = tname;
    if (!gen_destroy(sema, rt, did, &dh, &dt)) dh = NULL;
    vd->next = asgn;
    asgn->next = dh;
    if (dh) dt->next = NULL;
    if (!sema_append_prelude(sema, vd)) {
      as->discard_mode = 3;
      return;
    }
    as->discard_mode = 2;
    return;
  }

  /* 左值从 VM scope 链 lookup（与 sema 作用域树同构）。
     先判 comptime var：它的**常量真实值**现在也会绑定进 VM 作用域
     （常量槽位解析需要，见 sema_eval_comptime_var 步骤 5），因此不能再靠
     "VM scope 查不到" 来识别编译期常量——必须在命中之前查符号表。 */
  sema_symbol_t *csym = sema_lookup(scope, name);
  if (csym && csym->is_comptime && csym->ct_valid) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to compile-time constant '%.*s'",
               (int)name.len, name.ptr);
    return;
  }

  value_t *lhs = scope_lookup(sema->vm->current_scope, name);
  if (!lhs) {
    /* comptime var 不在 VM scope：赋值给编译期常量 → 编译错误 */
    sema_symbol_t *sym = sema_lookup(scope, name);
    if (sym && sym->is_comptime && sym->ct_valid) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to compile-time constant '%.*s'",
                 (int)name.len, name.ptr);
      return;
    }
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "undefined variable '%.*s' in assignment", (int)name.len,
               name.ptr);
    return;
  }

  /* nil 赋值（a = nil）：nil 非 value（AST_NIL 的 sema_expr 会报错），
     先于 rhs 求值识别——合法消费点在 shadow_assign 内特判（STORE_NIL）。 */
  bool rhs_nil = as->value->kind == AST_NIL;
  /* 简单赋值 RHS 匿名构造注入：左值类型已知 → push anon_ct，使 RHS 的
     .{...} 推断目标类型（q: Point = .{...} 赋值给已声明变量）。 */
  const type_t *lt_ty = value_type(lhs);
  value_t *rhs = NULL;
  if (rhs_nil) {
    rhs = NULL;
  } else {
    bool ct_pushed = sema_anon_push(sema, lt_ty);
    rhs = sema_expr(sema, &as->value, scope);
    sema_anon_pop(sema, ct_pushed);
  }
  bool rhs_bad = value_is_error(sema->vm, rhs) ||
                 value_is_type(rhs, TYPE_KIND_VOID);

  if (token_is(as->op, "=")) {
    sema_symbol_t *sym = sema_lookup(scope, name);

    /* const 赋值检查（TDZ 豁免）：声明 const 且已初始化（flow_init=true）
       的变量不可再赋值；flow_init=false（未初始化声明 var a:const T =
       undefined）时的赋值是首次初始化，豁免（const 变量的 TDZ 赋值 =
       初始化，仅一次）。经 value 层接口查询 const（type is value）。 */
    if (sym && value_has_const(lhs) && sym->flow_init) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to const variable '%.*s'",
                 (int)name.len, name.ptr);
      return;
    }

    if (rhs_nil) {
      /* nil 赋值（docs m2-design §12.5/12.6）：a = nil → STORE_NIL <name>
         （运行期只置 ok=false，不释放 value 字段）。左值必须 ?T 类型。 */
      const type_t *lt = value_type(lhs);
      if (!lt || lt->kind != TYPE_KIND_OPTION) {
        char tn[64];
        sema_type_name(lt, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, as->value),
                   "cannot assign nil to variable '%.*s' of type %s (only "
                   "optional types accept nil)",
                   (int)name.len, name.ptr, tn);
        return;
      }
      if (sym) sym->flow_init = true;
      return;
    }

    if (rhs_bad) return; /* 错误恢复产物跳过，已有诊断 */

    /* **赋值点旧值销毁**（A 方案 §2.3 表 "旧块摘挂"）：`p = new i32{2}` 这类
       覆盖自有堆块的位置，旧块必须先释放（否则再也找不到 → 泄漏）。
       由 sema 生成销毁序列挂在 as->destroy，compiler 在「右值求值之后、
       STORE 之前」逐条编译（右值可能读旧值，顺序不能反）。

       只对 **ptr / slice** 生成：聚合（struct/tuple/union/array/option）的
       `value_assign` 走 compound_assign_transfer，运行期已按槽位所有权
       释放旧值——这里再生成会双释放。指针/切片的 assign 是纯 memcpy
       （设计如此：释放权归 sema），所以旧值只能由这里释放。
       move 过的变量（moved）已交出堆块，不再释放；未初始化（flow_init=false）
       也没有旧值。 */
    if (sym && sym->type && sym->flow_init && !sym->moved &&
        sym->ownership == OWN_OWN &&
        (sym->type->kind == TYPE_KIND_PTR ||
         sym->type->kind == TYPE_KIND_SLICE ||
         sym->type->kind == TYPE_KIND_FUNC)) {
      /* FUNC：闭包 = 捕获聚合结构体。`func_assign` 只是 memcpy（不释放旧
         env）→ 旧闭包的 own 捕获必须在这里 `FREE f` 掉（VM 按 capture_type
         聚合释放）。own 闭包是 move-only，唯一持有者，安全。 */
      size_t nlen = name.len;
      char *nbuf = (char *)arena_calloc(sema->arena, nlen + 1, 1,
                                        ALIGNOF(max_align_t));
      if (nbuf) {
        memcpy(nbuf, name.ptr, nlen);
        ast_node_t *old = ast_ident_new(sema->arena, 0, 0);
        if (old) {
          ((ast_ident_t *)old)->name = strslice_from_bytes(nbuf, nlen);
          ast_node_t *dhead = NULL, *dtail = NULL;
          if (gen_destroy(sema, sym->type, old, &dhead, &dtail))
            as->destroy = dhead;
        }
      }
    }

    /* 简单赋值：value_assign 校验；赋值成功 → 数据流 flow_init=true
       （TDZ 退出由确定性赋值分析承担，VM 值层不感知）。
       R1（own 不可 copy，§3.1）：own *T 变量被赋值 = copy → 编译错误
       （own 唯一所有权只能 move/clone 转移，不能按值复制）。
       R2 恢复：赋值成功 → moved 标记清除（move 后重新赋值 = 重新获得
       所有权，变量恢复可用）。 */
    value_t *r = value_assign(sema->vm, lhs, rhs);
    if (value_is_error(sema->vm, r)) {
      char tn[64], rn[64];
      sema_type_name(value_type(lhs), tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s to variable '%.*s' of type %s", rn,
                 (int)name.len, name.ptr, tn);
    } else {
      const type_t *lt = value_type(lhs);
      if (lt && value_ownership(lhs) == OWN_OWN &&
          !sema_value_is_fatal(rhs)) {
        char rn[64];
        sema_type_name(value_type(rhs), rn, sizeof(rn));
        diag_error(sema->diag, sema_loc(sema, as->value),
                   "cannot copy %s into own pointer '%.*s' (own is "
                   "move-only; use move() or clone())",
                   rn, (int)name.len, name.ptr);
        return;
      }
      /* 注 2：share/weak 引用计数共享已移除。
         注 3：原先这里还有一条"含 own 字段的复合类型赋值须 move/clone"的守卫
         （所有权扩散）。复合类型字段现已禁止携带指针/切片，该形态不存在；
         own 指针本身的 copy 拦截由上方分支负责（判据是**值/符号层 ownership**，
         不是类型层——所有权已从类型剥离）。 */
      if (sym) {
        sym->flow_init = true;
        /* R2 恢复：赋值成功 → moved 清除。**之前**是 moved 的变量，旧内容所有权
           已随 move 转移（内存里是陈旧字节）——重新赋值必须走 STORE_REINIT
           （只 memcpy，不释放旧值），否则运行期 compound_assign_transfer 会按
           旧字节二次释放 own 槽位（实测 0xC0000005）。 */
        if (sym->moved) as->reinit = true;
        sym->moved = false;
        /* 注：这里原先还有一条"含 ref 字段的聚合类型赋值作用域检查"
           （判据 `type_contains_ref`）。所有权从类型层迁移到值/符号层后该谓词
           失效，且复合类型已禁止携带指针/切片，含 ref 字段的聚合不存在——整段
           检查恒不可达，已删除。
           将来恢复含指针的聚合时，判据应改为"聚合的 ref 槽位表"（扁平槽位模型，
           见 docs/ownership-refactor.md §2.5），而不是恢复类型层递归谓词。
           顶层 ref 的重绑定检查见下面的 R3 分支。 */
        /* R3 重绑定切换借源：ref/opaque 变量 r = c; 换借目标（旧来源集合
           释放、新来源集合登记，与定义点同一收集器——含函数返回值按标注
           继承多来源）。 */
        if (sym->type &&
            (sym->ownership == OWN_REF ||
             sym->type->kind == TYPE_KIND_OPAQUE)) {
          sema_symbol_t *srcs[16];
          size_t nsrc = borrow_collect(sema, as->value, scope, srcs, 16);
          if (nsrc == 0 && sym->annot_names) {
            for (size_t i = 0; i < sym->annot_count; i++) {
              sema_symbol_t *a = sema_lookup(scope, sym->annot_names[i]);
              if (a && a->kind == SEMA_SYM_VAR)
                nsrc = borrow_collect_add(srcs, nsrc, 16, a);
            }
          }
          /* §5 重绑定逃逸校验（m3-design §5"重绑定时校验是否逃逸"）：
             r 的标注集合（定义时记录）须涵盖**每个**新借源的定义作用域——
             任一来源存活超出标注上限即逃逸。annot_count=0 = 定义时无标注，
             重绑定同样跳过。 */
          if (sym->annot_count > 0) {
            for (size_t k = 0; k < nsrc; k++) {
              sema_symbol_t *src = srcs[k];
              if (!src || !src->own_scope) continue;
              for (size_t i = 0; i < sym->annot_count; i++) {
                sema_scope_t *anchor =
                    annot_anchor_scope(sema, sym->annot_names[i], scope);
                if (!anchor) {
                  diag_error(sema->diag, sema_loc(sema, as->value),
                             "scope annotation '%.*s' does not name a "
                             "parameter, global or local variable",
                             (int)sym->annot_names[i].len,
                             sym->annot_names[i].ptr);
                  continue;
                }
                if (!sema_scope_within(src->own_scope, anchor)) {
                  diag_error(sema->diag, sema_loc(sema, as->value),
                             "rebinding ref variable '%.*s' escapes scope "
                             "annotation '%.*s' (new source's scope is not "
                             "nested within the annotated scope)",
                             (int)name.len, name.ptr,
                             (int)sym->annot_names[i].len,
                             sym->annot_names[i].ptr);
                }
              }
            }
          }
          borrow_srcs_set(sema, sym, srcs, nsrc);
        }

        /* 聚合（含 ref 槽位）**整体赋值**：换值 = 换借源集合（旧集合释放，
           新集合从右值构造/拷贝里收集）。ref/opaque 变量走上一条 R3 分支。 */
        if (sym->type &&
            !(sym->ownership == OWN_REF ||
              sym->type->kind == TYPE_KIND_OPAQUE) &&
            type_has_ref_slots(sym->type)) {
          sema_symbol_t *csrcs[16];
          size_t cn = borrow_collect_value(sema, sym->type, OWN_NONE,
                                           as->value, scope, csrcs, 16, 0);
          borrow_srcs_set(sema, sym, csrcs, cn);
        }
      }
    }
    return;
  }

  /* const 检查：复合赋值是读+写，const 变量已初始化后禁止
     （经 value 层接口查询 const，type is value） */
  {
    sema_symbol_t *sym = sema_lookup(scope, name);
    if (sym && value_has_const(lhs) && sym->flow_init) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to const variable '%.*s'",
                 (int)name.len, name.ptr);
      return;
    }
  }

  /* 复合赋值 x op= nil：nil 非 value，无二元运算语义 → 编译错误 */
  if (rhs_nil) {
    diag_error(sema->diag, sema_loc(sema, as->value),
               "cannot use nil in compound assignment");
    return;
  }

  /* 复合赋值 x op= rhs → x = x op rhs（shadow 走 vtable 类型协商） */
  value_t *(*op)(vm_t *, value_t *, value_t *) = compound_binop(as->op);
  if (!op) {
    char ob[16];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "unsupported compound assignment operator '%s'", ob);
    return;
  }
  value_t *result = op(sema->vm, lhs, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(value_type(lhs), tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  /* 复合赋值结果必须能赋回变量（value_assign 单一校验点） */
  if (value_is_error(sema->vm, value_assign(sema->vm, lhs, result))) {
    char tn[64], rn[64];
    sema_type_name(value_type(lhs), tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign %s to variable '%.*s' of type %s", rn,
               (int)name.len, name.ptr, tn);
  } else {
    /* 复合赋值等价于读+写：变量确定已初始化 */
    sema_symbol_t *sym = sema_lookup(scope, name);
    if (sym) sym->flow_init = true;
  }
}

/* ---- 下标左值赋值：a[i] = v / a[i] op= v ---- */

/*
 * 校验 base 可下标（数组）、索引为整数、元素类型可赋值（value_assign
 * 单一校验点）。元素非独立符号，不写回 flow_init（无 TDZ 概念）。
 * 泛型实例化（base 是类型值）不落入此路径——sema_expr 已报占位诊断。
 * 复合赋值走同一元素类型校验（op 结果可赋回元素）。
 */
static void shadow_assign_index(sema_t *sema, ast_assign_t *as,
                                sema_scope_t *scope) {
  ast_index_t *ix = (ast_index_t *)as->target;

  /* base 求值 + 可下标校验（多维 m[i][j] = v：ix->object 是 AST_INDEX，
     sema_expr 递归走右值下标分支返回元素类型，此处对最外层索引校验；
     vm 层借用引用让写回直达原数组，见 type_array.c 借用引用支持） */
  value_t *base = sema_expr(sema, &ix->object, scope);
  bool bad = value_is_error(sema->vm, base) ||
             value_is_type(base, TYPE_KIND_VOID);
  const type_t *bt = bad ? NULL : value_type(base);
  /* 限定视图不影响字段定位（写 const 由下方 value_has_const(base) 拒绝） */
  if (!bad) bt = type_strip_quals(bt);
  if (!bad && (!bt || (bt->kind != TYPE_KIND_ARRAY &&
                       bt->kind != TYPE_KIND_TUPLE &&
                       !type_is_slice(bt)))) {
    char tn[64];
    sema_type_name(bt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "invalid assignment target: cannot index value of type %s", tn);
    bad = true;
  }

  /* 单索引校验（多索引 = 泛型实参语法预留） */
  if (!bad) {
    size_t nidx = sema_count_siblings(ix->indices);
    if (nidx != 1) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "array subscript expects exactly 1 index, got %zu", nidx);
      bad = true;
    }
  }

  /* 索引表达式求值 + 整数校验 */
  if (!bad) {
    value_t *idx = sema_expr(sema, &ix->indices, scope);
    if (value_is_error(sema->vm, idx) ||
        value_is_type(idx, TYPE_KIND_VOID))
      bad = true;
    else if (!value_is_type(idx, TYPE_KIND_INT)) {
      char tn[64];
      sema_type_name(value_type(idx), tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, ix->indices),
                 "array index must be an integer, got %s", tn);
      bad = true;
    }
  }

  value_t *rhs = sema_expr(sema, &as->value, scope);
  if (bad) return; /* 已有诊断，右值已求值（错误恢复） */
  if (value_is_error(sema->vm, rhs) ||
      value_is_type(rhs, TYPE_KIND_VOID))
    return;

  /* const 数组/切片元素不可写 */
  if (value_has_const(base)) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to element of const array");
    return;
  }
  /* §10.3 [] const T：元素类型是 const → 不可写 */
  if (type_is_slice(bt)) {
    const type_t *elem = slice_type_elem(bt);
    if (elem && type_is_const(elem)) {
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to element of const slice (element type is const)");
      return;
    }
  }

  /* 元素类型可赋值性（value_assign 单一校验点）：数组取元素类型；
     tuple 按位置取元素类型（索引须编译期常量，sema_expr AST_INDEX 已
     校验越界；此处直接按 0 号元素类型校验可赋值性——元素类型同构，
     任意位置校验等价） */
  const type_t *et;
  ownership_t et_own = OWN_NONE;
  if (bt->kind == TYPE_KIND_TUPLE) {
    et = tuple_type_elem(bt, 0) ? tuple_type_elem(bt, 0)->type : NULL;
    /* 元素槽位所有权按编译期下标取（越界已被 sema_expr 拒绝） */
    if (ix->indices && ix->indices->kind == AST_INT_LIT) {
      uint64_t k = ((ast_int_lit_t *)ix->indices)->value;
      const tuple_elem_t *te = tuple_type_elem(bt, (size_t)k);
      if (te) et_own = te->ownership;
    }
  } else if (type_is_slice(bt)) {
    et = slice_type_elem(bt);
    et_own = slice_type_elem_ownership(bt);
  } else {
    et = array_type_elem(bt);
    et_own = array_type_elem_ownership(bt);
  }
  if (!et) return;

  /* 元素是 ref 槽位（或含 ref 槽位）：`s[i] = b.p` 把 b 登记为载体的借源。 */
  if (et_own == OWN_REF || type_has_ref_slots(et)) {
    sema_symbol_t *carrier = borrow_root_symbol(sema, as->target, scope);
    sema_symbol_t *csrcs[16];
    size_t cn = borrow_collect_value(sema, et, et_own, as->value, scope,
                                     csrcs, 16, 0);
    if (carrier && cn) borrow_carry_merge(sema, carrier, csrcs, cn);
  }

  /* **赋值点旧值销毁**：`a[i] = v` 是纯覆盖（INDEX_SET 只 memcpy），
     旧元素里的 own 堆块必须先释放。own 叶子 / 含 own 槽位的复合元素
     都要展开（ref 元素跳过）。 */
  {
    ast_node_t *dh = NULL, *dt = NULL;
    if (gen_destroy_slot(sema, et, et_own, as->target, &dh, &dt))
      as->destroy = dh;
  }

  value_t *dst = value_make_shadow(sema->vm, et);
  if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
    char tn[64], rn[64];
    sema_type_name(et, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, as->value),
               bt->kind == TYPE_KIND_TUPLE
                   ? "cannot assign %s to tuple element of type %s"
                   : "cannot assign %s to array/slice element of type %s",
               rn, tn);
  }
}

/* ---- 字段赋值（p.field = v）：读+写+复合赋值 ----
 * 目标求值（sema_expr AST_MEMBER 返回字段类型 shadow，嵌套 p.a.b 递归）、
 * const 检查（value_has_const(base)）、右值求值（匿名构造注入：字段类型
 * 已知 → push anon_ct）、value_assign 校验可赋值给字段类型。复合赋值
 * 走同一字段类型校验（op 结果可赋回字段）。
 * 运行期由 FIELD_SET 指令写回（compiler 按 op 分派 FIELD_GET/op/FIELD_SET）。
 */
static void shadow_assign_member(sema_t *sema, ast_assign_t *as,
                                 sema_scope_t *scope) {
  ast_member_t *m = (ast_member_t *)as->target;

  /* base 求值 + 可访问校验（sema_expr AST_MEMBER 返回字段类型 shadow；
     非 struct base 已在其中诊断） */
  value_t *base = sema_expr(sema, &m->object, scope);
  bool bad = value_is_error(sema->vm, base) ||
             value_is_type(base, TYPE_KIND_VOID);
  const type_t *bt = bad ? NULL : value_type(base);
  /* 限定视图不影响字段定位（写 const 由下方 value_has_const(base) 拒绝） */
  if (!bad) bt = type_strip_quals(bt);
  /* 指针上的成员写入同样**自动解引用**：`p.field = v` ≡ `p.*.field = v`
     （与读取路径 ex_member 同一改写：包一层 AST_DEREF，compiler 发 PTR_GET）。
     pointee 带 const/volatile 时**拒绝写入**（C 语义：const 只能加不能去）。 */
  if (!bad && bt && ptr_type_base(bt)) {
    const type_t *pbase = ptr_type_base(bt);
    const type_t *ub = type_strip_quals(pbase);
    if (ub && (ub->kind == TYPE_KIND_STRUCT ||
               ub->kind == TYPE_KIND_CUNION ||
               ub->kind == TYPE_KIND_UNION)) {
      if (type_has_quals(pbase)) {
        diag_error(sema->diag, sema_loc(sema, &as->base),
                   "cannot assign to field through a pointer to const value");
        sema_expr(sema, &as->value, scope);
        return;
      }
      ast_node_t *deref =
          ast_deref_new(sema->arena, as->target->tok_begin, as->target->tok_end);
      if (!deref) {
        sema_expr(sema, &as->value, scope);
        return;
      }
      ((ast_deref_t *)deref)->operand = m->object;
      m->object = deref;
      bt = ub;
    }
  }
  if (!bad && (!bt || (bt->kind != TYPE_KIND_STRUCT &&
                       bt->kind != TYPE_KIND_UNION &&
                       bt->kind != TYPE_KIND_CUNION))) {
    char tn[64];
    sema_type_name(bt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "invalid assignment target: cannot access field of value of "
               "type %s",
               tn);
    bad = true;
  }

  /* 字段存在性 + 字段类型（struct：直接按名查表；union：反查所属 member，
     字段名全局唯一——sema 已校验，命中即得字段类型） */
  int idx = -1;
  const type_t *ft = NULL;
  ownership_t slot_own = OWN_NONE;
  if (!bad) {
    if (bt->kind == TYPE_KIND_STRUCT) {
      idx = struct_type_find_field(bt, m->field);
      if (idx < 0) {
        diag_error(sema->diag, sema_loc(sema, &as->base),
                   "struct '%.*s' has no field '%.*s'", (int)bt->name.len,
                   bt->name.ptr, (int)m->field.len, m->field.ptr);
        bad = true;
      } else {
        ft = struct_type_field(bt, (size_t)idx)->type;
        slot_own = struct_type_field(bt, (size_t)idx)->ownership;
      }
    } else if (bt->kind == TYPE_KIND_UNION) {
      /* union 字段：字段名 = member 名（tag 名），按名反查 member →
         该 member 的 payload 类型即字段类型 */
      int mi = union_type_find_member(bt, m->field);
      if (mi < 0) {
        diag_error(sema->diag, sema_loc(sema, &as->base),
                   "union '%.*s' has no field '%.*s'", (int)bt->name.len,
                   bt->name.ptr, (int)m->field.len, m->field.ptr);
        bad = true;
      } else {
        const union_member_t *mem = union_type_member(bt, (size_t)mi);
        if (!mem || !mem->payload_type) {
          bad = true; /* 防御：字段已命中则 payload 必存在 */
        } else {
          ft = mem->payload_type;
          slot_own = mem->ownership;
        }
      }
    } else {
      /* cunion 字段：字段名 = member 名，按名反查 member → 该 member 的
         type 即字段类型（无 tag 校验，C union 语义） */
      int mi = cunion_type_find_member(bt, m->field);
      if (mi < 0) {
        diag_error(sema->diag, sema_loc(sema, &as->base),
                   "cunion '%.*s' has no field '%.*s'", (int)bt->name.len,
                   bt->name.ptr, (int)m->field.len, m->field.ptr);
        bad = true;
      } else {
        const cunion_member_t *mem = cunion_type_member(bt, (size_t)mi);
        if (!mem || !mem->type) {
          bad = true; /* 防御：字段已命中则 member 类型必存在 */
        } else {
          ft = mem->type;
          slot_own = mem->ownership;
        }
      }
    }
  }

  /* const 字段不可写 */
  if (!bad && value_has_const(base)) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to field of const value");
    return;
  }

  /* 右值求值（匿名构造注入：字段类型已知） */
  bool rhs_nil = as->value->kind == AST_NIL;
  value_t *rhs = NULL;
  if (!rhs_nil) {
    bool ct_pushed = sema_anon_push(sema, ft);
    rhs = sema_expr(sema, &as->value, scope);
    sema_anon_pop(sema, ct_pushed);
  }
  if (bad) return; /* 已有诊断，右值已求值（错误恢复） */
  if (value_is_error(sema->vm, rhs) ||
      value_is_type(rhs, TYPE_KIND_VOID))
    return;

  /* 字段是 ref 槽位（或含 ref 槽位）：`s2.q = b.p` 把 b 登记为 s2 的借源
     ——s2 里存着指向 b 拥有堆块的 ref，b 被 move 后该 ref 悬垂。 */
  if (!rhs_nil && ft &&
      (slot_own == OWN_REF || type_has_ref_slots(ft))) {
    sema_symbol_t *carrier = borrow_root_symbol(sema, as->target, scope);
    sema_symbol_t *csrcs[16];
    size_t cn = borrow_collect_value(sema, ft, slot_own, as->value, scope,
                                     csrcs, 16, 0);
    if (carrier && cn) borrow_carry_merge(sema, carrier, csrcs, cn);
  }

  if (rhs_nil) {
    if (!ft || ft->kind != TYPE_KIND_OPTION) {
      char tn[64];
      sema_type_name(ft, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign nil to field '%.*s' (field type %s is not "
                 "optional)",
                 (int)m->field.len, m->field.ptr, tn);
      return;
    }
    /* 改写为匿名构造 .{nil}（AST_CONSTRUCT type=NULL fields=[AST_NIL]）：
       复用 struct 构造 nil 字段路径（compiler 发 PUSH_OPT_NONE <field id>
       → FIELD_SET 隐式转换 ?T→?T 身份短路）。sema_expr 经 anon_ct 注入
       字段类型完成校验。 */
    ast_node_t *nil_node = ast_nil_new(sema->arena, as->value->tok_begin,
                                       as->value->tok_end);
    ast_node_t *ctor = ast_construct_new(sema->arena, as->value->tok_begin,
                                         as->value->tok_end);
    if (nil_node && ctor) {
      ((ast_construct_t *)ctor)->type        = NULL;
      ((ast_construct_t *)ctor)->fields      = nil_node;
      ((ast_construct_t *)ctor)->fields_last = nil_node;
      bool ct_pushed = sema_anon_push(sema, ft);
      sema_expr(sema, &as->value, scope);
      sema_anon_pop(sema, ct_pushed);
    }
    return;
  }

  if (token_is(as->op, "=")) {
    /* own 槽位接收规则（§0.4）：字段/成员位置也是槽位——own 槽位只接
       FATAL，非 own 复合槽位不得 copy OWN 聚合（与构造/`new` 同一张表）。 */
    char what[96];
    snprintf(what, sizeof what, "field '%.*s'", (int)m->field.len,
             m->field.ptr);
    if (!sema_check_slot_flow(sema, slot_own, ft, as->value, rhs, what))
      return;
    /* 简单赋值：value_assign 校验字段类型可赋值性 */
    value_t *dst = value_make_shadow(sema->vm, ft);
    if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
      char tn[64], rn[64];
      sema_type_name(ft, tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s to field '%.*s' of type %s", rn,
                 (int)m->field.len, m->field.ptr, tn);
    }
    return;
  }

  /* 复合赋值 p.field op= rhs → p.field = p.field op rhs（shadow 走 vtable
     类型协商，结果须能赋回字段类型） */
  value_t *(*op)(vm_t *, value_t *, value_t *) = compound_binop(as->op);
  if (!op) {
    char ob[16];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "unsupported compound assignment operator '%s'", ob);
    return;
  }
  value_t *lhs_field = value_make_shadow(sema->vm, ft);
  value_t *result = op(sema->vm, lhs_field, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  if (value_is_error(sema->vm, value_assign(sema->vm, lhs_field, result))) {
    char tn[64], rn[64];
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign %s to field '%.*s' of type %s", rn,
               (int)m->field.len, m->field.ptr, tn);
  }
}

/* ---- M6 FFI：`<库>::<数据符号>` = v（extern var 的写入）----
 *
 * 与读取同源：C ABI 层面就是"往 dlsym 得到的变量地址写 size 字节"。这里做三件事：
 *   ① 解析限定名符号（必须是 SEMA_SYM_EXTERN_VAR——函数符号要报"这是函数"）；
 *   ② 在目标节点上置 is_ffi_var/ffi_var_kind（compiler 据此发 EXTERN_STORE，
 *      契约见 ast_scope_ref.h；读取态的 EXTERN_LOAD 也读同样两个字段）；
 *   ③ 右值类型校验：简单赋值 value_assign(T, rhs)；复合赋值先 shadow 算
 *      `T op rhs` 再校验结果可赋回 T（与普通变量的复合赋值同一套判据）。
 *
 * `nil` 不是合法右值（数据符号没有 optional 形态）。 */
static void shadow_assign_extern_var(sema_t *sema, ast_assign_t *as,
                                     sema_scope_t *scope) {
  ast_scope_ref_t *n = (ast_scope_ref_t *)as->target;

  /* ② 先取符号（type_expr 是库别名 AST_IDENT）。
     `::` 左侧可能是：extern lib 别名（我们的目标）、模块命名空间、枚举类型。
     parser 只放行语法形态，语义在这里分流——每种情况给出各自的精确诊断。 */
  sema_symbol_t *ds = NULL;
  sema_symbol_t *left = NULL;
  const char *libname = NULL;
  size_t liblen = 0;
  if (n->type_expr && n->type_expr->kind == AST_IDENT) {
    ast_ident_t *id = (ast_ident_t *)n->type_expr;
    libname = id->name.ptr;
    liblen = id->name.len;
    left = sema_lookup(scope, id->name);
    char qual[512];
    int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s", (int)id->name.len,
                      id->name.ptr, (int)n->variant.len, n->variant.ptr);
    if (qn > 0 && (size_t)qn < sizeof(qual))
      ds = sema_lookup(sema->global_scope,
                       strslice_from_bytes(qual, (size_t)qn));
  }
  const type_t *ft = (ds && ds->kind == SEMA_SYM_EXTERN_VAR) ? ds->type : NULL;
  if (!ft) {
    if (left && left->kind == SEMA_SYM_MODULE)
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to module member '%.*s::%.*s' (module exports "
                 "are read-only through the namespace)",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    else if (left && left->kind == SEMA_SYM_TYPE)
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to '%.*s::%.*s' (enum variant is not a variable)",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    else if (ds && ds->kind == SEMA_SYM_FUNC)
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "cannot assign to '%.*s::%.*s': it is an `extern cfunc` "
                 "(a function, not a variable)",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    else
      diag_error(sema->diag, sema_loc(sema, &as->base),
                 "invalid assignment target: '%.*s::%.*s' is not an "
                 "`extern var` declaration",
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr);
    sema_expr(sema, &as->value, scope);
    return;
  }
  uint32_t kind = 0;
  if (ffi_data_kind_of(ft, sema->vm, &kind)) {
    /* 标量/指针/str：宽度由类型码表达 */
  } else if (ffi_data_kind_needs_type(ft) && token_is(as->op, "=")) {
    /* struct/cunion/enum：compiler 在写之前压一个类型值（类型表达式取自
       extern var 的声明）。复合赋值（+= 等）对聚合类型无意义 → 拒绝。 */
    kind = BCODE_FFI_DATA_TYPED;
    ast_var_def_t *evd = (ds->ast && ds->ast->kind == AST_VAR_DEF)
                             ? (ast_var_def_t *)ds->ast
                             : NULL;
    n->ffi_var_type_expr = evd ? evd->type_expr : NULL;
  } else {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign to '%.*s::%.*s': type is not supported for "
               "extern var", (int)liblen, libname, (int)n->variant.len,
               n->variant.ptr);
    sema_expr(sema, &as->value, scope);
    return;
  }
  n->is_ffi_var = true;
  n->ffi_var_kind = kind;

  if (as->value->kind == AST_NIL) {
    diag_error(sema->diag, sema_loc(sema, as->value),
               "cannot assign nil to `extern var` (no optional form for a "
               "library data symbol)");
    return;
  }

  /* ③ 右值求值 + 类型校验（简单 / 复合两条路径） */
  value_t *rhs = sema_expr(sema, &as->value, scope);
  if (value_is_error(sema->vm, rhs) || value_is_type(rhs, TYPE_KIND_VOID))
    return;

  value_t *dst = value_make_shadow(sema->vm, ft);
  if (token_is(as->op, "=")) {
    if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
      char tn[64], rn[64];
      sema_type_name(ft, tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s to `%.*s::%.*s` of type %s", rn,
                 (int)liblen, libname, (int)n->variant.len, n->variant.ptr, tn);
    }
    return;
  }
  value_t *(*op)(vm_t *, value_t *, value_t *) = compound_binop(as->op);
  if (!op) {
    char ob[16];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "unsupported compound assignment operator '%s'", ob);
    return;
  }
  value_t *result = op(sema->vm, dst, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  if (value_is_error(sema->vm, value_assign(sema->vm, dst, result))) {
    char tn[64], rn[64];
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, as->value),
               "cannot assign %s to `%.*s::%.*s` of type %s", rn,
               (int)liblen, libname, (int)n->variant.len, n->variant.ptr, tn);
  }
}

/* ---- 解引用左值赋值（r.* = v，m3-design §8.2 解引用 SET 严格分离）----
 * 目标求值（指针操作数须为 own/ref/fatal 指针 → 取被指向类型 T 为字段
 * 类型；非指针已在诊断中报错）、const 检查（被指向类型含 const → 不可
 * 写）、右值求值（匿名构造注入 T）、value_assign 校验可赋值给 T。运行期
 * 由 PTR_SET 指令写回（compiler 按 op 分派 PTR_GET/op/PTR_SET）。 */
static void shadow_assign_deref(sema_t *sema, ast_assign_t *as,
                                sema_scope_t *scope) {
  ast_deref_t *n = (ast_deref_t *)as->target;
  /* 指针操作数求值（sema_expr AST_DEREF 分支：非指针已在其中诊断） */
  value_t *base = sema_expr(sema, &n->operand, scope);
  bool bad = value_is_error(sema->vm, base) ||
             value_is_type(base, TYPE_KIND_VOID);
  const type_t *bt = bad ? NULL : value_type(base);
  /* 限定视图不影响字段定位（写 const 由下方 value_has_const(base) 拒绝） */
  if (!bad) bt = type_strip_quals(bt);
  const type_t *ft = bad ? NULL : ptr_type_base(bt);
  if (!bad && !ft) {
    char tn[64];
    sema_type_name(bt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "invalid assignment target: cannot dereference a value of "
               "type %s (expected a pointer)",
               tn);
    bad = true;
  }

  /* 被指向类型含 const → 不可写（ref 可写性取决于被指向是否 const） */
  if (!bad && type_has_const(ft)) {
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign through pointer to const value");
    return;
  }

  /* 右值求值（匿名构造注入：被指向类型已知） */
  bool rhs_nil = as->value->kind == AST_NIL;
  value_t *rhs = NULL;
  if (!rhs_nil) {
    bool ct_pushed = sema_anon_push(sema, ft);
    rhs = sema_expr(sema, &as->value, scope);
    sema_anon_pop(sema, ct_pushed);
  }
  if (bad) return; /* 已有诊断，右值已求值（错误恢复） */
  if (value_is_error(sema->vm, rhs) ||
      value_is_type(rhs, TYPE_KIND_VOID))
    return;

  /* 字段是 ref 槽位（或含 ref 槽位）：`s2.q = b.p` 把 b 登记为 s2 的借源
     ——s2 里存着指向 b 拥有堆块的 ref，b 被 move 后该 ref 悬垂。 */
  if (!rhs_nil && ft && type_has_ref_slots(ft)) {
    sema_symbol_t *carrier = borrow_root_symbol(sema, as->target, scope);
    sema_symbol_t *csrcs[16];
    size_t cn = borrow_collect_value(sema, ft, OWN_NONE, as->value, scope,
                                     csrcs, 16, 0);
    if (carrier && cn) borrow_carry_merge(sema, carrier, csrcs, cn);
  }

  if (rhs_nil) {
    if (!ft || ft->kind != TYPE_KIND_OPTION) {
      char tn[64];
      sema_type_name(ft, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign nil through pointer (pointee type %s is not "
                 "optional)",
                 tn);
      return;
    }
    /* 改写为匿名构造 .{nil}（与 shadow_assign_member 同款：复用 struct
       构造 nil 字段路径，compiler 发 PUSH_OPT_NONE → PTR_SET 隐式转换
       ?T→?T 身份短路）。 */
    ast_node_t *nil_node = ast_nil_new(sema->arena, as->value->tok_begin,
                                       as->value->tok_end);
    ast_node_t *ctor = ast_construct_new(sema->arena, as->value->tok_begin,
                                         as->value->tok_end);
    if (nil_node && ctor) {
      ((ast_construct_t *)ctor)->type        = NULL;
      ((ast_construct_t *)ctor)->fields      = nil_node;
      ((ast_construct_t *)ctor)->fields_last = nil_node;
      bool ct_pushed = sema_anon_push(sema, ft);
      sema_expr(sema, &as->value, scope);
      sema_anon_pop(sema, ct_pushed);
    }
    return;
  }

  if (token_is(as->op, "=")) {
    /* 被指向值含 ref 槽位：`p.* = .S{ .q = b.p }` 把 b 登记为 p 的借源。 */
    if (ft && type_has_ref_slots(ft)) {
      sema_symbol_t *carrier = borrow_root_symbol(sema, as->target, scope);
      sema_symbol_t *csrcs[16];
      size_t cn = borrow_collect_value(sema, ft, OWN_NONE, as->value, scope,
                                       csrcs, 16, 0);
      if (carrier && cn) borrow_carry_merge(sema, carrier, csrcs, cn);
    }
    /* **赋值点旧值销毁**：`p.* = v` 是纯覆盖（PTR_SET 只 memcpy），
       旧被指向值里的 own 槽位必须先释放。只对 **own 指针**生成——
       经 ref 指针写别人拥有的内容时不能替对方释放（借用方不拥有）。 */
    if (value_ownership(base) == OWN_OWN && ft) {
      ast_node_t *dh = NULL, *dt = NULL;
      if (gen_destroy(sema, ft, as->target, &dh, &dt)) as->destroy = dh;
    }
    /* 简单赋值：value_assign 校验被指向类型可赋值性 */
    value_t *dst = value_make_shadow(sema->vm, ft);
    if (value_is_error(sema->vm, value_assign(sema->vm, dst, rhs))) {
      char tn[64], rn[64];
      sema_type_name(ft, tn, sizeof(tn));
      sema_type_name(value_type(rhs), rn, sizeof(rn));
      diag_error(sema->diag, sema_loc(sema, as->value),
                 "cannot assign %s through pointer to %s", rn, tn);
    }
    return;
  }

  /* 复合赋值 r.* op= v → r.* = r.* op v（shadow 走 vtable 类型协商，
     结果须能赋回被指向类型） */
  value_t *(*op)(vm_t *, value_t *, value_t *) = compound_binop(as->op);
  if (!op) {
    char ob[16];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "unsupported compound assignment operator '%s'", ob);
    return;
  }
  value_t *lhs_field = value_make_shadow(sema->vm, ft);
  value_t *result = op(sema->vm, lhs_field, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], tn[64], rn[64];
    size_t len = 0;
    const char *text = as->op ? token_get_text(as->op, &len) : NULL;
    snprintf(ob, sizeof(ob), "%.*s", (int)len, text ? text : "?");
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(rhs), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, tn, rn);
    return;
  }
  if (value_is_error(sema->vm, value_assign(sema->vm, lhs_field, result))) {
    char tn[64], rn[64];
    sema_type_name(ft, tn, sizeof(tn));
    sema_type_name(value_type(result), rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &as->base),
               "cannot assign %s through pointer to %s", rn, tn);
  }
}

/* ---- 语句：控制流 ---- */

/* 捕获读取被改写为 `__ctx.env.<cap>`（捕获聚合结构体）后的**同构识别**：
   返回捕获名。返回 ref 的 §6/§9 检查（来源必须是参数/捕获/全局、标注不得
   更宽）按名字做静态分析，必须能看穿这个改写。 */
static bool capture_access_name(ast_node_t *e, strslice_t *out) {
  if (!e || e->kind != AST_MEMBER) return false;
  ast_member_t *cap = (ast_member_t *)e;
  if (!cap->object || cap->object->kind != AST_MEMBER) return false;
  ast_member_t *env = (ast_member_t *)cap->object;
  if (!env->object || env->object->kind != AST_IDENT) return false;
  if (env->field.len != 3 || !env->field.ptr ||
      memcmp(env->field.ptr, "env", 3) != 0)
    return false;
  ast_ident_t *ctx = (ast_ident_t *)env->object;
  if (ctx->name.len != 5 || !ctx->name.ptr ||
      memcmp(ctx->name.ptr, "__ctx", 5) != 0)
    return false;
  *out = cap->field;
  return true;
}

static block_result_t walk_return(sema_t *sema, ast_return_t *rt,
                                  sema_scope_t *scope) {
  block_result_t r = {.definitely_returns = true};
  if (rt->value) {
    value_t *v = sema_expr(sema, &rt->value, scope);
    bool v_bad = value_is_error(sema->vm, v) ||
                 value_is_type(v, TYPE_KIND_VOID);
    if (!v_bad) {
      if (!sema->fn.return_type) {
        diag_error(sema->diag, sema_loc(sema, rt->value),
                   "void function cannot return a value");
      } else {
        /* value_assign 校验返回类型可赋给签名返回类型 */
        value_t *dst = value_make_shadow(sema->vm, sema->fn.return_type);
        if (value_is_error(sema->vm, value_assign(sema->vm, dst, v))) {
          char tn[64], rn[64];
          sema_type_name(sema->fn.return_type, tn, sizeof(tn));
          sema_type_name(value_type(v), rn, sizeof(rn));
          diag_error(sema->diag, sema_loc(sema, rt->value),
                     "cannot return %s from function returning %s", rn, tn);
        } else if (sema->fn.return_ownership == OWN_REF &&
                   (rt->value->kind == AST_IDENT ||
                    capture_access_name(rt->value, &(strslice_t){0}))) {
          /* R3（返回 ref 来源，m3-design §6）：返回值 ref 的（标注）作用域
             必然来自参数或全局——本地 own 变量/临时借用不能作为返回值 ref
             来源（返回后栈上值销毁，借用悬垂）。检查：return 的值是
             标识符时，须是 ref/fatal 参数（fatal 返回继续传递所有权）或
             全局 ref。非标识符表达式（x.& 取栈值地址等）一律拒绝。 */
          strslice_t rname = ((ast_ident_t *)rt->value)->name;
          if (rt->value->kind != AST_IDENT)
            capture_access_name(rt->value, &rname);
          sema_symbol_t *rs = sema_lookup(scope, rname);
          if (rs && rs->kind == SEMA_SYM_VAR && rs->type &&
              rs->ownership == OWN_REF) {
            /* §6 返回 ref 来源：必须是参数或全局。参数层符号
               （sema_lookup 沿 scope 链第一个命中）或全局符号放行；
               本地 own 的 ref 变量/外层局部借用 → 悬垂 → 拒绝。 */
            sema_symbol_t *g = sema_lookup(sema->global_scope, rname);
            /* 参数判定：遍历真实参数列表（body 顶层变量也注册在
               param_scope，sema_scope_find_local 无法区分） */
            bool is_param = false;
            for (ast_node_t *p = sema->fn.def ? sema->fn.def->params : NULL;
                 p && !is_param; p = p->next) {
              ast_var_def_t *pvd = (ast_var_def_t *)p;
              is_param = pvd->name.len == rname.len &&
                         memcmp(pvd->name.ptr, rname.ptr, pvd->name.len) == 0;
            }
            bool is_global = g == rs;
            /* §9 闭包捕获视同特殊参数：返回 ref 可来自捕获变量
               （调用闭包时闭包对象必然存活，捕获的 ref 有效） */
            bool is_capture = false;
            if (sema->fn.def && sema->fn.def->captures) {
              for (ast_node_t *c = sema->fn.def->captures; c; c = c->next) {
                ast_var_def_t *cv = (ast_var_def_t *)c;
                if (cv->name.len == rname.len &&
                    memcmp(cv->name.ptr, rname.ptr, rname.len) == 0) {
                  is_capture = true;
                  break;
                }
              }
            }
            if (!is_param && !is_global && !is_capture) {
              diag_error(sema->diag, sema_loc(sema, rt->value),
                         "cannot return ref borrowed from local "
                         "'%.*s' (returned ref must come from a parameter, "
                         "capture or global)",
                         (int)rname.len, rname.ptr);
            }
            /* §9 闭包返回捕获 ref 的标注关系检查：返回值标注的每个锚点
               须嵌套于闭包标注的每个锚点（返回值存活 ≤ 闭包存活 ≤ 锚点）。
               返回值来自捕获 → 返回值存活受闭包存活约束；返回值标注不得
               比闭包标注更宽（否则调用方以为返回值活到 y，实际活不过 x）。 */
            if (is_capture && sema->fn.def &&
                sema->fn.def->scope_annot_count > 0) {
              strslice_t *ret_names = NULL;
              size_t ret_count = 0;
              sema_annot_peek(sema->fn.def->return_expr,
                              &ret_names, &ret_count);
              for (size_t i = 0; i < ret_count; i++) {
                sema_scope_t *ret_anchor =
                    annot_anchor_scope(sema, ret_names[i], scope);
                if (!ret_anchor) continue; /* 未知锚点由他处报 */
                for (size_t j = 0; j < sema->fn.def->scope_annot_count;
                     j++) {
                  sema_scope_t *clo_anchor = annot_anchor_scope(
                      sema, sema->fn.def->scope_annot_names[j], scope);
                  if (!clo_anchor) continue;
                  if (!sema_scope_within(ret_anchor, clo_anchor)) {
                    diag_error(sema->diag, sema_loc(sema, rt->value),
                               "returned ref annotation '%.*s' escapes "
                               "closure annotation '%.*s' (return scope must "
                               "be nested within closure scope)",
                               (int)ret_names[i].len, ret_names[i].ptr,
                               (int)sema->fn.def->scope_annot_names[j].len,
                               sema->fn.def->scope_annot_names[j].ptr);
                  }
                }
              }
            }
          }
        }
      }
    }

    /* R1（fatal 参数消费，§4）：`return p;`（p 为 fatal 指针**或切片**参数）
       = 所有权继续传递给调用方 → 标记已消费，函数结束的"未消费"检查放行。
       **与返回类型无关**：这段标记原先写在 `: ref *T` 的分支内部，fatal 指针/
       切片返回根本走不到，于是 `func id(p: fatal []i32): fatal []i32 { return p; }`
       被误报"未消费"（诊断文本自己写的是 "move() or return it"）。
       判据与 sema_walk_function 末尾的检查（PTR_FATAL || SLICE_FATAL）一致。 */
    if (rt->value && rt->value->kind == AST_IDENT) {
      sema_symbol_t *rs = sema_lookup(scope, ((ast_ident_t *)rt->value)->name);
      if (rs && rs->kind == SEMA_SYM_VAR && sema_sym_is_fatal(rs)) {
        sema_mark_fatal_consumed(rs);
      }
    }
  } else {
    if (sema->fn.return_type &&
        sema->fn.return_type->kind != TYPE_KIND_VOID) {
      char tn[64];
      sema_type_name(sema->fn.return_type, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &rt->base),
                 "function returning %s must return a value", tn);
    }
  }
  /* 为 return 路径沿 scope 链收集 own 变量的 FREE 语句，追加到 defers 链末尾。
     FREE 在 defer calls 之后执行（defer 可能引用 own 变量，须先执行）。
     defers 链由 Pass 3a desugar_defers 设置（可能为 NULL）；FREE 追加到末尾。 */
  {
    const sema_scope_t *stop = find_func_stop_sema(scope);
    ast_node_t *free_stmts = collect_free_stmts(sema, scope, stop);
    if (free_stmts) {
      if (rt->base.defers) {
        ast_node_t *tail = rt->base.defers;
        while (tail->next) tail = tail->next;
        tail->next = free_stmts;
      } else {
        rt->base.defers = free_stmts;
      }
      /* walk 生成的 FREE 语句 */
      for (ast_node_t *f = free_stmts; f; f = f->next)
        walk_stmt(sema, f, scope, &((size_t){0}));
    }
  }
  return r;
}

static block_result_t walk_while(sema_t *sema, ast_while_t *wl,
                                 sema_scope_t *scope, size_t *idx) {
  value_t *cond = sema_expr(sema, &wl->cond, scope);
  sema_check_bool(sema, wl->cond, cond, "while condition");

  /* 循环体可能执行 0 次：体内赋值不提升外层变量的确定性（保守） */
  flow_snap_t snap = flow_capture(sema, scope);

  sema_scope_t *body_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* 循环体块：VM scope 与 sema scope 树同构 */
  size_t sub = 0;
  walk_block(sema, wl->body, body_scope ? body_scope : scope, &sub);
  sema_scope_release_borrows(body_scope); /* R3：体内借用的 ref 载体随作用域消亡 */
  vm_pop_scope(sema->vm);

  flow_restore(&snap);
  flow_release(&snap);
  return (block_result_t){0}; /* 循环体可能不执行，不贡献 definitely_returns */
}

static block_result_t walk_dowhile(sema_t *sema, ast_dowhile_t *dw,
                                   sema_scope_t *scope, size_t *idx) {
  /* do-while：条件在体后。体至少执行一次——但条件不满足时循环结束，
     体内赋值对外层变量的确定性仍不提升（保守，与 while 一致） */
  flow_snap_t snap = flow_capture(sema, scope);

  sema_scope_t *body_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* 循环体块：VM scope 与 sema scope 树同构 */
  size_t sub = 0;
  walk_block(sema, dw->body, body_scope ? body_scope : scope, &sub);
  sema_scope_release_borrows(body_scope); /* R3 */
  vm_pop_scope(sema->vm);

  value_t *cond = sema_expr(sema, &dw->cond, scope);
  sema_check_bool(sema, dw->cond, cond, "do-while condition");

  flow_restore(&snap);
  flow_release(&snap);
  return (block_result_t){0};
}

static block_result_t walk_for(sema_t *sema, ast_for_t *fr,
                               sema_scope_t *scope, size_t *idx) {
  sema_scope_t *for_scope = sema_scope_child(scope, (*idx)++);
  sema_scope_t *fs = for_scope ? for_scope : scope;

  /* 循环体可能执行 0 次：体内对外层变量的赋值不提升确定性（保守）。
     先快照外层符号，循环结束后恢复（for 变量本身出作用域不可见） */
  flow_snap_t snap = flow_capture(sema, scope);

  vm_push_scope(sema->vm); /* for 作用域（init 变量） */

  /* init 在 for scope 内求值 */
  if (fr->init) {
    switch (fr->init->kind) {
      case AST_VAR_DEF:
        shadow_var_def(sema, (ast_var_def_t *)fr->init, fs);
        break;
      case AST_ASSIGN:
        shadow_assign(sema, (ast_assign_t *)fr->init, fs);
        break;
      case AST_EXPR_STMT:
        sema_expr(sema, &((ast_expr_stmt_t *)fr->init)->expr, fs);
        break;
      default:
        break;
    }
  }

  if (fr->cond) {
    value_t *c = sema_expr(sema, &fr->cond, fs);
    sema_check_bool(sema, fr->cond, c, "for condition");
  }

  /* body 是 for scope 的子 scope（新局部迭代器） */
  size_t body_idx = 0;
  sema_scope_t *body_scope = sema_scope_child(for_scope, body_idx++);
  vm_push_scope(sema->vm); /* 循环体块 */
  size_t sub = 0;
  walk_block(sema, fr->body, body_scope ? body_scope : fs, &sub);
  sema_scope_release_borrows(body_scope); /* R3 */
  vm_pop_scope(sema->vm);

  /* update 与 init 同形：可放赋值语句（j = j + 1）或表达式。
     AST_ASSIGN 必须走 shadow_assign（赋值是语句级检查，sema_expr 只处理
     表达式）。此前无条件走 sema_expr：AST_ASSIGN 落入 switch 的 default
     被静默返回 void shadow ⇒ **for 的步进语句从未被类型检查**。 */
  if (fr->update) {
    if (fr->update->kind == AST_ASSIGN) {
      shadow_assign(sema, (ast_assign_t *)fr->update, fs);
    } else {
      sema_expr(sema, &fr->update, fs);
    }
  }

  flow_restore(&snap); /* 丢弃体内确定性提升（保守） */
  flow_release(&snap);

  sema_scope_release_borrows(for_scope); /* R3：for 作用域（init 变量）退出 */
  vm_pop_scope(sema->vm); /* 退出 for 作用域 */
  return (block_result_t){0};
}

/* ---- if 语句 ---- */

static block_result_t walk_if(sema_t *sema, ast_if_t *it, sema_scope_t *scope,
                              size_t *idx) {
  block_result_t r = {0};
  value_t *cond = sema_expr(sema, &it->cond, scope);
  sema_check_bool(sema, it->cond, cond, "if condition");

  /* 确定性赋值合并点：快照分支前状态 → then → 记录 → 恢复 → else →
     meet（AND）：两分支都 flow_init 才 true。保守策略：非全部分支赋值
     （如 if(c){a=1;}else{}）→ 合并后仍 UNKNOWN，读取编译错误。
     optional 解包不在此做 flow 记录：if (x != nil) 分支内的 x 仍是 ?T，
     解包由显式 x.!（assert）承担（docs m2-design §12.5）。 */
  flow_snap_t snap = flow_capture(sema, scope);

  block_result_t tr = {0};
  sema_scope_t *then_scope = sema_scope_child(scope, (*idx)++);
  vm_push_scope(sema->vm); /* then 块 */
  size_t sub = 0;
  tr = walk_block(sema, it->then_body, then_scope ? then_scope : scope, &sub);
  sema_scope_release_borrows(then_scope); /* R3 */
  vm_pop_scope(sema->vm);

  /* 记录 then 后状态（flow），恢复分支前 */
  for (size_t i = 0; i < snap.n; i++)
    snap.after[i] = snap.syms[i]->flow_init;
  flow_restore(&snap);

  block_result_t er = {0};
  if (it->else_body) {
    if (it->else_body->kind == AST_IF) {
      /* else-if 链：同层递归（子作用域顺序与 3a 一致：else-if 不单独
         建 scope，其 then 是当前 scope 的下一个子节点） */
      er = walk_if(sema, (ast_if_t *)it->else_body, scope, idx);
    } else {
      sema_scope_t *else_scope = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm); /* else 块 */
      size_t sub2 = 0;
      er = walk_block(sema, it->else_body, else_scope ? else_scope : scope,
                      &sub2);
      sema_scope_release_borrows(else_scope); /* R3 */
      vm_pop_scope(sema->vm);
    }
  }

  /* meet：两分支都 INIT 才 INIT（else 缺失视为"未赋值分支"） */
  for (size_t i = 0; i < snap.n; i++)
    snap.syms[i]->flow_init = snap.after[i] && snap.syms[i]->flow_init;
  flow_release(&snap);

  r.definitely_returns = tr.definitely_returns && er.definitely_returns;
  return r;
}

/* ---- switch 语句（docs m2-design §4：if 语法糖，语义内联） ----
 *
 * cond shadow 求值一次（C 局部持有，不注册临时符号——desugar 的
 * `var __switch_N` 是编译期实现细节，用户代码不可见）。每分支每模式
 * value_eq 校验（模式须与 cond 可比），全部分支 walk + flow meet：
 * AND 累积——某变量在**全部分支**都赋值才确定性初始化。无 default 时
 * "不匹配穿透"路径视为未赋值分支（与 else 缺失同语义）。
 * definitely_returns：有 default 且全部分支返回才 true（无 default 时
 * 可能穿透，不贡献）。 */
static block_result_t walk_switch(sema_t *sema, ast_switch_t *sw,
                                  sema_scope_t *scope, size_t *idx) {
  block_result_t r = {0};

  value_t *cond = sema_expr(sema, &sw->cond, scope);
  if (value_is_error(sema->vm, cond) ||
      value_is_type(cond, TYPE_KIND_VOID)) {
    /* cond 错误/void（错误恢复产物）：仍 walk 分支体消费子作用域
       （索引对齐），跳过模式校验避免级联二次诊断 */
    for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
      ast_switch_case_t *sc = (ast_switch_case_t *)cs;
      sema_scope_t *case_scope = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm);
      size_t sub = 0;
      walk_block(sema, sc->body, case_scope ? case_scope : scope, &sub);
      sema_scope_release_borrows(case_scope); /* R3 */
      vm_pop_scope(sema->vm);
    }
    if (sw->default_body) {
      sema_scope_t *def_scope = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm);
      size_t sub = 0;
      walk_block(sema, sw->default_body, def_scope ? def_scope : scope, &sub);
      sema_scope_release_borrows(def_scope); /* R3 */
      vm_pop_scope(sema->vm);
    }
    return r;
  }

  /* 模式校验：每模式与 cond value_eq（值匹配，shadow 间比较）。
     类型不可比（value_eq 返回 error）→ 报错，分支体仍 walk（消费子
     作用域 + 内部检查），flow 不参与 meet（错误已 fail-fast）。 */
  for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
    ast_switch_case_t *sc = (ast_switch_case_t *)cs;
    /* 链上指针迭代：把真实链槽交给 sema_expr，折叠（comptime 变量引用 →
       字面量）就地写回；传局部变量副本会丢折叠，编译期符号引用漏到运行期。 */
    for (ast_node_t **link = &sc->patterns; *link; link = &(*link)->next) {
      value_t *pv = sema_expr(sema, link, scope);
      ast_node_t *pt = *link; /* 折叠可能替换节点：后续用刷新后的节点 */
      if (value_is_error(sema->vm, pv) ||
          value_is_type(pv, TYPE_KIND_VOID))
        continue; /* 错误恢复产物静默通过 */
      value_t *eq = value_eq(sema->vm, cond, pv);
      if (value_is_error(sema->vm, eq)) {
        char cn[64], pn[64];
        sema_type_name(value_type(cond), cn, sizeof(cn));
        sema_type_name(value_type(pv), pn, sizeof(pn));
        diag_error(sema->diag, sema_loc(sema, pt),
                   "switch pattern type mismatch: cannot match %s against %s",
                   cn, pn);
      }
    }
  }

  /* 确定性赋值合并点：快照分支前状态 → 逐分支 walk → 记录 → 恢复 →
     AND 累积。 */
  flow_snap_t snap = flow_capture(sema, scope);
  bool first = true;
  bool all_return = true;

  for (ast_node_t *cs = sw->cases; cs; cs = cs->next) {
    ast_switch_case_t *sc = (ast_switch_case_t *)cs;
    sema_scope_t *case_scope = sema_scope_child(scope, (*idx)++);
    vm_push_scope(sema->vm);
    size_t sub = 0;
    block_result_t br = walk_block(sema, sc->body, case_scope ? case_scope : scope,
                                   &sub);
    sema_scope_release_borrows(case_scope); /* R3 */
    vm_pop_scope(sema->vm);
    if (!br.definitely_returns) all_return = false;
    for (size_t i = 0; i < snap.n; i++) {
      snap.after[i] = first ? snap.syms[i]->flow_init
                            : (snap.after[i] && snap.syms[i]->flow_init);
    }
    first = false;
    flow_restore(&snap);
  }

  if (sw->default_body) {
    sema_scope_t *def_scope = sema_scope_child(scope, (*idx)++);
    vm_push_scope(sema->vm);
    size_t sub = 0;
    block_result_t dr = walk_block(sema, sw->default_body,
                                   def_scope ? def_scope : scope, &sub);
    sema_scope_release_borrows(def_scope); /* R3 */
    vm_pop_scope(sema->vm);
    if (!dr.definitely_returns) all_return = false;
    for (size_t i = 0; i < snap.n; i++) {
      snap.after[i] = first ? snap.syms[i]->flow_init
                            : (snap.after[i] && snap.syms[i]->flow_init);
    }
    first = false;
    flow_restore(&snap);
  }

  if (!first) {
    /* 有分支被 walk：meet。有 default → 全路径覆盖，final = 全分支 AND
       （当前 flow_init 是 restore 后的分支前状态，直接覆盖）；无 default
       → 穿透路径保持分支前状态，final = after && before（AND 进 meet）。 */
    for (size_t i = 0; i < snap.n; i++) {
      if (sw->default_body)
        snap.syms[i]->flow_init = snap.after[i];
      else
        snap.syms[i]->flow_init =
            snap.after[i] && snap.syms[i]->flow_init;
    }
    if (sw->default_body && all_return)
      r.definitely_returns = true;
  }
  flow_release(&snap);
  return r;
}

/* ---- 语句分派 ---- */

/* 索引语义：idx 是"当前 scope"的 children 迭代器。一个块内的语句按序
   消费当前 scope 的子节点（sema_scope_child(scope, (*idx)++)），进入
   嵌套子 scope（BLOCK/if then/else/while body/for body）时用新局部
   迭代器遍历子 scope 的 children——与原版共享外层迭代器相比，嵌套消费
   不再污染浅层（第二个 for 的 init 变量因此找不到定义点）。 */

/* walk_stmt 的**包装**：维护 sema->cur_stmt（扁平化 clone 的切片路径要把
   生成的 prelude 语句挂到"当前语句"上）。保存/恢复保证嵌套语句（if/while
   体里的赋值等）结束后外层语句恢复正确。 */
static block_result_t walk_stmt(sema_t *sema, ast_node_t *stmt,
                                sema_scope_t *scope, size_t *idx);

static block_result_t walk_stmt_impl(sema_t *sema, ast_node_t *stmt,
                                     sema_scope_t *scope, size_t *idx) {
  block_result_t r = {0};
  /* **sema 生成的销毁语句不再回走**（`is_generated`，见 gen_destroy_* /
     ast_free_new）：销毁序列的类型判定已由 type 对象完成，回走没有新信息；
     而回走复合生成形态会造成两类错误：
     - `AST_IF`/`AST_BLOCK` 会到 3a 建的 scope 树里消费子作用域，但生成
       节点没有对应作用域 → "scope tree desync"；
     - `AST_IS_TAG` 是 sema 内部直接形态节点，不是用户表达式，sema_expr
       未登记 → "unhandled expression node"。
     生成节点只由 compiler 消费（见 compile_stmt / compile_expr）。
     **注意**：defer 改写的 `__defer_N()` 调用语句同样是生成节点（tok_begin
     == 0），但**必须继续 walk**——它们要建 sema 符号/VM 绑定，CTFE 求值
     comptime 函数体时靠它解析到 `__defer_N`。故这里只按 `is_generated`
     跳过销毁语句，不按 tok_begin 一刀切。 */
  if (stmt->is_generated) return r;
  switch (stmt->kind) {
    case AST_VAR_DEF:
      shadow_var_def(sema, (ast_var_def_t *)stmt, scope);
      break;
    case AST_TYPE_DEF:
      /* 局部 type 定义：已在 walk_block 入口提升时求值绑定（作用域入口
         生效，前向引用安全）。此处保留为防御分支（未来 walk_if 单语句
         分支等若直接调用 walk_stmt 处理 type def，语义与本分支一致：
         rhs 求值 → 折叠 AST_TYPE_REF → 绑定 vm 当前作用域 → 激活符号）。
         定义点不摘除（进入字节码，运行时 DEFINE 绑定 type value）。 */
      sema_eval_type_def(sema, (ast_type_def_t *)stmt, scope);
      break;
    case AST_ASSIGN:
      shadow_assign(sema, (ast_assign_t *)stmt, scope);
      break;
    case AST_BLOCK: {
      sema_scope_t *child = sema_scope_child(scope, (*idx)++);
      vm_push_scope(sema->vm); /* VM scope 与 sema scope 树同构 */
      size_t sub = 0;
      r = walk_block(sema, stmt, child ? child : scope, &sub);
      sema_scope_release_borrows(child); /* R3：块内借用的 ref 载体随块消亡 */
      vm_pop_scope(sema->vm);
      break;
    }
    case AST_IF:
      r = walk_if(sema, (ast_if_t *)stmt, scope, idx);
      break;
    case AST_SWITCH:
      r = walk_switch(sema, (ast_switch_t *)stmt, scope, idx);
      break;
    case AST_WHILE:
      r = walk_while(sema, (ast_while_t *)stmt, scope, idx);
      break;
    case AST_DOWHILE:
      r = walk_dowhile(sema, (ast_dowhile_t *)stmt, scope, idx);
      break;
    case AST_FOR:
      r = walk_for(sema, (ast_for_t *)stmt, scope, idx);
      break;
    case AST_RETURN:
      r = walk_return(sema, (ast_return_t *)stmt, scope);
      break;
    case AST_BREAK:
    case AST_CONTINUE: {
      /* 为 break/continue 路径沿 scope 链收集 own 变量的 FREE 语句，
         追加到 defers 链末尾（与 return 同机制）。 */
      const sema_scope_t *stop = find_loop_stop_sema(scope);
      ast_node_t *free_stmts = collect_free_stmts(sema, scope, stop);
      if (free_stmts) {
        if (stmt->defers) {
          ast_node_t *tail = stmt->defers;
          while (tail->next) tail = tail->next;
          tail->next = free_stmts;
        } else {
          stmt->defers = free_stmts;
        }
      }
      break;
    }
    case AST_FUNC_DEF:
      /* 局部函数定义：入口提升已处理签名，主循环（walk_block）已跳过并
         消费 fscope 子作用域。防御分支（非块上下文直接调用时零操作）。
         defer 改写后的 __defer_N 也是 AST_FUNC_DEF，走同一路径。 */
      break;
    /* AST_DEFER 已在 Pass 3a desugar_defers 中改写为 AST_FUNC_DEF +
       退出点 call（AST_EXPR_STMT），到达此处时已不存在 AST_DEFER。 */
    case AST_FREE:
      /* sema 生成的 own 变量释放语句：纯运行期指令，sema 无需类型检查。 */
      break;
    case AST_EXPR_STMT: {
      ast_expr_stmt_t *es = (ast_expr_stmt_t *)stmt;
      value_t *v = sema_expr(sema, &es->expr, scope);
      if (!value_is_error(sema->vm, v) &&
          !value_is_type(v, TYPE_KIND_VOID)) {
        char tn[64];
        sema_type_name(value_type(v), tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, es->expr),
                   "expression result of type %s is unused; use '_ = expr' "
                   "to discard",
                   tn);
      }
      break;
    }
    default:
      break;
  }
  return r;
}

/* 见上文包装声明：进入语句时设置 cur_stmt，离开时恢复（嵌套安全）。 */
static block_result_t walk_stmt(sema_t *sema, ast_node_t *stmt,
                                sema_scope_t *scope, size_t *idx) {
  ast_node_t *saved = sema->cur_stmt;
  sema->cur_stmt = stmt;
  block_result_t r = walk_stmt_impl(sema, stmt, scope, idx);
  sema->cur_stmt = saved;
  return r;
}

/* ---- 提升遮蔽预检（局部 type 定义） ----
 *
 * 提升语义下 type RHS 在块入口求值（早于块内 var 绑定到 VM scope），
 * 若 RHS 引用"块内声明序靠前的 var 名"会错误落到外层同名 type/未定义。
 * 入口提升循环按声明序累积 prior_vars，此处递归检查 RHS 的 AST_IDENT
 * 引用（未折叠的名字，折叠产物 AST_TYPE_REF 不在此列）是否命中——
 * 命中即 var 完全遮罩（遮蔽平等），报错并跳过求值。 */

/* 类型 rhs 是否引用了 prior_vars 中的运行期变量名（返回命中的那个名字）。
   基于共享 AST 遍历（parser/ast_walk.h）：谓词只关心标识符名，遍历覆盖
   类型表达式与值表达式的全部子节点（此前手写版漏掉 ?T / *T / []T / 元组
   等子树，导致 `type T = ?[x]i32` 这类遮蔽要到 CTFE 才以另一种消息报出）。 */
typedef struct {
  const strslice_t *prior_vars;
  size_t            prior_n;
  const strslice_t *hit;
} uses_var_probe_t;

static ast_walk_action_t probe_uses_var(ast_node_t *n, void *user) {
  uses_var_probe_t *p = (uses_var_probe_t *)user;
  if (n->kind != AST_IDENT) return AST_WALK_CONTINUE;
  strslice_t name = ((ast_ident_t *)n)->name;
  for (size_t i = 0; i < p->prior_n; i++) {
    if (p->prior_vars[i].len == name.len &&
        memcmp(p->prior_vars[i].ptr, name.ptr, name.len) == 0) {
      p->hit = &p->prior_vars[i];
      return AST_WALK_STOP;
    }
  }
  return AST_WALK_CONTINUE;
}

static const strslice_t *type_rhs_uses_var(const ast_node_t *e,
                                           const strslice_t *prior_vars,
                                           size_t prior_n) {
  if (!e) return NULL;
  uses_var_probe_t p = {prior_vars, prior_n, NULL};
  ast_walk_preorder((ast_node_t *)e, probe_uses_var, &p);
  return p.hit;
}

/* ---- 局部函数签名解析（3b 提升时） ---- */

/* 按定义 AST 节点反查 sema_func_t（局部函数提升时定位 fscope，补全参数
   符号 type）。线性扫描，函数数量少。 */
static sema_func_t *sema_func_find_by_def(sema_t *sema, ast_node_t *def) {
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    if (sf->def == def) return sf;
  }
  return NULL;
}

/* 局部函数签名解析：与 pass2_types 同构（type_func_sig 建签名 →
   sema_type_register 登记 → fn->sig_id 记录），但类型槽位在定义点块作用域
   的 VM 链上解析（局部 type 已由入口提升按声明序绑定，可见）。
   参数符号 type 一并补全（3a 建树时局部类型槽位可能解析失败为 NULL，
   函数体 walk 前就绪——sema_walk_function 参数 shadow 用）。 */
static void resolve_local_func_sig(sema_t *sema, ast_func_def_t *fn,
                                   sema_scope_t *scope) {
  size_t nparams = sema_count_siblings(fn->params);
  const type_t **params = NULL;
  ownership_t *powns = NULL;
  if (nparams > 0) {
    params = allocator_new_ex(sema->vm->alloc, "type_t*", sizeof(type_t *),
                              NULL, NULL, NULL, nparams);
    powns = allocator_new_ex(sema->vm->alloc, "ownership_t", sizeof(ownership_t),
                             NULL, NULL, NULL, nparams);
    size_t j = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, j++) {
      ast_var_def_t *vd = (ast_var_def_t *)p;
      ownership_t pown = sema_type_expr_ownership(vd->type_expr);
      const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, p),
                   "unknown type in parameter '%.*s'",
                   (int)vd->name.len, vd->name.ptr);
      }
      params[j] = t; /* 失败置 NULL，位置对齐，func_shadow_call 校验时跳过 */
      powns[j] = pown;
    }
  }

  ownership_t ret_own = fn->return_expr ? sema_type_expr_ownership(fn->return_expr) : OWN_NONE;
  const type_t *rt = NULL;
  if (fn->return_expr) {
    rt = sema_resolve_type_slot(sema, &fn->return_expr);
    if (!rt) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
                 "unknown return type");
    }
  }
  /* §5 强制规则（m3-design §5）：返回 ref 必标注（集合 = 参数名 ∪ {*}）。 */
  sema_check_ret_ref_annot(sema, fn, rt, ret_own);
  const type_t *sig = type_func_sig(sema->vm, params, powns, nparams, rt, ret_own, false);
  /* 符号 type 填充（调用点/引用点签名校验 + AST_FUNC_REF 改写用） */
  sema_symbol_t *sym = sema_lookup(scope, fn->name);
  if (sym) {
    sym->type = sig;
    sym->ast = (ast_node_t *)fn;
  }
  /* 签名类型登记（hoist 构造 + fn->sig_id：compiler LOAD_TYPE <sig_id>） */
  const sema_type_t *st = sema_type_register(sema, sig);
  if (st) fn->sig_id = st->id;

  /* 参数符号 type 补全（3a 建树时解析失败的局部类型槽位）；
     参数符号在参数层（param_scope，fscope 子） */
  sema_func_t *sf = sema_func_find_by_def(sema, (ast_node_t *)fn);
  if (sf && sf->param_scope) {
    size_t j = 0;
    for (ast_node_t *p = fn->params; p; p = p->next, j++) {
      ast_var_def_t *vd = (ast_var_def_t *)p;
      sema_symbol_t *ps = sema_scope_find_local(sf->param_scope, vd->name);
      if (ps && !ps->type) ps->type = params ? params[j] : NULL;
    }
  }

  if (params) allocator_free(sema->vm->alloc, (void **)&params);
  if (powns) allocator_free(sema->vm->alloc, (void **)&powns);
}

/* ===========================================================================
 * 3a 建树 / 3b 消费的**对位自检**（作用域树索引协议）
 *
 * 协议：3a（stmt_build）用 sema_scope_add_child 按源码序为每个建作用域的语句
 * （block / if 的 then·else / switch 的 case·default / while / do-while /
 * for + body / 函数定义）挂一个子作用域；3b（本文件 walker）用
 * sema_scope_child(scope, (*idx)++) 按同一顺序消费。
 *
 * 两侧一旦失配，此前只会**静默降级**（调用点统一写 `child ? child : scope`）
 * ——表现为符号永不激活、遮罩语义反转、借用计数泄漏等极难定位的错误。
 * 这里在每个块消费结束时校验"消费数 == 子作用域数"，使失配立刻变响。
 *
 * 唯一允许的差异：块内出现"确定返回"之后的不可达尾部——3a 仍按结构建树，
 * 而 3b 提前 break（不可达语句本就不做类型检查），故跳过校验。
 * =========================================================================== */

static void check_scope_consumed(sema_t *sema, sema_scope_t *scope,
                                 ast_node_t *where_node, size_t consumed) {
  size_t have = sema_scope_children_count(scope);
  if (consumed == have) return;
  diag_error(sema->diag, sema_loc(sema, where_node),
             "internal: scope tree desync (consumed %zu of %zu child scopes) "
             "— sema 3a/3b 建树与消费不一致，请报告此缺陷",
             consumed, have);
}

static block_result_t walk_block(sema_t *sema, ast_node_t *block,
                                 sema_scope_t *scope, size_t *idx) {
  block_result_t r = {0};
  ast_block_t *b = (ast_block_t *)block;
  /* 提升：局部 type 定义与局部函数定义在块入口按声明序绑定——名字整个
     块内可见（无 TDZ，前向引用安全，同块局部函数互相调用）。type rhs 求值
     仍按声明序（类型引用依赖前序类型，与全局 pass1b 一致）；求值会折叠
     td->expr 为 AST_TYPE_REF 并激活符号（sema_eval_type_def）。局部函数
     提升解析签名（resolve_local_func_sig：type_func_sig 建签名 → 登记 →
     fn->sig_id + 符号 type 填充）。walk 主循环跳过这两类语句（已处理）。
     prior_vars 累积块内声明序靠前的 var 名：type RHS 引用命中 → var 完全
     遮罩（遮蔽平等），预检报错并跳过求值（不绑定/不激活）。 */
  strslice_t prior_vars[64];
  size_t prior_n = 0;
  for (ast_node_t *s = b->stmts; s; s = s->next) {
    if (s->kind == AST_VAR_DEF) {
      ast_var_def_t *pv = (ast_var_def_t *)s;
      if (pv->is_comptime) {
        /* 局部 comptime var 在提升预扫里按源序求值：同块后面的 type def
           （`type T = [C]i32`）在提升阶段就要用到它的常量值（值同时绑定进
           本块 VM 作用域，常量槽位解析才查得到）。
           comptime var 是**编译期常量**，不是运行期变量——不纳入
           prior_vars 遮蔽预检（否则 `type T = [C]i32` 会被误判为
           "引用变量遮蔽类型名"）。
           守卫：init 引用了靠前的**运行期变量**时**不**提前求值——此刻那些
           变量尚未在定义点激活，提前求值会把诊断退化成 "undefined variable"；
           留给 3b 定义点求值才能给出准确的 "not a compile-time constant"。 */
        bool uses_runtime_var =
            pv->init && type_rhs_uses_var(pv->init, prior_vars, prior_n) != NULL;
        if (!uses_runtime_var) {
          sema_symbol_t *csym = sema_scope_find_local(scope, pv->name);
          if (!csym || !csym->ct_valid)
            sema_eval_comptime_var(sema, pv, scope);
        }
      } else if (prior_n < 64) {
        prior_vars[prior_n++] = pv->name;
      }
    } else if (s->kind == AST_TYPE_DEF) {
      ast_type_def_t *td = (ast_type_def_t *)s;
      const strslice_t *shadow =
          type_rhs_uses_var(td->expr, prior_vars, prior_n);
      if (shadow) {
        diag_error(sema->diag, sema_loc(sema, td->expr),
                   "'%.*s' is a variable, not a type", (int)shadow->len,
                   shadow->ptr);
        continue; /* 遮蔽：跳过求值，符号保持未激活 */
      }
      sema_eval_type_def(sema, td, scope);
    } else if (s->kind == AST_ENUM_DEF) {
      /* 局部 enum 定义提升：与 type def 同构——底层类型是类型槽位（内建
         整型名，非变量可遮蔽），variant 值是值表达式（ctfe 按 vm scope
         查找，局部 var 未绑定报 undefined——天然正确），无需 prior_vars
         遮蔽预检。sema_eval_enum_def 完成：底层校验 + variant 求值 +
         type_enum_intern + 登记 sema->types（hoist 自动构造）+ 绑定 type
         value + 激活符号。 */
      sema_eval_enum_def(sema, (ast_enum_def_t *)s, scope);
    } else if (s->kind == AST_STRUCT_DEF) {
      /* 局部 struct 定义提升：与 type def 同构——字段类型是类型槽位
         （sema_resolve_type_slot，内建/具名类型名，非变量可遮蔽），无需
         prior_vars 遮蔽预检。sema_eval_struct_def 完成：字段类型解析 +
         查重 + type_struct_intern + 登记 sema->types（hoist 自动构造）+
         绑定 type value + 激活符号。 */
      sema_eval_struct_def(sema, (ast_struct_def_t *)s, scope);
    } else if (s->kind == AST_UNION_DEF) {
      /* 局部 union 定义提升：与 struct def 同构——字段类型是类型槽位，
         字段（member）名非变量可遮蔽，无需 prior_vars 遮蔽预检。
         sema_eval_union_def 完成：字段类型解析 + 查重 + type_union_intern
         + 登记 sema->types（hoist 自动构造）+ 绑定 type value + 激活符号。 */
      sema_eval_union_def(sema, (ast_union_def_t *)s, scope);
    } else if (s->kind == AST_CUNION_DEF) {
      /* 局部 cunion 定义提升：与 union def 同构——字段类型是类型槽位，
         字段（member）名非变量可遮蔽，无需 prior_vars 遮蔽预检。
         sema_eval_cunion_def 完成：字段类型解析 + 查重 + type_cunion_intern
         + 登记 sema->types（hoist 自动构造）+ 绑定 type value + 激活符号。 */
      sema_eval_cunion_def(sema, (ast_cunion_def_t *)s, scope);
    } else if (s->kind == AST_FUNC_DEF) {
      ast_func_def_t *fn = (ast_func_def_t *)s;
      /* 统一解析签名（含 comptime：调用点折叠前符号 type 须就绪——
         调用点 !sym->type 会误报 "undefined function"）。
         3a 拒绝（遮蔽全局函数名）的符号未注册 → find_local 为空 → 跳过。 */
      if (!sema_scope_find_local(scope, fn->name)) continue;
      resolve_local_func_sig(sema, fn, scope);
    }
  }
  /* prev 维护：comptime var 定义点求值后从语句链摘除（不进入运行时）。
     var def 不消费子作用域（3a 只注册符号），摘除不影响索引对齐。 */
  ast_node_t **prev = &b->stmts;
  for (ast_node_t *s = b->stmts; s;) {
    if (s->kind == AST_TYPE_DEF || s->kind == AST_ENUM_DEF ||
        s->kind == AST_STRUCT_DEF || s->kind == AST_UNION_DEF ||
        s->kind == AST_CUNION_DEF) {
      /* 入口提升已处理（type def / enum def / struct def / union def /
         cunion def 均无子作用域） */
      prev = &s->next;
      s = s->next;
      continue;
    }
    if (s->kind == AST_FUNC_DEF) {
      /* 入口提升已处理签名；捕获解析（定义点，外层 scope 在线——纯 id 捕获
         类型/flow_init、括号捕获 init 求值与类型校验）在此完成，写回 fscope
         捕获符号（sema_walk_function 读取定义 shadow value）。
         消费 3a 建的 fscope 子作用域（被拒无子作用域）——comptime 局部函数
         同样建树，须一并消费，与 3a 建树严格对齐。 */
      ast_func_def_t *fn = (ast_func_def_t *)s;
      if (sema_scope_find_local(scope, fn->name)) {
        sema_func_t *sf = sema_func_find_by_def(sema, (ast_node_t *)fn);
        resolve_func_captures(sema, fn, sf ? sf->scope : NULL, scope);
        (*idx)++;
      }
      prev = &s->next;
      s = s->next;
      continue;
    }
    block_result_t sr = walk_stmt(sema, s, scope, idx);
    if (sr.definitely_returns) {
      r.definitely_returns = true;
      /* 退出点节点上挂载的 defer calls（desugar_defers 设到 defers 字段）
         仍需 walk（类型检查）——它们不在语句链中，是节点附载的。 */
      for (ast_node_t *d = s->defers; d; d = d->next)
        walk_stmt(sema, d, scope, idx);
      break; /* 后续语句不可达，不再检查 */
    }
    if (s->kind == AST_VAR_DEF && ((ast_var_def_t *)s)->is_comptime) {
      *prev = s->next; /* 摘除定义点 */
      s = s->next;
      continue;
    }
    prev = &s->next;
    s = s->next;
  }
  /* 为本 scope 的 own 变量生成 FREE <name> 语句，追加到块尾语句链末尾。
     仅在块正常退出（非 definitely_returns）时追加——return/break/continue
     路径的 FREE 由 walk_return/walk_break/walk_continue 沿 scope 链收集
     并挂载到退出点节点的 defers 字段。 */
  if (!r.definitely_returns) {
    ast_node_t *free_stmts = make_free_stmts(sema, scope);
    if (free_stmts) {
      if (!b->stmts) {
        b->stmts = free_stmts;
      } else {
        ast_node_t *tail = b->stmts;
        while (tail->next) tail = tail->next;
        tail->next = free_stmts;
      }
      /* walk 生成的 FREE 语句（类型检查：AST_FREE 在 walk_stmt 中无操作） */
      for (ast_node_t *f = free_stmts; f; f = f->next)
        walk_stmt(sema, f, scope, idx);
    }
  }
  /* 3a/3b 对位自检（见 check_scope_consumed 说明）。不可达尾部不校验：
     3a 按结构建树，3b 已提前 break——属预期差异。 */
  if (!r.definitely_returns)
    check_scope_consumed(sema, scope, block, *idx);
  return r;
}

/* ---- 函数入口 ---- */

void sema_walk_function(sema_t *sema, sema_func_t *sf) {
  ast_func_def_t *fn = (ast_func_def_t *)sf->def;
  if (!sf->scope) return; /* 建树失败（结构错误已诊断），不进入 shadow run */

  /* 进入函数 walk：**整体保存**当前函数上下文并整体恢复（含 return_type /
     def / 捕获检查上下文 / comptime 标志 / callee·循环标志）。
     本函数可被嵌套调用（sema_check_func_literal 在外层 walk 中调用它走字面量
     body），逐字段清空 + 部分恢复的写法极易漏字段——整体结构赋值则天然完备。
     上下文类型见 sema_fn_ctx_t。 */
  sema_fn_ctx_t saved = sema_fn_ctx_enter(sema);

  /* 捕获检查上下文：局部函数体引用外层局部符号 → 报错（无闭包，运行时
     函数体查找链只有参数 + 全局）。全局函数不设 local_func_base（fscope
     parent = 全局，无外层局部可捕获），但参数层照设——walk_return 的 §6
     返回 ref 来源检查（参数放行）需要参数层判定。 */
  sema->fn.local_func_base = sf->is_local ? sf->scope : NULL;
  sema->fn.local_func_param_scope = sf->param_scope;

  /* comptime 函数体 walk 标志：body 内对 comptime func 的调用走普通
     shadow 校验（参数是 shadow，不能 CTFE），标志在整个 body walk 期间
     保持。 */
  sema->fn.walking_comptime = fn->is_comptime;

  /* 所有权须在 sema_resolve_type_slot 之前提取——该函数折叠 AST 节点。 */
  sema->fn.return_ownership =
      fn->return_expr ? sema_type_expr_ownership(fn->return_expr) : OWN_NONE;
  sema->fn.return_type =
      fn->return_expr ? sema_resolve_type_slot(sema, &fn->return_expr) : NULL;
  sema->fn.def = fn;

  /* R1（返回类型约束，m3-design §4）：参数/返回值只能 ref 或 fatal，不能
     own——own 是单所有权，跨函数传 own 会破坏唯一所有权（调用方已销毁 /
     返回值无法显式接管）。返回 own *T / own []T 应改写为返回 fatal *T /
     fatal []T（调用点 `var p: own *T = f()` 经 fatal→own 接管）。 */
  if (sema->fn.return_type && sema->fn.return_ownership == OWN_OWN) {
    diag_error(sema->diag, sema_loc(sema, fn->return_expr),
               "function cannot return own pointer/slice (return fatal *T / "
               "fatal []T instead; the caller takes ownership via "
               "var p: own *T = f())");
    sema->fn.return_type = NULL; /* 防级联：后续 return 不再按 own 校验 */
  }

  /* 函数级 VM scope（与作用域树同构）：捕获层（父）定义捕获 shadow value，
     参数层（子，遮蔽捕获）定义参数 shadow value——进入函数体即可读（名字
     取自 ast）。与运行时 func_vcall 结构一致：closure_scope（捕获）→
     参数匿名层（子）。 */
  vm_push_scope(sema->vm); /* 捕获层 */
  for (ast_node_t *c = fn->captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(sf->scope, cv->name);
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
    sema_symbol_t *ps = sema_scope_find_local(sf->param_scope, vd->name);
    if (ps) {
      ps->flow_init = true; /* 参数由调用方传入，确定已初始化 */
      ps->is_active = true; /* 参数进入函数体立即可见 */
      /* **参数位只允许 `own` / `ref` / none**（所有权模型 §0.5）。
         - `own` 形参：调用点必须以将亡值（FATAL）传入（`f(move(p))`）。
           接收后**形参的值状态是 OWN**，由函数负责释放（或 move 出去、
           或随 `fatal` 返回位离开）——不会与调用方分裂所有权，因为调用点的
           源已被 move 进 TDZ。
         - `fatal` **不能作形参**：`fatal` 描述的是"函数调用表达式"的值
           （见 §0.3），不是形参的接收能力。传所有权请写 `own`。 */
      if (ps->type && ps->ownership == OWN_FATAL) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
                   "function parameter '%.*s' cannot be 'fatal': fatal describes "
                   "the value of a *call expression*, not a parameter. Use "
                   "'own' to take ownership (call with `f(move(x))`) or 'ref' "
                   "to borrow",
                   (int)vd->name.len, vd->name.ptr);
      }
      /* 注：§5 强制规则（参数类型**含 ref 字段**必标注）现无对象——含 ref 字段
         的聚合类型暂不支持（复合类型禁止携带指针/切片）。**顶层 `ref *T` /
         `ref []T` 是"借用"本身的声明形式**，不在强制标注之列
         （其存活由调用点单源推断承担）。 */
    }
    value_t *pv = value_make_shadow(sema->vm,
                                    ps && ps->type ? ps->type
                                                   : sema->vm->type_void);
    char nb[256];
    name_to_cstr(vd->name, nb, sizeof nb);
    scope_define(sema->vm, sema->vm->current_scope, nb, pv);
  }

  /* 闭包调用上下文（隐藏局部）：`__ctx` = 当前被调用的 func 值。捕获读取在
     ex_ident 里改写为 `__ctx.env.<cap>`（捕获聚合结构体，与 C 后端的
     `ctx->env.cap` 同构）。符号注册在参数层——函数体可见、捕获/参数同名时
     参数先行遮蔽（与捕获按名遮蔽的既有语义一致）。 */
  if (fn->capture_type_id != 0 && fn->sig_id != 0) {
    const sema_type_t *sig_st = sema_type_by_id(sema, fn->sig_id);
    const type_t *sig_type = sig_st ? sig_st->type : NULL;
    if (sig_type) {
      sema_symbol_t ctxsym;
      memset(&ctxsym, 0, sizeof ctxsym);
      ctxsym.kind       = SEMA_SYM_VAR;
      ctxsym.type       = sig_type;
      ctxsym.ownership  = OWN_REF;   /* 借用：ctx 是调用方传进来的 func 值 */
      ctxsym.flow_init  = true;
      ctxsym.is_active  = true;
      ctxsym.own_scope  = sf->param_scope;
      sema_scope_define(sf->param_scope, strslice_from_bytes("__ctx", 5),
                        &ctxsym);
      value_t *csv = value_make_shadow(sema->vm, sig_type);
      scope_define(sema->vm, sema->vm->current_scope, "__ctx", csv);
    }
  }

  /* 返回路径完整性分析已在 Pass 3a（建树阶段）完成；
     walk_block 的 block_result_t 仅用于跳过不可达语句的类型检查 */
  size_t child_idx = 0;
  (void)walk_block(sema, fn->body, sf->param_scope, &child_idx);

  /* R1（fatal 参数消费，m3-design §4）：fatal 参数必须被消费——move 接管
     （AST_MOVE 标记 moved）或 return 出去（walk_return 标记 moved）。
     未消费（函数结束时 fatal 仍"存活"）→ 所有权悬空 → 编译错误。 */
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    sema_symbol_t *ps = sema_scope_find_local(sf->param_scope, vd->name);
    if (ps && sema_sym_is_fatal(ps) && !sema_fatal_consumed(ps)) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)vd),
                 "fatal parameter '%.*s' is not consumed (take ownership "
                 "with move() or return it)",
                 (int)vd->name.len, vd->name.ptr);
    }
  }

  sema_scope_release_borrows(sf->param_scope); /* R3：参数层借用载体消亡 */
  vm_pop_scope(sema->vm);
  sema_scope_release_borrows(sf->scope);       /* R3：捕获层借用载体消亡 */
  vm_pop_scope(sema->vm);

  /* 返回路径完整性分析已在 Pass 3a（建树阶段）完成。整体恢复进入函数 walk
     前的上下文（见函数入口的 saved_fn_ctx）——不再逐字段清空/部分恢复。 */
  sema_fn_ctx_leave(sema, saved);
}

/* 函数字面量 body walk 入口（stmt_build.c 的 sema_check_func_literal 用；
   walk_block 是 static，此处暴露薄封装）。idx 内部自持——临时作用域树
   由 sema_check_func_literal 独立构建，消费顺序与建树严格对齐。 */
void sema_walk_block(sema_t *sema, ast_node_t *block, sema_scope_t *scope) {
  size_t child_idx = 0;
  (void)walk_block(sema, block, scope, &child_idx);
}
