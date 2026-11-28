/* §2.11 闭包退化 P1：具名非 cfunc 函数 → 「$closure$M struct +
   $callee$M 纯函数」双产物（emit 期脱糖）。
   - 协议（用户示例）：闭包变量平铺进 callee 打头的 struct；callee 首参
     $env: *$closure$M；体内捕获引用改写 $env.<name>；调用点
     N(args) → N.$callee(N.&, args)；自递归 $env.$callee($env, ...)。
   - 块级条目（块尾延迟绑定）：定义点换 var N: $closure$M = undefined
     （TDZ 占位），块尾（unwind 之前）追加构造赋值——mid-block 调用读
     TDZ var 报错是有意语义。
   - 模块级条目：定义语句原位换 var N: $closure$N = .$closure$N{...}。
   - 资格（collect 期谓词全集）：具名 func（cfunc/泛型/export/comptime/
     装饰器/匿名不退化）；模块级全部退化（含零捕获）；块级 P2 起含零
     捕获（定义点原位构造——无前向依赖）+ 含捕获条目块尾延迟绑定；
     定义块须在 owner 体直属语句树内（不进嵌套函数/defer 体）+ 敏感名
     字域检查。P2 收紧：块级条目 bail 一律显式报错阻断 emit（不再静默
     跌回旧提升通道——提升为静态函数丢失闭包值语义）；嵌套条目（定义
     在已退化条目体内）保留旧通道待 P3 递归退化。全部机制落 emit 期：
     check 在原树上照跑（诊断零变化），合成体直进产物不经 check。 */
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
   "func"（非 cfunc）、NAME 为名字叶、无泛型、形参全带类型。返回
   FUNC_DECL，不达标返回 NULL。 */
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
      /* 形参名 env 不再保留——合成首参已迁移 $env（§1.3 '$' 前缀隔离），
         用户形参 env 与之并存无冲突。 */
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

/* 捕获表提取（收集期登记）：FUNC_CAPTURES 的唯一 FUNC_PARAMS 子节点，
   逐 FUNC_PARAM 登记。标注捕获即带 TYPE 槽克隆源；裸捕获只登记名字，
   锚点定型延后到 closure_anchor_caps（按源可见集统一锚定——§2.12 P3
   跨层捕获：定义块 / 块路径 / 源形参 / 容器捕获）。返回捕获表（out
   持有，可为空表）；形态不合法写入 *ok=0（整条目不退化，调用方释放）。*/
