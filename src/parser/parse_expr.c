#include "parser/parse_expr.h"
#include "parser/parse_utils.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_ident.h"
#include "parser/ast_const.h"
#include "parser/ast_volatile.h"
#include "parser/ast_option.h"
#include "parser/ast_fill.h"
#include "parser/ast_undef.h"
#include "parser/ast_nil.h"
#include "parser/ast_unary.h"
#include "parser/ast_binary.h"
#include "parser/ast_assign.h"
#include "parser/ast_call.h"
#include "parser/ast_member.h"
#include "parser/ast_index.h"
#include "parser/ast_array.h"
#include "parser/ast_func_def.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_enum_ref.h"
#include "parser/ast_error.h"
#include "parser/ast_ternary.h"
#include "parser/ast_unwrap.h"

/* ---- Pratt parser 绑定力表 ---- */

/**
 * 查询中缀运算符的左右绑定力。
 * 遵循 M1 文档 4.1 节定义。
 *
 * 左结合运算符：right_prec = left_prec + 1
 * 返回 false 表示当前 token 不是中缀运算符。
 * 注意：赋值运算符不在此表中，由 parse_expr_prec 单独处理。
 */
static bool infix_binding(const token_t *tok, int *lp, int *rp) {
    if (token_get_kind(tok) != TOKEN_TYPE_SYMBOL &&
        token_get_kind(tok) != TOKEN_TYPE_KEYWORD) {
        return false;
    }

    strslice_t s = token_strslice(tok);

    /* 关键字运算符 */
    if (s.len == 2 && s.ptr[0] == 'a' && s.ptr[1] == 's') {
        *lp = 21; *rp = 22;
        return true;
    }
    /* extends：类型兼容判断（类型计算），与 ==/!= 同级比较运算符 */
    if (s.len == 7 && s.ptr[0] == 'e' && s.ptr[1] == 'x' && s.ptr[2] == 't' &&
        s.ptr[3] == 'e' && s.ptr[4] == 'n' && s.ptr[5] == 'd' &&
        s.ptr[6] == 's') {
        *lp = 11; *rp = 12;
        return true;
    }

    /* 双字符符号运算符 */
    if (s.len == 2) {
        if (s.ptr[0] == '|' && s.ptr[1] == '|') { *lp = 1;  *rp = 2;  return true; }
        if (s.ptr[0] == '&' && s.ptr[1] == '&') { *lp = 3;  *rp = 4;  return true; }
        if (s.ptr[0] == '=' && s.ptr[1] == '=') { *lp = 11; *rp = 12; return true; }
        if (s.ptr[0] == '!' && s.ptr[1] == '=') { *lp = 11; *rp = 12; return true; }
        if (s.ptr[0] == '<' && s.ptr[1] == '=') { *lp = 13; *rp = 14; return true; }
        if (s.ptr[0] == '>' && s.ptr[1] == '=') { *lp = 13; *rp = 14; return true; }
        if (s.ptr[0] == '<' && s.ptr[1] == '<') { *lp = 15; *rp = 16; return true; }
        if (s.ptr[0] == '>' && s.ptr[1] == '>') { *lp = 15; *rp = 16; return true; }
        /* 复合赋值：+= -= *= /= %= */
        if (s.ptr[1] == '=' && (s.ptr[0] == '+' || s.ptr[0] == '-' ||
                                s.ptr[0] == '*' || s.ptr[0] == '/' ||
                                s.ptr[0] == '%')) {
            /* 赋值优先级：低于所有二元运算符，右结合
             * 不返回到 binding 表，由 parse_expr_prec 单独处理 */
            return false;
        }
        return false;
    }

    /* 单字符符号运算符 */
    if (s.len == 1) {
        switch (s.ptr[0]) {
        case '|': *lp = 5;  *rp = 6;  return true;
        case '^': *lp = 7;  *rp = 8;  return true;
        case '&': *lp = 9;  *rp = 10; return true;
        case '<': *lp = 13; *rp = 14; return true;
        case '>': *lp = 13; *rp = 14; return true;
        case '+': *lp = 17; *rp = 18; return true;
        case '-': *lp = 17; *rp = 18; return true;
        case '*': *lp = 19; *rp = 20; return true;
        case '/': *lp = 19; *rp = 20; return true;
        case '%': *lp = 19; *rp = 20; return true;
        case '=':
            /* 简单赋值 = ：最低优先级，右结合，单独处理 */
            return false;
        default:  return false;
        }
    }

    return false;
}

