#ifndef _H_CLUX_PARSER_PARSE_UTILS_
#define _H_CLUX_PARSER_PARSE_UTILS_
#ifdef __cplusplus
extern "C" {
#endif

#include "parser/parser.h"
#include "parser/lexer.h"
#include "core/strslice.h"
#include "parser/ast_node.h"

/* ---- 游标操作 ---- */

/** 当前 token（不推进游标）。EOF 时返回 EOF token，不会返回 NULL。 */
const token_t *cur_token(const parser_t *p);

/** 前一个 token（pos > 0 时有效）。 */
const token_t *prev_token(const parser_t *p);

/** 推进游标一步，返回被跳过的 token。不跳 trivia。 */
const token_t *advance(parser_t *p);

/** 跳过空白和注释，停在下一个有效 token 上。 */
void skip_trivia(parser_t *p);

/** 推进并跳 trivia（advance + skip_trivia 的便捷组合）。 */
const token_t *advance_skip(parser_t *p);

/** 是否到达 EOF。 */
bool at_end(const parser_t *p);

/* ---- 查看（不消费） ---- */

/** 当前 token 是指定关键字？ */
bool check_keyword(const parser_t *p, const char *kw);

/** 当前 token 是指定符号？ */
bool check_symbol(const parser_t *p, const char *sym);

/**
 * 合成二元移位运算符：当前与下一有效 token 均为 '<'（或 '>'）时返回
 * "<"/">" 两字符 token。lexer 不产出 << / >>（与嵌套元组类型语法冲突），
 * 中缀位置的连续尖括号由 parser 在此合成。
 * 不消费 token；未命中返回 NULL。返回的 token 由 arena 分配。
 */
const token_t *synthesize_shift_token(parser_t *p);

/** 当前 token 是指定 kind？ */
bool check_kind(const parser_t *p, token_kind_t kind);

/* ---- 匹配与消费 ---- */

/** 当前 token 是指定关键字？匹配则消费返回 true，否则不动返回 false。 */
bool match_keyword(parser_t *p, const char *kw);

/** 当前 token 是指定符号？匹配则消费返回 true，否则不动返回 false。 */
bool match_symbol(parser_t *p, const char *sym);

/* ---- 期望与报错 ---- */

/** 当前 token 必须是指定关键字，成功则消费返回 true；失败报错返回 false，不推进游标。 */
bool expect_keyword(parser_t *p, const char *kw);

/** 当前 token 必须是指定符号，成功则消费返回 true；失败报错返回 false，不推进游标。 */
bool expect_symbol(parser_t *p, const char *sym);

/* ---- 诊断 ---- */

/** 报告语法错误（不改变游标），置 has_error。 */
void parse_error(parser_t *p, const char *fmt, ...);

/* ---- Token → strslice 便捷 ---- */

/** 取 token 的零拷贝文本切片。 */
strslice_t token_strslice(const token_t *t);

/* ---- 字面量解析工具 ---- */

/**
 * 解析单个 escape 序列，返回码点值，*consumed 设为消耗的字节数。
 * text 指向反斜杠后面的第一个字符（不含反斜杠）。
 */
uint32_t parse_escape_seq(const char *text, size_t len, size_t *consumed);

/**
 * 计算一个码点的 UTF-8 编码长度。
 */
size_t utf8_encode_len(uint32_t cp);

/**
 * 将码点写入 UTF-8 缓冲区，返回写入字节数。
 */
size_t utf8_encode(uint32_t cp, char *out);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_PARSE_UTILS_ */
