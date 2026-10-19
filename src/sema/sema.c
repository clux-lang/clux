#include "sema/sema.h"
#include "sema/struct_compose.h" /* 结构体组合（`...Base;`）pass */
#include "vm/type_cfunc.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
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
#include "parser/ast_import.h"
#include "parser/ast_reexport.h"
#include "parser/ast_extern_lib.h"
#include "vm/module.h"
#include "vm/ffi_lib.h"   /* M6 FFI：§2 C ABI 类型校验 */
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
  sema->closures = vec_new(vm->alloc, false);
  sema->types = vec_new(vm->alloc, false); /* 元素手动释放（sema_type_t 无 dispose） */
  sema->fn.return_type = NULL;
  sema->fn.local_func_base = NULL;
  sema->fn.local_func_param_scope = NULL;
  sema->fn.in_call_callee = false;
  sema->fn.loop_depth = 0;
  sema->func_id_next = FUNC_ID_PROGRAM_BASE;
  /* import 回调默认未注入：单文件/测试场景下 import 语句报「当前编译模式不支持」 */
  sema->imports.resolve = NULL;
  sema->imports.user = NULL;
  return sema;
}

/* 注入 import 解析回调（driver 在 sema_create 后、sema_analyze 前调用）。
   见 sema.h 的 sema_import_resolver_t：sema 借此把"路径解析 + 递归编译"交给
   驱动层，自身保持不依赖 driver（消除 sema → driver 反向依赖）。 */
