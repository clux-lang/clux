#include "sema/sema.h"
#include "sema/reflect.h"
#include "vm/type_ownership.h"
#include "sema/struct_compose.h" /* 嵌入继承的成员名字查找 */
#include "core/string.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_return.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_error.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_spread.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_member.h"
#include "parser/ast_move.h"
#include "parser/ast_new.h"
#include "parser/ast_deref.h"
#include "parser/ast_addr.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_slice.h"
#include "parser/ast_make.h"
#include "parser/ast_nil.h"
#include "parser/ast_option.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_typeof.h"
#include "parser/ast_alignof.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_unary.h"
#include "parser/ast_undef.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_volatile.h"
#include "parser/ast_assign.h"
#include "parser/ast_var_def.h"
#include "parser/ast_block.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_clone.h"
#include "parser/ast_take.h"
#include "parser/ast_take_slot.h"
#include "parser/ast_for_index.h"
#include "parser/lexer.h"
#include "ctfe/ctfe.h"
#include "sema/comptime.h"
#include "vm/function.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_error.h"
#include "vm/type_func.h"
#include "vm/type_cfunc.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_int.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_opaque.h"
#include "vm/module.h"
#include "core/ffi_abi.h" /* M6 FFI：ffi_data_kind_of（extern var 的类型码） */
#include "core/ffi_abi.h"   /* M6 FFI：CLUX_FFI_RET_*（extern cfunc 的返回类型码） */
#include "core/strmap.h"

#include <stdio.h>
#include <string.h>


#include "sema_expr_internal.h"

static ast_node_t *gen_clone_slice(sema_t *sema, sema_scope_t *scope, const type_t *t, ast_node_t *access);
static ast_node_t *gen_clone_expr_impl(sema_t *sema, sema_scope_t *scope, const type_t *t, ast_node_t *access, ownership_t slot_own, bool top_level);
static bool gen_clone_slots(sema_t *sema, sema_scope_t *scope, const type_t *t, ast_node_t *access, ast_node_t **head, ast_node_t **tail);


/* ==== 生成的 clone 函数（与销毁函数同构，2026-10）====
 *
 * clone() 调用点只发 `CALL __clone*_<n>(...)`；函数体内部按槽位展开，嵌套
 * 复合/指针/切片槽位再调用对应函数 —— 自引用类型（链表/树）由函数自身递归，
 * 天然终止（不再依赖运行期 CLONE 兜底）。运行时递归是合理的，扁平化留给未来。
 *
 * 三种形态（key 是去限定后的类型）：
 *   kind 0 `__clone_<n>(dv: ref *T): fatal T`        —— 值克隆（T 为复合值类型）
 *   kind 1 `__cloneptr_<n>(dv: ref *U): fatal *U`    —— 指针克隆（新块 + 克隆被指向值）
 *   kind 2 `__cloneslice_<n>(dv: ref []T): fatal []T`—— 切片克隆（循环逐元素 clone）
 */

/* ===========================================================================
 * 扁平化 clone 生成（2026-10）
 *
 * `clone(x)` **不再**交给运行期按类型递归（`CLONE` 指令 → ptr_clone_block），
 * 而是由 sema 在编译期展开成显式的「申请 + 赋值」序列——与销毁序列
 * （`gen_destroy`，stmt.c）对称：
 *
 *   | 类型                | 生成的 AST                                            |
 *   |---------------------|-------------------------------------------------------|
 *   | 无 own 槽位（标量/cunion/无 own 字段的聚合） | 值本身（普通拷贝）          |
 *   | 叶子指针/slice（内容无 own 槽位） | `AST_CLONE` → `CLONE`：申请新块 + 拷贝字节 |
 *   | struct / tuple / [N]T（含 own 槽位） | `.T{ <逐槽位 clone/拷贝> }`（构造 = 申请 + 赋值） |
 *   | ?T（inner 含 own 槽位） | `(v != nil) ? .?T{ clone(v.!) } : .?T{ nil }`（三元） |
 *   | union（含 own 槽位）  | `(v is m1) ? .U{ .m1 = clone(v.m1) } : …`（三元链） |
 *   | own *U（U 需深拷贝）  | `new U{ <逐槽位 clone/拷贝> }` / `new ?U{…}` / `new U{.m=…}` |
 *   | []T（own 元素）       | prelude：`var __cl_N: []T = make(T, len(v))` + `for i … { __cl_N[i] = … }`，
 *                           表达式 = `AST_TAKE(__cl_N)`                       |
 *
 * 深拷贝的**槽位规则与销毁完全一致**（type 对象是唯一权威）：
 *   - 叶子（ptr / slice）：槽位所有权 == own 才深拷贝（ref 借用只拷贝指针值）；
 *   - 复合（struct/tuple/array/union）：含 own 槽位就递归展开（与槽位 ownership
 *     无关——其内部 own 槽位归外层值所有）。
 *
 * 运行期只剩 CLONE / NEW / CONSTRUCT / MAKE 这些「申请 + 拷贝」原语，没有按
 * 类型递归的辅助函数——C 后端可以逐条等价翻译（这正是扁平化的目的）。
 * 这些节点全部带 `is_generated`：Pass 3b 不回走，只由 compiler 消费。
 * =========================================================================== */

