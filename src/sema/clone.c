/* 克隆器：逆向编排（设计文档 §4 ②）两条新建通道之一——未变换子树的
   deep clone。产物分配在 out_allocator，与原树无任何共享节点；
   loc 按值浅拷（position 的 offset 仍可借用 parser 源码缓冲，
   与原树同源，program_t 不因此持有 engine 指针）。
   Phase 4 变换（§2.4–2.7）在克隆通道内做结构改写：
   - STMT_DEFER → 立即捕获闭包 var __deferN + 退出点 LIFO 调用（§2.4）；
   - STMT_COMPTIME_IF → walk 期 CTFE 决策的选中臂（§2.5，prunes 表）；
   - STMT_COMPTIME_FOREACH → EXPR_TUPLE 字面序列逐元素展开（§2.6）；
   - STMT_FOREACH → 索引 for 脱糖（§2.7）。 */
#include "sema/sema_internal.h"
#include "core/location.h"
#include "core/string.h"
#include "core/vec.h"
#include "engine/ptr.h"
#include "engine/qualifier.h"
#include "engine/type.h"
#include "parser/node_assign.h"
#include "parser/node_builtin.h"
#include "parser/node_binary.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_func_type.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_var.h"
#include "parser/node_subscript.h"
#include <stdio.h>
#include <string.h>

/* 实例名折叠（§2.3 最小版）：使用点（EXPR_SUBSCRIPT）替换为单个
   名字节点，location 区间指向 intern 的实例名（line/col/filename
   沿 base 的值）。命中即整棵子树不再递归。 */
static node_t *sema_clone_instant(node_t *node, sema_clone_env_t *env) {
  if (!env->instants || node_get_kind(node) != CLUX_NODE_EXPR_SUBSCRIPT) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(env->instants); idx++) {
    sema_instant_t *instant = (sema_instant_t *)vec_get(env->instants, idx);
    if (instant->node != node) {
      continue;
    }
    size_t len = strlen(instant->name);
    const char *text = program_intern_string(env->program, instant->name, len);
    node_t *clone = create_node(env->out, CLUX_NODE_LITERAL_IDENTIFIER);
    location_t renamed = instant->base_loc;
    renamed.begin.offset = text;
    renamed.end.offset = text + len;
    node_set_location(clone, &renamed);
    return clone;
  }
  return NULL;
}

static node_t *sema_clone_impl(node_t *node, sema_clone_env_t *env);

/* ---- §2.3 实例体克隆态 ----
   instantiate 置位期间（emit 尾部克隆实例体）：
   - 泛型形参引用（LITERAL_IDENTIFIER 命中形参名表）→ 实参子树克隆；
   - 定义体内自引用使用点（EXPR_SUBSCRIPT base 命中 renames[0].from）
     → 整体折叠为代换后的实例名（形参文本先换成实参文本再拼名）；
   - GENERIC_PARAMS → NULL（实例体非泛型，GENERICS 槽置空）。 */

/* 名字形态叶子（identifier 无孩子），inst 实参折叠的前置条件。 */
static int sema_clone_is_identifier_leaf(node_t *node) {
  return node && node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER &&
         vec_get_size(node_get_children(node)) == 0;
}

/* 形参代换：文本命中 inst_params → 对应实参克隆（实参收口为名字形态
   叶子，walk 已验证）。未命中返回 NULL。 */
static node_t *sema_clone_param_subst(node_t *node, sema_clone_env_t *env) {
  if (!env->inst_params || !env->inst_args) {
    return NULL;
  }
  char *text = location_get(node_get_location(node), env->out);
  if (!text) {
    return NULL;
  }
  node_t *replacement = NULL;
  size_t count = vec_get_size(env->inst_params);
  for (size_t idx = 0; idx < count; idx++) {
    const char *param = (const char *)vec_get(env->inst_params, idx);
    if (!param || strcmp(param, text) != 0) {
      continue;
    }
    node_t *arg = (node_t *)vec_get(env->inst_args, idx);
    if (arg) {
      replacement = sema_clone_impl(arg, env);
    }
    break;
  }
  allocator_free(env->out, (void **)&text);
  return replacement;
}

/* 自引用折叠：SUBSCRIPT base 命中 renames[0].from（被实例化的泛型名）
   → base$代换实参1$...（实参文本先经形参代换），intern 为实例名
   identifier。任一实参非名字形态叶子不折叠（交由使用点原样克隆）。 */
static node_t *sema_clone_self_instant(node_t *node, sema_clone_env_t *env) {
  if (!env->renames || env->rename_count == 0) {
    return NULL;
  }
  vec_t *children = node_get_children(node);
  node_t *base =
      vec_get_size(children) > 0 ? (node_t *)vec_get(children, 0) : NULL;
  if (!sema_clone_is_identifier_leaf(base) ||
      !location_is(node_get_location(base), env->renames[0].from)) {
    return NULL;
  }
  string_t *joined = create_string(env->out);
  string_append_cstr(joined, env->renames[0].from);
  for (size_t idx = 1; idx < vec_get_size(children); idx++) {
    node_t *arg = (node_t *)vec_get(children, idx);
    if (!sema_clone_is_identifier_leaf(arg)) {
      delete_string(&joined);
      return NULL;
    }
    char *text = location_get(node_get_location(arg), env->out);
    if (!text) {
      delete_string(&joined);
      return NULL;
    }
    /* 实参是泛型形参 → 换成实例化实参的文本。 */
    for (size_t p = 0; env->inst_params && p < vec_get_size(env->inst_params);
         p++) {
      const char *param = (const char *)vec_get(env->inst_params, p);
      if (param && strcmp(param, text) == 0) {
        node_t *real_arg = (node_t *)vec_get(env->inst_args, p);
        if (sema_clone_is_identifier_leaf(real_arg)) {
          allocator_free(env->out, (void **)&text);
          text = location_get(node_get_location(real_arg), env->out);
        }
        break;
      }
    }
    if (!text) {
      delete_string(&joined);
      return NULL;
    }
    string_append_cstr(joined, "$");
    string_append_cstr(joined, text);
    allocator_free(env->out, (void **)&text);
  }
  size_t len = string_get_size(joined);
  const char *text =
      program_intern_string(env->program, string_get_data(joined), len);
  delete_string(&joined);
  node_t *clone = create_node(env->out, CLUX_NODE_LITERAL_IDENTIFIER);
  location_t renamed = *node_get_location(base);
  renamed.begin.offset = text;
  renamed.end.offset = text + len;
  node_set_location(clone, &renamed);
  return clone;
}

