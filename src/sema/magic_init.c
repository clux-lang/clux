#include "sema/magic.h"
#include "sema/comptime.h"
#include "parser/ast_addr.h"
#include "vm/type_ptr.h"
#include "diag/diagnostic.h"

#include "parser/ast_block.h"
#include "parser/ast_call.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_func_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_member.h"
#include "parser/ast_program.h"
#include "parser/ast_return.h"
#include "parser/ast_var_def.h"
#include "sema/struct_compose.h"
#include "vm/type_func.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * `__init__` / `__init_copy__` 初始化转换（§3.15.5）
 *
 * 构造**不再生成** `__clux_init_<id>` 助手函数：sema 保留构造节点并打
 * `init_checked` / `copy_init` 标记，C 后端就地降级成语句表达式
 *
 *     ({ T _t = (T){0}; T_init(&_t, args); _t; })          // .T{args}
 *     ({ T _t = (T){0}; _t.__init_copy__(&src); _t; })     // 同类型初始化
 *
 * 这样泛型 `__init__` 也是普通方法调用，不必让"生成的泛型函数"走实例化器
 * （实例化器会重新解析源码 token，生成的函数没有 token）。
 * =========================================================================== */

/* 该类型是否有 `__init__`（struct + 成员方法）。 */
bool sema_magic_has_init(sema_t *sema, const type_t *t) {
    if (!sema || !t) return false;
    const type_t *core = type_strip_quals(t);
    if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) return false;
    return sema_magic_member(sema, core, "__init__") != NULL;
}

/* 初始化表达式是否是**已存在值的位置**（变量 / 成员 / 下标 / 解引用 / 解包）。
   只有这种"把一个已有值复制进目标"的形态才受单参 __init__ 拦截：
   构造（`.T{...}`）与函数调用返回的是新值，初始化就是把它放进去。 */
static bool init_expr_is_place(ast_node_t *n) {
  if (!n) return false;
  switch (n->kind) {
    case AST_IDENT:
    case AST_MEMBER:
    case AST_INDEX:
    case AST_DEREF:
    case AST_UNWRAP:
    case AST_SCOPE_REF:
      return true;
    default:
      return false;
  }
}

