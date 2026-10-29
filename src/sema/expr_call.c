#include "sema/sema.h"
#include "sema/reflect.h"
#include "vm/type_ownership.h"
#include "sema/struct_compose.h" /* 嵌入继承的成员名字查找 */
#include "core/string.h"
#include "parser/ast_array.h"
#include "parser/ast_binary.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_return.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_const.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_error.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_addr.h"
#include "parser/ast_pack_type.h"
#include "parser/ast_index.h"
#include "parser/ast_spread.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_member.h"
#include "parser/ast_new.h"
#include "parser/ast_deref.h"
#include "parser/ast_addr.h"
#include "parser/ast_slice_type.h"
#include "parser/ast_slice.h"
#include "parser/ast_make.h"
#include "parser/ast_nil.h"
#include "parser/ast_option.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_ternary.h"
#include "parser/ast_type_ref.h"
#include "parser/ast_typeof.h"
#include "parser/ast_alignof.h"
#include "parser/ast_sizeof.h"
#include "parser/ast_unary.h"
#include "parser/ast_undef.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_volatile.h"
#include "parser/ast_assign.h"
#include "parser/ast_var_def.h"
#include "parser/ast_block.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_clone.h"
#include "parser/ast_take.h"
#include "parser/ast_take_slot.h"
#include "parser/ast_for_index.h"
#include "parser/lexer.h"
#include "ctfe/ctfe.h"
#include "sema/comptime.h"
#include "sema/metadata.h"
#include "sema/sema_cross.h"
#include "sema/sema_generic.h"
#include "vm/function.h"
#include "vm/type_array.h"
#include "vm/type_enum.h"
#include "vm/type_error.h"
#include "vm/type_func.h"
#include "vm/type_cfunc.h"
#include "vm/type_option.h"
#include "vm/type_struct.h"
#include "vm/type_tuple.h"
#include "vm/type_union.h"
#include "vm/type_cunion.h"
#include "vm/type_int.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_opaque.h"
#include "vm/module.h"
#include "core/ffi_abi.h" /* M6 FFI：ffi_data_kind_of（extern var 的类型码） */
#include "core/ffi_abi.h"   /* M6 FFI：CLUX_FFI_RET_*（extern cfunc 的返回类型码） */
#include "core/strmap.h"

#include <stdio.h>
#include <string.h>


#include "sema_expr_internal.h"

/* M6 FFI：剥掉 const/volatile 限定符（它们是同一份数据的限定视图，C ABI 描述符
   与底层类型一致）。`const i32` 的实参就应该按 int32 传。 */
/* FFI 只看**表示层**：const/volatile 与 own/ref/fatal 都不改变 C 侧布局，
   一律剥到裸类型（所有权契约由 ffi_c_abi_reject_reason_own 单独判定）。 */
static const type_t *ffi_strip_quals(const type_t *t) {
  return type_strip_quals(t);
}

/* M6 FFI：实参类型能否传给该形参。
 *
 * clux 的标量与 C 是 **1:1** 的，调用层用 libffi 的精确 `ffi_type` 描述符，
 * 因此判据是"能否共用同一个描述符"：
 *   - 整数族（int/bool/enum）⇄ 整数族：宽度/符号可以不同（与 C 的隐式转换同义，
 *     都由整数寄存器承载）——所以 `malloc(16)` 的 i32 字面量喂 u64 形参合法；
 *   - 浮点 ⇄ 浮点：**要求同型**（f32⇄f32、f64⇄f64）。宽度不同不能共用一个
 *     描述符；要换精度请显式 `as`；
 *   - `opaque` 收 opaque 与各种指针/切片/str（C 里都是裸指针）；
 *   - `str` 收 `str`；指针形参收 opaque 与指针/切片；
 *   - 其它（结构体/cunion）：按值传参需要构造 struct 描述符，暂不支持 → 拒绝。
 * const/volatile 限定符先剥掉再判（视图不影响 ABI）。 */
static bool ffi_arg_acceptable(sema_t *sema, const type_t *pt_raw,
                               const type_t *at_raw) {
  const type_t *pt = ffi_strip_quals(pt_raw);
  const type_t *at = ffi_strip_quals(at_raw);
  if (!pt || !at) return false;
  bool p_int = type_kind_of(pt) == TYPE_KIND_INT || type_kind_of(pt) == TYPE_KIND_BOOL ||
               type_kind_of(pt) == TYPE_KIND_ENUM;
  bool a_int = type_kind_of(at) == TYPE_KIND_INT || type_kind_of(at) == TYPE_KIND_BOOL ||
               type_kind_of(at) == TYPE_KIND_ENUM;
  if (p_int) return a_int;
  if (type_kind_of(pt) == TYPE_KIND_FLOAT) return at == pt; /* 同型（f32/f64 不可混） */
  if (pt == sema->vm->type_opaque)
    return at == sema->vm->type_opaque || type_is_ptr(at) ||
           /* C 函数指针（cfunc(...)）在 C 里就是裸指针：老声明把它写成
              `opaque`（如 `cb: opaque`）时仍要能接受 `cfunc_ptr(f)`。 */
           type_kind_of(at) == TYPE_KIND_CFUNC ||
           /* 所有权迁移：at 是形参/实参类型（无对应 value），at->ownership
              恒为 OWN_NONE——切片不再经 ownership 放行进 opaque，只靠
              type_is_ptr/type_is_slice 的 kind 判定。保留 false 分支待
              值层 ownership 接入 FFI 实参匹配。 */
           (type_is_slice(at) && false) ||
           type_kind_of(at) == TYPE_KIND_STR;
  if (pt == sema->vm->type_str) return at == sema->vm->type_str;
  /* C 函数指针形参（`cfunc(...)`）：接受同型 cfunc、`opaque` 与各种指针
     （C 里都是裸函数指针/void*）。 */
  if (type_kind_of(pt) == TYPE_KIND_CFUNC)
    return at == pt || at == sema->vm->type_opaque || type_is_ptr(at) ||
           type_kind_of(at) == TYPE_KIND_CFUNC;
  /* 结构体/cunion 按值：要求**同型**（描述符按字段构造，布局必须完全一致）。
     所有权已经是类型的一部分：两侧都先剥到裸类型再比身份，于是
     `own *T` / `fatal *T` 与 `*T` 在 C ABI 表示上等价；`ref *T` 也按裸指针
     表示处理（C 侧看不到借用语义）。 */
  /* 指针形参：C 允许把 `T*` 传给 `const T*`（加 const 是隐式转换）。
     基类型按去掉 const/volatile 后比较；`opaque`（void*）可传给任意
     const 指针，这样 `memcmp(data(x), ...)` 这类零拷贝调用合法。 */
  if (type_is_ptr(pt)) {
    const type_t *pb = ffi_strip_quals(ptr_type_base(pt));
    /* `T *` → `const void *`（C 隐式转换）：基类型是 opaque 时接受任何指针。 */
    if (pb == sema->vm->type_opaque) return true;
    if (at == sema->vm->type_opaque) return true;
    if (type_is_ptr(at)) {
      const type_t *ab = ffi_strip_quals(ptr_type_base(at));
      if (ffi_arg_acceptable(sema, pb, ab)) return true;
    }
    return false;
  }
  return type_strip_ownership(pt) == type_strip_ownership(at);
}

/* M6 FFI：变参（`...`）位置的额外实参是否可搬运。整数族/浮点/指针/str 都可以
 * （浮点按 C 规则提升：f32→double）；结构体/cunion（按值）尚不支持。 */
static bool ffi_vararg_acceptable(sema_t *sema, const type_t *at_raw) {
  const type_t *at = ffi_strip_quals(at_raw);
  if (!at) return false;
  (void)sema;
  switch (at->kind) {
  case TYPE_KIND_INT:
  case TYPE_KIND_BOOL:
  case TYPE_KIND_ENUM:
  case TYPE_KIND_FLOAT:
  case TYPE_KIND_STR:
  case TYPE_KIND_OPAQUE:
  case TYPE_KIND_PTR:
  /* 变参也能按值传结构体/cunion（libffi 支持；C 对聚合类型不做默认提升） */
  case TYPE_KIND_STRUCT:
  case TYPE_KIND_CUNION:
    return true;
  default:
    return false; /* 切片/option/tag union/元组/闭包：变参位置不可搬运 */
  }
}

/* 统一的 C ABI 调用处理：直接 `lib::fn` 与 port 导入的 `port::fn` 共用。
 * `fs` 是 extern cfunc 的限定名符号（签名带真实所有权），`qual` 只用于诊断。
 * 填充 ast_call_t 的 FFI 字段（返回码 + 实际 C 符号 + 解析后的签名），
 * 逐个校验实参（类型 + own/fatal 契约），返回返回类型的 shadow。 */