void sema_set_import_resolver(sema_t *sema, sema_import_resolver_t resolve,
                              void *user) {
  if (!sema) return;
  sema->imports.resolve = resolve;
  sema->imports.user = user;
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
  if ((*sema)->closures) vec_free(alloc, &(*sema)->closures);
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
 * 判据入口现为**符号/值层**：`sema_symbol_t::ownership` 与 `value_ownership()`。
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
  /* 变体构造是**唯一权威**，在 VM 层（vm/type.h 的 type_fatal_variant_of）——
     sema 与运行期必须拿到同一个实例，否则会 intern 出同布局不同身份的 fatal
     副本，转移判定在函数边界上落空。sema 这层只多做一件事：把变体登记进
     sema 类型表（hoist 需要它按名 LOAD_TYPE 物化）。 */
  const type_t *fatal = type_fatal_variant_of(sema->vm, ot);
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

  /* id 全 VM 唯一（type_id_alloc）：vm->types_by_id 是 VM 单表、被所有模块
     共享，若按"模块内下标 + TYPE_ID_PROGRAM_BASE"分配，两个模块的 id 会重叠，
     后编译模块的 DEFINE_TYPE 覆盖先者槽位 → 先者 LOAD_TYPE 取到别的模块类型
     （被导入模块内的 struct 构造会运行期报 "construct: unsupported type"）。 */
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
      ownership_t inner_own = sema_type_expr_ownership(op->sub);
      const type_t *sub = resolve_type_expr(sema, op->sub);
      return sub ? type_option_intern_own(sema->vm, sub, inner_own) : NULL;
    }
    case AST_ARRAY: {
      /* [N]T 数组类型：递归解析基础类型 + 编译期求值边界 N →
         按 (elem_type, length, 元素槽位所有权) 去重 intern。边界槽位支持嵌套
         数组（[2][3]i32 = 元素类型是 [3]i32）。
         元素可以是指针/切片/含 own 槽位的聚合——`[N]own *T` 的 own 记在类型
         描述上（销毁序列 `for i in 0..N` 据此决定是否 FREE `a[i]`）。 */
      ast_array_t *arr = (ast_array_t *)type_expr;
      ownership_t elem_own = sema_type_expr_ownership(arr->base_type);
      const type_t *base = resolve_type_expr(sema, arr->base_type);
      if (!base) return NULL;
      size_t len;
      if (!sema_eval_array_bound(sema, &arr->length, &len)) return NULL;
      return type_array_intern_own(sema->vm, base, len, elem_own);
    }
    case AST_TUPLE: {
      /* <T1,T2,...> 元组类型：逐元素递归解析（可为任意类型表达式，
         含嵌套元组/数组）→ 按（元素类型 + 顺序 + 元素槽位所有权）去重 intern。
         元素可以是指针/切片/含 own 槽位的聚合（销毁序列逐元素展开）。 */
      ast_tuple_t *tp = (ast_tuple_t *)type_expr;
      size_t n = sema_count_siblings(tp->elem_types);
      tuple_elem_t elems_arr[n > 0 ? n : 1];
      size_t i = 0;
      for (ast_node_t *et = tp->elem_types; et; et = et->next, i++) {
        /* 所有权必须在 resolve **之前**取（折叠会丢 ownership token） */
        ownership_t eown = sema_type_expr_ownership(et);
        const type_t *pt = resolve_type_expr(sema, et);
        if (!pt) return NULL;
        elems_arr[i].type   = pt;
        elems_arr[i].offset = 0; /* seal 统一布局 */
        elems_arr[i].ownership = eown;
      }
      return type_tuple_intern(sema->vm, n > 0 ? elems_arr : NULL, n);
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
      /* `cfunc(...)` = **C 函数指针**（无 env）：独立类型，与 func(...) 互不
         兼容（用户要求"不互通"）。只在 FFI 边界出现。 */
      if (ft->is_c_abi)
        return type_cfunc_sig(sema->vm, n > 0 ? params_arr : NULL, n, rt,
                              /*is_variadic=*/false);
      return type_func_sig(sema->vm, n > 0 ? params_arr : NULL, NULL, n, rt,
                           OWN_NONE, /*is_variadic=*/false);
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
      /* 指针类型（m3-design §3）：own *T / ref *T / fatal *T。按所有权修饰
         token 文本分派 type_ptr_intern（三种所有权是独立 intern 实例）。
         递归解析被指向类型 T（可为任意类型表达式，含嵌套指针）。 */
      ast_ptr_t *ptr = (ast_ptr_t *)type_expr;
      /* 所有权分两类：
         - own/ref/fatal：**纯编译期**——校验 token 合法性，不进类型系统
           （ownership 由调用方在 symbol 层记录）；
         - share/weak（§12 RC 系）：**类型层属性**——copy = 计数 +1、drop =
           计数 -1 由指针类型的 vtable 承担，所以这里落到带 rc 的 intern 实例。 */
      strslice_t own = token_strslice(ptr->ownership);
      uint8_t rc = 0;
      if (own.len == 5 && memcmp(own.ptr, "share", 5) == 0)
        rc = 1;
      else if (own.len == 4 && memcmp(own.ptr, "weak", 4) == 0)
        rc = 2;
      else if (!((own.len == 3 && memcmp(own.ptr, "own", 3) == 0) ||
                 (own.len == 3 && memcmp(own.ptr, "ref", 3) == 0) ||
                 (own.len == 5 && memcmp(own.ptr, "fatal", 5) == 0))) {
        diag_error(sema->diag, sema_loc(sema, type_expr),
                   "unsupported pointer ownership");
        return NULL;
      }
      const type_t *base = resolve_type_expr(sema, ptr->base_type);
      if (!base) return NULL;
      return rc ? type_ptr_intern_rc(sema->vm, base, rc)
                : type_ptr_intern(sema->vm, base);
    }
    case AST_SLICE_TYPE: {
      /* 切片类型（m4-design §1/§10）：[]T（所有权纯编译期，类型不区分
         own/ref/fatal）。
         裸 []T（ownership=NULL）不合法——parser 在 parse_primary 层暂存
         为 ownership=NULL 的 AST_SLICE_TYPE，此处校验拒绝。 */
      ast_slice_type_t *sl = (ast_slice_type_t *)type_expr;
      if (!sl->ownership) {
        diag_error(sema->diag, sema_loc(sema, type_expr),
                   "bare '[]T' is not allowed; use 'own []T' / 'ref []T' / "
                   "'fatal []T'");
        return NULL;
      }
      /* 校验 ownership token 合法性（所有权纯编译期，不传入类型系统） */
      strslice_t own = token_strslice(sl->ownership);
      if (!((own.len == 3 && memcmp(own.ptr, "own", 3) == 0) ||
            (own.len == 3 && memcmp(own.ptr, "ref", 3) == 0) ||
            (own.len == 5 && memcmp(own.ptr, "fatal", 5) == 0))) {
        diag_error(sema->diag, sema_loc(sema, type_expr),
                   "unsupported slice ownership '%.*s' (use own/ref/fatal)",
                   (int)own.len, own.ptr);
        return NULL;
      }
      const type_t *elem = resolve_type_expr(sema, sl->elem_type);
      if (!elem) return NULL;
      /* 元素槽位所有权（`own []own *T` 的元素是 own 叶子；销毁序列
         `for i in 0..len` 据此决定是否 FREE `s[i]`）。所有权须在 resolve
         之前取——折叠会丢 ownership token。 */
      ownership_t elem_own = sema_type_expr_ownership(sl->elem_type);
      return type_slice_intern_own(sema->vm, elem, elem_own);
    }
    case AST_SCOPE_ANNOT: {
      /* 作用域标注（m3-design §5）：'<a,b,c> type。标注不是类型的一部分，
         是绑定在 value 上的位置信息（Step A 静默：只解析存储，不检查）。
         解析直接穿透到被标注类型。 */
      ast_scope_annot_t *sa = (ast_scope_annot_t *)type_expr;
      return resolve_type_expr(sema, sa->sub);
    }
    case AST_OWNED_TYPE: {
      /* `fatal T` / `own T`：所有权限定的**非指针**类型。
         按所有权模型（docs/ownership-refactor.md §0）：所有权是**值状态**，
         不进类型——所以类型**解析结果就是 base 类型本身**，限定词只用于
         记录"该位置要求什么状态的值"（由 sema_type_expr_ownership 从 token 取）。
         这里**不再 policing 哪个修饰词能用**：`own i32` 也可解析为 `i32`，
         它是否**可用**由流动规则决定（`own` 位置需要将亡值，`i32` 没有
         FATAL 产生式 → 初始化点自然报错淘汰）。 */
      ast_owned_type_t *ot = (ast_owned_type_t *)type_expr;
      const type_t *base = resolve_type_expr(sema, ot->base_type);
      if (!base) return NULL;
      return base;
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
                              const type_t *rt, ownership_t return_ownership) {
  if (!rt || !fn->return_expr) return true;
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
 * Pass 0：import 语句处理（递归编译被导入模块）
 * =========================================================================== */

/* 取 import / 代理导出声明的路径文本。 */
static strslice_t import_decl_path(ast_node_t *f) {
    if (f->kind == AST_IMPORT) return ((ast_import_t *)f)->path;
    return ((ast_reexport_t *)f)->path;
}

static void pass0_imports(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    /* M6 FFI：extern lib 由 **driver** 处理（在 lexer pool 释放前扫描 AST 并
       dlopen/LoadLibrary，句柄挂 module_t.ffi，失败即诊断）——sema 不再重复
       诊断，但仍要跳过它（它不是可编译的声明）。`extern cfunc` 的符号解析与
       调用属后续阶段。 */
    if (f->kind == AST_EXTERN_LIB) {
      /* M6 FFI：注册库别名符号（kind = SEMA_SYM_EXTERN_LIB），使 `crt::malloc` 能
         解析到别名并在 :: 分支给出"调用未实现"的明确诊断，而不是含混的
         "unknown enum type in enum reference"。加载与符号解析由 driver 负责。 */
      ast_extern_lib_t *el = (ast_extern_lib_t *)f;
      sema_symbol_t init = {0};
      init.kind = SEMA_SYM_EXTERN_LIB;
      init.is_active = true;
      sema_symbol_t *fsym = sema_scope_define(sema->global_scope, el->alias,
                                             &init);
      if (!fsym) {
        diag_error(sema->diag, sema_loc(sema, f),
                   "duplicate name '%.*s' for extern lib alias",
                   (int)el->alias.len, el->alias.ptr);
      }
      continue;
    }
    if (f->kind != AST_IMPORT && f->kind != AST_REEXPORT) continue;
    strslice_t raw = import_decl_path(f);

    /* import 的解析与编译由**驱动层注入的回调**完成：路径分类/规范化、
       存在性检查、递归编译（cache + 循环依赖检测）与诊断都在 driver 侧。
       sema 因此不 include 任何 driver 头、不认识 module_t 的内容、不做路径
       处理——依赖方向保持单向（driver → sema）。 */
    if (!sema->imports.resolve) {
      diag_error(sema->diag, sema_loc(sema, f),
                 "imports are not supported in this compilation mode");
      continue;
    }
    struct module_t *mod =
        sema->imports.resolve(sema->imports.user, sema, f, raw);
    if (!mod) continue; /* 回调已诊断（非相对路径 / 未找到 / 递归编译失败） */

    if (f->kind == AST_IMPORT) {
      /* 在当前模块全局作用域注册 <alias> 为 MODULE 符号。
         （AST_REEXPORT（`export * from "./x"`）无需在此记账：依赖模块已在回调里
         编译并注册进 vm->modules，driver 构建本模块导出表时会重解析该路径并
         并入其导出符号，同名冲突在彼处诊断。） */
      ast_import_t *imp = (ast_import_t *)f;
      sema_symbol_t init = {0};
      init.kind = SEMA_SYM_MODULE;
      init.module = mod;
      init.is_active = true;
      sema_symbol_t *sym = sema_scope_define(sema->global_scope, imp->alias,
                                             &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f),
                   "duplicate name '%.*s' for import alias",
                   (int)imp->alias.len, imp->alias.ptr);
      }
    }
  }
}

