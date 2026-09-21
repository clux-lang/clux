#ifndef _H_CLUX_COMPILER_COMPILER_
#define _H_CLUX_COMPILER_COMPILER_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/strslice.h"
#include "core/vec.h"
#include "diag/diagnostic.h"
#include "parser/ast_block.h"
#include "parser/ast_func_def.h"
#include "parser/ast_node.h"
#include "parser/ast_var_def.h"
#include "sema/sema.h"
#include "sema/symbol.h"
#include "vm/bcode.h"
#include "vm/vm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ===========================================================================
 * 字节码编译器（AST → bcode）
 *
 * 输入：AST（parser 产物，arena 持有）+ sema 作用域树（仅用于查找
 * 函数签名类型 / 变量符号元数据）。
 * 输出：bytecode_t 模块（strtable + code 流），与执行器约定完全对齐
 * （见 docs/architecture-m1.md 2.7 与 tests/exec_test.cpp 函数测试）。
 *
 * 编译错误策略：diag 收集 + fail-fast。任一节点编译出错即置
 * compiler->failed，停止继续编译；driver 出口统一 diag_print_all。
 *
 * 平衡规则（编译期静态追踪）：
 *   - scope_depth：跳转跨出 N 层块作用域时，先发 N 个 POP_SCOPE 再跳
 *   - stack_depth：条件分支汇合点（if 的 end / for 的 update 入口）要求
 *     两侧栈深一致（表达式语句压栈的值由调用方 POP 平衡）
 * =========================================================================== */

/* ---- 标签（前向占位 + 回填） ---- */

typedef struct compile_patch_t {
    size_t pos;                 /* JMP/JZ/JNZ 的操作数字段偏移（opcode+4） */
    struct compile_patch_t *next;
} compile_patch_t;

typedef struct compile_label_t {
    bool           defined;     /* 已定义：pos 为实际目标 pc */
    size_t         pos;
    compile_patch_t *patches;   /* 未定义时的前向跳转 patch 列表 */
} compile_label_t;

/* ---- 循环上下文（break/continue 目标标签 + 跳出深度） ---- */

typedef struct compile_loop_t {
    compile_label_t *break_label;    /* 循环出口（break 目标） */
    compile_label_t *continue_label; /* 循环体底部（continue 目标） */
    size_t           scope_depth;    /* 循环进入时的 scope 深度（break/continue 跳出基准） */
    struct compile_loop_t *next;
} compile_loop_t;

/* ---- 编译器上下文 ---- */

typedef struct compiler_t {
    allocator_t    *alloc;
    vm_t           *vm;
    diag_buf_t     *diag;
    bytecode_t     *bc;
    vec_t          *tokens;        /* token pool（借 driver，诊断用位置） */

    /* sema 作用域树（借用，不拥有；编译完成后由 driver 释放） */
    sema_scope_t   *global_scope;
    sema_scope_t   *current_scope; /* 编译期当前词法作用域（符号元数据查找） */

    /* sema 类型登记表（借用，不拥有）：sema->types（sema_type_t* 数组）。
       hoist 类型提升区**两遍扫描**遍历它：pass 1 声明所有类型（PUSH_XXXX
       创建开放对象 → DEFINE_TYPE <id> 登记，不设字段），pass 2 定义所有
       类型（LOAD_TYPE 拉回 → 设字段 → SEAL 封闭，依赖后序）。AST_TYPE_REF
       槽位经 sema_type_find_name 查表拿 id 发 LOAD_TYPE。 */
    vec_t          *sema_types;

    /* 全部程序函数收集（compiler_compile 开头预扫描填充，ast_func_def_t* 列表，
       按 fid 序）：全局函数 + 局部函数 + 嵌套函数字面量（含表达式内与 comptime
       body 内被折叠产物引用的函数）。hoist 函数注册区（构造全部函数对象）与
       函数体区（编译各函数体、回填 PUSH_FUNCTION body 占位）按此列表统一驱动
       ——fid 序即构造/回填序。fid 单一来源在 sema（创建函数对象即分配，
       sema_func_id_alloc），预扫描只校验读取。comptime func 本身不入列表
       （不进入运行时）。 */
    vec_t          *funcs_all;

    /* 类型 id 分配计数器：sema_types 已占 [TYPE_ID_PROGRAM_BASE,
       TYPE_ID_PROGRAM_BASE + sema_types 数量)（sema_type_register 按登记
       序分配，签名类型亦登记于此）。本计数器仅作防御分支备用——compile_type.c
       AST_ARRAY 未替换场景临时分配 id（与 hoist 区同类型可成多 id 别名，
       types_by_id 幂等，语义无害）。签名类型 id 由 sema 分配（不在此列）。 */
    uint32_t        type_id_next;

    /* switch 临时变量序号（__switch_N 名字后缀）：每次编译 switch 递增，
       compiler_new memset 0 初始化。临时名只在本 switch 编译期间有效
       （bcode_write_str 即时拷入 strtable），嵌套 switch 经子作用域遮蔽，
       序号保证与用户变量零碰撞。 */
    uint32_t        switch_seq;

    /* 静态平衡追踪 */
    size_t          scope_depth;    /* 当前已 PUSH_SCOPE 未 POP 的层数 */
    int             stack_depth;    /* 静态操作数栈深度（压栈 +1 / 弹栈 -1） */

    compile_loop_t *loop_stack;     /* 循环上下文栈（break/continue） */

    bool            failed;         /* 已发生编译错误（fail-fast） */
} compiler_t;

