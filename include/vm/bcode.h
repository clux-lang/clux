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
    BCODE_PUSH_TUPLE,      /* 分配空 tuple type（开放，elems=NULL，不入池）+ 压其 type value */
    BCODE_APPEND_ELEM,     /* 无操作数：弹栈顶 type value（元素类型）→ peek 开放
                              tuple → 追加元素（元素匿名，长度=追加次数） */
    BCODE_SET_TYPE,        /* 弹栈顶 type value（sub）→ peek 栈顶开放对象 → 设为 sub */
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
} bcode_op_t;

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
