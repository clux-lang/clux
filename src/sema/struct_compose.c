#include "sema/struct_compose.h"

#include "core/strslice.h"
#include "parser/ast_array.h"
#include "parser/ast_block.h"
#include "parser/ast_const.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_option.h"
#include "parser/ast_program.h"
#include "parser/ast_ptr.h"
#include "parser/ast_scope_annot.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_struct_embed.h"
#include "parser/ast_tuple.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_volatile.h"
#include "sema/comptime.h"
#include "sema/sema.h"
#include "sema/symbol.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_cunion.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 结构体组合（`...Base;`）——**目标类型 seal 之前**的类型层字段组合
 *
 * 语法面（parser）：`...Base;` 只是 AST_STRUCT_EMBED 节点，记录"名字"与
 * "出现在自己第几个自有字段之前"（field_index）；AST 里**不**拼字段。
 *
 * 用户规格（两句话，本次实现的骨架）：
 *   (1) "这个结构体组合构建类型应该在 sema 后阶段，不是语法树嫁接，而是遍历
 *       旧的 type（Base）复制到新的 type（Test）中，因为需要考虑未来的装饰器
 *       会在 sema 创建 type 后修改 type。"——组合的输入是 Base 的 **type**
 *       字段表快照（Base 已 sealed，装饰器对它的修改都已落在字段表里）。
 *   (2) "新 struct 在聚合后的 attribute 可以覆盖原来的（重写，但是字段不允许
 *       覆盖，冲突报错），也就是一条隐含的规则，在 seal 之前可以复写编辑类型
 *       的 attribute。"——类型的属性/元数据只能在 **seal 之前**编辑；seal 之后
 *       不可变。因此组合必须发生在 Test 的类型**还是开放对象时**，然后只 seal
 *       一次（见 include/vm/type_struct.h 的不变量与 VM 写入 API 的 sealed 拒绝）。
 *
 * 因此本模块的求值流程（sema_struct_compose_def）是：
 *     自有字段槽位解析（未就绪 → 静默延后）
 *   → 逐 `...Base` 解析成 **已 sealed** 的 struct 类型（尚未 seal 就地先求值；
 *     环 → 报错）
 *   → 字段冲突检查（自有字段 vs 嵌入字段、嵌入 vs 嵌入：**字段不允许覆盖**）
 *   → type_struct_alloc_open 建开放类型 → 逐自有字段 type_struct_add_field
 *   → type_struct_insert_embed_fields（按 field_index 插入嵌入字段表）
 *   → type_struct_set_embeds（登记嵌入表，成员继承查找用）
 *   → type_struct_seal（**只此一次**）→ 登记 / 绑定 / 激活符号
 *
 * 语义推论：组合后的 Test 字段表 == "手写同样字段"的 struct，因此
 *   - `...Base` 在开头 ⟹ Test 的字段前缀 == Base 的字段 ⟹ 既有的结构体指针
 *     向上转换（struct_type_prefix_compatible）自然生效，`t.getId()` 可用；
 *   - `...Base` 前面还有字段 ⟹ 前缀不成立 ⟹ `t.getId()` **自然编译失败**。
 * 位置限制不需要任何特殊判断——它由字段表 + 已有的向上转换规则自然产生。
 *
 * 成员（`var` 静态属性 / `func` 方法）**不复制**：`Base` 的成员只有一份
 * （`Base::name`）。`Test` 通过嵌入链**继承名字查找**（下方的
 * `sema_struct_member_lookup`：方法糖 `t.f()` 与限定访问 `Test::f` 共用）；
 * 而 `Test` **自己声明**的同名 attribute 会**静默覆盖**继承来的那一份
 * （命中自己即返回，不再沿嵌入链找）——attribute = 结构体的字段 + `var` 静态
 * 属性 + `func` 方法；**字段是唯一例外**（不参与覆盖，同名一律报错）。
 * =========================================================================== */

/* 嵌入链在一次递归求值中的最大嵌套深度 / 成员来源收集上限
 * （防御病态输入；正常代码远小于此） */
#define COMPOSE_MAX_DEPTH 64
#define COMPOSE_MAX_SRC   64

/* 字段名查重表条目：来源（自有字段 / 嵌入结构体）用于给出准确诊断 */
typedef struct {
  strslice_t          name;
  const struct_type_t *owner; /* NULL = 自有字段 */
} compose_name_t;

bool ast_struct_def_has_embed(const ast_struct_def_t *sd) {
  return sd && sd->embeds != NULL;
}

/* 全局名字查找（结构体名与成员限定名都是全局名字）。
 * 局部结构体（块内 `struct X {...}`）的 token 关键字已由 pass1 展开成全局
 * 声明，因此这里查全局作用域即可。 */
static sema_symbol_t *find_global(sema_t *sema, strslice_t name) {
  return sema->global_scope ? sema_scope_find_local(sema->global_scope, name)
                            : NULL;
}

/* ===========================================================================
 * 递归类型（自引用 / 互相引用）
 *
 * 物化（materialize_struct_type）：为目标类型先建**开放**对象 + 分配类型 id +
 * 设显示名 + 绑定编译期类型名——字段类型表达式（`next: ref *const Node` 写在
 * Node 自己体内、或 A/B 互指）此刻就能解析到对象。
 *
 * 构造侧本就支持成环：hoist 的 pass 1 先把**所有**类型 id 声明登记（LOAD_TYPE
 * 可拉回开放对象），pass 2 里指针/切片/func 的密封**不依赖 base 布局**（指针宽、
 * 切片头恒定），结构体 define 对字段类型走 emit_dep_type（指针定义不递归进
 * base）→ 自引用天然成立。sema 侧此前缺的只是"先给环上每个类型一个可命名的
 * 开放对象"，于是自引用一律报 unknown field type。
 *
 * 但"按值内联"的字段仍需要被引用类型**已 seal** 才能定型（struct 自身布局、
 * array/option/tuple/union 的布局都在构造时结算）。因此：
 *   - 允许（布局恒定或可延迟定型）：指针、切片、func 签名；以及 const/volatile/
 *     数组这些"布局由 sub/elem 派生"的包装——后者在 sub seal 后由 VM 侧的
 *     type_sync_derived_layout 就地**延迟定型**；
 *   - 需要 seal：按值内联了尚未 seal 的 struct（含 option/tuple/union/cunion
 *     包装，它们没有延迟定型路径）→ 本轮 DEFERRED；若最终成环，由
 *     report_incomplete_field_cycles 报"类型不完整（递归）"。
 *
 * 判定走 **AST 结构预判**（slot_blocked_by_unsealed），不做任何类型构造——
 * 否则"解析到一半又延后"会把未定型的派生类型（如 ?T）intern 进类型池并折叠进
 * 槽位，下一轮拿到的是同一个未定型对象。
 * =========================================================================== */

/* 命中"按值内联了尚未 seal 的结构体"时的阻塞信息（诊断用） */
typedef struct {
  ast_node_t       *slot;       /* 命中的类型表达式节点（诊断位置） */
  sema_symbol_t    *sym;        /* 尚未 seal 的结构体类型符号 */
  strslice_t        target;     /* 被点名的类型名（诊断用） */
  strslice_t        field;      /* 触发阻塞的字段名（诊断用） */
  bool              behind_ind; /* 命中点是否位于指针/切片/func 之下 */
  bool              wrapped;    /* 命中点外层是否套了 option/tuple/union 包装 */
} compose_block_t;

