#include "sema/sema.h"
#include "sema/struct_compose.h" /* 结构体组合（`...Base;`）pass */
#include "vm/type_cfunc.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_opaque.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "core/panic.h"
#include "core/string.h"
#include "ctfe/ctfe.h"
#include "parser/ast_array.h"
#include "parser/ast_tuple.h"
#include "parser/ast_const.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_interface_def.h"
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_option.h"
#include "parser/ast_program.h"
#include "parser/ast_ptr.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_scope_annot.h"
#include "parser/ast_type_def.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_var_def.h"
#include "parser/ast_volatile.h"
#include "parser/ast_alignof.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_typeof.h"
#include "parser/parse_utils.h"
#include "parser/lexer.h"
#include "sema/comptime.h"
#include "parser/ast_import.h"
#include "parser/ast_reexport.h"
#include "parser/ast_extern_lib.h"
#include "vm/module.h"
#include "core/ffi_abi.h"   /* M6 FFI：§2 C ABI 类型校验 */
#include <string.h>
#include <stdio.h>

#include "sema_internal.h"

void pass_globals(sema_t *sema, ast_program_t *prog, bool speculative);

/* ===========================================================================
 * pass_globals：全局 comptime var 求值
 *
 * 遍历 prog->funcs 链上的 AST_VAR_DEF（comptime var），逐一定义点求值
 * （sema_eval_comptime_var：改写 init → ctfe 求值 → 符号表编码）。
 * 无论成功失败都从链摘除——comptime var 不进入运行时。
 * 在 pass2_types 之后执行：函数签名已解析（pass1b 已绑定全局 type def），
 * init 可调用函数（含 comptime func）与引用自定义类型。
 * =========================================================================== */

/* ===========================================================================
 * 求值顺序无关：试跑轮 + 不动点
 *
 * 全局编译期实体之间有真实依赖环，单趟顺序无解：
 *     comptime var N = add(1, 2);   // N 需要 add 的签名（pass2 才解析）
 *     type T = [N]i32;              // T 需要 N（而 type def 在 pass1b 解析）
 *     func f(a: T): void { }        // f 的签名需要 T
 * 因此 pass1b / pass2 / pass_globals 都做成**可反复调用**（各自跳过已就绪的
 * 符号），外面套一轮循环反复"试跑"直到不再有新的实体就绪：
 *   - 每轮诊断进临时缓冲并丢弃（半成品不报错）
 *   - 轮次之间的进展由"就绪实体计数"衡量
 *   - 循环结束后再用**真实诊断**跑一遍，把真正无解的错误报出来
 * 这样 `[N]i32` 之类常量位置不再因为书写顺序而挑食。
 * =========================================================================== */


/* ===========================================================================
 * Pass 0：import 语句处理（递归编译被导入模块）
 * =========================================================================== */

/* 取 import / 代理导出声明的路径文本。 */
static strslice_t import_decl_path(ast_node_t *f) {
    if (f->kind == AST_IMPORT) return ((ast_import_t *)f)->path;
    return ((ast_reexport_t *)f)->path;
}

void pass0_imports(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    /* M6 FFI：extern lib 由 **driver** 处理（在 lexer pool 释放前扫描 AST 并
       dlopen/LoadLibrary，句柄挂 module_t.ffi，失败即诊断）——sema 不再重复
       诊断，但仍要跳过它（它不是可编译的声明）。`extern cfunc` 的符号解析与
       调用属后续阶段。 */
    if (f->kind == AST_EXTERN_LIB) {
      /* M6 FFI：注册库别名符号（kind = SEMA_SYM_EXTERN_LIB），使 `crt::malloc` 能
         解析到别名并在 :: 分支给出"调用未实现"的明确诊断，而不是含混的
         "unknown enum type in enum reference"。加载与符号解析由 driver 负责。 */
      ast_extern_lib_t *el = (ast_extern_lib_t *)f;
      sema_symbol_t init = {0};
      init.kind = SEMA_SYM_EXTERN_LIB;
      init.is_active = true;
      sema_symbol_t *fsym = sema_scope_define(sema->global_scope, el->alias,
                                             &init);
      if (!fsym) {
        diag_error(sema->diag, sema_loc(sema, f),
                   "duplicate name '%.*s' for extern lib alias",
                   (int)el->alias.len, el->alias.ptr);
      }
      continue;
    }
    if (f->kind != AST_IMPORT && f->kind != AST_REEXPORT) continue;
    strslice_t raw = import_decl_path(f);

    /* import 的解析与编译由**驱动层注入的回调**完成：路径分类/规范化、
       存在性检查、递归编译（cache + 循环依赖检测）与诊断都在 driver 侧。
       sema 因此不 include 任何 driver 头、不认识 module_t 的内容、不做路径
       处理——依赖方向保持单向（driver → sema）。 */
    if (!sema->imports.resolve) {
      diag_error(sema->diag, sema_loc(sema, f),
                 "imports are not supported in this compilation mode");
      continue;
    }
    struct module_t *mod =
        sema->imports.resolve(sema->imports.user, sema, f, raw);
    if (!mod) continue; /* 回调已诊断（非相对路径 / 未找到 / 递归编译失败） */

    if (f->kind == AST_IMPORT) {
      /* 在当前模块全局作用域注册 <alias> 为 MODULE 符号。
         （AST_REEXPORT（`export * from "./x"`）无需在此记账：依赖模块已在回调里
         编译并注册进 vm->modules，driver 构建本模块导出表时会重解析该路径并
         并入其导出符号，同名冲突在彼处诊断。） */
      ast_import_t *imp = (ast_import_t *)f;
      sema_symbol_t init = {0};
      init.kind = SEMA_SYM_MODULE;
      init.module = mod;
      init.is_active = true;
      sema_symbol_t *sym = sema_scope_define(sema->global_scope, imp->alias,
                                             &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f),
                   "duplicate name '%.*s' for import alias",
                   (int)imp->alias.len, imp->alias.ptr);
      }
    }
  }
}

