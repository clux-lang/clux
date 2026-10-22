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
#include "parser/ast_index.h"
#include "parser/ast_spread.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_member.h"
#include "parser/ast_move.h"
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
#include "vm/ffi_lib.h" /* M6 FFI：ffi_data_kind_of（extern var 的类型码） */
#include "vm/bcode.h"   /* M6 FFI：BCODE_FFI_RET_*（extern cfunc 的返回类型码） */
#include "core/strmap.h"

#include <stdio.h>
#include <string.h>


#include "sema_expr_internal.h"

static const type_t *static_lvalue_type(sema_t *sema, sema_scope_t *scope, ast_node_t *e);

/* 函数自身符号归属检查（捕获检查用）：sym 是**函数自身子树**里的符号 → true。
   false = 外层局部（需捕获）或非函数自身符号。

   自身子树 = fscope（捕获层，捕获符号注册在此）及其全部后代：
   param_scope（参数层，fscope 子）+ 函数体 block + 体内每一层块 / for 作用域。
   参数在参数层，捕获在 fscope——参数遮蔽捕获，两者同属函数自身符号，放行。

   **必须查后代作用域，不能只查 fscope / param_scope 的直系**：函数体内
   `if (c) { var t = … }`、`for (var i = …)` 的变量定义在**嵌套块作用域**，
   只查直系会把它们误判成"外层局部"（`local function cannot access outer
   local 't'`）——这是 foreach 展开（生成 `for (var __idx_N …)`）在闭包里
   报错的根因。符号的 own_scope 由 3b 定义点写入，引用点必然在其后（TDZ），
   因此此刻可用。 */
static bool func_own_symbol(sema_t *sema, strslice_t name,
                            sema_symbol_t *sym) {
  if (!sema->fn.local_func_base) return false;
  if (sema_scope_find_local(sema->fn.local_func_base, name) == sym) return true;
  if (sema->fn.local_func_param_scope &&
      sema_scope_find_local(sema->fn.local_func_param_scope, name) == sym)
    return true;
  if (sym->own_scope &&
      sema_scope_within(sym->own_scope, sema->fn.local_func_base))
    return true;
  return false;
}

/* 闭包 TDZ 检查（编译期）：被引用/调用的局部函数若有捕获（fscope 捕获符号
   存在），其捕获值在定义点 resolve_func_captures 才绑定（is_active 激活）。
   walk 顺序保证：定义点前的引用点（函数提升后）捕获符号尚未激活 → TDZ，
   编译期报错（捕获槽运行期仍为 undefined 占位，但不应静默到运行期才暴露）。
   无捕获的局部函数提升后即可安全引用（hoist 只绑定地址，无捕获槽）。 */
static bool func_capture_tdz(sema_scope_t *fscope, ast_node_t *captures) {
  if (!fscope || !captures) return false;
  for (ast_node_t *c = captures; c; c = c->next) {
    ast_var_def_t *cv = (ast_var_def_t *)c;
    sema_symbol_t *cs = sema_scope_find_local(fscope, cv->name);
    if (cs && !cs->is_active) return true;
  }
  return false;
}

