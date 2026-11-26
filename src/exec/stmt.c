#include "exec/stmt.h"
#include "core/allocator.h"
#include "core/vec.h"
#include "core/error.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/array.h"
#include "engine/context.h"
#include "engine/func.h"
#include "engine/interface.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/slice.h"
#include "engine/tuple.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/call.h"
#include "exec/exec.h"
#include "exec/expr.h"
#include "parser/node.h"
#include "parser/node_decorator.h"
#include "parser/node_enum_decl.h"
#include "parser/node_func_capture.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_interface_decl.h"
#include "parser/node_stmt_assert.h"
#include "parser/node_stmt_block.h"
#include "parser/node_stmt_defer.h"
#include "parser/node_stmt_do_while.h"
#include "parser/node_stmt_empty.h"
#include "parser/node_stmt_enum.h"
#include "parser/node_stmt_expr.h"
#include "parser/node_stmt_for.h"
#include "parser/node_stmt_foreach.h"
#include "parser/node_stmt_func.h"
#include "parser/node_stmt_if.h"
#include "parser/node_stmt_interface.h"
#include "parser/node_bitfield_decl.h"
#include "parser/node_stmt_bitfield.h"
#include "parser/node_stmt_jump.h"
#include "parser/node_stmt_return.h"
#include "parser/node_stmt_static_assert.h"
#include "parser/node_stmt_struct.h"
#include "parser/node_stmt_switch.h"
#include "parser/node_stmt_type.h"
#include "parser/node_struct_decl.h"
#include "parser/node_stmt_union.h"
#include "parser/node_stmt_var.h"
#include "parser/node_stmt_while.h"
#include "parser/node_switch_branch.h"
#include "parser/node_union.h"
#include <string.h>

/* 单装饰器应用（白皮书 §3.2/§9.1）：普通装饰器是值级包装——装饰器表达式
   求值为 func 值时以当前值为唯一实参调用（[[twice]] var x = 21 落地为
   x = twice(21)；[[add100]] func raw 落地为 raw = add100(raw)）；
   求值为其余值（编译期标记，如 metadata）不改当前值原样返回。
   expr 缺失 / 装饰器求值 error / invoke error 均产出 error。 */
