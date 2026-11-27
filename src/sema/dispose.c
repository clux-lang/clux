/* §4.17 RAII：自动析构通道（P1 struct + P2 容器展开）。
   - disposable 谓词（§4.17.1）：struct 实现 __dispose__ 钩子，或传递性
     包含需要析构的成分——struct 字段、?T 载荷、tag union variant、
     [N]T 元素、元组成员；基础类型/指针/切片/函数值不析构；cunion
     排除（禁 disposable 字段由 check 报编译错误）。钩子判定走
     sema->methods 预扫描表——check 期引擎 struct 类型只有字段表，
     method 三表在 exec 两阶段 install，clux_struct_is_method 不可用。
   - 类型收集：emit 期命中 disposable 使用点（局部 var / #dispose 实参）
     时登记，合成体由 emit 尾部统一追加（谓词命中的成分递归登记，
     被引用的 $dispose$<成分> 必然同批存在）。
   - 合成体展开（§4.17.2）：struct=钩子+逆声明序字段调用；
     ?T=if (!#is_none) { $dispose$B(#take(self.*).&) }；
     [N]T=逆序固定 for 循环逐元素；tag union=逐 disposable variant 的
     is 判定链；元组=逆序逐成员下标。合成体不经 check（内容全部由
     谓词保证可解析），参数类型经 render_type 按 kind 递归重建，
     interning 保证与源码类型表达式还原同一实例。
   - 合成名 mangling：类型名中 [A-Za-z0-9_$] 保留，其余字节转 $xHH
     （$ 前缀名字空间编译器保留 §1.3，用户标识符不可能撞车）——
     "?i32"→"$x3fi32"、"[2]Guard"→"$x5b2$x5dGuard"。 */
#include "sema/sema_internal.h"
#include "core/program.h"
#include "core/vec.h"
#include "engine/array.h"
#include "engine/optional.h"
#include "engine/ptr.h"
#include "engine/qualifier.h"
#include "engine/slice.h"
#include "engine/struct.h"
#include "engine/tuple.h"
#include "engine/type.h"
#include "engine/union.h"
#include "parser/node_assign.h"
#include "parser/node_binary.h"
#include "parser/node_builtin.h"
#include "parser/node_call.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_var.h"
#include "parser/node_subscript.h"
#include "parser/node_tuple.h"
#include <stdio.h>
#include <string.h>

/* __dispose__ 钩子的方法表条目（§2.10 预扫描表按 struct 名 + 方法名
   直匹配），未实现返回 NULL。 */
static sema_method_t *sema_dispose_find_hook(vec_t *methods,
                                             const char *struct_name) {
  if (!methods || !struct_name) {
    return NULL;
  }
  for (size_t idx = 0; idx < vec_get_size(methods); idx++) {
    sema_method_t *method = (sema_method_t *)vec_get(methods, idx);
    if (method && strcmp(method->struct_name, struct_name) == 0 &&
        strcmp(method->method_name, "__dispose__") == 0) {
      return method;
    }
  }
  return NULL;
}

