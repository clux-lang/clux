#ifndef CLUX_CBACKEND_C_LOWER_INTERNAL_H
#define CLUX_CBACKEND_C_LOWER_INTERNAL_H

#include "cbackend/c_lower.h"

#include "parser/ast_assign.h"
#include "parser/ast_addr.h"
#include "parser/ast_binary.h"
#include "parser/ast_block.h"
#include "parser/ast_bool_lit.h"
#include "parser/ast_char_lit.h"
#include "parser/ast_call.h"
#include "parser/ast_construct.h"
#include "parser/ast_construct_field.h"
#include "parser/ast_cunion_def.h"
#include "parser/ast_defer.h"
#include "parser/ast_deref.h"
#include "parser/ast_dowhile.h"
#include "parser/ast_enum_def.h"
#include "parser/ast_expr_stmt.h"
#include "parser/ast_extern_lib.h"
#include "parser/ast_fill.h"
#include "parser/ast_float_lit.h"
#include "parser/ast_for.h"
#include "parser/ast_for_index.h"
#include "parser/ast_free.h"
#include "parser/ast_func_def.h"
#include "parser/ast_func_ref.h"
#include "parser/ast_ident.h"
#include "parser/ast_if.h"
#include "parser/ast_index.h"
#include "parser/ast_int_lit.h"
#include "parser/ast_is_tag.h"
#include "parser/ast_make.h"
#include "parser/ast_member.h"
#include "parser/ast_move.h"
#include "parser/ast_new.h"
#include "parser/ast_nil.h"
#include "parser/ast_opt_is_some.h"
#include "parser/ast_option.h"
#include "parser/ast_return.h"
#include "parser/ast_scope_ref.h"
#include "parser/ast_slice.h"
#include "parser/ast_string_lit.h"
#include "parser/ast_struct_def.h"
#include "parser/ast_switch.h"
#include "parser/ast_take.h"
#include "parser/ast_take_slot.h"
#include "parser/ast_ternary.h"
#include "parser/ast_try.h"
#include "parser/ast_type_def.h"
#include "parser/ast_unary.h"
#include "parser/ast_union_def.h"
#include "parser/ast_unwrap.h"
#include "parser/ast_var_def.h"
#include "parser/ast_while.h"
#include "parser/lexer.h"
#include "sema/sema_generic.h"
#include "sema/sema_cross.h"
#include "vm/module.h"
#include "vm/type.h"
#include "vm/type_array.h"
#include "vm/type_tuple.h"
#include "vm/type_cfunc.h"
#include "vm/type_cunion.h"
#include "vm/type_enum.h"
#include "vm/type_func.h"
#include "vm/type_option.h"
#include "vm/type_ptr.h"
#include "vm/type_slice.h"
#include "vm/type_struct.h"
#include "vm/type_union.h"
#include "vm/vm.h"

typedef struct lower_defer {
    strslice_t name;
    ast_node_t *body;
    const char *env_name; /* 内联 defer 体时 `__ctx.env` 绑定的捕获结构体局部 */
    ast_func_def_t *def; /* defer 闭包定义（捕获槽位类型来源） */
    struct lower_defer *next;
} lower_defer_t;

typedef struct lower_local_fn {
    strslice_t name;
    const char *cname; /* 静态 C 函数名 */
    const char *obj;   /* 函数对象结构名 */
    const char *var_name; /* 闭包值局部变量名（is_closure 时） */
    bool is_closure;      /* 有值捕获：调用走闭包值 */
    uint32_t fid;         /* sema 函数 id（本地函数引用按 fid 解析） */
    bool predeclared;     /* 闭包值局部已在块入口前向声明 */
    bool defined;         /* 闭包值已赋值（可被兄弟 env fixup 引用） */
    const char *env_tag;  /* 闭包 env 结构体名（如 struct clux_env_0） */
    strslice_t *cap_refs; /* 捕获的本地函数名（函数引用捕获） */
    size_t cap_ref_count;
    const type_t *type;
    struct lower_local_fn *next;
} lower_local_fn_t;

typedef struct lower_local {
    strslice_t name;
    const type_t *type;
    const char *cname; /* 该声明在 C 里的唯一符号名（同名遮蔽/展开时带后缀） */
    struct lower_local *next;
} lower_local_t;

typedef struct lower_capture {
    strslice_t name;
    const char *expr;
    const type_t *type;
    size_t index; /* 闭包 env 位置字段序 */
    struct lower_capture *next;
} lower_capture_t;

