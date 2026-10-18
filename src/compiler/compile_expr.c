#include <stdio.h>   /* snprintf：M6 FFI 限定名拼接 */
#include "compiler/compiler.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_free.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_member.h"
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
#include "vm/type_struct.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_ptr.h"
#include "parser/ast_new.h"
#include "parser/ast_deref.h"
#include "parser/ast_addr.h"
#include "parser/ast_move.h"
#include "parser/ast_slice.h"
#include "parser/ast_make.h"
#include "vm/type_slice.h"
#include "vm/module.h"
#include "sema/symbol.h"
#include "core/strslice.h"

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
  case AST_SCOPE_REF: {
    ast_scope_ref_t *n = (ast_scope_ref_t *)node;

    /* M5 模块成员访问：type_expr 是 AST_IDENT（未被 sema 折叠为 AST_TYPE_REF）
       → 模块命名空间访问。sema 已校验成员存在。compiler 发：
       IMPORT <path> → GET_MEMBER <name>。
       模块路径：sema 在 pass0_imports 解析时已知，但 AST_IMPORT 节点的
       path 保留在 AST 中。这里通过 sema 作用域查找模块符号获取 module_t*，
       从中取 canonical 路径。 */
    if (n->type_expr && n->type_expr->kind == AST_IDENT) {
      /* 检查是否是模块符号（而非枚举类型——枚举类型 sema 已折叠为
         AST_TYPE_REF，AST_IDENT 只可能是模块别名） */
      sema_symbol_t *sym = sema_lookup(c->global_scope,
                                       ((ast_ident_t *)n->type_expr)->name);
      if (sym && sym->kind == SEMA_SYM_MODULE && sym->module) {
        module_t *mod = sym->module;
        /* IMPORT <源码路径>：发射**源码里写的路径**（`"./math"`），而非编译期
           规范化后的宿主路径——产物因此与宿主目录布局解耦；运行期由 op_import
           以"当前模块的规范路径"为基准重新规范化（见 module/module_path.h）。
           import_path 为 NULL 只会出现在入口模块，而入口不会被 import。 */
        bcode_write_op(c->bc, BCODE_IMPORT);
        bcode_write_str(c->bc, strslice_from_cstr(
                                   mod->import_path ? mod->import_path
                                                    : mod->canonical));
        st_push(c, 1);
        /* GET_MEMBER <variant name> */
        bcode_write_op(c->bc, BCODE_GET_MEMBER);
        bcode_write_str(c->bc, n->variant);
        st_push(c, 0); /* 弹模块值，压成员值：净 0 */
        break;
      }
    }

    /* M6 FFI：`<库>::<数据符号>`（extern var 的读取）。sema 已置 is_ffi_var +
       ffi_var_kind（契约见 ast_scope_ref.h），这里发 EXTERN_LOAD：运行期 dlsym
       得变量地址，按类型码读 size 字节并压对应内建类型的值。**不**编译成
       枚举 variant 路径（那样会去查类型表并报 unknown enum type）。 */
    if (n->is_ffi_var) {
      if (n->type_expr && n->type_expr->kind == AST_IDENT) {
        ast_ident_t *lib_id = (ast_ident_t *)n->type_expr;
        char qual[512];
        int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                          (int)lib_id->name.len, lib_id->name.ptr,
                          (int)n->variant.len, n->variant.ptr);
        if (qn > 0 && (size_t)qn < sizeof(qual)) {
          bool typed = (n->ffi_var_kind == BCODE_FFI_DATA_TYPED);
          if (typed) compile_type_expr(c, n->ffi_var_type_expr);
          bcode_write_op(c->bc, BCODE_EXTERN_LOAD);
          bcode_write_str(c->bc, strslice_from_bytes(qual, (size_t)qn));
          bcode_write_u32(c->bc, n->ffi_var_kind);
          /* TYPED：类型值被指令消费（压 1 → 指令弹 1 压 1 = 净 0） */
          st_push(c, typed ? 0 : 1);
          break;
        }
      }
      c_error(c, node, "malformed extern var reference");
      return;
    }

    /* struct 静态属性 `T::x`（sema 置 is_struct_attr + struct_attr_type_id）：
       发 LOAD_ATTR <type_id> <name> 从 struct_type_t::attributes 取值。
       sema 已校验成员存在性（含嵌入链继承）。 */
    if (n->is_struct_attr) {
      bcode_write_op(c->bc, BCODE_LOAD_ATTR);
      bcode_write_u32(c->bc, n->struct_attr_type_id);
      bcode_write_str(c->bc, n->variant);
      st_push(c, 1);
      break;
    }
    /* 枚举 variant 引用 Color::Red（sema 已折叠 value 入节点）：
       PUSH_I* <value>（按底层宽度选立即数指令）→ LOAD_TYPE <enum_id> →
       MAKE_ENUM（弹 type + 整数值 → 按底层宽度截断构造 enum 值）。
       enum 类型 id：sema 登记（c_sema_type_find_name 查 type_expr 的
       AST_TYPE_REF 名字）。 */
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
    /* is：tag union tag 判定（<expr> is <member>，docs m2-design §tag
       union）。lhs 普通表达式求值（union 对象）；rhs 已被 sema 折叠为
       AST_INT_LIT（member 的 tag 整数值，member 名 → tag 在 sema 完成，
       compiler 零感知 union 类型）→ IS_TAG <tag> 弹 union 值 → 压 bool。 */
    if (token_is(n->op, "is")) {
      compile_expr(c, n->lhs);              /* 栈: [value] */
      if (!n->rhs || n->rhs->kind != AST_INT_LIT) {
        c_error(c, node, "is: right operand must be a member tag value");
        return;
      }
      uint64_t tag = ((ast_int_lit_t *)n->rhs)->value;
      if (tag > 0xFFFFFFFFull) {
        c_error(c, n->rhs, "is: member tag value %llu out of range",
                (unsigned long long)tag);
        return;
      }
      bcode_write_op(c->bc, BCODE_IS_TAG);
      bcode_write_u32(c->bc, (uint32_t)tag); /* 弹 value → 压 bool */
      st_push(c, 0);
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

    /* `data(s)`（C ABI 适配，sema 置 is_data_call）：从序列取首元素指针。
       编译成 `compile_expr(arg)` + `DATA` 指令——**不是函数调用**：内置 cfunc
       的实参会被 clone 进被调作用域（保证内置改不到调用方的值），函数形态只能
       拿到副本的地址（slice 缓冲区副本 / 数组副本块），调用一结束就悬垂。
       指令形态就地读序列的存储身份（与 `x.&` 同一道理）。 */
    if (n->is_data_call) {
      if (!n->args || n->args->next) {
        c_error(c, node, "data: expects exactly 1 argument");
        return;
      }
      compile_expr(c, n->args);           /* 栈: [seq] */
      bcode_write_op(c->bc, BCODE_DATA);  /* 弹序列 → 压 ref *T */
      st_push(c, 0);                      /* 弹 1 压 1 */
      break;
    }

    /* M6 FFI：extern cfunc 调用（sema 在 AST_CALL 上置 is_ffi_call + ffi_ret_kind，
       契约见 include/parser/ast_call.h 与 docs/m6-design.md §14.7）。
       走这条路径时：**不编译 callee**（它没有 fid、没有运行时函数对象），只按序编译
       实参，然后发 `EXTERN_CALL <STR "lib::name"> <U32 argc> <U32 ret_kind>`，
       运行期由 op_extern_call 从当前模块的库表取句柄并调用。
       注意：标志在 sema 接线（步骤 ②）之前恒为 false，故本分支是惰性的。 */
    if (n->is_ffi_call && n->callee && n->callee->kind == AST_SCOPE_REF) {
      ast_scope_ref_t *er = (ast_scope_ref_t *)n->callee;
      if (er->type_expr && er->type_expr->kind == AST_IDENT) {
        ast_ident_t *lib_id = (ast_ident_t *)er->type_expr;
        char qual[512];
        int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                          (int)lib_id->name.len, lib_id->name.ptr,
                          (int)er->variant.len, er->variant.ptr);
        if (qn <= 0 || (size_t)qn >= sizeof(qual)) {
          c_error(c, node, "extern call: qualified name too long");
          return;
        }
        size_t ffi_argc = 0;
        /* 返回类型是程序自定义类型（struct/cunion/enum/带类型的指针）：先压**类型值**
           （与 CONSTRUCT 同一机制），运行期按它读返回内存并造值。 */
        bool typed_ret = (BCODE_FFI_RET_UNPACK_KIND(n->ffi_ret_kind) ==
                          BCODE_FFI_RET_TYPED);
        if (typed_ret) compile_type_expr(c, n->ffi_ret_type_expr);
        for (ast_node_t *a = n->args; a; a = a->next) {
          compile_expr(c, a); /* 栈: [type?][arg1..argN]（无 callee） */
          ffi_argc++;
        }
        bcode_write_op(c->bc, BCODE_EXTERN_CALL);
        bcode_write_str(c->bc, strslice_from_bytes(qual, (size_t)qn));
        bcode_write_u32(c->bc, (uint32_t)ffi_argc);
        bcode_write_u32(c->bc, (uint32_t)n->ffi_ret_kind);
        /* 实参弹出，结果压入；TYPED 时类型值也被指令消费掉（净变化相同） */
        st_push(c, -((int)ffi_argc));
        break;
      }
      c_error(c, node, "extern call: callee must be `<lib>::<name>`");
      return;
    }

    /* struct 方法调用（sema 置 is_member_call + member_type_id）：
       不编译 callee 为函数值——编译 receiver 压栈 + 实参压栈 → MEMBER_CALL。
       实例方法 `a.b(args)`：receiver = a（AST_MEMBER 的 object）。
       静态方法 `T::f(args)`：receiver = T 的 type value（AST_SCOPE_REF 的
       type_expr，编译为 LOAD_TYPE <id> 压 type value）。 */
    if (n->is_member_call) {
      strslice_t method_name = {0};
      if (n->callee->kind == AST_MEMBER) {
        ast_member_t *mem = (ast_member_t *)n->callee;
        method_name = mem->field;
        /* 实例方法：编译 object 作为 receiver 压栈 */
        compile_expr(c, mem->object);  /* 栈: [receiver] */
      } else if (n->callee->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *er = (ast_scope_ref_t *)n->callee;
        method_name = er->variant;
        /* 静态方法：编译 type_expr 作为 receiver（type value）压栈。
           跨模块 `mod::Type::f()`：type_expr 是嵌套 AST_SCOPE_REF，走 compile_expr
           的 IMPORT→GET_MEMBER 路径获取模块导出的 type value。
           本模块 `Type::f()`：type_expr 是 AST_IDENT/AST_TYPE_REF，走 compile_type_expr
           的 LOAD_TYPE 路径。 */
        if (er->type_expr && er->type_expr->kind == AST_SCOPE_REF)
          compile_expr(c, er->type_expr);     /* 栈: [type_value] */
        else
          compile_type_expr(c, er->type_expr);  /* 栈: [type_value] */
      } else {
        c_error(c, node, "member call: invalid callee kind");
        return;
      }
      st_push(c, 1);  /* receiver */
      size_t argc = 0;
      for (ast_node_t *a = n->args; a; a = a->next) {
        compile_expr(c, a);           /* 栈: [receiver, arg1..argN] */
        argc++;
      }
      bcode_write_op(c->bc, BCODE_MEMBER_CALL);
      bcode_write_str(c->bc, method_name);
      bcode_write_u32(c->bc, (uint32_t)argc);
      /* 弹 receiver + argc 个实参，压 1 个结果 = 净 -argc */
      st_push(c, -((int)argc));
      break;
    }

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

    if (t->kind == TYPE_KIND_STRUCT) {
      /* struct 构造（具名/匿名，sema 已折叠匿名 type 为 AST_TYPE_REF）：
         字段数已由 sema 校验 == 类型字段数。按字段序压值：
         - 具名字段（AST_CONSTRUCT_FIELD）取 value 编译（sema 已按名匹配
           到正确字段，运行期 CONSTRUCT 按声明序布值——具名字段编译序即
           AST 链序，与类型字段表序一致）
         - nil 字段 → 字段须 ?T（sema 已校验）→ PUSH_OPT_NONE <field id>
         CONSTRUCT N 弹 N+1 压 1。 */
      const struct_type_t *st = (const struct_type_t *)t;
      size_t fcount = 0;
      for (ast_node_t *f = n->fields; f; f = f->next) {
        ast_node_t *value = f;
        if (f->kind == AST_CONSTRUCT_FIELD)
          value = ((ast_construct_field_t *)f)->value;
        if (value->kind == AST_NIL) {
          /* nil 字段：须 ?T（sema 已校验），发 PUSH_OPT_NONE <field type id>。
             id 用字段类型（option 类型须已登记；sema 已 resolve 字段类型） */
          const sema_type_t *fst =
              c_sema_type_find_ptr(c->sema_types, st->fields[fcount].type);
          if (!fst) {
            c_error(c, value, "compiler: optional field type not registered");
            return;
          }
          bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
          bcode_write_u32(c->bc, fst->id);
          st_push(c, 1);
        } else {
          compile_expr(c, value);
        }
        fcount++;
      }
      bcode_write_op(c->bc, BCODE_CONSTRUCT);
      bcode_write_u32(c->bc, (uint32_t)fcount);
      st_push(c, -((int)fcount));
      break;
    }

    if (t->kind == TYPE_KIND_TUPLE) {
      /* tuple 构造（具名/匿名，sema 已折叠匿名 type 为 AST_TYPE_REF）：
         元素数已由 sema 校验 == 类型元素数。元组元素匿名 → 字段链只能是
         值表达式（sema 已校验无具名/值包字段），按元素序压值：
         - nil 元素 → 元素须 ?T（sema 已校验）→ PUSH_OPT_NONE <elem id>
         CONSTRUCT N 弹 N+1 压 1。 */
      const tuple_type_t *tt = (const tuple_type_t *)t;
      size_t fcount = 0;
      for (ast_node_t *f = n->fields; f; f = f->next) {
        if (f->kind == AST_NIL) {
          const sema_type_t *est =
              c_sema_type_find_ptr(c->sema_types, tt->elems[fcount].type);
          if (!est) {
            c_error(c, f, "compiler: optional element type not registered");
            return;
          }
          bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
          bcode_write_u32(c->bc, est->id);
          st_push(c, 1);
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

    if (t->kind == TYPE_KIND_UNION) {
      /* union 构造（.Value{.i = 42}，struct 同构单字段，显式 tag 哨兵协议）：
         sema 已校验恰好 1 个具名字段 + 按名定位 member 并写回 member_tag
         （member 下标）。编译序列：
           1. type_value（compile_type_expr 已压）
           2. tag 哨兵：member_tag 是 member 下标（0..member_count-1），
              < 128 → PUSH_I8（op_construct union 分支按 size 1 读 int8）；
              < 32768 → PUSH_I16（size 2 读 int16）；否则 PUSH_I32
           3. payload 值：compile 具名字段的 value（nil 字段 → 须 ?T，
              PUSH_OPT_NONE <member payload type id>）
           4. CONSTRUCT 2：栈 [type_value, tag_value, payload_value]
              弹 3 压 1（union 分支按 tag 值定位 member → implicit_cast →
              blit 到 payload_offset）
         栈深净变化 -1：压 3，CONSTRUCT 弹 3 压 1。 */
      ast_node_t *f = n->fields; /* 恰好 1 个（sema 已校验） */
      uint32_t tag = n->member_tag;
      if (tag < 128) {
        bcode_write_op(c->bc, BCODE_PUSH_I8);
        bcode_write_i8(c->bc, (int8_t)tag);
      } else if (tag < 32768) {
        bcode_write_op(c->bc, BCODE_PUSH_I16);
        bcode_write_i16(c->bc, (int16_t)tag);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_I32);
        bcode_write_i32(c->bc, (int32_t)tag);
      }
      st_push(c, 1);
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((ast_construct_field_t *)f)->value;
      if (value->kind == AST_NIL) {
        /* nil payload：member 须 ?T（sema 已校验），发 PUSH_OPT_NONE */
        const union_type_t *ut = (const union_type_t *)t;
        const union_member_t *m = &ut->members[tag];
        const sema_type_t *mst =
            c_sema_type_find_ptr(c->sema_types, m->payload_type);
        if (!mst) {
          c_error(c, value, "compiler: optional union member type not registered");
          return;
        }
        bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
        bcode_write_u32(c->bc, mst->id);
        st_push(c, 1);
      } else {
        compile_expr(c, value);
      }
      bcode_write_op(c->bc, BCODE_CONSTRUCT);
      bcode_write_u32(c->bc, 2);
      st_push(c, -3);
      break;
    }

    if (t->kind == TYPE_KIND_CUNION) {
      /* cunion 构造（.C{.i = 42}，struct 同构单字段，member 下标哨兵协议）：
         sema 已校验恰好 1 个具名字段 + 按名定位 member 并写回 member_tag
         （member 下标）。与 tag union 同款协议（区别：cunion 无 tag——运行期
         op_construct 不把下标写进 data，blit 到 offset 0 即联合体本体）：
           1. type_value（compile_type_expr 已压）
           2. member 下标哨兵：member_tag < 128 → PUSH_I8；< 32768 →
              PUSH_I16；否则 PUSH_I32（op_construct cunion 分支按宽度读整数）
           3. payload 值：compile 具名字段的 value（nil 字段 → 须 ?T，
              PUSH_OPT_NONE <member type id>）
           4. CONSTRUCT 2：栈 [type_value, member_idx, payload_value]
              弹 3 压 1（按 member 下标定位 member 类型 → implicit_cast →
              memcpy 到 offset 0）
         栈深净变化 -1：压 3，CONSTRUCT 弹 3 压 1。 */
      ast_node_t *f = n->fields; /* 恰好 1 个（sema 已校验） */
      uint32_t tag = n->member_tag;
      if (tag < 128) {
        bcode_write_op(c->bc, BCODE_PUSH_I8);
        bcode_write_i8(c->bc, (int8_t)tag);
      } else if (tag < 32768) {
        bcode_write_op(c->bc, BCODE_PUSH_I16);
        bcode_write_i16(c->bc, (int16_t)tag);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_I32);
        bcode_write_i32(c->bc, (int32_t)tag);
      }
      st_push(c, 1);
      ast_node_t *value = f;
      if (f->kind == AST_CONSTRUCT_FIELD)
        value = ((ast_construct_field_t *)f)->value;
      if (value->kind == AST_NIL) {
        /* nil payload：member 须 ?T（sema 已校验），发 PUSH_OPT_NONE */
        const cunion_type_t *ct = (const cunion_type_t *)t;
        const cunion_member_t *m = &ct->members[tag];
        const sema_type_t *mst =
            c_sema_type_find_ptr(c->sema_types, m->type);
        if (!mst) {
          c_error(c, value, "compiler: optional cunion member type not registered");
          return;
        }
        bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
        bcode_write_u32(c->bc, mst->id);
        st_push(c, 1);
      } else {
        compile_expr(c, value);
      }
      bcode_write_op(c->bc, BCODE_CONSTRUCT);
      bcode_write_u32(c->bc, 2);
      st_push(c, -3);
      break;
    }

    if (t->kind != TYPE_KIND_ARRAY) {
      c_error(c, node, "construct: unsupported type (only array, optional, struct, tuple, union and cunion implemented)");
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
  case AST_MEMBER: {
    /* 字段读取 p.field：object → FIELD_GET <name>（弹 self → 借用引用）。
       字段名进 strtable（编译期常量），运行期按名查偏移。嵌套 p.a.b：
       object 是 AST_MEMBER → 递归 FIELD_GET，借用引用偏移链正确。 */
    ast_member_t *n = (ast_member_t *)node;
    compile_expr(c, n->object);       /* 栈: [self] */
    bcode_write_op(c->bc, BCODE_FIELD_GET);
    bcode_write_str(c->bc, n->field);
    st_push(c, 0);                    /* 弹 1 压 1，净 0 */
    break;
  }
  case AST_NEW: {
    /* new T{...}（m3-design §8.1）：.T{...} 的堆分配形态 → fatal *T
       （§3.3 统一接管协议：new/clone/move 都产将亡值，由 DEFINE/赋值
       implicit_cast 接管为 own）。与 AST_CONSTRUCT 同构，但类型位压栈
       时机不同（统一 NEW 协议 [member, type_value]，type_value 在顶）：
       - 标量/option：恰好 1 字段（标量 new i32{123}；option new ?T{v}）
         ——先编译成员值，后补压类型位 → [member, type_value]
       - struct/tuple：N 字段（sema 已重排为声明序）→ 先压类型位走
         CONSTRUCT N 得 T 值 → 再补压类型位 → [T值, type_value]
       - array：N 元素（fill 展开 + nil）→ 同 struct 路径
       - union/cunion：恰好 1 个具名字段 → 与 AST_CONSTRUCT 同款 tag/member
         哨兵协议（sema 已把 member 下标写回 `n->member_tag`）：
         [type_value, tag, payload] → CONSTRUCT 2 → 补压类型位 → NEW
       最后 NEW 指令弹 2 压 1（成员值 + 类型位 → fatal *T 指针）。 */
    ast_new_t *n = (ast_new_t *)node;
    /* 注意：类型位不在开头压——struct/tuple/array 走 CONSTRUCT（消费类型
       位）后补压；标量分支在字段编译后压。NEW 统一协议 [member, type_value]
       （type_value 在顶，op_new 先弹类型位再弹成员值）。 */
    const type_t *t = c_resolve_type(c, n->type);
    if (!t) { c_error(c, n->type, "unknown construct type"); return; }

    if (t->kind == TYPE_KIND_STRUCT || t->kind == TYPE_KIND_TUPLE) {
      /* struct/tuple：先压类型位（CONSTRUCT 协议 [type_value, v1..vN]），
         编译全部字段 → CONSTRUCT N（得 T 值）→ 补压类型位 → NEW。
         nil 字段 → 字段须 ?T（sema 已校验）→ PUSH_OPT_NONE <field id>。 */
      compile_type_expr(c, n->type);      /* 栈: [type_value] */
      size_t fcount = 0;
      for (ast_node_t *f = n->fields; f; f = f->next) {
        ast_node_t *value = f;
        if (f->kind == AST_CONSTRUCT_FIELD)
          value = ((ast_construct_field_t *)f)->value;
        if (value->kind == AST_NIL) {
          const type_t *ft = (t->kind == TYPE_KIND_STRUCT)
                                 ? ((const struct_type_t *)t)->fields[fcount].type
                                 : ((const tuple_type_t *)t)->elems[fcount].type;
          const sema_type_t *fst =
              c_sema_type_find_ptr(c->sema_types, ft);
          if (!fst) {
            c_error(c, value, "compiler: optional field type not registered");
            return;
          }
          bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
          bcode_write_u32(c->bc, fst->id);
          st_push(c, 1);
        } else {
          compile_expr(c, value);
        }
        fcount++;
      }
      bcode_write_op(c->bc, BCODE_CONSTRUCT);
      bcode_write_u32(c->bc, (uint32_t)fcount);
      st_push(c, -((int)fcount));
      /* CONSTRUCT 已消费类型位（弹 N+1 压 1 = T 值）；NEW 协议需要
         [type_value, member]——补压 type_value（compile_type_expr 契约：
         类型表达式压类型值）。 */
      compile_type_expr(c, n->type);
    } else if (t->kind == TYPE_KIND_ARRAY) {
      /* array：先压类型位（CONSTRUCT 协议）→ fill 展开 + nil 元素（元素
         须 ?T，sema 已校验）+ 普通元素 → CONSTRUCT N（得 T 值）→ 补压
         类型位 → NEW */
      compile_type_expr(c, n->type);      /* 栈: [type_value] */
      const type_t *et = array_type_elem(t);
      size_t fcount = 0;
      for (ast_node_t *f = n->fields; f; f = f->next) {
        if (f->kind == AST_FILL) {
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
            const sema_type_t *est =
                c_sema_type_find_ptr(c->sema_types, et);
            if (!est) { c_error(c, f, "compiler: optional element type not registered"); return; }
            for (uint64_t k = 0; k < cnt; k++) {
              bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
              bcode_write_u32(c->bc, est->id);
              st_push(c, 1);
            }
          } else {
            for (uint64_t k = 0; k < cnt; k++)
              compile_expr(c, fl->value);
          }
          fcount += (size_t)cnt;
        } else {
          ast_node_t *value = f;
          if (f->kind == AST_CONSTRUCT_FIELD)
            value = ((ast_construct_field_t *)f)->value;
          if (value->kind == AST_NIL) {
            const sema_type_t *est =
                c_sema_type_find_ptr(c->sema_types, et);
            if (!est) { c_error(c, f, "compiler: optional element type not registered"); return; }
            bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
            bcode_write_u32(c->bc, est->id);
            st_push(c, 1);
          } else {
            compile_expr(c, value);
          }
          fcount++;
        }
      }
      bcode_write_op(c->bc, BCODE_CONSTRUCT);
      bcode_write_u32(c->bc, (uint32_t)fcount);
      st_push(c, -((int)fcount));
      /* CONSTRUCT 已消费类型位（弹 N+1 压 1 = T 值）；NEW 协议需要
         [type_value, member]——补压 type_value（compile_type_expr 契约：
         类型表达式压类型值）。 */
      compile_type_expr(c, n->type);
    } else if (t->kind == TYPE_KIND_UNION || t->kind == TYPE_KIND_CUNION) {
      /* union/cunion：与 AST_CONSTRUCT 同款 **tag/member 哨兵协议**：
         [type_value, tag_idx, payload] → CONSTRUCT 2（按 tag/subscript 定位
         member → implicit_cast → 写 payload）→ 补压类型位 → NEW。
         此前这里把 payload 直接交给 NEW，指望 NEW 把 payload 隐式转换成
         union——union 的 implicit_cast 只接受同类型，必然报
         "implicit cast: not a widening conversion"（实测），即
         `new U{...}` 对任何 union 都不可用。 */
      compile_type_expr(c, n->type);      /* 栈: [type_value] */
      uint32_t tag = n->member_tag;       /* sema 按名定位后写回的下标 */
      if (tag < 128) {
        bcode_write_op(c->bc, BCODE_PUSH_I8);
        bcode_write_i8(c->bc, (int8_t)tag);
      } else if (tag < 32768) {
        bcode_write_op(c->bc, BCODE_PUSH_I16);
        bcode_write_i16(c->bc, (int16_t)tag);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_I32);
        bcode_write_i32(c->bc, (int32_t)tag);
      }
      st_push(c, 1);
      ast_node_t *f = n->fields;
      ast_node_t *value = f;
      if (f && f->kind == AST_CONSTRUCT_FIELD)
        value = ((ast_construct_field_t *)f)->value;
      if (!value) {
        c_error(c, node, "compiler: new requires exactly 1 field");
        return;
      }
      if (value->kind == AST_NIL) {
        /* nil payload：member 须 ?T（sema 已校验）→ PUSH_OPT_NONE */
        const type_t *mt = NULL;
        if (tag < ((t->kind == TYPE_KIND_UNION)
                       ? (uint32_t)((const union_type_t *)t)->member_count
                       : (uint32_t)((const cunion_type_t *)t)->member_count)) {
          mt = (t->kind == TYPE_KIND_UNION)
                   ? union_type_member(t, tag)->payload_type
                   : cunion_type_member(t, tag)->type;
        }
        const sema_type_t *mst = mt ? c_sema_type_find_ptr(c->sema_types, mt)
                                    : NULL;
        if (!mst) {
          c_error(c, value, "compiler: optional union member type not registered");
          return;
        }
        bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
        bcode_write_u32(c->bc, mst->id);
        st_push(c, 1);
      } else {
        compile_expr(c, value);           /* 栈: [type_value, tag, payload] */
      }
      bcode_write_op(c->bc, BCODE_CONSTRUCT);
      bcode_write_u32(c->bc, 2);
      st_push(c, -3);
      compile_type_expr(c, n->type);      /* 栈: [union 值, type_value] */
    } else {
      /* 标量/option：恰好 1 个字段（sema 已校验） */
      ast_node_t *f = n->fields;
      if (f && f->kind == AST_CONSTRUCT_FIELD)
        f = ((ast_construct_field_t *)f)->value;
      if (f && f->kind == AST_NIL) {
        /* nil 字段：字段须 ?T（sema 已校验）→ PUSH_OPT_NONE <field id> */
        const sema_type_t *fst = NULL;
        if (t->kind == TYPE_KIND_OPTION) {
          const type_t *inner = type_option_inner(t);
          fst = c_sema_type_find_ptr(c->sema_types, inner ? inner : t);
        }
        if (!fst) {
          c_error(c, f, "compiler: optional new field type not registered");
          return;
        }
        bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
        bcode_write_u32(c->bc, fst->id);
      } else if (f) {
        compile_expr(c, f);               /* 栈: [member] */
      } else {
        c_error(c, node, "compiler: new requires exactly 1 field");
        return;
      }
      compile_type_expr(c, n->type);      /* 栈: [member, type_value] */
    }
    bcode_write_op(c->bc, BCODE_NEW);   /* 弹 2 压 1（fatal *T 指针） */
    st_push(c, -1);
    break;
  }
  case AST_DEREF: {
    /* 后置解引用取值 r.*（m3-design §8.2）：operand（指针）→ PTR_GET
       （弹指针 → 被指向 T 值借用引用，零拷贝）。 */
    ast_deref_t *n = (ast_deref_t *)node;
    compile_expr(c, n->operand);        /* 栈: [ptr] */
    bcode_write_op(c->bc, BCODE_PTR_GET);
    st_push(c, 0);                      /* 弹 1 压 1，净 0 */
    break;
  }
  case AST_ADDR: {
    /* 后置取地址 x.&（m3-design §8.2）：operand → ADDR（弹值 → ref *T
       借用指针，ref 总借用，运行期零标志 §13.2）。 */
    ast_addr_t *n = (ast_addr_t *)node;
    compile_expr(c, n->operand);        /* 栈: [value] */
    bcode_write_op(c->bc, BCODE_ADDR);
    st_push(c, 0);                      /* 弹 1 压 1，净 0 */
    break;
  }
  case AST_IS_TAG: {
    /* sema 生成的 tag 判定：编译 object → `IS_TAG <tag>`（弹 union 值 → 压
       bool）。与用户写 `v is m` 编译出的指令完全一致，只是 tag 已由 sema
       填好（不需要 token_t）。 */
    ast_is_tag_t *n = (ast_is_tag_t *)node;
    if (!n->object) {
      c_error(c, node, "is_tag: missing object");
      return;
    }
    compile_expr(c, n->object);            /* 栈: [value] */
    bcode_write_op(c->bc, BCODE_IS_TAG);
    bcode_write_u32(c->bc, n->tag);        /* 弹 value → 压 bool */
    st_push(c, 0);
    break;
  }
  case AST_FREE: {
    /* `FREE <expr>` 是**表达式**（结果 void）：编译操作数压栈，再发 FREE
       （只消费栈顶）。**compiler 不知道任何类型结构**——"释放什么"完全由
       sema 生成的操作数表达式决定（struct 槽位 → FIELD_GET 链、数组/切片
       元素 → INDEX 链、闭包捕获 → FIELD_GET 按捕获名）。
       所有权模型 A 方案：销毁在 sema 侧**完全由 AST 生成**，切片插 for、
       optional/union 插 if，退出点插入的是**语句列表**而不是单条 FREE。 */
    ast_free_t *n = (ast_free_t *)node;
    if (n->operand) {
      compile_expr(c, n->operand);         /* 栈: [handle] */
      bcode_write_op(c->bc, BCODE_FREE);   /* 释放栈顶，净 -1 */
      st_push(c, -1);
    }
    break;
  }
  case AST_MOVE: {
    /* 所有权原语 move(x)/clone(x)（m3-design §7）：operand → MOVE/CLONE
       （弹值 → 同类型新值，Step A 静默）。op 为原语关键字 token。 */
    ast_move_t *n = (ast_move_t *)node;
    compile_expr(c, n->operand);        /* 栈: [value] */
    if (n->op && token_is(n->op, "clone"))
      bcode_write_op(c->bc, BCODE_CLONE);
    else
      bcode_write_op(c->bc, BCODE_MOVE);
    st_push(c, 0);                      /* 弹 1 压 1，净 0 */
    break;
  }
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
  case AST_SLICE: {
    /* 切片表达式 s[low:high]（m4-design §4）：恒产 ref []T 胖指针。
       栈协议：[self, high?, low?]——low 在栈顶（最后编译先弹）。
       low/high 可省略（NULL）：省略 low = 0，省略 high = len(self)。
       NULL 时不压栈——SLICE 指令 u8 flags 标识 low/high 存在性
       （bit0=low, bit1=high），op_slice 按 flags 决定弹栈次数。 */
    ast_slice_t *n = (ast_slice_t *)node;
    compile_expr(c, n->object);       /* 栈: [self] */
    int pushed = 1;                   /* 已压 self */
    uint8_t flags = 0;
    /* high 在下、low 在上（SLICE 弹栈顺序：low 先弹，high 后弹） */
    if (n->high) {
      compile_expr(c, n->high);       /* 栈: [self, high] */
      pushed++;
      flags |= 0x02;
    }
    if (n->low) {
      compile_expr(c, n->low);        /* 栈: [self, high, low] */
      pushed++;
      flags |= 0x01;
    }
    bcode_write_op(c->bc, BCODE_SLICE);
    bcode_write_u8(c->bc, flags);
    st_push(c, -(pushed - 1));        /* 弹 pushed 压 1，净 -(pushed-1) */
    break;
  }
  case AST_MAKE: {
    /* make(T, N, init...) 宏函数（m4-design §2）：→ fatal []T。
       栈协议：[elem_type, length, init1..initN]——elem_type 在栈底，
       initN 在栈顶。MAKE <init_count> 弹 init_count+2 压 1。
       inits 支持 <v,M> fill 展开（同数组构造），nil 元素须 ?T（sema 已
       校验）。clux 无默认零值，init_count 须 == length（sema 已校验）。 */
    ast_make_t *n = (ast_make_t *)node;
    /* 元素类型位 */
    compile_type_expr(c, n->elem_type);  /* 栈: [elem_type] */
    /* 长度值（可运行期值） */
    compile_expr(c, n->length);          /* 栈: [elem_type, length] */
    /* 初始值逐个压栈（fill 展开 + nil → PUSH_OPT_NONE） */
    const type_t *et = c_resolve_type(c, n->elem_type);
    size_t icount = 0;
    for (ast_node_t *f = n->inits; f; f = f->next) {
      if (f->kind == AST_FILL) {
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
          const sema_type_t *est = c_sema_type_find_ptr(c->sema_types, et);
          if (!est) { c_error(c, f, "compiler: optional element type not registered"); return; }
          for (uint64_t k = 0; k < cnt; k++) {
            bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
            bcode_write_u32(c->bc, est->id);
            st_push(c, 1);
          }
        } else {
          for (uint64_t k = 0; k < cnt; k++) {
            compile_expr(c, fl->value);
          }
        }
        icount += (size_t)cnt;
      } else if (f->kind == AST_NIL) {
        const sema_type_t *est = c_sema_type_find_ptr(c->sema_types, et);
        if (!est) { c_error(c, f, "compiler: optional element type not registered"); return; }
        bcode_write_op(c->bc, BCODE_PUSH_OPT_NONE);
        bcode_write_u32(c->bc, est->id);
        st_push(c, 1);
        icount++;
      } else {
        compile_expr(c, f);
        icount++;
      }
    }
    bcode_write_op(c->bc, BCODE_MAKE);
    bcode_write_u32(c->bc, (uint32_t)icount);
    st_push(c, -((int)icount) - 1);     /* MAKE 弹 icount+2 压 1，净 -(icount+1) */
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