/* AST 结构预判：类型表达式是否按值内联了一个尚未 seal 的结构体？
 * behind_ind：当前位置在指针/切片/func 之下（布局恒定，struct 只需身份）。
 * wrapped：已经穿过 option/tuple/union/cunion 包装（用于给出准确诊断）。
 * **不创建任何类型对象**（见上面的说明）。 */
static bool slot_blocked_by_unsealed(sema_t *sema, ast_node_t *slot,
                                     bool behind_ind, bool wrapped,
                                     compose_block_t *out) {
  if (!sema || !slot) return false;
  switch (slot->kind) {
    case AST_IDENT:
    case AST_TYPE_REF: {
      strslice_t nm = (slot->kind == AST_IDENT)
                          ? ((ast_ident_t *)slot)->name
                          : ((ast_type_ref_t *)slot)->name;
      if (!nm.ptr) return false;
      sema_symbol_t *sym = find_global(sema, nm);
      if (!sym || sym->kind != SEMA_SYM_TYPE || !sym->type) return false;
      if (sym->type->kind != TYPE_KIND_STRUCT || type_is_sealed(sym->type))
        return false;
      if (behind_ind) return false; /* 指针/切片/func 之下：只需身份（布局恒定） */
      if (out) {
        out->slot       = slot;
        out->sym        = sym;
        out->target     = nm;
        out->behind_ind = behind_ind;
        out->wrapped    = wrapped;
      }
      return true;
    }
    case AST_CONST:
      return slot_blocked_by_unsealed(sema, ((const ast_const_t *)slot)->sub,
                                      behind_ind, wrapped, out);
    case AST_VOLATILE:
      return slot_blocked_by_unsealed(sema, ((const ast_volatile_t *)slot)->sub,
                                      behind_ind, wrapped, out);
    case AST_ARRAY:
      return slot_blocked_by_unsealed(sema, ((const ast_array_t *)slot)->base_type,
                                      behind_ind, wrapped, out);
    case AST_PTR:
      return slot_blocked_by_unsealed(sema, ((const ast_ptr_t *)slot)->base_type,
                                      true, wrapped, out);
    case AST_SLICE_TYPE:
      return slot_blocked_by_unsealed(
          sema, ((const ast_slice_type_t *)slot)->elem_type, true, wrapped, out);
    case AST_FUNC_TYPE: {
      ast_func_type_t *ft = (ast_func_type_t *)slot;
      for (ast_node_t *pr = ft->params; pr; pr = pr->next)
        if (slot_blocked_by_unsealed(sema, pr, true, wrapped, out)) return true;
      return slot_blocked_by_unsealed(sema, ft->return_type, true, wrapped, out);
    }
    case AST_OPTION:
      /* ?T 布局 = struct{bool;T}：**按值**内联 T 的布局，且没有延迟定型路径
         → 只看 T 自身是否需要 seal（重置 behind_ind：包装内部的"按值"语义与
         外层是否在指针之下无关——`?ref *Node` 的 T 是指针 → 不阻塞） */
      return slot_blocked_by_unsealed(sema, ((ast_option_t *)slot)->sub, false,
                                      true, out);
    case AST_TUPLE: {
      ast_tuple_t *tp = (ast_tuple_t *)slot;
      for (ast_node_t *et = tp->elem_types; et; et = et->next)
        if (slot_blocked_by_unsealed(sema, et, false, true, out)) return true;
      return false;
    }
    case AST_SCOPE_ANNOT:
      return slot_blocked_by_unsealed(sema, ((const ast_scope_annot_t *)slot)->sub,
                                      behind_ind, wrapped, out);
    default:
      return false; /* sizeof/typeof/内建标量：交给正常解析路径 */
  }
}

/* 物化：为目标结构体先建开放类型对象（分配 id + 显示名 + 编译期类型名绑定），
 * 使字段类型表达式（自引用/互相引用）能解析到它。幂等：已物化直接返回。
 * **不 seal**——字段表由 sema_struct_compose_def 在同一个开放对象上填充。 */
static const type_t *materialize_struct_type(sema_t *sema, ast_struct_def_t *sd,
                                             sema_symbol_t *sym) {
  if (!sema || !sd || !sym) return NULL;
  if (sym->type) return sym->type; /* 已物化（幂等） */
  vm_t *vm = sema->vm;
  const type_t *t = type_struct_alloc_open(vm);
  if (!t) return NULL;
  /* 具名 struct：独立身份（seal 跳过去重 intern，见 type_struct_mark_unique） */
  type_struct_mark_unique(vm, t);
  /* **物化即入池**：开放对象若一直等不到 seal（错误路径：字段冲突/未知类型/
     递归无解），pool 是它唯一的拥有者——vm_destroy 会释放它（含显示名/字段表）。
     正常路径 seal 时按指针检测跳过重复入池（见 type_struct_seal）。 */
  if (!vm->struct_types) vm->struct_types = vec_new(vm->alloc, false);
  vec_push(vm->struct_types, vm->alloc, (void *)t);
  /* 登记：分配全 VM 唯一类型 id（type_set_name 要求 id >= PROGRAM_BASE） */
  const sema_type_t *st = sema_type_register(sema, t);
  if (!st) return NULL; /* 对象已入池：归 vm 所有，不在此释放 */
  sd->type_id = st->id;
  if (!type_set_name(vm, t, sd->name)) return NULL;
  /* 编译期 vm 作用域绑定：类型表达式按名字解析走 type_lookup（AST_IDENT 分支） */
  value_t *tv = type_as_value(vm, t);
  if (!tv) return NULL;
  char nb[256];
  if (sd->name.len >= sizeof nb) return NULL;
  memcpy(nb, sd->name.ptr, sd->name.len);
  nb[sd->name.len] = '\0';
  scope_define(vm, vm->current_scope, nb, tv);
  sym->type = t; /* 开放对象（未 seal）；is_active 保持 false（TDZ 不变） */
  return t;
}

/* 槽位是否"点名了一个已知类型符号"（用于区分"未知类型"与"依赖尚未就绪"：
 * 前者报诊断，后者静默延后到下一轮，见下方 pending 分支）。 */
static bool slot_names_type_symbol(sema_t *sema, const ast_node_t *slot) {
  if (!sema || !slot) return false;
  strslice_t nm = {0};
  if (slot->kind == AST_IDENT) {
    nm = ((const ast_ident_t *)slot)->name;
  } else if (slot->kind == AST_TYPE_REF) {
    nm = ((const ast_type_ref_t *)slot)->name;
  } else if (slot->kind == AST_SCOPE_ANNOT) {
    return slot_names_type_symbol(sema, ((const ast_scope_annot_t *)slot)->sub);
  } else {
    return false;
  }
  if (!nm.ptr) return false;
  sema_symbol_t *sym = find_global(sema, nm);
  return sym && sym->kind == SEMA_SYM_TYPE;
}