/* mangling 保留字符：字母/数字/下划线/$（$ 编译器保留）。 */
static int sema_dispose_mangle_char(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* "$dispose$" + mangled(类型名) → program intern 文本（永久借用）。
   定义点与调用点共用同一字符串即通过 exec/check 的纯名字通道；
   合成体不经 lexer，mangling 只为 unparse 文本往返留可辨性。 */
const char *sema_dispose_callee_name(allocator_t *allocator,
                                     program_t *program,
                                     const char *type_name) {
  if (!allocator || !program || !type_name) {
    return NULL;
  }
  size_t name_len = strlen(type_name);
  size_t cap = strlen("$dispose$") + name_len * 4 + 1;
  char *buf = allocator_alloc(allocator, cap);
  if (!buf) {
    return NULL;
  }
  size_t pos = 0;
  (void)snprintf(buf, cap, "$dispose$");
  pos += strlen("$dispose$");
  for (size_t idx = 0; idx < name_len; idx++) {
    unsigned char c = (unsigned char)type_name[idx];
    if (sema_dispose_mangle_char(c)) {
      buf[pos++] = (char)c;
    } else {
      int written = snprintf(buf + pos, cap - pos, "$x%02x", (unsigned)c);
      if (written <= 0 || (size_t)written >= cap - pos) {
        break;
      }
      pos += (size_t)written;
    }
  }
  buf[pos] = '\0';
  const char *text = program_intern_string(program, buf, pos);
  allocator_free(allocator, (void **)&buf);
  return text;
}

/* disposable 谓词（白皮书 §4.17.1）：限定包裹走 kind_unwrap，容器按
   成分递归，递归深度由类型有限大小天然封顶。 */
int sema_type_needs_dispose(clux_sema_t *sema, clux_type_t *type) {
  if (!sema || !type) {
    return 0;
  }
  clux_type_kind_t kind = clux_type_kind_unwrap(type);
  if (kind == CLUX_TYPE_STRUCT) {
    const char *name = clux_type_get_name(type);
    if (!name) {
      return 0;
    }
    if (sema_dispose_find_hook(sema->methods, name)) {
      return 1;
    }
    size_t count = clux_struct_field_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_struct_field_t *field = clux_struct_get_field(type, idx);
      if (field && sema_type_needs_dispose(sema, field->type)) {
        return 1;
      }
    }
    return 0;
  }
  if (kind == CLUX_TYPE_OPTIONAL) {
    clux_type_t *base = clux_type_get_optional_base(type);
    return base ? sema_type_needs_dispose(sema, base) : 0;
  }
  if (kind == CLUX_TYPE_ARRAY) {
    clux_type_t *base = clux_array_get_base(type);
    return base ? sema_type_needs_dispose(sema, base) : 0;
  }
  if (kind == CLUX_TYPE_UNION) {
    size_t count = clux_union_member_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_union_member_t *member = clux_union_get_member(type, idx);
      if (member && sema_type_needs_dispose(sema, member->type)) {
        return 1;
      }
    }
    return 0;
  }
  if (kind == CLUX_TYPE_TUPLE) {
    size_t count = clux_tuple_elem_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      if (sema_type_needs_dispose(sema, clux_tuple_get_elem(type, idx))) {
        return 1;
      }
    }
  }
  return 0;
}

void sema_dispose_register(sema_clone_env_t *env,
                           clux_type_t *type,
                           const location_t *loc) {
  if (!env->disposes || !type) {
    return;
  }
  /* 引擎类型同组合同一实例，指针恒等即去重（登记先入表再递归成分，
     自引用组合经恒等短路终止）。 */
  for (size_t idx = 0; idx < vec_get_size(env->disposes); idx++) {
    if (((sema_dispose_type_t *)vec_get(env->disposes, idx))->type == type) {
      return;
    }
  }
  sema_dispose_type_t *entry =
      allocator_alloc(env->out, sizeof(sema_dispose_type_t));
  entry->type = type;
  memset(&entry->loc, 0, sizeof(entry->loc));
  if (loc) {
    entry->loc = *loc;
  }
  (void)vec_push(env->disposes, entry);
  /* 递归登记 disposable 成分（§4.17.2 展开表）：合成体内调用的
     $dispose$<成分> 必然同批合成。 */
  clux_type_kind_t kind = clux_type_kind_unwrap(type);
  if (kind == CLUX_TYPE_STRUCT) {
    size_t count = clux_struct_field_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_struct_field_t *field = clux_struct_get_field(type, idx);
      if (field && sema_type_needs_dispose(env->sema, field->type)) {
        sema_dispose_register(env, field->type, loc);
      }
    }
    return;
  }
  if (kind == CLUX_TYPE_OPTIONAL) {
    clux_type_t *base = clux_type_get_optional_base(type);
    if (base && sema_type_needs_dispose(env->sema, base)) {
      sema_dispose_register(env, base, loc);
    }
    return;
  }
  if (kind == CLUX_TYPE_ARRAY) {
    clux_type_t *base = clux_array_get_base(type);
    if (base && sema_type_needs_dispose(env->sema, base)) {
      sema_dispose_register(env, base, loc);
    }
    return;
  }
  if (kind == CLUX_TYPE_UNION) {
    size_t count = clux_union_member_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_union_member_t *member = clux_union_get_member(type, idx);
      if (member && sema_type_needs_dispose(env->sema, member->type)) {
        sema_dispose_register(env, member->type, loc);
      }
    }
    return;
  }
  if (kind == CLUX_TYPE_TUPLE) {
    size_t count = clux_tuple_elem_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      clux_type_t *elem = clux_tuple_get_elem(type, idx);
      if (sema_type_needs_dispose(env->sema, elem)) {
        sema_dispose_register(env, elem, loc);
      }
    }
  }
}

