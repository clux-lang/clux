#ifndef _H_CLUX_PARSER_FMT_INTERNAL_
#define _H_CLUX_PARSER_FMT_INTERNAL_
/* 内部头：fmt.c 拆分后的共享上下文与跨文件声明（parser 私有）。 */
#include "parser/fmt.h"

#ifdef __cplusplus
extern "C" {
#endif


#define CLUX_INDENT "    "

/* ---- 增长式输出缓冲 ---- */

typedef struct {
    allocator_t *alloc;
    char        *buf;
    size_t       len;
    size_t       cap;
} sb_t;

/* ---- formatter 上下文 ---- */

typedef struct {
    allocator_t *alloc;
    vec_t       *tokens;   /* token 池（含 trivia；注释挂载用） */
    sb_t         sb;
    int          indent;   /* 当前缩进层 */
    bool         at_line_start;
    /* 注释消费位图（bit per token 下标，1 = 已输出） */
    uint8_t     *comment_seen;
    size_t       comment_bytes;
    size_t       src_line;     /* 当前输出位置对应的源码行号（行内/独立行判定） */
} fmt_t;

uint32_t find_punct_token(fmt_t *f, uint32_t begin, uint32_t end, const char *sym);
size_t mount_comments(fmt_t *f, uint32_t begin, uint32_t end, size_t last_line, bool inline_only);
void render_block(fmt_t *f, ast_node_t *node);
void render_captures(fmt_t *f, ast_node_t *captures);
void render_comma_list(fmt_t *f, ast_node_t *head);
void render_expr(fmt_t *f, ast_node_t *node);
void render_func_def(fmt_t *f, ast_node_t *node);
void render_func_literal(fmt_t *f, ast_node_t *node);
void render_node(fmt_t *f, ast_node_t *node);
void render_param(fmt_t *f, ast_node_t *n);
void render_program(fmt_t *f, ast_node_t *node);
void render_scope_annot_names(fmt_t *f, const strslice_t *names, size_t count);
void render_semi_list(fmt_t *f, ast_node_t *head);
void render_stmt(fmt_t *f, ast_node_t *node);
void sb_ch(sb_t *sb, char c);
void sb_indent(sb_t *sb, int level);
void sb_put(sb_t *sb, const char *s, size_t n);
void sb_str(sb_t *sb, const char *s);
size_t sibling_count(ast_node_t *head);
size_t tok_line(const token_t *t);

/* 守卫：被污染的值（错误已报告）→ 直接返回 void shadow。 */
#define EX_GUARD(sema, v)                                                     \
  do {                                                                        \
    if (is_poison((sema), (v))) return val_void((sema));                       \
  } while (0)

#ifdef __cplusplus
}
#endif
#endif
