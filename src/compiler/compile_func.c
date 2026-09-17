#include "compiler/compiler.h"
#include "core/panic.h"
#include "parser/ast_array.h"
#include "parser/ast_assign.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_call.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_for.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_type.h"
#include "parser/ast_if.h"
#include "parser/ast_index.h"
#include "parser/ast_member.h"
#include "parser/ast_program.h"
#include "parser/ast_return.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_def.h"
#include "parser/ast_unary.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/ast_while.h"

#include <string.h>

/* ===========================================================================
 * 函数节点
 * =========================================================================== */

/**
 * 编译单个函数体：
 *  - 倒序 DEFINE 绑参（参数定义到 func_vcall 推入的匿名局部作用域）
 *  - DEFINE 完成后 PUSH_SCOPE：函数体临时变量/块内变量定义到独立子作用域，
 *    与参数隔离（同名不冲突，且临时变量随函数返回销毁）
 *  - body 语句 → POP_SCOPE → return 兜底
 * 返回函数体入口（entry_pc）；调用方（compile.c）在函数体区编译完成后
 * 回填 hoist 函数注册区 PUSH_FUNCTION 的 body 占位。
 */
size_t compile_func_body(compiler_t *c, ast_func_def_t *fn) {
  size_t body = bcode_tell(c->bc);

  /* 倒序绑定参数：每参数 push_undefined; define name（从值推断，与 var 定义一致） */
  /* 收集参数名（倒序） */
  strslice_t names[64];
  size_t ni = 0;
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    if (ni < sizeof(names) / sizeof(names[0])) names[ni++] = vd->name;
  }
  for (size_t i = ni; i-- > 0; ) {
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, names[i]);
  }

  /* 参数绑定完成 → push 函数体作用域（临时变量与参数隔离，遮蔽语义正确） */
  balance_push(c);

  /* 函数体语句（顶层变量定义到新作用域；compile_block_body 做局部
     type def 入口提升——函数体与块作用域一致，类型名整个函数体可见） */
  if (fn->body && fn->body->kind == AST_BLOCK) {
    compile_block_body(c, (ast_block_t *)fn->body);
  }

  balance_pop(c);

  /* return 兜底：无显式 return 时压 undefined + RET */
  bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
  bcode_write_op(c->bc, BCODE_RET);
  return body;
}

/* 预扫描收集：单个函数（全局/局部/字面量统一）——校验读取 fid（sema 创建
   函数对象时分配，单一来源）、追加 funcs_all 队列（fid 序）。comptime func
   不入队列（不进入运行时）；其 body 由 prescan_func_body 无条件递归——body
   内嵌套函数/字面量会被 comptime 调用点折叠产物（LOAD_FUNCTION <fid>）引用，
   必须收集构造。 */
static void prescan_func(compiler_t *c, ast_func_def_t *fn) {
  if (fn->is_comptime) return;
  if (fn->fid == 0) {
    c_error(c, (const ast_node_t *)fn,
            "internal: missing function id for '%.*s' (sema did not assign fid)",
            (int)fn->name.len, fn->name.ptr);
    return;
  }
  vec_push(c->funcs_all, c->alloc, fn);
}

/* ---- 预扫描递归遍历（收集嵌套函数定义） ---- */

static void prescan_expr(compiler_t *c, ast_node_t *n);
static void prescan_stmt(compiler_t *c, ast_node_t *n);
static void prescan_block(compiler_t *c, ast_block_t *b);

/* 函数体：body（块）+ return_expr（类型表达式，不含函数定义——类型槽位
   是 AST_TYPE_REF/内建名；函数类型 func(...)->T 是 AST_FUNC_TYPE，非定义）。
   函数字面量本身已由调用方处理（prescan_func），此处只递归 body。 */
static void prescan_func_body(compiler_t *c, ast_func_def_t *fn) {
  if (fn->body && fn->body->kind == AST_BLOCK) {
    prescan_block(c, (ast_block_t *)fn->body);
  }
}

