/* sema check pass（§8.8 P7）：类型环境 + 名字解析。
   在原树原名上做检查（与 mangling 解耦）：第一遍按声明序把模块级名字
   登记进 module scope（类型求值为 type value，失败降级占位值防止引用
   级联）；第二遍逐顶层函数体建帧（作用域栈 + 形参/捕获 + 块级预扫描）
   做名字/类型引用检查。只读 AST、只产 diag——错误全收集不 fast-fail，
   walk 通道已报过的错误（模块级重复/ambiguous 提升名）静默跳过不重复
   报。值登记全部走 create_* 通道（自动挂当前 scope，绝不手工
   clux_scope_define）。未覆盖形态一律静默放行（P8+ 扩展）。 */
#include "sema/sema_internal.h"
#include "core/allocator.h"
#include "core/location.h"
#include "core/vec.h"
#include "engine/bitfield.h"
#include "engine/enum.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/cunion.h"
#include "engine/struct.h"
#include "engine/type.h"
#include "engine/union.h"
#include "engine/value.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_bitfield_field.h"
#include "parser/node_builtin.h"
#include "parser/node_call.h"
#include "parser/node_construct.h"
#include "parser/node_construct_field.h"
#include "parser/node_enum_decl.h"
#include "parser/node_enum_variant.h"
#include "parser/node_field.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_group.h"
#include "parser/node_interface_decl.h"
#include "parser/node_program.h"
#include "parser/node_stmt_assert.h"
#include "parser/node_stmt_bitfield.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/node_stmt_do_while.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_extern_cfunc.h"
#include "parser/node_stmt_extern_var.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_static_assert.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_switch.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_while.h"
#include "parser/node_struct_decl.h"
#include "parser/node_subscript.h"
#include "parser/node_switch_branch.h"
#include "parser/node_tuple.h"
#include "parser/node_union.h"
#include <stdlib.h>
#include <string.h>

static void check_stmt(clux_sema_t *sema, node_t *stmt);
static void check_expr(clux_sema_t *sema, node_t *node);
static void check_func_decl(clux_sema_t *sema, node_t *decl);

/* ---- 基础提取 ---- */

/* 名字提取：location 文本拷贝（调用方 allocator_free）。 */
static char *check_name_of(allocator_t *allocator, node_t *node) {
  if (!node) {
    return NULL;
  }
  return location_get(node_get_location(node), allocator);
}

/* 名字形态：无孩子叶子且 kind ∈ {SYMBOL, LITERAL_IDENTIFIER}。 */
static int check_is_name_form(node_t *node) {
  if (!node) {
    return 0;
  }
  node_kind_t kind = node_get_kind(node);
  if (kind != CLUX_NODE_SYMBOL && kind != CLUX_NODE_LITERAL_IDENTIFIER) {
    return 0;
  }
  return vec_get_size(node_get_children(node)) == 0;
}

/* 数值叶子 → u64（负数/非数值叶子返回 0）。数组长度与 bitfield 宽度用。 */
static int
check_numeric_u64(allocator_t *allocator, node_t *node, uint64_t *out) {
  if (!node || node_get_kind(node) != CLUX_NODE_LITERAL_NUMERIC ||
      vec_get_size(node_get_children(node)) != 0) {
    return 0;
  }
  char *text = check_name_of(allocator, node);
  if (!text) {
    return 0;
  }
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 0);
  int ok = end && *end == '\0' && end != text && text[0] != '-';
  allocator_free(allocator, (void **)&text);
  if (ok) {
    *out = value;
  }
  return ok;
}

/* ---- 名字登记 ---- */

/* 登记前门禁：本层判重 + 内建类型遮蔽诊断。
   - 本层已有同名：dup_diag（局部语句）报 "duplicate declaration"；静默
     （模块级/块级预扫描）直接跳过——重复已由 walk 的 module_declare /
     ambiguous 通道报过，check 不重复报。
   - 命中 global 内建类型名（TYPE kind 值）：报 "cannot shadow builtin
     type"，仍继续登记（外层名字允许遮蔽，遮蔽本身合法只是要提示）。
   返回 1 = 允许登记（*name_out 由调用方释放），0 = 跳过。 */
static int check_define_gate(clux_sema_t *sema,
                             node_t *name_node,
                             int dup_diag,
                             char **name_out) {
  if (!name_node || !check_is_name_form(name_node)) {
    return 0;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, name_node);
  if (!name) {
    return 0;
  }
  clux_scope_t *scope = clux_context_get_scope(ctx);
  if (clux_scope_defined_here(scope, name)) {
    if (dup_diag) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(name_node),
                        "duplicate declaration '%s'",
                        name);
    }
    allocator_free(allocator, (void **)&name);
    return 0;
  }
  clux_value_t *builtin =
      clux_scope_get(clux_context_get_global_scope(ctx), name);
  if (builtin &&
      clux_type_get_kind(clux_value_get_type(builtin)) == CLUX_TYPE_TYPE) {
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(name_node),
                      "cannot shadow builtin type '%s'",
                      name);
  }
  *name_out = name;
  return 1;
}