/* ---- AST 渲染辅助（intern 文本挂 location，exec/check 按文本读取） ---- */

static node_t *sema_dispose_ident(allocator_t *out,
                                  program_t *program,
                                  const char *text,
                                  const location_t *loc) {
  return sema_render_text_node(
      out, program, CLUX_NODE_LITERAL_IDENTIFIER, text, loc);
}

static node_t *sema_dispose_symbol(allocator_t *out,
                                   program_t *program,
                                   const char *text,
                                   const location_t *loc) {
  return sema_render_text_node(out, program, CLUX_NODE_SYMBOL, text, loc);
}

static node_t *sema_dispose_numeric(allocator_t *out,
                                    program_t *program,
                                    size_t value,
                                    const location_t *loc) {
  char buf[32];
  int written = snprintf(buf, sizeof(buf), "%zu", value);
  if (written <= 0 || (size_t)written >= sizeof(buf)) {
    return NULL;
  }
  return sema_render_text_node(
      out, program, CLUX_NODE_LITERAL_NUMERIC, buf, loc);
}

static node_t *sema_dispose_binary(allocator_t *out,
                                   program_t *program,
                                   node_t *left,
                                   const char *op,
                                   node_t *right,
                                   const location_t *loc) {
  node_t *binary = create_binary_node(out);
  node_set_location(binary, loc);
  binary_node_set_left(binary, left);
  binary_node_set_symbol(binary, sema_dispose_symbol(out, program, op, loc));
  binary_node_set_right(binary, right);
  return binary;
}

/* 前缀一元表达式：LEFT 缺失，SYMBOL=运算符，RIGHT=操作数。 */
static node_t *sema_dispose_prefix_expr(allocator_t *out,
                                        program_t *program,
                                        const char *op,
                                        node_t *operand,
                                        const location_t *loc) {
  node_t *prefix = create_binary_node(out);
  node_set_location(prefix, loc);
  binary_node_set_symbol(prefix, sema_dispose_symbol(out, program, op, loc));
  binary_node_set_right(prefix, operand);
  return prefix;
}

/* 后缀一元形态（.& / .*）：复用二元节点，SYMBOL=运算符，RIGHT 缺失
   区分后缀（与 clone.c member_call 退化同构）。 */
static node_t *sema_dispose_wrap_postfix(allocator_t *out,
                                         program_t *program,
                                         node_t *operand,
                                         const char *op,
                                         const location_t *loc) {
  node_t *wrap = create_binary_node(out);
  node_set_location(wrap, node_get_location(operand));
  binary_node_set_left(wrap, operand);
  binary_node_set_symbol(wrap, sema_dispose_symbol(out, program, op, loc));
  return wrap;
}

/* CALL 语句包装：STMT_EXPR(EXPR_CALL(callee, args...))。 */
static node_t *
sema_dispose_call_stmt(allocator_t *out, node_t *call, const location_t *loc) {
  node_set_location(call, loc);
  node_t *wrap = create_stmt_expr_node(out);
  node_set_location(wrap, loc);
  stmt_expr_node_set_expr(wrap, call);
  return wrap;
}

/* "$dispose$" + mangled(类型名) → intern identifier 节点。 */
static node_t *sema_dispose_callee_node(allocator_t *out,
                                        program_t *program,
                                        clux_type_t *type,
                                        const location_t *loc) {
  const char *type_name = type ? clux_type_get_name(type) : NULL;
  if (!type_name) {
    return NULL;
  }
  const char *callee = sema_dispose_callee_name(out, program, type_name);
  return callee ? sema_dispose_ident(out, program, callee, loc) : NULL;
}

