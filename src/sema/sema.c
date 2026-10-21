#include "sema/sema.h"
#include "sema/struct_compose.h" /* 结构体组合（`...Base;`）pass */
#include "vm/type_cfunc.h"
#include "vm/type_func.h"
#include "vm/type_array.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_ownership.h"
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
#include "parser/ast_union_def.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_type.h"
#include "parser/ast_ident.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_option.h"
#include "parser/ast_program.h"
#include "parser/ast_ptr.h"
#include "parser/ast_owned_type.h"
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
#include "vm/ffi_lib.h"   /* M6 FFI：§2 C ABI 类型校验 */
#include <string.h>
#include <stdio.h>

#include "sema_internal.h"

/* ===========================================================================
 * 上下文管理
 * =========================================================================== */

sema_t *sema_create(vm_t *vm, diag_buf_t *diag, vec_t *tokens,
                    arena_t *arena) {
  if (!vm || !diag || !tokens || !arena) return NULL;
  sema_t *sema =
      allocator_new_ex(vm->alloc, "sema_t", sizeof(sema_t), NULL, NULL, NULL,
                       1);
  sema->vm = vm;
  sema->diag = diag;
  sema->tokens = tokens;
  sema->arena = arena;
  sema->global_scope = NULL;
  sema->funcs = vec_new(vm->alloc, false); /* 元素手动释放（sema_func_t 无 dispose） */
  sema->closures = vec_new(vm->alloc, false);
  sema->types = vec_new(vm->alloc, false); /* 元素手动释放（sema_type_t 无 dispose） */
  sema->fn.return_type = NULL;
  sema->fn.local_func_base = NULL;
  sema->fn.local_func_param_scope = NULL;
  sema->fn.in_call_callee = false;
  sema->fn.loop_depth = 0;
  sema->func_id_next = FUNC_ID_PROGRAM_BASE;
  /* import 回调默认未注入：单文件/测试场景下 import 语句报「当前编译模式不支持」 */
  sema->imports.resolve = NULL;
  sema->imports.user = NULL;
  return sema;
}

/* 注入 import 解析回调（driver 在 sema_create 后、sema_analyze 前调用）。
   见 sema.h 的 sema_import_resolver_t：sema 借此把"路径解析 + 递归编译"交给
   驱动层，自身保持不依赖 driver（消除 sema → driver 反向依赖）。 */
void sema_set_import_resolver(sema_t *sema, sema_import_resolver_t resolve,
                              void *user) {
  if (!sema) return;
  sema->imports.resolve = resolve;
  sema->imports.user = user;
}

void sema_destroy(sema_t **sema) {
  if (!sema || !*sema) return;
  allocator_t *alloc = (*sema)->vm->alloc;
  if ((*sema)->funcs) {
    size_t n = vec_len((*sema)->funcs);
    for (size_t i = 0; i < n; i++) {
      sema_func_t *sf = (sema_func_t *)vec_get((*sema)->funcs, i);
      allocator_free(alloc, (void **)&sf);
    }
    vec_free(alloc, &(*sema)->funcs);
  }
  if ((*sema)->closures) vec_free(alloc, &(*sema)->closures);
  if ((*sema)->types) {
    size_t n = vec_len((*sema)->types);
    for (size_t i = 0; i < n; i++) {
      sema_type_t *st = (sema_type_t *)vec_get((*sema)->types, i);
      allocator_free(alloc, (void **)&st);
    }
    vec_free(alloc, &(*sema)->types);
  }
  allocator_free(alloc, (void **)sema);
}

location_t sema_loc(sema_t *sema, ast_node_t *node) {
  location_t zero = {0};
  if (!sema || !node) return zero;
  const token_t *t = (const token_t *)vec_get(sema->tokens, node->tok_begin);
  const location_t *loc = t ? token_get_location(t) : NULL;
  return loc ? *loc : zero;
}

void sema_type_name(const type_t *t, char *buf, size_t cap) {
  if (!buf || cap == 0) return;
  buf[0] = '\0';
  if (!t || !t->name.ptr) return;
  size_t n = t->name.len < cap - 1 ? t->name.len : cap - 1;
  memcpy(buf, t->name.ptr, n);
  buf[n] = '\0';
}