/* 嵌入目标 `...Name` 的三种"还没就绪"形态：
 *   - 名字不是类型符号 → **不是** pending（下面按"无此结构体"报错）；
 *   - 名字是 struct 定义、类型尚未 **seal** → pending（静默延后；注意类型可能
 *     已被"物化"成开放对象，`es->type` 非空但未 seal）；
 *   - 名字是 **type 别名**（`type A = ...`）：类型值尚未建出时算 pending
 *     （同一轮里别名可能随后就绪）；已建出但不是 struct 时**不是** pending
 *     （它就是错的，下面报"not a struct type"）。 */
static bool embed_targets_pending(sema_t *sema, const ast_struct_def_t *sd) {
  for (ast_node_t *e = sd->embeds; e; e = e->next) {
    sema_symbol_t *es = find_global(sema, ((ast_struct_embed_t *)e)->name);
    if (!es || es->kind != SEMA_SYM_TYPE) continue;
    if (es->type && type_is_sealed(es->type)) continue; /* 已 seal：交给下方检查 */
    if (es->ast && es->ast->kind != AST_STRUCT_DEF && es->type)
      continue; /* 别名等已建出的非 struct：交给下方报"not a struct" */
    return true;
  }
  return false;
}

/* 把 `...Name;` 解析成结构体**类型**（Base 的 seal 后快照）。
 * - 名字不存在 / 非类型 → 报错返回 NULL
 * - 名字是类型但尚未 seal（前向引用）→ **静默**返回 NULL（调用方延后本轮）
 * - 是类型但不是 struct（enum、union、标量、别名…）→ 报错返回 NULL
 * 跨 import 的结构体不在本模块的全局名字表里，自然落到"未知结构体"一支
 * （规格：本期只支持同一编译单元内声明的结构体）。 */
static const type_t *resolve_embed_struct(sema_t *sema, const ast_struct_def_t *sd,
                                          const ast_struct_embed_t *emb) {
  sema_symbol_t *es = find_global(sema, emb->name);
  if (!es || es->kind != SEMA_SYM_TYPE) {
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)&emb->base),
               "struct '%.*s': cannot embed '%.*s': no struct type with that "
               "name in this compilation unit",
               (int)sd->name.len, sd->name.ptr, (int)emb->name.len,
               emb->name.ptr);
    return NULL;
  }
  if (!es->type) return NULL; /* 前向引用：本轮还没就绪（不报错） */
  const type_t *bt = es->type;
  if (bt->kind != TYPE_KIND_STRUCT) {
    char tn[128];
    sema_type_name(bt, tn, sizeof tn);
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)&emb->base),
               "struct '%.*s': cannot embed '%.*s': not a struct type (got %s)",
               (int)sd->name.len, sd->name.ptr, (int)emb->name.len,
               emb->name.ptr, tn);
    return NULL;
  }
  return bt;
}

/* 求值单个结构体定义（含 seal 前的字段组合）。
 *
 * 返回三态：OK / DEFERRED（依赖未就绪，静默）/ FAILED（诊断已记录）。
 * 组合窗口：自有字段 + 各 `...Base` 的字段表都装进**开放**类型，最后只 seal
 * 一次——seal 之后类型不可变（见 include/vm/type_struct.h 的不变量）。
 * 定义顺序：`...Base` 的类型若尚未 seal 则 DEFERRED（不递归求值），由
 * sema_struct_compose_pass 的依赖序遍历反复尝试解决；嵌入环由该 pass 的
 * 环检测先一步报出，因此这里不会无限延后。
 * 幂等：符号已激活（类型已建好并 seal）直接返回 OK。 */
