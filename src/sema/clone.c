/* 克隆器：逆向编排（设计文档 §4 ②）两条新建通道之一——未变换子树的
   deep clone。产物分配在 out_allocator，与原树无任何共享节点；
   loc 按值浅拷（position 的 offset 仍可借用 parser 源码缓冲，
   与原树同源，program_t 不因此持有 engine 指针）。 */
#include "sema/sema_internal.h"
#include "core/location.h"
#include "core/vec.h"
#include "parser/node_stmt_var.h"
#include <stdio.h>
#include <string.h>

/* 实例名折叠（§2.3 最小版）：使用点（EXPR_SUBSCRIPT）替换为单个
   名字节点，location 区间指向 intern 的实例名（line/col/filename
   沿 base 的值）。命中即整棵子树不再递归。 */
static node_t *sema_clone_instant(node_t *node,
                                  allocator_t *out,
                                  program_t *program,
                                  vec_t *instants) {
  if (!instants || node_get_kind(node) != CLUX_NODE_EXPR_SUBSCRIPT) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(instants); idx++) {
    sema_instant_t *instant = (sema_instant_t *)vec_get(instants, idx);
    if (instant->node != node) {
      continue;
    }
    size_t len = strlen(instant->name);
    const char *text = program_intern_string(program, instant->name, len);
    node_t *clone = create_node(out, CLUX_NODE_LITERAL_IDENTIFIER);
    location_t renamed = instant->base_loc;
    renamed.begin.offset = text;
    renamed.end.offset = text + len;
    node_set_location(clone, &renamed);
    return clone;
  }
  return NULL;
}

/* 常量折叠（§2.1 最小版）：引用点（LITERAL_IDENTIFIER）命中常量表
   → 渲染为字面量节点（intern 文本，位置字段沿引用点）。命中即止。 */
static node_t *sema_clone_const_fold(node_t *node,
                                     allocator_t *out,
                                     program_t *program,
                                     vec_t *consts) {
  if (!consts || node_get_kind(node) != CLUX_NODE_LITERAL_IDENTIFIER) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(consts); idx++) {
    sema_const_use_t *use = (sema_const_use_t *)vec_get(consts, idx);
    if (use->node != node) {
      continue;
    }
    return sema_render_const(
        out, program, &use->def->value, node_get_location(node));
  }
  return NULL;
}

/* 推断显式化（§2.8 最小版）：TYPE 槽缺失时按 VALUE 字面量回填——
   带后缀整数以后缀定类型；无后缀整数默认 i32 且文本补 i32 后缀
   （浮点后缀由语法层强制）；布尔为 bool。VALUE 非字面量（调用、
   未折叠引用等）不动，等类型检查分期。补后缀只改 VALUE 节点的
   location（指向 intern 文本），不替换节点，避免孤儿分配。 */
static void
sema_clone_var_fixup(node_t *clone, allocator_t *out, program_t *program) {
  vec_t *children = node_get_children(clone);
  if (vec_get_size(children) < (size_t)CLUX_STMT_VAR_FIRST_DECORATOR) {
    return;
  }
  node_t *type_child = (node_t *)vec_get(children, CLUX_STMT_VAR_TYPE);
  node_t *value_child = (node_t *)vec_get(children, CLUX_STMT_VAR_VALUE);
  if (type_child || !value_child) {
    return;
  }
  const location_t *loc = node_get_location(value_child);
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
  }
  if (!type_name) {
    return;
  }
  /* 类型名与 identifier 合一（白皮书 §2.1）→ LITERAL_IDENTIFIER。 */
  node_t *type_node = sema_render_text_node(
      out, program, CLUX_NODE_LITERAL_IDENTIFIER, type_name, loc);
  node_set_child(clone, CLUX_STMT_VAR_TYPE, type_node);
}

/* 递归主体：命名与摘除判定由调用方经 renames/skip/instants/consts
   注入，均可为空。 */
static node_t *sema_clone_impl(node_t *node,
                               allocator_t *out,
                               program_t *program,
                               const sema_rename_t *renames,
                               size_t rename_count,
                               vec_t *skip,
                               vec_t *instants,
                               vec_t *consts) {
  if (!node) {
    return NULL;
  }
  node_t *const_clone = sema_clone_const_fold(node, out, program, consts);
  if (const_clone) {
    return const_clone;
  }
  node_t *instant_clone = sema_clone_instant(node, out, program, instants);
  if (instant_clone) {
    return instant_clone;
  }
  vec_t *children = node_get_children(node);
  size_t count = vec_get_size(children);
  if (skip && count > 0) {
    /* 块内语句逐位判重：被提升的声明语句（skip 集合）整条不进入克隆。
       skip 只在块语句位命中——提升语句只作为 stmt 出现在块中。 */
    vec_t *kept = NULL;
    for (size_t idx = 0; idx < count; idx++) {
      node_t *child = (node_t *)vec_get(children, idx);
      int dropped = 0;
      if (child) {
        for (size_t s = 0; s < vec_get_size(skip); s++) {
          if (child == (node_t *)vec_get(skip, s)) {
            dropped = 1;
            break;
          }
        }
      }
      if (dropped) {
        continue;
      }
      if (!kept) {
        kept = create_vec(out);
      }
      vec_push(kept, child);
    }
    if (kept) {
      children = kept;
      count = vec_get_size(kept);
    }
  }
  node_t *clone = create_node(out, node_get_kind(node));
  node_set_location(clone, node_get_location(node));
  /* 改写目标：LITERAL_IDENTIFIER（名字/类型引用）、SYMBOL（关键字，
     不会与用户声明名同文本，条件覆盖仅为统一）。 */
  node_kind_t kind = node_get_kind(node);
  if ((kind == CLUX_NODE_SYMBOL || kind == CLUX_NODE_LITERAL_IDENTIFIER) &&
      renames && rename_count > 0) {
    const location_t *loc = node_get_location(node);
    for (size_t idx = 0; idx < rename_count; idx++) {
      if (!location_is(loc, renames[idx].from)) {
        continue;
      }
      /* 命中：文本区间改指 intern 的提升名（line/col/filename 保留
         原 SYMBOL 的值，to_json 的 text 即提升名，span 无地址）。 */
      size_t len = strlen(renames[idx].to);
      const char *text = program_intern_string(program, renames[idx].to, len);
      location_t renamed = *loc;
      renamed.begin.offset = text;
      renamed.end.offset = text + len;
      node_set_location(clone, &renamed);
      break;
    }
  }
  for (size_t idx = 0; idx < count; idx++) {
    node_t *child = sema_clone_impl((node_t *)vec_get(children, idx),
                                    out,
                                    program,
                                    renames,
                                    rename_count,
                                    skip,
                                    instants,
                                    consts);
    node_add_child(clone, child);
  }
  if (children != node_get_children(node)) {
    delete_vec(&children);
  }
  if (node_get_kind(node) == CLUX_NODE_STMT_VAR) {
    sema_clone_var_fixup(clone, out, program);
  }
  return clone;
}

node_t *sema_clone_node_renamed(node_t *node,
                                allocator_t *out,
                                program_t *program,
                                const sema_rename_t *renames,
                                size_t rename_count,
                                vec_t *skip,
                                vec_t *instants,
                                vec_t *consts) {
  return sema_clone_impl(
      node, out, program, renames, rename_count, skip, instants, consts);
}
