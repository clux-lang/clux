#include "compiler/compiler.h"
#include "parser/ast_assign.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_ident.h"
#include "parser/ast_scope_ref.h" /* M6 FFI：extern var 赋值目标（is_ffi_var 契约） */
#include "parser/ast_index.h"
#include "parser/ast_member.h"
#include "parser/ast_block.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_for.h"
#include "parser/ast_for_index.h"
#include "parser/ast_if.h"
#include "parser/ast_nil.h"
#include "parser/ast_free.h"
#include "parser/ast_return.h"
#include "parser/ast_switch.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_deref.h"
#include "parser/lexer.h"

#include <stdio.h>

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

  if (!n->op || token_is(n->op, "=")) {   /* op==NULL：sema 生成赋值 */
    compile_expr(c, ix->object);    /* 栈: [self] */
    compile_expr(c, ix->indices);   /* 栈: [self, index] */
    compile_expr(c, n->value);      /* 栈: [self, index, val] */
    /* 赋值点旧值销毁（sema 生成；INDEX_SET 是纯覆盖，不释放旧元素） */
    for (ast_node_t *d = n->destroy; d; d = d->next) compile_stmt(c, d);
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

/* ---- 解引用左值赋值编译：r.* = v / r.* op= v（m3-design §8.2）----
   PTR_GET/PTR_SET 严格分离：解引用写（SET）由本分支承载。
   简单赋值：指针+value 压 2 → PTR_SET 弹 2 压 1（指针）→ POP。
   复合赋值：指针压 1 → PUSH_VALUE dup self（保留引用，避免双求值）
             → PTR_GET 弹 1 压 1（被指向 T 值副本）→ value 压 1
             → op 弹 2 压 1 → PTR_SET 弹 2 压 1 → POP。
   与 compile_assign_member 的 FIELD_GET/op/FIELD_SET 模式同构。 */
/* ---- M6 FFI：`<库>::<数据符号>` = v（extern var 的写入）----
 *
 * EXTERN_STORE 弹 1 压 0（写进库里的变量地址，无结果值），因此：
 *   简单赋值：value → EXTERN_STORE                     栈净 -1（赋值是语句）
 *   复合赋值：EXTERN_LOAD → value → op → EXTERN_STORE  同样是净 -1
 * 与 compile_assign_deref 的 PTR_GET/op/PTR_SET 模式同构（差别只在目标不是
 * 求值出来的指针，而是指令里的限定名——地址由运行期 dlsym 得到）。 */
static void compile_assign_extern_var(compiler_t *c, ast_assign_t *n) {
  ast_scope_ref_t *t = (ast_scope_ref_t *)n->target;
  if (!t->is_ffi_var || !t->type_expr || t->type_expr->kind != AST_IDENT) {
    c_error(c, &n->base, "malformed extern var assignment target");
    return;
  }
  ast_ident_t *lib_id = (ast_ident_t *)t->type_expr;
  char qual[512];
  int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s", (int)lib_id->name.len,
                    lib_id->name.ptr, (int)t->variant.len, t->variant.ptr);
  if (qn <= 0 || (size_t)qn >= sizeof(qual)) {
    c_error(c, &n->base, "malformed extern var assignment target");
    return;
  }
  strslice_t q = strslice_from_bytes(qual, (size_t)qn);

  if (!n->op || token_is(n->op, "=")) {   /* op==NULL：sema 生成赋值 */
    /* TYPED（struct/cunion/enum）：先压类型值，再压值 → 指令弹 2 写 size 字节；
       非 TYPED：只压值 → 指令弹 1。两者最终都净 -1（赋值是语句）。 */
    if (t->ffi_var_kind == BCODE_FFI_DATA_TYPED)
      compile_type_expr(c, t->ffi_var_type_expr);
    compile_expr(c, n->value);
    bcode_write_op(c->bc, BCODE_EXTERN_STORE);
    bcode_write_str(c->bc, q);
    bcode_write_u32(c->bc, t->ffi_var_kind);
    st_push(c, -1);                           /* 弹 1 压 0（TYPED 同样净 -1） */
    return;
  }

  /* 复合赋值：先读旧值（EXTERN_LOAD +1），再算，再写回 */
  bcode_write_op(c->bc, BCODE_EXTERN_LOAD);
  bcode_write_str(c->bc, q);
  bcode_write_u32(c->bc, t->ffi_var_kind);
  st_push(c, 1);                              /* 栈: [old] */
  compile_expr(c, n->value);                  /* 栈: [old, v] */
  if (token_is(n->op, "+="))      bcode_write_op(c->bc, BCODE_ADD);
  else if (token_is(n->op, "-=")) bcode_write_op(c->bc, BCODE_SUB);
  else if (token_is(n->op, "*=")) bcode_write_op(c->bc, BCODE_MUL);
  else if (token_is(n->op, "/=")) bcode_write_op(c->bc, BCODE_DIV);
  else if (token_is(n->op, "%=")) bcode_write_op(c->bc, BCODE_MOD);
  else { c_error(c, &n->base, "unsupported compound assignment"); return; }
  st_push(c, -1);                             /* 弹 2 压 1 → [new] */
  bcode_write_op(c->bc, BCODE_EXTERN_STORE);
  bcode_write_str(c->bc, q);
  bcode_write_u32(c->bc, t->ffi_var_kind);
  st_push(c, -1);                             /* 弹 1 压 0 */
}

