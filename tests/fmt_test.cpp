#include "parser/fmt.h"
#include "parser/lexer.h"

#include "core/allocator.h"
#include "core/stream.h"
#include "core/vec.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "test_common.h"

namespace {

/* 对源码文本执行格式化，返回结果字符串（alloc 缓冲随 delete_allocator 回收）。 */
std::string fmt(const char *src) {
    allocator_t *a = create_allocator(malloc, free);
    EXPECT_NE(a, nullptr);

    stream_source_t source =
        stream_source_mem(a, src, std::strlen(src), /*owns_data=*/false);
    istream_t *stream = istream_open(a, source);
    EXPECT_NE(stream, nullptr);

    lexer_t *lexer = lexer_create(a, stream, "<test>");
    EXPECT_NE(lexer, nullptr);

    vec_t *pool = vec_new(a, /*owns_element=*/true);
    for (;;) {
        token_t *t = lexer_next(lexer);
        if (!t) break;
        vec_push(pool, a, t);
        if (token_get_kind(t) == TOKEN_TYPE_EOF) break;
    }

    char *out = fmt_format(a, pool, nullptr);
    std::string result = out ? out : "";

    /* fmt_format 的缓冲由 alloc 分配，需显式释放（不随 delete_allocator 回收） */
    if (out) allocator_free(a, (void **)&out);
    vec_free(a, &pool);
    lexer_close(&lexer);
    EXPECT_ALLOCATOR_EMPTY_DELETE(&a);
    return result;
}

} // namespace

/* 基本排版：'{' 跟随前行、'{' 后换行、'}' 单独一行、4 空格缩进。 */
TEST(Fmt, BasicBracesAndIndent) {
    std::string out = fmt("func main():i32{\nreturn 0;\n}\n");
    EXPECT_EQ(out,
              "func main(): i32 {\n"
              "    return 0;\n"
              "}\n");
}

/* 空块紧凑：`{}` 写在同一行。 */
TEST(Fmt, EmptyBlockIsCompact) {
    std::string out = fmt("func f():void{}\n");
    EXPECT_EQ(out, "func f(): void {}\n");
}

/* 分号后必须换行（非括号内）。 */
TEST(Fmt, SemicolonForcesNewline) {
    std::string out = fmt("func main():void{var a:i32=1;var b:i32=2;}\n");
    EXPECT_EQ(out,
              "func main(): void {\n"
              "    var a: i32 = 1;\n"
              "    var b: i32 = 2;\n"
              "}\n");
}

/* for 头部括号内的分号不换行。 */
TEST(Fmt, SemicolonInsideParensStaysInline) {
    std::string out = fmt("func main():void{for(var i:i32=0;i<5;i=i+1){}}\n");
    EXPECT_EQ(out,
              "func main(): void {\n"
              "    for (var i: i32 = 0; i < 5; i = i + 1) {}\n"
              "}\n");
}

/* '}' 后跟 else 时同行：`} else {`（非空块场景）。 */
TEST(Fmt, ElseFollowsCloseBrace) {
    std::string out = fmt(
        "func f():void{\n"
        "if(1>0){\n"
        "return;\n"
        "}else{\n"
        "return;\n"
        "}\n"
        "}\n");
    EXPECT_EQ(out,
              "func f(): void {\n"
              "    if (1 > 0) {\n"
              "        return;\n"
              "    } else {\n"
              "        return;\n"
              "    }\n"
              "}\n");
}

/* 两个空块：各自紧凑（空块规则优先），成为 `if() {} else {}`。 */
TEST(Fmt, EmptyBlocksWithElseStayCompact) {
    std::string out = fmt("func f():void{if(1>0){}else{}}\n");
    EXPECT_EQ(out,
              "func f(): void {\n"
              "    if (1 > 0) {} else {}\n"
              "}\n");
}

/* TAB 缩进被替换为 4 空格。 */
TEST(Fmt, TabBecomesFourSpaces) {
    std::string out = fmt("func f():void{\n\treturn;\n}\n");
    EXPECT_EQ(out,
              "func f(): void {\n"
              "    return;\n"
              "}\n");
}

/* 注释原样保留：独立行注释与行内注释。 */
TEST(Fmt, CommentsPreserved) {
    std::string out = fmt("// lead\nfunc f():void{\nvar a:i32=1; // inline\n}\n");
    EXPECT_EQ(out,
              "// lead\n"
              "func f(): void {\n"
              "    var a: i32 = 1; // inline\n"
              "}\n");
}

/* 空行保留（用户的分组意图）。 */
TEST(Fmt, BlankLinePreserved) {
    std::string out = fmt("func f():void{\nvar a:i32=1;\n\nvar b:i32=2;\n}\n");
    EXPECT_EQ(out,
              "func f(): void {\n"
              "    var a: i32 = 1;\n"
              "\n"
              "    var b: i32 = 2;\n"
              "}\n");
}

