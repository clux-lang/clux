/* 类型声明族落地（struct/union/cunion/enum/type alias，白皮书 §3.5-§3.8、
   §3.11）：声明即执行——类型经 context 创建，包成 type value 后经
   declare_value 具名化（类型名是脚本的一等名字，与变量共用作用域遮罩
   规则）。带泛型参数的声明走 type template 值（实例化通道属后续任务）。
   声明语句不产值，与 var/func 声明一致。 */
#include "exec_internal.h"
#include "parser/node_enum_decl.h"
#include "parser/node_field.h"
#include "parser/node_struct_decl.h"
#include "parser/node_struct_member.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_type.h"
#include "parser/node_stmt_union.h"
#include "parser/node_union.h"

/* enum underlying 校验（§3.6：Underlying 必须是整数类型）。 */
static bool is_integer_kind(clux_type_kind_t kind) {
  return (kind >= CLUX_TYPE_I8 && kind <= CLUX_TYPE_U64) ||
         kind == CLUX_TYPE_ISIZE || kind == CLUX_TYPE_USIZE;
}

/* type value / template 值具名化落地；声明语句不产值。 */
static exec_value_t declare_named(clux_exec_t *self,
                                  node_t *name_node,
                                  clux_value_t *value,
                                  node_t *node) {
  char *name = exec_node_text(self, name_node);
  clux_value_t *declared =
      clux_context_declare_value(self->ctx, value, name, exec_loc(node));
  allocator_free(self->allocator, (void **)&name);
  if (exec_is_error_value(declared)) {
    return exec_err(declared);
  }
  return exec_ok(NULL);
}

/* 泛型声明的模板通道：AST 声明节点整体挂进 type template 值，
   实例化时按泛型参数表重新求值（编译流水线职责，脚本模式只落地名字）。 */
static exec_value_t declare_template(clux_exec_t *self,
                                     node_t *name_node,
                                     node_t *decl_node,
                                     node_t *node) {
  clux_value_t *tmpl = clux_context_create_type_template(
      self->ctx, decl_node, NULL, exec_loc(node));
  if (exec_is_error_value(tmpl)) {
    return exec_err(tmpl);
  }
  return declare_named(self, name_node, tmpl, node);
}

/* 字段数组释放：只释放前 count 个已填充槽的 name 副本与数组本体。 */
static void free_struct_fields(clux_exec_t *self,
                               clux_struct_field_t *fields,
                               size_t count) {
  if (!fields) {
    return;
  }
  for (size_t idx = 0; idx < count; idx++) {
    allocator_free(self->allocator, (void **)&fields[idx].name);
  }
  allocator_free(self->allocator, (void **)&fields);
}

/* 名字 + 类型表达式 → 成员表槽位（name 为临时副本，create_*_type 内部
   驻留后由调用方释放）。失败时返回硬错误（含语义化定位）。 */
static exec_value_t eval_member_slot(clux_exec_t *self,
                                     node_t *member,
                                     node_t *name_node,
                                     node_t *type_node,
                                     const char *what,
                                     const char **out_name,
                                     clux_type_t **out_type) {
  if (!name_node || !type_node) {
    return exec_errorf(
        self, exec_loc(member), "%s requires a name and a type", what);
  }
  exec_value_t error;
  *out_type = exec_eval_type(self, type_node, &error);
  if (!*out_type) {
    return error;
  }
  *out_name = exec_node_text(self, name_node);
  return exec_ok(NULL);
}

/* 字段表求值：struct（CLUX_NODE_STRUCT_MEMBERS）与 union/cunion
   （CLUX_NODE_FIELDS）共用——两者 children 都是 FIELD；三种成员结构与
   clux_struct_field_t 布局一致，union 侧调用点做布局兼容 cast。
   fields 数组与各 name 副本由调用方释放。 */
static exec_value_t eval_struct_fields(clux_exec_t *self,
                                       node_t *members_node,
                                       clux_struct_field_t **out,
                                       size_t *out_count) {
  bool is_fields_list = node_get_kind(members_node) == CLUX_NODE_FIELDS;
  size_t count = is_fields_list
                     ? fields_node_get_field_count(members_node)
                     : struct_members_node_get_member_count(members_node);
  clux_struct_field_t *fields =
      count ? allocator_alloc(self->allocator, count * sizeof(*fields)) : NULL;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *member = is_fields_list
                         ? fields_node_get_field(members_node, idx)
                         : struct_members_node_get_member(members_node, idx);
    if (node_get_kind(member) != CLUX_NODE_FIELD) {
      /* 脚本模式的 struct 声明只收字段：attribute（var 成员）、方法
         （func 成员）需要调用帧与成员命名空间通道，嵌入展开需要被嵌
         入类型的字段表——三者统一留给后续任务。 */
      free_struct_fields(self, fields, idx);
      return exec_errorf(self,
                         exec_loc(member),
                         "struct member is not supported in script mode");
    }
    exec_value_t failed = eval_member_slot(self,
                                           member,
                                           field_node_get_name(member),
                                           field_node_get_type(member),
                                           "struct field",
                                           &fields[idx].name,
                                           &fields[idx].type);
    if (exec_is_error(failed)) {
      free_struct_fields(self, fields, idx);
      return failed;
    }
  }
  *out = fields;
  *out_count = count;
  return exec_ok(NULL);
}

/* 类型创建后包 type value 并具名化：type 创建失败时 context 已在
   error 通道记录原因，这里补声明定位。 */
