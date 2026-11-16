/* 函数声明落地：签名（参数/返回/捕获表）求值后经
   clux_context_create_func_type 建型，create_func 造零值，AST 挂
   声明节点，闭包捕获按值拷贝写穿捕获 view（clux 值传递约定：
   捕获/返回/传参一律 copy，声明时快照）。具名函数语句经
   declare_value 具名化；匿名闭包（表达式位置）直接产出 func value。 */
#include "exec_internal.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_stmt_func.h"
#include <string.h>

exec_value_t exec_visit_func_stmt(clux_exec_t *self, node_t *node) {
  node_t *decl = stmt_func_node_get_func_decl(node);
  if (!decl) {
    return exec_errorf(
        self, exec_loc(node), "function statement is incomplete");
  }
  return exec_visit_func_decl(self, decl);
}

/* 参数类型序列：每项求值为 clux_type_t*，写入调用方提供的数组。 */
static exec_value_t
eval_param_types(clux_exec_t *self, node_t *params_node, clux_type_t **out) {
  size_t count = func_params_node_get_param_count(params_node);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *param = func_params_node_get_param(params_node, idx);
    exec_value_t error;
    out[idx] = exec_eval_type(self, func_param_node_get_type(param), &error);
    if (!out[idx]) {
      return error;
    }
  }
  return exec_ok(NULL);
}

/* 函数声明捕获项节点：捕获列表 |( name: type = expr, ... )| 的唯一
   child 是一个 FUNC_PARAMS，捕获项复用 FUNC_PARAM 槽位（NAME/TYPE）。 */
static node_t *captures_params_of(node_t *captures_node) {
  size_t count = func_captures_node_get_capture_count(captures_node);
  for (size_t idx = 0; idx < count; idx++) {
    node_t *child = func_captures_node_get_capture(captures_node, idx);
    if (node_get_kind(child) == CLUX_NODE_FUNC_PARAMS) {
      return child;
    }
  }
  return NULL;
}

/* 捕获表：捕获类型必填（声明语法 |( name: type = expr )| 类型不可省略），
   捕获类型与捕获名一并写入签名数组（name 为临时副本，调用方释放）。 */
static exec_value_t eval_captures(clux_exec_t *self,
                                  node_t *capture_params_node,
                                  clux_func_capture_t *out,
                                  size_t count) {
  for (size_t idx = 0; idx < count; idx++) {
    node_t *capture = func_params_node_get_param(capture_params_node, idx);
    node_t *name_node = func_param_node_get_name(capture);
    node_t *type_node = func_param_node_get_type(capture);
    if (!name_node) {
      return exec_errorf(self, exec_loc(capture), "capture requires a name");
    }
    if (!type_node) {
      return exec_errorf(self, exec_loc(capture), "capture requires a type");
    }
    char *name = exec_node_text(self, name_node);
    exec_value_t error;
    out[idx].type = exec_eval_type(self, type_node, &error);
    if (!out[idx].type) {
      allocator_free(self->allocator, (void **)&name);
      return error;
    }
    out[idx].name = name;
  }
  return exec_ok(NULL);
}

/* 捕获字段按值拷贝：逐项取捕获 view 后 assign 写穿——源值是声明时
   当前作用域同名值的快照（按值捕获）。 */
static exec_value_t copy_captures(clux_exec_t *self,
                                  node_t *node,
                                  clux_value_t *fn,
                                  const clux_func_capture_t *captures,
                                  size_t count) {
  clux_scope_t *scope = clux_context_get_scope(self->ctx);
  location_t *loc = exec_loc(node);
  for (size_t idx = 0; idx < count; idx++) {
    clux_value_t *view =
        clux_context_func_capture(self->ctx, fn, captures[idx].name, loc);
    if (exec_is_error_value(view)) {
      return exec_err(view);
    }
    clux_value_t *source = clux_scope_get(scope, captures[idx].name);
    if (!source) {
      return exec_errorf(self, loc, "undefined name '%s'", captures[idx].name);
    }
    clux_value_t *written = clux_context_assign(self->ctx, view, source, loc);
    if (exec_is_error_value(written)) {
      return exec_err(written);
    }
  }
  return exec_ok(NULL);
}

