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

/** 编译函数注册段（LOAD_TYPE 拉取 hoist 提升的签名类型 + PUSH_FUNCTION +
 *  BIND_FUNC + SET_FUNC_NAME + push_undefined + DEFINE）
 *  返回 PUSH_FUNCTION body 操作数字段位置（opcode+4，emit_jump 同款取样
 *  时机）。函数体在产物最后（HALT 之后），入口 pc 编译时未知，此处先写
 *  占位 0，compile.c 编译函数体区后按返回的槽位回填真实入口。
 *
 *  函数类型（func type，签名）与函数变量（func value）分离：
 *  - 签名类型：已由 hoist 类型提升区**两遍扫描**构造（pass 1 PUSH_FUNC_TYPE
 *    → DEFINE_TYPE <id> 声明登记进 types_by_id；pass 2 LOAD_TYPE 拉回 →
 *    设参数/返回 → SEAL 封闭，依赖后序——签名可引用签名，函数指针作参数
 *    时依赖后声明签名）。sema 已把签名类型登记进 sema->types 并写入
 *    sig->id，此处 LOAD_TYPE <sig->id> 直接拉取密封签名类型，不再内联构造。
 *  - 函数变量：LOAD_TYPE <sig_id> 主动拉取签名类型 → PUSH_FUNCTION 构造
 *    func value → BIND_FUNC <fid> 填充函数 id（functions_by_id 表，fid 由
 *    func_id_next 分配）→ DEFINE 名字绑定到 scope。
 *  sig_id（类型 id，sema 分配）与 fid（函数 id，compiler 分配）分属两张
 *  独立表，值域可重叠但互不串用。 */
size_t compile_func_reg(compiler_t *c, ast_func_def_t *fn) {
  /* 函数 id：compiler 按声明顺序分配（不写回 AST），仅 BIND_FUNC 携带 */
  uint32_t fid = c->func_id_next++;

  /* 签名类型：从 sema 登记的全局函数符号取（sym->type 即签名类型，id 由
     sema_type_register 写入 sig->id）。hoist 已构造密封，LOAD_TYPE 拉回。 */
  const sema_symbol_t *sym = sema_lookup(c->global_scope, fn->name);
  const type_t *sig = sym ? sym->type : NULL;
  if (!sig || sig->kind != TYPE_KIND_FUNC) {
    c_error(c, (const ast_node_t *)fn,
            "internal: missing signature type for function '%.*s'",
            (int)fn->name.len, fn->name.ptr);
    return 0;
  }
  uint32_t sig_id = sig->id;

  /* 1. LOAD_TYPE <sig_id>：主动从类型表拉取密封签名类型 */
  bcode_write_op(c->bc, BCODE_LOAD_TYPE);
  bcode_write_u32(c->bc, sig_id);
  st_push(c, 1);

  bcode_write_op(c->bc, BCODE_PUSH_FUNCTION);
  size_t slot = bcode_tell(c->bc); /* 操作数字段（write_op 之后取样） */
  bcode_write_u32(c->bc, 0);       /* body 占位，函数体编译后回填 */
  st_push(c, 0);  /* 弹签名类型，压 func value */

  /* 2. BIND_FUNC <id>：填充 fn->id + 登记 id→func 到 functions_by_id
     （peek 不弹栈——DEFINE 需保留 func value 在栈上）。id 单一来源，
     PUSH_FUNCTION 不再携带。 */
  bcode_write_op(c->bc, BCODE_BIND_FUNC);
  bcode_write_u32(c->bc, fid);

  /* 3. SET_FUNC_NAME "name"：写入函数显示名（peek 不弹栈）。
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
