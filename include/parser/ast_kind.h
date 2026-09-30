#ifndef _H_CLUX_PARSER_AST_KIND_
#define _H_CLUX_PARSER_AST_KIND_

typedef enum {
    /* --- 顶层 --- */
    AST_PROGRAM,         /* 函数定义列表 */
    AST_FUNC_DEF,        /* func name(params):type { body } */

    /* --- 语句 --- */
    AST_VAR_DEF,         /* var name[:type] [= init]; */
    AST_TYPE_DEF,        /* type name = <type-expr>;（sema 求值折叠为类型引用） */
    AST_ENUM_DEF,        /* enum name:underlying { Var = val, ... }（顶层枚举定义） */
    AST_ENUM_VARIANT,    /* 枚举 variant 声明（name = value，仅 enum 定义体内） */
    AST_STRUCT_DEF,      /* struct name { field: type; ... }（顶层结构体定义） */
    AST_STRUCT_FIELD,    /* 结构体字段声明（name: type，仅 struct 定义体内） */
    AST_UNION_DEF,       /* union name { field: type; ... }（顶层 union 定义，struct 同构平铺字段） */
    AST_UNION_MEMBER,    /* union member 声明（field: type，仅 union 定义体内） */
    AST_CUNION_DEF,      /* cunion name { field: type; ... }（顶层 C 语义 union 定义，
                            struct 同构平铺字段；无 tag，所有 member 共享 offset 0） */
    AST_ASSIGN,          /* name = expr; / name += expr; */
    AST_IF,              /* if cond { then } [else { else_body }] */
    AST_SWITCH,          /* switch(cond) { (pat,..)->{..} default->{..} } */
    AST_SWITCH_CASE,     /* switch 分支（模式列表 + 分支体，仅 switch 内出现） */
    AST_WHILE,           /* while cond { body } */
    AST_DOWHILE,         /* do { body } while (cond); 后置条件循环 */
    AST_FOR,             /* for (init; cond; update) { body } */
    AST_RETURN,          /* return [expr]; */
    AST_BREAK,           /* break; */
    AST_CONTINUE,        /* continue; */
    AST_BLOCK,           /* { stmts... } */
    AST_EXPR_STMT,       /* expr;（表达式作为语句） */
    AST_EMPTY_STMT,      /* ;（空语句，无操作） */
    /* AST_DISCARD removed: _ = expr is AST_ASSIGN, discard semantics in Sema */

    /* --- 表达式 --- */
    AST_BINARY,          /* lhs op rhs */
    AST_UNARY,           /* op expr */
    AST_CALL,            /* callee(args...) */
    AST_MEMBER,          /* expr.field */
    AST_INDEX,           /* expr[expr, ...]（下标 / 泛型实例化，语义阶段区分） */
    AST_ARRAY,           /* [N]T 数组类型表达式（N=长度，T=基础类型） */
    AST_TUPLE,           /* <T1,T2,...> 元组类型表达式（元素类型兄弟链） */
    AST_CONSTRUCT,       /* .T { f1, f2, ... } 类型字面量构造（'.' 前导） */
    AST_INT_LIT,         /* 整数字面量 */
    AST_FLOAT_LIT,       /* 浮点字面量 */
    AST_BOOL_LIT,        /* true / false */
    AST_STRING_LIT,      /* "..."（escape 展开后文本） */
    AST_CHAR_LIT,        /* 'a'（u8 码点值） */
    AST_IDENT,           /* 标识符引用 */
    AST_UNDEF,           /* undefined（未初始化声明标记，sema 数据流分析消费） */
    AST_NIL,             /* nil（内置类型字面量值，函数 0 初始化/未来空指针） */

    AST_ERROR,           /* 解析错误恢复节点（记录错误位置，占位） */

    /* --- 类型修饰（类型即表达式，M2 关键架构决策 6）---
     * const/volatile 是真实类型 kind（有 vtable 代理），以嵌套节点表达
     * 递归修饰：const i32 → AST_CONST(AST_IDENT("i32"))；
     * volatile const i32 → AST_VOLATILE(AST_CONST(...))。
     * 无顺序约束，const const i32 嵌套重复合法（语义上幂等，消费层收敛）。 */
    AST_CONST,           /* const <type-expr> */
    AST_VOLATILE,        /* volatile <type-expr> */
    AST_OPTION,          /* ?T optional 类型修饰（类型即表达式，? 前导） */
    AST_FILL,            /* <v,N> 值包（仅 CONSTRUCT 字段链中出现；v=值，N=重复次数） */
    AST_CONSTRUCT_FIELD, /* struct 构造具名字段：.name = expr（仅 CONSTRUCT 字段链） */
    AST_FUNC_TYPE,       /* func(param_types...)->ret 函数签名类型（类型构造） */
    AST_TYPE_REF,        /* 类型引用：sema 登记的具名类型（__type_N）→ LOAD_TYPE <id> */
    AST_FUNC_REF,        /* 函数引用：sema 确认的函数名（函数值）→ LOAD_FUNCTION <id> */
    AST_ENUM_REF,        /* 枚举 variant 引用：Color::Red（sema 折叠 value 入节点） */
    AST_TERNARY,         /* cond ? then : else 三元条件表达式 */
    AST_UNWRAP,          /* optional 解包：a.!（assert，none 时 panic）/
                            a.?（try，仅词法预留，语义未实现） */

    /* --- 编译期运算符（SEMA→CTFE 桥梁，m2-design §8）---
     * sizeof/alignof/typeof 是前缀运算符：操作数只做 shadow 求值（仅取
     * 类型，不真实执行），运算符自身产出真实编译期常量（sizeof/alignof →
     * u64，typeof → type value）。sema 求值后折叠为字面量节点写回。 */
    AST_SIZEOF,          /* sizeof(T) / sizeof(expr) → u64 */
    AST_ALIGNOF,         /* alignof(T) / alignof(expr) → u64 */
    AST_TYPEOF,          /* typeof(expr) → type value */

    /* --- M3 指针与所有权（m3-design §3/§7/§8）---
     * own/ref/fatal *T 指针类型修饰（无裸指针）、new 堆分配、后置取址
     * x.& / 解引用 r.*、move/clone 所有权原语、作用域标注 '<a,b,c>。 */
    AST_PTR,             /* own/ref/fatal *T 指针类型修饰（类型即表达式） */
    AST_NEW,             /* new T{...} 堆分配构造 → own *T */
    AST_ADDR,            /* 后置取地址 x.&（由值得指针） */
    AST_DEREF,           /* 后置解引用取值 r.*（指针得值，GET） */
    AST_MOVE,            /* move(x) / clone(x) 所有权原语（op token 区分） */
    AST_SCOPE_ANNOT,     /* '<a,b,c> type 作用域标注（' 前导，位置信息非类型） */

    AST_KIND_COUNT,      /* 哨兵值，用于数组索引 */
} ast_kind_t;

#endif /* _H_CLUX_PARSER_AST_KIND_ */
