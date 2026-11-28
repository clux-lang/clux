/* §2.11 闭包退化 P1：具名非 cfunc 函数 → 「$closure$M struct +
   $callee$M 纯函数」双产物（emit 期脱糖）。
   - 协议（用户示例）：闭包变量平铺进 callee 打头的 struct；callee 首参
     env: *$closure$M；体内捕获引用改写 env.<name>；调用点
     N(args) → N.callee(N.&, args)；自递归 env.callee(env, ...)。
   - 块级条目（块尾延迟绑定）：定义点换 var N: $closure$M = undefined
     （TDZ 占位），块尾（unwind 之前）追加构造赋值——mid-block 调用读
     TDZ var 报错是有意语义。
   - 模块级条目：定义语句原位换 var N: $closure$N = .$closure$N{...}。
   - 资格（collect 期谓词全集）：具名 func（cfunc/泛型/export/comptime/
     装饰器/匿名不退化）；模块级全部退化（含零捕获）；块级另需 ≥1 捕获
     + 定义块在 owner 体直属语句树内（不进嵌套函数/defer 体）+ 敏感名
     字域检查（env 不可直达 / 名字槽误改写 / 函数值传递回归全部 bail
     回旧提升通道）。全部机制落 emit 期：check 在原树上照跑（诊断零
     变化），合成体直进产物不经 check。 */
#include "sema/sema_internal.h"
#include "core/program.h"
#include "core/string.h"
#include "core/vec.h"
#include "engine/array.h"
#include "engine/optional.h"
#include "engine/ptr.h"
#include "engine/qualifier.h"
#include "engine/slice.h"
#include "engine/struct.h"
#include "engine/tuple.h"
#include "engine/type.h"
#include "engine/union.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_construct_field.h"
#include "parser/node_field.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_func_type.h"
#include "parser/node_literal.h"
#include "parser/node_program.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_var.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include <stdio.h>
#include <string.h>

/* ---- 渲染辅助（dispose.c 同构，intern 文本挂 location） ---- */

static node_t *
closure_ident(sema_clone_env_t *env, const char *text, const location_t *loc) {
  return sema_render_text_node(
      env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, text, loc);
}

static node_t *
closure_symbol(sema_clone_env_t *env, const char *text, const location_t *loc) {
  return sema_render_text_node(
      env->out, env->program, CLUX_NODE_SYMBOL, text, loc);
}

static node_t *closure_binary(sema_clone_env_t *env,
                              node_t *left,
                              const char *op,
                              node_t *right,
                              const location_t *loc) {
  node_t *binary = create_binary_node(env->out);
  node_set_location(binary, loc);
  binary_node_set_left(binary, left);
  binary_node_set_symbol(binary, closure_symbol(env, op, loc));
  binary_node_set_right(binary, right);
  return binary;
}

/* 前缀一元形态：LEFT 缺失（类型位置 *T）。 */
static node_t *closure_prefix(sema_clone_env_t *env,
                              const char *op,
                              node_t *operand,
                              const location_t *loc) {
  node_t *prefix = create_binary_node(env->out);
  node_set_location(prefix, loc);
  binary_node_set_symbol(prefix, closure_symbol(env, op, loc));
  binary_node_set_right(prefix, operand);
  return prefix;
}

/* 后缀一元形态（.&）：LEFT + SYMBOL，RIGHT 缺失。 */
static node_t *closure_postfix(sema_clone_env_t *env,
                               node_t *operand,
                               const char *op,
                               const location_t *loc) {
  node_t *postfix = create_binary_node(env->out);
  node_set_location(postfix, node_get_location(operand));
  binary_node_set_left(postfix, operand);
  binary_node_set_symbol(postfix, closure_symbol(env, op, loc));
  return postfix;
}

static int closure_is_ident_leaf(node_t *node) {
  return node && node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER &&
         vec_get_size(node_get_children(node)) == 0;
}

/* 名字叶文本比对（临时 buffer，即取即释）。 */
static int closure_leaf_is(node_t *node, allocator_t *out, const char *name) {
  if (!closure_is_ident_leaf(node) || !name) {
    return 0;
  }
  char *text = location_get(node_get_location(node), out);
  int hit = text && strcmp(text, name) == 0;
  allocator_free(out, (void **)&text);
  return hit;
}

static node_t *closure_capture_params(node_t *decl) {
  node_t *captures = func_decl_node_get_captures(decl);
  if (!captures || func_captures_node_get_capture_count(captures) == 0) {
    return NULL;
  }
  return func_captures_node_get_capture(captures, 0);
}

/* ---- 收集期资格检查 ---- */

/* 形态谓词（共同）：STMT_FUNC、无 EXPORT/COMPTIME/装饰器、keyword 文本
   "func"（非 cfunc）、NAME 为名字叶、无泛型、形参全带类型且不占
   "env" 保留名。返回 FUNC_DECL，不达标返回 NULL。 */