/* arena 持久化 NUL 串 → strslice */
static strslice_t clone_cstr(sema_t *sema, const char *s) {
  size_t n = strlen(s);
  char *buf = (char *)arena_calloc(sema->arena, n + 1, 1,
                                   ALIGNOF(max_align_t));
  if (!buf) return STRSLICE_EMPTY;
  memcpy(buf, s, n);
  return strslice_from_bytes(buf, n);
}

/* 类型引用表达式：登记类型 → AST_TYPE_REF <__type_N>；内建标量走规范名
   （compiler 的 AST_TYPE_REF 分支对未登记名用 type_lookup 兜底）。 */
ast_node_t *sema_gen_type_ref(sema_t *sema, const type_t *t) {
  if (!t) return NULL;
  ast_node_t *n = ast_type_ref_new(sema->arena, 0, 0);
  if (!n) return NULL;
  n->is_generated = true;
  const sema_type_t *st = sema_type_register(sema, t);
  if (st) {
    ((ast_type_ref_t *)n)->name = st->name;
  } else {
    char buf[128];
    sema_type_name(t, buf, sizeof buf);
    ((ast_type_ref_t *)n)->name = clone_cstr(sema, buf);
  }
  return n;
}

static ast_node_t *clone_ident(sema_t *sema, strslice_t name) {
  ast_node_t *n = ast_ident_new(sema->arena, 0, 0);
  if (!n) return NULL;
  n->is_generated = true;
  ((ast_ident_t *)n)->name = name;
  return n;
}

static ast_node_t *clone_int_lit(sema_t *sema, uint64_t v) {
  ast_node_t *n = ast_int_lit_new(sema->arena, 0, 0);
  if (!n) return NULL;
  n->is_generated = true;
  ((ast_int_lit_t *)n)->value = v;
  return n;
}

/* <obj>[<idx>] 下标访问（indices 单元素链） */
static ast_node_t *clone_index(sema_t *sema, ast_node_t *obj,
                               ast_node_t *idx) {
  ast_node_t *n = ast_index_new(sema->arena, 0, 0);
  if (!n || !obj || !idx) return NULL;
  n->is_generated = true;
  ((ast_index_t *)n)->object       = obj;
  ((ast_index_t *)n)->indices      = idx;
  ((ast_index_t *)n)->indices_last = idx;
  return n;
}

static ast_node_t *clone_member(sema_t *sema, ast_node_t *obj,
                                strslice_t field) {
  ast_node_t *n = ast_member_new(sema->arena, 0, 0);
  if (!n || !obj) return NULL;
  n->is_generated = true;
  ((ast_member_t *)n)->object = obj;
  ((ast_member_t *)n)->field  = field;
  return n;
}

static ast_node_t *clone_deref(sema_t *sema, ast_node_t *obj) {
  ast_node_t *n = ast_deref_new(sema->arena, 0, 0);
  if (!n || !obj) return NULL;
  n->is_generated = true;
  ((ast_deref_t *)n)->operand = obj;
  return n;
}

static ast_node_t *clone_unwrap(sema_t *sema, ast_node_t *obj) {
  ast_node_t *n = ast_unwrap_new(sema->arena, 0, 0);
  if (!n || !obj) return NULL;
  n->is_generated = true;
  ((ast_unwrap_t *)n)->operand = obj;
  return n;
}

static ast_node_t *clone_is_tag(sema_t *sema, ast_node_t *obj, uint32_t tag) {
  ast_node_t *n = ast_is_tag_new(sema->arena, 0, 0);
  if (!n || !obj) return NULL;
  n->is_generated = true;
  ((ast_is_tag_t *)n)->object = obj;
  ((ast_is_tag_t *)n)->tag    = tag;
  return n;
}

static ast_node_t *clone_opt_is_some(sema_t *sema, ast_node_t *obj) {
  ast_node_t *n = ast_opt_is_some_new(sema->arena, 0, 0);
  if (!n || !obj) return NULL;
  ((ast_opt_is_some_t *)n)->object    = obj;
  ((ast_opt_is_some_t *)n)->want_some = true;  /* 源码 `v != nil` */
  return n;
}

/* len(<obj>)：内建 `len` 是运行期函数值（PUSH "len"; CALL 1），生成代码同样走
   普通调用——与用户写 `len(x)` 逐字节一致。 */
static ast_node_t *clone_len_call(sema_t *sema, ast_node_t *obj) {
  ast_node_t *call = ast_call_new(sema->arena, 0, 0);
  ast_node_t *callee = clone_ident(sema, STRSLICE_LIT("len"));
  ast_node_t *arg = obj;
  if (!call || !callee || !arg) return NULL;
  call->is_generated = true;
  ((ast_call_t *)call)->callee    = callee;
  ((ast_call_t *)call)->args      = arg;
  ((ast_call_t *)call)->args_last = arg;
  arg->next = NULL;
  return call;
}

static ast_node_t *clone_ternary(sema_t *sema, ast_node_t *cond,
                                 ast_node_t *then_b, ast_node_t *else_b) {
  ast_node_t *n = ast_ternary_new(sema->arena, 0, 0);
  if (!n || !cond || !then_b || !else_b) return NULL;
  n->is_generated = true;
  ((ast_ternary_t *)n)->cond        = cond;
  ((ast_ternary_t *)n)->then_branch = then_b;
  ((ast_ternary_t *)n)->else_branch = else_b;
  return n;
}