/* 类型名登记（type alias / struct / enum / union / bitfield 名）：
   described 非空 → type value（名字即类型名，check_type_find 命中）；
   空 → 占位值（error 类型 + 零存储——名字可解析但不可当类型用，
   防引用级联 "undefined name" 假阳性）。 */
static void check_define_type_name(clux_sema_t *sema,
                                   node_t *name_node,
                                   clux_type_t *described,
                                   int dup_diag) {
  char *name = NULL;
  if (!check_define_gate(sema, name_node, dup_diag, &name)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  if (described) {
    clux_context_create_type_value(
        ctx, described, name, node_get_location(name_node));
  } else {
    clux_context_create_value(ctx,
                              clux_context_get_error_type(ctx),
                              0,
                              name,
                              node_get_location(name_node));
  }
  allocator_free(clux_context_get_allocator(ctx), (void **)&name);
}

/* 值名登记（var / 形参 / 捕获 / 函数名）：create_value 挂当前 scope，
   携带值的类型（缺省占位 error 类型，零存储）。 */
static void check_define_value_name(clux_sema_t *sema,
                                    node_t *name_node,
                                    clux_type_t *value_type,
                                    int dup_diag) {
  char *name = NULL;
  if (!check_define_gate(sema, name_node, dup_diag, &name)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  clux_context_create_value(ctx,
                            value_type ? value_type
                                       : clux_context_get_error_type(ctx),
                            0,
                            name,
                            node_get_location(name_node));
  allocator_free(clux_context_get_allocator(ctx), (void **)&name);
}

/* 泛型定义登记：模板值（AST 载荷借用原树，P7 仅作名字环境占位——
   泛型使用点 EXPR_SUBSCRIPT 静默跳过，载荷无人读取）。 */
static void check_define_template(clux_sema_t *sema,
                                  node_t *name_node,
                                  node_t *ast,
                                  int is_func) {
  char *name = NULL;
  if (!check_define_gate(sema, name_node, 0, &name)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  if (is_func) {
    clux_context_create_func_template(
        ctx, ast, name, node_get_location(name_node));
  } else {
    clux_context_create_type_template(
        ctx, ast, name, node_get_location(name_node));
  }
  allocator_free(clux_context_get_allocator(ctx), (void **)&name);
}

/* ---- 类型求值 ---- */

/* 名字 → 类型（沿 scope 链查 TYPE kind 值并解引用）。 */
static clux_type_t *check_type_find(clux_sema_t *sema, const char *name) {
  clux_value_t *found = clux_scope_get(clux_context_get_scope(sema->ctx), name);
  if (!found ||
      clux_type_get_kind(clux_value_get_type(found)) != CLUX_TYPE_TYPE) {
    return NULL;
  }
  return *(clux_type_t **)clux_value_get_raw(found, 0);
}

/* 类型表达式 → clux_type_t。report=1 时未声明类型报 diag（形参/返回/
   局部 var/构造/标注等类型位置）；模块级与预扫描一律 report=0（求值
   失败降级占位即可，未声明的引用由名字检查通道兜底）。前缀 * / [] /
   [N] / ? / const / volatile 为 EXPR_BINARY 单/双子，tuple 为
   EXPR_TUPLE；未覆盖形态（泛型实例使用点 EXPR_SUBSCRIPT、func type、
   声明本体）静默返回 NULL。 */
static clux_type_t *
check_type_eval(clux_sema_t *sema, node_t *node, int report) {
  if (!node) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  switch (node_get_kind(node)) {
  case CLUX_NODE_EXPR_GROUP:
    return check_type_eval(sema, group_node_get_expr(node), report);
  case CLUX_NODE_SYMBOL:
  case CLUX_NODE_LITERAL_IDENTIFIER: {
    if (!check_is_name_form(node)) {
      return NULL;
    }
    char *name = check_name_of(allocator, node);
    clux_type_t *type = name ? check_type_find(sema, name) : NULL;
    if (!type && report && name) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "undeclared type '%s'",
                        name);
    }
    allocator_free(allocator, (void **)&name);
    return type;
  }
  case CLUX_NODE_EXPR_BINARY: {
    node_t *symbol = binary_node_get_symbol(node);
    char *op = check_name_of(allocator, symbol);
    node_t *left = binary_node_get_left(node);
    node_t *right = binary_node_get_right(node);
    clux_type_t *result = NULL;
    if (op && strcmp(op, "*") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result = base ? clux_context_get_ptr_type(ctx, base) : NULL;
    } else if (op && strcmp(op, "[]") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      if (!base) {
        result = NULL;
      } else if (left) {
        uint64_t len = 0;
        result = check_numeric_u64(allocator, left, &len)
                     ? clux_context_get_array_type(ctx, base, (size_t)len)
                     : NULL;
      } else {
        result = clux_context_get_slice_type(ctx, base);
      }
    } else if (op && strcmp(op, "?") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result = base ? clux_context_get_optional_type(ctx, base) : NULL;
    } else if (op && strcmp(op, "const") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result =
          base ? clux_type_get_qualified_type(base, CLUX_TYPE_CONST) : NULL;
    } else if (op && strcmp(op, "volatile") == 0) {
      clux_type_t *base = check_type_eval(sema, right, report);
      result =
          base ? clux_type_get_qualified_type(base, CLUX_TYPE_VOLATILE) : NULL;
    } else if (op && strcmp(op, "as") == 0) {
      result = check_type_eval(sema, right, report);
    }
    allocator_free(allocator, (void **)&op);
    return result;
  }
  case CLUX_NODE_EXPR_TUPLE: {
    size_t count = tuple_node_get_elem_count(node);
    if (!count) {
      return clux_context_get_tuple_type(ctx, NULL, 0);
    }
    clux_type_t **elems =
        allocator_alloc(allocator, count * sizeof(clux_type_t *));
    int ok = 1;
    for (size_t idx = 0; idx < count; idx++) {
      elems[idx] =
          check_type_eval(sema, tuple_node_get_elem(node, idx), report);
      if (!elems[idx]) {
        ok = 0;
      }
    }
    clux_type_t *result =
        ok ? clux_context_get_tuple_type(ctx, elems, count) : NULL;
    allocator_free(allocator, (void **)&elems);
    return result;
  }
  default:
    return NULL;
  }
}

/* ---- 类型构建（声明体 → 引擎类型） ---- */

/* 非泛型 STMT_STRUCT → struct 类型。FIELD 成员逐个求值，STMT_VAR /
   STMT_FUNC / EMBED 成员跳过（方法与属性已由 walk §2.10 hoist）；任一
   字段求值失败整体降级（返回 NULL，登记占位）。字段名经引擎 strs
   驻留，调用方临时 buffer 构建后释放。 */
static clux_type_t *check_build_struct(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_struct_node_get_struct_decl(stmt);
  node_t *body = decl ? struct_decl_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, struct_decl_node_get_name(decl));
  if (!name) {
    return NULL;
  }
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!count) {
    clux_type_t *empty = clux_context_begin_struct_type(ctx, name, NULL, 0);
    allocator_free(allocator, (void **)&name);
    return empty; /* 零字段合法 */
  }
  clux_struct_field_t *fields =
      allocator_alloc(allocator, count * sizeof(clux_struct_field_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *member = vec_get(children, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_FIELD) {
      continue; /* var/func/embed 成员不参与字段表 */
    }
    char *name = check_name_of(allocator, field_node_get_name(member));
    clux_type_t *type = check_type_eval(sema, field_node_get_type(member), 0);
    if (name && type) {
      fields[filled].name = name;
      fields[filled].type = type;
      filled++;
    } else {
      ok = 0;
      allocator_free(allocator, (void **)&name);
    }
  }
  clux_type_t *result =
      ok ? clux_context_begin_struct_type(ctx, name, fields, filled) : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&fields[idx].name);
  }
  allocator_free(allocator, (void **)&fields);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* 非泛型 STMT_ENUM → enum 类型。底层缺省 i32，须整数或 str；variant 值
   NULL 按序号、数值叶子按字面量，其余形态（含 str 底层常量）P7 降级。 */