/* 常量折叠（§2.1 最小版）：引用点（LITERAL_IDENTIFIER）命中常量表
   → 渲染为字面量节点（intern 文本，位置字段沿引用点）。命中即止。 */
static node_t *sema_clone_const_fold(node_t *node, sema_clone_env_t *env) {
  if (!env->consts || node_get_kind(node) != CLUX_NODE_LITERAL_IDENTIFIER) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(env->consts); idx++) {
    sema_const_use_t *use = (sema_const_use_t *)vec_get(env->consts, idx);
    if (use->node != node) {
      continue;
    }
    return sema_render_const(
        env->out, env->program, &use->def->value, node_get_location(node));
  }
  return NULL;
}

/* 类型名可渲染判定（§8.5→§2.8 回填）：标量/具名类型（struct/enum/
   别名）与名字即合法类型表达的复合类型（数组等）按 name 渲染为
   identifier；func 类型签名无名字形态可渲染（闭包捕获表无法闭合
   书写），返回 NULL 由合成侧自行构造类型表达式。 */
static const char *sema_type_render_name(clux_type_t *type) {
  if (!type || clux_type_get_kind(type) == CLUX_TYPE_FUNC) {
    return NULL;
  }
  const char *name = clux_type_get_name(type);
  return name && name[0] ? name : NULL;
}

/* 推断显式化（§2.8）：TYPE 槽缺失时回填——第一层按 VALUE 字面量
   （带后缀整数以后缀定类型；无后缀整数默认 i32 且文本补 i32 后缀
   （浮点后缀由语法层强制）；布尔为 bool；构造值（具名构造，或已
   经 §2.9 回填合成名的匿名构造）回填构造类型名）；字面量层不覆盖
   的形态（调用、标识符引用等）按原树 STMT_VAR 锚点查 sema 类型
   结果表（§8.5，P10 补全回填）渲染。src 是原树 STMT_VAR（合成
   节点传 NULL，不查表）。补后缀只改 VALUE 节点的 location（指向
   intern 文本），不替换节点，避免孤儿分配。 */
static void
sema_clone_var_fixup(node_t *clone, node_t *src, sema_clone_env_t *env) {
  vec_t *children = node_get_children(clone);
  if (vec_get_size(children) < (size_t)CLUX_STMT_VAR_FIRST_DECORATOR) {
    return;
  }
  node_t *type_child = (node_t *)vec_get(children, CLUX_STMT_VAR_TYPE);
  node_t *value_child = (node_t *)vec_get(children, CLUX_STMT_VAR_VALUE);
  if (type_child || !value_child) {
    return;
  }
  allocator_t *out = env->out;
  program_t *program = env->program;
  const location_t *loc = node_get_location(value_child);
  char *owned_name = NULL;
  const char *type_name = NULL;
  if (node_get_kind(value_child) == CLUX_NODE_LITERAL_BOOLEAN) {
    type_name = "bool";
  } else if (node_get_kind(value_child) == CLUX_NODE_LITERAL_NUMERIC) {
    char *text = location_get(loc, out);
    if (!text) {
      return;
    }
    size_t num_len = 0;
    const char *suffix = sema_render_strip_suffix(text, strlen(text), &num_len);
    if (suffix) {
      type_name = suffix;
    } else {
      /* 无后缀整数默认 i32：文本补后缀（intern，原地改 location）。 */
      char suffixed[64];
      if (snprintf(suffixed, sizeof(suffixed), "%si32", text) >=
          (int)sizeof(suffixed)) {
        allocator_free(out, (void **)&text);
        return;
      }
      size_t len = strlen(suffixed);
      const char *stored = program_intern_string(program, suffixed, len);
      location_t relocated = *loc;
      relocated.begin.offset = stored;
      relocated.end.offset = stored + len;
      node_set_location(value_child, &relocated);
      type_name = "i32";
    }
    allocator_free(out, (void **)&text);
  } else if (node_get_kind(value_child) == CLUX_NODE_EXPR_CONSTRUCT) {
    /* §2.9：构造值的类型即构造点名（匿名构造的合成名已先经 anons
       通道回填进 TYPE 槽——构造体克隆先于本 var 的 children 循环
       完成）。构造点名 location 指向 intern 文本，读取后渲染副本。 */
    node_t *value_type = construct_node_get_type(value_child);
    if (value_type) {
      owned_name = location_get(node_get_location(value_type), out);
      if (owned_name) {
        type_name = owned_name;
      }
    }
  }
  /* 第一层（字面量/构造）不覆盖：查类型结果表（§8.5→§2.8 P10）。
     标注优先语义由 type_child 前置守卫保证；表条目缺失（不可演算/
     func 类型等）保持 NULL 不强填。 */
  if (!type_name && src && env->sema) {
    type_name = sema_type_render_name(sema_get_node_type(env->sema, src));
  }
  if (!type_name) {
    return;
  }
  /* 类型名与 identifier 合一（白皮书 §2.1）→ LITERAL_IDENTIFIER。 */
  node_t *type_node = sema_render_text_node(
      out, program, CLUX_NODE_LITERAL_IDENTIFIER, type_name, loc);
  node_set_child(clone, CLUX_STMT_VAR_TYPE, type_node);
  allocator_free(out, (void **)&owned_name);
}

/* ---- §2.4 defer 脱糖 ---- */

/* 已登记 defer / dispose 的回执（name 为 intern 文本，loc 借用源语句）。
   §4.17：dispose 形态与 defer 共用同一条 LIFO 栈——登记序即执行逆序，
   块内 defer 与析构交错（先声明的 var 先析构在 defer 之后执行）。 */
typedef struct {
  const char *name; /* __deferN 或 $dispose$T（intern 文本） */
  const location_t *loc;
  int dispose;          /* 1=析构调用形态：追加实参 var_name.& */
  const char *var_name; /* dispose 目标变量名（intern 文本） */
} sema_defer_ref_t;

static int sema_stmt_is_exit_point(node_t *stmt) {
  node_kind_t kind = node_get_kind(stmt);
  return kind == CLUX_NODE_STMT_RETURN || kind == CLUX_NODE_STMT_BREAK ||
         kind == CLUX_NODE_STMT_CONTINUE;
}

static int
sema_prune_lookup(node_t *node, sema_clone_env_t *env, sema_prune_t **out);

