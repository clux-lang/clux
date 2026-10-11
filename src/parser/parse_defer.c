#include "parser/ast_defer.h"
#include "parser/ast_block.h"
#include "parser/ast_error.h"
#include "parser/parse_utils.h"
#include "core/strslice.h"

/* ================================================================ */
/* parse_defer: defer [annot] [captures] { body }                     */
/*                                                                   */
/* 生成式：                                                          */
/*   defer [ '<a,b> ] [ |cap, (name: T = init)|  |  || ] '{' stmts '}'*/
/*                                                                   */
/* 产出 AST_DEFER（语句节点），其 closure 是一枚**无参 void 闭包**    */
/* （AST_FUNC_DEF 字面量形态，params/return_expr 均为 NULL）：捕获列表 */
/* 与块体解析**逐字复用**闭包那一套（parse_capture_list /            */
/* parse_scope_annotation / parse_block），不含任何复制的表达式解析。 */
/*                                                                   */
/* 空捕获列表两种写法都接受：`defer { ... }`（省略）与 `defer || {    */
/* ... }`（显式空列表；`||` 是 lexer 的单个 SYMBOL token，在 defer   */
/* 之后的位置不可能与逻辑或表达式混淆——那里只能是捕获列表或块体）。 */
/* 两者产出完全相同的 AST（captures = NULL），fmt 统一渲染为省略形态。 */
/*                                                                   */
/* 作用域标注 '<a,b> 与闭包同款（捕获 ref/opaque 时 sema 强制要求）。 */
/* ================================================================ */

ast_node_t *parse_defer(parser_t *p) {
    uint32_t tb = p->pos;

    if (!check_keyword(p, "defer")) return NULL;
    advance(p); /* 消费 defer */
    skip_trivia(p);

    /* 可选作用域标注：'<a,b>（'< 是 lexer 合成的复合 SYMBOL token，
       与闭包 func '<x> |...| 同一套解析与同一套语义）。 */
    strslice_t *scope_annot_names = NULL;
    size_t      scope_annot_count = 0;
    if (check_symbol(p, "'<")) {
        advance(p); /* 消费 '< */
        skip_trivia(p);
        scope_annot_names = parse_scope_annotation(p, &scope_annot_count);
        if (!scope_annot_names) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "invalid scope annotation on defer");
        }
    }

    /* 可选捕获列表：|a, (b: i32 = c)| —— 与闭包共用同一份解析。
       显式空列表 || 单独识别（lexer 把 || 合成单个 SYMBOL token）。 */
    ast_node_t *captures = NULL;
    if (check_symbol(p, "||")) {
        advance(p); /* 空捕获列表：等价省略 */
        skip_trivia(p);
    } else if (check_symbol(p, "|")) {
        captures = parse_capture_list(p);
        if (captures && captures->kind == AST_ERROR) {
            return captures;
        }
    }

    /* 块体：与函数体同一套语句/作用域语义（parse_block）。 */
    ast_node_t *body = parse_block(p);
    if (!body || body->kind == AST_ERROR) {
        if (!body) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected '{' for defer body");
        }
        return body;
    }

    /* 承载捕获/标注/块体的无参 void 闭包（AST_FUNC_DEF 字面量形态）。
       params / return_expr 为 NULL：无参、void——sema 的
       sema_check_func_literal 与 compiler 的 compile_expr(AST_FUNC_DEF)
       完全不区分它与源码里手写的 `func |a| (): void { ... }`。 */
    ast_node_t *fn_node = ast_func_def_new(p->arena, tb, p->pos);
    if (!fn_node) return NULL;
    ast_func_def_t *fn = (ast_func_def_t *)fn_node;
    fn->captures          = captures;
    fn->scope_annot_names = scope_annot_names;
    fn->scope_annot_count = scope_annot_count;
    fn->params            = NULL;
    fn->params_last       = NULL;
    fn->return_expr       = NULL; /* void */
    fn->body              = body;

    ast_node_t *node = ast_defer_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_defer_t *)node)->closure = fn;
    return node;
}
