#include "compiler/compiler.h"
#include "parser/ast_block.h"
#include "parser/ast_func_def.h"
#include "parser/ast_var_def.h"

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
 * 回填注册段 PUSH_FUNCTION 的 body 占位。
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

  /* 函数体语句（顶层变量定义到新作用域） */
  if (fn->body && fn->body->kind == AST_BLOCK) {
    ast_block_t *b = (ast_block_t *)fn->body;
    for (ast_node_t *s = b->stmts; s; s = s->next) compile_stmt(c, s);
  }

  balance_pop(c);

  /* return 兜底：无显式 return 时压 undefined + RET */
  bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
  bcode_write_op(c->bc, BCODE_RET);
  return body;
}

/** 编译函数注册段（签名构造 + PUSH_FUNCTION + BIND_FUNC + SET_FUNC_NAME
 *  + push_undefined + DEFINE）
 *  返回 PUSH_FUNCTION body 操作数字段位置（opcode+4，emit_jump 同款取样
 *  时机）。函数体在产物最后（HALT 之后），入口 pc 编译时未知，此处先写
 *  占位 0，compile.c 编译函数体区后按返回的槽位回填真实入口。 */
size_t compile_func_reg(compiler_t *c, ast_func_def_t *fn) {
  /* 函数 id：compiler 按声明顺序分配（不写回 AST），仅 BIND_FUNC 携带 */
  uint32_t fid = c->func_id_next++;

  /* 签名弹栈顺序：[return, param1..argc, is_variadic] */
  /* 1. PUSH_FUNC_TYPE：分配空 func type，压其 type value（构造起点） */
  bcode_write_op(c->bc, BCODE_PUSH_FUNC_TYPE);
  st_push(c, 1);

  /* 2. 参数类型（按声明顺序）：每个参数编译出 type value，FUNC_TYPE_PARAM 追加 */
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *vd = (ast_var_def_t *)p;
    compile_type_expr(c, vd->type_expr);
    bcode_write_op(c->bc, BCODE_FUNC_TYPE_PARAM);
    st_push(c, 0);  /* 弹参数 type value，追加到 func type（func type 仍在栈） */
  }

  /* 3. 返回类型：void 兜底用 PUSH "void"，否则编译返回类型表达式 */
  if (!fn->return_expr) {
    bcode_write_op(c->bc, BCODE_PUSH);
    bcode_write_str(c->bc, STRSLICE_LIT("void"));
    st_push(c, 1);
  } else {
    compile_type_expr(c, fn->return_expr);
  }
  bcode_write_op(c->bc, BCODE_FUNC_TYPE_RETURN);
  st_push(c, -1);  /* 弹返回 type value，设为返回类型 */

  /* M1 无用户变参函数，省略 FUNC_TYPE_VARARG（func type 默认 is_variadic=false） */

  /* 4. 密封 func type（按签名去重 intern，标记 sealed） */
  bcode_write_op(c->bc, BCODE_SEAL);
  st_push(c, 0);  /* 栈顶 func type 不变（已 sealed） */

  bcode_write_op(c->bc, BCODE_PUSH_FUNCTION);
  size_t slot = bcode_tell(c->bc); /* 操作数字段（write_op 之后取样） */
  bcode_write_u32(c->bc, 0);       /* body 占位，函数体编译后回填 */
  st_push(c, 0);  /* 弹 func type，压 func value */

  /* 5. BIND_FUNC <id>：填充 fn->id + 登记 id→func 到 functions_by_id
     （peek 不弹栈——DEFINE 需保留 func value 在栈上）。id 单一来源，
     PUSH_FUNCTION 不再携带。 */
  bcode_write_op(c->bc, BCODE_BIND_FUNC);
  bcode_write_u32(c->bc, fid);

  /* 6. SET_FUNC_NAME "name"：写入函数显示名（peek 不弹栈）。
     仅命名函数定义写入；匿名函数表达式（var add = func(){}）不写——
     该形式 M1 暂不支持（AST_FUNC_LIT 未实现），无分支。 */
  bcode_write_op(c->bc, BCODE_SET_FUNC_NAME);
  bcode_write_str(c->bc, fn->name);

  bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
  bcode_write_op(c->bc, BCODE_DEFINE);
  bcode_write_str(c->bc, fn->name);
  st_push(c, -1);

  return slot;
}
