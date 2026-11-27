#include "sema/sema.h"
#include "core/location.h"
#include "core/program.h"
#include "core/string.h"
#include "engine/diag.h"
#include "engine/module.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_construct.h"
#include "parser/node_construct_field.h"
#include "parser/node_enum_decl.h"
#include "parser/node_field.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_generic_param.h"
#include "parser/node_interface_decl.h"
#include "parser/node_program.h"
#include "parser/node_rest.h"
#include "parser/node_stmt_bitfield.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_block.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_tuple.h"
#include "parser/node_union.h"
#include "parser/parser.h"
#include "sema/sema_internal.h"
#include <stdio.h>
#include <string.h>

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
  sema->hoists = create_vec(allocator);
  sema->instants = create_vec(allocator);
  sema->generic_defs = create_vec(allocator);
  sema->instance_bodies = create_vec(allocator);
  sema->inst_program = NULL;    /* 物化期按需创建（§2.3 P11） */
  sema->inst_defer_counter = 0; /* 物化未发生即 0 起点 */
  sema->const_defs = create_vec(allocator);
  sema->const_uses = create_vec(allocator);
  sema->prunes = create_vec(allocator);
  sema->anons = create_vec(allocator);
  sema->methods = create_vec(allocator);
  sema->type_results = create_vec(allocator);
  sema->ast = NULL;
  sema->return_type = NULL;
  sema->dfa_entries = create_vec(allocator);
  sema->dfa_marks = create_vec(allocator);
  sema->dfa_frames = 0;
  return sema;
}