static void prescan_stmt(compiler_t *c, ast_node_t *n) {
  if (!n) return;
  switch (n->kind) {
    case AST_VAR_DEF:
      prescan_expr(c, ((ast_var_def_t *)n)->init);
      break;
    case AST_TYPE_DEF:
      prescan_expr(c, ((ast_type_def_t *)n)->expr);
      break;
    case AST_ASSIGN:
      prescan_expr(c, ((ast_assign_t *)n)->target);
      prescan_expr(c, ((ast_assign_t *)n)->value);
      break;
    case AST_IF: {
      ast_if_t *it = (ast_if_t *)n;
      prescan_expr(c, it->cond);
      if (it->then_body) prescan_stmt(c, it->then_body);
      if (it->else_body) prescan_stmt(c, it->else_body);
      break;
    }
    case AST_WHILE:
      prescan_expr(c, ((ast_while_t *)n)->cond);
      prescan_stmt(c, ((ast_while_t *)n)->body);
      break;
    case AST_FOR: {
      ast_for_t *fr = (ast_for_t *)n;
      if (fr->init) prescan_stmt(c, fr->init);
      if (fr->cond) prescan_expr(c, fr->cond);
      if (fr->update) prescan_stmt(c, fr->update);
      prescan_stmt(c, fr->body);
      break;
    }
    case AST_RETURN:
      prescan_expr(c, ((ast_return_t *)n)->value);
      break;
    case AST_BLOCK:
      prescan_block(c, (ast_block_t *)n);
      break;
    case AST_EXPR_STMT:
      prescan_expr(c, ((ast_expr_stmt_t *)n)->expr);
      break;
    case AST_FUNC_DEF: {
      /* 局部函数：收集 + 递归 body（嵌套函数/字面量在其体内） */
      ast_func_def_t *fn = (ast_func_def_t *)n;
      prescan_func(c, fn);
      prescan_func_body(c, fn);
      break;
    }
    default:
      break; /* BREAK/CONTINUE 无子节点 */
  }
}

static void prescan_block(compiler_t *c, ast_block_t *b) {
  if (!b) return;
  for (ast_node_t *s = b->stmts; s; s = s->next) prescan_stmt(c, s);
}

static void prescan_expr(compiler_t *c, ast_node_t *n) {
  if (!n) return;
  switch (n->kind) {
    case AST_BINARY:
      prescan_expr(c, ((ast_binary_t *)n)->lhs);
      prescan_expr(c, ((ast_binary_t *)n)->rhs);
      break;
    case AST_UNARY:
      prescan_expr(c, ((ast_unary_t *)n)->operand);
      break;
    case AST_CALL: {
      ast_call_t *cl = (ast_call_t *)n;
      prescan_expr(c, cl->callee);
      for (ast_node_t *a = cl->args; a; a = a->next) prescan_expr(c, a);
      break;
    }
    case AST_MEMBER:
      prescan_expr(c, ((ast_member_t *)n)->object);
      break;
    case AST_INDEX:
      prescan_expr(c, ((ast_index_t *)n)->object);
      for (ast_node_t *i = ((ast_index_t *)n)->indices; i; i = i->next)
        prescan_expr(c, i);
      break;
    case AST_ARRAY:
      prescan_expr(c, ((ast_array_t *)n)->base_type);
      prescan_expr(c, ((ast_array_t *)n)->length);
      break;
    case AST_CONSTRUCT:
      prescan_expr(c, ((ast_construct_t *)n)->type);
      for (ast_node_t *f = ((ast_construct_t *)n)->fields; f; f = f->next)
        prescan_expr(c, f);
      break;
    case AST_CONST:
      prescan_expr(c, ((ast_const_t *)n)->sub);
      break;
    case AST_VOLATILE:
      prescan_expr(c, ((ast_volatile_t *)n)->sub);
      break;
    case AST_FUNC_TYPE:
      for (ast_node_t *p = ((ast_func_type_t *)n)->params; p; p = p->next)
        prescan_expr(c, p);
      prescan_expr(c, ((ast_func_type_t *)n)->return_type);
      break;
    case AST_TERNARY: {
      ast_ternary_t *t = (ast_ternary_t *)n;
      prescan_expr(c, t->cond);
      prescan_expr(c, t->then_branch);
      prescan_expr(c, t->else_branch);
      break;
    }
    case AST_FUNC_DEF: {
      /* 函数字面量（表达式内函数值）：收集 + 递归 body */
      ast_func_def_t *fn = (ast_func_def_t *)n;
      prescan_func(c, fn);
      prescan_func_body(c, fn);
      break;
    }
    default:
      break; /* 叶子节点（字面量/标识符/引用/类型引用）无函数定义 */
  }
}