/* self.<name> 成员访问（self 是 *T，成员访问经 ptr 委托解引用）。 */
static node_t *sema_dispose_member_access(allocator_t *out,
                                          program_t *program,
                                          const char *name,
                                          const location_t *loc) {
  return sema_dispose_binary(out,
                             program,
                             sema_dispose_ident(out, program, "self", loc),
                             ".",
                             sema_dispose_ident(out, program, name, loc),
                             loc);
}

/* $dispose$<target_type>(<target>.&) 语句。 */
static node_t *sema_dispose_call_of(allocator_t *out,
                                    program_t *program,
                                    clux_type_t *target_type,
                                    node_t *target,
                                    const location_t *loc) {
  node_t *callee = sema_dispose_callee_node(out, program, target_type, loc);
  if (!callee || !target) {
    return NULL;
  }
  node_t *call = create_call_node(out);
  node_set_location(call, loc);
  call_node_set_callee(call, callee);
  call_node_add_arg(call,
                    sema_dispose_wrap_postfix(out, program, target, ".&", loc));
  return sema_dispose_call_stmt(out, call, loc);
}

/* if (cond) { stmt } 语句（THEN 为单语句块）。 */
static node_t *sema_dispose_wrap_if(allocator_t *out,
                                    node_t *cond,
                                    node_t *stmt,
                                    const location_t *loc) {
  node_t *then = create_stmt_block_node(out);
  node_set_location(then, loc);
  node_add_child(then, stmt);
  node_t *if_stmt = create_stmt_if_node(out);
  node_set_location(if_stmt, loc);
  stmt_if_node_set_condition(if_stmt, cond);
  stmt_if_node_set_then(if_stmt, then);
  stmt_if_node_set_else(if_stmt, NULL);
  return if_stmt;
}

/* ---- 类型表达式重建（合成参数类型 *T 的 T 部分） ---- */

node_t *sema_dispose_render_type(allocator_t *out,
                                 program_t *program,
                                 clux_type_t *type,
                                 const location_t *loc);

/* 前缀类型节点（*T / ?T / []T / const T）：exec 前缀通道经 interning
   还原同一复合类型实例。base 不可名时返回 NULL。 */
static node_t *sema_dispose_type_prefix(allocator_t *out,
                                        program_t *program,
                                        const char *op,
                                        clux_type_t *base,
                                        const location_t *loc) {
  node_t *base_node = sema_dispose_render_type(out, program, base, loc);
  if (!base_node) {
    return NULL;
  }
  return sema_dispose_prefix_expr(out, program, op, base_node, loc);
}

/* clux_type_t → 类型表达式 AST：按 kind 递归重建（具名叶子 identifier、
   指针/optional/切片/限定前缀、[N]T 数组二元形态、元组节点）。
   失败（无名叶子、零元元组）返回 NULL，调用方放弃该展开。 */