size_t sema_count_siblings(const ast_node_t *node) {
  size_t n = 0;
  for (const ast_node_t *p = node; p; p = p->next) n++;
  return n;
}

/* 函数 id 分配（幂等）：创建 sema 函数对象时调用——fid 单一来源在 sema，
   compiler 预扫描只读取。fn->fid 已分配则复用（CTFE 求值字面量可能早于
   sema_check_func_literal，两者幂等）。 */
uint32_t sema_func_id_alloc(sema_t *sema, ast_func_def_t *fn) {
  if (!sema || !fn) return 0;
  if (fn->fid != 0) return fn->fid;
  if (sema->func_id_next >= FUNC_ID_PROGRAM_BASE &&
      (uint64_t)sema->func_id_next + 1u <= 0xFFFFFFFFull) {
    fn->fid = sema->func_id_next++;
    return fn->fid;
  }
  return 0; /* id 溢出 */
}

/* 按函数 id 查 sema 函数对象（线性扫描，函数数量少）。函数定义 AST 托管在
   sema->funcs，sema/ctfe 经此按 id 查询（折叠产物按 fid 取签名构造引用）。 */
sema_func_t *sema_func_by_id(sema_t *sema, uint32_t fid) {
  if (!sema || !sema->funcs) return NULL;
  size_t n = vec_len(sema->funcs);
  for (size_t i = 0; i < n; i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    if (!sf || !sf->def || sf->def->kind != AST_FUNC_DEF) continue;
    if (((ast_func_def_t *)sf->def)->fid == fid) return sf;
  }
  return NULL;
}

/* ===========================================================================
 * 三遍编排
 * =========================================================================== */