/* 幂等性：格式化结果再格式化保持不变。 */
TEST(Fmt, Idempotent) {
    const char *src =
        "func main():i32{\n"
        "var sum:i32=0;var i:i32=0;\n"
        "while(i<5){sum=sum+i;i=i+1;\n"
        "}\n"
        "for(var j:i32=0;j<5;j=j+1){if(j==3){continue;}\n"
        "sum=sum+j;}\n"
        "if(sum>100){printf(\"big\\n\");}else{printf(\"small\\n\");}\n"
        "return sum;\n"
        "}\n";
    std::string once = fmt(src);
    std::string twice = fmt(once.c_str());
    EXPECT_EQ(once, twice);
}

/* 嵌套块缩进逐层 +4。 */
TEST(Fmt, NestedIndent) {
    std::string out = fmt(
        "func f():void{\n"
        "while(1>0){\n"
        "if(2>0){\n"
        "return;\n"
        "}\n"
        "}\n"
        "}\n");
    EXPECT_EQ(out,
              "func f(): void {\n"
              "    while (1 > 0) {\n"
              "        if (2 > 0) {\n"
              "            return;\n"
              "        }\n"
              "    }\n"
              "}\n");
}

/* 控制流关键字与 `(` 之间须有空格；函数调用 `name(` 紧贴。 */
TEST(Fmt, ControlKeywordSpaceBeforeParen) {
    std::string out = fmt(
        "func f():void{\n"
        "if(1>0){}\n"
        "while(1>0){}\n"
        "for(var i:i32=0;i<1;i=i+1){}\n"
        "}\n");
    EXPECT_EQ(out,
              "func f(): void {\n"
              "    if (1 > 0) {}\n"
              "    while (1 > 0) {}\n"
              "    for (var i: i32 = 0; i < 1; i = i + 1) {}\n"
              "}\n");

    /* 函数调用：名与 `(` 紧贴，参数列表内外无多余空格 */
    std::string call = fmt("func main():void{printf(\"hi\\n\");}\n");
    EXPECT_EQ(call,
              "func main(): void {\n"
              "    printf(\"hi\\n\");\n"
              "}\n");
}

/* 数字类型后缀须与数值紧贴：`7i8` / `2.5f32` 不得被切成 `7 i8`，
 * 否则会改变语义（后缀本由词法器切为独立 token，但语法上必须紧邻）。 */
TEST(Fmt, NumericTypeSuffixStaysGlued) {
    std::string out = fmt(
        "func main():void{\n"
        "var a:i8=7i8;\n"
        "var b:f32=2.5f32;\n"
        "var c:u32=255u32;\n"
        "var d:f64=1.0f64;\n"
        "}\n");
    EXPECT_EQ(out,
              "func main(): void {\n"
              "    var a: i8 = 7i8;\n"
              "    var b: f32 = 2.5f32;\n"
              "    var c: u32 = 255u32;\n"
              "    var d: f64 = 1.0f64;\n"
              "}\n");

    /* 幂等性：再次格式化不变 */
    std::string twice = fmt(out.c_str());
    EXPECT_EQ(out, twice);
}

/* 运算符后的括号须与运算符以空格分隔（`a + (b)` / `x = (y)`），
 * 不得紧贴成 `+(` / `=(`，否则破坏可读性。函数调用 `name(` 仍紧贴。 */
TEST(Fmt, SpaceBeforeParenAfterOperator) {
    std::string out = fmt(
        "func main():void{\n"
        "var r:f64=(a as f64)+b;\n"
        "_ =dec+hex+oct+bin+(v8 as i32)+(vU8 as i32);\n"
        "}\n");
    EXPECT_EQ(out,
              "func main(): void {\n"
              "    var r: f64 = (a as f64) + b;\n"
              "    _ = dec + hex + oct + bin + (v8 as i32) + (vU8 as i32);\n"
              "}\n");

    /* 幂等性 */
    EXPECT_EQ(out, fmt(out.c_str()));
}

/* 一元前缀运算符紧贴其后操作数（`-x` / `~x` / `!x` / `-(a+b)`），
 * 区别于二元运算符两侧留空格（`a + b`）。 */
TEST(Fmt, UnaryPrefixOperatorNoTrailingSpace) {
    std::string out = fmt(
        "func main():void{\n"
        "var neg:i32=-sum;\n"
        "var bnot:i32=~a;\n"
        "var notb:bool=!yes;\n"
        "y=-(a+b);\n"
        "if(-x>0){return;}\n"
        "printf(!ok);\n"
        "}\n");
    EXPECT_EQ(out,
              "func main(): void {\n"
              "    var neg: i32 = -sum;\n"
              "    var bnot: i32 = ~a;\n"
              "    var notb: bool = !yes;\n"
              "    y = -(a + b);\n"
              "    if (-x > 0) {\n"
              "        return;\n"
              "    }\n"
              "    printf(!ok);\n"
              "}\n");

    /* 幂等性 */
    EXPECT_EQ(out, fmt(out.c_str()));
}

/* 输出总以换行结束（非空输入）。 */
TEST(Fmt, EndsWithNewline) {
    std::string out = fmt("func f():void{}");
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.back(), '\n');
}

/* 空输入不崩溃。 */
TEST(Fmt, EmptyInput) {
    std::string out = fmt("");
    EXPECT_TRUE(out.empty());
}