clux_value_t *exec_apply_decorator(clux_context_t *ctx,
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
      value = exec_apply_decorator(ctx,
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
   语义，exec 层不处理；泛型别名产 type 模板值（STMT_TYPE 节点本身即
   AST 载荷，实例化通道按下标实参重求值 VALUE 表达式）。 */
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
  if (stmt_type_node_get_generics(node)) {
    allocator_t *allocator = clux_context_get_allocator(ctx);
    char *name = location_get(node_get_location(name_node), allocator);
    clux_value_t *template_value = clux_context_create_type_template(
        ctx, node, name, node_get_location(node));
    allocator_free(allocator, (void **)&name);
    return template_value;
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

/* 块出口协议：error / return signal / break/continue signal / 最后
   语句值都注册在块作用域内，作用域销毁后继续引用即悬空——先回退作用
   域，再转换出值（error 借 err 指针在外层重建，err 本体归 context 的
   errors 向量，值销毁不动本体；signal 载荷与普通值克隆，与「函数返回
   一律 copy」的值语义一致；break/continue signal 无载荷，直接重建同种
   信号）。
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
      if (clux_value_is_error(value) || clux_value_is_return_signal(value) ||
          clux_value_is_break_signal(value) ||
          clux_value_is_continue_signal(value)) {
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
  } else if (clux_value_is_break_signal(result)) {
    out = clux_context_create_break_signal(ctx);
  } else if (clux_value_is_continue_signal(result)) {
    out = clux_context_create_continue_signal(ctx);
  } else if (result) {
    out = clux_context_clone(ctx, result, NULL);
  }
  /* defer 出口协议：非 error 出口（return/break/continue/普通值/无值）
     按登记逆序（LIFO）执行本块登记的 defer 延期体（0 参 invoke，帧协议
     走 call.c 的 STMT_DEFER 分支）；任一 defer 产 error 替代块出口值并
     中止后续。error 出口绕过 defer（与帧协议「error 出帧不走 defer」
     一致）。defer 值本体与 defers 借用栈都归 block，执行必须排在
     delete 之前；此时 scope 已 swap 到 outer，defer 产 error 在 outer
     重建，天然越过块销毁存活。 */
  vec_t *defers = clux_scope_get_defers(block);
  if (defers && !(out && clux_value_is_error(out))) {
    size_t defer_count = vec_get_size(defers);
    for (size_t idx = defer_count; idx > 0; idx--) {
      clux_value_t *deferred = vec_get(defers, idx - 1);
      exec_call_install_executor(ctx);
      clux_value_t *value =
          clux_context_invoke(ctx, deferred, 0, NULL, node_get_location(node));
      if (clux_value_is_error(value)) {
        clux_error_t *err = *(clux_error_t **)clux_value_get_raw(value, 0);
        out = clux_context_create_error(ctx, err);
        break;
      }
    }
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

/* if 语句 if (cond) { then } [else ...]（白皮书 §4.4）：条件经
   exec_operand 求值必须是 bool——true 执行 then、false 执行 else；
   else 缺失且条件为 false 时语句无值（NULL，与空块同一契约）。
   分支经 exec_node 执行——块语句出口已转换出作用域外的值，return
   signal 原样上抛交调用帧边界；else if 链是嵌套的 STMT_IF，递归
   自然覆盖。 */
clux_value_t *exec_stmt_if(clux_context_t *ctx, node_t *node) {
  node_t *cond_node = stmt_if_node_get_condition(node);
  if (!cond_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "if statement requires a condition");
  }
  node_t *then_node = stmt_if_node_get_then(node);
  if (!then_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "if statement requires a then branch");
  }
  clux_value_t *cond = exec_operand(ctx, cond_node);
  if (!cond || clux_value_is_error(cond)) {
    return cond;
  }
  clux_type_t *type = clux_value_get_type(cond);
  if (clux_type_get_kind(type) != CLUX_TYPE_BOOL) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        node_get_location(cond_node),
        "if condition must be 'bool', got '%s'",
        type_name ? type_name : "<unknown>");
  }
  bool cond_bool = *(bool *)clux_value_get_raw(cond, 0);
  if (cond_bool) {
    return exec_node(ctx, then_node);
  }
  node_t *else_node = stmt_if_node_get_else(node);
  if (!else_node) {
    return NULL;
  }
  return exec_node(ctx, else_node);
}

/* 空语句 `;`（白皮书 §4.14）：不产生操作，只占一个语句位置——产
   NULL（无值，与空块同一契约，block 出口协议对 NULL 结果天然放行）。 */
clux_value_t *exec_stmt_empty(clux_context_t *ctx, node_t *node) {
  (void)ctx;
  (void)node;
  return NULL;
}

/* 循环条件求值公共体（while/do-while 共用）：条件经 exec_operand 求值
   必须产出 bool（操作数 error 优先传播），成功经 *out_cond 写出布尔值，
   失败返回语义化 error。 */
static clux_value_t *stmt_loop_cond_of(clux_context_t *ctx,
                                       node_t *cond_node,
                                       const char *stmt_name,
                                       bool *out_cond) {
  clux_value_t *cond = exec_operand(ctx, cond_node);
  if (!cond) {
    return clux_context_create_error_message(ctx,
                                             node_get_location(cond_node),
                                             "%s condition requires a value",
                                             stmt_name);
  }
  if (clux_value_is_error(cond)) {
    return cond;
  }
  clux_type_t *type = clux_value_get_type(cond);
  if (clux_type_get_kind(type) != CLUX_TYPE_BOOL) {
    const char *type_name = clux_type_get_name(type);
    return clux_context_create_error_message(
        ctx,
        node_get_location(cond_node),
        "%s condition must be 'bool', got '%s'",
        stmt_name,
        type_name ? type_name : "<unknown>");
  }
  *out_cond = *(bool *)clux_value_get_raw(cond, 0);
  return NULL;
}

/* 循环出口值重建（块出口协议的循环版）：排干 owner 内嵌套的子 scope
   ——但保留 owner 本身（out 挂在其上仍存活）——先 swap 到 outer 在
   其中重建 error / return signal（新建值由 outer track），最后才
   delete owner。顺序不可颠倒：先销毁 owner 再读 out 即 use-after-free。 */