void sema_destroy(clux_sema_t **sema) {
  /* 生命周期链：sema 先于 ctx/engine 销毁（设计文档 §7.3），
     此处借用 ctx 的 allocator 释放本体依然安全。 */
  if (!sema || !*sema) {
    return;
  }
  clux_sema_t *self = *sema;
  allocator_t *allocator = clux_context_get_allocator(self->ctx);
  for (size_t idx = 0; idx < vec_get_size(self->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(self->hoists, idx);
    allocator_free(allocator, (void **)&hoist->name);
    allocator_free(allocator, (void **)&hoist->mangled);
    allocator_free(allocator, (void **)&hoist);
  }
  delete_vec(&self->hoists);
  for (size_t idx = 0; idx < vec_get_size(self->instants); idx++) {
    sema_instant_t *instant = (sema_instant_t *)vec_get(self->instants, idx);
    allocator_free(allocator, (void **)&instant->name);
    allocator_free(allocator, (void **)&instant);
  }
  delete_vec(&self->instants);
  for (size_t idx = 0; idx < vec_get_size(self->generic_defs); idx++) {
    sema_generic_def_t *def =
        (sema_generic_def_t *)vec_get(self->generic_defs, idx);
    allocator_free(allocator, (void **)&def->name);
    allocator_free(allocator, (void **)&def);
  }
  delete_vec(&self->generic_defs);
  for (size_t idx = 0; idx < vec_get_size(self->instance_bodies); idx++) {
    sema_instance_body_t *body =
        (sema_instance_body_t *)vec_get(self->instance_bodies, idx);
    allocator_free(allocator, (void **)&body->base);
    allocator_free(allocator, (void **)&body->name);
    for (size_t p = 0; p < vec_get_size(body->param_names); p++) {
      char *param = (char *)vec_get(body->param_names, p);
      allocator_free(allocator, (void **)&param);
    }
    delete_vec(&body->param_names);
    delete_vec(&body->args); /* 实参节点借用原树，只释放 vec 外壳（包槽
                                TUPLE 由 pack_arg 独立释放，见下） */
    if (body->pack_arg) {
      delete_node(&body->pack_arg); /* 包槽聚合 TUPLE 归 sema 所有（P13） */
    }
    if (body->inst_stmt) {
      delete_node(&body->inst_stmt); /* 物化体归 sema 所有（§2.3 P11） */
    }
    allocator_free(allocator, (void **)&body);
  }
  delete_vec(&self->instance_bodies);
  if (self->inst_program) {
    /* 文本池只持 intern 字符串表（物化体根不挂 decls），单独释放外壳 */
    delete_program(&self->inst_program);
  }
  for (size_t idx = 0; idx < vec_get_size(self->const_defs); idx++) {
    sema_const_def_t *def = (sema_const_def_t *)vec_get(self->const_defs, idx);
    allocator_free(allocator, (void **)&def->name);
    allocator_free(allocator, (void **)&def);
  }
  delete_vec(&self->const_defs);
  for (size_t idx = 0; idx < vec_get_size(self->const_uses); idx++) {
    sema_const_use_t *use = (sema_const_use_t *)vec_get(self->const_uses, idx);
    allocator_free(allocator, (void **)&use);
  }
  delete_vec(&self->const_uses);
  for (size_t idx = 0; idx < vec_get_size(self->prunes); idx++) {
    sema_prune_t *prune = (sema_prune_t *)vec_get(self->prunes, idx);
    allocator_free(allocator, (void **)&prune);
  }
  delete_vec(&self->prunes);
  for (size_t idx = 0; idx < vec_get_size(self->anons); idx++) {
    sema_anon_t *anon = (sema_anon_t *)vec_get(self->anons, idx);
    allocator_free(allocator, (void **)&anon->name);
    allocator_free(allocator, (void **)&anon);
  }
  delete_vec(&self->anons);
  for (size_t idx = 0; idx < vec_get_size(self->methods); idx++) {
    sema_method_t *method = (sema_method_t *)vec_get(self->methods, idx);
    allocator_free(allocator, (void **)&method->struct_name);
    allocator_free(allocator, (void **)&method->method_name);
    allocator_free(allocator, (void **)&method->mangled);
    allocator_free(allocator, (void **)&method);
  }
  delete_vec(&self->methods);
  /* 类型结果表条目全借用（node 原树、type context 托管），只释放外壳 */
  for (size_t idx = 0; idx < vec_get_size(self->type_results); idx++) {
    sema_type_result_t *entry =
        (sema_type_result_t *)vec_get(self->type_results, idx);
    allocator_free(allocator, (void **)&entry);
  }
  delete_vec(&self->type_results);
  /* DFA 条目（§8.6）：帧正常配对时已随帧清空，防御性兜底释放 */
  for (size_t idx = 0; idx < vec_get_size(self->dfa_entries); idx++) {
    sema_dfa_entry_t *entry =
        (sema_dfa_entry_t *)vec_get(self->dfa_entries, idx);
    allocator_free(allocator, (void **)&entry->name);
    allocator_free(allocator, (void **)&entry);
  }
  delete_vec(&self->dfa_entries);
  delete_vec(&self->dfa_marks);
  allocator_free(allocator, (void **)sema);
}

bool sema_has_error(const clux_sema_t *sema) {
  return sema ? clux_context_diag_has_error(sema->ctx) : false;
}

void sema_record_node_type(clux_sema_t *sema, node_t *node, clux_type_t *type) {
  if (!sema || !node) {
    return;
  }
  for (size_t idx = 0; idx < vec_get_size(sema->type_results); idx++) {
    sema_type_result_t *entry =
        (sema_type_result_t *)vec_get(sema->type_results, idx);
    if (entry->node == node) {
      if (type) {
        entry->type = type;
      }
      return;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  sema_type_result_t *entry =
      allocator_alloc(allocator, sizeof(sema_type_result_t));
  entry->node = node;
  entry->type = type;
  (void)vec_push(sema->type_results, entry);
}

clux_type_t *sema_get_node_type(clux_sema_t *sema, node_t *node) {
  if (!sema || !node) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(sema->type_results); idx++) {
    sema_type_result_t *entry =
        (sema_type_result_t *)vec_get(sema->type_results, idx);
    if (entry->node == node) {
      return entry->type;
    }
  }
  return NULL;
}

/* ---- 声明元数据表（kind 为行、pass 为列） ----
   同一组声明 kind 的元数据查询（名字槽/泛型槽/可提升）收敛为一张
   静态表：新增声明种类只填一行，漏填即查询返回 NULL（与原 switch
   default 等价），不会静默漂移。行 = node kind，列 = 各查询通道；
   后续全节点 walk（类型校验/DFA/CFA）作为新列挂表，不另起 switch。 */
static node_t *sema_func_name_of(node_t *stmt) {
  node_t *decl = stmt_func_node_get_func_decl(stmt);
  return decl ? func_decl_node_get_name(decl) : NULL;
}

static node_t *sema_func_generics_of(node_t *stmt) {
  node_t *decl = stmt_func_node_get_func_decl(stmt);
  return decl ? func_decl_node_get_generics(decl) : NULL;
}

static node_t *sema_struct_name_of(node_t *stmt) {
  node_t *decl = stmt_struct_node_get_struct_decl(stmt);
  return decl ? struct_decl_node_get_name(decl) : NULL;
}

static node_t *sema_struct_generics_of(node_t *stmt) {
  node_t *decl = stmt_struct_node_get_struct_decl(stmt);
  return decl ? struct_decl_node_get_generics(decl) : NULL;
}

static node_t *sema_enum_name_of(node_t *stmt) {
  node_t *decl = stmt_enum_node_get_enum_decl(stmt);
  return decl ? enum_decl_node_get_name(decl) : NULL;
}

static node_t *sema_union_name_of(node_t *stmt) {
  node_t *decl = stmt_union_node_get_union_decl(stmt);
  return decl ? union_node_get_name(decl) : NULL;
}

static node_t *sema_union_generics_of(node_t *stmt) {
  node_t *decl = stmt_union_node_get_union_decl(stmt);
  return decl ? union_node_get_generics(decl) : NULL;
}

static node_t *sema_bitfield_name_of(node_t *stmt) {
  node_t *decl = stmt_bitfield_node_get_bitfield_decl(stmt);
  return decl ? bitfield_decl_node_get_name(decl) : NULL;
}

static node_t *sema_interface_name_of(node_t *stmt) {
  node_t *decl = stmt_interface_node_get_interface_decl(stmt);
  return decl ? interface_decl_node_get_name(decl) : NULL;
}

/* 泛型 interface 的 GENERICS 槽（§8.9 P17）：非空时走模板物化通道。 */
static node_t *sema_interface_generics_of(node_t *stmt) {
  node_t *decl = stmt_interface_node_get_interface_decl(stmt);
  return decl ? interface_decl_node_get_generics(decl) : NULL;
}

typedef struct {
  node_t *(*name_of)(node_t *stmt);
  node_t *(*generics_of)(node_t *stmt);
  int hoistable;
} sema_decl_meta_t;

/* VAR 是声明但不提升（原位置克隆）、无泛型，仅挂名通道。 */
static const sema_decl_meta_t g_sema_decl_meta[] = {
    [CLUX_NODE_STMT_VAR] = {stmt_var_node_get_name, NULL, 0},
    [CLUX_NODE_STMT_TYPE] = {stmt_type_node_get_name,
                             stmt_type_node_get_generics,
                             1},
    [CLUX_NODE_STMT_FUNC] = {sema_func_name_of, sema_func_generics_of, 1},
    [CLUX_NODE_STMT_STRUCT] = {sema_struct_name_of, sema_struct_generics_of, 1},
    [CLUX_NODE_STMT_ENUM] = {sema_enum_name_of, NULL, 1},
    [CLUX_NODE_STMT_UNION] = {sema_union_name_of, sema_union_generics_of, 1},
    [CLUX_NODE_STMT_BITFIELD] = {sema_bitfield_name_of, NULL, 1},
    [CLUX_NODE_STMT_INTERFACE] = {sema_interface_name_of,
                                  sema_interface_generics_of,
                                  1},
};

/* 越界 / 未登记 kind 返回 NULL 行（等价于原 switch default）。 */
static const sema_decl_meta_t *sema_decl_meta_of(node_t *stmt) {
  node_kind_t kind = node_get_kind(stmt);
  if ((size_t)kind >= sizeof(g_sema_decl_meta) / sizeof(g_sema_decl_meta[0])) {
    return NULL;
  }
  return g_sema_decl_meta[kind].name_of ? &g_sema_decl_meta[kind] : NULL;
}

/* 顶层声明语句 → 名字节点。Phase 1 覆盖七类具名声明；无名 / 未覆盖
   的顶层语句（test/import/reexport/extern/assert/表达式语句等）暂不
   参与模块级查重，随各变换分期落地。 */
static node_t *sema_stmt_name_node(node_t *stmt) {
  const sema_decl_meta_t *meta = sema_decl_meta_of(stmt);
  return meta && meta->name_of ? meta->name_of(stmt) : NULL;
}

/* 提升（§2.2）：函数体内声明的类型/函数登记为待提升项，mangled 名
   = enclosing链$原名（最近具名函数边界，嵌套递归拼接）。var 不提升
   （仍在原位置克隆）。 */
static int sema_stmt_is_hoistable(node_t *stmt) {
  const sema_decl_meta_t *meta = sema_decl_meta_of(stmt);
  return meta ? meta->hoistable : 0;
}

/* 登记：同函数内同名（作用域遮蔽）Phase 2 不支持，直接 diag；
   与模块级名冲突沿用 duplicate declaration 语义。 */
static void sema_hoist_register(clux_sema_t *sema,
                                node_t *stmt,
                                node_t *owner,
                                const char *prefix,
                                node_t *name_node) {
  clux_module_t *module = sema->module;
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  if (!name) {
    return;
  }
  /* mangled = prefix$（prefix 为外层 enclosing 链，顶层函数自己不进表）。*/
  string_t *joined = create_string(allocator);
  string_append_cstr(joined, prefix);
  string_append_cstr(joined, "$");
  string_append_cstr(joined, name);
  const char *data = string_get_data(joined);
  size_t len = string_get_size(joined);
  char *mangled = allocator_alloc(allocator, len + 1);
  memcpy(mangled, data, len + 1);
  delete_string(&joined);
  /* 同函数同名：遮蔽提升不支持（精确作用域随后续分期落地）。*/
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    if (hoist->owner == owner && strcmp(hoist->name, name) == 0) {
      clux_context_diag(sema->ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(name_node),
                        "ambiguous local declaration '%s' in module '%s': "
                        "hoisting shadowed names is not supported yet",
                        name,
                        clux_module_get_name(module));
      allocator_free(allocator, (void **)&name);
      allocator_free(allocator, (void **)&mangled);
      return;
    }
  }
  /* 合成名进模块符号表参与查重（与用户名冲突即 $ 编码天然隔离）。*/
  if (!clux_module_declare(module, mangled, false)) {
    clux_context_diag(sema->ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(name_node),
                      "duplicate declaration '%s' in module '%s'",
                      mangled,
                      clux_module_get_name(module));
    allocator_free(allocator, (void **)&name);
    allocator_free(allocator, (void **)&mangled);
    return;
  }
  sema_hoist_t *hoist = allocator_alloc(allocator, sizeof(sema_hoist_t));
  hoist->stmt = stmt;
  hoist->owner = owner;
  hoist->name = name;
  hoist->mangled = mangled;
  vec_push(sema->hoists, hoist);
}

/* 语句流递归收集：声明语句登记后（STMT_FUNC 换 prefix 继续进体），
   其余语句沿全部 children 递归——块/分支/闭包体内的声明同属
   当前 enclosing 函数边界。 */
static void sema_hoist_block(clux_sema_t *sema,
                             node_t *block,
                             node_t *owner,
                             const char *prefix);
static void sema_hoist_stmt(clux_sema_t *sema,
                            node_t *stmt,
                            node_t *owner,
                            const char *prefix) {
  if (!stmt) {
    return;
  }
  if (sema_stmt_is_hoistable(stmt)) {
    node_t *name_node = sema_stmt_name_node(stmt);
    if (!name_node) {
      return;
    }
    sema_hoist_register(sema, stmt, owner, prefix, name_node);
    if (node_get_kind(stmt) == CLUX_NODE_STMT_FUNC) {
      node_t *decl = stmt_func_node_get_func_decl(stmt);
      node_t *body = decl ? func_decl_node_get_body(decl) : NULL;
      if (body) {
        /* 嵌套函数体沿用新的 enclosing 链（最近具名函数边界）。*/
        char *inner_prefix = NULL;
        sema_hoist_t *entry =
            vec_get_size(sema->hoists) > 0
                ? (sema_hoist_t *)vec_get(sema->hoists,
                                          vec_get_size(sema->hoists) - 1)
                : NULL;
        if (entry && entry->stmt == stmt) {
          inner_prefix = entry->mangled;
        }
        if (inner_prefix) {
          sema_hoist_block(sema, body, owner, inner_prefix);
        }
      }
    }
    return;
  }
  vec_t *children = node_get_children(stmt);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    sema_hoist_stmt(sema, (node_t *)vec_get(children, idx), owner, prefix);
  }
}