/* AST_IDENT 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_ident(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 变量读取：从 VM scope 链 lookup shadow value（与 sema 作用域树同构，
         天然遮罩）。未初始化检查走符号表 flow_init（确定性赋值分析，
         VM 值层不感知 TDZ）。
         comptime var：定义点已从语句链摘除（不在 VM scope），引用点折叠为
         字面量 AST 节点（sema_ct_lit，arena 分配），下游（编译器）零感知。 */
      ast_ident_t *n = (ast_ident_t *)*node;

      /* 类型参数包变参不是运行期变量：只允许 args[i] / len(args) /
         ...args 这三类纯 AST 变换；需要 tuple 值时显式 `.{ ...args }`。 */
      if (sema_generic_pack_is_param(sema, n->name)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "variadic parameter '%.*s' has no runtime value; use "
                   "'%.*s[i]', 'len(%.*s)', '...%.*s', or explicitly "
                   "construct a tuple with '.{ ...%.*s }'",
                   (int)n->name.len, n->name.ptr, (int)n->name.len,
                   n->name.ptr, (int)n->name.len, n->name.ptr,
                   (int)n->name.len, n->name.ptr, (int)n->name.len,
                   n->name.ptr);
        return val_void(sema);
      }

      if (sema->generics.active_inst) {
        const sema_generic_instance_t *gi = sema->generics.active_inst;
        if (gi->has_pack && gi->pack_is_value &&
            strslice_eq(gi->pack_name, n->name)) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "comptime value parameter pack '%.*s' is a compile-time "
                     "sequence; use '%.*s[i]' or 'len(%.*s)'",
                     (int)n->name.len, n->name.ptr, (int)n->name.len,
                     n->name.ptr, (int)n->name.len, n->name.ptr);
          return val_void(sema);
        }
      }

      sema_symbol_t *sym = sema_lookup(scope, n->name);

      /* M7 comptime 值参数：当前正在 walk 的泛型实例有同名值参数时，把引用
         折叠为字面量（`[N]T` 的边界由 CTFE 配合 VM scope 里的真实值处理；
         这里处理表达式位置的 `if N > 0` / `N * 2` 等）。 */
      if (sema->generics.active_inst) {
        const sema_generic_instance_t *gi = sema->generics.active_inst;
        for (size_t i = 0; i < gi->param_count && i < gi->arg_count; i++) {
          if (!gi->args[i].is_value || !gi->param_names) continue;
          if (!strslice_eq(gi->param_names[i], n->name)) continue;
          ast_node_t *lit = sema_ct_lit(sema, *node, &gi->args[i].value);
          if (lit) {
            lit->next = (*node)->next;
            *node = lit;
            return value_make_shadow(sema->vm, gi->args[i].value.type);
          }
        }
      }

      /* M7：泛型定义只登记符号，不是值。函数/类型名必须带类型实参实例化
         （`f[i32](...)` / `Vec[i32]`）——裸引用给出可操作的诊断。 */
      if (sym && sym->is_generic) {
        bool is_func = sym->ast && sym->ast->kind == AST_FUNC_DEF;
        diag_error(sema->diag, sema_loc(sema, *node),
                   is_func ? "generic function '%.*s' requires type arguments: "
                             "write '%.*s[...](...)'"
                           : "generic type '%.*s' requires type arguments: "
                             "write '%.*s[...]'",
                   (int)n->name.len, n->name.ptr, (int)n->name.len, n->name.ptr);
        return val_void(sema);
      }

      /* 函数引用（函数值，编译期/运行期二元）：符号种类 SYM_FUNC（用户函数
         与内建函数 printf 统一——两者在 sema/ctfe/compiler/vm 视角无区别，
         仅 func_t id 段不同，由 func_new 创建时分配）。
         遮蔽平等：局部变量遮蔽函数名时 sym 解析到变量符号（SYM_VAR），
         不走此分支——`var add = 2; var x = add;` 中 add 是变量引用。
         改写为 AST_FUNC_REF（compiler 发 LOAD_FUNCTION <id> 运行期加载
         真实函数值），此处返回签名类型 shadow（编译期折叠，供赋值/实参/
         返回值的签名匹配校验）。comptime func 不进入运行时，作为值引用
         是非法用法（调用点在 AST_CALL 折叠）。 */
      if (sym && sym->kind == SEMA_SYM_FUNC && sym->type &&
          type_kind_of(sym->type) == TYPE_KIND_FUNC) {
        /* 局部函数体内引用兄弟/自身函数值：运行时函数体查找链只有参数 +
           全局（closure_scope 为空、调用时临时接 root_scope），兄弟/自身
           符号在定义点块作用域，不可见（需闭包）。FUNC 分支改写 AST_FUNC_REF
           → LOAD_FUNCTION 查 functions_by_id 全局表，会绕过作用域可见性
           ——在此显式拦截。参数（param_scope 直系，含函数类型参数——可调用，
           运行时参数可见）与全局函数（global_scope 符号）放行，与变量
           分支捕获检查同构。 */
        if (sema->fn.local_func_base && !func_own_symbol(sema, n->name, sym) &&
            sema_lookup(sema->global_scope, n->name) != sym) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "local function cannot reference sibling or self '%.*s' "
                     "(add it to the capture list)",
                     (int)n->name.len, n->name.ptr);
          return val_void(sema);
        }
        if (sym->is_comptime) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "comptime function '%.*s' cannot be used as a value",
                     (int)n->name.len, n->name.ptr);
          return val_void(sema);
        }
        /* 闭包捕获 TDZ（编译期）：局部函数的捕获值在定义点 resolve_func_captures
           才绑定（is_active 激活）。walk 顺序保证定义点前的调用点捕获符号未激活
           → 编译期报错，不静默到运行期（捕获槽运行期仍为 undefined 占位）。
           仅对调用点生效（in_call_callee）——值引用（var f = b）放行：f/b
           浅拷贝共享同一 func_t 实例，运行时块入口已 MAKE_FUNCTION 实例化并
           绑定名字，定义点前取值拿到的是同实例浅拷贝，捕获槽在定义点后由
           SET_CLOSURE 填齐，调用时才读取，无 TDZ 悬垂。
           全局函数 captures 恒空（hoist 基底即最终实例），天然放行。 */
        sema_func_t *tdz_sf = sema_func_by_id(sema, sym->fid);
        if (sema->fn.in_call_callee && tdz_sf && tdz_sf->is_local &&
            tdz_sf->def && tdz_sf->def->kind == AST_FUNC_DEF) {
          ast_func_def_t *tdz_fn = (ast_func_def_t *)tdz_sf->def;
          if (func_capture_tdz(tdz_sf->scope, tdz_fn->captures)) {
            diag_error(sema->diag, sema_loc(sema, *node),
                       "closure '%.*s' used before its captures are bound (TDZ)",
                       (int)n->name.len, n->name.ptr);
            return val_void(sema);
          }
        }
        ast_node_t *ref = ast_func_ref_new(sema->arena, (*node)->tok_begin,
                                           (*node)->tok_end);
        if (ref) {
          /* fid 由 sema 创建函数对象时分配（符号表字段；内建函数 = 内建 id）。
             name 指向源标识符 token（compiler 发 PUSH name 沿作用域链查找——
             局部函数取块入口 MAKE_FUNCTION 绑定的实例（全块同一实例，浅拷贝
             共享 func_t）；全局/内建取全局绑定基底，与定义点实例化语义对齐）。 */
          ast_func_ref_t *fr = (ast_func_ref_t *)ref;
          fr->fid  = sym->fid;
          fr->name = n->name;
          ref->next = (*node)->next; /* 保留兄弟链 */
          *node = ref;
        }
        return value_make_shadow(sema->vm, sym->type);
      }

      /* **闭包捕获读取**：改写为 `__ctx.env.<cap>`（捕获聚合结构体）。
         __ctx 是调用帧绑定的隐藏局部（被调用的 func 值），`.env` 是 env 块的
         借用 struct 值（VM FIELD_GET 的闭包分支），`.cap` 就是普通结构体字段
         ——运行时不再有"按名查 closure_scope"的隐式捕获读取。 */
      if (sym && sym->kind == SEMA_SYM_VAR && sym->is_capture &&
          sema->fn.def && sema->fn.def->capture_type_id != 0) {
        uint32_t tb = (*node)->tok_begin, te = (*node)->tok_end;
        ast_node_t *ctx = ast_ident_new(sema->arena, tb, te);
        ast_node_t *env = ast_member_new(sema->arena, tb, te);
        ast_node_t *cap = ast_member_new(sema->arena, tb, te);
        if (ctx && env && cap) {
          ctx->is_generated = true;
          env->is_generated = true;
          cap->is_generated = true;
          ((ast_ident_t *)ctx)->name = strslice_from_bytes("__ctx", 5);
          ((ast_member_t *)env)->object = ctx;
          ((ast_member_t *)env)->field = strslice_from_bytes("env", 3);
          ((ast_member_t *)cap)->object = env;
          ((ast_member_t *)cap)->field = n->name;
          cap->next = (*node)->next;
          *node = cap;
          /* 不求值生成链（__ctx 是 VM 调用帧的隐藏局部，sema 作用域里不需要
             它）：直接按捕获槽类型/所有权产出 shadow。 */
          value_t *csv = value_make_shadow(sema->vm, sym->type);
          value_set_type_ownership(sema->vm, csv, sema_sym_ownership(sym));
          return csv;
        }
      }

      if (sym && !sym->flow_init) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "variable '%.*s' used before initialization",
                   (int)n->name.len, n->name.ptr);
        return val_void(sema);
      }
      if (sym && sym->moved) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "variable '%.*s' used after move (ownership was "
                   "transferred)",
                   (int)n->name.len, n->name.ptr);
        return val_void(sema);
      }
      if (sym && sym->is_comptime && sym->ct_valid) {
        /* 折叠引用点为字面量（保留兄弟链，供调用点/语句链继续遍历） */
        ast_node_t *lit = sema_ct_lit(sema, *node, &sym->ct);
        if (lit) {
          lit->next = (*node)->next;
          *node = lit;
          return value_make_shadow(sema->vm, sym->ct.type);
        }
      }
      /* 捕获检查（无闭包）：局部函数体内引用外层局部符号（外层块/外层函数
         的变量、参数、类型名）→ 运行时函数体查找链只有参数 + 全局
         （closure_scope 为空、调用时临时接 root_scope），外层局部不可见。
         fscope parent = 定义点块作用域（同块局部函数互相可见）——沿链命中的
         "非 fscope 直系、非 global"符号即外层局部。全局符号（sema_lookup
         global 命中）、函数名（FUNC 分支已先行返回）、comptime 符号（上方
         已折叠，不捕获）合法。 */
      if (sema->fn.local_func_base && sym &&
          !func_own_symbol(sema, n->name, sym) &&
          sema_lookup(sema->global_scope, n->name) != sym) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "local function cannot access outer local '%.*s' "
                   "(add it to the capture list)",
                   (int)n->name.len, n->name.ptr);
        return val_void(sema);
      }
      /* 生成节点 `__ctx`（闭包调用帧的隐藏局部）：**不做作用域查找**。
         捕获读取在第一次 walk 时被改写成 `__ctx.env.<cap>` 并就地写回 AST；
         之后同一条函数体可能被再次类型检查（字面量重复 walk），重新求值
         生成链时若按符号查找就会误报 "undefined variable '__ctx'"——
         它只存在于运行期调用帧。这里按当前函数的签名类型直接给 shadow。 */
      if (n->base.is_generated && n->name.len == 5 && n->name.ptr &&
          memcmp(n->name.ptr, "__ctx", 5) == 0) {
        const sema_type_t *ctx_sig = sema->fn.def
                                         ? sema_type_by_id(sema, sema->fn.def->sig_id)
                                         : NULL;
        return value_make_shadow(sema->vm, ctx_sig && ctx_sig->type
                                               ? ctx_sig->type
                                               : sema->vm->type_void);
      }
      value_t *v = scope_lookup(sema->vm->current_scope, n->name);
      if (!v) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "undefined variable '%.*s'", (int)n->name.len, n->name.ptr);
        return val_void(sema);
      }
      /* 类型即表达式：内建/自定义类型值注册在作用域链（global/root/current），
         与变量同机制查找。命中 type value 直接返回该真实值（type value 是
         编译期实体，data 恒为 type_t*——无 shadow 形态，见 sema 注释）。 */
      if (value_type(v) == sema->vm->type_type)
        return v;

      /* ?T 变量读取：始终按 ?T 类型返回 shadow（无窄化退化——optional
         解包由显式 .! assert 承担，见 docs m2-design §12.5） */
      value_t *sv = value_make_shadow(sema->vm, value_type(v));
      value_set_type_ownership(sema->vm, sv, sym ? sema_sym_ownership(sym) : OWN_NONE);
      return sv;
}