static clux_value_t *scope_exit_rebuild(clux_context_t *ctx,
                                        clux_scope_t *outer,
                                        clux_scope_t *owner,
                                        clux_value_t *out) {
  clux_scope_t *global = clux_context_get_global_scope(ctx);
  clux_scope_t *current = clux_context_get_scope(ctx);
  while (current != owner && current != global) {
    clux_context_pop_scope(ctx);
    current = clux_context_get_scope(ctx);
  }
  (void)clux_context_swap_scope(ctx, outer);
  clux_value_t *rebuilt = NULL;
  if (clux_value_is_error(out)) {
    clux_error_t *err = *(clux_error_t **)clux_value_get_raw(out, 0);
    rebuilt = clux_context_create_error(ctx, err);
  } else if (clux_value_is_return_signal(out)) {
    clux_value_t *payload = clux_return_signal_get_payload(out);
    clux_value_t *cloned =
        payload ? clux_context_clone(ctx, payload, NULL) : NULL;
    rebuilt = clux_context_create_return_signal(ctx, cloned);
  }
  delete_clux_scope(&owner);
  return rebuilt;
}

/* break;（白皮书 §4.10）：产出 break signal（无载荷），语句层原样
   上抛，唯一消费点是最近一层循环边界（消费即退出循环）。 */
clux_value_t *exec_stmt_break(clux_context_t *ctx, node_t *node) {
  (void)node;
  return clux_context_create_break_signal(ctx);
}

/* continue;（白皮书 §4.10）：产出 continue signal（无载荷），语句层
   原样上抛，唯一消费点是最近一层循环边界（消费即进入下一轮）。 */
clux_value_t *exec_stmt_continue(clux_context_t *ctx, node_t *node) {
  (void)node;
  return clux_context_create_continue_signal(ctx);
}

/* while 语句 while (cond) { body }（白皮书 §4.6）：先判条件（必须
   bool）后执行——false 即退出。body 值分流：error / return signal
   原样上抛；break signal 消费（退出循环）；continue signal 与其余值
   丢弃进下一轮。循环语句无值（NULL）。 */
clux_value_t *exec_stmt_while(clux_context_t *ctx, node_t *node) {
  node_t *cond_node = stmt_while_node_get_condition(node);
  if (!cond_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "while statement requires a condition");
  }
  node_t *body_node = stmt_while_node_get_body(node);
  if (!body_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "while statement requires a body");
  }
  for (;;) {
    bool cond_bool = false;
    clux_value_t *fail = stmt_loop_cond_of(ctx, cond_node, "while", &cond_bool);
    if (fail) {
      return fail;
    }
    if (!cond_bool) {
      return NULL;
    }
    clux_value_t *value = exec_node(ctx, body_node);
    if (!value) {
      continue;
    }
    if (clux_value_is_error(value) || clux_value_is_return_signal(value)) {
      return value;
    }
    if (clux_value_is_break_signal(value)) {
      return NULL;
    }
  }
}

/* do-while 语句 do { body } while (cond);（白皮书 §4.7）：body 先
   执行一次再判条件（必须 bool）——break signal 在判定前消费（直接
   退出，不触发条件求值），continue signal 消费后落到条件判定；条件
   false 即退出。循环语句无值（NULL）。 */
clux_value_t *exec_stmt_do_while(clux_context_t *ctx, node_t *node) {
  node_t *body_node = stmt_do_while_node_get_body(node);
  if (!body_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "do-while statement requires a body");
  }
  node_t *cond_node = stmt_do_while_node_get_condition(node);
  if (!cond_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "do-while statement requires a condition");
  }
  for (;;) {
    clux_value_t *value = exec_node(ctx, body_node);
    if (value) {
      if (clux_value_is_error(value) || clux_value_is_return_signal(value)) {
        return value;
      }
      if (clux_value_is_break_signal(value)) {
        return NULL;
      }
    }
    bool cond_bool = false;
    clux_value_t *fail =
        stmt_loop_cond_of(ctx, cond_node, "do-while", &cond_bool);
    if (fail) {
      return fail;
    }
    if (!cond_bool) {
      return NULL;
    }
  }
}

/* for 语句 for (init; cond; update) { body }（白皮书 §4.8）：三段式
   循环——init 先执行一次（STMT_VAR 或表达式，可省略），变量生命周期
   为整个 for 语句（独立 scope）；cond 可省略（缺省视为 true），给出时
   必须 bool；update 每轮 body 后执行（可省略），continue 落到 update、
   break 消费退出（跳过 update）。循环语句无值（NULL）。 */