/* ===========================================================================
 * Pass 1/2：函数名收集 + 类型解析（func_t 签名）
 * =========================================================================== */

/* 解析 + 校验 + 注册一个 extern cfunc 声明。
 * 返回 false = 有类型还没激活（程序类型未就绪）——`report=false` 时静默返回、
 * 交给 `pass1c_extern_abi` 处理。`report=true` 时才产出诊断（避免同一错误两次）。 */
static bool check_extern_cfunc(sema_t *sema, ast_func_def_t *efd, bool report) {
  const type_t *ptypes[64];
  size_t pn = 0;
  bool resolved = true;
  /* 符号名（限定名 "<lib>::<name>"），既用于注册也用于回查已建签名。 */
  char qual[512];
  int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                    (int)efd->lib_name.len, efd->lib_name.ptr,
                    (int)efd->name.len, efd->name.ptr);

  for (ast_node_t *p = efd->params; p && pn < 64; p = p->next) {
    ast_var_def_t *pvd = (ast_var_def_t *)p;
    const type_t *pt = sema_resolve_type_slot(sema, &pvd->type_expr);
    if (!pt) {
      resolved = false;
      ptypes[pn++] = NULL;
      continue;
    }
    const char *why = ffi_c_abi_reject_reason(pt);
    /* 聚合（struct/cunion）要**递归**检查字段：字段本身必须能跨 C ABI。
       用 libffi 描述符构造做校验，失败原因（含字段类型名）直接报在声明处。 */
    char agg_err[256] = {0};
    if (!why && !ffi_c_abi_check_aggregate(pt, sema->vm, agg_err,
                                           sizeof agg_err))
      why = agg_err[0] ? agg_err : "aggregate field cannot cross the C ABI";
    if (!why && type_kind_of(pt) == TYPE_KIND_VOID)
      why = "void is not a valid parameter type";
    if (why && report) {
      diag_error(sema->diag, sema_loc(sema, p),
                 "extern cfunc %.*s::%.*s: parameter '%.*s': %s",
                 (int)efd->lib_name.len, efd->lib_name.ptr,
                 (int)efd->name.len, efd->name.ptr,
                 (int)pvd->name.len, pvd->name.ptr, why);
    }
    ptypes[pn++] = pt;
  }
  const type_t *rt = sema->vm->type_void;
  if (efd->return_expr) {
    rt = sema_resolve_type_slot(sema, &efd->return_expr);
    if (!rt) {
      resolved = false;
    } else if (report) {
      const char *rwhy = ffi_c_abi_reject_reason(rt);
      char agg_rerr[256] = {0};
      if (!rwhy && !ffi_c_abi_check_aggregate(rt, sema->vm, agg_rerr,
                                              sizeof agg_rerr))
        rwhy = agg_rerr[0] ? agg_rerr : "aggregate field cannot cross the C ABI";
      if (rwhy) {
        diag_error(sema->diag, sema_loc(sema, (ast_node_t *)efd),
                   "extern cfunc %.*s::%.*s: return type: %s",
                   (int)efd->lib_name.len, efd->lib_name.ptr,
                   (int)efd->name.len, efd->name.ptr, rwhy);
      }
    }
  }
  if (!resolved) return false;

  /* 注册限定名函数符号 "<lib>::<name>"（`::` 分支据此解析）。
     **不分配 fid**：无函数体、不进运行时函数队列。 */
  if (qn <= 0 || (size_t)qn >= sizeof(qual)) return resolved;
  const type_t *sig =
      type_func_sig(sema->vm, ptypes, pn,
                    rt ? rt : sema->vm->type_void, efd->is_variadic);
  if (sig) {
    /* `.ast` 指向声明节点：调用点需要它取返回类型表达式（程序自定义类型的
       "按类型值返回"要用，见 sema/expr.c 的 FFI 返回类型码映射）。 */
    sema_symbol_t einit = {.kind = SEMA_SYM_FUNC, .type = sig,
                           .ast = (ast_node_t *)efd, .is_active = true,
                           .fid = 0};
    sema_scope_define(sema->global_scope,
                      strslice_from_bytes(qual, (size_t)qn), &einit);
  }
  return resolved;
}

/* 解析 + 校验 + 注册一个 extern var（动态库导出的数据符号）。
 * 与 extern cfunc 对称：限定名变量符号、无本地存储、运行期 EXTERN_LOAD/STORE。 */