static void sema_hoist_block(clux_sema_t *sema,
                             node_t *block,
                             node_t *owner,
                             const char *prefix) {
  if (!block || node_get_kind(block) != CLUX_NODE_STMT_BLOCK) {
    return;
  }
  size_t count = stmt_block_node_get_stmt_count(block);
  for (size_t idx = 0; idx < count; idx++) {
    sema_hoist_stmt(sema, stmt_block_node_get_stmt(block, idx), owner, prefix);
  }
}

/* ---- 实例化（§2.3 完整版：类型定义） ----
   模块级泛型声明名 → 使用点（EXPR_SUBSCRIPT）合成实例名
   base$arg1$arg2...；module_declare 天然去重（同组合首次登记成功，
   之后静默）。首登组合同步登记实例体条目：emit 从定义语句深拷贝 +
   形参代换产出实例声明，追加 PROGRAM 尾部，泛型定义本身不进输出。 */

/* 语句 → 泛型参数表节点（TYPE/STRUCT/UNION；func/interface 的
   实例化随后续分期）。无泛型返回 NULL。 */
/* 按 node kind 分派取泛型参数表（P12 起 check 侧约束检查复用）。 */
node_t *sema_stmt_generics_node(node_t *stmt) {
  const sema_decl_meta_t *meta = sema_decl_meta_of(stmt);
  return meta && meta->generics_of ? meta->generics_of(stmt) : NULL;
}

/* 名字形态：携带文本的无孩子叶子（用户名/类型引用 LITERAL_IDENTIFIER，
   关键字 SYMBOL 一并覆盖仅为统一）。 */
static int sema_node_is_name_form(node_t *node) {
  return node && vec_get_size(node_get_children(node)) == 0 &&
         (node_get_kind(node) == CLUX_NODE_SYMBOL ||
          node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER);
}

/* 带非空泛型参数表的声明语句 = 泛型定义（§2.3）。 */
static int sema_stmt_is_generic_def(node_t *stmt) {
  node_t *generics = sema_stmt_generics_node(stmt);
  return generics && vec_get_size(node_get_children(generics)) > 0;
}

/* 带泛型表的声明 → 泛型定义条目（名+语句，去重后入表）。
   收集的是声明名（如 List），不是泛型参数名（T）。 */