typedef struct lower_method {
    strslice_t name;
    uint32_t owner_id;
    ast_func_def_t *def;
    const char *cname;
    struct lower_method *next;
} lower_method_t;

/* extern cfunc 声明（`<lib>::<name>`）：调用点据此取真实 C 符号名与签名。 */
typedef struct lower_extern_fn {
    strslice_t lib;
    strslice_t name;
    ast_func_def_t *def;
    const char *proto; /* 已生成的 C 原型文本（同名冲突检测用） */
    struct lower_extern_fn *next;
} lower_extern_fn_t;

/* cfunc(...) 的 C 函数指针 typedef 缓存（同 intern 类型复用同一 typedef）。 */
typedef struct lower_cfn_typedef {
    const type_t *src;
    const char *tag;
    struct lower_cfn_typedef *next;
} lower_cfn_typedef_t;

/* `cfunc_ptr(f)` 生成的 C trampoline（每个被指向的函数一个）。 */
typedef struct lower_cb_wrapper {
    const ast_func_def_t *def;
    const char *name;
    struct lower_cb_wrapper *next;
} lower_cb_wrapper_t;

/* 跨模块 extern 声明去重（同一导入符号只声明一次）。 */
typedef struct lower_import_decl {
    const char *name;
    struct lower_import_decl *next;
} lower_import_decl_t;

/* extern var（`extern var <lib>::<name>: T;`）：库里的**数据符号**。 */
typedef struct lower_extern_var {
    strslice_t lib;
    strslice_t name;
    ast_var_def_t *def;
    const type_t *type;   /* 已解析的 clux 类型（C 侧按 ffi_c_type 渲染） */
    const char *cname;    /* 真实 C 符号名（未 mangling） */
    bool is_str;          /* C 符号是 char*，读写要做 str 包装/取值 */
    struct lower_extern_var *next;
} lower_extern_var_t;

typedef struct lower_ctx {
    arena_t *arena;
    sema_t  *sema;
    cir_unit_t *unit;
    lower_defer_t *defers;
    /* 当前正在 lowering 的函数：闭包自递归直接调用自身 static 函数。 */
    strslice_t cur_self_name;
    const char *cur_fn_cname;
    const char *cur_env_param;
    const char *last_closure_env_tag;
    /* 局部具名函数（块内 `func`）：名字 → 静态 C 函数/函数对象。 */
    lower_local_fn_t *local_fns;
    /* 正在内联的 defer 体的捕获结构体局部名（`__ctx.env` → 它）。 */
    const char *ctx_env_name;
    unsigned defer_seq;
    lower_local_t *locals;
    /* 本函数内**曾经声明过**的所有局部（不随块作用域弹出）：用于给 C
       名字做函数级唯一化（CIR 把块拍平，C 作用域不能依赖花括号）。 */
    lower_local_t *local_decls;
    lower_local_t *globals; /* 全局变量类型表（推断表达式类型 / 值调用用） */
    lower_capture_t *captures;
    lower_method_t *methods;
    lower_extern_fn_t *externs;
    lower_extern_var_t *extern_vars;
    lower_cfn_typedef_t *cfn_typedefs;
    lower_cb_wrapper_t *cb_wrappers;
    lower_import_decl_t *import_decls;
    /* `export struct/union/cunion/enum/type` 的名字：这些类型（的 C 形态）
       无条件进模块头文件。 */
    strslice_t *exported_types;
    size_t exported_type_count;
    const char *ffi_error; /* 最近一次 FFI 类型渲染的具体失败原因（可为 NULL） */
    cir_stmt_t *cleanup;
    cir_type_t *return_type;
    const type_t *return_sema_type; /* 对应的 sema 类型（return 兼容转换） */
    unsigned    ret_seq;
    unsigned    sig_seq;
    unsigned    fnlit_seq;
    unsigned    switch_seq;
    unsigned    ffi_seq;
    unsigned    assign_seq; /* 赋值旧值销毁的临时变量编号 */
    bool        failed;
} lower_ctx_t;

/* util */
const char *copy_arena(arena_t *arena, const char *s);
const char *mangle(arena_t *arena, const char *prefix, strslice_t name);
/** 模块符号名：`<unit->sym_prefix><kind><name>`（如 `clux_a61b_fn_main`）。 */
const char *module_symbol(arena_t *arena, const cir_unit_t *unit,
                          const char *kind, strslice_t name);