static bool check_extern_var(sema_t *sema, ast_var_def_t *evd, bool report) {
  char qual[512];
  int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                    (int)evd->lib_name.len, evd->lib_name.ptr,
                    (int)evd->name.len, evd->name.ptr);
  if (qn <= 0 || (size_t)qn >= sizeof(qual)) {
    if (report) diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                           "extern var name too long");
    return false;
  }
  if (report) {
    sema_symbol_t *libsym = sema_lookup(sema->global_scope, evd->lib_name);
    if (!libsym || libsym->kind != SEMA_SYM_EXTERN_LIB) {
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                 "extern var %s uses undeclared library '%.*s' "
                 "(add `extern lib %.*s from \"<name>\";`)",
                 qual, (int)evd->lib_name.len, evd->lib_name.ptr,
                 (int)evd->lib_name.len, evd->lib_name.ptr);
    }
  }
  const type_t *vt = sema_resolve_type_slot(sema, &evd->type_expr);
  if (!vt) {
    if (report)
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                 "extern var %s: type could not be resolved", qual);
    return false;
  }
  if (report) {
    const char *why = ffi_c_abi_reject_reason(vt);
    /* 数据符号同样按 C ABI 布局暴露：聚合字段里的 share/weak（以及其他无 C
       表示的字段类型）必须在声明处拒绝，不能等到 lowering。 */
    char agg_err[256] = {0};
    if (!why && !ffi_c_abi_check_aggregate(vt, sema->vm, agg_err,
                                           sizeof agg_err))
      why = agg_err[0] ? agg_err : "aggregate field cannot cross the C ABI";
    uint32_t kind = 0;
    if (!why && type_kind_of(vt) == TYPE_KIND_VOID) why = "void is not a value type";
    if (!why && !ffi_data_kind_of(vt, sema->vm, &kind) &&
        !ffi_data_kind_needs_type(vt))
      why = "type cannot be read/written as an external data symbol";
    if (why)
      diag_error(sema->diag, sema_loc(sema, (ast_node_t *)evd),
                 "extern var %s: %s", qual, why);
  }
  sema_symbol_t einit = {.kind = SEMA_SYM_EXTERN_VAR, .type = vt,
                         .ast = (ast_node_t *)evd, .is_active = true};
  sema_scope_define(sema->global_scope,
                    strslice_from_bytes(qual, (size_t)qn), &einit);
  return true;
}

/* cfunc 定义（有体）同样是 C ABI 函数：extern 声明的类型约束在这里同样成立。
   不做符号注册（pass1_names 已注册），只做声明处校验——否则 `cfunc f(p:
   share *T)` 这种签名在没有 export / cfunc_ptr 使用时不会被任何路径检查到。
   注意：`own` 形参是合法的（C 侧负责释放），`own` 返回仍应写成 `fatal`。 */
static void check_cfunc_definition_abi(sema_t *sema, ast_func_def_t *fn) {
  for (ast_node_t *p = fn->params; p; p = p->next) {
    ast_var_def_t *pvd = (ast_var_def_t *)p;
    const type_t *pt = pvd->resolved_type;
    if (!pt && pvd->type_expr) pt = sema_resolve_type_slot(sema, &pvd->type_expr);
    if (!pt) return; /* 类型尚未激活：交给其他诊断，避免级联 */
    const char *why = ffi_c_abi_reject_reason(pt);
    char agg_err[256] = {0};
    if (!why && !ffi_c_abi_check_aggregate(pt, sema->vm, agg_err,
                                           sizeof agg_err))
      why = agg_err[0] ? agg_err : "aggregate field cannot cross the C ABI";
    if (!why && type_kind_of(pt) == TYPE_KIND_VOID)
      why = "void is not a valid parameter type";
    if (why)
      diag_error(sema->diag, sema_loc(sema, p),
                 "cfunc %.*s: parameter '%.*s': %s",
                 (int)fn->name.len, fn->name.ptr,
                 (int)pvd->name.len, pvd->name.ptr, why);
  }
  const type_t *rt = sema->vm->type_void;
  if (!fn->return_expr) return;
  rt = sema_resolve_type_slot(sema, &fn->return_expr);
  if (!rt) return;
  const char *rwhy = ffi_c_abi_reject_reason(rt);
  char agg_rerr[256] = {0};
  if (!rwhy && !ffi_c_abi_check_aggregate(rt, sema->vm, agg_rerr,
                                          sizeof agg_rerr))
    rwhy = agg_rerr[0] ? agg_rerr : "aggregate field cannot cross the C ABI";
  if (rwhy)
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)fn),
               "cfunc %.*s: return type: %s",
               (int)fn->name.len, fn->name.ptr, rwhy);
}

/* pass1c：程序类型激活之后统一校验 extern 签名（并把 pass1_names 未能登记的
   ——签名里含程序自定义类型的——补登记）。 */
void pass1c_extern_abi(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    if (f->kind == AST_FUNC_DEF && ((ast_func_def_t *)f)->is_extern_cfunc)
      (void)check_extern_cfunc(sema, (ast_func_def_t *)f, /*report=*/true);
    else if (f->kind == AST_FUNC_DEF && ((ast_func_def_t *)f)->is_cfunc)
      check_cfunc_definition_abi(sema, (ast_func_def_t *)f);
    else if (f->kind == AST_VAR_DEF && ((ast_var_def_t *)f)->is_extern_var)
      (void)check_extern_var(sema, (ast_var_def_t *)f, /*report=*/true);
  }
}

