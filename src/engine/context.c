#include "engine/context.h"
#include "engine/array.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "core/rbtree.h"
#include "core/safe_alloc.h"
#include "core/vec.h"
#include "engine/integer.h"
#include "engine/interface.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/unsigned.h"
#include "engine/value.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine/bool.h"
#include "engine/cunion.h"
#include "engine/error.h"
#include "engine/floating.h"
#include "engine/func.h"
#include "engine/optional.h"
#include "engine/ptr.h"
#include "engine/qualifier.h"
#include "engine/slice.h"
#include "engine/str.h"
#include "engine/struct.h"
#include "engine/void.h"
#include "engine/template.h"
#include "engine/type_value.h"
#include "engine/tuple.h"
#include "engine/union.h"

struct _clux_context_t {
  allocator_t *allocator;
  safe_allocator_t *safe_allocator;
  vec_t *errors;
  rbtree_t *strs;
  clux_scope_t *global_scope;
  clux_scope_t *current_scope;
  /* AST 执行器（栈帧协议入口）：engine 只透传，帧语义住执行层。 */
  clux_ast_executor_t executor;
  /* 调用深度护栏：执行器 enter/leave 配对，超限报语义化错误。
     上限默认 1024，环境变量 CLUX_MAX_CALL_DEPTH 覆盖。 */
  size_t call_depth;
  size_t max_call_depth;
  /* shadow 模式：块提升 pass 借执行器整树推断类型时开启——create_value
     拦截为 shadow 构造、invoke 拦截为返回类型 shadow（不执行函数体）。
     error 值不拦截，保证推断期错误照常传播。 */
  bool shadow_mode;
  /* 具名复合类型（cunion 等）托管表：无 base 可挂 children，
     由 context 按创建逆序统一释放。 */
  vec_t *named_types;
  struct {
    clux_type_t *type_error;
    clux_type_t *type_i8;
    clux_type_t *type_i16;
    clux_type_t *type_i32;
    clux_type_t *type_i64;
    clux_type_t *type_u8;
    clux_type_t *type_u16;
    clux_type_t *type_u32;
    clux_type_t *type_u64;
    clux_type_t *type_isize;
    clux_type_t *type_usize;
    clux_type_t *type_f32;
    clux_type_t *type_f64;
    clux_type_t *type_bool;
    clux_type_t *type_str;
    clux_type_t *type_type;
    clux_type_t *type_type_template;
    clux_type_t *type_void;
    clux_type_t *type_func_template;
    clux_type_t *type_return_signal;
  } builtin_types;
};
/* 内建类型布局即时可知：创建即 seal（完整）。void 是唯一例外——
   永不 seal、天然不完整，值创建以 seal 为完整性判据，从根上
   拒绝 void 作为值类型。 */
static clux_type_t *create_sealed_builtin(allocator_t *allocator,
                                          clux_type_kind_t kind,
                                          const char *name,
                                          size_t size,
                                          size_t align,
                                          clux_vtable_t *vtable) {
  clux_type_t *type =
      create_clux_type(allocator, kind, name, size, align, vtable);
  clux_type_seal(type);
  return type;
}
clux_context_t *create_clux_context(allocator_t *allocator) {
  clux_context_t *ctx =
      allocator_alloc(allocator, sizeof(struct _clux_context_t));
  ctx->allocator = allocator;
  ctx->safe_allocator = create_safe_allocator(allocator);
  ctx->errors = create_vec(allocator);
  ctx->strs = create_rbtree(allocator);
  ctx->global_scope = create_clux_scope(allocator, NULL);
  ctx->current_scope = ctx->global_scope;
  ctx->named_types = create_vec(allocator);
  ctx->executor = NULL;
  ctx->call_depth = 0;
  ctx->shadow_mode = false;
  /* 调用深度上限：环境变量 CLUX_MAX_CALL_DEPTH 覆盖，默认 1024。 */
  ctx->max_call_depth = 1024;
  const char *depth_env = getenv("CLUX_MAX_CALL_DEPTH");
  if (depth_env && *depth_env) {
    char *end = NULL;
    unsigned long long parsed = strtoull(depth_env, &end, 10);
    if (end != depth_env && parsed > 0) {
      ctx->max_call_depth = (size_t)parsed;
    }
  }
  ctx->builtin_types.type_error =
      create_sealed_builtin(allocator,
                            CLUX_TYPE_ERROR,
                            "error",
                            sizeof(clux_error_t *),
                            alignof(clux_error_t *),
                            get_clux_error_vtable());
  ctx->builtin_types.type_i8 = create_sealed_builtin(allocator,
                                                     CLUX_TYPE_I8,
                                                     "i8",
                                                     sizeof(int8_t),
                                                     alignof(int8_t),
                                                     get_clux_i8_vtable());
  ctx->builtin_types.type_i16 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_I16,
                                                      "i16",
                                                      sizeof(int16_t),
                                                      alignof(int16_t),
                                                      get_clux_i16_vtable());
  ctx->builtin_types.type_i32 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_I32,
                                                      "i32",
                                                      sizeof(int32_t),
                                                      alignof(int32_t),
                                                      get_clux_i32_vtable());
  ctx->builtin_types.type_i64 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_I64,
                                                      "i64",
                                                      sizeof(int64_t),
                                                      alignof(int64_t),
                                                      get_clux_i64_vtable());
  ctx->builtin_types.type_u8 = create_sealed_builtin(allocator,
                                                     CLUX_TYPE_U8,
                                                     "u8",
                                                     sizeof(uint8_t),
                                                     alignof(uint8_t),
                                                     get_clux_u8_vtable());
  ctx->builtin_types.type_u16 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_U16,
                                                      "u16",
                                                      sizeof(uint16_t),
                                                      alignof(uint16_t),
                                                      get_clux_u16_vtable());
  ctx->builtin_types.type_u32 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_U32,
                                                      "u32",
                                                      sizeof(uint32_t),
                                                      alignof(uint32_t),
                                                      get_clux_u32_vtable());
  ctx->builtin_types.type_u64 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_U64,
                                                      "u64",
                                                      sizeof(uint64_t),
                                                      alignof(uint64_t),
                                                      get_clux_u64_vtable());
  ctx->builtin_types.type_isize =
      create_sealed_builtin(allocator,
                            CLUX_TYPE_ISIZE,
                            "isize",
                            sizeof(intptr_t),
                            alignof(intptr_t),
                            get_clux_isize_vtable());
  ctx->builtin_types.type_usize =
      create_sealed_builtin(allocator,
                            CLUX_TYPE_USIZE,
                            "usize",
                            sizeof(size_t),
                            alignof(size_t),
                            get_clux_usize_vtable());
  ctx->builtin_types.type_f32 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_F32,
                                                      "f32",
                                                      sizeof(float),
                                                      alignof(float),
                                                      get_clux_f32_vtable());
  ctx->builtin_types.type_f64 = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_F64,
                                                      "f64",
                                                      sizeof(double),
                                                      alignof(double),
                                                      get_clux_f64_vtable());
  ctx->builtin_types.type_bool = create_sealed_builtin(allocator,
                                                       CLUX_TYPE_BOOL,
                                                       "bool",
                                                       sizeof(bool),
                                                       alignof(bool),
                                                       get_clux_bool_vtable());
  ctx->builtin_types.type_str = create_sealed_builtin(allocator,
                                                      CLUX_TYPE_STR,
                                                      "str",
                                                      sizeof(char *),
                                                      alignof(char *),
                                                      get_clux_str_vtable());
  ctx->builtin_types.type_type =
      create_sealed_builtin(allocator,
                            CLUX_TYPE_TYPE,
                            "type",
                            sizeof(clux_type_t *),
                            alignof(clux_type_t *),
                            get_clux_type_value_vtable());
  ctx->builtin_types.type_type_template =
      create_sealed_builtin(allocator,
                            CLUX_TYPE_TYPE_TEMPLATE,
                            "type_template",
                            sizeof(void *),
                            alignof(void *),
                            get_clux_type_template_vtable());
  ctx->builtin_types.type_func_template =
      create_sealed_builtin(allocator,
                            CLUX_TYPE_FUNC_TEMPLATE,
                            "func_template",
                            sizeof(void *),
                            alignof(void *),
                            get_clux_func_template_vtable());
  ctx->builtin_types.type_void = create_clux_type(
      allocator, CLUX_TYPE_VOID, "void", 0, 1, get_clux_void_vtable());
  /* return signal：引擎级控制值，载荷为返回值借用指针。vtable 全空
     （无运算、无成员），仅在调用边界构造/判定/取载荷后即消费。 */
  ctx->builtin_types.type_return_signal =
      create_sealed_builtin(allocator,
                            CLUX_TYPE_RETURN_SIGNAL,
                            "return_signal",
                            sizeof(clux_value_t *),
                            alignof(clux_value_t *),
                            NULL);
  /* 类型名与 identifier 合一（白皮书 §2.1）：内建类型作为 type value
     按名注册进 global scope，script 层经 context_lookup 按名查找，
     与用户类型/变量共用同一查找通道。机制上统一处理；遮蔽内建
     类型名由 sema 阶段拒绝。 */
  static const clux_type_kind_t named_kinds[] = {
      CLUX_TYPE_I8,
      CLUX_TYPE_I16,
      CLUX_TYPE_I32,
      CLUX_TYPE_I64,
      CLUX_TYPE_U8,
      CLUX_TYPE_U16,
      CLUX_TYPE_U32,
      CLUX_TYPE_U64,
      CLUX_TYPE_ISIZE,
      CLUX_TYPE_USIZE,
      CLUX_TYPE_F32,
      CLUX_TYPE_F64,
      CLUX_TYPE_BOOL,
      CLUX_TYPE_STR,
      CLUX_TYPE_VOID,
  };
  for (size_t idx = 0; idx < sizeof(named_kinds) / sizeof(named_kinds[0]);
       idx++) {
    clux_type_t *type = clux_context_builtin_type(ctx, named_kinds[idx]);
    clux_context_create_type_value(ctx, type, clux_type_get_name(type), NULL);
  }
  return ctx;
}
static void strs_dispose(uintptr_t key, char *str, allocator_t *allocator) {
  (void)key;
  allocator_free(allocator, (void **)&str);
}
void delete_clux_context(clux_context_t **self) {
  if (!self || !*self) {
    return;
  }
  clux_context_t *ctx = *self;
  while (ctx->current_scope != ctx->global_scope) {
    clux_context_pop_scope(ctx);
  }
  delete_clux_scope(&ctx->global_scope);
  for (size_t idx = vec_get_size(ctx->named_types); idx > 0; idx--) {
    clux_type_t *type = vec_get(ctx->named_types, idx - 1);
    delete_clux_type(&type);
  }
  delete_vec(&ctx->named_types);
  for (size_t idx = 0; idx < vec_get_size(ctx->errors); idx++) {
    clux_error_t *err = vec_get(ctx->errors, idx);
    delete_error(&err);
  }
  delete_clux_type(&ctx->builtin_types.type_str);
  delete_clux_type(&ctx->builtin_types.type_type);
  delete_clux_type(&ctx->builtin_types.type_type_template);
  delete_clux_type(&ctx->builtin_types.type_func_template);
  delete_clux_type(&ctx->builtin_types.type_bool);
  delete_clux_type(&ctx->builtin_types.type_f64);
  delete_clux_type(&ctx->builtin_types.type_f32);
  delete_clux_type(&ctx->builtin_types.type_u64);
  delete_clux_type(&ctx->builtin_types.type_usize);
  delete_clux_type(&ctx->builtin_types.type_isize);
  delete_clux_type(&ctx->builtin_types.type_u32);
  delete_clux_type(&ctx->builtin_types.type_u16);
  delete_clux_type(&ctx->builtin_types.type_u8);
  delete_clux_type(&ctx->builtin_types.type_i64);
  delete_clux_type(&ctx->builtin_types.type_i32);
  delete_clux_type(&ctx->builtin_types.type_i16);
  delete_clux_type(&ctx->builtin_types.type_i8);
  delete_clux_type(&ctx->builtin_types.type_error);
  delete_clux_type(&ctx->builtin_types.type_void);
  delete_clux_type(&ctx->builtin_types.type_return_signal);
  delete_vec(&ctx->errors);
  rbtree_foreach(ctx->strs, (rbtree_callback_t)strs_dispose, ctx->allocator);
  delete_rbtree(&ctx->strs);
  delete_safe_allocator(&ctx->safe_allocator);
  allocator_free(ctx->allocator, (void **)self);
}