/* ---- 生命周期 ---- */

/**
 * 创建编译器上下文。
 * - vm: 运行 VM（类型查询 / LOAD 校验用；不执行）
 * - diag: 诊断收集器（driver 出口打印）
 * - tokens: token pool（借 driver，节点位置 → location 转换用）
 * - global_scope: sema 作用域树根（借用，符号元数据查找）
 * - sema_types: sema 类型登记表（借用，hoist 提升 + LOAD_TYPE 槽位查找）
 * 返回 NULL 表示参数无效或 OOM。
 */
compiler_t *compiler_new(allocator_t *alloc, vm_t *vm, diag_buf_t *diag,
                         vec_t *tokens, sema_scope_t *global_scope,
                         vec_t *sema_types);

/** 销毁编译器上下文（不释放 vm/diag/tokens/scope 树，均为借用） */
void compiler_destroy(compiler_t **pc);

/**
 * 编译 AST 程序为字节码模块（产物归调用方，bcode_destroy 释放）。
 * 编译错误时返回 NULL（诊断已记入 diag，driver 出口打印）。
 * 成功后 bc 即编译产物（含函数注册段 + 各函数体），调用方
 * exec_run 后 scope_lookup("main") + value_call 触发执行。
 */
bytecode_t *compiler_compile(compiler_t *c, ast_node_t *program);

/* ===========================================================================
 * internal（compile.c / compile_type.c / compile_expr.c / compile_stmt.c /
 *          compile_func.c 共享，不对外）
 * =========================================================================== */

/* ---- 内部工具（compile.c 实现） ---- */

/** AST 节点 → 源码位置（经 token pool）。 */
location_t c_loc(compiler_t *c, const ast_node_t *node);

/** 编译错误：记入 diag + 置 failed（fail-fast）。 */
void c_error(compiler_t *c, const ast_node_t *node, const char *fmt, ...);

/* ---- 标签工具（前向占位 + 回填） ---- */

void label_init(compile_label_t *l);
void label_here(compiler_t *c, compile_label_t *l);
void emit_jump(compiler_t *c, compile_label_t *l);

/* ---- 静态平衡工具（scope/操作数栈深度静态追踪） ---- */

void balance_push(compiler_t *c);
void balance_pop(compiler_t *c);
void balance_scopes_out(compiler_t *c, size_t n);
void st_push(compiler_t *c, int delta);

/* ---- 节点编译入口（各文件实现） ---- */

