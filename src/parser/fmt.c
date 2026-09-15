#include "parser/fmt.h"
#include "parser/lexer.h"
#include "core/allocator.h"
#include "core/vec.h"

#include <string.h>

/* ================================================================ */
/* clux 源码格式化（最小版）：基于 token 流规整 trivia                */
/* ================================================================ */
/*
 * 策略：不重排代码结构，只规整 token 之间的空白与缩进。
 *
 * 步骤：
 *   ① 过滤 trivia：把 WHITESPACE / COMMENT / MULTILINE_COMMENT 之外
 *      的实义 token 收集成数组，同时记录每个实义 token 之前遇到的
 *      trivia 信息（是否含换行、注释文本序列）。
 *   ② 按 token 序列 + 简单上下文规则输出：
 *        - '{' 跟随前行；非空块：'{' 后换行、缩进 +1；空块：紧凑 {}
 *        - '}' 单独一行；'}' 后 token 为 'else' 时同行（} else）
 *        - ';' 后换行（() 内除外）；',' 后一个空格
 *        - 运算符两侧空格；标识符/关键字/字面量之间空格
 *        - 注释原样保留在它出现的位置
 *   ③ 缩进一律 4 空格；输出以换行结束。
 */

#define CLUX_INDENT "    "

/* ---- 增长式输出缓冲 ---- */

typedef struct {
    allocator_t *alloc;
    char        *buf;
    size_t       len;
    size_t       cap;
} sb_t;

static void sb_reserve(sb_t *sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return;
    size_t nc = sb->cap ? sb->cap : 256;
    while (nc < sb->len + extra + 1) nc *= 2;
    char *nb = (char *)allocator_new_ex(
        sb->alloc, "clux.parser.fmt.buf", nc, NULL, NULL, NULL, 1);
    if (sb->buf) {
        memcpy(nb, sb->buf, sb->len);
        allocator_free(sb->alloc, (void **)&sb->buf);
    }
    sb->buf = nb;
    sb->cap = nc;
}

static void sb_put(sb_t *sb, const char *s, size_t n) {
    if (n == 0) return;
    sb_reserve(sb, n);
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
}

static void sb_str(sb_t *sb, const char *s) { sb_put(sb, s, strlen(s)); }
static void sb_ch(sb_t *sb, char c)         { sb_put(sb, &c, 1); }

static void sb_indent(sb_t *sb, int level) {
    for (int i = 0; i < level; i++) sb_str(sb, CLUX_INDENT);
}

/* ---- token 分类辅助 ---- */

static bool sym_is(const token_t *t, char c) {
    if (token_get_kind(t) != TOKEN_TYPE_SYMBOL) return false;
    size_t n = 0;
    const char *s = token_get_text(t, &n);
    return n == 1 && s && s[0] == c;
}

static bool kw_is(const token_t *t, const char *kw) {
    if (token_get_kind(t) != TOKEN_TYPE_KEYWORD) return false;
    return token_is(t, kw);
}

/* 词法上类似"值"的 token（标识符/关键字/字面量） */
static bool is_word_like(token_kind_t k) {
    return k == TOKEN_TYPE_IDENTIFIER || k == TOKEN_TYPE_KEYWORD ||
           k == TOKEN_TYPE_NUMERIC    || k == TOKEN_TYPE_STRING  ||
           k == TOKEN_TYPE_CHARACTER;
}

/* 运算符（两侧留空格） */
static bool is_operator(const token_t *t) {
    if (token_get_kind(t) != TOKEN_TYPE_SYMBOL) return false;
    static const char *ops[] = {
        "=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>=",
        "+", "-", "*", "/", "%",
        "==", "!=", "<", ">", "<=", ">=",
        "&&", "||", "&", "|", "^", "<<", ">>",
        "!", "~", "?", ":", "->", "=>",
        NULL,
    };
    for (size_t i = 0; ops[i]; i++) {
        if (token_is(t, ops[i])) return true;
    }
    return false;
}

/* 前一个 token 后是否禁用空格（`(`/`[` 后紧跟内容，`.` 后紧贴成员） */
static bool no_space_after(const token_t *t) {
    return sym_is(t, '(') || sym_is(t, '[') || sym_is(t, '.');
}

