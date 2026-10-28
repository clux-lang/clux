#include "sema/sema.h"
#include "sema/sema_cross.h" /* 跨模块导出符号（`mod::Type`） */
#include "sema/struct_compose.h" /* 结构体组合（`...Base;`）pass */
#include "vm/type_cfunc.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_ownership.h"
#include "vm/type_opaque.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "core/panic.h"
#include "core/string.h"
#include "ctfe/ctfe.h"
#include "parser/ast_array.h"
#include "parser/ast_tuple.h"
#include "parser/ast_const.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_pack_type.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_spread.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_option.h"
#include "parser/ast_program.h"
#include "parser/ast_ptr.h"
#include "parser/ast_owned_type.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_scope_annot.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/ast_alignof.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_typeof.h"
#include "parser/parse_utils.h"
#include "parser/lexer.h"
#include "sema/comptime.h"
#include "sema/sema_cross.h"
#include "sema/sema_generic.h"
#include "parser/ast_import.h"
#include "parser/ast_reexport.h"
#include "parser/ast_extern_lib.h"
#include "vm/module.h"
#include "core/ffi_abi.h"   /* M6 FFI：§2 C ABI 类型校验 */
#include <string.h>
#include <stdio.h>

#include "sema_internal.h"

const sema_type_t *sema_type_register(sema_t *sema, const type_t *t);
const sema_type_t *sema_type_find(sema_t *sema, const type_t *t);
const type_t *resolve_type_expr(sema_t *sema, ast_node_t *type_expr);

/* ===========================================================================
 * 公共工具
 * =========================================================================== */

/* 编译期求值数组边界 N（[N]T 的长度槽位）。
 *
 * N 是编译期常量表达式（m2-design §6）：字面量直接读值；复杂表达式
 * （[2+3]i32 / [sizeof(a)]i32 / comptime var 引用）走 ctfe 真实求值
 * 成功后把非字面量边界折叠为 AST_INT_LIT 写回
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

  /* `port::CONST`：先走 sema_expr，把跨模块 comptime 常量折叠成字面量
     （CTFE 不解析模块成员）。sema_expr 已报错时不要再让 CTFE 报第二条。 */
  if (b->kind == AST_SCOPE_REF) {
    size_t before = sema->diag ? diag_count(sema->diag) : 0;
    sema_expr(sema, bound, sema->global_scope);
    if (sema->diag && diag_count(sema->diag) > before) return false;
    b = *bound;
    if (b->kind == AST_INT_LIT) {
      ast_int_lit_t *il = (ast_int_lit_t *)b;
      if (il->value > (uint64_t)SIZE_MAX) {
        diag_error(sema->diag, sema_loc(sema, b),
                   "array length %llu too large",
                   (unsigned long long)il->value);
        return false;
      }
      *len = (size_t)il->value;
      return true;
    }
  }

  /* 复杂表达式：ctfe 编译期求值（shadow 值不可用，须真实常量） */
  vm_t *vm = sema->vm;
  ctfe_ctx_t ctx;
  memset(&ctx, 0, sizeof ctx);
  ctx.vm = vm;
  ctx.sema = sema;
  ctx.budget = 100000;
  ctx.max_depth = 128;
  value_t *r = ctfe_eval(&ctx, b);

  if (!r || value_is_error(vm, r)) {
    diag_error(sema->diag, sema_loc(sema, b),
               "array length must be a compile-time constant");
    return false;
  }
  const type_t *t = value_type(r);
  if (!t || type_kind_of(t) != TYPE_KIND_INT) {
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
    case TYPE_KIND_TUPLE: {
      /* 元组类型：元素类型也是程序类型（嵌套元组/数组/struct 递归覆盖）。
         依赖后序：先登记依赖，hoist pass 2 先定义依赖。 */
      size_t n = tuple_type_elem_count(t);
      for (size_t i = 0; i < n; i++) {
        const tuple_elem_t *e = tuple_type_elem(t, i);
        if (e && e->type) sema_type_register(sema, e->type);
      }
      break;
    }
    case TYPE_KIND_CONST:
    case TYPE_KIND_VOLATILE:
    case TYPE_KIND_OWN:
    case TYPE_KIND_REF:
    case TYPE_KIND_FATAL: {
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
    case TYPE_KIND_UNION: {
      /* union 类型：各 member 的 payload 类型也是程序类型（布局依赖）。
         依赖后序：先登记依赖，hoist pass 2 先构造 payload 类型再密封
         union（SEAL 算布局需要 payload size/align 已确定）。 */
      size_t n = union_type_member_count(t);
      for (size_t i = 0; i < n; i++) {
        const union_member_t *m = union_type_member(t, i);
        if (m && m->payload_type)
          sema_type_register(sema, m->payload_type);
      }
      break;
    }
    case TYPE_KIND_CUNION: {
      /* cunion 类型：各 member 的类型也是程序类型（布局依赖）。
         依赖后序：先登记依赖，hoist pass 2 先构造 member 类型再密封
         cunion（SEAL 算 size=max/align=max 需要 member size/align 已确定）。 */
      size_t n = cunion_type_member_count(t);
      for (size_t i = 0; i < n; i++) {
        const cunion_member_t *m = cunion_type_member(t, i);
        if (m && m->type)
          sema_type_register(sema, m->type);
      }
      break;
    }
    case TYPE_KIND_PTR: {
      /* 指针类型：被指向类型 T 也是程序类型（引用依赖——seal 不依赖
         base 布局，但 hoist 中 LOAD_TYPE 拉回 base 需要其已登记。
         PTR 段 < COUNT 自动登记；此处只递归登记 base 依赖。 */
      const type_t *base = ptr_type_base(t);
      if (base) sema_type_register(sema, base);
      break;
    }
    default:
      break; /* 标量/str/bool/type/error/opaque：无结构依赖 */
  }
}

