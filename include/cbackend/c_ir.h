#ifndef _H_CLUX_CBACKEND_C_IR_
#define _H_CLUX_CBACKEND_C_IR_

#include "core/arena.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CIR_TYPE_VOID = 0,
    CIR_TYPE_I32,
    CIR_TYPE_USIZE,
    CIR_TYPE_BOOL,
} cir_type_kind_t;

typedef struct cir_type {
    cir_type_kind_t kind;
    const char     *c_name;
} cir_type_t;

typedef enum {
    CIR_EXPR_INT = 0,
    CIR_EXPR_IDENT,
    CIR_EXPR_UNARY,
    CIR_EXPR_BINARY,
} cir_expr_kind_t;

typedef struct cir_expr {
    cir_expr_kind_t kind;
    int64_t         int_value;
    const char     *text;
    const char     *op;
    struct cir_expr *lhs;
    struct cir_expr *rhs;
} cir_expr_t;

typedef struct cir_param {
    const char *name;
    cir_type_t *type;
} cir_param_t;

typedef struct cir_signature {
    const char  *tag;       /* struct clux_fn_i32_void */
    cir_type_t  *ret;
    size_t       param_count;
    cir_type_t **params;
    struct cir_signature *next;
} cir_signature_t;

typedef struct cir_function {
    const char  *name;      /* clux_fn_main */
    const char  *object;    /* clux_obj_main */
    cir_signature_t *sig;
    size_t       param_count;
    cir_param_t *params;
    cir_expr_t  *return_expr; /* NULL = void */
    int          source_line;
    bool         is_entry;   /* 生成 C main wrapper */
    struct cir_function *next;
} cir_function_t;

typedef struct cir_unit {
    const char  *source_path;
    const char **includes;
    size_t       include_count;
    cir_signature_t *signatures;
    cir_function_t  *functions;
} cir_unit_t;

cir_unit_t    *cir_unit_new(arena_t *arena, const char *source_path);
void           cir_unit_add_include(cir_unit_t *u, arena_t *arena,
                                    const char *include);
cir_type_t    *cir_type_new(arena_t *arena, cir_type_kind_t kind,
                            const char *c_name);
cir_expr_t    *cir_expr_int(arena_t *arena, int64_t value);
cir_expr_t    *cir_expr_ident(arena_t *arena, const char *name);
cir_expr_t    *cir_expr_unary(arena_t *arena, const char *op, cir_expr_t *e);
cir_expr_t    *cir_expr_binary(arena_t *arena, const char *op, cir_expr_t *lhs,
                               cir_expr_t *rhs);
cir_signature_t *cir_signature_new(arena_t *arena, const char *tag,
                                   cir_type_t *ret, size_t param_count,
                                   cir_type_t **params);
cir_function_t  *cir_function_new(arena_t *arena, const char *name,
                                  const char *object, cir_signature_t *sig,
                                  size_t param_count, cir_param_t *params,
                                  cir_expr_t *return_expr, int source_line,
                                  bool is_entry);
void cir_unit_add_signature(cir_unit_t *u, cir_signature_t *sig);
void cir_unit_add_function(cir_unit_t *u, cir_function_t *fn);

#ifdef __cplusplus
}
#endif
#endif