/* 单个具名构造字段（union member 构造用） */
static ast_node_t *clone_field(sema_t *sema, strslice_t name,
                               ast_node_t *value) {
  ast_node_t *n = ast_construct_field_new(sema->arena, 0, 0);
  if (!n || !value) return NULL;
  n->is_generated = true;
  ((ast_construct_field_t *)n)->name  = name;
  ((ast_construct_field_t *)n)->value = value;
  return n;
}

/* 构造"结果值"：as_new ? `new T{ fields }`（→ fatal *T）: `.T{ fields }`。 */
static ast_node_t *clone_construct(sema_t *sema, const type_t *t,
                                   ast_node_t *fields, uint32_t member_tag,
                                   bool as_new) {
  if (!t) return NULL;
  ast_node_t *n = as_new ? ast_new_new(sema->arena, 0, 0)
                         : ast_construct_new(sema->arena, 0, 0);
  if (!n) return NULL;
  n->is_generated = true;
  if (as_new) {
    ast_new_t *c = (ast_new_t *)n;
    c->type        = sema_gen_type_ref(sema, t);
    c->fields      = fields;
    c->fields_last = fields;
    c->member_tag  = member_tag;
    if (!c->type) return NULL;
  } else {
    ast_construct_t *c = (ast_construct_t *)n;
    c->type        = sema_gen_type_ref(sema, t);
    c->fields      = fields;
    c->fields_last = fields;
    c->member_tag  = member_tag;
    if (!c->type) return NULL;
  }
  return n;
}

/* 该**槽位/值**是否需要深拷贝（申请新块）——与销毁序列的槽位规则一致：
   叶子（ptr/slice）看槽位所有权（顶层 clone 恒深拷贝，与旧 CLONE 语义一致）；
   复合看是否含 own 槽位。 */
static bool clone_needs_deep(const type_t *t, ownership_t slot_own,
                             bool top_level) {
  if (!t) return false;
  const type_t *s = type_strip_quals(t);
  switch (s->kind) {
    case TYPE_KIND_PTR:
    case TYPE_KIND_SLICE:
      return top_level || slot_own == OWN_OWN;
    case TYPE_KIND_FUNC:
      /* 闭包 = 捕获聚合结构体：own 槽位（含 own 捕获）必须深拷贝 env；
         ref/none 槽位是借用/共享引用 → 普通拷贝。字段未标 own、但捕获里
         含 own 资源时同样是"拥有 env"，必须深拷贝。 */
      if (top_level || slot_own == OWN_OWN) return true;
      {
        size_t nc = func_type_capture_count(s);
        for (size_t i = 0; i < nc; i++) {
          const type_t *ct = func_type_capture_type(s, i);
          if (!ct) continue;
          if (type_ownership_of(ct) == OWN_OWN ||
              type_has_own_slots(type_strip_ownership(ct)))
            return true;
        }
      }
      return false;
    default:
      return type_has_own_slots(t);
  }
}

/* 把生成语句追加到当前语句的 prelude（compiler 在语句主体前编译）。
   无语句上下文（comptime 折叠）返回 false —— 调用方回退到运行期 CLONE。 */
bool sema_append_prelude(sema_t *sema, ast_node_t *stmts) {
  if (!stmts) return false;
  ast_node_t *stmt = sema->cur_stmt;
  if (!stmt) return false;
  if (!stmt->prelude) {
    stmt->prelude = stmts;
    return true;
  }
  ast_node_t *tail = stmt->prelude;
  while (tail->next) tail = tail->next;
  tail->next = stmts;
  return true;
}

/* 调用点用的 callee 节点（每个调用点独立节点，不与注册表共享）。 */
static ast_node_t *clone_fn_callee_node(sema_t *sema,
                                        const sema_clone_fn_t *reg) {
  ast_node_t *callee = ast_func_ref_new(sema->arena, 0, 0);
  if (!callee) return NULL;
  ((ast_func_ref_t *)callee)->fid = ((const ast_func_def_t *)reg->fn)->fid;
  ((ast_func_ref_t *)callee)->name = reg->name;
  return callee;
}

/* 取（必要时**生成**）clone 函数，返回 callee。生成顺序与销毁函数一致：
 * **先登记再生成 body**，自引用类型在生成 body 时命中注册表 → 递归终止。 */
