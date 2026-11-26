#include "exec/call.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/func.h"
#include "engine/function.h"
#include "engine/interface.h"
#include "engine/module.h"
#include "engine/qualifier.h"
#include "engine/scope.h"
#include "engine/struct.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "exec/generic.h"
#include "parser/node.h"
#include "parser/node_binary.h"
#include "parser/node_call.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"
#include "parser/node_rest.h"
#include "parser/node_stmt_defer.h"

/* 帧出口协议（栈帧协议核心的收尾半程）：帧内产出的值（error / return
   signal）都注册在帧 scope 内（closure 之下各层），直接 delete 后引用
   即悬空——先平衡栈并恢复调用方作用域，再转换出值（error 借 err 指针
   在调用方重建，err 本体归 context 的 errors 向量，值销毁不动本体；
   signal 在帧边界解包——载荷克隆出帧后 safe_cast 适配签名返回类型
   （return 值与实参/赋值走同一兼容通道），与「函数返回一律 copy」的
   值语义一致；error 出帧不走 defer），最后才销毁帧（argument 层连同
   体内未平衡层随 closure 递归销毁）、恢复调用方模块并归还调用深度。
   return_type 为 NULL（fail/defer 路径）时不 cast。 */
static clux_value_t *frame_exit(clux_context_t *ctx,
                                clux_scope_t *closure,
                                clux_scope_t *argument,
                                clux_scope_t *caller,
                                clux_module_t *caller_module,
                                clux_type_t *return_type,
                                clux_value_t *result) {
  /* 帧内栈平衡：从真实栈顶逐层 pop 回 argument——任何未配对退出的中间
     层都走各自的 pop 通道销毁（不跳层递归删，defer 批次的挂点就在每层
     出口），弹出以 global 兜底防越界挂死。argument/closure 两层不由
     pop 销毁（pop 的模块边界守卫可能挡住越层弹出），帧收尾统一 delete
     ——逐层 detach 后递归删不会 double free。 */
  clux_scope_t *current = clux_context_get_scope(ctx);
  while (current != argument && current != clux_context_get_global_scope(ctx)) {
    clux_context_pop_scope(ctx);
    current = clux_context_get_scope(ctx);
  }
  (void)clux_context_swap_scope(ctx, caller);
  (void)clux_context_swap_current_module(ctx, caller_module);
  clux_value_t *out = NULL;
  if (result && clux_value_is_error(result)) {
    clux_error_t *err = *(clux_error_t **)clux_value_get_raw(result, 0);
    out = clux_context_create_error(ctx, err);
  } else if (result && clux_value_is_return_signal(result)) {
    /* 帧边界是 return signal 的唯一消费点：解包——载荷 clone 出帧后
       safe_cast 适配签名返回类型（return 值与实参/赋值同一兼容通道，
       失败产 error 在 caller 作用域出帧）。无值 return; 产出 NULL
       （无返回值函数）。 */
    clux_value_t *payload = clux_return_signal_get_payload(result);
    if (!payload || !return_type) {
      out = payload ? clux_context_clone(ctx, payload, NULL) : NULL;
    } else {
      clux_value_t *cloned = clux_context_clone(ctx, payload, NULL);
      out = cloned ? clux_context_safe_cast(ctx, cloned, return_type, NULL)
                   : cloned;
    }
  }
  delete_clux_scope(&argument);
  delete_clux_scope(&closure);
  clux_context_call_depth_leave(ctx);
  return out;
}

/* AST 执行器（clux_ast_executor_t 帧协议的入口，由 func_invoke
   校验通过后回调）：帧作用域链严格为 global -> module -> closure ->
   argument -> body（body 由块语句自身 push）。module 层取函数定义
   模块（function 对象携带，global 函数无此层），调用期间
   current_module 临时切换——体内自由名字解析到形参、捕获与定义模块
   的模块级名字（经 parent 链），不挂调用方——那是动态作用域漏洞。
   形参按 copy 语义克隆后具名化进 argument 层（遮蔽同名捕获）；捕获经
   clux_context_func_capture 取读写 view 具名化进 closure 层（写穿
   函数值存储）。return 语句经语句层上抛为 signal，在帧出口解包；
   fallthrough（无 return，含空块 NULL）报 missing return。 */