/* 语句位 comptime if 解析：沿 else-if 链（ELSE 槽为嵌套 COMPTIME_IF）
   追到最终选中臂；假臂无 else 的链节点返回 NULL=整条摘除。未命中
   决策表（walk 被阻断的路径，正常 emit 不可达）原样返回。 */
static node_t *sema_resolve_prune_stmt(node_t *stmt, sema_clone_env_t *env) {
  node_t *cur = stmt;
  while (cur && node_get_kind(cur) == CLUX_NODE_STMT_COMPTIME_IF) {
    sema_prune_t *prune = NULL;
    if (sema_prune_lookup(cur, env, &prune) != 0) {
      return cur;
    }
    if (!prune->arm) {
      return NULL;
    }
    cur = prune->arm;
  }
  return cur;
}

static node_t *sema_clone_impl(node_t *node, sema_clone_env_t *env);

/* 退出点调用 __deferN(); / $dispose$T(x.); —— STMT_EXPR(EXPR_CALL)。
   dispose 形态实参为 x.&（左值零拷贝取址，析构函数按地址写目标）。 */
static node_t *sema_defer_call(const sema_defer_ref_t *ref,
                               sema_clone_env_t *env) {
  node_t *call = create_call_node(env->out);
  node_set_location(call, ref->loc);
  node_t *callee = sema_render_text_node(env->out,
                                         env->program,
                                         CLUX_NODE_LITERAL_IDENTIFIER,
                                         ref->name,
                                         ref->loc);
  call_node_set_callee(call, callee);
  if (ref->dispose) {
    node_t *var = sema_render_text_node(env->out,
                                        env->program,
                                        CLUX_NODE_LITERAL_IDENTIFIER,
                                        ref->var_name,
                                        ref->loc);
    node_t *addr = create_binary_node(env->out);
    node_set_location(addr, ref->loc);
    binary_node_set_left(addr, var);
    binary_node_set_symbol(
        addr,
        sema_render_text_node(
            env->out, env->program, CLUX_NODE_SYMBOL, ".&", ref->loc));
    call_node_add_arg(call, addr);
  }
  node_t *wrap = create_stmt_expr_node(env->out);
  node_set_location(wrap, ref->loc);
  stmt_expr_node_set_expr(wrap, call);
  return wrap;
}

/* defer → var __deferN = func |captures| (空形参): void { body };
   捕获一律 copy（值语义），捕获值在定义点（原 defer 位置）克隆求值。 */
static node_t *sema_defer_def_synth(node_t *src,
                                    sema_clone_env_t *env,
                                    sema_defer_ref_t *ref) {
  const location_t *loc = node_get_location(src);
  char buf[32];
  int written = snprintf(buf, sizeof(buf), "__defer%zu", env->defer_counter++);
  if (written <= 0 || (size_t)written >= sizeof(buf)) {
    return NULL;
  }
  ref->name = program_intern_string(env->program, buf, (size_t)written);
  ref->loc = loc;
  /* 闭包：func |captures| (): void { body }（捕获项带 DEFAULT，
     经通用克隆通道——内部引用点照常折叠/改名）。构造器均预置槽位，
     须经槽位 setter 填充，不可 node_add_child 追加。 */
  node_t *fn = create_func_decl_node(env->out);
  node_set_location(fn, loc);
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_KEYWORD,
                 sema_render_text_node(
                     env->out, env->program, CLUX_NODE_SYMBOL, "func", loc));
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_NAME, NULL); /* 匿名闭包 */
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_GENERICS, NULL);
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_CAPTURES,
                 sema_clone_impl(stmt_defer_node_get_captures(src), env));
  node_set_child(
      fn, CLUX_EXPR_FUNC_DECL_PARAMS, create_func_params_node(env->out));
  node_set_child(
      fn,
      CLUX_EXPR_FUNC_DECL_RETURN,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, "void", loc));
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_BODY,
                 sema_clone_impl(stmt_defer_node_get_body(src), env));
  /* var __deferN = <闭包>；槽序 KEYWORD/NAME/TYPE/VALUE/EXPORT/COMPTIME。 */
  node_t *var = create_stmt_var_node(env->out);
  node_set_location(var, loc);
  node_set_child(var,
                 CLUX_STMT_VAR_KEYWORD,
                 sema_render_text_node(
                     env->out, env->program, CLUX_NODE_SYMBOL, "var", loc));
  node_set_child(var,
                 CLUX_STMT_VAR_NAME,
                 sema_render_text_node(env->out,
                                       env->program,
                                       CLUX_NODE_LITERAL_IDENTIFIER,
                                       ref->name,
                                       loc));
  /* §8.5 类型表回填：闭包 var 的类型用结构化 func type 表达式
     （捕获表 + 空形参 + void 返回）闭合书写。 */
  node_t *type = create_func_type_node(env->out);
  node_set_location(type, loc);
  func_type_node_set_captures(
      type, sema_clone_impl(stmt_defer_node_get_captures(src), env));
  func_type_node_set_params(type, create_func_params_node(env->out));
  func_type_node_set_return(
      type,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, "void", loc));
  node_set_child(var, CLUX_STMT_VAR_TYPE, type);
  node_set_child(var, CLUX_STMT_VAR_VALUE, fn);
  node_set_child(var, CLUX_STMT_VAR_EXPORT, NULL);
  node_set_child(var, CLUX_STMT_VAR_COMPTIME, NULL);
  return var;
}

/* 块级改写：defer 定义点换形态、LIFO 登记退出调用（块尾 + 语句流中
   直接子级的 return/break/continue 前——退出点链当前只覆盖本块直接
   子语句，深层嵌套退出点（如 if 臂内 return 触发外层块 defer）随后续
   分期落地）；comptime if 按剪枝决策替换/摘除。 */
static void sema_expand_comptime_foreach_into(node_t *src,
                                              node_t *into,
                                              sema_clone_env_t *env);