clux_value_t *exec_stmt_for(clux_context_t *ctx, node_t *node) {
  node_t *body_node = stmt_for_node_get_body(node);
  if (!body_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "for statement requires a body");
  }
  node_t *init_node = stmt_for_node_get_init(node);
  node_t *cond_node = stmt_for_node_get_condition(node);
  node_t *update_node = stmt_for_node_get_update(node);
  clux_scope_t *outer = clux_context_get_scope(ctx);
  clux_context_push_scope(ctx);
  clux_scope_t *loop_scope = clux_context_get_scope(ctx);
  clux_value_t *out = NULL;
  if (init_node) {
    clux_value_t *init = exec_node(ctx, init_node);
    if (init && clux_value_is_error(init)) {
      out = init;
    }
  }
  for (; !out;) {
    if (cond_node) {
      bool cond_bool = false;
      clux_value_t *fail = stmt_loop_cond_of(ctx, cond_node, "for", &cond_bool);
      if (fail) {
        out = fail;
        break;
      }
      if (!cond_bool) {
        break;
      }
    }
    clux_value_t *value = exec_node(ctx, body_node);
    if (value) {
      if (clux_value_is_error(value) || clux_value_is_return_signal(value)) {
        out = value;
        break;
      }
      if (clux_value_is_break_signal(value)) {
        break;
      }
    }
    if (update_node) {
      clux_value_t *update = exec_node(ctx, update_node);
      if (update && clux_value_is_error(update)) {
        out = update;
        break;
      }
    }
  }
  /* 出口协议：排干循环作用域并在 outer 中重建 error / return signal，
     避免 pop_scope 销毁后返回悬空值。 */
  return scope_exit_rebuild(ctx, outer, loop_scope, out);
}

/* foreach 语句 foreach ([var] name [: type]? of iterable) { body }
   （白皮书 §4.9）：逐元素迭代——iterable 求值一次后按长度（array 取
   类型长度 / slice 读胖指针 len / tuple 取元素个数）逐索引经 index vtable
   取元素 view；迭代变量每轮独立 scope 内 declare（借用即拷贝落地，
   与 clux 值语义一致），显式类型标注走 safe_cast；body 值分流同 for。
   var 关键字有无是静态可写性约束，exec 层不区分；comptime foreach 是
   编译期展开语义，不经此通道。循环语句无值（NULL）。 */