void pass1_names(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    /* AST_IMPORT / AST_REEXPORT 已在 pass0_imports 处理（注册 MODULE 符号 /
       收集代理导出），跳过 */
    if (f->kind == AST_IMPORT || f->kind == AST_REEXPORT ||
        f->kind == AST_EXTERN_LIB)
      continue;
    if (f->kind == AST_FUNC_DEF && ((ast_func_def_t *)f)->is_extern_cfunc) {
      /* M6 FFI：extern cfunc 声明（无体，符号由 driver 用 dlsym 解析）。
         这里登记**限定名函数符号**，签名校验交给 pass1c（程序自定义类型要等
         pass1b_types 激活后才能 resolve，见 check_extern_cfunc 的说明）。 */
      (void)check_extern_cfunc(sema, (ast_func_def_t *)f, /*report=*/false);
      continue;
    }

    /* M6 FFI：extern var 声明（动态库导出的**数据符号**）。
       与 extern cfunc 对称：注册**限定名变量符号** "<lib>::<name>"，且不进运行时
       变量队列——它没有本地存储，运行期由 dlsym 得到库里的变量地址。
       校验（别名已声明 / 类型可搬运）同样交给 pass1c。 */
    if (f->kind == AST_VAR_DEF && ((ast_var_def_t *)f)->is_extern_var) {
      (void)check_extern_var(sema, (ast_var_def_t *)f, /*report=*/false);
      continue;
    }

    /* 全局 comptime var：注册变量符号（暂不激活，pass_globals 求值后激活）。
       不创建 sema_func_t（不入 Pass 3 队列——不是函数，无函数体）。
       `is_comptime` 必须在**注册时**就置位：类型槽位的延迟判定
       （type_refs_unevaluated_comptime）与按需求值都靠它区分
       "尚未求值的 comptime var" 与普通变量。 */
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_VAR, .ast = f,
                            .is_comptime = vd->is_comptime};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, vd->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)vd->name.len, vd->name.ptr);
      }
      continue;
    }

    /* 全局 type 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 DEFINE 绑定 type value）。 */
    if (f->kind == AST_TYPE_DEF) {
      ast_type_def_t *td = (ast_type_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f,
                            .is_generic = td->generic.count > 0};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, td->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)td->name.len, td->name.ptr);
      }
      continue;
    }

    /* 全局 enum 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_ENUM_DEF) {
      ast_enum_def_t *ed = (ast_enum_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f,
                            .is_generic = ed->generic.count > 0};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, ed->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)ed->name.len, ed->name.ptr);
      }
      continue;
    }

    /* 全局 struct 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_STRUCT_DEF) {
      ast_struct_def_t *sd = (ast_struct_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f,
                            .is_generic = sd->generic.count > 0};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, sd->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)sd->name.len, sd->name.ptr);
      }
      continue;
    }

    /* 全局 union 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_INTERFACE_DEF) {
      ast_interface_def_t *id = (ast_interface_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_INTERFACE, .ast = f,
                            .is_generic = id->generic.count > 0};
      if (!sema_scope_define(sema->global_scope, id->name, &init)) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)id->name.len, id->name.ptr);
      }
      continue;
    }

    if (f->kind == AST_UNION_DEF) {
      ast_union_def_t *ud = (ast_union_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f,
                            .is_generic = ud->generic.count > 0};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, ud->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)ud->name.len, ud->name.ptr);
      }
      continue;
    }

    /* 全局 cunion 定义：注册符号（暂不激活，pass1b 求值后激活）。
       定义点不摘除（进入字节码，运行时 hoist 构造 + 名字绑定）。 */
    if (f->kind == AST_CUNION_DEF) {
      ast_cunion_def_t *cd = (ast_cunion_def_t *)f;
      sema_symbol_t init = {.kind = SEMA_SYM_TYPE, .ast = f};
      sema_symbol_t *sym =
          sema_scope_define(sema->global_scope, cd->name, &init);
      if (!sym) {
        diag_error(sema->diag, sema_loc(sema, f), "duplicate name '%.*s'",
                   (int)cd->name.len, cd->name.ptr);
      }
      continue;
    }

    ast_func_def_t *fn = (ast_func_def_t *)f;
    sema_symbol_t init = {.kind = SEMA_SYM_FUNC}; /* 函数定义顺序自由：Pass 1 全部注册，无遮罩问题 */
    sema_symbol_t *sym = sema_scope_define(sema->global_scope, fn->name, &init);
    if (!sym) {
      diag_error(sema->diag, sema_loc(sema, f), "duplicate function '%.*s'",
                 (int)fn->name.len, fn->name.ptr);
      continue; /* 重复定义不入队 */
    }
    /* M7 泛型函数：只登记符号（未实例化不是运行时实体——无 fid / 签名 /
       队列条目）。使用点由泛型实例化路径解析；这里入队会让普通 pass2/3
       把 `T` 当未知类型。 */
    if (fn->generic.count > 0) {
      /* 泛型 comptime 函数：M7 允许作为**装饰器模板**使用；实例在 sema_generic
         中按普通泛型函数单态化，调用点仍只能出现在编译期上下文（装饰器
         展开体由 CTFE/编译器折叠）。 */
      sym->is_generic = true;
      sym->is_active = true;
      sym->ast = f;
      sym->is_comptime = fn->is_comptime;
      sym->is_noreturn = fn->is_noreturn;
      continue;
    }
    /* 函数名全局可见（无 TDZ）→ 注册即激活 */
    sym->is_active = true;
    sym->is_comptime = fn->is_comptime;
    sym->is_noreturn = fn->is_noreturn;
    /* 登记 sema 层函数对象（Pass 3 队列驱动；局部函数/泛型实例将来追加） */
    sema_func_t *sf = allocator_new_ex(sema->vm->alloc, "sema_func_t",
                                       sizeof(sema_func_t), NULL, NULL, NULL,
                                       1);
    if (!sf) panic("sema: out of memory allocating sema_func");
    sf->def = f;
    sf->scope = NULL;
    sf->param_scope = NULL;
    sf->def_scope = sema->global_scope;
    sf->is_walked = false;
      /* fid 单一来源：创建 sema 函数对象即分配（comptime func 亦分配——统一
       流程；compiler 预扫描跳过 comptime 不读取，占位无冲突）。符号表 fid
       字段同步（AST_FUNC_REF 替换 / 折叠按 id 查询用） */
    sema_func_id_alloc(sema, fn);
    sym->fid = fn->fid;
    vec_push(sema->funcs, sema->vm->alloc, sf);
  }
}

