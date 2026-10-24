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
    CIR_TYPE_STRUCT,
    CIR_TYPE_ARRAY,
    CIR_TYPE_PTR,
    CIR_TYPE_SLICE,
    CIR_TYPE_STR,
    CIR_TYPE_OPTION,
    CIR_TYPE_UNION,
    CIR_TYPE_CUNION,
} cir_type_kind_t;

typedef struct cir_type cir_type_t;
typedef struct cir_expr cir_expr_t;

typedef struct cir_field {
    const char *name;
    cir_type_t *type;
    size_t      tag;
    struct cir_field *next;
} cir_field_t;

typedef struct cir_type {
    cir_type_kind_t kind;
    const char     *c_name;
    const void     *src;      /* const type_t*（仅做缓存键） */
    cir_type_t     *elem;     /* array / ptr */
    size_t          array_len;
    cir_field_t    *fields;
    bool            emitted;
    struct cir_type *next;
} cir_type_t;

typedef enum {
    CIR_EXPR_INT = 0,
    CIR_EXPR_IDENT,
    CIR_EXPR_BOOL,
    CIR_EXPR_UNARY,
    CIR_EXPR_BINARY,
    CIR_EXPR_CALL,
    CIR_EXPR_ASSIGN,
    CIR_EXPR_MEMBER,
    CIR_EXPR_INDEX,
    CIR_EXPR_CONSTRUCT,
    CIR_EXPR_NEW,
    CIR_EXPR_SLICE,
    CIR_EXPR_LEN,
    CIR_EXPR_MAKE,
    CIR_EXPR_SLICE_DATA,
    CIR_EXPR_STRING,
    CIR_EXPR_OPTION,
    CIR_EXPR_OPT_IS_SOME,
    CIR_EXPR_UNWRAP,
    CIR_EXPR_UNION_CONSTRUCT,
    CIR_EXPR_UNION_MEMBER,
    CIR_EXPR_UNION_TAG,
    CIR_EXPR_CUNION_CONSTRUCT,
} cir_expr_kind_t;

typedef struct cir_init {
    const char *name;  /* named field；NULL = positional */
    cir_expr_t *value;
    struct cir_init *next;
} cir_init_t;

typedef struct cir_expr {
    cir_expr_kind_t kind;
    int64_t         int_value;
    bool            bool_value;
    const char     *text;
    size_t          text_len;
    const char     *op;
    cir_type_t     *type;
    cir_type_t     *elem;
    cir_type_t     *src_type;
    bool            array_data; /* index through wrapper .data */
    size_t          tag;
    struct cir_expr *lhs;
    struct cir_expr *rhs;
    struct cir_expr *high;
    struct cir_expr *args;
    cir_init_t      *inits;
    struct cir_expr *next;
} cir_expr_t;

typedef enum {
    CIR_STMT_VAR = 0,
    CIR_STMT_ASSIGN,
    CIR_STMT_EXPR,
    CIR_STMT_RETURN,
    CIR_STMT_IF,
    CIR_STMT_WHILE,
    CIR_STMT_FOR,
    CIR_STMT_DOWHILE,
    CIR_STMT_BLOCK,
    CIR_STMT_BREAK,
    CIR_STMT_CONTINUE,
    CIR_STMT_FREE,
} cir_stmt_kind_t;

typedef struct cir_stmt {
    cir_stmt_kind_t kind;
    const char     *name;    /* var name / assign target */
    const char     *op;      /* assign op */
    cir_type_t     *type;    /* var type */
    cir_expr_t     *expr;    /* init / assign value / condition / return value */
    cir_expr_t     *lhs;     /* assign target */
    cir_expr_t     *update;  /* for update */
    struct cir_stmt *init;   /* for init */
    struct cir_stmt *body;   /* block / then / loop body */
    struct cir_stmt *else_body;
    bool            cleanup_return;
    struct cir_stmt *next;
} cir_stmt_t;

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
    cir_stmt_t  *body;
    cir_stmt_t  *cleanup;
    int          source_line;
    bool         is_entry;   /* 生成 C main wrapper */
    struct cir_function *next;
} cir_function_t;

typedef struct cir_global {
    const char  *name;
    cir_type_t  *type;
    cir_expr_t  *init;
    struct cir_global *next;
} cir_global_t;

typedef struct cir_unit {
    const char  *source_path;
    const char **includes;
    size_t       include_count;
    cir_signature_t *signatures;
    cir_function_t  *functions;
    cir_global_t    *globals;
    cir_type_t      *types;
    bool             uses_alloc;
    bool             uses_str_cmp;
    bool             uses_opt_unwrap;
} cir_unit_t;

