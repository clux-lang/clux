#include "compiler/compiler.h"
#include "parser/ast_assign.h"
#include "parser/ast_ident.h"
#include "parser/ast_index.h"
#include "parser/ast_block.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_for.h"
#include "parser/ast_if.h"
#include "parser/ast_return.h"
#include "parser/ast_type_def.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"

/* ===========================================================================
 * 语句节点
 *
 * 语句编译：语句产生的栈上值由语句自身平衡（压栈 → 使用 → 清理）。
 * 块作用域经 PUSH_SCOPE/POP_SCOPE 成对平衡（scope_depth 静态追踪）；
 * 跳转跨出 N 层块时先发 N 个 POP_SCOPE（balance_scopes_out）。
 * =========================================================================== */

/* ---- 下标左值赋值编译：a[i] = v / a[i] op= v ---- */

/* 栈深静态平衡：compile_expr 内部 st_push 累积 + 指令弹压。
   简单赋值：object+index+value 压 3 → INDEX_SET 弹 3 压 1 → POP 弹 1。
   复合赋值：object+index 压 2 → PUSH_VALUE dup self/index 压 2（保留引用）
             → GET 弹 2 压 1 → value 压 1 → op 弹 2 压 1
             → SET 弹 3 压 1 → POP 弹 1。
   复合赋值用 PUSH_VALUE dup 保留 self/index 引用，避免双求值 base 表达式，
   且栈序保持 [self, index, val]（val 在顶）满足 INDEX_SET 协议。 */
static void compile_assign_index(compiler_t *c, ast_assign_t *n) {
  ast_index_t *ix = (ast_index_t *)n->target;

  if (token_is(n->op, "=")) {
    compile_expr(c, ix->object);    /* 栈: [self] */
    compile_expr(c, ix->indices);   /* 栈: [self, index] */
    compile_expr(c, n->value);      /* 栈: [self, index, val] */
    bcode_write_op(c->bc, BCODE_INDEX_SET); /* 弹 3 压 1（self） */
    st_push(c, -2);
    bcode_write_op(c->bc, BCODE_POP);       /* 赋值是语句：丢弃结果 */
    st_push(c, -1);
    return;
  }

  /* 复合赋值 a[i] op= v：保留 self/index → GET → v → op → SET */
  compile_expr(c, ix->object);      /* 栈: [self] */
  compile_expr(c, ix->indices);     /* 栈: [self, index] */
  bcode_write_op(c->bc, BCODE_PUSH_VALUE);
  bcode_write_u32(c->bc, 1);        /* dup self（peek 1） */
  st_push(c, 1);                    /* 栈: [self, index, self] */
  bcode_write_op(c->bc, BCODE_PUSH_VALUE);
  bcode_write_u32(c->bc, 1);        /* dup index（peek 1） */
  st_push(c, 1);                    /* 栈: [self, index, self, index] */
  bcode_write_op(c->bc, BCODE_INDEX_GET);   /* 弹 2 压 1 → [self, index, old] */
  st_push(c, -1);
  compile_expr(c, n->value);        /* 栈: [self, index, old, v] */
  if (token_is(n->op, "+="))      bcode_write_op(c->bc, BCODE_ADD);
  else if (token_is(n->op, "-=")) bcode_write_op(c->bc, BCODE_SUB);
  else if (token_is(n->op, "*=")) bcode_write_op(c->bc, BCODE_MUL);
  else if (token_is(n->op, "/=")) bcode_write_op(c->bc, BCODE_DIV);
  else if (token_is(n->op, "%=")) bcode_write_op(c->bc, BCODE_MOD);
  else { c_error(c, &n->base, "unsupported compound assignment"); return; }
  st_push(c, -1);                   /* 弹 2 压 1 → [self, index, new] */
  bcode_write_op(c->bc, BCODE_INDEX_SET);   /* 弹 3 压 1 → [self] */
  st_push(c, -2);
  bcode_write_op(c->bc, BCODE_POP);         /* 语句丢弃 */
  st_push(c, -1);
}