/* 当前 token 是否为**一元前缀**运算符。
 *  - `!` / `~` 永远是一元；
 *  - `-` / `+` 仅当其前置 token 处于"期待一元操作数"上下文时视为一元，
 *    例如 `= -x`、`(-x)`、`, -x`、`; -x`、行首、另一运算符之后、`return -x`。
 * 一元前缀运算符紧贴其后操作数，不插入空格（`-x` / `~x` / `!x` / `+(...)` 等）。
 * 这与二元 `+`/`-`（两侧留空格，如 `a + b`）区分开来。 */
static bool is_unary_prefix(const token_t *prev, const token_t *t) {
    if (token_get_kind(t) != TOKEN_TYPE_SYMBOL) return false;
    if (token_is(t, "!") || token_is(t, "~")) return true;
    if (!token_is(t, "-") && !token_is(t, "+")) return false;
    /* `-` / `+`：依靠上下文区分一元/二元 */
    if (!prev) return true;                 /* 行首 / 表达式起始 */
    if (sym_is(prev, '=') || sym_is(prev, ':') || sym_is(prev, ',') ||
        sym_is(prev, ';') || sym_is(prev, '(') || sym_is(prev, '[') ||
        is_operator(prev) || kw_is(prev, "return"))
        return true;
    return false;
}

/* 当前 token 前是否禁用空格（闭合符 / 分隔符 / 类型标注冒号） */
static bool no_space_before(const token_t *t) {
    return sym_is(t, ')') || sym_is(t, ']') || sym_is(t, ',') ||
           sym_is(t, ';') || sym_is(t, '.') || sym_is(t, ':');
}

/* 控制流关键字：其后紧跟 `(` 的须以空格分隔，写作 `if (cond)` /
 * `while (cond)` / `for (...)` / `foreach (x of ...)` / `switch (...)`.
 * 注意 `func f(` 这类函数名调用不属此列。
 *
 * 注意：clux 的软关键字（foreach / switch / of 等）在词法器中并未登记进
 * g_keywords，而是以标识符（IDENTIFIER）身份产出，到 parser 阶段才按上下文
 * 识别为关键字。因此这里直接按文本内容判定，而非依赖 kind == KEYWORD，
 * 否则 foreach(x of b) 会被判为非控制流关键字而漏掉应有的空格。 */
static bool is_control_keyword(const token_t *t) {
    if (!t) return false;
    return token_is(t, "if") || token_is(t, "while") ||
           token_is(t, "for") || token_is(t, "foreach") ||
           token_is(t, "switch");
}

/* `(` 之前是否需要空格：
 *  - 控制流关键字调用括号 `if (` / `while (` / `foreach (` / `switch (` 须空格；
 *  - 运算符后的括号 `a + (b)` / `x = (y)` 须空格（否则 `+(` / `=(` 紧贴破坏可读性）；
 *  - 函数调用 / 泛型实例化的 `name(` 紧贴（`main(`, `foo(`）。
 * 注意：clux 软关键字（foreach / switch 等）以标识符身份产出，故控制流判定
 * 按文本而非 kind。 */
static bool need_space_before_paren(const token_t *prev,
                                    const token_t *cur) {
    if (!sym_is(cur, '(')) return false;
    if (!prev) return false;
    return is_control_keyword(prev) || is_operator(prev);
}

/* 两个 token 在源码中是否紧贴（无任何字符间隔）。
 *
 * 注意：lexer 不把数值的类型后缀并入 NUMERIC，而是切成两个 token
 * （`7i8` → NUMERIC("7") + IDENTIFIER("i8")，后缀由 parser 消费）。这类
 * 组合在语法上必须紧贴，格式化时若插入空格会直接改变语义（`7 i8`）。
 * 用字节偏移判断相邻性，从而只对"源码本就紧贴"的后缀生效。 */
static bool tok_adjacent(const token_t *a, const token_t *b) {
    if (!a || !b) return false;
    const location_t *la = token_get_location(a);
    const location_t *lb = token_get_location(b);
    if (!la || !lb) return false;
    return la->end.offset == lb->begin.offset;
}