static void sema_generic_names_collect(clux_sema_t *sema, node_t *stmt) {
  node_t *generics = sema_stmt_generics_node(stmt);
  node_t *name_node = sema_stmt_name_node(stmt);
  if (!generics || vec_get_size(node_get_children(generics)) == 0 ||
      !sema_node_is_name_form(name_node)) {
    return;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  if (!name) {
    return;
  }
  for (size_t idx = 0; idx < vec_get_size(sema->generic_defs); idx++) {
    sema_generic_def_t *def =
        (sema_generic_def_t *)vec_get(sema->generic_defs, idx);
    if (strcmp(def->name, name) == 0) {
      allocator_free(allocator, (void **)&name);
      return;
    }
  }
  sema_generic_def_t *def =
      allocator_alloc(allocator, sizeof(sema_generic_def_t));
  def->name = name;
  def->stmt = stmt;
  vec_push(sema->generic_defs, def);
}

static int sema_name_is_generic(clux_sema_t *sema, const char *name) {
  for (size_t idx = 0; idx < vec_get_size(sema->generic_defs); idx++) {
    sema_generic_def_t *def =
        (sema_generic_def_t *)vec_get(sema->generic_defs, idx);
    if (strcmp(def->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* 泛型形参名字文本：GENERIC_PARAM 取 NAME 槽；尾部 rest（...Args，§6.4）
   取操作数名字。无名/非名字形态返回 NULL（不参与代换）。 */
static char *sema_generic_param_name(allocator_t *allocator, node_t *param) {
  node_t *name = NULL;
  if (node_get_kind(param) == CLUX_NODE_GENERIC_PARAM) {
    name = generic_param_node_get_name(param);
  } else if (node_get_kind(param) == CLUX_NODE_EXPR_REST) {
    name = rest_node_get_expr(param);
  }
  return sema_node_is_name_form(name)
             ? location_get(node_get_location(name), allocator)
             : NULL;
}

/* 拼名文本段：名字形态取源文本（省略槽与显式同文本折叠为同一实例名），
   复合形态记 '?'。 */
static void sema_instant_append_arg_name(string_t *joined,
                                         allocator_t *allocator,
                                         node_t *node) {
  string_append_cstr(joined, "$");
  char *text = sema_node_is_name_form(node)
                   ? location_get(node_get_location(node), allocator)
                   : NULL;
  if (text) {
    string_append_cstr(joined, text);
    allocator_free(allocator, (void **)&text);
  } else {
    string_append_cstr(joined, "?");
  }
}

/* 登记：使用点 + base 名 → 实例名 base$arg1$...（§6.2 省略槽并入默认值
   文本、§6.4 包吸收剩余实参逐个拼接）。args 已由调用方验证全部名字
   形态。对齐校验：positional 段缺参逐尾部查 DEFAULT 槽补齐，无默认报
   "missing generic argument"（对齐 exec generic.c）；无包多给保留旧
   计数消息。module_declare 失败=同组合已登记，静默去重；首登成功同步
   登记实例体（§2.3 完整版）。instants 是按使用点的表，每个使用点各
   一条（emit 按原树 node 指针匹配折叠）。 */
static void sema_instant_register(clux_sema_t *sema,
                                  node_t *use,
                                  node_t *base,
                                  const char *base_name) {
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  sema_generic_def_t *def = NULL;
  for (size_t idx = 0; idx < vec_get_size(sema->generic_defs); idx++) {
    sema_generic_def_t *cur =
        (sema_generic_def_t *)vec_get(sema->generic_defs, idx);
    if (strcmp(cur->name, base_name) == 0) {
      def = cur;
      break;
    }
  }
  if (!def) {
    return; /* 防御：使用点收集时已验证泛型名 */
  }
  node_t *generics = sema_stmt_generics_node(def->stmt);
  vec_t *params = generics ? node_get_children(generics) : NULL;
  size_t param_count = params ? vec_get_size(params) : 0;
  /* 尾部 rest = 参数包：吸收剩余实参（§6.4）。 */
  size_t pack_pos = (size_t)-1;
  if (param_count && node_get_kind((node_t *)vec_get(
                         params, param_count - 1)) == CLUX_NODE_EXPR_REST) {
    pack_pos = param_count - 1;
  }
  size_t positional = pack_pos != (size_t)-1 ? pack_pos : param_count;
  vec_t *children = node_get_children(use);
  size_t arg_count = vec_get_size(children) - 1;
  if (arg_count > positional && pack_pos == (size_t)-1) {
    clux_context_diag(sema->ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(use),
                      "generic '%s' expects %zu type arguments, got %zu",
                      base_name,
                      param_count,
                      arg_count);
    return;
  }
  if (arg_count < positional) {
    /* 缺参逐尾部查 DEFAULT 槽（§6.2 默认值从右向左省略）。 */
    for (size_t idx = arg_count; idx < positional; idx++) {
      node_t *param = (node_t *)vec_get(params, idx);
      if (node_get_kind(param) != CLUX_NODE_GENERIC_PARAM ||
          !generic_param_node_get_default(param)) {
        char *pname = sema_generic_param_name(allocator, param);
        clux_context_diag(sema->ctx,
                          CLUX_DIAG_ERROR,
                          node_get_location(use),
                          "missing generic argument '%s'",
                          pname ? pname : base_name);
        allocator_free(allocator, (void **)&pname);
        return;
      }
    }
  }
  /* 拼名：positional 段（显式实参/省略槽默认值文本）+ 包段（剩余实参
     逐个拼接）——省略形态与显式形态折叠到同一实例名。 */
  string_t *joined = create_string(allocator);
  string_append_cstr(joined, base_name);
  for (size_t idx = 0; idx < positional; idx++) {
    node_t *arg =
        idx < arg_count
            ? (node_t *)vec_get(children, idx + 1)
            : generic_param_node_get_default((node_t *)vec_get(params, idx));
    sema_instant_append_arg_name(joined, allocator, arg);
  }
  for (size_t idx = positional; idx < arg_count; idx++) {
    sema_instant_append_arg_name(
        joined, allocator, (node_t *)vec_get(children, idx + 1));
  }
  const char *data = string_get_data(joined);
  size_t len = string_get_size(joined);
  char *name = allocator_alloc(allocator, len + 1);
  memcpy(name, data, len + 1);
  delete_string(&joined);
  /* 符号表登记（同组合第二次 declare 失败即去重，静默）；首登成功
     同步登记实例体——positional 段缺参以 DEFAULT 借用节点补位，包槽
     放聚合 TUPLE（元素为剩余实参深拷贝叶子，body 持有；物化期 ...Args
     代换为 ...<e1,e2,...>，exec 层 exec_pack_tuple_type 天然支持）。 */
  if (clux_module_declare(sema->module, name, false)) {
    sema_instance_body_t *body =
        allocator_alloc(allocator, sizeof(sema_instance_body_t));
    body->stmt = def->stmt;
    body->base = location_get(node_get_location((node_t *)vec_get(children, 0)),
                              allocator);
    body->name = allocator_alloc(allocator, len + 1);
    memcpy(body->name, name, len + 1);
    body->param_names = create_vec(allocator);
    body->args = create_vec(allocator);
    body->pack_arg = NULL;
    for (size_t idx = 0; idx < positional; idx++) {
      node_t *param = (node_t *)vec_get(params, idx);
      vec_push(body->param_names, sema_generic_param_name(allocator, param));
      vec_push(body->args,
               idx < arg_count ? (node_t *)vec_get(children, idx + 1)
                               : generic_param_node_get_default(param));
    }
    if (pack_pos != (size_t)-1) {
      vec_push(body->param_names,
               sema_generic_param_name(allocator,
                                       (node_t *)vec_get(params, pack_pos)));
      body->pack_arg = create_tuple_node(allocator);
      for (size_t idx = positional; idx < arg_count; idx++) {
        /* 实参收口为名字形态叶子（收集期已验证）：按 kind+location 复制
           新叶挂入 TUPLE——delete_node 递归释放子节点，不可借原树。 */
        node_t *arg = (node_t *)vec_get(children, idx + 1);
        node_t *elem = create_node(allocator, node_get_kind(arg));
        node_set_location(elem, node_get_location(arg));
        tuple_node_add_elem(body->pack_arg, elem);
      }
      vec_push(body->args, body->pack_arg);
    }
    body->inst_stmt = NULL;
    vec_push(sema->instance_bodies, body);
  }
  sema_instant_t *instant = allocator_alloc(allocator, sizeof(sema_instant_t));
  instant->node = use;
  instant->name = name;
  instant->base_loc = *node_get_location(base);
  vec_push(sema->instants, instant);
}

/* 使用点递归收集：SUBSCRIPT 的 base 是泛型名且全部实参名字形态
   → 登记后整棵子树不再递归（emit 折叠命中即止）；否则正常递归
   ——嵌套使用点（List[List[i32]]）与复合实参（W[*i32]）的内层
   继续收集。 */
static void sema_instant_collect(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return;
  }
  if (node_get_kind(node) == CLUX_NODE_EXPR_SUBSCRIPT) {
    vec_t *children = node_get_children(node);
    node_t *base =
        vec_get_size(children) > 0 ? (node_t *)vec_get(children, 0) : NULL;
    allocator_t *allocator = clux_context_get_allocator(sema->ctx);
    char *base_name = sema_node_is_name_form(base)
                          ? location_get(node_get_location(base), allocator)
                          : NULL;
    if (base_name && sema_name_is_generic(sema, base_name)) {
      size_t count = vec_get_size(children);
      size_t idx = 1;
      for (; idx < count; idx++) {
        if (!sema_node_is_name_form((node_t *)vec_get(children, idx))) {
          break;
        }
      }
      if (idx == count) {
        sema_instant_register(sema, node, base, base_name);
        allocator_free(allocator, (void **)&base_name);
        return;
      }
    }
    if (base_name) {
      allocator_free(allocator, (void **)&base_name);
    }
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    sema_instant_collect(sema, (node_t *)vec_get(children, idx));
  }
}

/* ---- 常量回填（§2.1 最小版：顶层 comptime var） ----
   按源码序对每个顶层 comptime var 做常量表达式直推求值入常量表
   （后定义可引用先定义），随后全树收集名字命中常量表的引用点
   （per-use-site 登记，定义语句自身在 emit 摘除，登记无害）。 */

/* STMT_VAR 的 COMPTIME 槽非 NULL 即 comptime var。 */
static int sema_stmt_is_comptime_var(node_t *stmt) {
  return stmt && node_get_kind(stmt) == CLUX_NODE_STMT_VAR &&
         stmt_var_node_get_comptime(stmt) != NULL;
}

static void
sema_const_defs_collect(clux_sema_t *sema, size_t count, node_t *program) {
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = program_node_get_stmt(program, idx);
    if (!sema_stmt_is_comptime_var(stmt)) {
      continue;
    }
    node_t *name_node = stmt_var_node_get_name(stmt);
    node_t *value = stmt_var_node_get_value(stmt);
    if (!sema_node_is_name_form(name_node)) {
      continue;
    }
    char *name = location_get(node_get_location(name_node), allocator);
    if (!name) {
      continue;
    }
    sema_const_def_t *def =
        allocator_alloc(allocator, sizeof(sema_const_def_t));
    def->stmt = stmt;
    def->name = name;
    if (!value ||
        sema_ctfe_eval(sema->ctx, sema->const_defs, value, &def->value) != 0) {
      if (!value) {
        clux_context_diag(sema->ctx,
                          CLUX_DIAG_ERROR,
                          node_get_location(stmt),
                          "comptime variable '%s' requires an initializer",
                          name);
      }
      /* 求值失败：diag 已由 ctfe 报，条目不入表（emit 被 has_error 阻断）。*/
      allocator_free(allocator, (void **)&def->name);
      allocator_free(allocator, (void **)&def);
      continue;
    }
    vec_push(sema->const_defs, def);
  }
}

/* 引用点收集：LITERAL_IDENTIFIER 文本命中常量表 → 逐使用点登记
   （同名每个引用点各一条，emit 按原树 node 指针匹配折叠）。 */
static void sema_const_use_collect(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return;
  }
  if (vec_get_size(sema->const_defs) > 0 &&
      node_get_kind(node) == CLUX_NODE_LITERAL_IDENTIFIER) {
    allocator_t *allocator = clux_context_get_allocator(sema->ctx);
    char *name = location_get(node_get_location(node), allocator);
    if (name) {
      for (size_t idx = 0; idx < vec_get_size(sema->const_defs); idx++) {
        sema_const_def_t *def =
            (sema_const_def_t *)vec_get(sema->const_defs, idx);
        if (strcmp(def->name, name) != 0) {
          continue;
        }
        sema_const_use_t *use =
            allocator_alloc(allocator, sizeof(sema_const_use_t));
        use->node = node;
        use->def = def;
        vec_push(sema->const_uses, use);
      }
      allocator_free(allocator, (void **)&name);
    }
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    sema_const_use_collect(sema, (node_t *)vec_get(children, idx));
  }
}

/* ---- Phase 4 决策收集（§2.5 剪枝 + §2.6 校验，walk 期） ---- */

/* CTFE 结果判真：INT magnitude 恒非负存储，非零即真；FLOAT 按数值，
   BOOL 按布尔值。 */
static int sema_const_truthy(const sema_const_t *value) {
  switch (value->kind) {
  case SEMA_CONST_BOOL:
    return value->bool_value != 0;
  case SEMA_CONST_INT:
    return value->magnitude != 0;
  case SEMA_CONST_FLOAT:
    return value->float_value != 0.0;
  }
  return 0;
}

/* comptime if 剪枝决策（§2.5）：条件 CTFE 求值 → prunes 表（选中臂源
   指针，假臂无 else 记 NULL=整体摘除）；选中臂内嵌套 comptime if 递归
   决策（else-if 链经 ELSE 槽的嵌套 COMPTIME_IF 一并命中）。条件不可
   求值 = 硬错误（ctfe 已报 ERROR diag，不登记，emit 被 has_error 阻断），
   未选臂整棵丢弃无需决策。 */
static void sema_prune_collect(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return;
  }
  if (node_get_kind(node) == CLUX_NODE_STMT_COMPTIME_IF) {
    node_t *cond = stmt_if_node_get_condition(node);
    sema_const_t value;
    if (cond &&
        sema_ctfe_eval(sema->ctx, sema->const_defs, cond, &value) == 0) {
      allocator_t *allocator = clux_context_get_allocator(sema->ctx);
      sema_prune_t *prune = allocator_alloc(allocator, sizeof(sema_prune_t));
      prune->stmt = node;
      prune->arm = sema_const_truthy(&value) ? stmt_if_node_get_then(node)
                                             : stmt_if_node_get_else(node);
      vec_push(sema->prunes, prune);
      sema_prune_collect(sema, prune->arm);
    }
    return;
  }
  /* §2.6 可展开性校验：iterable 必须元组字面、迭代变量必须名字形态，
     不可展开 = 硬错误（emit 被 has_error 阻断）。 */
  if (node_get_kind(node) == CLUX_NODE_STMT_COMPTIME_FOREACH) {
    node_t *iterable = stmt_foreach_node_get_iterable(node);
    if (!iterable || node_get_kind(iterable) != CLUX_NODE_EXPR_TUPLE) {
      clux_context_diag(sema->ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "comptime foreach requires a tuple literal iterable");
    } else if (!sema_node_is_name_form(stmt_foreach_node_get_name(node))) {
      clux_context_diag(sema->ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "comptime foreach requires an identifier iteration "
                        "variable");
    }
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    sema_prune_collect(sema, (node_t *)vec_get(children, idx));
  }
}

/* ---- 匿名构造具名化（§2.9，walk 收集） ----
   整树递归收集 TYPE 槽为 NULL 的 EXPR_CONSTRUCT：按发现序登记
   nonamed$N（计数器模块内唯一，$ 专属编译器合成与用户名天然隔离），
   符号表登记参与查重；元素内嵌套的匿名构造继续各自登记。 */

static void sema_anon_collect(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return;
  }
  if (node_get_kind(node) == CLUX_NODE_EXPR_CONSTRUCT &&
      construct_node_get_type(node) == NULL) {
    allocator_t *allocator = clux_context_get_allocator(sema->ctx);
    char buf[32];
    int written = snprintf(
        buf, sizeof(buf), "nonamed$%zu", vec_get_size(sema->anons) + 1);
    if (written > 0 && (size_t)written < sizeof(buf)) {
      char *name = allocator_alloc(allocator, (size_t)written + 1);
      memcpy(name, buf, (size_t)written + 1);
      clux_module_declare(sema->module, name, false);
      sema_anon_t *anon = allocator_alloc(allocator, sizeof(sema_anon_t));
      anon->node = node;
      anon->name = name;
      vec_push(sema->anons, anon);
    }
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    sema_anon_collect(sema, (node_t *)vec_get(children, idx));
  }
}