static ast_node_t *clone_fn_callee(sema_t *sema, uint8_t kind,
                                   const type_t *key) {
  const type_t *k = type_strip_quals(key);
  if (!k) return NULL;
  for (sema_clone_fn_t *f = sema->clone_fns; f; f = f->next) {
    if (f->kind == kind && f->type == k) return clone_fn_callee_node(sema, f);
  }

  const char *prefix = kind == 0   ? "__clone_"
                       : kind == 1 ? "__cloneptr_"
                                   : "__cloneslice_";
  char nb[40];
  int nl = snprintf(nb, sizeof(nb), "%s%u", prefix, sema->clone_fn_seq++);
  if (nl <= 0 || (size_t)nl >= sizeof(nb)) return NULL;
  char *name_buf = (char *)arena_calloc(sema->arena, (size_t)nl + 1, 1,
                                        ALIGNOF(max_align_t));
  if (!name_buf) return NULL;
  memcpy(name_buf, nb, (size_t)nl);
  strslice_t name = strslice_from_bytes(name_buf, (size_t)nl);

  /* 形参/返回类型：
       kind 0 → 形参 *T，返回 T
       kind 1 → 形参 *U，返回 *U
       kind 2 → 形参 []T，返回 []T */
  const type_t *pt0 = (kind == 2) ? k : type_ptr_intern(sema->vm, k);
  if (!pt0) return NULL;
  /* 形参是借用（ref）、返回是将亡值（fatal）：所有权写进类型。 */
  const type_t *pt = type_ownership_apply(sema->vm, OWN_REF, pt0);
  if (!pt) return NULL;
  const type_t *ret0 = (kind == 0) ? k : pt0;
  const type_t *ret = type_ownership_apply(sema->vm, OWN_FATAL, ret0);
  if (!ret) return NULL;

  ast_func_def_t *fn = (ast_func_def_t *)arena_calloc(
      sema->arena, 1, sizeof(ast_func_def_t), ALIGNOF(max_align_t));
  if (!fn) return NULL;
  fn->base.kind = AST_FUNC_DEF;
  fn->base.is_generated = true;
  fn->name = name;
  sema_func_id_alloc(sema, fn);
  if (fn->fid == 0) return NULL;

  ast_node_t *pnode = ast_var_def_new(sema->arena, 0, 0);
  ast_node_t *ptexpr = sema_gen_type_ref(sema, pt);
  if (!pnode || !ptexpr) return NULL;
  ((ast_var_def_t *)pnode)->name = strslice_from_bytes("dv", 2);
  ((ast_var_def_t *)pnode)->type_expr = ptexpr;
  fn->params = pnode;

  const type_t *sigparams[1] = {pt};
  const type_t *sig = type_func_sig(sema->vm, sigparams, 1, ret, false);
  if (!sig) return NULL;
  const sema_type_t *st = sema_type_register(sema, sig);
  if (st) fn->sig_id = st->id;

  ast_node_t *blk = ast_block_new(sema->arena, 0, 0);
  if (!blk) return NULL;
  blk->is_generated = true;
  fn->body = blk;

  /* **先登记再生成 body**（自引用类型递归在此终止） */
  sema_clone_fn_t *wrap = (sema_clone_fn_t *)arena_calloc(
      sema->arena, 1, sizeof(sema_clone_fn_t), ALIGNOF(max_align_t));
  if (!wrap) return NULL;
  wrap->type = k;
  wrap->kind = kind;
  wrap->fn = (ast_node_t *)fn;
  wrap->name = name;
  wrap->next = sema->clone_fns;
  sema->clone_fns = wrap;

  /* body：`return <clone 表达式>` —— 用 impl（槽位级展开；嵌套槽位再发 CALL）。
     生成期间把 cur_stmt 指到 return 语句，切片 prelude（make + 循环）才有的挂。 */
  ast_node_t *vid = ast_ident_new(sema->arena, 0, 0);
  ast_node_t *ret_stmt = ast_return_new(sema->arena, 0, 0);
  if (!vid || !ret_stmt) return NULL;
  vid->is_generated = true;
  ((ast_ident_t *)vid)->name = ((ast_var_def_t *)pnode)->name;

  ast_node_t *prev_stmt = sema->cur_stmt;
  sema->cur_stmt = ret_stmt;
  ast_node_t *expr = NULL;
  if (kind == 0) {
    ast_node_t *deref = ast_deref_new(sema->arena, 0, 0);
    if (deref) ((ast_deref_t *)deref)->operand = vid;
    expr = deref ? gen_clone_expr_impl(sema, sema->global_scope, k, deref,
                                       OWN_OWN, true)
                 : NULL;
  } else if (kind == 1) {
    /* 形参就是 *U 指针值；impl 的 PTR 分支按 `new U{…}` 生成 */
    expr = gen_clone_expr_impl(sema, sema->global_scope, pt, vid, OWN_OWN, true);
  } else {
    expr = gen_clone_slice(sema, sema->global_scope, k, vid);
  }
  sema->cur_stmt = prev_stmt;
  if (!expr) return NULL;
  ((ast_return_t *)ret_stmt)->value = expr;
  ret_stmt->is_generated = true;
  ((ast_block_t *)blk)->stmts = ret_stmt;
  ((ast_block_t *)blk)->stmts_last = ret_stmt;

  return clone_fn_callee_node(sema, wrap);
}

/* 调用 clone 函数：by_address = true → 实参 `&access`（值）；false → 实参就是
   access（它本身已经是指针/切片值）。 */
static ast_node_t *gen_clone_call(sema_t *sema, uint8_t kind, const type_t *key,
                                  ast_node_t *access, bool by_address) {
  if (!access) return NULL;
  ast_node_t *callee = clone_fn_callee(sema, kind, key);
  if (!callee) return NULL;
  ast_node_t *arg = access;
  if (by_address) {
    arg = ast_addr_new(sema->arena, 0, 0);
    if (!arg) return NULL;
    ((ast_addr_t *)arg)->operand = access;
  }
  ast_node_t *call = ast_call_new(sema->arena, 0, 0);
  if (!call) return NULL;
  ((ast_call_t *)call)->callee = callee;
  ((ast_call_t *)call)->args = arg;
  call->is_generated = true;
  return call;
}

