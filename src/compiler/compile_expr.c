#include "compiler/compiler.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_construct.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/lexer.h"

/* ===========================================================================
 * 表达式节点
 *
 * 每个表达式压栈恰好一个值（或零值占位）；操作数栈净变化由 st_push 静态
 * 追踪。短路 &&/|| 走独立编译路径（JZ/JNZ + 常量兜底），结果恒在栈上。
 * =========================================================================== */

void compile_expr(compiler_t *c, ast_node_t *node) {
  if (!node || c->failed) return;
  switch (node->kind) {
  case AST_INT_LIT: {
    ast_int_lit_t *n = (ast_int_lit_t *)node;
    /* 默认 i32；带后缀 → 按后缀宽度发射对应 PUSH_* */
    if (strslice_is_empty(n->type)) {
      bcode_write_op(c->bc, BCODE_PUSH_I32);
      bcode_write_i32(c->bc, (int32_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("i8"))) {
      bcode_write_op(c->bc, BCODE_PUSH_I8);  bcode_write_i8(c->bc, (int8_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("i16"))) {
      bcode_write_op(c->bc, BCODE_PUSH_I16); bcode_write_i16(c->bc, (int16_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("i32"))) {
      bcode_write_op(c->bc, BCODE_PUSH_I32); bcode_write_i32(c->bc, (int32_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("i64"))) {
      bcode_write_op(c->bc, BCODE_PUSH_I64); bcode_write_i64(c->bc, (int64_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("u8"))) {
      bcode_write_op(c->bc, BCODE_PUSH_U8);  bcode_write_u8(c->bc, (uint8_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("u16"))) {
      bcode_write_op(c->bc, BCODE_PUSH_U16); bcode_write_u16(c->bc, (uint16_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("u32"))) {
      bcode_write_op(c->bc, BCODE_PUSH_U32); bcode_write_u32(c->bc, (uint32_t)n->value);
    } else if (strslice_eq(n->type, STRSLICE_LIT("u64"))) {
      bcode_write_op(c->bc, BCODE_PUSH_U64); bcode_write_u64(c->bc, (uint64_t)n->value);
    } else {
      c_error(c, node, "unsupported integer literal type '%.*s'",
              (int)n->type.len, n->type.ptr);
      return;
    }
    st_push(c, 1);
    break;
  }
  case AST_FLOAT_LIT: {
    ast_float_lit_t *n = (ast_float_lit_t *)node;
    if (strslice_eq(n->type, STRSLICE_LIT("f32"))) {
      bcode_write_op(c->bc, BCODE_PUSH_F32); bcode_write_f32(c->bc, (float)n->value);
    } else if (strslice_is_empty(n->type) || strslice_eq(n->type, STRSLICE_LIT("f64"))) {
      bcode_write_op(c->bc, BCODE_PUSH_F64); bcode_write_f64(c->bc, n->value);
    } else {
      c_error(c, node, "unsupported float literal type '%.*s'",
              (int)n->type.len, n->type.ptr);
      return;
    }
    st_push(c, 1);
    break;
  }
  case AST_BOOL_LIT: {
    ast_bool_lit_t *n = (ast_bool_lit_t *)node;
    bcode_write_op(c->bc, BCODE_PUSH_BOOL); bcode_write_bool(c->bc, n->value);
    st_push(c, 1);
    break;
  }
  case AST_CHAR_LIT: {
    ast_char_lit_t *n = (ast_char_lit_t *)node;
    bcode_write_op(c->bc, BCODE_PUSH_U8); bcode_write_u8(c->bc, (uint8_t)n->value);
    st_push(c, 1);
    break;
  }
  case AST_STRING_LIT: {
    ast_string_lit_t *n = (ast_string_lit_t *)node;
    bcode_write_op(c->bc, BCODE_PUSH_STR); bcode_write_str(c->bc, n->text);
    st_push(c, 1);
    break;
  }
  case AST_IDENT: {
    ast_ident_t *n = (ast_ident_t *)node;
    bcode_write_op(c->bc, BCODE_PUSH); bcode_write_str(c->bc, n->name);
    st_push(c, 1);
    break;
  }
  case AST_TYPE_REF: {
    /* sema 登记的具名类型引用：LOAD_TYPE <id> 查表压栈（与 compile_type_expr
       一致；表达式位置出现时同样可用，类型即表达式）。
       内建类型引用（名字 = 规范名，不登记）→ type_lookup 兜底。 */
    ast_type_ref_t *n = (ast_type_ref_t *)node;
    const sema_type_t *st = c_sema_type_find_name(c->sema_types, n->name);
    if (!st) {
      const type_t *bt = type_lookup(c->vm, n->name);
      if (!bt) {
        c_error(c, node, "unknown type reference '%.*s'",
                (int)n->name.len, n->name.ptr);
        return;
      }
      bcode_write_op(c->bc, BCODE_LOAD_TYPE);
      bcode_write_u32(c->bc, bt->id);
      st_push(c, 1);
      break;
    }
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);
    bcode_write_u32(c->bc, st->id);
    st_push(c, 1);
    break;
  }
  case AST_FUNC_REF: {
    /* sema 确认的函数引用（函数值）：查函数名→fid 映射（compile_compile
       开头构建：内建 + 程序函数按声明序）→ LOAD_FUNCTION <id> 运行期从
       functions_by_id 查表压真实函数值。未命中（如 comptime func）是
       sema 应已拦截的错误。 */
    ast_func_ref_t *n = (ast_func_ref_t *)node;
    char nb[256];
    size_t nlen = n->name.len < sizeof(nb) - 1 ? n->name.len : sizeof(nb) - 1;
    memcpy(nb, n->name.ptr, nlen);
    nb[nlen] = '\0';
    void *fidv = c->func_ids ? strmap_get(c->func_ids, nb) : NULL;
    if (!fidv) {
      c_error(c, node, "unknown function '%.*s'", (int)n->name.len, n->name.ptr);
      return;
    }
    /* id+1 编码（见 compile.c func_ids 构建注释），还原真实 id */
    uint32_t fid = (uint32_t)(uintptr_t)fidv - 1u;
    bcode_write_op(c->bc, BCODE_LOAD_FUNCTION);
    bcode_write_u32(c->bc, fid);
    st_push(c, 1);
    break;
  }
  case AST_UNDEF:
    /* undefined：无初始值占位（var x:T = undefined 的 DEFINE 前置）。
       运行时存声明类型零值；sema 已保证未初始化变量不可读。 */
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
    st_push(c, 1);
    break;
  case AST_BINARY: {
    ast_binary_t *n = (ast_binary_t *)node;
    if (token_is(n->op, "&&") || token_is(n->op, "||")) {
      /* 短路编译（结果恒在栈上）：
         a && b  →  lhs; JZ L_false; rhs; JMP L_end; L_false: PUSH_BOOL false; L_end:
         a || b  →  lhs; JNZ L_true;  rhs; JMP L_end; L_true:  PUSH_BOOL true;  L_end: */
      bool is_and = token_is(n->op, "&&");
      compile_expr(c, n->lhs);            /* 栈: [lhs] */
      compile_label_t short_out, short_skip;
      label_init(&short_out);
      label_init(&short_skip);
      bcode_write_op(c->bc, is_and ? BCODE_JZ : BCODE_JNZ);
      emit_jump(c, &short_out);           /* 弹 lhs，短路跳 */
      compile_expr(c, n->rhs);            /* 栈: [rhs] */
      bcode_write_op(c->bc, BCODE_JMP);
      emit_jump(c, &short_skip);
      label_here(c, &short_out);
      bcode_write_op(c->bc, BCODE_PUSH_BOOL);
      bcode_write_bool(c->bc, is_and ? false : true);  /* 栈: [短路常量] */
      label_here(c, &short_skip);
      /* 结果保留在栈上，栈深不变（压 1 净 +1 与普通二元一致） */
      st_push(c, 1);
      break;
    }
    /* as：显式类型转换。lhs 普通表达式求值，rhs 是类型表达式
       （compile_type_expr：BCODE_PUSH 查当前作用域链 type value），
       BCODE_CAST 弹 type + value → 结果。 */
    if (token_is(n->op, "as")) {
      compile_expr(c, n->lhs);              /* 栈: [value] */
      compile_type_expr(c, n->rhs);         /* 栈: [value, type] */
      bcode_write_op(c->bc, BCODE_CAST);    /* 弹 type + value → 结果 */
      st_push(c, -1);
      break;
    }
    /* 常规二元：lhs → rhs → op */
    compile_expr(c, n->lhs);
    compile_expr(c, n->rhs);
    if (token_is(n->op, "+"))  bcode_write_op(c->bc, BCODE_ADD);
    else if (token_is(n->op, "-"))  bcode_write_op(c->bc, BCODE_SUB);
    else if (token_is(n->op, "*"))  bcode_write_op(c->bc, BCODE_MUL);
    else if (token_is(n->op, "/"))  bcode_write_op(c->bc, BCODE_DIV);
    else if (token_is(n->op, "%"))  bcode_write_op(c->bc, BCODE_MOD);
    else if (token_is(n->op, "==")) bcode_write_op(c->bc, BCODE_EQ);
    else if (token_is(n->op, "!=")) bcode_write_op(c->bc, BCODE_NE);
    else if (token_is(n->op, "<"))  bcode_write_op(c->bc, BCODE_LT);
    else if (token_is(n->op, "<=")) bcode_write_op(c->bc, BCODE_LE);
    else if (token_is(n->op, ">"))  bcode_write_op(c->bc, BCODE_GT);
    else if (token_is(n->op, ">=")) bcode_write_op(c->bc, BCODE_GE);
    else if (token_is(n->op, "&"))  bcode_write_op(c->bc, BCODE_AND);
    else if (token_is(n->op, "|"))  bcode_write_op(c->bc, BCODE_OR);
    else if (token_is(n->op, "^"))  bcode_write_op(c->bc, BCODE_BXOR);
    else if (token_is(n->op, "<<")) bcode_write_op(c->bc, BCODE_SHL);
    else if (token_is(n->op, ">>")) bcode_write_op(c->bc, BCODE_SHR);
    else {
      c_error(c, node, "unsupported binary operator");
      return;
    }
    st_push(c, -1); /* 两弹一压 */
    break;
  }
  case AST_UNARY: {
    ast_unary_t *n = (ast_unary_t *)node;
    compile_expr(c, n->operand);
    if (token_is(n->op, "-"))       bcode_write_op(c->bc, BCODE_NEG);
    else if (token_is(n->op, "!"))  bcode_write_op(c->bc, BCODE_NOT);
    else if (token_is(n->op, "~"))  bcode_write_op(c->bc, BCODE_BNOT);
    else {
      c_error(c, node, "unsupported unary operator");
      return;
    }
    break;
  }
  case AST_CALL: {
    ast_call_t *n = (ast_call_t *)node;
    compile_expr(c, n->callee);     /* 栈: [callee] */
    size_t argc = 0;
    for (ast_node_t *a = n->args; a; a = a->next) {
      compile_expr(c, a);           /* 栈: [callee, arg1..argN] */
      argc++;
    }
    bcode_write_op(c->bc, BCODE_CALL);
    bcode_write_u32(c->bc, (uint32_t)argc);
    st_push(c, -((int)argc));       /* callee+args 弹出，结果压入 */
    break;
  }
  case AST_CONSTRUCT: {
    /* 类型字面量构造 .<type>{ fields }：
       1. 类型位：compile_type_expr（[N]T → 声明-定义两步构造，LOAD_TYPE 留类型值栈顶）
       2. 各字段值按序压栈（栈: [type_value, v1..vN]）
       3. CONSTRUCT N：弹 N 个成员值 + 类型位 → 数组值（结果压栈）
       栈深净变化 -(N)：压 N+1，CONSTRUCT 弹 N+1 压 1。 */
    ast_construct_t *n = (ast_construct_t *)node;
    compile_type_expr(c, n->type);              /* 栈: [type_value] */
    size_t fcount = 0;
    for (ast_node_t *f = n->fields; f; f = f->next) {
      compile_expr(c, f);                       /* 栈: [type_value, v1..vN] */
      fcount++;
    }
    bcode_write_op(c->bc, BCODE_CONSTRUCT);
    bcode_write_u32(c->bc, (uint32_t)fcount);
    st_push(c, -((int)fcount));                 /* CONSTRUCT 弹 N+1 压 1 */
    break;
  }
  case AST_MEMBER:
    c_error(c, node, "member access is not supported in M1");
    return;
  case AST_INDEX: {
    /* 右值下标：object → index → INDEX_GET（弹 self+index → 元素副本）。
       a[i][j] 多维是 parse 链式嵌套（((a[i])[j])），自然编译为两次
       INDEX_GET 逐维下降。sema 已保证单索引（多索引 = 泛型预留诊断）。 */
    ast_index_t *n = (ast_index_t *)node;
    compile_expr(c, n->object);       /* 栈: [self] */
    compile_expr(c, n->indices);      /* 栈: [self, index] */
    bcode_write_op(c->bc, BCODE_INDEX_GET);
    st_push(c, -1);                   /* 弹 2 压 1 */
    break;
  }
  case AST_TERNARY: {
    /* 三元条件表达式（结果恒在栈上）：
       cond; JZ L_else; then; JMP L_end; L_else: else; L_end:
       仿短路 &&/|| 的 label 模式：JZ 弹 cond，false → else 分支；then
       后 JMP 跳过 else；两分支汇合于 L_end。结果类型由 sema 保证一致，
       运行时选中分支的值留在栈顶（净 +1，与普通表达式一致）。 */
    ast_ternary_t *n = (ast_ternary_t *)node;
    compile_expr(c, n->cond);             /* 栈: [cond] */
    compile_label_t l_else, l_end;
    label_init(&l_else);
    label_init(&l_end);
    bcode_write_op(c->bc, BCODE_JZ);
    emit_jump(c, &l_else);                /* 弹 cond，false → else */
    compile_expr(c, n->then_branch);      /* 栈: [then] */
    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, &l_end);
    label_here(c, &l_else);
    compile_expr(c, n->else_branch);      /* 栈: [else] */
    label_here(c, &l_end);
    /* 结果保留在栈上，净 +1（与普通表达式一致） */
    st_push(c, 1);
    break;
  }
  default:
    c_error(c, node, "unsupported expression node '%s'", ast_kind_name(node->kind));
    return;
  }
}
