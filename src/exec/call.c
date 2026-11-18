#include "exec/call.h"
#include "core/allocator.h"
#include "core/error.h"
#include "core/location.h"
#include "engine/context.h"
#include "engine/error.h"
#include "engine/func.h"
#include "engine/interface.h"
#include "engine/scope.h"
#include "engine/type.h"
#include "engine/value.h"
#include "exec/exec.h"
#include "parser/node.h"
#include "parser/node_call.h"
#include "parser/node_func_decl.h"
#include "parser/node_func_param.h"

/* 帧出口协议（栈帧协议核心的收尾半程）：帧内产出的值（error / return
   signal）都注册在帧 scope 内，直接 delete 后引用即悬空——先恢复调用
   方作用域，再转换出值（error 借 err 指针在调用方重建，err 本体归
   context 的 errors 向量，值销毁不动本体；signal 在帧边界解包——
   载荷克隆出帧作为调用结果，与「函数返回一律 copy」的值语义一致；
   error 出帧不走 defer），最后才销毁帧并归还调用深度。 */
static clux_value_t *frame_exit(clux_context_t *ctx,
                                clux_scope_t *frame,
                                clux_scope_t *caller,
                                clux_value_t *result) {
  /* 帧内栈平衡：从真实栈顶逐层 pop 回帧——任何未配对退出的中间层都
     走各自的 pop 通道销毁（不跳层递归删，defer 批次的挂点就在每层
     出口），弹出以 global 兜底防越界挂死。帧本身不在调用方 push 链
     上（父是 global），pop 到位后 swap 回 caller 再销毁帧。 */
  clux_scope_t *current = clux_context_get_scope(ctx);
  while (current != frame && current != clux_context_get_global_scope(ctx)) {
    clux_context_pop_scope(ctx);
    current = clux_context_get_scope(ctx);
  }
  (void)clux_context_swap_scope(ctx, caller);
  clux_value_t *out = NULL;
  if (result && clux_value_is_error(result)) {
    clux_error_t *err = *(clux_error_t **)clux_value_get_raw(result, 0);
    out = clux_context_create_error(ctx, err);
  } else if (result && clux_value_is_return_signal(result)) {
    /* 帧边界是 return signal 的唯一消费点：解包——载荷 clone 出帧
       作为调用结果返回（函数返回一律 copy）。无值 return; 产出 NULL
       （无返回值函数）。 */
    clux_value_t *payload = clux_return_signal_get_payload(result);
    out = payload ? clux_context_clone(ctx, payload, NULL) : NULL;
  }
  delete_clux_scope(&frame);
  clux_context_call_depth_leave(ctx);
  return out;
}

/* AST 执行器（clux_ast_executor_t 帧协议的入口，由 func_invoke
   校验通过后回调）：帧 = 挂 global 的临时 scope（显式捕获语义下体内
   自由名字只解析到形参、捕获与模块级名字，不挂调用方——那是动态
   作用域漏洞）。形参按 copy 语义克隆后具名化进帧；捕获经
   clux_context_func_capture 取读写 view 具名化进帧（写穿函数值存储）。
   return 语句经语句层上抛为 signal，在帧出口解包；fallthrough（无
   return，含空块 NULL）报 missing return。 */
static clux_value_t *exec_call_executor(clux_context_t *ctx,
                                        void *ast,
                                        clux_value_t *func,
                                        size_t argc,
                                        clux_value_t *argv[],
                                        const location_t *loc) {
  (void)loc;
  node_t *decl = ast;
  const location_t *decl_loc = node_get_location(decl);
  if (node_get_kind(decl) != CLUX_NODE_EXPR_FUNC_DECL) {
    return clux_context_create_error_message(
        ctx, decl_loc, "function AST is not a function definition");
  }
  if (!clux_context_call_depth_enter(ctx)) {
    return clux_context_create_error_message(
        ctx, decl_loc, "maximum call depth exceeded");
  }
  allocator_t *allocator = clux_context_get_allocator(ctx);
  clux_scope_t *frame =
      create_clux_scope(allocator, clux_context_get_global_scope(ctx));
  clux_scope_t *caller = clux_context_swap_scope(ctx, frame);
  /* 形参绑定：copy 语义——实参克隆进帧后按 AST 形参名具名化
     （func_invoke 已交验个数与类型，argc == 形参个数）。 */
  node_t *params_node = func_decl_node_get_params(decl);
  clux_value_t *fail = NULL;
  for (size_t idx = 0; idx < argc && !fail; idx++) {
    node_t *param =
        params_node ? func_params_node_get_param(params_node, idx) : NULL;
    node_t *name_node = param && node_get_kind(param) == CLUX_NODE_FUNC_PARAM
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
  }
  /* 捕获绑定：函数值存储的读写 view 按捕获名具名化进帧，体内对捕获
     名字的赋值经 view 写穿函数值存储。 */
  clux_type_t *type = clux_value_get_type(func);
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
  if (fail) {
    return frame_exit(ctx, frame, caller, fail);
  }
  node_t *body = func_decl_node_get_body(decl);
  if (!body) {
    fail = clux_context_create_error_message(
        ctx, decl_loc, "function definition requires a body");
    return frame_exit(ctx, frame, caller, fail);
  }
  clux_value_t *result = exec_node(ctx, body);
  /* fallthrough：无 return（块产出普通最后语句值或空块 NULL）统一报
     missing return——clux 的函数体必须显式 return（白皮书 §4.11）。 */
  if (!result ||
      (!clux_value_is_error(result) && !clux_value_is_return_signal(result))) {
    const char *fname = clux_type_get_name(type);
    fail = clux_context_create_error_message(ctx,
                                             decl_loc,
                                             "missing return in function '%s'",
                                             fname ? fname : "<anonymous>");
    return frame_exit(ctx, frame, caller, fail);
  }
  return frame_exit(ctx, frame, caller, result);
}

/* 懒安装帧协议执行器：任何直接走 clux_context_invoke 的路径（调用表达式、
   var 装饰器的值级包装等）invoke 前必须保证 executor 已挂——首次触发时
   安装，engine 不依赖 exec 的分层保住。 */
void exec_call_install_executor(clux_context_t *ctx) {
  if (!clux_context_get_executor(ctx)) {
    clux_context_set_executor(ctx, exec_call_executor);
  }
}

/* 函数调用表达式 f(args)（白皮书 §2.7）：callee 求值 → 实参逐个求值
   （error 优先传播、spread/rest 实参暂不支持）→ 懒安装执行器（首次
   调用挂帧协议执行器，engine 不依赖 exec 的分层保住）→ clux_context_
   invoke 交验签名并分派。 */
clux_value_t *exec_expr_call(clux_context_t *ctx, node_t *node) {
  node_t *callee_node = call_node_get_callee(node);
  if (!callee_node) {
    return clux_context_create_error_message(
        ctx, node_get_location(node), "function call requires a callee");
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
  exec_call_install_executor(ctx);
  return clux_context_invoke(
      ctx, callee, argc, argc ? argv : NULL, node_get_location(node));
}