allocator_t *clux_context_get_allocator(clux_context_t *self) {
  return self->allocator;
}
safe_allocator_t *clux_context_get_safe_allocator(clux_context_t *self) {
  return self->safe_allocator;
}
clux_value_t *clux_context_create_value(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t data,
                                        const char *name,
                                        const location_t *loc) {
  /* seal 即完整性判据：未 seal 的类型（两阶段未定局的壳、永不 seal 的
     void/interface）不得作为值类型。 */
  if (!clux_type_is_seal(type)) {
    const char *type_name = clux_type_get_name(type);
    clux_error_t *err = create_error(self->allocator,
                                     loc,
                                     "cannot create a value of incomplete "
                                     "type '%s'",
                                     type_name ? type_name : "<unknown>");
    return clux_context_create_error(self, err);
  }
  /* shadow 模式拦截：类型推断不落存储、不具名、不 define——直接产出
     shadow 值。error 值豁免（推断期的错误必须原样传播）；type 值豁免
     （类型表达式是推断的输入而非被推断对象，须保持真实存储供解包）。 */
  if (self->shadow_mode && clux_type_get_kind(type) != CLUX_TYPE_ERROR &&
      clux_type_get_kind(type) != CLUX_TYPE_TYPE) {
    clux_value_t *shadow = create_clux_value(self->allocator, type, 0);
    /* shadow 无存储（data=0）但结构体本体仍需归属：匿名 track 进当前
       scope 的 owned 向量，与真实值共用统一所有权通道，scope 销毁统一
       释放——匿名不进命名表，无重名冲突可能。 */
    (void)clux_scope_define(self->current_scope, NULL, shadow);
    return shadow;
  }
  clux_value_t *value = create_clux_value(self->allocator, type, data);
  clux_value_t *previous = clux_scope_define(self->current_scope, name, value);
  if (previous != NULL) {
    delete_clux_value(&value);
    clux_error_t *err = create_error(self->allocator,
                                     loc,
                                     "duplicate declaration '%s'",
                                     name ? name : "<anonymous>");
    return clux_context_create_error(self, err);
  }
  return value;
}
clux_value_t *clux_context_create_error(clux_context_t *self,
                                        clux_error_t *error) {
  /* 出口协议会用已在列的 err 重建 error 值（块/帧边界借 err 指针转
     换出值）：查重防同一 err 重复入列——context 销毁按向量逐个
     delete_error，重复入列即双重释放。 */
  bool queued = false;
  for (size_t idx = 0; idx < vec_get_size(self->errors); idx++) {
    if (vec_get(self->errors, idx) == error) {
      queued = true;
      break;
    }
  }
  if (!queued) {
    (void)vec_push(self->errors, error);
  }
  size_t size = clux_type_get_size(self->builtin_types.type_error);
  ptr_handle_t data =
      safe_alloc(self->safe_allocator, size, error_get_location(error));
  void *raw = ptr_to_raw(data, 0);
  (void)memcpy(raw, &error, size);
  return clux_context_create_value(
      self, self->builtin_types.type_error, data, NULL, NULL);
}
static clux_value_t *create_integer_value(clux_context_t *self,
                                          clux_type_t *type,
                                          const void *value,
                                          const char *name,
                                          const location_t *loc) {
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  void *raw = ptr_to_raw(data, 0);
  (void)memcpy(raw, value, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_create_shadow_value(clux_context_t *self,
                                               clux_type_t *type) {
  return clux_context_create_value(self, type, 0, NULL, NULL);
}
void clux_context_set_shadow_mode(clux_context_t *self, bool enabled) {
  if (self) {
    self->shadow_mode = enabled;
  }
}
bool clux_context_is_shadow_mode(clux_context_t *self) {
  return self ? self->shadow_mode : false;
}
clux_value_t *clux_context_create_i8(clux_context_t *self,
                                     int8_t value,
                                     const char *name,
                                     const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i8, &value, name, loc);
}
clux_value_t *clux_context_create_i16(clux_context_t *self,
                                      int16_t value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i16, &value, name, loc);
}
clux_value_t *clux_context_create_i32(clux_context_t *self,
                                      int32_t value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i32, &value, name, loc);
}
clux_value_t *clux_context_create_i64(clux_context_t *self,
                                      int64_t value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_i64, &value, name, loc);
}
clux_value_t *clux_context_create_u8(clux_context_t *self,
                                     uint8_t value,
                                     const char *name,
                                     const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u8, &value, name, loc);
}
clux_value_t *clux_context_create_u16(clux_context_t *self,
                                      uint16_t value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u16, &value, name, loc);
}
clux_value_t *clux_context_create_u32(clux_context_t *self,
                                      uint32_t value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u32, &value, name, loc);
}
clux_value_t *clux_context_create_u64(clux_context_t *self,
                                      uint64_t value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_u64, &value, name, loc);
}
clux_value_t *clux_context_create_isize(clux_context_t *self,
                                        intptr_t value,
                                        const char *name,
                                        const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_isize, &value, name, loc);
}
clux_value_t *clux_context_create_usize(clux_context_t *self,
                                        size_t value,
                                        const char *name,
                                        const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_usize, &value, name, loc);
}
clux_value_t *clux_context_create_f32(clux_context_t *self,
                                      float value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_f32, &value, name, loc);
}
clux_value_t *clux_context_create_f64(clux_context_t *self,
                                      double value,
                                      const char *name,
                                      const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_f64, &value, name, loc);
}
clux_value_t *clux_context_create_bool(clux_context_t *self,
                                       bool value,
                                       const char *name,
                                       const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_bool, &value, name, loc);
}
/* type value（值描述类型）：data 存被描述类型的指针，eq/value_extends
   代理类型层的判等与派生关系（见 type_value.c）。 */
clux_value_t *clux_context_create_type_value(clux_context_t *self,
                                             clux_type_t *type,
                                             const char *name,
                                             const location_t *loc) {
  if (!type) {
    return clux_context_create_error_message(
        self, loc, "type value creation requires a type");
  }
  return create_integer_value(
      self, self->builtin_types.type_type, &type, name, loc);
}
/* 模板值（type_template / func_template）：data 只存原始 AST 指针
   （定长平点位图），参数表不驻留——每次实例化时从 AST 重新计算。
   ast 须非空，否则返回 error。与 type value 一样经 scope define
   进词法命名空间。 */
static clux_value_t *create_template_value(clux_context_t *self,
                                           clux_type_t *type,
                                           void *ast,
                                           const char *name,
                                           const location_t *loc) {
  if (!ast) {
    return clux_context_create_error_message(
        self, loc, "template creation requires an AST node");
  }
  return create_integer_value(self, type, &ast, name, loc);
}
clux_value_t *clux_context_create_type_template(clux_context_t *self,
                                                void *ast,
                                                const char *name,
                                                const location_t *loc) {
  return create_template_value(
      self, self->builtin_types.type_type_template, ast, name, loc);
}
clux_value_t *clux_context_create_func_template(clux_context_t *self,
                                                void *ast,
                                                const char *name,
                                                const location_t *loc) {
  return create_template_value(
      self, self->builtin_types.type_func_template, ast, name, loc);
}
char *clux_context_alloc_str(clux_context_t *self, size_t len) {
  char *str = allocator_alloc(self->allocator, len + 1);
  str[len] = '\0';
  (void)rbtree_insert(self->strs, (uintptr_t)str, str);
  return str;
}
clux_value_t *clux_context_create_str(clux_context_t *self,
                                      const char *value,
                                      const char *name,
                                      const location_t *loc) {
  size_t len = value ? strlen(value) : 0;
  char *str = clux_context_alloc_str(self, len);
  if (value) {
    (void)memcpy(str, value, len);
  }
  return clux_context_create_str_value(self, str, name, loc);
}