static node_t *sema_clone_block(node_t *node, sema_clone_env_t *env) {
  node_t *clone = create_stmt_block_node(env->out);
  node_set_location(clone, node_get_location(node));
  vec_t *children = node_get_children(node);
  size_t count = vec_get_size(children);
  vec_t *deferred = create_vec(env->out);
  int ends_in_exit = 0;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *child = (node_t *)vec_get(children, idx);
    if (!child) {
      node_add_child(clone, NULL);
      continue;
    }
    /* 提升（§2.2）：块语句位命中 skip 集 → 整条摘除（原逻辑在通用
       克隆路径，块通道接管后须自持）。 */
    if (env->skip) {
      int dropped = 0;
      for (size_t s = 0; s < vec_get_size(env->skip); s++) {
        if (child == (node_t *)vec_get(env->skip, s)) {
          dropped = 1;
          break;
        }
      }
      if (dropped) {
        continue;
      }
    }
    /* §2.5 语句位 comptime if：解析 else-if 链后替换为选中臂克隆，
       假臂无 else 整条摘除（不占语句位）。 */
    if (node_get_kind(child) == CLUX_NODE_STMT_COMPTIME_IF) {
      node_t *arm = sema_resolve_prune_stmt(child, env);
      if (arm) {
        node_add_child(clone, sema_clone_impl(arm, env));
      }
      continue;
    }
    /* §2.6 语句位 comptime foreach：逐元素展开直接拼接（不包块，
       保持语句位一一对应）。 */
    if (node_get_kind(child) == CLUX_NODE_STMT_COMPTIME_FOREACH) {
      sema_expand_comptime_foreach_into(child, clone, env);
      continue;
    }
    /* 直接子级退出点：已登记 defer 逆序先执行（LIFO）。 */
    ends_in_exit = sema_stmt_is_exit_point(child);
    if (ends_in_exit) {
      for (size_t d = vec_get_size(deferred); d > 0; d--) {
        node_add_child(
            clone,
            sema_defer_call((sema_defer_ref_t *)vec_get(deferred, d - 1), env));
      }
      node_add_child(clone, sema_clone_impl(child, env));
      continue;
    }
    if (node_get_kind(child) == CLUX_NODE_STMT_DEFER) {
      sema_defer_ref_t *ref =
          allocator_alloc(env->out, sizeof(sema_defer_ref_t));
      ref->dispose = 0;
      ref->var_name = NULL;
      node_t *synth = sema_defer_def_synth(child, env, ref);
      if (!synth) {
        allocator_free(env->out, (void **)&ref);
        continue;
      }
      node_add_child(clone, synth);
      vec_push(deferred, ref);
      continue;
    }
    /* §4.17 disposable 局部 var：登记作用域出口析构（与 defer 同栈）。 */
    if (env->disposes && env->sema &&
        node_get_kind(child) == CLUX_NODE_STMT_VAR) {
      clux_type_t *var_type = sema_get_node_type(env->sema, child);
      if (sema_type_needs_dispose(env->sema, var_type)) {
        node_t *name_node = stmt_var_node_get_name(child);
        char *var_name =
            name_node ? location_get(node_get_location(name_node), env->out)
                      : NULL;
        const char *type_name = clux_type_get_name(var_type);
        const char *callee =
            type_name
                ? sema_dispose_callee_name(env->out, env->program, type_name)
                : NULL;
        if (var_name && callee) {
          sema_defer_ref_t *ref =
              allocator_alloc(env->out, sizeof(sema_defer_ref_t));
          ref->dispose = 1;
          ref->name = callee;
          ref->var_name =
              program_intern_string(env->program, var_name, strlen(var_name));
          ref->loc = node_get_location(name_node);
          vec_push(deferred, ref);
          sema_dispose_register(env, var_type, ref->loc);
        }
        allocator_free(env->out, (void **)&var_name);
      }
    }
    node_add_child(clone, sema_clone_impl(child, env));
  }
  /* 块尾正常退出：全部已登记 defer 逆序执行；末语句已是退出点时
     块尾不可达，不再补调用（defer 调用已在退出点前插入）。 */
  if (!ends_in_exit) {
    for (size_t d = vec_get_size(deferred); d > 0; d--) {
      node_add_child(
          clone,
          sema_defer_call((sema_defer_ref_t *)vec_get(deferred, d - 1), env));
    }
  }
  /* ref 结构体归块通道所有（name 为 program 内串、loc 指向源树，均
     借用不拥有），vec 是 non-own，须手动逐个释放。 */
  for (size_t d = 0; d < vec_get_size(deferred); d++) {
    sema_defer_ref_t *ref = (sema_defer_ref_t *)vec_get(deferred, d);
    allocator_free(env->out, (void **)&ref);
  }
  delete_vec(&deferred);
  return clone;
}

/* ---- §2.7 foreach 脱糖 ----
   foreach (var name [: type]? of iterable) { body }
   → { var __iter: T? = iterable;
       var __i = 0;
       for (; __i < #len(__iter); __i = __i + 1) {
           var name [: type]? = __iter[__i];
           <body 块>;
       } }
   foreach (name of iterable) { body }（裸名，前提 name 已在外层定义）
   → 同上，但循环体首句改为赋值：name = __iter[__i];
   #len 与下标是现有语言能力，零新构造；iterable 提升为隐藏 var
   __iter（类型从 check 侧 iterable 锚点显式渲染）只求值一次——
   #len 与下标两处若各自求值，call 形态 iterable 会被求值 2N 次，
   与 exec 直连路径（求值一次）语义漂移。var/裸名是两种语义：
   var 每轮重新定义迭代变量；裸名每轮给既有绑定赋值。 */

static node_t *sema_foreach_ident(const char *name,
                                  const location_t *loc,
                                  sema_clone_env_t *env) {
  return sema_render_text_node(
      env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, name, loc);
}

static node_t *sema_foreach_binary(const char *op,
                                   node_t *left,
                                   node_t *right,
                                   const location_t *loc,
                                   sema_clone_env_t *env) {
  node_t *binary = create_node(env->out, CLUX_NODE_EXPR_BINARY);
  node_set_location(binary, loc);
  node_add_child(binary, left);
  node_add_child(
      binary,
      sema_render_text_node(env->out, env->program, CLUX_NODE_SYMBOL, op, loc));
  node_add_child(binary, right);
  return binary;
}

