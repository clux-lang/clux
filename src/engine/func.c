#include "engine/func.h"
#include "core/allocator.h"
#include "engine/context.h"
#include "engine/interface.h"
#include "engine/type.h"
#include "engine/value.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct _clux_func_meta_t {
  clux_func_capture_t *captures;
  size_t capture_count;
  clux_type_t **params;
  size_t param_count;
  clux_type_t *return_type;
} clux_func_meta_t;

static clux_func_meta_t *func_meta(clux_type_t *type) {
  return clux_type_get_meta(type);
}

static bool func_kind(clux_type_kind_t kind) { return kind == CLUX_TYPE_FUNC; }

static bool is_error_value(clux_value_t *value) {
  return clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR;
}

size_t clux_func_capture_count(clux_type_t *type) {
  clux_func_meta_t *meta = type ? func_meta(type) : NULL;
  return meta ? meta->capture_count : 0;
}

const clux_func_capture_t *clux_func_get_capture(clux_type_t *type,
                                                 size_t idx) {
  clux_func_meta_t *meta = type ? func_meta(type) : NULL;
  if (!meta || idx >= meta->capture_count) {
    return NULL;
  }
  return &meta->captures[idx];
}

const clux_func_capture_t *clux_func_find_capture(clux_type_t *type,
                                                  const char *name) {
  clux_func_meta_t *meta = type ? func_meta(type) : NULL;
  if (!meta || !name) {
    return NULL;
  }
  for (size_t idx = 0; idx < meta->capture_count; idx++) {
    if (strcmp(meta->captures[idx].name, name) == 0) {
      return &meta->captures[idx];
    }
  }
  return NULL;
}

const clux_func_capture_t *clux_func_get_capture_table(clux_type_t *type) {
  clux_func_meta_t *meta = type ? func_meta(type) : NULL;
  return meta ? meta->captures : NULL;
}

/* 捕获字段从类型对齐处开始平铺（各自按自身对齐取偏移）：
   payload 偏移恒等于类型对齐值——align >= sizeof(void*) 且为 2 的幂，
   round_up(sizeof(void*), align) == align。 */
static size_t func_payload_offset(clux_type_t *type) {
  return clux_type_get_align(type);
}

size_t clux_func_capture_offset(clux_type_t *type, size_t idx) {
  clux_func_meta_t *meta = func_meta(type);
  size_t offset = func_payload_offset(type);
  for (size_t pos = 0; pos < idx && pos < meta->capture_count; pos++) {
    size_t cap_align = clux_type_get_align(meta->captures[pos].type);
    size_t cap_size = clux_type_get_size(meta->captures[pos].type);
    offset = (offset + cap_align - 1) / cap_align * cap_align + cap_size;
  }
  size_t cap_align = clux_type_get_align(meta->captures[idx].type);
  return (offset + cap_align - 1) / cap_align * cap_align;
}

size_t clux_func_param_count(clux_type_t *type) {
  clux_func_meta_t *meta = type ? func_meta(type) : NULL;
  return meta ? meta->param_count : 0;
}

clux_type_t *clux_func_get_param(clux_type_t *type, size_t idx) {
  clux_func_meta_t *meta = type ? func_meta(type) : NULL;
  if (!meta || idx >= meta->param_count) {
    return NULL;
  }
  return meta->params[idx];
}

clux_type_t *clux_func_get_return_type(clux_type_t *type) {
  clux_func_meta_t *meta = type ? func_meta(type) : NULL;
  return meta ? meta->return_type : NULL;
}

size_t clux_func_ast_offset(void) { return 0; }

void *clux_func_get_ast(clux_value_t *value) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(void **)raw : NULL;
}

void clux_func_set_ast(clux_value_t *value, void *ast) {
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  if (raw) {
    *(void **)raw = ast;
  }
}

/* 结构相等核心：捕获表 + 参数序列 + 返回类型按声明序一一对应即相等。 */
static bool signature_equal(clux_func_meta_t *self,
                            const clux_func_capture_t *captures,
                            size_t capture_count,
                            clux_type_t **params,
                            size_t param_count,
                            clux_type_t *return_type) {
  if (!self || self->capture_count != capture_count ||
      self->param_count != param_count ||
      !clux_type_equal(self->return_type, return_type)) {
    return false;
  }
  for (size_t idx = 0; idx < capture_count; idx++) {
    if (strcmp(self->captures[idx].name, captures[idx].name) != 0 ||
        !clux_type_equal(self->captures[idx].type, captures[idx].type)) {
      return false;
    }
  }
  for (size_t idx = 0; idx < param_count; idx++) {
    if (!clux_type_equal(self->params[idx], params[idx])) {
      return false;
    }
  }
  return true;
}

