#include "exec/stmt.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/call.h"
#include "exec/exec.h"
#include "exec/expr.h"
#include "parser/node.h"
#include "parser/node_decorator.h"
#include "parser/node_enum_decl.h"
#include "parser/node_func_decl.h"
#include "parser/node_interface_decl.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_type.h"
#include "parser/node_struct_decl.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_union.h"
#include <string.h>

/* 单装饰器应用（白皮书 §3.2/§9.1）：普通装饰器是值级包装——装饰器表达式
   求值为 func 值时以当前值为唯一实参调用（[[twice]] var x = 21 落地为
   x = twice(21)；[[add100]] func raw 落地为 raw = add100(raw)）；
   求值为其余值（编译期标记，如 metadata）不改当前值原样返回。
   expr 缺失 / 装饰器求值 error / invoke error 均产出 error。 */
static clux_value_t *apply_decorator(clux_context_t *ctx,
                                     clux_value_t *value,
                                     node_t *decorator,
                                     const location_t *fallback_loc) {
  node_t *decorator_expr =
      decorator ? decorator_node_get_expr(decorator) : NULL;
  if (!decorator_expr) {
    return clux_context_create_error_message(
        ctx,
        decorator ? node_get_location(decorator) : fallback_loc,
        "decorator requires an expression");
  }
  clux_value_t *wrapper = exec_operand(ctx, decorator_expr);
  if (clux_value_is_error(wrapper)) {
    return wrapper;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(wrapper)) != CLUX_TYPE_FUNC) {
    return value;
  }
  /* invoke 走帧协议执行器——先保证已挂（首次调用时懒安装） */
  exec_call_install_executor(ctx);
  return clux_context_invoke(
      ctx, wrapper, 1, &value, node_get_location(decorator));
}

/* 类型标注求值公共体（exec_stmt_var 与块提升 pass 1c 共用）：求值类型
   表达式并要求 type value，返回被描述类型；失败返回 NULL 且 *out_fail
   携带 error（操作数 error 优先传播）。 */
static clux_type_t *stmt_var_type_of(clux_context_t *ctx,
                                     node_t *node_type,
                                     clux_value_t **out_fail) {
  *out_fail = NULL;
  clux_value_t *vtype = exec_operand(ctx, node_type);
  if (clux_value_is_error(vtype)) {
    *out_fail = vtype;
    return NULL;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(vtype)) != CLUX_TYPE_TYPE) {
    *out_fail = clux_context_create_error_message(
        ctx,
        node_get_location(node_type),
        "expected type expression in var declaration");
    return NULL;
  }
  /* exec 已拒绝 shadow，raw 必有真实存储 */
  return *(clux_type_t **)clux_value_get_raw(vtype, 0);
}

clux_value_t *exec_stmt_var(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  node_t *node_type = stmt_var_node_get_type(node);
  node_t *node_name = stmt_var_node_get_name(node);
  node_t *node_value = stmt_var_node_get_value(node);
  /* var x: T = undefined：右值是 undefined 字面量时创建 TDZ 变量——
     分配 T 的未初始化存储并标记（首赋值清除、消费拒绝）。
     类型标注必填：无标注无法确定存储类型。 */
  bool is_undefined =
      node_value && node_get_kind(node_value) == CLUX_NODE_LITERAL_UNDEFINED;
  if (is_undefined && !node_type) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node_value),
        "undefined initializer requires a type annotation");
  }
  clux_value_t *value = NULL;
  if (!is_undefined) {
    value = exec_operand(ctx, node_value);
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  /* 装饰器按源码顺序链式包装（apply_decorator）。TDZ（= undefined）无
     初值可包装，装饰器不适用。 */
  if (value) {
    size_t decorator_count = stmt_var_node_get_decorator_count(node);
    for (size_t idx = 0; idx < decorator_count; idx++) {
      value = apply_decorator(ctx,
                              value,
                              stmt_var_node_get_decorator(node, idx),
                              node_get_location(node));
      if (clux_value_is_error(value)) {
        return value;
      }
    }
  }
  /* 类型标注可省略（var x = 10 按初值自身类型落地）。
     有标注时求值类型表达式——必须产出 type value，
     其余值当 clux_type_t* 解引用是内存错误。 */
  clux_type_t *type = NULL;
  if (node_type) {
    clux_value_t *type_fail = NULL;
    type = stmt_var_type_of(ctx, node_type, &type_fail);
    if (!type) {
      return type_fail;
    }
  }
  char *name = location_get(node_get_location(node_name), allocator);
  if (is_undefined) {
    /* 类型标注已由上方的必填检查保证存在 */
    value = clux_context_create_tdz_value(
        ctx, type, name, node_get_location(node_value));
  } else {
    const location_t *loc = node_get_location(node_value);
    if (type) {
      value = clux_context_safe_cast(ctx, value, type, loc);
      if (clux_value_is_error(value)) {
        allocator_free(allocator, (void **)&name);
        return value;
      }
    }
    value = clux_context_declare_value(ctx, value, name, loc);
  }
  allocator_free(allocator, (void **)&name);
  return value;
}

