#include "sema/sema.h"
#include "vm/type_ownership.h"
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
#include "parser/ast_ptr.h"
#include "parser/ast_slice_type.h"
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
#include "core/ffi_abi.h"          /* M6 FFI：ffi_data_kind_of（数据符号类型码） */
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

#include "sema_stmt_internal.h"

static ast_node_t *destroy_fn_callee_node(sema_t *sema, const sema_destroy_fn_t *reg);
bool gen_destroy(sema_t *sema, const type_t *t, ast_node_t *access, ast_node_t **head, ast_node_t **tail);
static bool gen_destroy_impl(sema_t *sema, const type_t *t, ast_node_t *access, ast_node_t **head, ast_node_t **tail);

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

static bool type_needs_destroy_fn(const type_t *t) {
  if (!t) return false;
  const type_t *s = type_strip_quals(t);
  if (!s) return false;
  if (type_kind_of(s) == TYPE_KIND_PTR || type_kind_of(s) == TYPE_KIND_SLICE) return false;
  return type_has_own_slots(s);
}

/* 注册表查找：类型对象指针（intern 后唯一）→ 已生成的销毁函数。 */
static sema_destroy_fn_t *destroy_fn_find(sema_t *sema, const type_t *t) {
  for (sema_destroy_fn_t *f = sema->destroy_fns; f; f = f->next)
    if (f->type == t) return f;
  return NULL;
}

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
  const type_t *pt0 = type_ptr_intern(sema->vm, s);
  if (!pt0) return NULL;
  /* 形参是借用：`ref *T`（所有权进类型）。 */
  const type_t *pt = type_ownership_apply(sema->vm, OWN_REF, pt0);
  if (!pt) return NULL;
  ast_node_t *pnode = ast_var_def_new(sema->arena, 0, 0);
  ast_node_t *ptexpr = sema_gen_type_ref(sema, pt); /* 登记 *T + AST_TYPE_REF */
  if (!pnode || !ptexpr) return NULL;
  ((ast_var_def_t *)pnode)->name = strslice_from_bytes("dv", 2);
  ((ast_var_def_t *)pnode)->type_expr = ptexpr;
  fn->params = pnode;

  /* 签名 void(ref *T)：登记进 sema->types（hoist 两遍扫描构造），记 sig_id */
  const type_t *sigparams[1] = {pt};
  const type_t *sig = type_func_sig(sema->vm, sigparams, 1,
                                    sema->vm->type_void, false);
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
  if (!sema || !env || type_kind_of(env) != TYPE_KIND_STRUCT) return false;

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
        if (ft && (type_kind_of(ft) == TYPE_KIND_PTR || type_kind_of(ft) == TYPE_KIND_SLICE) &&
            type_ownership_of(f->type) == OWN_OWN)
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

bool sema_find_destroy_fn(sema_t *sema, const type_t *t, uint32_t *fid,
                          strslice_t *name) {
  if (fid) *fid = 0;
  if (name) *name = STRSLICE_EMPTY;
  if (!sema || !t) return false;
  const type_t *s = type_strip_quals(t);
  if (!s) return false;
  sema_destroy_fn_t *reg = destroy_fn_find(sema, s);
  if (!reg) return false;
  const ast_func_def_t *fd = (const ast_func_def_t *)reg->fn;
  if (fid) *fid = fd ? fd->fid : 0;
  if (name) *name = reg->name;
  return true;
}

