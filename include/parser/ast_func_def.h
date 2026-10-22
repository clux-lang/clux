#ifndef _H_CLUX_PARSER_AST_FUNC_DEF_
#define _H_CLUX_PARSER_AST_FUNC_DEF_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/strslice.h"
#include "parser/ast_generic_param.h"
#include "parser/ast_node.h"
#include "parser/parser.h"

typedef struct {
    ast_node_t  base;
    strslice_t  name;        /* 函数名 */
    ast_node_t *captures;    /* 闭包捕获列表（func |a,(b:i32=c+d)| ...）：AST_VAR_DEF
                                兄弟链。纯 id 捕获 = type_expr/init 均为 NULL（类型取自
                                外层符号）；括号 VALUE DECL = 完整 var def（显式类型 +
                                init 初始化表达式）。NULL = 无捕获。 */
    strslice_t *scope_annot_names; /* 闭包作用域标注（m3-design §9）：func '<x> |...|
                                      的标注集合（arena 数组，'*' = global）。
                                      捕获 ref/opaque 时强制标注（视同 struct ref 字段）。
                                      NULL/scope_annot_count=0 = 无标注。 */
    size_t      scope_annot_count; /* 标注名字个数 */
    ast_node_t *params;      /* AST_VAR_DEF 兄弟链 */
    ast_node_t *params_last; /* O(1) 追加 */
    ast_node_t *return_expr; /* 返回类型表达式（NULL = void） */
    ast_node_t *body;        /* AST_BLOCK */
    bool        is_comptime; /* comptime func：调用点编译期求值折叠为常量，不注册到运行时 */
    bool        is_noreturn; /* `[[noreturn]]`：调用点永不正常返回（TDZ 终止分支） */
    /* ---- M6 FFI ---- */
    bool        is_cfunc;        /* cfunc 定义（有体，C ABI，可被 C 侧 dlsym 调用） */
    bool        is_extern_cfunc; /* extern cfunc 声明（无体，运行期 dlsym 查找） */
    bool        is_variadic;     /* 参数表以 ... 结尾（仅 extern cfunc 允许） */
    strslice_t  lib_name;        /* extern cfunc 的库别名（<lib>::<name> 的 lib 部分） */
    uint32_t    sig_id;

    /* 闭包**捕获聚合类型** id（0 = 无捕获）：捕获槽表登记成一个 struct 类型
       （fields = 捕获名 + 类型 + 槽位所有权），VM 层据此把闭包当作聚合值处
       理——clone 深拷贝捕获、free 按槽位所有权释放、字段读写走同一套规则
       （§7.1.10 "闭包 = 捕获结构体"）。编译器在 hoist 注册函数对象后发
       `SET_FUNC_CAPTURES <id>`。 */
    uint32_t       capture_type_id;

    /* 闭包 env 的**生成函数** id（"闭包 = 捕获聚合结构体 + vtable"）：
       - env_dispose_fid：`__destroy_<env>(dv: ref *Env)` —— 按 capture_type 的
         槽位所有权释放 env 里的 own 捕获（VM 在闭包销毁时调用）；
       - env_clone_fid  ：`__clone_<env>(dv: ref *Env): fatal Env` —— 深拷贝
         env（VM 在闭包 clone 时调用，结果拷贝进新实例的 env 块）。
       0 = 无捕获（无 env）。这两个函数由 sema 在收尾阶段按需生成（与普通复合
       类型的销毁/clone 函数同一套生成器），compiler 在 hoist 随
       `SET_FUNC_CAPTURES` 一起写入函数对象——于是 clone/free 不再需要运行期
       类型内省（也就不再需要 `__deep_clone`）。 */
    uint32_t       env_dispose_fid;
    uint32_t       env_clone_fid;      /* 签名类型 id（sema 3b 填充，compiler LOAD_TYPE <sig_id> 用；
                                全局函数经 sema_type_register 分配，局部函数提升时解析分配） */
    uint32_t    fid;         /* 函数 id（compiler 预扫描分配，LOAD_FUNCTION <fid> 用；
                                hoist 函数注册区构造时 BIND_FUNC <fid> 携带） */

    /* `[[<expr>]]` 装饰器链（表达式兄弟链，源码序；NULL = 无）。
       **只被前脱糖消费**：[[d1]] [[d2]] func f(...) 展开为"源函数 + 装饰后绑定"，
       展开后本字段清空（sema/compiler 看不到装饰器）。 */
    ast_node_t *decorators;

    /* M7 泛型参数表：func f[T, N: comptime u64](...)。count == 0 = 普通函数。
       泛型定义本身不产生运行时实体（sema 只登记符号，实例化时再生成
       mangling 名的普通函数副本）。 */
    ast_generic_params_t generic;
    /* M7 跨模块泛型：实例函数的**定义模块**（module_t*；NULL = 本模块）。
       compiler 据此按定义模块 sema 作用域编译 body，并把运行时 root_scope
       指到定义模块 module scope。 */
    void *owner_module;
} ast_func_def_t;

static inline ast_node_t *ast_func_def_new(arena_t *arena,
                                           uint32_t tok_begin, uint32_t tok_end) {
    ast_func_def_t *n = (ast_func_def_t *)arena_calloc(
        arena, 1, sizeof(ast_func_def_t), ALIGNOF(max_align_t));
    if (!n) return NULL;
    n->base.kind      = AST_FUNC_DEF;
    n->base.tok_begin = tok_begin;
    n->base.tok_end   = tok_end;
    return &n->base;
}

/** 解析函数定义：func name(params):type { body } */
ast_node_t *parse_func_def(parser_t *p);

/** M6 FFI：解析 cfunc 定义（有体，C ABI）。 */
ast_node_t *parse_cfunc_def(parser_t *p);

/**
 * func 统一入口（语句级 / 表达式级）。
 * expected_kind = AST_FUNC_DEF → 语句级函数定义（有 name）
 * M2+: expected_kind = AST_FUNC_LIT → 表达式级匿名函数
 */
ast_node_t *parse_func_like(parser_t *p, ast_kind_t expected_kind,
                                  const char *kw);

/** 解析参数列表中的单个参数（name: type）——接口签名解析复用。 */
ast_node_t *parse_func_param_decl(parser_t *p);

/** 解析捕获列表：`| item (, item)* |`（空列表 `||` 返回 NULL）。
 *  闭包（func |...|）与 defer（defer |...|）共用同一份解析——捕获项形态
 *  （纯 id / 括号 VALUE DECL）逐字一致，不另造一套。调用方已确认当前
 *  token 是开界符号 `|`。成功后游标停在闭界 `|` 之后。 */
ast_node_t *parse_capture_list(parser_t *p);

/** M6 FFI：extern cfunc 声明（无体，dlsym 解析）。 */
ast_node_t *parse_extern_cfunc(parser_t *p, uint32_t tb);

/** M6 FFI：同上，但消费的关键字由 `kw` 给出（`"cfunc"` 规范写法 /
 *  `"func"` 等价习惯写法）。两者产生完全相同的 AST。 */
ast_node_t *parse_extern_cfunc_kw(parser_t *p, uint32_t tb, const char *kw);

/** 内部：带预解析名/库的扩展入口（extern cfunc 复用 func 的签名解析）。 */
ast_node_t *parse_func_like_ex(parser_t *p, ast_kind_t expected_kind,
                           const char *kw, strslice_t pre_name,
                           strslice_t pre_lib);

#ifdef __cplusplus
}
#endif
#endif