/* ===========================================================================
 * Pass 1/2：函数名收集 + 类型解析（func_t 签名）
 * =========================================================================== */

/* 解析 + 校验 + 注册一个 extern cfunc 声明。
 * 返回 false = 有类型还没激活（程序类型未就绪）——`report=false` 时静默返回、
 * 交给 `pass1c_extern_abi` 处理。`report=true` 时才产出诊断（避免同一错误两次）。 */
static bool check_extern_cfunc(sema_t *sema, ast_func_def_t *efd, bool report) {
  const type_t *ptypes[64];
  ownership_t powns[64];
  size_t pn = 0;
  bool resolved = true;
  /* 符号名（限定名 "<lib>::<name>"），既用于注册也用于回查已建签名。 */
  char qual[512];
  int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                    (int)efd->lib_name.len, efd->lib_name.ptr,
                    (int)efd->name.len, efd->name.ptr);
  sema_symbol_t *prior =
      (qn > 0 && (size_t)qn < sizeof(qual))
          ? sema_lookup(sema->global_scope, strslice_from_bytes(qual, (size_t)qn))
          : NULL;
  const func_sig_t *prior_sig =
      (prior && prior->type && prior->type->kind == TYPE_KIND_FUNC)
          ? &((const func_type_t *)prior->type)->sig
          : NULL;

  for (ast_node_t *p = efd->params; p && pn < 64; p = p->next) {
    ast_var_def_t *pvd = (ast_var_def_t *)p;
    /* ownership 时序：本次函数**首次**执行时会折叠 AST（AST_PTR→AST_TYPE_REF），
       折叠后 `sema_type_expr_ownership` 只会得到 OWN_NONE。所以优先从已建签名
       读回，其次用**解析器缓存**的 `pvd->ownership`（折叠前提取，与 vm 的
       ownership_t 同编码），最后才是就地提取。 */
    ownership_t pown =
        (prior_sig && prior_sig->param_ownerships && pn < prior_sig->param_count)
            ? prior_sig->param_ownerships[pn]
            : (pvd->ownership != AST_OWN_NONE
                   ? (ownership_t)pvd->ownership
                   : sema_type_expr_ownership(pvd->type_expr));
    const type_t *pt = sema_resolve_type_slot(sema, &pvd->type_expr);
    if (!pt) {
      resolved = false;
      ptypes[pn] = NULL;
      powns[pn++] = pown;
      continue;
    }
    const char *why = ffi_c_abi_reject_reason_own(pt, pown);
    if (!why && pt->kind == TYPE_KIND_VOID)
      why = "void is not a valid parameter type";
    if (why && report) {
      diag_error(sema->diag, sema_loc(sema, p),
                 "extern cfunc %.*s::%.*s: parameter '%.*s': %s",
                 (int)efd->lib_name.len, efd->lib_name.ptr,
                 (int)efd->name.len, efd->name.ptr,
                 (int)pvd->name.len, pvd->name.ptr, why);
    }
    ptypes[pn] = pt;
    powns[pn] = pown;
    pn++;
  }
  const type_t *rt = sema->vm->type_void;
  ownership_t ret_own = prior_sig
                            ? prior_sig->return_ownership
                            : (efd->return_expr
                                   ? sema_type_expr_ownership(efd->return_expr)
                                   : OWN_NONE);
  if (efd->return_expr) {
    rt = sema_resolve_type_slot(sema, &efd->return_expr);
    if (!rt) {
      resolved = false;
    } else if (report) {
      const char *rwhy = ffi_c_abi_reject_reason_own(rt, ret_own);
      if (rwhy) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)efd),
                   "extern cfunc %.*s::%.*s: return type: %s",
                   (int)efd->lib_name.len, efd->lib_name.ptr,
                   (int)efd->name.len, efd->name.ptr, rwhy);
      }
    }
  }
  if (!resolved) return false;

  /* 注册限定名函数符号 "<lib>::<name>"（`::` 分支据此解析）。
     **不分配 fid**：无函数体、不进运行时函数队列。 */
  if (qn <= 0 || (size_t)qn >= sizeof(qual)) return resolved;
  /* **签名携带真实 ownership**（不是硬编码 OWN_NONE）——它是折叠后唯一还能
     读到所有权的地方，pass1c 的 pass1c_extern_abi 复审依赖它（否则 AST 已折叠
     成 AST_TYPE_REF，ownership token 丢失，`own *T` 会漏过 C ABI 约束）。 */
  const type_t *sig =
      type_func_sig(sema->vm, ptypes, powns, pn,
                    rt ? rt : sema->vm->type_void, ret_own, efd->is_variadic);
  if (sig) {
    /* `.ast` 指向声明节点：调用点需要它取返回类型表达式（程序自定义类型的
       "按类型值返回"要用，见 sema/expr.c 的 FFI 返回类型码映射）。 */
    sema_symbol_t einit = {.kind = SEMA_SYM_FUNC, .type = sig,
                           .ast = (ast_node_t *)efd, .is_active = true,
                           .fid = 0};
    sema_scope_define(sema->global_scope,
                      strslice_from_bytes(qual, (size_t)qn), &einit);
  }
  return resolved;
}

/* 解析 + 校验 + 注册一个 extern var（动态库导出的数据符号）。
 * 与 extern cfunc 对称：限定名变量符号、无本地存储、运行期 EXTERN_LOAD/STORE。 */