sema_compose_result_t sema_struct_compose_def(sema_t *sema, ast_struct_def_t *sd,
                                              sema_scope_t *scope) {
  if (!sema || !sd || !scope) return SEMA_COMPOSE_FAILED;
  sema_symbol_t *sym = sema_scope_find_local(scope, sd->name);
  if (!sym) return SEMA_COMPOSE_FAILED; /* pass1 重复定义已诊断，符号未注册 */

  /* 环检测的在途标记（`composing`）：本函数**不**递归求值 Base（定义顺序由
     sema_struct_compose_pass 的依赖序遍历 + 反复尝试解决），环也由该 pass 的
     report_embed_cycles 在求值之前统一报出，因此这里为真只可能是"同一趟里
     重入"这种内部不变式被破坏的情形——拒绝，不重复报环。 */
  if (sd->composing) return SEMA_COMPOSE_FAILED;

  /* 已求值（激活）→ 跳过：使本函数可反复调用（不动点轮次 / 组合遍历）。
     **必须**跳过——重新求值会用 AST 里的自有字段重建类型，把已经组合好的
     类型（`...Base;` 的字段已定型在 sealed 类型里）overwrite 掉。 */
  if (sym->is_active) return SEMA_COMPOSE_OK;

  /* 0. 物化（幂等）：先给本定义一个**可命名的开放类型对象**（+ 类型 id + 显示名
        + 编译期类型名绑定），字段类型表达式才可能解析到"正在定义中的自己"
       （自引用）或另一个同样在环上的定义（互相引用）。 */
  const type_t *open = materialize_struct_type(sema, sd, sym);
  if (!open) return SEMA_COMPOSE_FAILED;

  /* 0b. 嵌入目标尚未 seal（前向引用 / 环）→ 静默延后，不建任何类型、不报错。
        点名的 Base 若根本不存在 / 不是结构体，则不在此列（下面报错）。 */
  if (embed_targets_pending(sema, sd)) return SEMA_COMPOSE_DEFERRED;

  vm_t *vm = sema->vm;

  /* 1. 自有字段：类型槽位解析 + 重复字段名检查。
        未被标记的槽位失败 = 真未知类型（报诊断）；点名了已知类型符号 = 该
        类型还没就绪（延后本轮，语义与其它 pass 的"未解析槽位"一致）。 */
  size_t nfs = 0;
  for (ast_node_t *f = sd->fields; f; f = f->next) nfs++;

  const type_t **ftype = NULL;
  strslice_t     *fname = NULL;
  /* 每个字段槽位的所有权（销毁序列从 type 对象反推的依据；类型描述的一部分） */
  ownership_t    *fown  = NULL;
  if (nfs > 0) {
    ftype = (const type_t **)arena_alloc(sema->arena, nfs * sizeof(type_t *),
                                         ALIGNOF(max_align_t));
    fname = (strslice_t *)arena_alloc(sema->arena, nfs * sizeof(strslice_t),
                                      ALIGNOF(max_align_t));
    fown  = (ownership_t *)arena_alloc(sema->arena, nfs * sizeof(ownership_t),
                                       ALIGNOF(max_align_t));
    if (!ftype || !fname || !fown) return SEMA_COMPOSE_FAILED;
    for (size_t k = 0; k < nfs; k++) fown[k] = OWN_NONE;
    bool pending = false, bad = false;
    size_t i = 0;
    for (ast_node_t *f = sd->fields; f; f = f->next, i++) {
      ast_struct_field_t *sf = (ast_struct_field_t *)f;
      /* 按值内联了尚未 seal 的结构体 → 本轮延后（**不解析、不折叠槽位**：
         解析会把未定型的派生类型 intern 进池并写回槽位，下一轮拿到同一个
         未定型对象）。指针/切片/func/const/volatile/数组不受此限。 */
      compose_block_t blk;
      memset(&blk, 0, sizeof blk);
      if (slot_blocked_by_unsealed(sema, sf->type, false, false, &blk)) {
        pending = true;
        continue;
      }
      /* **先提取槽位所有权，再解析**：sema_resolve_type_slot 会把
         AST_PTR/AST_SLICE_TYPE 折叠成 AST_TYPE_REF，之后 token 丢失。 */
      ownership_t field_own = sema_type_expr_ownership(sf->type);
      const type_t *ft = sema_resolve_type_slot(sema, &sf->type);
      if (!ft) {
        if (slot_names_type_symbol(sema, sf->type)) {
          pending = true; /* 类型名存在但还没 seal：下一轮再试 */
        } else {
          diag_error(sema->diag, sema_loc(sema, sf->type),
                     "struct '%.*s': unknown field type for '%.*s'",
                     (int)sd->name.len, sd->name.ptr, (int)sf->name.len,
                     sf->name.ptr);
          bad = true;
        }
        continue;
      }
      /* 查重：自有字段之间不允许重名（与既有诊断口径一致） */
      for (size_t j = 0; j < i; j++) {
        if (fname[j].len == sf->name.len && fname[j].ptr && sf->name.ptr &&
            memcmp(fname[j].ptr, sf->name.ptr, sf->name.len) == 0) {
          diag_error(sema->diag, sema_loc(sema, f),
                     "struct '%.*s': duplicate field name '%.*s'",
                     (int)sd->name.len, sd->name.ptr, (int)sf->name.len,
                     sf->name.ptr);
          bad = true;
          break;
        }
      }
      /* 注：原先这里禁止"结构体字段携带指针/切片"。该限制现已**放开**——
         所有权与类型解耦后，字段的 own/ref 由 `struct_field_t::ownership`
         记录（类型描述的一部分），销毁序列由 sema 按 type 对象生成：
         只有 own 槽位进销毁序列，ref 槽位不进。因此 ref 字段不再有
         "被误当作 own 释放"的风险。 */
      ftype[i] = ft;
      fname[i] = sf->name;
      fown[i]  = field_own;
    }
    if (bad) return SEMA_COMPOSE_FAILED;
    if (pending) return SEMA_COMPOSE_DEFERRED;
  }

  /* 2. 逐 `...Base`：取 Base **已 sealed** 的字段表快照（名字 + 类型 + 顺序）
        + 登记嵌入类型。位置（field_index）由 parser 记录：出现在自己第几个
        自有字段之前。
        Base 的求值顺序由 sema_struct_compose_pass 解决（依赖序遍历 + 反复
        尝试），本函数只判断"是否就绪"：未就绪 → 静默延后。 */
  size_t nembs = 0;
  for (ast_node_t *e = sd->embeds; e; e = e->next) nembs++;

  struct_embed_fields_t *sections = NULL;
  const type_t         **embed_ty   = NULL;
  if (nembs > 0) {
    sections = (struct_embed_fields_t *)arena_alloc(
        sema->arena, nembs * sizeof(struct_embed_fields_t),
        ALIGNOF(max_align_t));
    embed_ty = (const type_t **)arena_alloc(sema->arena, nembs * sizeof(type_t *),
                                            ALIGNOF(max_align_t));
    if (!sections || !embed_ty) return SEMA_COMPOSE_FAILED;
  }

  if (nembs > 0) {
    size_t j = 0;
    for (ast_node_t *e = sd->embeds; e; e = e->next, j++) {
      ast_struct_embed_t *emb = (ast_struct_embed_t *)e;
      sema_symbol_t *bs = find_global(sema, emb->name);
      if (!bs || bs->kind != SEMA_SYM_TYPE) {
        resolve_embed_struct(sema, sd, emb); /* 报"未知结构体" */
        return SEMA_COMPOSE_FAILED;
      }
      /* 嵌入目标尚未 **seal**（前向引用：Base 声明在 Test 之后；或 Base 已被
         物化成开放对象但还没填充字段）→ **静默延后**。
         不在这里递归求值 Base："定义顺序无关"由 sema_struct_compose_pass 的
         依赖序遍历 + 反复尝试保证（它会把能做的先做掉，下一轮再回来）；环由
         该 pass 的 report_embed_cycles 先一步报出，所以不会无限延后。 */
      if (!bs->type || !type_is_sealed(bs->type)) return SEMA_COMPOSE_DEFERRED;
      const type_t *bt = bs->type;
      if (bt->kind != TYPE_KIND_STRUCT) {
        resolve_embed_struct(sema, sd, emb); /* 报"不是结构体类型" */
        return SEMA_COMPOSE_FAILED;
      }
      if (emb->field_index > nfs || !emb->name.ptr) {
        return SEMA_COMPOSE_FAILED; /* 位置非法（parser 不变式被破坏） */
      }
      const struct_type_t *bst = (const struct_type_t *)bt;
      sections[j].fields      = bst->fields;
      sections[j].field_count = bst->field_count;
      sections[j].layer       = emb->field_index;
      embed_ty[j]             = bt;
    }
  }

  /* 3. 字段**不允许覆盖**：把"按位置插入后的最终字段序列"逐名比对，任一同名
        冲突（自有 vs 嵌入 / 嵌入 vs 嵌入 / 重复嵌入同一 Base）都报错并拒绝。
        口径：字段不参与 attribute 覆盖语义（只有属性/元数据可在 seal 前被
        改写覆盖），因此字段冲突是错误而不是"后者覆盖前者"。 */
  {
    size_t total = nfs;
    for (size_t i = 0; i < nembs; i++) total += sections[i].field_count;
    compose_name_t *seen = NULL;
    if (total > 0) {
      seen = (compose_name_t *)arena_alloc(sema->arena, total * sizeof(*seen),
                                           ALIGNOF(max_align_t));
      if (!seen) return SEMA_COMPOSE_FAILED;
    }
    size_t sn = 0;
    bool   dup = false;
    for (size_t fi = 0; fi <= nfs && !dup; fi++) {
      for (size_t j = 0; j < nembs && !dup; j++) {
        if (sections[j].layer != fi) continue;
        const struct_type_t *owner = (const struct_type_t *)embed_ty[j];
        for (size_t k = 0; k < sections[j].field_count && !dup; k++) {
          strslice_t nm = sections[j].fields[k].name;
          for (size_t q = 0; q < sn; q++) {
            if (seen[q].name.len != nm.len || !seen[q].name.ptr || !nm.ptr)
              continue;
            if (memcmp(seen[q].name.ptr, nm.ptr, nm.len) != 0) continue;
            /* 冲突：说清是"嵌入字段 vs 自有字段"还是"嵌入字段 vs 另一嵌入字段" */
            if (!seen[q].owner) {
              diag_error(sema->diag, sema_loc(sema, &sd->base),
                         "struct '%.*s': field '%.*s' conflicts with embedded "
                         "field from '%.*s' (fields cannot be overridden by "
                         "struct embedding)",
                         (int)sd->name.len, sd->name.ptr, (int)nm.len, nm.ptr,
                         (int)owner->base.name.len, owner->base.name.ptr);
            } else {
              diag_error(sema->diag, sema_loc(sema, &sd->base),
                         "struct '%.*s': duplicate field name '%.*s' after "
                         "struct embedding: inherited from both '%.*s' and "
                         "'%.*s'",
                         (int)sd->name.len, sd->name.ptr, (int)nm.len, nm.ptr,
                         (int)seen[q].owner->base.name.len,
                         seen[q].owner->base.name.ptr,
                         (int)owner->base.name.len, owner->base.name.ptr);
            }
            dup = true;
            break;
          }
          if (!dup) {
            seen[sn].name  = nm;
            seen[sn].owner = owner;
            sn++;
          }
        }
      }
      if (fi < nfs && !dup) {
        strslice_t nm = fname[fi];
        for (size_t q = 0; q < sn; q++) {
          if (seen[q].name.len != nm.len || !seen[q].name.ptr || !nm.ptr)
            continue;
          if (memcmp(seen[q].name.ptr, nm.ptr, nm.len) != 0) continue;
          /* 自有字段撞上嵌入字段：字段不允许覆盖 → 报错 */
          diag_error(sema->diag, sema_loc(sema, &sd->base),
                     "struct '%.*s': field '%.*s' conflicts with embedded field "
                     "from '%.*s' (fields cannot be overridden by struct "
                     "embedding)",
                     (int)sd->name.len, sd->name.ptr, (int)nm.len, nm.ptr,
                     (int)seen[q].owner->base.name.len,
                     seen[q].owner->base.name.ptr);
          dup = true;
          break;
        }
        if (!dup) {
          seen[sn].name  = nm;
          seen[sn].owner = NULL;
          sn++;
        }
      }
    }
    if (dup) return SEMA_COMPOSE_FAILED;
  }

  /* 4. 就地填充**物化过**的开放类型（**尚未 seal**：字段表 + 嵌入表都在这个
        窗口里编辑）：自有字段按声明序 add_field → 按位置插入各 `...Base` 的
        字段表 → 登记嵌入表 → seal 一次（布局在 seal 里统一计算）。
        type_t* 身份：物化时登记的对象就是最终对象（具名 struct 走 unique，
        seal 不做去重 intern、不换对象）——后续 c_sema_type_find_ptr /
        types_by_id / CTFE 用的都是它；显示名/id 也已在物化时设置（type_set_name
        只对已分配 program id 的类型生效，物化已登记 id）。 */
  const type_t *t = open;
  if (!t) return SEMA_COMPOSE_FAILED;

  /* 具名 struct 定义 → **独立身份**：不参与 seal 去重 intern（`struct A` 与
     `struct B` 字段表逐位相同也是两个不同的具名类型：各自有独立的成员命名空间
     `A::f` / `B::f` 与嵌入表）。若被去重合并，(1) 显示名会互相覆盖；(2) 组合的
     嵌入表会退化成 [X, X]，成员继承查找按类型去重后只剩一个命中——歧义不再
     报出、覆盖判定失去依据（`struct R { ...P; ...Q; }` 里 P、Q 是空字段 struct
     时尤其明显）。与其它类型属性一样，本标记也只在 seal 之前的开放窗口里设。 */
  type_struct_mark_unique(vm, t);

  {
    size_t i = 0;
    for (ast_node_t *f = sd->fields; f; f = f->next, i++)
      type_struct_add_field_ex(vm, t, fname[i], ftype[i], fown[i]);
  }

  if (nembs > 0) {
    if (!type_struct_insert_embed_fields(vm, t, sections, nembs) ||
        !type_struct_set_embeds(vm, t, embed_ty, nembs)) {
      /* seal 前窗口被破坏（类型被提前 seal 之类的内部错误）：明确诊断，
         不静默产出半成品类型。 */
      diag_error(sema->diag, sema_loc(sema, &sd->base),
                 "struct '%.*s': failed to compose embedded fields before type "
                 "seal",
                 (int)sd->name.len, sd->name.ptr);
      /* 不释放 t：它已在物化时登记进 sema->types / 名字绑定 / 符号表——释放会
         留下悬垂引用。诊断已记录，编译会中止。 */
      sd->composing = false;
      return SEMA_COMPOSE_FAILED;
    }
  }

  const type_t *sealed = type_struct_seal(vm, t);
  if (!sealed) {
    /* 字段类型未解析等：拒绝密封（与既有路径一致） */
    sd->composing = false;
    return SEMA_COMPOSE_FAILED;
  }

  /* 4b. 递归类型的"布局延迟定型"已收敛到 type_struct_seal 内部（编译期与
         运行期共用同一处）：目标 seal 之后，以它为布局来源的 const/volatile/
         数组包装会被同步到最终布局。 */

  /* 5. 登记（hoist 构造用）+ 绑定 type value 到编译期 vm 作用域 */
  const sema_type_t *st = sema_type_register(sema, sealed);
  if (!st) { sd->composing = false; return SEMA_COMPOSE_FAILED; }
  sd->type_id = st->id; /* compiler 顶层名字绑定 LOAD_TYPE 用 */
  /* 显示名（诊断/值 dump 用）：物化时已设过（此时 id 已分配）；这里只做
     "seal 换了对象"的防御性同步——具名 struct 走 unique 不去重，正常不会发生。 */
  if (sealed != t) {
    ((sema_type_t *)st)->type = sealed;
    type_set_name(vm, sealed, sd->name);
  }
  /* 名字绑定已在**物化**时完成（同一个对象；具名 struct 不走 seal 去重，
     sealed == t）。只有"seal 换了对象"的防御分支才需要重新绑定。 */
  if (sealed != t) {
    value_t *tv = type_as_value(vm, sealed);
    if (!tv) { sd->composing = false; return SEMA_COMPOSE_FAILED; }
    char nb[256];
    if (sd->name.len >= sizeof nb) {
      sd->composing = false;
      return SEMA_COMPOSE_FAILED;
    }
    memcpy(nb, sd->name.ptr, sd->name.len);
    nb[sd->name.len] = '\0';
    scope_define(vm, vm->current_scope, nb, tv);
  }

  /* 6. 符号激活（TDZ：定义前不可见）——类型已 seal 且登记完毕，"构造中"结束 */
  sd->composing  = false;
  sym->type      = sealed;
  sym->is_active = true;
  sym->flow_init = true;
  return SEMA_COMPOSE_OK;
}