/* ---- 赋值运算符判断 ---- */

/** 赋值运算符的绑定力（最低，右结合） */
#define ASSIGN_LEFT_PREC  0

/** 三元条件表达式的绑定力（与赋值同级，低于所有二元运算符；右结合） */
#define TERNARY_LEFT_PREC 0

static bool is_assign_op_token(const token_t *tok) {
    if (token_get_kind(tok) != TOKEN_TYPE_SYMBOL) return false;
    strslice_t s = token_strslice(tok);
    if (s.len == 1 && s.ptr[0] == '=') return true;
    if (s.len == 2 && s.ptr[1] == '=') {
        char c = s.ptr[0];
        return c == '+' || c == '-' || c == '*' || c == '/' || c == '%';
    }
    return false;
}

/* ---- parse_construct_field: 构造器字段（含 <v,N> 值包） ---- */

/**
 * 解析构造器字段：.<type>{ f1, f2, ... } 中的单个字段 f。
 *
 * 特殊形态：<v,N> 值包（尖括号前导的"值 × 重复次数"批量初始化，如
 * .[1000]i32{<0,1000>}）。值包只在 CONSTRUCT 的匿名构造字段链中存在，
 * 与普通表达式位置的 <...>（元组类型，M2 预留）天然区别——本函数仅在
 * 构造器字段上下文被调用，遇到 '<' 一律按值包解析；普通表达式位置的
 * '<' 不会进入此路径。
 *
 * 值包语法：<expr, expr>（v=填充值，N=重复次数编译期常量）。
 *   - v 用 parse_expr 解析（',' 天然停止，v 可为任意表达式含比较）
 *   - N 用 parse_expr_prec(p, 15) 解析（绑定力高于 '>' 的 13/14，
 *     保证 '>' 不被当比较运算符消费，N 解析在 '>' 前停止）
 */
static ast_node_t *parse_construct_field(parser_t *p) {
    /* 值包：<v,N> */
    if (check_symbol(p, "<")) {
        uint32_t fb = p->pos;
        advance(p);
        skip_trivia(p);

        ast_node_t *value = parse_expr(p);
        if (!value || value->kind == AST_ERROR) {
            if (!value) {
                return ast_error_new(p->diag, p->tokens, p->arena, fb, p->pos,
                                     "expected value in fill '<v,N>'");
            }
            return value;
        }
        skip_trivia(p);
        if (!expect_symbol(p, ",")) {
            return ast_error_new(p->diag, p->tokens, p->arena, fb, p->pos,
                                 "expected ',' in fill '<v,N>'");
        }
        skip_trivia(p);

        ast_node_t *count = parse_expr_prec(p, 15);
        if (!count || count->kind == AST_ERROR) {
            if (!count) {
                return ast_error_new(p->diag, p->tokens, p->arena, fb, p->pos,
                                     "expected count in fill '<v,N>'");
            }
            return count;
        }
        skip_trivia(p);
        if (!expect_symbol(p, ">")) {
            return ast_error_new(p->diag, p->tokens, p->arena, fb, p->pos,
                                 "expected '>' in fill '<v,N>'");
        }

        ast_node_t *node = ast_fill_new(p->arena, fb, p->pos);
        ((ast_fill_t *)node)->value = value;
        ((ast_fill_t *)node)->count = count;
        return node;
    }

    /* 具名字段：.name = expr（struct 构造字段形态）。回溯探测 `.IDENT =`
       ——命中则构造 AST_CONSTRUCT_FIELD；否则恢复游标回落普通表达式
       （嵌套构造 .Point{...} / 成员访问 .x 等由 parse_expr 重新解析）。 */
    if (check_symbol(p, ".")) {
        uint32_t save = p->pos;
        uint32_t fb2 = p->pos;
        advance(p);
        skip_trivia(p);
        if (check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
            strslice_t name = token_strslice(cur_token(p));
            advance(p);
            skip_trivia(p);
            if (check_symbol(p, "=")) {
                advance(p);
                skip_trivia(p);
                ast_node_t *value = parse_expr(p);
                if (!value || value->kind == AST_ERROR) {
                    if (!value) {
                        return ast_error_new(p->diag, p->tokens, p->arena, fb2,
                                             p->pos,
                                             "expected value after '.field =' "
                                             "in construct");
                    }
                    return value;
                }
                ast_node_t *n = ast_construct_field_new(p->arena, fb2, p->pos);
                if (!n) return NULL;
                ((ast_construct_field_t *)n)->name  = name;
                ((ast_construct_field_t *)n)->value = value;
                return n;
            }
        }
        p->pos = save; /* 非具名字段：恢复游标，回落普通表达式 */
    }

    /* 普通字段表达式 */
    return parse_expr(p);
}