static bool check_extern_var(sema_t *sema, ast_var_def_t *evd, bool report) {
  char qual[512];
  int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                    (int)evd->lib_name.len, evd->lib_name.ptr,
                    (int)evd->name.len, evd->name.ptr);
  if (qn <= 0 || (size_t)qn >= sizeof(qual)) {
    if (report) diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                           "extern var name too long");
    return false;
  }
  if (report) {
    sema_symbol_t *libsym = sema_lookup(sema->global_scope, evd->lib_name);
    if (!libsym || libsym->kind != SEMA_SYM_EXTERN_LIB) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                 "extern var %s uses undeclared library '%.*s' "
                 "(add `extern lib %.*s from \"<name>\";`)",
                 qual, (int)evd->lib_name.len, evd->lib_name.ptr,
                 (int)evd->lib_name.len, evd->lib_name.ptr);
    }
  }
  const type_t *vt = sema_resolve_type_slot(sema, &evd->type_expr);
  if (!vt) {
    if (report)
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                 "extern var %s: type could not be resolved", qual);
    return false;
  }
  if (report) {
    const char *why = ffi_c_abi_reject_reason_own(
        vt, evd->ownership != AST_OWN_NONE
                ? (ownership_t)evd->ownership
                : sema_type_expr_ownership(evd->type_expr));
    uint32_t kind = 0;
    if (!why && vt->kind == TYPE_KIND_VOID) why = "void is not a value type";
    if (!why && !ffi_data_kind_of(vt, sema->vm, &kind) &&
        !ffi_data_kind_needs_type(vt))
      why = "type cannot be read/written as an external data symbol";
    if (why)
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                 "extern var %s: %s", qual, why);
  }
  sema_symbol_t einit = {.kind = SEMA_SYM_EXTERN_VAR, .type = vt,
                         .ast = (ast_node_t *)evd, .is_active = true};
  sema_scope_define(sema->global_scope,
                    strslice_from_bytes(qual, (size_t)qn), &einit);
  return true;
}

/* pass1c：程序类型激活之后统一校验 extern 签名（并把 pass1_names 未能登记的
   ——签名里含程序自定义类型的——补登记）。 */
static void pass1c_extern_abi(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind == AST_FUNC_DEF && ((ast_func_def_t *)f)->is_extern_cfunc)
      (void)check_extern_cfunc(sema, (ast_func_def_t *)f, /*report=*/true);
    else if (f->kind == AST_VAR_DEF && ((ast_var_def_t *)f)->is_extern_var)
      (void)check_extern_var(sema, (ast_var_def_t *)f, /*report=*/true);
  }
}

static void pass1_names(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    /* AST_IMPORT / AST_REEXPORT 已在 pass0_imports 处理（注册 MODULE 符号 /
       收集代理导出），跳过 */
    if (f->kind == AST_IMPORT || f->kind == AST_REEXPORT ||
        f->kind == AST_EXTERN_LIB)
      continue;
    if (f->kind == AST_FUNC_DEF && ((ast_func_def_t *)f)->is_extern_cfunc) {
      /* M6 FFI：extern cfunc 声明（无体，符号由 driver 用 dlsym 解析）。
         这里登记**限定名函数符号**，签名校验交给 pass1c（程序自定义类型要等
         pass1b_types 激活后才能 resolve，见 check_extern_cfunc 的说明）。 */
      (void)check_extern_cfunc(sema, (ast_func_def_t *)f, /*report=*/false);
      continue;
    }

    /* M6 FFI：extern var 声明（动态库导出的**数据符号**）。
       与 extern cfunc 对称：注册**限定名变量符号** "<lib>::<name>"，且不进运行时
       变量队列——它没有本地存储，运行期由 dlsym 得到库里的变量地址。
       校验（别名已声明 / 类型可搬运）同样交给 pass1c。 */
    if (f->kind == AST_VAR_DEF && ((ast_var_def_t *)f)->is_extern_var) {
      (void)check_extern_var(sema, (ast_var_def_t *)f, /*report=*/false);
      continue;
    }

    /* 全局 comptime var：注册变量符号（暂不激活，pass_globals 求值后激活）。
       不创建 sema_func_t（不入 Pass 3 队列——不是函数，无函数体）。
       `is_comptime` 必须在**注册时**就置位：类型槽位的延迟判定
       （type_refs_unevaluated_comptime）与按需求值都靠它区分
       "尚未求值的 comptime var" 与普通变量。 */
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_VAR,
                            .is_comptime = vd->is_comptime};
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

    /* 全局 struct 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_STRUCT_DEF) {
      ast_struct_def_t *sd = (ast_struct_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, sd->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)sd->name.len, sd->name.ptr);
      }
      continue;
    }

    /* 全局 union 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_UNION_DEF) {
      ast_union_def_t *ud = (ast_union_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, ud->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)ud->name.len, ud->name.ptr);
      }
      continue;
    }

    /* 全局 cunion 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_CUNION_DEF) {
      ast_cunion_def_t *cd = (ast_cunion_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, cd->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)cd->name.len, cd->name.ptr);
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
    sf->def_scope = sema->global_scope;
      /* fid 单一来源：创建 sema 函数对象即分配（comptime func 亦分配——统一
       流程；compiler 预扫描跳过 comptime 不读取，占位无冲突）。符号表 fid
       字段同步（AST_FUNC_REF 替换 / 折叠按 id 查询用） */
    sema_func_id_alloc(sema, fn);
    sym->fid = fn->fid;
    vec_push(sema->funcs, sema->vm->alloc, sf);
  }
}

/* pass1d：结构体成员注册（struct 体内的 var/func → 成员作用域符号）
 *
 * 成员不再展开为全局 `T::name` 声明。本 pass 为每个有成员的 sealed struct
 * 类型创建一个 sema 子作用域（global_scope 的 child），注册成员符号：
 *   - func 成员 → SEMA_SYM_FUNC + sema_func_t（入 Pass 2/3 队列，与全局函数
 *     同构），fid 由 sema 统一分配
 *   - var 成员 → SEMA_SYM_VAR（comptime var 标记 is_comptime；pass_globals
 *     求值后激活）
 * member_scope 挂在 sema_symbol_t::member_scope 上，供 ex_call 方法调用
 * 与 ex_scope_ref `T::x` 静态访问解析。
 *
 * 调用时机：pass1b_types 之后（所有 struct 已 seal）、pass2_types 之前
 * （方法函数需要先注册符号 + 入队，pass2 才能解析签名）。 */