/* ===========================================================================
 * 复合类型的 own 资源判定：**已删除**（2026-10 所有权重构）
 *
 * 原先的 `sema_type_contains_owned_resource` / `sema_type_contains_own_resource`
 * 是类型层递归谓词，随重构失效（类型层不再携带 ownership；且复合类型已禁止
 * 携带指针/切片，"含 own 字段的复合类型"这一形态当前不存在），已无调用方，
 * 故连同 `type_directly_owns` 空壳一并删除。
 *
 * 判据入口现为**符号/值层**：`sema_symbol_t::ownership` 与 `value_type_ownership()`。
 * 将来恢复含指针的复合类型时，按**扁平槽位表**重新设计，不恢复这两个谓词。
 * =========================================================================== */


/* move(x) / clone(x) 的**产物类型**（将亡值变体，m3-design §3.3/§7）。
 *
 * 规则（与运行期 MOVE/CLONE 指令压栈的类型必须一致）：
 *   - ot 已是 fatal → ot（幂等：move(fatal 参数) 继续传下去）
 *   - 指针 own/ref  → fatal *T（fatal 指针已是 fatal，走上一分支）
 *   - 切片 own/ref  → fatal []T
 *   - 含 own 资源的复合类型（struct/tuple/array/option/union）→ fatal T
 *   - 其余（标量 / 平凡 struct / ref 切片）→ ot 原样（无资源可转移）
 *
 * 返回 NULL = 无法构造（类型槽位内部错误），调用方转 val_void。 */