static node_t *sema_desugar_foreach(node_t *src, sema_clone_env_t *env) {
  const location_t *loc = node_get_location(src);
  node_t *iter_src = stmt_foreach_node_get_iterable(src);
  /* var __iter: T? = <iterable>;（求值一次；TYPE 从 iterable 锚点渲染，
     合成 var 无原树锚点，§2.8 字面量回填对符号引用值推不出来）。 */
  node_t *iter_var = create_stmt_var_node(env->out);
  node_set_location(iter_var, loc);
  node_set_child(iter_var,
                 CLUX_STMT_VAR_KEYWORD,
                 sema_render_text_node(
                     env->out, env->program, CLUX_NODE_SYMBOL, "var", loc));
  node_set_child(
      iter_var, CLUX_STMT_VAR_NAME, sema_foreach_ident("__iter", loc, env));
  node_t *iter_type = NULL;
  if (env->sema) {
    const char *iter_type_name =
        sema_type_render_name(sema_get_node_type(env->sema, iter_src));
    if (iter_type_name) {
      iter_type = sema_render_text_node(env->out,
                                        env->program,
                                        CLUX_NODE_LITERAL_IDENTIFIER,
                                        iter_type_name,
                                        loc);
    }
  }
  node_set_child(iter_var, CLUX_STMT_VAR_TYPE, iter_type);
  node_set_child(iter_var, CLUX_STMT_VAR_VALUE, sema_clone_impl(iter_src, env));
  node_set_child(iter_var, CLUX_STMT_VAR_EXPORT, NULL);
  node_set_child(iter_var, CLUX_STMT_VAR_COMPTIME, NULL);
  node_t *len_iter = sema_foreach_ident("__iter", loc, env);
  node_t *idx_iter = sema_foreach_ident("__iter", loc, env);
  /* var __i = 0;（§2.8 回填 i32） */
  node_t *idx_var = create_stmt_var_node(env->out);
  node_set_location(idx_var, loc);
  node_set_child(idx_var,
                 CLUX_STMT_VAR_KEYWORD,
                 sema_render_text_node(
                     env->out, env->program, CLUX_NODE_SYMBOL, "var", loc));
  node_set_child(
      idx_var, CLUX_STMT_VAR_NAME, sema_foreach_ident("__i", loc, env));
  node_set_child(idx_var, CLUX_STMT_VAR_TYPE, NULL);
  node_set_child(
      idx_var,
      CLUX_STMT_VAR_VALUE,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_LITERAL_NUMERIC, "0", loc));
  node_set_child(idx_var, CLUX_STMT_VAR_EXPORT, NULL);
  node_set_child(idx_var, CLUX_STMT_VAR_COMPTIME, NULL);
  sema_clone_var_fixup(idx_var, NULL, env);
  /* for (; __i < #len(iterable); __i = __i + 1) */
  node_t *len = create_builtin_node(env->out);
  node_set_location(len, loc);
  builtin_node_set_name(
      len,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_SYMBOL, "len", loc));
  builtin_node_add_arg(len, len_iter);
  node_t *cond = sema_foreach_binary(
      "<", sema_foreach_ident("__i", loc, env), len, loc, env);
  node_t *step = create_assign_node(env->out);
  node_set_location(step, loc);
  assign_node_set_left(step, sema_foreach_ident("__i", loc, env));
  assign_node_set_symbol(
      step,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_SYMBOL, "=", loc));
  assign_node_set_value(
      step,
      sema_foreach_binary(
          "+",
          sema_foreach_ident("__i", loc, env),
          sema_render_text_node(
              env->out, env->program, CLUX_NODE_LITERAL_NUMERIC, "1", loc),
          loc,
          env));
  /* 循环体首句：var 形态为 var name[:T]? = __iter[__i];
     裸名形态为赋值 name = __iter[__i]（每轮给既有绑定赋值）。 */
  node_t *body_block = create_stmt_block_node(env->out);
  node_set_location(body_block, loc);
  node_t *index = create_subscript_node(env->out);
  node_set_location(index, loc);
  subscript_node_set_base(index, idx_iter);
  subscript_node_add_arg(index, sema_foreach_ident("__i", loc, env));
  node_t *name_src = stmt_foreach_node_get_name(src);
  if (stmt_foreach_node_get_keyword(src)) {
    node_t *item_var = create_stmt_var_node(env->out);
    node_set_location(item_var, node_get_location(name_src));
    node_set_child(item_var,
                   CLUX_STMT_VAR_KEYWORD,
                   sema_render_text_node(
                       env->out, env->program, CLUX_NODE_SYMBOL, "var", loc));
    node_set_child(
        item_var, CLUX_STMT_VAR_NAME, sema_clone_impl(name_src, env));
    node_t *item_type = sema_clone_impl(stmt_foreach_node_get_type(src), env);
    if (!item_type && env->sema) {
      const char *item_type_name =
          sema_type_render_name(sema_get_node_type(env->sema, name_src));
      if (item_type_name) {
        item_type = sema_render_text_node(env->out,
                                          env->program,
                                          CLUX_NODE_LITERAL_IDENTIFIER,
                                          item_type_name,
                                          loc);
      }
    }
    node_set_child(item_var, CLUX_STMT_VAR_TYPE, item_type);
    node_set_child(item_var, CLUX_STMT_VAR_VALUE, index);
    node_set_child(item_var, CLUX_STMT_VAR_EXPORT, NULL);
    node_set_child(item_var, CLUX_STMT_VAR_COMPTIME, NULL);
    node_add_child(body_block, item_var);
  } else {
    /* 裸名：name = __iter[__i];（name 已在外层定义，每轮赋值）。 */
    node_t *item_assign = create_assign_node(env->out);
    node_set_location(item_assign, node_get_location(name_src));
    assign_node_set_left(item_assign, sema_clone_impl(name_src, env));
    assign_node_set_symbol(
        item_assign,
        sema_render_text_node(
            env->out, env->program, CLUX_NODE_SYMBOL, "=", loc));
    assign_node_set_value(item_assign, index);
    /* 语句位须包 STMT_EXPR（unparse/分派按语句 kind 走）。 */
    node_t *item_stmt = create_stmt_expr_node(env->out);
    node_set_location(item_stmt, node_get_location(name_src));
    stmt_expr_node_set_expr(item_stmt, item_assign);
    node_add_child(body_block, item_stmt);
  }
  node_add_child(body_block,
                 sema_clone_impl(stmt_foreach_node_get_body(src), env));
  /* for 槽序 INIT/CONDITION/UPDATE/BODY。 */
  node_t *loop = create_stmt_for_node(env->out);
  node_set_location(loop, loc);
  node_set_child(loop, CLUX_STMT_FOR_INIT, NULL); /* __i 在块首声明 */
  node_set_child(loop, CLUX_STMT_FOR_CONDITION, cond);
  node_set_child(loop, CLUX_STMT_FOR_UPDATE, step);
  node_set_child(loop, CLUX_STMT_FOR_BODY, body_block);
  /* 外层块：{ var __iter: T? = ...; var __i = 0; for (...); } */
  node_t *block = create_stmt_block_node(env->out);
  node_set_location(block, loc);
  node_add_child(block, iter_var);
  node_add_child(block, idx_var);
  node_add_child(block, loop);
  return block;
}

