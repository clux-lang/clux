#ifndef _H_CLUX_PARSER_TYPE_QUAL_
#define _H_CLUX_PARSER_TYPE_QUAL_
#ifdef __cplusplus
extern "C" {
#endif

/**
 * 类型限定位标志（const/volatile 前缀）
 *
 * 解析 `[const|volatile]* <type>` 类型标注的结果。位可组合：
 *   TYPE_QUAL_CONST | TYPE_QUAL_VOLATILE = const volatile T
 * 组合语义：固定顺序 volatile(const(T))——
 * volatile 外层、const 内层。
 */
typedef enum {
    TYPE_QUAL_NONE    = 0,
    TYPE_QUAL_CONST   = 1,
    TYPE_QUAL_VOLATILE = 2,
} type_qual_t;

#ifdef __cplusplus
}
#endif
#endif /* _H_CLUX_PARSER_TYPE_QUAL_ */