static clux_value_t *exec_call_executor(clux_context_t *ctx,
                                        void *ast,
                                        clux_value_t *func,
                                        size_t argc,
                                        clux_value_t *argv[],
                                        const location_t *loc) {
  (void)loc;
  node_t *decl = ast;
  const location_t *decl_loc = node_get_location(decl);
  node_kind_t decl_kind = node_get_kind(decl);
  /* defer 延期体挂在 STMT_DEFER 节点本身（不能合成 FUNC_DECL——setter
     会 parent 重指 + delete_node 无条件递归删 children，借用子节点即
     double free），与函数定义共用本帧协议：无形参、void 返回。 */
  bool is_defer = decl_kind == CLUX_NODE_STMT_DEFER;
  if (decl_kind != CLUX_NODE_EXPR_FUNC_DECL && !is_defer) {
    return clux_context_create_error_message(
        ctx, decl_loc, "function AST is not a function definition");
  }
  if (!clux_context_call_depth_enter(ctx)) {
    return clux_context_create_error_message(
        ctx, decl_loc, "maximum call depth exceeded");
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  /* 帧构造：closure 挂定义模块作用域（global 函数直接挂 global），
     argument 挂 closure。模块作用域归 scope 树持久持有（module 借用），
     帧只销毁自建的两层。 */
  clux_function_t *function = clux_func_get_function(func);
  clux_module_t *fn_module =
      function ? clux_function_get_module(function) : NULL;
  clux_scope_t *root = fn_module ? clux_module_get_scope(fn_module)
                                 : clux_context_get_global_scope(ctx);
  clux_scope_t *closure = create_clux_scope(allocator, root);
  clux_scope_t *argument = create_clux_scope(allocator, closure);
  clux_scope_t *caller = clux_context_swap_scope(ctx, argument);
  clux_module_t *caller_module =
      clux_context_swap_current_module(ctx, fn_module);
  /* 形参绑定：copy 语义——实参克隆进帧后按 AST 形参名具名化
     （func_invoke 已交验个数与类型，argc == 展开后形参个数）。rest 形参
     （白皮书 §6.4 参数包的帧侧投影）：从当前实参位起剩余实参克隆后打包
     为 tuple 值（类型 = 展开后参数表该区段构成的 TUPLE），有名（
     args: ...Ts 形态）按形参名声明——体内 args 即整个实参组；裸 rest
     只消费不绑定。rest 必为最后形参（类型构造已校验），打包后即止。 */
  node_t *params_node = is_defer ? NULL : func_decl_node_get_params(decl);
  clux_type_t *func_type = clux_value_get_type(func);
  clux_value_t *fail = NULL;
  for (size_t idx = 0, pidx = 0; idx < argc && !fail;) {
    node_t *param =
        params_node ? func_params_node_get_param(params_node, pidx) : NULL;
    if (!param) {
      fail = clux_context_create_error_message(
          ctx, decl_loc, "parameter %zu requires a name", idx);
      break;
    }
    node_t *rest = node_get_kind(param) == CLUX_NODE_EXPR_REST
                       ? param
                       : (node_get_kind(param) == CLUX_NODE_FUNC_PARAM
                              ? func_param_node_get_type(param)
                              : NULL);
    if (rest && node_get_kind(rest) == CLUX_NODE_EXPR_REST) {
      size_t pack_count = argc - idx;
      clux_type_t *pack_elems[pack_count ? pack_count : 1];
      for (size_t aidx = 0; aidx < pack_count; aidx++) {
        pack_elems[aidx] = clux_func_get_param(func_type, idx + aidx);
      }
      clux_type_t *pack_type = clux_context_get_tuple_type(
          ctx, pack_count ? pack_elems : NULL, pack_count);
      clux_value_t *packed =
          clux_context_create_tuple(ctx, pack_type, NULL, decl_loc);
      if (!packed || clux_value_is_error(packed)) {
        fail = packed ? packed
                      : clux_context_create_error_message(
                            ctx, decl_loc, "parameter pack binding failed");
        break;
      }
      for (size_t aidx = 0; aidx < pack_count && !fail; aidx++) {
        clux_value_t *copy = clux_context_clone(ctx, argv[idx + aidx], NULL);
        if (!copy || clux_value_is_error(copy)) {
          fail = copy ? copy
                      : clux_context_create_error_message(
                            ctx, decl_loc, "parameter pack binding failed");
          break;
        }
        clux_value_t *index =
            clux_context_create_i32(ctx, (int32_t)aidx, NULL, decl_loc);
        clux_value_t *view =
            index && !clux_value_is_error(index)
                ? clux_context_index(ctx, packed, index, decl_loc)
                : index;
        if (!view || clux_value_is_error(view)) {
          fail = view ? view
                      : clux_context_create_error_message(
                            ctx, decl_loc, "parameter pack binding failed");
          break;
        }
        clux_value_t *written = clux_context_assign(ctx, view, copy, decl_loc);
        if (!written || clux_value_is_error(written)) {
          fail = written ? written
                         : clux_context_create_error_message(
                               ctx, decl_loc, "parameter pack binding failed");
          break;
        }
      }
      if (!fail && node_get_kind(param) == CLUX_NODE_FUNC_PARAM) {
        node_t *name_node = func_param_node_get_name(param);
        if (name_node) {
          char *name = location_get(node_get_location(name_node), allocator);
          clux_value_t *bound =
              clux_context_declare_value(ctx, packed, name, decl_loc);
          allocator_free(allocator, (void **)&name);
          if (clux_value_is_error(bound)) {
            fail = bound;
          }
        }
      }
      break;
    }
    node_t *name_node = node_get_kind(param) == CLUX_NODE_FUNC_PARAM
                            ? func_param_node_get_name(param)
                            : NULL;
    if (!name_node) {
      fail = clux_context_create_error_message(
          ctx, decl_loc, "parameter %zu requires a name", idx);
      break;
    }
    char *name = location_get(node_get_location(name_node), allocator);
    clux_value_t *copy = clux_context_clone(ctx, argv[idx], NULL);
    clux_value_t *bound = clux_context_declare_value(ctx, copy, name, decl_loc);
    allocator_free(allocator, (void **)&name);
    if (clux_value_is_error(bound)) {
      fail = bound;
    }
    idx++;
    pidx++;
  }
  /* 捕获绑定：函数值存储的读写 view 按捕获名具名化进 closure 层
     （argument 之下、模块级名字之上——形参遮蔽同名捕获），体内对捕获
     名字的赋值经 view 写穿函数值存储。 */
  clux_type_t *type = func_type;
  (void)clux_context_swap_scope(ctx, closure);
  for (size_t idx = 0; idx < clux_func_capture_count(type) && !fail; idx++) {
    const clux_func_capture_t *capture = clux_func_get_capture(type, idx);
    clux_value_t *view =
        clux_context_func_capture(ctx, func, capture->name, decl_loc);
    if (clux_value_is_error(view)) {
      fail = view;
      break;
    }
    clux_value_t *bound =
        clux_context_declare_value(ctx, view, capture->name, decl_loc);
    if (clux_value_is_error(bound)) {
      fail = bound;
    }
  }
  (void)clux_context_swap_scope(ctx, argument);
  if (fail) {
    return frame_exit(
        ctx, closure, argument, caller, caller_module, NULL, fail);
  }
  node_t *body =
      is_defer ? stmt_defer_node_get_body(decl) : func_decl_node_get_body(decl);
  if (!body) {
    fail = clux_context_create_error_message(
        ctx,
        decl_loc,
        is_defer ? "defer statement requires a body"
                 : "function definition requires a body");
    return frame_exit(
        ctx, closure, argument, caller, caller_module, NULL, fail);
  }
  clux_value_t *result = exec_node(ctx, body);
  /* break/continue signal 逃逸到帧边界：循环跳转的唯一消费点是最近一层
     循环边界，直达帧说明循环边界缺失（break/continue 写在循环外）。 */
  if (clux_value_is_break_signal(result)) {
    fail = clux_context_create_error_message(
        ctx, decl_loc, "break outside of a loop");
    return frame_exit(
        ctx, closure, argument, caller, caller_module, NULL, fail);
  }
  if (clux_value_is_continue_signal(result)) {
    fail = clux_context_create_error_message(
        ctx, decl_loc, "continue outside of a loop");
    return frame_exit(
        ctx, closure, argument, caller, caller_module, NULL, fail);
  }
  /* defer 延期体是无参 void 闭包：fallthrough（无 return，块产出普通
     最后语句值或空块 NULL）合法——frame_exit 只转换 error 与 return
     signal，普通值即弃（void 语义不外传）。 */
  if (is_defer) {
    return frame_exit(ctx,
                      closure,
                      argument,
                      caller,
                      caller_module,
                      clux_func_get_return_type(type),
                      result);
  }
  /* fallthrough：无 return（块产出普通最后语句值或空块 NULL）统一报
     missing return——clux 的函数体必须显式 return（白皮书 §4.11）。 */
  if (!result ||
      (!clux_value_is_error(result) && !clux_value_is_return_signal(result))) {
    const char *fname = clux_type_get_name(type);
    fail = clux_context_create_error_message(ctx,
                                             decl_loc,
                                             "missing return in function '%s'",
                                             fname ? fname : "<anonymous>");
    return frame_exit(
        ctx, closure, argument, caller, caller_module, NULL, fail);
  }
  return frame_exit(ctx,
                    closure,
                    argument,
                    caller,
                    caller_module,
                    clux_func_get_return_type(type),
                    result);
}

/* 懒安装帧协议执行器：任何直接走 clux_context_invoke 的路径（调用表达式、
   var 装饰器的值级包装等）invoke 前必须保证 executor 已挂——首次触发时
   安装，engine 不依赖 exec 的分层保住。 */
void exec_call_install_executor(clux_context_t *ctx) {
  if (!clux_context_get_executor(ctx)) {
    clux_context_set_executor(ctx, exec_call_executor);
  }
}

/* 成员方法调用 a.b(args)（白皮书 §3.5.1）：实例调用是语法糖——
   a 是结构体值时展开为 T::b(a.&, args)（receiver 取址作首参），a 已是
   指针时为 T::b(a, args)（原样作首参）。方法本体从 receiver 类型的
   method 名单经 attribute 表取 func 值（借用），receiver 之后才是声明
   实参——签名交验/帧协议与普通调用零差异。method 名单外的名字（字
   段/attribute）不走本糖：字段调用落通用 callee 求值路径（字段类型是
   func 就能调），attribute 属类型命名空间，须 T::attr 显式访问。 */
static clux_value_t *exec_member_call(clux_context_t *ctx, node_t *node) {
  node_t *callee_node = call_node_get_callee(node);
  node_t *receiver_node = binary_node_get_left(callee_node);
  node_t *name_node = binary_node_get_right(callee_node);
  const location_t *op_loc =
      node_get_location(binary_node_get_symbol(callee_node));
  if (!receiver_node) {
    return clux_context_create_error_message(
        ctx, op_loc, "member call requires a receiver");
  }
  if (!name_node || node_get_kind(name_node) != CLUX_NODE_LITERAL_IDENTIFIER) {
    return clux_context_create_error_message(
        ctx, op_loc, "member call requires a method name");
  }
  clux_value_t *receiver = exec_operand(ctx, receiver_node);
  if (clux_value_is_error(receiver)) {
    return receiver;
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  char *name = location_get(node_get_location(name_node), allocator);
  /* 指针接收者先 deref 产 base view（共享内存）供方法名单查找（白皮书
     §3.5.1：a 已是指针时 T::b(a, args)——T 取 base）；argv[0] 仍用原
     指针接收者，deref 只是查找通道，不改变前置语义。 */
  clux_value_t *lookup = receiver;
  if (clux_type_get_kind(clux_value_get_type(receiver)) == CLUX_TYPE_PTR) {
    lookup = clux_context_deref(ctx, receiver, (location_t *)op_loc);
    if (clux_value_is_error(lookup)) {
      allocator_free(allocator, (void **)&name);
      return lookup;
    }
  }
  /* qualifier 包装不遮蔽 method 名单，先 unwrap 再查类型命名空间。 */
  clux_type_t *receiver_type = clux_type_unwrap(clux_value_get_type(lookup));
  if (!receiver_type) {
    receiver_type = clux_value_get_type(lookup);
  }
  if (clux_type_get_kind(receiver_type) != CLUX_TYPE_STRUCT ||
      !clux_struct_is_method(receiver_type, name)) {
    const char *type_name = clux_type_get_name(receiver_type);
    clux_value_t *fail =
        clux_context_create_error_message(ctx,
                                          op_loc,
                                          "type '%s' has no method '%s'",
                                          type_name ? type_name : "<unknown>",
                                          name);
    allocator_free(allocator, (void **)&name);
    return fail;
  }
  clux_value_t *method =
      clux_context_struct_attr(ctx, receiver_type, name, op_loc);
  if (clux_value_is_error(method)) {
    allocator_free(allocator, (void **)&name);
    return method;
  }
  allocator_free(allocator, (void **)&name);
  /* receiver 前置为一般首参：值接收者取址（a.&），指针接收者原样——
     形参类型交验由 invoke 的普通实参规则接管。 */
  clux_value_t *first = receiver;
  if (clux_type_get_kind(clux_value_get_type(receiver)) != CLUX_TYPE_PTR) {
    first = clux_context_ref(ctx, receiver, op_loc);
    if (clux_value_is_error(first)) {
      return first;
    }
  }
  /* C++ const 原则：receiver 与 self 形参类型恒等直过；不恒等经
     safe_cast 吸收——*T 接收者对 *const T 形参加 const 协变放行
     （帧内 self 为 *const T），*const T 接收者对 *T 形参被协变方向
     拒绝（丢 const 撕毁写保护）。 */
  clux_type_t *self_param = clux_func_get_param(clux_value_get_type(method), 0);
  if (self_param && !clux_type_equal(clux_value_get_type(first), self_param)) {
    clux_value_t *absorbed =
        clux_context_safe_cast(ctx, first, self_param, op_loc);
    if (clux_value_is_error(absorbed)) {
      return absorbed;
    }
    first = absorbed;
  }
  size_t argc = call_node_get_arg_count(node);
  clux_value_t *argv[argc + 1];
  argv[0] = first;
  for (size_t idx = 0; idx < argc; idx++) {
    node_t *arg = call_node_get_arg(node, idx);
    if (node_get_kind(arg) == CLUX_NODE_EXPR_REST) {
      return clux_context_create_error_message(
          ctx, node_get_location(arg), "spread argument is not supported yet");
    }
    argv[idx + 1] = exec_operand(ctx, arg);
    if (clux_value_is_error(argv[idx + 1])) {
      return argv[idx + 1];
    }
  }
  exec_call_install_executor(ctx);
  return clux_context_invoke(
      ctx, method, argc + 1, argv, node_get_location(node));
}

/* 函数调用表达式 f(args)（白皮书 §2.7）：callee 求值 → 实参逐个求值
   （error 优先传播、spread/rest 实参暂不支持）→ func 模板 callee 先走
   调用点推导（§6.7）产出实例 → 懒安装执行器（首次调用挂帧协议执行
   器，engine 不依赖 exec 的分层保住）→ clux_context_invoke 交验签名
   并分派。callee 为成员访问 a.b 时转入方法调用糖（receiver 前置）；
   T::b / T::variant 静态形态经通用 callee 求值（binary "::" 分支）自然
   打通。 */
clux_value_t *exec_expr_call(clux_context_t *ctx, node_t *node) {
  node_t *callee_node = call_node_get_callee(node);
  if (!callee_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "function call requires a callee");
  }
  if (node_get_kind(callee_node) == CLUX_NODE_EXPR_BINARY &&
      location_is(node_get_location(binary_node_get_symbol(callee_node)),
                  ".")) {
    return exec_member_call(ctx, node);
  }
  clux_value_t *callee = exec_operand(ctx, callee_node);
  if (clux_value_is_error(callee)) {
    return callee;
  }
  size_t argc = call_node_get_arg_count(node);
  clux_value_t *argv[argc ? argc : 1];
  for (size_t idx = 0; idx < argc; idx++) {
    node_t *arg = call_node_get_arg(node, idx);
    if (node_get_kind(arg) == CLUX_NODE_EXPR_REST) {
      return clux_context_create_error_message(
          ctx, node_get_location(arg), "spread argument is not supported yet");
    }
    argv[idx] = exec_operand(ctx, arg);
    if (clux_value_is_error(argv[idx])) {
      return argv[idx];
    }
  }
  /* 调用点推导（白皮书 §6.7）：只有函数调用触发推断——callee 是 func
     模板时从实参类型推断类型参数绑定，产出 func 实例后再走普通 invoke。
     推断不出/值参数由实例化核心走默认值回退（无默认值报 missing）。 */
  if (clux_type_get_kind(clux_value_get_type(callee)) ==
      CLUX_TYPE_FUNC_TEMPLATE) {
    clux_value_t *instance = exec_generic_infer_instance(
        ctx, callee, argc, argc ? argv : NULL, node_get_location(node));
    if (!instance || clux_value_is_error(instance)) {
      return instance ? instance
                      : clux_context_create_error_message(
                            ctx,
                            node_get_location(node),
                            "generic inference failed");
    }
    callee = instance;
  }
  exec_call_install_executor(ctx);
  return clux_context_invoke(
      ctx, callee, argc, argc ? argv : NULL, node_get_location(node));
}