const type_t *sema_fatal_variant_of(sema_t *sema, const type_t *ot) {
  if (!sema || !ot) return NULL;
  /* 所有权是类型修饰：fatal 变体 = 在**裸类型**外面套 `fatal`。
     `fatal T` 幂等；标量/无资源的类型 move 后仍是原类型。 */
  if (type_ownership_of(ot) == OWN_FATAL) return ot;
  const type_t *core = type_strip_ownership(ot);
  if (!core) return NULL;
  bool resource = type_is_ptr(core) || type_is_slice(core) ||
                  type_kind_of(core) == TYPE_KIND_FUNC ||
                  type_has_own_slots(core);
  if (!resource) return ot;
  const type_t *fatal = type_ownership_apply(sema->vm, OWN_FATAL, core);
  if (fatal) sema_type_register(sema, fatal);
  return fatal;
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
    case TYPE_KIND_OPAQUE:
      return NULL; /* 内建标量/哨兵（opaque 是内建单例，id 17 已固定绑定） */
    case TYPE_KIND_FUNC:
      break; /* 用户函数签名类型：登记（提升进 hoist 区） */
    default:
      break; /* 复合段（CONST..CUNION）+ 指针段（PTR_*）：登记 */
  }

  /* 按 type_t 指针去重：同一 intern 实例只登记一次 */
  const sema_type_t *found = sema_type_find(sema, t);
  if (found) return found;

  size_t n = vec_len(sema->types);
  uint32_t id = type_id_alloc(sema->vm);
  if (id == 0) return NULL; /* id 耗尽 */

  sema_type_t *st = (sema_type_t *)allocator_new_ex(
      sema->vm->alloc, "sema_type_t", sizeof(sema_type_t), NULL, NULL, NULL, 1);
  if (!st) return NULL;
  memset(st, 0, sizeof(*st));
  st->type  = t;
  st->id    = id;
  st->index = (uint32_t)n;

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

/* 所有权修饰 token → ownership_t（仅类型解析内部使用）。 */
static ownership_t ownership_token_own(const token_t *tok) {
  if (!tok) return OWN_NONE;
  strslice_t own = token_strslice(tok);
  if (own.len == 3 && own.ptr && memcmp(own.ptr, "own", 3) == 0)
    return OWN_OWN;
  if (own.len == 3 && own.ptr && memcmp(own.ptr, "ref", 3) == 0)
    return OWN_REF;
  if (own.len == 5 && own.ptr && memcmp(own.ptr, "fatal", 5) == 0)
    return OWN_FATAL;
  return OWN_NONE;
}

/* 纯求值（不登记、不替换槽位）：switch 分派类型表达式节点 → 类型单例。
 * 由 resolve_type_expr（登记）与递归 sub 解析共用。 */