/* pass1d：结构体成员注册（struct 体内的 var/func → 成员作用域符号）
 *
 * 成员不再展开为全局 `T::name` 声明。本 pass 为每个有成员的 sealed struct
 * 类型创建一个 sema 子作用域（global_scope 的 child），注册成员符号：
 *   - func 成员 → SEMA_SYM_FUNC + sema_func_t（入 Pass 2/3 队列，与全局函数
 *     同构），fid 由 sema 统一分配
 *   - var 成员 → SEMA_SYM_VAR（comptime var 标记 is_comptime；pass_globals
 *     求值后激活）
 * member_scope 挂在 sema_symbol_t::member_scope 上，供 ex_call 方法调用
 * 与 ex_scope_ref `T::x` 静态访问解析。
 *
 * 调用时机：pass1b_types 之后（所有 struct 已 seal）、pass2_types 之前
 * （方法函数需要先注册符号 + 入队，pass2 才能解析签名）。 */
void pass1d_struct_members(sema_t *sema, ast_program_t *prog) {
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind != AST_STRUCT_DEF) continue;
    ast_struct_def_t *sd = (ast_struct_def_t *)d;
    if (!sd->members) continue;

    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, sd->name);
    if (!sym || !sym->is_active || !sym->type) continue; /* 类型未就绪 */

    /* M7 P4b：泛型方法（方法自身带类型参数）在调用点按实例化处理：
       这里只登记 is_generic 符号，不入队、不解析签名（调用点负责）。 */
    /* 幂等：试跑轮已创建 member_scope 时跳过（不重复创建/注册） */
    if (sym->member_scope) continue;

    /* 创建成员子作用域（global_scope 的 child） */
    sema_scope_t *mscope = sema_scope_new(sema->vm->alloc, SEMA_SCOPE_GLOBAL,
                                           sema->global_scope);
    if (!mscope) panic("sema: out of memory creating struct member scope");
    sema_scope_add_child(sema->global_scope, mscope);
    sym->member_scope = mscope;

    for (ast_node_t *m = sd->members; m; m = m->next) {
      if (m->kind == AST_FUNC_DEF) {
        ast_func_def_t *fn = (ast_func_def_t *)m;
        /* M7 P4b：泛型方法只登记符号（is_generic），调用点实例化。 */
        if (fn->generic.count > 0) {
          sema_symbol_t ginit = {.kind = SEMA_SYM_FUNC,
                                 .ast = m,
                                 .is_generic = true,
                                 .is_noreturn = fn->is_noreturn,
                                 .is_active = true};
          sema_symbol_t *gmsym = sema_scope_define(mscope, fn->name, &ginit);
          if (!gmsym) {
            diag_error(sema->diag, sema_loc(sema, m),
                       "struct '%.*s': duplicate method '%.*s'",
                       (int)sd->name.len, sd->name.ptr,
                       (int)fn->name.len, fn->name.ptr);
          }
          continue;
        }
        sema_symbol_t init = {.kind = SEMA_SYM_FUNC};
        sema_symbol_t *msym = sema_scope_define(mscope, fn->name, &init);
        if (!msym) {
          diag_error(sema->diag, sema_loc(sema, m),
                     "struct '%.*s': duplicate method '%.*s'",
                     (int)sd->name.len, sd->name.ptr,
                     (int)fn->name.len, fn->name.ptr);
          continue;
        }
        msym->is_active = true;
        msym->is_comptime = fn->is_comptime;
        msym->is_noreturn = fn->is_noreturn;
        /* 登记 sema 层函数对象（与全局函数同构） */
        sema_func_t *sf = allocator_new_ex(sema->vm->alloc, "sema_func_t",
                                           sizeof(sema_func_t), NULL, NULL, NULL,
                                           1);
        if (!sf) panic("sema: out of memory allocating struct method sema_func");
        sf->def = m;
        sf->scope = NULL;
        sf->param_scope = NULL;
        sf->def_scope = mscope; /* struct 方法符号在 member_scope 中 */
        sf->is_walked = false;
        sema_func_id_alloc(sema, fn);
        msym->fid = fn->fid;
        vec_push(sema->funcs, sema->vm->alloc, sf);
      } else if (m->kind == AST_VAR_DEF) {
        ast_var_def_t *vd = (ast_var_def_t *)m;
        sema_symbol_t init = {.kind = SEMA_SYM_VAR,
                              .is_comptime = vd->is_comptime};
        sema_symbol_t *msym = sema_scope_define(mscope, vd->name, &init);
        if (!msym) {
          diag_error(sema->diag, sema_loc(sema, m),
                     "struct '%.*s': duplicate attribute '%.*s'",
                     (int)sd->name.len, sd->name.ptr,
                     (int)vd->name.len, vd->name.ptr);
          continue;
        }
        /* comptime var 成员必须 const：标记后赋值时拦截（TODO: 赋值拦截） */
      }
    }
  }
}