/* type 别名语句 <decorator>* [export]? type name = type_expr;
   （白皮书 §3.11、§8.1）：求值右侧类型表达式——经 exec_operand 统一
   通道（error 优先传播、shadow 拒绝、TDZ 消费校验均由其内置语义化
   报错），必须产出 type value（否则按 clux_type_t* 解引用是内存错误），
   经 declare_value 以别名注册进作用域。export（run 阶段产出）是编译期
   语义，exec 层不处理；generics/decorators 待泛型实现完成后接入。 */
clux_value_t *exec_stmt_type(clux_context_t *ctx, node_t *node) {
  node_t *name_node = stmt_type_node_get_name(node);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "type statement requires a name");
  }
  node_t *value_node = stmt_type_node_get_value(node);
  if (!value_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "type statement requires a type expression");
  }
  clux_value_t *value = exec_operand(ctx, value_node);
  if (!value || clux_value_is_error(value)) {
    return value;
  }
  if (clux_type_kind_unwrap(clux_value_get_type(value)) != CLUX_TYPE_TYPE) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(value_node),
        "expected type expression in type alias");
  }
  /* 完整性门禁：别名目标必须已定局——指针以不完整类型为 base 合法
     （ptr 构造即 seal），但直接别名后声明类型的两阶段空壳仍是错误。 */
  clux_type_t *target = *(clux_type_t **)clux_value_get_raw(value, 0);
  if (!clux_type_is_seal(target)) {
    const char *target_name = clux_type_get_name(target);
    return clux_context_create_error_message(
        ctx,
        node_get_location(value_node),
        "cannot alias incomplete type '%s'",
        target_name ? target_name : "<unknown>");
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}

/* union/cunion 声明按 KEYWORD 文本判别（与 exec_expr_union_impl 同一
   约定：cunion → C union，其余 → tag union）。 */