static const type_t *sema_resolve_inner(sema_t *sema, ast_node_t *type_expr) {
  if (!sema || !type_expr) return NULL;

  switch (type_expr->kind) {
    case AST_IDENT: {
      /* 类型名引用：i32/bool 等关键字类型名与用户类型名（IDENTIFIER）
         统一为 AST_IDENT（类型即表达式），查类型表。 */
      ast_ident_t *id = (ast_ident_t *)type_expr;
      const type_t *t = type_lookup(sema->vm, id->name);
      if (!t) {
        sema_symbol_t *gs =
            sema_scope_find_local(sema->global_scope, id->name);
        if (gs && gs->kind == SEMA_SYM_INTERFACE) {
          diag_error(sema->diag, sema_loc(sema, type_expr),
                     "'%.*s' is an interface (a compile-time constraint), not "
                     "a type: use it in 'extends' / 'implements'",
                     (int)id->name.len, id->name.ptr);
          return NULL;
        }
        /* M7：泛型类型名不能直接当类型用——必须实例化。 */
        if (gs && gs->is_generic) {
          diag_error(sema->diag, sema_loc(sema, type_expr),
                     "generic type '%.*s' requires type arguments: write "
                     "'%.*s[...]' to instantiate it",
                     (int)id->name.len, id->name.ptr, (int)id->name.len,
                     id->name.ptr);
          return NULL;
        }
      }
      return t;
    }
    case AST_CONST: {
      /* const 类型修饰：嵌套递归（const const T 收敛为 const T） */
      const type_t *sub = resolve_type_expr(sema, ((ast_const_t *)type_expr)->sub);
      return sub ? type_const_intern(sema->vm, sub) : NULL;
    }
    case AST_VOLATILE: {
      /* `volatile T` **在脚本态无意义** → 折叠为 T（完全放行）：所有类型检查按 T
         走；compiler 侧同样不发 CREATE_VOLATILE（两处必须一致，否则声明处会构造出
         volatile i32 而类型检查按 i32 走）。AST/fmt 不动，格式化不会丢关键字。 */
      return resolve_type_expr(sema, ((ast_volatile_t *)type_expr)->sub);
    }
    case AST_OPTION: {
      /* ?T optional 类型修饰：递归解析内层 T → 按 (inner + inner 槽位所有权)
         去重 intern。与 const/volatile 同族（类型即表达式，嵌套递归）。
         内层可以是指针/切片/含 own 槽位的聚合——`?own *T` 的 own 记在类型
         描述上（销毁序列 `if (v != nil)` 据此决定是否 FREE `v.!`）。
         所有权必须在 resolve **之前**取（折叠会丢 ownership token）。 */
      ast_option_t *op = (ast_option_t *)type_expr;
      const type_t *sub = resolve_type_expr(sema, op->sub);
      if (!sub) return NULL;
      /* 所有权已编码在 inner 类型上（`?own *T` 的 inner 就是 `own *T`）。 */
      return type_option_intern(sema->vm, sub);
    }
    case AST_ARRAY: {
      /* [N]T 数组类型：递归解析基础类型 + 编译期求值边界 N →
         按 (elem_type, length, 元素槽位所有权) 去重 intern。边界槽位支持嵌套
         数组（[2][3]i32 = 元素类型是 [3]i32）。
         元素可以是指针/切片/含 own 槽位的聚合——`[N]own *T` 的 own 记在类型
         描述上（销毁序列 `for i in 0..N` 据此决定是否 FREE `a[i]`）。 */
      ast_array_t *arr = (ast_array_t *)type_expr;
      const type_t *base = resolve_type_expr(sema, arr->base_type);
      if (!base) return NULL;
      size_t len;
      if (!sema_eval_array_bound(sema, &arr->length, &len)) return NULL;
      return type_array_intern(sema->vm, base, len);
    }
    case AST_TUPLE: {
      /* <T1,T2,...> 元组类型：逐元素递归解析（可为任意类型表达式，
         含嵌套元组/数组）→ 按（元素类型 + 顺序 + 元素槽位所有权）去重 intern。
         M7 §30.4：元素 `...Args` 在泛型实例内展开为该参数包的类型实参。 */
      ast_tuple_t *tp = (ast_tuple_t *)type_expr;
      const sema_generic_instance_t *gi = sema->generics.active_inst;
      size_t total = 0;
      for (ast_node_t *et = tp->elem_types; et; et = et->next) {
        if (et->kind == AST_SPREAD) {
          ast_spread_t *sp = (ast_spread_t *)et;
          strslice_t nm = sp->operand && sp->operand->kind == AST_IDENT
                              ? ((ast_ident_t *)sp->operand)->name
                              : STRSLICE_EMPTY;
          if (!gi || !gi->has_pack || !strslice_eq(nm, gi->pack_name)) {
            diag_error(sema->diag, sema_loc(sema, et),
                       "'...%.*s' does not name a parameter pack of the "
                       "enclosing generic instance",
                       (int)nm.len, nm.ptr);
            return NULL;
          }
          total += gi->pack_count;
        } else {
          total++;
        }
      }
      tuple_elem_t *elems = NULL;
      if (total > 0) {
        elems = (tuple_elem_t *)arena_alloc(
            sema->arena, total * sizeof(tuple_elem_t), ALIGNOF(max_align_t));
        if (!elems) return NULL;
      }
      size_t i = 0;
      for (ast_node_t *et = tp->elem_types; et; et = et->next) {
        if (et->kind == AST_SPREAD) {
          for (size_t k = 0; k < gi->pack_count; k++) {
            const sema_generic_arg_t *a = &gi->args[gi->pack_offset + k];
            if (a->is_value) {
              diag_error(sema->diag, sema_loc(sema, et),
                         "comptime value parameter packs are not implemented "
                         "yet (M7 P5c)");
              return NULL;
            }
            elems[i].type = a->type;
            elems[i].offset = 0;
            i++;
          }
          continue;
        }
        const type_t *pt = resolve_type_expr(sema, et);
        if (!pt) return NULL;
        elems[i].type = pt;
        elems[i].offset = 0;
        i++;
      }
      return type_tuple_intern(sema->vm, total > 0 ? elems : NULL, total);
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
        params_arr[i] = pt; /* 类型已携带所有权修饰（ref/own/fatal） */
      }
      const type_t *rt = ft->return_type
                             ? resolve_type_expr(sema, ft->return_type)
                             : NULL;
      if (ft->return_type && !rt) return NULL;
      /* `cfunc(...)` = **C 函数指针**（无 env）：独立类型，与 func(...) 互不
         兼容（用户要求"不互通"）。只在 FFI 边界出现。 */
      if (ft->is_c_abi) {
        if (ft->captures) {
          diag_error(sema->diag, sema_loc(sema, type_expr),
                     "cfunc type cannot have a capture list (the C ABI has "
                     "no closures)");
          return NULL;
        }
        return type_cfunc_sig(sema->vm, n > 0 ? params_arr : NULL, n, rt,
                              /*is_variadic=*/false);
      }
      if (ft->captures) {
        size_t nc = sema_count_siblings(ft->captures);
        const type_t *caps[nc > 0 ? nc : 1];
        size_t k = 0;
        for (ast_node_t *c = ft->captures; c; c = c->next, k++) {
          ast_var_def_t *cv = (ast_var_def_t *)c;
          const type_t *ct = cv->type_expr
                                 ? resolve_type_expr(sema, cv->type_expr)
                                 : NULL;
          if (!ct) {
            diag_error(sema->diag, sema_loc(sema, c),
                       "cannot resolve closure capture type");
            return NULL;
          }
          caps[k] = ct;
        }
        return type_func_closure(sema->vm, n > 0 ? params_arr : NULL, n, rt,
                                 /*is_variadic=*/false, caps, nc);
      }
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
    case AST_SIZEOF:
    case AST_ALIGNOF:
    case AST_TYPEOF: {
      /* 编译期运算符（m2-design §8，SEMA→CTFE 桥梁）：类型槽位中的
         sizeof/alignof/typeof 由 ctfe 求值（操作数按 shadow 语义只取类型）。
         typeof → 类型本身；sizeof/alignof → 无法作为类型槽位（结果是 u64
         值，非类型），报错。 */
      ctfe_ctx_t ctx;
      memset(&ctx, 0, sizeof ctx);
      ctx.vm = sema->vm;
      ctx.sema = sema;
      ctx.budget = 100000;
      ctx.max_depth = 128;
      value_t *v = ctfe_eval(&ctx, type_expr);
      if (value_is_error(sema->vm, v)) {
        diag_error(sema->diag, sema_loc(sema, type_expr),
                   "unsupported type expression");
        return NULL;
      }
      if (value_is_type(v, TYPE_KIND_TYPE)) return value_as(v, const type_t *);
      diag_error(sema->diag, sema_loc(sema, type_expr),
                 "sizeof/alignof is a value, not a type");
      return NULL;
    }
    case AST_PTR: {
      /* 裸 *T；所有权与内建 RC 修饰符都已移除。 */
      ast_ptr_t *ptr = (ast_ptr_t *)type_expr;
      if (ptr->ownership) {
        diag_error(sema->diag, sema_loc(sema, type_expr),
                   "ownership/RC modifiers were removed; use '*T'");
        return NULL;
      }
      sema->opaque_base_ctx++;
      const type_t *base = resolve_type_expr(sema, ptr->base_type);
      sema->opaque_base_ctx--;
      if (!base) return NULL;
      return type_ptr_intern(sema->vm, base);
    }
    case AST_SLICE_TYPE: {
      /* 裸 []T。 */
      ast_slice_type_t *sl = (ast_slice_type_t *)type_expr;
      if (sl->ownership) {
        diag_error(sema->diag, sema_loc(sema, type_expr),
                   "ownership modifiers were removed; use '[]T'");
        return NULL;
      }
      const type_t *elem = resolve_type_expr(sema, sl->elem_type);
      if (!elem) return NULL;
      return type_slice_intern(sema->vm, elem);
    }
    case AST_SCOPE_ANNOT: {
      /* 作用域标注（m3-design §5）：'<a,b,c> type。标注不是类型的一部分，
         是绑定在 value 上的位置信息（Step A 静默：只解析存储，不检查）。
         解析直接穿透到被标注类型。 */
      ast_scope_annot_t *sa = (ast_scope_annot_t *)type_expr;
      return resolve_type_expr(sema, sa->sub);
    }
    case AST_OWNED_TYPE:
      return resolve_type_expr(sema, ((ast_owned_type_t *)type_expr)->base_type);
    case AST_PACK_TYPE: {
      ast_pack_type_t *pk = (ast_pack_type_t *)type_expr;
      diag_error(sema->diag, sema_loc(sema, type_expr),
                 "parameter pack '...%.*s' is only valid as the last parameter "
                 "of a generic function",
                 (int)pk->name.len, pk->name.ptr);
      return NULL;
    }
    case AST_INDEX: {
      /* M7 泛型实例化：Type[arg, ...]（parser 与数组下标共用 AST_INDEX，
         类型槽位里只可能是泛型实例化）。类型上下文**不推导**（设计 §4），
         实参必须有显式类型实参。 */
      ast_index_t *ix = (ast_index_t *)type_expr;
      sema_symbol_t *gs = NULL;
      strslice_t gname = STRSLICE_EMPTY;
      bool cross = false;
      if (ix->object && ix->object->kind == AST_IDENT) {
        gname = ((ast_ident_t *)ix->object)->name;
        gs = sema_scope_find_local(sema->global_scope, gname);
      } else if (ix->object && ix->object->kind == AST_SCOPE_REF) {
        cross = true;
        ast_scope_ref_t *sr = (ast_scope_ref_t *)ix->object;
        gname = sr->variant;
        gs = sema_cross_generic_symbol(sema, ix->object);
        if (!gs) {
          diag_error(sema->diag, sema_loc(sema, type_expr),
                     "'%.*s' is not an exported generic type of the imported "
                     "module",
                     (int)sr->variant.len, sr->variant.ptr);
          return NULL;
        }
      }
      if (gs && gs->is_generic) {
        if (gs->kind == SEMA_SYM_INTERFACE) {
          diag_error(sema->diag, sema_loc(sema, type_expr),
                     "'%.*s' is an interface (a compile-time constraint), "
                     "not a type",
                     (int)gname.len, gname.ptr);
          return NULL;
        }
        if (!gs->ast || (gs->ast->kind != AST_STRUCT_DEF &&
                         gs->ast->kind != AST_ENUM_DEF &&
                         gs->ast->kind != AST_UNION_DEF &&
                         gs->ast->kind != AST_TYPE_DEF)) {
          diag_error(sema->diag, sema_loc(sema, type_expr),
                     "'%.*s' is a generic function, not a type",
                     (int)gname.len, gname.ptr);
          return NULL;
        }
        size_t n = 0;
        sema_generic_arg_t *args = NULL;
        bool ok = sema_generic_parse_explicit_args(sema, gs, ix->indices, &n,
                                                   &args);
        const type_t *t =
            ok ? sema_generic_instantiate_type(sema, gs, args, n, type_expr,
                                               NULL)
               : NULL;
        if (args) allocator_free(sema->vm->alloc, (void **)&args);
        return t;
      }
      (void)cross;
      diag_error(sema->diag, sema_loc(sema, type_expr),
                 "unsupported type expression (index on a non-generic type)");
      return NULL;
    }
    default: {
      /* `mod::Type`：跨模块具名类型引用（非泛型；泛型走上面的 AST_INDEX
         形式 `mod::Type[args]`）。 */
      if (type_expr->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *sr = (ast_scope_ref_t *)type_expr;
        if (sr->type_expr && sr->type_expr->kind == AST_IDENT) {
          sema_symbol_t *ms = sema_scope_find_local(
              sema->global_scope, ((ast_ident_t *)sr->type_expr)->name);
          if (!ms || ms->kind != SEMA_SYM_MODULE || !ms->module) {
            diag_error(sema->diag, sema_loc(sema, type_expr),
                       "'%.*s' is not an imported module",
                       (int)((ast_ident_t *)sr->type_expr)->name.len,
                       ((ast_ident_t *)sr->type_expr)->name.ptr);
            return NULL;
          }
          sema_symbol_t *es =
              sema_module_export_symbol(ms->module, sr->variant);
          if (!es || es->kind != SEMA_SYM_TYPE) {
            diag_error(sema->diag, sema_loc(sema, type_expr),
                       "module has no exported type '%.*s'",
                       (int)sr->variant.len, sr->variant.ptr);
            return NULL;
          }
          if (es->is_generic) {
            diag_error(sema->diag, sema_loc(sema, type_expr),
                       "generic type '%.*s' requires type arguments "
                       "(`mod::Name[args]`)",
                       (int)sr->variant.len, sr->variant.ptr);
            return NULL;
          }
          if (!es->type) {
            diag_error(sema->diag, sema_loc(sema, type_expr),
                       "exported type '%.*s' is not materialized",
                       (int)sr->variant.len, sr->variant.ptr);
            return NULL;
          }
          return es->type;
        }
      }
      /* 类型表达式里的编译期计算（`(T extends own *? ? fatal T : T)` 等）：
         交给 CTFE 真实求值，结果必须是 type value。 */
      ctfe_ctx_t ctx;
      memset(&ctx, 0, sizeof ctx);
      ctx.vm = sema->vm;
      ctx.sema = sema;
      ctx.sema_scope = sema->global_scope;
      ctx.budget = 100000;
      ctx.max_depth = 128;
      value_t *sh = ctfe_eval(&ctx, type_expr);
      if (sh && !value_is_error(sema->vm, sh) &&
          value_is_type(sh, TYPE_KIND_TYPE)) {
        return value_as(sh, const type_t *);
      }
      /* M2 扩展点：元组/func 类型表达式 + 类型计算 */
      diag_error(sema->diag, sema_loc(sema, type_expr),
                 "unsupported type expression");
      return NULL;
    }
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
 * 复合类型组装用 value->data 而非名字），消除运行时作用域查找。
 *
 * M3 作用域标注（m3-design §5）：标注不是类型的一部分，是绑定在 value 上
 * 的位置信息——AST_SCOPE_ANNOT 节点**保留在槽位**（只折叠 sub 为
 * AST_TYPE_REF），标注集合（names/count）在折叠后仍可从节点读取。sema 的
 * §5/§6 检查与 compiler 的 compile_type_expr（AST_SCOPE_ANNOT 穿透分支）
 * 均从保留节点消费标注。 */
void sema_annot_peek(ast_node_t *type_expr, strslice_t **names, size_t *count) {
  if (names) *names = NULL;
  if (count) *count = 0;
  if (type_expr && type_expr->kind == AST_SCOPE_ANNOT) {
    ast_scope_annot_t *sa = (ast_scope_annot_t *)type_expr;
    if (names) *names = sa->names;
    if (count) *count = sa->count;
  }
}

/* §5 强制规则：**返回值是 ref 必标注作用域**。
   `fn->return_expr` 已折叠（标注节点保留，sema_annot_peek 可读）。未标注 → 编译错误
   ——调用点无法确定返回值的存活上限，借用会逃逸。

   判据是**返回位的 ownership**（`return_ownership`，须在 `sema_resolve_type_slot`
   折叠 AST 之前从 token 提取）：`ref` 位置未标注即报错。
   注：`'<a> ref *T` 是**ref 产生式**（§0.3）——调用点拿到 REF 状态的值，
   只能流向 `ref` 位置，不可 move。 */
bool sema_check_ret_ref_annot(sema_t *sema, ast_func_def_t *fn,
                              const type_t *rt) {
  if (!rt || !fn->return_expr) return true;
  ownership_t return_ownership = type_ownership_of(rt);
  /* **返回位只允许 `ref` / `fatal` / `none`**（所有权模型 §0.4）：
     `own` 不能作返回标注——调用点拿到的是"调用表达式"的值，
     若返回位是 `own`，该值状态是 OWN，而 OWN 只能流向 `ref` 位置
     （既不能给 own、也不能给 NONE），等于把一个无处安放的状态交给调用方。
     要交所有权请标 `fatal`（调用点即可用 own 位置接收）。 */
  if (return_ownership == OWN_OWN) {
    char tn[64];
    sema_type_name(rt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, fn->return_expr),
               "function '%.*s' cannot return 'own %s': a return type can only "
               "be ref, fatal or none (use 'fatal %s' to hand ownership to the "
               "caller, or 'ref *%s' to lend it)",
               (int)fn->name.len, fn->name.ptr, tn, tn, tn);
    return false;
  }
  /* 含 own 槽位的聚合（如 `<own *T>`）按值返回必须显式 fatal：调用点
     拿到的是将亡值，裸类型会让调用方无法接管，返回临时也无人释放。 */
  if (return_ownership == OWN_NONE && type_has_own_slots(rt)) {
    char tn[64];
    sema_type_name(rt, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, fn->return_expr),
               "function '%.*s' cannot return %s by value: its type contains "
               "own slots; declare the return type as 'fatal %s' so the caller "
               "takes ownership",
               (int)fn->name.len, fn->name.ptr, tn, tn);
    return false;
  }
  if (return_ownership != OWN_REF) return true;
  strslice_t *names = NULL;
  size_t count = 0;
  sema_annot_peek(fn->return_expr, &names, &count);
  if (count == 0) {
    diag_error(sema->diag, sema_loc(sema, fn->return_expr),
               "function '%.*s' returns a ref but does not declare its "
               "scope (annotate the return type, e.g. '<p> ref *T)",
               (int)fn->name.len, fn->name.ptr);
    return false;
  }
  return true;
}