/* ---- struct 静态属性赋值：`T::x = v` / `T::x op= v` ----
 * sema 已校验成员存在性（含嵌入链继承）+ comptime var const 拦截 +
 * 置 is_struct_attr + struct_attr_type_id。
 *
 * 简单赋值：value → STORE_ATTR <type_id> <name>（净 -1，赋值是语句）
 * 复合赋值：LOAD_ATTR <type_id> <name> → value → op → STORE_ATTR（净 -1） */
static void compile_assign_struct_attr(compiler_t *c, ast_assign_t *n) {
  ast_scope_ref_t *t = (ast_scope_ref_t *)n->target;

  if (!n->op || token_is(n->op, "=")) {   /* op==NULL：sema 生成赋值 */
    compile_expr(c, n->value);               /* 栈: [value] */
    bcode_write_op(c->bc, BCODE_STORE_ATTR);
    bcode_write_u32(c->bc, t->struct_attr_type_id);
    bcode_write_str(c->bc, t->variant);
    st_push(c, -1);                           /* 弹 1 压 0 */
    return;
  }

  /* 复合赋值：先读旧值（LOAD_ATTR +1），再算，再写回 */
  bcode_write_op(c->bc, BCODE_LOAD_ATTR);
  bcode_write_u32(c->bc, t->struct_attr_type_id);
  bcode_write_str(c->bc, t->variant);
  st_push(c, 1);                              /* 栈: [old] */
  compile_expr(c, n->value);                  /* 栈: [old, v] */
  if (token_is(n->op, "+="))      bcode_write_op(c->bc, BCODE_ADD);
  else if (token_is(n->op, "-=")) bcode_write_op(c->bc, BCODE_SUB);
  else if (token_is(n->op, "*=")) bcode_write_op(c->bc, BCODE_MUL);
  else if (token_is(n->op, "/=")) bcode_write_op(c->bc, BCODE_DIV);
  else if (token_is(n->op, "%=")) bcode_write_op(c->bc, BCODE_MOD);
  else { c_error(c, &n->base, "unsupported compound assignment"); return; }
  st_push(c, -1);                             /* 弹 2 压 1 → [new] */
  bcode_write_op(c->bc, BCODE_STORE_ATTR);
  bcode_write_u32(c->bc, t->struct_attr_type_id);
  bcode_write_str(c->bc, t->variant);
  st_push(c, -1);                             /* 弹 1 压 0 */
}

static void compile_assign_deref(compiler_t *c, ast_assign_t *n) {
  ast_deref_t *d = (ast_deref_t *)n->target;

  if (!n->op || token_is(n->op, "=")) {   /* op==NULL：sema 生成赋值 */
    compile_expr(c, d->operand);             /* 栈: [ptr] */
    compile_expr(c, n->value);               /* 栈: [ptr, val] */
    /* 赋值点旧值销毁（sema 生成；PTR_SET 是纯覆盖，不释放旧被指向内容的
       own 槽位） */
    for (ast_node_t *ds = n->destroy; ds; ds = ds->next) compile_stmt(c, ds);
    bcode_write_op(c->bc, BCODE_PTR_SET);    /* 弹 2 压 1（ptr） */
    st_push(c, -1);
    bcode_write_op(c->bc, BCODE_POP);        /* 赋值是语句：丢弃结果 */
    st_push(c, -1);
    return;
  }

  /* 复合赋值 r.* op= v：保留 ptr → PTR_GET → v → op → PTR_SET */
  compile_expr(c, d->operand);               /* 栈: [ptr] */
  bcode_write_op(c->bc, BCODE_PUSH_VALUE);
  bcode_write_u32(c->bc, 0);                 /* dup ptr（peek 0=栈顶） */
  st_push(c, 1);                             /* 栈: [ptr, ptr] */
  bcode_write_op(c->bc, BCODE_PTR_GET);      /* 弹 1 压 1 → [ptr, old] */
  st_push(c, 0);
  compile_expr(c, n->value);                 /* 栈: [ptr, old, v] */
  if (token_is(n->op, "+="))      bcode_write_op(c->bc, BCODE_ADD);
  else if (token_is(n->op, "-=")) bcode_write_op(c->bc, BCODE_SUB);
  else if (token_is(n->op, "*=")) bcode_write_op(c->bc, BCODE_MUL);
  else if (token_is(n->op, "/=")) bcode_write_op(c->bc, BCODE_DIV);
  else if (token_is(n->op, "%=")) bcode_write_op(c->bc, BCODE_MOD);
  else { c_error(c, &n->base, "unsupported compound assignment"); return; }
  st_push(c, -1);                            /* 弹 2 压 1 → [ptr, new] */
  bcode_write_op(c->bc, BCODE_PTR_SET);      /* 弹 2 压 1 → [ptr] */
  st_push(c, -1);
  bcode_write_op(c->bc, BCODE_POP);          /* 语句丢弃 */
  st_push(c, -1);
}