static node_t *closure_shape_decl(allocator_t *out, node_t *stmt) {
  if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_FUNC) {
    return NULL;
  }
  if (stmt_func_node_get_export(stmt) || stmt_func_node_get_comptime(stmt) ||
      stmt_func_node_get_decorator_count(stmt) > 0) {
    return NULL;
  }
  node_t *decl = stmt_func_node_get_func_decl(stmt);
  if (!decl) {
    return NULL;
  }
  node_t *keyword = func_decl_node_get_keyword(decl);
  char *kw = keyword ? location_get(node_get_location(keyword), out) : NULL;
  int is_func = kw && strcmp(kw, "func") == 0;
  allocator_free(out, (void **)&kw);
  if (!is_func) {
    return NULL;
  }
  if (!closure_is_ident_leaf(func_decl_node_get_name(decl)) ||
      func_decl_node_get_generics(decl)) {
    return NULL;
  }
  node_t *params = func_decl_node_get_params(decl);
  if (params) {
    vec_t *children = node_get_children(params);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      node_t *param = (node_t *)vec_get(children, idx);
      if (!param || node_get_kind(param) != CLUX_NODE_FUNC_PARAM ||
          !func_param_node_get_type(param)) {
        return NULL;
      }
      if (closure_leaf_is(func_param_node_get_name(param), out, "env")) {
        return NULL;
      }
    }
  }
  return decl;
}

/* 裸捕获锚点类型可用性：kind FUNC / 不可名叶子 / 零元元组 / 引用提升名
   （render 出的名字在产物中已改名，语义漂移）均不可用。 */
static int closure_bare_cap_type_ok(clux_type_t *type, vec_t *hoists) {
  if (!type) {
    return 0;
  }
  switch (clux_type_get_kind(type)) {
  case CLUX_TYPE_FUNC:
    return 0;
  case CLUX_TYPE_PTR:
    return closure_bare_cap_type_ok(clux_type_get_ptr_base(type), hoists);
  case CLUX_TYPE_CONST:
  case CLUX_TYPE_VOLATILE:
    return closure_bare_cap_type_ok(clux_type_unwrap(type), hoists);
  case CLUX_TYPE_OPTIONAL:
    return closure_bare_cap_type_ok(clux_type_get_optional_base(type), hoists);
  case CLUX_TYPE_SLICE:
    return closure_bare_cap_type_ok(clux_slice_get_base(type), hoists);
  case CLUX_TYPE_ARRAY:
    return closure_bare_cap_type_ok(clux_array_get_base(type), hoists);
  case CLUX_TYPE_TUPLE: {
    size_t count = clux_tuple_elem_count(type);
    if (!count) {
      return 0;
    }
    for (size_t idx = 0; idx < count; idx++) {
      if (!closure_bare_cap_type_ok(clux_tuple_get_elem(type, idx), hoists)) {
        return 0;
      }
    }
    return 1;
  }
  default: {
    const char *name = clux_type_get_name(type);
    if (!name) {
      return 0;
    }
    for (size_t idx = 0; idx < vec_get_size(hoists); idx++) {
      sema_hoist_t *hoist = (sema_hoist_t *)vec_get(hoists, idx);
      if (hoist && strcmp(hoist->name, name) == 0) {
        return 0;
      }
    }
    return 1;
  }
  }
}

/* 捕获表提取（收集期定型）：FUNC_CAPTURES 的唯一 FUNC_PARAMS 子节点，
   逐 FUNC_PARAM 定型。返回捕获表（out 持有，可为空表）；任一捕获定型
   失败写入 *ok=0（整条目不退化，调用方释放）。 */
static vec_t *closure_extract_caps(clux_sema_t *sema,
                                   allocator_t *out,
                                   program_t *program,
                                   node_t *decl,
                                   node_t *block,
                                   int *ok) {
  vec_t *caps = create_vec(out);
  *ok = 1;
  node_t *params = closure_capture_params(decl);
  if (!params) {
    return caps;
  }
  vec_t *children = node_get_children(params);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    node_t *param = (node_t *)vec_get(children, idx);
    if (!param || node_get_kind(param) != CLUX_NODE_FUNC_PARAM) {
      *ok = 0;
      continue;
    }
    node_t *name_node = func_param_node_get_name(param);
    char *name = closure_is_ident_leaf(name_node)
                     ? location_get(node_get_location(name_node), out)
                     : NULL;
    if (!name) {
      *ok = 0;
      continue;
    }
    if (strcmp(name, "callee") == 0) {
      /* 合成字段名 callee 保留。 */
      *ok = 0;
      allocator_free(out, (void **)&name);
      continue;
    }
    sema_closure_cap_t *cap = allocator_alloc(out, sizeof(sema_closure_cap_t));
    cap->name = program_intern_string(program, name, strlen(name));
    allocator_free(out, (void **)&name);
    node_t *type_src = func_param_node_get_type(param);
    if (type_src) {
      /* 标注捕获：TYPE 槽为合成期克隆源，值取默认值表达式（可 NULL）。*/
      cap->type_src = type_src;
      cap->default_src = func_param_node_get_default(param);
      cap->type = NULL;
    } else {
      /* 裸捕获：定义块直接子级 STMT_VAR 锚点 → §8.5 类型表定型。
         block=NULL 为 collect 预检遍（定义块尚未定位），只登记名字，
         定型留给带定义块的第二遍。 */
      cap->type_src = NULL;
      cap->default_src = NULL;
      cap->type = NULL;
      if (block) {
        vec_t *stmts = node_get_children(block);
        for (size_t s = 0; s < vec_get_size(stmts); s++) {
          node_t *var_stmt = (node_t *)vec_get(stmts, s);
          if (!var_stmt || node_get_kind(var_stmt) != CLUX_NODE_STMT_VAR) {
            continue;
          }
          if (closure_leaf_is(
                  stmt_var_node_get_name(var_stmt), out, cap->name)) {
            cap->type = sema_get_node_type(sema, var_stmt);
            break;
          }
        }
        if (!cap->type || !closure_bare_cap_type_ok(cap->type, sema->hoists)) {
          *ok = 0;
        }
      }
    }
    vec_push(caps, cap);
  }
  return caps;
}

