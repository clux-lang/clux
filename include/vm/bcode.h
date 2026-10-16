#ifndef _H_CLUX_VM_BCODE_
#define _H_CLUX_VM_BCODE_
#ifdef __cplusplus
extern "C" {
#endif

#include "core/allocator.h"
#include "core/strslice.h"
#include "core/vec.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * 字节码模块（bytecode_t）：聚合字符串表 + 字节码流的自包含可执行单元
 *
 * 产物只有两块（见 docs 2.7.3）：
 *   1. strs（strtable）：编译期收集的字符串，运行期只读。含变量名
 *      （PUSH/STORE/DEFINE 等指令的 strtable 索引）与字符串字面量
 *      （PUSH_STR 的索引）。
 *   2. code（字节码流）：[opcode:u32][变长操作数...] 线性序列。基本类型
 *      字面量全部内嵌为立即数（小端），无独立常量池。
 *
 * 两块产物构成完整可执行单元，不含任何指向 AST/符号表等中间产物的引用，
 * 可序列化落盘、重新加载直接执行。
 */

/* ================================================================ */
/* 指令集 opcode（docs 2.7.5）                                        */
/* ================================================================ */

typedef enum {
    BCODE_PUSH,            /* strtable 索引：scope_lookup 借用引用压栈 */
    BCODE_STORE,           /* strtable 索引：value_assign(dst, pop) 压结果 */
    BCODE_STORE_NIL,       /* strtable 索引：?T 变量置 none（只置 ok=false，压回 dst） */
    BCODE_PUSH_STR,        /* strtable 索引：字符串字面量压栈 */

    BCODE_PUSH_I8,         /* i8 立即数 */
    BCODE_PUSH_I16,        /* i16 立即数 */
    BCODE_PUSH_I32,        /* i32 立即数 */
    BCODE_PUSH_I64,        /* i64 立即数 */
    BCODE_PUSH_U8,         /* u8 立即数 */
    BCODE_PUSH_U16,        /* u16 立即数 */
    BCODE_PUSH_U32,        /* u32 立即数 */
    BCODE_PUSH_U64,        /* u64 立即数 */
    BCODE_PUSH_F32,        /* f32 立即数 */
    BCODE_PUSH_F64,        /* f64 立即数 */
    BCODE_PUSH_BOOL,       /* 1 字节布尔立即数 */

    BCODE_PUSH_VALUE,      /* offset：压入 stack[sp-1-offset] 借用引用 */
    BCODE_LOAD,            /* strtable 索引：从 global scope 查 type value 压栈 */
    BCODE_LOAD_TYPE,       /* u32 id：从 vm->types_by_id 查表压 type value 栈 */
    BCODE_LOAD_FUNCTION,   /* u32 id：从 vm->functions_by_id 查表压 func value 栈
                               （函数值运行期加载：变量/参数/返回值传递） */
    BCODE_SET_TYPE_NAME,   /* strtable 索引：弹栈顶 type value → 设置显示名
                               （未来 struct 等具名类型用，当前无生成） */
    BCODE_PUSH_UNDEFINED,  /* 压入 void 类型 value（"类型待推导"） */

    BCODE_DEFINE,          /* strtable 索引：弹栈定义（永远双弹 [value, type-spec]） */

    /* ---- 类型声明 / 定义两步模型（docs 2.7.5）----
       类型声明：PUSH_XXXX 创建开放 type 对象 + 压栈 → DEFINE_TYPE <id>
       绑定程序 id + 登记进 types_by_id（此后 LOAD_TYPE <id> 可拉回）。
       类型定义：LOAD_TYPE <id> 拉回开放对象 → SET 系列指令设置字段
       （元素类型/长度/参数/返回/sub 等）→ SEAL 封闭计算内存布局。
       const/volatile 亦有开放构造（PUSH_CONST/PUSH_VOLATILE 创建 sub=NULL
       的开放对象 → DEFINE_TYPE 声明 → LOAD_TYPE 拉回 → SET_TYPE 设 sub →
       SEAL），获得向前声明能力；内建别名（无开放构造阶段）LOAD_TYPE
       <内建 id> → DEFINE_TYPE <id> 直接登记。 */
    BCODE_PUSH_FUNC_TYPE,  /* 分配空 func type 入池 + 压其 type value */
    BCODE_FUNC_TYPE_PARAM, /* 弹栈 type value → 追加为下一参数 */
    BCODE_FUNC_TYPE_RETURN,/* 弹栈 type value → 设为返回类型 */
    BCODE_FUNC_TYPE_VARARG,/* 标记可变参数（无操作数） */
    BCODE_DEFINE_TYPE,     /* u32 id：弹栈顶 type value → 绑定程序 id + 登记进 types_by_id（幂等） */
    BCODE_SEAL,            /* 弹栈顶 type value → 密封（value_seal 分派 vtable->type_seal：
                              去重 intern + 布局计算 + 置 sealed）；密封后按开放对象自身 id 更新
                              登记（去重复用时重定向到新实例）。不带 id 操作数。 */

    BCODE_PUSH_FUNCTION,   /* entry pc：构造 bcode_function_t + 签名类型 → func value（id 默认 0，由 BIND_FUNC 填充） */
    BCODE_BIND_FUNC,       /* u32 id：peek 栈顶 func value → 填充 fn->id + 登记 id→func（幂等，不弹栈） */
    BCODE_SET_FUNC_NAME,   /* strtable 索引：peek 栈顶 func value → 设置函数名（不弹栈） */
    BCODE_SET_CLOSURE,     /* strtable 索引：弹栈顶值 → clone 进栈下函数对象的
                              closure_scope 捕获槽（define-or-replace，占位
                              undefined 被真实捕获值替换） */
    BCODE_MAKE_FUNCTION,   /* u32 fid：从 functions_by_id 拉基底函数对象 → 实例化
                              新函数实例（共享 entry_pc/cfunc/type/name/id，新
                              closure_scope 捕获槽 undefined 占位）→ 压栈。
                              函数定义点每次求值生成独立实例——捕获绑定互不干扰
                              （对标 LOAD_FUNCTION 的"同一对象"引用语义）。 */

    BCODE_ADD, BCODE_SUB, BCODE_MUL, BCODE_DIV, BCODE_MOD,
    BCODE_EQ,  BCODE_NE,  BCODE_LT,  BCODE_LE,  BCODE_GT,  BCODE_GE,
    BCODE_AND, BCODE_OR,
    BCODE_BXOR, BCODE_SHL, BCODE_SHR,
    BCODE_NEG, BCODE_NOT, BCODE_BNOT,

    BCODE_CAST,            /* 显式转换：类型经栈顶 type value（LOAD 压入），弹 type+值 */
    BCODE_CREATE_CONST,    /* 弹 type value → type_const_intern → type value 压回 */
    BCODE_CREATE_VOLATILE, /* 弹 type value → type_volatile_intern → type value 压回 */
    BCODE_PUSH_CONST,      /* 分配空 const type（开放，sub=NULL，不入池）+ 压其 type value */
    BCODE_PUSH_VOLATILE,   /* 分配空 volatile type（开放，sub=NULL，不入池）+ 压其 type value */
    BCODE_PUSH_OPT,        /* 分配空 optional type（开放，inner=NULL，不入池）+ 压其 type value */
    BCODE_PUSH_ENUM,       /* 分配空 enum type（开放，underlying=NULL，不入池）+ 压其 type value */
    BCODE_ENUM_VARIANT,    /* [strtable_idx:u32][value:i64]：peek 开放 enum → 追加 variant
                              （名从 strtable 拷贝，值按 underlying->size 截断） */
    BCODE_MAKE_ENUM,       /* 弹 type value + 弹底层整数值 → 按底层宽度截断构造 enum 值 */
    BCODE_PUSH_STRUCT,     /* 分配空 struct type（开放，fields=NULL，不入池）+ 压其 type value */
    BCODE_DEFINE_FIELD,    /* strtable 索引：弹栈顶 type value（字段类型）→ peek 开放
                              struct → 追加字段（名从 strtable 拷贝） */
    BCODE_ADD_EMBED,       /* U32 embed_type_id：peek 栈顶开放 struct → vm_type_load
                              拉 embed 类型 → 追加到 embeds 表（seal 前可编辑）。
                              无栈深变化（与 SET_EMBEDS 不同，逐个追加）。 */
    BCODE_PUSH_TUPLE,      /* 分配空 tuple type（开放，elems=NULL，不入池）+ 压其 type value */
    BCODE_APPEND_ELEM,     /* 无操作数：弹栈顶 type value（元素类型）→ peek 开放
                              tuple → 追加元素（元素匿名，长度=追加次数） */
    BCODE_SET_TYPE,        /* 弹栈顶 type value（sub）→ peek 栈顶开放对象 → 设为 sub */
    BCODE_SET_OWNERSHIP,   /* u8 ownership：peek 栈顶开放复合类型 → 设其所有权修饰
                              （只允许 OWN_NONE → OWN_FATAL，产出 fatal T 将亡值
                              变体；密封后拒绝——见 type_set_ownership）。无栈深变化。 */
    BCODE_CALL,            /* argc：value_call（callee 在 stack[sp-1-argc]） */
    BCODE_RET,             /* 返回 interrupt 哨兵，栈顶即返回值 */

    BCODE_JMP,             /* 目标 pc（绝对字节偏移） */
    BCODE_JZ,              /* 目标 pc：弹引用，false/0 则跳 */
    BCODE_JNZ,             /* 目标 pc：弹引用，非 false/0 则跳 */

    BCODE_PUSH_SCOPE,      /* scope_new(alloc, current) */
    BCODE_POP_SCOPE,       /* 销毁当前 scope */
    BCODE_POP,             /* 丢弃栈顶引用（不释放，归 scope） */
    BCODE_HALT,            /* 停止执行 */

    /* ---- array type 构造（与 func type 统一：声明 DEFINE_TYPE + 定义 SEAL）----
       仅构造"数组类型"（array type）；数组值由 CONSTRUCT 指令构造。 */
    BCODE_PUSH_ARRAY,      /* 分配空 array type（开放，暂不入池） + 压其 type value */
    BCODE_DEFINE_BOUND,   /* U32：弹栈顶元素 type value → 设为元素类型 + 边界立即数 N（编译期常量） */

    /* ---- 值构造 / 下标访问（对应 construct / set_item / get_item 流程）----
       值构造（统一多步协议收尾）：类型经声明-定义两步构造（PUSH_XXXX →
       DEFINE_TYPE → LOAD_TYPE → ... → SEAL → LOAD_TYPE）后以 type value
       形式留在栈顶，随后按类型字段序压入成员值，construct N 收尾。
       当前仅实现 array 分支（struct / tuple 待后续 Phase）。 */
    BCODE_CONSTRUCT,        /* U32：成员数量；弹 N 个成员值 + 类型位 → 按类型构造 value */
    BCODE_INDEX_GET,        /* 弹 self + index，返回 self[index]（get_item） */
    BCODE_INDEX_SET,        /* 弹 self + index + val，返回 self（set_item） */
    BCODE_FIELD_GET,        /* STR：字段名；弹 self → 返回 self.field 借用引用
                               （零拷贝，data 指向 self data 块内偏移；FIELD 名
                               编译期常量，strtable 索引，不走 vtable 分派） */
    BCODE_FIELD_SET,        /* STR：字段名；弹 self + val → 写回字段 → 返回 self
                               （隐式转换 val → 字段类型；嵌套字段经借用链偏移正确） */
    BCODE_PUSH_OPT_NONE,    /* u32 id：从 types_by_id 查 option 类型 → 压 ok=false +
                               value 全零的 ?T 值块（构造器字段/fill 的 nil） */
    BCODE_UNWRAP,           /* 无操作数：弹 ?T 值 → ok 则借用返回 value 字段的
                               （a.! assert 解包；零拷贝，data 指向 option 值块
                               内偏移）；ok=false（none）→ panic 错误值
                               （"unwrap '.!' on none optional value"）。 */
    BCODE_OPT_IS_NONE,      /* 无操作数：弹 ?T 值 → 压 bool（ok tag == false）
                               （nil 判定 x==nil / nil==x → OPT_IS_NONE；
                               x!=nil / nil!=x → OPT_IS_NONE + NOT。
                               tag 比较专用指令，非 vtable eq 分派——nil 非 value） */

    /* ---- 长度查询：代理到 vtable->length（当前仅数组实现，返回 u64 元素个数） ---- */
    BCODE_LENGTH,           /* 弹 self，返回 value_length(self)（如数组 → u64 元素个数） */

    /* ---- tag union 构造 / 判 tag（与 struct 同族：开放构造协议
       PUSH_UNION → DEFINE_TYPE → LOAD_TYPE → UNION_MEMBER×N → SEAL） ---- */
    BCODE_PUSH_UNION,       /* 分配空 union type（开放，members=NULL，不入池）
                               + 压其 type value */
    BCODE_UNION_MEMBER,     /* STR：strtable 索引；peek 开放 union → 追加 member
                               （名拷贝，tag = 追加序） */
    BCODE_IS_TAG,           /* U32：tag 立即数；弹 union 值 → 读 data 首部 tag
                               与立即数比较 → 压 bool（`x is Member` 编译期已
                               解析 member → tag 值） */

    /* ---- C 语义 union 构造（与 struct 同族：开放构造协议
       PUSH_CUNION → DEFINE_TYPE → LOAD_TYPE → DEFINE_FIELD×N → SEAL；
       无 tag——member 追加走 DEFINE_FIELD 协议，与 struct 同款） ---- */
    BCODE_PUSH_CUNION,      /* 分配空 cunion type（开放，members=NULL，不入池）
                               + 压其 type value */

    /* ---- M3 指针与所有权（m3-design §3/§7/§8）----
       指针类型与 func 签名同族：开放构造协议
       PUSH_PTR → DEFINE_TYPE → LOAD_TYPE → SET_TYPE 设 base → SEAL。
       base 是引用依赖（LOAD_TYPE 拉回，不递归）。
       值级指令：NEW（堆分配构造）、PTR_GET/PTR_SET（解引用 GET/SET 严格
       分离）、ADDR（后置取址）、MOVE/CLONE（所有权原语）。 */
    BCODE_PUSH_PTR,         /* u8 ownership：分配空 ptr type（开放，base=NULL，不入池）
                               + 压其 type value。ownership = 所有权修饰
                               （OWN_OWN/REF/FATAL） */
    BCODE_NEW,              /* 弹类型位 + 弹 1 个成员值 → 按类型构造 own *T
                               堆块（new T{...}；Step A 不追踪，值=裸指针） */
    BCODE_PTR_GET,          /* 弹指针值 → 压被指向 T 值副本（r.* 解引用 GET） */
    BCODE_PTR_SET,          /* 弹指针值 + val → 写回被指向 T（r.* = v 解引用
                               SET；val 隐式转换 → T）→ 压回指针 */
    BCODE_ADDR,             /* 弹值 → 压 ref *T 指针（x.& 后置取址，借用） */
    BCODE_MOVE,             /* 弹值 → 压同类型值（move(x)，所有权转移，
                               Step A 静默：值平凡传递） */
    BCODE_CLONE,            /* 弹值 → 压深拷贝值（clone(x)，value_clone 分派） */
    BCODE_DISPOSE,          /* 无操作数：弹值 → 递归释放其内嵌 own 堆块
                               （`_ = expr` 显式丢弃 fatal/own，m3-design
                               §3.3：fatal 必被 own 接管，_ = 丢弃例外）→
                               不压回（表达式语句消费）。借用（ref/x.&）
                               跳过。 */

    /* ---- M4 切片段（m4-design §1/§2/§4）---- */
    BCODE_PUSH_SLICE,       /* u8 ownership：分配空 slice type（开放，elem=NULL，
                               不入池）+ 压其 type value。ownership = 所有权修饰
                               （OWN_OWN/REF/FATAL） */
    BCODE_SLICE,            /* u8 flags：bit0=low present, bit1=high present。
                               弹 self + 按 flags 弹 high/low → 压 ref []T 胖指针
                               （s[low:high] 切片表达式，恒产 ref []T，m4-design
                               §4；省略 low=0, 省略 high=len(self)） */
    BCODE_MAKE,             /* u32 init_count：弹 init_count 个初始值 + 弹 length
                               + 弹 elem_type → 分配 len*elem_size 堆块 + 逐元素
                               布值 → 压 fatal []T（make(T,N,init...) 宏函数，
                               m4-design §2） */

    /* ---- M5 模块系统 ---- */
    BCODE_IMPORT,           /* STR：strtable 索引（模块规范路径）。vm_import 查                               vm->modules 表 → 构造模块值（type=type_module，
                               data=module_t*）压栈。幂等：同一路径返回同一模块。 */
    BCODE_GET_MEMBER,       /* STR：strtable 索引（成员名）。弹模块值 → 从
                               module_t->exports 按名取成员值压栈。模块命名空间
                               隔离：仅导出成员可访问。 */
    /* ---- M6 FFI（追加在末尾：不得改变既有操作码编号，否则旧容器会被静默误读）---- */
    BCODE_EXTERN_CALL,      /* STR "lib::name" + U32 argc + U32 ret_kind：从当前模块库表取句柄 →
                             * ffi_lib_sym → 按 argc 弹实参调类型化调用层（整数/指针/浮点）。 */
    BCODE_EXTERN_LOAD,      /* STR "lib::name" + U32 kind：动态库导出的**数据符号** →
                             * ffi_lib_sym 得**变量地址** → 按 kind 读 size 字节 →
                             * 压对应内建类型的值（extern var 的读取）。 */
    BCODE_EXTERN_STORE,     /* STR "lib::name" + U32 kind：弹栈顶值 → 按 kind 把 size 字节
                             * 写进符号地址（extern var 的写入；不压回结果）。 */
    BCODE_DATA,             /* 无操作数（**追加在末尾**，不动既有编号）：弹一个**序列值**
                             * （array/slice/str）→ 压 `ref *T`（T = 元素类型，str → u8），
                             * 指向首元素。C ABI 适配：`f(data(s), len(s))` 与 C 的
                             * `(T*, size_t)` 同形。
                             *
                             * **为什么必须是指令而不是内置函数**：内置 cfunc 的实参会被
                             * clone 进被调作用域（value_call 的既有语义，保证内置不会改到
                             * 调用方的值），因此函数形态只能拿到"副本"的地址——slice 的
                             * 缓冲区副本、数组的副本块，调用一结束就悬垂。取**存储身份**
                             * 必须像 `x.&`（ADDR）那样在调用点就地完成：编译器压序列的
                             * 借用视图，本指令从中读出真实的 ptr 字段。 */

    /* ---- struct 方法调用（**追加在末尾**，不动既有编号）----
     *
     * 不复用 CALL：方法 func value 存在 struct_type 的 attributes 表中，而非
     * 全局作用域——CALL 需要栈上是 callee 值，而 MEMBER_CALL 在运行期从
     * receiver 的 struct 类型中解析方法。sema 不再把 `a.b(args)` 改写为
     * `AST_FUNC_REF + 接收者插首参`，而是标记 AST_CALL，compiler 据此发
     * MEMBER_CALL。
     *
     * 栈: [receiver, arg1..argN] → [result]。净变化 = -argc。
     * 实例方法：receiver 是值或指针，作为第一个参数（self）传递。
     *   值 receiver → 运行期取地址（ref *T）；指针 receiver → 原样传递。
     * 静态方法：receiver 是 type value → 不作为参数传递（type value 只用于
     *   确定在哪个 struct 类型的 attributes 中查找方法）。 */
    BCODE_MEMBER_CALL,      /* STR method_name + U32 argc：弹 argc 个实参 +
                             * 弹 receiver → 从 receiver 对应 struct 类型的
                             * attributes 中查方法 func value → value_call。
                             * receiver 为 type value 时是静态方法调用（无 self）。 */

    /* ---- struct 静态成员（attribute）读写（**追加在末尾**，不动既有编号）----
     * struct_type_t 运行时持有 attributes（name→value_t* 借用映射）+ methods
     * （方法名白名单）。这两个指令按 type_id 查到 struct 类型后，从 attributes
     * 按名取/存值。value 归 member_scope 管（挂在 module global_scope 下）。 */
    BCODE_LOAD_ATTR,        /* U32 type_id + STR name：从 struct_type_t::attributes
                             * 按名取值压栈（借用引用）。沿嵌入链继承查找（sema 已
                             * 校验存在性）。type_id 索引 vm->types_by_id。 */
    BCODE_STORE_ATTR,       /* U32 type_id + STR name：弹栈顶值 → 写入
                             * struct_type_t::attributes[name]（赋值语句，不压回
                             * 结果）。沿嵌入链继承查找目标类型。 */
    BCODE_MARK_UNIQUE,      /* 无操作数：peek 栈顶开放 struct → type_struct_mark_unique
                             *（seal 前调用，seal 跳过去重 intern）。无栈深变化。 */
} bcode_op_t;

/* ---- BCODE_EXTERN_LOAD / BCODE_EXTERN_STORE 的类型码（M6 FFI 数据符号）----
 * 宽度 + 类别：决定从符号地址读/写多少字节、按哪个内建类型建模。与 clux 内建
 * 标量一一对应；`bool` 单独一码（宽度同 u8，但**类型必须还原成 bool**——否则
 * `crt::flag != true` 会因为拿到的值是 u8 而报类型不匹配）。`opaque` 用 PTR，
 * 即指针宽度。**宽度必须与库里符号的真实类型一致**：读多/写多会越界踩到相邻
 * 符号，因此 sema 只放行这张表里的类型，别的类型明确报错。 */
#define BCODE_FFI_DATA_I8   0u
#define BCODE_FFI_DATA_I16  1u
#define BCODE_FFI_DATA_I32  2u
#define BCODE_FFI_DATA_I64  3u
#define BCODE_FFI_DATA_U8   4u
#define BCODE_FFI_DATA_U16  5u
#define BCODE_FFI_DATA_U32  6u
#define BCODE_FFI_DATA_U64  7u
#define BCODE_FFI_DATA_F32  8u
#define BCODE_FFI_DATA_F64  9u
#define BCODE_FFI_DATA_PTR  10u
#define BCODE_FFI_DATA_BOOL 11u
/* 数据符号：库里的 `char *` 全局（clux 侧声明为 str）。 */
#define BCODE_FFI_DATA_STR  12u
/* 数据符号：**按类型值读写**（指令前由 `compile_type_expr` 压入类型值）。
 * struct/cunion/enum 走这里——读写字节数 = 该类型的 size。 */
#define BCODE_FFI_DATA_TYPED 13u

/* ---- BCODE_EXTERN_CALL 的 ret_kind（返回类型码）----
 * 前 5 个是 v0 的原始编码（`.cxb` 兼容：旧容器里只有这些值，语义保持不变）：
 *   0 = void、1 = i32、2 = i64（旧产物里 u64 也走这里）、3 = 指针/opaque、
 *   4 = f64（旧产物里 f32 也走这里）。
 * 之后**逐个类型补齐**——clux 的标量与 C 是 1:1 的，调用层用 libffi 的精确
 * `ffi_type` 描述符，因此返回值的类型必须与源码声明**完全一致**（旧编码把 u64
 * 返回造成 i64，使用点会在比较/赋值时撞上隐式转换）。 */
#define BCODE_FFI_RET_VOID  0u
#define BCODE_FFI_RET_I32   1u
#define BCODE_FFI_RET_I64   2u
#define BCODE_FFI_RET_PTR   3u
#define BCODE_FFI_RET_F64   4u
#define BCODE_FFI_RET_U32   5u
#define BCODE_FFI_RET_U64   6u
#define BCODE_FFI_RET_I8    7u
#define BCODE_FFI_RET_I16   8u
#define BCODE_FFI_RET_U8    9u
#define BCODE_FFI_RET_U16   10u
#define BCODE_FFI_RET_BOOL  11u
#define BCODE_FFI_RET_F32   12u
/* 返回 C 的 char*：读回一个指针并按 clux `str` 造值（如 getenv/strchr）。 */
#define BCODE_FFI_RET_STR   13u
/* **按类型值返回**：指令前有一个由 `compile_type_expr` 压入的**类型值**
 * （与 `CONSTRUCT` 同一套机制），运行期按该类型读返回内存并造值。
 * struct/cunion/enum 等程序自定义类型走这里——宽度/布局必须查类型，
 * 用一个小整数码表达不了。 */
#define BCODE_FFI_RET_TYPED 14u

/* ---- ret_kind 操作数的高位：变参信息（M6 FFI；与低 8 位的类型码共用一个 u32）----
 * 不新增第四个操作数的理由：`EXTERN_CALL` 的 `.cxs` 文本与旧 `.cxb` 都按
 * "STR + U32 + U32" 解析，加操作数会让**旧产物读错**。旧产物写 ret_kind 时高位
 * 恒为 0（值是 0..4），因此下面的解包对旧产物天然等价于"非变参" ✓。
 *
 *   bits 0..7   : 返回类型码（BCODE_FFI_RET_*）
 *   bit  8      : 变参（1 = 被调函数是 C 的 `...`，走 libffi ffi_prep_cif_var）
 *   bits 9..15  : **固定参数个数**（变参时有意义；非变参恒 0）
 *
 * 变参调用必须知道"前几个是固定参数"才能按 C 的约定铺寄存器/栈（且变参部分要
 * 施加 C 的默认实参提升：f32→double、窄整型→int），所以固定参数个数随指令落盘。 */
#define BCODE_FFI_RET_KIND_MASK 0xFFu
#define BCODE_FFI_VARIADIC_BIT  (1u << 8)
#define BCODE_FFI_NFIXED_SHIFT  9u
#define BCODE_FFI_RET_PACK(kind, variadic, nfixed)                     \
    (((uint32_t)(kind) & BCODE_FFI_RET_KIND_MASK) |                    \
     ((variadic) ? BCODE_FFI_VARIADIC_BIT : 0u) |                      \
     ((((uint32_t)(nfixed)) << BCODE_FFI_NFIXED_SHIFT) & 0x0000FE00u))
#define BCODE_FFI_RET_UNPACK_KIND(v) ((uint32_t)(v) & BCODE_FFI_RET_KIND_MASK)
#define BCODE_FFI_RET_UNPACK_VARIADIC(v) \
    (((uint32_t)(v) & BCODE_FFI_VARIADIC_BIT) != 0u)
#define BCODE_FFI_RET_UNPACK_NFIXED(v) \
    (((uint32_t)(v) >> BCODE_FFI_NFIXED_SHIFT) & 0x7Fu)

/* ================================================================ */
/* 聚合结构：bytecode 模块                                            */
/* ================================================================ */

typedef struct bytecode_t {
    allocator_t *alloc;
    vec_t       *strs;   /* strtable：string_t*，vec owns 生命周期 */
    struct {             /* code 字节流缓冲（小端，可原地回填） */
        uint8_t *data;
        size_t   len;
        size_t   cap;
    } code;
    /* 本模块的**规范路径**（借用 module_t::canonical，不拥有；NULL = 独立
       字节码，无模块上下文）。运行期 `IMPORT <源码路径>` 必须以"当前正在执行
       的模块"为基准重新规范化路径，此字段就是那个基准（见 exec.c op_import）。 */
    const char *module_path;
} bytecode_t;

/* ---- 生命周期 ---- */

/** 创建空 bytecode 模块（空 strtable + 空 code 流） */
bytecode_t *bcode_new(allocator_t *alloc);

/** 销毁模块并置空调用方指针（释放 strtable 全部字符串 + code 缓冲） */
void bcode_destroy(bytecode_t **bc);

/* ---- strtable 访问（运行期只读） ---- */

/** 返回 strtable 字符串个数 */
size_t bcode_str_count(const bytecode_t *bc);

/** 按索引取回 strtable 字符串（越界返回空 slice） */
strslice_t bcode_str_at(const bytecode_t *bc, size_t idx);

/** intern 查找：strtable 中命中返回既有索引，未命中追加后返回新索引 */
size_t bcode_str_index(bytecode_t *bc, strslice_t s);

/* ================================================================ */
/* writer：生成字节码（编译期）                                       */
/* ================================================================ */

/** 写 opcode（u32） */
void bcode_write_op(bytecode_t *bc, bcode_op_t op);

/** 写立即数（小端） */
void bcode_write_u8(bytecode_t *bc, uint8_t v);
void bcode_write_i8(bytecode_t *bc, int8_t v);
void bcode_write_u16(bytecode_t *bc, uint16_t v);
void bcode_write_i16(bytecode_t *bc, int16_t v);
void bcode_write_u32(bytecode_t *bc, uint32_t v);
void bcode_write_i32(bytecode_t *bc, int32_t v);
void bcode_write_u64(bytecode_t *bc, uint64_t v);
void bcode_write_i64(bytecode_t *bc, int64_t v);
void bcode_write_f32(bytecode_t *bc, float v);
void bcode_write_f64(bytecode_t *bc, double v);
void bcode_write_bool(bytecode_t *bc, bool v);

/**
 * strtable 无感写：intern 到 strtable（去重）并写 u32 索引。
 * 调用方无需关心索引值；返回索引（供需要时复用）。
 */
size_t bcode_write_str(bytecode_t *bc, strslice_t s);

/** 返回 code 流当前位置（字节偏移），用作跳转标签 */
size_t bcode_tell(const bytecode_t *bc);

/** 原地回填：在 pos 处覆写 u32（跳转目标 pc 回填）。pos 须 < 当前长度 */
void bcode_patch_u32(bytecode_t *bc, size_t pos, uint32_t v);

/* ================================================================ */
/* reader：读取字节码（执行器指令回调）                                */
/* ================================================================ */

/**
 * 读函数族：从 bc 的 code 流 *pc 处读对应宽度立即数（小端），*pc 前进。
 * 越界读取触发 panic（执行器信任字节码完整性，越界 = 字节码损坏）。
 */
bcode_op_t bcode_read_op(const bytecode_t *bc, size_t *pc);
uint8_t    bcode_read_u8(const bytecode_t *bc, size_t *pc);
int8_t     bcode_read_i8(const bytecode_t *bc, size_t *pc);
uint16_t   bcode_read_u16(const bytecode_t *bc, size_t *pc);
int16_t    bcode_read_i16(const bytecode_t *bc, size_t *pc);
uint32_t   bcode_read_u32(const bytecode_t *bc, size_t *pc);
int32_t    bcode_read_i32(const bytecode_t *bc, size_t *pc);
uint64_t   bcode_read_u64(const bytecode_t *bc, size_t *pc);
int64_t    bcode_read_i64(const bytecode_t *bc, size_t *pc);
float      bcode_read_f32(const bytecode_t *bc, size_t *pc);
double     bcode_read_f64(const bytecode_t *bc, size_t *pc);
bool       bcode_read_bool(const bytecode_t *bc, size_t *pc);

/**
 * strtable 无感读：读 u32 索引 → 从 strtable 取回字符串（strslice，零拷贝）。
 * 调用方无需感知索引，直接拿到字符串视图。
 */
strslice_t bcode_read_str(const bytecode_t *bc, size_t *pc);

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_VM_BCODE_ */