/* ---- 字段左值赋值编译：p.field = v / p.field op= v ----
 * 简单赋值：object+value 压 2 → FIELD_SET 弹 2 压 1（self）→ POP。
 * 复合赋值：object 压 1 → PUSH_VALUE dup self（保留引用，避免双求值）
 *           → FIELD_GET 弹 1 压 1 → value 压 1 → op 弹 2 压 1
 *           → FIELD_SET 弹 2 压 1 → POP。
 * 嵌套 p.a.b：object 是 AST_MEMBER → compile_expr 递归 FIELD_GET，
 * 借用引用偏移链正确；FIELD_SET 直接写回原 struct 值。
 * nil 赋值（p.field = nil）：sema 已改写 value 为匿名构造 .{nil}
 * （AST_CONSTRUCT，走 option 分支 → PUSH_OPT_NONE），此处零特判。 */
static void compile_assign_member(compiler_t *c, ast_assign_t *n) {
  ast_member_t *m = (ast_member_t *)n->target;

  if (!n->op || token_is(n->op, "=")) {   /* op==NULL：sema 生成赋值 */
    compile_expr(c, m->object);              /* 栈: [self] */
    compile_expr(c, n->value);               /* 栈: [self, val] */
    bcode_write_op(c->bc, BCODE_FIELD_SET);  /* 弹 2 压 1（self） */
    bcode_write_str(c->bc, m->field);
    st_push(c, -1);
    bcode_write_op(c->bc, BCODE_POP);        /* 赋值是语句：丢弃结果 */
    st_push(c, -1);
    return;
  }

  /* 复合赋值 p.field op= v：保留 self → GET → v → op → SET。
     PUSH_VALUE 0 dup 栈顶（[self] 时 peek 0 即 self；index 场景
     栈上有 [self,index] 才用 peek 1 取次顶）。 */
  compile_expr(c, m->object);                /* 栈: [self] */
  bcode_write_op(c->bc, BCODE_PUSH_VALUE);
  bcode_write_u32(c->bc, 0);                 /* dup self（peek 0=栈顶） */
  st_push(c, 1);                             /* 栈: [self, self] */
  bcode_write_op(c->bc, BCODE_FIELD_GET);
  bcode_write_str(c->bc, m->field);          /* 弹 1 压 1 → [self, old] */
  st_push(c, 0);
  compile_expr(c, n->value);                 /* 栈: [self, old, v] */
  if (token_is(n->op, "+="))      bcode_write_op(c->bc, BCODE_ADD);
  else if (token_is(n->op, "-=")) bcode_write_op(c->bc, BCODE_SUB);
  else if (token_is(n->op, "*=")) bcode_write_op(c->bc, BCODE_MUL);
  else if (token_is(n->op, "/=")) bcode_write_op(c->bc, BCODE_DIV);
  else if (token_is(n->op, "%=")) bcode_write_op(c->bc, BCODE_MOD);
  else { c_error(c, &n->base, "unsupported compound assignment"); return; }
  st_push(c, -1);                            /* 弹 2 压 1 → [self, new] */
  bcode_write_op(c->bc, BCODE_FIELD_SET);    /* 弹 2 压 1 → [self] */
  bcode_write_str(c->bc, m->field);
  st_push(c, -1);
  bcode_write_op(c->bc, BCODE_POP);          /* 语句丢弃 */
  st_push(c, -1);
}

/* 块体编译：入口提升局部 type 定义与局部函数定义——
   先按声明序发 type def 字节码（LOAD_TYPE <id>; PUSH_UNDEFINED; DEFINE，
   类型构造在全局 hoist 区，此处仅运行时名字绑定），再按声明序提升局部
   函数名字绑定（compile_func_bind_instance：MAKE_FUNCTION <fid>;
   PUSH_UNDEFINED; DEFINE——运行时构造实例并绑定名字到块作用域，名字整个
   块内可见，前向引用安全，同块互相调用；块入口即实例化，定义点之前引用
   拿到的是同一实例的浅拷贝，无基底/实例双态）。定义点跳过（第三循环仅对
   有捕获的局部函数发捕获绑定，见下）。
   函数体统一由 compiler_compile 函数体区按 funcs_all 队列（预扫描收集）
   编译回填——compile_block_body 不再收集。comptime 局部函数不进入运行时
   （调用点 sema 折叠），跳过。与 sema walk_block 提升严格一致。
   balance（PUSH_SCOPE）由调用方在调用前发出，DEFINE 落到块作用域。
   函数体块（compile_func_body）与各控制流块（compile_stmt）共用。 */