clux_value_t *clux_context_create_ptr(clux_context_t *self,
                                      clux_type_t *type,
                                      ptr_handle_t value,
                                      const char *name,
                                      const location_t *loc) {
  ptr_handle_t data =
      safe_alloc(self->safe_allocator, sizeof(ptr_handle_t), loc);
  ptr_handle_t *raw = ptr_to_raw(data, 0);
  *raw = value;
  return clux_context_create_value(self, type, data, name, loc);
}
clux_type_t *clux_context_get_ptr_type(clux_context_t *self,
                                       clux_type_t *base) {
  (void)self;
  return base ? clux_type_get_ptr_type(base) : NULL;
}
clux_type_t *clux_context_get_slice_type(clux_context_t *self,
                                         clux_type_t *base) {
  (void)self;
  return base ? clux_type_get_slice_type(base) : NULL;
}
clux_value_t *clux_context_create_slice(clux_context_t *self,
                                        clux_type_t *type,
                                        ptr_handle_t ptr,
                                        size_t len,
                                        const char *name,
                                        const location_t *loc) {
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  void *raw = ptr_to_raw(data, 0);
  (void)memcpy(raw, &(clux_slice_data_t){ptr, len}, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_slice_len(clux_context_t *self,
                                     clux_value_t *slice,
                                     const location_t *loc) {
  if (!slice) {
    return clux_context_create_error_message(
        self, loc, "slice length requires a slice");
  }
  clux_type_t *type = clux_value_get_type(slice);
  if (clux_type_get_kind(type) == CLUX_TYPE_ERROR) {
    return slice;
  }
  if (clux_type_get_kind(type) != CLUX_TYPE_SLICE) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(self,
                                             loc,
                                             "type '%s' is not a slice",
                                             type_name ? type_name
                                                       : "<unknown>");
  }
  if (clux_value_is_shadow(slice)) {
    return clux_context_create_shadow_value(self,
                                            self->builtin_types.type_usize);
  }
  ptr_handle_t data = clux_value_get_data(slice);
  clux_slice_data_t *raw = ptr_to_raw(data, 0);
  if (!raw) {
    return clux_context_create_error_message(
        self, loc, "cannot read length of an invalid slice");
  }
  return clux_context_create_usize(self, raw->len, NULL, loc);
}
clux_type_t *clux_context_get_array_type(clux_context_t *self,
                                         clux_type_t *base,
                                         size_t len) {
  (void)self;
  return base ? clux_type_get_array_type(base, len) : NULL;
}
clux_type_t *clux_context_get_tuple_type(clux_context_t *self,
                                         clux_type_t **elems,
                                         size_t count) {
  if (count > 0 && !elems) {
    (void)clux_context_create_error_message(
        self, NULL, "tuple type requires element types");
    return NULL;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (!elems[idx]) {
      (void)clux_context_create_error_message(
          self, NULL, "tuple type element %zu requires a type", idx);
      return NULL;
    }
  }
  /* 结构去重：已存在元素类型序列按声明序一致的实例时恒等返回。 */
  for (size_t idx = 0; idx < vec_get_size(self->named_types); idx++) {
    clux_type_t *existing = vec_get(self->named_types, idx);
    if (clux_tuple_signature_equal(existing, elems, count)) {
      return existing;
    }
  }
  /* 命名 "<a, b>" 风格组装，仅供错误显示，不参与判等。 */
  size_t name_len = 2; /* '<' '>' */
  for (size_t idx = 0; idx < count; idx++) {
    const char *elem_name = clux_type_get_name(elems[idx]);
    name_len += strlen(elem_name ? elem_name : "<unknown>") + 2;
  }
  char name[name_len + 1];
  size_t pos = 0;
  name[pos++] = '<';
  for (size_t idx = 0; idx < count; idx++) {
    const char *elem_name = clux_type_get_name(elems[idx]);
    elem_name = elem_name ? elem_name : "<unknown>";
    if (idx > 0) {
      name[pos++] = ',';
      name[pos++] = ' ';
    }
    size_t len = strlen(elem_name);
    (void)memcpy(name + pos, elem_name, len);
    pos += len;
  }
  name[pos++] = '>';
  name[pos] = '\0';
  clux_type_t *type =
      create_clux_tuple_type(self->allocator, name, elems, count);
  (void)vec_push(self->named_types, type);
  return type;
}
clux_value_t *clux_context_create_array(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        const location_t *loc) {
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_type_t *clux_context_get_optional_type(clux_context_t *self,
                                            clux_type_t *base) {
  (void)self;
  return base ? clux_type_get_optional_type(base) : NULL;
}
/* #some(T, v) -> ?T{v}：T 是载荷类型，内部提升为 ?T。载荷经 safe_cast
   通道吸收到 T（如 *i32 → *void 擦除吸收、协变 extends），不可吸收时
   透传其语义化 error——optional 不引入独立的类型检验规则。 */
clux_value_t *clux_context_create_optional_some(clux_context_t *self,
                                                clux_type_t *base,
                                                clux_value_t *value,
                                                const char *name,
                                                const location_t *loc) {
  if (!base || !value) {
    return clux_context_create_error_message(
        self, loc, "#some requires a type and a value");
  }
  clux_value_t *payload = clux_context_safe_cast(self, value, base, loc);
  if (!payload ||
      clux_type_get_kind(clux_value_get_type(payload)) == CLUX_TYPE_ERROR) {
    return payload;
  }
  clux_type_t *type = clux_type_get_optional_type(base);
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  *(bool *)ptr_to_raw(data, 0) = true;
  void *src = ptr_to_raw(clux_value_get_data(payload), 0);
  void *dst = ptr_to_raw(data, clux_optional_payload_offset(base));
  if (src && dst) {
    (void)memcpy(dst, src, clux_type_get_size(base));
  }
  return clux_context_create_value(self, type, data, name, loc);
}
/* #none(T) -> ?T{none}：T 是载荷类型，内部提升为 ?T（零值）。 */
clux_value_t *clux_context_create_optional_none(clux_context_t *self,
                                                clux_type_t *base,
                                                const char *name,
                                                const location_t *loc) {
  if (!base) {
    return clux_context_create_error_message(
        self, loc, "#none requires a type");
  }
  clux_type_t *type = clux_type_get_optional_type(base);
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_is_none(clux_context_t *self,
                                   clux_value_t *value,
                                   const location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "#is_none requires an optional value");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->is_none) {
    return vt->is_none(self, value);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "#is_none requires an optional value, got '%s'",
      type_name ? type_name : "<unknown>");
}
clux_value_t *
clux_context_take(clux_context_t *self, clux_value_t *value, location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "#take requires an optional value");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->take) {
    return vt->take(self, value);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "#take requires an optional value, got '%s'",
      type_name ? type_name : "<unknown>");
}
clux_type_t *
clux_context_create_cunion_type(clux_context_t *self,
                                const char *name,
                                const clux_cunion_member_t *members,
                                size_t count) {
  if (!name || !members || count == 0) {
    (void)clux_context_create_error_message(
        self, NULL, "cunion requires a name and at least one member");
    return NULL;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (!members[idx].name || !members[idx].type) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "cunion '%s' member %zu requires a name and a type",
          name,
          idx);
      return NULL;
    }
    /* 完整性门禁：值位置成员必须已定局——指针以不完整类型为 base 合法
       （ptr 构造即 seal），但直接使用不完整类型（后声明类型的壳）作
       值成员仍是错误。 */
    if (!clux_type_is_seal(members[idx].type)) {
      const char *member_type_name = clux_type_get_name(members[idx].type);
      (void)clux_context_create_error_message(
          self,
          NULL,
          "cunion '%s' member '%s' has incomplete type '%s'",
          name,
          members[idx].name,
          member_type_name ? member_type_name : "<unknown>");
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(members[idx].name, members[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "cunion '%s' has duplicate member '%s'",
            name,
            members[idx].name);
        return NULL;
      }
    }
  }
  /* 结构去重：已存在成员名+类型按声明序一致的实例时恒等返回
     （对比原始成员表，无需先构造临时类型实例）。 */
  const clux_cunion_member_t *member_ptrs[count];
  for (size_t idx = 0; idx < count; idx++) {
    member_ptrs[idx] = &members[idx];
  }
  for (size_t idx = 0; idx < vec_get_size(self->named_types); idx++) {
    clux_type_t *existing = vec_get(self->named_types, idx);
    if (clux_cunion_members_equal(existing, member_ptrs, count)) {
      return existing;
    }
  }
  /* 两阶段构造：空壳 → 逐成员填充（name 由引擎自拷持有）→ seal 定布局。
     前置校验已拦截重名/缺槽，add_member 失败即引擎层异常。 */
  clux_type_t *type = create_clux_cunion_type(self->allocator, name);
  for (size_t idx = 0; idx < count; idx++) {
    if (!clux_cunion_type_add_member(
            type, self->allocator, members[idx].name, members[idx].type)) {
      (void)delete_clux_type(&type);
      (void)clux_context_create_error_message(self,
                                              NULL,
                                              "cunion '%s' has duplicate "
                                              "member '%s'",
                                              name,
                                              members[idx].name);
      return NULL;
    }
  }
  clux_cunion_type_seal(type);
  (void)vec_push(self->named_types, type);
  return type;
}
clux_value_t *clux_context_create_cunion(clux_context_t *self,
                                         clux_type_t *type,
                                         const char *name,
                                         const location_t *loc) {
  if (!type || clux_type_get_kind(type) != CLUX_TYPE_CUNION) {
    return clux_context_create_error_message(
        self, loc, "cunion value creation requires a cunion type");
  }
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  return clux_context_create_value(self, type, data, name, loc);
}
/* 块提升 pass 的两阶段空壳：为后声明的 union/cunion 预建未定局壳并
   托管进 named_types（源码序的声明语句复用同一实例逐成员补全 + seal）。
   指针可先行以空壳为 base；直接以空壳作值类型会被 seal 门禁拒绝。 */