/* 当前 token 是否为前一 NUMERIC 的**紧贴类型后缀**（`7i8` / `2.5f32`）。
 * 后缀是标识符且与数字在源码中相邻（区别于 `7 i8` 这种本就分开的写法）。 */
/* 当前 token 是否为前一 NUMERIC 的**紧贴类型后缀**（`7i8` / `2.5f32`）。
 * 词法器把数值与类型后缀切成两个 token（`7` + `i8`，后缀由 parser 消费）。
 * 后缀是类型名，在词法器中以关键字形式出现（kind = KEYWORD），也可能落为
 * 普通标识符，二者都算"紧贴后缀"。若在此处插入空格会直接改变语义
 * （`7 i8` 不再是字面量）。用字节偏移判断相邻性，只对"源码本就紧贴"的
 * 后缀生效（区别于 `7 i8` 这种本就分开的写法）。 */
static bool is_numeric_suffix(const token_t *prev, const token_t *cur) {
    if (!prev || !cur) return false;
    if (token_get_kind(prev) != TOKEN_TYPE_NUMERIC) return false;
    token_kind_t ck = token_get_kind(cur);
    if (ck != TOKEN_TYPE_IDENTIFIER && ck != TOKEN_TYPE_KEYWORD) return false;
    return tok_adjacent(prev, cur);
}

/* 该实义 token 之前是否有换行（来自其前导空白） */
typedef struct {
    const token_t *tok;
    bool           leading_newline;   /* 前面是否出现过换行 */
    bool           leading_blank;     /* 前面是否出现过空行（>=2 换行） */
} fmt_item_t;

/* ---- 预看：tok[i+1] 是否为 '}'（用于空块判定） ---- */