void   compile_type_expr(compiler_t *c, ast_node_t *type_expr); /* compile_type.c */
void   compile_expr(compiler_t *c, ast_node_t *node);          /* compile_expr.c */
void   compile_stmt(compiler_t *c, ast_node_t *node);          /* compile_stmt.c */
void   compile_block_body(compiler_t *c, ast_block_t *b);      /* compile_stmt.c：块体编译 + 局部 type/函数入口提升 */
size_t compile_func_body(compiler_t *c, ast_func_def_t *fn);   /* compile_func.c */
size_t compile_func_reg_hoist(compiler_t *c, ast_func_def_t *fn); /* compile_func.c：hoist 函数注册区构造（返回 PUSH_FUNCTION body 操作数字段位置） */
void   compile_func_bind(compiler_t *c, ast_func_def_t *fn);   /* compile_func.c：LOAD_FUNCTION <fid> + DEFINE 名字绑定（全局绑定用，基底即实例） */
void   compile_func_bind_instance(compiler_t *c, ast_func_def_t *fn); /* compile_func.c：MAKE_FUNCTION + DEFINE 名字绑定（局部函数块入口提升，绑定实例——全块同一实例，循环内每次进入新实例） */
void   compile_func_capture_bind(compiler_t *c, ast_func_def_t *fn, bool keep); /* compile_func.c：定义点捕获绑定（keep=false 局部函数：PUSH name + 每捕获值 SET_CLOSURE + POP；keep=true 字面量：MAKE_FUNCTION + 每捕获值 SET_CLOSURE，函数值留栈顶） */
void   compile_prescan_funcs(compiler_t *c, ast_node_t *program); /* compile_func.c：递归收集全部函数定义（校验读取 sema 分配的 fid；comptime body 无条件递归） */

/* ---- hoist 类型提升区（compile_hoist.c） ---- */

/**
 * 编译类型提升区（产物最前、注册段之前）：
 * **两遍扫描**遍历 sema->types，把全部程序类型构造进 types_by_id 表。
 * pass 1（compile_hoist_declare，声明所有类型）：PUSH_XXXX 创建开放类型
 * 对象（数组 PUSH_ARRAY / 签名 PUSH_FUNC_TYPE / 限定符 PUSH_CONST·
 * PUSH_VOLATILE）→ DEFINE_TYPE <id> 绑定 program id + 登记；内建别名
 * LOAD_TYPE <内建 id> → DEFINE_TYPE。所有类型 id 先登记，向前引用安全。
 *
 * 两遍之间（由 compile.c 编排）插入**顶层 typedef 名字绑定**（LOAD_TYPE
 * <id>; PUSH_UNDEFINED; DEFINE "name"）——类型定义自动提升：pass 1 后类型
 * 对象已可 LOAD_TYPE 拉回，名字绑定先行，函数签名/变量类型槽位引用名字时
 * 类型已可查；类型对象仍开放（未密封），但 DEFINE 只存引用，pass 2 密封后
 * 名字解析到最终类型。
 *
 * pass 2（compile_hoist_define，定义所有类型）：LOAD_TYPE <id> 拉回 →
 * 设字段（DEFINE_BOUND / FUNC_TYPE_PARAM·RETURN / SET_TYPE）→ SEAL 封闭
 * 算布局，依赖后序（elem / sub / 参数·返回先密封）。槽位 LOAD_TYPE <id>
 * 运行时直接查表。净栈深 0。
 */
void compile_hoist_declare(compiler_t *c); /* pass 1：声明所有类型 */
void compile_hoist_define(compiler_t *c);  /* pass 2：定义所有类型 */

/** 类型登记表查找（AST_TYPE_REF 名字 / type_t 指针 → sema_type_t）。 */
const sema_type_t *c_sema_type_find_name(vec_t *types, strslice_t name);
const sema_type_t *c_sema_type_find_ptr(vec_t *types, const type_t *t);

/* ---- nil 初始化发码（compile_expr.c 实现，stmt 侧 var/assign 共用） ---- */

/** 类型槽位解析为真实 type_t（CONSTRUCT 类型位 / PUSH_OPT_NONE 查询用）：
 *  AST_TYPE_REF → sema 登记表 → 内建 type_lookup 兜底；AST_IDENT →
 *  type_lookup。解析失败返回 NULL（调用方负责报错）。 */
const type_t *c_resolve_type(compiler_t *c, ast_node_t *type_expr);

/** 发 PUSH_OPT_NONE <id>：nil 初始化（?T 构造器 nil 字段 / 数组 nil 元素 /
 *  var 声明 init nil）。option 类型须已登记 sema_types，否则报错。 */
void emit_push_opt_none(compiler_t *c, ast_node_t *type_expr);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_COMPILER_COMPILER_ */