static vec_t *closure_extract_caps(allocator_t *out,
                                   program_t *program,
                                   node_t *decl,
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
    /* 捕获名 callee 不再保留——合成协议键已迁移 $callee（§1.3 三分法：
       '$' 前缀隔离编译器合成名），用户捕获字段 callee 与之并存无冲突。 */
    sema_closure_cap_t *cap = allocator_alloc(out, sizeof(sema_closure_cap_t));
    cap->name = program_intern_string(program, name, strlen(name));
    allocator_free(out, (void **)&name);
    node_t *type_src = func_param_node_get_type(param);
    cap->type_src = type_src;
    cap->default_src = type_src ? func_param_node_get_default(param) : NULL;
    cap->type = NULL;
    cap->via_env = 0;
    cap->discovered = 0;
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

/* 敏感名字集命中：名字叶 == 自身名或任一捕获名。caps == NULL 表示
   只查自身名（scan_owner 边界分支：克隆后由 caps 通道处理捕获名）。 */
static int closure_leaf_is_sensitive(allocator_t *out,
                                     node_t *leaf,
                                     const char *self_name,
                                     vec_t *caps) {
  if (closure_leaf_is(leaf, out, self_name)) {
    return 1;
  }
  if (!caps) {
    return 0;
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

/* 指针包含搜索（AST 包含判定——§2.12 P3 嵌套资格）。 */
static int closure_stmt_in_subtree(node_t *node, node_t *target) {
  if (!node) {
    return 0;
  }
  if (node == target) {
    return 1;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (closure_stmt_in_subtree((node_t *)vec_get(children, idx), target)) {
      return 1;
    }
  }
  return 0;
}

static int closure_stmt_in_vec(vec_t *vec, node_t *stmt) {
  if (!vec) {
    return 0;
  }
  for (size_t idx = 0; idx < vec_get_size(vec); idx++) {
    if ((node_t *)vec_get(vec, idx) == stmt) {
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
   $env.<name>）；自身名只允许 EXPR_CALL callee 位（自递归）。嵌套候选
   子树（cand 命中）放行——其在容器体克隆中被 skip 通道整体替换，体内
   名字由各候选自己的发现/敏感域扫描收口。 */
static int closure_scan_body(allocator_t *out,
                             node_t *node,
                             const char *self_name,
                             vec_t *caps,
                             vec_t *cand) {
  if (!node) {
    return 0;
  }
  node_kind_t kind = node_get_kind(node);
  if (kind == CLUX_NODE_STMT_FUNC && closure_stmt_in_vec(cand, node)) {
    return 0; /* 嵌套候选子树放行 */
  }
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
    /* 自身名仅限 callee 位（自递归改写 $env.$callee）；捕获名 callee 位
       合法（调用捕获的函数值，叶克隆期 $env.<name> 改写）。 */
    return 0;
  } else if (closure_is_ident_leaf(node)) {
    /* 普通读位置的捕获名正是 $env.<name> 改写目标——放行；仅自身名
       敏感（EXPR_CALL 已在上面整体放行，自递归 callee 位合法）。 */
    return closure_leaf_is(node, out, self_name) ? -1 : 0;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (closure_scan_body(
            out, (node_t *)vec_get(children, idx), self_name, caps, cand) !=
        0) {
      return -1;
    }
  }
  return 0;
}

/* 条目整体（decl 的 PARAMS/RETURN/捕获 TYPE 槽/BODY）Scan-A。 */
static int closure_scan_entry(allocator_t *out,
                              node_t *decl,
                              const char *self_name,
                              vec_t *caps,
                              vec_t *cand) {
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
  return closure_scan_body(
      out, func_decl_node_get_body(decl), self_name, caps, cand);
}

/* Scan-B：owner 体（排除条目自身子树）内条目名检查。条目名只允许
   出现在定义块内（in_block）的 EXPR_CALL callee 位——定义块外引用
   改写后指向不可达的块局部 var，bail。包含条目的祖先候选子树递归
   进入保持 positional 语义（嵌套条目的定义块/调用域在容器体克隆中
   物化）；其余边界子树整体敏感检查。 */
static int closure_scan_owner(allocator_t *out,
                              node_t *node,
                              const char *self_name,
                              node_t *entry_stmt,
                              node_t *entry_block,
                              int in_block,
                              vec_t *cand) {
  if (!node || node == entry_stmt) {
    return 0;
  }
  if (node == entry_block) {
    in_block = 1;
  }
  node_kind_t kind = node_get_kind(node);
  if ((closure_is_func_boundary(kind) || closure_is_decl_subtree(kind)) &&
      !((kind == CLUX_NODE_STMT_FUNC || kind == CLUX_NODE_EXPR_FUNC_DECL) &&
        cand && closure_stmt_in_subtree(node, entry_stmt))) {
    /* 放行域 = 包含条目的祖先候选子树（STMT_FUNC 及其声明本体
       EXPR_FUNC_DECL——后者是前者的唯一直接子级，若不放行会在
       递归中再次落入 boundary 整树检查，误伤嵌套条目体内引用）。 */
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
        if (closure_scan_owner(out,
                               child,
                               self_name,
                               entry_stmt,
                               entry_block,
                               in_block,
                               cand) != 0) {
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
                           in_block,
                           cand) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Scan-C：同 owner 其它提升项（含退化候选与未退化）体内出现条目名 →
   bail——提升体顶层引用块内 var 不可达（旧通道 rename 会指向不存在的
   提升函数）。包含条目的祖先候选除外（其体内合法引用由 Scan-B 的
   positional 检查收口）。 */
static int closure_scan_sibling_hoists(allocator_t *out,
                                       clux_sema_t *sema,
                                       node_t *owner,
                                       node_t *entry_stmt,
                                       const char *self_name,
                                       vec_t *cand) {
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    if (!hoist || hoist->owner != owner || hoist->stmt == entry_stmt) {
      continue;
    }
    if (closure_stmt_in_vec(cand, hoist->stmt) &&
        closure_stmt_in_subtree(hoist->stmt, entry_stmt)) {
      continue; /* 祖先候选：嵌套条目的定义块内调用合法 */
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

/* ---- §2.12 P3 批次 1：嵌套资格 AST 包含 + 递归退化 + 跨层捕获 ----
   - 嵌套资格：AST 指针包含判定替代旧名字前缀检测（前缀法漏掉模块级
     条目作容器、误伤同名链）；直接容器 = 条目表中体子树包含候选 stmt
     的最深条目（收集序 = walk 先序，祖先先于后代，最后命中即最深）。
   - 递归退化：嵌套候选照常入表——克隆语境（skip/块尾构造/调用改写）
     按 owner（恒为顶层 stmt）+ 原树块/句指针匹配，容器体克隆时天然
     落位，clone.c 零改动；收集期扫描域改按容器体。
   - 跨层捕获：捕获来源 S = 直接容器（否则 owner 兜底——owner 未退化
     时其体克隆同样带 closure_owner）。可见名集 = S 形参（plain，TYPE
     槽克隆源）+ S 捕获（via_env——构造点渲染 $env.<name>，借用容器
     捕获锚点）+ 定义块至 S 体根的块路径 STMT_VAR（§8.5 锚点 + 可存储
     校验）。隐式发现扫描候选体把自由名收敛为捕获；不可改写位置 / 经
     中间闭包的多级链 / 按名引用父闭包 → 显式 bail。 */

/* 直接容器 = 收集表中体子树包含 stmt 的最深条目；模块级条目（owner
   NULL）天然可作容器。未命中返回 NULL（候选直接嵌套于顶层函数体）。 */
static sema_closure_t *closure_find_container(vec_t *closures, node_t *stmt) {
  sema_closure_t *deepest = NULL;
  for (size_t idx = 0; idx < vec_get_size(closures); idx++) {
    sema_closure_t *entry = (sema_closure_t *)vec_get(closures, idx);
    if (entry && closure_stmt_in_subtree(entry->stmt, stmt)) {
      deepest = entry;
    }
  }
  return deepest;
}

/* 提升候选 stmt 集：owner 为顶层 STMT_FUNC 且 shape 合格的提升项。
   Scan-A/发现扫描对嵌套候选子树放行——其子树在容器体克隆中被 skip
   通道整体替换（不进 $callee 体产物）；候选最终失败必经 diag 通道
   （形态外的失败均已收紧为显式报错），emit 整体短路，放行安全。 */
static vec_t *closure_candidate_stmts(clux_sema_t *sema, allocator_t *out) {
  vec_t *cand = create_vec(out);
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    if (!hoist || !hoist->owner ||
        node_get_kind(hoist->owner) != CLUX_NODE_STMT_FUNC) {
      continue;
    }
    if (closure_shape_decl(out, hoist->stmt)) {
      vec_push(cand, hoist->stmt);
    }
  }
  return cand;
}

/* 块路径条目：block = 路径上的 STMT_BLOCK，lead = 该块内通往目标方向
   的子级下标（叶块 = 候选语句自身；块语义下位置先于通路的 var 才对
   候选可见——与符号激活语义一致）。DFS 回溯期压栈：向量为叶→根序。 */
typedef struct {
  node_t *block;
  size_t lead;
} closure_block_ref_t;

static int closure_block_path(allocator_t *out,
                              node_t *node,
                              node_t *target,
                              vec_t *path) {
  if (!node || node == target) {
    return node == target;
  }
  if (closure_is_func_boundary(node_get_kind(node))) {
    return 0;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (closure_block_path(
            out, (node_t *)vec_get(children, idx), target, path)) {
      if (node_get_kind(node) == CLUX_NODE_STMT_BLOCK) {
        closure_block_ref_t *ref = allocator_alloc(out, sizeof(*ref));
        ref->block = node;
        ref->lead = idx;
        vec_push(path, ref); /* 叶→根序：叶块先入，根块后入 */
      }
      return 1;
    }
  }
  return 0;
}

/* ---- 源可见集（跨层捕获锚点） ---- */

typedef struct {
  char *name;              /* out 持有 */
  int kind;                /* 0=源形参 1=容器捕获 2=块路径 var */
  node_t *type_src;        /* 形参 TYPE 槽（借用源树） */
  clux_type_t *type;       /* var §8.5 锚点（借用类型表） */
  sema_closure_cap_t *cap; /* 容器捕获条目（借用） */
} closure_vis_t;

static void closure_visible_free(allocator_t *out, vec_t **vis) {
  if (!vis || !*vis) {
    return;
  }
  vec_t *vec = *vis;
  for (size_t idx = 0; idx < vec_get_size(vec); idx++) {
    closure_vis_t *entry = (closure_vis_t *)vec_get(vec, idx);
    if (entry) {
      allocator_free(out, (void **)&entry->name);
      allocator_free(out, (void **)&entry);
    }
  }
  delete_vec(vis);
}

/* 同名遮蔽：深层路径块 var 覆盖浅层/形参（遮蔽仅在路径内成立）。
   name 由本函数复制持有，调用方临时串可随即释放。 */
static void closure_visible_add(allocator_t *out,
                                vec_t *vis,
                                const char *name,
                                int kind,
                                node_t *type_src,
                                clux_type_t *type,
                                sema_closure_cap_t *cap) {
  for (size_t idx = 0; idx < vec_get_size(vis); idx++) {
    closure_vis_t *entry = (closure_vis_t *)vec_get(vis, idx);
    if (entry && strcmp(entry->name, name) == 0) {
      allocator_free(out, (void **)&entry->name);
      allocator_free(out, (void **)&entry);
      vec_set(vis, idx, NULL);
      break;
    }
  }
  char *owned = allocator_alloc(out, strlen(name) + 1);
  memcpy(owned, name, strlen(name) + 1);
  for (size_t idx = 0; idx < vec_get_size(vis); idx++) {
    if (!vec_get(vis, idx)) {
      closure_vis_t *entry = allocator_alloc(out, sizeof(closure_vis_t));
      entry->name = owned;
      entry->kind = kind;
      entry->type_src = type_src;
      entry->type = type;
      entry->cap = cap;
      vec_set(vis, idx, entry);
      return;
    }
  }
  closure_vis_t *entry = allocator_alloc(out, sizeof(closure_vis_t));
  entry->name = owned;
  entry->kind = kind;
  entry->type_src = type_src;
  entry->type = type;
  entry->cap = cap;
  vec_push(vis, entry);
}

static closure_vis_t *closure_vis_find(vec_t *vis, const char *text) {
  for (size_t idx = 0; idx < vec_get_size(vis); idx++) {
    closure_vis_t *entry = (closure_vis_t *)vec_get(vis, idx);
    if (entry && strcmp(entry->name, text) == 0) {
      return entry;
    }
  }
  return NULL;
}

/* 源可见集：S = container（退化容器）否则 owner（未退化顶层函数——
   其体克隆经 has_hoists_for 同样携带 closure_owner，机制一致）。
   def_stmt 为候选定义点（NULL 表模块级祖先的可见集重建）时块路径收
   集生效；可见性 = S 形参（plain）+ S 捕获（via_env，借用容器条目）
   + S 体根至 def_stmt 的 STMT_BLOCK 链上全部直接子级 STMT_VAR（§8.5
   锚点，可存性交 anchor 阶段校验；后向 var 一并收录——构造点在块
   尾，取值时已定型）。深层路径 var 遮蔽浅层与形参。 */
static vec_t *closure_visible_build(clux_sema_t *sema,
                                    allocator_t *out,
                                    sema_closure_t *container,
                                    node_t *owner,
                                    node_t *def_stmt) {
  vec_t *vis = create_vec(out);
  node_t *s_stmt = container ? container->stmt : owner;
  if (!s_stmt) {
    return vis; /* 模块级祖先：其作用域名即模块级名字，产物直接可达 */
  }
  node_t *s_decl = stmt_func_node_get_func_decl(s_stmt);
  if (!s_decl) {
    return vis;
  }
  /* S 形参（plain 来源）：TYPE 槽为合成期克隆源（§8.5 对形参不落锚，
     可存储性交由类型节点原样克隆保证）。 */
  node_t *params = func_decl_node_get_params(s_decl);
  if (params) {
    vec_t *children = node_get_children(params);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      node_t *param = (node_t *)vec_get(children, idx);
      if (!param || node_get_kind(param) != CLUX_NODE_FUNC_PARAM) {
        continue;
      }
      node_t *name_node = func_param_node_get_name(param);
      char *name = closure_is_ident_leaf(name_node)
                       ? location_get(node_get_location(name_node), out)
                       : NULL;
      if (name) {
        closure_visible_add(
            out, vis, name, 0, func_param_node_get_type(param), NULL, NULL);
      }
      allocator_free(out, (void **)&name); /* 释放临时，add 已复制留存 */
    }
  }
  /* S 捕获（via_env 来源）：构造点在容器 callee 体内渲染 $env.<name>
     （S 的 env 彼时在域内）；类型借用容器捕获条目。 */
  if (container && container->captures) {
    vec_t *caps = container->captures;
    for (size_t idx = 0; idx < vec_get_size(caps); idx++) {
      sema_closure_cap_t *cap = (sema_closure_cap_t *)vec_get(caps, idx);
      if (cap && cap->name) {
        closure_visible_add(out, vis, cap->name, 1, NULL, cap->type, cap);
      }
    }
  }
  /* 块路径 var：S 体根 → def_stmt 链上全部直接子级 STMT_VAR（含后向
     var——构造点在块尾，取值时已定型；显式裸捕获后向语义与发现式
     对齐）。路径向量叶→根序，根→叶遍历使深层遮蔽浅层。 */
  if (def_stmt) {
    vec_t *path = create_vec(out);
    node_t *body = func_decl_node_get_body(s_decl);
    if (body && closure_block_path(out, body, def_stmt, path)) {
      for (size_t pidx = vec_get_size(path); pidx > 0; pidx--) {
        closure_block_ref_t *ref =
            (closure_block_ref_t *)vec_get(path, pidx - 1);
        vec_t *children = node_get_children(ref->block);
        for (size_t cidx = 0; cidx < vec_get_size(children); cidx++) {
          node_t *child = (node_t *)vec_get(children, cidx);
          if (!child || node_get_kind(child) != CLUX_NODE_STMT_VAR) {
            continue;
          }
          node_t *name_node = stmt_var_node_get_name(child);
          char *name = closure_is_ident_leaf(name_node)
                           ? location_get(node_get_location(name_node), out)
                           : NULL;
          if (name) {
            closure_visible_add(
                out, vis, name, 2, NULL, sema_get_node_type(sema, child), NULL);
            allocator_free(out, (void **)&name);
          }
        }
      }
    }
    for (size_t pidx = 0; pidx < vec_get_size(path); pidx++) {
      closure_block_ref_t *ref = (closure_block_ref_t *)vec_get(path, pidx);
      allocator_free(out, (void **)&ref);
    }
    delete_vec(&path);
  }
  return vis;
}

/* ---- 锚点定型 + 隐式发现（§2.12 P3 跨层捕获） ---- */

/* 捕获锚点定型：显式标注捕获（有 type_src）不动；裸捕获 / 发现式捕获
   按源可见集三路锚定——kind 0 源形参（TYPE 槽克隆源，可存储性由类型
   节点原样克隆保证）、kind 1 容器捕获（via_env，借用容器捕获锚点）、
   kind 2 块路径 var（§8.5 类型 + 可存储校验）。显式裸捕获（非发现式）
   可见集未命中时回退定义块全量 var 扫描（旧语义：var 声明允许后于
   闭包定义——构造点在块尾，取值时已定型）。不命中或校验失败返回
   0（调用方 bail）。 */
static int closure_anchor_caps(clux_sema_t *sema,
                               allocator_t *out,
                               vec_t *vis,
                               vec_t *caps,
                               node_t *entry_block) {
  (void)out;
  if (!caps) {
    return 1;
  }
  for (size_t idx = 0; idx < vec_get_size(caps); idx++) {
    sema_closure_cap_t *cap = (sema_closure_cap_t *)vec_get(caps, idx);
    if (!cap || cap->type_src) {
      continue; /* 显式标注捕获已定型 */
    }
    closure_vis_t *hit = closure_vis_find(vis, cap->name);
    if (!hit && !cap->discovered && entry_block) {
      /* 后向 var 兜底：定义块直接子级全量扫描（不做 positional 限定）。*/
      vec_t *stmts = node_get_children(entry_block);
      for (size_t s = 0; s < vec_get_size(stmts); s++) {
        node_t *var_stmt = (node_t *)vec_get(stmts, s);
        if (!var_stmt || node_get_kind(var_stmt) != CLUX_NODE_STMT_VAR) {
          continue;
        }
        if (closure_leaf_is(stmt_var_node_get_name(var_stmt), out, cap->name)) {
          clux_type_t *type = sema_get_node_type(sema, var_stmt);
          if (type && closure_bare_cap_type_ok(type, sema->hoists)) {
            cap->type = type;
          }
          break;
        }
      }
      if (cap->type) {
        continue;
      }
    }
    if (!hit) {
      return 0; /* 裸捕获不可达或发现名不在可见集 */
    }
    if (hit->kind == 0) {
      cap->type_src = hit->type_src;
      cap->default_src = NULL;
    } else if (hit->kind == 1) {
      if (!hit->cap) {
        return 0; /* 容器捕获锚点缺失（未定型形态） */
      }
      cap->via_env = 1;
      cap->type = hit->cap->type;
      cap->type_src = hit->cap->type_src;
      if (!cap->type && !cap->type_src) {
        return 0;
      }
    } else {
      if (!hit->type || !closure_bare_cap_type_ok(hit->type, sema->hoists)) {
        return 0;
      }
      cap->type = hit->type;
    }
  }
  return 1;
}

/* 发现扫描语境：作用域栈（positional）+ 源可见集 + 捕获表 + 容器链
   bail 名单。返回码：0=通过，-1=不可改写位置命中，-2=非候选边界内
   引用，-3=经中间闭包的跨层链，-4=按名引用容器闭包，-5=锚定失败。 */
typedef struct {
  clux_sema_t *sema;
  allocator_t *out;
  program_t *program;
  vec_t *cand;   /* 候选 stmt 集（子树放行） */
  vec_t *scopes; /* 作用域栈：vec<vec<char*>>，out 持有名字 */
  vec_t *vis;    /* 源可见集（命中 → 发现捕获） */
  vec_t *caps;   /* 条目捕获表（发现登记，去重） */
  vec_t *chain;  /* const char* 借用：链上块级容器名 */
  vec_t *anc;    /* vec<vec<closure_vis_t*>> 借用：祖先可见名并集 */
} closure_disc_t;

static int closure_scope_has(vec_t *scopes, const char *text) {
  for (size_t sidx = vec_get_size(scopes); sidx > 0; sidx--) {
    vec_t *scope = (vec_t *)vec_get(scopes, sidx - 1);
    for (size_t idx = 0; idx < vec_get_size(scope); idx++) {
      if (strcmp((const char *)vec_get(scope, idx), text) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static void
closure_scope_declare(allocator_t *out, vec_t *scopes, node_t *name_node) {
  if (vec_get_size(scopes) == 0) {
    return;
  }
  char *text = closure_is_ident_leaf(name_node)
                   ? location_get(node_get_location(name_node), out)
                   : NULL;
  if (!text) {
    return;
  }
  char *owned = allocator_alloc(out, strlen(text) + 1);
  memcpy(owned, text, strlen(text) + 1);
  allocator_free(out, (void **)&text);
  vec_push((vec_t *)vec_get(scopes, vec_get_size(scopes) - 1), owned);
}

static int closure_chain_has(vec_t *chain, const char *text) {
  for (size_t idx = 0; idx < vec_get_size(chain); idx++) {
    if (strcmp((const char *)vec_get(chain, idx), text) == 0) {
      return 1;
    }
  }
  return 0;
}

static int closure_anc_has(vec_t *anc, const char *text) {
  for (size_t aidx = vec_get_size(anc); aidx > 0; aidx--) {
    vec_t *vis = (vec_t *)vec_get(anc, aidx - 1);
    if (closure_vis_find(vis, text)) {
      return 1;
    }
  }
  return 0;
}

/* 发现登记（去重）：读位置命中可见集 → 收敛为捕获（克隆期 $env.<name>
   改写）。 */
static void closure_disc_cover(closure_disc_t *ctx, const char *name) {
  for (size_t idx = 0; idx < vec_get_size(ctx->caps); idx++) {
    sema_closure_cap_t *cap = (sema_closure_cap_t *)vec_get(ctx->caps, idx);
    if (cap && strcmp(cap->name, name) == 0) {
      return;
    }
  }
  sema_closure_cap_t *cap =
      allocator_alloc(ctx->out, sizeof(sema_closure_cap_t));
  cap->name = program_intern_string(ctx->program, name, strlen(name));
  cap->type_src = NULL;
  cap->default_src = NULL;
  cap->type = NULL;
  cap->via_env = 0;
  cap->discovered = 1;
  vec_push(ctx->caps, cap);
}

/* 读位置分类：local → 发现捕获 → 链上容器名(-4) → 祖先可见名(-3) →
   忽略（模块级名，产物直接可达）。 */
static int closure_disc_read(closure_disc_t *ctx, const char *text) {
  if (closure_scope_has(ctx->scopes, text)) {
    return 0;
  }
  if (closure_vis_find(ctx->vis, text)) {
    closure_disc_cover(ctx, text);
    return 0;
  }
  if (closure_chain_has(ctx->chain, text)) {
    return -4;
  }
  if (closure_anc_has(ctx->anc, text)) {
    return -3;
  }
  return 0;
}

/* 非候选边界（旧通道嵌套函数/defer/声明子树）内命中三类外层名 → -2；
   其体内名字产物不可达，语义漂移，显式 bail。 */
static int closure_disc_boundary(closure_disc_t *ctx, node_t *node) {
  if (!node) {
    return 0;
  }
  if (closure_is_ident_leaf(node)) {
    char *text = location_get(node_get_location(node), ctx->out);
    if (!text) {
      return 0;
    }
    int hit = closure_vis_find(ctx->vis, text) != NULL ||
              closure_chain_has(ctx->chain, text) ||
              closure_anc_has(ctx->anc, text);
    allocator_free(ctx->out, (void **)&text);
    return hit;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (closure_disc_boundary(ctx, (node_t *)vec_get(children, idx))) {
      return 1;
    }
  }
  return 0;
}

/* type_pos：类型/默认值位置的读命中三类外层名 → -1（克隆改写无法
   触达该位置）。 */
static int closure_disc_scan(closure_disc_t *ctx, node_t *node, int type_pos) {
  if (!node) {
    return 0;
  }
  node_kind_t kind = node_get_kind(node);
  if (kind == CLUX_NODE_STMT_FUNC && closure_stmt_in_vec(ctx->cand, node)) {
    return 0; /* 嵌套候选子树放行（各自扫描轮收口） */
  }
  if (closure_is_func_boundary(kind) || closure_is_decl_subtree(kind)) {
    return closure_disc_boundary(ctx, node) ? -2 : 0;
  }
  if (kind == CLUX_NODE_STMT_BLOCK) {
    vec_push(ctx->scopes, create_vec(ctx->out));
    int rc = 0;
    vec_t *children = node_get_children(node);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      rc = closure_disc_scan(ctx, (node_t *)vec_get(children, idx), type_pos);
      if (rc != 0) {
        break;
      }
    }
    vec_t *scope = (vec_t *)vec_get(ctx->scopes, vec_get_size(ctx->scopes) - 1);
    for (size_t idx = 0; idx < vec_get_size(scope); idx++) {
      char *name = (char *)vec_get(scope, idx);
      allocator_free(ctx->out, (void **)&name);
    }
    delete_vec(&scope);
    vec_pop(ctx->scopes);
    return rc;
  }
  if (kind == CLUX_NODE_STMT_VAR) {
    int rc = closure_disc_scan(ctx, stmt_var_node_get_type(node), 1);
    if (rc != 0) {
      return rc;
    }
    rc = closure_disc_scan(ctx, stmt_var_node_get_value(node), 0);
    if (rc != 0) {
      return rc;
    }
    closure_scope_declare(ctx->out, ctx->scopes, stmt_var_node_get_name(node));
    return 0;
  }
  if (kind == CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
    /* name 槽不检（Scan-A 名字槽检查覆盖捕获名碰撞）；值位置照扫。 */
    vec_t *children = node_get_children(node);
    node_t *value =
        vec_get_size(children) > (size_t)CLUX_EXPR_CONSTRUCT_FIELD_VALUE
            ? (node_t *)vec_get(children, CLUX_EXPR_CONSTRUCT_FIELD_VALUE)
            : NULL;
    return closure_disc_scan(ctx, value, type_pos);
  }
  if (closure_is_ident_leaf(node)) {
    char *text = location_get(node_get_location(node), ctx->out);
    if (!text) {
      return 0;
    }
    int rc = 0;
    if (type_pos) {
      if (closure_vis_find(ctx->vis, text) ||
          closure_chain_has(ctx->chain, text) ||
          closure_anc_has(ctx->anc, text)) {
        rc = -1;
      }
    } else {
      rc = closure_disc_read(ctx, text);
    }
    allocator_free(ctx->out, (void **)&text);
    return rc;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    int rc = closure_disc_scan(ctx, (node_t *)vec_get(children, idx), type_pos);
    if (rc != 0) {
      return rc;
    }
  }
  return 0;
}

static const char *closure_discover_reason(int rc) {
  switch (rc) {
  case -1:
    return "it uses an outer-scope name in a position that desugaring "
           "cannot rewrite, which is not supported yet";
  case -2:
    return "it references an outer-scope name from inside a nested function "
           "or defer body, which is not supported yet";
  case -3:
    return "it captures an outer-scope name through an intermediate closure, "
           "which is not supported yet";
  case -4:
    return "it references an enclosing closure by name, which is not "
           "supported yet";
  default:
    return "a capture has a type that cannot be stored in the environment "
           "struct";
  }
}

/* 条目级发现扫描：容器链 bail 名单（chain/anc）→ 源可见集 → 先导作用
   域（形参+显式捕获名）→ 签名槽类型位置检查（-1）→ BODY positional
   扫描（发现 / bail -1~-4）→ 锚点定型（-5）。 */
static int closure_discover(clux_sema_t *sema,
                            allocator_t *out,
                            program_t *program,
                            sema_closure_t *entry,
                            vec_t *cand) {
  node_t *decl = stmt_func_node_get_func_decl(entry->stmt);
  closure_disc_t ctx;
  ctx.sema = sema;
  ctx.out = out;
  ctx.program = program;
  ctx.cand = cand;
  ctx.scopes = create_vec(out);
  ctx.vis = NULL;
  ctx.caps = entry->captures;
  ctx.chain = create_vec(out);
  ctx.anc = create_vec(out);
  int rc = 0;
  /* 容器链：块级容器名进 chain（模块级条目名是模块级 var，产物直接
     可达，不收）；各祖先自身源可见集进 anc。 */
  for (sema_closure_t *cur = entry->container; cur; cur = cur->container) {
    if (cur->owner) {
      vec_push(ctx.chain, (void *)cur->name);
    }
    vec_t *anc_vis =
        closure_visible_build(sema, out, cur->container, cur->owner, cur->stmt);
    vec_push(ctx.anc, anc_vis);
  }
  ctx.vis = closure_visible_build(
      sema, out, entry->container, entry->owner, entry->stmt);
  /* 先导作用域：自身形参名 + 显式捕获名（体内读视为 local/$env 改写）。 */
  vec_push(ctx.scopes, create_vec(out));
  node_t *params = func_decl_node_get_params(decl);
  if (params) {
    vec_t *children = node_get_children(params);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      node_t *param = (node_t *)vec_get(children, idx);
      if (param && node_get_kind(param) == CLUX_NODE_FUNC_PARAM) {
        closure_scope_declare(out, ctx.scopes, func_param_node_get_name(param));
      }
    }
  }
  node_t *cap_params = closure_capture_params(decl);
  if (cap_params) {
    vec_t *children = node_get_children(cap_params);
    for (size_t idx = 0; idx < vec_get_size(children); idx++) {
      node_t *param = (node_t *)vec_get(children, idx);
      if (param && node_get_kind(param) == CLUX_NODE_FUNC_PARAM) {
        closure_scope_declare(out, ctx.scopes, func_param_node_get_name(param));
      }
    }
  }
  /* 签名槽：形参 TYPE/DEFAULT、捕获 TYPE（DEFAULT 在构造点求值，容器
     域内可达，不检）、RETURN——类型位置命中 → -1。 */
  if (params) {
    vec_t *children = node_get_children(params);
    for (size_t idx = 0; idx < vec_get_size(children) && rc == 0; idx++) {
      node_t *param = (node_t *)vec_get(children, idx);
      if (!param || node_get_kind(param) != CLUX_NODE_FUNC_PARAM) {
        continue;
      }
      rc = closure_disc_scan(&ctx, func_param_node_get_type(param), 1);
      if (rc == 0) {
        rc = closure_disc_scan(&ctx, func_param_node_get_default(param), 1);
      }
    }
  }
  if (rc == 0 && cap_params) {
    vec_t *children = node_get_children(cap_params);
    for (size_t idx = 0; idx < vec_get_size(children) && rc == 0; idx++) {
      node_t *param = (node_t *)vec_get(children, idx);
      if (param && node_get_kind(param) == CLUX_NODE_FUNC_PARAM) {
        rc = closure_disc_scan(&ctx, func_param_node_get_type(param), 1);
      }
    }
  }
  if (rc == 0) {
    rc = closure_disc_scan(&ctx, func_decl_node_get_return(decl), 1);
  }
  /* BODY：positional 发现扫描。 */
  if (rc == 0) {
    rc = closure_disc_scan(&ctx, func_decl_node_get_body(decl), 0);
  }
  /* 锚点定型（vis 生命周期内）。 */
  if (rc == 0 &&
      !closure_anchor_caps(sema, out, ctx.vis, entry->captures, entry->block)) {
    rc = -5;
  }
  /* 统一释放（anc 各 vis 借用 anc 名单，本体随 vec 释放）。 */
  for (size_t idx = 0; idx < vec_get_size(ctx.anc); idx++) {
    vec_t *anc_vis = (vec_t *)vec_get(ctx.anc, idx);
    closure_visible_free(out, &anc_vis);
  }
  delete_vec(&ctx.anc);
  delete_vec(&ctx.chain);
  closure_visible_free(out, &ctx.vis);
  for (size_t idx = 0; idx < vec_get_size(ctx.scopes); idx++) {
    vec_t *scope = (vec_t *)vec_get(ctx.scopes, idx);
    for (size_t sidx = 0; sidx < vec_get_size(scope); sidx++) {
      char *name = (char *)vec_get(scope, sidx);
      allocator_free(out, (void **)&name);
    }
    delete_vec(&scope);
  }
  delete_vec(&ctx.scopes);
  return rc;
}
/* 拼接退化合成名（prefix + mangled）。mangled NULL（模块级条目——收集后
   由调用方以原名覆写）返回 NULL。 */
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

/* §2.12 P2 收紧：块级条目退化失败不再静默跌回旧提升通道（语义漂移——
   提升为静态函数丢失闭包值语义），改报显式编译错误阻断 emit。 */
static void closure_diag_bail(clux_sema_t *sema,
                              node_t *stmt,
                              const char *name,
                              const char *reason) {
  clux_context_diag(sema->ctx,
                    CLUX_DIAG_ERROR,
                    node_get_location(stmt),
                    "closure '%s' cannot be desugared: %s",
                    name,
                    reason);
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
    /* 模块级名字 M = 原名$id（重取 name 文本——entry_make 内部已释放）。
       id 统一自增（§2.11 P3）：函数名只在定义处作用域唯一，提升到全局
       须消歧（多模块同名 / 兄弟块同名遮蔽同源问题）。*/
    char *mangled =
        location_get(node_get_location(func_decl_node_get_name(decl)), out);
    if (!mangled) {
      allocator_free(out, (void **)&entry);
      continue;
    }
    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "$%zu", ++sema->synth_seq);
    string_t *mangled_joined = create_string(out);
    string_append_cstr(mangled_joined, mangled);
    string_append_cstr(mangled_joined, id_buf);
    allocator_free(out, (void **)&mangled);
    mangled = allocator_alloc(out, string_get_size(mangled_joined) + 1);
    memcpy(mangled,
           string_get_data(mangled_joined),
           string_get_size(mangled_joined) + 1);
    delete_string(&mangled_joined);
    entry->closure_name =
        closure_synth_name(out, program, "$closure$", mangled);
    entry->callee_name = closure_synth_name(out, program, "$callee$", mangled);
    allocator_free(out, (void **)&mangled);
    int ok = 0;
    entry->container = NULL;
    entry->captures = closure_extract_caps(out, program, decl, &ok);
    if (!ok ||
        closure_scan_entry(out, decl, entry->name, entry->captures, NULL)) {
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    vec_push(closures, entry);
  }
  /* 块级条目（§2.12 P2 起含零捕获原位构造；P3 起嵌套候选照常入表——
     AST 包含判定替代旧名字前缀检测）+ 跨层捕获 + 敏感名字域全套检查；
     bail 一律显式报错，不再跌回旧提升通道。 */
  vec_t *cand = closure_candidate_stmts(sema, out);
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
    int ok = 0;
    vec_t *caps = closure_extract_caps(out, program, decl, &ok);
    if (!ok) {
      closure_free_caps(out, &caps);
      continue;
    }
    /* 定义块定位：嵌套候选从直接容器体内找（其 var/构造随容器体克隆
       落位），否则 owner 体；两者皆不可达（非候选嵌套函数/defer 体内
       定义）→ bail。 */
    sema_closure_t *container = closure_find_container(closures, hoist->stmt);
    node_t *scope_body = NULL;
    if (container) {
      node_t *ctn_decl = stmt_func_node_get_func_decl(container->stmt);
      scope_body = ctn_decl ? func_decl_node_get_body(ctn_decl) : NULL;
    } else {
      node_t *owner_decl = stmt_func_node_get_func_decl(hoist->owner);
      scope_body = owner_decl ? func_decl_node_get_body(owner_decl) : NULL;
    }
    node_t *block = closure_find_defining_block(scope_body, hoist->stmt);
    if (!block) {
      closure_diag_bail(sema,
                        hoist->stmt,
                        hoist->name,
                        "it is defined inside a nested function or defer "
                        "body, which is not supported yet");
      closure_free_caps(out, &caps);
      continue;
    }
    sema_closure_t *entry = closure_entry_make(
        out, program, hoist->stmt, hoist->owner, block, hoist->mangled);
    if (!entry) {
      closure_free_caps(out, &caps);
      continue;
    }
    entry->container = container;
    entry->captures = caps;
    /* 跨层捕获：源可见集 + 隐式发现 + 锚点定型（§2.12 P3 批次 1）。 */
    int rc = closure_discover(sema, out, program, entry, cand);
    if (rc != 0) {
      closure_diag_bail(
          sema, hoist->stmt, hoist->name, closure_discover_reason(rc));
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    if (closure_scan_entry(out, decl, entry->name, entry->captures, cand) !=
        0) {
      closure_diag_bail(sema,
                        hoist->stmt,
                        hoist->name,
                        "its body uses its own name or a capture name in a "
                        "position that desugaring cannot rewrite");
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    node_t *owner_decl = stmt_func_node_get_func_decl(hoist->owner);
    node_t *owner_body =
        owner_decl ? func_decl_node_get_body(owner_decl) : NULL;
    if (closure_scan_owner(
            out, owner_body, entry->name, entry->stmt, entry->block, 0, cand) !=
        0) {
      closure_diag_bail(sema,
                        hoist->stmt,
                        hoist->name,
                        "it is referenced outside its defining block");
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    if (closure_scan_sibling_hoists(
            out, sema, hoist->owner, entry->stmt, entry->name, cand) != 0) {
      closure_diag_bail(sema,
                        hoist->stmt,
                        hoist->name,
                        "it is referenced from within a sibling hoisted "
                        "declaration");
      closure_free_caps(out, &entry->captures);
      allocator_free(out, (void **)&entry);
      continue;
    }
    vec_push(closures, entry);
  }
  /* cand 只借用 AST 指针作成员判定，向量自身在此收口释放。 */
  delete_vec(&cand);
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

/* 构造表达式 .$closure$M{.$callee=$callee$M, .<cap>=...}：捕获字段值取
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
                                closure_ident(env, "$callee", loc));
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
    node_t *value = NULL;
    if (cap->default_src) {
      /* 标注捕获默认值表达式克隆（构造点求值）。 */
      value = sema_clone_node_renamed(cap->default_src, env);
    } else if (cap->via_env) {
      /* 容器捕获来源：构造点位于容器 callee 体内，经 $env.<name> 取值
         （§2.12 P3 跨层捕获）。 */
      value = closure_binary(env,
                             closure_ident(env, "$env", loc),
                             ".",
                             closure_ident(env, cap->name, loc),
                             loc);
    } else {
      /* 源形参 / 块 var 来源：构造点域内直接可达，快照现值。 */
      value = closure_ident(env, cap->name, loc);
    }
    construct_field_node_set_value(field, value);
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

node_t *sema_closure_define_var(sema_closure_t *entry, sema_clone_env_t *env) {
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

/* func(*$closure$M, 参数类型...) -> ret：$callee 字段类型。形参用裸类型
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
  field_node_set_name(callee_field, closure_ident(env, "$callee", loc));
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
  func_param_node_set_name(env_param, closure_ident(env, "$env", loc));
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
  /* 自递归优先：$env.$callee($env, args)——$env 已是 *$closure$M 指针，
     直传不取址。 */
  if (env->closure_self && strcmp(text, env->closure_self) == 0) {
    allocator_free(env->out, (void **)&text);
    call_node_set_callee(call,
                         closure_binary(env,
                                        closure_ident(env, "$env", loc),
                                        ".",
                                        closure_ident(env, "$callee", loc),
                                        loc));
    call_node_add_arg(call, closure_ident(env, "$env", loc));
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
                                      closure_ident(env, "$callee", loc),
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
                        closure_ident(env, "$env", loc),
                        ".",
                        closure_ident(env, hit->name, loc),
                        loc);
}