/** 模块短 tag：源文件相对项目根目录（cwd）路径的 FNV-1a 低 16 位。 */
void module_tag_for_path(const char *src, char *out, size_t cap);
const char *token_text(arena_t *arena, const token_t *tok);
const type_t *strip(sema_t *sema, const type_t *t);
/** 变量落 C 的实际值类型：声明类型为普通 func 而初始化器是带捕获闭包时，
 *  改用闭包类型（否则内联 env 被截断）。 */
const type_t *decl_value_type(lower_ctx_t *lc, const type_t *decl,
                              ast_node_t *init);
/** 是否生成/内建的 clone 调用（返回值已是完整值，勿再包 option）。 */
bool c_lower_is_clone_call(const ast_node_t *val);

/* types / signatures */
cir_type_t *map_type(lower_ctx_t *lc, const type_t *t);
cir_signature_t *signature_for(lower_ctx_t *lc, cir_type_t *ret, size_t pc,
                               cir_type_t **pt);
const type_t *func_type_of_def(lower_ctx_t *lc, ast_func_def_t *fn);
ast_func_def_t *func_def_of_ref(lower_ctx_t *lc, ast_func_ref_t *ref);
lower_method_t *find_method(lower_ctx_t *lc, uint32_t owner_id,
                            strslice_t name);

/* expressions */
cir_expr_t *lower_expr(lower_ctx_t *lc, ast_node_t *e);
cir_expr_t *lower_call(lower_ctx_t *lc, ast_call_t *call);
const type_t *infer_expr_type(lower_ctx_t *lc, ast_node_t *e);
bool call_is_len(const ast_call_t *c);
cir_expr_t *lower_len(lower_ctx_t *lc, ast_call_t *call);
/** 按目标类型 lower 一个值：匿名构造类型槽修正 + 布局兼容 bitcast。 */
cir_expr_t *lower_compat_value(lower_ctx_t *lc, const type_t *target,
                               ast_node_t *expr);
/** share/weak 拷贝点插入 retain；创建/升级表达式跳过。 */
cir_expr_t *lower_rc_copy(lower_ctx_t *lc, const type_t *target,
                          cir_expr_t *v, ast_node_t *expr);
cir_expr_t *lower_slice(lower_ctx_t *lc, ast_slice_t *sl);
cir_expr_t *lower_make(lower_ctx_t *lc, ast_make_t *mk);
cir_expr_t *lower_closure_literal(lower_ctx_t *lc, ast_func_def_t *fn);
/** 函数定义的值类型：有捕获 → 闭包类型，无捕获 → 普通签名类型。 */
const type_t *closure_type_of_def(lower_ctx_t *lc, ast_func_def_t *fn);
cir_expr_t *lower_func_literal(lower_ctx_t *lc, ast_func_def_t *fn);
/** 按名查找块内局部函数（调用点/函数值引用用）。 */
const type_t *lower_scope_ref_owner_type(lower_ctx_t *lc,
                                        ast_scope_ref_t *sr);

lower_local_fn_t *find_local_fn(lower_ctx_t *lc, strslice_t name);
lower_local_fn_t *find_local_fn_fid(lower_ctx_t *lc, uint32_t fid);
/* bitfield：逻辑字段按位读写（storage 是 struct 的隐藏数组字段）。 */
const type_t *bitfield_value_type(lower_ctx_t *lc, uint32_t type_id);
const ast_struct_def_t *bitfield_def_of(lower_ctx_t *lc, const type_t *t);
cir_expr_t *lower_bitfield_assign(lower_ctx_t *lc, ast_member_t *m,
                                  const char *op, ast_node_t *value);

/* statements */
cir_stmt_t *lower_stmt(lower_ctx_t *lc, ast_node_t *s, bool *ok);
cir_stmt_t *lower_ast_list(lower_ctx_t *lc, ast_node_t *list, bool *ok);
cir_stmt_t *lower_block(lower_ctx_t *lc, ast_node_t *block, bool *ok);
/* `.?` / `.!`（AST_TRY）的语句化展开：setup + `if (!ok) { tail }`。
   成功路径的值由调用方接续（赋值/return）。 */
cir_stmt_t *lower_try_prefix(lower_ctx_t *lc, ast_try_t *tr, bool *ok);
void register_local(lower_ctx_t *lc, strslice_t name, const type_t *type);
/** 用指定 C 名登记局部（闭包值局部需要与预登记映射一致）。 */
void register_local_as(lower_ctx_t *lc, strslice_t name, const type_t *type,
                       const char *cname);