/* 取/生成类型 t 的 clone 函数并返回其 fid（供闭包 env vtable 使用）。 */
uint32_t sema_gen_clone_fn_fid(sema_t *sema, uint8_t kind, const type_t *t) {
  if (!sema || !t) return 0;
  ast_node_t *callee = clone_fn_callee(sema, kind, t);
  if (!callee) return 0;
  return ((const ast_func_ref_t *)callee)->fid;
}

bool sema_find_clone_fn(sema_t *sema, uint8_t kind, const type_t *t,
                        uint32_t *fid, strslice_t *name) {
  if (fid) *fid = 0;
  if (name) *name = STRSLICE_EMPTY;
  if (!sema || !t) return false;
  const type_t *k = type_strip_quals(t);
  if (!k) return false;
  for (sema_clone_fn_t *f = sema->clone_fns; f; f = f->next) {
    if (f->kind != kind || f->type != k) continue;
    const ast_func_def_t *fd = (const ast_func_def_t *)f->fn;
    if (fid) *fid = fd ? fd->fid : 0;
    if (name) *name = f->name;
    return true;
  }
  return false;
}

/* 收尾：把生成的 clone 函数追加到程序函数链尾部（sema_analyze 调用）。 */
void sema_attach_clone_fns(sema_t *sema, ast_node_t *program) {
  if (!sema || !program || program->kind != AST_PROGRAM) return;
  ast_program_t *prog = (ast_program_t *)program;
  for (sema_clone_fn_t *f = sema->clone_fns; f; f = f->next) {
    if (!f->fn) continue;
    f->fn->next = NULL;
    if (prog->funcs_last) prog->funcs_last->next = f->fn;
    else prog->funcs = f->fn;
    prog->funcs_last = f->fn;
  }
}

/* `__deep_clone(v)` 调用节点：运行期对象图深拷贝（闭包 env 的唯一入口）。 */
static ast_node_t *gen_deep_clone_call(sema_t *sema, ast_node_t *access) {
  if (!sema || !access) return NULL;
  ast_node_t *callee = ast_ident_new(sema->arena, 0, 0);
  ast_node_t *call = ast_call_new(sema->arena, 0, 0);
  if (!callee || !call) return NULL;
  ((ast_ident_t *)callee)->name = strslice_from_bytes("__deep_clone", 12);
  callee->is_generated = true;
  ((ast_call_t *)call)->callee = callee;
  ((ast_call_t *)call)->args = access;
  call->is_generated = true;
  return call;
}

/* 公共入口：生成 `__deep_clone(v)`（闭包值拷贝/运行期深拷贝）。
   用于 sema 在"闭包值拷贝 = 深拷贝 env"处插入（字面量不需要）。 */
ast_node_t *sema_gen_deep_clone(sema_t *sema, ast_node_t *value) {
  return gen_deep_clone_call(sema, value);
}

/* **调用入口**（销毁点/槽位/`clone()` 共用）：需要深拷贝时发 CALL 到生成的
 * clone 函数，否则返回 access（普通拷贝）。 */
static ast_node_t *gen_clone_expr(sema_t *sema, sema_scope_t *scope,
                                  const type_t *t, ast_node_t *access,
                                  ownership_t slot_own, bool top_level) {
  (void)scope;
  if (!t || !access) return NULL;
  const type_t *s = type_strip_quals(t);
  if (!s) return NULL;
  if (!clone_needs_deep(s, slot_own, top_level)) return access;
  switch (s->kind) {
  case TYPE_KIND_PTR: {
    const type_t *base = ptr_type_base(s);
    if (!base) return NULL;
    /* 指针克隆一律走生成的 `__cloneptr_<U>`：函数体 `new U{ … }` 分配新块
       （被指向值含 own 槽位时逐槽位克隆；标量/指针/opaque 等单字段构造 = 整块
       拷贝）。不再用 CLONE 兜底（cunion/闭包除外，见 impl）。 */
    return gen_clone_call(sema, 1, base, access, /*by_address=*/false);
  }
  case TYPE_KIND_SLICE:
    return gen_clone_call(sema, 2, s, access, /*by_address=*/false);
  case TYPE_KIND_FUNC: {
    /* 闭包值的深拷贝：env 的捕获聚合类型只有运行期才知道 → 调用运行期的
       `__deep_clone(v)`（对象图克隆，闭包分支会新建实例并克隆 env）。 */
    return gen_deep_clone_call(sema, access);
  }
  case TYPE_KIND_OPTION:
  case TYPE_KIND_UNION:
    /* 联合/可空的 tag 判定只能在运行期 → 值克隆函数体内用三元表达式处理 */
    if (type_has_own_slots(s))
      return gen_clone_call(sema, 0, s, access, /*by_address=*/true);
    return access;
  case TYPE_KIND_STRUCT:
  case TYPE_KIND_TUPLE:
  case TYPE_KIND_ARRAY:
    if (type_has_own_slots(s))
      return gen_clone_call(sema, 0, s, access, /*by_address=*/true);
    return access;
  default:
    return access;   /* 标量 / cunion 等：普通拷贝 */
  }
}

/* []T 的深拷贝（长度运行期才知）：prelude 生成临时变量 + make + for 循环，
   表达式结果 = TAKE(临时变量)。 */
