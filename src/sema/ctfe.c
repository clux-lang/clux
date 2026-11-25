/* CTFE 最小版（§2.1）：comptime var 初值的常量表达式直推求值。
   覆盖：数字字面量（0x/0o/0b/十进制 + i8..f64 后缀，后缀解析与
   exec/literal.c 同构）、布尔字面量、已求值常量引用（LITERAL_
   IDENTIFIER）、一元负号（前置 EXPR_BINARY 的 LEFT 为 NULL 形态）
   与二元 + - * / %。构造/字符串等完整 exec 接线随后续分期。
   不可求值 → ERROR diag（返回 -1），emit 被 has_error 阻断。 */
#include "sema/sema_internal.h"
#include "core/allocator.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/diag.h"
#include "parser/node.h"
#include "parser/node_binary.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static sema_const_def_t *sema_ctfe_lookup(vec_t *defs, const char *name) {
  for (size_t idx = 0; idx < vec_get_size(defs); idx++) {
    sema_const_def_t *def = (sema_const_def_t *)vec_get(defs, idx);
    if (strcmp(def->name, name) == 0) {
      return def;
    }
  }
  return NULL;
}

/* 数字字面量文本 → 常量值：剥后缀 → 剥负号 → 按前缀定进制 → 解析。
   浮点后缀（f32/f64）走 strtod，其余按 strtoull 幅值（无后缀默认 i32）。 */
static int sema_ctfe_eval_numeric(clux_context_t *ctx,
                                  node_t *node,
                                  const char *text,
                                  sema_const_t *out) {
  size_t len = strlen(text);
  size_t num_len = 0;
  const char *suffix = sema_render_strip_suffix(text, len, &num_len);
  const char *digits = text;
  int negative = 0;
  if (num_len > 0 && digits[0] == '-') {
    negative = 1;
    digits++;
    num_len--;
  }
  int base = 10;
  if (num_len > 2 && digits[0] == '0') {
    if (digits[1] == 'x' || digits[1] == 'X') {
      base = 16;
      digits += 2;
    } else if (digits[1] == 'o' || digits[1] == 'O') {
      base = 8;
      digits += 2;
    } else if (digits[1] == 'b' || digits[1] == 'B') {
      base = 2;
      digits += 2;
    }
  }
  if (num_len == 0) {
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "numeric constant '%s' has no digits",
                      text);
    return -1;
  }
  if (suffix && (suffix[0] == 'f')) {
    out->kind = SEMA_CONST_FLOAT;
    out->float_value = strtod(digits, NULL);
    if (negative) {
      out->float_value = -out->float_value;
    }
    out->suffix = suffix;
    out->type_name = suffix;
    return 0;
  }
  out->kind = SEMA_CONST_INT;
  out->magnitude = strtoull(digits, NULL, base);
  out->negative = negative;
  out->suffix = suffix;
  out->type_name = suffix ? suffix : "i32";
  return 0;
}

/* 一元前缀（LEFT 为 NULL）：仅负号，数值取反。 */
static int sema_ctfe_eval_unary(clux_context_t *ctx,
                                vec_t *const_defs,
                                node_t *node,
                                node_t *operand,
                                const char *op,
                                sema_const_t *out) {
  if (sema_ctfe_eval(ctx, const_defs, operand, out) != 0) {
    return -1;
  }
  if (strcmp(op, "-") != 0) {
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(node),
                      "operator '%s' is not supported in constant expressions",
                      op);
    return -1;
  }
  if (out->kind == SEMA_CONST_INT) {
    out->negative = !out->negative;
    return 0;
  }
  if (out->kind == SEMA_CONST_FLOAT) {
    out->float_value = -out->float_value;
    return 0;
  }
  clux_context_diag(ctx,
                    CLUX_DIAG_ERROR,
                    node_get_location(node),
                    "negation requires a numeric constant");
  return -1;
}

