/* §4.17 RAII P3：临时值析构（lift 通道）。
   - 临时根判定：EXPR_CALL / EXPR_CONSTRUCT 且 §8.5 类型结果表命中
     disposable 类型（锚点由 P3a check 侧登记）。armed 上下文（块级
     语句槽克隆期，clone.c 槽驱动置 lift_active）命中即捕获。
   - 捕获协议：临时根克隆后登记 sema_lift_temp_t（$tmp$N interned 名 +
     预渲染类型表达式 + $dispose$T 合成 callee——渲染即验证，类型不可名
     返回 NULL 由 guard 原样保留=pre-P3 行为），调用点替换为 $tmp$N
     标识符替身；flush 在语句槽边界：var 提升到语句前（水位线 base
     隔离外层 pending），$dispose$T($tmp.&) 逆序追加语句后，随后 drain。
   - 槽直通（lift_skip_top）：var/return 的 VALUE 槽顶层单一临时根不
     捕获——var 直接 bind（避免 __init_copy__ 二次拷贝）、return 所有权
     随值转移；foreach iterable 由脱糖直连 __iter。槽位在 children 循环
     按 idx 结构化识别消费（clone.c），嵌套子树照常捕获。
   - return 槽 $ret 物化：捕获非空时 return 表达式引用 $tmp，直接
     [析构][return] 会 use-after-dispose——先物化 var $ret$N: T =
     <return 表达式>（类型取 return 值根锚点），析构后 return $ret$N。
     $ret 不析构（所有权流出）。锚点缺失不物化（调用方沿用裸形态，
     跳过析构保语义——泄漏保守正确，P3c 登记验证）。
   - 循环降维（零 flag 方案）：cond/update（for 另含 init）含临时根的
     while/do-while/for 统一降为 for(;;) + 段内 gate 块——临时在 gate
     块内每轮声明每轮析构（exec 块每执行 push 新 scope），cond 求值
     次数与原语义一致。do-while 的 gate 放 body 尾（continue 跳过
     cond_gate 为 v1 已知偏差：原语义 continue 会再求值 cond）；
     for 的 update gate 放 UPDATE 槽（STMT_BLOCK：[vars]update'[析构]，
     continue 经 for 出口协议先走 update 再走 cond，对齐原语义）。 */
#include "sema/sema_internal.h"
#include "core/program.h"
#include "core/vec.h"
#include "engine/type.h"
#include "parser/node.h"
#include "parser/node_binary.h"
#include "parser/node_builtin.h"
#include "parser/node_call.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_do_while.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_jump.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_while.h"
#include <stdio.h>
#include <string.h>

int sema_lift_is_root(node_t *node, sema_clone_env_t *env) {
  if (!node || !env->sema || !env->lift_temps) {
    return 0;
  }
  node_kind_t kind = node_get_kind(node);
  if (kind != CLUX_NODE_EXPR_CALL && kind != CLUX_NODE_EXPR_CONSTRUCT) {
    return 0;
  }
  clux_type_t *type = sema_get_node_type(env->sema, node);
  return sema_type_needs_dispose(env->sema, type);
}

/* 惰性节点（&&/||/?:）：子表达式条件求值，提升即提前求值破坏短路，
   guard 在克隆期对整棵解除武装（clone.c），扫描期同构跳过。 */
int sema_lift_is_lazy_parent(node_t *node, sema_clone_env_t *env) {
  node_kind_t kind = node_get_kind(node);
  if (kind == CLUX_NODE_EXPR_TERNARY) {
    return 1;
  }
  if (kind != CLUX_NODE_EXPR_BINARY) {
    return 0;
  }
  vec_t *children = node_get_children(node);
  node_t *symbol =
      vec_get_size(children) > 1 ? (node_t *)vec_get(children, 1) : NULL;
  if (!symbol || node_get_kind(symbol) != CLUX_NODE_SYMBOL) {
    return 0;
  }
  char *op = location_get(node_get_location(symbol), env->out);
  int lazy = op && (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0);
  allocator_free(env->out, (void **)&op);
  return lazy;
}