/* ---- struct 成员退化（§2.10，walk 收集） ----
   非泛型 struct 的 STMT_FUNC/STMT_VAR 成员提升为模块级 Type$name：
   提升表负责 emit 重编排（成员从 struct_members 摘除、尾部追加声明），
   方法表负责 member_call 退化改写（Type$name(receiver, ...)）。首形参
   为 FUNC_PARAM 的 func 成员登记方法条目，self 形参类型为指针形态
   （*T 文本）时 receiver 需包 .&。泛型 struct 成员不提升——实例体
   克隆与成员提升的组合随后续分期。 */
static void
sema_struct_member_hoist(clux_sema_t *sema, node_t *stmt, const char *prefix) {
  node_t *decl = stmt_struct_node_get_struct_decl(stmt);
  node_t *body = decl ? struct_decl_node_get_body(decl) : NULL;
  if (!body) {
    return;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  size_t count = struct_members_node_get_member_count(body);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *member = struct_members_node_get_member(body, idx);
    if (!member) {
      continue;
    }
    node_kind_t mkind = node_get_kind(member);
    if (mkind != CLUX_NODE_STMT_FUNC && mkind != CLUX_NODE_STMT_VAR) {
      continue;
    }
    node_t *name_node = sema_stmt_name_node(member);
    if (!sema_node_is_name_form(name_node)) {
      continue;
    }
    sema_hoist_register(sema, member, stmt, prefix, name_node);
    if (mkind != CLUX_NODE_STMT_FUNC) {
      continue;
    }
    /* 登记失败（查重 diag 已报）→ 提升表无此条目，方法条目不入表，
       与提升保持同一可见性。 */
    size_t hoist_count = vec_get_size(sema->hoists);
    sema_hoist_t *hoisted =
        hoist_count > 0 ? (sema_hoist_t *)vec_get(sema->hoists, hoist_count - 1)
                        : NULL;
    if (!hoisted || hoisted->stmt != member) {
      continue;
    }
    node_t *func_decl = stmt_func_node_get_func_decl(member);
    node_t *params = func_decl ? func_decl_node_get_params(func_decl) : NULL;
    node_t *first = params && func_params_node_get_param_count(params) > 0
                        ? func_params_node_get_param(params, 0)
                        : NULL;
    /* 无形参 / rest 形态不是方法（无 self 接收者），只提升不改写。 */
    if (!first || node_get_kind(first) != CLUX_NODE_FUNC_PARAM) {
      continue;
    }
    node_t *self_type = func_param_node_get_type(first);
    char *type_text =
        self_type ? location_get(node_get_location(self_type), allocator)
                  : NULL;
    sema_method_t *method = allocator_alloc(allocator, sizeof(sema_method_t));
    method->method_name = location_get(node_get_location(name_node), allocator);
    if (!method->method_name) {
      allocator_free(allocator, (void **)&method);
      continue;
    }
    size_t len = strlen(prefix);
    method->struct_name = allocator_alloc(allocator, len + 1);
    memcpy(method->struct_name, prefix, len + 1);
    len = strlen(hoisted->mangled);
    method->mangled = allocator_alloc(allocator, len + 1);
    memcpy(method->mangled, hoisted->mangled, len + 1);
    method->self_ptr = type_text && type_text[0] == '*';
    allocator_free(allocator, (void **)&type_text);
    vec_push(sema->methods, method);
  }
}