static value_t *ex_ffi_call(sema_t *sema, ast_call_t *call, sema_symbol_t *fs,
                            const char *qual, sema_scope_t *scope) {
  if (!sema || !call || !fs || !fs->type) {
    diag_error(sema->diag, sema_loc(sema, (ast_node_t *)call),
               "extern call %s: missing extern cfunc signature",
               qual ? qual : "<unknown>");
    return val_void(sema);
  }
  const type_t *rt = func_type_return(fs->type);
  ast_func_def_t *fs_def =
      (fs->ast && fs->ast->kind == AST_FUNC_DEF)
          ? (ast_func_def_t *)fs->ast
          : NULL;
  /* 所有权修饰（`fatal opaque` / `fatal *T`）不是 C 表示的一部分：
     返回码按**裸类型**映射，值仍带修饰类型（所有权交回调用方）。 */
  const type_t *rt_core = type_strip_ownership(rt);
  uint32_t rk = CLUX_FFI_RET_VOID;
  if (!rt || rt_core == sema->vm->type_void) rk = CLUX_FFI_RET_VOID;
  else if (rt_core == sema->vm->type_i8) rk = CLUX_FFI_RET_I8;
  else if (rt_core == sema->vm->type_i16) rk = CLUX_FFI_RET_I16;
  else if (rt_core == sema->vm->type_i32) rk = CLUX_FFI_RET_I32;
  else if (rt_core == sema->vm->type_i64) rk = CLUX_FFI_RET_I64;
  else if (rt_core == sema->vm->type_u8) rk = CLUX_FFI_RET_U8;
  else if (rt_core == sema->vm->type_u16) rk = CLUX_FFI_RET_U16;
  else if (rt_core == sema->vm->type_u32) rk = CLUX_FFI_RET_U32;
  else if (rt_core == sema->vm->type_u64) rk = CLUX_FFI_RET_U64;
  else if (rt_core == vm_type_usize(sema->vm)) rk = CLUX_FFI_RET_USIZE;
  else if (rt_core == sema->vm->type_bool) rk = CLUX_FFI_RET_BOOL;
  else if (rt_core == sema->vm->type_f32) rk = CLUX_FFI_RET_F32;
  else if (rt_core == sema->vm->type_f64) rk = CLUX_FFI_RET_F64;
  else if (rt_core == sema->vm->type_opaque) rk = CLUX_FFI_RET_PTR;
  else if (rt_core == sema->vm->type_str) rk = CLUX_FFI_RET_STR;
  else if (rt && (type_kind_of(rt) == TYPE_KIND_STRUCT ||
                  type_kind_of(rt) == TYPE_KIND_CUNION ||
                  type_kind_of(rt) == TYPE_KIND_ENUM ||
                  type_kind_of(rt) == TYPE_KIND_CFUNC ||
                  type_is_ptr(rt_core))) {
    rk = CLUX_FFI_RET_TYPED;
    call->ffi_ret_type_expr = fs_def ? fs_def->return_expr : NULL;
  } else {
    diag_error(sema->diag, sema_loc(sema, &call->base),
               "extern call %s: return type is not marshalable", qual);
    return val_void(sema);
  }
  const func_sig_t *sig =
      &((const func_type_t *)type_strip_ownership(fs->type))->sig;
  call->is_ffi_call = true;
  call->ffi_ret_kind =
      CLUX_FFI_RET_PACK(rk, sig->is_variadic, sig->param_count);
  /* codegen 只需这两个字段：实际 C 符号名 + 已解析签名。直接 lib::fn 与
     port 导入的 port::fn 因此走完全相同的 lowering。 */
  call->ffi_sig = fs->type;
  call->ffi_c_name = fs_def ? fs_def->name : (strslice_t){0};
  call->ffi_callconv = fs_def ? fs_def->callconv : 0;

  size_t nargs = 0;
  for (ast_node_t *a = call->args; a; a = a->next) nargs++;
  if (!sig->is_variadic && nargs > sig->param_count)
    diag_error(sema->diag, sema_loc(sema, &call->base),
               "extern call %s: expected %zu argument(s), got %zu", qual,
               sig->param_count, nargs);

  size_t ai = 0;
  for (ast_node_t **slot = &call->args; *slot; slot = &(*slot)->next, ai++) {
    value_t *av = sema_expr(sema, slot, scope);
    if (!av || value_is_error(sema->vm, av) ||
        value_is_type(av, TYPE_KIND_VOID))
      continue;
    ast_node_t *a = *slot;
    const type_t *at = value_type(av);
    const type_t *pt =
        (ai < sig->param_count && sig->params) ? sig->params[ai] : NULL;
    if (!pt) {
      if (!ffi_vararg_acceptable(sema, at))
        diag_error(sema->diag, sema_loc(sema, a),
                   "extern call %s: variadic argument %zu: this type cannot "
                   "cross the C ABI", qual, ai + 1);
      continue;
    }
    /* own 形参必须接收将亡值（move/clone 的结果）；fatal 实参必须被 own
       形参接住。规则与普通调用的 func_shadow_call 一致。 */
    ownership_t pown = type_ownership_of(pt);
    ownership_t aown = value_type_ownership(av);
    if (pown == OWN_OWN && aown != OWN_FATAL) {
      char pn[64];
      sema_type_name(pt, pn, sizeof pn);
      diag_error(sema->diag, sema_loc(sema, a),
                 "extern call %s: argument %zu: cannot copy a value holding "
                 "owned resources into parameter of type %s (use move() or "
                 "clone(), or declare the parameter as 'ref *T' to borrow)",
                 qual, ai + 1, pn);
      continue;
    }
    if (aown == OWN_FATAL && pown != OWN_OWN) {
      char pn[64];
      sema_type_name(pt, pn, sizeof pn);
      diag_error(sema->diag, sema_loc(sema, a),
                 "extern call %s: argument %zu: a dying value (FATAL) must be "
                 "caught by an 'own' position; parameter of type %s does not "
                 "take ownership (bind it to an own variable, or pass it to an "
                 "own parameter)", qual, ai + 1, pn);
      continue;
    }
    if (!ffi_arg_acceptable(sema, pt, at))
      diag_error(sema->diag, sema_loc(sema, a),
                 "extern call %s: argument %zu: cannot pass this value as the "
                 "declared parameter type", qual, ai + 1);
  }
  return value_make_shadow(sema->vm, rt ? rt : sema->vm->type_void);
}

/* 从 `mod::Type` 形式的 AST_SCOPE_REF 解析被导入模块导出的 struct 类型。
 * type_expr 是 AST_IDENT（模块别名），variant 是类型名。
 * 直接从 module->exports 查 type value，取其指向的 type_t*。
 * 非 struct / 未找到返回 NULL。 */
const type_t *resolve_cross_module_struct(sema_t *sema,
                                                  ast_scope_ref_t *er,
                                                  sema_scope_t *scope) {
  if (!er || !er->type_expr || er->type_expr->kind != AST_IDENT) return NULL;
  ast_ident_t *id = (ast_ident_t *)er->type_expr;
  sema_symbol_t *sym = sema_lookup(scope, id->name);
  if (!sym || sym->kind != SEMA_SYM_MODULE || !sym->module) return NULL;
  module_t *mod = sym->module;
  if (!mod) return NULL;
  /* 阶段 1：依赖模块可能尚未 finalize（无 runtime exports），先查 sema 符号。 */
  sema_symbol_t *ms = sema_module_export_symbol(mod, er->variant);
  if (ms && ms->kind == SEMA_SYM_TYPE && ms->type &&
      type_kind_of(ms->type) == TYPE_KIND_STRUCT)
    return ms->type;
  if (!mod->exports) return NULL;
  char buf[256];
  if (er->variant.len >= sizeof(buf)) return NULL;
  memcpy(buf, er->variant.ptr, er->variant.len);
  buf[er->variant.len] = '\0';
  value_t *member = (value_t *)strmap_get(mod->exports, buf);
  if (!member || value_type(member) != sema->vm->type_type) return NULL;
  const type_t *t = value_as(member, const type_t *);
  if (!t || type_kind_of(t) != TYPE_KIND_STRUCT) return NULL;
  return t;
}

/* 沿 struct 嵌入链查找拥有指定属性名的 struct_type_t（自己优先，再沿 embeds
 * 深度优先）。运行时 attributes 查找用——跨模块时 sema member_scope 已销毁，
 * 只能依赖运行时 struct_type_t.attributes。 */
const struct_type_t *find_runtime_attr_owner(const type_t *t,
                                                    const char *name) {
  if (!t || type_kind_of(t) != TYPE_KIND_STRUCT) return NULL;
  const struct_type_t *st = (const struct_type_t *)t;
  if (st->attributes && strmap_contains(st->attributes, name))
    return st;
  size_t ne = 0;
  const type_t *const *emb = type_struct_embeds(t, &ne);
  for (size_t i = 0; i < ne; i++) {
    const type_t *eb = type_strip_quals(emb[i]);
    const struct_type_t *owner = find_runtime_attr_owner(eb, name);
    if (owner) return owner;
  }
  return NULL;
}

/* AST_CALL 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
/* ===========================================================================
 * 实参展开 `...operand`（sema 阶段）
 *
 *   fn(a, ...b, c)   ⇒   fn(a, b[0], b[1], …, c)
 *
 * 语义要点：
 *   - 操作数必须是**编译期长度已知**的数组或元组（`[N]T` / `<T1,T2,…>`）；
 *     切片（长度运行期才知道）一律拒绝。
 *   - 操作数必须是**可复读的位置表达式**（变量 / 成员访问）：展开后每个元素
 *     都是一次 `operand[i]` 读取，带副作用的表达式（调用/构造…）会被重复求值，
 *     所以这里直接拒绝，并要求先落一个临时变量。
 *   - 空数组展开出 **0 个实参**（`fn(a, ...empty, c)` ⇒ `fn(a, c)`）。
 *   - 展开是**就地**的（改写实参链），因此可重入：第二次调用时链中已无
 *     AST_SPREAD，直接返回。
 * =========================================================================== */
/* `...args` 展开到只含一个类型参数包的泛型函数时，直接以 Args 元素为
 * 显式类型实参，避免 `move(__pack_arg)` 的 fatal 值状态污染类型推导。 */
static ast_node_t *pack_spread_explicit_callee(sema_t *sema, ast_node_t *callee,
                                               const sema_generic_instance_t *gi,
                                               sema_scope_t *scope) {
  if (!callee || !gi || !gi->has_pack || gi->pack_is_value) return NULL;
  sema_symbol_t *gsym = NULL;
  if (callee->kind == AST_IDENT)
    gsym = sema_lookup(scope, ((ast_ident_t *)callee)->name);
  else if (callee->kind == AST_SCOPE_REF)
    gsym = sema_cross_generic_symbol(sema, callee);
  if (!gsym || !gsym->is_generic || !gsym->ast ||
      gsym->ast->kind != AST_FUNC_DEF)
    return NULL;
  ast_func_def_t *gdef = (ast_func_def_t *)gsym->ast;
  if (gdef->generic.count != 1) return NULL;
  ast_node_t *gp = gdef->generic.first;
  if (!gp || !((ast_generic_param_t *)gp)->is_pack) return NULL;

  ast_node_t *indices = NULL, *last = NULL;
  for (size_t i = 0; i < gi->pack_count; i++) {
    const sema_generic_arg_t *a = &gi->args[gi->pack_offset + i];
    ast_node_t *ref = sema_gen_type_ref(sema, a->type);
    if (!ref) return NULL;
    ref->next = NULL;
    if (last) last->next = ref;
    else indices = ref;
    last = ref;
  }
  ast_node_t *idx = ast_index_new(sema->arena, callee->tok_begin,
                                  callee->tok_end);
  if (!idx) return NULL;
  idx->is_generated = true;
  ((ast_index_t *)idx)->object = callee;
  ((ast_index_t *)idx)->indices = indices;
  ((ast_index_t *)idx)->indices_last = last;
  return idx;
}

static ast_node_t *value_pack_literal_chain(
    sema_t *sema, const sema_generic_instance_t *gi, uint32_t tb,
    uint32_t te) {
  ast_node_t *head = NULL, *tail = NULL;
  for (size_t i = 0; i < gi->pack_count; i++) {
    ast_node_t *e =
        sema_generic_value_pack_elem_expr(sema, gi, i, tb, te);
    if (!e) return NULL;
    e->next = NULL;
    if (tail) tail->next = e;
    else head = e;
    tail = e;
  }
  return head;
}