/* AST_MOVE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_move(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 所有权原语 move(x) / clone(x)（m3-design §7）：操作数求值 → 返回
         操作数类型的 shadow。move 转移所有权（原对象进 TDZ，Step A 静默），
         clone 深拷贝。指针操作数（own/ref/fatal）返回 fatal 所有权修饰类型
         （将亡值，§3.3——运行期 MOVE/CLONE 指令产物即 fatal 类型）；
         非指针原样返回操作数类型。 */
      ast_move_t *n = (ast_move_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);
      const type_t *ot = value_type(operand);
      const type_t *fatal = sema_fatal_variant_of(sema, ot);
      if (!fatal) return val_void(sema);
      bool is_clone = n->op && token_is(n->op, "clone");
      /* 所有权纯编译期：fatal 是值层状态，不再有独立 fatal 类型变体。
         move/clone 产生将亡值：**指针/切片**整体转移；**含 own 槽位的聚合**
         也携带资源（其 own 字段），同样产 FATAL。标量无资源 → fatal 折叠为
         自身，故不在此列。
         判据读 type 对象（`type_has_own_slots`），与销毁序列生成器同一权威。 */
      /* FUNC（闭包）同样产 FATAL：闭包 = 捕获聚合结构体，可能携带 own 捕获
         （运行期才知道），因此按"携带资源"处理——`move(cl)` 可赋给 own 槽位/
         变量，`_ = move(cl)` 即显式销毁。 */
      bool produces_fatal = ot && (type_kind_of(ot) == TYPE_KIND_PTR ||
                                   type_kind_of(ot) == TYPE_KIND_SLICE ||
                                   type_kind_of(ot) == TYPE_KIND_FUNC ||
                                   type_has_own_slots(ot));

      /* ---- union / cunion 成员 move 豁免（2026-10）----
         union / cunion 的成员与整体**共享同一块内存布局**（cunion 全 offset 0；
         tag union 的 payload 共用 payload 区）——"局部 = 整体"。move 成员就是把
         整体交出去：整体进 TDZ 既没有"per-field TDZ 无法记录"的问题，也是
         **cunion 指针成员的唯一释放途径**（`_ = move(c.p)`）。

         约束（只对**直接变量根**成立，见下）：
           - 根必须是 `ast_member.object` 直接是变量标识符（`u.a`）——经指针
             （`p.a`，sema 已改写为 `p.*.a`）或嵌套结构（`s.u.a`）时，整体 TDZ 会
             跳过其他 own 槽位/堆块的销毁 → 泄漏，故不豁免（保持"字段不可 move"）；
           - tag union：member 槽位所有权必须是 **OWN**（ref payload 是借用，不能
             转移）；cunion：成员类型含资源（指针/切片/含 own 槽位聚合）即可
             （cunion 不参与所有权运算，槽位所有权恒 NONE）；
           - 根变量有**存活借用**时拒绝（与整体 move 同一 R3 规则）。 */
      sema_symbol_t *union_root = NULL;
      strslice_t union_root_name = STRSLICE_EMPTY;
      if (!is_clone && n->operand->kind == AST_MEMBER) {
        ast_member_t *m = (ast_member_t *)n->operand;
        if (m->object && m->object->kind == AST_IDENT) {
          sema_symbol_t *s =
              sema_lookup(scope, ((ast_ident_t *)m->object)->name);
          const type_t *bt =
              s && s->kind == SEMA_SYM_VAR && s->type
                  ? type_strip_quals(s->type)
                  : NULL;
          if (bt && type_kind_of(bt) == TYPE_KIND_UNION) {
            int mi = union_type_find_member(bt, m->field);
            const union_member_t *mm =
                mi >= 0 ? union_type_member(bt, (size_t)mi) : NULL;
            if (mm && mm->payload_type &&
                type_ownership_of(mm->payload_type) == OWN_OWN) {
              union_root = s;
              union_root_name = ((ast_ident_t *)m->object)->name;
            }
          }
          /* cunion 成员 move **不再**登记 union_root（2026-10）：cunion 是
             不安全类型（无 tag、成员共享 offset 0、不参与所有权运算），它的成员
             move 走"用户自负"路径——不受访问路径限制、**不进 TDZ**、运行期
             真转移（TAKE_SLOT 把槽位置零）。见下方 cunion_member_move 的处理。 */
        }
      }

      /* ---- cunion 成员 move：**放宽为不受限、不进 TDZ**（2026-10）----
         cunion 是**不安全类型**：无 tag、成员共享 offset 0、不参与所有权运算，
         编译器不保证重复释放/错误释放能被发现。因此对它的成员 move 采用
         "用户自负"语义：
           - 任意访问路径都允许：`_ = move(c.p)` / `_ = move(s.c.p)` /
             `_ = move(p.c.p)`（嵌套、经指针均可）——这是 cunion 指针成员在
             这些形态下**唯一的释放途径**；
           - **不进入 TDZ**：整体仍可读 / 可再次 move（重复释放由用户负责；
             FREE 后槽位指针被置 NULL，重复 FREE 是 no-op）；
           - 跳过"来源必须拥有该值"检查（cunion 槽位没有所有权记录）。
         tag union 仍是严格语义（直接变量根 → 整体 TDZ），不受影响。 */
      bool cunion_member_move = false;
      if (!is_clone && n->operand && n->operand->kind == AST_MEMBER) {
        const ast_member_t *cm = (const ast_member_t *)n->operand;
        const type_t *ct = static_lvalue_type(sema, scope, cm->object);
        if (ct && type_kind_of(ct) == TYPE_KIND_CUNION) cunion_member_move = true;
      }

      /* cunion 成员 move 的**运行期真转移**：把操作数包成 AST_TAKE_SLOT ——
         读取槽位值后把槽位 memset 为 0。于是：
           - `_ = move(c.p)` 之后槽位是 NULL → 重复 move 是 no-op（不会二次释放）；
           - `var q = move(c.p)` 之后 cunion 里不残留悬垂指针。
         语义上仍是"用户自负"（cunion 无 tag、不参与所有权运算，编译器不保证
         重复/错误释放）。 */
      if (cunion_member_move) {
        ast_node_t *slot = ast_take_slot_new(sema->arena, (*node)->tok_begin,
                                             (*node)->tok_end);
        if (slot) {
          ((ast_take_slot_t *)slot)->operand = n->operand;
          slot->next = (*node)->next;
          *node = slot;
        }
      }

      /* ---- 约束 1：**字段/元素级 move 一律禁止**（只能整体 move）----
         TDZ 是**符号级**的（`sema_symbol_t.moved`，只跟踪变量名），字段没有
         "局部 TDZ"：`move(s.p)` 把值/资源从字段里掏走，却没有任何地方记录
         `s.p` 已失效 —— 于是同一字段可以重复 move、被读成陈旧值或悬垂指针。

         判据**不能**依赖"是否产生 fatal 变体"：标量（i32 等）没有资源，其 fatal
         变体折叠为自身，`produces_fatal` 为假 —— 用 fatal 当门槛会把
         `move(p.x)`（标量字段）整条漏掉（实测放行）。所以这里独立判断
         "操作数是不是字段/元素访问"。

         **union / cunion 成员豁免**（见上）：布局局部 = 整体，整体进 TDZ。
         `clone` 不受限：拷贝不改变源的绑定状态。 */
      if (!is_clone && !union_root && !cunion_member_move &&
          (n->operand->kind == AST_MEMBER || n->operand->kind == AST_INDEX)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot move a struct/aggregate field or element "
                   "(fields have no per-field TDZ, so a moved-out field cannot be "
                   "tracked or refilled; move the whole value instead — "
                   "`move(s)` — or use clone() to copy the field)");
        return value_make_shadow(sema->vm, fatal);
      }

      /* ---- 约束 0：**move 的来源必须拥有该值** ----
         own / fatal 才能转移所有权；ref/借用（`x.&`、字段/元素读、ref 变量、
         ref 返回、解引用读到的被指向内容）都不拥有堆块——move 它只会把借用
         当所有权，销毁时 free 借用源（实测 0xC0000005）。
         clone 不受限：拷贝不改变源的绑定状态。 */
      if (!is_clone && !union_root && !cunion_member_move && produces_fatal &&
          value_type_ownership(operand) != OWN_OWN &&
          value_type_ownership(operand) != OWN_FATAL) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot move a borrowed value: move transfers ownership, but "
                   "a ref/borrow does not own the block (use clone() to copy)");
        return value_make_shadow(sema->vm, fatal);
      }

      if (!is_clone && n->operand->kind == AST_IDENT) {
        sema_symbol_t *src =
            sema_lookup(scope, ((ast_ident_t *)n->operand)->name);

        /* 注：闭包值（TYPE_KIND_FUNC）**可以 move**（2026-10 起）——闭包 =
           捕获聚合结构体（env 块），move 是合法的所有权转移：源进 TDZ、
           目标接管 env（own 捕获随之转移）；`_ = move(cl)` 即显式销毁闭包
           （FREE 按 capture_type 聚合释放 env 的 own 捕获 + env 块）。它捕获槽
           仍然不可 move（见下方约束 3：捕获槽被每次调用共享，不能留在 TDZ）。 */

        /* ---- 约束 3：**闭包捕获不可 move**（含捕获里的结构体）----
           捕获符号定义在闭包的捕获作用域上，而闭包每次调用共享同一份捕获
           作用域：move 把捕获槽标进 TDZ 后无法恢复，第二次调用就读到已转移的
           槽位。所以闭包内的捕获值（`own *T` 或含 own 的聚合）只能 clone/借用，
           不能 move。 */
        if (src && src->kind == SEMA_SYM_VAR && src->is_capture &&
            (sema_sym_ownership(src) == OWN_OWN ||
             type_kind_of(src->type) == TYPE_KIND_FUNC)) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "cannot move closure capture '%.*s': the capture slot is "
                     "shared by every call of the closure and cannot be left in "
                     "a moved (TDZ) state (use clone(), or capture by "
                     "`(x = move(y))` and clone inside)",
                     (int)((ast_ident_t *)n->operand)->name.len,
                     ((ast_ident_t *)n->operand)->name.ptr);
          return value_make_shadow(sema->vm, fatal);
        }
      }

      if (!is_clone && (produces_fatal || sema_value_is_fatal(operand)) &&
          n->operand->kind == AST_IDENT) {
        sema_symbol_t *src =
            sema_lookup(scope, ((ast_ident_t *)n->operand)->name);
        if (src && src->kind == SEMA_SYM_VAR && src->type &&
            sema_sym_ownership(src) == OWN_OWN) {
          /* `own *T`（含 own 切片同规则）：move 后源不再持有资源 →
             标记 moved，后续读取报 "used after move"。
             判据是**符号层 ownership**——所有权已从类型层迁移到值/符号层。 */
          if (src->borrow_count > 0) {
            diag_error(
                sema->diag, sema_loc(sema, *node),
                "cannot move '%.*s': %d live borrow(s) still "
                "reference it (borrows must end before move)",
                (int)((ast_ident_t *)n->operand)->name.len,
                ((ast_ident_t *)n->operand)->name.ptr, src->borrow_count);
          } else {
            src->moved = true;
          }
        } else if (src && src->kind == SEMA_SYM_VAR &&
                   sema_sym_is_fatal(src)) {
          sema_mark_fatal_consumed(src);
        }
      }
      /* **扁平化 clone**（2026-10）：把 `clone(x)` 在编译期展开成显式的
         「申请 + 赋值」序列（构造 / NEW / CLONE / 循环），运行期不再按类型
         递归（见 sema_gen_clone 的说明）。生成失败（无语句上下文等）时保留
         原 AST_MOVE 节点 → compiler 退化为运行期 CLONE 指令。
         语义返回值不变：仍是 fatal 阴影（所有权判定与消费规则完全一致）。 */
      if (is_clone) {
        /* `clone(<func 字面量>)`：字面量在定义点**本来就产出全新实例**（新的
           函数对象 + 新的 env 块），再深拷贝一份既浪费，又会让那个中间实例的
           捕获（own 堆块）随临时值丢弃而泄漏。直接当作转移：把字面量节点提到
           调用点（所有权照常交给消费者）。 */
        if (n->operand && n->operand->kind == AST_FUNC_DEF) {
          ast_node_t *lit = n->operand;
          lit->next = (*node)->next;
          *node = lit;
        } else {
          ast_node_t *gen = sema_gen_clone(sema, scope, ot, n->operand);
          if (gen) {
            *node = gen;
          } else {
            /* 静态无法表达深拷贝的形态：闭包值（env 的捕获聚合类型只有运行期
               才知道）、cunion 被指向值、无语句上下文的切片 clone
               → 改写为运行期 `__deep_clone(v)`（对象图克隆，同 ptr_clone_block）。
               这样 CLONE 指令可以彻底移除——深拷贝统一由生成函数或该内置函数承担。 */
            ast_node_t *callee = ast_ident_new(sema->arena, (*node)->tok_begin,
                                               (*node)->tok_end);
            ast_node_t *call = ast_call_new(sema->arena, (*node)->tok_begin,
                                            (*node)->tok_end);
            if (callee && call) {
              ((ast_ident_t *)callee)->name =
                  strslice_from_bytes("__deep_clone", 12);
              callee->is_generated = true;
              ((ast_call_t *)call)->callee = callee;
              ((ast_call_t *)call)->args = n->operand;
              call->is_generated = true;
              call->next = (*node)->next;
              *node = call;
            }
          }
        }
      }
      /* union / cunion 成员 move：**整体进 TDZ**（"局部 = 整体"）。
         作用域退出不再销毁整体——payload 已移出，由接管方销毁；不标 TDZ 会让
         销毁序列（tag union 的 own payload）二次 FREE 同一块。 */
      if (!is_clone && union_root) {
        if (union_root->borrow_count > 0) {
          diag_error(sema->diag, sema_loc(sema, *node),
                     "cannot move a member of '%.*s': %d live borrow(s) still "
                     "reference it (borrows must end before move)",
                     (int)union_root_name.len, union_root_name.ptr,
                     union_root->borrow_count);
        } else if (produces_fatal) {
          union_root->moved = true;
          if (sema_sym_is_fatal(union_root))
            sema_mark_fatal_consumed(union_root);
        }
      }

      value_t *r = value_make_shadow(sema->vm, fatal);
      value_set_type_ownership(sema->vm, r, OWN_FATAL);
      return r;
}