/* ---- §2.6 comptime foreach 展开 ----
   ITERABLE 为 EXPR_TUPLE 字面序列（walk 已校验，不可展开已在 walk 报
   硬错误）：循环体按元素逐份拷贝，迭代变量引用替换为元素源子树克隆
   （文本匹配，最小版不做遮蔽分析）。 */
static void sema_expand_comptime_foreach_into(node_t *src,
                                              node_t *into,
                                              sema_clone_env_t *env) {
  node_t *iterable = stmt_foreach_node_get_iterable(src);
  node_t *body = stmt_foreach_node_get_body(src);
  if (!iterable || !body) {
    return;
  }
  char *name = location_get(node_get_location(stmt_foreach_node_get_name(src)),
                            env->out);
  vec_t *elems = node_get_children(iterable);
  size_t count = vec_get_size(elems);
  for (size_t idx = 0; idx < count; idx++) {
    env->subst_name = name;
    env->subst_src = (node_t *)vec_get(elems, idx);
    node_t *copy = sema_clone_impl(body, env);
    env->subst_name = NULL;
    env->subst_src = NULL;
    if (copy) {
      node_add_child(into, copy);
    }
  }
  allocator_free(env->out, (void **)&name);
}

static node_t *sema_expand_comptime_foreach(node_t *src,
                                            sema_clone_env_t *env) {
  node_t *block = create_stmt_block_node(env->out);
  node_set_location(block, node_get_location(src));
  sema_expand_comptime_foreach_into(src, block, env);
  return block;
}

/* §2.5 剪枝决策查询：comptime if 命中 → 选中臂（NULL=整体摘除在
   父块语句位处理）；未命中返回 -1。 */
static int
sema_prune_lookup(node_t *node, sema_clone_env_t *env, sema_prune_t **out) {
  if (!env->prunes || node_get_kind(node) != CLUX_NODE_STMT_COMPTIME_IF) {
    return -1;
  }
  for (size_t idx = 0; idx < vec_get_size(env->prunes); idx++) {
    sema_prune_t *prune = (sema_prune_t *)vec_get(env->prunes, idx);
    if (prune->stmt == node) {
      *out = prune;
      return 0;
    }
  }
  return -1;
}

/* §2.10 member_call 退化：callee 为 "." 成员访问（receiver.method）时
   按方法表改写为 Type$name(receiver, ...)——receiver 为方法所属类型
   的具名构造时按 struct 名直匹配，否则方法名全表唯一才匹配（防跨
   类型同名方法误改写）。指针 self 形态（形参类型 *T 文本）receiver
   先包 .&。未命中返回 NULL 走通用克隆。 */
static node_t *sema_desugar_member_call(node_t *node, sema_clone_env_t *env) {
  vec_t *children = node_get_children(node);
  node_t *callee =
      vec_get_size(children) > 0 ? (node_t *)vec_get(children, 0) : NULL;
  if (!callee || node_get_kind(callee) != CLUX_NODE_EXPR_BINARY) {
    return NULL;
  }
  vec_t *bin_children = node_get_children(callee);
  node_t *receiver = vec_get_size(bin_children) > 0
                         ? (node_t *)vec_get(bin_children, 0)
                         : NULL;
  node_t *symbol = vec_get_size(bin_children) > 1
                       ? (node_t *)vec_get(bin_children, 1)
                       : NULL;
  node_t *method_ref = vec_get_size(bin_children) > 2
                           ? (node_t *)vec_get(bin_children, 2)
                           : NULL;
  if (!receiver || !symbol || !sema_clone_is_identifier_leaf(method_ref)) {
    return NULL;
  }
  /* 只处理 "."（"::" 命名空间访问同形，文本判别）。 */
  char *op = location_get(node_get_location(symbol), env->out);
  int is_member = op && strcmp(op, ".") == 0;
  allocator_free(env->out, (void **)&op);
  if (!is_member) {
    return NULL;
  }
  char *method_name = location_get(node_get_location(method_ref), env->out);
  if (!method_name) {
    return NULL;
  }
  /* receiver 为具名构造时按 struct 名直匹配（方法表收集自原树，
     method_ref 同样来自原树文本，不经 renames 改写）。 */
  char *recv_type = NULL;
  if (node_get_kind(receiver) == CLUX_NODE_EXPR_CONSTRUCT) {
    node_t *type = construct_node_get_type(receiver);
    if (sema_clone_is_identifier_leaf(type)) {
      recv_type = location_get(node_get_location(type), env->out);
    }
  }
  sema_method_t *hit = NULL;
  int unique = 1;
  for (size_t idx = 0; idx < vec_get_size(env->methods); idx++) {
    sema_method_t *method = (sema_method_t *)vec_get(env->methods, idx);
    if (strcmp(method->method_name, method_name) != 0) {
      continue;
    }
    if (recv_type && strcmp(method->struct_name, recv_type) == 0) {
      hit = method;
      unique = 1;
      break;
    }
    /* 非直匹配：方法名全表唯一才可改写（跨类型同名保持原样）。 */
    hit = hit ? hit : method;
    unique = hit == method;
  }
  allocator_free(env->out, (void **)&method_name);
  allocator_free(env->out, (void **)&recv_type);
  if (!hit || !unique) {
    return NULL;
  }
  node_t *call = create_call_node(env->out);
  node_set_location(call, node_get_location(node));
  call_node_set_callee(call,
                       sema_render_text_node(env->out,
                                             env->program,
                                             CLUX_NODE_LITERAL_IDENTIFIER,
                                             hit->mangled,
                                             node_get_location(method_ref)));
  node_t *self_arg = sema_clone_impl(receiver, env);
  if (hit->self_ptr) {
    /* 指针 self：receiver 包 .&（后缀形态复用 binary，RIGHT 置 NULL）。 */
    node_t *addr = create_binary_node(env->out);
    node_set_location(addr, node_get_location(receiver));
    binary_node_set_left(addr, self_arg);
    binary_node_set_symbol(addr,
                           sema_render_text_node(env->out,
                                                 env->program,
                                                 CLUX_NODE_SYMBOL,
                                                 ".&",
                                                 node_get_location(receiver)));
    call_node_add_arg(call, addr);
  } else {
    call_node_add_arg(call, self_arg);
  }
  for (size_t idx = 1; idx < vec_get_size(children); idx++) {
    call_node_add_arg(call,
                      sema_clone_impl((node_t *)vec_get(children, idx), env));
  }
  return call;
}