bool sema_ensure_destroy_fn(sema_t *sema, const type_t *t, strslice_t *name) {
    if (!sema || !t) return false;
    const type_t *s = type_strip_quals(t);
    if (!s) return false;
    if (!destroy_fn_find(sema, s)) {
        /* 生成并登记（函数体在 sema_attach_destroy_fns 收尾时追加到 prog）。 */
        if (!destroy_fn_callee(sema, s)) return false;
    }
    return sema_find_destroy_fn(sema, s, NULL, name);
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
    bool leaf = pt && (type_kind_of(pt) == TYPE_KIND_PTR || type_kind_of(pt) == TYPE_KIND_SLICE);
    if (leaf && type_ownership_of(m->payload_type) != OWN_OWN)
      continue;   /* ref/fatal 槽位不释放 */

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
bool gen_destroy(sema_t *sema, const type_t *t, ast_node_t *access,
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
    if (ft && (type_kind_of(ft) == TYPE_KIND_PTR || type_kind_of(ft) == TYPE_KIND_SLICE)) {
      /* 叶子：只有 own 槽位才释放。**必须递归进被指向值**（PTR/SLICE 分支会
         展开被指向聚合的 own 槽位 / 切片元素），否则 `struct Outer { b: own
         *Box }` 只 FREE 掉 Box 块，Box.p 泄漏（实测 leak）。 */
      if (type_ownership_of(f->type) != OWN_OWN) continue;
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
strslice_t discard_name(sema_t *sema, const char *s) {
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
bool gen_destroy_slot(sema_t *sema, const type_t *slot_type,
                             ownership_t slot_own, ast_node_t *access,
                             ast_node_t **head, ast_node_t **tail) {
  if (!slot_type || !access) return true;
  const type_t *s = type_strip_quals(slot_type);
  if (s && (type_kind_of(s) == TYPE_KIND_PTR || type_kind_of(s) == TYPE_KIND_SLICE)) {
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
  bool leaf = es && (type_kind_of(es) == TYPE_KIND_PTR || type_kind_of(es) == TYPE_KIND_SLICE);
  if (leaf && slot_own != OWN_OWN) return true;

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
        if (bs && (type_kind_of(bs) == TYPE_KIND_PTR || type_kind_of(bs) == TYPE_KIND_SLICE)) {
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
                             type_ownership_of(slice_type_elem(t)), access, var_name,
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
        if (!gen_destroy_slot(sema, et, type_ownership_of(tt->elems[i].type), elem, head,
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
      if (!gen_destroy_elems(sema, at->elem_type,
                            type_ownership_of(at->elem_type), access,
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
      ownership_t inner_own = type_ownership_of(ot->inner);
      const type_t *is = type_strip_quals(inner);
      bool leaf = is && (type_kind_of(is) == TYPE_KIND_PTR || type_kind_of(is) == TYPE_KIND_SLICE);
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

ast_node_t *make_free_stmts(sema_t *sema, sema_scope_t *scope) {
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
    /* 仅 **own** 变量需要显式释放。判据用符号层的 `sema_sym_ownership(sym)`
       （所有权已从类型层迁移到值/符号层——类型不携带 ownership，
       所以**不能**用类型层的谓词判断）。
       moved 变量已被转移（TDZ），不需要释放。
       comptime 变量不进入运行时，不需要释放。 */
    if (sym->moved || sym->is_comptime) continue;
    /* 定义点尚未走到（提前 return/break 的路径）→ 该变量在运行时还不存在，
       不能生成销毁（否则 PUSH 一个尚未 DEFINE 的名字 → undefined variable）。 */
    if (!sym->declared) continue;
    /* own 变量必销毁；**含 share/weak 槽位的普通变量**也要销毁（递减计数）。 */
    if (sema_sym_ownership(sym) != OWN_OWN) continue;
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
ast_node_t *collect_free_stmts(sema_t *sema, sema_scope_t *from_scope,
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
const sema_scope_t *find_func_stop_sema(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FUNCTION) return s->parent;
  }
  return NULL;
}

/* 找到最近循环 scope 的 parent（break/continue 的 FREE 收集范围 stop）。 */
const sema_scope_t *find_loop_stop_sema(sema_scope_t *scope) {
  for (sema_scope_t *s = scope; s; s = s->parent) {
    if (s->kind == SEMA_SCOPE_FOR) return s->parent;
  }
  return NULL;
}