/* AST_INT_LIT 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_int_lit(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      ast_int_lit_t *n = (ast_int_lit_t *)*node;
      const type_t *t = n->type.len ? type_lookup(sema->vm, n->type)
                                    : sema->vm->type_i32;
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, *node), "unknown type '%.*s'",
                   (int)n->type.len, n->type.ptr);
        t = sema->vm->type_i32;
      }
      return value_make_shadow(sema->vm, t);
}

/* AST_FLOAT_LIT 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_float_lit(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      ast_float_lit_t *n = (ast_float_lit_t *)*node;
      const type_t *t = n->type.len ? type_lookup(sema->vm, n->type)
                                    : sema->vm->type_f64;
      if (!t) {
        diag_error(sema->diag, sema_loc(sema, *node), "unknown type '%.*s'",
                   (int)n->type.len, n->type.ptr);
        t = sema->vm->type_f64;
      }
      return value_make_shadow(sema->vm, t);
}

/* AST_CONST 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_const(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* const 类型修饰（类型即表达式）：操作数必须是类型值，结果仍是
         类型值。遮蔽/类型检查在此感知（as 右值、M2 类型字面量）。 */
      ast_const_t *n = (ast_const_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (is_poison(sema, sub))
        return val_void(sema);
      /* 必须是**类型实体**（real type value，data=type_t*）：类型为 `type` 的
         普通表达式（shadow）不是类型，不能在这里解引用 data。 */
      if (!value_is_type_value(sub)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'const' requires a type operand");
        return val_void(sema);
      }
      /* type value 恒真实（编译期实体）：取 data 里的类型指针 intern 后
         返回新的 type value（auto-track 当前 scope） */
      const type_t *sub_t = value_as(sub, const type_t *);
      const type_t *ct = type_const_intern(sema->vm, sub_t);
      return type_as_value(sema->vm, ct);
}