/* extends 关键字运算符：作为 word-like KEYWORD token，两侧按通用规则留单
 * 空格（`i32 extends i64`），与 as 一致；复合类型操作数两侧同样规整。 */
TEST(Fmt, ExtendsKeywordOperatorSpaced) {
    std::string out = fmt("func main():i32{\n"
                          "var a:bool=i32   extends   i32;\n"
                          "var b:bool=i64 extends i32;\n"
                          "var c:bool=[2]i32 extends[2]i32;\n"
                          "if(a&&!b&&c){return 1;}\n"
                          "return 0;\n"
                          "}\n");
    EXPECT_EQ(out,
              "func main(): i32 {\n"
              "    var a: bool = i32 extends i32;\n"
              "    var b: bool = i64 extends i32;\n"
              "    var c: bool = [2]i32 extends [2]i32;\n"
              "    if (a && !b && c) {\n"
              "        return 1;\n"
              "    }\n"
              "    return 0;\n"
              "}\n");

    /* 幂等性 */
    EXPECT_EQ(out, fmt(out.c_str()));
}

/* 数组类型构造 `.T{...}`：`.T{` 前保留运算符空格（`= .[2]i32`）、
 * `{` 紧凑单行不展开（`.T{10, 20}`）；多维数组类型 `]` 后 `[` 紧贴
 * （`.[2][2]i32`）；嵌套构造内层 `}` 后外层 `}` 同行。 */
TEST(Fmt, ArrayConstructCompact) {
    std::string out = fmt(
        "func main():i32{\n"
        "var arr=.[2]i32{10,20};\n"
        "var a=.[2][2]i32{.[2]i32{1,2},.[2]i32{3,4}};\n"
        "var u=.[2]str{\"a\",\"b\"};\n"
        "var e=.[0]i32{};\n"
        "return 0;\n"
        "}\n");
    EXPECT_EQ(out,
              "func main(): i32 {\n"
              "    var arr = .[2]i32{10, 20};\n"
              "    var a = .[2][2]i32{.[2]i32{1, 2}, .[2]i32{3, 4}};\n"
              "    var u = .[2]str{\"a\", \"b\"};\n"
              "    var e = .[0]i32{};\n"
              "    return 0;\n"
              "}\n");

    /* 幂等性 */
    EXPECT_EQ(out, fmt(out.c_str()));
}

/* 数组下标 `arr[i]` 紧贴不插空格；下标出现在左值/右值/多维/变量下标
 * 场景；类型标注 `: [3]i32` 与运算符后的 `[` 仍留空格。 */
TEST(Fmt, IndexSubscriptGlued) {
    std::string out = fmt(
        "func main():i32{\n"
        "var a=.[3]i32{10,20,30};\n"
        "var x=a[0]+a[2];\n"
        "a[1]=99;\n"
        "a[2]+=1;\n"
        "var m=.[2][2]i32{.[2]i32{1,2},.[2]i32{3,4}};\n"
        "var y=m[1][0];\n"
        "var i=1;\n"
        "var z=a[i];\n"
        "var t:[2]i32=.[2]i32{0,0};\n"
        "var ok:bool=[2]i32 extends[2]i32;\n"
        "return x+y+z;\n"
        "}\n");
    EXPECT_EQ(out,
              "func main(): i32 {\n"
              "    var a = .[3]i32{10, 20, 30};\n"
              "    var x = a[0] + a[2];\n"
              "    a[1] = 99;\n"
              "    a[2] += 1;\n"
              "    var m = .[2][2]i32{.[2]i32{1, 2}, .[2]i32{3, 4}};\n"
              "    var y = m[1][0];\n"
              "    var i = 1;\n"
              "    var z = a[i];\n"
              "    var t: [2]i32 = .[2]i32{0, 0};\n"
              "    var ok: bool = [2]i32 extends [2]i32;\n"
              "    return x + y + z;\n"
              "}\n");

    /* 幂等性 */
    EXPECT_EQ(out, fmt(out.c_str()));
}

/* 三元条件表达式：`?` 与 `:` 两侧留空格（`a ? b : c`）；嵌套三元右结合
 * 同样规整；类型标注冒号（`var x: i32`）不受影响（仍前不插空格）。 */
TEST(Fmt, TernaryOperatorSpaced) {
    std::string out = fmt("func main():i32{\n"
                          "var a:i32=3;\n"
                          "var b:i32=7;\n"
                          "var r:i32=(a>b)?a:b;\n"
                          "var s:i32=(a>b)?a:(a==b)?b:a;\n"
                          "if((a>b)?true:false){return r;}\n"
                          "return s;\n"
                          "}\n");
    EXPECT_EQ(out,
              "func main(): i32 {\n"
              "    var a: i32 = 3;\n"
              "    var b: i32 = 7;\n"
              "    var r: i32 = (a > b) ? a : b;\n"
              "    var s: i32 = (a > b) ? a : (a == b) ? b : a;\n"
              "    if ((a > b) ? true : false) {\n"
              "        return r;\n"
              "    }\n"
              "    return s;\n"
              "}\n");

    /* 幂等性 */
    EXPECT_EQ(out, fmt(out.c_str()));
}