/* 子树根扫描（loop_applies 判定用）：与 guard 的捕获规则镜像——惰性
   上下文、函数定义子树（形参默认值不捕获）、#dispose 实参（P1 沿用
   直连克隆不捕获）跳过，其余递归判定临时根存在性。 */
static int sema_lift_subtree_has_root(node_t *node, sema_clone_env_t *env) {
  if (!node) {
    return 0;
  }
  node_kind_t kind = node_get_kind(node);
  if (kind == CLUX_NODE_EXPR_FUNC_DECL || kind == CLUX_NODE_STMT_FUNC) {
    return 0;
  }
  if (sema_lift_is_lazy_parent(node, env)) {
    return 0;
  }
  if (kind == CLUX_NODE_EXPR_BUILTIN) {
    node_t *name_sym = builtin_node_get_name(node);
    char *bname =
        name_sym ? location_get(node_get_location(name_sym), env->out) : NULL;
    int is_dispose = bname && strcmp(bname, "dispose") == 0;
    allocator_free(env->out, (void **)&bname);
    if (is_dispose) {
      return 0;
    }
  }
  if (sema_lift_is_root(node, env)) {
    return 1;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    if (sema_lift_subtree_has_root((node_t *)vec_get(children, idx), env)) {
      return 1;
    }
  }
  return 0;
}

node_t *
sema_lift_capture(sema_clone_env_t *env, clux_type_t *type, node_t *cloned) {
  if (!env->lift_temps || !type || !cloned) {
    return NULL;
  }
  const location_t *loc = node_get_location(cloned);
  char buf[32];
  int written = snprintf(buf, sizeof(buf), "$tmp$%zu", env->lift_counter);
  if (written <= 0 || (size_t)written >= sizeof(buf)) {
    return NULL;
  }
  const char *name = program_intern_string(env->program, buf, (size_t)written);
  const char *type_name = clux_type_get_name(type);
  const char *callee =
      type_name ? sema_dispose_callee_name(env->out, env->program, type_name)
                : NULL;
  node_t *type_node =
      sema_dispose_render_type(env->out, env->program, type, loc);
  if (!name || !callee || !type_node) {
    /* 捕获期渲染即验证：类型不可名/分配失败放弃捕获（调用方原样保留
       克隆子树，pre-P3 行为）。type_node 为 out 借用产物，program 持有
       文本，节点孤立不回灌即随 out 释放。 */
    return NULL;
  }
  sema_lift_temp_t *entry = allocator_alloc(env->out, sizeof(sema_lift_temp_t));
  if (!entry) {
    return NULL;
  }
  env->lift_counter++;
  entry->name = name;
  entry->value = cloned;
  entry->type_node = type_node;
  entry->callee = callee;
  entry->loc = loc;
  (void)vec_push(env->lift_temps, entry);
  /* 递归登记 disposable 成分：合成体的 $dispose$<成分> 同批存在
     （与 P1 var 登记同通道）。 */
  sema_dispose_register(env, type, loc);
  return sema_render_text_node(
      env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, name, loc);
}

void sema_lift_emit_vars(sema_clone_env_t *env, node_t *block, size_t base) {
  vec_t *temps = env->lift_temps;
  for (size_t idx = base; idx < vec_get_size(temps); idx++) {
    sema_lift_temp_t *entry = (sema_lift_temp_t *)vec_get(temps, idx);
    /* var $tmp$N: T = <init>;（TYPE 预渲染非 NULL，不经 §2.8 回填）。 */
    node_t *var = create_stmt_var_node(env->out);
    node_set_location(var, entry->loc);
    stmt_var_node_set_keyword(
        var,
        sema_render_text_node(
            env->out, env->program, CLUX_NODE_SYMBOL, "var", entry->loc));
    stmt_var_node_set_name(var,
                           sema_render_text_node(env->out,
                                                 env->program,
                                                 CLUX_NODE_LITERAL_IDENTIFIER,
                                                 entry->name,
                                                 entry->loc));
    stmt_var_node_set_type(var, entry->type_node); /* 所有权移入 */
    stmt_var_node_set_value(var, entry->value);    /* 所有权移入 */
    stmt_var_node_set_export(var, NULL);
    stmt_var_node_set_comptime(var, NULL);
    node_add_child(block, var);
  }
}