clux_value_t *exec_stmt_foreach(clux_context_t *ctx, node_t *node) {
  node_t *iterable_node = stmt_foreach_node_get_iterable(node);
  if (!iterable_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "foreach statement requires an iterable");
  }
  node_t *body_node = stmt_foreach_node_get_body(node);
  if (!body_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "foreach statement requires a body");
  }
  node_t *name_node = stmt_foreach_node_get_name(node);
  if (!name_node) {
    return clux_context_create_error_message(ctx,
                                             node_get_location(node),
                                             "foreach statement requires an "
                                             "iteration variable");
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  clux_value_t *iterable = exec_operand(ctx, iterable_node);
  if (clux_value_is_error(iterable)) {
    allocator_free(allocator, (void **)&name);
    return iterable;
  }
  clux_type_kind_t kind = clux_type_kind_unwrap(clux_value_get_type(iterable));
  size_t count = 0;
  switch (kind) {
  case CLUX_TYPE_ARRAY:
    count = clux_array_get_length(clux_value_get_type(iterable));
    break;
  case CLUX_TYPE_SLICE:
    count = ((clux_slice_data_t *)clux_value_get_raw(iterable, 0))->len;
    break;
  case CLUX_TYPE_TUPLE:
    count = clux_tuple_elem_count(clux_value_get_type(iterable));
    break;
  default: {
    const char *type_name = clux_type_get_name(clux_value_get_type(iterable));
    clux_value_t *fail = clux_context_create_error_message(
        ctx,
        node_get_location(iterable_node),
        "foreach iterable must be an array, slice, or tuple, got '%s'",
        type_name ? type_name : "<unknown>");
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  }
  node_t *type_node = stmt_foreach_node_get_type(node);
  clux_scope_t *outer = clux_context_get_scope(ctx);
  clux_value_t *out = NULL;
  bool done = false;
  for (size_t idx = 0; idx < count && !done; idx++) {
    clux_context_push_scope(ctx);
    clux_scope_t *round_scope = clux_context_get_scope(ctx);
    clux_value_t *round_out = NULL;
    clux_value_t *index = clux_context_create_i64(
        ctx, (int64_t)idx, NULL, node_get_location(iterable_node));
    clux_value_t *element =
        clux_context_index(ctx, iterable, index, node_get_location(node));
    if (clux_value_is_error(element)) {
      round_out = element;
    } else if (type_node) {
      clux_value_t *type_fail = NULL;
      clux_type_t *type = stmt_var_type_of(ctx, type_node, &type_fail);
      if (!type) {
        round_out = type_fail;
      } else {
        element = clux_context_safe_cast(
            ctx, element, type, node_get_location(type_node));
        if (clux_value_is_error(element)) {
          round_out = element;
        }
      }
    }
    if (!round_out) {
      clux_value_t *declared = clux_context_declare_value(
          ctx, element, name, node_get_location(name_node));
      if (!declared || clux_value_is_error(declared)) {
        round_out = declared;
        done = true;
      }
    }
    if (!round_out) {
      clux_value_t *value = exec_node(ctx, body_node);
      if (value) {
        if (clux_value_is_error(value) || clux_value_is_return_signal(value)) {
          round_out = value;
          done = true;
        } else if (clux_value_is_break_signal(value)) {
          done = true;
        }
      }
    }
    /* 本轮出口：排干轮作用域并在 outer 中重建 error / return signal */
    out = scope_exit_rebuild(ctx, outer, round_scope, round_out);
  }
  allocator_free(allocator, (void **)&name);
  return out;
}

/* switch 语句 switch (value) { (p, ...) -> { body } ... default -> {...} }
   （白皮书 §4.5）：VALUE 求值一次后逐分支逐模式比较——模式经
   exec_operand 求值 + binary eq 读 bool，命中即执行该分支 body 并以
   其值为语句值（同 if 协议），不 fallthrough；default 分支（PATTERNS
   为 NULL）先记住，模式全扫完无命中才执行；无 default 无命中时语句
   无值（NULL）。break 语义上属外层循环（白皮书 switch 不消费 break），
   命中分支经 exec_node 执行，break signal 原样上抛给最近循环边界。 */
static clux_value_t *stmt_switch_match_branch(clux_context_t *ctx,
                                              clux_value_t *value,
                                              node_t *branch,
                                              bool *out_matched) {
  node_t *patterns = switch_branch_node_get_patterns(branch);
  if (!patterns) {
    /* default 分支由调用方单独处理，此函数只面对模式分支。 */
    *out_matched = false;
    return NULL;
  }
  size_t pattern_count = switch_patterns_node_get_pattern_count(patterns);
  for (size_t idx = 0; idx < pattern_count; idx++) {
    node_t *pattern = switch_patterns_node_get_pattern(patterns, idx);
    if (!pattern) {
      *out_matched = false;
      return clux_context_create_error_message(
          ctx, node_get_location(branch), "switch pattern requires a value");
    }
    clux_value_t *candidate = exec_operand(ctx, pattern);
    if (!candidate || clux_value_is_error(candidate)) {
      *out_matched = false;
      return clux_value_is_error(candidate)
                 ? candidate
                 : clux_context_create_error_message(
                       ctx,
                       node_get_location(pattern),
                       "switch pattern requires a value");
    }
    clux_value_t *eq = clux_context_binary(
        ctx, CLUX_OP_EQ, value, candidate, node_get_location(pattern));
    if (!eq || clux_value_is_error(eq)) {
      *out_matched = false;
      return clux_value_is_error(eq)
                 ? eq
                 : clux_context_create_error_message(
                       ctx,
                       node_get_location(pattern),
                       "switch pattern comparison requires a value");
    }
    if (clux_type_get_kind(clux_value_get_type(eq)) != CLUX_TYPE_BOOL) {
      *out_matched = false;
      return clux_context_create_error_message(
          ctx,
          node_get_location(pattern),
          "switch pattern comparison requires a 'bool'");
    }
    if (*(bool *)clux_value_get_raw(eq, 0)) {
      *out_matched = true;
      return NULL;
    }
  }
  *out_matched = false;
  return NULL;
}

clux_value_t *exec_stmt_switch(clux_context_t *ctx, node_t *node) {
  node_t *value_node = stmt_switch_node_get_value(node);
  if (!value_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "switch statement requires a value");
  }
  node_t *body_node = stmt_switch_node_get_body(node);
  if (!body_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "switch statement requires a body");
  }
  clux_value_t *value = exec_operand(ctx, value_node);
  if (!value || clux_value_is_error(value)) {
    return clux_value_is_error(value)
               ? value
               : clux_context_create_error_message(
                     ctx,
                     node_get_location(value_node),
                     "switch statement requires a value");
  }
  if (node_get_kind(body_node) != CLUX_NODE_SWITCH_BRANCHES) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(body_node),
        "switch statement requires a branch list");
  }
  size_t branch_count = switch_branches_node_get_branch_count(body_node);
  /* 先扫模式分支（default 先记住），全无命中才落到 default。 */
  node_t *default_branch = NULL;
  for (size_t idx = 0; idx < branch_count; idx++) {
    node_t *branch = switch_branches_node_get_branch(body_node, idx);
    if (!branch) {
      continue;
    }
    if (!switch_branch_node_get_patterns(branch)) {
      default_branch = branch;
      continue;
    }
    bool matched = false;
    clux_value_t *fail = stmt_switch_match_branch(ctx, value, branch, &matched);
    if (fail) {
      return fail;
    }
    if (matched) {
      node_t *branch_body = switch_branch_node_get_body(branch);
      if (!branch_body) {
        return clux_context_create_error_message(
            ctx, node_get_location(branch), "switch branch requires a body");
      }
      return exec_node(ctx, branch_body);
    }
  }
  if (default_branch) {
    node_t *branch_body = switch_branch_node_get_body(default_branch);
    if (!branch_body) {
      return clux_context_create_error_message(
          ctx,
          node_get_location(default_branch),
          "switch branch requires a body");
    }
    return exec_node(ctx, branch_body);
  }
  return NULL;
}