static void closure_free_caps(allocator_t *out, vec_t **caps) {
  if (!caps || !*caps) {
    return;
  }
  for (size_t idx = 0; idx < vec_get_size(*caps); idx++) {
    sema_closure_cap_t *cap = (sema_closure_cap_t *)vec_get(*caps, idx);
    allocator_free(out, (void **)&cap);
  }
  delete_vec(caps);
}

/* 敏感名字集命中：名字叶 == 自身名或任一捕获名。 */
static int closure_leaf_is_sensitive(allocator_t *out,
                                     node_t *leaf,
                                     const char *self_name,
                                     vec_t *caps) {
  if (closure_leaf_is(leaf, out, self_name)) {
    return 1;
  }
  for (size_t idx = 0; idx < vec_get_size(caps); idx++) {
    sema_closure_cap_t *cap = (sema_closure_cap_t *)vec_get(caps, idx);
    if (cap && closure_leaf_is(leaf, out, cap->name)) {
      return 1;
    }
  }
  return 0;
}

/* 子树含敏感名（skip 子树排除）。 */
static int closure_subtree_has_sensitive(allocator_t *out,
                                         node_t *node,
                                         const char *self_name,
                                         vec_t *caps,
                                         node_t *skip) {
  if (!node || node == skip) {
    return 0;
  }
  if (closure_is_ident_leaf(node) &&
      closure_leaf_is_sensitive(out, node, self_name, caps)) {
    return 1;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (closure_subtree_has_sensitive(
            out, (node_t *)vec_get(children, idx), self_name, caps, skip)) {
      return 1;
    }
  }
  return 0;
}

/* 闭包边界 kind：体内嵌套函数 / defer 子树内 env 不可直达。 */
static int closure_is_func_boundary(node_kind_t kind) {
  return kind == CLUX_NODE_EXPR_FUNC_DECL || kind == CLUX_NODE_STMT_DEFER ||
         kind == CLUX_NODE_STMT_FUNC;
}

/* 声明子树 kind：内含名字槽 / 类型引用，命中敏感名即整条目 bail
   （防克隆期成批改写误伤名字槽）。 */
static int closure_is_decl_subtree(node_kind_t kind) {
  switch (kind) {
  case CLUX_NODE_STMT_STRUCT:
  case CLUX_NODE_STMT_ENUM:
  case CLUX_NODE_STMT_UNION:
  case CLUX_NODE_STMT_TYPE:
  case CLUX_NODE_STMT_BITFIELD:
  case CLUX_NODE_STMT_INTERFACE:
  case CLUX_NODE_EXPR_STRUCT_DECL:
  case CLUX_NODE_EXPR_ENUM_DECL:
  case CLUX_NODE_EXPR_UNION:
  case CLUX_NODE_EXPR_BITFIELD_DECL:
  case CLUX_NODE_EXPR_INTERFACE_DECL:
    return 1;
  default:
    return 0;
  }
}

/* Scan-A：条目自身子树（PARAMS/RETURN 槽 + BODY）敏感名字域检查。
   返回 0=通过，-1=bail。BODY 内规则：捕获名任意位置可（改写
   env.<name>）；自身名只允许 EXPR_CALL callee 位（自递归）。 */
static int closure_scan_body(allocator_t *out,
                             node_t *node,
                             const char *self_name,
                             vec_t *caps) {
  if (!node) {
    return 0;
  }
  node_kind_t kind = node_get_kind(node);
  if (closure_is_func_boundary(kind) || closure_is_decl_subtree(kind)) {
    return closure_subtree_has_sensitive(out, node, self_name, caps, NULL) ? -1
                                                                           : 0;
  }
  if (kind == CLUX_NODE_STMT_VAR) {
    vec_t *children = node_get_children(node);
    node_t *name_slot = vec_get_size(children) > (size_t)CLUX_STMT_VAR_NAME
                            ? (node_t *)vec_get(children, CLUX_STMT_VAR_NAME)
                            : NULL;
    if (closure_leaf_is_sensitive(out, name_slot, self_name, caps)) {
      return -1;
    }
  } else if (kind == CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
    vec_t *children = node_get_children(node);
    node_t *name_slot =
        vec_get_size(children) > (size_t)CLUX_EXPR_CONSTRUCT_FIELD_NAME
            ? (node_t *)vec_get(children, CLUX_EXPR_CONSTRUCT_FIELD_NAME)
            : NULL;
    if (closure_leaf_is_sensitive(out, name_slot, self_name, caps)) {
      return -1;
    }
  } else if (kind == CLUX_NODE_EXPR_CALL) {
    /* 自身名仅限 callee 位（自递归改写 env.callee）；捕获名 callee 位
       合法（调用捕获的函数值，叶克隆期 env.<name> 改写）。 */
    return 0;
  } else if (closure_is_ident_leaf(node)) {
    /* 普通读位置的捕获名正是 env.<name> 改写目标——放行；仅自身名
       敏感（EXPR_CALL 已在上面整体放行，自递归 callee 位合法）。 */
    return closure_leaf_is(node, out, self_name) ? -1 : 0;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (closure_scan_body(
            out, (node_t *)vec_get(children, idx), self_name, caps) != 0) {
      return -1;
    }
  }
  return 0;
}