/* AST_VOLATILE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_volatile(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      ast_volatile_t *n = (ast_volatile_t *)*node;
      value_t *sub = sema_expr(sema, &n->sub, scope);
      if (is_poison(sema, sub))
        return val_void(sema);
      if (!value_is_type_value(sub)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "'volatile' requires a type operand");
        return val_void(sema);
      }
      /* `volatile T` 在脚本态无意义：表达式形式同样折叠为 T（与类型解析一致） */
      return sub;
}

/* AST_SIZEOF / AST_ALIGNOF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_sizeof_alignof(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 编译期运算符（m2-design §8）：操作数 shadow 求值只取类型（不真实
         执行），结果恒 u64。sizeof = 类型宽度，alignof = 类型对齐。
         折叠为 AST_INT_LIT(u64) 写回（保留兄弟链）——编译器零感知消费
         立即数；返回 shadow u64。 */
      ast_node_t *opnd = (*node)->kind == AST_SIZEOF
                             ? ((ast_sizeof_t *)*node)->operand
                             : ((ast_alignof_t *)*node)->operand;
      value_t *op = sema_expr(sema, &opnd, scope);
      /* 折叠写回：sema_expr 通过可写节点槽就地改写（comptime 变量引用 → 字面量），
         本处因两个结构体二选一而只能传局部变量，必须显式回写——否则 comptime
         符号引用会漏到运行期（该名字在运行期不存在 → "undefined variable"）。 */
      if ((*node)->kind == AST_SIZEOF)
        ((ast_sizeof_t *)*node)->operand = opnd;
      else
        ((ast_alignof_t *)*node)->operand = opnd;
      if (is_poison(sema, op))
        return val_void(sema);
      /* type value（data=type_t*）解引用 data；普通 shadow value 取
         value_type。注意 value_type(type value) 返回元类型 vm->type_type
         （size=8），直接读取会得错值——sizeof(u8) 折叠成 8。
         `value_is_type_value` 额外要求 data 非空：类型为 `type` 的 **shadow**
         （data=NULL）不是类型实体，若按 type value 解引用就是空指针解引用
         （`sizeof(t)` 曾是 0xC0000005）。此时退化为"操作数的静态类型是
         `type`"，即 sizeof(type) = 指针宽度，行为良定义。 */
      const type_t *t = value_is_type_value(op)
                            ? value_as(op, const type_t *)
                            : value_type(op);
      uint64_t n = (*node)->kind == AST_SIZEOF ? (uint64_t)t->size
                                               : (uint64_t)t->align;
      ast_node_t *lit = ast_int_lit_new(sema->arena, (*node)->tok_begin,
                                        (*node)->tok_end);
      if (lit) {
        /* 折叠成 `usize` 字面量：宽度/长度类结果的正确类型（C 的 size_t） */
        ((ast_int_lit_t *)lit)->type = vm_type_usize(sema->vm)->name;
        ((ast_int_lit_t *)lit)->value = n;
        lit->next = (*node)->next;
        *node = lit;
      }
      return value_make_shadow(sema->vm, vm_type_usize(sema->vm));
}

