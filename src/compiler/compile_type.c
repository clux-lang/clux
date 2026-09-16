#include "compiler/compiler.h"
#include "parser/ast_array.h"
#include "parser/ast_const.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_volatile.h"

/* ===========================================================================
 * 类型表达式编译（类型即表达式，m2-design 关键架构决策 6）
 *
 * 类型槽位 = 普通表达式节点。命名类型（AST_IDENT）→ BCODE_PUSH 从当前
 * 作用域链查 type value 压栈（内建类型注册在 global scope，自定义类型注册
 * 到定义点当前作用域，遮蔽语义与变量同机制）；AST_CONST/AST_VOLATILE →
 * 递归编译 sub 后应用 BCODE_CREATE_CONST/VOLATILE。
 * 空类型（type_expr == NULL）→ PUSH_UNDEFINED（"待推导"占位，op_define 从值推断）。
 * =========================================================================== */

void compile_type_expr(compiler_t *c, ast_node_t *type_expr) {
  if (!type_expr) {
    /* 空类型：PUSH_UNDEFINED（"待推导"占位，op_define 从值推断）。
       限定符无意义（无基础类型可修饰），sema 已保证限定符不带空类型。 */
    bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
    st_push(c, 1);
    return;
  }

  if (type_expr->kind == AST_IDENT) {
    /* 类型名引用：编译期可解析（内建/全局 type def 在编译期 vm 作用域）
       → LOAD_TYPE <id> 直接加载真实 type_t（别名透明）；否则 PUSH 从当前
       作用域链查 type value 压栈（局部 type def 在函数体运行时绑定）。
       遮蔽语义与变量同机制：type_lookup 命中非 type value → NULL。 */
    ast_ident_t *id = (ast_ident_t *)type_expr;
    const type_t *t = type_lookup(c->vm, id->name);
    if (t) {
      bcode_write_op(c->bc, BCODE_LOAD_TYPE);
      bcode_write_u32(c->bc, t->id);
      st_push(c, 1);
      return;
    }
    bcode_write_op(c->bc, BCODE_PUSH);
    bcode_write_str(c->bc, id->name);
    st_push(c, 1);
    return;
  }

  if (type_expr->kind == AST_TYPE_REF) {
    /* sema 登记的具名类型引用（__type_N）→ 查登记表拿 program id →
       LOAD_TYPE <id> 查表压栈。类型构造收敛到 hoist 提升区。
       内建类型引用（名字 = 规范名 "i32"，不登记）→ type_lookup 兜底 →
       LOAD_TYPE <内建 id>（别名透明，m2-design 关键决策 6）。 */
    ast_type_ref_t *ref = (ast_type_ref_t *)type_expr;
    const sema_type_t *st = c_sema_type_find_name(c->sema_types, ref->name);
    if (!st) {
      const type_t *bt = type_lookup(c->vm, ref->name);
      if (!bt) {
        c_error(c, type_expr, "unknown type reference '%.*s'",
                (int)ref->name.len, ref->name.ptr);
        return;
      }
      bcode_write_op(c->bc, BCODE_LOAD_TYPE);
      bcode_write_u32(c->bc, bt->id);
      st_push(c, 1);
      return;
    }
    bcode_write_op(c->bc, BCODE_LOAD_TYPE);
    bcode_write_u32(c->bc, st->id);
    st_push(c, 1);
    return;
  }

  if (type_expr->kind == AST_CONST) {
    /* const 修饰：递归编译 sub（类型表达式），再应用 const 构造。
       固定组合顺序 volatile(const(T))：先 const 后 volatile，与
       resolve_type_expr 一致。弹一压一，栈深不变。 */
    compile_type_expr(c, ((ast_const_t *)type_expr)->sub);
    bcode_write_op(c->bc, BCODE_CREATE_CONST);
    return;
  }

  if (type_expr->kind == AST_VOLATILE) {
    compile_type_expr(c, ((ast_volatile_t *)type_expr)->sub);
    bcode_write_op(c->bc, BCODE_CREATE_VOLATILE);
    return;
  }

  if (type_expr->kind == AST_ARRAY) {
    /* [N]T 数组类型：PUSH_ARRAY 压开放 array type value → 元素类型
       （递归 compile_type_expr）→ DEFINE_BOUND N 一次性设元素类型+边界
       （弹元素类型）→ SEAL <id> 密封+登记（消费栈）→ LOAD_TYPE <id>
       拉回类型值（保持"类型表达式压类型值"契约，调用方如 CONSTRUCT
       类型位仍得 +1）。边界 N 已被 sema 折叠为 AST_INT_LIT（编译期常量）。
       注：常规路径该分支不可达（sema_resolve_type_slot 已把复合类型槽位
       替换为 AST_TYPE_REF → LOAD_TYPE），此分支仅防御未替换场景；id 由
       compiler 临时分配（type_id_next），与 hoist 区同类型可成多 id 别名
       （types_by_id 幂等），语义无害。 */
    ast_array_t *arr = (ast_array_t *)type_expr;

    bcode_write_op(c->bc, BCODE_PUSH_ARRAY);   /* 栈: [open_array_type] */
    st_push(c, 1);

    compile_type_expr(c, arr->base_type);      /* 栈: [open, elem_type] */
    st_push(c, 1);

    /* 边界立即数（sema 已保证为字面量常量） */
    uint64_t len = 0;
    if (arr->length && arr->length->kind == AST_INT_LIT) {
      len = ((ast_int_lit_t *)arr->length)->value;
    } else {
      c_error(c, arr->length ? arr->length : type_expr,
              "array length must be a compile-time constant");
      return;
    }
    if (len > 0xFFFFFFFFull) {
      c_error(c, arr->length, "array length %llu out of range",
              (unsigned long long)len);
      return;
    }
    bcode_write_op(c->bc, BCODE_DEFINE_BOUND); /* 弹 elem_type → 设进 open */
    bcode_write_u32(c->bc, (uint32_t)len);
    st_push(c, -1);

    uint32_t tid = c->type_id_next++;
    bcode_write_op(c->bc, BCODE_SEAL);         /* 弹 array type → 密封+登记 */
    bcode_write_u32(c->bc, tid);
    st_push(c, -1);

    bcode_write_op(c->bc, BCODE_LOAD_TYPE);    /* 拉回类型值（契约：压 +1） */
    bcode_write_u32(c->bc, tid);
    st_push(c, 1);
    return;
  }

  /* M2 扩展点：元组/func 类型表达式（sema 已保证可达此处时合法） */
  bcode_write_op(c->bc, BCODE_PUSH_UNDEFINED);
  st_push(c, 1);
}
