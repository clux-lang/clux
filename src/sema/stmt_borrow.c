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
#include "parser/ast_owned_type.h"
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

static size_t borrow_collect_fields(sema_t *sema, const type_t *t, ast_node_t *fields, sema_scope_t *scope, sema_symbol_t **out, size_t cap, size_t n);


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

flow_snap_t flow_capture(sema_t *sema, sema_scope_t *scope) {
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

void flow_restore(const flow_snap_t *snap) {
  for (size_t i = 0; i < snap->n; i++)
    snap->syms[i]->flow_init = snap->before[i];
}

void flow_release(flow_snap_t *snap) {
  if (!snap || !snap->syms) return;
  allocator_free(snap->alloc, (void **)&snap->syms);
  allocator_free(snap->alloc, (void **)&snap->before);
  allocator_free(snap->alloc, (void **)&snap->after);
  snap->n = 0;
}

/* ---- 语句：变量定义 ---- */

/* strslice → NUL 终止临时缓冲（scope_define 内部复制 key，栈缓冲安全） */
void name_to_cstr(strslice_t s, char *buf, size_t cap) {
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
void borrow_srcs_set(sema_t *sema, sema_symbol_t *var,
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
      (sema_sym_ownership(root) == OWN_OWN || sema_sym_ownership(root) == OWN_FATAL);
  const type_t *ct0 = type_strip_quals(cur);
  bool ptr_owned = root_owned && ct0 &&
                   (type_kind_of(ct0) == TYPE_KIND_PTR ||
                    type_kind_of(ct0) == TYPE_KIND_SLICE);
  bool storage = true;   /* 当前值存储是否归根（根的值存储归根） */

  for (int i = n - 1; i >= 0; i--) {
    const type_t *ct = type_strip_quals(cur);
    ast_node_t *h = hops[i].node;
    if (hops[i].kind == AST_DEREF) {
      if (!ct || type_kind_of(ct) != TYPE_KIND_PTR) return NULL;
      storage = ptr_owned;   /* 解引用得到的内存归根当且仅当指针值归根 */
      cur = ptr_type_base(ct);
      continue;              /* ptr_owned 继承：own *own *T 链式解引用仍归根 */
    }
    if (hops[i].kind == AST_UNWRAP) {
      if (!ct || type_kind_of(ct) != TYPE_KIND_OPTION) return NULL;
      if (storage)
        ptr_owned = (type_ownership_of(type_option_inner(ct)) == OWN_OWN);
      cur = type_option_inner(ct);
      continue;
    }
    if (hops[i].kind == AST_INDEX) {
      ast_index_t *ix = (ast_index_t *)h;
      if (!ix->indices || ix->indices->kind != AST_INT_LIT) return NULL;
      uint64_t k = ((ast_int_lit_t *)ix->indices)->value;
      if (!ct) return NULL;
      if (type_kind_of(ct) == TYPE_KIND_ARRAY) {
        if (storage)
        ptr_owned = (type_ownership_of(array_type_elem(ct)) == OWN_OWN);
        cur = array_type_elem(ct);
        continue;
      }
      if (type_kind_of(ct) == TYPE_KIND_SLICE) {
        if (storage || ptr_owned)
          ptr_owned = (type_ownership_of(slice_type_elem(ct)) == OWN_OWN);
        storage = storage || ptr_owned;   /* 元素在缓冲里 */
        cur = slice_type_elem(ct);
        continue;
      }
      if (type_kind_of(ct) == TYPE_KIND_TUPLE) {
        const tuple_elem_t *te = tuple_type_elem(ct, (size_t)k);
        if (!te) return NULL;
        if (storage) ptr_owned = (type_ownership_of(te->type) == OWN_OWN);
        cur = te->type;
        continue;
      }
      return NULL;
    }
    /* MEMBER */
    {
      ast_member_t *m = (ast_member_t *)h;
      if (!ct) return NULL;
      if (type_kind_of(ct) == TYPE_KIND_PTR) {
        if (!ptr_owned) return NULL;   /* 穿过 ref/借用指针：借的是别人的块 */
        ct = type_strip_quals(ptr_type_base(ct));
        storage = true;
        ptr_owned = false;
      }
      if (!ct) return NULL;
      if (type_kind_of(ct) == TYPE_KIND_STRUCT) {
        int fi = struct_type_find_field(ct, m->field);
        const struct_field_t *f =
            fi >= 0 ? struct_type_field(ct, (size_t)fi) : NULL;
        if (!f || !f->type) return NULL;
        const type_t *ft = type_strip_quals(f->type);
        if (ft && (type_kind_of(ft) == TYPE_KIND_PTR || type_kind_of(ft) == TYPE_KIND_SLICE)) {
          ptr_owned = storage && (type_ownership_of(f->type) == OWN_OWN);
          storage = false;
        } else {
          ptr_owned = false;   /* 复合字段：存储仍是根的存储 */
        }
        cur = f->type;
        continue;
      }
      if (type_kind_of(ct) == TYPE_KIND_UNION) {
        int mi = union_type_find_member(ct, m->field);
        const union_member_t *mm =
            mi >= 0 ? union_type_member(ct, (size_t)mi) : NULL;
        if (!mm || !mm->payload_type) return NULL;
        const type_t *pt = type_strip_quals(mm->payload_type);
        if (pt && (type_kind_of(pt) == TYPE_KIND_PTR || type_kind_of(pt) == TYPE_KIND_SLICE)) {
          ptr_owned = storage &&
                       (type_ownership_of(mm->payload_type) == OWN_OWN);
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

size_t borrow_collect_add(sema_symbol_t **out, size_t n, size_t cap,
                                 sema_symbol_t *s) {
  if (!s || n >= cap) return n;
  for (size_t i = 0; i < n; i++)
    if (out[i] == s) return n;   /* 去重 */
  out[n] = s;
  return n + 1;
}

size_t borrow_collect(sema_t *sema, ast_node_t *e, sema_scope_t *scope,
                             sema_symbol_t **out, size_t cap) {
  if (!e || cap == 0) return 0;
  size_t n = 0;
  switch (e->kind) {
  case AST_IDENT: {
    sema_symbol_t *s = sema_lookup(scope, ((ast_ident_t *)e)->name);
    if (!s || s->kind != SEMA_SYM_VAR) return 0;
    bool carrier = s->type && (sema_sym_ownership(s) == OWN_OWN ||
                               sema_sym_ownership(s) == OWN_REF ||
                               type_kind_of(s->type) == TYPE_KIND_OPAQUE);
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
void shadow_borrow_register(sema_t *sema, sema_symbol_t *sym,
                                   ast_node_t *init, sema_scope_t *scope) {
  if (!sym || !sym->type) return;
  if (sema_sym_ownership(sym) != OWN_REF &&
      type_kind_of(sym->type) != TYPE_KIND_OPAQUE)
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
      if ((type_kind_of(ft) == TYPE_KIND_PTR || type_kind_of(ft) == TYPE_KIND_SLICE) &&
          type_ownership_of(f->type) == OWN_REF)
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
      if ((type_kind_of(et) == TYPE_KIND_PTR || type_kind_of(et) == TYPE_KIND_SLICE) &&
          type_ownership_of(e->type) == OWN_REF)
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
      if ((type_kind_of(pt) == TYPE_KIND_PTR || type_kind_of(pt) == TYPE_KIND_SLICE) &&
          type_ownership_of(m->payload_type) == OWN_REF)
        return true;
      if (type_has_ref_slots_rec(pt, path, depth)) return true;
    }
    return false;
  }
  case TYPE_KIND_OPTION: {
    const option_type_t *ot = (const option_type_t *)s;
    const type_t *in = type_strip_quals(ot->inner);
    if (!in) return false;
    if ((type_kind_of(in) == TYPE_KIND_PTR || type_kind_of(in) == TYPE_KIND_SLICE) &&
        type_ownership_of(ot->inner) == OWN_REF)
      return true;
    return type_has_ref_slots_rec(in, path, depth);
  }
  case TYPE_KIND_ARRAY: {
    const array_type_t *at = (const array_type_t *)s;
    const type_t *et = type_strip_quals(at->elem_type);
    if (!et) return false;
    if ((type_kind_of(et) == TYPE_KIND_PTR || type_kind_of(et) == TYPE_KIND_SLICE) &&
        type_ownership_of(at->elem_type) == OWN_REF)
      return true;
    return type_has_ref_slots_rec(et, path, depth);
  }
  case TYPE_KIND_SLICE: {
    const type_t *et = type_strip_quals(slice_type_elem(s));
    if (!et) return false;
    if ((type_kind_of(et) == TYPE_KIND_PTR || type_kind_of(et) == TYPE_KIND_SLICE) &&
        type_ownership_of(slice_type_elem(s)) == OWN_REF)
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
bool type_has_ref_slots(const type_t *t) {
  const type_t *path[32];
  return type_has_ref_slots_rec(t, path, 0);
}

/* 收集"类型 t、槽位所有权 slot_own 的值表达式 e"中所有 ref 槽位的借源。 */
size_t borrow_collect_value(sema_t *sema, const type_t *t,
                                   ownership_t slot_own, ast_node_t *e,
                                   sema_scope_t *scope, sema_symbol_t **out,
                                   size_t cap, size_t n) {
  if (!t || !e || n >= cap) return n;
  const type_t *s = type_strip_quals(t);
  if (!s) return n;
  /* 叶子槽位：ref 槽位的值表达式本身就是借用 → 收集来源 */
  if (type_kind_of(s) == TYPE_KIND_PTR || type_kind_of(s) == TYPE_KIND_SLICE) {
    if (slot_own == OWN_REF) {
      sema_symbol_t *tmp[16];
      size_t m = borrow_collect(sema, e, scope, tmp, 16);
      for (size_t i = 0; i < m; i++)
        n = borrow_collect_add(out, n, cap, tmp[i]);
      return n;
    }
    /* own 指针的 pointee 里若有 ref 槽位：构造是 `new T{…}`（按字段收集） */
    if (type_kind_of(s) == TYPE_KIND_PTR && e->kind == AST_NEW) {
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
  if (type_kind_of(s) == TYPE_KIND_STRUCT) {
    const struct_type_t *st = (const struct_type_t *)s;
    for (ast_node_t *f = fields; f && idx < st->field_count; f = f->next, idx++) {
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((const ast_construct_field_t *)f)->value;
      if (!value || value->kind == AST_NIL) continue;
      const struct_field_t *fd = &st->fields[idx];
      n = borrow_collect_value(sema, fd->type, type_ownership_of(fd->type), value,
                             scope,
                               out, cap, n);
    }
    return n;
  }
  if (type_kind_of(s) == TYPE_KIND_TUPLE) {
    const tuple_type_t *tt = (const tuple_type_t *)s;
    for (ast_node_t *f = fields; f && idx < tt->elem_count; f = f->next, idx++) {
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((const ast_construct_field_t *)f)->value;
      if (!value || value->kind == AST_NIL) continue;
      n = borrow_collect_value(sema, tt->elems[idx].type,
                               type_ownership_of(tt->elems[idx].type), value, scope, out, cap,
                               n);
    }
    return n;
  }
  if (type_kind_of(s) == TYPE_KIND_ARRAY) {
    const array_type_t *at = (const array_type_t *)s;
    for (ast_node_t *f = fields; f; f = f->next) {
      if (f->kind == AST_FILL) {
        n = borrow_collect_value(sema, at->elem_type, type_ownership_of(at->elem_type),
                                 ((const ast_fill_t *)f)->value, scope, out,
                                 cap, n);
        continue;   /* fill 重复同一值，来源相同 */
      }
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((const ast_construct_field_t *)f)->value;
      if (!value || value->kind == AST_NIL) continue;
      n = borrow_collect_value(sema, at->elem_type, type_ownership_of(at->elem_type), value,
                               scope, out, cap, n);
    }
    return n;
  }
  if (type_kind_of(s) == TYPE_KIND_OPTION) {
    const option_type_t *ot = (const option_type_t *)s;
    ast_node_t *f = fields;
    if (f && f->kind == AST_CONSTRUCT_FIELD)
      f = ((const ast_construct_field_t *)f)->value;
    if (!f || f->kind == AST_NIL) return n;
    return borrow_collect_value(sema, ot->inner, type_ownership_of(ot->inner), f,
                                scope,
                                out, cap, n);
  }
  if (type_kind_of(s) == TYPE_KIND_UNION) {
    const union_type_t *ut = (const union_type_t *)s;
    ast_node_t *f = fields;
    if (f && f->kind == AST_CONSTRUCT_FIELD)
      f = ((const ast_construct_field_t *)f)->value;
    if (!f || f->kind == AST_NIL) return n;
    /* 具名构造：按字段名定位 member；否则用 AST 上的 member_tag（sema 填） */
    return borrow_collect_value(sema, ut->member_count ? ut->members[0].payload_type
                                                       : NULL,
                                ut->member_count ? type_ownership_of(ut->members[0].payload_type)
                                                 : OWN_NONE,
                                f, scope, out, cap, n);
  }
  return n;
}

/* 把来源合并进载体（carrier）的借源集合：**不释放旧集合**——字段级重绑定
   无法逐槽位精确回收，保守保留（只会更严格，不会漏拦）。 */
void borrow_carry_merge(sema_t *sema, sema_symbol_t *carrier,
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
sema_symbol_t *borrow_root_symbol(sema_t *sema, ast_node_t *e,
                                         sema_scope_t *scope) {
  (void)sema; /* 签名与同族 helper 一致（调用点统一传 sema），此处不消费 */
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