void pass2_types(sema_t *sema, bool speculative) {
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    ast_node_t *f = sf->def;
    ast_func_def_t *fn = (ast_func_def_t *)f;
    /* 符号定义所在作用域：全局函数在 global_scope（sema_lookup 沿 parent 链），
       struct 方法在类型的 member_scope（sema_scope_find_local 只查本层）。 */
    sema_scope_t *def_scope = sf->def_scope ? sf->def_scope : sema->global_scope;
    sema_symbol_t *sym = (def_scope == sema->global_scope)
                             ? sema_lookup(sema->global_scope, fn->name)
                             : sema_scope_find_local(def_scope, fn->name);
    if (!sym) continue;
    if (sym->ast && sym->ast != f) continue; /* 理论上不可达：队列只含有效函数 */

    /* 参数/返回类型槽位解析。**未解析成功则不建签名**（留待下一轮试跑或
       最终正式报错）——否则会用 NULL 参数构造出半有效的签名并把它标记为
       已完成，后续轮次不再重试。
       （槽位解析幂等：首次解析后槽位已折叠为 AST_TYPE_REF。） */
    bool unresolved = false;
    size_t nparams = sema_count_siblings(fn->params);
    const type_t **params = NULL;
    if (nparams > 0) {
      params = allocator_new_ex(sema->vm->alloc, "type_t*", sizeof(type_t *),
                                NULL, NULL, NULL, nparams);
      size_t j = 0;
      for (ast_node_t *p = fn->params; p; p = p->next, j++) {
        ast_var_def_t *vd = (ast_var_def_t *)p;
        const type_t *t = sema_resolve_type_slot(sema, &vd->type_expr);
        if (!t) {
          if (!speculative)
            diag_error(sema->diag, sema_loc(sema, p),
                       "unknown type in parameter '%.*s'",
                       (int)vd->name.len, vd->name.ptr);
          unresolved = true;
        }
        params[j] = t; /* 失败置 NULL，位置对齐，func_shadow_call 校验时跳过 */
      }
    }

    const type_t *rt = NULL;
    if (fn->return_expr) {
      rt = sema_resolve_type_slot(sema, &fn->return_expr);
      if (!rt) {
        if (!speculative)
          diag_error(sema->diag, sema_loc(sema, f), "unknown return type");
        unresolved = true;
      }
    }
    if (unresolved) {
      if (params) allocator_free(sema->vm->alloc, (void **)&params);
      continue; /* 保持未就绪，等下一轮（符号 type 仍为 NULL） */
    }
    if (sym->type) { /* 签名已就绪（试跑轮已解析）：无需重建 */
      if (params) allocator_free(sema->vm->alloc, (void **)&params);
      continue;
    }

    /* 注册签名类型（按签名去重 intern 到 vm 类型池，type_func_sig 复制 params）；
       符号统一记录定义 AST 节点（函数 = AST_FUNC_DEF）；签名类型存于
       sym->type，调用点经 value_make_shadow(vm, sym->type) 构造 shadow callee。
       签名类型**登记进 sema->types**（与数组/const 同机制）——签名本质是
       普通类型，且函数指针作参数时签名引用签名，须经 hoist 两遍扫描
       依赖后序构造（参数/返回类型已在此前解析登记，父先入队）。 */
    const type_t *sig = type_func_sig(sema->vm, params, nparams, rt, false);
    sym->type = sig;
    sym->ast = f;
    /* 签名类型提升：登记 + 递归依赖登记；fn->sig_id 记录（compiler
       compile_func_reg 发 LOAD_TYPE <sig_id>，不再作用域查找） */
    const sema_type_t *st = sema_type_register(sema, sig);
    if (st) fn->sig_id = st->id;

    /* params 临时数组已被 type_func_sig 复制，此处释放 */
    if (params) allocator_free(sema->vm->alloc, (void **)&params);
  }
}