node_t *sema_dispose_render_type(allocator_t *out,
                                 program_t *program,
                                 clux_type_t *type,
                                 const location_t *loc) {
  if (!type) {
    return NULL;
  }
  switch (clux_type_get_kind(type)) {
  case CLUX_TYPE_PTR:
    return sema_dispose_type_prefix(
        out, program, "*", clux_type_get_ptr_base(type), loc);
  case CLUX_TYPE_CONST:
    return sema_dispose_type_prefix(
        out, program, "const", clux_type_unwrap(type), loc);
  case CLUX_TYPE_VOLATILE:
    return sema_dispose_type_prefix(
        out, program, "volatile", clux_type_unwrap(type), loc);
  case CLUX_TYPE_OPTIONAL:
    return sema_dispose_type_prefix(
        out, program, "?", clux_type_get_optional_base(type), loc);
  case CLUX_TYPE_SLICE:
    return sema_dispose_type_prefix(
        out, program, "[]", clux_slice_get_base(type), loc);
  case CLUX_TYPE_ARRAY: {
    /* [N]T 二元形态：SYMBOL 文本 "[N]"（exec 按首尾方括号判别），
       LEFT=长度数值叶子，RIGHT=元素类型。 */
    size_t len = clux_array_get_length(type);
    node_t *elem =
        sema_dispose_render_type(out, program, clux_array_get_base(type), loc);
    node_t *length = sema_dispose_numeric(out, program, len, loc);
    if (!elem || !length) {
      return NULL;
    }
    char buf[32];
    int written = snprintf(buf, sizeof(buf), "[%zu]", len);
    if (written <= 0 || (size_t)written >= sizeof(buf)) {
      return NULL;
    }
    return sema_dispose_binary(out, program, length, buf, elem, loc);
  }
  case CLUX_TYPE_TUPLE: {
    size_t count = clux_tuple_elem_count(type);
    if (!count) {
      return NULL;
    }
    node_t *tuple = create_tuple_node(out);
    node_set_location(tuple, loc);
    for (size_t idx = 0; idx < count; idx++) {
      node_t *elem = sema_dispose_render_type(
          out, program, clux_tuple_get_elem(type, idx), loc);
      if (!elem) {
        return NULL;
      }
      tuple_node_add_elem(tuple, elem);
    }
    return tuple;
  }
  default: {
    const char *name = clux_type_get_name(type);
    return name ? sema_dispose_ident(out, program, name, loc) : NULL;
  }
  }
}

/* ---- 按 kind 的展开体（§4.17.2 展开表） ---- */

/* #name(arg) 内建调用节点（名字为去 '#' 的 SYMBOL，与 parser 同构）。 */
static node_t *sema_dispose_builtin(allocator_t *out,
                                    program_t *program,
                                    const char *name,
                                    node_t *arg,
                                    const location_t *loc) {
  node_t *builtin = create_builtin_node(out);
  node_set_location(builtin, loc);
  builtin_node_set_name(builtin, sema_dispose_symbol(out, program, name, loc));
  builtin_node_add_arg(builtin, arg);
  return builtin;
}

/* ?T 展开体：if (!#is_none(self.*)) { $dispose$B(#take(self.*).&) }
   （§4.17.2：none 无操作，some 析构载荷）。 */
static void sema_dispose_emit_optional(allocator_t *out,
                                       program_t *program,
                                       node_t *block,
                                       clux_type_t *type,
                                       const location_t *loc) {
  clux_type_t *base = clux_type_get_optional_base(type);
  if (!base) {
    return;
  }
  node_t *payload = sema_dispose_builtin(
      out,
      program,
      "take",
      sema_dispose_wrap_postfix(out,
                                program,
                                sema_dispose_ident(out, program, "self", loc),
                                ".*",
                                loc),
      loc);
  node_t *call = sema_dispose_call_of(out, program, base, payload, loc);
  if (!call) {
    return;
  }
  node_t *cond = sema_dispose_prefix_expr(
      out,
      program,
      "!",
      sema_dispose_builtin(out,
                           program,
                           "is_none",
                           sema_dispose_wrap_postfix(
                               out,
                               program,
                               sema_dispose_ident(out, program, "self", loc),
                               ".*",
                               loc),
                           loc),
      loc);
  node_add_child(block, sema_dispose_wrap_if(out, cond, call, loc));
}

/* [N]T 展开体（§4.17.2）：N 为编译期常量，逆序固定循环——
   for (var i = N; i > 0; i = i - 1) { $dispose$E(self.*[i - 1].&) }。 */