/* ---- parse_primary: 原子表达式入口 ---- */

ast_node_t *parse_primary(parser_t *p) {
    ast_node_t *node;

    /* 函数类型表达式：func(param_types...)->ret（'func' 关键字分支，
       统一生成式入口 parse_func_like，')' 后按 ':'/'->' 分叉。必须先于
       末尾 TOKEN_TYPE_KEYWORD 的 AST_IDENT 兜底）。 */
    if (check_keyword(p, "func")) {
        node = parse_func_like(p, AST_FUNC_TYPE);
        if (node) return node;
    }

    node = parse_bool_lit(p);    if (node) return node;
    node = parse_float_lit(p);   if (node) return node;
    node = parse_int_lit(p);     if (node) return node;
    node = parse_string_lit(p);  if (node) return node;
    node = parse_char_lit(p);    if (node) return node;
    node = parse_ident(p);       if (node) return node;
    node = parse_undef(p);       if (node) return node;
    node = parse_nil(p);         if (node) return node;

    /* 分组表达式：(expr) */
    if (check_symbol(p, "(")) {
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);
        ast_node_t *inner = parse_expr(p);
        if (!inner) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected expression after '('");
        }
        if (inner->kind == AST_ERROR) return inner;
        skip_trivia(p);
        if (!expect_symbol(p, ")")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ')' after grouped expression");
        }
        return inner;
    }

    /* 数组类型表达式：[N]T（N=长度，T=基础类型）。
     * 仅当 '[' 出现在原子位置时（无 lhs）解析为类型，后缀 '[' 仍归 AST_INDEX。
     * ']' 与基础类型 T 之间的空格/注释由 skip_trivia 天然容错。 */
    if (check_symbol(p, "[")) {
        uint32_t tb = p->pos;
        advance(p);
        skip_trivia(p);

        ast_node_t *length = parse_expr(p);
        if (!length || length->kind == AST_ERROR) {
            if (!length) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected length expression after '[' in array type");
            }
            return length;
        }
        skip_trivia(p);
        if (!expect_symbol(p, "]")) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected ']' after array type length");
        }
        skip_trivia(p);

        /* base_type 用 parse_unary：支持 const/volatile 修饰、嵌套 [M][N]T */
        ast_node_t *base_type = parse_unary(p);
        if (!base_type || base_type->kind == AST_ERROR) {
            if (!base_type) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected base type after ']' in array type");
            }
            return base_type;
        }

        ast_node_t *node = ast_array_new(p->arena, tb, p->pos);
        ((ast_array_t *)node)->base_type = base_type;
        ((ast_array_t *)node)->length    = length;
        return node;
    }

    /* 关键字 → 标识符引用（类型即表达式）：
     * 类型名（i32/bool/...）与关键字在表达式位置统一解析为 AST_IDENT，
     * 消费层感知 value 是否为类型。const/volatile 已在 parse_unary
     * 提前消费为 AST_CONST/AST_VOLATILE，不会落到此处。 */
    if (check_kind(p, TOKEN_TYPE_KEYWORD)) {
        uint32_t tb = p->pos;
        const token_t *t = cur_token(p);
        strslice_t name = token_strslice(t);
        advance(p);
        ast_node_t *id = ast_ident_new(p->arena, tb, p->pos);
        if (!id) return NULL;
        ((ast_ident_t *)id)->name = name;
        return id;
    }

    return NULL;
}

/* ---- parse_unary: 前缀一元表达式 ---- */

/** 前缀运算符右绑定力 = 23（M1 文档） */
#define PREFIX_RIGHT_PREC 23