/* ===========================================================================
 * pass1b_types：全局 type def 求值
 *
 * 在 pass2_types（函数签名解析）之前求值全局 type def（sema_eval_type_def：
 * rhs 真实求值 → 折叠 AST_TYPE_REF → 绑定 type value 到编译期 vm 作用域），
 * 使函数签名/参数/返回类型可引用全局 type 定义（type Foo = ... 先于 func
 * 使用）。定义点不摘除——type def 进入字节码，运行时 DEFINE 绑定 type value
 * 到运行时作用域（编译期 vm 与运行时 vm 解耦）。
 * 注：局部 type def 由 3b 按语句顺序求值（定义点激活，TDZ 与 var 一致）。
 *
 * `speculative`：试跑轮（诊断进临时缓冲、不报真错）。两种模式都**跳过已激活
 * 的符号**，因此可反复调用（求值顺序无关的不动点轮次）。
 * =========================================================================== */

void pass1b_types(sema_t *sema, ast_program_t *prog, bool speculative) {
  (void)speculative;
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    strslice_t name = {0};
    switch (f->kind) {
      case AST_TYPE_DEF:   name = ((ast_type_def_t *)f)->name; break;
      case AST_ENUM_DEF:   name = ((ast_enum_def_t *)f)->name; break;
      case AST_STRUCT_DEF: name = ((ast_struct_def_t *)f)->name; break;
      case AST_UNION_DEF:  name = ((ast_union_def_t *)f)->name; break;
      case AST_CUNION_DEF: name = ((ast_cunion_def_t *)f)->name; break;
      default: continue;
    }
    /* 已求值（激活）→ 跳过：使本函数可反复调用（不动点轮次）。
       泛型定义（is_generic）不产生运行时类型——实例化才生成，永远跳过。 */
    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, name);
    if (sym && (sym->is_active || sym->is_generic)) continue;

    if (f->kind == AST_TYPE_DEF) {
      sema_eval_type_def(sema, (ast_type_def_t *)f, sema->global_scope);
    } else if (f->kind == AST_ENUM_DEF) {
      /* 与全局 type def 同 pass：enum 类型先于函数签名解析（签名/变量
         可引用 enum 类型）。定义点保留（进字节码，运行时 hoist 构造）。 */
      sema_eval_enum_def(sema, (ast_enum_def_t *)f, sema->global_scope);
    } else if (f->kind == AST_STRUCT_DEF) {
      /* 与全局 type def 同 pass：struct 类型先于函数签名解析（签名/变量
         可引用 struct 类型）。定义点保留（进字节码，运行时 hoist 构造）。
         实现已收敛到 struct_compose.c 的 sema_struct_compose_pass——它按
         "嵌入依赖优先"反复遍历未求值的定义，因此 `...Base` 的 Base 写在
         Test 之后也能解析。**结构体组合发生在类型 seal 之前**：组合入口在
         开放对象窗口里插字段 + 登记嵌入表，然后只 seal 一次。整条链一次
         处理完，循环继续处理后面的定义（type/union/cunion）。 */
      sema_struct_compose_pass(sema, prog, sema->global_scope);
    } else if (f->kind == AST_UNION_DEF) {
      /* 与全局 type def 同 pass：union 类型先于函数签名解析（签名/变量
         可引用 union 类型）。定义点保留（进字节码，运行时 hoist 构造）。 */
      sema_eval_union_def(sema, (ast_union_def_t *)f, sema->global_scope);
    } else if (f->kind == AST_CUNION_DEF) {
      /* 与全局 type def 同 pass：cunion 类型先于函数签名解析（签名/变量
         可引用 cunion 类型）。定义点保留（进字节码，运行时 hoist 构造）。 */
      sema_eval_cunion_def(sema, (ast_cunion_def_t *)f, sema->global_scope);
    }
  }
}

/* 已就绪的全局编译期实体计数（进展度量）：
   类型/枚举/结构/联合定义与普通全局变量 → 符号已激活；comptime var →
   ct_valid；函数签名 → sym->type 已解析。 */
static size_t sema_ready_count(sema_t *sema, ast_program_t *prog) {
  size_t n = 0;
  for (ast_node_t *f = prog->funcs; f; f = f->next) {
    strslice_t name = {0};
    switch (f->kind) {
      case AST_TYPE_DEF:   name = ((ast_type_def_t *)f)->name; break;
      case AST_ENUM_DEF:   name = ((ast_enum_def_t *)f)->name; break;
      case AST_STRUCT_DEF: name = ((ast_struct_def_t *)f)->name; break;
      case AST_UNION_DEF:  name = ((ast_union_def_t *)f)->name; break;
      case AST_CUNION_DEF: name = ((ast_cunion_def_t *)f)->name; break;
      case AST_VAR_DEF: {
        ast_var_def_t *vd = (ast_var_def_t *)f;
        sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, vd->name);
        if (sym && (vd->is_comptime ? sym->ct_valid : sym->is_active)) n++;
        continue;
      }
      default: continue;
    }
    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, name);
    if (sym && sym->is_active) n++;
  }
  size_t nf = vec_len(sema->funcs);
  for (size_t i = 0; i < nf; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    ast_func_def_t *fn = (ast_func_def_t *)sf->def;
    sema_scope_t *def_scope = sf->def_scope ? sf->def_scope : sema->global_scope;
    sema_symbol_t *sym = (def_scope == sema->global_scope)
                             ? sema_lookup(sema->global_scope, fn->name)
                             : sema_scope_find_local(def_scope, fn->name);
    if (sym && sym->type) n++;
  }
  return n;
}