exec_value_t exec_visit_func_decl(clux_exec_t *self, node_t *node) {
  node_t *keyword = func_decl_node_get_keyword(node);
  if (exec_node_is(keyword, "cfunc")) {
    return exec_errorf(self, exec_loc(node), "cfunc has no executable body");
  }
  if (func_decl_node_get_generics(node)) {
    return exec_errorf(self,
                       exec_loc(node),
                       "generic function is not supported in script mode");
  }
  node_t *name_node = func_decl_node_get_name(node);
  node_t *captures_node = func_decl_node_get_captures(node);
  node_t *params_node = func_decl_node_get_params(node);
  node_t *return_node = func_decl_node_get_return(node);

  exec_value_t error;
  exec_value_t failed = exec_ok(NULL);
  clux_type_t *return_type = exec_eval_type(self, return_node, &error);
  if (!return_type) {
    return error;
  }

  size_t param_count =
      params_node ? func_params_node_get_param_count(params_node) : 0;
  node_t *capture_params_node =
      captures_node ? captures_params_of(captures_node) : NULL;
  size_t capture_count =
      capture_params_node
          ? func_params_node_get_param_count(capture_params_node)
          : 0;
  clux_type_t **param_types =
      param_count
          ? allocator_alloc(self->allocator, param_count * sizeof(*param_types))
          : NULL;
  /* 捕获名是 exec_node_text 临时副本：数组清零保证清理只释放已填充槽。 */
  clux_func_capture_t *captures =
      capture_count
          ? allocator_alloc(self->allocator, capture_count * sizeof(*captures))
          : NULL;
  if (captures) {
    memset(captures, 0, capture_count * sizeof(*captures));
  }

  if (param_count) {
    failed = eval_param_types(self, params_node, param_types);
    if (exec_is_error(failed)) {
      goto cleanup;
    }
  }
  if (capture_count) {
    failed = eval_captures(self, capture_params_node, captures, capture_count);
    if (exec_is_error(failed)) {
      goto cleanup;
    }
  }

  char *decl_name = name_node ? exec_node_text(self, name_node) : NULL;
  clux_type_t *ftype =
      clux_context_create_func_type(self->ctx,
                                    decl_name ? decl_name : "<closure>",
                                    captures,
                                    capture_count,
                                    param_types,
                                    param_count,
                                    return_type);
  if (!ftype) {
    /* create_func_type 已在 error 通道记录原因，这里补声明定位。 */
    failed = exec_errorf(self,
                         exec_loc(node),
                         "function signature for '%s' is invalid",
                         decl_name ? decl_name : "<closure>");
    allocator_free(self->allocator, (void **)&decl_name);
    goto cleanup;
  }
  clux_value_t *fn =
      clux_context_create_func(self->ctx, ftype, NULL, exec_loc(node));
  allocator_free(self->allocator, (void **)&decl_name);
  if (exec_is_error_value(fn)) {
    failed = exec_err(fn);
    goto cleanup;
  }
  clux_value_t *bound =
      clux_context_func_set_ast(self->ctx, fn, node, exec_loc(node));
  if (exec_is_error_value(bound)) {
    failed = exec_err(bound);
    goto cleanup;
  }
  failed = copy_captures(self, node, fn, captures, capture_count);
  if (exec_is_error(failed)) {
    goto cleanup;
  }

  /* 具名函数语句：具名化落地（声明语句不产值，与 var 一致）；
     匿名闭包（表达式位置）：func value 本身即产物。 */
  if (name_node) {
    char *name = exec_node_text(self, name_node);
    clux_value_t *declared =
        clux_context_declare_value(self->ctx, fn, name, exec_loc(node));
    allocator_free(self->allocator, (void **)&name);
    if (exec_is_error_value(declared)) {
      failed = exec_err(declared);
      goto cleanup;
    }
    failed = exec_ok(NULL);
  } else {
    failed = exec_ok(fn);
  }

cleanup:
  if (captures) {
    for (size_t idx = 0; idx < capture_count; idx++) {
      allocator_free(self->allocator, (void **)&captures[idx].name);
    }
  }
  allocator_free(self->allocator, (void **)&captures);
  allocator_free(self->allocator, (void **)&param_types);
  return failed;
}