static bool stmt_union_decl_is_cunion(clux_context_t *ctx, node_t *decl) {
  node_t *keyword_node = decl ? union_node_get_keyword(decl) : NULL;
  if (!keyword_node) {
    return false;
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *keyword = location_get(node_get_location(keyword_node), allocator);
  bool is_cunion = keyword && strcmp(keyword, "cunion") == 0;
  allocator_free(allocator, (void **)&keyword);
  return is_cunion;
}

/* 块提升 pass（exec 与 sema 分离后 exec 自嵌的静态分析）：块作用域入口
   完成三类提升，为块内代码提供前向引用——
   pass 1a 具名无装饰 union/cunion：预建两阶段空壳并按名注册，成员补全
   留待源码序的声明语句复用同一实例；指针先行以空壳为 base 即合法，
   直接以空壳作值类型/别名目标被 seal 门禁拒绝；
   pass 1b 无装饰 func（两阶段）：phase 1 全部仅定义（签名求值 + 按
   名 define + 挂 AST，捕获初始化推迟），phase 2 在 var TDZ 预注册后
   统一做定义点捕获初始化——前向捕获成立（TDZ 源拒绝保持；相互递归
   被类型判等拒绝——捕获表参与判等致类型无限回归，2026-11-20），
   函数体惰性，任何真实调用点执行时全部声明已在位；
   pass 1c 无装饰 var：TDZ 预注册——有标注按标注类型，无标注开 shadow
   模式求初值表达式取类型（不执行函数体、不落存储、不触发 TDZ 消费
   检查），源码序的定义处转首赋值（exec_stmt_var 的 TDZ 通道）。
   装饰器是值级包装，依赖被装饰目标的求值时序，不参与提升，保持源码
   序执行。任何一步失败立即上抛（快速失败），返回 NULL 表示全部成功。
   匿名/残缺节点不抢源码序的报错，仅跳过。 */
static clux_value_t *exec_stmt_block_hoist(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  size_t count = stmt_block_node_get_stmt_count(node);
  /* pass 1a */
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = stmt_block_node_get_stmt(node, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_UNION ||
        stmt_union_node_get_decorator_count(stmt) != 0) {
      continue;
    }
    node_t *decl = stmt_union_node_get_union_decl(stmt);
    node_t *name_node = decl ? union_node_get_name(decl) : NULL;
    if (!name_node) {
      continue;
    }
    char *name = location_get(node_get_location(name_node), allocator);
    if (!name) {
      return clux_context_create_error_message(
          ctx, node_get_location(name_node), "union hoist requires a name");
    }
    clux_type_t *shell = clux_context_create_union_shell(
        ctx, name, stmt_union_decl_is_cunion(ctx, decl));
    clux_value_t *bound =
        shell ? clux_context_create_type_value(
                    ctx, shell, name, node_get_location(name_node))
              : NULL;
    allocator_free(allocator, (void **)&name);
    if (!shell) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(name_node),
          "union hoist failed to create a type shell");
    }
    if (!bound || clux_value_is_error(bound)) {
      return bound;
    }
  }
  /* pass 1b phase 1：无装饰 func 仅定义（签名求值 + 按名 define + 挂
     AST，捕获初始化推迟）——全部函数名字先就位。 */
  size_t func_count = 0;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = stmt_block_node_get_stmt(node, idx);
    if (stmt && node_get_kind(stmt) == CLUX_NODE_STMT_FUNC &&
        stmt_func_node_get_decorator_count(stmt) == 0 &&
        stmt_func_node_get_func_decl(stmt)) {
      func_count++;
    }
  }
  struct {
    node_t *decl;
    clux_value_t *value;
  } hoisted_funcs[func_count ? func_count : 1];
  size_t hoisted = 0;
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = stmt_block_node_get_stmt(node, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_FUNC ||
        stmt_func_node_get_decorator_count(stmt) != 0) {
      continue;
    }
    node_t *decl = stmt_func_node_get_func_decl(stmt);
    if (!decl) {
      continue;
    }
    clux_value_t *value = exec_func_decl_define(ctx, decl);
    if (!value) {
      return clux_context_create_error_message(
          ctx, node_get_location(stmt), "function hoist produced no value");
    }
    if (clux_value_is_error(value)) {
      return value;
    }
    hoisted_funcs[hoisted].decl = decl;
    hoisted_funcs[hoisted].value = value;
    hoisted++;
  }
  /* pass 1c */
  for (size_t idx = 0; idx < count; idx++) {
    node_t *stmt = stmt_block_node_get_stmt(node, idx);
    if (!stmt || node_get_kind(stmt) != CLUX_NODE_STMT_VAR ||
        stmt_var_node_get_decorator_count(stmt) != 0) {
      continue;
    }
    node_t *name_node = stmt_var_node_get_name(stmt);
    if (!name_node) {
      continue;
    }
    node_t *type_node = stmt_var_node_get_type(stmt);
    node_t *value_node = stmt_var_node_get_value(stmt);
    bool is_undefined =
        value_node && node_get_kind(value_node) == CLUX_NODE_LITERAL_UNDEFINED;
    /* = undefined 无标注：源码序报 annotation 必填，不在此抢 */
    if (is_undefined && !type_node) {
      continue;
    }
    clux_type_t *type = NULL;
    if (type_node) {
      clux_value_t *type_fail = NULL;
      type = stmt_var_type_of(ctx, type_node, &type_fail);
      if (!type) {
        return type_fail;
      }
    } else {
      /* 无标注：shadow 模式求初值表达式取类型（推断不落存储、函数体
         不执行、TDZ 读取按变量类型产 shadow）。 */
      if (!value_node) {
        continue;
      }
      bool prev_mode = clux_context_is_shadow_mode(ctx);
      clux_context_set_shadow_mode(ctx, true);
      clux_value_t *probe = exec_operand(ctx, value_node);
      clux_context_set_shadow_mode(ctx, prev_mode);
      if (!probe) {
        return clux_context_create_error_message(
            ctx,
            node_get_location(value_node),
            "variable hoist cannot infer the initializer type");
      }
      if (clux_value_is_error(probe)) {
        return probe;
      }
      type = clux_value_get_type(probe);
    }
    char *name = location_get(node_get_location(name_node), allocator);
    clux_value_t *tdz = clux_context_create_tdz_value(
        ctx, type, name, node_get_location(name_node));
    allocator_free(allocator, (void **)&name);
    if (!tdz || clux_value_is_error(tdz)) {
      return tdz;
    }
  }
  /* pass 1b phase 2：全部提升名字就位后统一做定义点捕获初始化——
     f 捕获后声明的 g 成立（前向捕获；f/g 互相捕获被类型判等拒绝）；
     var 此刻是 TDZ，捕获 var 源按 TDZ 拒绝（定义点快照语义）。 */
  for (size_t idx = 0; idx < hoisted; idx++) {
    clux_value_t *capture_fail = exec_func_decl_bind_captures(
        ctx, hoisted_funcs[idx].value, hoisted_funcs[idx].decl);
    if (capture_fail) {
      return capture_fail;
    }
  }
  return NULL;
}