/** 返回该局部声明的 C 符号名（未登记返回 NULL）。 */
const char *lookup_local_cname(lower_ctx_t *lc, strslice_t name);
/** 布局兼容复合值（匿名推断结构 vs 声明类型）重解释后的 lowering。 */
cir_expr_t *lower_compat_value(lower_ctx_t *lc, const type_t *target,
                               ast_node_t *expr);
void register_global(lower_ctx_t *lc, strslice_t name, const type_t *type);
const type_t *lookup_local(lower_ctx_t *lc, strslice_t name);

/* functions */
cir_function_t *lower_function(lower_ctx_t *lc, ast_func_def_t *fn,
                               bool is_entry);

/* M6 FFI：extern lib → 链接参数；extern cfunc → C 原型 + 调用点映射。 */
void lower_extern_decls(lower_ctx_t *lc, ast_program_t *prog);
cir_expr_t *lower_ffi_call(lower_ctx_t *lc, ast_call_t *call);
cir_expr_t *lower_data_call(lower_ctx_t *lc, ast_call_t *call);
cir_expr_t *lower_cfunc_ptr(lower_ctx_t *lc, ast_call_t *call);
bool call_is_cfunc_ptr(const ast_call_t *c);
/* `export cfunc`：生成 C ABI 包装（无 env），函数名 `clux_<tag>_<name>`，
 * 原型进模块头文件。 */
void lower_exported_cfunc(lower_ctx_t *lc, ast_func_def_t *fn);

/* M7 import：跨编译单元引用。 */
cir_expr_t *lower_import_call(lower_ctx_t *lc, ast_call_t *call,
                              ast_scope_ref_t *sr);
cir_expr_t *lower_import_var_read(lower_ctx_t *lc, ast_scope_ref_t *sr);
const char *lower_import_var_target(lower_ctx_t *lc, ast_scope_ref_t *sr);
bool is_import_scope_ref(lower_ctx_t *lc, ast_scope_ref_t *sr);
/* 跨模块泛型实例调用：`mod::f[i32](...)` 的 callee 已被 sema 改写成实例
 * 的 FUNC_REF（实例在**定义模块**的 prog 里）。 */
cir_expr_t *lower_cross_module_func_call(lower_ctx_t *lc, ast_call_t *call,
                                         ast_func_def_t *def, sema_func_t *sf);
/** 按 fid 在所有模块的 sema->funcs 里找函数，返回定义模块。 */
sema_func_t *lower_find_func_module(lower_ctx_t *lc, uint32_t fid,
                                    module_t **out_owner);
/* 跨模块结构体**方法**调用：按类型 id + 方法名在定义模块找回方法符号，
 * 生成 extern 并填充 `out`（调用点沿用普通方法调用 lowering）。 */
lower_method_t *lower_cross_module_method(lower_ctx_t *lc, uint32_t type_id,
                                          strslice_t name, ast_node_t *site,
                                          lower_method_t *out);
/* 在导入方 `.c` 里声明一个外部函数原型（`extern <ret> f(void *env, ...);`）。 */
void lower_import_func_extern(lower_ctx_t *lc, const char *cname,
                              const type_t *ft, bool env_first);
/** 方法实例定义在导入模块时返回其 owner 前缀符号（并生成 extern 声明）；
 *  同模块/无 owner 返回 NULL。 */
const char *lower_method_owner_cname(lower_ctx_t *lc, ast_func_def_t *def,
                                     const type_t *ft);
/* `cfunc(...)` → C 函数指针 typedef 名（map_type 用；同 intern 类型复用）。 */
const char *ffi_c_func_pointer_type(lower_ctx_t *lc, const type_t *t, bool *ok);
/* extern var 的读取（`is_str` 时包装成 clux_str）与赋值目标（raw 左值）。 */
cir_expr_t *lower_ffi_var_read(lower_ctx_t *lc, ast_scope_ref_t *sr);
const char *lower_ffi_var_target(lower_ctx_t *lc, ast_scope_ref_t *sr,
                                 bool *is_str);
/* extern var 的 clux 类型（infer_expr_type 用；未登记返回 NULL）。 */
const type_t *ffi_extern_var_type(lower_ctx_t *lc, ast_scope_ref_t *sr);

#endif
