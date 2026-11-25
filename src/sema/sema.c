#include "sema/sema.h"
#include "core/location.h"
#include "core/program.h"
#include "engine/diag.h"
#include "engine/module.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_enum_decl.h"
#include "parser/node_func_decl.h"
#include "parser/node_interface_decl.h"
#include "parser/node_program.h"
#include "parser/node_stmt_bitfield.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_struct_decl.h"
#include "parser/node_union.h"
#include "parser/parser.h"

struct _clux_sema_t {
  clux_context_t *ctx;        /* 借用：engine 侧全部状态（scope/module/diag） */
  allocator_t *out_allocator; /* 借用：emit 产物（program_t）的唯一分配来源 */
  clux_module_t *module;      /* 借用：ctx 的 modules 注册表持有 */
  int walked;                 /* 0 未走 / 1 已走（walk 只允许一次） */
};

clux_sema_t *sema_create(clux_context_t *ctx, allocator_t *out_allocator) {
  if (!ctx || !out_allocator) {
    return NULL;
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  clux_sema_t *sema = allocator_alloc(allocator, sizeof(struct _clux_sema_t));
  sema->ctx = ctx;
  sema->out_allocator = out_allocator;
  sema->module = NULL;
  sema->walked = 0;
  return sema;
}

void sema_destroy(clux_sema_t **sema) {
  /* 生命周期链：sema 先于 ctx/engine 销毁（设计文档 §7.3），
     此处借用 ctx 的 allocator 释放本体依然安全。 */
  if (!sema || !*sema) {
    return;
  }
  clux_sema_t *self = *sema;
  allocator_free(clux_context_get_allocator(self->ctx), (void **)sema);
}

bool sema_has_error(const clux_sema_t *sema) {
  return sema ? clux_context_diag_has_error(sema->ctx) : false;
}

/* 顶层声明语句 → 名字节点。Phase 1 覆盖七类具名声明；无名 / 未覆盖
   的顶层语句（test/import/reexport/extern/assert/表达式语句等）暂不
   参与模块级查重，随各变换分期落地。 */
static node_t *sema_stmt_name_node(node_t *stmt) {
  switch (node_get_kind(stmt)) {
  case CLUX_NODE_STMT_VAR:
    return stmt_var_node_get_name(stmt);
  case CLUX_NODE_STMT_FUNC: {
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    return decl ? func_decl_node_get_name(decl) : NULL;
  }
  case CLUX_NODE_STMT_TYPE:
    return stmt_type_node_get_name(stmt);
  case CLUX_NODE_STMT_STRUCT: {
    node_t *decl = stmt_struct_node_get_struct_decl(stmt);
    return decl ? struct_decl_node_get_name(decl) : NULL;
  }
  case CLUX_NODE_STMT_ENUM: {
    node_t *decl = stmt_enum_node_get_enum_decl(stmt);
    return decl ? enum_decl_node_get_name(decl) : NULL;
  }
  case CLUX_NODE_STMT_UNION: {
    node_t *decl = stmt_union_node_get_union_decl(stmt);
    return decl ? union_node_get_name(decl) : NULL;
  }
  case CLUX_NODE_STMT_BITFIELD: {
    node_t *decl = stmt_bitfield_node_get_bitfield_decl(stmt);
    return decl ? bitfield_decl_node_get_name(decl) : NULL;
  }
  case CLUX_NODE_STMT_INTERFACE: {
    node_t *decl = stmt_interface_node_get_interface_decl(stmt);
    return decl ? interface_decl_node_get_name(decl) : NULL;
  }
  default:
    return NULL;
  }
}

int sema_walk(clux_sema_t *sema, node_t *program) {
  if (!sema) {
    return -1;
  }
  if (sema->walked) {
    clux_context_diag(
        sema->ctx, CLUX_DIAG_ERROR, NULL, "sema walk has already completed");
    return -1;
  }
  sema->walked = 1;
  if (!program || node_get_kind(program) != CLUX_NODE_PROGRAM) {
    clux_context_diag(sema->ctx,
                      CLUX_DIAG_ERROR,
                      program ? node_get_location(program) : NULL,
                      "sema walk requires a program node");
    return -1;
  }
  /* 本次 walk 的诊断从空表开始（sema 是 diag 通道的唯一写者）。 */
  clux_context_clear_diags(sema->ctx);
  /* 入口主模块：名字 Phase 1 固定 "main"（import 多模块随后续分期扩展）。
     AST 载荷传 NULL——原 AST 归 parser，所有权保持调用方，
     不随 module 销毁（module 的 node 槽会 delete 载荷，不可挂原树）。 */
  clux_module_t *module = clux_context_create_module(sema->ctx, "main", NULL);
  if (!module) {
    clux_context_diag(
        sema->ctx, CLUX_DIAG_ERROR, NULL, "failed to register the main module");
    return -1;
  }
  sema->module = module;
  clux_context_set_current_module(sema->ctx, module);
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  size_t count = program_node_get_stmt_count(program);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = program_node_get_stmt(program, idx);
    if (!stmt) {
      continue;
    }
    node_t *name_node = sema_stmt_name_node(stmt);
    if (!name_node) {
      continue;
    }
    char *name = location_get(node_get_location(name_node), allocator);
    if (!name) {
      continue;
    }
    /* 逐 stmt 收集：重复登记报 diag 后从下一个 stmt 继续，不快速失败。 */
    if (!clux_module_declare(module, name, false)) {
      clux_context_diag(sema->ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(name_node),
                        "duplicate declaration '%s' in module '%s'",
                        name,
                        clux_module_get_name(module));
    }
    allocator_free(allocator, (void **)&name);
  }
  clux_context_set_current_module(sema->ctx, NULL);
  return 0;
}

vec_t *sema_emit(clux_sema_t *sema) {
  if (!sema || !sema->walked || sema_has_error(sema)) {
    return NULL;
  }
  /* Phase 1 骨架：单主模块，decls 留空——逆向编排的渲染器 / 克隆器
     两条新建通道随 Phase 2+ 落地后在此填充。emit 只读幂等：
     每次调用产出全新集合。 */
  vec_t *out = create_vec(sema->out_allocator);
  program_t *program = create_program(sema->out_allocator);
  program_set_module_name(program, clux_module_get_name(sema->module));
  vec_push(out, program);
  return out;
}
