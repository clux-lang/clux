#include "sema/sema.h"
#include "core/location.h"
#include "core/program.h"
#include "core/string.h"
#include "engine/diag.h"
#include "engine/module.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_enum_decl.h"
#include "parser/node_func_decl.h"
#include "parser/node_interface_decl.h"
#include "parser/node_program.h"
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
#include "parser/node_union.h"
#include "parser/parser.h"
#include "sema/sema_internal.h"
#include <string.h>

struct _clux_sema_t {
  clux_context_t *ctx;        /* 借用：engine 侧全部状态（scope/module/diag） */
  allocator_t *out_allocator; /* 借用：emit 产物（program_t）的唯一分配来源 */
  clux_module_t *module;      /* 借用：ctx 的 modules 注册表持有 */
  int walked;                 /* 0 未走 / 1 已走（walk 只允许一次） */
  vec_t *hoists;              /* sema_hoist_t*，engine allocator 持有 */
  vec_t *instants;            /* sema_instant_t*，模块级实例化表（§2.3） */
  vec_t *generic_names;       /* char*，模块级泛型声明名集合（§2.3） */
  vec_t *const_defs; /* sema_const_def_t*，顶层 comptime 常量表（§2.1） */
  vec_t *const_uses; /* sema_const_use_t*，常量引用点表（§2.1） */
  vec_t *prunes;     /* sema_prune_t*，comptime if 剪枝决策表（§2.5） */
  node_t *ast;       /* 借用：walk 入参 PROGRAM（emit 只读） */
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
  sema->hoists = create_vec(allocator);
  sema->instants = create_vec(allocator);
  sema->generic_names = create_vec(allocator);
  sema->const_defs = create_vec(allocator);
  sema->const_uses = create_vec(allocator);
  sema->prunes = create_vec(allocator);
  sema->ast = NULL;
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
  for (size_t idx = 0; idx < vec_get_size(self->generic_names); idx++) {
    char *name = (char *)vec_get(self->generic_names, idx);
    allocator_free(allocator, (void **)&name);
  }
  delete_vec(&self->generic_names);
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
  allocator_free(allocator, (void **)sema);
}

bool sema_has_error(const clux_sema_t *sema) {
  return sema ? clux_context_diag_has_error(sema->ctx) : false;
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
    [CLUX_NODE_STMT_FUNC] = {sema_func_name_of, NULL, 1},
    [CLUX_NODE_STMT_STRUCT] = {sema_struct_name_of, sema_struct_generics_of, 1},
    [CLUX_NODE_STMT_ENUM] = {sema_enum_name_of, NULL, 1},
    [CLUX_NODE_STMT_UNION] = {sema_union_name_of, sema_union_generics_of, 1},
    [CLUX_NODE_STMT_BITFIELD] = {sema_bitfield_name_of, NULL, 1},
    [CLUX_NODE_STMT_INTERFACE] = {sema_interface_name_of, NULL, 1},
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

/* ---- 实例化名回填（§2.3 最小版：只类型） ----
   模块级泛型声明名 → 使用点（EXPR_SUBSCRIPT）合成实例名
   base$arg1$arg2...；module_declare 天然去重（同组合首次登记成功，
   之后静默）。实例体深拷贝代换是 Phase 5。 */

/* 语句 → 泛型参数表节点（TYPE/STRUCT/UNION；func/interface 的
   实例化随后续分期）。无泛型返回 NULL。 */
static node_t *sema_stmt_generics_node(node_t *stmt) {
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

/* 带泛型表的声明 → 声明名入模块级泛型名集合（去重后入表）。
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
  for (size_t idx = 0; idx < vec_get_size(sema->generic_names); idx++) {
    if (strcmp((char *)vec_get(sema->generic_names, idx), name) == 0) {
      allocator_free(allocator, (void **)&name);
      return;
    }
  }
  vec_push(sema->generic_names, name);
}

static int sema_name_is_generic(clux_sema_t *sema, const char *name) {
  for (size_t idx = 0; idx < vec_get_size(sema->generic_names); idx++) {
    if (strcmp((char *)vec_get(sema->generic_names, idx), name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* 登记：使用点 + base 名 → 实例名 base$arg1$...。args 已由调用方
   验证全部名字形态，此处按原序拼名。module_declare 失败=同组合
   已登记，静默去重。 */
static void sema_instant_register(clux_sema_t *sema,
                                  node_t *use,
                                  node_t *base,
                                  const char *base_name) {
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  vec_t *children = node_get_children(use);
  string_t *joined = create_string(allocator);
  string_append_cstr(joined, base_name);
  for (size_t idx = 1; idx < vec_get_size(children); idx++) {
    node_t *arg = (node_t *)vec_get(children, idx);
    char *text = location_get(node_get_location(arg), allocator);
    string_append_cstr(joined, "$");
    if (text) {
      string_append_cstr(joined, text);
      allocator_free(allocator, (void **)&text);
    }
  }
  const char *data = string_get_data(joined);
  size_t len = string_get_size(joined);
  char *name = allocator_alloc(allocator, len + 1);
  memcpy(name, data, len + 1);
  delete_string(&joined);
  /* 符号表登记（同组合第二次 declare 失败即去重，静默）；instants 是
     按使用点的表，每个使用点各一条（emit 按原树 node 指针匹配折叠）。*/
  clux_module_declare(sema->module, name, false);
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
    allocator_free(allocator, (void **)&name);
  }
  for (size_t idx = 0; idx < count; idx++) {
    sema_instant_collect(sema, program_node_get_stmt(program, idx));
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
      &env, out, program, sema->instants, sema->const_uses, sema->prunes);
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
    if (stmt && node_get_kind(stmt) == CLUX_NODE_STMT_FUNC && hoist_total > 0) {
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
  program_set_decls(program, ast);
  vec_push(programs, program);
  return programs;
}