/* 条目整体（decl 的 PARAMS/RETURN/捕获 TYPE 槽/BODY）Scan-A。 */
static int closure_scan_entry(allocator_t *out,
                              node_t *decl,
                              const char *self_name,
                              vec_t *caps) {
  if (closure_subtree_has_sensitive(
          out, func_decl_node_get_params(decl), self_name, caps, NULL) ||
      closure_subtree_has_sensitive(
          out, func_decl_node_get_return(decl), self_name, caps, NULL)) {
    return -1;
  }
  /* 捕获 TYPE 槽在 struct synth 期按 owner 上下文克隆（caps 改写不
     激活），引用捕获名即悬空——bail；DEFAULT 槽在构造点求值（owner
     现值），不检。 */
  node_t *params = closure_capture_params(decl);
  if (params) {
    vec_t *children = node_get_children(params);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      node_t *param = (node_t *)vec_get(children, idx);
      if (!param || node_get_kind(param) != CLUX_NODE_FUNC_PARAM) {
        continue;
      }
      if (closure_subtree_has_sensitive(
              out, func_param_node_get_type(param), self_name, caps, NULL)) {
        return -1;
      }
    }
  }
  return closure_scan_body(out, func_decl_node_get_body(decl), self_name, caps);
}

/* Scan-B：owner 体（排除条目自身子树）内条目名检查。条目名只允许
   出现在定义块内（in_block）的 EXPR_CALL callee 位——定义块外引用
   改写后指向不可达的块局部 var，bail 回旧通道。 */