ast_node_t *parse_unary(parser_t *p) {
    uint32_t tb = p->pos;

    const token_t *op_tok = NULL;
    if (check_symbol(p, "!"))      op_tok = cur_token(p);
    else if (check_symbol(p, "~")) op_tok = cur_token(p);
    else if (check_symbol(p, "-")) op_tok = cur_token(p);
    else if (check_symbol(p, ".")) op_tok = cur_token(p);
    else if (check_symbol(p, "?")) op_tok = cur_token(p);
    else if (check_keyword(p, "const"))     op_tok = cur_token(p);
    else if (check_keyword(p, "volatile"))  op_tok = cur_token(p);

    if (!op_tok) return parse_primary(p);

    /* 类型字面量前缀：.<type> { fields } → AST_CONSTRUCT。
     * 构造语法必须 '.' 前导（与 %v 调试输出 '.type{value}' 一致）。
     * 注意 '.' 在此处仅作为表达式起始前缀；成员访问 '.' 由 parse_postfix 处理，
     * 不会与前置 '.' 冲突。 */
    if (token_is(op_tok, ".")) {
        uint32_t dot_pos = tb;
        advance(p);                  /* 消费 '.' */
        skip_trivia(p);

        ast_node_t *type = NULL;
        /* 匿名构造：'.' 后直接 '{' → type=NULL，目标类型由 sema 依上下文
         * 推断（var 声明/赋值/嵌套字段）。否则照旧解析具名类型。 */
        if (!check_symbol(p, "{")) {
            type = parse_unary(p);   /* 类型：i32 / [N]T / const i32 / 嵌套 */
            if (!type || type->kind == AST_ERROR) {
                if (!type) {
                    return ast_error_new(p->diag, p->tokens, p->arena, dot_pos, p->pos,
                                         "expected type after '.' in typed literal");
                }
                return type;
            }
            /* type 解析（如 [N] i32）返回后游标可能停在 trivia 上（关键字消费不跳空白），
             * 检查 '{' 前必须跳过，否则 '}' 前的空格会导致误报。 */
            skip_trivia(p);
            if (!check_symbol(p, "{")) {
                return ast_error_new(p->diag, p->tokens, p->arena, dot_pos, p->pos,
                                     "expected '{' after type in typed literal");
            }
        }
        advance(p);
        skip_trivia(p);

        ast_node_t *fields = NULL, *fields_last = NULL;
        if (!check_symbol(p, "}")) {
            ast_node_t *f = parse_construct_field(p);
            if (!f || f->kind == AST_ERROR) {
                if (!f) {
                    return ast_error_new(p->diag, p->tokens, p->arena, dot_pos, p->pos,
                                         "expected expression in construct");
                }
                return f;
            }
            ast_append(&fields, &fields_last, NULL, f);
            skip_trivia(p);
            while (check_symbol(p, ",")) {
                advance(p);
                skip_trivia(p);
                f = parse_construct_field(p);
                if (!f || f->kind == AST_ERROR) {
                    if (!f) {
                        return ast_error_new(p->diag, p->tokens, p->arena, dot_pos, p->pos,
                                             "expected expression after ',' in construct");
                    }
                    return f;
                }
                ast_append(&fields, &fields_last, NULL, f);
                skip_trivia(p);
            }
        }
        if (!expect_symbol(p, "}")) {
            return ast_error_new(p->diag, p->tokens, p->arena, dot_pos, p->pos,
                                 "expected '}' after construct fields");
        }

        ast_node_t *node = ast_construct_new(p->arena, dot_pos, p->pos);
        ((ast_construct_t *)node)->type        = type;
        ((ast_construct_t *)node)->fields      = fields;
        ((ast_construct_t *)node)->fields_last = fields_last;
        return node;
    }

    advance(p);
    skip_trivia(p);

    ast_node_t *operand = parse_expr_prec(p, PREFIX_RIGHT_PREC);
    if (!operand || operand->kind == AST_ERROR) {
        if (!operand) {
            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                 "expected expression after unary operator");
        }
        return operand;
    }

    /* const/volatile：类型修饰节点（类型即表达式），嵌套递归表达修饰顺序，
       重复 const（const const T）语法合法，消费层收敛。 */
    if (token_is(op_tok, "const")) {
        ast_node_t *node = ast_const_new(p->arena, tb, p->pos);
        ((ast_const_t *)node)->sub = operand;
        return node;
    }
    if (token_is(op_tok, "volatile")) {
        ast_node_t *node = ast_volatile_new(p->arena, tb, p->pos);
        ((ast_volatile_t *)node)->sub = operand;
        return node;
    }
    /* ?T optional 类型修饰：与 const/volatile 同族，嵌套递归表达包裹顺序
       （?[N]T / ?const i32 等）。注意 parse_expr_prec 2a 的 '?' 三元检查在
       中缀位置，此处是前缀位置——'?' 出现在原子位置即类型前导，二者不冲突。 */
    if (token_is(op_tok, "?")) {
        ast_node_t *node = ast_option_new(p->arena, tb, p->pos);
        ((ast_option_t *)node)->sub = operand;
        return node;
    }

    ast_node_t *node = ast_unary_new(p->arena, tb, p->pos);
    ((ast_unary_t *)node)->op      = op_tok;
    ((ast_unary_t *)node)->operand = operand;
    return node;
}

