#include "parser/ast_import.h"
#include "parser/ast_extern_lib.h"
#include "parser/ast_reexport.h"
#include "parser/ast_error.h"
#include "parser/parse_utils.h"

/* ---- 公共：`from "<path>"` 解析 ---- */

/** 解析 `from "<path>"`：消费 from 关键字与字符串字面量，返回去引号的路径。
 *
 * 模块路径不做转义展开（路径不应含转义）；引号剥离后即为路径文本。
 * 失败时构造 AST_ERROR（覆盖 [tb, p->pos)）并经 *out_err 返回。
 */
static bool parse_from_path(parser_t *p, uint32_t tb, strslice_t *out_path,
                            ast_node_t **out_err) {
    if (!expect_keyword(p, "from")) {
        *out_err = ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected 'from' in import statement");
        return false;
    }
    skip_trivia(p);

    if (!check_kind(p, TOKEN_TYPE_STRING)) {
        *out_err = ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected module path string after 'from'");
        return false;
    }
    strslice_t path_text = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    /* 去掉首尾引号（内容不含转义，无需展开） */
    if (path_text.len >= 2 && path_text.ptr[0] == '"') {
        *out_path = strslice_from_bytes(path_text.ptr + 1, path_text.len - 2);
    } else {
        *out_path = path_text;
    }
    return true;
}

/* ---- parse_import ---- */

/** 解析 import 语句：import <alias> from "<path>";
 *
 * 语法：
 *   import math from "./math";
 *
 * alias 必须是合法标识符，path 必须是字符串字面量。路径分类（相对模块 /
 * 动态库 / 包导入）与相对解析在 sema 阶段完成（见 module/module_path.h）。
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
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "namespace alias");
        if (rerr) return rerr;
    }
    {
        ast_node_t *rerr = parse_reserved_ident_error(p, cur_token(p), "library alias");
        if (rerr) return rerr;
    }
    strslice_t alias = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);

    strslice_t path = {0};
    ast_node_t *err = NULL;
    if (!parse_from_path(p, tb, &path, &err)) return err;

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
    imp->path  = path;
    return node;
}

/* ---- parse_reexport ---- */

/** 解析代理导出：export * from "<path>";
 *
 * 调用约定：调用点已消费 `export` 关键字，当前 token 是 `*`（parser.c 的
 * export 分支据此分派）。语义见 ast_reexport.h。
 */
ast_node_t *parse_reexport(parser_t *p) {
    uint32_t tb = p->pos;   /* 当前 token = '*' */

    if (!check_symbol(p, "*")) {
        /* 不是本构造：游标未动，返回 NULL 由调用点报错 */
        return NULL;
    }
    advance(p);   /* commit */
    skip_trivia(p);

    strslice_t path = {0};
    ast_node_t *err = NULL;
    if (!parse_from_path(p, tb, &path, &err)) return err;

    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after 'export * from'");
    }

    ast_node_t *node = ast_reexport_new(p->arena, tb, p->pos);
    if (!node) return NULL;
    ((ast_reexport_t *)node)->path = path;
    return node;
}

/* ---- parse_extern_lib（M6 FFI）---- */

/** 解析 `extern lib <别名> from "<库名>";`
 *
 * 调用方（parse_program）已消费 `extern` 关键字，tb 为其 token 下标（诊断跨度
 * 从 `extern` 起算）。
 *
 * **库名是通用名（generic name），不是文件名**：语法上写 `extern lib libc from
 * "c";`，执行器按平台把 `c` 展开成候选文件（Windows `c.dll`；Linux
 * `libc.so` / `c.so`；macOS 再加 `libc.dylib` / `c.dylib`）逐个尝试——与 CMake
 * 的 `find_library`/`target_link_libraries` 同名风格一致，源码因此**跨平台**。
 * 特殊名 `"*"` 仍表示"当前进程自身"。
 *
 * 也接受显式路径/文件名（`from "/abs/libfoo.so"`、`from "foo.dll"`）：解析器
 * 原样保留字符串，由加载器先按 as-is 尝试，失败再走通用名候选。
 *
 * 旧形式 `extern lib "<路径>" as <别名>;` 已删除，这里给出**迁移提示**而不是
 * 笼统语法错误。
 */
ast_node_t *parse_extern_lib(parser_t *p, uint32_t tb) {
    if (!expect_keyword(p, "lib")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected 'lib' after 'extern'");
    }
    skip_trivia(p);
    /* 旧语法：extern lib "<路径>" as <别名>; —— 明确指引迁移 */
    if (check_kind(p, TOKEN_TYPE_STRING)) {
        return ast_error_new(
            p->diag, p->tokens, p->arena, tb, p->pos,
            "old form 'extern lib \"<path>\" as <alias>;' is no longer "
            "supported; write 'extern lib <alias> from \"<name>\";' instead "
            "(<name> is a generic library name such as \"c\", resolved to "
            "c.dll / libc.so by the loader)");
    }
    if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected library alias identifier after 'extern lib'");
    }
    strslice_t alias = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);
    if (!expect_keyword(p, "from")) {
        return ast_error_new(
            p->diag, p->tokens, p->arena, tb, p->pos,
            "expected 'from' in 'extern lib <alias> from \"<name>\";'");
    }
    skip_trivia(p);
    if (!check_kind(p, TOKEN_TYPE_STRING)) {
        return ast_error_new(
            p->diag, p->tokens, p->arena, tb, p->pos,
            "expected library name string after 'from'");
    }
    strslice_t name_text = token_strslice(cur_token(p));
    advance(p);
    skip_trivia(p);
    /* 去掉首尾引号（与 import 路径同一约定：字符串不做转义展开） */
    strslice_t name = name_text;
    if (name.len >= 2 && name.ptr[0] == '"')
        name = strslice_from_bytes(name.ptr + 1, name.len - 2);
    if (name.len == 0) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "extern lib: empty library name");
    }
    if (!expect_symbol(p, ";")) {
        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                             "expected ';' after extern lib declaration");
    }
    ast_node_t *n = ast_extern_lib_new(p->arena, tb, p->pos);
    if (!n) return NULL;
    ((ast_extern_lib_t *)n)->alias = alias;
    ((ast_extern_lib_t *)n)->name  = name;
    return n;
}