/* AST_TYPEOF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_typeof(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* typeof(expr) → type value（m2-design §8）：操作数 shadow 求值只取
         类型，结果即该类型。折叠为 AST_TYPE_REF（sema_ct_type_ref 登记到
         types 表，编译器 LOAD_TYPE <id> 零感知）；返回真实 type value
         （编译期实体，data 恒为 type_t*）。 */
      ast_typeof_t *n = (ast_typeof_t *)*node;
      value_t *op = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, op))
        return val_void(sema);
      /* 同上：type value 解引用 data，普通值取 value_type
         （value_is_type_value 要求 data 非空，避免 shadow 空指针解引用） */
      const type_t *t = value_is_type_value(op)
                            ? value_as(op, const type_t *)
                            : value_type(op);
      ast_node_t *ref = sema_ct_type_ref(sema, t, *node);
      if (ref) {
        ref->next = (*node)->next;
        *node = ref;
      }
      return type_as_value(sema->vm, t);
}

/* AST_UNARY 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_unary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      ast_unary_t *n = (ast_unary_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      /* 错误恢复产物静默通过，避免级联二次诊断 */
      if (is_poison(sema, operand))
        return val_void(sema);
      value_t *result = NULL;
      if (token_is(n->op, "-")) {
        result = value_neg(sema->vm, operand);
      } else if (token_is(n->op, "!")) {
        sema_check_bool(sema, n->operand, operand, "logical not");
        result = value_lnot(sema->vm, operand);
      } else if (token_is(n->op, "~")) {
        result = value_bnot(sema->vm, operand);
      } else {
        char ob[16];
        op_text(n->op, ob, sizeof(ob));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "unsupported unary operator '%s'", ob);
        return val_void(sema);
      }
      if (value_is_error(sema->vm, result)) {
        char ob[16], tn[64];
        op_text(n->op, ob, sizeof(ob));
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '%s' cannot be applied to %s", ob, tn);
        return val_void(sema);
      }
      return result;
}

/* AST_UNWRAP 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_unwrap(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* optional 解包：.!（assert）/ .?（try）。
         .! 要求操作数是 ?T：none 时运行期 panic（用户范式先判空再解包：
         if (x != nil) { var v = x.!; ... }）。返回 inner 类型 shadow。
         .? 仅词法预留（try），语义未实现 → 编译期报错。 */
      ast_unwrap_t *uw = (ast_unwrap_t *)*node;
      value_t *operand = sema_expr(sema, &uw->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);

      const type_t *ot = value_type(operand);
      if (!ot || type_kind_of(ot) != TYPE_KIND_OPTION) {
        char tn[64], ob[16];
        size_t len = 0;
        const char *text =
            uw->op ? token_get_text(uw->op, &len) : NULL;
        snprintf(ob, sizeof(ob), ".%.*s", (int)len, text ? text : "?");
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '%s' requires an optional operand, got %s", ob,
                   tn);
        return val_void(sema);
      }
      if (uw->op && token_is(uw->op, "?")) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "operator '.?' (try) is not implemented yet");
        return val_void(sema);
      }
      return value_make_shadow(sema->vm, type_option_inner(ot));
}

