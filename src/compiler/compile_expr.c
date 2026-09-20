#include "compiler/compiler.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_construct.h"
#include "parser/ast_enum_ref.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_nil.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/lexer.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_option.h"

/* ===========================================================================
 * 表达式节点
 *
 * 每个表达式压栈恰好一个值（或零值占位）；操作数栈净变化由 st_push 静态
 * 追踪。短路 &&/|| 走独立编译路径（JZ/JNZ + 常量兜底），结果恒在栈上。
 * =========================================================================== */

/* 类型槽位解析为真实 type_t（CONSTRUCT 类型位 / PUSH_OPT_NONE 的 option
 * 类型查询用）。AST_TYPE_REF → sema 登记表（__type_N）→ 内建 type_lookup
 * 兜底；AST_IDENT → type_lookup（编译期可解析的类型名）。sema 已保证类型
 * 槽位在常规路径为 AST_TYPE_REF（复合类型）或 AST_IDENT（内建别名）；
 * 解析失败返回 NULL（调用方已 c_error，fail-fast）。 */
const type_t *c_resolve_type(compiler_t *c, ast_node_t *type_expr) {
  if (!type_expr) return NULL;
  if (type_expr->kind == AST_TYPE_REF) {
    ast_type_ref_t *ref = (ast_type_ref_t *)type_expr;
    const sema_type_t *st = c_sema_type_find_name(c->sema_types, ref->name);
    if (st) return st->type;
    return type_lookup(c->vm, ref->name);
  }
  if (type_expr->kind == AST_IDENT) {
    ast_ident_t *id = (ast_ident_t *)type_expr;
    return type_lookup(c->vm, id->name);
  }
  return NULL;
}

/* 从类型槽位发 PUSH_OPT_NONE <id>：?T 构造器 nil 字段 / 数组 nil 元素 /
 * var 声明 init nil。option 类型一定登记在 sema_types（sema_type_register
 * 递归登记复合类型），取 st->id；未登记（内建兜底失败）报错。 */