/* §4.17 #dispose(x) 脱糖：实参类型可命中 disposable 谓词时改写为
   $dispose$T 调用——指针实参（*T 且 T disposable）直接作用于目标，
   左值实参（T disposable）自动取址 x.&（零拷贝，不触发 __init_copy__）。
   谓词假返回 NULL 走通用克隆（实参照常求值后丢弃=静默无操作），
   编译期不做重复释放静态检查。实参类型锚点由 check pass 在
   EXPR_BUILTIN 分支登记（§8.5 类型结果表）。 */
static node_t *sema_desugar_dispose_builtin(node_t *node,
                                            sema_clone_env_t *env) {
  if (!env->sema || !env->disposes) {
    return NULL;
  }
  if (builtin_node_get_arg_count(node) != 1) {
    return NULL;
  }
  node_t *name_sym = builtin_node_get_name(node);
  char *bname =
      name_sym ? location_get(node_get_location(name_sym), env->out) : NULL;
  int is_dispose = bname && strcmp(bname, "dispose") == 0;
  allocator_free(env->out, (void **)&bname);
  if (!is_dispose) {
    return NULL;
  }
  node_t *arg = builtin_node_get_arg(node, 0);
  if (!arg) {
    return NULL;
  }
  clux_type_t *type = sema_get_node_type(env->sema, arg);
  clux_type_t *target = NULL;
  int take_addr = 0;
  if (type && clux_type_kind_unwrap(type) == CLUX_TYPE_PTR) {
    /* 指针实参：析构指向对象，指针直传不改写。 */
    target = clux_type_get_ptr_base(type);
  } else if (sema_type_needs_dispose(env->sema, type)) {
    /* 左值实参：自动取址。 */
    target = type;
    take_addr = 1;
  }
  if (!sema_type_needs_dispose(env->sema, target)) {
    /* 谓词假：静默无操作——#dispose(x) 退化为裸实参表达式（实参照常
       求值后丢弃），不留 exec 无法执行的 builtin 节点。 */
    return type ? sema_clone_impl(arg, env) : NULL;
  }
  const char *type_name = clux_type_get_name(target);
  if (!type_name) {
    return NULL;
  }
  const location_t *loc = node_get_location(node);
  const char *callee =
      sema_dispose_callee_name(env->out, env->program, type_name);
  if (!callee) {
    return NULL;
  }
  node_t *call = create_call_node(env->out);
  node_set_location(call, loc);
  call_node_set_callee(
      call,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, callee, loc));
  node_t *arg_clone = sema_clone_impl(arg, env);
  if (take_addr) {
    node_t *addr = create_binary_node(env->out);
    node_set_location(addr, loc);
    binary_node_set_left(addr, arg_clone);
    binary_node_set_symbol(
        addr,
        sema_render_text_node(
            env->out, env->program, CLUX_NODE_SYMBOL, ".&", loc));
    arg_clone = addr;
  }
  call_node_add_arg(call, arg_clone);
  sema_dispose_register(env, target, loc);
  return call;
}