static clux_type_t *check_build_enum(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_enum_node_get_enum_decl(stmt);
  node_t *body = decl ? enum_decl_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, enum_decl_node_get_name(decl));
  clux_type_t *underlying =
      check_type_eval(sema, enum_decl_node_get_underlying(decl), 0);
  if (!underlying) {
    underlying = clux_context_builtin_type(ctx, CLUX_TYPE_I32);
  }
  clux_type_kind_t kind = clux_type_get_kind(underlying);
  int is_integer = (kind >= CLUX_TYPE_I8 && kind <= CLUX_TYPE_U64) ||
                   kind == CLUX_TYPE_ISIZE || kind == CLUX_TYPE_USIZE;
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!name || (!is_integer && kind != CLUX_TYPE_STR) || !count) {
    allocator_free(allocator, (void **)&name);
    return NULL; /* 空表/底层非整数非 str 由引擎 error 通道兜底 */
  }
  clux_enum_variant_t *variants =
      allocator_alloc(allocator, count * sizeof(clux_enum_variant_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *variant = vec_get(children, idx);
    if (!variant || node_get_kind(variant) != CLUX_NODE_ENUM_VARIANT) {
      continue;
    }
    char *vname = check_name_of(allocator, enum_variant_node_get_name(variant));
    node_t *value = enum_variant_node_get_value(variant);
    uint64_t raw = 0;
    if (!vname || kind == CLUX_TYPE_STR ||
        (value && !check_numeric_u64(allocator, value, &raw))) {
      ok = 0; /* str 底层常量与非数值形态留待 P8+（常量折叠通道） */
      allocator_free(allocator, (void **)&vname);
      continue;
    }
    variants[filled].name = vname;
    variants[filled].value = value ? raw : (uint64_t)filled;
    variants[filled].str = NULL;
    filled++;
  }
  clux_type_t *result = ok ? clux_context_create_enum_type(
                                 ctx, name, underlying, variants, filled)
                           : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&variants[idx].name);
  }
  allocator_free(allocator, (void **)&variants);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* 非泛型 STMT_UNION → tag union / cunion（按关键字文本分派）。成员
   FIELD 逐个求值，任一失败或空表降级。 */