/* ---- parse_postfix: 后缀表达式（绑定力 25，贪婪循环） ---- */

/** 后缀绑定力 = 25（M1 文档，函数调用最高） */
#define POSTFIX_LEFT_PREC 25

static ast_node_t *parse_postfix(parser_t *p, ast_node_t *lhs) {
    for (;;) {
        skip_trivia(p);

        /* 枚举 variant 引用：Type::Variant（lhs 须为枚举类型名标识符）。
         * '::' 是 lexer 产出的双字符 SYMBOL token。 */
        if (check_symbol(p, "::")) {
            if (lhs->kind != AST_IDENT) {
                return ast_error_new(p->diag, p->tokens, p->arena, lhs->tok_begin,
                                     p->pos, "expected enum type name before '::'");
            }
            uint32_t tb = p->pos;
            advance(p);
            skip_trivia(p);

            if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected variant name after '::'");
            }
            strslice_t variant = token_strslice(cur_token(p));
            advance(p);

            ast_node_t *node = ast_enum_ref_new(p->arena, tb, p->pos);
            if (!node) return NULL;
            ((ast_enum_ref_t *)node)->type_expr = lhs;
            ((ast_enum_ref_t *)node)->variant   = variant;
            lhs = node;
            continue;
        }

        /* 函数调用：(args...) */
        if (check_symbol(p, "(")) {
            uint32_t tb = p->pos;
            advance(p);
            skip_trivia(p);

            ast_node_t *args = NULL, *args_last = NULL;

            if (!check_symbol(p, ")")) {
                ast_node_t *arg = parse_expr(p);
                if (!arg || arg->kind == AST_ERROR) {
                    if (!arg) {
                        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                             "expected expression in function call");
                    }
                    return arg;
                }
                ast_append(&args, &args_last, NULL, arg);

                skip_trivia(p);
                while (check_symbol(p, ",")) {
                    advance(p);
                    skip_trivia(p);
                    arg = parse_expr(p);
                    if (!arg || arg->kind == AST_ERROR) {
                        if (!arg) {
                            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                                 "expected expression after ','");
                        }
                        return arg;
                    }
                    ast_append(&args, &args_last, NULL, arg);
                    skip_trivia(p);
                }
            }

            if (!expect_symbol(p, ")")) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected ')' after function call arguments");
            }

            ast_node_t *node = ast_call_new(p->arena, tb, p->pos);
            ((ast_call_t *)node)->callee    = lhs;
            ((ast_call_t *)node)->args      = args;
            ((ast_call_t *)node)->args_last = args_last;
            lhs = node;
            continue;
        }

        /* 成员访问 / optional 解包：.field / .!（assert）/ .?（try）。
         * .! / .? 不做双字符 token（与构造器 .?T{...} 前导冲突，见 lexer
         * 注释）——此处识别 '.' 后紧跟 '!' 或 '?' 的组合。 */
        if (check_symbol(p, ".")) {
            uint32_t tb = p->pos;
            advance(p);
            skip_trivia(p);

            /* 解包：. 后紧跟 !（assert）/ ?（try） */
            if (check_symbol(p, "!") || check_symbol(p, "?")) {
                const token_t *op_tok = cur_token(p);
                advance(p);

                ast_node_t *node = ast_unwrap_new(p->arena, tb, p->pos);
                if (!node) return NULL;
                ((ast_unwrap_t *)node)->operand = lhs;
                ((ast_unwrap_t *)node)->op      = (token_t *)op_tok;
                lhs = node;
                continue;
            }

            /* 成员访问：.field */
            if (!check_kind(p, TOKEN_TYPE_IDENTIFIER)) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected field name after '.'");
            }
            strslice_t field = token_strslice(cur_token(p));
            advance(p);

            ast_node_t *node = ast_member_new(p->arena, tb, p->pos);
            ((ast_member_t *)node)->object = lhs;
            ((ast_member_t *)node)->field  = field;
            lhs = node;
            continue;
        }

        /* 下标 / 泛型索引：[expr, ...] */
        if (check_symbol(p, "[")) {
            uint32_t tb = p->pos;
            advance(p);
            skip_trivia(p);

            ast_node_t *indices = NULL, *indices_last = NULL;

            if (!check_symbol(p, "]")) {
                ast_node_t *idx = parse_expr(p);
                if (!idx || idx->kind == AST_ERROR) {
                    if (!idx) {
                        return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                             "expected expression in index");
                    }
                    return idx;
                }
                ast_append(&indices, &indices_last, NULL, idx);

                skip_trivia(p);
                while (check_symbol(p, ",")) {
                    advance(p);
                    skip_trivia(p);
                    idx = parse_expr(p);
                    if (!idx || idx->kind == AST_ERROR) {
                        if (!idx) {
                            return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                                 "expected expression after ','");
                        }
                        return idx;
                    }
                    ast_append(&indices, &indices_last, NULL, idx);
                    skip_trivia(p);
                }
            }

            if (!expect_symbol(p, "]")) {
                return ast_error_new(p->diag, p->tokens, p->arena, tb, p->pos,
                                     "expected ']' after index expression");
            }

            ast_node_t *node = ast_index_new(p->arena, tb, p->pos);
            ((ast_index_t *)node)->object       = lhs;
            ((ast_index_t *)node)->indices      = indices;
            ((ast_index_t *)node)->indices_last = indices_last;
            lhs = node;
            continue;
        }

        break;
    }
    return lhs;
}