static void sema_dispose_emit_array(allocator_t *out,
                                    program_t *program,
                                    node_t *block,
                                    clux_type_t *type,
                                    const location_t *loc) {
  size_t len = clux_array_get_length(type);
  clux_type_t *base = clux_array_get_base(type);
  if (!len || !base) {
    return;
  }
  /* 循环体内：self.*[i - 1].& → $dispose$E(...)。 */
  node_t *index =
      sema_dispose_binary(out,
                          program,
                          sema_dispose_ident(out, program, "i", loc),
                          "-",
                          sema_dispose_numeric(out, program, 1, loc),
                          loc);
  node_t *sub = create_subscript_node(out);
  node_set_location(sub, loc);
  subscript_node_set_base(
      sub,
      sema_dispose_wrap_postfix(out,
                                program,
                                sema_dispose_ident(out, program, "self", loc),
                                ".*",
                                loc));
  subscript_node_add_arg(sub, index);
  node_t *body_stmt = sema_dispose_call_of(out, program, base, sub, loc);
  if (!body_stmt) {
    return;
  }
  node_t *body = create_stmt_block_node(out);
  node_set_location(body, loc);
  node_add_child(body, body_stmt);
  /* var i = N; */
  node_t *init = create_stmt_var_node(out);
  node_set_location(init, loc);
  stmt_var_node_set_keyword(init,
                            sema_dispose_symbol(out, program, "var", loc));
  stmt_var_node_set_name(init, sema_dispose_ident(out, program, "i", loc));
  stmt_var_node_set_type(init, NULL);
  stmt_var_node_set_value(init, sema_dispose_numeric(out, program, len, loc));
  stmt_var_node_set_export(init, NULL);
  stmt_var_node_set_comptime(init, NULL);
  /* i > 0；i = i - 1。 */
  node_t *cond = sema_dispose_binary(out,
                                     program,
                                     sema_dispose_ident(out, program, "i", loc),
                                     ">",
                                     sema_dispose_numeric(out, program, 0, loc),
                                     loc);
  node_t *i_ident = sema_dispose_ident(out, program, "i", loc);
  node_t *update = create_assign_node(out);
  node_set_location(update, loc);
  assign_node_set_left(update, i_ident);
  assign_node_set_symbol(update, sema_dispose_symbol(out, program, "=", loc));
  assign_node_set_value(
      update,
      sema_dispose_binary(out,
                          program,
                          sema_dispose_ident(out, program, "i", loc),
                          "-",
                          sema_dispose_numeric(out, program, 1, loc),
                          loc));
  node_t *loop = create_stmt_for_node(out);
  node_set_location(loop, loc);
  stmt_for_node_set_init(loop, init);
  stmt_for_node_set_condition(loop, cond);
  stmt_for_node_set_update(loop, update);
  stmt_for_node_set_body(loop, body);
  node_add_child(block, loop);
}

/* 合成顶层语句 func $dispose$T(self: *T): void { ... }——按 kind 分派
   展开体；参数类型 *T 经 render_type 重建（interning 还原同一实例）。 */