bool sema_analyze(sema_t *sema, ast_node_t *program) {
  if (!sema || !program || program->kind != AST_PROGRAM) return false;
  ast_program_t *prog = (ast_program_t *)program;

  sema->global_scope =
      sema_scope_new(sema->vm->alloc, SEMA_SCOPE_GLOBAL, NULL);
  if (!sema->global_scope) return false;

  /* 预注册 len 内置函数（m4-design §5/§11.3：len(s) -> usize，即 C 的 size_t）。
     与 VM 侧 vm_register_len 对应。len 接受任意可长度的值（数组/切片/str），
     注册为 variadic（0 固定参数）——sema 调用点特判从实参类型校验可长性
     并产出 usize 返回类型（C 的 size_t：随架构变化）。fid 从 vm->functions 按名查询。 */
  {
    const type_t *lsig = type_func_sig(sema->vm, NULL, 0, vm_type_usize(sema->vm),
                                       /*is_variadic=*/true);
    uint32_t lfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 3 &&
            memcmp(bf->name.ptr, "len", 3) == 0) {
          lfid = bf->id;
          break;
        }
      }
    }
    if (lsig) {
      sema_symbol_t linit = {.kind = SEMA_SYM_FUNC, .type = lsig,
                             .is_active = true, .fid = lfid,
                             .builtin_op = SEMA_BUILTIN_LEN};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("len"), &linit);
    }
  }

  /* 预注册 RC 内置函数（share/weak 是真实类型，源码不写这些名字）：
       - __make_share(p)：fatal/own *T → share *T（§12.1 创建通道）
       - __weak_get(w)  ：weak *T → ?share *T（§12.4 原子升级，`.get()` 语法糖） */
  {
    const type_t *vsig = type_func_sig(sema->vm, NULL, 0,
                                       sema->vm->type_opaque,
                                       /*is_variadic=*/true);
    if (vsig) {
      const char *names[2] = {"__make_share", "__weak_get"};
      sema_builtin_op_t ops[2] = {SEMA_BUILTIN_MAKE_SHARE,
                                  SEMA_BUILTIN_WEAK_GET};
      for (int k = 0; k < 2; k++) {
        uint32_t fid = 0;
        if (sema->vm->functions) {
          size_t nf = vec_len(sema->vm->functions);
          for (size_t i = 0; i < nf; i++) {
            func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
            if (bf && bf->name.ptr &&
                bf->name.len == strlen(names[k]) &&
                memcmp(bf->name.ptr, names[k], bf->name.len) == 0) {
              fid = bf->id;
              break;
            }
          }
        }
        sema_symbol_t init = {.kind = SEMA_SYM_FUNC, .type = vsig,
                              .is_active = true, .fid = fid,
                              .builtin_op = ops[k]};
        sema_scope_define(sema->global_scope,
                          strslice_from_bytes((char *)names[k],
                                              strlen(names[k])),
                          &init);
      }
    }
  }

  /* 预注册 __deep_clone 内置函数（运行期对象图深拷贝）：静态类型表达不了
     克隆语义的形态（闭包值 / cunion 被指向值 / 无 prelude 的切片）在 sema
     里改写为对它的调用，因此 CLONE 指令可以彻底移除。变参签名，调用点按实参
     类型精化返回类型（同类型 fatal）。fid 从 vm->functions 按名查询。 */
  {
    const type_t *dsig = type_func_sig(sema->vm, NULL, 0,
                                       sema->vm->type_opaque,
                                       /*is_variadic=*/true);
    uint32_t dfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 12 &&
            memcmp(bf->name.ptr, "__deep_clone", 12) == 0) {
          dfid = bf->id;
          break;
        }
      }
    }
    if (dsig) {
      sema_symbol_t init = {.kind = SEMA_SYM_FUNC, .type = dsig,
                            .is_active = true, .fid = dfid,
                            .builtin_op = SEMA_BUILTIN_DEEP_CLONE};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("__deep_clone"), &init);
    }
  }

  /* 预注册 data 内置函数（C ABI 适配：data(array/slice/str) -> ref *T，取首元素
     指针）。与 VM 侧 vm_register_data 对应。注册为 variadic（0 固定参数）——
     sema 调用点特判按实参类型精化返回类型为 `ref *<元素类型>`（str → ref *u8），
     运行期也按实参类型重新推导。fid 从 vm->functions 按名查询。 */
  {
    const type_t *ref_opaque = type_ptr_intern(sema->vm,
                                               sema->vm->type_opaque);
    const type_t *dsig = ref_opaque
                             ? type_func_sig(sema->vm, NULL, 0, ref_opaque,
                                             /*is_variadic=*/true)
                             : NULL;
    uint32_t dfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 4 &&
            memcmp(bf->name.ptr, "data", 4) == 0) {
          dfid = bf->id;
          break;
        }
      }
    }
    if (dsig) {
      sema_symbol_t dinit = {.kind = SEMA_SYM_FUNC, .type = dsig,
                             .is_active = true, .fid = dfid,
                             .builtin_op = SEMA_BUILTIN_DATA};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("data"), &dinit);
    }
  }

  /* 预注册 cfunc_ptr 内置函数（M6 Phase 5：cfunc_ptr(f) -> opaque，C 函数指针）。
     与 VM 侧 vm_register_cfunc_ptr 对应。参数是任意函数值（cfunc 定义），
     注册为 variadic（0 固定参数）——具体 §2 校验在运行期由 cfunc 内部完成
     （与 len 同一套路），返回类型 opaque。fid 从 vm->functions 按名查询。 */
  {
    const type_t *csig = type_func_sig(sema->vm, NULL, 0,
                                       sema->vm->type_opaque,
                                       /*is_variadic=*/true);
    uint32_t cfid = 0;
    if (sema->vm->functions) {
      size_t nf = vec_len(sema->vm->functions);
      for (size_t i = 0; i < nf; i++) {
        func_t *bf = (func_t *)vec_get(sema->vm->functions, i);
        if (bf && bf->name.len == 10 &&
            memcmp(bf->name.ptr, "cfunc_ptr", 10) == 0) {
          cfid = bf->id;
          break;
        }
      }
    }
    if (csig) {
      sema_symbol_t cinit = {.kind = SEMA_SYM_FUNC, .type = csig,
                             .is_active = true, .fid = cfid,
                             .builtin_op = SEMA_BUILTIN_CFUNC_PTR};
      sema_scope_define(sema->global_scope, STRSLICE_LIT("cfunc_ptr"), &cinit);
    }
  }

  pass0_imports(sema, prog);
  pass1_names(sema, prog);

  /* 全局编译期实体按**不动点轮次**求值（求值顺序无关）：
     type def / 函数签名 / comptime var / 全局变量之间的依赖没有固定拓扑序
     ——`comptime var N = add(1,2); type T = [N]i32;` 里 N 需要 add 的签名
     （pass2）、T 需要 N（pass1b 阶段）、可能还有 `func f(a: T)` 需要 T。
     先反复试跑（诊断丢弃）到不再有新实体就绪，再用真实诊断跑一遍报错。 */
  for (int round = 0; round < 8; round++) {
    if (sema_speculative_round(sema, prog) == 0) break;
  }
  /* 试跑轮的诊断进临时缓冲被丢弃，但"环已诊断"标记会留下来；复位它，正式轮
     才会重新检测并报出循环嵌入（否则环错误只存在于被丢弃的缓冲区里）。 */
  sema_struct_compose_reset_reports(prog);
  pass1b_types(sema, prog, /*speculative=*/false); /* 全局 type def */
  pass1c_extern_abi(sema, prog); /* extern cfunc/var 签名校验（程序类型已激活） */
  pass1d_struct_members(sema, prog); /* struct 成员注册（方法入队 + 属性符号） */
  pass2_types(sema, /*speculative=*/false);         /* 函数签名 */
  pass_globals(sema, prog, /*speculative=*/false);  /* comptime var + 全局变量 */

  /* Pass 3a：作用域树构建 + 控制流分析（符号注册、break/continue 位置检查、
     不可达语句、非 void 函数返回路径完整性）。快速失败：3a 有错误则
     不进入 3b——控制流/结构错误已使作用域树不可信，继续 shadow run
     只会产生级联的二次诊断。 */
  sema_build_scope_tree(sema);
  if (diag_has_error(sema->diag)) return false;

  /* Pass 3b：shadow VM 运行（按作用域树严格对应遍历，纯类型检查与推导）。
     队列驱动：遍历 sema->funcs，解析过程中队列可增长（局部函数提升 /
     泛型单态化追加到队尾），len 每次重取自动覆盖新函数。 */
  for (size_t i = 0; i < vec_len(sema->funcs); i++) {
    sema_func_t *sf = (sema_func_t *)vec_get(sema->funcs, i);
    /* comptime func 同样 shadow walk：body 内语句做类型检查 + 折叠
       （函数字面量分配 fid / 登记签名、comptime var 求值折叠）。不生成
       运行时字节码（compiler 预扫描跳过 comptime）——walk 只为编译期
       求值服务（调用点 CTFE 解释执行，sema_eval_comptime_call）。 */
    /* 函数字面量 body 内登记的局部函数：sema_check_func_literal 已同步
       walk 并标记，其 fscope 挂在临时作用域树上已销毁——跳过防二次 walk
       访问悬空作用域。 */
    if (sf->is_literal_owned) continue;
    sema_walk_function(sema, sf);
  }

  /* Pass 3b 期间按需生成的销毁函数（每个含 own 槽位的复合类型一个）：统一追加到
     程序函数链尾部 —— compiler 预扫描为它们构造函数对象 + 全局名字绑定，
     销毁点发的 `CALL __destroy_<n>` 才能解析到。 */
  /* 闭包 env 的 vtable：为每个含捕获的函数生成 `__destroy_<env>`（释放 env 的
     own 捕获）与 `__clone_<env>`（深拷贝 env），把 fid 记到 AST 上——compiler
     在 hoist 随 `SET_FUNC_CAPTURES` 一起写进函数对象，于是闭包的 clone/free
     走**编译期生成的函数**，不需要运行期类型内省（也不必用 __deep_clone）。 */
  if (!diag_has_error(sema->diag) && sema->closures) {
    size_t ncl = vec_len(sema->closures);
    for (size_t i = 0; i < ncl; i++) {
      ast_func_def_t *fd = (ast_func_def_t *)vec_get(sema->closures, i);
      if (!fd || fd->capture_type_id == 0) continue;
      const sema_type_t *ct = sema_type_by_id(sema, fd->capture_type_id);
      if (!ct || !ct->type) continue;
      uint32_t dfid = 0, cfid = 0;
      if (sema_gen_env_vtable(sema, ct->type, &dfid, &cfid)) {
        fd->env_dispose_fid = dfid;
        fd->env_clone_fid = cfid;
      }
    }
  }
  if (!diag_has_error(sema->diag)) {
    sema_attach_destroy_fns(sema, program);
    sema_attach_clone_fns(sema, program);
  }

  return !diag_has_error(sema->diag);
}
