#include "sema/sema.h"
#include "core/string.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_error.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_nil.h"
#include "parser/ast_option.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_unary.h"
#include "parser/ast_undef.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_volatile.h"
#include "parser/lexer.h"
#include "ctfe/ctfe.h"
#include "sema/comptime.h"
#include "vm/function.h"
#include "vm/type_array.h"
#include "vm/type_error.h"
#include "vm/type_option.h"

#include <stdio.h>
#include <string.h>

/* ===========================================================================
 * 表达式 walker（shadow value 求值）
 *
 * 每个表达式节点返回一个 shadow value（只有类型，data=NULL）。shadow value
 * 经 vtable 运算路径，类型协商结果即为推导结果类型。错误恢复产物
 * （error/void shadow）沿运算传播但不级联二次诊断。
 *
 * 未初始化（TDZ）检查由确定性赋值分析在符号表 flow_init 上完成
 * （sema_lookup 跳过未激活符号），VM 值层不感知。
 * =========================================================================== */

/* 把操作数类型名写入诊断缓冲区 */
static void op_type_name(value_t *v, char *buf, size_t cap) {
  if (!v) {
    snprintf(buf, cap, "<none>");
    return;
  }
  const type_t *t = value_type(v);
  if (t && t->name.ptr)
    snprintf(buf, cap, "%.*s", (int)t->name.len, t->name.ptr);
  else
    snprintf(buf, cap, "<none>");
}

/* 检查操作数必须为 bool；错误/void shadow（错误恢复产物）静默通过 */
void sema_check_bool(sema_t *sema, ast_node_t *node, value_t *v,
                     const char *what) {
  if (value_is_error(sema->vm, v)) return;
  if (value_is_type(v, TYPE_KIND_VOID)) return;
  if (!value_is_type(v, TYPE_KIND_BOOL)) {
    char tn[64];
    op_type_name(v, tn, sizeof(tn));
    diag_error(sema->diag, sema_loc(sema, node), "%s operand must be bool, got %s",
               what, tn);
  }
}

/* 函数自身符号归属检查（捕获检查用）：sym 是函数可见链上的符号（fscope
   直系捕获 / param_scope 直系参数）→ true。false = 外层局部（需捕获）或
   非函数自身符号。参数在参数层（fscope 子 scope），捕获在 fscope——参数
   遮蔽捕获，两者同属函数自身符号，放行。 */
static bool func_own_symbol(sema_t *sema, strslice_t name,
                            sema_symbol_t *sym) {
  if (!sema->local_func_base) return false;
  if (sema_scope_find_local(sema->local_func_base, name) == sym) return true;
  if (sema->local_func_param_scope &&
      sema_scope_find_local(sema->local_func_param_scope, name) == sym)
    return true;
  return false;
}

/* 闭包 TDZ 检查（编译期）：被引用/调用的局部函数若有捕获（fscope 捕获符号
   存在），其捕获值在定义点 resolve_func_captures 才绑定（is_active 激活）。
   walk 顺序保证：定义点前的引用点（函数提升后）捕获符号尚未激活 → TDZ，
   编译期报错（捕获槽运行期仍为 undefined 占位，但不应静默到运行期才暴露）。
   无捕获的局部函数提升后即可安全引用（hoist 只绑定地址，无捕获槽）。 */
static bool func_capture_tdz(sema_scope_t *fscope, ast_node_t *captures) {
  if (!fscope || !captures) return false;
  for (ast_node_t *c = captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (cs && !cs->is_active) return true;
  }
  return false;
}

/* 二元运算符 token → vtable 分派函数 */
static value_t *(*binop_of(const token_t *op))(vm_t *, value_t *, value_t *) {
  if (token_is(op, "+")) return value_add;
  if (token_is(op, "-")) return value_sub;
  if (token_is(op, "*")) return value_mul;
  if (token_is(op, "/")) return value_div;
  if (token_is(op, "%")) return value_mod;
  if (token_is(op, "==")) return value_eq;
  if (token_is(op, "!=")) return value_ne;
  if (token_is(op, "<")) return value_lt;
  if (token_is(op, "<=")) return value_le;
  if (token_is(op, ">")) return value_gt;
  if (token_is(op, ">=")) return value_ge;
  if (token_is(op, "&")) return value_band;
  if (token_is(op, "|")) return value_bor;
  if (token_is(op, "^")) return value_bxor;
  if (token_is(op, "<<")) return value_shl;
  if (token_is(op, ">>")) return value_shr;
  return NULL;
}

/* 从 token 取运算符文本（诊断用），写入 buf */
static void op_text(const token_t *op, char *buf, size_t cap) {
  size_t len = 0;
  const char *text = op ? token_get_text(op, &len) : NULL;
  if (!text || len == 0) {
    snprintf(buf, cap, "?");
    return;
  }
  size_t n = len < cap - 1 ? len : cap - 1;
  memcpy(buf, text, n);
  buf[n] = '\0';
}