static int closure_scan_owner(allocator_t *out,
                              node_t *node,
                              const char *self_name,
                              node_t *entry_stmt,
                              node_t *entry_block,
                              int in_block) {
  if (!node || node == entry_stmt) {
    return 0;
  }
  if (node == entry_block) {
    in_block = 1;
  }
  node_kind_t kind = node_get_kind(node);
  if (closure_is_func_boundary(kind) || closure_is_decl_subtree(kind)) {
    return closure_subtree_has_sensitive(out, node, self_name, NULL, entry_stmt)
               ? -1
               : 0;
  }
  if (kind == CLUX_NODE_EXPR_CALL) {
    /* 定义块内 EXPR_CALL callee 位豁免（调用点改写 add.callee(add.&)）；
       定义块外（含 callee 位）一律 bail——改写后指向不可达的块局部
       var。 */
    vec_t *children = node_get_children(node);
    if (vec_get_size(children) > 0) {
      node_t *callee = (node_t *)vec_get(children, 0);
      int callee_is_self = closure_is_ident_leaf(callee) &&
                           closure_leaf_is(callee, out, self_name);
      if (callee_is_self && !in_block) {
        return -1;
      }
      for (size_t idx = 0; idx < vec_get_size(children); idx++) {
        node_t *child = (node_t *)vec_get(children, idx);
        if (idx == 0 && callee_is_self) {
          continue; /* callee 位：in_block 内放行 */
        }
        if (closure_scan_owner(
                out, child, self_name, entry_stmt, entry_block, in_block) !=
            0) {
          return -1;
        }
      }
    }
    return 0;
  }
  if (kind == CLUX_NODE_STMT_VAR) {
    vec_t *children = node_get_children(node);
    node_t *name_slot = vec_get_size(children) > (size_t)CLUX_STMT_VAR_NAME
                            ? (node_t *)vec_get(children, CLUX_STMT_VAR_NAME)
                            : NULL;
    if (closure_leaf_is(name_slot, out, self_name)) {
      return -1;
    }
  } else if (kind == CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
    vec_t *children = node_get_children(node);
    node_t *name_slot =
        vec_get_size(children) > (size_t)CLUX_EXPR_CONSTRUCT_FIELD_NAME
            ? (node_t *)vec_get(children, CLUX_EXPR_CONSTRUCT_FIELD_NAME)
            : NULL;
    if (closure_leaf_is(name_slot, out, self_name)) {
      return -1;
    }
  } else if (closure_is_ident_leaf(node)) {
    return closure_leaf_is(node, out, self_name) ? -1 : 0;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (closure_scan_owner(out,
                           (node_t *)vec_get(children, idx),
                           self_name,
                           entry_stmt,
                           entry_block,
                           in_block) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Scan-C：同 owner 其它提升项（含退化候选与未退化）体内出现条目名 →
   bail——提升体顶层引用块内 var 不可达（旧通道 rename 会指向不存在的
   提升函数）。 */
static int closure_scan_sibling_hoists(allocator_t *out,
                                       clux_sema_t *sema,
                                       node_t *owner,
                                       node_t *entry_stmt,
                                       const char *self_name) {
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    if (!hoist || hoist->owner != owner || hoist->stmt == entry_stmt) {
      continue;
    }
    if (closure_subtree_has_sensitive(
            out, hoist->stmt, self_name, NULL, NULL)) {
      return -1;
    }
  }
  return 0;
}

/* 定义块定位（清洁路径）：owner 体直属语句树内直接包含 entry_stmt 的
   STMT_BLOCK——不进入嵌套函数 / defer 体（其内定义的闭包退化随后续
   分期）。 */
static node_t *closure_find_defining_block(node_t *node, node_t *target) {
  if (!node) {
    return NULL;
  }
  node_kind_t kind = node_get_kind(node);
  if (closure_is_func_boundary(kind) && node != target) {
    /* 嵌套函数体 / defer 体：owner 边界内不再下潜（随后续分期）。 */
    return NULL;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if ((node_t *)vec_get(children, idx) == target) {
      return kind == CLUX_NODE_STMT_BLOCK ? node : NULL;
    }
  }
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    node_t *hit =
        closure_find_defining_block((node_t *)vec_get(children, idx), target);
    if (hit) {
      return hit;
    }
  }
  return NULL;
}

/* "$closure$M" / "$callee$M"（intern 进产物 program，永久借用）。
   mangled NULL（模块级条目——收集后由调用方以原名覆写）返回 NULL。 */
static const char *closure_synth_name(allocator_t *out,
                                      program_t *program,
                                      const char *prefix,
                                      const char *mangled) {
  if (!mangled) {
    return NULL;
  }
  string_t *joined = create_string(out);
  string_append_cstr(joined, prefix);
  string_append_cstr(joined, mangled);
  const char *text = program_intern_string(
      program, string_get_data(joined), string_get_size(joined));
  delete_string(&joined);
  return text;
}

static sema_closure_t *closure_entry_make(allocator_t *out,
                                          program_t *program,
                                          node_t *stmt,
                                          node_t *owner,
                                          node_t *block,
                                          const char *mangled) {
  node_t *decl = stmt_func_node_get_func_decl(stmt);
  node_t *name_node = func_decl_node_get_name(decl);
  char *name = location_get(node_get_location(name_node), out);
  if (!name) {
    return NULL;
  }
  sema_closure_t *entry = allocator_alloc(out, sizeof(sema_closure_t));
  entry->stmt = stmt;
  entry->owner = owner;
  entry->block = block;
  entry->name = program_intern_string(program, name, strlen(name));
  entry->closure_name = closure_synth_name(out, program, "$closure$", mangled);
  entry->callee_name = closure_synth_name(out, program, "$callee$", mangled);
  entry->captures = NULL;
  allocator_free(out, (void **)&name);
  return entry;
}

vec_t *
sema_closure_collect(clux_sema_t *sema, allocator_t *out, program_t *program) {
  vec_t *closures = create_vec(out);
  if (!sema || !sema->ast) {
    return closures;
  }
  /* 模块级条目：全部退化（含零捕获——var N 通道统一）。 */
  size_t count = program_node_get_stmt_count(sema->ast);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = program_node_get_stmt(sema->ast, idx);
    node_t *decl = closure_shape_decl(out, stmt);
    if (!decl) {
      continue;
    }
    sema_closure_t *entry =
        closure_entry_make(out, program, stmt, NULL, NULL, NULL);
    if (!entry) {
      continue;
    }
    /* 模块级名字 M = 原名（重取 name 文本——entry_make 内部已释放）。 */
    char *mangled =
        location_get(node_get_location(func_decl_node_get_name(decl)), out);
    if (!mangled) {
      allocator_free(out, (void **)&entry);
      continue;
    }
    entry->closure_name =
        closure_synth_name(out, program, "$closure$", mangled);
    entry->callee_name = closure_synth_name(out, program, "$callee$", mangled);
    allocator_free(out, (void **)&mangled);
    int ok = 0;
    entry->captures = closure_extract_caps(sema, out, program, decl, NULL, &ok);
    if (!ok || closure_scan_entry(out, decl, entry->name, entry->captures)) {
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    vec_push(closures, entry);
  }
  /* 块级条目：提升项内 ≥1 捕获 + 敏感名字域全套检查。 */
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    if (!hoist || !hoist->owner ||
        node_get_kind(hoist->owner) != CLUX_NODE_STMT_FUNC) {
      continue; /* struct 成员方法体随后续批次 */
    }
    node_t *decl = closure_shape_decl(out, hoist->stmt);
    if (!decl) {
      continue;
    }
    /* 嵌套在已退化块级条目内：不作候选（保守，保留旧通道）。 */
    int nested = 0;
    for (size_t c = 0; c < vec_get_size(closures); c++) {
      sema_closure_t *done = (sema_closure_t *)vec_get(closures, c);
      if (!done->owner || !done->callee_name) {
        continue;
      }
      size_t prefix_len = strlen(done->callee_name) - strlen("$callee$");
      if (strncmp(hoist->mangled,
                  done->callee_name + strlen("$callee$"),
                  prefix_len) == 0 &&
          hoist->mangled[prefix_len] == '$') {
        nested = 1;
        break;
      }
    }
    if (nested) {
      continue;
    }
    int ok = 0;
    vec_t *caps = closure_extract_caps(sema, out, program, decl, NULL, &ok);
    if (!ok || vec_get_size(caps) == 0) {
      closure_free_caps(out, &caps);
      continue;
    }
    /* 定义块定位依赖捕获锚点（裸捕获查 STMT_VAR），先定位再补定型。 */
    node_t *owner_decl = stmt_func_node_get_func_decl(hoist->owner);
    node_t *owner_body =
        owner_decl ? func_decl_node_get_body(owner_decl) : NULL;
    node_t *block = closure_find_defining_block(owner_body, hoist->stmt);
    if (!block) {
      closure_free_caps(out, &caps);
      continue;
    }
    closure_free_caps(out, &caps);
    caps = closure_extract_caps(sema, out, program, decl, block, &ok);
    if (!ok) {
      closure_free_caps(out, &caps);
      continue;
    }
    sema_closure_t *entry = closure_entry_make(
        out, program, hoist->stmt, hoist->owner, block, hoist->mangled);
    if (!entry) {
      closure_free_caps(out, &caps);
      continue;
    }
    entry->captures = caps;
    if (closure_scan_entry(out, decl, entry->name, entry->captures) != 0) {
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    if (closure_scan_owner(
            out, owner_body, entry->name, entry->stmt, entry->block, 0) != 0) {
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    if (closure_scan_sibling_hoists(
            out, sema, hoist->owner, entry->stmt, entry->name) != 0) {
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    vec_push(closures, entry);
  }
  return closures;
}

sema_closure_t *sema_closure_find_by_stmt(vec_t *closures, node_t *stmt) {
  if (!closures || !stmt) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(closures); idx++) {
    sema_closure_t *entry = (sema_closure_t *)vec_get(closures, idx);
    if (entry && entry->stmt == stmt) {
      return entry;
    }
  }
  return NULL;
}

/* 条目表收尾释放（emit 结束调用；条目与捕获条目均 out 持有，合成名
   intern 进产物 program 不随本表释放失效）。 */
void sema_closure_entries_free(allocator_t *out, vec_t **closures) {
  if (!closures || !*closures) {
    return;
  }
  vec_t *vec = *closures;
  for (size_t idx = 0; idx < vec_get_size(vec); idx++) {
    sema_closure_t *entry = (sema_closure_t *)vec_get(vec, idx);
    if (!entry) {
      continue;
    }
    if (entry->captures) {
      closure_free_caps(out, &entry->captures);
    }
    allocator_free(out, (void **)&entry);
  }
  delete_vec(closures);
}

/* ---- 产物合成 ---- */

/* 构造表达式 .$closure$M{.callee=$callee$M, .<cap>=...}：捕获字段值取
   默认值表达式克隆（标注捕获带默认值），否则为捕获名 identifier
   （块尾现值快照）。 */
static node_t *closure_construct(sema_closure_t *entry, sema_clone_env_t *env) {
  const location_t *loc = node_get_location(entry->stmt);
  node_t *construct = create_construct_node(env->out);
  node_set_location(construct, loc);
  construct_node_set_type(construct,
                          closure_ident(env, entry->closure_name, loc));
  node_t *callee_field = create_construct_field_node(env->out);
  node_set_location(callee_field, loc);
  construct_field_node_set_name(callee_field,
                                closure_ident(env, "callee", loc));
  construct_field_node_set_value(callee_field,
                                 closure_ident(env, entry->callee_name, loc));
  construct_node_add_elem(construct, callee_field);
  for (size_t idx = 0; idx < vec_get_size(entry->captures); idx++) {
    sema_closure_cap_t *cap =
        (sema_closure_cap_t *)vec_get(entry->captures, idx);
    if (!cap) {
      continue;
    }
    node_t *field = create_construct_field_node(env->out);
    node_set_location(field, loc);
    construct_field_node_set_name(field, closure_ident(env, cap->name, loc));
    construct_field_node_set_value(
        field,
        cap->default_src ? sema_clone_node_renamed(cap->default_src, env)
                         : closure_ident(env, cap->name, loc));
    construct_node_add_elem(construct, field);
  }
  return construct;
}

/* var N: $closure$M = <value>（KEYWORD/NAME/TYPE/VALUE/EXPORT/COMPTIME）。 */
static node_t *
closure_var_stmt(sema_closure_t *entry, sema_clone_env_t *env, node_t *value) {
  const location_t *loc = node_get_location(entry->stmt);
  node_t *var = create_stmt_var_node(env->out);
  node_set_location(var, loc);
  stmt_var_node_set_keyword(var, closure_symbol(env, "var", loc));
  stmt_var_node_set_name(var, closure_ident(env, entry->name, loc));
  stmt_var_node_set_type(var, closure_ident(env, entry->closure_name, loc));
  stmt_var_node_set_value(var, value);
  stmt_var_node_set_export(var, NULL);
  stmt_var_node_set_comptime(var, NULL);
  return var;
}

node_t *sema_closure_module_var(sema_closure_t *entry, sema_clone_env_t *env) {
  return closure_var_stmt(entry, env, closure_construct(entry, env));
}

node_t *sema_closure_placeholder(sema_closure_t *entry, sema_clone_env_t *env) {
  /* TDZ 占位：undefined 合成节点须 intern "undefined" 文本——location
     直挂原语句会让 unparse 的 loc-text 回退打出整段原声明。 */
  const location_t *loc = node_get_location(entry->stmt);
  node_t *undef = sema_render_text_node(
      env->out, env->program, CLUX_NODE_LITERAL_UNDEFINED, "undefined", loc);
  return closure_var_stmt(entry, env, undef);
}

node_t *sema_closure_construct_assign(sema_closure_t *entry,
                                      sema_clone_env_t *env) {
  const location_t *loc = node_get_location(entry->stmt);
  node_t *assign = create_assign_node(env->out);
  node_set_location(assign, loc);
  assign_node_set_left(assign, closure_ident(env, entry->name, loc));
  assign_node_set_symbol(assign, closure_symbol(env, "=", loc));
  assign_node_set_value(assign, closure_construct(entry, env));
  node_t *stmt = create_stmt_expr_node(env->out);
  node_set_location(stmt, loc);
  stmt_expr_node_set_expr(stmt, assign);
  return stmt;
}

/* func(*$closure$M, 参数类型...) -> ret：callee 字段类型。形参用裸类型
   形态（TYPE 槽），env 首参经前缀一元 * 包裹。 */
static node_t *closure_callee_field_type(sema_closure_t *entry,
                                         sema_clone_env_t *env) {
  const location_t *loc = node_get_location(entry->stmt);
  node_t *type = create_func_type_node(env->out);
  node_set_location(type, loc);
  func_type_node_set_captures(type, NULL);
  node_t *params = create_func_params_node(env->out);
  node_t *env_param = create_func_param_node(env->out);
  node_set_location(env_param, loc);
  func_param_node_set_type(
      env_param,
      closure_prefix(
          env, "*", closure_ident(env, entry->closure_name, loc), loc));
  func_params_node_add_param(params, env_param);
  node_t *src_params =
      func_decl_node_get_params(stmt_func_node_get_func_decl(entry->stmt));
  if (src_params) {
    vec_t *children = node_get_children(src_params);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      node_t *src_param = (node_t *)vec_get(children, idx);
      if (!src_param || node_get_kind(src_param) != CLUX_NODE_FUNC_PARAM) {
        continue;
      }
      node_t *param = create_func_param_node(env->out);
      node_set_location(param, node_get_location(src_param));
      func_param_node_set_type(
          param,
          sema_clone_node_renamed(func_param_node_get_type(src_param), env));
      func_params_node_add_param(params, param);
    }
  }
  func_type_node_set_params(type, params);
  node_t *src_return =
      func_decl_node_get_return(stmt_func_node_get_func_decl(entry->stmt));
  func_type_node_set_return(type,
                            src_return
                                ? sema_clone_node_renamed(src_return, env)
                                : closure_ident(env, "void", loc));
  return type;
}

node_t *sema_closure_struct_synth(sema_closure_t *entry,
                                  sema_clone_env_t *env) {
  const location_t *loc = node_get_location(entry->stmt);
  node_t *members = create_struct_members_node(env->out);
  node_set_location(members, loc);
  node_t *callee_field = create_field_node(env->out);
  node_set_location(callee_field, loc);
  field_node_set_name(callee_field, closure_ident(env, "callee", loc));
  field_node_set_type(callee_field, closure_callee_field_type(entry, env));
  struct_members_node_add_member(members, callee_field);
  for (size_t idx = 0; idx < vec_get_size(entry->captures); idx++) {
    sema_closure_cap_t *cap =
        (sema_closure_cap_t *)vec_get(entry->captures, idx);
    if (!cap) {
      continue;
    }
    node_t *field = create_field_node(env->out);
    node_set_location(field, loc);
    field_node_set_name(field, closure_ident(env, cap->name, loc));
    field_node_set_type(
        field,
        cap->type_src
            ? sema_clone_node_renamed(cap->type_src, env)
            : sema_dispose_render_type(env->out, env->program, cap->type, loc));
    struct_members_node_add_member(members, field);
  }
  node_t *decl = create_struct_decl_node(env->out);
  node_set_location(decl, loc);
  struct_decl_node_set_keyword(decl, closure_symbol(env, "struct", loc));
  struct_decl_node_set_name(decl, closure_ident(env, entry->closure_name, loc));
  struct_decl_node_set_generics(decl, NULL);
  struct_decl_node_set_implements(decl, NULL);
  struct_decl_node_set_body(decl, members);
  node_t *stmt = create_stmt_struct_node(env->out);
  node_set_location(stmt, loc);
  stmt_struct_node_set_struct_decl(stmt, decl);
  stmt_struct_node_set_export(stmt, NULL);
  return stmt;
}

node_t *sema_closure_callee_synth(sema_closure_t *entry,
                                  sema_clone_env_t *env) {
  node_t *src_decl = stmt_func_node_get_func_decl(entry->stmt);
  const location_t *loc = node_get_location(entry->stmt);
  node_t *fn = create_func_decl_node(env->out);
  node_set_location(fn, loc);
  func_decl_node_set_keyword(fn, closure_symbol(env, "func", loc));
  func_decl_node_set_name(fn, closure_ident(env, entry->callee_name, loc));
  func_decl_node_set_generics(fn, NULL);
  func_decl_node_set_captures(fn, NULL);
  node_t *params = create_func_params_node(env->out);
  node_t *env_param = create_func_param_node(env->out);
  node_set_location(env_param, loc);
  func_param_node_set_name(env_param, closure_ident(env, "env", loc));
  func_param_node_set_type(
      env_param,
      closure_prefix(
          env, "*", closure_ident(env, entry->closure_name, loc), loc));
  func_params_node_add_param(params, env_param);
  node_t *src_params = func_decl_node_get_params(src_decl);
  if (src_params) {
    vec_t *children = node_get_children(src_params);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      node_t *src_param = (node_t *)vec_get(children, idx);
      if (src_param) {
        func_params_node_add_param(params,
                                   sema_clone_node_renamed(src_param, env));
      }
    }
  }
  func_decl_node_set_params(fn, params);
  func_decl_node_set_return(
      fn,
      func_decl_node_get_return(src_decl)
          ? sema_clone_node_renamed(func_decl_node_get_return(src_decl), env)
          : NULL);
  /* 函数体边界水位：callee 体内 return/break/continue 不越出合成体
     unwind（外层登记项对 callee 体不可见）。caps/self 由调用方
     （sema.c 合成轮）配对注入。 */
  size_t saved_func_mark = env->unwind_func_mark;
  size_t saved_loop_mark = env->unwind_loop_mark;
  env->unwind_func_mark = vec_get_size(env->unwind);
  env->unwind_loop_mark = env->unwind_func_mark;
  func_decl_node_set_body(
      fn, sema_clone_node_renamed(func_decl_node_get_body(src_decl), env));
  env->unwind_func_mark = saved_func_mark;
  env->unwind_loop_mark = saved_loop_mark;
  node_t *stmt = create_stmt_func_node(env->out);
  node_set_location(stmt, loc);
  stmt_func_node_set_func_decl(stmt, fn);
  stmt_func_node_set_export(stmt, NULL);
  stmt_func_node_set_comptime(stmt, NULL);
  return stmt;
}