/* struct 成员初始化（var 静态属性 + func 方法值注册到 attributes）。
 * 必须在函数对象提升区（BIND_FUNC）之后执行——func 成员发 LOAD_FUNCTION
 * <fid> 取回 func value 需先经 BIND_FUNC 登记到 functions_by_id。
 * 遍历 members：func → LOAD_FUNCTION + STORE_ATTR；var → init + STORE_ATTR。
 * comptime 成员不进运行时（sema 已在 pass_globals 摘除或折叠）。 */
void compile_struct_member_init(compiler_t *c, ast_struct_def_t *n) {
  for (ast_node_t *m = n->members; m; m = m->next) {
    if (m->kind == AST_FUNC_DEF) {
      ast_func_def_t *fn = (ast_func_def_t *)m;
      if (fn->is_comptime) continue;        /* comptime func 不进运行时 */
      if (fn->is_extern_cfunc) continue;    /* extern cfunc 无体，不注册 */
      bcode_write_op(c->bc, BCODE_LOAD_FUNCTION);
      bcode_write_u32(c->bc, fn->fid);
      st_push(c, 1);
      bcode_write_op(c->bc, BCODE_STORE_ATTR);
      bcode_write_u32(c->bc, n->type_id);
      bcode_write_str(c->bc, fn->name);
      st_push(c, -1);
    } else if (m->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)m;
      if (vd->is_comptime) continue;        /* comptime var 不进运行时 */
      if (vd->init && vd->init->kind == AST_NIL) {
        /* nil 初始化（var x: ?T = nil）：PUSH_OPT_NONE <id> 压 none 值 */
        emit_push_opt_none(c, vd->type_expr);
      } else if (vd->init) {
        compile_expr(c, vd->init);
      } else {
        bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
        st_push(c, 1);
      }
      bcode_write_op(c->bc, BCODE_STORE_ATTR);
      bcode_write_u32(c->bc, n->type_id);
      bcode_write_str(c->bc, vd->name);
      st_push(c, -1);
    }
  }
}

/* 前向声明：compile_block_body 遍历时对 return/break/continue 调用 */
static void compile_exit_with_defers(compiler_t *c, ast_node_t *node);

void compile_block_body(compiler_t *c, ast_block_t *b) {
  for (ast_node_t *s = b->stmts; s; s = s->next)
    if (s->kind == AST_TYPE_DEF || s->kind == AST_ENUM_DEF ||
        s->kind == AST_STRUCT_DEF || s->kind == AST_UNION_DEF ||
        s->kind == AST_CUNION_DEF)
      compile_stmt(c, s);
  for (ast_node_t *s = b->stmts; s; s = s->next) {
    if (s->kind != AST_FUNC_DEF) continue;
    ast_func_def_t *fn = (ast_func_def_t *)s;
    if (fn->is_comptime) continue;
    compile_func_bind_instance(c, fn);
    if (c->failed) return;
  }
  /* 第三循环：按声明序编译语句；AST_FUNC_DEF 定义点发捕获绑定
     （compile_func_capture_bind，keep=false：PUSH name 取块入口实例 +
     每捕获 SET_CLOSURE + POP，净 0）。捕获延迟到定义点：此时被捕获变量
     已定义（sema 保证捕获符号已激活），SET_CLOSURE 把当前值 clone 进实例
     的 closure_scope。无捕获函数跳过（块入口实例无需再绑定任何捕获）。
     循环内每次迭代块入口都重新 MAKE_FUNCTION，每轮新实例，捕获互不干扰。 */
  for (ast_node_t *s = b->stmts; s; s = s->next) {
    if (s->kind == AST_TYPE_DEF || s->kind == AST_ENUM_DEF ||
        s->kind == AST_STRUCT_DEF || s->kind == AST_UNION_DEF ||
        s->kind == AST_CUNION_DEF)
      continue;
    if (s->kind == AST_FUNC_DEF) {
      ast_func_def_t *fn = (ast_func_def_t *)s;
      if (!fn->is_comptime && fn->captures) {
        compile_func_capture_bind(c, fn, false);
        if (c->failed) return;
      }
      continue;
    }
    /* return/break/continue：退出点 defer calls 挂载在节点 defers 字段，
       compiler 先求值返回值/准备跳转，再编译 defer calls，最后发 RET/JMP。
       不在语句链中插入 defer calls，块尾正常退出路径单独追加。 */
    if (s->kind == AST_RETURN || s->kind == AST_BREAK ||
        s->kind == AST_CONTINUE) {
      compile_exit_with_defers(c, s);
      if (c->failed) return;
      continue;
    }
    compile_stmt(c, s);
  }
}

