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
    AST_STRUCT_EMBED,    /* 结构体嵌入声明 `...Name;`（仅 struct 定义体内；纯语法节点，
                            type 层的字段组合由 sema 在类型 seal 之前完成） */
    AST_UNION_DEF,       /* union name { field: type; ... }（顶层 union 定义，struct 同构平铺字段） */
    AST_UNION_MEMBER,    /* union member 声明（field: type，仅 union 定义体内） */
    AST_CUNION_DEF,      /* cunion name { field: type; ... }（顶层 C 语义 union 定义，
                            struct 同构平铺字段；无 tag，所有 member 共享 offset 0） */
    AST_ASSIGN,          /* name = expr; / name += expr; */
    AST_IF,              /* if cond { then } [else { else_body }] */
    AST_COMPTIME_IF,     /* comptime if (const_expr) { … } [else …]：**编译期
                            裁剪**（sema Pass 3a 把存活分支拼接进外层块；丢弃
                            分支只做语法检查；大括号不是作用域）。sema 之后
                            不再出现在语句链中。 */
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
    AST_DEFER,           /* defer ['<a,b>] [|caps|] { body }（延迟执行块：无参
                            void 闭包 + 登记到当前作用域的 defer 栈；作用域退出
                            （pop_scope）时在内存释放之前逆序调用。复用闭包捕获
                            语义——captures 是 AST_VAR_DEF 兄弟链，见 ast_defer.h） */
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
    AST_SCOPE_REF,        /* 作用域限定引用：lhs::name（枚举 variant / 模块成员 /
                              FFI 符号 / struct 静态成员 / 跨模块 struct 成员） */
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
    AST_OWNED_TYPE,      /* fatal T 所有权限定的**非指针**类型（复合类型的
                            将亡值变体，跨函数边界的 move/clone 载体） */
    AST_NEW,             /* new T{...} 堆分配构造 → own *T */
    AST_ADDR,            /* 后置取地址 x.&（由值得指针） */
    AST_DEREF,           /* 后置解引用取值 r.*（指针得值，GET） */
    AST_MOVE,            /* move(x) / clone(x) 所有权原语（op token 区分） */
    AST_SCOPE_ANNOT,     /* '<a,b,c> type 作用域标注（' 前导，位置信息非类型） */

    /* --- M4 切片（m4-design §1/§2/§4）---
     * 切片类型 own/ref/fatal []T（胖指针 {ptr,len}）、make 宏函数创建、
     * s[a:b] 切片表达式（恒产 ref []T）。 */
    AST_SLICE_TYPE,      /* own/ref/fatal []T 切片类型修饰（类型即表达式） */
    AST_SLICE,           /* expr[low:high] 切片表达式 → ref []T */
    AST_MAKE,            /* make(T, N, init...) 编译期宏函数 → fatal []T */

    /* --- M5 模块系统 --- */
    AST_IMPORT,          /* import <alias> from "<path>"; — 导入模块为命名空间 */
    AST_REEXPORT,        /* export * from "<path>"; — 代理导出依赖模块的全部导出符号 */
    AST_EXTERN_LIB,      /* extern lib "<path>" as <alias>; — M6 FFI 动态库声明（关键字判据，不按后缀） */
    AST_EXPORT,          /* export <decl> — 标记全局定义进入模块导出表（前缀修饰） */

    /* --- sema 生成的内部节点（不出现在用户源码中）--- */
    AST_FREE,            /* `FREE <expr>`：释放 expr 所指的堆块。sema 在作用域
                            退出点按类型生成 expr（简单指针 = 变量引用 → PUSH
                            <name>），compiler 编译 expr 后发 BCODE_FREE
                            （只消费栈顶）。compiler 不知道类型结构。 */
    AST_IS_TAG,          /* `<object> is <member>` 的**直接形态**（sema 生成）：
                            object 求值得 union 值，tag 是编译期已知的 member
                            编号（uint32）→ compiler 直接发 `IS_TAG <tag>`。
                            为什么不复用 AST_BINARY："is" 的 op 是 `token_t*`，
                            而 sema 无法合成词法 token。仅用于 sema 生成的
                            销毁序列（own tag union 的条件 FREE），不出现在
                            用户源码中。 */

    AST_OPT_IS_SOME,     /* `<object> != nil` 的**直接形态**（sema 生成）：
                            object 求值得 ?T，compiler 发 `OPT_IS_NONE [+ NOT]`。
                            为什么不复用 AST_BINARY："!=" 的 op 是 `token_t*`，
                            sema 无法合成词法 token。仅用于 own ?T 的销毁序列
                            （`if (v != nil) { <inner 销毁序列> }`）。 */
    AST_FOR_INDEX,       /* `for (i = 0; i < len(src); i = i + 1)` 的**直接形态**
                            （sema 生成）：计数器名 + 容器表达式 + body。
                            为什么不拼 AST_FOR + AST_BINARY：`<` / `+` 的 op 是
                            token_t*，sema 无法合成。仅用于逐元素销毁序列
                            （own []T / [N]T 元素含 own 槽位）。 */

    AST_CLONE,           /* 叶子深拷贝的直接形态（sema 生成）：operand → CLONE
                            （申请新块 + 拷贝字节）。只用于不含 own 槽位的类型；
                            复合类型的逐槽位展开由 sema 生成构造/NEW/循环 AST。
                            "clone" 的 op 是 token_t*，sema 无法合成。 */
    AST_TAKE,
    AST_TAKE_SLOT,       /* 把值从**内存槽位**取出并置零（cunion 成员 move）：
                            operand 是槽位左值访问 → TAKE_SLOT（拷贝值 + memset
                            槽位）。cunion 不安全：这样 move 才是真转移。 */            /* 把值移出变量的直接形态（sema 生成，语义 = move）：
                            operand → MOVE。用于扁平化 clone 的切片路径把临时
                            变量里的深拷贝结果移出（临时变量已标 moved，不再
                            FREE）。"move" 的 op 同样是 token_t*。 */

    AST_KIND_COUNT,      /* 哨兵值，用于数组索引 */
} ast_kind_t;

#endif /* _H_CLUX_PARSER_AST_KIND_ */