static clux_type_t *check_build_union(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_union_node_get_union_decl(stmt);
  node_t *body = decl ? union_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, union_node_get_name(decl));
  char *keyword = check_name_of(allocator, union_node_get_keyword(decl));
  int is_cunion = keyword && strcmp(keyword, "cunion") == 0;
  allocator_free(allocator, (void **)&keyword);
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!name || !count) {
    allocator_free(allocator, (void **)&name);
    return NULL; /* 空成员表非法（引擎契约） */
  }
  clux_union_member_t *members =
      allocator_alloc(allocator, count * sizeof(clux_union_member_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *member = vec_get(children, idx);
    if (!member || node_get_kind(member) != CLUX_NODE_FIELD) {
      continue;
    }
    char *mname = check_name_of(allocator, field_node_get_name(member));
    clux_type_t *type = check_type_eval(sema, field_node_get_type(member), 0);
    if (mname && type) {
      members[filled].name = mname;
      members[filled].type = type;
      filled++;
    } else {
      ok = 0;
      allocator_free(allocator, (void **)&mname);
    }
  }
  clux_type_t *result =
      ok && filled
          ? (is_cunion
                 ? clux_context_create_cunion_type(
                       ctx,
                       name,
                       (const clux_cunion_member_t *)(const void *)members,
                       filled)
                 : clux_context_create_union_type(ctx, name, members, filled))
          : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&members[idx].name);
  }
  allocator_free(allocator, (void **)&members);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* 非泛型 STMT_BITFIELD → bitfield 类型。底层缺省 u32 且仅 u8/u16/u32/
   u64；字段名 + 数值宽度，任一非法降级。 */
static clux_type_t *check_build_bitfield(clux_sema_t *sema, node_t *stmt) {
  node_t *decl = stmt_bitfield_node_get_bitfield_decl(stmt);
  node_t *body = decl ? bitfield_decl_node_get_body(decl) : NULL;
  if (!decl || !body) {
    return NULL;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = check_name_of(allocator, bitfield_decl_node_get_name(decl));
  clux_type_t *underlying =
      check_type_eval(sema, bitfield_decl_node_get_underlying(decl), 0);
  if (!underlying) {
    underlying = clux_context_builtin_type(ctx, CLUX_TYPE_U32);
  }
  clux_type_kind_t kind = clux_type_get_kind(underlying);
  if (kind != CLUX_TYPE_U8 && kind != CLUX_TYPE_U16 && kind != CLUX_TYPE_U32 &&
      kind != CLUX_TYPE_U64) {
    allocator_free(allocator, (void **)&name);
    return NULL;
  }
  vec_t *children = node_get_children(body);
  size_t count = vec_get_size(children);
  if (!name || !count) {
    allocator_free(allocator, (void **)&name);
    return NULL; /* 空字段表非法（引擎契约） */
  }
  clux_bitfield_field_def_t *fields =
      allocator_alloc(allocator, count * sizeof(clux_bitfield_field_def_t));
  size_t filled = 0;
  int ok = 1;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *field = vec_get(children, idx);
    if (!field || node_get_kind(field) != CLUX_NODE_BITFIELD_FIELD) {
      continue;
    }
    char *fname = check_name_of(allocator, bitfield_field_node_get_name(field));
    uint64_t width = 0;
    if (!fname ||
        !check_numeric_u64(
            allocator, bitfield_field_node_get_width(field), &width) ||
        width == 0) {
      ok = 0;
      allocator_free(allocator, (void **)&fname);
      continue;
    }
    fields[filled].name = fname;
    fields[filled].width = (size_t)width;
    filled++;
  }
  clux_type_t *result = ok ? clux_context_create_bitfield_type(
                                 ctx, name, underlying, fields, filled)
                           : NULL;
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&fields[idx].name);
  }
  allocator_free(allocator, (void **)&fields);
  allocator_free(allocator, (void **)&name);
  return result;
}

/* ---- 模块级注册（第一遍，按声明序） ---- */

static void check_module_register(clux_sema_t *sema, node_t *stmt);

static void check_module_register_list(clux_sema_t *sema, node_t *block) {
  if (!block) {
    return;
  }
  vec_t *children = node_get_children(block);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    check_module_register(sema, vec_get(children, idx));
  }
}