/* $dispose$T($tmp.&)——与 P1 var 出口析构同形态（后缀 .& 零拷贝取址）。
   逆序（捕获序 LIFO）：后捕获的临时先析构。 */
void sema_lift_emit_disposes(sema_clone_env_t *env,
                             node_t *block,
                             size_t base) {
  vec_t *temps = env->lift_temps;
  for (size_t idx = vec_get_size(temps); idx > base; idx--) {
    sema_lift_temp_t *entry = (sema_lift_temp_t *)vec_get(temps, idx - 1);
    node_t *call = create_call_node(env->out);
    node_set_location(call, entry->loc);
    call_node_set_callee(call,
                         sema_render_text_node(env->out,
                                               env->program,
                                               CLUX_NODE_LITERAL_IDENTIFIER,
                                               entry->callee,
                                               entry->loc));
    node_t *target = sema_render_text_node(env->out,
                                           env->program,
                                           CLUX_NODE_LITERAL_IDENTIFIER,
                                           entry->name,
                                           entry->loc);
    node_t *addr = create_binary_node(env->out);
    node_set_location(addr, entry->loc);
    binary_node_set_left(addr, target);
    binary_node_set_symbol(
        addr,
        sema_render_text_node(
            env->out, env->program, CLUX_NODE_SYMBOL, ".&", entry->loc));
    call_node_add_arg(call, addr);
    node_t *wrap = create_stmt_expr_node(env->out);
    node_set_location(wrap, entry->loc);
    stmt_expr_node_set_expr(wrap, call);
    node_add_child(block, wrap);
  }
}

void sema_lift_drain(sema_clone_env_t *env, size_t base) {
  vec_t *temps = env->lift_temps;
  for (size_t idx = vec_get_size(temps); idx > base; idx--) {
    sema_lift_temp_t *entry = (sema_lift_temp_t *)vec_get(temps, idx - 1);
    /* value/type_node 已随 emit_vars 移入提升 var；name/callee 为 program
       内串，loc 借用源树——条目结构体本身归 out，手动释放。 */
    allocator_free(env->out, (void **)&entry);
  }
  (void)vec_resize(temps, base);
}

int sema_lift_materialize_return(sema_clone_env_t *env,
                                 node_t *block,
                                 node_t *src_ret,
                                 node_t *ret_clone) {
  if (!env->sema || !block || !src_ret || !ret_clone) {
    return 0;
  }
  node_t *src_value = stmt_return_node_get_value(src_ret);
  if (!src_value) {
    return 0;
  }
  /* 类型取 return 值根的 §8.5 锚点（P3a：CALL/CONSTRUCT 已锚）。锚点
     缺失不物化，调用方沿用裸形态。 */
  clux_type_t *type = sema_get_node_type(env->sema, src_value);
  if (!type) {
    return 0;
  }
  const location_t *loc = node_get_location(src_ret);
  char buf[32];
  int written = snprintf(buf, sizeof(buf), "$ret$%zu", env->lift_counter);
  if (written <= 0 || (size_t)written >= sizeof(buf)) {
    return 0;
  }
  const char *ret_name =
      program_intern_string(env->program, buf, (size_t)written);
  node_t *type_node =
      sema_dispose_render_type(env->out, env->program, type, loc);
  if (!ret_name || !type_node) {
    return 0;
  }
  /* 摘出 return 克隆体的值表达式 → var $ret$N: T = <expr>;（匿名值
     move 具名化，$tmp 引用的临时经 __init_copy__ 置空源后析构为幂等
     空操作）。 */
  node_t *value_clone = stmt_return_node_get_value(ret_clone);
  if (!value_clone) {
    return 0;
  }
  node_t *var = create_stmt_var_node(env->out);
  node_set_location(var, loc);
  stmt_var_node_set_keyword(
      var,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_SYMBOL, "var", loc));
  stmt_var_node_set_name(
      var,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, ret_name, loc));
  stmt_var_node_set_type(var, type_node);
  stmt_var_node_set_value(var, value_clone);
  stmt_var_node_set_export(var, NULL);
  stmt_var_node_set_comptime(var, NULL);
  node_add_child(block, var);
  env->lift_counter++;
  /* return 表达式改指 $ret$N（node_set_child 换槽，值已移走无孤儿）。 */
  node_set_child(
      ret_clone,
      CLUX_STMT_RETURN_VALUE,
      sema_render_text_node(
          env->out, env->program, CLUX_NODE_LITERAL_IDENTIFIER, ret_name, loc));
  return 1;
}