static value_t *shadow_binary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope);

value_t *sema_expr(sema_t *sema, ast_node_t **node, sema_scope_t *scope) {
  if (!node || !*node) return value_make_shadow(sema->vm, sema->vm->type_void);
  switch ((*node)->kind) {
    case AST_INT_LIT: {
      ast_int_lit_t *n = (ast_int_lit_t *)*node;
      const type_t *t = n->type.len ? type_lookup(sema->vm, n->type)
                                    : sema->vm->type_i32;
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, *node), "unknown type '%.*s'",
                   (int)n->type.len, n->type.ptr);
        t = sema->vm->type_i32;
      }
      return value_make_shadow(sema->vm, t);
    }
    case AST_FLOAT_LIT: {
      ast_float_lit_t *n = (ast_float_lit_t *)*node;
      const type_t *t = n->type.len ? type_lookup(sema->vm, n->type)
                                    : sema->vm->type_f64;
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, *node), "unknown type '%.*s'",
                   (int)n->type.len, n->type.ptr);
        t = sema->vm->type_f64;
      }
      return value_make_shadow(sema->vm, t);
    }
    case AST_BOOL_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    case AST_CHAR_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_u8);
    case AST_STRING_LIT:
      return value_make_shadow(sema->vm, sema->vm->type_str);
    case AST_IDENT: {
      /* 变量读取：从 VM scope 链 lookup shadow value（与 sema 作用域树同构，
         天然遮罩）。未初始化检查走符号表 flow_init（确定性赋值分析，
         VM 值层不感知 TDZ）。
         comptime var：定义点已从语句链摘除（不在 VM scope），引用点折叠为
         字面量 AST 节点（sema_ct_lit，arena 分配），下游（编译器）零感知。 */
      ast_ident_t *n = (ast_ident_t *)*node;
      sema_symbol_t *sym = sema_lookup(scope, n->name);

      /* 函数引用（函数值，编译期/运行期二元）：符号种类 SYM_FUNC（用户函数
         与内建函数 printf 统一——两者在 sema/ctfe/compiler/vm 视角无区别，
         仅 func_t id 段不同，由 func_new 创建时分配）。
         遮蔽平等：局部变量遮蔽函数名时 sym 解析到变量符号（SYM_VAR），
         不走此分支——`var add = 2; var x = add;` 中 add 是变量引用。
         改写为 AST_FUNC_REF（compiler 发 LOAD_FUNCTION <id> 运行期加载
         真实函数值），此处返回签名类型 shadow（编译期折叠，供赋值/实参/
         返回值的签名匹配校验）。comptime func 不进入运行时，作为值引用
         是非法用法（调用点在 AST_CALL 折叠）。 */
      if (sym && sym->kind == SEMA_SYM_FUNC && sym->type &&
          sym->type->kind == TYPE_KIND_FUNC) {
        /* 局部函数体内引用兄弟/自身函数值：运行时函数体查找链只有参数 +
           全局（closure_scope 为空、调用时临时接 root_scope），兄弟/自身
           符号在定义点块作用域，不可见（需闭包）。FUNC 分支改写 AST_FUNC_REF
           → LOAD_FUNCTION 查 functions_by_id 全局表，会绕过作用域可见性
           ——在此显式拦截。参数（param_scope 直系，含函数类型参数——可调用，
           运行时参数可见）与全局函数（global_scope 符号）放行，与变量
           分支捕获检查同构。 */
        if (sema->local_func_base && !func_own_symbol(sema, n->name, sym) &&
            sema_lookup(sema->global_scope, n->name) != sym) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "local function cannot reference sibling or self '%.*s' "
                     "(add it to the capture list)",
                     (int)n->name.len, n->name.ptr);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        if (sym->is_comptime) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "comptime function '%.*s' cannot be used as a value",
                     (int)n->name.len, n->name.ptr);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        /* 闭包捕获 TDZ（编译期）：局部函数的捕获值在定义点 resolve_func_captures
           才绑定（is_active 激活）。walk 顺序保证定义点前的引用点捕获符号未激活
           → 编译期报错，不静默到运行期（捕获槽运行期仍为 undefined 占位）。
           全局函数 captures 恒空（hoist 基底即最终实例），天然放行。 */
        sema_func_t *tdz_sf = sema_func_by_id(sema, sym->fid);
        if (tdz_sf && tdz_sf->is_local && tdz_sf->def &&
            tdz_sf->def->kind == AST_FUNC_DEF) {
          ast_func_def_t *tdz_fn = (ast_func_def_t *)tdz_sf->def;
          if (func_capture_tdz(tdz_sf->scope, tdz_fn->captures)) {
            diag_error(sema->diag, sema_loc(sema, *node),
                       "closure '%.*s' used before its captures are bound (TDZ)",
                       (int)n->name.len, n->name.ptr);
            return value_make_shadow(sema->vm, sema->vm->type_void);
          }
        }
        ast_node_t *ref = ast_func_ref_new(sema->arena, (*node)->tok_begin,
                                           (*node)->tok_end);
        if (ref) {
          /* fid 由 sema 创建函数对象时分配（符号表字段；内建函数 = 内建 id）。
             name 指向源标识符 token（compiler 发 PUSH name 沿作用域链查找——
             局部函数取定义点 STORE 重定向的新实例；全局/内建取全局绑定基底，
             与定义点 MAKE_FUNCTION 新实例语义对齐）。 */
          ast_func_ref_t *fr = (ast_func_ref_t *)ref;
          fr->fid  = sym->fid;
          fr->name = n->name;
          ref->next = (*node)->next; /* 保留兄弟链 */
          *node = ref;
        }
        return value_make_shadow(sema->vm, sym->type);
      }

      if (sym && !sym->flow_init) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "variable '%.*s' used before initialization",
                   (int)n->name.len, n->name.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (sym && sym->is_comptime && sym->ct_valid) {
        /* 折叠引用点为字面量（保留兄弟链，供调用点/语句链继续遍历） */
        ast_node_t *lit = sema_ct_lit(sema, *node, &sym->ct);
        if (lit) {
          lit->next = (*node)->next;
          *node = lit;
          return value_make_shadow(sema->vm, sym->ct.type);
        }
      }
      /* 捕获检查（无闭包）：局部函数体内引用外层局部符号（外层块/外层函数
         的变量、参数、类型名）→ 运行时函数体查找链只有参数 + 全局
         （closure_scope 为空、调用时临时接 root_scope），外层局部不可见。
         fscope parent = 定义点块作用域（同块局部函数互相可见）——沿链命中的
         "非 fscope 直系、非 global"符号即外层局部。全局符号（sema_lookup
         global 命中）、函数名（FUNC 分支已先行返回）、comptime 符号（上方
         已折叠，不捕获）合法。 */
      if (sema->local_func_base && sym &&
          !func_own_symbol(sema, n->name, sym) &&
          sema_lookup(sema->global_scope, n->name) != sym) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "local function cannot access outer local '%.*s' "
                   "(add it to the capture list)",
                   (int)n->name.len, n->name.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      value_t *v = scope_lookup(sema->vm->current_scope, n->name);
      if (!v) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "undefined variable '%.*s'", (int)n->name.len, n->name.ptr);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      /* 类型即表达式：内建/自定义类型值注册在作用域链（global/root/current），
         与变量同机制查找。命中 type value 直接返回该真实值（type value 是
         编译期实体，data 恒为 type_t*——无 shadow 形态，见 sema 注释）。 */
      if (value_type(v) == sema->vm->type_type)
        return v;

      /* ?T 变量读取：始终按 ?T 类型返回 shadow（无窄化退化——optional
         解包由显式 .! assert 承担，见 docs m2-design §12.5） */
      return value_make_shadow(sema->vm, value_type(v));
    }
    case AST_CONST: {
      /* const 类型修饰（类型即表达式）：操作数必须是类型值，结果仍是
         类型值。遮蔽/类型检查在此感知（as 右值、M2 类型字面量）。 */
      ast_const_t *n = (ast_const_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (value_is_error(sema->vm, sub) || value_is_type(sub, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      if (!value_is_type(sub, TYPE_KIND_TYPE)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'const' requires a type operand");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      /* type value 恒真实（编译期实体）：取 data 里的类型指针 intern 后
         返回新的 type value（auto-track 当前 scope） */
      const type_t *sub_t = value_as(sub, const type_t *);
      const type_t *ct = type_const_intern(sema->vm, sub_t);
      return type_as_value(sema->vm, ct);
    }
    case AST_VOLATILE: {
      ast_volatile_t *n = (ast_volatile_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (value_is_error(sema->vm, sub) || value_is_type(sub, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      if (!value_is_type(sub, TYPE_KIND_TYPE)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'volatile' requires a type operand");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      const type_t *sub_t = value_as(sub, const type_t *);
      const type_t *vt = type_volatile_intern(sema->vm, sub_t);
      return type_as_value(sema->vm, vt);
    }
    case AST_UNDEF:
      /* undefined 只允许作为 var 初始化的"未初始化声明"（shadow_var_def
         消费）；普通表达式位置引用是非法用法。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "'undefined' can only be used as a variable initializer");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    case AST_NIL:
      /* nil 不是 value：只允许四种 nil 判定（x==nil 等，窄化）、?T 初始化
         赋值（a = nil）、构造器字段（.?T{nil}）与 fill 值包（<nil,N>）等
         特殊位置消费（shadow_binary / 赋值 / 构造器 / fill 分别处理）。
         普通表达式位置引用是非法用法。 */
      diag_error(sema->diag, sema_loc(sema, *node),
                 "'nil' can only be used in nil comparisons, ?T assignment, "
                 "constructor fields or fills");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    case AST_BINARY:
      return shadow_binary(sema, node, scope);
    case AST_UNARY: {
      ast_unary_t *n = (ast_unary_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      /* 错误恢复产物静默通过，避免级联二次诊断 */
      if (value_is_error(sema->vm, operand) ||
          value_is_type(operand, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      value_t *result = NULL;
      if (token_is(n->op, "-")) {
        result = value_neg(sema->vm, operand);
      } else if (token_is(n->op, "!")) {
        sema_check_bool(sema, n->operand, operand, "logical not");
        result = value_lnot(sema->vm, operand);
      } else if (token_is(n->op, "~")) {
        result = value_bnot(sema->vm, operand);
      } else {
        char ob[16];
        op_text(n->op, ob, sizeof(ob));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "unsupported unary operator '%s'", ob);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (value_is_error(sema->vm, result)) {
        char ob[16], tn[64];
        op_text(n->op, ob, sizeof(ob));
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '%s' cannot be applied to %s", ob, tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return result;
    }
    case AST_CALL: {
      /* 函数调用：统一路径 = exec callee（shadow 求值）→ exec 各实参 →
         value_call 分派到 func_vcall 的 shadow 分支（唯一校验点：参数
         数量/隐式转换，不执行 cfunc）→ 错误翻译为诊断。
         遮蔽/可见性全部交给作用域机制：callee 是 AST_IDENT 时经
         sema_expr → AST_IDENT 分支自然解析（局部变量遮蔽函数名 → 变量
         符号 → 下方 func 类型检查报"不可调用"；兄弟/自身函数 → 该分支
         闭包检查拦截；未定义 → "undefined variable"）。callee 是函数值
         表达式（get_fn()()、函数字面量）时递归求值 shadow（可能触发内层
         comptime 折叠）。
         唯一保留的符号感知前置：comptime func 调用点折叠（真实调用点
         CTFE 求值；walking_comptime 时实参是参数 shadow value 无法求值，
         只构造 callee shadow 做类型检查，折叠留到真实调用点）。 */
      ast_call_t *call = (ast_call_t *)*node;
      if (!call->callee) {
        diag_error(sema->diag, sema_loc(sema, &call->base),
                   "M1: missing callee");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      value_t *callee = NULL;
      if (call->callee->kind == AST_IDENT) {
        sema_symbol_t *sym =
            sema_lookup(scope, ((ast_ident_t *)call->callee)->name);
        if (sym && sym->is_comptime) {
          /* comptime func 调用：实参改写（折叠引用为字面量）+ ctfe 求值 →
             整个调用折叠为字面量。函数本身不注册到运行时。 */
          if (!sema->walking_comptime)
            return sema_eval_comptime_call(sema, node, scope);
          /* walking_comptime：只做 shadow 类型检查（callee shadow 走下方
             统一 value_call 路径，返回 return type 的 shadow） */
          callee = value_make_shadow(sema->vm, sym->type);
        }
      }
      if (!callee) {
        /* 一般 callee（函数名 / 函数值表达式）统一 shadow 求值 */
        callee = sema_expr(sema, &call->callee, scope);
        if (value_is_error(sema->vm, callee) ||
            value_is_type(callee, TYPE_KIND_VOID))
          return value_make_shadow(sema->vm, sema->vm->type_void);
        const type_t *ctype = value_type(callee);
        if (!ctype || ctype->kind != TYPE_KIND_FUNC) {
          char tn[64];
          op_type_name(callee, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "cannot call value of type %s", tn);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
      }

      size_t argc = sema_count_siblings(call->args);
      value_t *arg_shadows[argc > 0 ? argc : 1];
      ast_node_t **link = &call->args;
      for (size_t i = 0; *link; link = &(*link)->next, i++) {
        arg_shadows[i] = sema_expr(sema, link, scope);
      }
      value_t *result = value_call(sema->vm, callee, arg_shadows, argc);

      if (value_is_error(sema->vm, result)) {
        error_data_t *ed = (error_data_t *)value_data(result);
        const char *msg = ed && ed->message ? string_cstr(ed->message)
                                            : "function call failed";
        diag_error(sema->diag, sema_loc(sema, &call->base), "%s", msg);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      return result; /* shadow in → shadow out（return_type shadow） */
    }
    case AST_UNWRAP: {
      /* optional 解包：.!（assert）/ .?（try）。
         .! 要求操作数是 ?T：none 时运行期 panic（用户范式先判空再解包：
         if (x != nil) { var v = x.!; ... }）。返回 inner 类型 shadow。
         .? 仅词法预留（try），语义未实现 → 编译期报错。 */
      ast_unwrap_t *uw = (ast_unwrap_t *)*node;
      value_t *operand = sema_expr(sema, &uw->operand, scope);
      if (value_is_error(sema->vm, operand) ||
          value_is_type(operand, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);

      const type_t *ot = value_type(operand);
      if (!ot || ot->kind != TYPE_KIND_OPTION) {
        char tn[64], ob[16];
        size_t len = 0;
        const char *text =
            uw->op ? token_get_text(uw->op, &len) : NULL;
        snprintf(ob, sizeof(ob), ".%.*s", (int)len, text ? text : "?");
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '%s' requires an optional operand, got %s", ob,
                   tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (uw->op && token_is(uw->op, "?")) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '.?' (try) is not implemented yet");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return value_make_shadow(sema->vm, type_option_inner(ot));
    }
    case AST_MEMBER:
      diag_error(sema->diag, sema_loc(sema, *node),
                 "member access is not supported in M1");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    case AST_INDEX: {
      /* 下标 / 泛型索引（GAP 延后落点）：parser 只收集 <expr>[<expr,...>
         的 object + indices 链，此处首次可区分——base 是数组 → 下标（GET）；
         base 是类型值 → 泛型实例化语法（M2 未实现，占位诊断）。 */
      ast_index_t *n = (ast_index_t *)*node;
      value_t *base = sema_expr(sema, &n->object, scope);
      if (value_is_error(sema->vm, base) ||
          value_is_type(base, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);

      const type_t *bt = value_type(base);
      if (bt && bt->kind == TYPE_KIND_TYPE) {
        /* 泛型实例化占位：<type>[<expr,...> 与下标语法重叠，延后到 sema
           才可区分，M2 泛型未实现前明确占位。 */
        diag_error(sema->diag, sema_loc(sema, *node),
                   "generic instantiation is not implemented (index on a type value)");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      if (!bt || bt->kind != TYPE_KIND_ARRAY) {
        char tn[64];
        sema_type_name(bt, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot index value of type %s", tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      /* 数组下标只消费 1 个索引；a[i,j] 多索引 = 泛型实参语法预留。
         a[i][j] 多维是 parse 链式嵌套（((a[i])[j])），逐维下降天然处理。 */
      size_t nidx = sema_count_siblings(n->indices);
      if (nidx != 1) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "array subscript expects exactly 1 index, got %zu", nidx);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      value_t *idx = sema_expr(sema, &n->indices, scope);
      if (value_is_error(sema->vm, idx) ||
          value_is_type(idx, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      if (!value_is_type(idx, TYPE_KIND_INT)) {
        char tn[64];
        op_type_name(idx, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, n->indices),
                   "array index must be an integer, got %s", tn);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      const type_t *et = array_type_elem(bt);
      return value_make_shadow(sema->vm, et ? et : sema->vm->type_void);
    }
    case AST_ARRAY: {
      /* 数组类型表达式 [N]T（类型即表达式）：表达式位置求值 = 类型值。
         解析为真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），
         返回真实 type value（data=type_t*，auto-track 当前 scope），供
         type 定义 / as 右值 / sizeof / 嵌套类型构造消费。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
      return type_as_value(sema->vm, t);
    }
    case AST_TYPE_REF: {
      /* 具名类型引用（sema 登记的 "__type_N"）：重复求值（折叠重访）时
         命中——解析回类型，返回真实 type value。 */
      const type_t *t = resolve_type_expr(sema, *node);
      if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);
      return type_as_value(sema->vm, t);
    }
    case AST_CONSTRUCT: {
      /* 类型字面量构造 .<type>{ fields }：求值类型位为真实类型，校验
         fields 数量与元素类型（value_assign 单一校验点），返回该类型
         的 shadow value（运行期由 CONSTRUCT 字节码完成值构造）。
         当前实现 array + option 分支（struct/tuple 待后续 Phase）。 */
      ast_construct_t *n = (ast_construct_t *)*node;
      const type_t *t = sema_resolve_type_slot(sema, &n->type);
      if (!t) return value_make_shadow(sema->vm, sema->vm->type_void);

      /* ?T 构造器（二值单槽位，docs m2-design §12.2）：字段数强制 == 1，
         字段类型 ∈ {nil, T}。nil 字段 → 运行期 PUSH_OPT_NONE；T 字段 →
         隐式提升 D1。字段类型显式校验（不走 value_assign——option_assign
         的 shadow 短路会绕过类型检查，须在此处直接比较 inner）。 */
      if (t->kind == TYPE_KIND_OPTION) {
        size_t nfields = sema_count_siblings(n->fields);
        if (nfields != 1) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "optional constructor expects exactly 1 field, got %zu",
                     nfields);
          return value_make_shadow(sema->vm, sema->vm->type_void);
        }
        ast_node_t *f = n->fields;
        if (f && f->kind != AST_NIL) {
          value_t *fv = sema_expr(sema, &f, scope);
          if (!value_is_error(sema->vm, fv) &&
              !value_is_type(fv, TYPE_KIND_VOID)) {
            const type_t *inner = type_option_inner(t);
            const type_t *ft = value_type(fv);
            if (ft != t && ft != inner) {
              char tn[64], fn[64];
              sema_type_name(t, tn, sizeof tn);
              sema_type_name(ft, fn, sizeof fn);
              diag_error(sema->diag, sema_loc(sema, f),
                         "cannot initialize optional %s with %s", tn, fn);
            }
          }
        }
        return value_make_shadow(sema->vm, t);
      }

      if (t->kind != TYPE_KIND_ARRAY) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "construct: unsupported type (only array and optional "
                   "implemented)");
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      /* 数组构造（完全显式，docs m2-design §12.4）：总元素数 =
         Σfill counts + 显式字段数，必须 == 数组长度（无自动 0 填充、
         无运行时裁剪）。fill 的 N 编译期常量（sema_eval_array_bound
         求值 + 折叠为 AST_INT_LIT，编译器读立即数）。 */
      const type_t *et = array_type_elem(t);
      size_t len = array_type_len(t);

      /* 第一遍：统计总元素数 + 求值 fill count（折叠写回） */
      size_t total = 0;
      for (ast_node_t *f = n->fields; f; f = f->next) {
        if (f->kind == AST_FILL) {
          size_t cnt;
          if (!sema_eval_array_bound(sema, &((ast_fill_t *)f)->count, &cnt))
            return value_make_shadow(sema->vm, sema->vm->type_void);
          total += cnt;
        } else {
          total += 1;
        }
      }
      if (len != SIZE_MAX && total != len) {
        char tn[64];
        sema_type_name(t, tn, sizeof tn);
        diag_error(sema->diag, sema_loc(sema, *node),
                   "construct: expected %zu elements for %s, got %zu", len, tn,
                   total);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }

      /* 第二遍：逐字段求值 + 元素类型校验（经链上指针传递，使 comptime
         引用折叠就地写回字段链，编译器零感知）。fill 的 v 求值一次，
         count 份同类型；nil 字段/fill 的 nil 值 → 元素须为 ?T（运行期
         PUSH_OPT_NONE，跳过求值——nil 非 value）。 */
      size_t idx = 0;
      for (ast_node_t **link = &n->fields; *link; link = &(*link)->next) {
        ast_node_t *f = *link;
        value_t *fv = NULL;
        bool is_nil_field = false;
        if (f->kind == AST_FILL) {
          ast_fill_t *fl = (ast_fill_t *)f;
          is_nil_field = fl->value->kind == AST_NIL;
          if (!is_nil_field)
            fv = sema_expr(sema, &fl->value, scope);
        } else if (f->kind == AST_NIL) {
          is_nil_field = true;
        } else {
          fv = sema_expr(sema, link, scope);
        }

        if (is_nil_field) {
          if (!et || et->kind != TYPE_KIND_OPTION) {
            char tn[64];
            sema_type_name(et, tn, sizeof tn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "cannot initialize array element with nil (element "
                       "type %s is not optional)",
                       tn);
          }
          idx += 1;
          continue;
        }

        if (fv && !value_is_error(sema->vm, fv) &&
            !value_is_type(fv, TYPE_KIND_VOID) && et) {
          value_t *dst = value_make_shadow(sema->vm, et);
          if (value_is_error(sema->vm, value_assign(sema->vm, dst, fv))) {
            char tn[64], fn[64];
            sema_type_name(et, tn, sizeof tn);
            sema_type_name(value_type(fv), fn, sizeof fn);
            diag_error(sema->diag, sema_loc(sema, f),
                       "cannot initialize array element %zu with %s (element "
                       "type %s)",
                       idx, fn, tn);
          }
        }
        idx += 1;
      }
      return value_make_shadow(sema->vm, t);
    }
    case AST_FUNC_DEF: {
      /* 函数字面量（表达式内 func 定义，函数值）：签名解析 + body 类型
         检查（sema_check_func_literal：临时 fscope 同步建树+walk，不注册
         作用域符号、不提升）。返回签名类型的 shadow——函数值表达式的类型
         即签名。失败（已诊断）返回 void shadow。 */
      ast_func_def_t *fn = (ast_func_def_t *)*node;
      const type_t *sig = sema_check_func_literal(sema, fn, scope);
      if (!sig) return value_make_shadow(sema->vm, sema->vm->type_void);
      return value_make_shadow(sema->vm, sig);
    }
    case AST_FUNC_REF: {
      /* 函数引用折叠产物（sema AST_IDENT 确认函数符号改写 / comptime 折叠
         AST_FUNC_REF）：纯 fid 标识，shadow walk 只关心签名类型——返回签名
         类型的 shadow（函数值表达式的类型即签名；运行期 LOAD_FUNCTION 加载
         真实函数值）。
         签名查询双路径（与 CTFE 一致）：程序函数（全局/局部）按 fid 查
         sema->funcs（def->sig_id → sema_type_by_id 反查）；内建函数与 CTFE
         构造的函数字面量引用对象按 fid 查 vm->functions。 */
      ast_func_ref_t *n = (ast_func_ref_t *)*node;
      const type_t *sig = NULL;
      sema_func_t *sf = sema_func_by_id(sema, n->fid);
      if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
        ast_func_def_t *fd = (ast_func_def_t *)sf->def;
        const sema_type_t *st = sema_type_by_id(sema, fd->sig_id);
        if (st) sig = st->type;
      }
      /* 内建函数 / CTFE 构造的函数字面量引用对象：vm 侧按 fid 查签名。
         内建函数登记 functions_by_id（id < PROGRAM_BASE）；字面量引用对象
         （func_new_program_ref）只注册 vm->functions 不登记 functions_by_id
         ——线性扫描兜底（函数数量少）。 */
      if (!sig) {
        func_t *fn = vm_func_load(sema->vm, n->fid);
        if (!fn && sema->vm->functions) {
          size_t nf = vec_len(sema->vm->functions);
          for (size_t i = 0; i < nf; i++) {
            func_t *f = (func_t *)vec_get(sema->vm->functions, i);
            if (f && f->id == n->fid && f->type) { fn = f; break; }
          }
        }
        if (fn) sig = fn->type;
      }
      if (!sig) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "unknown function reference (id %u)", n->fid);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return value_make_shadow(sema->vm, sig);
    }
    case AST_TERNARY: {
      /* 三元条件表达式：cond 必须 bool；两分支都 shadow 求值（两侧都要
         类型检查，运行时惰性只执行选中分支）；结果类型要求两分支一致。
         类型即表达式天然支持：`cond ? i32 : i64` 两侧都是 type_type。 */
      ast_ternary_t *n = (ast_ternary_t *)*node;
      value_t *cond = sema_expr(sema, &n->cond, scope);
      sema_check_bool(sema, n->cond, cond, "ternary condition");
      value_t *tval = sema_expr(sema, &n->then_branch, scope);
      value_t *eval = sema_expr(sema, &n->else_branch, scope);
      if (value_is_error(sema->vm, cond) || value_is_error(sema->vm, tval) ||
          value_is_error(sema->vm, eval) ||
          value_is_type(tval, TYPE_KIND_VOID) ||
          value_is_type(eval, TYPE_KIND_VOID))
        return value_make_shadow(sema->vm, sema->vm->type_void);
      const type_t *tt = value_type(tval);
      const type_t *et = value_type(eval);
      if (tt != et) {
        char tn[64], en[64];
        op_type_name(tval, tn, sizeof(tn));
        op_type_name(eval, en, sizeof(en));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "ternary branches must have the same type, got %s and %s",
                   tn, en);
        return value_make_shadow(sema->vm, sema->vm->type_void);
      }
      return tval; /* shadow，类型 = 两分支共同类型 */
    }
    case AST_ERROR:
    default:
      return value_make_shadow(sema->vm, sema->vm->type_void);
  }
}

static value_t *shadow_binary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  ast_binary_t *b = (ast_binary_t *)*node;
  /* as：显式类型转换（普通中缀运算符）。lhs shadow 求值；rhs 是类型表达式，
     走 ctfe 编译期真实求值（vm->comptime 模式，调用 ctfe_eval），拿真实
     type value 的 data（type_t*）作为转换目标——遮蔽语义在此感知。 */
  if (token_is(b->op, "as")) {
    value_t *expr = sema_expr(sema, &b->lhs, scope);
    if (value_is_error(sema->vm, expr) ||
        value_is_type(expr, TYPE_KIND_VOID))
      return value_make_shadow(sema->vm, sema->vm->type_void);
    vm_t *vm = sema->vm;
    bool saved = vm->comptime;
    vm->comptime = true;
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vm = vm;
    ctx.sema = sema;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *ty = ctfe_eval(&ctx, b->rhs);
    vm->comptime = saved;
    if (!ty || value_is_error(vm, ty) ||
        !value_is_type(ty, TYPE_KIND_TYPE)) {
      char tn[64];
      op_type_name(expr, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "cast target is not a type (operand is %s)", tn);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    const type_t *target = value_as(ty, const type_t *);
    value_t *result = value_explicit_cast(sema->vm, expr, target);
    if (value_is_error(sema->vm, result)) {
      char tn[64], tt[64];
      op_type_name(expr, tn, sizeof(tn));
      sema_type_name(target, tt, sizeof(tt));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "cannot cast %s to %s", tn, tt);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    return result;
  }

  /* extends：类型兼容判断（编译期类型计算，ctfe 求值后折叠为常量）。
     两侧都是类型表达式（类型即表达式），ctfe 在 vm->comptime 模式下真实
     求值（scope 中的类型值为真实 type value，非 shadow），value_extends
     分派 vtable->extends → bool。成功后把二元节点折叠为 AST_BOOL_LIT
     写回（保留兄弟链），下游编译器零感知——纯编译期，无运行时指令。 */
  if (token_is(b->op, "extends")) {
    vm_t *vm = sema->vm;
    bool saved = vm->comptime;
    vm->comptime = true;
    ctfe_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.vm = vm;
    ctx.sema = sema;
    ctx.budget = 100000;
    ctx.max_depth = 128;
    value_t *r = ctfe_eval(&ctx, *node);
    vm->comptime = saved;
    if (!r || value_is_error(vm, r)) {
      const char *msg = NULL;
      if (r) {
        error_data_t *ed = (error_data_t *)value_data(r);
        msg = ed && ed->message ? string_cstr(ed->message) : NULL;
      }
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends: not a compile-time type computation%s%s",
                 msg ? ": " : "", msg ? msg : "");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    if (value_type(r) != vm->type_bool) {
      char tn[64];
      sema_type_name(value_type(r), tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends must yield bool (got %s)", tn);
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    sema_ct_const_t ct;
    if (!sema_ct_encode(sema, r, &ct)) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends result cannot be folded");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    ast_node_t *lit = sema_ct_lit(sema, *node, &ct);
    if (!lit) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "extends result cannot be folded");
      return value_make_shadow(sema->vm, sema->vm->type_void);
    }
    lit->next = (*node)->next; /* 保留兄弟链 */
    *node = lit;
    return value_make_shadow(sema->vm, vm->type_bool);
  }

  /* 短路 && / ||：操作数必须 bool，结果 bool */
  if (token_is(b->op, "&&") || token_is(b->op, "||")) {
    value_t *lhs = sema_expr(sema, &b->lhs, scope);
    sema_check_bool(sema, b->lhs, lhs, "logical operator");
    value_t *rhs = sema_expr(sema, &b->rhs, scope);
    sema_check_bool(sema, b->rhs, rhs, "logical operator");
    return value_make_shadow(sema->vm, sema->vm->type_bool);
  }

  /* nil 比较（docs m2-design §12.5）：x==nil / nil==x / x!=nil / nil!=x
     四种判定形式是 ?T 唯一合法比较（?T == ?T 直接比较报错，须先窄化）。
     nil 不是 value：不进入 vtable eq 分派，sema 直接识别符号形态 →
     返回 bool shadow。x 非 ?T 与 nil 比较 → 编译错误（str/func 无空值）。 */
  if ((token_is(b->op, "==") || token_is(b->op, "!=")) &&
      (b->lhs->kind == AST_NIL || b->rhs->kind == AST_NIL)) {
    ast_node_t *other = b->lhs->kind == AST_NIL ? b->rhs : b->lhs;
    if (other->kind != AST_IDENT) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "nil can only be compared with an optional variable");
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    }
    ast_ident_t *oid = (ast_ident_t *)other;
    sema_symbol_t *sym = sema_lookup(scope, oid->name);
    const type_t *ot = sym ? sym->type : NULL;
    if (!ot || ot->kind != TYPE_KIND_OPTION) {
      diag_error(sema->diag, sema_loc(sema, &b->base),
                 "variable '%.*s' is not optional; cannot compare with nil",
                 (int)oid->name.len, oid->name.ptr);
      return value_make_shadow(sema->vm, sema->vm->type_bool);
    }
    return value_make_shadow(sema->vm, sema->vm->type_bool);
  }

  value_t *lhs = sema_expr(sema, &b->lhs, scope);
  value_t *rhs = sema_expr(sema, &b->rhs, scope);
  /* 错误恢复产物（error/void shadow）静默通过，避免级联二次诊断 */
  if (value_is_error(sema->vm, lhs) || value_is_error(sema->vm, rhs) ||
      value_is_type(lhs, TYPE_KIND_VOID) || value_is_type(rhs, TYPE_KIND_VOID))
    return value_make_shadow(sema->vm, sema->vm->type_void);
  value_t *(*op)(vm_t *, value_t *, value_t *) = binop_of(b->op);
  if (!op) {
    char ob[16];
    op_text(b->op, ob, sizeof(ob));
    diag_error(sema->diag, sema_loc(sema, &b->base),
               "unsupported binary operator '%s'", ob);
    return value_make_shadow(sema->vm, sema->vm->type_void);
  }
  value_t *result = op(sema->vm, lhs, rhs);
  if (value_is_error(sema->vm, result)) {
    char ob[16], ln[64], rn[64];
    op_text(b->op, ob, sizeof(ob));
    op_type_name(lhs, ln, sizeof(ln));
    op_type_name(rhs, rn, sizeof(rn));
    diag_error(sema->diag, sema_loc(sema, &b->base),
               "type mismatch: cannot apply '%s' to %s and %s", ob, ln, rn);
    return value_make_shadow(sema->vm, sema->vm->type_void);
  }
  return result; /* shadow in → shadow out */
}