/* ---- 调用点 / 捕获引用改写（clone.c 接线） ---- */

static sema_closure_t *closure_find_call_target(sema_clone_env_t *env,
                                                const char *text) {
  for (size_t idx = 0; idx < vec_get_size(env->closures); idx++) {
    sema_closure_t *entry = (sema_closure_t *)vec_get(env->closures, idx);
    if (!entry || strcmp(entry->name, text) != 0) {
      continue;
    }
    /* 块级条目只在其 owner 体克隆语境内可达（定义块的局部 var）。 */
    if (entry->owner == NULL || entry->owner == env->closure_owner) {
      return entry;
    }
  }
  return NULL;
}

node_t *sema_desugar_closure_call(node_t *node, sema_clone_env_t *env) {
  vec_t *children = node_get_children(node);
  node_t *callee =
      vec_get_size(children) > 0 ? (node_t *)vec_get(children, 0) : NULL;
  if (!closure_is_ident_leaf(callee) &&
      !(env->closure_self && closure_is_ident_leaf(callee))) {
    return NULL;
  }
  char *text = location_get(node_get_location(callee), env->out);
  if (!text) {
    return NULL;
  }
  const location_t *loc = node_get_location(node);
  node_t *call = create_call_node(env->out);
  node_set_location(call, loc);
  /* 自递归优先：env.callee(env, args)——env 已是 *$closure$M 指针，
     直传不取址。 */
  if (env->closure_self && strcmp(text, env->closure_self) == 0) {
    allocator_free(env->out, (void **)&text);
    call_node_set_callee(call,
                         closure_binary(env,
                                        closure_ident(env, "env", loc),
                                        ".",
                                        closure_ident(env, "callee", loc),
                                        loc));
    call_node_add_arg(call, closure_ident(env, "env", loc));
    for (size_t idx = 1; idx < vec_get_size(children); idx++) {
      call_node_add_arg(
          call, sema_clone_lift_guard((node_t *)vec_get(children, idx), env));
    }
    return call;
  }
  if (!env->closures) {
    allocator_free(env->out, (void **)&text);
    delete_node(&call);
    return NULL;
  }
  sema_closure_t *entry = closure_find_call_target(env, text);
  allocator_free(env->out, (void **)&text);
  if (!entry) {
    /* call 未挂树即早退——delete_node 连同 children 一并释放。 */
    delete_node(&call);
    return NULL;
  }
  call_node_set_callee(call,
                       closure_binary(env,
                                      closure_ident(env, entry->name, loc),
                                      ".",
                                      closure_ident(env, "callee", loc),
                                      loc));
  call_node_add_arg(
      call,
      closure_postfix(env, closure_ident(env, entry->name, loc), ".&", loc));
  for (size_t idx = 1; idx < vec_get_size(children); idx++) {
    call_node_add_arg(
        call, sema_clone_lift_guard((node_t *)vec_get(children, idx), env));
  }
  return call;
}

node_t *sema_closure_cap_rewrite(node_t *node, sema_clone_env_t *env) {
  if (!env->closure_caps || !closure_is_ident_leaf(node)) {
    return NULL;
  }
  char *text = location_get(node_get_location(node), env->out);
  if (!text) {
    return NULL;
  }
  sema_closure_cap_t *hit = NULL;
  for (size_t idx = 0; idx < vec_get_size(env->closure_caps); idx++) {
    sema_closure_cap_t *cap =
        (sema_closure_cap_t *)vec_get(env->closure_caps, idx);
    if (cap && strcmp(cap->name, text) == 0) {
      hit = cap;
      break;
    }
  }
  allocator_free(env->out, (void **)&text);
  if (!hit) {
    return NULL;
  }
  const location_t *loc = node_get_location(node);
  return closure_binary(env,
                        closure_ident(env, "env", loc),
                        ".",
                        closure_ident(env, hit->name, loc),
                        loc);
}