clux_type_t *clux_context_create_union_shell(clux_context_t *self,
                                             const char *name,
                                             bool is_cunion) {
  if (!self || !name) {
    return NULL;
  }
  clux_type_t *shell = is_cunion
                           ? create_clux_cunion_type(self->allocator, name)
                           : create_clux_union_type(self->allocator, name);
  if (shell) {
    (void)vec_push(self->named_types, shell);
  }
  return shell;
}
clux_type_t *clux_context_create_union_type(clux_context_t *self,
                                            const char *name,
                                            const clux_union_member_t *members,
                                            size_t count) {
  if (!name || !members || count == 0) {
    (void)clux_context_create_error_message(
        self, NULL, "union requires a name and at least one member");
    return NULL;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (!members[idx].name || !members[idx].type) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "union '%s' member %zu requires a name and a type",
          name,
          idx);
      return NULL;
    }
    /* 完整性门禁：值位置成员必须已定局——指针以不完整类型为 base 合法
       （ptr 构造即 seal），但直接使用不完整类型（后声明类型的壳）作
       值成员仍是错误。 */
    if (!clux_type_is_seal(members[idx].type)) {
      const char *member_type_name = clux_type_get_name(members[idx].type);
      (void)clux_context_create_error_message(
          self,
          NULL,
          "union '%s' member '%s' has incomplete type '%s'",
          name,
          members[idx].name,
          member_type_name ? member_type_name : "<unknown>");
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(members[idx].name, members[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "union '%s' has duplicate member '%s'",
            name,
            members[idx].name);
        return NULL;
      }
    }
  }
  /* 结构去重：已存在成员名+类型按声明序一致的实例时恒等返回
     （对比原始成员表，无需先构造临时类型实例）。 */
  const clux_union_member_t *member_ptrs[count];
  for (size_t idx = 0; idx < count; idx++) {
    member_ptrs[idx] = &members[idx];
  }
  for (size_t idx = 0; idx < vec_get_size(self->named_types); idx++) {
    clux_type_t *existing = vec_get(self->named_types, idx);
    if (clux_union_members_equal(existing, member_ptrs, count)) {
      return existing;
    }
  }
  /* 两阶段构造：空壳 → 逐成员填充（name 由引擎自拷持有）→ seal 定布局。
     前置校验已拦截重名/缺槽，add_member 失败即引擎层异常。 */
  clux_type_t *type = create_clux_union_type(self->allocator, name);
  for (size_t idx = 0; idx < count; idx++) {
    if (!clux_union_type_add_member(
            type, self->allocator, members[idx].name, members[idx].type)) {
      (void)delete_clux_type(&type);
      (void)clux_context_create_error_message(self,
                                              NULL,
                                              "union '%s' has duplicate "
                                              "member '%s'",
                                              name,
                                              members[idx].name);
      return NULL;
    }
  }
  clux_union_type_seal(type);
  (void)vec_push(self->named_types, type);
  return type;
}
/* 创建具名 enum 类型（白皮书 §3.6）：布局与底层整数类型一致，variant
   表整体拷入 meta，variant 名经 strs 驻留。结构去重：已存在底层类型 +
   成员名 + 成员值按声明序一致的实例时恒等返回。 */
clux_type_t *clux_context_create_enum_type(clux_context_t *self,
                                           const char *name,
                                           clux_type_t *underlying,
                                           const clux_enum_variant_t *variants,
                                           size_t count) {
  if (!name || !underlying || !variants || count == 0) {
    (void)clux_context_create_error_message(
        self,
        NULL,
        "enum requires a name, an underlying type and at least one variant");
    return NULL;
  }
  clux_type_kind_t underlying_kind = clux_type_kind_unwrap(underlying);
  switch (underlying_kind) {
  case CLUX_TYPE_I8:
  case CLUX_TYPE_I16:
  case CLUX_TYPE_I32:
  case CLUX_TYPE_I64:
  case CLUX_TYPE_U8:
  case CLUX_TYPE_U16:
  case CLUX_TYPE_U32:
  case CLUX_TYPE_U64:
  case CLUX_TYPE_ISIZE:
  case CLUX_TYPE_USIZE:
  case CLUX_TYPE_STR:
    break;
  default:
    (void)clux_context_create_error_message(
        self,
        NULL,
        "enum '%s' underlying type must be an integer or str type",
        name);
    return NULL;
  }
  bool str_underlying = underlying_kind == CLUX_TYPE_STR;
  for (size_t idx = 0; idx < count; idx++) {
    if (!variants[idx].name) {
      (void)clux_context_create_error_message(
          self, NULL, "enum '%s' variant %zu requires a name", name, idx);
      return NULL;
    }
    if (str_underlying && !variants[idx].str) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "enum '%s' variant '%s' requires a string constant value",
          name,
          variants[idx].name);
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(variants[idx].name, variants[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "enum '%s' has duplicate variant '%s'",
            name,
            variants[idx].name);
        return NULL;
      }
    }
  }
  /* 结构去重：已存在底层类型与成员名/值按声明序一致的实例时恒等返回。 */
  for (size_t idx = 0; idx < vec_get_size(self->named_types); idx++) {
    clux_type_t *existing = vec_get(self->named_types, idx);
    if (clux_enum_variants_equal(existing, underlying, variants, count)) {
      return existing;
    }
  }
  /* 成员名与 str 常量经 strs 驻留（随 context 释放），成员表由类型
     meta 持有。 */
  clux_enum_variant_t interned[count];
  for (size_t idx = 0; idx < count; idx++) {
    char *variant_name =
        clux_context_alloc_str(self, strlen(variants[idx].name));
    (void)memcpy(variant_name, variants[idx].name, strlen(variants[idx].name));
    interned[idx].name = variant_name;
    interned[idx].value = str_underlying ? 0 : variants[idx].value;
    interned[idx].str = NULL;
    if (str_underlying) {
      size_t len = strlen(variants[idx].str);
      char *variant_str = clux_context_alloc_str(self, len);
      (void)memcpy(variant_str, variants[idx].str, len);
      interned[idx].str = variant_str;
    }
  }
  clux_type_t *type =
      create_clux_enum_type(self->allocator, name, underlying, interned, count);
  (void)vec_push(self->named_types, type);
  return type;
}

/* 创建具名 interface 类型（白皮书 §3.10、§6.9）：纯编译期结构约束——
   成员是方法签名（名 + func 类型，无捕获），不产生运行期对象/虚表。
   永不 seal（与 void 同族天然不完整）：create_value 的 seal 门禁由此
   拒绝 interface 作值类型。结构去重：已存在成员名+签名按声明序一致的
   实例时恒等返回。 */
clux_type_t *
clux_context_create_interface_type(clux_context_t *self,
                                   const char *name,
                                   const clux_interface_member_t *members,
                                   size_t count) {
  if (!name || !members || count == 0) {
    (void)clux_context_create_error_message(
        self, NULL, "interface requires a name and at least one member");
    return NULL;
  }
  for (size_t idx = 0; idx < count; idx++) {
    if (!members[idx].name || !members[idx].func) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "interface '%s' member %zu requires a name and a signature",
          name,
          idx);
      return NULL;
    }
    if (clux_type_get_kind(members[idx].func) != CLUX_TYPE_FUNC) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "interface '%s' member '%s' signature must be a func type",
          name,
          members[idx].name);
      return NULL;
    }
    if (clux_func_capture_count(members[idx].func) != 0) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "interface '%s' member '%s' signature cannot have captures",
          name,
          members[idx].name);
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(members[idx].name, members[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "interface '%s' has duplicate member '%s'",
            name,
            members[idx].name);
        return NULL;
      }
    }
  }
  /* 结构去重：已存在成员名+签名按声明序一致的实例时恒等返回。 */
  for (size_t idx = 0; idx < vec_get_size(self->named_types); idx++) {
    clux_type_t *existing = vec_get(self->named_types, idx);
    if (clux_interface_members_equal(existing, members, count)) {
      return existing;
    }
  }
  /* 成员名经 strs 驻留（随 context 释放），成员表由类型 meta 持有；
     签名 func 类型借用 context 的具名类型表（其生命周期覆盖本类型）。 */
  clux_interface_member_t interned[count];
  for (size_t idx = 0; idx < count; idx++) {
    size_t len = strlen(members[idx].name);
    char *member_name = clux_context_alloc_str(self, len);
    (void)memcpy(member_name, members[idx].name, len);
    interned[idx].name = member_name;
    interned[idx].func = members[idx].func;
  }
  clux_type_t *type =
      create_clux_interface_type(self->allocator, name, interned, count);
  (void)vec_push(self->named_types, type);
  return type;
}