static bool sema_expand_spread_args(sema_t *sema, ast_call_t *call,
                                    sema_scope_t *scope) {
  ast_node_t **slot = &call->args;
  while (*slot) {
    ast_node_t *a = *slot;
    if (a->kind != AST_SPREAD) {
      slot = &a->next;
      continue;
    }
    ast_spread_t *sp = (ast_spread_t *)a;
    ast_node_t *op = sp->operand;
    /* `...N`（comptime 值参数包）：展开为编译期字面量序列。 */
    if (op && op->kind == AST_IDENT &&
        sema_generic_value_pack_is_param(sema,
                                        ((ast_ident_t *)op)->name)) {
      const sema_generic_instance_t *gi = sema->generics.active_inst;
      ast_node_t *head =
          value_pack_literal_chain(sema, gi, a->tok_begin, a->tok_end);
      if (head) {
        ast_node_t *tail = head;
        while (tail->next) tail = tail->next;
        tail->next = a->next;
        *slot = head;
        slot = &tail->next;
      } else {
        *slot = a->next;
      }
      continue;
    }
    /* `...args`（类型参数包）：编译期展开为隐藏形参；含 own 槽位的元素
       自动 move 交出所有权。 */
    if (op && op->kind == AST_IDENT &&
        sema_generic_pack_is_param(sema,
                                  ((ast_ident_t *)op)->name)) {
      const sema_generic_instance_t *gi = sema->generics.active_inst;
      if (call->callee && call->callee->kind != AST_INDEX) {
        ast_node_t *explicit_callee =
            pack_spread_explicit_callee(sema, call->callee, gi, scope);
        if (explicit_callee) call->callee = explicit_callee;
      }
      ast_node_t *head = NULL, *tail = NULL;
      for (size_t i = 0; i < gi->pack_count; i++) {
        ast_node_t *e = sema_generic_pack_arg_expr(
            sema, gi, i, a->tok_begin, a->tok_end);
        if (!e) return false;
        e->next = NULL;
        if (tail) tail->next = e;
        else head = e;
        tail = e;
      }
      if (head) {
        tail->next = a->next;
        *slot = head;
        slot = &tail->next;
      } else {
        *slot = a->next;
      }
      continue;
    }
    if (!op ||
        (op->kind != AST_IDENT && op->kind != AST_MEMBER)) {
      diag_error(sema->diag, sema_loc(sema, a),
                 "spread operand must be a variable or a field access "
                 "(it is read once per element; bind temporaries first)");
      return false;
    }
    /* 取类型：sema_expr 只做 shadow 求值（不产生运行期代码）；随后每个
       `operand[i]` 会各自重读一次——操作数是位置表达式，无副作用。 */
    value_t *v = sema_expr(sema, &sp->operand, scope);
    if (is_poison(sema, v)) return false;
    const type_t *t = value_type(v);
    size_t n = 0;
    if (t && type_kind_of(t) == TYPE_KIND_ARRAY) {
      n = array_type_len(t);
    } else if (t && type_kind_of(t) == TYPE_KIND_TUPLE) {
      n = tuple_type_elem_count(t);
    } else {
      char tn[64];
      sema_type_name(t, tn, sizeof(tn));
      diag_error(sema->diag, sema_loc(sema, a),
                 "spread operand must be an array or tuple with a "
                 "compile-time-known length, got %s",
                 tn);
      return false;
    }
    /* 构造 operand[0] … operand[n-1] 链 */
    ast_node_t *head = NULL, *tail = NULL;
    for (size_t i = 0; i < n; i++) {
      ast_node_t *idx = ast_index_new(sema->arena, a->tok_begin, a->tok_end);
      if (!idx) return false;
      idx->is_generated = true;
      ast_node_t *lit = ast_int_lit_new(sema->arena, a->tok_begin, a->tok_end);
      if (!lit) return false;
      lit->is_generated = true;
      ((ast_int_lit_t *)lit)->type = strslice_from_bytes("i32", 3);
      ((ast_int_lit_t *)lit)->value = i;
      ((ast_index_t *)idx)->object = sp->operand;
      ((ast_index_t *)idx)->indices = lit;
      ((ast_index_t *)idx)->indices_last = lit;
      if (tail) {
        tail->next = idx;
        tail = idx;
      } else {
        head = tail = idx;
      }
    }
    if (head) {
      tail->next = a->next;
      *slot = head;
      slot = &tail->next;
    } else {
      *slot = a->next; /* 空数组：0 个实参 */
    }
  }
  /* 裸值参数包 `f(N)`：comptime 序列没有标量值，按元素展开为用户决定的
     实参序列（要 tuple 时写 `.{ N }`）。`len(N)` 是内建特例，保持单实参
     由调用层折叠。 */
  bool is_len_call =
      call->callee && call->callee->kind == AST_IDENT &&
      strslice_eq(((ast_ident_t *)call->callee)->name,
                  strslice_from_cstr("len"));
  if (!is_len_call) {
    ast_node_t **slot = &call->args;
    while (*slot) {
      ast_node_t *a = *slot;
      if (a->kind == AST_IDENT &&
          sema_generic_value_pack_is_param(
              sema, ((ast_ident_t *)a)->name)) {
        const sema_generic_instance_t *gi = sema->generics.active_inst;
        ast_node_t *head =
            value_pack_literal_chain(sema, gi, a->tok_begin, a->tok_end);
        if (head) {
          ast_node_t *tail = head;
          while (tail->next) tail = tail->next;
          tail->next = a->next;
          *slot = head;
          slot = &tail->next;
        } else {
          *slot = a->next;
        }
        continue;
      }
      slot = &a->next;
    }
  }
  /* 重算 args_last（展开会改变链尾；O(n) 但只在 sema 跑一次） */
  ast_node_t *last = NULL;
  for (ast_node_t *a = call->args; a; a = a->next) last = a;
  call->args_last = last;
  return true;
}

/* M7 泛型调用收尾：callee 已是实例签名。
 *
 * `pre_args` 非 NULL 时表示实参已在推导阶段求值过一次（不得重复求值——
 * `move(x)` 第二次会报 used after move）；否则按实例签名逐参求值
 * （匿名构造可注入具体形参类型）。最后走与普通调用同一个 value_call
 * 校验/分派入口（shadow 分支只做类型检查，不执行）。
 */
static value_t *generic_call_finish(sema_t *sema, ast_call_t *call,
                                    const type_t *sig, uint32_t fid,
                                    sema_scope_t *scope, value_t **pre_args,
                                    size_t argc) {
  ast_node_t *site = (ast_node_t *)call;
  ast_node_t *ref =
      ast_func_ref_new(sema->arena, site->tok_begin, site->tok_end);
  if (!ref) return val_void(sema);
  ((ast_func_ref_t *)ref)->fid = fid;
  ref->is_generated = true;
  call->callee = ref;

  value_t *callee = value_make_shadow(sema->vm, sig);
  value_t *local[argc > 0 ? argc : 1];
  value_t **args = pre_args;
  if (!args) {
    size_t i = 0;
    for (ast_node_t **slot = &call->args; *slot; slot = &(*slot)->next, i++) {
      const type_t *pt = func_type_param(sig, i);
      bool pushed = sema_anon_push(sema, pt);
      local[i] = sema_expr(sema, slot, scope);
      sema_anon_pop(sema, pushed);
    }
    args = local;
  }
  value_t *result = value_call(sema->vm, callee, args, argc);
  if (value_is_error(sema->vm, result)) {
    error_data_t *ed = (error_data_t *)value_data(result);
    const char *msg = ed && ed->message ? ed->message
                                        : "function call failed";
    diag_error(sema->diag, sema_loc(sema, site), "%s", msg);
    return val_void(sema);
  }
  return result;
}

/* P4b：泛型方法调用 `recv.method(args)`。
 *
 * 方法实例不注册进 struct attributes（按名只能存一个），改为把调用改写成
 * 直接 CALL 实例 fid：实参 = [接收者, 原实参...]。接收者是值而 self 形参是
 * 指针时合成 `.&`（与 MEMBER_CALL 的接收者转换同义）。
 * 实参只求值一次（shadow 缓存给 value_call 校验）。 */
static value_t *ex_generic_method_call(sema_t *sema, ast_node_t **node,
                                       ast_call_t *call, ast_member_t *mem,
                                       sema_symbol_t *ms, const type_t *st,
                                       ast_node_t *type_args,
                                       sema_scope_t *scope, value_t *pre_rv) {
  (void)node;
  value_t *rv = pre_rv ? pre_rv : sema_expr(sema, &mem->object, scope);
  if (is_poison(sema, rv)) return val_void(sema);
  const type_t *rvt = value_type(rv);

  /* 显式实例化入口可能没有预先解析方法符号：用接收者类型补查。 */
  if (!st) st = rvt ? type_strip_quals(rvt) : NULL;
  if (!st) return val_void(sema);
  if (!ms) {
    const struct_type_t *owner = NULL;
    ms = sema_struct_find_member(sema, st, mem->field, call->callee, &owner);
  }
  if (!ms || ms->kind != SEMA_SYM_FUNC || !ms->ast) return val_void(sema);
  ast_func_def_t *mfn = (ast_func_def_t *)ms->ast;
  if (!mfn || mfn->generic.count == 0) return val_void(sema);

  /* self 形参是否指针（用于值接收者 → .& 转换）。 */
  ast_var_def_t *self_vd =
      (mfn->params && mfn->params->kind == AST_VAR_DEF)
          ? (ast_var_def_t *)mfn->params
          : NULL;
  bool self_ptr = self_vd && self_vd->type_expr &&
                  (self_vd->type_expr->kind == AST_PTR ||
                   self_vd->type_expr->kind == AST_SLICE_TYPE);

  ast_node_t *recv_expr = mem->object;
  value_t *recv_shadow = rv;
  if (self_ptr && rvt && !ptr_type_base(rvt)) {
    ast_node_t *addr = ast_addr_new(sema->arena, mem->object->tok_begin,
                                    mem->object->tok_end);
    if (!addr) return val_void(sema);
    ((ast_addr_t *)addr)->operand = mem->object;
    addr->is_generated = true;
    const type_t *pt = type_ptr_intern(sema->vm, type_strip_quals(rvt));
    const type_t *rt = pt ? type_ownership_apply(sema->vm, OWN_REF, pt) : NULL;
    if (!rt) return val_void(sema);
    sema_type_register(sema, rt);
    recv_expr = addr;
    recv_shadow = value_make_shadow(sema->vm, rt);
  }

  size_t argc = sema_count_siblings(call->args);
  value_t **shadows = (value_t **)allocator_new_ex(
      sema->vm->alloc, "value_t*", sizeof(value_t *), NULL, NULL, NULL,
      argc + 1);
  if (!shadows) return val_void(sema);
  shadows[0] = recv_shadow;
  size_t ai = 1;
  bool args_ok = true;
  for (ast_node_t **slot = &call->args; *slot; slot = &(*slot)->next, ai++) {
    shadows[ai] = sema_expr(sema, slot, scope);
    if (is_poison(sema, shadows[ai])) args_ok = false;
  }

  const sema_generic_instance_t *owner =
      sema_generic_find_type_instance(sema, st);
  const sema_generic_instance_t *inst = NULL;
  const type_t *sig = NULL;
  if (args_ok) {
    if (type_args) {
      sema_generic_arg_t *eargs = NULL;
      size_t nta = 0;
      if (!sema_generic_parse_explicit_args(sema, ms, type_args, &nta, &eargs)) {
        allocator_free(sema->vm->alloc, (void **)&shadows);
        return val_void(sema);
      }
      sig = sema_generic_instantiate_method_explicit(
          sema, ms, owner, st->name, eargs, nta, shadows, argc + 1,
          call->callee, &inst);
      if (eargs) allocator_free(sema->vm->alloc, (void **)&eargs);
    } else {
      sig = sema_generic_instantiate_method_inferred(
          sema, ms, owner, st->name, shadows, argc + 1, call->callee, &inst);
    }
  }
  if (!sig || !inst) {
    allocator_free(sema->vm->alloc, (void **)&shadows);
    return val_void(sema);
  }

  /* 改写成直接 CALL：callee = 实例 fid，args = [接收者, 原实参...]。 */
  recv_expr->next = call->args;
  call->args = recv_expr;
  call->is_member_call = false;
  value_t *r = generic_call_finish(sema, call, sig, inst->fid, scope, shadows,
                                   argc + 1);
  allocator_free(sema->vm->alloc, (void **)&shadows);
  return r;
}

