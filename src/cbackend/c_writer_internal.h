#ifndef CLUX_CBACKEND_C_WRITER_INTERNAL_H
#define CLUX_CBACKEND_C_WRITER_INTERNAL_H

#include "cbackend/c_writer.h"
#include "core/string.h"

#include <stdarg.h>
#include <stdio.h>

void cw_put(string_t *sb, const char *s);
void cw_printf(string_t *sb, const char *fmt, ...);
void cw_write_c_bytes(string_t *sb, const char *data, size_t len);
void cw_write_c_string(string_t *sb, const char *s);

void cw_write_expr(string_t *sb, const cir_expr_t *e);
void cw_write_type_defs(string_t *sb, cir_type_t *types);
void cw_emit_type(string_t *sb, cir_type_t *t);
void cw_emit_all_types(string_t *sb, cir_type_t *types);

void cw_write_params(string_t *sb, const cir_signature_t *sig, bool with_names,
                     const cir_function_t *fn);
void cw_write_signature_def(string_t *sb, const cir_signature_t *sig);
void cw_write_function_decl(string_t *sb, const cir_function_t *fn);
void cw_write_function_object(string_t *sb, const cir_function_t *fn);
void cw_write_function_def(string_t *sb, const cir_function_t *fn);
void cw_write_indent(string_t *sb, int indent);
void cw_write_cond(string_t *sb, const cir_expr_t *e);
void cw_write_stmt(string_t *sb, const cir_stmt_t *s, int indent);
void cw_write_stmt_list(string_t *sb, const cir_stmt_t *s, int indent);

#endif
