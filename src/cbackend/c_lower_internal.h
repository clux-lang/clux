#ifndef CLUX_CBACKEND_C_LOWER_INTERNAL_H
#define CLUX_CBACKEND_C_LOWER_INTERNAL_H

#include "cbackend/c_lower.h"

#include "parser/ast_assign.h"
#include "parser/ast_addr.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_defer.h"
#include "parser/ast_deref.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_for.h"
#include "parser/ast_for_index.h"
#include "parser/ast_free.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_make.h"
#include "parser/ast_member.h"
#include "parser/ast_move.h"
#include "parser/ast_new.h"
#include "parser/ast_nil.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_option.h"
#include "parser/ast_return.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_slice.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_take.h"
#include "parser/ast_unary.h"
#include "parser/ast_union_def.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_cunion.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_union.h"
#include "vm/vm.h"

typedef struct lower_defer {
    strslice_t name;
    ast_node_t *body;
    struct lower_defer *next;
} lower_defer_t;

typedef struct lower_local {
    strslice_t name;
    const type_t *type;
    struct lower_local *next;
} lower_local_t;

typedef struct lower_capture {
    strslice_t name;
    const char *expr;
    struct lower_capture *next;
} lower_capture_t;

typedef struct lower_method {
    strslice_t name;
    uint32_t owner_id;
    ast_func_def_t *def;
    const char *cname;
    struct lower_method *next;
} lower_method_t;

typedef struct lower_ctx {
    arena_t *arena;
    sema_t  *sema;
    cir_unit_t *unit;
    lower_defer_t *defers;
    lower_local_t *locals;
    lower_capture_t *captures;
    lower_method_t *methods;
    cir_stmt_t *cleanup;
    cir_type_t *return_type;
    unsigned    ret_seq;
    unsigned    sig_seq;
    unsigned    fnlit_seq;
    bool        failed;
} lower_ctx_t;

/* util */
const char *copy_arena(arena_t *arena, const char *s);
const char *mangle(arena_t *arena, const char *prefix, strslice_t name);
const char *token_text(arena_t *arena, const token_t *tok);
const type_t *strip(sema_t *sema, const type_t *t);

/* types / signatures */
cir_type_t *map_type(lower_ctx_t *lc, const type_t *t);
cir_signature_t *signature_for(lower_ctx_t *lc, cir_type_t *ret, size_t pc,
                               cir_type_t **pt);
const type_t *func_type_of_def(lower_ctx_t *lc, ast_func_def_t *fn);
ast_func_def_t *func_def_of_ref(lower_ctx_t *lc, ast_func_ref_t *ref);
lower_method_t *find_method(lower_ctx_t *lc, uint32_t owner_id,
                            strslice_t name);

/* expressions */
cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e);
cir_expr_t *lower_call(lower_ctx_t *lc, ast_call_t *call);
const type_t *infer_expr_type(lower_ctx_t *lc, ast_node_t *e);
bool call_is_len(const ast_call_t *c);
cir_expr_t *lower_len(lower_ctx_t *lc, ast_call_t *call);
cir_expr_t *lower_slice(lower_ctx_t *lc, ast_slice_t *sl);
cir_expr_t *lower_make(lower_ctx_t *lc, ast_make_t *mk);
cir_expr_t *lower_closure_literal(lower_ctx_t *lc, ast_func_def_t *fn);
cir_expr_t *lower_func_literal(lower_ctx_t *lc, ast_func_def_t *fn);

/* statements */
cir_stmt_t *lower_stmt(lower_ctx_t *lc, ast_node_t *s, bool *ok);
cir_stmt_t *lower_ast_list(lower_ctx_t *lc, ast_node_t *list, bool *ok);
cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block, bool *ok);
void register_local(lower_ctx_t *lc, strslice_t name, const type_t *type);
const type_t *lookup_local(lower_ctx_t *lc, strslice_t name);

/* functions */
cir_function_t *lower_function(lower_ctx_t *lc, ast_func_def_t *fn,
                               bool is_entry);

#endif