static int sema_ctfe_arith(clux_context_t *ctx,
                           node_t *node,
                           const char *op,
                           const sema_const_t *lhs,
                           const sema_const_t *rhs,
                           sema_const_t *out) {
  if (lhs->kind == SEMA_CONST_INT && rhs->kind == SEMA_CONST_INT) {
    bool lu = lhs->suffix && lhs->suffix[0] == 'u';
    bool ru = rhs->suffix && rhs->suffix[0] == 'u';
    if (lu != ru) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(node),
                        "mixed signed and unsigned constants are not "
                        "supported in constant expressions");
      return -1;
    }
    *out = *lhs;
    if (lu) {
      uint64_t l = lhs->magnitude;
      uint64_t r = rhs->magnitude;
      switch (op[0]) {
      case '+':
        out->magnitude = l + r;
        break;
      case '-':
        out->magnitude = l - r;
        break;
      case '*':
        out->magnitude = l * r;
        break;
      case '/':
      case '%':
        if (r == 0) {
          clux_context_diag(ctx,
                            CLUX_DIAG_ERROR,
                            node_get_location(node),
                            "division by zero in constant expression");
          return -1;
        }
        out->magnitude = op[0] == '/' ? l / r : l % r;
        break;
      default:
        goto unsupported;
      }
      out->negative = 0;
      return 0;
    }
    int64_t l =
        lhs->negative ? -(int64_t)lhs->magnitude : (int64_t)lhs->magnitude;
    int64_t r =
        rhs->negative ? -(int64_t)rhs->magnitude : (int64_t)rhs->magnitude;
    int64_t result = 0;
    switch (op[0]) {
    case '+':
      result = l + r;
      break;
    case '-':
      result = l - r;
      break;
    case '*':
      result = l * r;
      break;
    case '/':
    case '%':
      if (r == 0) {
        clux_context_diag(ctx,
                          CLUX_DIAG_ERROR,
                          node_get_location(node),
                          "division by zero in constant expression");
        return -1;
      }
      result = op[0] == '/' ? l / r : l % r;
      break;
    default:
      goto unsupported;
    }
    out->negative = result < 0;
    out->magnitude =
        result < 0 ? (uint64_t)(-(result + 1)) + 1 : (uint64_t)result;
    return 0;
  }
  if (lhs->kind == SEMA_CONST_FLOAT && rhs->kind == SEMA_CONST_FLOAT) {
    double l = lhs->float_value;
    double r = rhs->float_value;
    *out = *lhs;
    switch (op[0]) {
    case '+':
      out->float_value = l + r;
      break;
    case '-':
      out->float_value = l - r;
      break;
    case '*':
      out->float_value = l * r;
      break;
    case '/':
      out->float_value = l / r;
      break;
    default:
      goto unsupported;
    }
    return 0;
  }
  clux_context_diag(ctx,
                    CLUX_DIAG_ERROR,
                    node_get_location(node),
                    "operands of different kinds in constant expression");
  return -1;
unsupported:
  clux_context_diag(ctx,
                    CLUX_DIAG_ERROR,
                    node_get_location(node),
                    "operator '%s' is not supported in constant expressions",
                    op);
  return -1;
}

int sema_ctfe_eval(clux_context_t *ctx,
                   vec_t *const_defs,
                   node_t *expr,
                   sema_const_t *out) {
  if (!expr) {
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      NULL,
                      "compile-time constant requires an expression");
    return -1;
  }
  memset(out, 0, sizeof(*out));
  switch (node_get_kind(expr)) {
  case CLUX_NODE_LITERAL_NUMERIC: {
    allocator_t *allocator = clux_context_get_allocator(ctx);
    char *text = location_get(node_get_location(expr), allocator);
    if (!text) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(expr),
                        "numeric constant has no source text");
      return -1;
    }
    int rc = sema_ctfe_eval_numeric(ctx, expr, text, out);
    allocator_free(allocator, (void **)&text);
    return rc;
  }
  case CLUX_NODE_LITERAL_BOOLEAN: {
    allocator_t *allocator = clux_context_get_allocator(ctx);
    char *text = location_get(node_get_location(expr), allocator);
    out->kind = SEMA_CONST_BOOL;
    out->suffix = NULL;
    out->type_name = "bool";
    out->bool_value = text ? strcmp(text, "true") == 0 : 0;
    allocator_free(allocator, (void **)&text);
    return 0;
  }
  case CLUX_NODE_LITERAL_IDENTIFIER: {
    allocator_t *allocator = clux_context_get_allocator(ctx);
    char *name = location_get(node_get_location(expr), allocator);
    if (!name) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(expr),
                        "constant reference has no source text");
      return -1;
    }
    sema_const_def_t *def = sema_ctfe_lookup(const_defs, name);
    if (!def) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(expr),
                        "constant '%s' is not a comptime value",
                        name);
      allocator_free(allocator, (void **)&name);
      return -1;
    }
    *out = def->value;
    allocator_free(allocator, (void **)&name);
    return 0;
  }
  case CLUX_NODE_EXPR_BINARY: {
    node_t *symbol = binary_node_get_symbol(expr);
    node_t *left = binary_node_get_left(expr);
    node_t *right = binary_node_get_right(expr);
    allocator_t *allocator = clux_context_get_allocator(ctx);
    char *op =
        symbol ? location_get(node_get_location(symbol), allocator) : NULL;
    if (!op) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(expr),
                        "constant expression requires an operator");
      return -1;
    }
    int rc;
    if (!left && right) {
      rc = sema_ctfe_eval_unary(ctx, const_defs, expr, right, op, out);
      allocator_free(allocator, (void **)&op);
      return rc;
    }
    if (!left || !right) {
      clux_context_diag(ctx,
                        CLUX_DIAG_ERROR,
                        node_get_location(expr),
                        "expression is not a compile-time constant");
      allocator_free(allocator, (void **)&op);
      return -1;
    }
    sema_const_t lhs;
    sema_const_t rhs;
    if (sema_ctfe_eval(ctx, const_defs, left, &lhs) != 0 ||
        sema_ctfe_eval(ctx, const_defs, right, &rhs) != 0) {
      allocator_free(allocator, (void **)&op);
      return -1;
    }
    rc = sema_ctfe_arith(ctx, expr, op, &lhs, &rhs, out);
    allocator_free(allocator, (void **)&op);
    return rc;
  }
  default:
    clux_context_diag(ctx,
                      CLUX_DIAG_ERROR,
                      node_get_location(expr),
                      "expression is not a compile-time constant");
    return -1;
  }
}