static ast_node_t *gen_clone_slice(sema_t *sema, sema_scope_t *scope,
                                   const type_t *t, ast_node_t *access) {
  if (!sema->cur_stmt) return NULL; /* 无语句上下文：交给运行期 __deep_clone 兜底 */

  const type_t *elem = slice_type_elem(t);
  ownership_t eown = type_ownership_of(slice_type_elem(t));
  uint32_t n = sema->clone_seq++;
  char nbuf[64], ibuf[64];
  snprintf(nbuf, sizeof nbuf, "__cl_%u", n);
  snprintf(ibuf, sizeof ibuf, "__cl_i_%u", n);
  strslice_t vname = clone_cstr(sema, nbuf);
  strslice_t iname = clone_cstr(sema, ibuf);
  if (!vname.ptr || !iname.ptr) return NULL;

  /* 临时变量符号：own（结果被 TAKE 移出）+ moved=true（块退出不生成 FREE） */
  sema_symbol_t sym;
  memset(&sym, 0, sizeof sym);
  sym.kind       = SEMA_SYM_VAR;
  sym.type       = t;
  sym.flow_init  = true;
  sym.moved      = true;
  sym.is_active  = true;
  sym.own_scope  = scope;
  if (!sema_scope_define(scope, vname, &sym))
    return NULL;

  /* 1) var __cl_N: []T = make(T, len(access));  —— MAKE 0（零初始化缓冲区） */
  ast_node_t *vd = ast_var_def_new(sema->arena, 0, 0);
  if (!vd) return NULL;
  vd->is_generated = true;
  ast_var_def_t *v = (ast_var_def_t *)vd;
  v->name      = vname;
  v->type_expr = sema_gen_type_ref(sema, t);
  ast_node_t *mk = ast_make_new(sema->arena, 0, 0);
  if (!mk || !v->type_expr) return NULL;
  mk->is_generated = true;
  ast_make_t *m = (ast_make_t *)mk;
  m->elem_type      = sema_gen_type_ref(sema, elem);
  m->length         = clone_len_call(sema, access);
  if (!m->elem_type || !m->length) return NULL;
  v->init = mk;

  /* 2) for __cl_i_N in 0..len(access) { __cl_N[__cl_i_N] = <元素 clone>; } */
  ast_node_t *elem_access = clone_index(sema, access,
                                        clone_ident(sema, iname));
  ast_node_t *elem_clone = elem_access
      ? gen_clone_expr(sema, scope, elem, elem_access, eown, false)
      : NULL;
  ast_node_t *lhs = clone_index(sema, clone_ident(sema, vname),
                                clone_ident(sema, iname));
  ast_node_t *as = ast_assign_new(sema->arena, 0, 0);
  if (!elem_clone || !lhs || !as) return NULL;
  as->is_generated = true;
  ((ast_assign_t *)as)->target = lhs;
  ((ast_assign_t *)as)->op     = NULL;   /* NULL = 普通赋值（compiler 特判） */
  ((ast_assign_t *)as)->value  = elem_clone;

  ast_node_t *blk = ast_block_new(sema->arena, 0, 0);
  if (!blk) return NULL;
  blk->is_generated = true;
  ((ast_block_t *)blk)->stmts      = as;
  ((ast_block_t *)blk)->stmts_last = as;

  ast_node_t *loop = ast_for_index_new(sema->arena, 0, 0);
  if (!loop) return NULL;
  ((ast_for_index_t *)loop)->var    = iname;
  ((ast_for_index_t *)loop)->source = access;
  ((ast_for_index_t *)loop)->body   = blk;

  vd->next = loop;
  loop->next = NULL;
  if (!sema_append_prelude(sema, vd)) return NULL;

  /* 3) 表达式 = TAKE(__cl_N) */
  ast_node_t *tk = ast_take_new(sema->arena, 0, 0);
  if (!tk) return NULL;
  ((ast_take_t *)tk)->operand = clone_ident(sema, vname);
  return tk;
}

/* ?T 的深拷贝：`(v != nil) ? .?T{ clone(v.!) } : .?T{ nil }`；
   as_new 时两个分支都用 `new ?T{…}`（结果 fatal *?T）。 */
static ast_node_t *gen_clone_option(sema_t *sema, sema_scope_t *scope,
                                    const type_t *t, ast_node_t *access,
                                    bool as_new) {
  const option_type_t *ot = (const option_type_t *)t;
  const type_t *inner = ot->inner;
  ownership_t iown = type_ownership_of(ot->inner);
  ast_node_t *unwrap = clone_unwrap(sema, access);
  ast_node_t *some_val = unwrap
      ? gen_clone_expr(sema, scope, inner, unwrap, iown, false)
      : NULL;
  ast_node_t *some = clone_construct(sema, t, some_val, 0, as_new);
  ast_node_t *nil = ast_nil_new(sema->arena, 0, 0);
  ast_node_t *none = nil ? clone_construct(sema, t, nil, 0, as_new) : NULL;
  ast_node_t *cond = clone_opt_is_some(sema, access);
  if (!some || !none || !cond) return NULL;
  return clone_ternary(sema, cond, some, none);
}

/* union 的深拷贝：member 三元链。as_new 时分支用 `new U{ .m = … }`
   （member_tag 逐分支写入），否则 `.U{ .m = … }`。 */