int sema_lift_slot_applies(node_t *stmt) {
  if (!stmt) {
    return 0;
  }
  switch (node_get_kind(stmt)) {
  case CLUX_NODE_STMT_EXPR:
  case CLUX_NODE_STMT_IF:
  case CLUX_NODE_STMT_SWITCH:
  case CLUX_NODE_STMT_ASSERT:
  case CLUX_NODE_STMT_RETURN:
  case CLUX_NODE_STMT_VAR:
  case CLUX_NODE_STMT_FOREACH:
    return 1;
  default:
    return 0;
  }
}

/* ---- 循环降维（§4.17 P3 零 flag 方案） ---- */

/* cond gate 块：{ [vars]; if (cond') {} else { [析构] break; } [析构]; }
   ——空 then 块；假臂先析构再 break，真臂尾析构，两分支互斥（幂等
   兜底）。cond 经槽协议克隆（捕获 temp 的 var 与析构全落 gate 块内，
   每轮随块重建作用域）。*/
static node_t *sema_lift_cond_gate(node_t *cond_src,
                                   sema_clone_env_t *env,
                                   const location_t *loc) {
  node_t *gate = create_stmt_block_node(env->out);
  node_set_location(gate, loc);
  size_t base = vec_get_size(env->lift_temps);
  int saved_active = env->lift_active;
  int saved_skip = env->lift_skip_top;
  env->lift_active = 1;
  env->lift_skip_top = 0;
  node_t *cond_clone = sema_clone_node_renamed(cond_src, env);
  env->lift_active = saved_active;
  env->lift_skip_top = saved_skip;
  int captured = vec_get_size(env->lift_temps) > base;
  if (captured) {
    sema_lift_emit_vars(env, gate, base);
  }
  node_t *then_block = create_stmt_block_node(env->out);
  node_set_location(then_block, loc);
  node_t *else_block = create_stmt_block_node(env->out);
  node_set_location(else_block, loc);
  if (captured) {
    sema_lift_emit_disposes(env, else_block, base);
  }
  node_t *brk = create_stmt_break_node(env->out);
  node_set_location(brk, loc);
  node_add_child(else_block, brk);
  node_t *if_stmt = create_stmt_if_node(env->out);
  node_set_location(if_stmt, loc);
  stmt_if_node_set_condition(if_stmt, cond_clone);
  stmt_if_node_set_then(if_stmt, then_block);
  stmt_if_node_set_else(if_stmt, else_block);
  node_add_child(gate, if_stmt);
  if (captured) {
    sema_lift_emit_disposes(env, gate, base);
    sema_lift_drain(env, base);
  }
  return gate;
}

/* update gate 块（for UPDATE 槽，STMT_BLOCK 形态）：{ [vars];
   <update>'; [析构]; }——无 break（退出由 body 首的 cond gate 决定），
   continue 经 for 出口协议先走 update 再走 cond，与原语义对齐。 */