/* AST_SLICE_TYPE 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_slice_type(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 切片类型表达式 own/ref/fatal []T（类型即表达式，m4-design §1）：
         解析为真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），
         返回真实 type value。与 AST_ARRAY 同构。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_ARRAY 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_array(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 数组类型表达式 [N]T（类型即表达式）：表达式位置求值 = 类型值。
         解析为真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），
         返回真实 type value（data=type_t*，auto-track 当前 scope），供
         type 定义 / as 右值 / sizeof / 嵌套类型构造消费。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_TUPLE / AST_FUNC_TYPE / AST_OPTION / AST_PTR 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_type_expr(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 其余**类型表达式**出现在表达式位置（类型即表达式，m2-design 关键
         架构决策 6）：元组 `<T1,T2>` / 签名 `func(T)->R` / 可选 `?T` /
         指针 `own *T` 都应求值为类型值，与上方 AST_ARRAY 完全同构——解析为
         真实类型并就地替换为 AST_TYPE_REF（编译器发 LOAD_TYPE），返回
         type value 供 type 定义 / as 右值 / sizeof / 嵌套类型构造消费。

         此前这 4 种 kind 没有 case，落到 switch 的 default 静默返回 void
         shadow：`var x = <i32,i32>;` 之类合法写法被误报成
         "cannot initialize ... with void"，在字段位则静默跳过校验。 */
      const type_t *t = sema_resolve_type_slot(sema, node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_TYPE_REF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_type_ref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 具名类型引用（sema 登记的 "__type_N"）：重复求值（折叠重访）时
         命中——解析回类型，返回真实 type value。 */
      const type_t *t = resolve_type_expr(sema, *node);
      if (!t) return val_void(sema);
      return type_as_value(sema->vm, t);
}

/* AST_DEREF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_deref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 后置解引用取值 r.*（m3-design §8.2）：由指针得值。操作数必须是
         指针类型（own/ref/fatal）→ 返回被指向类型 T 的 shadow（运行期
         PTR_GET 指令取 T 值副本）。 */
      ast_deref_t *n = (ast_deref_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);
      const type_t *ot = value_type(operand);
      /* **weak *T 不能直接解引用**（§12.4）：弱引用不保证对象存活，必须先
         `.get()` 升级成 `?share *T`（死对象得 nil），访问路径因此天然无竞态。 */
      if (type_is_weak(ot)) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot dereference a 'weak *T': a weak reference may already "
                   "be dead (use `w.get()` to upgrade it to `?share *T`)");
        return val_void(sema);
      }
      const type_t *base = ptr_type_base(ot);
      if (!base) {
        char tn[64];
        op_type_name(operand, tn, sizeof(tn));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot dereference a value of type %s (expected a pointer)",
                   tn);
        return val_void(sema);
      }
      return value_make_shadow(sema->vm, base);
}

/* AST_ADDR 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_addr(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 后置取地址 x.&（m3-design §8.2）：由值得指针。返回 ref *T 类型的
         shadow——x.& 指向栈上值（或他人堆块）是借用，ref 总借用（type kind
         区分，运行期零标志 §13.2），销毁 no-op。
         返回 ref（而非 own）与值层语义一致（Step B 借用存活检查的基座）。 */
      ast_addr_t *n = (ast_addr_t *)*node;
      value_t *operand = sema_expr(sema, &n->operand, scope);
      if (is_poison(sema, operand))
        return val_void(sema);
      const type_t *t = value_type(operand);
      if (!t || type_kind_of(t) == TYPE_KIND_VOID || type_kind_of(t) == TYPE_KIND_ERROR) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "cannot take address of value of unknown type");
        return val_void(sema);
      }
      const type_t *ptr = type_ptr_intern(sema->vm, t);
      if (!ptr) return val_void(sema);
      /* `x.&` 的类型是 **ref *T**（借用，不是 own）。 */
      const type_t *pt = type_ownership_apply(sema->vm, OWN_REF, ptr);
      if (!pt) return val_void(sema);
      sema_type_register(sema, pt);
      value_t *r = value_make_shadow(sema->vm, pt);
      value_set_type_ownership(sema->vm, r, OWN_REF);
      return r;
}

/* AST_FUNC_DEF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_func_def(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 函数字面量（表达式内 func 定义，函数值）：签名解析 + body 类型
         检查（sema_check_func_literal：临时 fscope 同步建树+walk，不注册
         作用域符号、不提升）。返回签名类型的 shadow——函数值表达式的类型
         即签名。失败（已诊断）返回 void shadow。 */
      ast_func_def_t *fn = (ast_func_def_t *)*node;
      const type_t *sig = sema_check_func_literal(sema, fn, scope);
      if (!sig) return val_void(sema);
      value_t *fv = value_make_shadow(sema->vm, sig);
      /* **捕获了 own 的闭包，它自己就是 OWN**（所有权模型里唯一的 own 产生式）。
         为什么不经 FATAL：函数是"定义 + 绑定名字"，定义本身已经把
         `fatal → move → own` 折叠掉了。因此它只能被 `own` 位置接收，
         写 `var f = <closure>` 会因"OWN 不能赋给 NONE 位置"被拒，
         必须显式 `var f = move(<closure>)`。
         捕获槽的所有权由 resolve_func_captures 回写进 AST 节点（cv->ownership）。 */
      if (fn->captures) {
        for (ast_node_t *c = fn->captures; c; c = c->next) {
          if (type_ownership_of(((ast_var_def_t *)c)->resolved_type) ==
              OWN_OWN) {
            value_set_type_ownership(sema->vm, fv, OWN_OWN);
            break;
          }
        }
      }
      return fv;
}