/* §2.3 P11 物化：walk 尾部（anons 收集后、check pass 前）对每个实例体
   做一次 inst 态克隆，产出物化体 inst_stmt——check pass 当普通声明检查
   （§8.7 泛型体检查），emit 尾部对其只做普通克隆，实例化与 emit 解耦。
   共享同一 clone env：defer_counter 跨实例体连续（消耗值记入
   inst_defer_counter 供 emit 预置起点）；instants/consts/prunes/anons/
   methods 全表注入，与旧 emit 尾部 inst 态克隆行为等价（定义体内引用
   点照常折叠/剪枝/退化）。新造文本 intern 进 sema 持有的 inst_program，
   物化体与后续 emit 产物的实例名 location 借其缓冲。 */
static void sema_instance_bodies_materialize(clux_sema_t *sema) {
  size_t total = vec_get_size(sema->instance_bodies);
  if (total == 0) {
    return;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  sema->inst_program = create_program(allocator);
  sema_clone_env_t env;
  sema_clone_env_init(&env,
                      allocator,
                      sema->inst_program,
                      sema->instants,
                      sema->const_uses,
                      sema->prunes,
                      sema);
  env.anons = sema->anons;
  env.methods = sema->methods;
  for (size_t idx = 0; idx < total; idx++) {
    sema_instance_body_t *body =
        (sema_instance_body_t *)vec_get(sema->instance_bodies, idx);
    sema_rename_t inst_rename;
    inst_rename.from = body->base;
    inst_rename.to = body->name;
    env.renames = &inst_rename;
    env.rename_count = 1;
    env.inst_params = body->param_names;
    env.inst_args = body->args;
    env.instantiate = 1;
    body->inst_stmt = sema_clone_node_renamed(body->stmt, &env);
    env.renames = NULL;
    env.rename_count = 0;
    env.inst_params = NULL;
    env.inst_args = NULL;
    env.instantiate = 0;
  }
  sema->inst_defer_counter = env.defer_counter;
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
  sema->ast = program;
  clux_context_set_current_module(sema->ctx, module);
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  size_t count = program_node_get_stmt_count(program);
  /* 实例化名回填（§2.3）：泛型名集合必须先收集完整，使用点才能匹配。 */
  for (size_t idx = 0; idx < count; idx++) {
    sema_generic_names_collect(sema, program_node_get_stmt(program, idx));
  }
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
    /* 提升（§2.2）：顶层函数体内的局部类型/函数在 walk 阶段收集，
       emit 时重编排（enclosing 链即函数名）。顶层函数自身是模块级
       声明不提升，只作 enclosing 边界——直接从其 body 进入收集。 */
    if (node_get_kind(stmt) == CLUX_NODE_STMT_FUNC) {
      node_t *decl = stmt_func_node_get_func_decl(stmt);
      node_t *body = decl ? func_decl_node_get_body(decl) : NULL;
      if (body) {
        sema_hoist_block(sema, body, stmt, name);
      }
    }
    /* §2.10：struct 成员提升 + 方法表收集（泛型 struct 跳过）。 */
    if (node_get_kind(stmt) == CLUX_NODE_STMT_STRUCT &&
        !sema_stmt_is_generic_def(stmt)) {
      sema_struct_member_hoist(sema, stmt, name);
    }
    allocator_free(allocator, (void **)&name);
  }
  /* §2.3：泛型定义体内不收使用点——体内形参文本（如 Node[T] 的 T）
     不是真实实例组合，真正的代换发生在实例体克隆期；定义体外的
     使用点才登记实例。 */
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = program_node_get_stmt(program, idx);
    if (sema_stmt_is_generic_def(stmt)) {
      continue;
    }
    sema_instant_collect(sema, stmt);
  }
  /* 常量回填（§2.1）：先按源码序求值成表，才能收集引用点
     （含函数体内的引用——整棵顶层语句树递归）。 */
  sema_const_defs_collect(sema, count, program);
  for (size_t idx = 0; idx < count; idx++) {
    sema_const_use_collect(sema, program_node_get_stmt(program, idx));
  }
  /* Phase 4 决策收集（§2.5/§2.6）：comptime if 剪枝决策 + comptime
     foreach 可展开性校验（整棵顶层语句树递归）。 */
  for (size_t idx = 0; idx < count; idx++) {
    sema_prune_collect(sema, program_node_get_stmt(program, idx));
  }
  /* 匿名构造具名化（§2.9）：整树收集匿名构造点。泛型定义体内不收
     ——体内字段可能引用泛型形参，实例体克隆期的合成随后续分期。 */
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = program_node_get_stmt(program, idx);
    if (sema_stmt_is_generic_def(stmt)) {
      continue;
    }
    sema_anon_collect(sema, stmt);
  }
  /* §2.3 P11 物化：实例体在 check pass 前落成——check 把物化体当普通
     声明检查（§8.7 泛型体检查随之免费获得），emit 只做普通克隆。 */
  sema_instance_bodies_materialize(sema);
  /* 类型环境 + 名字解析（§8.8 P7）：诊断全收集不 fast-fail，仍在
     current module 期内——模块级登记需要 current scope = module scope。 */
  sema_check_pass(sema);
  clux_context_set_current_module(sema->ctx, NULL);
  return 0;
}