void emit_push_opt_none(compiler_t *c, ast_node_t *type_expr) {
  const type_t *t = c_resolve_type(c, type_expr);
  if (!t) {
    c_error(c, type_expr, "unknown type in nil initializer");
    return;
  }
  if (t->kind != TYPE_KIND_OPTION) {
    c_error(c, type_expr, "compiler: nil initializer requires an optional type");
    return;
  }
  const sema_type_t *st = c_sema_type_find_ptr(c->sema_types, t);
  if (!st) {
    c_error(c, type_expr, "compiler: optional type not registered");
    return;
  }
  bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
  bcode_write_u32(c->bc, st->id);
  st_push(c, 1);
}

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
  case AST_UNWRAP: {
    /* optional 解包（a.! assert）：compile_expr(operand) 求值 ?T 值 →
       UNWRAP 弹 ?T 值 → ok 借用返回 value 字段（?T 退化为 T）/ none panic。
       栈深：operand 净 +1，UNWRAP 弹 1 压 1 净 0，表达式总净 +1。 */
    ast_unwrap_t *n = (ast_unwrap_t *)node;
    compile_expr(c, n->operand);
    bcode_write_op(c->bc, BCODE_UNWRAP);
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
  case AST_ENUM_REF: {
    /* 枚举 variant 引用 Color::Red（sema 已折叠 value 入节点）：
       PUSH_I* <value>（按底层宽度选立即数指令）→ LOAD_TYPE <enum_id> →
       MAKE_ENUM（弹 type + 整数值 → 按底层宽度截断构造 enum 值）。
       enum 类型 id：sema 登记（c_sema_type_find_name 查 type_expr 的
       AST_TYPE_REF 名字）。 */
    ast_enum_ref_t *n = (ast_enum_ref_t *)node;
    const sema_type_t *st = NULL;
    if (n->type_expr && n->type_expr->kind == AST_TYPE_REF) {
      st = c_sema_type_find_name(c->sema_types,
                                 ((ast_type_ref_t *)n->type_expr)->name);
    }
    if (!st || !st->type || st->type->kind != TYPE_KIND_ENUM) {
      c_error(c, node, "unknown enum type in enum reference");
      return;
    }
    const type_t *u = enum_type_underlying(st->type);
    if (!u) {
      c_error(c, node, "enum type missing underlying type");
      return;
    }
    switch (u->size) {
    case 1:
      if (u == c->vm->type_u8) {
        bcode_write_op(c->bc, BCODE_PUSH_U8);  bcode_write_u8(c->bc, (uint8_t)n->value);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_I8);  bcode_write_i8(c->bc, (int8_t)n->value);
      }
      break;
    case 2:
      if (u == c->vm->type_u16) {
        bcode_write_op(c->bc, BCODE_PUSH_U16); bcode_write_u16(c->bc, (uint16_t)n->value);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_I16); bcode_write_i16(c->bc, (int16_t)n->value);
      }
      break;
    case 4:
      if (u == c->vm->type_u32) {
        bcode_write_op(c->bc, BCODE_PUSH_U32); bcode_write_u32(c->bc, (uint32_t)n->value);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_I32); bcode_write_i32(c->bc, (int32_t)n->value);
      }
      break;
    default:
      if (u == c->vm->type_u64) {
        bcode_write_op(c->bc, BCODE_PUSH_U64); bcode_write_u64(c->bc, (uint64_t)n->value);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_I64); bcode_write_i64(c->bc, (int64_t)n->value);
      }
      break;
    }
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);
    bcode_write_u32(c->bc, st->id);
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_MAKE_ENUM);
    st_push(c, -1);
    break;
  }
  case AST_FUNC_REF: {
    /* sema 确认的函数引用（函数值）：
       - 有名字（源码引用）：PUSH name 沿作用域链查找——局部函数取定义点
         STORE 重定向的新实例（捕获独立），全局/内建函数取全局绑定基底。
         与 AST_FUNC_DEF 定义点 MAKE_FUNCTION 新实例语义对齐。
       - 无名字（comptime 折叠产物：匿名字面量引用）：LOAD_FUNCTION <fid>
         从 functions_by_id 加载基底。fid 段：内建函数 < FUNC_ID_PROGRAM_BASE
         （printf=0 合法）；程序函数 >= FUNC_ID_PROGRAM_BASE。运行期对未登记
         id 报 "unknown function id"。 */
    ast_func_ref_t *n = (ast_func_ref_t *)node;
    if (n->name.len > 0) {
      bcode_write_op(c->bc, BCODE_PUSH);
      bcode_write_str(c->bc, n->name);
    } else {
      bcode_write_op(c->bc, BCODE_LOAD_FUNCTION);
      bcode_write_u32(c->bc, n->fid);
    }
    st_push(c, 1);
    break;
  }
  case AST_FUNC_DEF: {
    /* 函数字面量（表达式内函数值）：MAKE_FUNCTION <fid> 从基底（hoist 区
       PUSH_FUNCTION + BIND_FUNC + [SET_FUNC_NAME] 构造）实例化独立新实例
       ——函数定义每次求值生成新对象（循环内字面量各持独立实例，捕获互不
       干扰）。有捕获的字面量随后发捕获绑定序列（keep=true：函数值留栈顶
       作表达式结果）——定义点把当前外层变量值 clone 进新实例 closure_scope。 */
    ast_func_def_t *fn = (ast_func_def_t *)node;
    compile_func_capture_bind(c, fn, true);
    break;
  }
  case AST_UNDEF:
    /* undefined：无初始值占位（var x:T = undefined 的 DEFINE 前置）。
       运行时存声明类型零值；sema 已保证未初始化变量不可读。 */
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
    st_push(c, 1);
    break;
  case AST_NIL:
    /* nil 不是 value：只允许四种 nil 判定、?T 初始化（STORE_NIL）、构造器
       字段（PUSH_OPT_NONE）与 fill 值包等特殊位置消费（sema 已校验）。
       普通表达式位置由 sema 拦截，此处不应到达。 */
    c_error(c, node, "compiler: nil is not an expression");
    return;
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
    /* nil 判定（docs m2-design §12.5）：x==nil / nil==x → PUSH x +
       OPT_IS_NONE（true=none）；x!=nil / nil!=x → PUSH x + OPT_IS_NONE
       + NOT。tag 比较专用指令，不进入 vtable eq 分派。sema 已保证另一
       侧是 ?T 变量（AST_IDENT）。 */
    if ((token_is(n->op, "==") || token_is(n->op, "!=")) &&
        (n->lhs->kind == AST_NIL || n->rhs->kind == AST_NIL)) {
      ast_node_t *other = n->lhs->kind == AST_NIL ? n->rhs : n->lhs;
      if (other->kind != AST_IDENT) {
        c_error(c, node, "nil can only be compared with an optional variable");
        return;
      }
      compile_expr(c, other);                 /* 栈: [x] */
      bcode_write_op(c->bc, BCODE_OPT_IS_NONE); /* 弹 x → bool(none)，栈: [bool] */
      if (token_is(n->op, "!=")) {
        bcode_write_op(c->bc, BCODE_NOT);     /* 取反：!= nil → !is_none */
      }
      st_push(c, 0);
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
       1. 类型位：compile_type_expr（[N]T / ?T → 声明-定义两步构造，
          LOAD_TYPE 留类型值栈顶；常规路径 sema 已替换为 AST_TYPE_REF）
       2. 各字段值按序压栈（栈: [type_value, v1..vN]；fill 展开为 N 份
          v；nil 字段 → PUSH_OPT_NONE <option id>）
       3. CONSTRUCT N：弹 N 个成员值 + 类型位 → 值（结果压栈）
       栈深净变化 -(N)：压 N+1，CONSTRUCT 弹 N+1 压 1。
       构造完全显式（docs m2-design §12.4）：总元素数 = Σfill counts +
       显式字段数，sema 已校验 == 数组长度（无 0 填充、无 AST_UNDEF
       自动补发）；fill 的 count 已被 sema 折叠为 AST_INT_LIT 立即数。 */
    ast_construct_t *n = (ast_construct_t *)node;
    compile_type_expr(c, n->type);              /* 栈: [type_value] */

    const type_t *t = c_resolve_type(c, n->type);
    if (!t) { c_error(c, n->type, "unknown construct type"); return; }

    if (t->kind == TYPE_KIND_OPTION) {
      /* ?T 构造器（二值单槽位）：字段数强制 == 1（sema 已校验）。
         nil → PUSH_OPT_NONE（none 态）；T 字段 → 压值，CONSTRUCT(1)
         的 option 分支做隐式提升（some 态）。 */
      size_t fcount = 0;
      for (ast_node_t *f = n->fields; f; f = f->next) {
        if (f->kind == AST_NIL) {
          emit_push_opt_none(c, n->type);
        } else {
          compile_expr(c, f);
        }
        fcount++;
      }
      bcode_write_op(c->bc, BCODE_CONSTRUCT);
      bcode_write_u32(c->bc, (uint32_t)fcount);
      st_push(c, -((int)fcount));
      break;
    }

    if (t->kind != TYPE_KIND_ARRAY) {
      c_error(c, node, "construct: unsupported type (only array and optional implemented)");
      return;
    }

    /* 数组构造：fill 展开 + nil 元素（元素须 ?T，sema 已校验）+ 普通字段 */
    const type_t *et = array_type_elem(t);
    size_t fcount = 0;
    for (ast_node_t *f = n->fields; f; f = f->next) {
      if (f->kind == AST_FILL) {
        /* <v,N>：count 已折叠为 AST_INT_LIT，N 份 v 依次压栈 */
        ast_fill_t *fl = (ast_fill_t *)f;
        uint64_t cnt = 0;
        if (fl->count && fl->count->kind == AST_INT_LIT) {
          cnt = ((ast_int_lit_t *)fl->count)->value;
        } else {
          c_error(c, fl->count ? fl->count : f,
                  "fill count must be a compile-time constant");
          return;
        }
        if (fl->value->kind == AST_NIL) {
          /* nil 填充：元素须 ?T（sema 已校验），发 N 份 PUSH_OPT_NONE */
          const sema_type_t *est = c_sema_type_find_ptr(c->sema_types, et);
          if (!est) { c_error(c, f, "compiler: optional element type not registered"); return; }
          for (uint64_t k = 0; k < cnt; k++) {
            bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
            bcode_write_u32(c->bc, est->id);
            st_push(c, 1);
          }
        } else {
          for (uint64_t k = 0; k < cnt; k++) {
            compile_expr(c, fl->value);         /* 栈: [type_value, v..] */
          }
        }
        fcount += (size_t)cnt;
      } else if (f->kind == AST_NIL) {
        /* nil 字段：元素须 ?T（sema 已校验）→ PUSH_OPT_NONE */
        const sema_type_t *est = c_sema_type_find_ptr(c->sema_types, et);
        if (!est) { c_error(c, f, "compiler: optional element type not registered"); return; }
        bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
        bcode_write_u32(c->bc, est->id);
        st_push(c, 1);
        fcount++;
      } else {
        compile_expr(c, f);                     /* 栈: [type_value, v1..vN] */
        fcount++;
      }
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