static ast_node_t *gen_clone_union(sema_t *sema, sema_scope_t *scope,
                                   const type_t *t, ast_node_t *access,
                                   bool as_new) {
  const union_type_t *ut = (const union_type_t *)t;
  ast_node_t *expr = NULL;
  for (size_t k = ut->member_count; k-- > 0;) {
    const union_member_t *m = &ut->members[k];
    if (!m->payload_type) continue;
    ast_node_t *mv = clone_member(sema, access, m->name);
    ast_node_t *payload = mv
        ? gen_clone_expr(sema, scope, m->payload_type, mv,
                             type_ownership_of(m->payload_type), false)
        : NULL;
    ast_node_t *field = payload ? clone_field(sema, m->name, payload) : NULL;
    ast_node_t *branch = field
        ? clone_construct(sema, t, field, m->tag, as_new) : NULL;
    ast_node_t *cond = clone_is_tag(sema, access, m->tag);
    if (!branch || !cond) continue;
    /* else：上一轮链；首轮（最内层）用**最后一个 member 的分支**兜底
       （不可达路径的防御：tag 一定命中某个 member）。 */
    ast_node_t *els = expr;
    if (!els) els = branch;
    else if (els == branch) els = access;
    expr = clone_ternary(sema, cond, branch, els);
  }
  return expr ? expr : (as_new ? NULL : access);
}

/* 生成「深拷贝 t 值」的表达式。access 是**被拷贝值的访问表达式**（变量/字段/
   下标/解引用链，纯读、可重复求值）。
   深拷贝判定为假时返回 access 本身（普通拷贝：借用/字节拷贝）。 */
static ast_node_t *gen_clone_expr_impl(sema_t *sema, sema_scope_t *scope,
                                       const type_t *t, ast_node_t *access,
                                       ownership_t slot_own, bool top_level) {
  if (!t || !access) return NULL;
  const type_t *s = type_strip_quals(t);
  if (!clone_needs_deep(s, slot_own, top_level)) return access;

  switch (s->kind) {
    case TYPE_KIND_PTR: {
      const type_t *base = ptr_type_base(s);
      if (!base) return NULL;
      const type_t *bs = type_strip_quals(base);
      ast_node_t *deref = clone_deref(sema, access);
      if (!deref) return NULL;
      switch (bs->kind) {
        case TYPE_KIND_OPTION:
        case TYPE_KIND_UNION: {
          /* ?T / union 的 tag 判定在运行期 → 逐分支 `new` */
          return (type_kind_of(bs) == TYPE_KIND_OPTION)
                     ? gen_clone_option(sema, scope, base, deref, true)
                     : gen_clone_union(sema, scope, base, deref, true);
        }
        case TYPE_KIND_SLICE: {
          ast_node_t *inner = gen_clone_expr(sema, scope, base, deref,
                                             OWN_OWN, true);
          return inner ? clone_construct(sema, base, inner, 0, true) : NULL;
        }
        case TYPE_KIND_STRUCT:
        case TYPE_KIND_TUPLE:
        case TYPE_KIND_ARRAY: {
          ast_node_t *fields = NULL, *ftail = NULL;
          if (!gen_clone_slots(sema, scope, base, deref, &fields, &ftail))
            return NULL;
          return clone_construct(sema, base, fields, 0, true);
        }
        case TYPE_KIND_CUNION:
          /* cunion 无所有权、成员布局重叠 → 无法用逐字段构造表达"整块克隆"；
             保留 CLONE（整块 memcpy）语义。 */
          return NULL;
        default:
          /* 标量 / 指针（双层）/ opaque / str / enum / bool：**单字段构造**
             `new T{ pointee }` —— CONSTRUCT 1 拷贝同类型值到新块，与 CLONE
             的"申请新块 + 拷贝字节"等价（不再需要 CLONE）。 */
          return clone_construct(sema, base, deref, 0, true);
      }
    }
    case TYPE_KIND_SLICE:
      return gen_clone_slice(sema, scope, t, access);
    case TYPE_KIND_OPTION: {
      ast_node_t *e = gen_clone_option(sema, scope, t, access, false);
      return e ? e : NULL;
    }
    case TYPE_KIND_UNION: {
      ast_node_t *e = gen_clone_union(sema, scope, t, access, false);
      return e ? e : NULL;
    }
    case TYPE_KIND_FUNC:
      /* 闭包 = 捕获聚合结构体：深拷贝必须按运行期 capture_type 克隆 env
         （静态类型表达不了时走运行期 `__deep_clone`；C 后端按闭包类型生成
         静态 env clone）。 */
      return gen_deep_clone_call(sema, access);
    case TYPE_KIND_STRUCT:
    case TYPE_KIND_TUPLE:
    case TYPE_KIND_ARRAY: {
      ast_node_t *fields = NULL, *ftail = NULL;
      if (!gen_clone_slots(sema, scope, t, access, &fields, &ftail))
        return NULL;
      ast_node_t *n = clone_construct(sema, t, fields, 0, false);
      return n ? n : NULL;
    }
    default:
      return access;   /* 标量/cunion 等：普通拷贝 */
  }
}