/* emit 侧：owner 函数的提升项 → rename 表（from=原名，to=提升名）。 */
static size_t sema_emit_renames_for(clux_sema_t *sema,
                                    node_t *owner,
                                    sema_rename_t *out,
                                    size_t capacity) {
  size_t count = 0;
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    if (hoist->owner != owner || count >= capacity) {
      continue;
    }
    out[count].from = hoist->name;
    out[count].to = hoist->mangled;
    count++;
  }
  return count;
}

/* emit 侧：owner 函数的 skip 集（被提升的原语句，克隆函数体时摘除）。 */
static vec_t *
sema_emit_skip_for(clux_sema_t *sema, node_t *owner, allocator_t *out) {
  vec_t *skip = create_vec(out);
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    if (hoist->owner == owner) {
      vec_push(skip, hoist->stmt);
    }
  }
  return skip;
}

/* emit 侧：声明是否为提升 owner（顶层函数体 / struct 成员），决定
   是否走 rename + skip 重编排通道。 */
static int sema_emit_has_hoists_for(clux_sema_t *sema, node_t *owner) {
  for (size_t idx = 0; idx < vec_get_size(sema->hoists); idx++) {
    if (((sema_hoist_t *)vec_get(sema->hoists, idx))->owner == owner) {
      return 1;
    }
  }
  return 0;
}

/* emit 侧：定义语句已有实例体（§2.3）→ 不进输出（尾部由实例体替代；
   复合实参等不可实例化的定义保持原样）。 */
static int sema_emit_generic_replaced(clux_sema_t *sema, node_t *stmt) {
  for (size_t idx = 0; idx < vec_get_size(sema->instance_bodies); idx++) {
    if (((sema_instance_body_t *)vec_get(sema->instance_bodies, idx))->stmt ==
        stmt) {
      return 1;
    }
  }
  return 0;
}