/* defer 语句 defer |captures|? { body }（白皮书 §4.12）：延期体落地为
   无参 void 闭包函数值——捕获表复用 FUNC_CAPTURES 布局经
   exec_func_decl_capture_entry 构造，类型名 "defer"（无参、void 返回）；
   AST 挂 defer 节点本身（合成 FUNC_DECL 会因 setter 改 parent +
   delete_node 递归删 children 而 double free，call.c 帧协议对
   STMT_DEFER 分流 body）；捕获初值在 defer 语句执行点快照（白皮书
   语义：登记时定格）。函数值匿名 define 进当前作用域（本体归
   scope），再挂借用进 defers 栈——块出口非 error 路径按 LIFO invoke
   （exec_stmt_block defer 出口协议）。defer 语句本身无值（NULL）。 */
clux_value_t *exec_stmt_defer(clux_context_t *ctx, node_t *node) {
  node_t *body_node = stmt_defer_node_get_body(node);
  if (!body_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "defer statement requires a body");
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  const location_t *loc = node_get_location(node);
  /* 捕获列表形态与函数定义共用：FUNC_CAPTURES 唯一子节点 FUNC_PARAMS，
     内为 FUNC_PARAM（name[: type] [= default]）。 */
  node_t *captures_node = stmt_defer_node_get_captures(node);
  node_t *capture_params = NULL;
  size_t capture_count = 0;
  if (captures_node) {
    capture_params = func_captures_node_get_capture(captures_node, 0);
    if (capture_params &&
        node_get_kind(capture_params) == CLUX_NODE_FUNC_PARAMS) {
      capture_count = func_params_node_get_param_count(capture_params);
    }
  }
  clux_func_capture_t captures[capture_count];
  size_t filled = 0;
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < capture_count && !fail; idx++) {
    node_t *entry = func_params_node_get_param(capture_params, idx);
    if (!entry || node_get_kind(entry) != CLUX_NODE_FUNC_PARAM) {
      fail = clux_context_create_error_message(
          ctx, loc, "generic capture is not supported yet");
      break;
    }
    if (!exec_func_decl_capture_entry(ctx,
                                      func_param_node_get_name(entry),
                                      func_param_node_get_type(entry),
                                      func_param_node_get_default(entry),
                                      idx,
                                      captures,
                                      &filled,
                                      &fail)) {
      break;
    }
  }
  clux_type_t *type = NULL;
  if (!fail) {
    type = clux_context_create_func_type(
        ctx,
        "defer",
        captures,
        filled,
        NULL,
        0,
        clux_context_builtin_type(ctx, CLUX_TYPE_VOID));
  }
  for (size_t idx = 0; idx < filled; idx++) {
    allocator_free(allocator, (void **)&captures[idx].name);
  }
  if (fail) {
    return fail;
  }
  if (!type) {
    return clux_context_create_error_message(
        ctx, loc, "defer type creation failed");
  }
  clux_value_t *value = clux_context_create_func(ctx, type, NULL, loc);
  if (clux_value_is_error(value)) {
    return value;
  }
  value = clux_context_func_set_ast(ctx, value, node, NULL, loc);
  if (clux_value_is_error(value)) {
    return value;
  }
  /* 登记点捕获快照（与函数定义点快照同一套写穿函数值存储的通道）。 */
  clux_value_t *capture_fail =
      exec_func_decl_init_captures(ctx, value, captures_node, node);
  if (capture_fail) {
    return capture_fail;
  }
  /* 值本体已由 create_func 经 create_value 的统一所有权通道匿名 track
     进当前 scope（owned 向量），这里只需挂借用进 defers 栈。 */
  clux_scope_t *scope = clux_context_get_scope(ctx);
  clux_scope_add_defer(scope, value);
  return NULL;
}