bool clux_func_signature_equal(clux_type_t *self,
                               const clux_func_capture_t *captures,
                               size_t capture_count,
                               clux_type_t **params,
                               size_t param_count,
                               clux_type_t *return_type) {
  if (!func_kind(clux_type_get_kind(self))) {
    return false;
  }
  return signature_equal(func_meta(self),
                         captures,
                         capture_count,
                         params,
                         param_count,
                         return_type);
}

static bool func_type_equal(clux_type_t *self, clux_type_t *other) {
  if (!func_kind(clux_type_get_kind(other))) {
    return false;
  }
  clux_func_meta_t *other_meta = func_meta(other);
  return signature_equal(func_meta(self),
                         other_meta ? other_meta->captures : NULL,
                         other_meta ? other_meta->capture_count : 0,
                         other_meta ? other_meta->params : NULL,
                         other_meta ? other_meta->param_count : 0,
                         other_meta ? other_meta->return_type : NULL);
}

/* 调用（f(args)，白皮书 §2.7 func 值）：按签名校验实参个数与类型。
   shadow 函数（任一 shadow：函数或实参）产出返回类型的 shadow——
   调用结果的形态是静态可知的。校验通过的真函数分派给挂在 context
   上的 AST 执行器回调（帧协议住 exec 侧，engine 不依赖 exec）；
   未挂 AST 或未装执行器时报运行期 error。 */
static clux_value_t *func_invoke(clux_context_t *ctx,
                                 clux_value_t *value,
                                 size_t argc,
                                 clux_value_t *argv[]) {
  clux_type_t *type = clux_value_get_type(value);
  clux_func_meta_t *meta = func_meta(type);
  if (argc != meta->param_count) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "function expects %zu argument%s, got %zu",
        meta->param_count,
        meta->param_count == 1 ? "" : "s",
        argc);
  }
  bool shadow = clux_value_is_shadow(value);
  for (size_t idx = 0; idx < argc; idx++) {
    if (is_error_value(argv[idx])) {
      return argv[idx];
    }
    if (clux_value_is_shadow(argv[idx])) {
      shadow = true;
      continue;
    }
    if (!clux_type_equal(clux_value_get_type(argv[idx]), meta->params[idx])) {
      return clux_context_create_error_message(
          ctx,
          NULL,
          "function argument %zu requires type '%s', got '%s'",
          idx,
          clux_type_get_name(meta->params[idx])
              ? clux_type_get_name(meta->params[idx])
              : "<unknown>",
          clux_type_get_name(clux_value_get_type(argv[idx]))
              ? clux_type_get_name(clux_value_get_type(argv[idx]))
              : "<unknown>");
    }
  }
  if (shadow) {
    return clux_context_create_shadow_value(ctx, meta->return_type);
  }
  void *ast = clux_func_get_ast(value);
  clux_ast_executor_t executor = clux_context_get_executor(ctx);
  if (ast && executor) {
    return executor(ctx, ast, value, argc, argv, NULL);
  }
  return clux_context_create_error_message(
      ctx, NULL, "function call has no executable environment attached");
}

/* 同类型比较：函数身份是 AST 指针，捕获环境按字段递归比较
   （复用捕获类型 vtable 的 eq 槽位）。 */