/* ===========================================================================
 * 嵌入环检测（DFS + 在途标记）
 *
 * 环里的结构体永远等不到依赖就绪：`struct X { ...Y; } struct Y { ...X; }` 下
 * X 等 Y、Y 等 X，两边都只能"延后"，最终静默地谁也没建出来（无诊断）。因此
 * 求值之前先用 DFS 把环找出来报掉：
 *   - 命中"在途"节点 → 报"循环嵌入"（位置 = 回到的那个结构体）；
 *   - 遍历完复位标记，路径式（非全局 visited）遍历保证菱形依赖（A→B、A→C、
 *     B→D、C→D）不会误报。
 * 覆盖全局与块内（局部）结构体定义——`...Name` 的名字都是全局符号，两者共用
 * 同一条解析路径。每个环只报一次（报在回到起点的那一步）。
 * =========================================================================== */

/* DFS：沿 `...Name` 走进被嵌入的结构体定义（只走能解析成结构体定义的名字；
 * 未知名字 / 非结构体留给求值阶段报错）。命中"在途"节点 = 嵌入链成环：把从
 * 在途节点到当前节点的整条路径都标记 cycle_reported（由调用方按定义顺序统一
 * 报出，保证环里每个结构体都报一次、且只报一次）。
 * 不打印诊断：诊断点统一放在 report_embed_cycles，避免"回到在途节点"那一步
 * 只认识"某个节点"、无法逐个报出环成员。 */