char *fmt_format(allocator_t *alloc, const vec_t *tokens, size_t *out_len) {
    if (!alloc || !tokens) return NULL;
    if (out_len) *out_len = 0;

    size_t n = vec_len(tokens);

    /* ① 收集实义 token + 前导换行信息（用 alloc 临时数组） */
    fmt_item_t *items = (fmt_item_t *)allocator_new_ex(
        alloc, "clux.parser.fmt.items", (n ? n : 1) * sizeof(fmt_item_t),
        NULL, NULL, NULL, 1);
    size_t count = 0;

    bool pending_newline = false;
    size_t pending_nl_count = 0;

    for (size_t i = 0; i < n; i++) {
        const token_t *tok = (const token_t *)vec_get(tokens, i);
        if (!tok) continue;
        token_kind_t kind = token_get_kind(tok);
        if (kind == TOKEN_TYPE_EOF) break;

        if (kind == TOKEN_TYPE_WHITESPACE) {
            size_t wl = 0;
            const char *w = token_get_text(tok, &wl);
            for (size_t k = 0; k < wl; k++) {
                if (w[k] == '\n') {
                    pending_newline = true;
                    pending_nl_count++;
                }
            }
            continue;
        }
        if (kind == TOKEN_TYPE_COMMENT || kind == TOKEN_TYPE_MULTILINE_COMMENT) {
            /* 注释也作为 item 保留（需原样输出） */
            items[count].tok = tok;
            items[count].leading_newline = pending_newline;
            items[count].leading_blank = pending_nl_count >= 2;
            count++;
            pending_newline = false;
            pending_nl_count = 0;
            continue;
        }

        items[count].tok = tok;
        items[count].leading_newline = pending_newline;
        items[count].leading_blank = pending_nl_count >= 2;
        count++;
        pending_newline = false;
        pending_nl_count = 0;
    }

    /* ② 逐 item 输出 */
    sb_t sb = { alloc, NULL, 0, 0 };
    int  indent = 0;
    int  paren_depth = 0;
    int  ternary_pending = 0; /* 未配对的 '?' 数量（三元冒号两侧留空格判定） */
    bool at_line_start = true;
    const token_t *prev = NULL;   /* 上一个输出过的实义 token（注释不计） */
    bool prev_unary_prefix = false; /* 上一 token 是否一元前缀（其操作数须紧贴） */

    for (size_t i = 0; i < count; i++) {
        const token_t *tok = items[i].tok;
        token_kind_t kind = token_get_kind(tok);
        size_t tlen = 0;
        const char *text = token_get_text(tok, &tlen);

        /* ---- 注释：原样保留 ---- */
        if (kind == TOKEN_TYPE_COMMENT || kind == TOKEN_TYPE_MULTILINE_COMMENT) {
            /* 关键：区分行内注释（源码中与前一 token 同行）与独立行注释。
             *   - 行内：紧跟前一 token，中间一个空格，注释归属前一语句；
             *   - 独立行：换行后按当前缩进输出（保留源码的空行分组）。 */
            bool inline_comment = !at_line_start &&
                                  !items[i].leading_newline;
            if (inline_comment) {
                sb_ch(&sb, ' ');
            } else {
                /* 独立行注释：若源码中其前有换行/空行，则换行输出 */
                if (!at_line_start) sb_ch(&sb, '\n');
                /* 源码中的空行 → 输出一个空行（保留用户分组意图） */
                if (items[i].leading_blank) sb_ch(&sb, '\n');
                sb_indent(&sb, indent);
                at_line_start = false;
            }
            sb_put(&sb, text, tlen);

            if (kind == TOKEN_TYPE_COMMENT) {
                sb_ch(&sb, '\n');
                at_line_start = true;
            } else if (i + 1 < count && items[i + 1].leading_newline) {
                /* 块注释后源码中有换行 → 换行 */
                sb_ch(&sb, '\n');
                at_line_start = true;
            }
            /* 行内块注释后无换行 → 保持同行，由后续 token 接续 */
            continue;
        }

        bool is_open  = sym_is(tok, '{');
        bool is_close = sym_is(tok, '}');
        bool is_semi  = sym_is(tok, ';');
        bool is_comma = sym_is(tok, ',');
        bool is_else  = kw_is(tok, "else");

        /* ---- '}' ---- */
        if (is_close) {
            bool empty = prev && sym_is(prev, '{');
            if (empty) {
                /* 空块：紧接 '{' 输出 '}'（'{' 处未换行、未增缩进） */
                sb_ch(&sb, '}');
                at_line_start = false;
                prev = tok;
                /* 空块后：若不是 ; , ) 则换行 */
                bool next_glue = (i + 1 < count) &&
                                 (sym_is(items[i + 1].tok, ';') ||
                                  sym_is(items[i + 1].tok, ',') ||
                                  sym_is(items[i + 1].tok, ')') ||
                                  kw_is(items[i + 1].tok, "else"));
                if (!next_glue) {
                    sb_ch(&sb, '\n');
                    at_line_start = true;
                }
                continue;
            }
            if (indent > 0) indent--;
            if (!at_line_start) sb_ch(&sb, '\n');
            sb_indent(&sb, indent);
            sb_ch(&sb, '}');
            at_line_start = false;
            prev = tok;
            /* '}' 后若为 else 则同行；否则换行（由下一个 token 决定） */
            if (i + 1 < count && !kw_is(items[i + 1].tok, "else") &&
                !sym_is(items[i + 1].tok, ';') && !sym_is(items[i + 1].tok, ',') &&
                !sym_is(items[i + 1].tok, ')') && !sym_is(items[i + 1].tok, '.')) {
                sb_ch(&sb, '\n');
                at_line_start = true;
            }
            continue;
        }

        /* ---- '{' ---- */
        if (is_open) {
            /* 空块判定：下一个实义/注释 item 是否为 '}' */
            bool empty_block = false;
            for (size_t j = i + 1; j < count; j++) {
                const token_t *nt = items[j].tok;
                token_kind_t nk = token_get_kind(nt);
                if (nk == TOKEN_TYPE_COMMENT || nk == TOKEN_TYPE_MULTILINE_COMMENT)
                    break; /* 有注释 → 非空块 */
                empty_block = sym_is(nt, '}');
                break;
            }

            if (at_line_start) {
                sb_indent(&sb, indent);
                at_line_start = false;
            } else if (prev) {
                /* '{' 跟随前行：前面补一个空格 */
                sb_ch(&sb, ' ');
            }
            sb_ch(&sb, '{');
            prev = tok;

            if (empty_block) {
                at_line_start = false;  /* '}' 将紧跟输出 */
            } else {
                sb_ch(&sb, '\n');
                at_line_start = true;
                indent++;
            }
            continue;
        }

        /* ---- 其余实义 token ---- */

        /* `else` 紧跟 `}` 同行：`} else`（一个空格分隔） */
        bool glue_else = is_else && prev && sym_is(prev, '}');

        if (at_line_start) {
            /* 源码中此 token 前有空行 → 保留一个空行（用户分组意图） */
            if (items[i].leading_blank && sb.len > 0 &&
                sb.buf[sb.len - 1] == '\n') {
                sb_ch(&sb, '\n');
            }
            sb_indent(&sb, indent);
            at_line_start = false;
            prev_unary_prefix = false;   /* 换行后一元前缀标志失效 */
        } else if (glue_else) {
            sb_ch(&sb, ' ');
        } else {
            bool need = false;
            if (prev_unary_prefix) {
                /* 上一 token 是一元前缀运算符：其操作数须紧贴，不插空格 */
                need = false;
                prev_unary_prefix = false;
            } else if (prev) {
                if (is_numeric_suffix(prev, tok)) {
                    need = false;   /* 数字类型后缀紧贴：7i8 / 2.5f32 */
                } else if (sym_is(tok, '(')) {
                    /* `(` 前：控制流关键字要空格（`if (`），
                     * 函数调用/泛型实例化紧贴（`main(`, `foo(`） */
                    need = need_space_before_paren(prev, tok);
                } else if (prev && sym_is(prev, ']') &&
                           is_word_like(token_get_kind(tok))) {
                    /* 数组类型 [N]T 紧贴（`[1]i32` / `[2][3]i32`），
                     * ']' 后紧跟类型名时不应插入空格 */
                    need = false;
                } else if (sym_is(tok, ':') && ternary_pending > 0) {
                    /* 三元冒号（与 '?' 配对）：两侧留空格 `a ? b : c`；
                       类型标注冒号（var x: i32）仍前不插空格（no_space_before） */
                    ternary_pending--;
                    need = true;
                } else if (!no_space_before(tok) && !no_space_after(prev)) {
                    need = true;   /* 运算符/字面量/标识符等均以单空格分隔 */
                }
            }
            if (need) sb_ch(&sb, ' ');
        }

        sb_put(&sb, text, tlen);
        at_line_start = false;

        if (sym_is(tok, '(')) paren_depth++;
        else if (sym_is(tok, ')') && paren_depth > 0) paren_depth--;
        if (sym_is(tok, '?')) ternary_pending++;

        prev_unary_prefix = is_unary_prefix(prev, tok);
        prev = tok;

        if (is_semi) {
            /* 行内注释保护：若源码中紧跟一个"同行注释"（其前无换行），
             * 则不在此处换行——注释归属本条语句，须与之同行。 */
            bool trailing_comment = (i + 1 < count) &&
                                    !items[i + 1].leading_newline &&
                                    (token_get_kind(items[i + 1].tok) == TOKEN_TYPE_COMMENT ||
                                     token_get_kind(items[i + 1].tok) == TOKEN_TYPE_MULTILINE_COMMENT);

            if (paren_depth > 0) {
                /* for (a; b; c)：不在此处补空格，交给下一个 token 的
                 * 常规空格判定（避免与它重复插入两个空格） */
                at_line_start = false;
            } else if (trailing_comment) {
                at_line_start = false;   /* 注释将紧随其后，保持同行 */
            } else {
                sb_ch(&sb, '\n');
                at_line_start = true;
                prev = NULL;           /* 换行后不参与跨行空格判定 */
                prev_unary_prefix = false;
            }
        } else if (is_comma) {
            /* 逗号后同样交给下一个 token 判定 */
            at_line_start = false;
        }
    }

    /* ③ 收尾：以换行结束 */
    if (sb.len > 0 && sb.buf[sb.len - 1] != '\n') sb_ch(&sb, '\n');
    sb_reserve(&sb, 0);
    sb.buf[sb.len] = '\0';

    allocator_free(alloc, (void **)&items);
    if (out_len) *out_len = sb.len;
    return sb.buf;
}