/* 提升预注册 var 的定义处（块 pass 2 专用）：求初值后经
   clux_context_assign 的 TDZ 通道首赋值——绕过 const 检查、safe_cast
   后整体覆写并清标记。= undefined 的目标状态就是 TDZ，直接返回。
   目标必是本块提升 pass 注册的 TDZ（块内重复名已在提升期以 duplicate
   快速失败），因此与 exec_stmt_var 的源码序声明路径语义不冲突。 */
static clux_value_t *
exec_stmt_var_define(clux_context_t *ctx, node_t *node, clux_value_t *target) {
  node_t *value_node = stmt_var_node_get_value(node);
  bool is_undefined =
      value_node && node_get_kind(value_node) == CLUX_NODE_LITERAL_UNDEFINED;
  if (is_undefined) {
    return target;
  }
  clux_value_t *value = exec_operand(ctx, value_node);
  if (!value || clux_value_is_error(value)) {
    return value;
  }
  return clux_context_assign(ctx, target, value, node_get_location(value_node));
}

/* 块出口协议：error / return signal / 最后语句值都注册在块作用域内，
   作用域销毁后继续引用即悬空——先回退作用域，再转换出值（error 借
   err 指针在外层重建，err 本体归 context 的 errors 向量，值销毁不动
   本体；signal 载荷与普通值克隆，与「函数返回一律 copy」的值语义
   一致）。
   栈平衡：从真实栈顶逐层走 pop_scope 直到块自身——任何未配对退出
   的中间层都走各自的 pop 通道销毁（不依赖 delete 跳层递归删，defer
   批次的挂点就在每层出口），以 global 兜底防越界挂死。块自身的
   销毁排在转换之后：先 swap 回外层（注册点转移，源值仍存活）转换
   出值，再 delete 块——此时块的 children 已被上方排干，delete 即
   本层的 pop，不做跳层递归删。空块产出 NULL（无值），唯一合法消费
   点是调用帧边界（报 missing return）。 */