/* return/break/continue 退出点 + 挂载的 defer calls 编译。
   sema desugar_defers 把沿作用域链收集的 defer call 语句挂载到退出点
   节点的 defers 字段。compiler 先编译退出点的值/跳转准备，然后编译
   defers 链中的 call 语句，最后发实际的 RET/JMP——保证：
   - return 值在 defer 之前求值（压栈后 defer 执行，最后 RET 弹出返回值）
   - defer 在实际跳转之前执行（break/continue 先跑 defer 再 POP+JMP）
   - 不引入临时变量，避免 fatal/ref/own 语义被 var 定义改变 */
static void compile_exit_with_defers(compiler_t *c, ast_node_t *node) {
  /* **语句前置序列**：退出语句（return/break/continue）同样可能有 prelude
     （sema 生成的扁平化 clone 切片路径 + 生成的 clone 函数体：函数体只有一条
     `return <clone 表达式>`，切片的临时变量/循环全挂在它的 prelude 上）。
     compile_block_body 对退出语句走本函数而不是 compile_stmt，因此这里必须
     补上 prelude 编译，否则 prelude 被静默丢弃 → 运行期 "undefined variable"。 */
  for (ast_node_t *pre = node->prelude; pre; pre = pre->next) {
    compile_stmt(c, pre);
    if (c->failed) return;
  }
  /* return：先编译返回值压栈 */
  if (node->kind == AST_RETURN) {
    ast_return_t *n = (ast_return_t *)node;
    if (n->value) {
      compile_expr(c, n->value);             /* 栈: [retval] */
    } else {
      bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED); /* void return */
      st_push(c, 1);
    }
    /* 编译挂载的 defer calls */
    for (ast_node_t *d = node->defers; d; d = d->next) {
      compile_stmt(c, d);
      if (c->failed) return;
    }
    bcode_write_op(c->bc, BCODE_RET);        /* 栈顶即返回值 */
    st_push(c, -1);                          /* 返回值被 RET 消费 */
    return;
  }
  /* break/continue：先编译 defer calls，再 balance_scopes_out + JMP */
  for (ast_node_t *d = node->defers; d; d = d->next) {
    compile_stmt(c, d);
    if (c->failed) return;
  }
  if (node->kind == AST_BREAK) {
    if (!c->loop_stack) { c_error(c, node, "break outside loop"); return; }
    size_t out = c->scope_depth - c->loop_stack->scope_depth;
    balance_scopes_out(c, out);
    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, c->loop_stack->break_label);
  } else { /* AST_CONTINUE */
    if (!c->loop_stack) { c_error(c, node, "continue outside loop"); return; }
    size_t out = c->scope_depth - c->loop_stack->scope_depth;
    balance_scopes_out(c, out);
    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, c->loop_stack->continue_label);
  }
}