/**
 * 预扫描收集全部程序函数定义（全局 + 局部 + 嵌套函数字面量），校验读取
 * sema 分配的 fid——hoist 函数注册区构造与函数体区回填按 funcs_all 队列
 * （fid 序）驱动。fid 单一来源在 sema（创建函数对象即分配）。
 *
 * comptime func 本身不收集（不进入运行时），但其 body **无条件递归**——body
 * 内嵌套函数/函数字面量会被 comptime 调用点折叠产物（LOAD_FUNCTION <fid>）
 * 在运行期引用，必须收集构造（hoist 注册区 + 函数体区）。
 */
void compile_prescan_funcs(compiler_t *c, ast_node_t *program) {
  if (!c || !program || program->kind != AST_PROGRAM) return;
  ast_program_t *prog = (ast_program_t *)program;
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind == AST_FUNC_DEF) {
      ast_func_def_t *fn = (ast_func_def_t *)f;
      prescan_func(c, fn);
      prescan_func_body(c, fn);
    } else {
      prescan_stmt(c, f); /* 全局 type def 等（含其表达式） */
    }
  }
}

/**
 * hoist 函数注册区：构造单个函数对象（签名类型已由 hoist 类型提升区密封）。
 *   LOAD_TYPE <sig_id>   —— 拉取密封签名类型
 *   PUSH_FUNCTION <slot> —— 构造 func value（body 入口占位，函数体区回填）
 *   BIND_FUNC <fid>      —— 填充 fn->id + 登记 functions_by_id（id 单一来源）
 *   [SET_FUNC_NAME name] —— 显示名（仅命名函数；匿名字面量跳过）
 * 返回 PUSH_FUNCTION body 操作数字段位置（opcode+4，emit_jump 同款取样时机）。
 *
 * 此区构造的是"基底"对象——主要作用绑定地址（id + entry_pc + 名字）。
 * 全局函数 captures 恒空（sema 拒绝捕获并摘除），捕获占位循环不执行，基底
 * 即最终实例；局部函数/字面量的捕获占位（PUSH_UNDEFINED + SET_CLOSURE）
 * 复制捕获槽名到基底 closure_scope，定义点 MAKE_FUNCTION 按基底实例化
 * 新实例时以此为模板（strmap_keys 枚举槽名 + undefined 占位）。
 *
 * 不 DEFINE：作用域名字绑定由消费点发出（全局 → compile.c 全局绑定段；
 * 局部 → compile_block_body 定义点；字面量无作用域绑定，只 MAKE_FUNCTION）。
 */
size_t compile_func_reg_hoist(compiler_t *c, ast_func_def_t *fn) {
  uint32_t sig_id = fn->sig_id;
  if (sig_id == 0) {
    c_error(c, (const ast_node_t *)fn,
            "internal: missing signature type for function '%.*s'",
            (int)fn->name.len, fn->name.ptr);
    return 0;
  }

  /* 1. LOAD_TYPE <sig_id>：主动从类型表拉取密封签名类型 */
  bcode_write_op(c->bc, BCODE_LOAD_TYPE);
  bcode_write_u32(c->bc, sig_id);
  st_push(c, 1);

  bcode_write_op(c->bc, BCODE_PUSH_FUNCTION);
  size_t slot = bcode_tell(c->bc); /* 操作数字段（write_op 之后取样） */
  bcode_write_u32(c->bc, 0);       /* body 占位，函数体区编译后回填 */
  st_push(c, 0);  /* 弹签名类型，压 func value */

  /* 2. BIND_FUNC <fid>：填充 fn->id + 登记 id→func 到 functions_by_id
     （peek 不弹栈）。id 单一来源——预扫描分配写回 fn->fid，此处携带。 */
  bcode_write_op(c->bc, BCODE_BIND_FUNC);
  bcode_write_u32(c->bc, fn->fid);

  /* 3. SET_FUNC_NAME "name"：写入函数显示名（peek 不弹栈）。
     仅命名函数写入；匿名字面量（name 空）跳过——实例化的 func value
     无显示名，语义无差异。 */
  if (fn->name.len > 0) {
    bcode_write_op(c->bc, BCODE_SET_FUNC_NAME);
    bcode_write_str(c->bc, fn->name);
  }

  /* 4. 捕获槽占位：每捕获 PUSH_UNDEFINED (+1) + SET_CLOSURE "name"（弹 cap
     → scope_set 定义或替换，净 0）——基底 closure_scope 先以 undefined
     绑定捕获名。定义点 MAKE_FUNCTION 实例化新实例时复制这些槽名（strmap_keys
     枚举 + undefined 占位），SET_CLOSURE 用真实捕获值替换——函数提升后、
     定义点前被调用 → 捕获槽是 undefined，函数体读到 → 引擎级错误（TDZ
     语义）。全局函数 captures 恒空（sema 拒绝捕获），此循环不执行。函数值
     留在栈顶供后续 BIND/名字绑定段使用。 */
  for (ast_node_t *cap = fn->captures; cap; cap = cap->next) {
    ast_var_def_t *cv = (ast_var_def_t *)cap;
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_SET_CLOSURE);
    bcode_write_str(c->bc, cv->name);
    st_push(c, -1);
  }

  return slot;
}