static void embed_cycle_dfs(sema_t *sema, ast_struct_def_t *sd,
                            ast_struct_def_t **path, size_t npath) {
  if (!sd) return;
  if (sd->composing) {
    /* 找到环：path 里从 sd 出现的位置到末尾都是环成员 */
    size_t start = npath;
    for (size_t i = 0; i < npath; i++)
      if (path[i] == sd) { start = i; break; }
    for (size_t i = start; i < npath; i++) path[i]->cycle_reported = true;
    sd->cycle_reported = true;
    return;
  }
  if (npath >= COMPOSE_MAX_DEPTH) return; /* 防御：病态深链 */
  sd->composing = true;
  path[npath] = sd;
  for (ast_node_t *e = sd->embeds; e; e = e->next) {
    sema_symbol_t *bs = find_global(sema, ((ast_struct_embed_t *)e)->name);
    if (bs && bs->kind == SEMA_SYM_TYPE && bs->ast &&
        bs->ast->kind == AST_STRUCT_DEF)
      embed_cycle_dfs(sema, (ast_struct_def_t *)bs->ast, path, npath + 1);
  }
  sd->composing = false;
}

/* 收集一个块里的局部结构体定义（遇到嵌套块递归进去）：用于把局部定义也纳入
 * 环检测。只收集本块直接声明的结构体（不跟随 embed 名字，避免重复访问）。 */
static void collect_block_structs(ast_node_t *stmts, ast_struct_def_t **out,
                                  size_t *n, size_t cap) {
  for (ast_node_t *s = stmts; s; s = s->next) {
    if (s->kind == AST_STRUCT_DEF) {
      if (*n < cap) out[(*n)++] = (ast_struct_def_t *)s;
      continue;
    }
    if (s->kind == AST_BLOCK) {
      collect_block_structs(((ast_block_t *)s)->stmts, out, n, cap);
    }
  }
}

/* 收集程序里全部结构体定义（顶层 + 各函数体内含嵌套块的局部定义）。
 * 返回个数；defs 由 arena 分配（*out = NULL 表示无定义 / OOM）。 */
static size_t collect_program_structs(sema_t *sema, ast_program_t *prog,
                                      ast_struct_def_t ***out) {
  if (out) *out = NULL;
  if (!sema || !prog) return 0;
  size_t cap = 0;
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind == AST_STRUCT_DEF) cap++;
    else if (d->kind == AST_FUNC_DEF) cap += 64; /* 局部结构体上限的粗估 */
  }
  if (cap == 0) return 0;
  ast_struct_def_t **defs = (ast_struct_def_t **)arena_alloc(
      sema->arena, cap * sizeof(*defs), ALIGNOF(max_align_t));
  if (!defs) return 0;
  size_t n = 0;
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind == AST_STRUCT_DEF) {
      if (n < cap) defs[n++] = (ast_struct_def_t *)d;
    } else if (d->kind == AST_FUNC_DEF) {
      collect_block_structs(((ast_func_def_t *)d)->body, defs, &n, cap);
    }
  }
  if (out) *out = defs;
  return n;
}

/* 环检测入口：收集全局 + 各函数体内（含嵌套块）的结构体定义，逐个 DFS。 */
/* 每次组合遍历只做一次环检测：pass1b_types 是"逐个顶层定义"的循环，每个
 * struct 定义都会调一次 sema_struct_compose_pass（幂等），没有这个门闩会把
 * 同一组环诊断打印多次。sema_struct_compose_reset_reports 打开门闩（正式轮
 * 之前调用），因此每个"轮次"最多检测一次。 */
static bool g_cycles_checked = false;

static void report_embed_cycles(sema_t *sema, ast_program_t *prog) {
  ast_struct_def_t **defs = NULL;
  size_t n = collect_program_structs(sema, prog, &defs);
  if (n == 0 || !defs) return;
  /* 本轮起点：复位上一轮留下的标记（试跑轮会反复调用本函数），再逐个 DFS。 */
  ast_struct_def_t *path[COMPOSE_MAX_DEPTH];
  for (size_t i = 0; i < n; i++) {
    defs[i]->cycle_reported = false;
    defs[i]->composing = false;
  }
  for (size_t i = 0; i < n; i++) {
    if (!defs[i]->composing) embed_cycle_dfs(sema, defs[i], path, 0);
  }
  /* 报告阶段：按定义顺序把本轮认领过的定义报出来——环里每个结构体恰好一次
     （一个不落、也不重复）。 */
  for (size_t i = 0; i < n; i++) {
    if (!defs[i]->cycle_reported) continue;
    diag_error(sema->diag, sema_loc(sema, &defs[i]->base),
               "struct '%.*s': circular struct embedding ('...' cycle involving "
               "this struct)",
               (int)defs[i]->name.len, defs[i]->name.ptr);
    defs[i]->cycle_reported = false; /* 本轮已报 */
  }
}

/* ===========================================================================
 * 按值递归环诊断（一次性门闩，与 g_cycles_checked 同款）
 *
 * `struct A { b: B; } struct B { a: A; }`（或自含）永远等不到依赖就绪——按值内联
 * 的类型布局必须先于自身确定，不是"延后"能解决的。判定：顺"按值阻塞"链走，
 * 链回到起点才算环（别的定义卡住时由那个定义自己的诊断负责，不重复报）。
 * 指针/切片/func/const/volatile/数组是允许的递归形态，不在此列。
 * =========================================================================== */

static bool g_incomplete_reported = false;

/* 从 sd 的字段里找"按值内联了尚未 seal 的结构体"的第一个阻塞字段。 */
static bool find_blocking_field(sema_t *sema, ast_struct_def_t *sd,
                                compose_block_t *out) {
  for (ast_node_t *f = sd->fields; f; f = f->next) {
    ast_struct_field_t *sf = (ast_struct_field_t *)f;
    compose_block_t blk;
    memset(&blk, 0, sizeof blk);
    if (slot_blocked_by_unsealed(sema, sf->type, false, false, &blk)) {
      blk.field = sf->name;
      if (out) *out = blk;
      return true;
    }
  }
  return false;
}