void compile_stmt(compiler_t *c, ast_node_t *node) {
  if (!node || c->failed) return;
  /* **语句前置序列**（sema 生成的扁平化 clone 切片路径）：临时变量定义 +
     make + 逐元素循环，必须在语句主体之前、**同一作用域**编译。 */
  for (ast_node_t *pre = node->prelude; pre; pre = pre->next)
    compile_stmt(c, pre);
  switch (node->kind) {
  case AST_VAR_DEF: {
    ast_var_def_t *n = (ast_var_def_t *)node;
    if (n->init && n->init->kind == AST_NIL) {
      /* nil 初始化（var x:?T = nil；sema 已保证 type_expr 非空且为 ?T）：
         PUSH_OPT_NONE <id> 压 none 值块，DEFINE 绑定。 */
      emit_push_opt_none(c, n->type_expr);     /* 栈: [value] */
    } else if (n->init) {
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
  case AST_ENUM_DEF: {
    /* enum Name:Underlying { ... } 名字绑定（顶层 enum 定义）：
       与 type def 同构——enum 类型在 hoist 区构造（pass 2 SEAL 完成），
       此处 LOAD_TYPE <enum_id>（sema 登记的 type_id 写回节点）→
       PUSH_UNDEFINED → DEFINE "Name" 绑定 type value 到作用域
       （运行时 type_lookup 解析 `var c: Color`）。 */
    ast_enum_def_t *n = (ast_enum_def_t *)node;
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);
    bcode_write_u32(c->bc, n->type_id);        /* 栈: [type_value] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED); /* 栈: [type_value, spec占位] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, n->name);
    /* DEFINE 永远双弹弹掉全部，栈深归零 */
    st_push(c, -2);
    break;
  }
  case AST_STRUCT_DEF: {
    /* struct Name { ... } 名字绑定（顶层 struct 定义）：
       与 enum def 同构——struct 类型在 hoist 区构造（pass 2 SEAL 完成），
       此处 LOAD_TYPE <struct_id>（sema 登记的 type_id 写回节点）→
       PUSH_UNDEFINED → DEFINE "Name" 绑定 type value 到作用域
       （运行时 type_lookup 解析 `var p: Point`）。
       成员初始化（var 静态属性 + func 方法值注册到 attributes）推迟到
       函数对象提升区之后——func 成员发 LOAD_FUNCTION <fid> 取回 func value
       需先经 BIND_FUNC 登记，而 BIND_FUNC 在函数对象提升区执行。 */
    ast_struct_def_t *n = (ast_struct_def_t *)node;
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);
    bcode_write_u32(c->bc, n->type_id);        /* 栈: [type_value] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED); /* 栈: [type_value, spec占位] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, n->name);
    /* DEFINE 永远双弹弹掉全部，栈深归零 */
    st_push(c, -2);
    break;
  }
  case AST_UNION_DEF: {
    /* union Name { i: i32; f: f32; ... } 名字绑定（顶层 union 定义）：
       与 struct def 同构——union 类型在 hoist 区构造（pass 2 SEAL 完成，
       UNION_MEMBER×N + DEFINE_FIELD 设 member payload 类型），此处
       LOAD_TYPE <union_id>（sema 登记的 type_id 写回节点）→
       PUSH_UNDEFINED → DEFINE "Name" 绑定 type value 到作用域
       （运行时 type_lookup 解析 `var s: Value`）。 */
    ast_union_def_t *n = (ast_union_def_t *)node;
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);
    bcode_write_u32(c->bc, n->type_id);        /* 栈: [type_value] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED); /* 栈: [type_value, spec占位] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, n->name);
    /* DEFINE 永远双弹弹掉全部，栈深归零 */
    st_push(c, -2);
    break;
  }
  case AST_CUNION_DEF: {
    /* cunion Name { i: i32; f: f32; ... } 名字绑定（顶层 cunion 定义）：
       与 union def 同构——cunion 类型在 hoist 区构造（pass 2 SEAL 完成，
       DEFINE_FIELD xN 追加 member 名 + 设 member 类型；无 UNION_MEMBER——
       cunion 无 tag），此处 LOAD_TYPE <cunion_id>（sema 登记的 type_id
       写回节点）-> PUSH_UNDEFINED -> DEFINE "Name" 绑定 type value 到作用域
       （运行时 type_lookup 解析 var v: U）。 */
    ast_cunion_def_t *n = (ast_cunion_def_t *)node;
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);
    bcode_write_u32(c->bc, n->type_id);        /* 栈: [type_value] */
    st_push(c, 1);
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
    if (n->target->kind == AST_MEMBER) {
      compile_assign_member(c, n);
      break;
    }
    if (n->target->kind == AST_DEREF) {
      /* 解引用左值赋值 r.* = v / r.* op= v（m3-design §8.2）：
         PTR_GET/PTR_SET 严格分离，SET 由本分支承载。 */
      compile_assign_deref(c, n);
      break;
    }
    if (n->target->kind == AST_SCOPE_REF) {
      ast_scope_ref_t *er = (ast_scope_ref_t *)n->target;
      if (er->is_struct_attr) {
        /* struct 静态属性赋值 `T::x = v`：sema 已校验成员存在性 +
           置 is_struct_attr + struct_attr_type_id。发 STORE_ATTR。 */
        compile_assign_struct_attr(c, n);
        break;
      }
      /* M6 FFI：`<库>::<数据符号>` = v / op= v（extern var 的写入）。
         sema 已在目标节点上置 is_ffi_var + ffi_var_kind。 */
      compile_assign_extern_var(c, n);
      break;
    }
    /* 左值标识符名（目前仅支持 AST_IDENT，由 parser/sema 保证） */
    strslice_t name = ((ast_ident_t *)n->target)->name;
    if (token_is(n->op, "=") && strslice_eq(name, STRSLICE_LIT("_"))) {
      /* 显式销毁 / 丢弃：`_ = expr`（sema 已按 discard_mode 分类，见
         ast_assign.h）。_ 不是变量：不 STORE。
           mode 2：sema 已把销毁序列放进语句 prelude（编译 expr + 逐槽位
                   FREE），语句本身不发指令；
           mode 1：借用/无资源 → 求值后 POP（**不得 DISPOSE**：借用不拥有
                   堆块，释放会破坏借用源）；
           其它  ：回退 DISPOSE（递归释放值内嵌 own 堆块）。 */
      if (n->discard_mode == 2) break;
      compile_expr(c, n->value);               /* 栈: [value] */
      if (n->discard_mode == 1) {
        bcode_write_op(c->bc, BCODE_POP);
      } else {
        bcode_write_op(c->bc, BCODE_DISPOSE);
      }
      st_push(c, -1);
      break;
    }
    if (!n->op || token_is(n->op, "=")) {   /* op==NULL：sema 生成赋值 */
      if (n->value->kind == AST_NIL) {
        /* nil 赋值（a = nil；sema 已校验 a 为 ?T）：STORE_NIL <name> 只置
           ok=false，压回 dst；POP 丢弃（赋值是语句）。STORE_NIL +1、
           POP -1，净 0。 */
        bcode_write_op(c->bc, BCODE_STORE_NIL);
        bcode_write_str(c->bc, name);          /* 压回 dst，栈: [dst] */
        bcode_write_op(c->bc, BCODE_POP);      /* 赋值是语句：丢弃结果 */
        break;
      }
      /* 直接赋值：value → STORE name */
      compile_expr(c, n->value);               /* 栈: [value] */
      /* 赋值点旧值销毁（sema 生成）：右值已求值（可能读过旧值），写之前
         释放旧堆块。FREE 链净栈深 0，不影响 [value]。 */
      for (ast_node_t *d = n->destroy; d; d = d->next) compile_stmt(c, d);
      bcode_write_op(c->bc, n->reinit ? BCODE_STORE_REINIT : BCODE_STORE);
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
  case AST_EMPTY_STMT:
    break; /* 空语句：无操作 */
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
  case AST_SWITCH: {
    /* switch（docs m2-design §4：if 语法糖，语义内联）：
       desugar 等价于 `var __switch_N = cond; if (...) {} else if ...`
       1. cond 求值一次 → DEFINE 临时变量 __switch_N（[value, spec] 协议）
       2. 每分支：PUSH __switch_N + 模式 + EQ + JNZ 分支体（模式间短接：
          任一模式命中即入体，惰性）
       3. 全模式未命中 → JMP 下一分支；default 兜底；无 fallthrough
          （分支体后 JMP end_l） */
    ast_switch_t *n = (ast_switch_t *)node;

    compile_expr(c, n->cond);                        /* 栈: [cond] */
    char tmp_name[32];
    snprintf(tmp_name, sizeof(tmp_name), "__switch_%u", c->switch_seq++);
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);     /* 栈: [cond, spec] */
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, strslice_from_cstr(tmp_name)); /* 栈: [] */
    st_push(c, -2);

    compile_label_t end_l;
    label_init(&end_l);
    for (ast_node_t *cs = n->cases; cs; cs = cs->next) {
      ast_switch_case_t *sc = (ast_switch_case_t *)cs;
      compile_label_t case_body_l, next_l;
      label_init(&case_body_l);
      label_init(&next_l);

      /* 模式比较链：PUSH tmp + pat + EQ + JNZ case_body（任一命中即入体） */
      for (ast_node_t *pt = sc->patterns; pt; pt = pt->next) {
        bcode_write_op(c->bc, BCODE_PUSH);
        bcode_write_str(c->bc, strslice_from_cstr(tmp_name));
        st_push(c, 1);                               /* 栈: [tmp] */
        compile_expr(c, pt);                         /* 栈: [tmp, pat] */
        bcode_write_op(c->bc, BCODE_EQ);             /* 栈: [bool] */
        st_push(c, -1);
        bcode_write_op(c->bc, BCODE_JNZ);
        emit_jump(c, &case_body_l);                  /* 弹 bool，命中跳 */
      }
      bcode_write_op(c->bc, BCODE_JMP);
      emit_jump(c, &next_l);                         /* 全模式未命中 → 下一分支 */

      label_here(c, &case_body_l);
      if (sc->body->kind == AST_BLOCK) {
        balance_push(c);
        compile_block_body(c, (ast_block_t *)sc->body);
        balance_pop(c);
      } else {
        compile_stmt(c, sc->body);
      }
      if (cs->next || n->default_body) {
        bcode_write_op(c->bc, BCODE_JMP);            /* 无 fallthrough */
        emit_jump(c, &end_l);
      }
      label_here(c, &next_l);
    }

    /* default 兜底 */
    if (n->default_body) {
      if (n->default_body->kind == AST_BLOCK) {
        balance_push(c);
        compile_block_body(c, (ast_block_t *)n->default_body);
        balance_pop(c);
      } else {
        compile_stmt(c, n->default_body);
      }
    }
    label_here(c, &end_l);
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
  case AST_DOWHILE: {
    ast_dowhile_t *n = (ast_dowhile_t *)node;

    compile_label_t loop_top, loop_cont, loop_end;
    label_init(&loop_top);
    label_init(&loop_cont);
    label_init(&loop_end);

    /* 循环上下文（break→end，continue→loop_cont）。continue 跳转到条件
       判断前（loop_cont 定位在条件求值处）；loop_top 定位在循环体入口，
       底部 JMP 跳回 loop_cont（判条件）而非 loop_top，保证每次迭代先判
       条件后进体。 */
    compile_loop_t lc;
    lc.break_label = &loop_end;
    lc.continue_label = &loop_cont;
    lc.scope_depth = c->scope_depth;
    lc.next = c->loop_stack;
    c->loop_stack = &lc;

    label_here(c, &loop_top);                /* 循环体入口（先执行体） */

    balance_push(c);                         /* 循环体块作用域 */
    ast_block_t *b = (ast_block_t *)n->body;
    compile_block_body(c, b);
    balance_pop(c);

    label_here(c, &loop_cont);               /* continue 目标 + 循环底：判条件 */
    compile_expr(c, n->cond);                /* 栈: [cond] */
    bcode_write_op(c->bc, BCODE_JNZ);
    emit_jump(c, &loop_top);                 /* 条件真 → 回循环体入口；假 → 落到 end */
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
  case AST_FOR_INDEX: {
    /* sema 生成的逐元素循环（销毁序列用）：`for (i=0; i<len(src); ++i) body`。
       计数器是**编译器私有**的 u64 变量（名字由 sema 分配、块内唯一）——
       与 LENGTH 的返回类型一致（u64），避免 i64/u64 混合比较触发
       "not a widening conversion"。body 里以 `src[i]` 访问当前元素。 */
    ast_for_index_t *n = (ast_for_index_t *)node;
    if (!n->var.ptr || !n->source || !n->body) {
      c_error(c, node, "compiler: for_index requires var/source/body");
      return;
    }
    balance_push(c);                          /* 循环变量作用域 */
    bcode_write_op(c->bc, BCODE_PUSH_U64);    /* 栈: [0] */
    bcode_write_u64(c->bc, 0);
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);   /* 栈: [0, u64 type] */
    bcode_write_u32(c->bc, c->vm->type_u64->id);
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_DEFINE);
    bcode_write_str(c->bc, n->var);
    st_push(c, -2);

    compile_label_t loop_top, loop_end, loop_cont;
    label_init(&loop_top);
    label_init(&loop_end);
    label_init(&loop_cont);

    compile_loop_t lc;
    lc.break_label = &loop_end;
    lc.continue_label = &loop_cont;
    lc.scope_depth = c->scope_depth;
    lc.next = c->loop_stack;
    c->loop_stack = &lc;

    label_here(c, &loop_top);
    bcode_write_op(c->bc, BCODE_PUSH);        /* 栈: [i] */
    bcode_write_str(c->bc, n->var);
    st_push(c, 1);
    compile_expr(c, n->source);               /* 栈: [i, src] */
    bcode_write_op(c->bc, BCODE_LENGTH);      /* 栈: [i, len] （u64） */
    bcode_write_op(c->bc, BCODE_LT);          /* 栈: [i < len] */
    bcode_write_op(c->bc, BCODE_JZ);
    emit_jump(c, &loop_end);                  /* 栈: [] */

    compile_stmt(c, n->body);                 /* 元素销毁序列 */

    label_here(c, &loop_cont);
    bcode_write_op(c->bc, BCODE_PUSH);        /* 栈: [i] */
    bcode_write_str(c->bc, n->var);
    st_push(c, 1);
    bcode_write_op(c->bc, BCODE_PUSH_U64);
    bcode_write_u64(c->bc, 1);
    bcode_write_op(c->bc, BCODE_ADD);         /* 栈: [i + 1] */
    bcode_write_op(c->bc, BCODE_STORE);
    bcode_write_str(c->bc, n->var);           /* 栈: [i'] */
    bcode_write_op(c->bc, BCODE_POP);         /* 栈: [] */
    bcode_write_op(c->bc, BCODE_JMP);
    emit_jump(c, &loop_top);
    label_here(c, &loop_end);

    c->loop_stack = lc.next;
    balance_pop(c);
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
  case AST_FUNC_DEF:
    /* 局部函数定义：compile_block_body 入口提升已处理（注册段字节码），
       定义点跳过。 */
    break;
  /* AST_DEFER 已在 sema Pass 3a desugar_defers 中改写为 AST_FUNC_DEF +
     退出点 call（AST_EXPR_STMT），到达 compiler 时已不存在 AST_DEFER。 */
  case AST_ERROR:
    c_error(c, node, "compile aborted on parse error node");
    return;
  case AST_FREE:
    /* `FREE <expr>` 本质是**表达式**（结果 void），语句位置直接复用表达式
       编译路径。sema 也可能把它包进 AST_EXPR_STMT，两种形态都合法。
       操作数是兄弟链（组合式释放 = 多于一条），逐条编译。 */
    for (ast_node_t *op = ((ast_free_t *)node)->operand; op; op = op->next) {
      ast_free_t one = *(const ast_free_t *)node;
      one.base.next = NULL;
      one.operand   = op;
      compile_expr(c, &one.base);          /* 编译一条 FREE，净 0 栈 */
      if (c->failed) return;
    }
    break;
  default:
    c_error(c, node, "unsupported statement node '%s'", ast_kind_name(node->kind));
    return;
  }
}