/**
 * 捕获绑定序列（函数定义点）：
 *   MAKE_FUNCTION <fid>            —— 从基底函数实例化新实例（新 closure_scope，
 *                                    捕获槽 undefined 占位，共享 entry_pc）
 *   每捕获：<捕获值>                —— 纯 id → PUSH "name"（当前作用域链查外层
 *                                    变量值，借用引用压栈）；括号 → compile_expr
 *                                    (init)（定义点求值构造临时捕获值）
 *            SET_CLOSURE "name"    —— 弹捕获值 → clone 进新实例 closure_scope
 *   keep=true：函数值留栈顶（函数字面量表达式，结果即函数值，净 +1）
 *   keep=false：STORE name 重定向——局部函数名字绑定（compile_func_bind 第二
 *     循环绑定的是基底对象）改为指向新实例（value_assign 改写 data 指针，净 0）
 * 函数定义每次求值生成独立实例：循环内多次求值各自新对象，捕获互不干扰
 * （hoist 区基底仅作模板，不再被定义点改写）。提升后定义点前调用 → 读到
 * 基底（捕获槽 undefined，TDZ 语义）。
 */
void compile_func_capture_bind(compiler_t *c, ast_func_def_t *fn, bool keep) {
  bcode_write_op(c->bc, BCODE_MAKE_FUNCTION);
  bcode_write_u32(c->bc, fn->fid);
  st_push(c, 1);
  for (ast_node_t *cap = fn->captures; cap; cap = cap->next) {
    ast_var_def_t *cv = (ast_var_def_t *)cap;
    if (cv->init) {
      compile_expr(c, cv->init); /* 括号捕获：定义点求值构造临时值 */
    } else {
      bcode_write_op(c->bc, BCODE_PUSH);
      bcode_write_str(c->bc, cv->name); /* 纯 id：作用域链查外层变量值 */
    }
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_SET_CLOSURE);
    bcode_write_str(c->bc, cv->name);
    st_push(c, -1);
  }
  if (!keep) {
    /* 局部函数语句定义点：名字已在块提升绑定基底对象，STORE 重定向到新实例。
       func value 的 assign 是 data 指针改写（identity 拷贝），名字指向新实例
       后递归/前向调用都解析到当前定义点的实例。STORE 压回结果引用（与
       AST_ASSIGN 同款协议），语句无结果 → POP 丢弃。 */
    bcode_write_op(c->bc, BCODE_STORE);
    bcode_write_str(c->bc, fn->name);
    st_push(c, -1);
    bcode_write_op(c->bc, BCODE_POP);
    st_push(c, -1);
  }
}

/**
 * 函数作用域名字绑定（LOAD_FUNCTION <fid> + DEFINE name）：
 *   LOAD_FUNCTION <fid> —— 从 functions_by_id 压入基底函数值（hoist 区构造）
 *   PUSH_UNDEFINED       —— 类型说明符占位（DEFINE 从值推断 decl_type）
 *   DEFINE name          —— 绑定到当前作用域（全局 → 全局作用域；
 *                          局部 → 定义点块作用域，compile_block_body 提升时调用）
 * 绑定的是基底对象：局部函数的定义点第三循环 MAKE_FUNCTION 生成新实例 +
 * STORE name 重定向名字到新实例；全局函数只定义一次、无捕获，基底即实例。
 * 函数值表达式（字面量）不调用本函数——MAKE_FUNCTION 实例化，无作用域绑定。
 */
void compile_func_bind(compiler_t *c, ast_func_def_t *fn) {
  if (fn->name.len == 0) return; /* 匿名函数无作用域绑定 */
  bcode_write_op(c->bc, BCODE_LOAD_FUNCTION);
  bcode_write_u32(c->bc, fn->fid);
  st_push(c, 1);
  bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
  st_push(c, 1);
  bcode_write_op(c->bc, BCODE_DEFINE);
  bcode_write_str(c->bc, fn->name);
  st_push(c, -2);
}