/* ---- parse_expr_prec: Pratt 核心 ---- */

ast_node_t *parse_expr_prec(parser_t *p, int min_prec) {
    /* 1. 前缀：一元或原子 */
    ast_node_t *left = parse_unary(p);
    if (!left || left->kind == AST_ERROR) return left;

    /* 2. 中缀/后缀循环 */
    for (;;) {
        skip_trivia(p);

        /* 2a. 三元条件表达式：cond ? then : else（右结合，C 语义）
         *     优先级与赋值同级（最低），检查必须先于中缀查表（'?' 不在
         *     binding 表，否则 break 提前结束循环）。then 分支用
         *     parse_expr_prec(p, 0) 解析——':' 不是任何运算符，解析天然
         *     停在 ':'；else 分支同样用 parse_expr_prec(p, 0) 解析完整
         *     表达式，嵌套三元右结合自然成立（a ? b : c ? d : e →
         *     else = c ? d : e）。 */
        if (check_symbol(p, "?")) {
            if (TERNARY_LEFT_PREC < min_prec) break;

            uint32_t op_pos = p->pos;
            advance(p);
            skip_trivia(p);

            ast_node_t *then_branch = parse_expr_prec(p, 0);
            if (!then_branch || then_branch->kind == AST_ERROR) {
                if (!then_branch) {
                    return ast_error_new(p->diag, p->tokens, p->arena, op_pos, p->pos,
                                         "expected expression after '?' in ternary");
                }
                return then_branch;
            }
            skip_trivia(p);
            if (!expect_symbol(p, ":")) {
                return ast_error_new(p->diag, p->tokens, p->arena, op_pos, p->pos,
                                     "expected ':' in ternary expression");
            }
            skip_trivia(p);
            ast_node_t *else_branch = parse_expr_prec(p, 0);
            if (!else_branch || else_branch->kind == AST_ERROR) {
                if (!else_branch) {
                    return ast_error_new(p->diag, p->tokens, p->arena, op_pos, p->pos,
                                         "expected expression after ':' in ternary");
                }
                return else_branch;
            }

            ast_node_t *node = ast_ternary_new(p->arena, left->tok_begin, p->pos);
            ((ast_ternary_t *)node)->cond         = left;
            ((ast_ternary_t *)node)->then_branch  = then_branch;
            ((ast_ternary_t *)node)->else_branch  = else_branch;
            left = node;
            continue;
        }

        /* 2b. 赋值运算符：最低优先级，右结合
         *     左值必须是标识符表达式节点，_ = expr 也是 AST_ASSIGN(target=IDENT "_")
         *     discard 语义由 Sema 处理 */
        if (is_assign_op_token(cur_token(p))) {
            if (ASSIGN_LEFT_PREC < min_prec) break;

            const token_t *op_tok = cur_token(p);
            uint32_t op_pos = p->pos;

            /* 左值必须是标识符、下标（a[i] = v）或成员访问（p.x = v）。
             * 下标/泛型索引（AST_INDEX）与泛型实例化语法重叠，parser 不区分——
             * sema 层做最终校验（数组下标赋值 / 泛型占位诊断）。
             * 成员访问目标（AST_MEMBER）支持 p.x = v、p.x += v 等复合赋值，
             * 字段存在性/const 检查在 sema 层。 */
            if (left->kind != AST_IDENT && left->kind != AST_INDEX &&
                left->kind != AST_MEMBER) {
                return ast_error_new(p->diag, p->tokens, p->arena, left->tok_begin, p->pos,
                                     "invalid assignment target");
            }

            advance(p);
            skip_trivia(p);

            ast_node_t *value = parse_expr_prec(p, ASSIGN_LEFT_PREC);
            if (!value || value->kind == AST_ERROR) {
                if (!value) {
                    return ast_error_new(p->diag, p->tokens, p->arena, op_pos, p->pos,
                                         "expected expression after assignment operator");
                }
                return value;
            }

            ast_node_t *node = ast_assign_new(p->arena, left->tok_begin, p->pos);
            ((ast_assign_t *)node)->target = left;
            ((ast_assign_t *)node)->op     = op_tok;
            ((ast_assign_t *)node)->value  = value;
            left = node;
            continue;
        }

        /* 2c. 后缀绑定力最高(25)，贪婪消费（含枚举 variant 引用 ::） */
        if (check_symbol(p, "(") || check_symbol(p, ".") || check_symbol(p, "[") ||
            check_symbol(p, "::")) {
            if (POSTFIX_LEFT_PREC < min_prec) break;
            left = parse_postfix(p, left);
            if (left->kind == AST_ERROR) return left;
            continue;
        }

        /* 2d. 中缀运算符查表 */
        const token_t *op_tok = cur_token(p);
        int lp, rp;
        if (!infix_binding(op_tok, &lp, &rp)) break;
        if (lp < min_prec) break;

        uint32_t op_pos = p->pos;
        advance(p);
        skip_trivia(p);

        /* as 是普通中缀运算符（lp=21/rp=22，关键字运算符表）：
           `a as i32` = <expr1> as <expr2>，rhs 是类型表达式（普通表达式，
           const/volatile 前缀由 parse_unary 消费为 AST_CONST/AST_VOLATILE）。
           语义在消费层感知：sema shadow 求值 lhs、解析 rhs 类型；
           compiler 编译 lhs + rhs（类型值）→ BCODE_CAST。 */

        /* 递归解析右侧，传入右绑定力作为最小绑定力 */
        ast_node_t *rhs = parse_expr_prec(p, rp);
        if (!rhs || rhs->kind == AST_ERROR) {
            if (!rhs) {
                return ast_error_new(p->diag, p->tokens, p->arena, op_pos, p->pos,
                                     "expected expression after operator");
            }
            return rhs;
        }

        ast_node_t *node = ast_binary_new(p->arena, op_pos, p->pos);
        ((ast_binary_t *)node)->op  = op_tok;
        ((ast_binary_t *)node)->lhs = left;
        ((ast_binary_t *)node)->rhs = rhs;
        left = node;
    }

    return left;
}

/* ---- parse_expr: Pratt parser 公开入口 ---- */

ast_node_t *parse_expr(parser_t *p) {
    return parse_expr_prec(p, 0);
}