static void check_module_register(clux_sema_t *sema, node_t *stmt) {
  if (!stmt) {
    return;
  }
  switch (node_get_kind(stmt)) {
  case CLUX_NODE_STMT_VAR:
    /* 模块级 var（含 comptime var）：占位登记。模块级表达式 P7 不检查，
       类型不评估；名字可解析即可（函数体 return B 引用 comptime 常量）。 */
    check_define_value_name(sema, stmt_var_node_get_name(stmt), NULL, 0);
    return;
  case CLUX_NODE_STMT_FUNC: {
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    node_t *name = decl ? func_decl_node_get_name(decl) : NULL;
    if (decl && func_decl_node_get_generics(decl)) {
      check_define_template(sema, name, decl, 1);
    } else {
      check_define_value_name(sema, name, NULL, 0);
    }
    return;
  }
  case CLUX_NODE_STMT_TYPE: {
    node_t *name = stmt_type_node_get_name(stmt);
    if (stmt_type_node_get_generics(stmt)) {
      check_define_template(sema, name, stmt, 0);
    } else {
      clux_type_t *type =
          check_type_eval(sema, stmt_type_node_get_value(stmt), 0);
      check_define_type_name(sema, name, type, 0);
    }
    return;
  }
  case CLUX_NODE_STMT_STRUCT: {
    node_t *decl = stmt_struct_node_get_struct_decl(stmt);
    node_t *name = decl ? struct_decl_node_get_name(decl) : NULL;
    if (decl && struct_decl_node_get_generics(decl)) {
      check_define_template(sema, name, stmt, 0);
    } else {
      check_define_type_name(sema, name, check_build_struct(sema, stmt), 0);
    }
    return;
  }
  case CLUX_NODE_STMT_ENUM: {
    node_t *decl = stmt_enum_node_get_enum_decl(stmt);
    node_t *name = decl ? enum_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, check_build_enum(sema, stmt), 0);
    return;
  }
  case CLUX_NODE_STMT_UNION: {
    node_t *decl = stmt_union_node_get_union_decl(stmt);
    node_t *name = decl ? union_node_get_name(decl) : NULL;
    if (decl && union_node_get_generics(decl)) {
      check_define_template(sema, name, stmt, 0);
    } else {
      check_define_type_name(sema, name, check_build_union(sema, stmt), 0);
    }
    return;
  }
  case CLUX_NODE_STMT_BITFIELD: {
    node_t *decl = stmt_bitfield_node_get_bitfield_decl(stmt);
    node_t *name = decl ? bitfield_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, check_build_bitfield(sema, stmt), 0);
    return;
  }
  case CLUX_NODE_STMT_INTERFACE: {
    /* interface 只出现在约束位置（类型形参约束），P7 占位登记——
       约束求值随泛型实例化通道（P8+），不会进入类型位置求值。 */
    node_t *decl = stmt_interface_node_get_interface_decl(stmt);
    node_t *name = decl ? interface_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, NULL, 0);
    return;
  }
  case CLUX_NODE_STMT_EXTERN_CFUNC:
    check_define_value_name(
        sema, stmt_extern_cfunc_node_get_name(stmt), NULL, 0);
    return;
  case CLUX_NODE_STMT_EXTERN_VAR:
    check_define_value_name(sema, stmt_extern_var_node_get_name(stmt), NULL, 0);
    return;
  case CLUX_NODE_STMT_COMPTIME_IF:
    /* 槽布局与 STMT_IF 相同：THEN/ELSE 为块，体内声明照常注册。 */
    check_module_register_list(sema, node_get_child(stmt, 2));
    check_module_register_list(sema, node_get_child(stmt, 3));
    return;
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    /* 槽布局与 STMT_FOREACH 相同：BODY 为块。 */
    check_module_register_list(sema, node_get_child(stmt, 4));
    return;
  default:
    return; /* import/reexport/extern_lib/test/表达式语句不登记 */
  }
}

/* ---- 函数体帧（第二遍） ---- */

/* 捕获表：FUNC_CAPTURES（FUNC_CAPTURE 单项，或 defer 容错下的
   FUNC_PARAMS 形态）。类型标注按类型位置报 diag，默认值 P7 跳过。 */
static void check_captures(clux_sema_t *sema, node_t *captures) {
  if (!captures || node_get_kind(captures) != CLUX_NODE_FUNC_CAPTURES) {
    return;
  }
  vec_t *children = node_get_children(captures);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    node_t *child = vec_get(children, idx);
    if (!child) {
      continue;
    }
    if (node_get_kind(child) == CLUX_NODE_FUNC_CAPTURE) {
      clux_type_t *type =
          check_type_eval(sema, func_capture_node_get_type(child), 1);
      check_define_value_name(sema, func_capture_node_get_name(child), type, 1);
    } else if (node_get_kind(child) == CLUX_NODE_FUNC_PARAMS) {
      size_t count = func_params_node_get_param_count(child);
      for (size_t pidx = 0; pidx < count; pidx++) {
        node_t *param = func_params_node_get_param(child, pidx);
        clux_type_t *type =
            check_type_eval(sema, func_param_node_get_type(param), 1);
        check_define_value_name(sema, func_param_node_get_name(param), type, 1);
      }
    }
  }
}

/* 块级预扫描：体内 hoistable 声明按原名预登记进函数 scope（前向引用
   天然成立）。重复静默跳过（ambiguous 已由 walk 报过）。闭包体与
   comptime 块不进（各有自己的检查通道/整树跳过）。非泛型 STMT_TYPE
   急切求值——否则 `var x: Foo` 引用局部类型别名会假阳性。 */