clux_value_t *clux_context_create_union(clux_context_t *self,
                                        clux_type_t *type,
                                        const char *name,
                                        const location_t *loc) {
  if (!type || clux_type_get_kind(type) != CLUX_TYPE_UNION) {
    return clux_context_create_error_message(
        self, loc, "union value creation requires a union type");
  }
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_union_set_member(clux_context_t *self,
                                            clux_value_t *value,
                                            const char *member,
                                            const location_t *loc) {
  if (!value || !member) {
    return clux_context_create_error_message(
        self, loc, "member activation requires a value and a member name");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  if (clux_type_get_kind(clux_value_get_type(value)) != CLUX_TYPE_UNION) {
    const char *type_name = clux_type_get_name(clux_value_get_type(value));
    return clux_context_create_error_message(
        self,
        loc,
        "member activation requires a union value, got '%s'",
        type_name ? type_name : "<unknown>");
  }
  if (clux_value_is_shadow(value)) {
    return value;
  }
  return clux_union_set_member(self, value, member);
}
clux_value_t *clux_context_is_member(clux_context_t *self,
                                     clux_value_t *value,
                                     const char *member,
                                     const location_t *loc) {
  if (!value || !member) {
    return clux_context_create_error_message(
        self, loc, "operator 'is' requires a value and a member name");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->is_member) {
    return vt->is_member(self, value, member);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "operator 'is' requires a tag union value, got '%s'",
      type_name ? type_name : "<unknown>");
}
clux_value_t *clux_context_field(clux_context_t *self,
                                 clux_value_t *value,
                                 const char *field,
                                 const location_t *loc) {
  if (!value || !field) {
    return clux_context_create_error_message(
        self, loc, "field access requires a value and a field name");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->field) {
    return vt->field(self, value, field);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support field access",
      type_name ? type_name : "<unknown>");
}
clux_type_t *clux_context_create_func_type(clux_context_t *self,
                                           const char *name,
                                           const clux_func_capture_t *captures,
                                           size_t capture_count,
                                           clux_type_t **params,
                                           size_t param_count,
                                           clux_type_t *return_type) {
  if (!name || !return_type) {
    (void)clux_context_create_error_message(
        self, NULL, "func type requires a name and a return type");
    return NULL;
  }
  for (size_t idx = 0; idx < capture_count; idx++) {
    if (!captures || !captures[idx].name || !captures[idx].type) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "func '%s' capture %zu requires a name and a type",
          name,
          idx);
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(captures[idx].name, captures[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "func '%s' has duplicate capture '%s'",
            name,
            captures[idx].name);
        return NULL;
      }
    }
  }
  for (size_t idx = 0; idx < param_count; idx++) {
    if (!params || !params[idx]) {
      (void)clux_context_create_error_message(
          self, NULL, "func '%s' parameter %zu requires a type", name, idx);
      return NULL;
    }
  }
  /* 结构去重：已存在捕获表+参数序列+返回类型按声明序一致的实例时恒等
     返回——捕获列表是类型签名的一部分，func|a:i32|(i32)->i32 与
     func(i32)->i32 是不同类型。 */
  for (size_t idx = 0; idx < vec_get_size(self->named_types); idx++) {
    clux_type_t *existing = vec_get(self->named_types, idx);
    if (clux_func_signature_equal(existing,
                                  captures,
                                  capture_count,
                                  params,
                                  param_count,
                                  return_type)) {
      return existing;
    }
  }
  /* 捕获名经 strs 驻留（随 context 释放），捕获表由类型 meta 持有。 */
  clux_func_capture_t interned[capture_count];
  for (size_t idx = 0; idx < capture_count; idx++) {
    char *capture_name =
        clux_context_alloc_str(self, strlen(captures[idx].name));
    (void)memcpy(capture_name, captures[idx].name, strlen(captures[idx].name));
    interned[idx].name = capture_name;
    interned[idx].type = captures[idx].type;
  }
  clux_type_t *type = create_clux_func_type(self->allocator,
                                            name,
                                            interned,
                                            capture_count,
                                            params,
                                            param_count,
                                            return_type);
  (void)vec_push(self->named_types, type);
  return type;
}
clux_value_t *clux_context_create_func(clux_context_t *self,
                                       clux_type_t *type,
                                       const char *name,
                                       const location_t *loc) {
  if (!type || clux_type_get_kind(type) != CLUX_TYPE_FUNC) {
    return clux_context_create_error_message(
        self, loc, "function value creation requires a func type");
  }
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_func_set_ast(clux_context_t *self,
                                        clux_value_t *value,
                                        void *ast,
                                        const location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "setting a function body requires a function value");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  if (clux_type_get_kind(clux_value_get_type(value)) != CLUX_TYPE_FUNC) {
    const char *type_name = clux_type_get_name(clux_value_get_type(value));
    return clux_context_create_error_message(
        self,
        loc,
        "setting a function body requires a func value, got '%s'",
        type_name ? type_name : "<unknown>");
  }
  if (clux_value_is_shadow(value)) {
    return value;
  }
  clux_func_set_ast(value, ast);
  return value;
}
clux_value_t *clux_context_func_capture(clux_context_t *self,
                                        clux_value_t *value,
                                        const char *capture,
                                        const location_t *loc) {
  if (!value || !capture) {
    return clux_context_create_error_message(
        self, loc, "capture access requires a value and a capture name");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_type_get_kind(type) != CLUX_TYPE_FUNC) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        self,
        loc,
        "capture access requires a func value, got '%s'",
        type_name ? type_name : "<unknown>");
  }
  /* func 捕获走独立 API（不占 field 槽位）：返回捕获类型的读写双向
     view，写入由通用 assign 写穿 view 完成。 */
  const clux_func_capture_t *found = clux_func_find_capture(type, capture);
  if (!found) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(self,
                                             NULL,
                                             "func '%s' has no capture '%s'",
                                             type_name ? type_name
                                                       : "<unknown>",
                                             capture);
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(self, found->type);
  }
  size_t offset = clux_func_capture_offset(
      type, (size_t)(found - clux_func_get_capture_table(type)));
  ptr_handle_t view = ptr_offset(clux_value_get_data(value), offset);
  if (!view) {
    return clux_context_create_error_message(
        self,
        loc,
        "cannot access capture '%s' of an invalid function",
        capture);
  }
  return clux_context_create_value(self, found->type, view, NULL, NULL);
}
clux_value_t *clux_context_invoke(clux_context_t *self,
                                  clux_value_t *value,
                                  size_t argc,
                                  clux_value_t *argv[],
                                  const location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "invoke requires a function value");
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *type = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  /* shadow 模式拦截：func 调用不执行函数体（类型推断不得有副作用），
     直接产出返回类型的 shadow。非 func 类型落入下方常规错误路径。 */
  if (self->shadow_mode && clux_type_kind_unwrap(type) == CLUX_TYPE_FUNC) {
    return clux_context_create_shadow_value(self,
                                            clux_func_get_return_type(type));
  }
  if (vt && vt->invoke) {
    return vt->invoke(self, value, argc, argv);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "invoke requires a function value, got '%s'",
      type_name ? type_name : "<unknown>");
}
void clux_context_set_executor(clux_context_t *self,
                               clux_ast_executor_t executor) {
  if (self) {
    self->executor = executor;
  }
}
clux_ast_executor_t clux_context_get_executor(clux_context_t *self) {
  return self ? self->executor : NULL;
}
bool clux_context_call_depth_enter(clux_context_t *self) {
  if (!self || self->call_depth >= self->max_call_depth) {
    return false;
  }
  self->call_depth++;
  return true;
}
void clux_context_call_depth_leave(clux_context_t *self) {
  if (self && self->call_depth > 0) {
    self->call_depth--;
  }
}
size_t clux_context_get_call_depth(clux_context_t *self) {
  return self ? self->call_depth : 0;
}
clux_value_t *clux_context_create_return_signal(clux_context_t *self,
                                                clux_value_t *payload) {
  ptr_handle_t data =
      safe_alloc(self->safe_allocator, sizeof(clux_value_t *), NULL);
  void *raw = ptr_to_raw(data, 0);
  memcpy(raw, &payload, sizeof(payload));
  return clux_context_create_value(
      self, self->builtin_types.type_return_signal, data, NULL, NULL);
}
bool clux_value_is_return_signal(clux_value_t *value) {
  return value && clux_type_kind_unwrap(clux_value_get_type(value)) ==
                      CLUX_TYPE_RETURN_SIGNAL;
}
clux_value_t *clux_return_signal_get_payload(clux_value_t *signal) {
  if (!clux_value_is_return_signal(signal)) {
    return NULL;
  }
  return *(clux_value_t **)clux_value_get_raw(signal, 0);
}
/* 字段表校验（一次性 create 与两阶段 begin 共用）：缺名/空表/字段缺名
   缺类型/重名记录语义化 error 并返回 false。 */
static bool struct_fields_valid(clux_context_t *self,
                                const char *name,
                                const clux_struct_field_t *fields,
                                size_t field_count) {
  if (!name || !fields || field_count == 0) {
    (void)clux_context_create_error_message(
        self, NULL, "struct requires a name and at least one field");
    return false;
  }
  for (size_t idx = 0; idx < field_count; idx++) {
    if (!fields[idx].name || !fields[idx].type) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' field %zu requires a name and a type",
          name,
          idx);
      return false;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(fields[idx].name, fields[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "struct '%s' has duplicate field '%s'",
            name,
            fields[idx].name);
        return false;
      }
    }
  }
  return true;
}
/* 三表校验（两阶段 install 使用；一次性 create 内联同构段）：attribute
   缺名缺值/重名、method 缺名/重名/不在 attribute 表/对应值非 func、
   subtype 缺名缺类型/重名记录语义化 error 并返回 false。 */
static bool struct_tables_valid(clux_context_t *self,
                                const char *name,
                                const clux_struct_attr_t *attrs,
                                size_t attr_count,
                                const char *const *methods,
                                size_t method_count,
                                const clux_struct_subtype_t *subtypes,
                                size_t subtype_count) {
  for (size_t idx = 0; idx < attr_count; idx++) {
    if (!attrs || !attrs[idx].name || !attrs[idx].value) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' attribute %zu requires a name and a value",
          name,
          idx);
      return false;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(attrs[idx].name, attrs[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "struct '%s' has duplicate attribute '%s'",
            name,
            attrs[idx].name);
        return false;
      }
    }
  }
  for (size_t idx = 0; idx < method_count; idx++) {
    const char *method = methods ? methods[idx] : NULL;
    if (!method) {
      (void)clux_context_create_error_message(
          self, NULL, "struct '%s' method %zu requires a name", name, idx);
      return false;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(method, methods[prev]) == 0) {
        (void)clux_context_create_error_message(
            self, NULL, "struct '%s' has duplicate method '%s'", name, method);
        return false;
      }
    }
    /* method 判定看声明形式：名单里的名字须有同名 attribute 且值为 func
       （var fn = func(...){...} 是 attr 不是 method，不进名单）。 */
    const clux_struct_attr_t *attr = NULL;
    for (size_t pos = 0; pos < attr_count; pos++) {
      if (strcmp(attrs[pos].name, method) == 0) {
        attr = &attrs[pos];
        break;
      }
    }
    if (!attr) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' method '%s' is not declared as an attribute",
          name,
          method);
      return false;
    }
    if (clux_type_get_kind(clux_value_get_type(attr->value)) !=
        CLUX_TYPE_FUNC) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' method '%s' requires a func value",
          name,
          method);
      return false;
    }
  }
  for (size_t idx = 0; idx < subtype_count; idx++) {
    if (!subtypes || !subtypes[idx].name || !subtypes[idx].type) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' subtype %zu requires a name and a type",
          name,
          idx);
      return false;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(subtypes[idx].name, subtypes[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "struct '%s' has duplicate subtype '%s'",
            name,
            subtypes[idx].name);
        return false;
      }
    }
  }
  return true;
}
/* 两阶段构造第一步：字段壳（布局即时 seal，三表留空待 install）。字段
   校验与一次性通道一致，各名字经 strs 驻留；壳托管进 named_types。
   不做结构去重——按名 define 的壳是本声明的唯一实例，struct 判等是
   纯结构判等（类型名不参与），跳过去重语义无损。 */
clux_type_t *clux_context_begin_struct_type(clux_context_t *self,
                                            const char *name,
                                            const clux_struct_field_t *fields,
                                            size_t field_count) {
  if (!struct_fields_valid(self, name, fields, field_count)) {
    return NULL;
  }
  /* 零长度 VLA 是 UB，count 为 0 的表占位 1 个槽位（不会被读取）。 */
  clux_struct_field_t interned_fields[field_count];
  for (size_t idx = 0; idx < field_count; idx++) {
    char *field_name = clux_context_alloc_str(self, strlen(fields[idx].name));
    (void)memcpy(field_name, fields[idx].name, strlen(fields[idx].name));
    interned_fields[idx].name = field_name;
    interned_fields[idx].type = fields[idx].type;
  }
  clux_type_t *type = create_clux_struct_type_shell(
      self->allocator, name, interned_fields, field_count);
  if (type) {
    (void)vec_push(self->named_types, type);
  }
  return type;
}
/* 两阶段构造收尾：三表校验 + 名字驻留 + 安装（只允许一次）。type 须是
   本 context 的 struct 类型；已安装返回 false 并记录 error。 */