/* 聚合的逐槽位 clone/拷贝表达式链（声明序；数组按编译期长度展开）。 */
static bool gen_clone_slots(sema_t *sema, sema_scope_t *scope,
                            const type_t *t, ast_node_t *access,
                            ast_node_t **head, ast_node_t **tail) {
  *head = *tail = NULL;
  const type_t *s = type_strip_quals(t);
  if (!s) return false;

  if (type_kind_of(s) == TYPE_KIND_STRUCT) {
    const struct_type_t *st = (const struct_type_t *)s;
    for (size_t i = 0; i < st->field_count; i++) {
      const struct_field_t *f = &st->fields[i];
      if (!f->type || !f->name.ptr) continue;
      ast_node_t *mem = clone_member(sema, access, f->name);
      ast_node_t *e = mem ? gen_clone_expr(sema, scope, f->type, mem,
                                           type_ownership_of(f->type), false)
                          : NULL;
      if (!e) return false;
      if (!*head) { *head = *tail = e; }
      else { (*tail)->next = e; *tail = e; }
      (*tail)->next = NULL;
    }
    return true;
  }
  if (type_kind_of(s) == TYPE_KIND_TUPLE) {
    const tuple_type_t *tt = (const tuple_type_t *)s;
    for (size_t i = 0; i < tt->elem_count; i++) {
      const tuple_elem_t *el = &tt->elems[i];
      if (!el->type) continue;
      ast_node_t *ix = clone_int_lit(sema, (uint64_t)i);
      ast_node_t *e = ix ? gen_clone_expr(sema, scope, el->type,
                                          clone_index(sema, access, ix),
                                          type_ownership_of(el->type), false)
                         : NULL;
      if (!e) return false;
      if (!*head) { *head = *tail = e; }
      else { (*tail)->next = e; *tail = e; }
      (*tail)->next = NULL;
    }
    return true;
  }
  if (type_kind_of(s) == TYPE_KIND_ARRAY) {
    const array_type_t *at = (const array_type_t *)s;
    size_t len = array_type_len(s);
    if (len == SIZE_MAX) return false;   /* 动态数组不存在：防御 */
    for (size_t i = 0; i < len; i++) {
      ast_node_t *ix = clone_int_lit(sema, (uint64_t)i);
      ast_node_t *e = ix ? gen_clone_expr(sema, scope, at->elem_type,
                                          clone_index(sema, access, ix),
                                          type_ownership_of(at->elem_type), false)
                         : NULL;
      if (!e) return false;
      if (!*head) { *head = *tail = e; }
      else { (*tail)->next = e; *tail = e; }
      (*tail)->next = NULL;
    }
    return true;
  }
  return false;
}

/* 扁平化 clone 入口（ex_move 的 clone 分支调用）：
   返回**替换用的表达式节点**（原 AST_MOVE 位置），深拷贝由它完成。
   失败返回 NULL（调用方回退到运行期 CLONE）。 */
ast_node_t *sema_gen_clone(sema_t *sema, sema_scope_t *scope,
                                  const type_t *t, ast_node_t *access) {
  if (!t || !access) return NULL;
  if (type_strip_quals(t)->kind == TYPE_KIND_FUNC) return NULL;
  return gen_clone_expr(sema, scope, t, access, OWN_OWN, true);
}

/* **RC 创建通道**（§12.1）：目标类型是 `share *T`、源值是将亡（FATAL）或
   own 的普通 `*T` → 把源表达式改写成对内置 `__make_share` 的调用（sema 生成，
   源码不写 `__` 名字），并把值的类型/状态换成 `share *T`（普通可拷贝值）。
   成功返回 true（*pvalue / *pv 已替换）。 */
bool rc_adapt_share_init(sema_t *sema, const type_t *target,
                         ast_node_t **pvalue, value_t **pv) {
  if (!sema || !target || !type_is_share(target) || !pvalue || !pv) return false;
  const type_t *base = ptr_type_base(target);
  if (base && type_has_own_slots(type_strip_quals(base))) {
    /* share *T 的 RC 块析构需要 T 的销毁函数：在 sema 阶段就确保生成，
       否则 C 后端只能看到 `__destroy_<T>` 缺失。 */
    strslice_t dname = STRSLICE_EMPTY;
    (void)sema_ensure_destroy_fn(sema, type_strip_quals(base), &dname);
  }
  value_t *v = *pv;
  if (!base || !v || value_is_error(sema->vm, v) || value_is_type(v, TYPE_KIND_VOID))
    return false;
  const type_t *st = value_type(v);
  if (!st || type_kind_of(st) != TYPE_KIND_PTR || ptr_type_base(st) != base) return false;
  if (type_is_rc_ptr(st)) return false;                    /* 已是 share/weak */
  if (!(sema_value_is_fatal(v) || value_type_ownership(v) == OWN_OWN)) return false;

  ast_node_t *callee = ast_ident_new(sema->arena, 0, 0);
  ast_node_t *call = ast_call_new(sema->arena, 0, 0);
  if (!callee || !call || !*pvalue) return false;
  ((ast_ident_t *)callee)->name = strslice_from_bytes("__make_share", 12);
  callee->is_generated = true;
  ((ast_call_t *)call)->callee = callee;
  ((ast_call_t *)call)->args = *pvalue;
  call->is_generated = true;
  *pvalue = call;

  value_t *sv = value_make_shadow(sema->vm, target);
  value_set_type_ownership(sema->vm, sv, OWN_NONE);   /* share 是普通值（copy = 计数 +1） */
  *pv = sv;
  return true;
}