static void pass1d_struct_members(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind != AST_STRUCT_DEF) continue;
    ast_struct_def_t *sd = (ast_struct_def_t *)d;
    if (!sd->members) continue;

    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, sd->name);
    if (!sym || !sym->is_active || !sym->type) continue; /* 类型未就绪 */

    /* 幂等：试跑轮已创建 member_scope 时跳过（不重复创建/注册） */
    if (sym->member_scope) continue;

    /* 创建成员子作用域（global_scope 的 child） */
    sema_scope_t *mscope = sema_scope_new(sema->vm->alloc, SEMA_SCOPE_GLOBAL,
                                           sema->global_scope);
    if (!mscope) panic("sema: out of memory creating struct member scope");
    sema_scope_add_child(sema->global_scope, mscope);
    sym->member_scope = mscope;

    for (ast_node_t *m = sd->members; m; m = m->next) {
      if (m->kind == AST_FUNC_DEF) {
        ast_func_def_t *fn = (ast_func_def_t *)m;
        sema_symbol_t init = {.kind = SEMA_SYM_FUNC};
        sema_symbol_t *msym = sema_scope_define(mscope, fn->name, &init);
        if (!msym) {
          diag_error(sema->diag, sema_loc(sema, m),
                     "struct '%.*s': duplicate method '%.*s'",
                     (int)sd->name.len, sd->name.ptr,
                     (int)fn->name.len, fn->name.ptr);
          continue;
        }
        msym->is_active = true;
        msym->is_comptime = fn->is_comptime;
        /* 登记 sema 层函数对象（与全局函数同构） */
        sema_func_t *sf = allocator_new_ex(sema->vm->alloc, "sema_func_t",
                                           sizeof(sema_func_t), NULL, NULL, NULL,
                                           1);
        if (!sf) panic("sema: out of memory allocating struct method sema_func");
        sf->def = m;
        sf->scope = NULL;
        sf->param_scope = NULL;
        sf->def_scope = mscope; /* struct 方法符号在 member_scope 中 */
        sema_func_id_alloc(sema, fn);
        msym->fid = fn->fid;
        vec_push(sema->funcs, sema->vm->alloc, sf);
      } else if (m->kind == AST_VAR_DEF) {
        ast_var_def_t *vd = (ast_var_def_t *)m;
        sema_symbol_t init = {.kind = SEMA_SYM_VAR,
                              .is_comptime = vd->is_comptime};
        sema_symbol_t *msym = sema_scope_define(mscope, vd->name, &init);
        if (!msym) {
          diag_error(sema->diag, sema_loc(sema, m),
                     "struct '%.*s': duplicate attribute '%.*s'",
                     (int)sd->name.len, sd->name.ptr,
                     (int)vd->name.len, vd->name.ptr);
          continue;
        }
        /* comptime var 成员必须 const：标记后赋值时拦截（TODO: 赋值拦截） */
      }
    }
  }
}

static void pass2_types(sema_t *sema, bool speculative) {
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    ast_node_t *f = sf->def;
    ast_func_def_t *fn = (ast_func_def_t *)f;
    /* 符号定义所在作用域：全局函数在 global_scope（sema_lookup 沿 parent 链），
       struct 方法在类型的 member_scope（sema_scope_find_local 只查本层）。 */
    sema_scope_t *def_scope = sf->def_scope ? sf->def_scope : sema->global_scope;
    sema_symbol_t *sym = (def_scope == sema->global_scope)
                             ? sema_lookup(sema->global_scope, fn->name)
                             : sema_scope_find_local(def_scope, fn->name);
    if (!sym) continue;
    if (sym->ast && sym->ast != f) continue; /* 理论上不可达：队列只含有效函数 */

    /* 注意：**不能**在函数入口用 "签名已解析" 短路——§5 返回 ref 标注校验
       （sema_check_ret_ref_annot）每次都要跑：试跑轮里它写进临时缓冲被丢弃，
       正式轮必须补报。槽位解析本身幂等（首次解析后槽位已折叠为 AST_TYPE_REF）。 */

    /* 参数/返回类型槽位解析。**未解析成功则不建签名**（留待下一轮试跑或
       最终正式报错）——否则会用 NULL 参数构造出半有效的签名并把它标记为
       已完成，后续轮次不再重试。
       ownership 时序：speculative 轮会折叠 AST（AST_PTR→AST_TYPE_REF），
       正式轮再从 AST 提取会得到 OWN_NONE。若签名已建（sym->type 非空），
       从签名的 param_ownerships/return_ownership 读取；否则从原始 AST 提取。 */
    bool unresolved = false;
    size_t nparams = sema_count_siblings(fn->params);
    const type_t **params = NULL;
    ownership_t *powns = NULL;
    const func_sig_t *existing_sig = (sym->type && sym->type->kind == TYPE_KIND_FUNC)
                                         ? &((const func_type_t *)sym->type)->sig
                                         : NULL;
    if (nparams > 0) {
      params = allocator_new_ex(sema->vm->alloc, "type_t*", sizeof(type_t *),
                                NULL, NULL, NULL, nparams);
      powns = allocator_new_ex(sema->vm->alloc, "ownership_t", sizeof(ownership_t),
                               NULL, NULL, NULL, nparams);
      size_t j = 0;
      for (ast_node_t *p = fn->params; p; p = p->next, j++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        ownership_t pown = (existing_sig && existing_sig->param_ownerships &&
                            j < existing_sig->param_count)
                               ? existing_sig->param_ownerships[j]
                               : sema_type_expr_ownership(vd->type_expr);
        const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t) {
          if (!speculative)
            diag_error(sema->diag, sema_loc(sema, p),
                       "unknown type in parameter '%.*s'",
                       (int)vd->name.len, vd->name.ptr);
          unresolved = true;
        }
        params[j] = t; /* 失败置 NULL，位置对齐，func_shadow_call 校验时跳过 */
        powns[j] = pown;
      }
    }

    ownership_t ret_own = existing_sig
                              ? existing_sig->return_ownership
                              : (fn->return_expr ? sema_type_expr_ownership(fn->return_expr) : OWN_NONE);
    const type_t *rt = NULL;
    if (fn->return_expr) {
      rt = sema_resolve_type_slot(sema, &fn->return_expr);
      if (!rt) {
        if (!speculative)
          diag_error(sema->diag, sema_loc(sema, f), "unknown return type");
        unresolved = true;
      }
    }
    if (unresolved) {
      if (params) allocator_free(sema->vm->alloc, (void **)&params);
      if (powns) allocator_free(sema->vm->alloc, (void **)&powns);
      continue; /* 保持未就绪，等下一轮（符号 type 仍为 NULL） */
    }
    /* §5 强制规则（m3-design §5）：返回 ref 必标注（集合 = 参数名 ∪ {*}）。
       未标注 → 编译错误（调用点无法确定返回值存活上限）。
       **每次都要跑**：签名可能已在试跑轮解析完毕（下面会跳过重建），但试跑轮
       的诊断进了临时缓冲被丢弃——正式轮必须补报；试跑轮里也只是做检查、
       不产生状态。 */
    sema_check_ret_ref_annot(sema, fn, rt, ret_own);
    if (sym->type) { /* 签名已就绪（试跑轮已解析）：无需重建 */
      if (params) allocator_free(sema->vm->alloc, (void **)&params);
      if (powns) allocator_free(sema->vm->alloc, (void **)&powns);
      continue;
    }

    /* 注册签名类型（按签名去重 intern 到 vm 类型池，type_func_sig 复制 params）；
       符号统一记录定义 AST 节点（函数 = AST_FUNC_DEF）；签名类型存于
       sym->type，调用点经 value_make_shadow(vm, sym->type) 构造 shadow callee。
       签名类型**登记进 sema->types**（与数组/const 同机制）——签名本质是
       普通类型，且函数指针作参数时签名引用签名，须经 hoist 两遍扫描
       依赖后序构造（参数/返回类型已在此前解析登记，父先入队）。 */
    const type_t *sig = type_func_sig(sema->vm, params, powns, nparams, rt, ret_own, false);
    sym->type = sig;
    sym->ast = f;
    /* 签名类型提升：登记 + 递归依赖登记；fn->sig_id 记录（compiler
       compile_func_reg 发 LOAD_TYPE <sig_id>，不再作用域查找） */
    const sema_type_t *st = sema_type_register(sema, sig);
    if (st) fn->sig_id = st->id;

    /* params/powns 临时数组已被 type_func_sig 复制，此处释放 */
    if (params) allocator_free(sema->vm->alloc, (void **)&params);
    if (powns) allocator_free(sema->vm->alloc, (void **)&powns);
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
 *
 * `speculative`：试跑轮（诊断进临时缓冲、不报真错）。两种模式都**跳过已激活
 * 的符号**，因此可反复调用（求值顺序无关的不动点轮次）。
 * =========================================================================== */