bool clux_context_install_struct_tables(clux_context_t *self,
                                        clux_type_t *type,
                                        const clux_struct_attr_t *attrs,
                                        size_t attr_count,
                                        const char *const *methods,
                                        size_t method_count,
                                        const clux_struct_subtype_t *subtypes,
                                        size_t subtype_count) {
  if (!type || clux_type_get_kind(type) != CLUX_TYPE_STRUCT) {
    (void)clux_context_create_error_message(
        self, NULL, "struct table installation requires a struct type");
    return false;
  }
  const char *type_name = clux_type_get_name(type);
  if (!struct_tables_valid(self,
                           type_name ? type_name : "<unknown>",
                           attrs,
                           attr_count,
                           methods,
                           method_count,
                           subtypes,
                           subtype_count)) {
    return false;
  }
  clux_struct_attr_t interned_attrs[attr_count ? attr_count : 1];
  for (size_t idx = 0; idx < attr_count; idx++) {
    char *attr_name = clux_context_alloc_str(self, strlen(attrs[idx].name));
    (void)memcpy(attr_name, attrs[idx].name, strlen(attrs[idx].name));
    interned_attrs[idx].name = attr_name;
    interned_attrs[idx].value = attrs[idx].value;
  }
  const char *interned_methods[method_count ? method_count : 1];
  for (size_t idx = 0; idx < method_count; idx++) {
    char *method_name = clux_context_alloc_str(self, strlen(methods[idx]));
    (void)memcpy(method_name, methods[idx], strlen(methods[idx]));
    interned_methods[idx] = method_name;
  }
  clux_struct_subtype_t interned_subtypes[subtype_count ? subtype_count : 1];
  for (size_t idx = 0; idx < subtype_count; idx++) {
    char *subtype_name =
        clux_context_alloc_str(self, strlen(subtypes[idx].name));
    (void)memcpy(subtype_name, subtypes[idx].name, strlen(subtypes[idx].name));
    interned_subtypes[idx].name = subtype_name;
    interned_subtypes[idx].type = subtypes[idx].type;
  }
  if (!clux_struct_type_install(type,
                                self->allocator,
                                attr_count ? interned_attrs : NULL,
                                attr_count,
                                method_count ? interned_methods : NULL,
                                method_count,
                                subtype_count ? interned_subtypes : NULL,
                                subtype_count)) {
    (void)clux_context_create_error_message(
        self,
        NULL,
        "struct '%s' tables are already installed",
        type_name ? type_name : "<unknown>");
    return false;
  }
  return true;
}
clux_type_t *clux_context_create_struct_type(clux_context_t *self,
                                             const char *name,
                                             const clux_struct_decl_t *decl) {
  if (!name || !decl || !decl->fields || decl->field_count == 0) {
    (void)clux_context_create_error_message(
        self, NULL, "struct requires a name and at least one field");
    return NULL;
  }
  for (size_t idx = 0; idx < decl->field_count; idx++) {
    if (!decl->fields[idx].name || !decl->fields[idx].type) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' field %zu requires a name and a type",
          name,
          idx);
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(decl->fields[idx].name, decl->fields[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "struct '%s' has duplicate field '%s'",
            name,
            decl->fields[idx].name);
        return NULL;
      }
    }
  }
  for (size_t idx = 0; idx < decl->attr_count; idx++) {
    if (!decl->attrs || !decl->attrs[idx].name || !decl->attrs[idx].value) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' attribute %zu requires a name and a value",
          name,
          idx);
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(decl->attrs[idx].name, decl->attrs[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "struct '%s' has duplicate attribute '%s'",
            name,
            decl->attrs[idx].name);
        return NULL;
      }
    }
  }
  for (size_t idx = 0; idx < decl->method_count; idx++) {
    const char *method = decl->methods ? decl->methods[idx] : NULL;
    if (!method) {
      (void)clux_context_create_error_message(
          self, NULL, "struct '%s' method %zu requires a name", name, idx);
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(method, decl->methods[prev]) == 0) {
        (void)clux_context_create_error_message(
            self, NULL, "struct '%s' has duplicate method '%s'", name, method);
        return NULL;
      }
    }
    /* method 判定看声明形式：名单里的名字须有同名 attribute 且值为 func
       （var fn = func(...){...} 是 attr 不是 method，不进名单）。 */
    const clux_struct_attr_t *attr = NULL;
    for (size_t pos = 0; pos < decl->attr_count; pos++) {
      if (strcmp(decl->attrs[pos].name, method) == 0) {
        attr = &decl->attrs[pos];
        break;
      }
    }
    if (!attr) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' method '%s' is not declared as an attribute",
          name,
          method);
      return NULL;
    }
    if (clux_type_get_kind(clux_value_get_type(attr->value)) !=
        CLUX_TYPE_FUNC) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' method '%s' requires a func value",
          name,
          method);
      return NULL;
    }
  }
  for (size_t idx = 0; idx < decl->subtype_count; idx++) {
    if (!decl->subtypes || !decl->subtypes[idx].name ||
        !decl->subtypes[idx].type) {
      (void)clux_context_create_error_message(
          self,
          NULL,
          "struct '%s' subtype %zu requires a name and a type",
          name,
          idx);
      return NULL;
    }
    for (size_t prev = 0; prev < idx; prev++) {
      if (strcmp(decl->subtypes[idx].name, decl->subtypes[prev].name) == 0) {
        (void)clux_context_create_error_message(
            self,
            NULL,
            "struct '%s' has duplicate subtype '%s'",
            name,
            decl->subtypes[idx].name);
        return NULL;
      }
    }
  }
  /* 结构去重：已存在四张声明表一致的实例时恒等返回。 */
  for (size_t idx = 0; idx < vec_get_size(self->named_types); idx++) {
    clux_type_t *existing = vec_get(self->named_types, idx);
    if (clux_struct_decls_equal(existing, decl)) {
      return existing;
    }
  }
  /* 各名字经 strs 驻留（随 context 释放），声明表由类型 meta 持有；
     零长度 VLA 是 UB，count 为 0 的表占位 1 个槽位（不会被读取）。 */
  clux_struct_field_t interned_fields[decl->field_count];
  for (size_t idx = 0; idx < decl->field_count; idx++) {
    char *field_name =
        clux_context_alloc_str(self, strlen(decl->fields[idx].name));
    (void)memcpy(
        field_name, decl->fields[idx].name, strlen(decl->fields[idx].name));
    interned_fields[idx].name = field_name;
    interned_fields[idx].type = decl->fields[idx].type;
  }
  clux_struct_attr_t interned_attrs[decl->attr_count ? decl->attr_count : 1];
  for (size_t idx = 0; idx < decl->attr_count; idx++) {
    char *attr_name =
        clux_context_alloc_str(self, strlen(decl->attrs[idx].name));
    (void)memcpy(
        attr_name, decl->attrs[idx].name, strlen(decl->attrs[idx].name));
    interned_attrs[idx].name = attr_name;
    interned_attrs[idx].value = decl->attrs[idx].value;
  }
  const char *interned_methods[decl->method_count ? decl->method_count : 1];
  for (size_t idx = 0; idx < decl->method_count; idx++) {
    char *method_name =
        clux_context_alloc_str(self, strlen(decl->methods[idx]));
    (void)memcpy(method_name, decl->methods[idx], strlen(decl->methods[idx]));
    interned_methods[idx] = method_name;
  }
  clux_struct_subtype_t
      interned_subtypes[decl->subtype_count ? decl->subtype_count : 1];
  for (size_t idx = 0; idx < decl->subtype_count; idx++) {
    char *subtype_name =
        clux_context_alloc_str(self, strlen(decl->subtypes[idx].name));
    (void)memcpy(subtype_name,
                 decl->subtypes[idx].name,
                 strlen(decl->subtypes[idx].name));
    interned_subtypes[idx].name = subtype_name;
    interned_subtypes[idx].type = decl->subtypes[idx].type;
  }
  clux_struct_decl_t interned = {interned_fields,
                                 decl->field_count,
                                 decl->attr_count ? interned_attrs : NULL,
                                 decl->attr_count,
                                 decl->method_count ? interned_methods : NULL,
                                 decl->method_count,
                                 decl->subtype_count ? interned_subtypes : NULL,
                                 decl->subtype_count};
  clux_type_t *type = create_clux_struct_type(self->allocator, name, &interned);
  (void)vec_push(self->named_types, type);
  return type;
}
clux_value_t *clux_context_create_struct(clux_context_t *self,
                                         clux_type_t *type,
                                         const char *name,
                                         const location_t *loc) {
  if (!type || clux_type_get_kind(type) != CLUX_TYPE_STRUCT) {
    return clux_context_create_error_message(
        self, loc, "struct value creation requires a struct type");
  }
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memset(ptr_to_raw(data, 0), 0, size);
  return clux_context_create_value(self, type, data, name, loc);
}
clux_value_t *clux_context_struct_attr(clux_context_t *self,
                                       clux_type_t *type,
                                       const char *name,
                                       const location_t *loc) {
  if (!type || !name) {
    return clux_context_create_error_message(
        self, loc, "attribute access requires a struct type and a name");
  }
  if (clux_type_get_kind(type) != CLUX_TYPE_STRUCT) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        self,
        loc,
        "attribute access requires a struct type, got '%s'",
        type_name ? type_name : "<unknown>");
  }
  /* attribute/method 是类型命名空间成员：借用返回值本体（不拷贝）。
     method 也是 func 值，调用方经 clux_context_invoke 调用并自行组
     receiver 首参（白皮书 §3.5.1 实例调用是语法糖）。 */
  const clux_struct_attr_t *found = clux_struct_find_attr(type, name);
  if (!found) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        self,
        loc,
        "struct '%s' has no attribute '%s'",
        type_name ? type_name : "<unknown>",
        name);
  }
  return found->value;
}
clux_type_t *clux_context_struct_subtype(clux_context_t *self,
                                         clux_type_t *type,
                                         const char *name,
                                         const location_t *loc) {
  if (!type || !name) {
    (void)clux_context_create_error_message(
        self, loc, "subtype access requires a struct type and a name");
    return NULL;
  }
  if (clux_type_get_kind(type) != CLUX_TYPE_STRUCT) {
    const char *type_name = clux_type_get_name(type);
    (void)clux_context_create_error_message(
        self,
        loc,
        "subtype access requires a struct type, got '%s'",
        type_name ? type_name : "<unknown>");
    return NULL;
  }
  const clux_struct_subtype_t *found = clux_struct_find_subtype(type, name);
  if (!found) {
    const char *type_name = clux_type_get_name(type);
    (void)clux_context_create_error_message(self,
                                            loc,
                                            "struct '%s' has no subtype '%s'",
                                            type_name ? type_name : "<unknown>",
                                            name);
    return NULL;
  }
  return found->type;
}
clux_value_t *
clux_context_deref(clux_context_t *self, clux_value_t *ptr, location_t *loc) {
  if (!ptr) {
    return clux_context_create_error_message(
        self, loc, "deref requires a pointer");
  }
  if (clux_type_get_kind(clux_value_get_type(ptr)) == CLUX_TYPE_ERROR) {
    return ptr;
  }
  clux_type_t *type = clux_value_get_type(ptr);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->deref) {
    return vt->deref(self, ptr);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support operator '*'",
      type_name ? type_name : "<unknown>");
}
clux_value_t *clux_context_index(clux_context_t *self,
                                 clux_value_t *aggregate,
                                 clux_value_t *index,
                                 const location_t *loc) {
  if (!aggregate || !index) {
    return clux_context_create_error_message(
        self, loc, "index requires an aggregate and an index");
  }
  if (clux_type_get_kind(clux_value_get_type(aggregate)) == CLUX_TYPE_ERROR) {
    return aggregate;
  }
  if (clux_type_get_kind(clux_value_get_type(index)) == CLUX_TYPE_ERROR) {
    return index;
  }
  clux_type_t *type = clux_value_get_type(aggregate);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->index) {
    return vt->index(self, aggregate, index);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support indexing",
      type_name ? type_name : "<unknown>");
}
/* 切片取段 base[lo?:hi?]：lo/hi 为 NULL 表示开放端点（缺省 0 / 长度）。
   数组/切片经 vtable 偏移出新切片值（[]T 胖指针借用内存），负数、越界、
   lo > hi 报 error，shadow 操作数产出切片类型的 shadow。 */