static node_t *sema_lift_update_gate(node_t *update_src,
                                     sema_clone_env_t *env,
                                     const location_t *loc) {
  node_t *gate = create_stmt_block_node(env->out);
  node_set_location(gate, loc);
  size_t base = vec_get_size(env->lift_temps);
  int saved_active = env->lift_active;
  int saved_skip = env->lift_skip_top;
  env->lift_active = 1;
  env->lift_skip_top = 0;
  node_t *update_clone = sema_clone_node_renamed(update_src, env);
  env->lift_active = saved_active;
  env->lift_skip_top = saved_skip;
  int captured = vec_get_size(env->lift_temps) > base;
  if (captured) {
    sema_lift_emit_vars(env, gate, base);
  }
  node_t *wrap = create_stmt_expr_node(env->out);
  node_set_location(wrap, loc);
  stmt_expr_node_set_expr(wrap, update_clone);
  node_add_child(gate, wrap);
  if (captured) {
    sema_lift_emit_disposes(env, gate, base);
    sema_lift_drain(env, base);
  }
  return gate;
}

int sema_lift_loop_applies(node_t *stmt, sema_clone_env_t *env) {
  if (!stmt || !env->sema || !env->lift_temps) {
    return 0;
  }
  node_kind_t kind = node_get_kind(stmt);
  if (kind == CLUX_NODE_STMT_WHILE || kind == CLUX_NODE_STMT_DO_WHILE) {
    node_t *cond = kind == CLUX_NODE_STMT_WHILE
                       ? stmt_while_node_get_condition(stmt)
                       : stmt_do_while_node_get_condition(stmt);
    return sema_lift_subtree_has_root(cond, env);
  }
  if (kind == CLUX_NODE_STMT_FOR) {
    return sema_lift_subtree_has_root(stmt_for_node_get_init(stmt), env) ||
           sema_lift_subtree_has_root(stmt_for_node_get_condition(stmt), env) ||
           sema_lift_subtree_has_root(stmt_for_node_get_update(stmt), env);
  }
  return 0;
}

/* while (C) B → for(;;) { gate(C); B'; }（B' 保持源块嵌套：作用域层次
   与原语义一致，break/continue 仍绑最近循环=for）。 */
static void
sema_lift_lower_while(node_t *src, node_t *block, sema_clone_env_t *env) {
  const location_t *loc = node_get_location(src);
  node_t *gate =
      sema_lift_cond_gate(stmt_while_node_get_condition(src), env, loc);
  node_t *body_clone =
      sema_clone_node_renamed(stmt_while_node_get_body(src), env);
  node_t *for_body = create_stmt_block_node(env->out);
  node_set_location(for_body, loc);
  node_add_child(for_body, gate);
  node_add_child(for_body, body_clone);
  node_t *loop = create_stmt_for_node(env->out);
  node_set_location(loop, loc);
  stmt_for_node_set_init(loop, NULL);
  stmt_for_node_set_condition(loop, NULL);
  stmt_for_node_set_update(loop, NULL);
  stmt_for_node_set_body(loop, for_body);
  node_add_child(block, loop);
}

/* do B while (C) → for(;;) { B'; gate(C); }——gate 在 body 块尾：
   continue 跳过 cond_gate 为 v1 已知偏差（原语义 continue 会再求值
   cond）。gate 不能放 UPDATE 槽（exec for-update 不消费 break 信号，
   gate 的 break 会被吞掉导致死循环）。 */
static void
sema_lift_lower_do_while(node_t *src, node_t *block, sema_clone_env_t *env) {
  const location_t *loc = node_get_location(src);
  node_t *gate =
      sema_lift_cond_gate(stmt_do_while_node_get_condition(src), env, loc);
  node_t *body_clone =
      sema_clone_node_renamed(stmt_do_while_node_get_body(src), env);
  node_t *for_body = create_stmt_block_node(env->out);
  node_set_location(for_body, loc);
  node_add_child(for_body, body_clone);
  node_add_child(for_body, gate);
  node_t *loop = create_stmt_for_node(env->out);
  node_set_location(loop, loc);
  stmt_for_node_set_init(loop, NULL);
  stmt_for_node_set_condition(loop, NULL);
  stmt_for_node_set_update(loop, NULL);
  stmt_for_node_set_body(loop, for_body);
  node_add_child(block, loop);
}

