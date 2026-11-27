/* §4.17 RAII P1：自动析构通道。
   - disposable 谓词：struct 实现 __dispose__ 钩子，或传递性包含需要
     析构的字段（白皮书 §4.17.1）；指针/切片/基础类型/容器不析构
     （容器展开留 P2）。钩子判定走 sema->methods 预扫描表——check 期
     引擎 struct 类型只有字段表，method 三表在 exec 两阶段 install，
     clux_struct_is_method 不可用。
   - 类型收集：emit 期命中 disposable 使用点（局部 var / #dispose 实参）
     时登记，$dispose$T 合成体由 emit 尾部统一追加。
   - 合成体：func $dispose$T(self: *T): void { 钩子（若有）先执行；
     逆声明序递归析构 disposable 字段（$dispose$F(self.f.&)） }。
     合成体不经 check（合成内容全部由谓词保证可解析）。 */
#include "sema/sema_internal.h"
#include "core/vec.h"
#include "engine/ptr.h"
#include "engine/qualifier.h"
#include "engine/struct.h"
#include "engine/type.h"
#include "parser/node_binary.h"
#include "parser/node_call.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_func.h"
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

int sema_type_needs_dispose(clux_sema_t *sema, clux_type_t *type) {
  if (!sema || !type) {
    return 0;
  }
  /* P1 限 struct 世界：容器（?T、[N]T、切片、元组、union 族）不析构，
     其元素展开留 P2；限定包裹走 unwrap。 */
  if (clux_type_kind_unwrap(type) != CLUX_TYPE_STRUCT) {
    return 0;
  }
  const char *name = clux_type_get_name(type);
  if (!name) {
    return 0;
  }
  if (sema_dispose_find_hook(sema->methods, name)) {
    return 1;
  }
  /* 字段递归：直接包含 struct 字段才递归（经指针/容器不传导），
     递归深度由类型有限大小天然封顶。 */
  size_t count = clux_struct_field_count(type);
  for (size_t idx = 0; idx < count; idx++) {
    const clux_struct_field_t *field = clux_struct_get_field(type, idx);
    if (field && sema_type_needs_dispose(sema, field->type)) {
      return 1;
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
  /* 引擎类型同组合同一实例，指针恒等即去重（登记先入表再递归字段，
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
  /* 递归登记 disposable 字段：合成体内会调用 $dispose$F，F 须同样合成。 */
  if (clux_type_kind_unwrap(type) == CLUX_TYPE_STRUCT) {
    size_t count = clux_struct_field_count(type);
    for (size_t idx = 0; idx < count; idx++) {
      const clux_struct_field_t *field = clux_struct_get_field(type, idx);
      if (field && sema_type_needs_dispose(env->sema, field->type)) {
        sema_dispose_register(env, field->type, loc);
      }
    }
  }
}

/* "$dispose$<name>" → intern identifier 节点。 */
static node_t *sema_dispose_render_fn(allocator_t *out,
                                      program_t *program,
                                      const char *prefix,
                                      const char *name,
                                      const location_t *loc) {
  size_t len = strlen(prefix) + strlen(name) + 1;
  char buf[len];
  (void)snprintf(buf, len, "%s%s", prefix, name);
  const char *text = program_intern_string(program, buf, strlen(buf));
  return sema_render_text_node(
      out, program, CLUX_NODE_LITERAL_IDENTIFIER, text, loc);
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
  binary_node_set_symbol(
      wrap, sema_render_text_node(out, program, CLUX_NODE_SYMBOL, op, loc));
  (void)op;
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
  /* 钩子（若有）先执行：Type$__dispose__(self)——指针 self 直传，
     按值 self 传 self.*（方法本体已由 §2.10 提升为顶层函数）。 */
  sema_method_t *hook = sema_dispose_find_hook(env->methods, name);
  if (hook) {
    node_t *call = create_call_node(out);
    call_node_set_callee(
        call,
        sema_render_text_node(
            out, program, CLUX_NODE_LITERAL_IDENTIFIER, hook->mangled, loc));
    node_t *self = sema_render_text_node(
        out, program, CLUX_NODE_LITERAL_IDENTIFIER, "self", loc);
    if (!hook->self_ptr) {
      self = sema_dispose_wrap_postfix(out, program, self, ".*", loc);
    }
    call_node_add_arg(call, self);
    node_add_child(block, sema_dispose_call_stmt(out, call, loc));
  }
  /* 逆声明序递归析构 disposable 字段：$dispose$F(self.f.&)。 */
  size_t count = clux_struct_field_count(type);
  for (size_t f = count; f > 0; f--) {
    const clux_struct_field_t *field = clux_struct_get_field(type, f - 1);
    if (!field || !sema_type_needs_dispose(env->sema, field->type)) {
      continue;
    }
    const char *field_type_name = clux_type_get_name(field->type);
    if (!field_type_name) {
      continue;
    }
    node_t *call = create_call_node(out);
    call_node_set_callee(call,
                         sema_dispose_render_fn(
                             out, program, "$dispose$", field_type_name, loc));
    /* self.<field> 后取址：. 成员访问 + .& 后缀。 */
    node_t *member = create_binary_node(out);
    node_set_location(member, loc);
    binary_node_set_left(
        member,
        sema_render_text_node(
            out, program, CLUX_NODE_LITERAL_IDENTIFIER, "self", loc));
    binary_node_set_symbol(
        member,
        sema_render_text_node(out, program, CLUX_NODE_SYMBOL, ".", loc));
    binary_node_set_right(
        member,
        sema_render_text_node(
            out, program, CLUX_NODE_LITERAL_IDENTIFIER, field->name, loc));
    call_node_add_arg(
        call, sema_dispose_wrap_postfix(out, program, member, ".&", loc));
    node_add_child(block, sema_dispose_call_stmt(out, call, loc));
  }
  /* func $dispose$T(self: *T): void { ... } 的声明本体（槽序与 defer
     闭包合成一致）。 */
  node_t *fn = create_func_decl_node(out);
  node_set_location(fn, loc);
  node_set_child(
      fn,
      CLUX_EXPR_FUNC_DECL_KEYWORD,
      sema_render_text_node(out, program, CLUX_NODE_SYMBOL, "func", loc));
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_NAME,
                 sema_dispose_render_fn(out, program, "$dispose$", name, loc));
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_GENERICS, NULL);
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_CAPTURES, NULL);
  node_t *params = create_func_params_node(out);
  node_t *param = create_func_param_node(out);
  node_set_location(param, loc);
  func_param_node_set_name(
      param,
      sema_render_text_node(
          out, program, CLUX_NODE_LITERAL_IDENTIFIER, "self", loc));
  /* 参数类型 *T：前缀一元形态（SYMBOL="*"，RIGHT=类型名，LEFT 缺失）。 */
  node_t *ptr_type = create_binary_node(out);
  node_set_location(ptr_type, loc);
  binary_node_set_symbol(
      ptr_type,
      sema_render_text_node(out, program, CLUX_NODE_SYMBOL, "*", loc));
  binary_node_set_right(
      ptr_type,
      sema_render_text_node(
          out, program, CLUX_NODE_LITERAL_IDENTIFIER, name, loc));
  func_param_node_set_type(param, ptr_type);
  func_param_node_set_default(param, NULL);
  func_params_node_add_param(params, param);
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_PARAMS, params);
  node_set_child(fn,
                 CLUX_EXPR_FUNC_DECL_RETURN,
                 sema_render_text_node(
                     out, program, CLUX_NODE_LITERAL_IDENTIFIER, "void", loc));
  node_set_child(fn, CLUX_EXPR_FUNC_DECL_BODY, block);
  /* 顶层语句包装（与匿名 struct 具名化同构：STMT_FUNC 外壳）。 */
  node_t *stmt = create_stmt_func_node(out);
  node_set_location(stmt, loc);
  stmt_func_node_set_func_decl(stmt, fn);
  stmt_func_node_set_export(stmt, NULL);
  stmt_func_node_set_comptime(stmt, NULL);
  return stmt;
}