node_t *sema_dispose_synth(sema_clone_env_t *env,
                           const sema_dispose_type_t *entry) {
  clux_type_t *type = entry ? entry->type : NULL;
  const char *name = type ? clux_type_get_name(type) : NULL;
  if (!name) {
    return NULL;
  }
  allocator_t *out = env->out;
  program_t *program = env->program;
  const location_t *loc = &entry->loc;
  node_t *block = create_stmt_block_node(out);
  node_set_location(block, loc);
  clux_type_kind_t kind = clux_type_kind_unwrap(type);
  if (kind == CLUX_TYPE_STRUCT) {
    /* 钩子（若有）先执行：Type$__dispose__(self)——指针 self 直传，
       按值 self 传 self.*（方法本体已由 §2.10 提升为顶层函数）。 */
    sema_method_t *hook = sema_dispose_find_hook(env->methods, name);
    if (hook) {
      node_t *call = create_call_node(out);
      call_node_set_callee(
          call, sema_dispose_ident(out, program, hook->mangled, loc));
      node_t *self = sema_dispose_ident(out, program, "self", loc);
      if (!hook->self_ptr) {
        self = sema_dispose_wrap_postfix(out, program, self, ".*", loc);
      }
      call_node_add_arg(call, self);
      node_add_child(block, sema_dispose_call_stmt(out, call, loc));
    }
    /* 逆声明序递归析构 disposable 字段：$dispose$F(self.f.&)——字段
       类型可为容器（谓词 P2 扩展后 ?T/[N]T/union/元组同样命中）。 */
    size_t count = clux_struct_field_count(type);
    for (size_t f = count; f > 0; f--) {
      const clux_struct_field_t *field = clux_struct_get_field(type, f - 1);
      if (!field || !sema_type_needs_dispose(env->sema, field->type)) {
        continue;
      }
      node_t *call = sema_dispose_call_of(
          out,
          program,
          field->type,
          sema_dispose_member_access(out, program, field->name, loc),
          loc);
      if (call) {
        node_add_child(block, call);
      }
    }
  } else if (kind == CLUX_TYPE_OPTIONAL) {
    sema_dispose_emit_optional(out, program, block, type, loc);
  } else if (kind == CLUX_TYPE_ARRAY) {
    sema_dispose_emit_array(out, program, block, type, loc);
  } else if (kind == CLUX_TYPE_UNION) {
    size_t count = clux_union_member_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_union_member_t *member = clux_union_get_member(type, idx);
      if (!member || !sema_type_needs_dispose(env->sema, member->type)) {
        continue;
      }
      node_t *call = sema_dispose_call_of(
          out,
          program,
          member->type,
          sema_dispose_member_access(out, program, member->name, loc),
          loc);
      if (!call) {
        continue;
      }
      node_t *test = sema_dispose_binary(
          out,
          program,
          sema_dispose_wrap_postfix(
              out,
              program,
              sema_dispose_ident(out, program, "self", loc),
              ".*",
              loc),
          "is",
          sema_dispose_ident(out, program, member->name, loc),
          loc);
      node_add_child(block, sema_dispose_wrap_if(out, test, call, loc));
    }
  } else if (kind == CLUX_TYPE_TUPLE) {
    size_t count = clux_tuple_elem_count(type);
    for (size_t e = count; e > 0; e--) {
      clux_type_t *elem = clux_tuple_get_elem(type, e - 1);
      if (!elem || !sema_type_needs_dispose(env->sema, elem)) {
        continue;
      }
      node_t *sub = create_subscript_node(out);
      node_set_location(sub, loc);
      subscript_node_set_base(sub,
                              sema_dispose_wrap_postfix(
                                  out,
                                  program,
                                  sema_dispose_ident(out, program, "self", loc),
                                  ".*",
                                  loc));
      subscript_node_add_arg(sub,
                             sema_dispose_numeric(out, program, e - 1, loc));
      node_t *call = sema_dispose_call_of(out, program, elem, sub, loc);
      if (call) {
        node_add_child(block, call);
      }
    }
  } else {
    return NULL; /* 谓词保证不可达（cunion/基础/指针等不登记） */
  }
  /* func $dispose$T(self: *T): void { ... } 的声明本体（槽序与 defer
     闭包合成一致）。 */
  node_t *fn = create_func_decl_node(out);
  node_set_location(fn, loc);
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_KEYWORD,
                 sema_dispose_symbol(out, program, "func", loc));
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_NAME,
                 sema_dispose_callee_node(out, program, type, loc));
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_GENERICS, NULL);
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_CAPTURES, NULL);
  node_t *params = create_func_params_node(out);
  node_t *param = create_func_param_node(out);
  node_set_location(param, loc);
  func_param_node_set_name(param,
                           sema_dispose_ident(out, program, "self", loc));
  /* 参数类型 *T：前缀一元形态（SYMBOL="*"，RIGHT=类型表达式，经
     render_type 按 kind 递归重建）。 */
  node_t *param_type = sema_dispose_type_prefix(out, program, "*", type, loc);
  if (!param_type) {
    return NULL;
  }
  func_param_node_set_type(param, param_type);
  func_param_node_set_default(param, NULL);
  func_params_node_add_param(params, param);
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_PARAMS, params);
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_RETURN,
                 sema_dispose_ident(out, program, "void", loc));
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_BODY, block);
  /* 顶层语句包装（与匿名 struct 具名化同构：STMT_FUNC 外壳）。 */
  node_t *stmt = create_stmt_func_node(out);
  node_set_location(stmt, loc);
  stmt_func_node_set_func_decl(stmt, fn);
  stmt_func_node_set_export(stmt, NULL);
  stmt_func_node_set_comptime(stmt, NULL);
  return stmt;
}