/* 块体编译：入口提升局部 type 定义（未来局部函数定义语句在此接入）——
   先按声明序发 type def 字节码（LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE，
   类型构造在全局 hoist 区，此处仅运行时名字绑定），定义点跳过。
   类型名字整个块内可见（前向引用安全），与 sema walk_block 提升一致。
   balance（PUSH_SCOPE）由调用方在调用前发出，DEFINE 落到块作用域。
   函数体块（compile_func_body）与各控制流块（compile_stmt）共用。 */
void compile_block_body(compiler_t *c, ast_block_t *b) {
  for (ast_node_t *s = b->stmts; s; s = s->next)
    if (s->kind == AST_TYPE_DEF) compile_stmt(c, s);
  for (ast_node_t *s = b->stmts; s; s = s->next)
    if (s->kind != AST_TYPE_DEF) compile_stmt(c, s);
}

void compile_stmt(compiler_t *c, ast_node_t *node) {
  if (!node || c->failed) return;
  switch (node->kind) {
  case AST_VAR_DEF: {
    ast_var_def_t *n = (ast_var_def_t *)node;
    if (n->init) {
      compile_expr(c, n->init);              /* 栈: [value] */
    } else {
      bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED); /* 无初始值 → 零值占位（sema 已保证未初始化不可读） */
      st_push(c, 1);
    }
    compile_type_expr(c, n->type_expr); /* 栈: [value, type-spec]；空类型压 PUSH_UNDEFINED（从值推断） */
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, n->name);
    /* DEFINE 永远双弹弹掉全部，栈深归零 */
    st_push(c, -2);
    break;
  }
  case AST_TYPE_DEF: {
    /* type name = <type-expr>;：定义 type value 变量。
       与 var def 同构（[value, type-spec] DEFINE 协议）：先压 rhs 求值出的
       type value（sema 已折叠为 AST_TYPE_REF → LOAD_TYPE；内建别名保持
       AST_IDENT → PUSH 从作用域查），再压 PUSH_UNDEFINED 作 spec 占位
       （DEFINE 弹 spec=undefined → 从 init 推断 decl_type=type_type），
       运行时 name 成为 type value 变量（类型 type_type，值=类型指针）。
       即用户指定序列：LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE "name"。 */
    ast_type_def_t *n = (ast_type_def_t *)node;
    compile_expr(c, n->expr);                  /* 栈: [type_value] */
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED); /* 栈: [type_value, spec占位] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, n->name);
    /* DEFINE 永远双弹弹掉全部，栈深归零 */
    st_push(c, -2);
    break;
  }
  case AST_ASSIGN: {
    ast_assign_t *n = (ast_assign_t *)node;
    /* 下标左值赋值：a[i] = v / a[i] op= v（sema 已校验下标合法）。
       简单赋值 → INDEX_SET；复合赋值 → INDEX_GET → op → INDEX_SET。
       注：复合赋值双求值 base 表达式（读一次写一次），M2 文档注明
       base 须无副作用；后续 Phase 以 value_xxx 封装 + 临时变量字节码
       消除（见 m2-design 索引段）。 */
    if (n->target->kind == AST_INDEX) {
      compile_assign_index(c, n);
      break;
    }
    /* 左值标识符名（目前仅支持 AST_IDENT，由 parser/sema 保证） */
    strslice_t name = ((ast_ident_t *)n->target)->name;
    if (token_is(n->op, "=") && strslice_eq(name, STRSLICE_LIT("_"))) {
      /* 显式丢弃：_ = expr → 只求值右值并 POP（不 STORE，_ 不是变量）。
         sema 已校验 op 必须是 '='。 */
      compile_expr(c, n->value);               /* 栈: [value] */
      bcode_write_op(c->bc, BCODE_POP);        /* 丢弃结果 */
      st_push(c, -1);
      break;
    }
    if (token_is(n->op, "=")) {
      /* 直接赋值：value → STORE name */
      compile_expr(c, n->value);               /* 栈: [value] */
      bcode_write_op(c->bc, BCODE_STORE);
      bcode_write_str(c->bc, name);            /* STORE 压回结果，栈: [result] */
      bcode_write_op(c->bc, BCODE_POP);        /* 赋值是语句：丢弃结果 */
      st_push(c, -1);
    } else {
      /* 复合赋值 name op= v → PUSH name; v; op; STORE name（C 语义：
         x += v 等价于 x = x + v；栈序 [old, v] 保证 BINARY_OP
         弹 b=v、弹 a=old → value_fn(old, v)） */
      bcode_write_op(c->bc, BCODE_PUSH);
      bcode_write_str(c->bc, name);           /* 栈: [old] */
      compile_expr(c, n->value);             /* 栈: [old, v] */
      if (token_is(n->op, "+="))      bcode_write_op(c->bc, BCODE_ADD);
      else if (token_is(n->op, "-=")) bcode_write_op(c->bc, BCODE_SUB);
      else if (token_is(n->op, "*=")) bcode_write_op(c->bc, BCODE_MUL);
      else if (token_is(n->op, "/=")) bcode_write_op(c->bc, BCODE_DIV);
      else if (token_is(n->op, "%=")) bcode_write_op(c->bc, BCODE_MOD);
      else { c_error(c, node, "unsupported compound assignment"); return; }
      /* 栈: [result] */
      bcode_write_op(c->bc, BCODE_STORE);
      bcode_write_str(c->bc, name);           /* 压回结果，栈: [result] */
      bcode_write_op(c->bc, BCODE_POP);      /* 赋值是语句：丢弃结果 */
      st_push(c, -2);
    }
    break;
  }
  case AST_EXPR_STMT: {
    ast_expr_stmt_t *n = (ast_expr_stmt_t *)node;
    compile_expr(c, n->expr);                /* 栈: [value] */
    bcode_write_op(c->bc, BCODE_POP);        /* 丢弃结果 */
    st_push(c, -1);
    break;
  }
  case AST_RETURN: {
    ast_return_t *n = (ast_return_t *)node;
    if (n->value) {
      compile_expr(c, n->value);             /* 栈: [retval] */
    } else {
      bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED); /* void return */
      st_push(c, 1);
    }
    bcode_write_op(c->bc, BCODE_RET);        /* 栈顶即返回值 */
    st_push(c, -1);                          /* 返回值被 RET 消费 */
    break;
  }
  case AST_BLOCK: {
    ast_block_t *n = (ast_block_t *)node;
    balance_push(c);
    compile_block_body(c, n);
    balance_pop(c);
    break;
  }
  case AST_IF: {
    ast_if_t *n = (ast_if_t *)node;
    compile_expr(c, n->cond);                /* 栈: [cond] */
    compile_label_t else_l, end_l;
    label_init(&else_l);
    label_init(&end_l);
    bcode_write_op(c->bc, BCODE_JZ);
    emit_jump(c, &else_l);                   /* 栈: [] */

    /* then */
    if (n->then_body->kind == AST_BLOCK) {
      balance_push(c);
      ast_block_t *b = (ast_block_t *)n->then_body;
      compile_block_body(c, b);
      balance_pop(c);
    } else {
      compile_stmt(c, n->then_body);
    }
    if (n->else_body) {
      bcode_write_op(c->bc, BCODE_JMP);
      emit_jump(c, &end_l);
    }
    label_here(c, &else_l);

    /* else */
    if (n->else_body) {
      if (n->else_body->kind == AST_BLOCK) {
        balance_push(c);
        ast_block_t *b = (ast_block_t *)n->else_body;
        compile_block_body(c, b);
        balance_pop(c);
      } else {
        compile_stmt(c, n->else_body);
      }
      label_here(c, &end_l);
    }
    break;
  }
  case AST_WHILE: {
    ast_while_t *n = (ast_while_t *)node;

    compile_label_t loop_top, loop_end;
    label_init(&loop_top);
    label_init(&loop_end);

    /* 循环上下文（break→end，continue→top）。label 存指针：
       循环体编译期间 emit_jump 直接读写同一 label（后向跳转已 defined
       时直接写目标，前向跳转共享 patches 列表统一回填） */
    compile_loop_t lc;
    lc.break_label = &loop_end;
    lc.continue_label = &loop_top;
    lc.scope_depth = c->scope_depth;
    lc.next = c->loop_stack;
    c->loop_stack = &lc;

    label_here(c, &loop_top);                /* 循环顶：条件 */
    compile_expr(c, n->cond);                /* 栈: [cond] */
    bcode_write_op(c->bc, BCODE_JZ);
    emit_jump(c, &loop_end);                 /* 栈: [] */

    balance_push(c);                         /* 循环体块作用域 */
    ast_block_t *b = (ast_block_t *)n->body;
    compile_block_body(c, b);
    balance_pop(c);

    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, &loop_top);
    label_here(c, &loop_end);

    c->loop_stack = lc.next;
    break;
  }
  case AST_FOR: {
    ast_for_t *n = (ast_for_t *)node;

    balance_push(c);                         /* for 作用域（init 变量） */
    if (n->init) compile_stmt(c, n->init);

    compile_label_t loop_top, loop_end, loop_cont;
    label_init(&loop_top);
    label_init(&loop_end);
    label_init(&loop_cont);

    compile_loop_t lc;
    lc.break_label = &loop_end;
    lc.continue_label = &loop_cont;          /* for 的 continue → update 段 */
    lc.scope_depth = c->scope_depth;
    lc.next = c->loop_stack;
    c->loop_stack = &lc;

    label_here(c, &loop_top);                /* 循环顶：条件 */
    if (n->cond) {
      compile_expr(c, n->cond);              /* 栈: [cond] */
      bcode_write_op(c->bc, BCODE_JZ);
      emit_jump(c, &loop_end);               /* 栈: [] */
    }

    /* 循环体块作用域 */
    balance_push(c);
    ast_block_t *b = (ast_block_t *)n->body;
    compile_block_body(c, b);
    balance_pop(c);

    label_here(c, &loop_cont);               /* continue 目标：update 段 */
    if (n->update) {
      if (n->update->kind == AST_ASSIGN || n->update->kind == AST_EXPR_STMT) {
        compile_stmt(c, n->update);
      } else {
        compile_expr(c, n->update);
        bcode_write_op(c->bc, BCODE_POP);
        st_push(c, -1);
      }
    }
    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, &loop_top);
    label_here(c, &loop_end);

    c->loop_stack = lc.next;
    balance_pop(c);                          /* 退出 for 作用域 */
    break;
  }
  case AST_BREAK: {
    if (!c->loop_stack) { c_error(c, node, "break outside loop"); return; }
    /* 跳出到循环出口：先平衡当前块作用域到循环基准深度 */
    size_t out = c->scope_depth - c->loop_stack->scope_depth;
    balance_scopes_out(c, out);
    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, c->loop_stack->break_label);
    break;
  }
  case AST_CONTINUE: {
    if (!c->loop_stack) { c_error(c, node, "continue outside loop"); return; }
    size_t out = c->scope_depth - c->loop_stack->scope_depth;
    balance_scopes_out(c, out);
    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, c->loop_stack->continue_label);
    break;
  }
  case AST_ERROR:
    c_error(c, node, "compile aborted on parse error node");
    return;
  default:
    c_error(c, node, "unsupported statement node '%s'", ast_kind_name(node->kind));
    return;
  }
}