/* AST_FUNC_REF 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_func_ref(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
  (void)scope; /* 本处理器不使用词法作用域 */
      /* 函数引用折叠产物（sema AST_IDENT 确认函数符号改写 / comptime 折叠
         AST_FUNC_REF）：纯 fid 标识，shadow walk 只关心签名类型——返回签名
         类型的 shadow（函数值表达式的类型即签名；运行期 LOAD_FUNCTION 加载
         真实函数值）。
         签名查询双路径（与 CTFE 一致）：程序函数（全局/局部）按 fid 查
         sema->funcs（def->sig_id → sema_type_by_id 反查）；内建函数与 CTFE
         构造的函数字面量引用对象按 fid 查 vm->functions。 */
      ast_func_ref_t *n = (ast_func_ref_t *)*node;
      const type_t *sig = NULL;
      sema_func_t *sf = sema_func_by_id(sema, n->fid);
      if (sf && sf->def && sf->def->kind == AST_FUNC_DEF) {
        ast_func_def_t *fd = (ast_func_def_t *)sf->def;
        const sema_type_t *st = sema_type_by_id(sema, fd->sig_id);
        if (st) sig = st->type;
      }
      /* 内建函数 / CTFE 构造的函数字面量引用对象：vm 侧按 fid 查签名。
         内建函数登记 functions_by_id（id < PROGRAM_BASE）；字面量引用对象
         （func_new_program_ref）只注册 vm->functions 不登记 functions_by_id
         ——线性扫描兜底（函数数量少）。 */
      if (!sig) {
        func_t *fn = vm_func_load(sema->vm, n->fid);
        if (!fn && sema->vm->functions) {
          size_t nf = vec_len(sema->vm->functions);
          for (size_t i = 0; i < nf; i++) {
            func_t *f = (func_t *)vec_get(sema->vm->functions, i);
            if (f && f->id == n->fid && f->type) { fn = f; break; }
          }
        }
        if (fn) sig = fn->type;
      }
      if (!sig) {
        diag_error(sema->diag, sema_loc(sema, *node),
                   "unknown function reference (id %u)", n->fid);
        return val_void(sema);
      }
      return value_make_shadow(sema->vm, sig);
}

/* AST_TERNARY 处理器（自 sema_expr 拆出；纯搬移，逻辑逐字一致） */
value_t *ex_ternary(sema_t *sema, ast_node_t **node,
                              sema_scope_t *scope) {
      /* 三元条件表达式：cond 必须 bool；两分支都 shadow 求值（两侧都要
         类型检查，运行时惰性只执行选中分支）；结果类型要求两分支一致。
         类型即表达式天然支持：`cond ? i32 : i64` 两侧都是 type_type。 */
      ast_ternary_t *n = (ast_ternary_t *)*node;
      value_t *cond = sema_expr(sema, &n->cond, scope);
      sema_check_bool(sema, n->cond, cond, "ternary condition");
      value_t *tval = sema_expr(sema, &n->then_branch, scope);
      value_t *eval = sema_expr(sema, &n->else_branch, scope);
      if (value_is_error(sema->vm, cond) || value_is_error(sema->vm, tval) ||
          value_is_error(sema->vm, eval) ||
          value_is_type(tval, TYPE_KIND_VOID) ||
          value_is_type(eval, TYPE_KIND_VOID))
        return val_void(sema);
      const type_t *tt = value_type(tval);
      const type_t *et = value_type(eval);
      if (tt != et) {
        char tn[64], en[64];
        op_type_name(tval, tn, sizeof(tn));
        op_type_name(eval, en, sizeof(en));
        diag_error(sema->diag, sema_loc(sema, *node),
                   "ternary branches must have the same type, got %s and %s",
                   tn, en);
        return val_void(sema);
      }
      return tval; /* shadow，类型 = 两分支共同类型 */
}

/* 左值路径的**静态类型**（不重复求值；供 move 的"容器类型"判定）：
   ident → 符号类型；member → 字段/member 类型（指针自动解引用一层）；
   deref → 被指向类型；index → 元素类型；unwrap → inner。失败返回 NULL。 */
static const type_t *static_lvalue_type(sema_t *sema, sema_scope_t *scope,
                                        ast_node_t *e) {
  if (!e) return NULL;
  switch (e->kind) {
  case AST_IDENT: {
    sema_symbol_t *s = sema_lookup(scope, ((const ast_ident_t *)e)->name);
    return s && s->type ? type_strip_quals(s->type) : NULL;
  }
  case AST_MEMBER: {
    const ast_member_t *m = (const ast_member_t *)e;
    const type_t *ot = static_lvalue_type(sema, scope, m->object);
    if (!ot) return NULL;
    const type_t *base = type_strip_quals(ptr_type_base(ot));
    const type_t *ct = base ? base : ot;   /* 指针上的成员访问自动解引用 */
    if (!ct) return NULL;
    if (type_kind_of(ct) == TYPE_KIND_STRUCT) {
      int i = struct_type_find_field(ct, m->field);
      if (i < 0) return NULL;
      const struct_field_t *f = struct_type_field(ct, (size_t)i);
      return f ? type_strip_quals(f->type) : NULL;
    }
    if (type_kind_of(ct) == TYPE_KIND_UNION) {
      int i = union_type_find_member(ct, m->field);
      if (i < 0) return NULL;
      const union_member_t *mm = union_type_member(ct, (size_t)i);
      return mm ? type_strip_quals(mm->payload_type) : NULL;
    }
    if (type_kind_of(ct) == TYPE_KIND_CUNION) {
      int i = cunion_type_find_member(ct, m->field);
      if (i < 0) return NULL;
      const cunion_member_t *cm = cunion_type_member(ct, (size_t)i);
      return cm ? type_strip_quals(cm->type) : NULL;
    }
    return NULL;
  }
  case AST_DEREF: {
    const type_t *pt =
        static_lvalue_type(sema, scope, ((const ast_deref_t *)e)->operand);
    return pt ? type_strip_quals(ptr_type_base(pt)) : NULL;
  }
  case AST_INDEX: {
    const ast_index_t *ix = (const ast_index_t *)e;
    const type_t *ot = static_lvalue_type(sema, scope, ix->object);
    if (!ot) return NULL;
    if (type_kind_of(ot) == TYPE_KIND_ARRAY)
      return type_strip_quals(array_type_elem(ot));
    if (type_kind_of(ot) == TYPE_KIND_SLICE)
      return type_strip_quals(slice_type_elem(ot));
    if (type_kind_of(ot) == TYPE_KIND_PTR) {
      const type_t *b = type_strip_quals(ptr_type_base(ot));
      if (b && type_kind_of(b) == TYPE_KIND_ARRAY)
        return type_strip_quals(array_type_elem(b));
      if (b && type_kind_of(b) == TYPE_KIND_SLICE)
        return type_strip_quals(slice_type_elem(b));
    }
    return NULL;
  }
  case AST_UNWRAP: {
    const type_t *ot =
        static_lvalue_type(sema, scope, ((const ast_unwrap_t *)e)->operand);
    return ot ? type_strip_quals(type_option_inner(ot)) : NULL;
  }
  default:
    return NULL;
  }
}
