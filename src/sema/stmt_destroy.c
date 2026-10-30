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
 * 仍是权威。
 *
 * 规则：**闭包槽位**（func 值，含结构体/元组/数组里的字段）生成 `FREE` 释放
 * env；指针/切片由用户显式 `free`，不自动销毁；复合值本身没有独立分配，其内部
 * 的闭包槽位在结构上属于外层值 → 递归展开。
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
    if (leaf) continue;   /* 指针/切片由用户显式 free */

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

/* 销毁一个**值**（`access` 所指、类型 t）：按类型内联展开（闭包槽位 FREE；
   指针/切片由用户显式 free，不自动销毁）。 */
bool gen_destroy(sema_t *sema, const type_t *t, ast_node_t *access,
                        ast_node_t **head, ast_node_t **tail) {
  if (!t || !access) return true;
  const type_t *s = type_strip_quals(t);
  if (!s) return true;
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
      /* 指针/切片由用户显式 free；字段直接跳过。 */
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

/* 为**元素类型**（array / slice）生成逐元素销毁循环体：
   `for <var> in 0..len(src) { <body> }` 的 body（引用 <var> 访问当前元素）。
   指针/切片元素由用户显式 free；元素无销毁语句时 body_head=NULL，调用方跳过
   整个循环。 */
static bool gen_destroy_elems(sema_t *sema, const type_t *elem_type,
                              ast_node_t *access,
                              strslice_t var_name, ast_node_t **body_head,
                              ast_node_t **body_tail) {
  *body_head = *body_tail = NULL;
  if (!elem_type) return true;
  const type_t *es = type_strip_quals(elem_type);
  if (es && (type_kind_of(es) == TYPE_KIND_PTR || type_kind_of(es) == TYPE_KIND_SLICE))
    return true;

  /* 元素访问：<access>[<var>] */
  ast_node_t *idx = ast_ident_new(sema->arena, 0, 0);
  if (!idx) return false;
  ((ast_ident_t *)idx)->name = var_name;
  ast_node_t *elem = make_index_access(sema, access, idx);
  if (!elem) return false;

  ast_node_t *bh = NULL, *bt = NULL;
  if (!gen_destroy(sema, elem_type, elem, &bh, &bt))
    return false;
  if (!bh) return true;   /* 元素没有可销毁槽位 → 无需循环 */
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
    case TYPE_KIND_PTR:
    case TYPE_KIND_SLICE:
      /* 裸指针/切片由用户显式 free，不做自动销毁。 */
      return true;
    case TYPE_KIND_STRUCT:
      return gen_destroy_struct(sema, (const struct_type_t *)t, access, head,
                                tail);
    case TYPE_KIND_UNION:
      return gen_destroy_union(sema, (const union_type_t *)t, access, head,
                               tail);
    case TYPE_KIND_TUPLE: {
      /* <T1,T2,…>：逐元素展开（复合元素递归；指针/切片跳过）。
         元素访问 = `access[i]`（INDEX_GET；i 是编译期字面量）。 */
      const tuple_type_t *tt = (const tuple_type_t *)t;
      for (size_t i = 0; i < tt->elem_count; i++) {
        const type_t *et = tt->elems[i].type;
        if (!et) continue;
        const type_t *es = type_strip_quals(et);
        if (es && (type_kind_of(es) == TYPE_KIND_PTR ||
                   type_kind_of(es) == TYPE_KIND_SLICE))
          continue;
        ast_node_t *idxv = make_int_lit_i64(sema, (uint64_t)i);
        if (!idxv) return false;
        ast_node_t *elem = make_index_access(sema, access, idxv);
        if (!elem) return false;
        if (!gen_destroy(sema, et, elem, head, tail))
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
      if (!gen_destroy_elems(sema, at->elem_type, access,
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
         指针/切片 inner 由用户显式 free；复合 inner 递归。
         判空用 AST_OPT_IS_SOME（直接形态：compiler 发 OPT_IS_NONE + NOT），
         取值用 AST_UNWRAP（编译器发 UNWRAP，借用引用 value 字段）。 */
      const option_type_t *ot = (const option_type_t *)t;
      const type_t *inner = ot->inner;
      if (!inner) return true;
      const type_t *is = type_strip_quals(inner);
      if (is && (type_kind_of(is) == TYPE_KIND_PTR || type_kind_of(is) == TYPE_KIND_SLICE))
        return true;

      ast_node_t *unwrap = ast_unwrap_new(sema->arena, 0, 0);
      if (!unwrap) return false;
      ((ast_unwrap_t *)unwrap)->operand = access;

      ast_node_t *bh = NULL, *bt = NULL;
      if (!gen_destroy(sema, inner, unwrap, &bh, &bt))
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
  if (sema->fn.walking_comptime) return NULL;
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
    if (sym->is_comptime) continue;
    /* 定义点尚未走到（提前 return/break 的路径）→ 该变量在运行时还不存在，
       不能生成销毁（否则 PUSH 一个尚未 DEFINE 的名字 → undefined variable）。 */
    if (!sym->declared) continue;
    /* 只有**闭包（func 值）**需要释放 env；指针/切片由用户显式 free，
       复合值不持有独立堆块。 */
    if (type_kind_of(sym->type) != TYPE_KIND_FUNC) continue;

    /* arena 持久化变量名（strslice → NUL 终止字符串；AST_IDENT 引用它） */
    size_t nlen = strlen(name);
    char *name_buf = (char *)arena_calloc(
        sema->arena, nlen + 1, 1, ALIGNOF(max_align_t));
    memcpy(name_buf, name, nlen);
    strslice_t name_slice = strslice_from_bytes(name_buf, nlen);

    /* ---- 闭包（func 值）：**单个 FREE** ----
       闭包 = 捕获聚合结构体（env 块）。VM 的 FREE 分支按 capture_type 释放
       env 块本体。 */
    ast_node_t *obj = ast_ident_new(sema->arena, 0, 0);
    if (!obj) continue;
    ((ast_ident_t *)obj)->name = name_slice;
    ast_node_t *fs = ast_free_new(sema->arena, obj);
    if (!fs) continue;
    if (!head) head = tail = fs;
    else { tail->next = fs; tail = fs; }
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