static void report_incomplete_field_cycles(sema_t *sema, ast_program_t *prog) {
  ast_struct_def_t **defs = NULL;
  size_t n = collect_program_structs(sema, prog, &defs);
  if (n == 0 || !defs) return;
  for (size_t i = 0; i < n; i++) {
    ast_struct_def_t *sd = defs[i];
    if (!sd->name.ptr) continue;
    sema_symbol_t *sym = find_global(sema, sd->name);
    if (sym && sym->is_active) continue; /* 已建好 */
    compose_block_t blk;
    if (!find_blocking_field(sema, sd, &blk)) continue; /* 卡在别处（如嵌入环） */
    /* 顺链找环：链回到自己才是递归环 */
    ast_struct_def_t *cur = sd;
    bool cycle = false;
    for (size_t hop = 0; hop <= n; hop++) {
      compose_block_t step;
      if (!find_blocking_field(sema, cur, &step)) break;
      ast_struct_def_t *next =
          (step.sym && step.sym->ast && step.sym->ast->kind == AST_STRUCT_DEF)
              ? (ast_struct_def_t *)step.sym->ast
              : NULL;
      if (next == sd) { cycle = true; blk = step; break; }
      if (!next) break;
      cur = next;
    }
    if (!cycle) continue;
    strslice_t target = blk.target.ptr ? blk.target : sd->name;
    location_t loc = blk.slot ? sema_loc(sema, blk.slot) : sema_loc(sema, &sd->base);
    if (blk.wrapped) {
      /* 命中点套了 option/tuple/union 包装：这些类型按值内联元素布局且没有
         "延迟定型"路径（见 type_sync_derived_layout 的说明）。 */
      diag_error(sema->diag, loc,
                 "struct '%.*s': field '%.*s' has incomplete type '%.*s': "
                 "option/tuple/union wrappers cannot be completed while the "
                 "struct is being defined (use a pointer, slice, qualified type "
                 "or array to make the reference recursive)",
                 (int)sd->name.len, sd->name.ptr, (int)blk.field.len,
                 blk.field.ptr, (int)target.len, target.ptr);
    } else {
      diag_error(sema->diag, loc,
                 "struct '%.*s': field '%.*s' has incomplete type '%.*s': "
                 "recursive struct by value (break the cycle with a pointer, "
                 "e.g. 'ref *%.*s')",
                 (int)sd->name.len, sd->name.ptr, (int)blk.field.len,
                 blk.field.ptr, (int)target.len, target.ptr, (int)sd->name.len,
                 sd->name.ptr);
    }
  }
}

void sema_struct_compose_reset_reports(ast_program_t *prog) {
  /* 打开"本轮环检测"门闩：正式轮会重新检测并报出循环嵌入（试跑轮的诊断进了
     临时缓冲被丢弃，但"已检测"标记会留下来）。同理复位按值递归环的门闩。 */
  g_cycles_checked    = false;
  g_incomplete_reported = false;
  if (!prog) return;
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind == AST_STRUCT_DEF) {
      ast_struct_def_t *sd = (ast_struct_def_t *)d;
      sd->cycle_reported = false;
      sd->composing = false;
    } else if (d->kind == AST_FUNC_DEF) {
      /* 块内（局部）结构体：与环检测收集范围一致 */
      size_t cap = 64;
      ast_struct_def_t *defs[64];
      size_t n = 0;
      collect_block_structs(((ast_func_def_t *)d)->body, defs, &n, cap);
      for (size_t i = 0; i < n; i++) {
        defs[i]->cycle_reported = false;
        defs[i]->composing = false;
      }
    }
  }
}

size_t sema_struct_compose_pass(sema_t *sema, ast_program_t *prog,
                                sema_scope_t *scope) {
  if (!sema || !prog || !scope) return 0;

  /* 1. 先做嵌入环检测（DFS + 在途标记 sd->composing）：直接自嵌入与间接环都在
        这里一次性报出（"循环嵌入"）。必须在求值之前——环里的定义永远等不到
        依赖就绪，先求值只会把它们静默延后（无诊断）。
        每轮只做一次（pass1b_types 每个 struct 定义都会调本函数，幂等）。 */
  if (!g_cycles_checked) {
    g_cycles_checked = true;
    report_embed_cycles(sema, prog);
  }

  /* 1b. 物化：为**全部**顶层结构体定义先建开放类型对象（+ 类型 id + 显示名 +
         编译期类型名绑定）。自引用/互相引用的字段类型表达式要求名字此刻就能解析
         到对象；hoist 的两遍构造（pass 1 声明全部 id、pass 2 指针/切片/func 密封
         不依赖 base 布局）本就支持成环，sema 侧只差这一步。
         局部（块内）结构体不在全局作用域：由 sema_struct_compose_def 按需物化。 */
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind != AST_STRUCT_DEF) continue;
    ast_struct_def_t *sd = (ast_struct_def_t *)d;
    sema_symbol_t *sym = sema_scope_find_local(scope, sd->name);
    if (!sym || sym->type) continue; /* 已物化 / 重复定义已诊断 */
    materialize_struct_type(sema, sd, sym);
  }

  /* 2. 依赖序遍历 + 反复尝试：每轮扫过全部顶层结构体定义，能做的当场做掉
        （"能做的"= 所有 `...Base` 已 seal、字段类型没有按值内联未 seal 的类型），
        直到某一轮没有任何新进展。由此 `...Base` 的 Base 写在 Test 之后也可用，
        自引用/互相引用在同轮内即可定型。环已在第 1 步被拒，不会无限延后。 */
  size_t done = 0;
  for (;;) {
    size_t progressed = 0;
    for (ast_node_t *d = prog->funcs; d; d = d->next) {
      if (d->kind != AST_STRUCT_DEF) continue;
      ast_struct_def_t *sd = (ast_struct_def_t *)d;
      sema_symbol_t *sym = sema_scope_find_local(scope, sd->name);
      if (!sym || sym->is_active) continue; /* 已就绪 / 重复定义已诊断 */
      if (sema_struct_compose_def(sema, sd, scope) == SEMA_COMPOSE_OK)
        progressed++;
    }
    if (progressed == 0) break;
    done += progressed;
  }

  /* 3. 按值递归环诊断（一次性）：不动点结束后仍有定义没建出来、且其字段按值
        内联了未 seal 的类型 → 这是无解的递归（不是延后能解决的），必须报错。 */
  if (!g_incomplete_reported) {
    g_incomplete_reported = true;
    report_incomplete_field_cycles(sema, prog);
  }
  return done;
}

/* ===========================================================================
 * 成员名字查找（含嵌入继承 + 新 struct 的 attribute 覆盖）
 *
 * 规则（**确定性**，见 docs/language-spec-m1 §结构体嵌入 与 docs/m6-design §14.23）：
 *   1. 先 `Test::name` 自己 —— **命中即返回**：新 struct 自己声明的 attribute
 *      （字段之外的 `var` 静态属性 / `func` 方法）**静默覆盖**继承来的同名
 *      attribute，不报歧义、不报重复。覆盖后两者**互相独立**（`Test::count` 与
 *      `Base::count` 是两个不同的全局变量；方法则是两个不同的函数）。跨 kind
 *      也按"名字优先、自己的胜出"：自己的是 `var` 就不会回退到 Base 的 `func`
 *      （kind 由使用点裁决，见 ex_call / shadow_assign / ex_member 的诊断）；
 *   2. 自己**没有**时，才沿嵌入链按 `...` 出现顺序**深度优先**递归（因此传递
 *      继承天然覆盖），按类型指针去重（同一个 Base 经两条路径只算一次命中，
 *      "重复嵌入同一 Base"不会被误报成歧义）；
 *   3. 命中数 == 1 → 用该成员的**实际定义**限定名（`Base::name`）——此时静态
 *      属性是**同一份**全局变量（`Test::count` 与 `Base::count` 互为别名）、
 *      方法是同一个函数；
 *      命中数 > 1 → 歧义报错（**只**发生在"自己没有、而多个嵌入都提供同名
 *      attribute"的情形）；命中数 == 0 → 不报错（沿用调用点既有诊断）。
 * 字段是唯一例外：字段**不参与**覆盖，同名一律报错（见 sema_struct_compose_def
 * 第 3 步的字段冲突检查）。
 * =========================================================================== */