clux_value_t *exec_stmt_block(clux_context_t *ctx, node_t *node) {
  allocator_t *allocator = clux_context_get_allocator(ctx);
  clux_context_push_scope(ctx);
  clux_scope_t *block = clux_context_get_scope(ctx);
  clux_scope_t *outer = clux_scope_get_parent(block);
  clux_value_t *result = NULL;
  size_t count = stmt_block_node_get_stmt_count(node);
  /* 提升 pass 失败快速失败：走与执行期一致的块出口协议转换 error。 */
  clux_value_t *hoist_fail = exec_stmt_block_hoist(ctx, node);
  if (hoist_fail) {
    result =
        clux_value_is_error(hoist_fail)
            ? hoist_fail
            : clux_context_create_error_message(
                  ctx, NULL, "block hoist produced a value without an error");
  } else {
    for (size_t idx = 0; idx < count; idx++) {
      node_t *stmt = stmt_block_node_get_stmt(node, idx);
      if (stmt && node_get_kind(stmt) == CLUX_NODE_STMT_FUNC &&
          stmt_func_node_get_decorator_count(stmt) == 0) {
        /* pass 1b 已预执行的无装饰函数声明不再执行（重复执行会撞
           duplicate declaration）。 */
        continue;
      }
      if (stmt && node_get_kind(stmt) == CLUX_NODE_STMT_VAR &&
          stmt_var_node_get_decorator_count(stmt) == 0) {
        /* 提升预注册 var 的定义处：名字命中 TDZ 即本块提升的变量，
           转首赋值；未命中说明提升 pass 跳过（残缺节点等），交回
           源码序声明路径报错。 */
        node_t *name_node = stmt_var_node_get_name(stmt);
        clux_value_t *target = NULL;
        if (name_node) {
          char *probe_name =
              location_get(node_get_location(name_node), allocator);
          target = clux_scope_get(clux_context_get_scope(ctx), probe_name);
          allocator_free(allocator, (void **)&probe_name);
        }
        if (target && clux_value_is_tdz(target)) {
          clux_value_t *value = exec_stmt_var_define(ctx, stmt, target);
          result = value;
          if (clux_value_is_error(value)) {
            break;
          }
          continue;
        }
      }
      clux_value_t *value = exec_node(ctx, stmt);
      result = value;
      if (clux_value_is_error(value) || clux_value_is_return_signal(value)) {
        break;
      }
    }
  }
  clux_scope_t *current = clux_context_get_scope(ctx);
  while (current != block && current != clux_context_get_global_scope(ctx)) {
    clux_context_pop_scope(ctx);
    current = clux_context_get_scope(ctx);
  }
  (void)clux_context_swap_scope(ctx, outer);
  clux_value_t *out = NULL;
  if (clux_value_is_error(result)) {
    clux_error_t *err = *(clux_error_t **)clux_value_get_raw(result, 0);
    out = clux_context_create_error(ctx, err);
  } else if (clux_value_is_return_signal(result)) {
    clux_value_t *payload = clux_return_signal_get_payload(result);
    clux_value_t *cloned =
        payload ? clux_context_clone(ctx, payload, NULL) : NULL;
    out = clux_context_create_return_signal(ctx, cloned);
  } else if (result) {
    out = clux_context_clone(ctx, result, NULL);
  }
  delete_clux_scope(&block);
  return out;
}

/* 表达式语句 expr ';'：表达式值即语句值。 */
clux_value_t *exec_stmt_expr(clux_context_t *ctx, node_t *node) {
  node_t *expr = stmt_expr_node_get_expr(node);
  if (!expr) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "expression statement requires an expression");
  }
  return exec_operand(ctx, expr);
}

/* return; / return expr;（白皮书 §4.11）：产出 return signal（载荷为
   返回值的借用），语句层原样上抛，唯一消费点是调用帧边界。 */
clux_value_t *exec_stmt_return(clux_context_t *ctx, node_t *node) {
  clux_value_t *payload = NULL;
  node_t *value_node = stmt_return_node_get_value(node);
  if (value_node) {
    payload = exec_operand(ctx, value_node);
    if (clux_value_is_error(payload)) {
      return payload;
    }
  }
  return clux_context_create_return_signal(ctx, payload);
}

/* 函数语句 <decorator>* [export]? [comptime]? <func_decl>（白皮书 §3.3、
   §3.4.1、§8.1）：无装饰器时直接执行声明本体——命名函数经
   exec_expr_func_decl 自行 define。有装饰器时（§9.1 函数装饰器）：
   先匿名求值源函数值（不按名 define——scope 无重绑定通道，先声明再
   包装会撞 duplicate 或让名字继续指向源函数），装饰器按源码顺序以
   函数值为实参链式调用（[[add100]] func raw 落地为 raw = add100(raw)，
   包装后的函数值接管名字，后续 raw(x) 经包装函数走捕获调用源函数，
   conformance/09-metadata.cx 断言 raw(5) == 105）；最后 declare_value
   具名化（包装值已注册本作用域，就地命名不拷贝，捕获 view 无损）。
   export（run 阶段产出）与 comptime（解释执行下与普通函数等价）是
   编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_func(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_func_node_get_func_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "function statement requires a function declaration");
  }
  size_t decorator_count = stmt_func_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  node_t *name_node = func_decl_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "decorated function declaration requires a name");
  }
  /* 匿名求值源函数值：注册进当前作用域但不具名——装饰失败无需回滚声明，
     源函数值生命周期归作用域（被装饰函数捕获引用，不可提前销毁）。 */
  clux_value_t *value = exec_func_decl_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_func_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}