/* 显式泛型方法调用 `recv.method[U](...)`：callee 是 AST_INDEX(object=MEMBER)。
 * 先探测方法符号；不是泛型方法时返回 false，交给普通下标/调用路径。 */
static bool try_explicit_generic_method_call(sema_t *sema, ast_node_t **node,
                                             ast_call_t *call,
                                             sema_scope_t *scope,
                                             value_t **out) {
  if (!call->callee || call->callee->kind != AST_INDEX) return false;
  ast_index_t *ix = (ast_index_t *)call->callee;
  if (!ix->object || ix->object->kind != AST_MEMBER) return false;
  ast_member_t *mem = (ast_member_t *)ix->object;
  value_t *rv = sema_expr(sema, &mem->object, scope);
  if (is_poison(sema, rv)) {
    *out = val_void(sema);
    return true;
  }
  const type_t *st = value_type(rv);
  if (!st) return false;
  st = type_strip_quals(st);
  const struct_type_t *owner = NULL;
  sema_symbol_t *ms = sema_struct_find_member(
      sema, st, mem->field, call->callee, &owner);
  if (!ms || ms->kind != SEMA_SYM_FUNC || !ms->ast ||
      ((ast_func_def_t *)ms->ast)->generic.count == 0)
    return false;
  *out = ex_generic_method_call(sema, node, call, mem, ms, st, ix->indices,
                                scope, rv);
  return true;
}


static value_t *ex_generic_static_method_call(sema_t *sema, ast_call_t *call,
                                              sema_symbol_t *ms,
                                              const type_t *st,
                                              sema_scope_t *scope) {
  size_t argc = sema_count_siblings(call->args);
  value_t **shadows = NULL;
  if (argc > 0) {
    shadows = (value_t **)allocator_new_ex(
        sema->vm->alloc, "value_t*", sizeof(value_t *), NULL, NULL, NULL, argc);
  }
  size_t ai = 0;
  bool ok = true;
  for (ast_node_t **slot = &call->args; *slot; slot = &(*slot)->next, ai++) {
    shadows[ai] = sema_expr(sema, slot, scope);
    if (is_poison(sema, shadows[ai])) ok = false;
  }
  const sema_generic_instance_t *owner =
      sema_generic_find_type_instance(sema, st);
  const sema_generic_instance_t *inst = NULL;
  const type_t *sig =
      ok ? sema_generic_instantiate_method_inferred(
               sema, ms, owner, st->name, shadows, argc, call->callee, &inst)
         : NULL;
  if (!sig || !inst) {
    if (shadows) allocator_free(sema->vm->alloc, (void **)&shadows);
    return val_void(sema);
  }
  call->is_member_call = false;
  value_t *r = generic_call_finish(sema, call, sig, inst->fid, scope, shadows,
                                   argc);
  if (shadows) allocator_free(sema->vm->alloc, (void **)&shadows);
  return r;
}

/* 显式泛型静态方法 `Type::method[U](...)`：callee 是
 * AST_INDEX(object=AST_SCOPE_REF(..., variant=method))。 */
static bool try_explicit_generic_static_method_call(sema_t *sema,
                                                    ast_node_t **node,
                                                    ast_call_t *call,
                                                    sema_scope_t *scope,
                                                    value_t **out) {
  (void)node;
  if (!call->callee || call->callee->kind != AST_INDEX) return false;
  ast_index_t *ix = (ast_index_t *)call->callee;
  if (!ix->object || ix->object->kind != AST_SCOPE_REF) return false;
  ast_scope_ref_t *er = (ast_scope_ref_t *)ix->object;
  if (!er->type_expr) return false;
  const type_t *st = resolve_type_expr(sema, er->type_expr);
  if (!st) return false;
  const struct_type_t *owner = NULL;
  sema_symbol_t *ms = sema_struct_find_member(sema, st, er->variant,
                                               call->callee, &owner);
  if (!ms || ms->kind != SEMA_SYM_FUNC || !ms->ast ||
      ((ast_func_def_t *)ms->ast)->generic.count == 0)
    return false;

  size_t argc = sema_count_siblings(call->args);
  value_t **shadows = NULL;
  if (argc > 0) {
    shadows = (value_t **)allocator_new_ex(
        sema->vm->alloc, "value_t*", sizeof(value_t *), NULL, NULL, NULL, argc);
    if (!shadows) return false;
  }
  size_t ai = 0;
  bool ok = true;
  for (ast_node_t **slot = &call->args; *slot; slot = &(*slot)->next, ai++) {
    shadows[ai] = sema_expr(sema, slot, scope);
    if (is_poison(sema, shadows[ai])) ok = false;
  }
  sema_generic_arg_t *eargs = NULL;
  size_t nta = 0;
  bool args_ok = ok &&
      sema_generic_parse_explicit_args(sema, ms, ix->indices, &nta, &eargs);
  const sema_generic_instance_t *owner_inst =
      sema_generic_find_type_instance(sema, st);
  const sema_generic_instance_t *inst = NULL;
  const type_t *sig =
      args_ok ? sema_generic_instantiate_method_explicit(
                    sema, ms, owner_inst, st->name, eargs, nta, shadows, argc,
                    call->callee, &inst)
              : NULL;
  if (eargs) allocator_free(sema->vm->alloc, (void **)&eargs);
  if (!sig || !inst) {
    if (shadows) allocator_free(sema->vm->alloc, (void **)&shadows);
    *out = val_void(sema);
    return true;
  }
  call->is_member_call = false;
  *out = generic_call_finish(sema, call, sig, inst->fid, scope, shadows, argc);
  if (shadows) allocator_free(sema->vm->alloc, (void **)&shadows);
  return true;
}