static void check_local_scan(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return;
  }
  switch (node_get_kind(node)) {
  case CLUX_NODE_STMT_FUNC: {
    node_t *decl = stmt_func_node_get_func_decl(node);
    node_t *name = decl ? func_decl_node_get_name(decl) : NULL;
    if (decl && func_decl_node_get_generics(decl)) {
      check_define_template(sema, name, decl, 1);
    } else {
      check_define_value_name(sema, name, NULL, 0);
    }
    return; /* 嵌套函数体由自身检查通道扫描 */
  }
  case CLUX_NODE_EXPR_FUNC_DECL:
    return; /* 闭包体同理：进入闭包检查（check_expr）时自建帧 */
  case CLUX_NODE_STMT_TYPE: {
    node_t *name = stmt_type_node_get_name(node);
    if (stmt_type_node_get_generics(node)) {
      check_define_template(sema, name, node, 0);
    } else {
      clux_type_t *type =
          check_type_eval(sema, stmt_type_node_get_value(node), 0);
      check_define_type_name(sema, name, type, 0);
    }
    return;
  }
  case CLUX_NODE_STMT_STRUCT: {
    node_t *decl = stmt_struct_node_get_struct_decl(node);
    node_t *name = decl ? struct_decl_node_get_name(decl) : NULL;
    if (decl && struct_decl_node_get_generics(decl)) {
      check_define_template(sema, name, node, 0);
    } else {
      check_define_type_name(sema, name, check_build_struct(sema, node), 0);
    }
    return;
  }
  case CLUX_NODE_STMT_ENUM: {
    node_t *decl = stmt_enum_node_get_enum_decl(node);
    node_t *name = decl ? enum_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, check_build_enum(sema, node), 0);
    return;
  }
  case CLUX_NODE_STMT_UNION: {
    node_t *decl = stmt_union_node_get_union_decl(node);
    node_t *name = decl ? union_node_get_name(decl) : NULL;
    if (decl && union_node_get_generics(decl)) {
      check_define_template(sema, name, node, 0);
    } else {
      check_define_type_name(sema, name, check_build_union(sema, node), 0);
    }
    return;
  }
  case CLUX_NODE_STMT_BITFIELD: {
    node_t *decl = stmt_bitfield_node_get_bitfield_decl(node);
    node_t *name = decl ? bitfield_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, check_build_bitfield(sema, node), 0);
    return;
  }
  case CLUX_NODE_STMT_INTERFACE: {
    node_t *decl = stmt_interface_node_get_interface_decl(node);
    node_t *name = decl ? interface_decl_node_get_name(decl) : NULL;
    check_define_type_name(sema, name, NULL, 0);
    return;
  }
  case CLUX_NODE_STMT_COMPTIME_IF:
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    return; /* comptime 块整树跳过（与 check_stmt 一致） */
  default:
    break;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    check_local_scan(sema, vec_get(children, idx));
  }
}

/* 函数体帧：push scope → 块级预扫描 → 捕获 → 形参（先全部登记再查
   默认值——默认值可引用任意形参）→ 返回类型 → 逐语句检查 → pop。
   STMT_FUNC 与闭包 EXPR_FUNC_DECL 共用本通道。泛型定义体不检查
   （实例体的检查随实例化通道，P8+）。 */
static void check_func_decl(clux_sema_t *sema, node_t *decl) {
  if (!decl || func_decl_node_get_generics(decl)) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  clux_context_push_scope(ctx);
  node_t *body = func_decl_node_get_body(decl);
  if (body) {
    check_local_scan(sema, body);
  }
  check_captures(sema, func_decl_node_get_captures(decl));
  node_t *params = func_decl_node_get_params(decl);
  size_t count = params ? func_params_node_get_param_count(params) : 0;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *param = func_params_node_get_param(params, idx);
    clux_type_t *type =
        check_type_eval(sema, func_param_node_get_type(param), 1);
    check_define_value_name(sema, func_param_node_get_name(param), type, 1);
  }
  for (size_t idx = 0; idx < count; idx++) {
    node_t *param = func_params_node_get_param(params, idx);
    check_expr(sema, func_param_node_get_default(param));
  }
  check_type_eval(sema, func_decl_node_get_return(decl), 1);
  if (body) {
    check_stmt(sema, body);
  }
  clux_context_pop_scope(ctx);
}

/* ---- 语句检查 ---- */