/* assert 语句 assert(expr);（白皮书 §11.3）：条件必须 bool——true 语句
   无值（NULL）；false 产出 error，消息直接引用条件表达式源码片段
   （SOURCE 槽 SOURCE_TEXT 节点的 location 指向原文）。 */
clux_value_t *exec_stmt_assert(clux_context_t *ctx, node_t *node) {
  node_t *cond_node = stmt_assert_node_get_condition(node);
  if (!cond_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "assert statement requires a condition");
  }
  bool cond_bool = false;
  clux_value_t *fail = stmt_loop_cond_of(ctx, cond_node, "assert", &cond_bool);
  if (fail) {
    return fail;
  }
  if (cond_bool) {
    return NULL;
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  node_t *source = stmt_assert_node_get_source(node);
  char *text =
      source ? location_get(node_get_location(source), allocator) : NULL;
  clux_value_t *err =
      clux_context_create_error_message(ctx,
                                        node_get_location(node),
                                        "assertion failed: %s",
                                        text ? text : "<unknown>");
  allocator_free(allocator, (void **)&text);
  return err;
}

/* static_assert 语句 static_assert(cond, "msg");（白皮书 §4.16）：exec
   本质是 CTFE——条件在执行点求值，必须 bool；true 语句无值（NULL）；
   false 产出 error，消息引用 MESSAGE 槽字符串字面量的求值文本
   （非字符串/求值失败回退 <unknown>）。 */