value_t *ex_call(sema_t *sema, ast_node_t **node, sema_scope_t *scope) {
      ast_call_t *call = (ast_call_t *)*node;
      /* 实参展开 `...b` 先于一切实参处理（方法调用 / FFI / 普通调用共用同一条
         实参链）。失败时诊断已记录。 */
      if (!sema_expand_spread_args(sema, call, scope)) return val_void(sema);
      {
        value_t *mv = NULL;
        if (sema_metadata_try_get(sema, node, scope, &mv))
          return mv ? mv : val_void(sema);
      }
      /* P9：`len(N)`（comptime 值参数包）折叠为元素个数常量。 */
      if (call->callee && call->callee->kind == AST_IDENT &&
          call->args && !call->args->next) {
        ast_ident_t *cid = (ast_ident_t *)call->callee;
        sema_symbol_t *cs = sema_lookup(scope, cid->name);
        const sema_generic_instance_t *gi = sema->generics.active_inst;
        bool is_value_pack =
            gi && gi->has_pack && gi->pack_is_value &&
            call->args->kind == AST_IDENT &&
            strslice_eq(((ast_ident_t *)call->args)->name, gi->pack_name);
        bool is_type_pack =
            gi && gi->has_pack && !gi->pack_is_value &&
            call->args->kind == AST_IDENT &&
            sema_generic_pack_is_param(
                sema, ((ast_ident_t *)call->args)->name);
        if (cs && cs->builtin_op == SEMA_BUILTIN_LEN &&
            (is_value_pack || is_type_pack)) {
          ast_node_t *lit = ast_int_lit_new(sema->arena, (*node)->tok_begin,
                                            (*node)->tok_end);
          if (!lit) return val_void(sema);
          ((ast_int_lit_t *)lit)->value = gi->pack_count;
          ((ast_int_lit_t *)lit)->type = strslice_from_cstr("u64");
          lit->is_generated = true;
          lit->next = (*node)->next;
          *node = lit;
          return value_make_shadow(sema->vm, sema->vm->type_u64);
        }
      }
      /* `T.<key>(i)`：type value 上的反射调用，先于方法调用解析。 */
      {
        value_t *rsv = NULL;
        if (reflect_member_call(sema, node, scope, &rsv))
          return rsv ? rsv : val_void(sema);
      }
      /* 显式泛型方法调用 `recv.method[U](...)`。 */
      {
        value_t *gmv = NULL;
        if (try_explicit_generic_method_call(sema, node, call, scope, &gmv))
          return gmv ? gmv : val_void(sema);
      }
      /* 显式泛型静态方法 `Type::method[U](...)`。 */
      {
        value_t *gsv = NULL;
        if (try_explicit_generic_static_method_call(sema, node, call, scope,
                                                    &gsv))
          return gsv ? gsv : val_void(sema);
      }
      /* ---- M7 泛型函数调用 ----
       *   f[i32, f64](args)   显式类型实参（callee 是 AST_INDEX）
       *   f(args)             调用点推导（P3）
       * 两条路径共用：先确定类型实参 → 单态化 → 把 callee 改写成
       * AST_FUNC_REF（实例 fid）→ 同一个 value_call 校验入口。 */
      {
        sema_symbol_t *gsym = NULL;
        ast_index_t *gix = NULL;
        if (call->callee && call->callee->kind == AST_INDEX) {
          gix = (ast_index_t *)call->callee;
          if (gix->object && gix->object->kind == AST_IDENT)
            gsym = sema_lookup(scope, ((ast_ident_t *)gix->object)->name);
          else if (gix->object && gix->object->kind == AST_SCOPE_REF)
            gsym = sema_cross_generic_symbol(sema, gix->object);
        } else if (call->callee && call->callee->kind == AST_IDENT) {
          gsym = sema_lookup(scope, ((ast_ident_t *)call->callee)->name);
        } else if (call->callee && call->callee->kind == AST_SCOPE_REF) {
          gsym = sema_cross_generic_symbol(sema, call->callee);
        }
        if (gsym && gsym->is_generic && gsym->ast &&
            gsym->ast->kind == AST_FUNC_DEF) {
          ast_func_def_t *gdef = (ast_func_def_t *)gsym->ast;
          size_t ntp = gdef ? gdef->generic.count : 0;
          size_t nta = 0;
          sema_generic_arg_t *eargs = NULL;
          bool targs_ok = true;
          if (gix && gix->indices)
            targs_ok = sema_generic_parse_explicit_args(
                sema, gsym, gix->indices, &nta, &eargs);

          size_t argc = sema_count_siblings(call->args);

          /* 参数包：固定泛型参数个数 / 固定值参数个数。 */
          size_t fixed_ng = 0;
          bool has_pack = false;
          bool pack_is_value = false;
          if (gdef) {
            for (ast_node_t *gp = gdef->generic.first; gp; gp = gp->next) {
              if (((ast_generic_param_t *)gp)->is_pack) {
                has_pack = true;
                pack_is_value =
                    ((ast_generic_param_t *)gp)->is_comptime;
                break;
              }
              fixed_ng++;
            }
          }
          /* comptime 值参数包无法从运行期实参推导：必须显式给出。 */
          if (pack_is_value && !gix) {
            diag_error(sema->diag, sema_loc(sema, call->callee),
                       "comptime value parameter pack requires explicit "
                       "generic arguments at the call site");
            return val_void(sema);
          }
          size_t fixed_vp = 0;
          if (gdef) {
            for (ast_node_t *p = gdef->params; p; p = p->next) {
              if (p->kind == AST_VAR_DEF &&
                  ((ast_var_def_t *)p)->type_expr &&
                  ((ast_var_def_t *)p)->type_expr->kind == AST_PACK_TYPE)
                break;
              fixed_vp++;
            }
          }
          size_t extra = argc > fixed_vp ? argc - fixed_vp : 0;
          bool args_ready =
              has_pack
                  ? (pack_is_value
                         ? (nta >= fixed_ng)
                         : (nta >= fixed_ng && nta - fixed_ng == extra))
                  : (nta == ntp);

          const sema_generic_instance_t *inst = NULL;
          const type_t *isig = NULL;

          if (targs_ok && args_ready) {
            /* 实参齐全：先实例化，再按具体签名求值实参（匿名构造可用）。 */
            isig = sema_generic_instantiate_func(sema, gsym, eargs, nta,
                                                 call->callee, &inst);
            if (eargs) allocator_free(sema->vm->alloc, (void **)&eargs);
            if (!isig || !inst) return val_void(sema);
            return generic_call_finish(sema, call, isig, inst->fid, scope, NULL,
                                       argc);
          }

          /* 部分/无显式实参：实参只求值一次 → 结构化推导 → 实例化 →
             用缓存下来的 shadow 完成调用校验。 */
          value_t **shadows = NULL;
          if (argc > 0) {
            shadows = (value_t **)allocator_new_ex(
                sema->vm->alloc, "value_t*", sizeof(value_t *), NULL, NULL, NULL,
                argc);
          }
          size_t ai = 0;
          bool args_ok = true;
          for (ast_node_t **slot = &call->args; *slot;
               slot = &(*slot)->next, ai++) {
            shadows[ai] = sema_expr(sema, slot, scope);
            if (is_poison(sema, shadows[ai])) args_ok = false;
          }
          if (targs_ok && args_ok)
            isig = sema_generic_instantiate_inferred(
                sema, gsym, eargs, nta, shadows, argc, call->callee, &inst);
          if (eargs) allocator_free(sema->vm->alloc, (void **)&eargs);
          if (!isig || !inst) {
            if (shadows) allocator_free(sema->vm->alloc, (void **)&shadows);
            return val_void(sema);
          }
          value_t *r = generic_call_finish(sema, call, isig, inst->fid, scope,
                                           shadows, argc);
          if (shadows) allocator_free(sema->vm->alloc, (void **)&shadows);
          return r;
        }
      }

      /* 实参位置的 `cfunc(...)` → `func(...)`：先按被调函数签名把 cfunc 指针
         实参替换成生成的包装字面量（与变量初始化同一条路径）。只在 callee 是
         普通函数名时做；方法调用/FFI 各有自己的签名解析路径。 */
      if (call->callee && call->callee->kind == AST_IDENT && call->args) {
        sema_symbol_t *cs =
            sema_lookup(scope, ((ast_ident_t *)call->callee)->name);
        if (cs && cs->kind == SEMA_SYM_FUNC && cs->type &&
            type_kind_of(cs->type) == TYPE_KIND_FUNC) {
          size_t pi = 0;
          for (ast_node_t **slot = &call->args; *slot;
               slot = &(*slot)->next, pi++) {
            const type_t *pt = func_type_param(cs->type, pi);
            if (pt && type_kind_of(pt) == TYPE_KIND_FUNC)
              sema_maybe_wrap_cfunc_expr(sema, slot, pt, scope);
          }
        }
      }
      /* ---- 方法调用糖（结构体命名空间成员）----
       *   a.b(args)   —— 实例方法：receiver = a（值或指针），self 隐式首参
       *   T::f(args)  —— 静态方法：receiver = T 的 type value，无 self
       *
       * sema 解析出方法名 + 方法所在的 struct 类型后，置 is_member_call +
       * member_type_id，compiler 据此发 `MEMBER_CALL <STR name> <U32 argc>`
       * （不复用 CALL）。receiver 由 compiler 编译 callee 的 object（实例方法）
       * 或 type_expr（静态方法）压栈，VM 从 receiver 的 struct 类型 attributes
       * 中查方法 func value 并调用。
       *
       * `self` **不是关键字**——它只是普通参数名，第一个参数按位置就是接收者。
       * 实例方法的接收者传递由 VM 的 op_member_call 完成（值 → 取地址 ref *T，
       * 指针 → 原样），sema 不再改写 AST。 */
      if (call->callee && call->callee->kind == AST_MEMBER) {
        ast_member_t *mem = (ast_member_t *)call->callee;
        const type_t *ot = NULL;
        if (mem->object && mem->object->kind == AST_IDENT) {
          sema_symbol_t *os =
              sema_lookup(scope, ((ast_ident_t *)mem->object)->name);
          if (os && os->type) ot = os->type;
        } else {
          value_t *ov = sema_expr(sema, &mem->object, scope);
          if (ov && !value_is_error(sema->vm, ov)) ot = value_type(ov);
        }
        const bool by_ptr = ot && ptr_type_base(ot) != NULL;
        const type_t *st = type_strip_quals(by_ptr ? ptr_type_base(ot) : ot);
        if (st && type_kind_of(st) == TYPE_KIND_STRUCT && st->name.len) {
          /* 在 struct 类型的 member_scope 中查方法名（自己优先 + 嵌入链递归） */
          const struct_type_t *owner = NULL;
          sema_symbol_t *ms = sema_struct_find_member(sema, st, mem->field,
                                                      call->callee, &owner);
          /* 以名字命中、但不是函数（自己的 var 成员 / 继承来的静态属性）：
             明确说清"它是变量，不是方法"，而不是落到下面的字段诊断。 */
          if (ms && ms->kind != SEMA_SYM_FUNC) {
            diag_error(sema->diag, sema_loc(sema, call->callee),
                       "cannot call '%.*s': it is a struct member variable, "
                       "not a method",
                       (int)mem->field.len, mem->field.ptr);
            return val_void(sema);
          }
          if (ms && ms->kind == SEMA_SYM_FUNC && ms->is_generic) {
            return ex_generic_method_call(sema, node, call, mem, ms, st,
                                          NULL, scope, NULL);
          }
          if (ms && ms->kind == SEMA_SYM_FUNC) {
            /* 标记为 member_call：compiler 发 MEMBER_CALL，不改写 AST */
            const sema_type_t *reg = sema_type_find(sema, st);
            if (reg) {
              call->is_member_call = true;
              call->member_type_id = reg->id;
            }
          } else if (!ms) {
            /* 跨模块回退：sema_struct_find_member 找不到 member_scope（源模块
               sema 已销毁），从运行时 struct_type_t.attributes 查方法。 */
            char fbuf[256];
            size_t fl = mem->field.len < sizeof(fbuf) - 1
                            ? mem->field.len : sizeof(fbuf) - 1;
            memcpy(fbuf, mem->field.ptr, fl);
            fbuf[fl] = '\0';
            {
              const type_t *rt = vm_type_load(sema->vm, st->id);
              if (rt && type_kind_of(rt) == TYPE_KIND_STRUCT) st = rt;
            }
            const struct_type_t *rt_owner = find_runtime_attr_owner(st, fbuf);
            if (rt_owner && rt_owner->attributes) {
              value_t *mval = (value_t *)strmap_get(rt_owner->attributes,
                                                    fbuf);
              if (mval && value_type(mval) &&
                  type_kind_of(value_type(mval)) == TYPE_KIND_FUNC) {
                /* 跨模块类型已由被导入模块注册并 vm_type_bind，直接用 t->id */
                if (rt_owner->base.id != 0) {
                  call->is_member_call = true;
                  call->member_type_id = rt_owner->base.id;
                }
              }
            }
          }
        }
      }
      /* ---- 泛型类型的静态方法：`Vec[i32]::new()` ----
       * callee 的 type_expr 是 AST_INDEX（parser 与下标共用）。先实例化得到
       * 类型，再把 type_expr 改写成实例的**运行时类型名**（mangling 名，已在
       * 全局作用域登记为 type 符号）——后续静态方法分支与 compiler 的
       * `PUSH <name>` 都按普通具名类型处理。 */
      if (!call->is_member_call && call->callee &&
          call->callee->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *ger = (ast_scope_ref_t *)call->callee;
        if (ger->type_expr && ger->type_expr->kind == AST_INDEX) {
          const type_t *gt = resolve_type_expr(sema, ger->type_expr);
          if (!gt) return val_void(sema);
          ast_node_t *id = ast_ident_new(sema->arena,
                                         ger->type_expr->tok_begin,
                                         ger->type_expr->tok_end);
          if (!id) return val_void(sema);
          ((ast_ident_t *)id)->name = gt->name;
          id->is_generated = true;
          ger->type_expr = id;
        }
      }

      /* ---- 静态方法调用 `T::f(args)` ----
       * callee 是 AST_SCOPE_REF，type_expr 解析到 SEMA_SYM_TYPE（struct），
       * variant 是方法名。sema 查 member_scope 命中函数 → 置 is_member_call。
       * compiler 编译 type_expr 压 type value（receiver），发 MEMBER_CALL。
       *
       * 跨模块 `mod::Type::f(args)`：type_expr 是嵌套 AST_SCOPE_REF（mod::Type）。
       * 被导入模块已 exec_run，运行时 struct_type_t.attributes / methods 已填充。
       * 从运行时 attributes 查方法 func value，取其签名类型做参数校验。 */
      if (!call->is_member_call && call->callee &&
          call->callee->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *er = (ast_scope_ref_t *)call->callee;
        if (er->type_expr && er->type_expr->kind == AST_SCOPE_REF) {
          /* ---- 跨模块静态方法 `mod::Type::f(args)` ---- */
          ast_scope_ref_t *inner_er = (ast_scope_ref_t *)er->type_expr;
          const type_t *st = resolve_cross_module_struct(sema, inner_er,
                                                         scope);
          if (!st) {
            diag_error(sema->diag, sema_loc(sema, call->callee),
                       "cross-module static method call: '%.*s::%.*s' is not "
                       "a struct type",
                       (int)inner_er->variant.len, inner_er->variant.ptr,
                       (int)er->variant.len, er->variant.ptr);
            return val_void(sema);
          }
          char mbuf[256];
          size_t mn = er->variant.len < sizeof(mbuf) - 1
                          ? er->variant.len : sizeof(mbuf) - 1;
          memcpy(mbuf, er->variant.ptr, mn);
          mbuf[mn] = '\0';
          if (inner_er->type_expr &&
              inner_er->type_expr->kind == AST_IDENT) {
            sema_symbol_t *modsym = sema_lookup(
                scope, ((ast_ident_t *)inner_er->type_expr)->name);
            if (modsym && modsym->kind == SEMA_SYM_MODULE && modsym->module &&
                !modsym->module->finalized &&
                sema->module_finalize.finalize) {
              if (!sema->module_finalize.finalize(sema->module_finalize.user,
                                                  modsym->module))
                return val_void(sema);
            }
          }
          {
            const type_t *rt = vm_type_load(sema->vm, st->id);
            if (rt && type_kind_of(rt) == TYPE_KIND_STRUCT) st = rt;
          }
          /* 优先用 sema member_scope 的签名（runtime attributes 可能尚未
             建好）；跨模块静态方法的参数/返回类型以 sema 为准。 */
          if (inner_er->type_expr &&
              inner_er->type_expr->kind == AST_IDENT) {
            sema_symbol_t *modsym = sema_lookup(
                scope, ((ast_ident_t *)inner_er->type_expr)->name);
            if (modsym && modsym->kind == SEMA_SYM_MODULE && modsym->module) {
              sema_symbol_t *tsym = sema_module_export_symbol(
                  modsym->module, inner_er->variant);
              sema_symbol_t *ms = NULL;
              if (tsym && tsym->kind == SEMA_SYM_TYPE && tsym->member_scope)
                ms = sema_scope_find_local(tsym->member_scope, er->variant);
              if (ms && ms->kind == SEMA_SYM_FUNC) {
                call->is_member_call = true;
                call->member_type_id = st->id;
              }
            }
          }
          const struct_type_t *owner =
              call->is_member_call ? NULL : find_runtime_attr_owner(st, mbuf);
          if (!owner && !call->is_member_call) {
            diag_error(sema->diag, sema_loc(sema, call->callee),
                       "struct '%.*s' has no method '%.*s'",
                       (int)st->name.len, st->name.ptr,
                       (int)er->variant.len, er->variant.ptr);
            return val_void(sema);
          }
          value_t *mval = (owner && owner->attributes)
              ? (value_t *)strmap_get(owner->attributes, mbuf) : NULL;
          if (mval && value_type(mval) &&
              type_kind_of(value_type(mval)) == TYPE_KIND_FUNC) {
            /* 跨模块类型已由被导入模块注册并 vm_type_bind，直接用 t->id。
               不调 sema_type_register——会覆盖 t->id 破坏原有绑定。 */
            if (owner && owner->base.id != 0) {
              call->is_member_call = true;
              call->member_type_id = owner->base.id;
            }
          }
        } else {
        /* type_expr 可能是 AST_IDENT（首次）或 AST_TYPE_REF（被 ex_scope_ref
           折叠过）——两种情况都取类型名查全局符号。 */
        strslice_t tn = STRSLICE_EMPTY;
        if (er->type_expr && er->type_expr->kind == AST_IDENT)
          tn = ((ast_ident_t *)er->type_expr)->name;
        else if (er->type_expr && er->type_expr->kind == AST_TYPE_REF)
          tn = ((ast_type_ref_t *)er->type_expr)->name;
        if (tn.ptr) {
          sema_symbol_t *sym = sema_lookup(scope, tn);
          if (sym && sym->kind == SEMA_SYM_TYPE && sym->type &&
              type_kind_of(sym->type) == TYPE_KIND_STRUCT) {
            const struct_type_t *owner = NULL;
            sema_symbol_t *ms = sema_struct_find_member(sema, sym->type,
                                                        er->variant,
                                                        call->callee, &owner);
            if (ms && ms->kind == SEMA_SYM_FUNC && ms->is_generic) {
              return ex_generic_static_method_call(sema, call, ms, sym->type,
                                                   scope);
            }
            if (ms && ms->kind == SEMA_SYM_FUNC) {
              const sema_type_t *reg = sema_type_find(sema, sym->type);
              if (reg) {
                call->is_member_call = true;
                call->member_type_id = reg->id;
              }
            }
          }
        }
        }
      }
      /* 函数调用：统一路径 = exec callee（shadow 求值）→ exec 各实参 →
         value_call 分派到 func_vcall 的 shadow 分支（唯一校验点：参数
         数量/隐式转换，不执行 cfunc）→ 错误翻译为诊断。
         遮蔽/可见性全部交给作用域机制：callee 是 AST_IDENT 时经
         sema_expr → AST_IDENT 分支自然解析（局部变量遮蔽函数名 → 变量
         符号 → 下方 func 类型检查报"不可调用"；兄弟/自身函数 → 该分支
         闭包检查拦截；未定义 → "undefined variable"）。callee 是函数值
         表达式（get_fn()()、函数字面量）时递归求值 shadow（可能触发内层
         comptime 折叠）。
         唯一保留的符号感知前置：comptime func 调用点折叠（真实调用点
         CTFE 求值；walking_comptime 时实参是参数 shadow value 无法求值，
         只构造 callee shadow 做类型检查，折叠留到真实调用点）。 */
      if (!call->callee) {
        diag_error(sema->diag, sema_loc(sema, &call->base),
                   "M1: missing callee");
        return val_void(sema);
      }

      /* ---- member_call 提前返回 ----
       * is_member_call 为 true 时，callee 不走 shadow 求值（它是 AST_MEMBER/
       * AST_SCOPE_REF，不是函数值表达式）。方法签名已在 member_scope 注册时由
       * pass2 解析；此处从符号取签名，做实参校验（含接收者），返回 return_type
       * shadow。compiler 据此发 MEMBER_CALL。
       *
       * 跨模块方法：源模块 sema 已销毁，sema_struct_find_member 找不到 member_scope，
       * 从运行时 struct_type_t.attributes 取方法 func value 的签名类型做校验。 */
      if (call->is_member_call) {
        /* 从 member_type_id 对应的 struct 类型 + 方法名查方法符号。
           编译期 struct 类型可能尚未 vm_type_bind（SEAL 在运行时执行），
           先用 sema_type_by_id 查 sema 登记的类型指针；失败再 fallback
           到 vm_type_load（跨模块已 bind 的类型）。 */
        const type_t *st = NULL;
        const sema_type_t *sreg = sema_type_by_id(sema, call->member_type_id);
        if (sreg) st = sreg->type;
        if (!st) st = vm_type_load(sema->vm, call->member_type_id);
        if (!st) return val_void(sema);
        strslice_t mname = {0};
        if (call->callee->kind == AST_MEMBER)
          mname = ((ast_member_t *)call->callee)->field;
        else if (call->callee->kind == AST_SCOPE_REF)
          mname = ((ast_scope_ref_t *)call->callee)->variant;
        const struct_type_t *owner = NULL;
        sema_symbol_t *msym = sema_struct_find_member(sema, st, mname,
                                                      call->callee, &owner);
        const type_t *sig_type = NULL;  /* 方法签名类型（func_type_t） */
        if (msym && msym->kind == SEMA_SYM_FUNC && msym->type) {
          sig_type = msym->type;
        } else if (call->callee->kind == AST_SCOPE_REF) {
          /* 跨模块静态方法：当前 global 没有该类型的 member_scope，
             从模块 sema_exports 的类型符号里取方法签名。 */
          ast_scope_ref_t *cr = (ast_scope_ref_t *)call->callee;
          if (cr->type_expr && cr->type_expr->kind == AST_SCOPE_REF) {
            ast_scope_ref_t *inner = (ast_scope_ref_t *)cr->type_expr;
            if (inner->type_expr && inner->type_expr->kind == AST_IDENT) {
              sema_symbol_t *modsym = sema_lookup(
                  scope, ((ast_ident_t *)inner->type_expr)->name);
              if (modsym && modsym->kind == SEMA_SYM_MODULE && modsym->module) {
                sema_symbol_t *tsym = sema_module_export_symbol(
                    modsym->module, inner->variant);
                if (tsym && tsym->kind == SEMA_SYM_TYPE && tsym->member_scope) {
                  sema_symbol_t *meth =
                      sema_scope_find_local(tsym->member_scope, mname);
                  if (meth && meth->kind == SEMA_SYM_FUNC && meth->type)
                    sig_type = meth->type;
                }
              }
            }
          }
        }
        if (!sig_type) {
          /* 跨模块回退：从运行时 attributes 取方法 func value 的签名 */
          char mbuf[256];
          size_t ml = mname.len < sizeof(mbuf) - 1 ? mname.len : sizeof(mbuf) - 1;
          memcpy(mbuf, mname.ptr, ml);
          mbuf[ml] = '\0';
          const struct_type_t *rt_owner = find_runtime_attr_owner(st, mbuf);
          if (rt_owner && rt_owner->attributes) {
            value_t *mval = (value_t *)strmap_get(rt_owner->attributes, mbuf);
            if (mval && value_type(mval) &&
                type_kind_of(value_type(mval)) == TYPE_KIND_FUNC)
              sig_type = value_type(mval);
          }
        }
        if (!sig_type) return val_void(sema);
        const func_sig_t *sig = &((const func_type_t *)type_strip_ownership(sig_type))->sig;
        /* 实参校验（参数数量 + 隐式转换），与 value_call 的 shadow 校验同构。
           实例方法：第一个参数是 self（接收者），实参从 args 开始对齐 self
           之后的形参。静态方法：无 self，实参从 args 开始对齐全部形参。 */
        bool is_instance = (call->callee->kind == AST_MEMBER);
        size_t nargs = sema_count_siblings(call->args);
        size_t expected = sig->param_count - (is_instance ? 1 : 0);
        if (!sig->is_variadic && nargs != expected) {
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "method '%.*s': expected %zu argument(s), got %zu",
                     (int)mname.len, mname.ptr, expected, nargs);
          return val_void(sema);
        }
        /* 逐实参求值（shadow）——匿名构造注入形参类型 */
        size_t ai = 0;
        for (ast_node_t **slot = &call->args; *slot;
             slot = &(*slot)->next, ai++) {
          const type_t *pt = (is_instance)
              ? func_type_param(sig_type, ai + 1)  /* skip self */
              : func_type_param(sig_type, ai);
          if (pt && type_kind_of(pt) == TYPE_KIND_FUNC)
            sema_maybe_wrap_cfunc_expr(sema, slot, pt, scope);
          bool ct_pushed = sema_anon_push(sema, pt);
          sema_expr(sema, slot, scope);
          sema_anon_pop(sema, ct_pushed);
        }
        const type_t *rt = func_type_return(sig_type);
        value_t *sv = value_make_shadow(sema->vm, rt ? rt : sema->vm->type_void);
        /* 返回值的所有权已编码在返回类型里（shadow 直接按 rt 构造）。 */
        return sv;
      }

      /* M6 FFI：extern cfunc 调用（`<库>::<名>(...)`）——callee 是 AST_SCOPE_REF 且库
         别名解析为 SEMA_SYM_EXTERN_LIB 时：**不做** value_call（那里没有运行时函数
         对象），置 AST 标志供 compiler 发 EXTERN_CALL（契约见 ast_call.h），并返回
         返回类型的 shadow 作为本表达式的类型。限定名符号由 pass1_names 注册
         （步骤 ①，含 §2 C ABI 校验）。 */
      if (call->callee->kind == AST_SCOPE_REF) {
        ast_scope_ref_t *er = (ast_scope_ref_t *)call->callee;
        if (er->type_expr && er->type_expr->kind == AST_IDENT) {
          ast_ident_t *lib_id = (ast_ident_t *)er->type_expr;
          sema_symbol_t *lsym = sema_lookup(scope, lib_id->name);
          /* port 导入：`port::fn` 的导出符号是 extern cfunc（其 ast 是
             port 里的 extern 声明）。直接复用 ex_ffi_call，无需重新声明。 */
          if (lsym && lsym->kind == SEMA_SYM_MODULE && lsym->module) {
            sema_symbol_t *msym =
                sema_module_export_symbol(lsym->module, er->variant);
            if (msym && msym->kind == SEMA_SYM_FUNC && msym->ast &&
                msym->ast->kind == AST_FUNC_DEF &&
                ((ast_func_def_t *)msym->ast)->is_extern_cfunc) {
              char qual[512];
              int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                                (int)lib_id->name.len, lib_id->name.ptr,
                                (int)er->variant.len, er->variant.ptr);
              if (qn > 0 && (size_t)qn < sizeof(qual))
                return ex_ffi_call(sema, call, msym, qual, scope);
            }
            if (msym && msym->kind == SEMA_SYM_EXTERN_VAR) {
              diag_error(sema->diag, sema_loc(sema, &call->base),
                         "'%.*s' is an extern var (data symbol); read it "
                         "without call syntax",
                         (int)er->variant.len, er->variant.ptr);
              return val_void(sema);
            }
          }
          if (lsym && lsym->kind == SEMA_SYM_EXTERN_LIB) {
            char qual[512];
            int qn = snprintf(qual, sizeof(qual), "%.*s::%.*s",
                              (int)lib_id->name.len, lib_id->name.ptr,
                              (int)er->variant.len, er->variant.ptr);
            sema_symbol_t *fs = (qn > 0 && (size_t)qn < sizeof(qual))
                ? sema_lookup(sema->global_scope,
                              strslice_from_bytes(qual, (size_t)qn))
                : NULL;
            if (!fs || fs->kind != SEMA_SYM_FUNC || !fs->type) {
              diag_error(sema->diag, sema_loc(sema, &call->base),
                         "extern call %.*s: no matching `extern cfunc` declaration",
                         qn > 0 ? qn : 0, qual);
              return val_void(sema);
            }
            const type_t *rt = func_type_return(fs->type);
            /* 所有权修饰（`fatal opaque` / `fatal *T`）不是 C 表示的一部分：
               返回码按**裸类型**映射，值仍带修饰类型（所有权交回调用方）。 */
            const type_t *rt_core = type_strip_ownership(rt);
            /* 返回类型码：**精确到类型**（vm/bcode.h 的 CLUX_FFI_RET_*）。
               clux 标量与 C 是 1:1 的，调用层用 libffi 的精确描述符，因此返回值
               的类型必须与源码声明完全一致（旧编码把 u64 返回造成 i64，使用点会
               在比较/赋值时撞上隐式转换）。 */
            uint32_t rk = CLUX_FFI_RET_VOID;
            if (!rt || rt_core == sema->vm->type_void) rk = CLUX_FFI_RET_VOID;
            else if (rt_core == sema->vm->type_i8) rk = CLUX_FFI_RET_I8;
            else if (rt_core == sema->vm->type_i16) rk = CLUX_FFI_RET_I16;
            else if (rt_core == sema->vm->type_i32) rk = CLUX_FFI_RET_I32;
            else if (rt_core == sema->vm->type_i64) rk = CLUX_FFI_RET_I64;
            else if (rt_core == sema->vm->type_u8) rk = CLUX_FFI_RET_U8;
            else if (rt_core == sema->vm->type_u16) rk = CLUX_FFI_RET_U16;
            else if (rt_core == sema->vm->type_u32) rk = CLUX_FFI_RET_U32;
            else if (rt_core == sema->vm->type_u64) rk = CLUX_FFI_RET_U64;
            /* usize（独立类型，C 的 size_t）：自己的返回码，宽度随平台 */
            else if (rt_core == vm_type_usize(sema->vm)) rk = CLUX_FFI_RET_USIZE;
            else if (rt_core == sema->vm->type_bool) rk = CLUX_FFI_RET_BOOL;
            else if (rt_core == sema->vm->type_f32) rk = CLUX_FFI_RET_F32;
            else if (rt_core == sema->vm->type_f64) rk = CLUX_FFI_RET_F64;
            else if (rt_core == sema->vm->type_opaque) rk = CLUX_FFI_RET_PTR;
            else if (rt_core == sema->vm->type_str) rk = CLUX_FFI_RET_STR;
            else if (rt && (type_kind_of(rt) == TYPE_KIND_STRUCT ||
                            type_kind_of(rt) == TYPE_KIND_CUNION ||
                            type_kind_of(rt) == TYPE_KIND_ENUM ||
                            /* C 函数指针返回值（`... : cfunc(i32)->i32`）：
                               与指针同宽，但类型身份要保留（裸指针字节构造
                               cfunc 值）→ 同样走"按类型值返回"。 */
                            type_kind_of(rt) == TYPE_KIND_CFUNC ||
                            type_is_ptr(rt_core))) {
              /* 程序自定义类型 / 带类型的指针：宽度与布局必须查类型表 →
                "按类型值返回"。compiler 会在 EXTERN_CALL 之前压一个类型值
                （类型表达式取自 extern cfunc 的声明，sema 已挂在符号的 ast 上）。 */
              rk = CLUX_FFI_RET_TYPED;
              ast_func_def_t *efd =
                  (fs->ast && fs->ast->kind == AST_FUNC_DEF)
                      ? (ast_func_def_t *)fs->ast
                      : NULL;
              call->ffi_ret_type_expr = efd ? efd->return_expr : NULL;
            } else {
              diag_error(sema->diag, sema_loc(sema, &call->base),
                         "extern call %s: return type is not marshalable",
                         qual);
              return val_void(sema);
            }
            const func_sig_t *sig = &((const func_type_t *)type_strip_ownership(fs->type))->sig;
            call->is_ffi_call = true;
            /* 返回类型码 + 变参信息打包（编解码见 vm/bcode.h）。
               变参调用必须知道"前几个是固定参数"，运行期才能按 C 的约定铺寄存器/
               栈并对变参部分施加默认实参提升。 */
            call->ffi_ret_kind =
                CLUX_FFI_RET_PACK(rk, sig->is_variadic, sig->param_count);
            call->ffi_sig = fs->type;
            call->ffi_c_name =
                (fs->ast && fs->ast->kind == AST_FUNC_DEF)
                    ? ((ast_func_def_t *)fs->ast)->name
                    : (strslice_t){0};
            call->ffi_callconv =
                (fs->ast && fs->ast->kind == AST_FUNC_DEF)
                    ? ((ast_func_def_t *)fs->ast)->callconv
                    : 0;

            /* 实参校验（此前**完全没做**）：数量 + 每个实参能否共用形参的 C 类型
               描述符（判据见 ffi_arg_acceptable）。变参的额外实参按 C 的默认实参
               提升处理（f32→double、窄整型→int），因此浮点/整型/指针都可以。 */
            size_t nargs = 0;
            for (ast_node_t *a = call->args; a; a = a->next) nargs++;
            if (!sig->is_variadic && nargs > sig->param_count)
              diag_error(sema->diag, sema_loc(sema, &call->base),
                         "extern call %s: expected %zu argument(s), got %zu",
                         qual, sig->param_count, nargs);
            /* 逐个求值实参：**必须经真正的 AST 槽**（`&call->args` / `&node->next`）——
               comptime 折叠会把节点就地替换成字面量（如 `add(1,2)` → `3`），写在
               局部指针副本上会丢，编译器随后仍去编译未折叠的调用）。 */
            size_t ai = 0;
            for (ast_node_t **slot = &call->args; *slot; slot = &(*slot)->next, ai++) {
              value_t *av = sema_expr(sema, slot, scope);
              if (!av || value_is_error(sema->vm, av) ||
                  value_is_type(av, TYPE_KIND_VOID))
                continue;
              ast_node_t *a = *slot;
              const type_t *at = value_type(av);
              const type_t *pt =
                  (ai < sig->param_count && sig->params) ? sig->params[ai] : NULL;
              if (!pt) {
                if (!ffi_vararg_acceptable(sema, at))
                  diag_error(sema->diag, sema_loc(sema, a),
                             "extern call %s: variadic argument %zu: this type "
                             "cannot cross the C ABI", qual, ai + 1);
                continue;
              }
              /* 所有权契约：`own` 形参（包括 `own Agg` 这种按值聚合）必须接收
                 将亡值（move/clone 的结果），否则实参仍由调用方持有，而被调方
                 按 own 契约释放 → double free。反向规则同理：fatal 实参必须被
                 own 形参接住，否则没有位置释放 → 泄漏。规则与普通调用的
                 `func_shadow_call`（src/vm/type_func.c）保持一致；差异只在于
                 FFI 调用不走 value_call，必须在这里显式补上。 */
              ownership_t pown = type_ownership_of(pt);
              ownership_t aown = value_type_ownership(av);
              if (pown == OWN_OWN && aown != OWN_FATAL) {
                char pn[64];
                sema_type_name(pt, pn, sizeof pn);
                diag_error(sema->diag, sema_loc(sema, a),
                           "extern call %s: argument %zu: cannot copy a value "
                           "holding owned resources into parameter of type %s "
                           "(use move() or clone(), or declare the parameter as "
                           "'ref *T' to borrow)", qual, ai + 1, pn);
                continue;
              }
              if (aown == OWN_FATAL && pown != OWN_OWN) {
                char pn[64];
                sema_type_name(pt, pn, sizeof pn);
                diag_error(sema->diag, sema_loc(sema, a),
                           "extern call %s: argument %zu: a dying value (FATAL) "
                           "must be caught by an 'own' position; parameter of "
                           "type %s does not take ownership (bind it to an own "
                           "variable, or pass it to an own parameter)",
                           qual, ai + 1, pn);
                continue;
              }
              if (!ffi_arg_acceptable(sema, pt, at))
                diag_error(sema->diag, sema_loc(sema, a),
                           "extern call %s: argument %zu: cannot pass this value "
                           "as the declared parameter type", qual, ai + 1);
            }
            value_t *sv = value_make_shadow(sema->vm, rt ? rt : sema->vm->type_void);
    
            return sv;
          }
        }
      }

      value_t *callee = NULL;
      sema_symbol_t *callee_sym = NULL;
      if (call->callee->kind == AST_IDENT) {
        callee_sym =
            sema_lookup(scope, ((ast_ident_t *)call->callee)->name);
        sema_symbol_t *sym = callee_sym;
        if (sym && sym->is_comptime) {
          /* comptime func 调用：实参改写（折叠引用为字面量）+ ctfe 求值 →
             整个调用折叠为字面量。函数本身不注册到运行时。 */
          if (!sema->fn.walking_comptime)
            return sema_eval_comptime_call(sema, node, scope);
          /* walking_comptime：只做 shadow 类型检查（callee shadow 走下方
             统一 value_call 路径，返回 return type 的 shadow） */
          callee = value_make_shadow(sema->vm, sym->type);
        }
      }
      if (!callee) {
        /* 一般 callee（函数名 / 函数值表达式）统一 shadow 求值。
           置 in_call_callee：TDZ 检查仅对调用点生效（值引用放行）。 */
        sema->fn.in_call_callee = true;
        callee = sema_expr(sema, &call->callee, scope);
        sema->fn.in_call_callee = false;
        if (is_poison(sema, callee))
          return val_void(sema);
        const type_t *ctype = value_type(callee);
        if (!ctype || type_kind_of(ctype) != TYPE_KIND_FUNC) {
          char tn[64];
          op_type_name(callee, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "cannot call value of type %s", tn);
          return val_void(sema);
        }
      }

      size_t argc = sema_count_siblings(call->args);
      value_t *arg_shadows[argc > 0 ? argc : 1];
      /* 逐实参求值（匿名构造注入：参数类型已知 → push anon_ct，使实参的
         .{...} 推断目标类型。签名参数类型从 callee 的 func 类型取——shadow
         value（data=NULL）仍携带完整签名；可变参数后无类型 → 不注入。 */
      const type_t *fnt = value_type(callee);
      ast_node_t **link = &call->args;
      size_t i = 0;
      for (; *link; link = &(*link)->next, i++) {
        const type_t *pt = fnt ? func_type_param(fnt, i) : NULL;
        bool ct_pushed = sema_anon_push(sema, pt);
        arg_shadows[i] = sema_expr(sema, link, scope);
        sema_anon_pop(sema, ct_pushed);
      }
      value_t *result = value_call(sema->vm, callee, arg_shadows, argc);

      /* 语言级内建的类型特判（**按符号上的 builtin_op 标记**，不按 callee
         名字/AST 形态/fid）：callee 求值后 AST_IDENT 已被折叠成 FUNC_REF，
         "按名字 + AST_IDENT 匹配"的老写法因此从未生效（见 symbol.h 的说明）。 */

      /* free(p) 只允许作为语句；表达式位置没有可用值。 */
      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_FREE) {
        diag_error(sema->diag, sema_loc(sema, &call->base),
                   "free() must be used as a statement");
        return val_void(sema);
      }

      /* take(x)：运行时移出 optional payload 并置 nil。
         - ?T（普通）        -> T
         - ?own T / 含 own 槽位 -> fatal T（strip own 后加 fatal） */
      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_TAKE) {
        if (argc != 1 || !arg_shadows[0]) {
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "take_opt: expects exactly 1 argument");
          return val_void(sema);
        }
        const type_t *ot = value_type(arg_shadows[0]);
        if (!ot || type_kind_of(ot) != TYPE_KIND_OPTION) {
          diag_error(sema->diag, sema_loc(sema, call->args),
                     "take_opt: argument must be an optional value");
          return val_void(sema);
        }
        const type_t *inner = type_option_inner(ot);
        if (!inner) return val_void(sema);
        const type_t *core = type_strip_ownership(inner);
        const type_t *res = inner;
        if (type_ownership_of(inner) == OWN_OWN ||
            type_has_own_slots(core)) {
          res = sema_fatal_variant_of(sema, core);
          if (!res) return val_void(sema);
        }
        call->is_take_call = true;
        return value_make_shadow(sema->vm, res);
      }


      /* `cfunc_ptr(f)` 的返回类型：按 f 的签名精化为 `cfunc(...)`（C 函数指针），
         而不是 opaque——这样 FFI 声明可以写真实的函数类型
         `cmp: cfunc(opaque, opaque) -> i32`，调用点也能按类型检查。 */
      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_CFUNC_PTR &&
          argc == 1 && arg_shadows[0] &&
          value_type(arg_shadows[0]) &&
          type_kind_of(value_type(arg_shadows[0])) == TYPE_KIND_FUNC) {
        const type_t *fsig = value_type(arg_shadows[0]);
        size_t n = func_type_param_count(fsig);
        const type_t *params[16];
        if (n <= 16) {
          for (size_t i = 0; i < n; i++) params[i] = func_type_param(fsig, i);
          const type_t *cft = type_cfunc_sig(sema->vm, n > 0 ? params : NULL, n,
                                             func_type_return(fsig), false);
          if (cft) {
            sema_type_register(sema, cft);
            return value_make_shadow(sema->vm, cft);
          }
        }
      }

      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_LEN && argc == 1 &&
          arg_shadows[0]) {
        const type_t *at = value_type(arg_shadows[0]);
        if (at && type_kind_of(at) != TYPE_KIND_ARRAY &&
            !type_is_slice(at) &&
            type_kind_of(at) != TYPE_KIND_STR &&
            type_kind_of(at) != TYPE_KIND_TUPLE) {
          char tn[64];
          sema_type_name(at, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, *node),
                     "len: argument must be array/slice/str/tuple, got %s", tn);
          return val_void(sema);
        }
      }

      if (value_is_error(sema->vm, result)) {
        error_data_t *ed = (error_data_t *)value_data(result);
        const char *msg = ed && ed->message ? ed->message
                                               : "function call failed";
        diag_error(sema->diag, sema_loc(sema, &call->base), "%s", msg);
        return val_void(sema);
      }

      /* data（C ABI 适配）：从**序列**取首元素指针。实参必须是 array/slice/str；
         返回类型精化为 `ref *<元素类型>`（str → ref *u8，即 C 的 const char*）。
         置 is_data_call：compiler 改发 BCODE_DATA 指令（不是 CALL——内置 cfunc
         的实参是副本，取不到调用方的存储身份，见 ast_call.h / vm/bcode.h）。 */
      /* __deep_clone(v)：运行期对象图深拷贝（闭包 env / cunion 兜底）。
         返回**同类型 fatal**（将亡值）：消费者按所有权规则接管。 */

      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_DEEP_CLONE) {
        value_t *arg = call->args ? sema_expr(sema, &call->args, scope) : NULL;
        if (!arg || is_poison(sema, arg) || value_is_type(arg, TYPE_KIND_VOID))
          return val_void(sema);
        value_t *sv = value_make_shadow(sema->vm, value_type(arg));
        value_set_type_ownership(sema->vm, sv, OWN_FATAL);
        return sv;
      }

      if (callee_sym && callee_sym->builtin_op == SEMA_BUILTIN_DATA) {
        if (argc != 1) {
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "data: expects exactly 1 argument, got %zu", argc);
          return val_void(sema);
        }
        /* 结构/布局视图：`own []T` / `ref []T` 与 `[]T` 同构。 */
        const type_t *at = type_strip_ownership(value_type(arg_shadows[0]));
        const type_t *elem = NULL;
        switch (at ? at->kind : TYPE_KIND_VOID) {
        case TYPE_KIND_SLICE:
          elem = slice_type_elem(at);
          break;
        case TYPE_KIND_ARRAY:
          elem = array_type_elem(at);
          break;
        case TYPE_KIND_STR:
          elem = sema->vm->type_u8;
          break;
        default:
          break;
        }
        if (!elem) {
          char tn[64];
          sema_type_name(at, tn, sizeof(tn));
          diag_error(sema->diag, sema_loc(sema, &call->base),
                     "data: argument must be array/slice/str, got %s", tn);
          return val_void(sema);
        }
        /* `data(x)` 是借用：返回 **ref *T**（指向调用方存储，不拥有）。 */
        const type_t *rp0 = type_ptr_intern(sema->vm, elem);
        if (!rp0) return val_void(sema);
        const type_t *rp = type_ownership_apply(sema->vm, OWN_REF, rp0);
        if (!rp) return val_void(sema);
        sema_type_register(sema, rp);
        call->is_data_call = true;
        value_t *sv = value_make_shadow(sema->vm, rp);
        value_set_type_ownership(sema->vm, sv, OWN_REF);
        return sv;
      }

      /* 返回值的所有权已编码在返回类型里（shadow 按返回类型构造）。 */
      return result; /* shadow in → shadow out（return_type shadow） */
}