/* 收集"嵌入链上**确实**定义了成员 `name` 的嵌入结构体"：沿类型上的嵌入表
 * （`...Base` 各自贡献字段时登记，见 type_struct_set_embeds）**深度优先、
 * 按 `...` 出现顺序**递归展开（传递继承天然覆盖），命中判定 = 该类型的
 * member_scope 中存在名为 `name` 的符号。按类型指针去重：同一个 Base 经
 * 两条 `...` 路径只算一次命中（于是"重复嵌入同一个 Base"不会被误报成歧义）。 */
static void collect_member_sources(sema_t *sema, const struct_type_t *st,
                                   strslice_t name, const struct_type_t **out,
                                   size_t *n, const struct_type_t **visited,
                                   size_t *nvis) {
  if (!st || *n >= COMPOSE_MAX_SRC || *nvis >= COMPOSE_MAX_SRC) return;
  for (size_t i = 0; i < *nvis; i++)
    if (visited[i] == st) return; /* 已访问（共享嵌入 / 环防御） */
  visited[(*nvis)++] = st;

  size_t ne = 0;
  const type_t *const *emb = type_struct_embeds(&st->base, &ne);
  for (size_t i = 0; i < ne; i++) {
    const type_t *bt = emb[i];
    if (!bt || bt->kind != TYPE_KIND_STRUCT) continue;
    const struct_type_t *bs = (const struct_type_t *)bt;
    /* 命中判定：查 Base 类型的 member_scope 中是否有名为 `name` 的符号 */
    sema_symbol_t *bs_sym = find_global(sema, bs->base.name);
    if (bs_sym && bs_sym->member_scope) {
      sema_symbol_t *ms = sema_scope_find_local(bs_sym->member_scope, name);
      if (ms) {
        bool dup = false;
        for (size_t k = 0; k < *n; k++)
          if (out[k] == bs) { dup = true; break; }
        if (!dup && *n < COMPOSE_MAX_SRC) out[(*n)++] = bs;
      }
    }
    /* 继续向下（Base 自己的嵌入链）——传递继承 */
    collect_member_sources(sema, bs, name, out, n, visited, nvis);
  }
}

/* 在 struct 类型的 member_scope 中查找成员符号（不沿嵌入链）。
 * 返回借用指针，未找到返回 NULL。 */
static sema_symbol_t *find_own_member(sema_t *sema, const type_t *t,
                                      strslice_t name) {
  if (!t || !t->name.ptr) return NULL;
  sema_symbol_t *sym = find_global(sema, t->name);
  if (!sym || !sym->member_scope) return NULL;
  return sema_scope_find_local(sym->member_scope, name);
}

/* 沿嵌入链递归查找成员符号（用于方法调用与静态访问的继承解析）。
 * 返回命中的成员符号 + 所属类型（out_owner）。歧义报诊断并返回 NULL。 */
static sema_symbol_t *find_inherited_member(sema_t *sema, const type_t *t,
                                            strslice_t name,
                                            ast_node_t *tnode,
                                            const struct_type_t **out_owner) {
  const type_t *base = type_strip_quals(t);
  if (!base || base->kind != TYPE_KIND_STRUCT) return NULL;
  const struct_type_t *st = (const struct_type_t *)base;

  const struct_type_t *src[COMPOSE_MAX_SRC];
  const struct_type_t *visited[COMPOSE_MAX_SRC];
  size_t nsrc = 0, nvis = 0;
  collect_member_sources(sema, st, name, src, &nsrc, visited, &nvis);
  if (nsrc == 0) return NULL;

  if (nsrc > 1) {
    char msg[512];
    int off = snprintf(msg, sizeof msg,
                       "member '%.*s' of struct '%.*s' is ambiguous: found in ",
                       (int)name.len, name.ptr, (int)base->name.len,
                       base->name.ptr);
    if (off < 0) off = 0;
    for (size_t i = 0; i < nsrc; i++) {
      const char *tn = src[i]->base.name.ptr ? src[i]->base.name.ptr : "?";
      int tl = src[i]->base.name.ptr ? (int)src[i]->base.name.len : 1;
      int k = snprintf(msg + off, sizeof msg - (size_t)off, "%s%.*s",
                       i ? ", " : "", tl, tn);
      if (k < 0 || (size_t)(off + k) >= sizeof msg) break;
      off += k;
    }
    diag_error(sema->diag, sema_loc(sema, tnode), "%s", msg);
    return NULL;
  }

  const struct_type_t *owner = src[0];
  sema_symbol_t *ms = find_own_member(sema, &owner->base, name);
  if (!ms) return NULL;
  if (out_owner) *out_owner = owner;
  return ms;
}

sema_symbol_t *sema_struct_find_member(sema_t *sema, const type_t *t,
                                       strslice_t name, ast_node_t *tnode,
                                       const struct_type_t **out_owner) {
  if (out_owner) *out_owner = NULL;
  if (!sema || !t || !name.ptr || name.len == 0) return NULL;
  const type_t *base = type_strip_quals(t);
  if (!base || base->kind != TYPE_KIND_STRUCT || !base->name.ptr) return NULL;

  /* 1. 自己优先：查 member_scope */
  sema_symbol_t *ms = find_own_member(sema, base, name);
  if (ms) {
    if (out_owner) *out_owner = (const struct_type_t *)base;
    return ms;
  }

  /* 2. 嵌入链继承查找（collect_member_sources 递归传递嵌入） */
  return find_inherited_member(sema, base, name, tnode, out_owner);
}

strslice_t *sema_struct_member_lookup(sema_t *sema, const type_t *t,
                                      strslice_t name,
                                      ast_node_t *tnode) {
  if (!sema || !t || !name.ptr || name.len == 0) return NULL;
  const type_t *base = type_strip_quals(t);
  if (!base || base->kind != TYPE_KIND_STRUCT || !base->name.ptr) return NULL;

  /* 1. 自己优先：查 member_scope */
  sema_symbol_t *ms = find_own_member(sema, base, name);
  if (ms) {
    strslice_t *res = (strslice_t *)arena_alloc(sema->arena, sizeof(strslice_t),
                                                ALIGNOF(max_align_t));
    if (!res) return NULL;
    *res = name;
    return res;
  }

  /* 2. 嵌入链继承查找 */
  const struct_type_t *owner = NULL;
  ms = find_inherited_member(sema, base, name, tnode, &owner);
  if (!ms) return NULL;

  strslice_t *res = (strslice_t *)arena_alloc(sema->arena, sizeof(strslice_t),
                                              ALIGNOF(max_align_t));
  if (!res) return NULL;
  *res = name;
  return res;
}