/* 递归主体：全部通道经 env 注入。 */
static node_t *sema_clone_impl(node_t *node, sema_clone_env_t *env) {
  if (!node) {
    return NULL;
  }
  /* §2.6 展开期迭代变量替换：命中即换为当前元素源子树的克隆
     （克隆元素期间挂起替换态，避免自引用）。 */
  if (env->subst_name && node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER) {
    char *text = location_get(node_get_location(node), env->out);
    if (text) {
      int match = strcmp(text, env->subst_name) == 0;
      allocator_free(env->out, (void **)&text);
      if (match) {
        const char *saved_name = env->subst_name;
        node_t *saved_src = env->subst_src;
        env->subst_name = NULL;
        env->subst_src = NULL;
        node_t *replacement = sema_clone_impl(saved_src, env);
        env->subst_name = saved_name;
        env->subst_src = saved_src;
        return replacement;
      }
    }
  }
  /* §2.3 实例体克隆态：代换/自引用折叠/泛型表摘除（须先于使用点
     instants 折叠——体内自引用使用点带形参文本，直接折叠会拼错名）。 */
  if (env->instantiate) {
    node_kind_t inst_kind = node_get_kind(node);
    if (inst_kind == CLUX_NODE_GENERIC_PARAMS) {
      return NULL; /* GENERICS 槽置 NULL：实例体非泛型 */
    }
    if (inst_kind == CLUX_NODE_LITERAL_IDENTIFIER) {
      node_t *substituted = sema_clone_param_subst(node, env);
      if (substituted) {
        return substituted;
      }
    }
    if (inst_kind == CLUX_NODE_EXPR_SUBSCRIPT) {
      node_t *folded = sema_clone_self_instant(node, env);
      if (folded) {
        return folded;
      }
    }
  }
  node_t *const_clone = sema_clone_const_fold(node, env);
  if (const_clone) {
    return const_clone;
  }
  node_t *instant_clone = sema_clone_instant(node, env);
  if (instant_clone) {
    return instant_clone;
  }
  /* 结构改写（§2.4–2.7）：命中即返回全新形态，不落通用克隆。 */
  node_kind_t kind = node_get_kind(node);
  if (kind == CLUX_NODE_STMT_BLOCK) {
    return sema_clone_block(node, env);
  }
  if (kind == CLUX_NODE_STMT_FOREACH) {
    return sema_desugar_foreach(node, env);
  }
  if (kind == CLUX_NODE_STMT_COMPTIME_FOREACH) {
    return sema_expand_comptime_foreach(node, env);
  }
  if (kind == CLUX_NODE_STMT_COMPTIME_IF) {
    sema_prune_t *prune = NULL;
    if (sema_prune_lookup(node, env, &prune) == 0) {
      /* 未选臂不存在：空臂。块级语句位的摘除由 sema_clone_block 预判，
         此处兜底返回 NULL（固定槽位场景 NULL 槽合法）。 */
      return prune->arm ? sema_clone_impl(prune->arm, env) : NULL;
    }
  }
  if (kind == CLUX_NODE_EXPR_CALL && env->methods) {
    node_t *call = sema_desugar_member_call(node, env);
    if (call) {
      return call;
    }
  }
  /* §4.17 #dispose 脱糖：谓词命中即改写为 $dispose$T 调用。 */
  if (kind == CLUX_NODE_EXPR_BUILTIN) {
    node_t *call = sema_desugar_dispose_builtin(node, env);
    if (call) {
      return call;
    }
  }
  vec_t *children = node_get_children(node);
  size_t count = vec_get_size(children);
  if (env->skip && count > 0) {
    /* 块内语句逐位判重：被提升的声明语句（skip 集合）整条不进入克隆。
       skip 只在块语句位命中——提升语句只作为 stmt 出现在块中。 */
    vec_t *kept = NULL;
    for (size_t idx = 0; idx < count; idx++) {
      node_t *child = (node_t *)vec_get(children, idx);
      int dropped = 0;
      if (child) {
        for (size_t s = 0; s < vec_get_size(env->skip); s++) {
          if (child == (node_t *)vec_get(env->skip, s)) {
            dropped = 1;
            break;
          }
        }
        /* §2.5 假臂无 else 的 comptime if：语句位整条摘除。 */
        if (!dropped) {
          sema_prune_t *prune = NULL;
          if (sema_prune_lookup(child, env, &prune) == 0 && !prune->arm) {
            dropped = 1;
          }
        }
      }
      if (dropped) {
        continue;
      }
      if (!kept) {
        kept = create_vec(env->out);
      }
      vec_push(kept, child);
    }
    if (kept) {
      children = kept;
      count = vec_get_size(kept);
    }
  }
  node_t *clone = create_node(env->out, node_get_kind(node));
  /* §2.3 P11：emit 克隆物化体时，location 文本命中 reown_from 文本池
     （新造文本 intern 自 sema->inst_program，指针精确匹配池内 string 数据）
     → 重 intern 进目标 program：产物新造文本自持，不借 sema 生命周期；
     源码文本不在池内，保持借调用方源缓冲的原契约。 */
  const location_t *loc = node_get_location(node);
  if (env->reown_from && loc && loc->begin.offset) {
    vec_t *pool = program_get_strings(env->reown_from);
    int hit = 0;
    for (size_t idx = 0; !hit && pool && idx < vec_get_size(pool); idx++) {
      string_t *str = vec_get(pool, idx);
      hit = loc->begin.offset == string_get_data(str);
    }
    if (hit) {
      size_t len = (size_t)(loc->end.offset - loc->begin.offset);
      const char *text =
          program_intern_string(env->program, loc->begin.offset, len);
      location_t owned = *loc;
      owned.begin.offset = text;
      owned.end.offset = text + len;
      node_set_location(clone, &owned);
    } else {
      node_set_location(clone, loc);
    }
  } else {
    node_set_location(clone, loc);
  }
  /* 改写目标：LITERAL_IDENTIFIER（名字/类型引用）、SYMBOL（关键字，
     不会与用户声明名同文本，条件覆盖仅为统一）。 */
  if ((kind == CLUX_NODE_SYMBOL || kind == CLUX_NODE_LITERAL_IDENTIFIER) &&
      env->renames && env->rename_count > 0) {
    for (size_t idx = 0; idx < env->rename_count; idx++) {
      if (!location_is(loc, env->renames[idx].from)) {
        continue;
      }
      /* 命中：文本区间改指 intern 的提升名（line/col/filename 保留
         原 SYMBOL 的值，to_json 的 text 即提升名，span 无地址）。 */
      size_t len = strlen(env->renames[idx].to);
      const char *text =
          program_intern_string(env->program, env->renames[idx].to, len);
      location_t renamed = *loc;
      renamed.begin.offset = text;
      renamed.end.offset = text + len;
      node_set_location(clone, &renamed);
      break;
    }
  }
  for (size_t idx = 0; idx < count; idx++) {
    node_t *child = (node_t *)vec_get(children, idx);
    /* §2.5：语句位 comptime if 命中剪枝 → 换克隆选中臂（drop 已在
       kept 过滤中移除，这里只剩 arm 替换）。 */
    sema_prune_t *prune = NULL;
    if (child && sema_prune_lookup(child, env, &prune) == 0 && prune->arm) {
      child = prune->arm;
    }
    node_add_child(clone, sema_clone_impl(child, env));
  }
  if (children != node_get_children(node)) {
    delete_vec(&children);
  }
  if (node_get_kind(node) == CLUX_NODE_STMT_VAR) {
    sema_clone_var_fixup(clone, node, env);
  }
  /* §2.9 匿名构造具名化：命中 anons 表的匿名构造点，TYPE 槽回填
     合成名 identifier（具名 struct 由 emit 尾部合成）。 */
  if (kind == CLUX_NODE_EXPR_CONSTRUCT && env->anons) {
    for (size_t idx = 0; idx < vec_get_size(env->anons); idx++) {
      sema_anon_t *anon = (sema_anon_t *)vec_get(env->anons, idx);
      if (anon->node != node) {
        continue;
      }
      vec_t *clone_children = node_get_children(clone);
      if (vec_get_size(clone_children) > (size_t)CLUX_EXPR_CONSTRUCT_TYPE &&
          vec_get(clone_children, CLUX_EXPR_CONSTRUCT_TYPE) == NULL) {
        node_set_child(clone,
                       CLUX_EXPR_CONSTRUCT_TYPE,
                       sema_render_text_node(env->out,
                                             env->program,
                                             CLUX_NODE_LITERAL_IDENTIFIER,
                                             anon->name,
                                             node_get_location(node)));
      }
      break;
    }
  }
  return clone;
}

void sema_clone_env_init(sema_clone_env_t *env,
                         allocator_t *out,
                         program_t *program,
                         vec_t *instants,
                         vec_t *consts,
                         vec_t *prunes,
                         clux_sema_t *sema) {
  env->out = out;
  env->program = program;
  env->instants = instants;
  env->consts = consts;
  env->prunes = prunes;
  env->anons = NULL;
  env->methods = NULL;
  env->renames = NULL;
  env->rename_count = 0;
  env->skip = NULL;
  env->subst_name = NULL;
  env->subst_src = NULL;
  env->inst_params = NULL;
  env->inst_args = NULL;
  env->instantiate = 0;
  env->reown_from = NULL;
  env->defer_counter = 0;
  env->disposes = NULL;
  env->sema = sema;
}

node_t *sema_clone_node_renamed(node_t *node, sema_clone_env_t *env) {
  return sema_clone_impl(node, env);
}