clux_value_t *clux_context_slice(clux_context_t *self,
                                 clux_value_t *aggregate,
                                 clux_value_t *lo,
                                 clux_value_t *hi,
                                 const location_t *loc) {
  if (!aggregate) {
    return clux_context_create_error_message(
        self, loc, "slice requires an aggregate");
  }
  if (clux_type_get_kind(clux_value_get_type(aggregate)) == CLUX_TYPE_ERROR) {
    return aggregate;
  }
  if (lo && clux_type_get_kind(clux_value_get_type(lo)) == CLUX_TYPE_ERROR) {
    return lo;
  }
  if (hi && clux_type_get_kind(clux_value_get_type(hi)) == CLUX_TYPE_ERROR) {
    return hi;
  }
  clux_type_t *type = clux_value_get_type(aggregate);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  if (vt && vt->slice) {
    return vt->slice(self, aggregate, lo, hi);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(self,
                                           loc,
                                           "type '%s' does not support slicing",
                                           type_name ? type_name : "<unknown>");
}
clux_value_t *clux_context_index_to_i64(clux_context_t *self,
                                        clux_value_t *index,
                                        int64_t *out) {
  clux_type_kind_t kind = clux_type_get_kind(clux_value_get_type(index));
  void *raw = ptr_to_raw(clux_value_get_data(index), 0);
  if (!raw) {
    return clux_context_create_error_message(
        self, NULL, "cannot read an invalid index");
  }
  int64_t value = 0;
  switch (kind) {
  case CLUX_TYPE_I8:
    value = *(int8_t *)raw;
    break;
  case CLUX_TYPE_I16:
    value = *(int16_t *)raw;
    break;
  case CLUX_TYPE_I32:
    value = *(int32_t *)raw;
    break;
  case CLUX_TYPE_I64:
    value = *(int64_t *)raw;
    break;
  case CLUX_TYPE_ISIZE:
    value = *(intptr_t *)raw;
    break;
  case CLUX_TYPE_U8:
    value = *(uint8_t *)raw;
    break;
  case CLUX_TYPE_U16:
    value = *(uint16_t *)raw;
    break;
  case CLUX_TYPE_U32:
    value = *(uint32_t *)raw;
    break;
  case CLUX_TYPE_U64:
    value = (int64_t)*(uint64_t *)raw;
    break;
  case CLUX_TYPE_USIZE:
    value = (int64_t)*(size_t *)raw;
    break;
  default: {
    const char *type_name = clux_type_get_name(clux_value_get_type(index));
    return clux_context_create_error_message(
        self,
        NULL,
        "index must be an integer, got '%s'",
        type_name ? type_name : "<unknown>");
  }
  }
  *out = value;
  return NULL;
}
clux_value_t *
clux_context_ref(clux_context_t *self, clux_value_t *value, location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(self, loc, "ref requires a value");
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_type_get_kind(type) == CLUX_TYPE_ERROR) {
    return value;
  }
  clux_type_t *ptr_type = clux_context_get_ptr_type(self, type);
  if (!ptr_type) {
    return clux_context_create_error_message(
        self, loc, "ref requires a typed value");
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(self, ptr_type);
  }
  return clux_context_create_ptr(
      self, ptr_type, clux_value_get_data(value), NULL, loc);
}
clux_value_t *clux_context_create_str_value(clux_context_t *self,
                                            char *str,
                                            const char *name,
                                            const location_t *loc) {
  return create_integer_value(
      self, self->builtin_types.type_str, &str, name, loc);
}
void clux_context_push_scope(clux_context_t *self) {
  clux_scope_t *scope = create_clux_scope(self->allocator, self->current_scope);
  self->current_scope = scope;
}
void clux_context_pop_scope(clux_context_t *self) {
  if (!self || self->current_scope == self->global_scope) {
    return;
  }
  clux_scope_t *parent = clux_scope_get_parent(self->current_scope);
  delete_clux_scope(&self->current_scope);
  self->current_scope = parent;
}
clux_scope_t *clux_context_get_scope(clux_context_t *self) {
  return self ? self->current_scope : NULL;
}
clux_scope_t *clux_context_get_global_scope(clux_context_t *self) {
  return self ? self->global_scope : NULL;
}
clux_scope_t *clux_context_swap_scope(clux_context_t *self,
                                      clux_scope_t *target) {
  clux_scope_t *previous = self->current_scope;
  self->current_scope = target;
  return previous;
}
clux_value_t *clux_context_declare_value(clux_context_t *self,
                                         clux_value_t *value,
                                         const char *name,
                                         const location_t *loc) {
  if (!self || !value || !name) {
    return clux_context_create_error_message(
        self, loc, "declaration requires a value and a name");
  }
  clux_scope_name_result_t named =
      clux_scope_name_value(self->current_scope, value, name);
  if (named == CLUX_SCOPE_NAME_OK) {
    return value;
  }
  if (named == CLUX_SCOPE_NAME_CONFLICT) {
    clux_error_t *err =
        create_error(self->allocator, loc, "duplicate declaration '%s'", name);
    return clux_context_create_error(self, err);
  }
  /* 借用值：按值语义拷贝为独立存储（data 布局按 type 尺寸平铺，
     借用/引用属性在声明处失去）。 */
  clux_type_t *type = clux_value_get_type(value);
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  memcpy(ptr_to_raw(data, 0), ptr_to_raw(clux_value_get_data(value), 0), size);
  return clux_context_create_value(self, type, data, name, loc);
}