value_t *sema_magic_init_convert(sema_t *sema, ast_node_t **slot,
                                 const type_t *src_t, const type_t *target_t,
                                 sema_scope_t *scope, bool *converted) {
    if (converted) *converted = false;
    if (!sema || !slot || !*slot || !target_t) return NULL;
    const type_t *core = type_strip_quals(target_t);
    if (!core || type_kind_of(core) != TYPE_KIND_STRUCT) return NULL;
    sema_symbol_t *ms = sema_magic_member(sema, core, "__init__");
    sema_symbol_t *mc = sema_magic_member(sema, core, "__init_copy__");
    if (!ms && !mc) return NULL;
    const type_t *src0 = src_t ? type_strip_quals(src_t) : NULL;

    /* 同类型初始化（`var b: T = a;`、实参、返回值、字段）一律走 `__init_copy__`
       ——它是固定签名 `(self: *T, src: *T)` 的移动/拷贝构造钩子，形参是指针，
       所以"参数传递 = 构造"不会自递归。没有这个钩子时，同类型初始化仍按"不
       可拷贝"拒绝。 */
    if (src0 && src0 == core && init_expr_is_place(*slot)) {
      if (!mc) {
        if (!ms) return NULL; /* 既无 __init__ 也无 __init_copy__：普通复制 */
        /* 落下去，由 __init__ 的形参检查报"不可从自身初始化"。 */
      } else {
        ast_node_t *ctor = ast_construct_new(sema->arena, (*slot)->tok_begin,
                                             (*slot)->tok_end);
        if (!ctor) return NULL;
        ast_node_t *tref = sema_ct_type_ref(sema, core, *slot);
        if (!tref) return NULL;
        ast_node_t *arg = *slot;
        arg->next = NULL;
        ((ast_construct_t *)ctor)->type = tref;
        ((ast_construct_t *)ctor)->fields = arg;
        ((ast_construct_t *)ctor)->fields_last = arg;
        ((ast_construct_t *)ctor)->copy_init = true;
        ctor->next = (*slot)->next;
        *slot = ctor;
        value_t *cv = sema_expr(sema, slot, scope);
        if (converted) *converted = true;
        return cv;
      }
    }

    if (!ms) return NULL;
    const bool generic_ctor = ms->is_generic;
    if (!generic_ctor) {
      if (!ms->type || type_kind_of(ms->type) != TYPE_KIND_FUNC) return NULL;
      if (func_type_param_count(ms->type) != 2) return NULL; /* self + 1 */
    }
    const type_t *src = src_t ? type_strip_quals(src_t) : NULL;
    /* 新值（构造 / 函数调用返回）：直接放入目标，不再构造一次。 */
    if (src && src == core && !init_expr_is_place(*slot)) return NULL;

    /* 预检：非泛型构造可以直接比对形参；泛型构造的形参类型由实参推断，交给
       构造路径（`sema_generic_instantiate_inferred`）校验。 */
    if (!generic_ctor) {
      const type_t *pt = func_type_param(ms->type, 1);
      if (!pt) return NULL;
      value_t *probe =
          value_make_shadow(sema->vm, src ? src : sema->vm->type_void);
      if (!probe) return NULL;
      if (value_is_error(sema->vm, value_implicit_cast(sema->vm, probe, pt))) {
        /* 目标类型有单参 __init__：**所有**初始化位置（var / 实参 / 返回值 /
           字段）都走构造，源表达式必须能转换成构造的第二个形参。源类型与
           目标类型相同时也不退化成值拷贝——这正是 move-only 类型靠在参数上
           拒绝自身类型来禁止 copy 的方式。 */
        char tn[64], pn[64], sn[64];
        sema_type_name(core, tn, sizeof tn);
        sema_type_name(pt, pn, sizeof pn);
        if (src) sema_type_name(src, sn, sizeof sn);
        else snprintf(sn, sizeof sn, "void");
        location_t loc = sema_loc(sema, *slot);
        if ((*slot)->is_generated || loc.begin.line == 0) {
            if (converted) *converted = true;
            return value_make_error(sema->vm, "bad __init__ argument");
        }
        bool dup = false;
        const diagnostic_t *items = diag_items(sema->diag);
        size_t nitems = diag_count(sema->diag);
        for (size_t i = 0; i < nitems; i++)
          if (items[i].message && items[i].loc.begin.line == loc.begin.line &&
              items[i].loc.begin.column == loc.begin.column &&
              strncmp(items[i].message, "cannot initialize ", 19) == 0) {
            dup = true;
            break;
          }
        if (!dup)
          diag_error(sema->diag, loc,
                     "cannot initialize %s with %s: %s::__init__ expects %s "
                     "(initialization always goes through __init__)",
                     tn, sn, tn, pn);
        if (converted) *converted = true;
        return value_make_error(sema->vm, "bad __init__ argument");
      }
    }

    /* 改写成等价的 `.T{ <原表达式> }`：sema 的构造路径会求值 / 校验实参，
       C 后端把它就地降级成
           ({ T _t = (T){0}; T_init(&_t, arg); _t; })
       不再生成 `__clux_init_<id>` 助手（泛型 `__init__` 因此也能用）。 */
    ast_node_t *ctor = ast_construct_new(sema->arena, (*slot)->tok_begin,
                                         (*slot)->tok_end);
    if (!ctor) return NULL;
    ast_node_t *tref = sema_ct_type_ref(sema, core, *slot);
    if (!tref) return NULL;
    ast_node_t *arg = *slot;
    arg->next = NULL;
    ((ast_construct_t *)ctor)->type = tref;
    ((ast_construct_t *)ctor)->fields = arg;
    ((ast_construct_t *)ctor)->fields_last = arg;
    ctor->next = (*slot)->next;
    *slot = ctor;
    value_t *v = sema_expr(sema, slot, scope);
    if (converted) *converted = true;
    return v;
}