const type_t *sema_resolve_type_slot(sema_t *sema, ast_node_t **slot) {
  if (!sema || !slot || !*slot) return NULL;
  ast_node_t *old = *slot;
  const type_t *t = resolve_type_expr(sema, old);
  if (!t) return NULL;
  const sema_type_t *st = sema_type_find(sema, t);
  if (!st && t->name.ptr == NULL) return t; /* 无名类型（异常防御）不替换 */

  /* 标注节点保留：只折叠 sub 为 AST_TYPE_REF，外层 AST_SCOPE_ANNOT 原位
     保留（标注是位置信息，随槽位节点存活到编译期，供检查消费）。 */
  if (old->kind == AST_SCOPE_ANNOT) {
    ast_scope_annot_t *sa = (ast_scope_annot_t *)old;
    ast_node_t *sub = sa->sub;
    ast_node_t *sub_ref =
        ast_type_ref_new(sema->arena, sub->tok_begin, sub->tok_end);
    if (!sub_ref) return NULL;
    ((ast_type_ref_t *)sub_ref)->name = st ? st->name : t->name;
    sub_ref->next = sub->next; /* 保留兄弟链 */
    sa->sub = sub_ref;         /* 标注节点保留，sub 折叠为 AST_TYPE_REF */
    return t;
  }

  ast_node_t *ref = ast_type_ref_new(sema->arena, old->tok_begin, old->tok_end);
  if (!ref) return NULL;
  ((ast_type_ref_t *)ref)->name = st ? st->name : t->name;
  ref->next = old->next; /* 保留兄弟链 */
  *slot = ref;
  return t;
}
