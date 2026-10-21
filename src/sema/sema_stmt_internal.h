#ifndef _H_CLUX_SEMA_STMT_INTERNAL_
#define _H_CLUX_SEMA_STMT_INTERNAL_
/* 内部头：stmt.c 拆分后的跨文件声明（sema 私有，不属于公共 API）。 */
#include "sema/sema.h"
#include "vm/type_struct.h"

#ifdef __cplusplus
extern "C" {
#endif


typedef struct block_result {
  bool definitely_returns; /* 该块保证返回（所有路径都 return） */
} block_result_t;

/* ---- 确定性赋值分析（definite assignment analysis） ---- */

/*
 * 变量初始化状态（flow_init）挂在 sema_symbol_t 上，随 Pass 3b walk 更新：
 *   - var x = expr         → flow_init = true（定义即初始化）
 *   - var x:T = undefined  → flow_init = false（未初始化声明，TDZ）
 *   - x = expr             → flow_init = true（赋值退出未初始化）
 *   - if 合并点            → meet（AND）：两分支都 flow_init 才 true
 *   - while/for            → 循环体可能执行 0 次，体内赋值不提升确定性
 *
 * 分支模拟通过"快照 → 执行 → 恢复"实现：快照收集分支前可见符号的
 * flow_init，分支执行后取 then/else 交集写回（保守策略：非全部分支
 * 赋值 → 仍视为未初始化，读取报编译错误）。
 */

typedef struct flow_snap {
  allocator_t     *alloc;   /* 快照数组分配器（scope 链 alloc，共享 vm alloc） */
  sema_symbol_t  **syms;    /* 分支前可见的变量符号 */
  bool            *before;  /* 分支前 flow_init */
  bool            *after;   /* then 分支后 flow_init（meet 用） */
  size_t           n;
} flow_snap_t;

sema_scope_t *annot_anchor_scope(sema_t *sema, strslice_t name, sema_scope_t *scope);
void borrow_carry_merge(sema_t *sema, sema_symbol_t *carrier, sema_symbol_t **srcs, size_t n);
size_t borrow_collect(sema_t *sema, ast_node_t *e, sema_scope_t *scope, sema_symbol_t **out, size_t cap);
size_t borrow_collect_add(sema_symbol_t **out, size_t n, size_t cap, sema_symbol_t *s);
size_t borrow_collect_value(sema_t *sema, const type_t *t, ownership_t slot_own, ast_node_t *e, sema_scope_t *scope, sema_symbol_t **out, size_t cap, size_t n);
sema_symbol_t *borrow_root_symbol(sema_t *sema, ast_node_t *e, sema_scope_t *scope);
void borrow_srcs_set(sema_t *sema, sema_symbol_t *var, sema_symbol_t **srcs, size_t n);
ast_node_t *collect_free_stmts(sema_t *sema, sema_scope_t *from_scope, const sema_scope_t *stop_scope);
strslice_t discard_name(sema_t *sema, const char *s);
const sema_scope_t *find_func_stop_sema(sema_scope_t *scope);
const sema_scope_t *find_loop_stop_sema(sema_scope_t *scope);
flow_snap_t flow_capture(sema_t *sema, sema_scope_t *scope);
void flow_release(flow_snap_t *snap);
void flow_restore(const flow_snap_t *snap);
bool gen_destroy(sema_t *sema, const type_t *t, ast_node_t *access, ast_node_t **head, ast_node_t **tail);
bool gen_destroy_slot(sema_t *sema, const type_t *slot_type, ownership_t slot_own, ast_node_t *access, ast_node_t **head, ast_node_t **tail);
ast_node_t *make_free_stmts(sema_t *sema, sema_scope_t *scope);
void name_to_cstr(strslice_t s, char *buf, size_t cap);
void shadow_assign(sema_t *sema, ast_assign_t *as, sema_scope_t *scope);
void shadow_borrow_register(sema_t *sema, sema_symbol_t *sym, ast_node_t *init, sema_scope_t *scope);
void shadow_var_def(sema_t *sema, ast_var_def_t *vd, sema_scope_t *scope);
bool type_has_ref_slots(const type_t *t);
block_result_t walk_block(sema_t *sema, ast_node_t *block, sema_scope_t *scope, size_t *idx);
block_result_t walk_dowhile(sema_t *sema, ast_dowhile_t *dw, sema_scope_t *scope, size_t *idx);
block_result_t walk_for(sema_t *sema, ast_for_t *fr, sema_scope_t *scope, size_t *idx);
block_result_t walk_if(sema_t *sema, ast_if_t *it, sema_scope_t *scope, size_t *idx);
block_result_t walk_return(sema_t *sema, ast_return_t *rt, sema_scope_t *scope);
block_result_t walk_stmt(sema_t *sema, ast_node_t *stmt, sema_scope_t *scope, size_t *idx);
block_result_t walk_switch(sema_t *sema, ast_switch_t *sw, sema_scope_t *scope, size_t *idx);
block_result_t walk_while(sema_t *sema, ast_while_t *wl, sema_scope_t *scope, size_t *idx);

/* 守卫：被污染的值（错误已报告）→ 直接返回 void shadow。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

#ifdef __cplusplus
}
#endif
#endif