static void check_stmt(clux_sema_t *sema, node_t *stmt) {
  if (!stmt) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  switch (node_get_kind(stmt)) {
  case CLUX_NODE_STMT_BLOCK: {
    clux_context_push_scope(ctx);
    size_t count = stmt_block_node_get_stmt_count(stmt);
    for (size_t idx = 0; idx < count; idx++) {
      check_stmt(sema, stmt_block_node_get_stmt(stmt, idx));
    }
    clux_context_pop_scope(ctx);
    return;
  }
  case CLUX_NODE_STMT_VAR: {
    clux_type_t *type = check_type_eval(sema, stmt_var_node_get_type(stmt), 1);
    check_expr(sema, stmt_var_node_get_value(stmt));
    check_define_value_name(sema, stmt_var_node_get_name(stmt), type, 1);
    return;
  }
  case CLUX_NODE_STMT_IF:
    check_expr(sema, stmt_if_node_get_condition(stmt));
    check_stmt(sema, stmt_if_node_get_then(stmt));
    check_stmt(sema, stmt_if_node_get_else(stmt));
    return;
  case CLUX_NODE_STMT_WHILE:
    check_expr(sema, stmt_while_node_get_condition(stmt));
    check_stmt(sema, stmt_while_node_get_body(stmt));
    return;
  case CLUX_NODE_STMT_DO_WHILE:
    check_stmt(sema, stmt_do_while_node_get_body(stmt));
    check_expr(sema, stmt_do_while_node_get_condition(stmt));
    return;
  case CLUX_NODE_STMT_FOR: {
    clux_context_push_scope(ctx);
    node_t *init = stmt_for_node_get_init(stmt);
    if (init) {
      if (node_get_kind(init) == CLUX_NODE_STMT_VAR) {
        check_stmt(sema, init);
      } else {
        check_expr(sema, init);
      }
    }
    check_expr(sema, stmt_for_node_get_condition(stmt));
    check_expr(sema, stmt_for_node_get_update(stmt));
    check_stmt(sema, stmt_for_node_get_body(stmt));
    clux_context_pop_scope(ctx);
    return;
  }
  case CLUX_NODE_STMT_FOREACH: {
    /* iterable 在外层作用域求值（迭代变量对 iterable 不可见）。 */
    check_expr(sema, stmt_foreach_node_get_iterable(stmt));
    clux_context_push_scope(ctx);
    clux_type_t *type =
        check_type_eval(sema, stmt_foreach_node_get_type(stmt), 1);
    check_define_value_name(sema, stmt_foreach_node_get_name(stmt), type, 1);
    check_stmt(sema, stmt_foreach_node_get_body(stmt));
    clux_context_pop_scope(ctx);
    return;
  }
  case CLUX_NODE_STMT_COMPTIME_IF:
  case CLUX_NODE_STMT_COMPTIME_FOREACH:
    return; /* 编译期构造整树跳过（walk 已剪枝/展开，emit 通道负责） */
  case CLUX_NODE_STMT_SWITCH: {
    check_expr(sema, stmt_switch_node_get_value(stmt));
    node_t *branches = stmt_switch_node_get_body(stmt);
    size_t count =
        branches ? switch_branches_node_get_branch_count(branches) : 0;
    for (size_t idx = 0; idx < count; idx++) {
      node_t *branch = switch_branches_node_get_branch(branches, idx);
      if (!branch) {
        continue;
      }
      node_t *patterns = switch_branch_node_get_patterns(branch);
      size_t pcount =
          patterns ? switch_patterns_node_get_pattern_count(patterns) : 0;
      for (size_t pidx = 0; pidx < pcount; pidx++) {
        check_expr(sema, switch_patterns_node_get_pattern(patterns, pidx));
      }
      check_stmt(sema, switch_branch_node_get_body(branch));
    }
    return;
  }
  case CLUX_NODE_STMT_DEFER:
    clux_context_push_scope(ctx);
    check_captures(sema, stmt_defer_node_get_captures(stmt));
    check_stmt(sema, stmt_defer_node_get_body(stmt));
    clux_context_pop_scope(ctx);
    return;
  case CLUX_NODE_STMT_RETURN:
    check_expr(sema, stmt_return_node_get_value(stmt));
    return;
  case CLUX_NODE_STMT_ASSERT:
    check_expr(sema, stmt_assert_node_get_condition(stmt));
    return;
  case CLUX_NODE_STMT_STATIC_ASSERT:
    check_expr(sema, stmt_static_assert_node_get_condition(stmt));
    return;
  case CLUX_NODE_STMT_EXPR:
    check_expr(sema, stmt_expr_node_get_expr(stmt));
    return;
  case CLUX_NODE_STMT_FUNC: {
    /* 嵌套函数：声明名已由预扫描登记，这里只进体。 */
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    if (decl && !func_decl_node_get_generics(decl)) {
      check_func_decl(sema, decl);
    }
    return;
  }
  default:
    return; /* 声明类已由预扫描处理；import/extern/test/…跳过 */
  }
}

/* ---- 表达式检查 ---- */

/* 泛型使用点判定：EXPR_SUBSCRIPT 的 base 是泛型定义名。args 是类型
   实参，整树不按值引用检查。 */
static int check_is_generic_use(clux_sema_t *sema, node_t *node) {
  node_t *base = subscript_node_get_base(node);
  if (!check_is_name_form(base)) {
    return 0;
  }
  allocator_t *allocator = clux_context_get_allocator(sema->ctx);
  char *name = check_name_of(allocator, base);
  if (!name) {
    return 0;
  }
  int hit = 0;
  for (size_t idx = 0; idx < vec_get_size(sema->generic_defs); idx++) {
    sema_generic_def_t *def =
        (sema_generic_def_t *)vec_get(sema->generic_defs, idx);
    if (def->name && strcmp(def->name, name) == 0) {
      hit = 1;
      break;
    }
  }
  allocator_free(allocator, (void **)&name);
  return hit;
}