vec_t *sema_emit(clux_sema_t *sema) {
  if (!sema || !sema->walked || sema_has_error(sema)) {
    return NULL;
  }
  /* 逆向编排（设计文档 §4 ②）：克隆器通道重建全新 PROGRAM——模块级
     声明原序克隆；函数经 rename + skip 重编排（提升声明摘除、引用点
     改提升名、声明点名随 rename 表一并通过 intern 文本改写）；
     提升项本体克隆后按收集序追加到尾部。emit 只读幂等。 */
  allocator_t *out = sema->out_allocator;
  vec_t *programs = create_vec(out);
  program_t *program = create_program(out);
  program_set_module_name(program, clux_module_get_name(sema->module));
  node_t *ast = create_program_node(out);
  size_t hoist_total = vec_get_size(sema->hoists);
  /* 克隆器通道环境：instants/consts/prunes 整个 walk 期收集表一次注入；
     renames/skip 按 stmt 覆写；defer_counter 跨 stmt 单调递增保证
     __deferN 模块内唯一。 */
  sema_clone_env_t env;
  sema_clone_env_init(
      &env, out, program, sema->instants, sema->const_uses, sema->prunes, sema);
  env.anons = sema->anons;
  env.methods = sema->methods;
  /* §2.3 P11：物化期已消耗若干 __deferN 编号（物化体闭包 var 固化名），
     emit 主循环从物化已消耗值之后起计数，防模块内撞名。 */
  env.defer_counter = sema->inst_defer_counter;
  sema_rename_t *renames = NULL;
  vec_t *skip = NULL;
  for (size_t idx = 0; idx < program_node_get_stmt_count(sema->ast); idx++) {
    node_t *stmt = program_node_get_stmt(sema->ast, idx);
    /* comptime var 求值后定义语句摘除（§2.1）：引用点已在克隆时
       折叠为字面量。 */
    if (sema_stmt_is_comptime_var(stmt)) {
      continue;
    }
    /* 顶层 comptime if（§2.5）：克隆器解析选中臂（假臂无 else 返回
       NULL 整条摘除）；臂是块语句，内含声明升为模块级逐条拼接。 */
    if (stmt && node_get_kind(stmt) == CLUX_NODE_STMT_COMPTIME_IF) {
      node_t *resolved = sema_clone_node_renamed(stmt, &env);
      if (resolved) {
        vec_t *arm_children = node_get_children(resolved);
        for (size_t a = 0; a < vec_get_size(arm_children); a++) {
          node_t *arm_stmt = (node_t *)vec_get(arm_children, a);
          if (arm_stmt) {
            program_node_add_stmt(ast, arm_stmt);
          }
        }
        /* 臂语句已移交 ast，外壳块清空 children 后只释放自身。 */
        node_resize_children(resolved, 0);
        delete_node(&resolved);
      }
      continue;
    }
    /* §2.3 完整版：实例化的泛型定义不进输出（emit 尾部按实例体
       逐组合产出替代声明）。 */
    if (sema_emit_generic_replaced(sema, stmt)) {
      continue;
    }
    /* 提升 owner（§2.2 顶层函数 / §2.10 struct 成员）：rename + skip
       双通道重编排——体内引用点改提升名、原声明语句摘除。 */
    if (hoist_total > 0 && stmt && sema_emit_has_hoists_for(sema, stmt)) {
      renames = allocator_alloc(out, sizeof(sema_rename_t) * hoist_total);
      env.renames = renames;
      env.rename_count =
          sema_emit_renames_for(sema, stmt, renames, hoist_total);
      skip = sema_emit_skip_for(sema, stmt, out);
      env.skip = skip;
      program_node_add_stmt(ast, sema_clone_node_renamed(stmt, &env));
      allocator_free(out, (void **)&renames);
      delete_vec(&skip);
      env.renames = NULL;
      env.rename_count = 0;
      env.skip = NULL;
      continue;
    }
    program_node_add_stmt(ast, sema_clone_node_renamed(stmt, &env));
  }
  /* 提升项本体：同一 owner 的 rename/skip 同样适用（body 内引用兄弟
     声明改名、更深层提升项摘除），name SYMBOL 由 rename 表改写。 */
  for (size_t idx = 0; idx < hoist_total; idx++) {
    sema_hoist_t *hoist = (sema_hoist_t *)vec_get(sema->hoists, idx);
    renames = allocator_alloc(out, sizeof(sema_rename_t) * hoist_total);
    env.renames = renames;
    env.rename_count =
        sema_emit_renames_for(sema, hoist->owner, renames, hoist_total);
    skip = sema_emit_skip_for(sema, hoist->owner, out);
    env.skip = skip;
    program_node_add_stmt(ast, sema_clone_node_renamed(hoist->stmt, &env));
    allocator_free(out, (void **)&renames);
    delete_vec(&skip);
    env.renames = NULL;
    env.rename_count = 0;
    env.skip = NULL;
  }
  /* §2.3 P11 实例体输出：walk 期已物化（inst_stmt），emit 只对物化体做
     纯普通深克隆——不再关心实例化（代换/自引用折叠均已固化）。物化
     失败条目（防御态 NULL）跳过。表注入保持 env 现状：物化体节点指针
     不在原树锚定的收集表中，不会命中。克隆期开启 reown_from：物化体
     新造文本（借 inst_program 缓冲）重 intern 进产物 program，产物
     不借 sema 生命周期（§5-2 引擎全灭后仍可序列化）。 */
  env.reown_from = sema->inst_program;
  for (size_t idx = 0; idx < vec_get_size(sema->instance_bodies); idx++) {
    sema_instance_body_t *body =
        (sema_instance_body_t *)vec_get(sema->instance_bodies, idx);
    if (body->inst_stmt) {
      program_node_add_stmt(ast,
                            sema_clone_node_renamed(body->inst_stmt, &env));
    }
  }
  env.reown_from = NULL;
  /* §2.9 匿名构造具名化：按收集序合成 struct nonamed$N { ... }。
     字段取自 CONSTRUCT_FIELD（NAME 渲染 identifier，TYPE 按 VALUE
     字面量推断：bool / 带后缀数值以后缀定类型 / 无后缀整数默认 i32，
     非字面量 TYPE 置 NULL 等类型检查分期）；位置元素与 rest 不进
     struct 形态（构造点 TYPE 槽已回填名，元素保持原样）。 */
  for (size_t idx = 0; idx < vec_get_size(sema->anons); idx++) {
    sema_anon_t *anon = (sema_anon_t *)vec_get(sema->anons, idx);
    const location_t *loc = node_get_location(anon->node);
    node_t *members = create_struct_members_node(out);
    vec_t *elems = node_get_children(anon->node);
    for (size_t e = (size_t)CLUX_EXPR_CONSTRUCT_FIRST_ELEM;
         e < vec_get_size(elems);
         e++) {
      node_t *elem = (node_t *)vec_get(elems, e);
      if (!elem || node_get_kind(elem) != CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
        continue;
      }
      node_t *name_src = construct_field_node_get_name(elem);
      node_t *value = construct_field_node_get_value(elem);
      char *field_name = sema_node_is_name_form(name_src)
                             ? location_get(node_get_location(name_src), out)
                             : NULL;
      const char *type_name = NULL;
      if (value && node_get_kind(value) == CLUX_NODE_LITERAL_BOOLEAN) {
        type_name = "bool";
      } else if (value && node_get_kind(value) == CLUX_NODE_LITERAL_NUMERIC) {
        char *text = location_get(node_get_location(value), out);
        if (text) {
          size_t num_len = 0;
          const char *suffix =
              sema_render_strip_suffix(text, strlen(text), &num_len);
          type_name = suffix ? suffix : "i32";
          allocator_free(out, (void **)&text);
        }
      }
      node_t *field = create_field_node(out);
      node_set_location(field, node_get_location(elem));
      field_node_set_name(
          field,
          field_name ? sema_render_text_node(out,
                                             program,
                                             CLUX_NODE_LITERAL_IDENTIFIER,
                                             field_name,
                                             node_get_location(name_src))
                     : NULL);
      field_node_set_type(
          field,
          type_name ? sema_render_text_node(out,
                                            program,
                                            CLUX_NODE_LITERAL_IDENTIFIER,
                                            type_name,
                                            node_get_location(value))
                    : NULL);
      struct_members_node_add_member(members, field);
      allocator_free(out, (void **)&field_name);
    }
    node_t *decl = create_struct_decl_node(out);
    node_set_location(decl, loc);
    struct_decl_node_set_keyword(
        decl,
        sema_render_text_node(out, program, CLUX_NODE_SYMBOL, "struct", loc));
    struct_decl_node_set_name(
        decl,
        sema_render_text_node(
            out, program, CLUX_NODE_LITERAL_IDENTIFIER, anon->name, loc));
    struct_decl_node_set_generics(decl, NULL);
    struct_decl_node_set_implements(decl, NULL);
    struct_decl_node_set_body(decl, members);
    node_t *stmt = create_stmt_struct_node(out);
    node_set_location(stmt, loc);
    stmt_struct_node_set_struct_decl(stmt, decl);
    stmt_struct_node_set_export(stmt, NULL);
    program_node_add_stmt(ast, stmt);
  }
  program_set_decls(program, ast);
  vec_push(programs, program);
  return programs;
}