/* 跑一轮试跑（诊断丢弃），返回本轮新就绪的实体数 */
size_t sema_speculative_round(sema_t *sema, ast_program_t *prog) {
  size_t before = sema_ready_count(sema, prog);
  diag_buf_t *real = sema->diag;
  diag_buf_t *scratch = diag_buf_new(sema->vm->alloc);
  if (!scratch) return 0; /* 内存不足：交给正式轮报错 */
  sema->diag = scratch;
  pass1b_types(sema, prog, /*speculative=*/true);
  pass1d_struct_members(sema, prog);
  pass2_types(sema, /*speculative=*/true);
  pass_globals(sema, prog, /*speculative=*/true);
  sema->diag = real;
  diag_buf_destroy(&scratch); /* 半成品诊断丢弃，正式轮再报 */
  size_t after = sema_ready_count(sema, prog);
  return after > before ? after - before : 0;
}

void pass_comptime_vars(sema_t *sema, ast_program_t *prog, bool speculative) {
  ast_node_t **prev = &prog->funcs;
  for (ast_node_t *f = prog->funcs; f;) {
    ast_node_t *next = f->next;
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      if (!vd->is_extern_var && vd->is_comptime) {
        sema_symbol_t *csym =
            sema_scope_find_local(sema->global_scope, vd->name);
        if (!csym || !csym->ct_valid)
          sema_eval_comptime_var(sema, vd, sema->global_scope);
        if (speculative) prev = &f->next;
        else             *prev = next;
        f = next;
        continue;
      }
    }
    prev = &f->next;
    f = next;
  }
}

void pass_globals(sema_t *sema, ast_program_t *prog, bool speculative) {
  ast_node_t **prev = &prog->funcs;
  for (ast_node_t *f = prog->funcs; f;) {
    ast_node_t *next = f->next;
    if (f->kind == AST_VAR_DEF) {
      ast_var_def_t *vd = (ast_var_def_t *)f;
      /* M6 FFI：extern var 没有本地存储（运行期直接读写库里的变量地址），
         既不是 comptime 常量也不需要 init 求值——原样保留进字节码序列，
         但**不**在这里求值/激活任何东西（引用点由 :: 分支直接处理）。 */
      if (vd->is_extern_var) {
        prev = &f->next;
        f = next;
        continue;
      }
      if (vd->is_comptime) {
        /* comptime var：求值编码常量。已求值（ct_valid）跳过——否则会二次
           绑定同名常量到 root_scope（重复定义）。
           节点摘除（"comptime var 不进运行时"）只在**正式轮**做：试跑轮里
           本轮失败、下轮可能成功，摘掉就再也看不到它了。 */
        sema_symbol_t *csym =
            sema_scope_find_local(sema->global_scope, vd->name);
        if (!csym || !csym->ct_valid)
          sema_eval_comptime_var(sema, vd, sema->global_scope);
        if (speculative) prev = &f->next;
        else             *prev = next;
      } else {
        /* 普通全局变量：init 折叠为字面量/函数引用，保留进字节码（运行时
           DEFINE 到 root_scope，函数体可见）。已求值（is_active）跳过。
           失败：正式轮摘除（避免下游误以为全局 var 存在）；试跑轮保留
           （下轮可能就绪）。 */
        sema_symbol_t *gsym =
            sema_scope_find_local(sema->global_scope, vd->name);
        if (!gsym || !gsym->is_active)
          sema_eval_global_var(sema, vd, sema->global_scope);
        gsym = sema_scope_find_local(sema->global_scope, vd->name);
        if (gsym && gsym->is_active) prev = &f->next;
        else if (speculative)        prev = &f->next;
        else                         *prev = next;
      }
    } else {
      prev = &f->next;
    }
    f = next;
  }

  /* struct 成员 var：与全局 var 同构，但在成员作用域（sym->member_scope）中
     求值/激活。成员 var 的 init 求值上下文是成员作用域（静态属性可引用同
     struct 的其他成员/全局符号）。不摘除节点（成员节点挂在 sd->members 上，
     不在 prog->funcs 链中，无需 prev 摘链）。 */
  for (ast_node_t *d = prog->funcs; d; d = d->next) {
    if (d->kind != AST_STRUCT_DEF) continue;
    ast_struct_def_t *sd = (ast_struct_def_t *)d;
    if (!sd->members) continue;
    sema_symbol_t *sym = sema_scope_find_local(sema->global_scope, sd->name);
    if (!sym || !sym->member_scope) continue;
    sema_scope_t *mscope = sym->member_scope;

    for (ast_node_t *m = sd->members; m; m = m->next) {
      if (m->kind != AST_VAR_DEF) continue;
      ast_var_def_t *vd = (ast_var_def_t *)m;
      if (vd->is_comptime) {
        /* comptime var 成员：求值编码常量（与全局 comptime var 同构） */
        sema_symbol_t *csym = sema_scope_find_local(mscope, vd->name);
        if (!csym || !csym->ct_valid)
          sema_eval_comptime_var(sema, vd, mscope);
      } else {
        /* 普通静态属性：init 折叠为字面量/函数引用，保留进字节码。
           已求值（is_active）跳过。 */
        sema_symbol_t *gsym = sema_scope_find_local(mscope, vd->name);
        if (!gsym || !gsym->is_active)
          sema_eval_global_var(sema, vd, mscope);
      }
    }
  }
}