static void check_expr(clux_sema_t *sema, node_t *node) {
  if (!node) {
    return;
  }
  clux_context_t *ctx = sema->ctx;
  allocator_t *allocator = clux_context_get_allocator(ctx);
  switch (node_get_kind(node)) {
  case CLUX_NODE_LITERAL_IDENTIFIER: {
    if (!check_is_name_form(node)) {
      return;
    }
    char *name = check_name_of(allocator, node);
    if (name && !clux_scope_get(clux_context_get_scope(ctx), name)) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "undefined name '%s'",
                        name);
    }
    allocator_free(allocator, (void **)&name);
    return;
  }
  case CLUX_NODE_EXPR_BINARY: {
    node_t *symbol = binary_node_get_symbol(node);
    char *op = check_name_of(allocator, symbol);
    node_t *left = binary_node_get_left(node);
    node_t *right = binary_node_get_right(node);
    if (op && (strcmp(op, ".") == 0 || strcmp(op, "::") == 0)) {
      check_expr(sema, left); /* RIGHT 恒为成员名，不可当名字查 */
    } else if (op && strcmp(op, "as") == 0) {
      check_expr(sema, left);
      check_type_eval(sema, right, 1);
    } else {
      check_expr(sema, left);
      check_expr(sema, right);
    }
    allocator_free(allocator, (void **)&op);
    return;
  }
  case CLUX_NODE_EXPR_ASSIGN:
    check_expr(sema, assign_node_get_left(node));
    check_expr(sema, assign_node_get_value(node));
    return;
  case CLUX_NODE_EXPR_SUBSCRIPT: {
    if (check_is_generic_use(sema, node)) {
      return; /* 泛型实例使用点：args 是类型实参 */
    }
    check_expr(sema, subscript_node_get_base(node));
    size_t count = subscript_node_get_arg_count(node);
    for (size_t idx = 0; idx < count; idx++) {
      check_expr(sema, subscript_node_get_arg(node, idx));
    }
    return;
  }
  case CLUX_NODE_EXPR_CALL:
    check_expr(sema, call_node_get_callee(node));
    for (size_t idx = 0; idx < call_node_get_arg_count(node); idx++) {
      check_expr(sema, call_node_get_arg(node, idx));
    }
    return;
  case CLUX_NODE_EXPR_BUILTIN:
    for (size_t idx = 0; idx < builtin_node_get_arg_count(node); idx++) {
      check_expr(sema, builtin_node_get_arg(node, idx)); /* NAME 排除 */
    }
    return;
  case CLUX_NODE_EXPR_CONSTRUCT: {
    check_type_eval(sema, construct_node_get_type(node), 1);
    for (size_t idx = 0; idx < construct_node_get_elem_count(node); idx++) {
      node_t *elem = construct_node_get_elem(node, idx);
      if (elem && node_get_kind(elem) == CLUX_NODE_EXPR_CONSTRUCT_FIELD) {
        check_expr(sema, construct_field_node_get_value(elem));
      } else {
        check_expr(sema, elem);
      }
    }
    return;
  }
  case CLUX_NODE_EXPR_FUNC_DECL: {
    if (func_decl_node_get_generics(node)) {
      return; /* 泛型闭包定义体随实例化通道（P8+） */
    }
    check_func_decl(sema, node);
    return;
  }
  case CLUX_NODE_EXPR_STRUCT_DECL:
  case CLUX_NODE_EXPR_ENUM_DECL:
  case CLUX_NODE_EXPR_UNION:
  case CLUX_NODE_EXPR_BITFIELD_DECL:
  case CLUX_NODE_EXPR_INTERFACE_DECL:
  case CLUX_NODE_EXPR_FUNC_TYPE:
    /* 声明本体/函数类型在表达式位置：名字归预扫描通道；成员名/捕获名/
       形参名是声明槽，不按引用查。 */
    return;
  default:
    break;
  }
  vec_t *children = node_get_children(node);
  for (size_t idx = 0; idx < vec_get_size(children); idx++) {
    check_expr(sema, vec_get(children, idx));
  }
}

/* ---- 入口 ---- */

int sema_check_pass(clux_sema_t *sema) {
  if (!sema || !sema->ast) {
    return 0;
  }
  /* current scope 须为 module scope（walk 尾部、模块摘除前）。 */
  size_t count = program_node_get_stmt_count(sema->ast);
  for (size_t idx = 0; idx < count; idx++) {
    check_module_register(sema, program_node_get_stmt(sema->ast, idx));
  }
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = program_node_get_stmt(sema->ast, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_FUNC) {
      continue;
    }
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    if (decl && !func_decl_node_get_generics(decl)) {
      check_func_decl(sema, decl);
    }
  }
  return 0;
}