cir_unit_t    *cir_unit_new(arena_t *arena, const char *source_path);
void           cir_unit_add_include(cir_unit_t *u, arena_t *arena,
                                    const char *include);
cir_type_t    *cir_type_new(arena_t *arena, cir_type_kind_t kind,
                            const char *c_name);
cir_type_t    *cir_type_find(const cir_unit_t *u, const void *src,
                             cir_type_kind_t kind);
cir_field_t   *cir_field_new(arena_t *arena, const char *name,
                             cir_type_t *type);
void           cir_type_add_field(cir_type_t *t, cir_field_t *f);
void           cir_unit_add_type(cir_unit_t *u, cir_type_t *t);
cir_expr_t    *cir_expr_int(arena_t *arena, int64_t value);
cir_expr_t    *cir_expr_bool(arena_t *arena, bool value);
cir_expr_t    *cir_expr_ident(arena_t *arena, const char *name);
cir_expr_t    *cir_expr_unary(arena_t *arena, const char *op, cir_expr_t *e);
cir_expr_t    *cir_expr_binary(arena_t *arena, const char *op, cir_expr_t *lhs,
                               cir_expr_t *rhs);
cir_expr_t    *cir_expr_call(arena_t *arena, const char *callee,
                             cir_expr_t *args);
cir_expr_t    *cir_expr_assign(arena_t *arena, cir_expr_t *target,
                               const char *op, cir_expr_t *value);
cir_expr_t    *cir_expr_member(arena_t *arena, cir_expr_t *object,
                               const char *field);
cir_expr_t    *cir_expr_index(arena_t *arena, cir_expr_t *object,
                              cir_expr_t *index, bool array_data);
cir_expr_t    *cir_expr_construct(arena_t *arena, cir_type_t *type,
                                  cir_init_t *inits);
cir_expr_t    *cir_expr_new(arena_t *arena, cir_type_t *type,
                            cir_init_t *inits);
cir_expr_t    *cir_expr_slice(arena_t *arena, cir_type_t *type,
                              cir_expr_t *object, cir_expr_t *low,
                              cir_expr_t *high);
cir_expr_t    *cir_expr_len(arena_t *arena, cir_expr_t *object,
                            cir_type_t *object_type);
cir_expr_t    *cir_expr_make(arena_t *arena, cir_type_t *slice_type,
                             cir_type_t *elem_type, cir_expr_t *length,
                             cir_init_t *inits);
cir_expr_t    *cir_expr_slice_data(arena_t *arena, cir_expr_t *object);
cir_expr_t    *cir_expr_string(arena_t *arena, const char *data, size_t len);
cir_expr_t    *cir_expr_option(arena_t *arena, cir_type_t *type, bool some,
                               cir_expr_t *value);
cir_expr_t    *cir_expr_opt_is_some(arena_t *arena, cir_expr_t *object,
                                    bool want_some);
cir_expr_t    *cir_expr_unwrap(arena_t *arena, cir_expr_t *object);
cir_expr_t    *cir_expr_union_construct(arena_t *arena, cir_type_t *type,
                                        size_t tag, const char *field,
                                        cir_expr_t *value);
cir_expr_t    *cir_expr_union_member(arena_t *arena, cir_expr_t *object,
                                     const char *field);
cir_expr_t    *cir_expr_union_tag(arena_t *arena, cir_expr_t *object);
cir_expr_t    *cir_expr_cunion_construct(arena_t *arena, cir_type_t *type,
                                         const char *field, cir_expr_t *value);
cir_init_t    *cir_init_new(arena_t *arena, const char *name,
                            cir_expr_t *value);
void           cir_expr_add_init(cir_expr_t *construct, cir_init_t *init);
void           cir_expr_append_arg(cir_expr_t *call, cir_expr_t *arg);
cir_stmt_t    *cir_stmt_new(arena_t *arena, cir_stmt_kind_t kind);
void           cir_stmt_append(cir_stmt_t *block, cir_stmt_t *stmt);
cir_signature_t *cir_signature_new(arena_t *arena, const char *tag,
                                   cir_type_t *ret, size_t param_count,
                                   cir_type_t **params);
cir_function_t  *cir_function_new(arena_t *arena, const char *name,
                                  const char *object, cir_signature_t *sig,
                                  size_t param_count, cir_param_t *params,
                                  cir_stmt_t *body, int source_line,
                                  bool is_entry);
void cir_unit_add_signature(cir_unit_t *u, cir_signature_t *sig);
void cir_unit_add_function(cir_unit_t *u, cir_function_t *fn);
void cir_unit_add_global(cir_unit_t *u, cir_global_t *g);

#ifdef __cplusplus
}
#endif
#endif