clux_value_t *clux_context_lookup(clux_context_t *self,
                                  const char *name,
                                  const location_t *loc) {
  clux_scope_t *scope = self->current_scope;
  clux_value_t *val = clux_scope_get(scope, name);
  if (val) {
    return val;
  }
  return clux_context_create_error_message(
      self, loc, "use of undeclared identifier '%s'", name);
}
clux_value_t *clux_context_consume_value(clux_context_t *self,
                                         clux_value_t *value,
                                         const location_t *loc) {
  if (clux_value_is_tdz(value)) {
    return clux_context_create_error_message(
        self, loc, "use of a value before initialization");
  }
  return value;
}
clux_value_t *clux_context_create_tdz_value(clux_context_t *self,
                                            clux_type_t *type,
                                            const char *name,
                                            const location_t *loc) {
  if (!type) {
    return clux_context_create_error_message(
        self, loc, "TDZ value creation requires a type");
  }
  size_t size = clux_type_get_size(type);
  /* 存储清零保证确定性：TDZ 期间不读取，首赋值整体覆写。 */
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  void *raw = ptr_to_raw(data, 0);
  if (raw) {
    memset(raw, 0, size);
  }
  clux_value_t *value = clux_context_create_value(self, type, data, name, loc);
  /* 同名冲突时 create_value 已删除新值并返回 error，不标 TDZ。 */
  if (clux_type_get_kind(clux_value_get_type(value)) != CLUX_TYPE_ERROR) {
    clux_value_set_tdz(value, true);
  }
  return value;
}
clux_value_t *clux_context_create_error_message(clux_context_t *self,
                                                const location_t *loc,
                                                const char *fmt,
                                                ...) {
  va_list args;
  va_start(args, fmt);
  size_t len = vsnprintf(NULL, 0, fmt, args);
  va_end(args);
  char *msg = allocator_alloc(self->allocator, len + 1);
  va_start(args, fmt);
  (void)vsnprintf(msg, len + 1, fmt, args);
  va_end(args);
  clux_error_t *err = create_error(self->allocator, loc, "%s", msg);
  allocator_free(self->allocator, (void **)&msg);
  return clux_context_create_error(self, err);
}
static const char *binary_op_name(clux_binary_op_t op) {
  switch (op) {
  case CLUX_OP_ADD:
    return "+";
  case CLUX_OP_SUB:
    return "-";
  case CLUX_OP_MUL:
    return "*";
  case CLUX_OP_DIV:
    return "/";
  case CLUX_OP_MOD:
    return "%";
  case CLUX_OP_AND:
    return "&";
  case CLUX_OP_OR:
    return "|";
  case CLUX_OP_XOR:
    return "^";
  case CLUX_OP_SHL:
    return "<<";
  case CLUX_OP_SHR:
    return ">>";
  case CLUX_OP_EQ:
    return "==";
  case CLUX_OP_NE:
    return "!=";
  case CLUX_OP_LT:
    return "<";
  case CLUX_OP_LE:
    return "<=";
  case CLUX_OP_GT:
    return ">";
  case CLUX_OP_GE:
    return ">=";
  }
  return "<unknown>";
}
static const char *prefix_op_name(clux_prefix_op_t op) {
  switch (op) {
  case CLUX_OP_PLUS:
    return "+";
  case CLUX_OP_NEGATIVE:
    return "-";
  case CLUX_OP_NOT:
    return "~";
  case CLUX_OP_LNOT:
    return "!";
  }
  return "<unknown>";
}
static clux_vt_binary_fn_t binary_op_fn(clux_vtable_t *vt,
                                        clux_binary_op_t op) {
  switch (op) {
  case CLUX_OP_ADD:
    return vt->add;
  case CLUX_OP_SUB:
    return vt->sub;
  case CLUX_OP_MUL:
    return vt->mul;
  case CLUX_OP_DIV:
    return vt->div;
  case CLUX_OP_MOD:
    return vt->mod;
  case CLUX_OP_AND:
    return vt->bit_and;
  case CLUX_OP_OR:
    return vt->bit_or;
  case CLUX_OP_XOR:
    return vt->bit_xor;
  case CLUX_OP_SHL:
    return vt->shl;
  case CLUX_OP_SHR:
    return vt->shr;
  case CLUX_OP_EQ:
    return vt->eq;
  case CLUX_OP_NE:
    return vt->ne;
  case CLUX_OP_LT:
    return vt->lt;
  case CLUX_OP_LE:
    return vt->le;
  case CLUX_OP_GT:
    return vt->gt;
  case CLUX_OP_GE:
    return vt->ge;
  }
  return NULL;
}
static clux_vt_prefix_fn_t prefix_op_fn(clux_vtable_t *vt,
                                        clux_prefix_op_t op) {
  switch (op) {
  case CLUX_OP_PLUS:
    return vt->plus;
  case CLUX_OP_NEGATIVE:
    return vt->negative;
  case CLUX_OP_NOT:
    return vt->bit_not;
  case CLUX_OP_LNOT:
    return vt->logic_not;
  }
  return NULL;
}
clux_value_t *clux_context_safe_cast(clux_context_t *self,
                                     clux_value_t *value,
                                     clux_type_t *target,
                                     const location_t *loc) {
  if (!value || !target) {
    return clux_context_create_error_message(
        self, loc, "safe cast requires a value and a target type");
  }
  clux_type_t *source = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(source);
  if (vt && vt->safe_cast) {
    return vt->safe_cast(self, value, target);
  }
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' cannot be safely cast to '%s'",
      source_name ? source_name : "<unknown>",
      target_name ? target_name : "<unknown>");
}
clux_value_t *clux_context_cast(clux_context_t *self,
                                clux_value_t *value,
                                clux_type_t *target,
                                const location_t *loc) {
  if (!value || !target) {
    return clux_context_create_error_message(
        self, loc, "cast requires a value and a target type");
  }
  clux_type_t *source = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(source);
  if (vt && vt->cast) {
    return vt->cast(self, value, target);
  }
  const char *source_name = clux_type_get_name(source);
  const char *target_name = clux_type_get_name(target);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' cannot be cast to '%s'",
      source_name ? source_name : "<unknown>",
      target_name ? target_name : "<unknown>");
}
clux_value_t *clux_context_binary(clux_context_t *self,
                                  clux_binary_op_t op,
                                  clux_value_t *left,
                                  clux_value_t *right,
                                  const location_t *loc) {
  const char *op_name = binary_op_name(op);
  if (!left || !right) {
    return clux_context_create_error_message(
        self, loc, "operator '%s' requires two operands", op_name);
  }
  clux_type_t *type = clux_value_get_type(left);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  clux_vt_binary_fn_t fn = vt ? binary_op_fn(vt, op) : NULL;
  if (fn) {
    return fn(self, left, right);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support operator "
      "'%s'",
      type_name ? type_name : "<unknown>",
      op_name);
}
clux_value_t *clux_context_prefix(clux_context_t *self,
                                  clux_prefix_op_t op,
                                  clux_value_t *value,
                                  const location_t *loc) {
  const char *op_name = prefix_op_name(op);
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "operator '%s' requires one operand", op_name);
  }
  clux_type_t *type = clux_value_get_type(value);
  clux_vtable_t *vt = clux_type_get_vtable(type);
  clux_vt_prefix_fn_t fn = vt ? prefix_op_fn(vt, op) : NULL;
  if (fn) {
    return fn(self, value);
  }
  const char *type_name = clux_type_get_name(type);
  return clux_context_create_error_message(
      self,
      loc,
      "type '%s' does not support operator "
      "'%s'",
      type_name ? type_name : "<unknown>",
      op_name);
}
clux_value_t *clux_context_add(clux_context_t *self,
                               clux_value_t *left,
                               clux_value_t *right,
                               const location_t *loc) {
  return clux_context_binary(self, CLUX_OP_ADD, left, right, loc);
}
clux_value_t *clux_context_assign(clux_context_t *self,
                                  clux_value_t *target,
                                  clux_value_t *value,
                                  const location_t *loc) {
  if (!target || !value) {
    return clux_context_create_error_message(
        self, loc, "assignment requires a target and a value");
  }
  clux_type_t *target_type = clux_value_get_type(target);
  if (clux_type_get_kind(target_type) == CLUX_TYPE_ERROR) {
    return target;
  }
  if (clux_type_get_kind(clux_value_get_type(value)) == CLUX_TYPE_ERROR) {
    return value;
  }
  /* const 是静态性质：目标限定链含 const 即拒绝写入（volatile 放行）；
     TDZ 例外——尚未完成首次赋值的目标绕过 mutable 检查（const 的
     TDZ 变量支持第一次赋值），赋值成功后清除标记。 */
  bool target_tdz = clux_value_is_tdz(target);
  if (!target_tdz && clux_type_is_const(target_type)) {
    const char *target_name = clux_type_get_name(target_type);
    return clux_context_create_error_message(
        self,
        loc,
        "cannot assign to a value of type '%s'",
        target_name ? target_name : "<unknown>");
  }
  /* 同类型无需转换：复合类型（slice/ptr/optional 等）没有 safe_cast，
     恒等直接 memcpy。optional 的 some/none 构造不经 assign：some 走显式
     create_optional_some（#some），none 是零值。 */

  clux_value_t *casted = value;
  if (!clux_type_equal(target_type, clux_value_get_type(value))) {
    casted = clux_context_safe_cast(self, value, target_type, loc);
  }
  clux_type_t *casted_type = clux_value_get_type(casted);
  if (clux_type_get_kind(casted_type) == CLUX_TYPE_ERROR) {
    return casted;
  }
  if (clux_value_is_shadow(target) || clux_value_is_shadow(casted)) {
    return target;
  }
  /* value 的 data 是平凡位图：右值 safe_cast 成左值类型后整体 memcpy 穿透 */
  size_t size = clux_type_get_size(target_type);
  void *dst = ptr_to_raw(clux_value_get_data(target), 0);
  void *src = ptr_to_raw(clux_value_get_data(casted), 0);
  if (dst && src) {
    (void)memcpy(dst, src, size);
  }
  clux_value_set_tdz(target, false);
  return target;
}
clux_value_t *
clux_context_clone(clux_context_t *self, clux_value_t *value, location_t *loc) {
  if (!value) {
    return clux_context_create_error_message(
        self, loc, "clone requires a value");
  }
  clux_type_t *type = clux_value_get_type(value);
  if (clux_type_get_kind(type) == CLUX_TYPE_ERROR) {
    return value;
  }
  if (clux_value_is_shadow(value)) {
    return clux_context_create_shadow_value(self, type);
  }
  /* value 的 data 是平凡位图：新分配同尺寸块整体拷贝即可 */
  size_t size = clux_type_get_size(type);
  ptr_handle_t data = safe_alloc(self->safe_allocator, size, loc);
  void *dst = ptr_to_raw(data, 0);
  void *src = ptr_to_raw(clux_value_get_data(value), 0);
  if (dst && src) {
    (void)memcpy(dst, src, size);
  }
  return clux_context_create_value(self, type, data, NULL, NULL);
}
/* 内建类型按 kind 取回：引擎内部与测试的通用通道（非逐类型接口）。
   script 层类型名与 identifier 合一——类型作为 type value 按名注册进
   global scope，经 context_lookup 查找，见 register_builtin_type_values。 */
clux_type_t *clux_context_builtin_type(clux_context_t *ctx,
                                       clux_type_kind_t kind) {
  switch (kind) {
  case CLUX_TYPE_I8:
    return ctx->builtin_types.type_i8;
  case CLUX_TYPE_I16:
    return ctx->builtin_types.type_i16;
  case CLUX_TYPE_I32:
    return ctx->builtin_types.type_i32;
  case CLUX_TYPE_I64:
    return ctx->builtin_types.type_i64;
  case CLUX_TYPE_U8:
    return ctx->builtin_types.type_u8;
  case CLUX_TYPE_U16:
    return ctx->builtin_types.type_u16;
  case CLUX_TYPE_U32:
    return ctx->builtin_types.type_u32;
  case CLUX_TYPE_U64:
    return ctx->builtin_types.type_u64;
  case CLUX_TYPE_ISIZE:
    return ctx->builtin_types.type_isize;
  case CLUX_TYPE_USIZE:
    return ctx->builtin_types.type_usize;
  case CLUX_TYPE_F32:
    return ctx->builtin_types.type_f32;
  case CLUX_TYPE_F64:
    return ctx->builtin_types.type_f64;
  case CLUX_TYPE_BOOL:
    return ctx->builtin_types.type_bool;
  case CLUX_TYPE_STR:
    return ctx->builtin_types.type_str;
  case CLUX_TYPE_VOID:
    return ctx->builtin_types.type_void;
  case CLUX_TYPE_TYPE:
    return ctx->builtin_types.type_type;
  case CLUX_TYPE_TYPE_TEMPLATE:
    return ctx->builtin_types.type_type_template;
  case CLUX_TYPE_FUNC_TEMPLATE:
    return ctx->builtin_types.type_func_template;
  case CLUX_TYPE_ERROR:
    return ctx->builtin_types.type_error;
  default:
    return NULL;
  }
}
clux_type_t *clux_context_get_type_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_type;
}
clux_type_t *clux_context_get_type_template_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_type_template;
}
clux_type_t *clux_context_get_func_template_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_func_template;
}
/* 模板值取回原始 AST（两种模板通用）：非模板值返回 NULL。
   实例化由上层取回 AST 后展开（对 Clux 是编译期通道）。 */
void *clux_context_template_ast(clux_value_t *value) {
  if (!value) {
    return NULL;
  }
  clux_type_kind_t kind = clux_type_kind_unwrap(clux_value_get_type(value));
  if (kind != CLUX_TYPE_TYPE_TEMPLATE && kind != CLUX_TYPE_FUNC_TEMPLATE) {
    return NULL;
  }
  void *raw = ptr_to_raw(clux_value_get_data(value), 0);
  return raw ? *(void **)raw : NULL;
}
clux_type_t *clux_context_get_error_type(clux_context_t *ctx) {
  return ctx->builtin_types.type_error;
}