/* union 语句 <decorator>* [export]? <union_decl>（白皮书 §3.7、§3.8、
   §8.1）：与函数语句同构——无装饰器时直接执行声明本体（经
   exec_expr_union 按名 define 出 type value）；有装饰器时先匿名求值
   源类型值（不按名 define），装饰器按源码顺序链式应用（求值为 func
   时以类型值为实参调用，编译期标记原样返回），最后 declare_value
   具名化。语句层的声明必须具名（匿名 union 类型只在表达式位置合法）；
   export（run 阶段产出）是编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_union(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_union_node_get_union_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "union statement requires a union declaration");
  }
  node_t *name_node = union_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "union statement requires a named union declaration");
  }
  size_t decorator_count = stmt_union_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  /* 匿名求值源类型值：不具名——type value 无捕获引用，装饰失败
     直接返回 error，无需回滚声明。 */
  clux_value_t *value = exec_expr_union_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_union_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}

/* enum 语句 <decorator>* [export]? <enum_decl>（白皮书 §3.6、§8.1）：
   与 union 语句同构——无装饰器时直接执行声明本体（经
   exec_expr_enum_decl 按名 define 出 type value）；有装饰器时先匿名求值
   源类型值（不按名 define），装饰器按源码顺序链式应用（求值为 func 时
   以类型值为实参调用，编译期标记原样返回），最后 declare_value 具名化。
   语句层的声明必须具名（匿名 enum 类型只在表达式位置合法）；
   export（run 阶段产出）是编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_enum(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_enum_node_get_enum_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "enum statement requires an enum declaration");
  }
  node_t *name_node = enum_decl_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "enum statement requires a named enum declaration");
  }
  size_t decorator_count = stmt_enum_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  /* 匿名求值源类型值：不具名——type value 无捕获引用，装饰失败
     直接返回 error，无需回滚声明。 */
  clux_value_t *value = exec_expr_enum_decl_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_enum_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}

/* interface 语句 <decorator>* [export]? <interface_decl>（白皮书 §3.10、
   §8.1）：与 enum/union 语句同构——无装饰器时直接执行声明本体（经
   exec_expr_interface_decl 按名 define 出 type value）；有装饰器时先
   匿名求值源类型值（不按名 define），装饰器按源码顺序链式应用（求值为
   func 时以类型值为实参调用，编译期标记原样返回），最后 declare_value
   具名化。语句层的声明必须具名（匿名 interface 类型只在表达式位置合法）；
   export（run 阶段产出）是编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_interface(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_interface_node_get_interface_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "interface statement requires an interface declaration");
  }
  node_t *name_node = interface_decl_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "interface statement requires a named interface declaration");
  }
  size_t decorator_count = stmt_interface_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  /* 匿名求值源类型值：不具名——type value 无捕获引用，装饰失败
     直接返回 error，无需回滚声明。 */
  clux_value_t *value = exec_expr_interface_decl_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_interface_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}

/* struct 语句 <decorator>* [export]? <struct_decl>（白皮书 §3.5、§8.1）：
   与 interface/enum 语句同构——无装饰器时直接执行声明本体（经
   exec_expr_struct_decl 两阶段构造：字段壳先行建并按名 define，方法
   成员再求值，三表收尾安装）；有装饰器时先匿名求值源类型值（不按名
   define），装饰器按源码顺序链式应用（求值为 func 时以类型值为实参
   调用，编译期标记原样返回），最后 declare_value 具名化。语句层的
   声明必须具名（匿名 struct 类型只在表达式位置合法）；export（run
   阶段产出）是编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_struct(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_struct_node_get_struct_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "struct statement requires a struct declaration");
  }
  node_t *name_node = struct_decl_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "struct statement requires a named struct declaration");
  }
  size_t decorator_count = stmt_struct_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  /* 匿名求值源类型值：不具名——type value 无捕获引用，装饰失败
     直接返回 error，无需回滚声明。注意：此路径方法签名引用 struct
     自身不可解析（壳名未 define），值级装饰路径待泛型批次处理。 */
  clux_value_t *value = exec_expr_struct_decl_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = apply_decorator(ctx,
                            value,
                            stmt_struct_node_get_decorator(node, idx),
                            node_get_location(node));
    if (clux_value_is_error(value)) {
      return value;
    }
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *bound = clux_context_declare_value(
      ctx, value, name, node_get_location(name_node));
  allocator_free(allocator, (void **)&name);
  return bound;
}
