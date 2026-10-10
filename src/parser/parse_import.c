#include "parser/ast_import.h"
#include "parser/ast_error.h"
#include "parser/parse_utils.h"

/* ---- parse_import ---- */

/** 解析 import 语句：import <alias> from "<path>";
 *
 * 语法：
 *   import math from "./math.clux";
 *
 * alias 必须是合法标识符，path 必须是字符串字面量（内容仅支持相对路径
 * ./ 或 ../ 开头，sema 阶段校验）。
 */
ast_node_t *parse_import(parser_t *p) {
    uint32_t tb = p->pos;

    /* import 关键字已由调用点确认，消费它 */
    advance(p);
    skip_trivia(p);

    /* 解析别名（命名空间标识符） */
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected namespace alias after 'import'");
    }
    strslice_t alias = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* from 关键字 */
    if (!expect_keyword(p, "from")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected 'from' in import statement");
    }
    skip_trivia(p);

    /* 模块路径（字符串字面量） */
    if (!check_kind(p, TOKEN_TYPE_STRING)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected module path string after 'from'");
    }
    const token_t *path_tok = cur_token(p);
    strslice_t path_text = token_strslice(path_tok);
    advance(p);
    skip_trivia(p);

    /* 结尾分号 */
    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after import statement");
    }

    /* 构造 AST_IMPORT 节点 */
    ast_node_t *node = ast_import_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ast_import_t *imp = (ast_import_t *)node;

    /* alias 直接用 token 切片（arena 持有 token pool 生命周期） */
    imp->alias = alias;

    /* path 需要去掉引号并展开 escape（复用字符串字面量逻辑） */
    /* 内联简化：去掉首尾引号，不做 escape 展开（模块路径不应含转义） */
    if (path_text.len >= 2 && path_text.ptr[0] == '"') {
        imp->path = strslice_from_bytes(path_text.ptr + 1, path_text.len - 2);
    } else {
        imp->path = path_text;
    }

    return node;
}