/* for (I; C; U) B 降低策略（分段处置）：
   - I 含根：temp 物化到循环外（var 提到 for 前、析构接 for 后——$tmp
     引用只在 init 槽，延后释放安全；var 形态 skip_top 直通 bind）；
   - C 含根：gate 入 body 首，COND 槽置空（NULL=恒真 §4.8）；每轮
     [gate(C)→B'→U'] 与原 [C→B→U] 求值序一致；
   - U 含根：gate 块入 UPDATE 槽（STMT_BLOCK）；
   - 无根段位保持原槽原样（active=0 克隆，不捕获）。 */
static void
sema_lift_lower_for(node_t *src, node_t *block, sema_clone_env_t *env) {
  const location_t *loc = node_get_location(src);
  /* init：var 形态顶层直通（bind），表达式形态照常捕获。 */
  node_t *init_src = stmt_for_node_get_init(src);
  size_t init_base = vec_get_size(env->lift_temps);
  node_t *init_clone = NULL;
  if (init_src) {
    int saved_active = env->lift_active;
    int saved_skip = env->lift_skip_top;
    env->lift_active = 1;
    env->lift_skip_top = node_get_kind(init_src) == CLUX_NODE_STMT_VAR ? 1 : 0;
    init_clone = sema_clone_node_renamed(init_src, env);
    env->lift_active = saved_active;
    env->lift_skip_top = saved_skip;
  }
  /* cond：有根→gate 入 body 首（COND 槽 NULL=恒真）；无根原样。 */
  node_t *cond_src = stmt_for_node_get_condition(src);
  node_t *cond_gate = NULL;
  node_t *cond_clone = NULL;
  if (cond_src && sema_lift_subtree_has_root(cond_src, env)) {
    cond_gate = sema_lift_cond_gate(cond_src, env, loc);
  } else if (cond_src) {
    cond_clone = sema_clone_node_renamed(cond_src, env);
  }
  /* update：有根→gate 块入 UPDATE 槽；无根原样。 */
  node_t *update_src = stmt_for_node_get_update(src);
  node_t *update_gate = NULL;
  node_t *update_clone = NULL;
  if (update_src && sema_lift_subtree_has_root(update_src, env)) {
    update_gate = sema_lift_update_gate(update_src, env, loc);
  } else if (update_src) {
    update_clone = sema_clone_node_renamed(update_src, env);
  }
  node_t *body_clone =
      sema_clone_node_renamed(stmt_for_node_get_body(src), env);
  node_t *for_body = create_stmt_block_node(env->out);
  node_set_location(for_body, loc);
  if (cond_gate) {
    node_add_child(for_body, cond_gate);
  }
  node_add_child(for_body, body_clone);
  node_t *loop = create_stmt_for_node(env->out);
  node_set_location(loop, loc);
  stmt_for_node_set_init(loop, init_clone);
  stmt_for_node_set_condition(loop, cond_clone);
  stmt_for_node_set_update(loop, update_gate ? update_gate : update_clone);
  stmt_for_node_set_body(loop, for_body);
  int init_captured = vec_get_size(env->lift_temps) > init_base;
  if (init_captured) {
    sema_lift_emit_vars(env, block, init_base);
  }
  node_add_child(block, loop);
  if (init_captured) {
    sema_lift_emit_disposes(env, block, init_base);
    sema_lift_drain(env, init_base);
  }
}

void sema_lift_lower_loop(node_t *src, node_t *block, sema_clone_env_t *env) {
  node_kind_t kind = node_get_kind(src);
  if (kind == CLUX_NODE_STMT_WHILE) {
    sema_lift_lower_while(src, block, env);
  } else if (kind == CLUX_NODE_STMT_DO_WHILE) {
    sema_lift_lower_do_while(src, block, env);
  } else if (kind == CLUX_NODE_STMT_FOR) {
    sema_lift_lower_for(src, block, env);
  }
}
