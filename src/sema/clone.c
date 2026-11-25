/* 克隆器：逆向编排（设计文档 §4 ②）两条新建通道之一——未变换子树的
   deep clone。产物分配在 out_allocator，与原树无任何共享节点；
   loc 按值浅拷（position 的 offset 仍可借用 parser 源码缓冲，
   与原树同源，program_t 不因此持有 engine 指针）。 */
#include "sema/sema_internal.h"
#include "core/location.h"
#include "core/vec.h"
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

/* 递归主体：命名与摘除判定由调用方经 renames/skip/instants 注入，
   三者均可为空。 */
static node_t *sema_clone_impl(node_t *node,
                               allocator_t *out,
                               program_t *program,
                               const sema_rename_t *renames,
                               size_t rename_count,
                               vec_t *skip,
                               vec_t *instants) {
  if (!node) {
    return NULL;
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
                                    instants);
    node_add_child(clone, child);
  }
  if (children != node_get_children(node)) {
    delete_vec(&children);
  }
  return clone;
}

node_t *sema_clone_node_renamed(node_t *node,
                                allocator_t *out,
                                program_t *program,
                                const sema_rename_t *renames,
                                size_t rename_count,
                                vec_t *skip,
                                vec_t *instants) {
  return sema_clone_impl(
      node, out, program, renames, rename_count, skip, instants);
}