static exec_value_t declare_type_value(clux_exec_t *self,
                                       node_t *name_node,
                                       clux_type_t *type,
                                       node_t *node,
                                       const char *what) {
  if (!type) {
    return exec_errorf(self, exec_loc(node), "%s type is invalid", what);
  }
  clux_value_t *value =
      clux_context_create_type_value(self->ctx, type, NULL, exec_loc(node));
  if (exec_is_error_value(value)) {
    return exec_err(value);
  }
  return declare_named(self, name_node, value, node);
}

exec_value_t exec_visit_type_stmt(clux_exec_t *self, node_t *node) {
  node_t *name_node = stmt_type_node_get_name(node);
  node_t *value_node = stmt_type_node_get_value(node);
  if (!name_node || !value_node) {
    return exec_errorf(self, exec_loc(node), "type declaration is incomplete");
  }
  if (stmt_type_node_get_generics(node)) {
    return declare_template(self, name_node, node, node);
  }
  exec_value_t value = exec_dispatch_expr(self, value_node);
  if (exec_is_error(value)) {
    return value;
  }
  if (!exec_type_of_value(value.value)) {
    return exec_errorf(
        self, exec_loc(value_node), "type alias requires a type");
  }
  return declare_named(self, name_node, value.value, node);
}

exec_value_t exec_visit_struct_stmt(clux_exec_t *self, node_t *node) {
  node_t *decl = stmt_struct_node_get_struct_decl(node);
  if (!decl) {
    return exec_errorf(
        self, exec_loc(node), "struct declaration is incomplete");
  }
  node_t *name_node = struct_decl_node_get_name(decl);
  if (!name_node) {
    return exec_errorf(
        self, exec_loc(decl), "struct declaration requires a name");
  }
  if (struct_decl_node_get_generics(decl)) {
    return declare_template(self, name_node, decl, node);
  }
  if (struct_decl_node_get_implements(decl)) {
    return exec_errorf(self,
                       exec_loc(node),
                       "interface implements is not supported in script mode");
  }
  node_t *body = struct_decl_node_get_body(decl);
  clux_struct_field_t *fields = NULL;
  size_t field_count = 0;
  if (body) {
    exec_value_t failed = eval_struct_fields(self, body, &fields, &field_count);
    if (exec_is_error(failed)) {
      return failed;
    }
  }
  clux_struct_decl_t sdecl = {fields, field_count, NULL, 0, NULL, 0, NULL, 0};
  char *name = exec_node_text(self, name_node);
  clux_type_t *type = clux_context_create_struct_type(self->ctx, name, &sdecl);
  allocator_free(self->allocator, (void **)&name);
  free_struct_fields(self, fields, field_count);
  return declare_type_value(self, name_node, type, node, "struct");
}

exec_value_t exec_visit_union_stmt(clux_exec_t *self, node_t *node) {
  node_t *decl = stmt_union_node_get_union_decl(node);
  if (!decl) {
    return exec_errorf(self, exec_loc(node), "union declaration is incomplete");
  }
  node_t *name_node = union_node_get_name(decl);
  node_t *keyword = union_node_get_keyword(decl);
  if (!name_node || !keyword) {
    return exec_errorf(
        self, exec_loc(decl), "union declaration requires a name");
  }
  if (union_node_get_generics(decl)) {
    return declare_template(self, name_node, decl, node);
  }
  node_t *body = union_node_get_body(decl);
  clux_struct_field_t *fields = NULL;
  size_t field_count = 0;
  if (body) {
    /* union 字段列表（CLUX_NODE_FIELDS）与 struct 成员列表访问器分立，
       这里借用 struct 成员通道读取：两者 children 都是 FIELD。 */
    exec_value_t failed = eval_struct_fields(self, body, &fields, &field_count);
    if (exec_is_error(failed)) {
      return failed;
    }
  }
  char *name = exec_node_text(self, name_node);
  clux_type_t *type = NULL;
  if (exec_node_is(keyword, "cunion")) {
    /* clux_cunion_member_t 与 clux_struct_field_t 布局一致（name+type）。 */
    type = clux_context_create_cunion_type(
        self->ctx, name, (const clux_cunion_member_t *)fields, field_count);
  } else {
    /* clux_union_member_t 与 clux_struct_field_t 布局一致（name+type）。 */
    type = clux_context_create_union_type(
        self->ctx, name, (const clux_union_member_t *)fields, field_count);
  }
  allocator_free(self->allocator, (void **)&name);
  free_struct_fields(self, fields, field_count);
  return declare_type_value(self, name_node, type, node, "union");
}

exec_value_t exec_visit_enum_stmt(clux_exec_t *self, node_t *node) {
  node_t *decl = stmt_enum_node_get_enum_decl(node);
  if (!decl) {
    return exec_errorf(self, exec_loc(node), "enum declaration is incomplete");
  }
  node_t *name_node = enum_decl_node_get_name(decl);
  node_t *underlying_node = enum_decl_node_get_underlying(decl);
  if (!name_node) {
    return exec_errorf(
        self, exec_loc(decl), "enum declaration requires a name");
  }
  exec_value_t error;
  clux_type_t *underlying = exec_eval_type(self, underlying_node, &error);
  if (!underlying) {
    if (underlying_node) {
      return error;
    }
    return exec_errorf(
        self, exec_loc(decl), "enum requires an underlying integer type");
  }
  if (!is_integer_kind(clux_type_get_kind(underlying))) {
    return exec_errorf(self,
                       exec_loc(underlying_node),
                       "enum underlying type must be an integer type");
  }
  /* variant 常量（编译期整数常量，§3.6）挂在枚举名的类型命名空间
     （Color.Red），脚本模式待 member 通道（调用帧任务）接线后落地；
     此处先把枚举名落为 underlying 的类型别名。 */
  return declare_type_value(self, name_node, underlying, node, "enum");
}