static void pass1b_types(sema_t *sema, ast_program_t *prog, bool speculative) {
  (void)speculative;
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    strslice_t name = {0};
    switch (f->kind) {
      case AST_TYPE_DEF:   name = ((ast_type_def_t *)f)->name; break;
      case AST_ENUM_DEF:   name = ((ast_enum_def_t *)f)->name; break;
      case AST_STRUCT_DEF: name = ((ast_struct_def_t *)f)->name; break;
      case AST_UNION_DEF:  name = ((ast_union_def_t *)f)->name; break;
      case AST_CUNION_DEF: name = ((ast_cunion_def_t *)f)->name; break;
      default: continue;
    }
    /* 已求值（激活）→ 跳过：使本函数可反复调用（不动点轮次） */
    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, name);
    if (sym && sym->is_active) continue;

    if (f->kind == AST_TYPE_DEF) {
      sema_eval_type_def(sema, (ast_type_def_t *)f, sema->global_scope);
    } else if (f->kind == AST_ENUM_DEF) {
      /* 与全局 type def 同 pass：enum 类型先于函数签名解析（签名/变量
         可引用 enum 类型）。定义点保留（进字节码，运行时 hoist 构造）。 */
      sema_eval_enum_def(sema, (ast_enum_def_t *)f, sema->global_scope);
    } else if (f->kind == AST_STRUCT_DEF) {
      /* 与全局 type def 同 pass：struct 类型先于函数签名解析（签名/变量
         可引用 struct 类型）。定义点保留（进字节码，运行时 hoist 构造）。
         实现已收敛到 struct_compose.c 的 sema_struct_compose_pass——它按
         "嵌入依赖优先"反复遍历未求值的定义，因此 `...Base` 的 Base 写在
         Test 之后也能解析。**结构体组合发生在类型 seal 之前**：组合入口在
         开放对象窗口里插字段 + 登记嵌入表，然后只 seal 一次。整条链一次
         处理完，循环继续处理后面的定义（type/union/cunion）。 */
      sema_struct_compose_pass(sema, prog, sema->global_scope);
    } else if (f->kind == AST_UNION_DEF) {
      /* 与全局 type def 同 pass：union 类型先于函数签名解析（签名/变量
         可引用 union 类型）。定义点保留（进字节码，运行时 hoist 构造）。 */
      sema_eval_union_def(sema, (ast_union_def_t *)f, sema->global_scope);
    } else if (f->kind == AST_CUNION_DEF) {
      /* 与全局 type def 同 pass：cunion 类型先于函数签名解析（签名/变量
         可引用 cunion 类型）。定义点保留（进字节码，运行时 hoist 构造）。 */
      sema_eval_cunion_def(sema, (ast_cunion_def_t *)f, sema->global_scope);
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

/* ===========================================================================
 * 求值顺序无关：试跑轮 + 不动点
 *
 * 全局编译期实体之间有真实依赖环，单趟顺序无解：
 *     comptime var N = add(1, 2);   // N 需要 add 的签名（pass2 才解析）
 *     type T = [N]i32;              // T 需要 N（而 type def 在 pass1b 解析）
 *     func f(a: T): void { }        // f 的签名需要 T
 * 因此 pass1b / pass2 / pass_globals 都做成**可反复调用**（各自跳过已就绪的
 * 符号），外面套一轮循环反复"试跑"直到不再有新的实体就绪：
 *   - 每轮诊断进临时缓冲并丢弃（半成品不报错）
 *   - 轮次之间的进展由"就绪实体计数"衡量
 *   - 循环结束后再用**真实诊断**跑一遍，把真正无解的错误报出来
 * 这样 `[N]i32` 之类常量位置不再因为书写顺序而挑食。
 * =========================================================================== */

static void pass_globals(sema_t *sema, ast_program_t *prog, bool speculative);

/* 已就绪的全局编译期实体计数（进展度量）：
   类型/枚举/结构/联合定义与普通全局变量 → 符号已激活；comptime var →
   ct_valid；函数签名 → sym->type 已解析。 */
static size_t sema_ready_count(sema_t *sema, ast_program_t *prog) {
  size_t n = 0;
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    strslice_t name = {0};
    switch (f->kind) {
      case AST_TYPE_DEF:   name = ((ast_type_def_t *)f)->name; break;
      case AST_ENUM_DEF:   name = ((ast_enum_def_t *)f)->name; break;
      case AST_STRUCT_DEF: name = ((ast_struct_def_t *)f)->name; break;
      case AST_UNION_DEF:  name = ((ast_union_def_t *)f)->name; break;
      case AST_CUNION_DEF: name = ((ast_cunion_def_t *)f)->name; break;
      case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)f;
        sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, vd->name);
        if (sym && (vd->is_comptime ? sym->ct_valid : sym->is_active)) n++;
        continue;
      }
      default: continue;
    }
    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, name);
    if (sym && sym->is_active) n++;
  }
  size_t nf = vec_len(sema->funcs);
  for (size_t i = 0; i < nf; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    ast_func_def_t *fn = (ast_func_def_t *)sf->def;
    sema_scope_t *def_scope = sf->def_scope ? sf->def_scope : sema->global_scope;
    sema_symbol_t *sym = (def_scope == sema->global_scope)
                             ? sema_lookup(sema->global_scope, fn->name)
                             : sema_scope_find_local(def_scope, fn->name);
    if (sym && sym->type) n++;
  }
  return n;
}