static clux_value_t *func_compare(clux_context_t *ctx,
                                  clux_value_t *left,
                                  clux_value_t *right,
                                  clux_binary_op_t op) {
  if (is_error_value(left)) {
    return left;
  }
  if (is_error_value(right)) {
    return right;
  }
  clux_type_t *type = clux_value_get_type(left);
  if (!func_kind(clux_type_get_kind(clux_value_get_type(right))) ||
      !clux_type_equal(type, clux_value_get_type(right))) {
    return clux_context_create_error_message(
        ctx,
        NULL,
        "operator '%s' requires operands of the same type",
        op == CLUX_OP_EQ ? "==" : "!=");
  }
  clux_type_t *bool_type = clux_context_builtin_type(ctx, CLUX_TYPE_BOOL);
  if (clux_value_is_shadow(left) || clux_value_is_shadow(right)) {
    return clux_context_create_shadow_value(ctx, bool_type);
  }
  void *left_raw = ptr_to_raw(clux_value_get_data(left), 0);
  void *right_raw = ptr_to_raw(clux_value_get_data(right), 0);
  if (!left_raw || !right_raw) {
    return clux_context_create_error_message(
        ctx, NULL, "cannot compare invalid function values");
  }
  bool equal = *(void **)left_raw == *(void **)right_raw;
  if (equal) {
    clux_func_meta_t *meta = func_meta(type);
    for (size_t idx = 0; idx < meta->capture_count; idx++) {
      clux_vtable_t *cap_vt = clux_type_get_vtable(meta->captures[idx].type);
      if (!cap_vt || !cap_vt->eq) {
        return clux_context_create_error_message(
            ctx,
            NULL,
            "function capture '%s' of type '%s' does not support operator '=='",
            meta->captures[idx].name,
            clux_type_get_name(meta->captures[idx].type)
                ? clux_type_get_name(meta->captures[idx].type)
                : "<unknown>");
      }
      ptr_handle_t left_view = ptr_offset(clux_value_get_data(left),
                                          clux_func_capture_offset(type, idx));
      ptr_handle_t right_view = ptr_offset(clux_value_get_data(right),
                                           clux_func_capture_offset(type, idx));
      if (!left_view || !right_view) {
        return clux_context_create_error_message(
            ctx, NULL, "cannot compare invalid function values");
      }
      clux_value_t *left_cap = clux_context_create_value(
          ctx, meta->captures[idx].type, left_view, NULL, NULL);
      clux_value_t *right_cap = clux_context_create_value(
          ctx, meta->captures[idx].type, right_view, NULL, NULL);
      clux_value_t *result = cap_vt->eq(ctx, left_cap, right_cap);
      if (is_error_value(result)) {
        return result;
      }
      void *raw = ptr_to_raw(clux_value_get_data(result), 0);
      if (!raw || !*(bool *)raw) {
        equal = false;
        break;
      }
    }
  }
  return clux_context_create_bool(
      ctx, op == CLUX_OP_EQ ? equal : !equal, NULL, NULL);
}

static clux_value_t *
func_eq(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return func_compare(ctx, left, right, CLUX_OP_EQ);
}

static clux_value_t *
func_ne(clux_context_t *ctx, clux_value_t *left, clux_value_t *right) {
  return func_compare(ctx, left, right, CLUX_OP_NE);
}

static clux_vtable_t g_func_vtable = {
    .eq = func_eq,
    .ne = func_ne,
    .invoke = func_invoke,
    .type_equal = func_type_equal,
};

clux_vtable_t *get_clux_func_vtable(void) { return &g_func_vtable; }

static size_t func_layout_align(const clux_func_capture_t *captures,
                                size_t capture_count) {
  size_t align = sizeof(void *); /* 头部 AST 指针，对齐下限为指针宽度 */
  for (size_t idx = 0; idx < capture_count; idx++) {
    size_t cap_align = clux_type_get_align(captures[idx].type);
    if (cap_align > align) {
      align = cap_align;
    }
  }
  return align;
}

clux_type_t *create_clux_func_type(allocator_t *allocator,
                                   const char *name,
                                   const clux_func_capture_t *captures,
                                   size_t capture_count,
                                   clux_type_t **params,
                                   size_t param_count,
                                   clux_type_t *return_type) {
  /* 头部 AST 指针在偏移 0，捕获字段从对齐处开始平铺：
     align = max(sizeof(void*), max(捕获 align))，
     size = round_up(align + 捕获平铺总大小, align)。 */
  size_t align = func_layout_align(captures, capture_count);
  size_t payload_end = 0;
  for (size_t idx = 0; idx < capture_count; idx++) {
    size_t cap_align = clux_type_get_align(captures[idx].type);
    size_t cap_size = clux_type_get_size(captures[idx].type);
    payload_end =
        (payload_end + cap_align - 1) / cap_align * cap_align + cap_size;
  }
  size_t size = (align + payload_end + align - 1) / align * align;
  clux_type_t *type = create_clux_type(
      allocator, CLUX_TYPE_FUNC, name, size, align, get_clux_func_vtable());
  /* meta 与捕获表/参数表连体成单块分配：delete_clux_type 释放 meta
     本体即整块释放 */
  clux_func_meta_t *meta = clux_type_create_meta(
      type,
      sizeof(clux_func_meta_t) + sizeof(clux_func_capture_t) * capture_count +
          sizeof(clux_type_t *) * param_count);
  meta->captures =
      (clux_func_capture_t *)((char *)meta + sizeof(clux_func_meta_t));
  if (captures && capture_count > 0) {
    (void)memcpy(
        meta->captures, captures, sizeof(clux_func_capture_t) * capture_count);
  }
  meta->capture_count = capture_count;
  meta->params = (clux_type_t **)((char *)meta->captures +
                                  sizeof(clux_func_capture_t) * capture_count);
  if (params && param_count > 0) {
    (void)memcpy(meta->params, params, sizeof(clux_type_t *) * param_count);
  }
  meta->param_count = param_count;
  meta->return_type = return_type;
  return type;
}