clux_value_t *exec_stmt_static_assert(clux_context_t *ctx, node_t *node) {
  node_t *cond_node = stmt_static_assert_node_get_condition(node);
  if (!cond_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "static assert statement requires a condition");
  }
  bool cond_bool = false;
  clux_value_t *fail =
      stmt_loop_cond_of(ctx, cond_node, "static assert", &cond_bool);
  if (fail) {
    return fail;
  }
  if (cond_bool) {
    return NULL;
  }
  const char *msg = "<unknown>";
  node_t *message_node = stmt_static_assert_node_get_message(node);
  if (message_node) {
    clux_value_t *message = exec_operand(ctx, message_node);
    if (message && !clux_value_is_error(message) &&
        clux_type_get_kind(clux_value_get_type(message)) == CLUX_TYPE_STR) {
      char *text = *(char **)clux_value_get_raw(message, 0);
      if (text) {
        msg = text;
      }
    }
  }
  return clux_context_create_error_message(
      ctx, node_get_location(node), "static assertion failed: %s", msg);
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
    value = exec_apply_decorator(ctx,
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
    value = exec_apply_decorator(ctx,
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
    value = exec_apply_decorator(ctx,
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

/* bitfield 语句 <decorator>* [export]? <bitfield_decl>（白皮书 §10、
   §8.1）：与 enum/union 语句同构——无装饰器时直接执行声明本体（经
   exec_expr_bitfield_decl 按名 define 出 type value）；有装饰器时先匿名
   求值源类型值（不按名 define），装饰器按源码顺序链式应用（求值为 func
   时以类型值为实参调用，编译期标记原样返回），最后 declare_value 具名化。
   语句层的声明必须具名（匿名 bitfield 类型只在表达式位置合法）；
   export（run 阶段产出）是编译期语义，exec 层不处理。 */
clux_value_t *exec_stmt_bitfield(clux_context_t *ctx, node_t *node) {
  node_t *decl = stmt_bitfield_node_get_bitfield_decl(node);
  if (!decl) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "bitfield statement requires a bitfield declaration");
  }
  node_t *name_node = bitfield_decl_node_get_name(decl);
  if (!name_node) {
    return clux_context_create_error_message(
        ctx,
        node_get_location(node),
        "bitfield statement requires a named bitfield declaration");
  }
  size_t decorator_count = stmt_bitfield_node_get_decorator_count(node);
  if (decorator_count == 0) {
    return exec_operand(ctx, decl);
  }
  /* 匿名求值源类型值：不具名——type value 无捕获引用，装饰失败
     直接返回 error，无需回滚声明。 */
  clux_value_t *value = exec_expr_bitfield_decl_value(ctx, decl);
  if (clux_value_is_error(value)) {
    return value;
  }
  for (size_t idx = 0; idx < decorator_count; idx++) {
    value = exec_apply_decorator(ctx,
                                 value,
                                 stmt_bitfield_node_get_decorator(node, idx),
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
    value = exec_apply_decorator(ctx,
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
  /* 装饰器表达式先行求值（白皮书 §9.1/§9.6）：标记类（非 func 值，如
     metadata）只做编译期标记；求值失败在壳 define 之前拦截——声明失败
     名字不可解析（与 union/enum 等语句的失败语义一致）。 */
  clux_value_t *wrappers[decorator_count ? decorator_count : 1];
  bool has_func = false;
  for (size_t idx = 0; idx < decorator_count; idx++) {
    node_t *decorator = stmt_struct_node_get_decorator(node, idx);
    node_t *decorator_expr =
        decorator ? decorator_node_get_expr(decorator) : NULL;
    if (!decorator_expr) {
      return clux_context_create_error_message(
          ctx,
          decorator ? node_get_location(decorator) : node_get_location(node),
          "decorator requires an expression");
    }
    wrappers[idx] = exec_operand(ctx, decorator_expr);
    if (!wrappers[idx] || clux_value_is_error(wrappers[idx])) {
      return wrappers[idx] ? wrappers[idx]
                           : clux_context_create_error_message(
                                 ctx,
                                 node_get_location(node),
                                 "struct '%s' decoration failed",
                                 "struct");
    }
    if (clux_type_kind_unwrap(clux_value_get_type(wrappers[idx])) ==
        CLUX_TYPE_FUNC) {
      has_func = true;
    }
  }
  /* 具名两阶段构造（壳先 define）：方法签名引用 struct 自身在 pass C
     可解析——泛型实例体经 sema 物化代换出的实例名同样在此 define。 */
  clux_value_t *bound = exec_operand(ctx, decl);
  if (!bound || clux_value_is_error(bound)) {
    return bound;
  }
  /* func 类类型装饰器在具名壳上按声明序 invoke，完成编译期调用计算
     （§9.9）；但类型绑定保持壳值——类型装饰器不产生运行期代码、
     不改写类型本体（§9.6），包装结果只消费 error，值丢弃。 */
  if (has_func) {
    exec_call_install_executor(ctx);
    for (size_t idx = 0; idx < decorator_count; idx++) {
      if (clux_type_kind_unwrap(clux_value_get_type(wrappers[idx])) !=
          CLUX_TYPE_FUNC) {
        continue;
      }
      node_t *decorator = stmt_struct_node_get_decorator(node, idx);
      clux_value_t *computed = clux_context_invoke(
          ctx,
          wrappers[idx],
          1,
          &bound,
          decorator ? node_get_location(decorator) : node_get_location(node));
      if (!computed || clux_value_is_error(computed)) {
        return computed ? computed
                        : clux_context_create_error_message(
                              ctx,
                              node_get_location(node),
                              "struct '%s' decoration failed",
                              "struct");
      }
    }
  }
  return bound;
}