/* 跑一轮试跑（诊断丢弃），返回本轮新就绪的实体数 */
static size_t sema_speculative_round(sema_t *sema, ast_program_t *prog) {
  size_t before = sema_ready_count(sema, prog);
  diag_buf_t *real = sema->diag;
  diag_buf_t *scratch = diag_buf_new(sema->vm->alloc);
  if (!scratch) return 0; /* 内存不足：交给正式轮报错 */
  sema->diag = scratch;
  pass1b_types(sema, prog, /*speculative=*/true);
  pass1d_struct_members(sema, prog);
  pass2_types(sema, /*speculative=*/true);
  pass_globals(sema, prog, /*speculative=*/true);
  sema->diag = real;
  diag_buf_destroy(&scratch); /* 半成品诊断丢弃，正式轮再报 */
  size_t after = sema_ready_count(sema, prog);
  return after > before ? after - before : 0;
}

static void pass_globals(sema_t *sema, ast_program_t *prog, bool speculative) {
  ast_node_t **prev = &prog->funcs;
  for (ast_node_t *f = prog->funcs; f;) {
    ast_node_t *next = f->next;
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      /* M6 FFI：extern var 没有本地存储（运行期直接读写库里的变量地址），
         既不是 comptime 常量也不需要 init 求值——原样保留进字节码序列，
         但**不**在这里求值/激活任何东西（引用点由 :: 分支直接处理）。 */
      if (vd->is_extern_var) {
        prev = &f->next;
        f = next;
        continue;
      }
      if (vd->is_comptime) {
        /* comptime var：求值编码常量。已求值（ct_valid）跳过——否则会二次
           绑定同名常量到 root_scope（重复定义）。
           节点摘除（"comptime var 不进运行时"）只在**正式轮**做：试跑轮里
           本轮失败、下轮可能成功，摘掉就再也看不到它了。 */
        sema_symbol_t *csym =
            sema_scope_find_local(sema->global_scope, vd->name);
        if (!csym || !csym->ct_valid)
          sema_eval_comptime_var(sema, vd, sema->global_scope);
        if (speculative) prev = &f->next;
        else             *prev = next;
      } else {
        /* 普通全局变量：init 折叠为字面量/函数引用，保留进字节码（运行时
           DEFINE 到 root_scope，函数体可见）。已求值（is_active）跳过。
           失败：正式轮摘除（避免下游误以为全局 var 存在）；试跑轮保留
           （下轮可能就绪）。 */
        sema_symbol_t *gsym =
            sema_scope_find_local(sema->global_scope, vd->name);
        if (!gsym || !gsym->is_active)
          sema_eval_global_var(sema, vd, sema->global_scope);
        gsym = sema_scope_find_local(sema->global_scope, vd->name);
        if (gsym && gsym->is_active) prev = &f->next;
        else if (speculative)        prev = &f->next;
        else                         *prev = next;
      }
    } else {
      prev = &f->next;
    }
    f = next;
  }

  /* struct 成员 var：与全局 var 同构，但在成员作用域（sym->member_scope）中
     求值/激活。成员 var 的 init 求值上下文是成员作用域（静态属性可引用同
     struct 的其他成员/全局符号）。不摘除节点（成员节点挂在 sd->members 上，
     不在 prog->funcs 链中，无需 prev 摘链）。 */
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind != AST_STRUCT_DEF) continue;
    ast_struct_def_t *sd = (ast_struct_def_t *)d;
    if (!sd->members) continue;
    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, sd->name);
    if (!sym || !sym->member_scope) continue;
    sema_scope_t *mscope = sym->member_scope;

    for (ast_node_t *m = sd->members; m; m = m->next) {
      if (m->kind != AST_VAR_DEF) continue;
      ast_var_def_t *vd = (ast_var_def_t *)m;
      if (vd->is_comptime) {
        /* comptime var 成员：求值编码常量（与全局 comptime var 同构） */
        sema_symbol_t *csym = sema_scope_find_local(mscope, vd->name);
        if (!csym || !csym->ct_valid)
          sema_eval_comptime_var(sema, vd, mscope);
      } else {
        /* 普通静态属性：init 折叠为字面量/函数引用，保留进字节码。
           已求值（is_active）跳过。 */
        sema_symbol_t *gsym = sema_scope_find_local(mscope, vd->name);
        if (!gsym || !gsym->is_active)
          sema_eval_global_var(sema, vd, mscope);
      }
    }
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

  /* 预注册 len 内置函数（m4-design §5/§11.3：len(s) -> usize，即 C 的 size_t）。
     与 VM 侧 vm_register_len 对应。len 接受任意可长度的值（数组/切片/str），
     注册为 variadic（0 固定参数）——sema 调用点特判从实参类型校验可长性
     并产出 usize 返回类型（C 的 size_t：随架构变化）。fid 从 vm->functions 按名查询。 */
  {
    const type_t *lsig = type_func_sig(sema->vm, NULL, NULL, 0, vm_type_usize(sema->vm),
                                       OWN_NONE, /*is_variadic=*/true);
    uint32_t lfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 3 &&
            memcmp(bf->name.ptr, "len", 3) == 0) {
          lfid = bf->id;
          break;
        }
      }
    }
    if (lsig) {
      sema_symbol_t linit = {.kind = SEMA_SYM_FUNC, .type = lsig,
                             .is_active = true, .fid = lfid,
                             .builtin_op = SEMA_BUILTIN_LEN};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("len"), &linit);
    }
  }

  /* 预注册 RC 内置函数（share/weak 是真实类型，源码不写这些名字）：
       - __make_share(p)：fatal/own *T → share *T（§12.1 创建通道）
       - __weak_get(w)  ：weak *T → ?share *T（§12.4 原子升级，`.get()` 语法糖） */
  {
    const type_t *vsig = type_func_sig(sema->vm, NULL, NULL, 0,
                                       sema->vm->type_opaque, OWN_NONE,
                                       /*is_variadic=*/true);
    if (vsig) {
      const char *names[2] = {"__make_share", "__weak_get"};
      sema_builtin_op_t ops[2] = {SEMA_BUILTIN_MAKE_SHARE,
                                  SEMA_BUILTIN_WEAK_GET};
      for (int k = 0; k < 2; k++) {
        uint32_t fid = 0;
        if (sema->vm->functions) {
          size_t nf = vec_len(sema->vm->functions);
          for (size_t i = 0; i < nf; i++) {
            func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
            if (bf && bf->name.ptr &&
                bf->name.len == strlen(names[k]) &&
                memcmp(bf->name.ptr, names[k], bf->name.len) == 0) {
              fid = bf->id;
              break;
            }
          }
        }
        sema_symbol_t init = {.kind = SEMA_SYM_FUNC, .type = vsig,
                              .is_active = true, .fid = fid,
                              .builtin_op = ops[k]};
        sema_scope_define(sema->global_scope,
                          strslice_from_bytes((char *)names[k],
                                              strlen(names[k])),
                          &init);
      }
    }
  }

  /* 预注册 __deep_clone 内置函数（运行期对象图深拷贝）：静态类型表达不了
     克隆语义的形态（闭包值 / cunion 被指向值 / 无 prelude 的切片）在 sema
     里改写为对它的调用，因此 CLONE 指令可以彻底移除。变参签名，调用点按实参
     类型精化返回类型（同类型 fatal）。fid 从 vm->functions 按名查询。 */
  {
    const type_t *dsig = type_func_sig(sema->vm, NULL, NULL, 0,
                                       sema->vm->type_opaque, OWN_NONE,
                                       /*is_variadic=*/true);
    uint32_t dfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 12 &&
            memcmp(bf->name.ptr, "__deep_clone", 12) == 0) {
          dfid = bf->id;
          break;
        }
      }
    }
    if (dsig) {
      sema_symbol_t init = {.kind = SEMA_SYM_FUNC, .type = dsig,
                            .is_active = true, .fid = dfid,
                            .builtin_op = SEMA_BUILTIN_DEEP_CLONE};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("__deep_clone"), &init);
    }
  }

  /* 预注册 data 内置函数（C ABI 适配：data(array/slice/str) -> ref *T，取首元素
     指针）。与 VM 侧 vm_register_data 对应。注册为 variadic（0 固定参数）——
     sema 调用点特判按实参类型精化返回类型为 `ref *<元素类型>`（str → ref *u8），
     运行期也按实参类型重新推导。fid 从 vm->functions 按名查询。 */
  {
    const type_t *ref_opaque = type_ptr_intern(sema->vm,
                                               sema->vm->type_opaque);
    const type_t *dsig = ref_opaque
                             ? type_func_sig(sema->vm, NULL, NULL, 0, ref_opaque,
                                             OWN_NONE, /*is_variadic=*/true)
                             : NULL;
    uint32_t dfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 4 &&
            memcmp(bf->name.ptr, "data", 4) == 0) {
          dfid = bf->id;
          break;
        }
      }
    }
    if (dsig) {
      sema_symbol_t dinit = {.kind = SEMA_SYM_FUNC, .type = dsig,
                             .is_active = true, .fid = dfid,
                             .builtin_op = SEMA_BUILTIN_DATA};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("data"), &dinit);
    }
  }

  /* 预注册 cfunc_ptr 内置函数（M6 Phase 5：cfunc_ptr(f) -> opaque，C 函数指针）。
     与 VM 侧 vm_register_cfunc_ptr 对应。参数是任意函数值（cfunc 定义），
     注册为 variadic（0 固定参数）——具体 §2 校验在运行期由 cfunc 内部完成
     （与 len 同一套路），返回类型 opaque。fid 从 vm->functions 按名查询。 */
  {
    const type_t *csig = type_func_sig(sema->vm, NULL, NULL, 0,
                                       sema->vm->type_opaque,
                                       OWN_NONE, /*is_variadic=*/true);
    uint32_t cfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 10 &&
            memcmp(bf->name.ptr, "cfunc_ptr", 10) == 0) {
          cfid = bf->id;
          break;
        }
      }
    }
    if (csig) {
      sema_symbol_t cinit = {.kind = SEMA_SYM_FUNC, .type = csig,
                             .is_active = true, .fid = cfid,
                             .builtin_op = SEMA_BUILTIN_CFUNC_PTR};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("cfunc_ptr"), &cinit);
    }
  }

  pass0_imports(sema, prog);
  pass1_names(sema, prog);

  /* 全局编译期实体按**不动点轮次**求值（求值顺序无关）：
     type def / 函数签名 / comptime var / 全局变量之间的依赖没有固定拓扑序
     ——`comptime var N = add(1,2); type T = [N]i32;` 里 N 需要 add 的签名
     （pass2）、T 需要 N（pass1b 阶段）、可能还有 `func f(a: T)` 需要 T。
     先反复试跑（诊断丢弃）到不再有新实体就绪，再用真实诊断跑一遍报错。 */
  for (int round = 0; round < 8; round++) {
    if (sema_speculative_round(sema, prog) == 0) break;
  }
  /* 试跑轮的诊断进临时缓冲被丢弃，但"环已诊断"标记会留下来；复位它，正式轮
     才会重新检测并报出循环嵌入（否则环错误只存在于被丢弃的缓冲区里）。 */
  sema_struct_compose_reset_reports(prog);
  pass1b_types(sema, prog, /*speculative=*/false); /* 全局 type def */
  pass1c_extern_abi(sema, prog); /* extern cfunc/var 签名校验（程序类型已激活） */
  pass1d_struct_members(sema, prog); /* struct 成员注册（方法入队 + 属性符号） */
  pass2_types(sema, /*speculative=*/false);         /* 函数签名 */
  pass_globals(sema, prog, /*speculative=*/false);  /* comptime var + 全局变量 */

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

  /* Pass 3b 期间按需生成的销毁函数（每个含 own 槽位的复合类型一个）：统一追加到
     程序函数链尾部 —— compiler 预扫描为它们构造函数对象 + 全局名字绑定，
     销毁点发的 `CALL __destroy_<n>` 才能解析到。 */
  /* 闭包 env 的 vtable：为每个含捕获的函数生成 `__destroy_<env>`（释放 env 的
     own 捕获）与 `__clone_<env>`（深拷贝 env），把 fid 记到 AST 上——compiler
     在 hoist 随 `SET_FUNC_CAPTURES` 一起写进函数对象，于是闭包的 clone/free
     走**编译期生成的函数**，不需要运行期类型内省（也不必用 __deep_clone）。 */
  if (!diag_has_error(sema->diag) && sema->closures) {
    size_t ncl = vec_len(sema->closures);
    for (size_t i = 0; i < ncl; i++) {
      ast_func_def_t *fd = (ast_func_def_t *)vec_get(sema->closures, i);
      if (!fd || fd->capture_type_id == 0) continue;
      const sema_type_t *ct = sema_type_by_id(sema, fd->capture_type_id);
      if (!ct || !ct->type) continue;
      uint32_t dfid = 0, cfid = 0;
      if (sema_gen_env_vtable(sema, ct->type, &dfid, &cfid)) {
        fd->env_dispose_fid = dfid;
        fd->env_clone_fid = cfid;
      }
    }
  }
  if (!diag_has_error(sema->diag)) {
    sema_attach_destroy_fns(sema, program);
    sema_attach_clone_fns(sema, program);
  }

  return !diag_has_error(sema->diag);
}
